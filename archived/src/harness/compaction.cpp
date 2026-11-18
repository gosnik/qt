#include <qt/harness/compaction.hpp>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <type_traits>
#include <utility>

#include <qt/harness/session_repository.hpp>
#include <qt/harness/session_manager.hpp>
#include <qt/harness/session_resume.hpp>

namespace qt::harness {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

constexpr const char* kSummarizationSystemPrompt =
    "You are a context summarization assistant. Your task is to read a "
    "conversation between a user and an AI coding assistant, then produce a "
    "structured summary following the exact format specified.\n\n"
    "Do NOT continue the conversation. Do NOT respond to any questions in the "
    "conversation. ONLY output the structured summary.";

constexpr const char* kSummarizationPrompt =
    "The messages above are a conversation to summarize. Create a structured "
    "context checkpoint summary that another LLM will use to continue the "
    "work.\n\n"
    "Use this EXACT format:\n\n"
    "## Goal\n"
    "[What is the user trying to accomplish? Can be multiple items if the "
    "session covers different tasks.]\n\n"
    "## Constraints & Preferences\n"
    "- [Any constraints, preferences, or requirements mentioned by user]\n"
    "- [Or \"(none)\" if none were mentioned]\n\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [Completed tasks/changes]\n\n"
    "### In Progress\n"
    "- [ ] [Current work]\n\n"
    "### Blocked\n"
    "- [Issues preventing progress, if any]\n\n"
    "## Key Decisions\n"
    "- **[Decision]**: [Brief rationale]\n\n"
    "## Next Steps\n"
    "1. [Ordered list of what should happen next]\n\n"
    "## Critical Context\n"
    "- [Any data, examples, or references needed to continue]\n"
    "- [Or \"(none)\" if not applicable]\n\n"
    "Keep each section concise. Preserve exact file paths, function names, and "
    "error messages.";

constexpr const char* kUpdateSummarizationPrompt =
    "The messages above are NEW conversation messages to incorporate into the "
    "existing summary provided in <previous-summary> tags.\n\n"
    "Update the existing structured summary with new information. RULES:\n"
    "- PRESERVE all existing information from the previous summary\n"
    "- ADD new progress, decisions, and context from the new messages\n"
    "- UPDATE the Progress section: move items from \"In Progress\" to \"Done\" "
    "when completed\n"
    "- UPDATE \"Next Steps\" based on what was accomplished\n"
    "- PRESERVE exact file paths, function names, and error messages\n"
    "- If something is no longer relevant, you may remove it\n\n"
    "Use this EXACT format:\n\n"
    "## Goal\n"
    "[Preserve existing goals, add new ones if the task expanded]\n\n"
    "## Constraints & Preferences\n"
    "- [Preserve existing, add new ones discovered]\n\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [Include previously done items AND newly completed items]\n\n"
    "### In Progress\n"
    "- [ ] [Current work - update based on progress]\n\n"
    "### Blocked\n"
    "- [Current blockers - remove if resolved]\n\n"
    "## Key Decisions\n"
    "- **[Decision]**: [Brief rationale] (preserve all previous, add new)\n\n"
    "## Next Steps\n"
    "1. [Update based on current state]\n\n"
    "## Critical Context\n"
    "- [Preserve important context, add new if needed]\n\n"
    "Keep each section concise. Preserve exact file paths, function names, and "
    "error messages.";

constexpr const char* kTurnPrefixSummarizationPrompt =
    "This is the PREFIX of a turn that was too large to keep. The SUFFIX "
    "(recent work) is retained.\n\n"
    "Summarize the prefix to provide context for the retained suffix:\n\n"
    "## Original Request\n"
    "[What did the user ask for in this turn?]\n\n"
    "## Early Progress\n"
    "- [Key decisions and work done in the prefix]\n\n"
    "## Context for Suffix\n"
    "- [Information needed to understand the retained recent work]\n\n"
    "Be concise. Focus on what's needed to understand the kept suffix.";

constexpr const char* kBranchSummaryPreamble =
    "The user explored a different conversation branch before returning "
    "here.\nSummary of that exploration:\n\n";

constexpr const char* kBranchSummaryPrompt =
    "Create a structured summary of this conversation branch for context when "
    "returning later.\n\n"
    "Use this EXACT format:\n\n"
    "## Goal\n"
    "[What was the user trying to accomplish in this branch?]\n\n"
    "## Constraints & Preferences\n"
    "- [Any constraints, preferences, or requirements mentioned]\n"
    "- [Or \"(none)\" if none were mentioned]\n\n"
    "## Progress\n"
    "### Done\n"
    "- [x] [Completed tasks/changes]\n\n"
    "### In Progress\n"
    "- [ ] [Work that was started but not finished]\n\n"
    "### Blocked\n"
    "- [Issues preventing progress, if any]\n\n"
    "## Key Decisions\n"
    "- **[Decision]**: [Brief rationale]\n\n"
    "## Next Steps\n"
    "1. [What should happen next to continue this work]\n\n"
    "Keep each section concise. Preserve exact file paths, function names, "
    "and error messages.";

std::optional<std::string> normalize_optional_text(
    const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::size_t> find_last_entry_index(
    const std::vector<SessionEntry>& entries, SessionEntryKind kind) {
  for (std::size_t index = entries.size(); index > 0; --index) {
    if (entries[index - 1].kind() == kind) {
      return index - 1;
    }
  }
  return std::nullopt;
}

std::string count_text(std::size_t value) {
  std::ostringstream output;
  output << value;
  return output.str();
}

std::optional<std::string> entry_id(const SessionEntry& entry) {
  return std::visit(
      [](const auto& value) -> std::optional<std::string> {
        using Entry = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Entry, SessionMetadataEntry> ||
                      std::is_same_v<Entry, SessionLabelEntry> ||
                      std::is_same_v<Entry, SessionSummaryEntry> ||
                      std::is_same_v<Entry, SessionCustomEntry>) {
          return std::nullopt;
        } else {
          return normalize_optional_text(value.upstream.id);
        }
      },
      entry.data);
}

std::optional<std::size_t> find_entry_index_by_id(
    const std::vector<SessionEntry>& entries, const std::string& id) {
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto current_id = entry_id(entries[index]);
    if (current_id.has_value() && *current_id == id) {
      return index;
    }
  }
  return std::nullopt;
}

std::string truncate_for_summary(const std::string& text,
                                 std::size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  std::ostringstream output;
  output << text.substr(0, max_chars) << "\n\n[... "
         << (text.size() - max_chars) << " more characters truncated]";
  return output.str();
}

class ConversationSerializer {
 public:
  explicit ConversationSerializer(std::ostringstream& output)
      : output_(output) {}

  void append_text_block(const std::string& prefix, const std::string& text) {
    if (text.empty()) {
      return;
    }
    if (!first_) {
      output_ << "\n\n";
    }
    output_ << prefix << text;
    first_ = false;
  }

 private:
  std::ostringstream& output_;
  bool first_ = true;
};

std::string text_from_user_blocks(
    const std::vector<SessionUserContentBlock>& blocks) {
  std::ostringstream output;
  for (const auto& block : blocks) {
    if (block.kind == SessionUserContentBlockKind::kText) {
      output << block.text;
    } else if (block.kind == SessionUserContentBlockKind::kImage) {
      output << "[Image: " << block.mime_type << "]";
    }
  }
  return output.str();
}

std::size_t compaction_chars_from_user_blocks(
    const std::vector<SessionUserContentBlock>& blocks) {
  std::size_t chars = 0;
  for (const auto& block : blocks) {
    chars += block.kind == SessionUserContentBlockKind::kImage ? 4800
                                                               : block.text.size();
  }
  return chars;
}

std::string serialize_conversation(const std::vector<SessionEntry>& entries) {
  std::ostringstream output;
  ConversationSerializer serializer(output);
  for (const auto& entry : entries) {
    switch (entry.kind()) {
      case SessionEntryKind::kUserImageMessage: {
        const auto* message =
            std::get_if<SessionUserImageMessageEntry>(&entry.data);
        serializer.append_text_block("[User]: ",
                                     text_from_user_blocks(message->content));
        break;
      }
      case SessionEntryKind::kAssistantMessage: {
        const auto* message =
            std::get_if<SessionAssistantMessageEntry>(&entry.data);
        std::vector<std::string> thinking_parts;
        std::vector<std::string> text_parts;
        std::vector<std::string> tool_calls;
        for (const auto& block : message->content) {
          if (block.kind == SessionAssistantContentBlockKind::kThinking) {
            thinking_parts.push_back(block.thinking);
          } else if (block.kind == SessionAssistantContentBlockKind::kText) {
            text_parts.push_back(block.text);
          } else if (block.kind ==
                     SessionAssistantContentBlockKind::kToolCall) {
            std::ostringstream call;
            call << block.tool_name << "(";
            bool first_arg = true;
            for (auto it = block.tool_arguments.begin();
                 it != block.tool_arguments.end(); ++it) {
              if (!first_arg) {
                call << ", ";
              }
              call << it.key() << "=" << it.value().dump();
              first_arg = false;
            }
            call << ")";
            tool_calls.push_back(call.str());
          }
        }
        std::ostringstream thinking;
        for (std::size_t index = 0; index < thinking_parts.size(); ++index) {
          if (index > 0) thinking << "\n";
          thinking << thinking_parts[index];
        }
        serializer.append_text_block("[Assistant thinking]: ", thinking.str());
        std::ostringstream text;
        for (std::size_t index = 0; index < text_parts.size(); ++index) {
          if (index > 0) text << "\n";
          text << text_parts[index];
        }
        serializer.append_text_block("[Assistant]: ", text.str());
        std::ostringstream calls;
        for (std::size_t index = 0; index < tool_calls.size(); ++index) {
          if (index > 0) calls << "; ";
          calls << tool_calls[index];
        }
        serializer.append_text_block("[Assistant tool calls]: ", calls.str());
        break;
      }
      case SessionEntryKind::kToolResultMessage: {
        const auto* message =
            std::get_if<SessionToolResultMessageEntry>(&entry.data);
        serializer.append_text_block(
            "[Tool result]: ",
            truncate_for_summary(text_from_user_blocks(message->content), 2000));
        break;
      }
      case SessionEntryKind::kBashExecution: {
        const auto* message =
            std::get_if<SessionBashExecutionEntry>(&entry.data);
        serializer.append_text_block(
            "[Bash execution]: ",
            "$ " + message->command + "\n" +
                truncate_for_summary(message->output, 2000));
        break;
      }
      case SessionEntryKind::kBranchSummary: {
        const auto* summary =
            std::get_if<SessionBranchSummaryEntry>(&entry.data);
        serializer.append_text_block("[Branch summary]: ", summary->summary);
        break;
      }
      case SessionEntryKind::kCompaction: {
        const auto* summary =
            std::get_if<SessionCompactionEntry>(&entry.data);
        serializer.append_text_block("[Compaction summary]: ", summary->summary);
        break;
      }
      case SessionEntryKind::kCustomMessage: {
        const auto* message =
            std::get_if<SessionCustomMessageEntry>(&entry.data);
        const std::string prefix = "[Custom:" + message->custom_type + "]: ";
        if (const auto* text =
                std::get_if<std::string>(&message->content)) {
          serializer.append_text_block(prefix, *text);
        } else if (const auto* blocks =
                       std::get_if<std::vector<SessionUserContentBlock>>(
                           &message->content)) {
          serializer.append_text_block(prefix, text_from_user_blocks(*blocks));
        }
        break;
      }
      case SessionEntryKind::kExtensionState: {
        const auto* state =
            std::get_if<SessionExtensionStateEntry>(&entry.data);
        serializer.append_text_block(
            "[Extension state:" + state->custom_type + "]: ",
            truncate_for_summary(state->data.dump(), 2000));
        break;
      }
      case SessionEntryKind::kSessionMetadata:
      case SessionEntryKind::kLabel:
      case SessionEntryKind::kSessionInfo:
      case SessionEntryKind::kSummary:
      case SessionEntryKind::kModelChange:
      case SessionEntryKind::kThinkingLevelChange:
      case SessionEntryKind::kCustom:
        break;
    }
  }
  return output.str();
}

std::int64_t estimate_entry_tokens(const SessionEntry& entry) {
  const auto chars_to_tokens = [](std::size_t chars) -> std::int64_t {
    return static_cast<std::int64_t>((chars + 3) / 4);
  };

  switch (entry.kind()) {
    case SessionEntryKind::kUserImageMessage: {
      const auto* message =
          std::get_if<SessionUserImageMessageEntry>(&entry.data);
      return chars_to_tokens(compaction_chars_from_user_blocks(message->content));
    }
    case SessionEntryKind::kAssistantMessage: {
      const auto* message =
          std::get_if<SessionAssistantMessageEntry>(&entry.data);
      std::size_t chars = 0;
      for (const auto& block : message->content) {
        if (block.kind == SessionAssistantContentBlockKind::kText) {
          chars += block.text.size();
        } else if (block.kind ==
                   SessionAssistantContentBlockKind::kThinking) {
          chars += block.thinking.size();
        } else if (block.kind ==
                   SessionAssistantContentBlockKind::kToolCall) {
          chars += block.tool_name.size() + block.tool_arguments.dump().size();
        }
      }
      return chars_to_tokens(chars);
    }
    case SessionEntryKind::kToolResultMessage: {
      const auto* message =
          std::get_if<SessionToolResultMessageEntry>(&entry.data);
      return chars_to_tokens(compaction_chars_from_user_blocks(message->content));
    }
    case SessionEntryKind::kBashExecution: {
      const auto* message =
          std::get_if<SessionBashExecutionEntry>(&entry.data);
      return chars_to_tokens(message->command.size() + message->output.size());
    }
    case SessionEntryKind::kBranchSummary: {
      const auto* summary =
          std::get_if<SessionBranchSummaryEntry>(&entry.data);
      return chars_to_tokens(summary->summary.size());
    }
    case SessionEntryKind::kCompaction: {
      const auto* summary =
          std::get_if<SessionCompactionEntry>(&entry.data);
      return chars_to_tokens(summary->summary.size());
    }
    case SessionEntryKind::kCustomMessage: {
      const auto* message =
          std::get_if<SessionCustomMessageEntry>(&entry.data);
      if (const auto* text = std::get_if<std::string>(&message->content)) {
        return chars_to_tokens(text->size());
      }
      const auto* blocks =
          std::get_if<std::vector<SessionUserContentBlock>>(&message->content);
      return chars_to_tokens(compaction_chars_from_user_blocks(*blocks));
    }
    case SessionEntryKind::kExtensionState: {
      const auto* state =
          std::get_if<SessionExtensionStateEntry>(&entry.data);
      return chars_to_tokens(state->custom_type.size() + state->data.dump().size());
    }
    case SessionEntryKind::kSessionMetadata:
    case SessionEntryKind::kLabel:
    case SessionEntryKind::kSessionInfo:
    case SessionEntryKind::kSummary:
    case SessionEntryKind::kModelChange:
    case SessionEntryKind::kThinkingLevelChange:
    case SessionEntryKind::kCustom:
      return 0;
  }
  return 0;
}

std::int64_t estimate_entries_tokens(const std::vector<SessionEntry>& entries) {
  std::int64_t total = 0;
  for (const auto& entry : entries) {
    total += estimate_entry_tokens(entry);
  }
  return total;
}

std::int64_t estimate_session_context_tokens(
    const std::vector<SessionEntry>& entries) {
  const auto context = SessionManager(entries).build_session_context();
  std::int64_t total = 0;
  for (const auto& message : context.messages) {
    total += qt::ai::estimate_token_count(message);
  }
  return total;
}

void append_compaction_source_entries(
    std::vector<SessionEntry>& target,
    std::vector<SessionEntry>::const_iterator first,
    std::vector<SessionEntry>::const_iterator last) {
  for (auto it = first; it != last; ++it) {
    if (it->kind() == SessionEntryKind::kCompaction) {
      continue;
    }
    target.push_back(*it);
  }
}

bool is_valid_cut_entry(const SessionEntry& entry) {
  switch (entry.kind()) {
    case SessionEntryKind::kUserImageMessage:
    case SessionEntryKind::kAssistantMessage:
    case SessionEntryKind::kBashExecution:
    case SessionEntryKind::kBranchSummary:
    case SessionEntryKind::kCustomMessage:
      return true;
    case SessionEntryKind::kToolResultMessage:
    case SessionEntryKind::kCompaction:
    case SessionEntryKind::kSessionMetadata:
    case SessionEntryKind::kLabel:
    case SessionEntryKind::kSessionInfo:
    case SessionEntryKind::kSummary:
    case SessionEntryKind::kModelChange:
    case SessionEntryKind::kThinkingLevelChange:
    case SessionEntryKind::kExtensionState:
    case SessionEntryKind::kCustom:
      return false;
  }
  return false;
}

bool is_turn_start_entry(const SessionEntry& entry) {
  switch (entry.kind()) {
    case SessionEntryKind::kUserImageMessage:
    case SessionEntryKind::kBashExecution:
    case SessionEntryKind::kBranchSummary:
    case SessionEntryKind::kCustomMessage:
      return true;
    default:
      return false;
  }
}

std::vector<std::size_t> find_valid_cut_points(
    const std::vector<SessionEntry>& entries,
    std::size_t start_index,
    std::size_t end_index) {
  std::vector<std::size_t> cut_points;
  for (std::size_t index = start_index; index < end_index; ++index) {
    if (is_valid_cut_entry(entries[index])) {
      cut_points.push_back(index);
    }
  }
  return cut_points;
}

std::optional<std::size_t> find_turn_start_index(
    const std::vector<SessionEntry>& entries,
    std::size_t entry_index,
    std::size_t start_index) {
  for (std::size_t index = entry_index + 1; index > start_index; --index) {
    const std::size_t current = index - 1;
    if (is_turn_start_entry(entries[current])) {
      return current;
    }
  }
  return std::nullopt;
}

struct CompactionCutPoint {
  std::size_t first_kept_entry_index = 0;
  std::optional<std::size_t> turn_start_index;
  bool is_split_turn = false;
};

CompactionCutPoint find_model_compaction_cut_point(
    const std::vector<SessionEntry>& entries,
    std::size_t boundary_start,
    std::size_t boundary_end,
    std::int64_t keep_recent_tokens) {
  const auto cut_points =
      find_valid_cut_points(entries, boundary_start, boundary_end);
  if (cut_points.empty()) {
    return CompactionCutPoint{boundary_start, std::nullopt, false};
  }

  std::size_t cut_index = cut_points.front();
  if (keep_recent_tokens <= 0) {
    cut_index = cut_points.back();
  } else {
    std::int64_t accumulated = 0;
    for (std::size_t index = boundary_end; index > boundary_start; --index) {
      const std::size_t current = index - 1;
      accumulated += estimate_entry_tokens(entries[current]);
      if (accumulated >= keep_recent_tokens) {
        for (const std::size_t point : cut_points) {
          if (point >= current) {
            cut_index = point;
            break;
          }
        }
        break;
      }
    }
  }

  while (cut_index > boundary_start) {
    const auto& previous = entries[cut_index - 1];
    if (previous.kind() == SessionEntryKind::kCompaction ||
        previous.kind() == SessionEntryKind::kSessionMetadata ||
        is_valid_cut_entry(previous) ||
        previous.kind() == SessionEntryKind::kToolResultMessage) {
      break;
    }
    --cut_index;
  }

  const bool starts_turn = is_turn_start_entry(entries[cut_index]);
  const auto turn_start =
      starts_turn ? std::nullopt
                  : find_turn_start_index(entries, cut_index, boundary_start);
  return CompactionCutPoint{cut_index, turn_start,
                            !starts_turn && turn_start.has_value()};
}

std::string build_summary_prompt(const std::string& conversation,
                                 const std::optional<std::string>& previous,
                                 const std::optional<std::string>& custom) {
  std::ostringstream prompt;
  prompt << "<conversation>\n" << conversation << "\n</conversation>\n\n";
  if (previous.has_value()) {
    prompt << "<previous-summary>\n"
           << *previous << "\n</previous-summary>\n\n";
  }
  prompt << (previous.has_value() ? kUpdateSummarizationPrompt
                                  : kSummarizationPrompt);
  if (custom.has_value() && !custom->empty()) {
    prompt << "\n\nAdditional focus: " << *custom;
  }
  return prompt.str();
}

std::string build_turn_prefix_prompt(const std::string& conversation) {
  std::ostringstream prompt;
  prompt << "<conversation>\n" << conversation << "\n</conversation>\n\n"
         << kTurnPrefixSummarizationPrompt;
  return prompt.str();
}

std::vector<SessionEntry> prepare_branch_summary_entries(
    const std::vector<SessionEntry>& entries, std::int64_t token_budget) {
  if (token_budget <= 0) {
    return entries;
  }

  std::vector<SessionEntry> prepared;
  std::int64_t total_tokens = 0;
  for (std::size_t index = entries.size(); index > 0; --index) {
    const auto& entry = entries[index - 1];
    const auto tokens = estimate_entry_tokens(entry);
    if (total_tokens + tokens > token_budget &&
        !prepared.empty() &&
        entry.kind() != SessionEntryKind::kCompaction &&
        entry.kind() != SessionEntryKind::kBranchSummary) {
      break;
    }
    prepared.push_back(entry);
    total_tokens += tokens;
  }
  std::reverse(prepared.begin(), prepared.end());
  return prepared;
}

std::string build_branch_summary_prompt(
    const std::string& conversation,
    const std::optional<std::string>& custom_instructions,
    bool replace_instructions) {
  const std::string instructions =
      replace_instructions && custom_instructions.has_value() &&
              !custom_instructions->empty()
          ? *custom_instructions
          : custom_instructions.has_value() && !custom_instructions->empty()
                ? std::string(kBranchSummaryPrompt) + "\n\nAdditional focus: " +
                      *custom_instructions
                : std::string(kBranchSummaryPrompt);
  std::ostringstream prompt;
  prompt << "<conversation>\n" << conversation << "\n</conversation>\n\n"
         << instructions;
  return prompt.str();
}

qt::ai::Message make_text_message(qt::ai::MessageRole role,
                                  std::string text) {
  qt::ai::Message message;
  message.role = role;
  message.content.push_back(qt::ai::TextContent{std::move(text), std::nullopt});
  return message;
}

void collect_file_ops_from_entry(const SessionEntry& entry,
                                 std::vector<std::string>& read_files,
                                 std::vector<std::string>& modified_files) {
  const auto append_unique = [](std::vector<std::string>& files,
                                const std::string& path) {
    if (!path.empty() &&
        std::find(files.begin(), files.end(), path) == files.end()) {
      files.push_back(path);
    }
  };

  if (const auto* compaction =
          std::get_if<SessionCompactionEntry>(&entry.data);
      compaction != nullptr && compaction->details.has_value() &&
      compaction->from_hook != true) {
    const auto& details = *compaction->details;
    if (details.contains("readFiles") && details.at("readFiles").is_array()) {
      for (const auto& file : details.at("readFiles")) {
        if (file.is_string()) append_unique(read_files, file.get<std::string>());
      }
    }
    if (details.contains("modifiedFiles") &&
        details.at("modifiedFiles").is_array()) {
      for (const auto& file : details.at("modifiedFiles")) {
        if (file.is_string()) {
          append_unique(modified_files, file.get<std::string>());
        }
      }
    }
    return;
  }

  if (const auto* assistant =
          std::get_if<SessionAssistantMessageEntry>(&entry.data);
      assistant != nullptr) {
    for (const auto& block : assistant->content) {
      if (block.kind != SessionAssistantContentBlockKind::kToolCall ||
          !block.tool_arguments.contains("path") ||
          !block.tool_arguments.at("path").is_string()) {
        continue;
      }
      const std::string path = block.tool_arguments.at("path").get<std::string>();
      if (block.tool_name == "read") {
        append_unique(read_files, path);
      } else if (block.tool_name == "write" || block.tool_name == "edit") {
        append_unique(modified_files, path);
      }
    }
  }
}

Json build_file_operation_details(const std::vector<SessionEntry>& entries) {
  std::vector<std::string> read_files;
  std::vector<std::string> modified_files;
  for (const auto& entry : entries) {
    collect_file_ops_from_entry(entry, read_files, modified_files);
  }
  std::sort(read_files.begin(), read_files.end());
  std::sort(modified_files.begin(), modified_files.end());
  read_files.erase(std::remove_if(read_files.begin(), read_files.end(),
                                  [&modified_files](const std::string& path) {
                                    return std::find(modified_files.begin(),
                                                     modified_files.end(),
                                                     path) !=
                                           modified_files.end();
                                  }),
                   read_files.end());
  return Json{{"readFiles", read_files}, {"modifiedFiles", modified_files}};
}

std::string format_file_operations(const Json& details) {
  std::ostringstream output;
  const auto append_section = [&output, &details](const char* field,
                                                  const char* tag) {
    if (!details.contains(field) || !details.at(field).is_array() ||
        details.at(field).empty()) {
      return;
    }
    output << "\n\n<" << tag << ">\n";
    for (const auto& file : details.at(field)) {
      if (file.is_string()) {
        output << file.get<std::string>() << "\n";
      }
    }
    output << "</" << tag << ">";
  };
  append_section("readFiles", "read-files");
  append_section("modifiedFiles", "modified-files");
  return output.str();
}

Result<std::string> text_from_completion_response(
    const qt::ai::CompletionResponse& response,
    const std::string& failure_prefix) {
  if (response.stop_reason == qt::ai::StopReason::error) {
    return Result<std::string>::failure(ErrorCode::internal_error,
                                        failure_prefix + " failed");
  }

  std::string text;
  for (const auto& content : response.message.content) {
    if (const auto* block = std::get_if<qt::ai::TextContent>(&content)) {
      if (!text.empty()) {
        text += "\n";
      }
      text += block->text;
    }
  }
  if (text.empty()) {
    return Result<std::string>::failure(ErrorCode::internal_error,
                                        failure_prefix + " returned no text");
  }
  return text;
}

SessionEntry make_synthetic_compacted_entry(
    const SessionCompactionResult& result) {
  Json payload = Json::object();
  payload["session_id"] = result.snapshot.metadata.session_id;
  payload["working_directory"] = result.snapshot.metadata.working_directory;
  payload["persistent"] = result.snapshot.metadata.persistent;
  payload["original_entry_count"] = result.original_entry_count;
  payload["kept_entry_count"] = result.kept_entry_count;
  payload["compacted_entry_count"] = result.compacted_entry_count;

  if (result.snapshot.metadata.display_name.has_value()) {
    payload["display_name"] = *result.snapshot.metadata.display_name;
  }
  if (result.snapshot.metadata.parent_session_id.has_value()) {
    payload["parent_session_id"] =
        *result.snapshot.metadata.parent_session_id;
  }
  if (result.snapshot.metadata.created_at.has_value()) {
    payload["created_at"] = *result.snapshot.metadata.created_at;
  }
  if (result.snapshot.latest_label.has_value()) {
    payload["latest_label"] = *result.snapshot.latest_label;
  }
  if (result.snapshot.latest_summary.has_value()) {
    payload["latest_summary"] = *result.snapshot.latest_summary;
  }

  SessionEntry entry;
  entry.data = SessionCustomEntry{kCompactedSessionEntryType, std::move(payload),
                                  SessionUpstreamTreeFields{}};
  return entry;
}

std::string bool_text(bool value) { return value ? "true" : "false"; }

void append_summary_line(std::vector<std::string>& lines,
                         const std::string& name,
                         const std::string& value) {
  lines.push_back(name + ": " + value);
}

Result<CompactSessionRepositoryResult> validate_compaction_result(
    CompactSessionRepositoryResult result) {
  if (result.repository_path.empty()) {
    return Result<CompactSessionRepositoryResult>::failure(
        ErrorCode::invalid_argument, "repository_path is required");
  }
  if (result.rewritten_entries.empty()) {
    return Result<CompactSessionRepositoryResult>::failure(
        ErrorCode::invalid_argument, "rewritten session entries are empty");
  }
  return result;
}

}  // namespace

Result<SessionCompactionResult> compact_session_entries(
    const std::vector<SessionEntry>& entries) {
  if (entries.empty()) {
    return Result<SessionCompactionResult>::failure(
        ErrorCode::invalid_argument, "session entries are empty");
  }

  const auto snapshot = derive_session_branch_snapshot(entries);
  if (!snapshot) {
    return snapshot.error();
  }

  const auto metadata_index =
      find_last_entry_index(entries, SessionEntryKind::kSessionMetadata);
  if (!metadata_index.has_value()) {
    return Result<SessionCompactionResult>::failure(
        ErrorCode::invalid_argument,
        "session entries did not include session metadata");
  }

  const auto label_index = find_last_entry_index(entries, SessionEntryKind::kLabel);
  const auto summary_index =
      find_last_entry_index(entries, SessionEntryKind::kSummary);

  SessionCompactionResult result;
  result.snapshot = snapshot.value();
  result.snapshot.metadata.display_name =
      normalize_optional_text(result.snapshot.metadata.display_name);
  result.snapshot.metadata.parent_session_id =
      normalize_optional_text(result.snapshot.metadata.parent_session_id);
  result.snapshot.metadata.created_at =
      normalize_optional_text(result.snapshot.metadata.created_at);
  result.original_entry_count = entries.size();

  for (std::size_t index = 0; index < entries.size(); ++index) {
    const bool keep_metadata = index == *metadata_index;
    const bool keep_label = label_index.has_value() && index == *label_index;
    const bool keep_summary =
        summary_index.has_value() && index == *summary_index;

    if (!keep_metadata && !keep_label && !keep_summary) {
      continue;
    }

    result.kept_entries.push_back(entries[index]);
  }

  result.kept_entry_count = result.kept_entries.size();
  result.compacted_entry_count =
      result.original_entry_count - result.kept_entry_count;
  result.synthetic_entry = make_synthetic_compacted_entry(result);

  return result;
}

Result<ModelBackedCompactionPlan> build_model_backed_compaction_plan(
    const std::vector<SessionEntry>& entries,
    const ModelBackedCompactionRequest& request) {
  if (!request.enabled) {
    return Result<ModelBackedCompactionPlan>::failure(
        ErrorCode::invalid_argument, "model-backed compaction is disabled");
  }
  if (entries.empty()) {
    return Result<ModelBackedCompactionPlan>::failure(
        ErrorCode::invalid_argument, "session entries are empty");
  }
  if (request.reserve_tokens <= 0) {
    return Result<ModelBackedCompactionPlan>::failure(
        ErrorCode::invalid_argument, "reserve_tokens must be positive");
  }
  if (request.keep_recent_tokens < 0) {
    return Result<ModelBackedCompactionPlan>::failure(
        ErrorCode::invalid_argument, "keep_recent_tokens must be non-negative");
  }

  const auto previous_compaction_index =
      find_last_entry_index(entries, SessionEntryKind::kCompaction);
  std::size_t boundary_start = 0;
  std::optional<std::string> previous_summary;
  if (previous_compaction_index.has_value()) {
    const auto* compaction = std::get_if<SessionCompactionEntry>(
        &entries[*previous_compaction_index].data);
    previous_summary = normalize_optional_text(compaction->summary);
    const auto first_kept_index =
        find_entry_index_by_id(entries, compaction->first_kept_entry_id);
    boundary_start = first_kept_index.value_or(*previous_compaction_index + 1);
  }

  if (entries.back().kind() == SessionEntryKind::kCompaction) {
    return Result<ModelBackedCompactionPlan>::failure(
        ErrorCode::invalid_argument, "nothing to compact");
  }

  if (boundary_start >= entries.size()) {
    return Result<ModelBackedCompactionPlan>::failure(
        ErrorCode::invalid_argument, "nothing to compact");
  }

  const CompactionCutPoint cut_point = find_model_compaction_cut_point(
      entries, boundary_start, entries.size(), request.keep_recent_tokens);
  const std::size_t cut_index = cut_point.first_kept_entry_index;
  const std::size_t history_end =
      cut_point.is_split_turn ? *cut_point.turn_start_index : cut_index;

  ModelBackedCompactionPlan plan;
  plan.provider_options = request.provider_options;
  plan.first_kept_entry_index = cut_index;
  const auto kept_entry_id = entry_id(entries[cut_index]);
  if (!kept_entry_id.has_value()) {
    return Result<ModelBackedCompactionPlan>::failure(
        ErrorCode::invalid_argument,
        "First kept entry has no UUID - session may need migration");
  }
  plan.first_kept_entry_id = *kept_entry_id;
  plan.previous_summary = previous_summary;
  plan.tokens_before = estimate_session_context_tokens(entries);
  plan.is_split_turn = cut_point.is_split_turn;
  append_compaction_source_entries(plan.entries_to_summarize,
                                   entries.begin() + boundary_start,
                                   entries.begin() + history_end);
  if (cut_point.is_split_turn) {
    append_compaction_source_entries(plan.turn_prefix_entries,
                                     entries.begin() + *cut_point.turn_start_index,
                                     entries.begin() + cut_index);
  }
  plan.kept_entries.insert(plan.kept_entries.end(), entries.begin() + cut_index,
                           entries.end());
  plan.summarized_tokens = estimate_entries_tokens(plan.entries_to_summarize);
  plan.turn_prefix_tokens = estimate_entries_tokens(plan.turn_prefix_entries);
  plan.kept_tokens = estimate_entries_tokens(plan.kept_entries);
  std::vector<SessionEntry> file_operation_entries;
  if (previous_compaction_index.has_value()) {
    file_operation_entries.push_back(entries[*previous_compaction_index]);
  }
  file_operation_entries.insert(file_operation_entries.end(),
                                plan.entries_to_summarize.begin(),
                                plan.entries_to_summarize.end());
  file_operation_entries.insert(file_operation_entries.end(),
                                plan.turn_prefix_entries.begin(),
                                plan.turn_prefix_entries.end());
  plan.details = build_file_operation_details(file_operation_entries);

  const std::string prompt = build_summary_prompt(
      serialize_conversation(plan.entries_to_summarize), previous_summary,
      request.custom_instructions);
  const std::int64_t requested_max_tokens = request.reserve_tokens * 8 / 10;
  const std::int64_t max_output_tokens =
      request.model_max_output_tokens > 0
          ? std::min(requested_max_tokens, request.model_max_output_tokens)
          : requested_max_tokens;

  plan.request.model = request.model;
  plan.request.max_output_tokens = max_output_tokens;
  plan.request.messages.push_back(make_text_message(
      qt::ai::MessageRole::system, std::string(kSummarizationSystemPrompt)));
  plan.request.messages.push_back(
      make_text_message(qt::ai::MessageRole::user, prompt));
  plan.request.metadata = Json{{"purpose", "session_compaction"},
                               {"first_kept_entry_id",
                                plan.first_kept_entry_id},
                               {"tokens_before", plan.tokens_before},
                               {"summarized_tokens", plan.summarized_tokens},
                               {"turn_prefix_tokens", plan.turn_prefix_tokens},
                               {"kept_tokens", plan.kept_tokens}};

  if (plan.is_split_turn && !plan.turn_prefix_entries.empty()) {
    qt::ai::CompletionRequest turn_request;
    turn_request.model = request.model;
    const std::int64_t requested_turn_tokens = request.reserve_tokens / 2;
    turn_request.max_output_tokens =
        request.model_max_output_tokens > 0
            ? std::min(requested_turn_tokens, request.model_max_output_tokens)
            : requested_turn_tokens;
    turn_request.messages.push_back(make_text_message(
        qt::ai::MessageRole::system, std::string(kSummarizationSystemPrompt)));
    turn_request.messages.push_back(make_text_message(
        qt::ai::MessageRole::user,
        build_turn_prefix_prompt(serialize_conversation(
            plan.turn_prefix_entries))));
    turn_request.metadata =
        Json{{"purpose", "session_compaction_turn_prefix"},
             {"first_kept_entry_id", plan.first_kept_entry_id},
             {"tokens_before", plan.tokens_before},
             {"turn_prefix_tokens", plan.turn_prefix_tokens}};
    plan.turn_prefix_request = std::move(turn_request);
  }

  return plan;
}

Result<ModelBackedCompactionResult> compact_session_entries_with_model(
    const std::vector<SessionEntry>& entries,
    const ModelBackedCompactionRequest& request,
    qt::ai::Provider& provider) {
  const auto plan = build_model_backed_compaction_plan(entries, request);
  if (!plan) {
    return plan.error();
  }

  qt::ai::CompletionResponse response;
  std::string summary;
  const bool has_history_to_summarize =
      !plan.value().entries_to_summarize.empty();
  const bool use_synthetic_empty_history =
      plan.value().turn_prefix_request.has_value() && !has_history_to_summarize;
  if (!use_synthetic_empty_history) {
    response = provider.complete(plan.value().request,
                                 plan.value().provider_options);
    const auto summary_text =
        text_from_completion_response(response, "summarization");
    if (!summary_text) {
      return summary_text.error();
    }
    summary = summary_text.value();
  } else {
    summary = "No prior history.";
    response.message.role = qt::ai::MessageRole::assistant;
    response.message.content.push_back(
        qt::ai::TextContent{summary, std::nullopt});
    response.stop_reason = qt::ai::StopReason::end_turn;
    response.metadata = Json{{"synthetic", true},
                             {"reason", "split_turn_without_history"}};
  }

  std::optional<qt::ai::CompletionResponse> turn_prefix_response;
  if (plan.value().turn_prefix_request.has_value()) {
    const auto turn_response = provider.complete(
        *plan.value().turn_prefix_request, plan.value().provider_options);
    const auto turn_summary =
        text_from_completion_response(turn_response, "turn prefix summarization");
    if (!turn_summary) {
      return turn_summary.error();
    }
    summary += "\n\n---\n\n**Turn Context (split turn):**\n\n";
    summary += turn_summary.value();
    turn_prefix_response = turn_response;
  }
  summary += format_file_operations(plan.value().details);

  ModelBackedCompactionResult result;
  result.summary = std::move(summary);
  result.first_kept_entry_id = plan.value().first_kept_entry_id;
  result.tokens_before = plan.value().tokens_before;
  result.details = plan.value().details;
  result.plan = plan.value();
  result.response = response;
  result.turn_prefix_response = std::move(turn_prefix_response);
  return result;
}

Result<ModelBackedBranchSummaryResult> summarize_branch_entries_with_model(
    const std::vector<SessionEntry>& entries,
    const ModelBackedBranchSummaryRequest& request,
    qt::ai::Provider& provider) {
  if (entries.empty()) {
    return Result<ModelBackedBranchSummaryResult>::failure(
        ErrorCode::invalid_argument, "branch summary entries are empty");
  }
  if (request.reserve_tokens <= 0) {
    return Result<ModelBackedBranchSummaryResult>::failure(
        ErrorCode::invalid_argument, "reserve_tokens must be positive");
  }

  const auto context_window =
      request.provider_options.metadata.value("contextWindow",
                                              static_cast<std::int64_t>(128000));
  const auto token_budget = std::max<std::int64_t>(
      0, context_window - request.reserve_tokens);
  const auto prepared_entries =
      prepare_branch_summary_entries(entries, token_budget);
  if (prepared_entries.empty()) {
    return Result<ModelBackedBranchSummaryResult>::failure(
        ErrorCode::invalid_argument, "branch summary entries are empty");
  }

  ModelBackedBranchSummaryResult result;
  result.details = build_file_operation_details(entries);
  result.request.model = request.model;
  result.request.max_output_tokens = request.model_max_output_tokens > 0
                                         ? request.model_max_output_tokens
                                         : static_cast<std::int64_t>(2048);
  result.request.messages.push_back(make_text_message(
      qt::ai::MessageRole::system, std::string(kSummarizationSystemPrompt)));
  result.request.messages.push_back(make_text_message(
      qt::ai::MessageRole::user,
      build_branch_summary_prompt(serialize_conversation(prepared_entries),
                                  request.custom_instructions,
                                  request.replace_instructions)));
  result.request.metadata =
      Json{{"purpose", "branch_summary"},
           {"entry_count", entries.size()},
           {"summarized_entry_count", prepared_entries.size()}};

  result.response =
      provider.complete(result.request, request.provider_options);
  const auto summary_text =
      text_from_completion_response(result.response, "branch summarization");
  if (!summary_text) {
    return summary_text.error();
  }

  result.summary = std::string(kBranchSummaryPreamble) + summary_text.value() +
                   format_file_operations(result.details);
  return result;
}

Result<CompactSessionRepositoryResult> compact_session_repository(
    const CompactSessionRepositoryRequest& request) {
  if (request.repository_path.empty()) {
    return Result<CompactSessionRepositoryResult>::failure(
        ErrorCode::invalid_argument, "repository_path is required");
  }

  const auto resumed = resume_session(request.repository_path);
  if (!resumed) {
    return resumed.error();
  }

  const auto compaction = compact_session_entries(resumed.value().entries);
  if (!compaction) {
    return compaction.error();
  }

  CompactSessionRepositoryResult result;
  result.repository_path = request.repository_path;
  result.persisted = request.persist;
  result.included_synthetic_entry = request.include_synthetic_entry;
  result.compaction = compaction.value();
  result.rewritten_entries = result.compaction.kept_entries;
  if (request.include_synthetic_entry) {
    result.rewritten_entries.push_back(result.compaction.synthetic_entry);
  }

  const auto validated = validate_compaction_result(std::move(result));
  if (!validated) {
    return validated.error();
  }

  if (request.persist) {
    const Status replaced =
        SessionRepository(request.repository_path)
            .replace_all(validated.value().rewritten_entries);
    if (!replaced) {
      return replaced.error();
    }
  }

  return validated.value();
}

Result<CompactSessionRepositorySummary> render_compact_session_repository_summary(
    const CompactSessionRepositoryResult& result) {
  const auto validated = validate_compaction_result(result);
  if (!validated) {
    return validated.error();
  }

  CompactSessionRepositorySummary summary;
  summary.persisted = validated.value().persisted;
  summary.included_synthetic_entry =
      validated.value().included_synthetic_entry;
  summary.original_entry_count =
      validated.value().compaction.original_entry_count;
  summary.kept_entry_count = validated.value().compaction.kept_entry_count;
  summary.compacted_entry_count =
      validated.value().compaction.compacted_entry_count;
  summary.rewritten_entry_count = validated.value().rewritten_entries.size();

  append_summary_line(summary.lines, "path", validated.value().repository_path);
  append_summary_line(summary.lines, "persisted", bool_text(summary.persisted));
  append_summary_line(summary.lines, "included_synthetic_entry",
                      bool_text(summary.included_synthetic_entry));
  append_summary_line(summary.lines, "original_entries",
                      count_text(summary.original_entry_count));
  append_summary_line(summary.lines, "kept_entries",
                      count_text(summary.kept_entry_count));
  append_summary_line(summary.lines, "compacted_entries",
                      count_text(summary.compacted_entry_count));
  append_summary_line(summary.lines, "rewritten_entries",
                      count_text(summary.rewritten_entry_count));
  append_summary_line(summary.lines, "synthetic_entry_type",
                      kCompactedSessionEntryType);
  append_summary_line(
      summary.lines, "session_id",
      validated.value().compaction.snapshot.metadata.session_id);
  if (validated.value().compaction.snapshot.latest_label.has_value()) {
    append_summary_line(summary.lines, "latest_label",
                        *validated.value().compaction.snapshot.latest_label);
  }
  if (validated.value().compaction.snapshot.latest_summary.has_value()) {
    append_summary_line(summary.lines, "latest_summary",
                        *validated.value().compaction.snapshot.latest_summary);
  }

  return summary;
}

}  // namespace qt::harness
