#pragma once

#include <optional>
#include <string>

#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>
#include <qt/harness/session_graph.hpp>

namespace qt::harness {

struct PersistentSessionRecord {
  SessionMetadata metadata;
  std::string repository_path;
};

struct CreatePersistentSessionRequest {
  std::string session_id;
  std::string working_directory;
  std::string repository_path;
  std::optional<std::string> display_name;
  std::optional<std::string> created_at;
};

struct ForkSessionRequest {
  SessionBranchSnapshot parent_snapshot;
  std::string child_session_id;
  std::string child_repository_path;
  std::optional<std::string> child_working_directory;
  std::optional<std::string> child_display_name;
  std::optional<std::string> child_created_at;
};

qt::foundation::Result<PersistentSessionRecord> make_persistent_session_record(
    const CreatePersistentSessionRequest& request);

qt::foundation::Status append_initial_session_metadata(
    const PersistentSessionRecord& record);

qt::foundation::Result<PersistentSessionRecord> create_persistent_session(
    const CreatePersistentSessionRequest& request);

qt::foundation::Result<SessionBranchSnapshot> load_session_branch_snapshot(
    const std::string& repository_path);

qt::foundation::Result<PersistentSessionRecord> fork_child_session(
    const ForkSessionRequest& request);

}  // namespace qt::harness
