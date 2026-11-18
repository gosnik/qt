#include "qt/app/cli/startup.hpp"

#include "qt/app/config/settings.hpp"
#include "qt/foundation/clock.hpp"
#include "qt/harness/repository.hpp"
#include "qt/harness/session_catalog.hpp"
#include "qt/harness/session_lifecycle.hpp"
#include "qt/harness/session_manager.hpp"
#include "qt/harness/session_repository.hpp"
#include "qt/harness/session_resume.hpp"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace qt::app {
namespace {

using Json = nlohmann::json;

std::string env_or_empty(const char* name) {
  if (const char* value = std::getenv(name)) {
    return std::string(value);
  }
  return {};
}

std::string default_cwd() {
  return std::filesystem::current_path().generic_string();
}

std::string current_date_string() {
  std::time_t now = std::time(nullptr);
  const std::tm* utc = std::gmtime(&now);
  if (!utc) {
    return "1970-01-01";
  }

  std::ostringstream out;
  out << std::put_time(utc, "%Y-%m-%d");
  return out.str();
}

std::optional<std::string> session_id_from_header(const Json& header) {
  if (header.value("type", std::string{}) == "session" &&
      header.contains("id") && header.at("id").is_string()) {
    return header.at("id").get<std::string>();
  }

  if (header.value("kind", std::string{}) == "session_metadata" &&
      header.contains("payload") && header.at("payload").is_object()) {
    const auto& payload = header.at("payload");
    if (payload.contains("metadata") && payload.at("metadata").is_object()) {
      const auto& metadata = payload.at("metadata");
      if (metadata.contains("session_id") &&
          metadata.at("session_id").is_string()) {
        return metadata.at("session_id").get<std::string>();
      }
    }
  }

  return std::nullopt;
}

bool session_dir_has_id_prefix(const std::string& session_dir,
                                         const std::string& id_prefix) {
  const auto catalog = qt::harness::load_session_catalog(session_dir);
  if (!catalog) {
    return false;
  }
  for (const auto& record : catalog.value().session_records) {
    if (record.snapshot.metadata.session_id.rfind(id_prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

foundation::Result<std::string> session_dir_resolve_id_prefix(
    const std::string& session_dir, const std::string& id_prefix,
    const std::string& preferred_working_directory = {}) {
  const auto catalog = qt::harness::load_session_catalog(session_dir);
  if (!catalog) {
    return catalog.error();
  }
  return qt::harness::resolve_session_catalog_path_by_id_prefix(
      catalog.value(), id_prefix, preferred_working_directory);
}

std::optional<std::string> most_recent_session_path(
    const std::string& session_dir,
    const std::string& preferred_working_directory = {}) {
  const auto catalog = qt::harness::load_session_catalog(session_dir);
  if (!catalog) {
    return std::nullopt;
  }
  return qt::harness::find_most_recent_session_catalog_path(
      catalog.value(), preferred_working_directory);
}

std::string safe_session_filename_part(std::string value) {
  if (value.empty()) {
    return "session";
  }
  for (char& ch : value) {
    const auto uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) == 0 && ch != '-' && ch != '_') {
      ch = '-';
    }
  }
  return value;
}

qt::harness::SessionEntry make_session_message_entry(
    const Json& message_json) {
  qt::harness::SessionEntry entry;
  entry.data = qt::harness::SessionCustomEntry{
      qt::harness::kUpstreamPiMonoSessionEntryType,
      Json{{"type", "message"}, {"message", message_json}},
      qt::harness::SessionUpstreamTreeFields{},
  };
  return entry;
}

}  // namespace

bool session_arg_looks_like_path(std::string_view session_arg) {
  return session_arg.find('/') != std::string_view::npos ||
         session_arg.find('\\') != std::string_view::npos ||
         (session_arg.size() >= 6 &&
          session_arg.substr(session_arg.size() - 6) == ".jsonl");
}

std::string generated_session_id() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(now).count();
  return "native-" + std::to_string(micros);
}

std::string child_session_path(const std::string& session_dir,
                                         const std::string& source_path,
                                         const std::string& child_session_id) {
  std::filesystem::path directory;
  if (!session_dir.empty()) {
    directory = std::filesystem::path(session_dir);
  } else {
    directory = std::filesystem::path(source_path).parent_path();
  }
  if (directory.empty()) {
    directory = std::filesystem::current_path();
  }

  const std::string stem = safe_session_filename_part(child_session_id);
  std::filesystem::path candidate = directory / (stem + ".jsonl");
  for (int suffix = 2; std::filesystem::exists(candidate) && suffix < 1000;
       ++suffix) {
    candidate = directory / (stem + "-" + std::to_string(suffix) + ".jsonl");
  }
  return candidate.generic_string();
}

foundation::Result<ReferenceStartupSession> create_startup_session(
    const std::string& session_dir, std::string mode) {
  if (session_dir.empty()) {
    ReferenceStartupSession session;
    session.mode = std::move(mode);
    session.working_directory = default_cwd();
    return foundation::Result<ReferenceStartupSession>::success(session);
  }

  std::error_code directory_error;
  std::filesystem::create_directories(session_dir, directory_error);
  if (directory_error) {
    return foundation::Result<ReferenceStartupSession>::failure(
        foundation::ErrorCode::io_error,
        "Cannot create session directory: " + directory_error.message());
  }

  const std::string session_id = generated_session_id();
  const std::string repository_path =
      child_session_path(session_dir, default_cwd(), session_id);
  const auto created = qt::harness::create_persistent_session(
      qt::harness::CreatePersistentSessionRequest{
          session_id,
          default_cwd(),
          repository_path,
          std::nullopt,
          current_date_string(),
      });
  if (!created) {
    return foundation::Result<ReferenceStartupSession>::failure(
        created.error().code, created.error().message,
        created.error().system_error);
  }

  ReferenceStartupSession session;
  session.mode = std::move(mode);
  session.persistent = true;
  session.repository_path = created.value().repository_path;
  session.session_id = created.value().metadata.session_id;
  session.working_directory = created.value().metadata.working_directory;
  session.entry_count = 1;
  return foundation::Result<ReferenceStartupSession>::success(session);
}

foundation::Result<ReferenceStartupSession> fork_startup_session(
    const std::string& source_path, const std::string& session_dir,
    std::optional<std::string> fork_entry_id) {
  const auto parent_entries = qt::harness::SessionRepository(source_path).load();
  if (!parent_entries) {
    return parent_entries.error();
  }
  const auto parent_snapshot =
      qt::harness::derive_session_branch_snapshot(parent_entries.value());
  if (!parent_snapshot) {
    return parent_snapshot.error();
  }

  const std::string child_session_id =
      "fork-" + parent_snapshot.value().metadata.session_id;
  const std::string child_path =
      child_session_path(session_dir, source_path, child_session_id);
  const auto child = qt::harness::fork_child_session(
      qt::harness::ForkSessionRequest{
          parent_snapshot.value(),
          child_session_id,
          child_path,
          default_cwd(),
          std::nullopt,
          std::nullopt,
      });
  if (!child) {
    return child.error();
  }

  std::vector<qt::harness::SessionEntry> branch_entries;
  if (fork_entry_id && !fork_entry_id->empty()) {
    const qt::harness::SessionManager manager(parent_entries.value());
    branch_entries = manager.get_branch(*fork_entry_id);
    if (branch_entries.empty()) {
      return foundation::Result<ReferenceStartupSession>::failure(
          foundation::ErrorCode::invalid_argument,
          "Invalid entry ID for forking");
    }
    branch_entries.pop_back();
  } else {
    branch_entries = parent_entries.value();
  }

  qt::harness::SessionRepository child_repository(child_path);
  for (const auto& entry : branch_entries) {
    if (entry.kind() == qt::harness::SessionEntryKind::kSessionMetadata) {
      continue;
    }
    const auto appended = child_repository.append(entry);
    if (!appended) {
      return foundation::Result<ReferenceStartupSession>::failure(
          appended.error().code, appended.error().message,
          appended.error().system_error);
    }
  }

  ReferenceStartupSession session;
  session.mode = "fork";
  session.persistent = true;
  session.repository_path = child.value().repository_path;
  session.parent_repository_path = source_path;
  session.session_id = child.value().metadata.session_id;
  session.parent_session_id = parent_snapshot.value().metadata.session_id;
  session.working_directory = child.value().metadata.working_directory;
  session.entry_count = branch_entries.size();
  return foundation::Result<ReferenceStartupSession>::success(session);
}

foundation::Result<ReferenceStartupSession> open_startup_session(
    const std::string& repository_path, std::string mode) {
  const auto resumed = qt::harness::resume_session(repository_path);
  if (!resumed) {
    return foundation::Result<ReferenceStartupSession>::failure(
        resumed.error().code, resumed.error().message,
        resumed.error().system_error);
  }

  ReferenceStartupSession session;
  session.mode = std::move(mode);
  session.persistent = true;
  session.repository_path = resumed.value().repository_path;
  session.session_id = resumed.value().metadata.session_id;
  session.working_directory = resumed.value().metadata.working_directory;
  session.prompt_history = resumed.value().metadata.prompt_history;
  session.entry_count = resumed.value().entries.size();
  return foundation::Result<ReferenceStartupSession>::success(
      std::move(session));
}

foundation::Result<ReferenceStartupSession> clone_startup_session(
    const ReferenceStartupSession& startup_session,
    const std::string& session_dir) {
  if (!startup_session.repository_path) {
    return foundation::Result<ReferenceStartupSession>::failure(
        foundation::ErrorCode::invalid_argument,
        "Cannot clone session: no active session file");
  }

  const std::string cloned_session_id =
      "clone-" +
      safe_session_filename_part(
          startup_session.session_id.value_or(generated_session_id()));
  const std::string cloned_path = child_session_path(
      session_dir, *startup_session.repository_path, cloned_session_id);
  const auto cloned = qt::harness::clone_session_repository(
      qt::harness::CloneSessionRepositoryRequest{*startup_session.repository_path,
                                                 cloned_session_id, cloned_path});
  if (!cloned) {
    return foundation::Result<ReferenceStartupSession>::failure(
        cloned.error().code, cloned.error().message,
        cloned.error().system_error);
  }

  auto opened = open_startup_session(cloned.value().repository_path, "clone");
  if (!opened) {
    return opened;
  }
  opened.value().parent_repository_path = startup_session.repository_path;
  opened.value().parent_session_id = startup_session.session_id;
  return opened;
}

bool session_file_has_header(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    return false;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
      continue;
    }

    try {
      const auto header = Json::parse(line);
      return session_id_from_header(header).has_value();
    } catch (const std::exception&) {
      return false;
    }
  }
  return false;
}

ReferenceStartupSessionResolution resolve_startup_session(
    const ReferenceCliArgs& parsed, const std::string& session_dir,
    const std::string& cwd) {
  ReferenceStartupSessionResolution resolution;

  if (parsed.no_session) {
    resolution.session.mode = "in_memory";
    resolution.session.persistent = false;
    resolution.session.working_directory = default_cwd();
    return resolution;
  }

  std::optional<std::string> repository_path;
  if (parsed.fork) {
    if (session_arg_looks_like_path(*parsed.fork)) {
      repository_path = *parsed.fork;
    } else {
      const auto resolved = session_dir_resolve_id_prefix(
          session_dir, *parsed.fork, cwd);
      if (!resolved) {
        resolution.error = resolved.error().message;
        return resolution;
      }
      repository_path = resolved.value();
    }
    const auto forked =
        fork_startup_session(*repository_path, session_dir);
    if (!forked) {
      resolution.error = forked.error().message;
      return resolution;
    }
    resolution.session = forked.value();
    return resolution;
  }

  if (parsed.session) {
    resolution.session.mode = "open";
    if (session_arg_looks_like_path(*parsed.session)) {
      repository_path = *parsed.session;
    } else {
      const auto resolved = session_dir_resolve_id_prefix(
          session_dir, *parsed.session, cwd);
      if (!resolved) {
        resolution.error = resolved.error().message;
        return resolution;
      }
      repository_path = resolved.value();
    }
  } else if (parsed.resume) {
    resolution.session.mode = "resume";
    repository_path = most_recent_session_path(session_dir, cwd);
    if (!repository_path) {
      resolution.stdout_message = "No session selected";
      return resolution;
    }
  } else if (parsed.continue_session) {
    resolution.session.mode = "continue";
    repository_path = most_recent_session_path(session_dir);
    if (!repository_path) {
      const auto created =
          create_startup_session(session_dir, "continue_new");
      if (!created) {
        resolution.error = created.error().message;
        return resolution;
      }
      resolution.session = created.value();
      return resolution;
    }
  } else {
    resolution.session.mode = "create";
    if (!session_dir.empty()) {
      const auto created = create_startup_session(session_dir, "create");
      if (!created) {
        resolution.error = created.error().message;
        return resolution;
      }
      resolution.session = created.value();
      return resolution;
    }
  }

  if (!repository_path) {
    return resolution;
  }

  const auto resumed = qt::harness::resume_session(*repository_path);
  if (!resumed) {
    resolution.error = resumed.error().message;
    return resolution;
  }

  resolution.session.repository_path = resumed.value().repository_path;
  resolution.session.session_id = resumed.value().metadata.session_id;
  resolution.session.working_directory =
      parsed.continue_session ? cwd : resumed.value().metadata.working_directory;
  resolution.session.prompt_history = resumed.value().metadata.prompt_history;
  resolution.session.entry_count = resumed.value().entries.size();
  return resolution;
}

foundation::Result<std::string> resolve_startup_session_dir(
    const ReferenceCliArgs& parsed, const std::string& cwd,
    const std::string& agent_home) {
  if (parsed.session_dir) {
    return foundation::Result<std::string>::success(*parsed.session_dir);
  }

  const std::string env_session_dir =
      env_or_empty("PI_CODING_AGENT_SESSION_DIR");
  if (!env_session_dir.empty()) {
    if (env_session_dir == "~" || env_session_dir.rfind("~/", 0) == 0) {
      const auto home = env_or_empty("HOME");
      if (!home.empty()) {
        return foundation::Result<std::string>::success(
            env_session_dir == "~" ? home : home + env_session_dir.substr(1));
      }
    }
    return foundation::Result<std::string>::success(env_session_dir);
  }

  qt::harness::FilesystemRepository repository(cwd);
  qt::app::LoadSettingsRequest request;
  request.cwd = cwd;
  request.agent_home = agent_home;
  const auto settings = qt::app::load_app_settings(repository, request);
  if (!settings) {
    return foundation::Result<std::string>::failure(
        settings.error().code, settings.error().message,
        settings.error().system_error);
  }

  return foundation::Result<std::string>::success(
      settings.value().session_dir);
}

std::optional<std::string> missing_session_lookup_arg(
    const ReferenceCliArgs& parsed, const std::string& session_dir) {
  if (session_dir.empty()) {
    return std::nullopt;
  }

  if (parsed.fork && !session_arg_looks_like_path(*parsed.fork) &&
      !session_dir_has_id_prefix(session_dir, *parsed.fork)) {
    return parsed.fork;
  }

  if (!parsed.no_session && parsed.session &&
      !session_arg_looks_like_path(*parsed.session) &&
      !session_dir_has_id_prefix(session_dir, *parsed.session)) {
    return parsed.session;
  }

  return std::nullopt;
}

qt::harness::SessionEntry repository_ready_session_entry(
    const qt::harness::SessionEntry& entry) {
  qt::harness::SessionEntry persisted = entry;
  auto* custom = std::get_if<qt::harness::SessionCustomEntry>(&persisted.data);
  if (custom == nullptr ||
      custom->entry_type != qt::harness::kUpstreamPiMonoSessionEntryType ||
      !custom->payload.is_object()) {
    return persisted;
  }

  if (custom->upstream.id) {
    custom->payload["id"] = *custom->upstream.id;
  }
  custom->payload["parentId"] =
      custom->upstream.parent_id ? Json(*custom->upstream.parent_id) : Json(nullptr);
  if (custom->upstream.timestamp) {
    custom->payload["timestamp"] = *custom->upstream.timestamp;
  }
  return persisted;
}

foundation::Status append_startup_turn(
    const std::string& repository_path, const qt::ai::Message& user_message,
    const qt::agent::TurnResult& turn) {
  if (repository_path.empty()) {
    return foundation::Status::success();
  }

  qt::harness::SessionRepository repository(repository_path);

  const auto appended_user = repository.append(
      make_session_message_entry(Json(user_message)));
  if (!appended_user) {
    return appended_user;
  }

  if (turn.response) {
    Json assistant_message = turn.response->message;
    if (turn.request) {
      assistant_message["api"] = turn.request->model.api;
      assistant_message["provider"] = turn.request->model.provider;
      assistant_message["model"] = turn.request->model.name;
    }
    assistant_message["usage"] = turn.response->usage;
    assistant_message["stopReason"] =
        qt::ai::to_string(turn.response->stop_reason);

    const auto appended_assistant = repository.append(
        make_session_message_entry(assistant_message));
    if (!appended_assistant) {
      return appended_assistant;
    }
  }

  return foundation::Status::success();
}

foundation::Status append_startup_turn(
    const std::string& repository_path, const NoninteractiveRunResult& run) {
  return append_startup_turn(repository_path, run.queued_message.message,
                                       run.turn);
}

foundation::Status append_startup_messages(
    const std::string& repository_path,
    const std::vector<qt::ai::Message>& messages) {
  if (repository_path.empty()) {
    return foundation::Status::success();
  }

  qt::harness::SessionRepository repository(repository_path);
  for (const auto& message : messages) {
    const auto appended =
        repository.append(make_session_message_entry(Json(message)));
    if (!appended) {
      return appended;
    }
  }
  return foundation::Status::success();
}

nlohmann::json session_header_json(
    const qt::harness::SessionMetadata& metadata) {
  Json json{{"type", "session"},
            {"version", metadata.source_version.value_or(3)},
            {"id", metadata.session_id},
            {"timestamp",
             metadata.created_at.value_or(qt::foundation::format_timestamp_utc(
                 qt::foundation::system_timestamp()))},
            {"cwd", metadata.working_directory}};
  if (metadata.parent_session_id) {
    json["parentSession"] = *metadata.parent_session_id;
  }
  return json;
}

}  // namespace qt::app
