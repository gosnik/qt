#include <qt/harness/session_graph.hpp>

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace qt::harness {
namespace {

std::optional<std::string> normalize_optional_text(
    const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return value;
}

std::string make_tree_display_text(const SessionBranchSnapshot& session,
                                   const std::vector<bool>& ancestor_continues,
                                   bool is_last, bool selected, bool orphaned) {
  std::ostringstream output;
  output << (selected ? "> " : "  ");

  for (const bool continues : ancestor_continues) {
    output << (continues ? "|  " : "   ");
  }

  if (!ancestor_continues.empty()) {
    output << (is_last ? "`- " : "|- ");
  } else {
    output << "- ";
  }

  output << session.metadata.session_id;

  if (session.metadata.display_name.has_value()) {
    output << " (" << *session.metadata.display_name << ")";
  }
  if (session.metadata.created_at.has_value()) {
    output << " created=" << *session.metadata.created_at;
  }
  if (session.latest_label.has_value()) {
    output << " label=" << *session.latest_label;
  }
  if (session.latest_summary.has_value()) {
    output << " summary=" << *session.latest_summary;
  }
  if (orphaned && session.metadata.parent_session_id.has_value()) {
    output << " missing_parent=" << *session.metadata.parent_session_id;
  }
  return output.str();
}

}  // namespace

const SessionGraphNode* SessionGraph::find(std::string_view session_id) const
    noexcept {
  for (const auto& node : nodes) {
    if (node.session.metadata.session_id == session_id) {
      return &node;
    }
  }
  return nullptr;
}

qt::foundation::Result<SessionBranchSnapshot> derive_session_branch_snapshot(
    const std::vector<SessionEntry>& entries) {
  std::optional<SessionBranchSnapshot> snapshot;

  for (const auto& entry : entries) {
    if (const auto* metadata =
            std::get_if<SessionMetadataEntry>(&entry.data)) {
      if (!snapshot.has_value()) {
        snapshot = SessionBranchSnapshot{metadata->metadata, std::nullopt,
                                         std::nullopt};
      } else {
        if (snapshot->metadata.session_id != metadata->metadata.session_id) {
          return qt::foundation::Result<SessionBranchSnapshot>::failure(
              qt::foundation::ErrorCode::invalid_argument,
              "session entries contain conflicting session ids");
        }
        snapshot->metadata = metadata->metadata;
      }
      snapshot->metadata.parent_session_id =
          normalize_optional_text(snapshot->metadata.parent_session_id);
      snapshot->metadata.display_name =
          normalize_optional_text(snapshot->metadata.display_name);
      snapshot->metadata.created_at =
          normalize_optional_text(snapshot->metadata.created_at);
      continue;
    }

    if (!snapshot.has_value()) {
      continue;
    }

    if (const auto* label = std::get_if<SessionLabelEntry>(&entry.data)) {
      snapshot->latest_label =
          label->label.empty() ? std::nullopt
                               : std::optional<std::string>(label->label);
      continue;
    }

    if (const auto* summary = std::get_if<SessionSummaryEntry>(&entry.data)) {
      snapshot->latest_summary =
          summary->summary.empty()
              ? std::nullopt
              : std::optional<std::string>(summary->summary);
    }
  }

  if (!snapshot.has_value()) {
    return qt::foundation::Result<SessionBranchSnapshot>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "session entries did not include session metadata");
  }

  if (snapshot->metadata.session_id.empty()) {
    return qt::foundation::Result<SessionBranchSnapshot>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "session metadata is missing session_id");
  }

  return *snapshot;
}

qt::foundation::Result<SessionGraph> build_session_graph(
    const std::vector<SessionBranchSnapshot>& sessions) {
  SessionGraph graph;
  std::unordered_map<std::string, std::size_t> node_indexes;

  graph.nodes.reserve(sessions.size());
  for (const auto& session : sessions) {
    if (session.metadata.session_id.empty()) {
      return qt::foundation::Result<SessionGraph>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "session graph cannot include an empty session_id");
    }

    const auto inserted = node_indexes.emplace(session.metadata.session_id,
                                               graph.nodes.size());
    if (!inserted.second) {
      return qt::foundation::Result<SessionGraph>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "session graph contains duplicate session_id: " +
              session.metadata.session_id);
    }

    graph.nodes.push_back(SessionGraphNode{session, {}});
    graph.nodes.back().session.metadata.parent_session_id =
        normalize_optional_text(graph.nodes.back().session.metadata.parent_session_id);
  }

  for (const auto& node : graph.nodes) {
    if (!node.session.metadata.parent_session_id.has_value()) {
      graph.root_session_ids.push_back(node.session.metadata.session_id);
    }
  }

  for (const auto& node : graph.nodes) {
    if (!node.session.metadata.parent_session_id.has_value()) {
      continue;
    }

    const auto parent = node_indexes.find(*node.session.metadata.parent_session_id);
    if (parent == node_indexes.end()) {
      if (graph.unresolved_parent_session_ids.empty() ||
          graph.unresolved_parent_session_ids.back() !=
              *node.session.metadata.parent_session_id) {
        const auto already_listed = std::find(
            graph.unresolved_parent_session_ids.begin(),
            graph.unresolved_parent_session_ids.end(),
            *node.session.metadata.parent_session_id);
        if (already_listed == graph.unresolved_parent_session_ids.end()) {
          graph.unresolved_parent_session_ids.push_back(
              *node.session.metadata.parent_session_id);
        }
      }
      continue;
    }

    graph.nodes[parent->second].child_session_ids.push_back(
        node.session.metadata.session_id);
  }

  return graph;
}

qt::foundation::Result<std::vector<SessionTreeRow>> render_session_tree(
    const SessionGraph& graph, const SessionTreeRenderOptions& options) {
  if (options.selected_session_id.has_value() &&
      options.selected_session_id->empty()) {
    return qt::foundation::Result<std::vector<SessionTreeRow>>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "selected_session_id cannot be empty");
  }

  if (options.selected_session_id.has_value() &&
      graph.find(*options.selected_session_id) == nullptr) {
    return qt::foundation::Result<std::vector<SessionTreeRow>>::failure(
        qt::foundation::ErrorCode::not_found,
        "selected session was not found: " + *options.selected_session_id);
  }

  std::unordered_set<std::string> folded_session_ids;
  for (const auto& session_id : options.folded_session_ids) {
    if (session_id.empty()) {
      return qt::foundation::Result<std::vector<SessionTreeRow>>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "folded_session_ids cannot contain empty session ids");
    }
    if (graph.find(session_id) == nullptr) {
      return qt::foundation::Result<std::vector<SessionTreeRow>>::failure(
          qt::foundation::ErrorCode::not_found,
          "folded session was not found: " + session_id);
    }
    folded_session_ids.insert(session_id);
  }

  std::vector<SessionTreeRow> rows;
  std::unordered_set<std::string> visited;

  const auto mark_descendants_visited =
      [&](const auto& self, const std::string& session_id) -> void {
    const SessionGraphNode* node = graph.find(session_id);
    if (node == nullptr) {
      return;
    }
    for (const auto& child_session_id : node->child_session_ids) {
      if (visited.insert(child_session_id).second) {
        self(self, child_session_id);
      }
    }
  };

  const auto append_subtree = [&](const auto& self,
                                  const std::string& session_id,
                                  std::size_t depth,
                                  std::vector<bool> ancestor_continues,
                                  bool is_last,
                                  bool orphaned) -> qt::foundation::Status {
    if (!visited.insert(session_id).second) {
      return qt::foundation::Status::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "session graph contains a cycle at session_id: " + session_id);
    }

    const SessionGraphNode* node = graph.find(session_id);
    if (node == nullptr) {
      return qt::foundation::Status::failure(
          qt::foundation::ErrorCode::not_found,
          "session graph references missing session_id: " + session_id);
    }

    const bool selected = options.selected_session_id.has_value() &&
                          *options.selected_session_id == session_id;
    const bool folded =
        folded_session_ids.find(session_id) != folded_session_ids.end();
    SessionTreeRow row;
    row.row_number = rows.size() + 1;
    row.depth = depth;
    row.session_id = node->session.metadata.session_id;
    row.parent_session_id = node->session.metadata.parent_session_id;
    row.selected = selected;
    row.orphaned = orphaned;
    row.folded = folded;
    row.display_text =
        make_tree_display_text(node->session, ancestor_continues, is_last,
                               selected, orphaned);
    rows.push_back(std::move(row));

    if (folded) {
      mark_descendants_visited(mark_descendants_visited, session_id);
      return qt::foundation::Status::success();
    }

    ancestor_continues.push_back(!is_last);
    for (std::size_t index = 0; index < node->child_session_ids.size(); ++index) {
      const auto& child_session_id = node->child_session_ids[index];
      const qt::foundation::Status child_status =
          self(self, child_session_id, depth + 1, ancestor_continues,
               index + 1 == node->child_session_ids.size(), false);
      if (!child_status) {
        return child_status;
      }
    }
    return qt::foundation::Status::success();
  };

  for (std::size_t index = 0; index < graph.root_session_ids.size(); ++index) {
    const auto& root_session_id = graph.root_session_ids[index];
    const qt::foundation::Status root_status =
        append_subtree(append_subtree, root_session_id, 0, {},
                       index + 1 == graph.root_session_ids.size(), false);
    if (!root_status) {
      return root_status.error();
    }
  }

  if (options.include_orphaned_sessions) {
    for (const auto& node : graph.nodes) {
      if (visited.find(node.session.metadata.session_id) != visited.end()) {
        continue;
      }
      const qt::foundation::Status orphan_status =
          append_subtree(append_subtree, node.session.metadata.session_id, 0,
                         {}, true, true);
      if (!orphan_status) {
        return orphan_status.error();
      }
    }
  }

  return rows;
}

}  // namespace qt::harness
