#include "qt/app/sessions/session_tree.hpp"

#include <algorithm>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace qt::app {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::harness::SessionAssistantMessageEntry;
using qt::harness::SessionCustomMessageEntry;
using qt::harness::SessionEntry;
using qt::harness::SessionEntryTreeNode;
using qt::harness::SessionToolResultMessageEntry;
using qt::harness::SessionUpstreamTreeFields;
using qt::harness::SessionUserContentBlock;
using qt::harness::SessionUserContentBlockKind;
using qt::harness::SessionUserImageMessageEntry;

std::optional<std::string> entry_id_of(const SessionEntry& entry) {
  return std::visit(
      [](const auto& value) -> std::optional<std::string> {
        using Entry = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Entry, qt::harness::SessionMetadataEntry> ||
                      std::is_same_v<Entry, qt::harness::SessionSummaryEntry>) {
          return std::nullopt;
        } else {
          return value.upstream.id;
        }
      },
      entry.data);
}

std::optional<std::string> parent_id_of(const SessionEntry& entry) {
  return std::visit(
      [](const auto& value) -> std::optional<std::string> {
        using Entry = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Entry, qt::harness::SessionMetadataEntry> ||
                      std::is_same_v<Entry, qt::harness::SessionSummaryEntry>) {
          return std::nullopt;
        } else {
          return value.upstream.parent_id;
        }
      },
      entry.data);
}

std::string flatten_user_blocks(const std::vector<SessionUserContentBlock>& blocks) {
  std::string text;
  for (const auto& block : blocks) {
    if (block.kind == SessionUserContentBlockKind::kText) {
      text += block.text;
    }
  }
  return text;
}

std::string flatten_custom_message_text(
    const qt::harness::SessionCustomMessageContent& content) {
  if (const auto* text = std::get_if<std::string>(&content)) {
    return *text;
  }
  return flatten_user_blocks(std::get<std::vector<SessionUserContentBlock>>(content));
}

std::string first_line_truncated(std::string text, std::size_t limit = 72U) {
  const auto newline = text.find('\n');
  if (newline != std::string::npos) {
    text = text.substr(0, newline);
  }
  if (text.size() > limit) {
    text = text.substr(0, limit - 3U) + "...";
  }
  return text;
}

std::string entry_summary_text(const SessionEntry& entry) {
  switch (entry.kind()) {
    case qt::harness::SessionEntryKind::kUserImageMessage: {
      const auto* user = std::get_if<SessionUserImageMessageEntry>(&entry.data);
      return "user: " + first_line_truncated(flatten_user_blocks(user->content));
    }
    case qt::harness::SessionEntryKind::kAssistantMessage: {
      const auto* assistant =
          std::get_if<SessionAssistantMessageEntry>(&entry.data);
      std::string text;
      for (const auto& block : assistant->content) {
        if (block.kind == qt::harness::SessionAssistantContentBlockKind::kText) {
          text += block.text;
        }
      }
      return "assistant: " + first_line_truncated(text);
    }
    case qt::harness::SessionEntryKind::kToolResultMessage: {
      const auto* tool = std::get_if<SessionToolResultMessageEntry>(&entry.data);
      return "tool: " + tool->tool_name;
    }
    case qt::harness::SessionEntryKind::kCustomMessage: {
      const auto* custom = std::get_if<SessionCustomMessageEntry>(&entry.data);
      return "custom: " +
             first_line_truncated(flatten_custom_message_text(custom->content));
    }
    case qt::harness::SessionEntryKind::kBranchSummary: {
      const auto* summary =
          std::get_if<qt::harness::SessionBranchSummaryEntry>(&entry.data);
      return "branch summary: " + first_line_truncated(summary->summary);
    }
    case qt::harness::SessionEntryKind::kCompaction: {
      const auto* summary =
          std::get_if<qt::harness::SessionCompactionEntry>(&entry.data);
      return "compaction: " + first_line_truncated(summary->summary);
    }
    case qt::harness::SessionEntryKind::kBashExecution: {
      const auto* bash = std::get_if<qt::harness::SessionBashExecutionEntry>(&entry.data);
      return "bash: " + first_line_truncated(bash->command);
    }
    case qt::harness::SessionEntryKind::kModelChange: {
      const auto* model = std::get_if<qt::harness::SessionModelChangeEntry>(&entry.data);
      return "model: " + model->provider + "/" + model->model_id;
    }
    case qt::harness::SessionEntryKind::kThinkingLevelChange: {
      const auto* thinking =
          std::get_if<qt::harness::SessionThinkingLevelChangeEntry>(&entry.data);
      return "thinking: " + thinking->thinking_level;
    }
    case qt::harness::SessionEntryKind::kSessionInfo: {
      const auto* info = std::get_if<qt::harness::SessionInfoEntry>(&entry.data);
      return "session info: " + info->name.value_or(std::string("(unnamed)"));
    }
    case qt::harness::SessionEntryKind::kLabel: {
      const auto* label = std::get_if<qt::harness::SessionLabelEntry>(&entry.data);
      return "label: " + label->label;
    }
    case qt::harness::SessionEntryKind::kExtensionState: {
      const auto* state = std::get_if<qt::harness::SessionExtensionStateEntry>(&entry.data);
      return "extension: " + state->custom_type;
    }
    case qt::harness::SessionEntryKind::kCustom: {
      const auto* custom = std::get_if<qt::harness::SessionCustomEntry>(&entry.data);
      return "custom entry: " + custom->entry_type;
    }
    case qt::harness::SessionEntryKind::kSessionMetadata:
      return "session metadata";
    case qt::harness::SessionEntryKind::kSummary: {
      const auto* summary = std::get_if<qt::harness::SessionSummaryEntry>(&entry.data);
      return "summary: " + first_line_truncated(summary->summary);
    }
  }
  return "entry";
}

void append_tree_items(const SessionEntryTreeNode& node, std::size_t depth,
                       std::vector<bool> ancestor_continues,
                       bool is_last, std::string_view selected_entry_id,
                       std::vector<ReferenceSessionTreeItem>& items) {
  const auto id = entry_id_of(node.entry);
  if (!id.has_value()) {
    return;
  }

  std::ostringstream label;
  for (const bool continues : ancestor_continues) {
    label << (continues ? "|  " : "   ");
  }
  if (!ancestor_continues.empty()) {
    label << (is_last ? "`- " : "|- ");
  } else {
    label << "- ";
  }
  label << *id << " " << entry_summary_text(node.entry);

  ReferenceSessionTreeItem item;
  item.entry_id = *id;
  item.label = label.str();
  item.selected = !selected_entry_id.empty() && *id == selected_entry_id;
  items.push_back(std::move(item));

  ancestor_continues.push_back(!is_last);
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    append_tree_items(node.children[index], depth + 1U, ancestor_continues,
                      index + 1U == node.children.size(), selected_entry_id, items);
  }
}

std::size_t selected_tree_item_index(
    const std::vector<ReferenceSessionTreeItem>& items,
    const std::optional<std::string>& selection_hint) {
  if (selection_hint.has_value() && !selection_hint->empty()) {
    const auto it = std::find_if(
        items.begin(), items.end(),
        [&](const ReferenceSessionTreeItem& item) {
          return item.entry_id.find(*selection_hint) == 0 ||
                 item.label.find(*selection_hint) != std::string::npos;
        });
    if (it != items.end()) {
      return static_cast<std::size_t>(std::distance(items.begin(), it));
    }
  }

  const auto selected = std::find_if(
      items.begin(), items.end(),
      [](const ReferenceSessionTreeItem& item) { return item.selected; });
  if (selected != items.end()) {
    return static_cast<std::size_t>(std::distance(items.begin(), selected));
  }
  return items.empty() ? 0U : items.size() - 1U;
}

struct CollectedBranchSummaryEntries {
  std::vector<SessionEntry> entries;
  std::optional<std::string> common_ancestor_id;
};

CollectedBranchSummaryEntries collect_entries_for_branch_summary(
    const qt::harness::SessionManager& session_manager,
    const std::optional<std::string>& old_leaf_id,
    const std::string& target_entry_id) {
  CollectedBranchSummaryEntries result;
  if (!old_leaf_id.has_value()) {
    return result;
  }

  const auto old_path = session_manager.get_branch(*old_leaf_id);
  const auto target_path = session_manager.get_branch(target_entry_id);
  std::unordered_set<std::string> old_ids;
  old_ids.reserve(old_path.size());
  for (const auto& entry : old_path) {
    const auto id = entry_id_of(entry);
    if (id.has_value()) {
      old_ids.insert(*id);
    }
  }

  for (std::size_t index = target_path.size(); index > 0; --index) {
    const auto id = entry_id_of(target_path[index - 1]);
    if (id.has_value() && old_ids.find(*id) != old_ids.end()) {
      result.common_ancestor_id = *id;
      break;
    }
  }

  std::optional<std::string> current = old_leaf_id;
  while (current.has_value() && current != result.common_ancestor_id) {
    const auto* entry = session_manager.get_entry(*current);
    if (entry == nullptr) {
      break;
    }
    result.entries.push_back(*entry);
    current = parent_id_of(*entry);
  }
  std::reverse(result.entries.begin(), result.entries.end());
  return result;
}

qt::ai::Json tree_preparation_json(
    const std::string& target_entry_id,
    const std::optional<std::string>& old_leaf_id,
    const CollectedBranchSummaryEntries& collected, bool summarize) {
  qt::ai::Json entries = qt::ai::Json::array();
  for (const auto& entry : collected.entries) {
    entries.push_back(qt::ai::Json(entry));
  }

  qt::ai::Json preparation{{"targetId", target_entry_id},
                           {"oldLeafId", old_leaf_id.value_or("")},
                           {"entriesToSummarize", std::move(entries)},
                           {"userWantsSummary", summarize}};
  if (collected.common_ancestor_id.has_value()) {
    preparation["commonAncestorId"] = *collected.common_ancestor_id;
  } else {
    preparation["commonAncestorId"] = nullptr;
  }
  return preparation;
}

}  // namespace

Result<std::vector<ReferenceSessionTreeItem>> build_session_tree_items(
    const qt::harness::SessionManager& session_manager,
    const std::optional<std::string>& selection_hint) {
  std::vector<ReferenceSessionTreeItem> items;
  const auto tree = session_manager.get_tree();
  const std::string selected_entry_id =
      session_manager.get_leaf_id().value_or(std::string{});
  for (std::size_t index = 0; index < tree.size(); ++index) {
    append_tree_items(tree[index], 0U, {}, index + 1U == tree.size(),
                      selected_entry_id, items);
  }
  if (!items.empty()) {
    const auto selected_index = selected_tree_item_index(items, selection_hint);
    for (std::size_t index = 0; index < items.size(); ++index) {
      items[index].selected = index == selected_index;
    }
  }
  return items;
}

Result<ReferenceSessionTreeSelection> make_session_tree_selection(
    const qt::harness::SessionManager& session_manager, const std::string& entry_id) {
  const auto* entry = session_manager.get_entry(entry_id);
  if (entry == nullptr) {
    return Result<ReferenceSessionTreeSelection>::failure(
        ErrorCode::not_found, "session tree entry not found: " + entry_id);
  }

  ReferenceSessionTreeSelection selection;
  selection.target_entry_id = entry_id;
  switch (entry->kind()) {
    case qt::harness::SessionEntryKind::kUserImageMessage: {
      const auto* user = std::get_if<SessionUserImageMessageEntry>(&entry->data);
      selection.editor_text = flatten_user_blocks(user->content);
      selection.next_leaf_id = parent_id_of(*entry);
      selection.reset_to_root = !selection.next_leaf_id.has_value();
      break;
    }
    case qt::harness::SessionEntryKind::kCustomMessage: {
      const auto* custom = std::get_if<SessionCustomMessageEntry>(&entry->data);
      selection.editor_text = flatten_custom_message_text(custom->content);
      selection.next_leaf_id = parent_id_of(*entry);
      selection.reset_to_root = !selection.next_leaf_id.has_value();
      break;
    }
    default:
      selection.next_leaf_id = entry_id;
      break;
  }

  return selection;
}

qt::foundation::Result<ReferenceSessionTreeNavigationResult>
apply_session_tree_selection(
    RuntimeHost& host, const ReferenceSessionTreeSelection& selection,
    ReferenceSessionTreeSummaryOptions options) {
  ReferenceSessionTreeNavigationResult result;
  result.editor_text = selection.editor_text;
  const auto old_leaf_id = host.session_manager().get_leaf_id();
  const auto collected = collect_entries_for_branch_summary(
      host.session_manager(), old_leaf_id, selection.target_entry_id);
  host.harness().emit_session_before_tree(tree_preparation_json(
      selection.target_entry_id, old_leaf_id, collected, options.summarize));

  if (options.summarize) {
    if (!collected.entries.empty()) {
      auto summarized = host.harness().summarize_branch(
          collected.entries,
          qt::harness::ModelBackedBranchSummaryRequest{
              host.harness().model(),
              host.harness().stream_options(),
              std::move(options.custom_instructions),
              options.replace_instructions});
      if (!summarized) {
        return summarized.error();
      }
      auto summary_id = host.session_manager().branch_with_summary(
          selection.next_leaf_id, summarized.value().summary,
          summarized.value().details, false);
      if (!summary_id) {
        return summary_id.error();
      }
      result.summary_entry_id = summary_id.value();
      const auto* summary_entry =
          host.session_manager().get_entry(result.summary_entry_id.value());
      std::optional<qt::ai::Json> summary_json;
      if (summary_entry != nullptr) {
        summary_json = qt::ai::Json(*summary_entry);
      }
      host.sync_harness_from_session_manager();
      host.harness().emit_session_tree(host.session_manager().get_leaf_id(),
                                       old_leaf_id, std::move(summary_json),
                                       false);
      return result;
    }
  }

  if (selection.reset_to_root) {
    host.session_manager().reset_leaf();
  } else if (selection.next_leaf_id.has_value()) {
    const auto status = host.session_manager().branch(*selection.next_leaf_id);
    if (!status.ok()) {
      return status.error();
    }
  }

  host.sync_harness_from_session_manager();
  host.harness().emit_session_tree(host.session_manager().get_leaf_id(),
                                   old_leaf_id);
  return result;
}

}  // namespace qt::app
