#pragma once

#include <optional>
#include <string>
#include <vector>

#include <qt/app/runtime/runtime_host.hpp>
#include <qt/foundation/error.hpp>
#include <qt/harness/session_manager.hpp>

namespace qt::app {

struct ReferenceSessionTreeItem {
  std::string entry_id;
  std::string label;
  bool selected = false;
};

struct ReferenceSessionTreeSelection {
  std::string target_entry_id;
  std::optional<std::string> next_leaf_id;
  std::optional<std::string> editor_text;
  bool reset_to_root = false;
};

struct ReferenceSessionTreeSummaryOptions {
  bool summarize = false;
  std::optional<std::string> custom_instructions;
  bool replace_instructions = false;
};

struct ReferenceSessionTreeNavigationResult {
  std::optional<std::string> editor_text;
  std::optional<std::string> summary_entry_id;
};

qt::foundation::Result<std::vector<ReferenceSessionTreeItem>>
build_session_tree_items(
    const qt::harness::SessionManager& session_manager,
    const std::optional<std::string>& selection_hint = std::nullopt);

qt::foundation::Result<ReferenceSessionTreeSelection>
make_session_tree_selection(
    const qt::harness::SessionManager& session_manager,
    const std::string& entry_id);

qt::foundation::Result<ReferenceSessionTreeNavigationResult>
apply_session_tree_selection(
    RuntimeHost& host, const ReferenceSessionTreeSelection& selection,
    ReferenceSessionTreeSummaryOptions options = {});

}  // namespace qt::app
