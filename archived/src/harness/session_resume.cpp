#include <qt/harness/session_resume.hpp>

#include <sstream>
#include <utility>

#include <qt/harness/session_repository.hpp>
#include <qt/harness/session_manager.hpp>

namespace qt::harness {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

Result<ResumedSessionState> validate_resumed_session(
    ResumedSessionState resumed) {
  if (resumed.repository_path.empty()) {
    return Result<ResumedSessionState>::failure(
        ErrorCode::invalid_argument, "repository_path is required");
  }
  if (resumed.snapshot.metadata.session_id.empty()) {
    return Result<ResumedSessionState>::failure(
        ErrorCode::invalid_argument, "session metadata is missing session_id");
  }

  resumed.metadata = resumed.snapshot.metadata;
  return resumed;
}

SessionEntry make_cloned_metadata_entry(const SessionMetadata& source_metadata,
                                        const std::string& cloned_session_id) {
  SessionMetadata cloned_metadata = source_metadata;
  cloned_metadata.session_id = cloned_session_id;
  cloned_metadata.parent_session_id = source_metadata.session_id;
  cloned_metadata.persistent = true;

  SessionEntry entry;
  entry.data = SessionMetadataEntry{cloned_metadata};
  return entry;
}

Status append_cloned_entries(const std::string& repository_path,
                             const std::vector<SessionEntry>& source_entries,
                             const std::string& cloned_session_id,
                             const SessionMetadata& source_metadata) {
  SessionRepository repository(repository_path);

  const Status metadata_status = repository.append(
      make_cloned_metadata_entry(source_metadata, cloned_session_id));
  if (!metadata_status) {
    return metadata_status;
  }

  for (const auto& entry : source_entries) {
    if (std::holds_alternative<SessionMetadataEntry>(entry.data)) {
      continue;
    }

    const Status append_status = repository.append(entry);
    if (!append_status) {
      return append_status;
    }
  }

  return Status::success();
}

void append_summary_line(std::vector<std::string>& lines,
                         const std::string& name,
                         const std::string& value) {
  lines.push_back(name + ": " + value);
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

std::string count_text(std::size_t value) {
  std::ostringstream output;
  output << value;
  return output.str();
}

}  // namespace

Result<ResumedSessionState> resume_session(const std::string& repository_path) {
  if (repository_path.empty()) {
    return Result<ResumedSessionState>::failure(
        ErrorCode::invalid_argument, "repository_path is required");
  }

  const auto entries = SessionRepository(repository_path).load();
  if (!entries) {
    return entries.error();
  }

  const auto snapshot = derive_session_branch_snapshot(entries.value());
  if (!snapshot) {
    return snapshot.error();
  }

  ResumedSessionState resumed;
  resumed.repository_path = repository_path;
  resumed.entries = entries.value();
  resumed.snapshot = snapshot.value();
  return validate_resumed_session(std::move(resumed));
}

Result<ResumeSessionSummary> render_resume_session_summary(
    const ResumedSessionState& resumed) {
  const auto validated = validate_resumed_session(resumed);
  if (!validated) {
    return validated.error();
  }

  ResumeSessionSummary summary;
  summary.entry_count = validated.value().entries.size();
  summary.has_display_name =
      validated.value().metadata.display_name.has_value();
  summary.has_parent_session =
      validated.value().metadata.parent_session_id.has_value();
  summary.has_created_at = validated.value().metadata.created_at.has_value();
  summary.has_latest_label =
      validated.value().snapshot.latest_label.has_value();
  summary.has_latest_summary =
      validated.value().snapshot.latest_summary.has_value();

  append_summary_line(summary.lines, "path", validated.value().repository_path);
  append_summary_line(summary.lines, "session_id",
                      validated.value().metadata.session_id);
  append_summary_line(summary.lines, "working_directory",
                      validated.value().metadata.working_directory);
  append_summary_line(summary.lines, "persistent",
                      bool_text(validated.value().metadata.persistent));
  append_summary_line(summary.lines, "entries",
                      count_text(summary.entry_count));

  if (validated.value().metadata.display_name.has_value()) {
    append_summary_line(summary.lines, "display_name",
                        *validated.value().metadata.display_name);
  }
  if (validated.value().metadata.parent_session_id.has_value()) {
    append_summary_line(summary.lines, "parent_session_id",
                        *validated.value().metadata.parent_session_id);
  }
  if (validated.value().metadata.created_at.has_value()) {
    append_summary_line(summary.lines, "created_at",
                        *validated.value().metadata.created_at);
  }
  if (validated.value().snapshot.latest_label.has_value()) {
    append_summary_line(summary.lines, "latest_label",
                        *validated.value().snapshot.latest_label);
  }
  if (validated.value().snapshot.latest_summary.has_value()) {
    append_summary_line(summary.lines, "latest_summary",
                        *validated.value().snapshot.latest_summary);
  }

  return summary;
}

Result<PersistentSessionRecord> clone_session_repository(
    const CloneSessionRepositoryRequest& request) {
  if (request.source_repository_path.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument, "source_repository_path is required");
  }
  if (request.cloned_session_id.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument, "cloned_session_id is required");
  }
  if (request.cloned_repository_path.empty()) {
    return Result<PersistentSessionRecord>::failure(
        ErrorCode::invalid_argument, "cloned_repository_path is required");
  }

  const auto source = resume_session(request.source_repository_path);
  if (!source) {
    return source.error();
  }

  PersistentSessionRecord cloned;
  cloned.metadata = source.value().metadata;
  cloned.metadata.session_id = request.cloned_session_id;
  cloned.metadata.parent_session_id = source.value().metadata.session_id;
  cloned.metadata.persistent = true;
  cloned.repository_path = request.cloned_repository_path;

  const auto validated = make_persistent_session_record(
      CreatePersistentSessionRequest{
          cloned.metadata.session_id,
          cloned.metadata.working_directory,
          cloned.repository_path,
          cloned.metadata.display_name,
          cloned.metadata.created_at,
      });
  if (!validated) {
    return validated.error();
  }

  cloned.metadata = validated.value().metadata;
  cloned.metadata.parent_session_id = source.value().metadata.session_id;

  const qt::harness::SessionManager manager(source.value().entries);
  const auto branch_entries = manager.get_branch();

  const Status append_status =
      append_cloned_entries(cloned.repository_path, branch_entries,
                            cloned.metadata.session_id, source.value().metadata);
  if (!append_status) {
    return append_status.error();
  }

  return cloned;
}

}  // namespace qt::harness
