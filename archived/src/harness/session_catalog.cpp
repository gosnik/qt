#include <qt/harness/session_catalog.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include <qt/foundation/filesystem.hpp>
#include <qt/harness/session_lifecycle.hpp>

namespace qt::harness {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Path;
using qt::foundation::Result;

std::string path_to_string(const Path& path) { return path.generic_string(); }

bool path_exists(const Path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool path_is_directory(const Path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

bool path_is_regular_file(const Path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

std::string normalize_path_string(const std::string& path) {
  return std::filesystem::path(path).lexically_normal().generic_string();
}

std::vector<Path> list_session_files(const Path& directory) {
  std::vector<Path> session_files;
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    const Path path = it->path();
    if (path.extension() == ".jsonl" && path_is_regular_file(path)) {
      session_files.push_back(path);
    }
  }

  std::sort(session_files.begin(), session_files.end(),
            [](const Path& lhs, const Path& rhs) {
              return lhs.generic_string() < rhs.generic_string();
            });
  return session_files;
}

std::string make_load_diagnostic(const std::string& path,
                                 const qt::foundation::Error& error) {
  return "failed to load session file: " + path + ": " + error.message;
}

bool record_matches_preferred_working_directory(
    const SessionCatalogRecord& record,
    const std::string& preferred_working_directory) {
  return !preferred_working_directory.empty() &&
         normalize_path_string(record.snapshot.metadata.working_directory) ==
             normalize_path_string(preferred_working_directory);
}

std::optional<std::filesystem::file_time_type> record_last_write_time(
    const SessionCatalogRecord& record) {
  std::error_code error;
  const auto modified = std::filesystem::last_write_time(record.repository_path, error);
  if (error) {
    return std::nullopt;
  }
  return modified;
}

bool record_is_better_candidate(
    const SessionCatalogRecord& candidate, bool candidate_is_preferred,
    const std::optional<std::filesystem::file_time_type>& candidate_modified,
    const SessionCatalogRecord& selected, bool selected_is_preferred,
    const std::optional<std::filesystem::file_time_type>& selected_modified) {
  if (candidate_is_preferred != selected_is_preferred) {
    return candidate_is_preferred;
  }
  if (candidate_modified != selected_modified) {
    if (!selected_modified) {
      return static_cast<bool>(candidate_modified);
    }
    if (!candidate_modified) {
      return false;
    }
    return *candidate_modified > *selected_modified;
  }
  return candidate.repository_path < selected.repository_path;
}

std::string ambiguous_prefix_message(
    const std::string& id_prefix,
    const std::vector<const SessionCatalogRecord*>& matches) {
  std::ostringstream message;
  message << "Session prefix '" << id_prefix << "' is ambiguous; matches:";
  for (const auto* match : matches) {
    message << ' ' << match->snapshot.metadata.session_id;
  }
  return message.str();
}

}  // namespace

Result<SessionCatalog> load_session_catalog(const std::string& sessions_directory) {
  if (sessions_directory.empty()) {
    return Result<SessionCatalog>::failure(ErrorCode::invalid_argument,
                                           "sessions_directory is required");
  }

  const Path directory_path(sessions_directory);
  if (!path_exists(directory_path)) {
    return Result<SessionCatalog>::failure(
        ErrorCode::not_found,
        "sessions directory does not exist: " + sessions_directory);
  }
  if (!path_is_directory(directory_path)) {
    return Result<SessionCatalog>::failure(
        ErrorCode::invalid_argument,
        "sessions directory is not a directory: " + sessions_directory);
  }

  SessionCatalog catalog;
  catalog.sessions_directory = sessions_directory;

  std::vector<SessionBranchSnapshot> snapshots;
  for (const auto& path : list_session_files(directory_path)) {
    const std::string repository_path = path_to_string(path);
    const auto snapshot = load_session_branch_snapshot(repository_path);
    if (!snapshot) {
      catalog.add_diagnostic(make_load_diagnostic(repository_path, snapshot.error()));
      continue;
    }

    snapshots.push_back(snapshot.value());
    catalog.session_records.push_back(
        SessionCatalogRecord{repository_path, snapshot.value()});
  }

  const auto graph = build_session_graph(snapshots);
  if (!graph) {
    return graph.error();
  }

  catalog.graph = graph.value();
  catalog.root_session_ids = catalog.graph.root_session_ids;
  return catalog;
}

std::optional<std::string> find_session_catalog_path_by_id_prefix(
    const SessionCatalog& catalog, const std::string& id_prefix,
    const std::string& preferred_working_directory) {
  const auto resolved = resolve_session_catalog_path_by_id_prefix(
      catalog, id_prefix, preferred_working_directory);
  if (!resolved) {
    return std::nullopt;
  }
  return resolved.value();
}

Result<std::string> resolve_session_catalog_path_by_id_prefix(
    const SessionCatalog& catalog, const std::string& id_prefix,
    const std::string& preferred_working_directory) {
  if (id_prefix.empty()) {
    return Result<std::string>::failure(ErrorCode::invalid_argument,
                                        "session id prefix is required");
  }

  std::vector<const SessionCatalogRecord*> matches;
  std::vector<const SessionCatalogRecord*> preferred_matches;
  for (const auto& record : catalog.session_records) {
    if (record.snapshot.metadata.session_id.rfind(id_prefix, 0) != 0) {
      continue;
    }

    matches.push_back(&record);
    if (record_matches_preferred_working_directory(
            record, preferred_working_directory)) {
      preferred_matches.push_back(&record);
    }
  }

  auto& candidates =
      preferred_matches.empty() ? matches : preferred_matches;
  if (candidates.empty()) {
    return Result<std::string>::failure(
        ErrorCode::not_found, "No session found matching '" + id_prefix + "'");
  }
  if (candidates.size() > 1U) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        ambiguous_prefix_message(id_prefix, candidates));
  }

  return candidates.front()->repository_path;
}

std::optional<std::string> find_most_recent_session_catalog_path(
    const SessionCatalog& catalog,
    const std::string& preferred_working_directory) {
  const SessionCatalogRecord* selected = nullptr;
  bool selected_is_preferred = false;
  std::optional<std::filesystem::file_time_type> selected_modified;

  for (const auto& record : catalog.session_records) {
    const bool candidate_is_preferred = record_matches_preferred_working_directory(
        record, preferred_working_directory);
    const auto candidate_modified = record_last_write_time(record);

    if (selected == nullptr ||
        record_is_better_candidate(record, candidate_is_preferred, candidate_modified,
                                   *selected, selected_is_preferred,
                                   selected_modified)) {
      selected = &record;
      selected_is_preferred = candidate_is_preferred;
      selected_modified = candidate_modified;
    }
  }

  if (selected == nullptr) {
    return std::nullopt;
  }
  return selected->repository_path;
}

}  // namespace qt::harness
