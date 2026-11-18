#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>
#include <qt/harness/session_graph.hpp>
#include <qt/harness/session_lifecycle.hpp>

namespace qt::harness {

struct ResumedSessionState {
  std::string repository_path;
  SessionMetadata metadata;
  std::vector<SessionEntry> entries;
  SessionBranchSnapshot snapshot;
};

struct ResumeSessionSummary {
  std::vector<std::string> lines;
  std::size_t entry_count = 0;
  bool has_display_name = false;
  bool has_parent_session = false;
  bool has_created_at = false;
  bool has_latest_label = false;
  bool has_latest_summary = false;
};

struct CloneSessionRepositoryRequest {
  std::string source_repository_path;
  std::string cloned_session_id;
  std::string cloned_repository_path;
};

qt::foundation::Result<ResumedSessionState> resume_session(
    const std::string& repository_path);

qt::foundation::Result<ResumeSessionSummary> render_resume_session_summary(
    const ResumedSessionState& resumed);

qt::foundation::Result<PersistentSessionRecord> clone_session_repository(
    const CloneSessionRepositoryRequest& request);

}  // namespace qt::harness
