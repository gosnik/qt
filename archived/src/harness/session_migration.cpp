#include <qt/harness/session_migration.hpp>

#include <sstream>
#include <utility>

#include <qt/harness/session_graph.hpp>
#include <qt/harness/session_repository.hpp>

namespace qt::harness {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

bool is_upstream_custom(const SessionEntry& entry) {
  const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
  return custom != nullptr &&
         custom->entry_type == kUpstreamPiMonoSessionEntryType &&
         custom->payload.is_object() && custom->payload.contains("type") &&
         custom->payload.at("type").is_string();
}

const SessionUpstreamTreeFields* typed_upstream_fields(const SessionEntry& entry) {
  return std::visit(
      [](const auto& value) -> const SessionUpstreamTreeFields* {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, SessionMetadataEntry> ||
                      std::is_same_v<Value, SessionSummaryEntry> ||
                      std::is_same_v<Value, SessionCustomEntry>) {
          return nullptr;
        } else {
          return value.upstream.source_format == kUpstreamPiMonoSessionEntryType
                     ? &value.upstream
                     : nullptr;
        }
      },
      entry.data);
}

SessionUpstreamTreeFields* typed_upstream_fields(SessionEntry& entry) {
  return std::visit(
      [](auto& value) -> SessionUpstreamTreeFields* {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, SessionMetadataEntry> ||
                      std::is_same_v<Value, SessionSummaryEntry> ||
                      std::is_same_v<Value, SessionCustomEntry>) {
          return nullptr;
        } else {
          return value.upstream.source_format == kUpstreamPiMonoSessionEntryType
                     ? &value.upstream
                     : nullptr;
        }
      },
      entry.data);
}

bool session_looks_upstream(const std::vector<SessionEntry>& entries) {
  for (const auto& entry : entries) {
    if (is_upstream_custom(entry)) {
      return true;
    }
    if (const auto* metadata = std::get_if<SessionMetadataEntry>(&entry.data);
        metadata != nullptr &&
        metadata->metadata.source_format == kUpstreamPiMonoSessionEntryType) {
      return true;
    }
  }
  return false;
}

int source_version_for_entries(const std::vector<SessionEntry>& entries) {
  for (const auto& entry : entries) {
    const auto* metadata = std::get_if<SessionMetadataEntry>(&entry.data);
    if (metadata == nullptr) {
      continue;
    }
    if (metadata->metadata.source_version.has_value()) {
      return *metadata->metadata.source_version;
    }
    if (metadata->metadata.source_format == kUpstreamPiMonoSessionEntryType) {
      return 1;
    }
  }
  return kPiMonoCurrentSessionVersion;
}

std::string deterministic_migrated_id(std::size_t entry_number) {
  std::ostringstream output;
  output << "migrated-";
  output.width(8);
  output.fill('0');
  output << entry_number;
  return output.str();
}

std::optional<std::string> upstream_entry_id(const Json& payload) {
  if (!payload.contains("id") || !payload.at("id").is_string() ||
      payload.at("id").get<std::string>().empty()) {
    return std::nullopt;
  }
  return payload.at("id").get<std::string>();
}

std::size_t count_upstream_entries(const std::vector<SessionEntry>& entries) {
  std::size_t count = 0;
  for (const auto& entry : entries) {
    if (is_upstream_custom(entry) || typed_upstream_fields(entry) != nullptr) {
      ++count;
    }
  }
  return count;
}

void add_step(SessionMigrationPlan& plan, std::string name, int from_version,
              int to_version, std::size_t affected_entry_count) {
  plan.steps.push_back(SessionMigrationStep{std::move(name), from_version,
                                            to_version, affected_entry_count});
  plan.migration_required = true;
}

void migrate_metadata(SessionMetadata& metadata, int target_version,
                      const std::optional<std::string>& cwd_override) {
  if (cwd_override.has_value() && !cwd_override->empty()) {
    metadata.working_directory = *cwd_override;
  }
  if (metadata.source_format == kUpstreamPiMonoSessionEntryType) {
    metadata.source_version = target_version;
  }
}

void append_native_projection_for_upstream_payload(
    const Json& payload, const SessionMetadata& current_metadata,
    std::vector<SessionEntry>& migrated_entries) {
  const std::string type = payload.value("type", std::string{});

  if (type == "label" && payload.contains("label") &&
      payload.at("label").is_string()) {
    SessionEntry label_entry;
    label_entry.data = SessionLabelEntry{payload.at("label").get<std::string>()};
    migrated_entries.push_back(std::move(label_entry));
    return;
  }

  if (type == "session_info") {
    SessionMetadata renamed_metadata = current_metadata;
    if (payload.contains("name") && payload.at("name").is_string()) {
      const std::string name = payload.at("name").get<std::string>();
      renamed_metadata.display_name =
          name.empty() ? std::nullopt : std::optional<std::string>(name);
    } else {
      renamed_metadata.display_name.reset();
    }

    SessionEntry metadata_entry;
    metadata_entry.data = SessionMetadataEntry{std::move(renamed_metadata)};
    migrated_entries.push_back(std::move(metadata_entry));
    return;
  }

  if (type == "compaction" && payload.contains("summary") &&
      payload.at("summary").is_string()) {
    SessionEntry summary_entry;
    summary_entry.data =
        SessionSummaryEntry{payload.at("summary").get<std::string>()};
    migrated_entries.push_back(std::move(summary_entry));
  }
}

Result<PersistentSessionRecord> record_from_entries(
    const std::vector<SessionEntry>& entries,
    const std::string& repository_path) {
  const auto snapshot = derive_session_branch_snapshot(entries);
  if (!snapshot) {
    return snapshot.error();
  }

  PersistentSessionRecord record;
  record.metadata = snapshot.value().metadata;
  record.repository_path = repository_path;
  return record;
}

}  // namespace

Result<SessionMigrationPlan> plan_session_migration(
    const std::vector<SessionEntry>& entries,
    const std::optional<std::string>& working_directory_override) {
  if (entries.empty()) {
    return Result<SessionMigrationPlan>::failure(ErrorCode::invalid_argument,
                                                 "session entries are required");
  }

  SessionMigrationPlan plan;
  plan.migrated_entries.reserve(entries.size());

  const bool upstream = session_looks_upstream(entries);
  plan.source_format =
      upstream ? kUpstreamPiMonoSessionEntryType : std::string("native.qt");
  plan.source_version = source_version_for_entries(entries);
  plan.target_version = upstream ? kPiMonoCurrentSessionVersion
                                 : kSessionEntryVersion;

  if (upstream && plan.source_version < 2) {
    add_step(plan, "upstream.v1.add_tree_ids", 1, 2,
             count_upstream_entries(entries));
  }
  if (upstream && plan.source_version < 3) {
    add_step(plan, "upstream.v2.rename_hook_message_role", 2, 3,
             count_upstream_entries(entries));
  }
  if (working_directory_override.has_value() &&
      !working_directory_override->empty()) {
    add_step(plan, "override_working_directory", plan.target_version,
             plan.target_version, 1);
  }

  std::vector<std::optional<std::string>> ids_by_entry_index(entries.size());
  std::size_t upstream_entry_number = 0;
  std::optional<std::string> previous_upstream_id;
  SessionMetadata current_metadata;

  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    if (const auto* metadata = std::get_if<SessionMetadataEntry>(&entry.data)) {
      SessionMetadata migrated_metadata = metadata->metadata;
      migrate_metadata(migrated_metadata, plan.target_version,
                       working_directory_override);
      current_metadata = migrated_metadata;

      SessionEntry migrated_entry;
      migrated_entry.version = entry.version;
      migrated_entry.data =
          SessionMetadataEntry{std::move(migrated_metadata)};
      plan.migrated_entries.push_back(std::move(migrated_entry));
      continue;
    }

    if (!is_upstream_custom(entry)) {
      SessionEntry migrated_entry = entry;
      if (SessionUpstreamTreeFields* upstream =
              typed_upstream_fields(migrated_entry);
          upstream != nullptr) {
        ++upstream_entry_number;

        std::optional<std::string> id = upstream->id;
        if (!id.has_value() && plan.source_version < 2) {
          id = deterministic_migrated_id(upstream_entry_number);
          upstream->id = *id;
        }
        ids_by_entry_index[index] = id;

        if (plan.source_version < 2 && !upstream->parent_id.has_value()) {
          upstream->parent_id = previous_upstream_id;
        }

        if (id.has_value()) {
          previous_upstream_id = id;
        }
      }

      plan.migrated_entries.push_back(std::move(migrated_entry));
      continue;
    }

    const auto& custom = std::get<SessionCustomEntry>(entry.data);
    Json payload = custom.payload;
    ++upstream_entry_number;

    std::optional<std::string> id = upstream_entry_id(payload);
    if (!id.has_value() && plan.source_version < 2) {
      id = deterministic_migrated_id(upstream_entry_number);
      payload["id"] = *id;
    }
    ids_by_entry_index[index] = id;

    if (plan.source_version < 2 &&
        (!payload.contains("parentId") || payload.at("parentId").is_null())) {
      payload["parentId"] =
          previous_upstream_id.has_value() ? Json(*previous_upstream_id)
                                           : Json(nullptr);
    }

    if (plan.source_version < 3 &&
        payload.value("type", std::string{}) == "message" &&
        payload.contains("message") && payload.at("message").is_object() &&
        payload.at("message").value("role", std::string{}) == "hookMessage") {
      payload["message"]["role"] = "custom";
    }

    if (plan.source_version < 2 &&
        payload.value("type", std::string{}) == "compaction" &&
        payload.contains("firstKeptEntryIndex") &&
        payload.at("firstKeptEntryIndex").is_number_integer()) {
      const std::size_t first_kept_index =
          payload.at("firstKeptEntryIndex").get<std::size_t>();
      if (first_kept_index < ids_by_entry_index.size() &&
          ids_by_entry_index[first_kept_index].has_value()) {
        payload["firstKeptEntryId"] = *ids_by_entry_index[first_kept_index];
        payload.erase("firstKeptEntryIndex");
      } else {
        plan.diagnostics.push_back(
            "compaction firstKeptEntryIndex could not be resolved");
      }
    }

    if (id.has_value()) {
      previous_upstream_id = id;
    }

    SessionEntry migrated_entry;
    migrated_entry.version = entry.version;
    migrated_entry.data =
        SessionCustomEntry{kUpstreamPiMonoSessionEntryType, payload};
    plan.migrated_entries.push_back(std::move(migrated_entry));
    append_native_projection_for_upstream_payload(payload, current_metadata,
                                                  plan.migrated_entries);
  }

  return plan;
}

Result<ImportSessionRepositoryResult> import_session_repository(
    const ImportSessionRepositoryRequest& request) {
  if (request.source_repository_path.empty()) {
    return Result<ImportSessionRepositoryResult>::failure(
        ErrorCode::invalid_argument, "source_repository_path is required");
  }
  if (request.destination_repository_path.empty()) {
    return Result<ImportSessionRepositoryResult>::failure(
        ErrorCode::invalid_argument, "destination_repository_path is required");
  }

  const auto loaded =
      SessionRepository(request.source_repository_path).load();
  if (!loaded) {
    return loaded.error();
  }

  auto plan = plan_session_migration(
      loaded.value(), request.working_directory_override);
  if (!plan) {
    return plan.error();
  }

  const std::vector<SessionEntry>& entries_to_write =
      request.apply_migrations ? plan.value().migrated_entries : loaded.value();
  const Status write_status =
      SessionRepository(request.destination_repository_path)
          .replace_all(entries_to_write);
  if (!write_status) {
    return write_status.error();
  }

  auto record = record_from_entries(entries_to_write,
                                    request.destination_repository_path);
  if (!record) {
    return record.error();
  }

  ImportSessionRepositoryResult result;
  result.record = record.value();
  result.imported_entry_count = entries_to_write.size();
  result.migration_plan = std::move(plan.value());
  return result;
}

Result<ExportSessionRepositoryResult> export_session_repository(
    const ExportSessionRepositoryRequest& request) {
  if (request.source_repository_path.empty()) {
    return Result<ExportSessionRepositoryResult>::failure(
        ErrorCode::invalid_argument, "source_repository_path is required");
  }
  if (request.destination_repository_path.empty()) {
    return Result<ExportSessionRepositoryResult>::failure(
        ErrorCode::invalid_argument, "destination_repository_path is required");
  }

  const auto loaded =
      SessionRepository(request.source_repository_path).load();
  if (!loaded) {
    return loaded.error();
  }

  const Status write_status =
      SessionRepository(request.destination_repository_path)
          .replace_all(loaded.value());
  if (!write_status) {
    return write_status.error();
  }

  ExportSessionRepositoryResult result;
  result.exported_entry_count = loaded.value().size();
  return result;
}

}  // namespace qt::harness
