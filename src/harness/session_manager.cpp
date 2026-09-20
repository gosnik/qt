#include <qt/harness/session_manager.hpp>

#include <algorithm>
#include <ctime>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <qt/harness/session_migration.hpp>

namespace qt::harness {
namespace {

constexpr char kGeneratedIdPrefix[] = "generated-";
constexpr char kBranchSummaryPrefix[] =
    "The following is a summary of a branch that this conversation came "
    "back from:\n\n<summary>\n";
constexpr char kBranchSummarySuffix[] = "\n</summary>";
constexpr char kCompactionSummaryPrefix[] =
    "The conversation history before this point was compacted into the "
    "following summary:\n\n<summary>\n";
constexpr char kCompactionSummarySuffix[] = "\n</summary>";

std::optional<std::string> optional_json_string(const Json& json,
                                                const char* key) {
  if (!json.is_object() || !json.contains(key) || json.at(key).is_null() ||
      !json.at(key).is_string()) {
    return std::nullopt;
  }
  return json.at(key).get<std::string>();
}

std::optional<bool> optional_json_bool(const Json& json, const char* key) {
  if (!json.is_object() || !json.contains(key) || json.at(key).is_null() ||
      !json.at(key).is_boolean()) {
    return std::nullopt;
  }
  return json.at(key).get<bool>();
}

const SessionUpstreamTreeFields* entry_upstream(const SessionEntry& entry) {
  return std::visit(
      [](const auto& value) -> const SessionUpstreamTreeFields* {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, SessionMetadataEntry> ||
                      std::is_same_v<Value, SessionSummaryEntry>) {
          return nullptr;
        } else {
          return &value.upstream;
        }
      },
      entry.data);
}

SessionUpstreamTreeFields* entry_upstream(SessionEntry& entry) {
  return std::visit(
      [](auto& value) -> SessionUpstreamTreeFields* {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, SessionMetadataEntry> ||
                      std::is_same_v<Value, SessionSummaryEntry>) {
          return nullptr;
        } else {
          return &value.upstream;
        }
      },
      entry.data);
}

const Json* raw_upstream_payload(const SessionEntry& entry) {
  const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
  if (custom == nullptr ||
      custom->entry_type != kUpstreamPiMonoSessionEntryType ||
      !custom->payload.is_object()) {
    return nullptr;
  }
  return &custom->payload;
}

std::optional<std::string> entry_id(const SessionEntry& entry) {
  if (const auto* upstream = entry_upstream(entry);
      upstream != nullptr && upstream->id.has_value() &&
      !upstream->id->empty()) {
    return upstream->id;
  }

  if (const auto* payload = raw_upstream_payload(entry)) {
    return optional_json_string(*payload, "id");
  }

  return std::nullopt;
}

std::optional<std::string> parent_id(const SessionEntry& entry) {
  if (const auto* upstream = entry_upstream(entry);
      upstream != nullptr && upstream->parent_id.has_value()) {
    return upstream->parent_id;
  }

  if (const auto* payload = raw_upstream_payload(entry)) {
    return optional_json_string(*payload, "parentId");
  }

  return std::nullopt;
}

std::optional<std::string> timestamp_of(const SessionEntry& entry) {
  if (const auto* upstream = entry_upstream(entry);
      upstream != nullptr && upstream->timestamp.has_value() &&
      !upstream->timestamp->empty()) {
    return upstream->timestamp;
  }

  if (const auto* payload = raw_upstream_payload(entry)) {
    return optional_json_string(*payload, "timestamp");
  }

  return std::nullopt;
}

std::optional<std::uint64_t> generated_id_suffix(std::string_view id) {
  constexpr std::size_t prefix_length = sizeof(kGeneratedIdPrefix) - 1U;
  if (id.size() <= prefix_length ||
      id.substr(0, prefix_length) != kGeneratedIdPrefix) {
    return std::nullopt;
  }

  std::uint64_t value = 0;
  for (std::size_t index = prefix_length; index < id.size(); ++index) {
    const char ch = id[index];
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    value = (value * 10U) + static_cast<std::uint64_t>(ch - '0');
  }
  return value;
}

std::optional<std::uint64_t> parse_timestamp_seconds(
    std::string_view timestamp) {
  if (timestamp.size() < 19U) {
    return std::nullopt;
  }

  std::tm time_info{};
  std::istringstream input(std::string(timestamp.substr(0U, 19U)));
  input >> std::get_time(&time_info, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) {
    return std::nullopt;
  }

#if defined(_WIN32)
  const std::time_t seconds = _mkgmtime(&time_info);
#else
  const std::time_t seconds = timegm(&time_info);
#endif
  if (seconds < 0) {
    return std::nullopt;
  }

  return static_cast<std::uint64_t>(seconds);
}

bool parse_user_blocks(const Json& json,
                       std::vector<SessionUserContentBlock>& blocks) {
  if (json.is_string()) {
    SessionUserContentBlock block;
    block.kind = SessionUserContentBlockKind::kText;
    block.text = json.get<std::string>();
    blocks.push_back(std::move(block));
    return true;
  }

  if (!json.is_array()) {
    return false;
  }

  for (const auto& item : json) {
    if (!item.is_object() || !item.contains("type") ||
        !item.at("type").is_string()) {
      return false;
    }

    const std::string type = item.at("type").get<std::string>();
    if (type == "text" && item.contains("text") && item.at("text").is_string()) {
      SessionUserContentBlock block;
      block.kind = SessionUserContentBlockKind::kText;
      block.text = item.at("text").get<std::string>();
      block.text_signature = optional_json_string(item, "textSignature");
      blocks.push_back(std::move(block));
      continue;
    }

    if (type == "image" && item.contains("data") &&
        item.at("data").is_string() && item.contains("mimeType") &&
        item.at("mimeType").is_string()) {
      SessionUserContentBlock block;
      block.kind = SessionUserContentBlockKind::kImage;
      block.data = item.at("data").get<std::string>();
      block.mime_type = item.at("mimeType").get<std::string>();
      blocks.push_back(std::move(block));
      continue;
    }

    return false;
  }

  return true;
}

bool parse_assistant_blocks(const Json& json,
                            std::vector<SessionAssistantContentBlock>& blocks) {
  if (!json.is_array()) {
    return false;
  }

  for (const auto& item : json) {
    if (!item.is_object() || !item.contains("type") ||
        !item.at("type").is_string()) {
      return false;
    }

    const std::string type = item.at("type").get<std::string>();
    if (type == "text" && item.contains("text") && item.at("text").is_string()) {
      SessionAssistantContentBlock block;
      block.kind = SessionAssistantContentBlockKind::kText;
      block.text = item.at("text").get<std::string>();
      block.text_signature = optional_json_string(item, "textSignature");
      blocks.push_back(std::move(block));
      continue;
    }

    if (type == "thinking" && item.contains("thinking") &&
        item.at("thinking").is_string()) {
      SessionAssistantContentBlock block;
      block.kind = SessionAssistantContentBlockKind::kThinking;
      block.thinking = item.at("thinking").get<std::string>();
      block.thinking_signature =
          optional_json_string(item, "thinkingSignature");
      block.redacted = optional_json_bool(item, "redacted");
      blocks.push_back(std::move(block));
      continue;
    }

    if (type == "toolCall" && item.contains("id") && item.at("id").is_string() &&
        item.contains("name") && item.at("name").is_string() &&
        item.contains("arguments") && item.at("arguments").is_object()) {
      SessionAssistantContentBlock block;
      block.kind = SessionAssistantContentBlockKind::kToolCall;
      block.tool_call_id = item.at("id").get<std::string>();
      block.tool_name = item.at("name").get<std::string>();
      block.tool_arguments = item.at("arguments");
      block.thought_signature = optional_json_string(item, "thoughtSignature");
      blocks.push_back(std::move(block));
      continue;
    }

    return false;
  }

  return true;
}

std::vector<qt::ai::Content> to_ai_user_content(
    const std::vector<SessionUserContentBlock>& blocks) {
  std::vector<qt::ai::Content> content;
  content.reserve(blocks.size());
  for (const auto& block : blocks) {
    if (block.kind == SessionUserContentBlockKind::kImage) {
      content.push_back(
          qt::ai::ImageContent{block.mime_type, block.data});
    } else {
      content.push_back(qt::ai::TextContent{block.text, block.text_signature});
    }
  }
  return content;
}

std::vector<qt::ai::Content> to_ai_assistant_content(
    const std::vector<SessionAssistantContentBlock>& blocks) {
  std::vector<qt::ai::Content> content;
  content.reserve(blocks.size());
  for (const auto& block : blocks) {
    if (block.kind == SessionAssistantContentBlockKind::kThinking) {
      content.push_back(qt::ai::ThinkingContent{
          block.thinking, block.thinking_signature,
          block.redacted.value_or(false)});
    } else if (block.kind == SessionAssistantContentBlockKind::kToolCall) {
      content.push_back(qt::ai::ToolCallContent{
          block.tool_call_id, block.tool_name, block.tool_arguments});
    } else {
      content.push_back(qt::ai::TextContent{block.text, block.text_signature});
    }
  }
  return content;
}

qt::ai::Message make_text_context_message(std::string text) {
  qt::ai::Message message;
  message.role = qt::ai::MessageRole::user;
  message.content.push_back(
      qt::ai::TextContent{std::move(text), std::nullopt});
  return message;
}

std::optional<qt::ai::Message> custom_message_to_ai(
    const SessionCustomMessageEntry& custom_message) {
  qt::ai::Message message;
  message.role = qt::ai::MessageRole::user;
  if (const auto* text = std::get_if<std::string>(&custom_message.content);
      text != nullptr) {
    message.content.push_back(qt::ai::TextContent{*text, std::nullopt});
  } else {
    message.content = to_ai_user_content(
        std::get<std::vector<SessionUserContentBlock>>(custom_message.content));
  }

  message.id = custom_message.upstream.id;
  message.metadata["role"] = "custom";
  message.metadata["customType"] = custom_message.custom_type;
  message.metadata["display"] = custom_message.display;
  if (custom_message.details) {
    message.metadata["details"] = *custom_message.details;
  }
  if (custom_message.upstream.timestamp) {
    message.metadata["timestamp"] = *custom_message.upstream.timestamp;
  }
  return message;
}

std::optional<qt::ai::Message> raw_message_to_ai(const SessionCustomEntry& entry) {
  if (entry.entry_type != kUpstreamPiMonoSessionEntryType ||
      !entry.payload.is_object() || entry.payload.value("type", std::string{}) !=
                                       "message" ||
      !entry.payload.contains("message") ||
      !entry.payload.at("message").is_object()) {
    return std::nullopt;
  }

  const Json& raw_message = entry.payload.at("message");
  const std::string role = raw_message.value("role", std::string{});
  if (role == "user" || role == "custom") {
    std::vector<SessionUserContentBlock> blocks;
    if (!raw_message.contains("content") ||
        !parse_user_blocks(raw_message.at("content"), blocks)) {
      return std::nullopt;
    }

    qt::ai::Message message;
    message.role = qt::ai::MessageRole::user;
    message.content = to_ai_user_content(blocks);
    if (entry.upstream.id) {
      message.id = *entry.upstream.id;
    }
    message.metadata["role"] = role;
    if (raw_message.contains("timestamp") &&
        raw_message.at("timestamp").is_number_integer()) {
      message.metadata["timestamp"] =
          raw_message.at("timestamp").get<std::int64_t>();
    }
    return message;
  }

  if (role == "assistant") {
    std::vector<SessionAssistantContentBlock> blocks;
    if (!raw_message.contains("content") ||
        !parse_assistant_blocks(raw_message.at("content"), blocks)) {
      return std::nullopt;
    }

    qt::ai::Message message;
    message.role = qt::ai::MessageRole::assistant;
    message.content = to_ai_assistant_content(blocks);
    if (entry.upstream.id) {
      message.id = *entry.upstream.id;
    }
    message.metadata["provider"] = raw_message.value("provider", std::string{});
    message.metadata["model"] = raw_message.value("model", std::string{});
    message.metadata["api"] = raw_message.value("api", std::string{});
    if (raw_message.contains("usage") && raw_message.at("usage").is_object()) {
      message.metadata["usage"] = raw_message.at("usage");
    }
    if (raw_message.contains("stopReason") &&
        raw_message.at("stopReason").is_string()) {
      message.metadata["stopReason"] =
          raw_message.at("stopReason").get<std::string>();
    }
    if (raw_message.contains("timestamp") &&
        raw_message.at("timestamp").is_number_integer()) {
      message.metadata["timestamp"] =
          raw_message.at("timestamp").get<std::int64_t>();
    }
    return message;
  }

  if (role == "bashExecution") {
    if (raw_message.value("excludeFromContext", false)) {
      return std::nullopt;
    }

    std::ostringstream rendered;
    rendered << "Ran `" << raw_message.value("command", std::string{}) << "`\n";
    const std::string output = raw_message.value("output", std::string{});
    if (!output.empty()) {
      rendered << "```\n" << output << "\n```";
    } else {
      rendered << "(no output)";
    }

    if (raw_message.value("cancelled", false)) {
      rendered << "\n\n(command cancelled)";
    } else if (raw_message.contains("exitCode") &&
               raw_message.at("exitCode").is_number_integer() &&
               raw_message.at("exitCode").get<int>() != 0) {
      rendered << "\n\nCommand exited with code "
               << raw_message.at("exitCode").get<int>();
    }

    if (raw_message.value("truncated", false) &&
        raw_message.contains("fullOutputPath") &&
        raw_message.at("fullOutputPath").is_string()) {
      rendered << "\n\n[Output truncated. Full output: "
               << raw_message.at("fullOutputPath").get<std::string>() << "]";
    }

    auto message = make_text_context_message(rendered.str());
    if (entry.upstream.id) {
      message.id = *entry.upstream.id;
    }
    if (raw_message.contains("timestamp") &&
        raw_message.at("timestamp").is_number_integer()) {
      message.metadata["timestamp"] =
          raw_message.at("timestamp").get<std::int64_t>();
    }
    return message;
  }

  return std::nullopt;
}

std::optional<qt::ai::Message> entry_to_ai_message(const SessionEntry& entry) {
  if (const auto* user = std::get_if<SessionUserImageMessageEntry>(&entry.data);
      user != nullptr) {
    qt::ai::Message message;
    message.role = qt::ai::MessageRole::user;
    message.content = to_ai_user_content(user->content);
    message.id = user->upstream.id;
    if (user->message_timestamp) {
      message.metadata["timestamp"] = *user->message_timestamp;
    }
    return message;
  }

  if (const auto* assistant =
          std::get_if<SessionAssistantMessageEntry>(&entry.data);
      assistant != nullptr) {
    qt::ai::Message message;
    message.role = qt::ai::MessageRole::assistant;
    message.content = to_ai_assistant_content(assistant->content);
    message.id = assistant->upstream.id;
    message.metadata["provider"] = assistant->provider;
    message.metadata["model"] = assistant->model;
    message.metadata["api"] = assistant->api;
    message.metadata["usage"] = assistant->usage;
    message.metadata["stopReason"] = assistant->stop_reason;
    if (assistant->response_model) {
      message.metadata["responseModel"] = *assistant->response_model;
    }
    if (assistant->response_id) {
      message.metadata["responseId"] = *assistant->response_id;
    }
    if (assistant->diagnostics) {
      message.metadata["diagnostics"] = *assistant->diagnostics;
    }
    if (assistant->error_message) {
      message.metadata["errorMessage"] = *assistant->error_message;
    }
    if (assistant->message_timestamp) {
      message.metadata["timestamp"] = *assistant->message_timestamp;
    }
    return message;
  }

  if (const auto* custom_message =
          std::get_if<SessionCustomMessageEntry>(&entry.data);
      custom_message != nullptr) {
    return custom_message_to_ai(*custom_message);
  }

  if (const auto* branch_summary =
          std::get_if<SessionBranchSummaryEntry>(&entry.data);
      branch_summary != nullptr) {
    auto message = make_text_context_message(
        std::string(kBranchSummaryPrefix) + branch_summary->summary +
        kBranchSummarySuffix);
    message.id = branch_summary->upstream.id;
    message.metadata["role"] = "branchSummary";
    message.metadata["fromId"] = branch_summary->from_id;
    if (branch_summary->details) {
      message.metadata["details"] = *branch_summary->details;
    }
    if (branch_summary->upstream.timestamp) {
      message.metadata["timestamp"] = *branch_summary->upstream.timestamp;
    }
    return message;
  }

  if (const auto* bash = std::get_if<SessionBashExecutionEntry>(&entry.data);
      bash != nullptr) {
    if (bash->exclude_from_context.value_or(false)) {
      return std::nullopt;
    }

    std::ostringstream rendered;
    rendered << "Ran `" << bash->command << "`\n";
    if (!bash->output.empty()) {
      rendered << "```\n" << bash->output << "\n```";
    } else {
      rendered << "(no output)";
    }

    if (bash->cancelled) {
      rendered << "\n\n(command cancelled)";
    } else if (bash->exit_code.has_value() && *bash->exit_code != 0) {
      rendered << "\n\nCommand exited with code " << *bash->exit_code;
    }

    if (bash->truncated && bash->full_output_path.has_value()) {
      rendered << "\n\n[Output truncated. Full output: "
               << *bash->full_output_path << "]";
    }

    auto message = make_text_context_message(rendered.str());
    message.id = bash->upstream.id;
    return message;
  }

  if (const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
      custom != nullptr) {
    if (custom->entry_type == kUpstreamPiMonoSessionEntryType &&
        custom->payload.is_object() &&
        custom->payload.value("type", std::string{}) == "branch_summary") {
      auto message = make_text_context_message(
          std::string(kBranchSummaryPrefix) +
          custom->payload.value("summary", std::string{}) +
          kBranchSummarySuffix);
      message.id = custom->upstream.id;
      message.metadata["role"] = "branchSummary";
      if (custom->payload.contains("fromId") &&
          custom->payload.at("fromId").is_string()) {
        message.metadata["fromId"] =
            custom->payload.at("fromId").get<std::string>();
      }
      if (custom->upstream.timestamp) {
        message.metadata["timestamp"] = *custom->upstream.timestamp;
      }
      return message;
    }

    return raw_message_to_ai(*custom);
  }

  return std::nullopt;
}

std::optional<SessionManagerModelState> model_state_of(
    const SessionEntry& entry) {
  if (const auto* model = std::get_if<SessionModelChangeEntry>(&entry.data);
      model != nullptr) {
    return SessionManagerModelState{model->provider, model->model_id};
  }

  if (const auto* assistant =
          std::get_if<SessionAssistantMessageEntry>(&entry.data);
      assistant != nullptr) {
    return SessionManagerModelState{assistant->provider, assistant->model};
  }

  if (const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
      custom != nullptr && custom->entry_type == kUpstreamPiMonoSessionEntryType &&
      custom->payload.is_object()) {
    const std::string type = custom->payload.value("type", std::string{});
    if (type == "model_change") {
      const auto provider = optional_json_string(custom->payload, "provider");
      const auto model_id = optional_json_string(custom->payload, "modelId");
      if (provider && model_id) {
        return SessionManagerModelState{*provider, *model_id};
      }
    }

    if (type == "message" && custom->payload.contains("message") &&
        custom->payload.at("message").is_object()) {
      const Json& raw_message = custom->payload.at("message");
      if (raw_message.value("role", std::string{}) == "assistant") {
        const auto provider = optional_json_string(raw_message, "provider");
        const auto model_id = optional_json_string(raw_message, "model");
        if (provider && model_id) {
          return SessionManagerModelState{*provider, *model_id};
        }
      }
    }
  }

  return std::nullopt;
}

std::optional<std::string> thinking_level_of(const SessionEntry& entry) {
  if (const auto* thinking =
          std::get_if<SessionThinkingLevelChangeEntry>(&entry.data);
      thinking != nullptr) {
    return thinking->thinking_level;
  }

  if (const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
      custom != nullptr && custom->entry_type == kUpstreamPiMonoSessionEntryType &&
      custom->payload.is_object() &&
      custom->payload.value("type", std::string{}) ==
          "thinking_level_change") {
    return optional_json_string(custom->payload, "thinkingLevel");
  }

  return std::nullopt;
}

struct CompactionState {
  std::string id;
  std::string first_kept_entry_id;
  std::string summary;
  std::int64_t tokens_before = 0;
  std::optional<Json> details;
  std::optional<std::string> timestamp;
};

std::optional<CompactionState> compaction_state_of(const SessionEntry& entry) {
  if (const auto* compaction = std::get_if<SessionCompactionEntry>(&entry.data);
      compaction != nullptr) {
    const auto id = entry_id(entry);
    if (!id.has_value()) {
      return std::nullopt;
    }

    return CompactionState{*id,
                           compaction->first_kept_entry_id,
                           compaction->summary,
                           compaction->tokens_before,
                           compaction->details,
                           compaction->upstream.timestamp};
  }

  if (const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
      custom != nullptr && custom->entry_type == kUpstreamPiMonoSessionEntryType &&
      custom->payload.is_object() &&
      custom->payload.value("type", std::string{}) == "compaction") {
    const auto id = entry_id(entry);
    const auto first_kept = optional_json_string(custom->payload,
                                                 "firstKeptEntryId");
    if (!id.has_value() || !first_kept.has_value()) {
      return std::nullopt;
    }

    std::optional<Json> details;
    if (custom->payload.contains("details") &&
        !custom->payload.at("details").is_null()) {
      details = custom->payload.at("details");
    }

    return CompactionState{*id,
                           *first_kept,
                           custom->payload.value("summary", std::string{}),
                           custom->payload.value("tokensBefore", 0),
                           details,
                           timestamp_of(entry)};
  }

  return std::nullopt;
}

qt::ai::Message compaction_summary_to_ai(const CompactionState& compaction) {
  auto message = make_text_context_message(
      std::string(kCompactionSummaryPrefix) + compaction.summary +
      kCompactionSummarySuffix);
  message.id = compaction.id;
  message.metadata["role"] = "compactionSummary";
  message.metadata["tokensBefore"] = compaction.tokens_before;
  if (compaction.details) {
    message.metadata["details"] = *compaction.details;
  }
  if (compaction.timestamp) {
    message.metadata["timestamp"] = *compaction.timestamp;
  }
  return message;
}

}  // namespace

SessionManager::SessionManager() = default;

SessionManager::SessionManager(std::vector<SessionEntry> entries)
    : entries_(std::move(entries)) {
  rebuild_index();
}

const SessionEntry* SessionManager::get_leaf_entry() const noexcept {
  if (!leaf_id_.has_value()) {
    return nullptr;
  }
  return get_entry(*leaf_id_);
}

const SessionEntry* SessionManager::get_entry(std::string_view id) const
    noexcept {
  const auto it = entry_indexes_.find(std::string(id));
  if (it == entry_indexes_.end()) {
    return nullptr;
  }
  return &entries_[it->second];
}

std::string SessionManager::append_user_message(std::string text) {
  SessionUserContentBlock block;
  block.kind = SessionUserContentBlockKind::kText;
  block.text = std::move(text);
  return append_user_message(std::vector<SessionUserContentBlock>{block});
}

std::string SessionManager::append_user_message(
    std::vector<SessionUserContentBlock> content,
    std::optional<std::int64_t> message_timestamp) {
  SessionUserImageMessageEntry entry;
  entry.content = std::move(content);
  entry.message_timestamp = message_timestamp;

  SessionEntry session_entry;
  session_entry.data = std::move(entry);
  return append_entry(std::move(session_entry));
}

std::string SessionManager::append_assistant_message(std::string text,
                                                     std::string provider,
                                                     std::string model,
                                                     std::string api,
                                                     std::string stop_reason) {
  SessionAssistantContentBlock block;
  block.kind = SessionAssistantContentBlockKind::kText;
  block.text = std::move(text);

  SessionAssistantMessageEntry entry;
  entry.content.push_back(std::move(block));
  entry.api = std::move(api);
  entry.provider = std::move(provider);
  entry.model = std::move(model);
  entry.stop_reason = std::move(stop_reason);

  return append_assistant_message(std::move(entry));
}

std::string SessionManager::append_assistant_message(
    SessionAssistantMessageEntry entry) {
  SessionEntry session_entry;
  session_entry.data = std::move(entry);
  return append_entry(std::move(session_entry));
}

std::string SessionManager::append_model_change(std::string provider,
                                                std::string model_id) {
  SessionEntry entry;
  entry.data = SessionModelChangeEntry{std::move(provider), std::move(model_id),
                                       SessionUpstreamTreeFields{}};
  return append_entry(std::move(entry));
}

std::string SessionManager::append_thinking_level_change(
    std::string thinking_level) {
  SessionEntry entry;
  entry.data = SessionThinkingLevelChangeEntry{std::move(thinking_level),
                                               SessionUpstreamTreeFields{}};
  return append_entry(std::move(entry));
}

std::string SessionManager::append_session_info(
    std::optional<std::string> name) {
  SessionEntry entry;
  entry.data = SessionInfoEntry{std::move(name), SessionUpstreamTreeFields{}};
  return append_entry(std::move(entry));
}

qt::foundation::Result<std::string> SessionManager::append_label_change(
    std::string target_id, std::optional<std::string> label) {
  if (get_entry(target_id) == nullptr) {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::not_found,
        "Entry " + target_id + " not found");
  }

  const bool clears_label = !label.has_value() || label->empty();
  SessionEntry entry;
  entry.data = SessionLabelEntry{clears_label ? std::string{} : *label,
                                 std::move(target_id), clears_label,
                                 SessionUpstreamTreeFields{}};
  return append_entry(std::move(entry));
}

std::string SessionManager::append_compaction(
    std::string summary, std::string first_kept_entry_id,
    std::int64_t tokens_before, std::optional<Json> details,
    std::optional<bool> from_hook) {
  SessionCompactionEntry compaction;
  compaction.summary = std::move(summary);
  compaction.first_kept_entry_id = std::move(first_kept_entry_id);
  compaction.tokens_before = tokens_before;
  compaction.details = std::move(details);
  compaction.from_hook = from_hook;

  SessionEntry entry;
  entry.data = std::move(compaction);
  return append_entry(std::move(entry));
}

std::string SessionManager::append_extension_state(std::string custom_type,
                                                   Json data) {
  SessionEntry entry;
  entry.data = SessionExtensionStateEntry{
      std::move(custom_type), std::move(data), SessionUpstreamTreeFields{}};
  return append_entry(std::move(entry));
}

std::string SessionManager::append_custom_entry(std::string entry_type,
                                                Json payload) {
  SessionEntry entry;
  entry.data =
      SessionCustomEntry{std::move(entry_type), std::move(payload),
                         SessionUpstreamTreeFields{}};
  return append_entry(std::move(entry));
}

std::string SessionManager::append_custom_message(
    std::string custom_type, SessionCustomMessageContent content, bool display,
    std::optional<Json> details) {
  SessionCustomMessageEntry entry;
  entry.custom_type = std::move(custom_type);
  entry.content = std::move(content);
  entry.display = display;
  entry.details = std::move(details);

  SessionEntry session_entry;
  session_entry.data = std::move(entry);
  return append_entry(std::move(session_entry));
}

std::optional<std::string> SessionManager::get_session_name() const {
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    const auto* entry = std::get_if<SessionInfoEntry>(&it->data);
    if (entry == nullptr) {
      continue;
    }
    if (!entry->name.has_value() || entry->name->empty()) {
      return std::nullopt;
    }
    return entry->name;
  }
  return std::nullopt;
}

std::optional<std::string> SessionManager::get_label(
    std::string_view target_id) const {
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    const auto* entry = std::get_if<SessionLabelEntry>(&it->data);
    if (entry == nullptr || !entry->target_id.has_value() ||
        *entry->target_id != target_id) {
      continue;
    }
    if (entry->clears_label || entry->label.empty()) {
      return std::nullopt;
    }
    return entry->label;
  }
  return std::nullopt;
}

qt::foundation::Status SessionManager::branch(std::string_view branch_from_id) {
  if (get_entry(branch_from_id) == nullptr) {
    return qt::foundation::Status::failure(
        qt::foundation::ErrorCode::not_found,
        "Entry " + std::string(branch_from_id) + " not found");
  }

  leaf_id_ = std::string(branch_from_id);
  return qt::foundation::Status::success();
}

qt::foundation::Result<std::string> SessionManager::branch_with_summary(
    std::optional<std::string> branch_from_id, std::string summary,
    std::optional<Json> details, std::optional<bool> from_hook) {
  if (branch_from_id.has_value() && get_entry(*branch_from_id) == nullptr) {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::not_found,
        "Entry " + *branch_from_id + " not found");
  }

  leaf_id_ = branch_from_id;

  SessionBranchSummaryEntry branch_summary;
  branch_summary.from_id = branch_from_id.value_or("root");
  branch_summary.summary = std::move(summary);
  branch_summary.details = std::move(details);
  branch_summary.from_hook = from_hook;

  SessionEntry entry;
  entry.data = std::move(branch_summary);
  return append_entry(std::move(entry));
}

void SessionManager::reset_leaf() noexcept { leaf_id_.reset(); }

std::vector<SessionEntry> SessionManager::get_branch() const {
  if (!leaf_id_.has_value()) {
    return {};
  }
  return get_branch(*leaf_id_);
}

std::vector<SessionEntry> SessionManager::get_branch(
    std::string_view from_id) const {
  std::vector<SessionEntry> path;
  const SessionEntry* current = get_entry(from_id);
  while (current != nullptr) {
    path.push_back(*current);
    const auto parent = parent_id(*current);
    current = parent.has_value() ? get_entry(*parent) : nullptr;
  }
  std::reverse(path.begin(), path.end());
  return path;
}

std::vector<SessionEntryTreeNode> SessionManager::get_tree() const {
  struct PendingNode {
    std::size_t entry_index = 0;
    std::vector<std::string> children;
  };

  std::unordered_map<std::string, PendingNode> pending;
  std::vector<std::string> roots;

  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const auto id = entry_id(entries_[index]);
    if (!id.has_value()) {
      continue;
    }
    pending[*id] = PendingNode{index, {}};
  }

  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const auto id = entry_id(entries_[index]);
    if (!id.has_value()) {
      continue;
    }

    const auto parent = parent_id(entries_[index]);
    if (!parent.has_value() || *parent == *id) {
      roots.push_back(*id);
      continue;
    }

    const auto parent_it = pending.find(*parent);
    if (parent_it == pending.end()) {
      roots.push_back(*id);
      continue;
    }

    parent_it->second.children.push_back(*id);
  }

  auto sort_children = [this, &pending](std::vector<std::string>& child_ids) {
    std::stable_sort(
        child_ids.begin(), child_ids.end(),
        [this, &pending](const std::string& lhs, const std::string& rhs) {
          const auto lhs_it = pending.find(lhs);
          const auto rhs_it = pending.find(rhs);
          const auto lhs_ts = timestamp_of(entries_[lhs_it->second.entry_index]);
          const auto rhs_ts = timestamp_of(entries_[rhs_it->second.entry_index]);
          if (lhs_ts != rhs_ts) {
            return lhs_ts.value_or(std::string{}) < rhs_ts.value_or(std::string{});
          }
          return lhs_it->second.entry_index < rhs_it->second.entry_index;
        });
  };

  std::function<SessionEntryTreeNode(const std::string&)> build_node =
      [&](const std::string& id) -> SessionEntryTreeNode {
    auto& node = pending.at(id);
    sort_children(node.children);

    SessionEntryTreeNode built{entries_[node.entry_index], {}};
    built.children.reserve(node.children.size());
    for (const auto& child_id : node.children) {
      built.children.push_back(build_node(child_id));
    }
    return built;
  };

  std::vector<SessionEntryTreeNode> tree;
  tree.reserve(roots.size());
  for (const auto& root_id : roots) {
    tree.push_back(build_node(root_id));
  }
  return tree;
}

SessionManagerContext SessionManager::build_session_context() const {
  if (!leaf_id_.has_value()) {
    return SessionManagerContext{};
  }
  return build_session_context(*leaf_id_);
}

SessionManagerContext SessionManager::build_session_context(
    std::string_view leaf_id) const {
  SessionManagerContext context;
  const SessionEntry* leaf = get_entry(leaf_id);
  if (leaf == nullptr) {
    const auto fallback_id = last_tree_entry_id();
    leaf = fallback_id.has_value() ? get_entry(*fallback_id) : nullptr;
  }
  if (leaf == nullptr) {
    return context;
  }

  std::vector<const SessionEntry*> path;
  const SessionEntry* current = leaf;
  while (current != nullptr) {
    path.push_back(current);
    const auto parent = parent_id(*current);
    current = parent.has_value() ? get_entry(*parent) : nullptr;
  }
  std::reverse(path.begin(), path.end());

  std::optional<CompactionState> latest_compaction;
  for (const SessionEntry* entry : path) {
    if (const auto thinking = thinking_level_of(*entry); thinking.has_value()) {
      context.thinking_level = *thinking;
    }

    if (const auto model = model_state_of(*entry); model.has_value()) {
      context.model = *model;
    }

    if (const auto compaction = compaction_state_of(*entry);
        compaction.has_value()) {
      latest_compaction = std::move(compaction);
    }
  }

  auto append_context_message = [&context](const SessionEntry& entry) {
    if (const auto message = entry_to_ai_message(entry); message.has_value()) {
      context.messages.push_back(*message);
    }
  };

  if (!latest_compaction.has_value()) {
    for (const SessionEntry* entry : path) {
      append_context_message(*entry);
    }
    return context;
  }

  context.messages.push_back(compaction_summary_to_ai(*latest_compaction));

  std::size_t compaction_index = path.size();
  for (std::size_t index = 0; index < path.size(); ++index) {
    const auto current_id = entry_id(*path[index]);
    if (current_id.has_value() && *current_id == latest_compaction->id) {
      compaction_index = index;
      break;
    }
  }

  if (compaction_index == path.size()) {
    for (const SessionEntry* entry : path) {
      append_context_message(*entry);
    }
    return context;
  }

  bool found_first_kept = false;
  for (std::size_t index = 0; index < compaction_index; ++index) {
    const auto current_id = entry_id(*path[index]);
    if (current_id.has_value() &&
        *current_id == latest_compaction->first_kept_entry_id) {
      found_first_kept = true;
    }
    if (found_first_kept) {
      append_context_message(*path[index]);
    }
  }

  for (std::size_t index = compaction_index + 1; index < path.size(); ++index) {
    append_context_message(*path[index]);
  }

  return context;
}

void SessionManager::rebuild_index() {
  entry_order_.clear();
  entry_indexes_.clear();
  leaf_id_ = last_tree_entry_id();
  next_id_ = 1;
  next_timestamp_ = 1;

  for (std::size_t index = 0; index < entries_.size(); ++index) {
    const auto id = entry_id(entries_[index]);
    if (id.has_value()) {
      entry_order_.push_back(*id);
      entry_indexes_[*id] = index;
      if (const auto suffix = generated_id_suffix(*id); suffix.has_value()) {
        next_id_ = std::max(next_id_, *suffix + 1U);
      }
    }

    if (const auto timestamp = timestamp_of(entries_[index]);
        timestamp.has_value()) {
      if (const auto seconds = parse_timestamp_seconds(*timestamp);
          seconds.has_value()) {
        next_timestamp_ = std::max(next_timestamp_, *seconds + 1U);
      }
    }
  }
}

std::string SessionManager::append_entry(SessionEntry entry) {
  const std::string id = generate_entry_id();
  const std::string timestamp = generate_timestamp();

  if (SessionUpstreamTreeFields* upstream = entry_upstream(entry);
      upstream != nullptr) {
    upstream->id = id;
    upstream->parent_id = leaf_id_;
    upstream->timestamp = timestamp;
    if (!upstream->source_format.has_value()) {
      upstream->source_format = kUpstreamPiMonoSessionEntryType;
    }
    if (!upstream->source_version.has_value()) {
      upstream->source_version = kPiMonoCurrentSessionVersion;
    }
  }

  entries_.push_back(std::move(entry));
  entry_order_.push_back(id);
  entry_indexes_[id] = entries_.size() - 1U;
  leaf_id_ = id;
  return id;
}

std::string SessionManager::generate_entry_id() {
  while (true) {
    std::ostringstream output;
    output << kGeneratedIdPrefix;
    output.width(8);
    output.fill('0');
    output << next_id_++;
    const std::string candidate = output.str();
    if (entry_indexes_.find(candidate) == entry_indexes_.end()) {
      return candidate;
    }
  }
}

std::string SessionManager::generate_timestamp() {
  const std::time_t raw_time = static_cast<std::time_t>(next_timestamp_++);
  std::tm time_info{};
#if defined(_WIN32)
  gmtime_s(&time_info, &raw_time);
#else
  time_info = *std::gmtime(&raw_time);
#endif

  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S.000Z",
                &time_info);
  return std::string(buffer);
}

std::optional<std::string> SessionManager::last_tree_entry_id() const {
  for (std::size_t index = entries_.size(); index > 0; --index) {
    const auto id = entry_id(entries_[index - 1U]);
    if (id.has_value()) {
      return id;
    }
  }
  return std::nullopt;
}

}  // namespace qt::harness
