#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>

namespace qt::harness {

struct SessionBranchSnapshot {
  SessionMetadata metadata;
  std::optional<std::string> latest_label;
  std::optional<std::string> latest_summary;
};

struct SessionGraphNode {
  SessionBranchSnapshot session;
  std::vector<std::string> child_session_ids;
};

struct SessionGraph {
  std::vector<SessionGraphNode> nodes;
  std::vector<std::string> root_session_ids;
  std::vector<std::string> unresolved_parent_session_ids;

  const SessionGraphNode* find(std::string_view session_id) const noexcept;
};

struct SessionTreeRenderOptions {
  std::optional<std::string> selected_session_id;
  bool include_orphaned_sessions = true;
  std::vector<std::string> folded_session_ids;
};

struct SessionTreeRow {
  std::size_t row_number = 0;
  std::size_t depth = 0;
  std::string session_id;
  std::optional<std::string> parent_session_id;
  std::string display_text;
  bool selected = false;
  bool orphaned = false;
  bool folded = false;
};

qt::foundation::Result<SessionBranchSnapshot> derive_session_branch_snapshot(
    const std::vector<SessionEntry>& entries);

qt::foundation::Result<SessionGraph> build_session_graph(
    const std::vector<SessionBranchSnapshot>& sessions);

qt::foundation::Result<std::vector<SessionTreeRow>> render_session_tree(
    const SessionGraph& graph,
    const SessionTreeRenderOptions& options = SessionTreeRenderOptions{});

}  // namespace qt::harness
