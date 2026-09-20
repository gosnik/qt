#include <qt/harness/session_lifecycle.hpp>

#include <utility>

#include <qt/harness/session_migration.hpp>
#include <qt/harness/session_repository.hpp>

namespace qt::harness {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

std::optional<std::string> normalize_optional_text(
    const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return value;
}

Result<PersistentSessionRecord> validate_record(PersistentSessionRecord record) {
  if (record.metadata.session_id.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument, "session_id is required");
  }
  if (record.metadata.working_directory.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument, "working_directory is required");
  }
  if (record.repository_path.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument, "repository_path is required");
  }

  record.metadata.display_name =
      normalize_optional_text(record.metadata.display_name);
  record.metadata.parent_session_id =
      normalize_optional_text(record.metadata.parent_session_id);
  record.metadata.created_at = normalize_optional_text(record.metadata.created_at);
  record.metadata.persistent = true;
  if (!record.metadata.source_format.has_value()) {
    record.metadata.source_format = kUpstreamPiMonoSessionEntryType;
  }
  if (!record.metadata.source_version.has_value()) {
    record.metadata.source_version = kPiMonoCurrentSessionVersion;
  }

  return record;
}

}  // namespace

Result<PersistentSessionRecord> make_persistent_session_record(
    const CreatePersistentSessionRequest& request) {
  PersistentSessionRecord record;
  record.metadata.session_id = request.session_id;
  record.metadata.working_directory = request.working_directory;
  record.metadata.display_name = request.display_name;
  record.metadata.created_at = request.created_at;
  record.metadata.persistent = true;
  record.repository_path = request.repository_path;

  return validate_record(std::move(record));
}

Status append_initial_session_metadata(const PersistentSessionRecord& record) {
  const auto validated = validate_record(record);
  if (!validated) {
    return Status(validated.error());
  }

  SessionEntry entry;
  entry.data = SessionMetadataEntry{validated.value().metadata};

  return SessionRepository(validated.value().repository_path).append(entry);
}

Result<PersistentSessionRecord> create_persistent_session(
    const CreatePersistentSessionRequest& request) {
  auto record = make_persistent_session_record(request);
  if (!record) {
    return record.error();
  }

  const Status appended = append_initial_session_metadata(record.value());
  if (!appended) {
    return appended.error();
  }

  return record.value();
}

Result<SessionBranchSnapshot> load_session_branch_snapshot(
    const std::string& repository_path) {
  if (repository_path.empty()) {
    return Result<SessionBranchSnapshot>::failure(
        ErrorCode::invalid_argument, "repository_path is required");
  }

  const auto entries = SessionRepository(repository_path).load();
  if (!entries) {
    return entries.error();
  }

  return derive_session_branch_snapshot(entries.value());
}

Result<PersistentSessionRecord> fork_child_session(
    const ForkSessionRequest& request) {
  if (request.parent_snapshot.metadata.session_id.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument, "parent snapshot is missing session_id");
  }
  if (request.parent_snapshot.metadata.working_directory.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument,
        "parent snapshot is missing working_directory");
  }

  CreatePersistentSessionRequest create_request;
  create_request.session_id = request.child_session_id;
  create_request.working_directory =
      request.child_working_directory.value_or(
          request.parent_snapshot.metadata.working_directory);
  create_request.repository_path = request.child_repository_path;
  create_request.display_name = request.child_display_name;
  create_request.created_at = request.child_created_at;

  auto record = make_persistent_session_record(create_request);
  if (!record) {
    return record.error();
  }

  record.value().metadata.parent_session_id =
      request.parent_snapshot.metadata.session_id;

  const auto validated = validate_record(record.value());
  if (!validated) {
    return validated.error();
  }

  const Status appended = append_initial_session_metadata(validated.value());
  if (!appended) {
    return appended.error();
  }

  return validated.value();
}

}  // namespace qt::harness
