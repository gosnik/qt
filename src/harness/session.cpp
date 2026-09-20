#include <qt/harness/session.hpp>
#include <sstream>
#include <stdexcept>

namespace qt::harness {
namespace {

template <typename T>
const T& require_payload(const Json& json) {
  return json.at("payload").get_ref<const T&>();
}

template <typename T>
T decode_payload(const Json& json) {
  return json.at("payload").get<T>();
}

bool is_upstream_pi_mono_entry(const Json& json) {
  return json.is_object() && json.contains("type") &&
         json.at("type").is_string();
}

std::optional<std::string> optional_string_field(const Json& json,
                                                 const char* key) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return std::nullopt;
  }
  return json.at(key).get<std::string>();
}

std::optional<bool> optional_bool_field(const Json& json, const char* key) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return std::nullopt;
  }
  return json.at(key).get<bool>();
}

std::optional<int> optional_int_field(const Json& json, const char* key) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return std::nullopt;
  }
  return json.at(key).get<int>();
}

std::optional<std::int64_t> optional_int64_field(const Json& json,
                                                 const char* key) {
  if (!json.contains(key) || json.at(key).is_null()) {
    return std::nullopt;
  }
  return json.at(key).get<std::int64_t>();
}

bool has_only_allowed_keys(const Json& json,
                           std::initializer_list<const char*> keys) {
  for (const auto& item : json.items()) {
    bool allowed = false;
    for (const char* key : keys) {
      if (item.key() == key) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      return false;
    }
  }
  return true;
}

std::optional<std::vector<SessionUserContentBlock>>
tool_result_content_from_json(const Json& json);

std::optional<SessionUserContentBlock> user_content_block_from_json(
    const Json& json) {
  if (!json.is_object() || !json.contains("type") ||
      !json.at("type").is_string()) {
    return std::nullopt;
  }

  const std::string type = json.at("type").get<std::string>();
  if (type == "text" && json.contains("text") && json.at("text").is_string() &&
      has_only_allowed_keys(json, {"type", "text", "textSignature"})) {
    SessionUserContentBlock block;
    block.kind = SessionUserContentBlockKind::kText;
    block.text = json.at("text").get<std::string>();
    block.text_signature = optional_string_field(json, "textSignature");
    return block;
  }

  if (type == "image" && json.contains("data") && json.at("data").is_string() &&
      json.contains("mimeType") && json.at("mimeType").is_string() &&
      has_only_allowed_keys(json, {"type", "data", "mimeType"})) {
    SessionUserContentBlock block;
    block.kind = SessionUserContentBlockKind::kImage;
    block.data = json.at("data").get<std::string>();
    block.mime_type = json.at("mimeType").get<std::string>();
    return block;
  }

  return std::nullopt;
}

std::optional<std::vector<SessionUserContentBlock>> user_content_from_json(
    const Json& json) {
  if (json.is_string()) {
    SessionUserContentBlock block;
    block.kind = SessionUserContentBlockKind::kText;
    block.text = json.get<std::string>();
    return std::vector<SessionUserContentBlock>{std::move(block)};
  }

  return tool_result_content_from_json(json);
}

std::optional<std::vector<SessionUserContentBlock>>
tool_result_content_from_json(const Json& json) {
  if (!json.is_array()) {
    return std::nullopt;
  }

  std::vector<SessionUserContentBlock> blocks;
  blocks.reserve(json.size());
  for (const auto& item : json) {
    auto block = user_content_block_from_json(item);
    if (!block.has_value()) {
      return std::nullopt;
    }
    blocks.push_back(std::move(*block));
  }
  return blocks;
}

std::optional<SessionCustomMessageContent> custom_message_content_from_json(
    const Json& json) {
  if (json.is_string()) {
    return json.get<std::string>();
  }

  auto blocks = tool_result_content_from_json(json);
  if (blocks.has_value()) {
    return std::move(*blocks);
  }

  return std::nullopt;
}

Json upstream_session_extra_fields_from_json(const Json& json) {
  Json extra = Json::object();
  for (const auto& item : json.items()) {
    if (item.key() == "type" || item.key() == "version" ||
        item.key() == "id" || item.key() == "timestamp" ||
        item.key() == "cwd" || item.key() == "parentSession" ||
        item.key() == "promptHistory") {
      continue;
    }
    extra[item.key()] = item.value();
  }
  return extra;
}

std::optional<SessionAssistantContentBlock> assistant_content_block_from_json(
    const Json& json) {
  if (!json.is_object() || !json.contains("type") ||
      !json.at("type").is_string()) {
    return std::nullopt;
  }

  const std::string type = json.at("type").get<std::string>();
  if (type == "text" && json.contains("text") && json.at("text").is_string() &&
      has_only_allowed_keys(json, {"type", "text", "textSignature"})) {
    SessionAssistantContentBlock block;
    block.kind = SessionAssistantContentBlockKind::kText;
    block.text = json.at("text").get<std::string>();
    block.text_signature = optional_string_field(json, "textSignature");
    return block;
  }

  if (type == "thinking" && json.contains("thinking") &&
      json.at("thinking").is_string() &&
      has_only_allowed_keys(
          json, {"type", "thinking", "thinkingSignature", "redacted"})) {
    SessionAssistantContentBlock block;
    block.kind = SessionAssistantContentBlockKind::kThinking;
    block.thinking = json.at("thinking").get<std::string>();
    block.thinking_signature = optional_string_field(json, "thinkingSignature");
    block.redacted = optional_bool_field(json, "redacted");
    return block;
  }

  if (type == "toolCall" && json.contains("id") && json.at("id").is_string() &&
      json.contains("name") && json.at("name").is_string() &&
      json.contains("arguments") && json.at("arguments").is_object() &&
      has_only_allowed_keys(
          json, {"type", "id", "name", "arguments", "thoughtSignature"})) {
    SessionAssistantContentBlock block;
    block.kind = SessionAssistantContentBlockKind::kToolCall;
    block.tool_call_id = json.at("id").get<std::string>();
    block.tool_name = json.at("name").get<std::string>();
    block.tool_arguments = json.at("arguments");
    block.thought_signature = optional_string_field(json, "thoughtSignature");
    return block;
  }

  return std::nullopt;
}

std::optional<std::vector<SessionAssistantContentBlock>>
assistant_content_from_json(const Json& json) {
  if (!json.is_array()) {
    return std::nullopt;
  }

  std::vector<SessionAssistantContentBlock> blocks;
  blocks.reserve(json.size());
  for (const auto& item : json) {
    auto block = assistant_content_block_from_json(item);
    if (!block.has_value()) {
      return std::nullopt;
    }
    blocks.push_back(std::move(*block));
  }
  return blocks;
}

bool is_known_upstream_message_entry_shape(const Json& json) {
  return has_only_allowed_keys(json, {"type", "id", "parentId", "timestamp",
                                      "message"});
}

SessionUpstreamTreeFields upstream_tree_fields_from_json(const Json& json) {
  SessionUpstreamTreeFields fields;
  fields.id = optional_string_field(json, "id");
  fields.parent_id = optional_string_field(json, "parentId");
  fields.timestamp = optional_string_field(json, "timestamp");
  fields.source_format = kUpstreamPiMonoSessionEntryType;
  fields.source_version = json.value("version", 3);
  return fields;
}

void add_upstream_tree_fields(Json& json,
                              const SessionUpstreamTreeFields& fields) {
  if (fields.id.has_value()) {
    json["id"] = *fields.id;
  }
  if (fields.parent_id.has_value()) {
    json["parentId"] = *fields.parent_id;
  } else {
    json["parentId"] = nullptr;
  }
  if (fields.timestamp.has_value()) {
    json["timestamp"] = *fields.timestamp;
  }
}

SessionEntry decode_upstream_pi_mono_entry(const Json& json) {
  const std::string type = json.at("type").get<std::string>();

  SessionEntry entry;
  if (type == "session") {
    SessionMetadata metadata;
    metadata.session_id = json.at("id").get<std::string>();
    metadata.working_directory = json.value("cwd", std::string{});
    metadata.persistent = true;

    if (json.contains("timestamp") && !json.at("timestamp").is_null()) {
      metadata.created_at = json.at("timestamp").get<std::string>();
    }
    if (json.contains("parentSession") && !json.at("parentSession").is_null()) {
      metadata.parent_session_id = json.at("parentSession").get<std::string>();
    }
    if (json.contains("promptHistory") && json.at("promptHistory").is_array()) {
      for (const auto& item : json.at("promptHistory")) {
        if (item.is_string()) {
          metadata.prompt_history.push_back(item.get<std::string>());
        }
      }
    }
    metadata.source_format = kUpstreamPiMonoSessionEntryType;
    metadata.source_version = optional_int_field(json, "version");
    metadata.upstream_extra = upstream_session_extra_fields_from_json(json);

    entry.data = SessionMetadataEntry{std::move(metadata)};
    return entry;
  }

  if (type == "model_change" && json.contains("provider") &&
      json.at("provider").is_string() && json.contains("modelId") &&
      json.at("modelId").is_string()) {
    entry.data = SessionModelChangeEntry{
        json.at("provider").get<std::string>(),
        json.at("modelId").get<std::string>(),
        upstream_tree_fields_from_json(json),
    };
    return entry;
  }

  if (type == "thinking_level_change" && json.contains("thinkingLevel") &&
      json.at("thinkingLevel").is_string()) {
    entry.data = SessionThinkingLevelChangeEntry{
        json.at("thinkingLevel").get<std::string>(),
        upstream_tree_fields_from_json(json),
    };
    return entry;
  }

  if (type == "session_info" &&
      json.contains("id") && json.at("id").is_string() &&
      json.contains("parentId") &&
      (json.at("parentId").is_null() || json.at("parentId").is_string()) &&
      json.contains("timestamp") && json.at("timestamp").is_string() &&
      (!json.contains("name") || json.at("name").is_null() ||
       json.at("name").is_string()) &&
      has_only_allowed_keys(json,
                            {"type", "id", "parentId", "timestamp", "name"})) {
    entry.data = SessionInfoEntry{
        optional_string_field(json, "name"),
        upstream_tree_fields_from_json(json),
    };
    return entry;
  }

  if (type == "label" && json.contains("id") && json.at("id").is_string() &&
      json.contains("parentId") &&
      (json.at("parentId").is_null() || json.at("parentId").is_string()) &&
      json.contains("timestamp") && json.at("timestamp").is_string() &&
      json.contains("targetId") && json.at("targetId").is_string() &&
      (!json.contains("label") || json.at("label").is_null() ||
       json.at("label").is_string()) &&
      has_only_allowed_keys(json, {"type", "id", "parentId", "timestamp",
                                   "targetId", "label"})) {
    entry.data = SessionLabelEntry{
        optional_string_field(json, "label").value_or(std::string{}),
        json.at("targetId").get<std::string>(),
        !optional_string_field(json, "label").has_value(),
        upstream_tree_fields_from_json(json),
    };
    return entry;
  }

  if (type == "branch_summary" && json.contains("fromId") &&
      json.at("fromId").is_string() && json.contains("summary") &&
      json.at("summary").is_string()) {
    std::optional<Json> details;
    if (json.contains("details") && !json.at("details").is_null()) {
      details = json.at("details");
    }
    entry.data = SessionBranchSummaryEntry{
        json.at("fromId").get<std::string>(),
        json.at("summary").get<std::string>(),
        std::move(details),
        optional_bool_field(json, "fromHook"),
        upstream_tree_fields_from_json(json),
    };
    return entry;
  }

  if (type == "compaction" && json.contains("summary") &&
      json.at("summary").is_string() && json.contains("firstKeptEntryId") &&
      json.at("firstKeptEntryId").is_string() &&
      json.contains("tokensBefore") &&
      json.at("tokensBefore").is_number_integer() &&
      has_only_allowed_keys(json, {"type", "id", "parentId", "timestamp",
                                   "summary", "firstKeptEntryId",
                                   "tokensBefore", "details", "fromHook"})) {
    std::optional<Json> details;
    if (json.contains("details") && !json.at("details").is_null()) {
      details = json.at("details");
    }
    entry.data = SessionCompactionEntry{
        json.at("summary").get<std::string>(),
        json.at("firstKeptEntryId").get<std::string>(),
        json.at("tokensBefore").get<std::int64_t>(),
        std::move(details),
        optional_bool_field(json, "fromHook"),
        upstream_tree_fields_from_json(json),
    };
    return entry;
  }

  if (type == "custom_message" && json.contains("customType") &&
      json.at("customType").is_string() && json.contains("content") &&
      json.contains("display") && json.at("display").is_boolean() &&
      has_only_allowed_keys(json, {"type", "id", "parentId", "timestamp",
                                   "customType", "content", "details",
                                   "display"})) {
    auto content = custom_message_content_from_json(json.at("content"));
    if (content.has_value()) {
      std::optional<Json> details;
      if (json.contains("details")) {
        details = json.at("details");
      }
      entry.data = SessionCustomMessageEntry{
          json.at("customType").get<std::string>(),
          std::move(*content),
          std::move(details),
          json.at("display").get<bool>(),
          false,
          std::nullopt,
          upstream_tree_fields_from_json(json),
      };
      return entry;
    }
  }

  if (type == "custom" && json.contains("customType") &&
      json.at("customType").is_string() &&
      has_only_allowed_keys(json, {"type", "id", "parentId", "timestamp",
                                   "customType", "data"})) {
    entry.data = SessionExtensionStateEntry{
        json.at("customType").get<std::string>(),
        json.contains("data") && !json.at("data").is_null() ? json.at("data")
                                                            : Json::object(),
        upstream_tree_fields_from_json(json),
    };
    return entry;
  }

  if (type == "message" && json.contains("message") &&
      json.at("message").is_object()) {
    const auto& message = json.at("message");
    if (message.value("role", std::string{}) == "toolResult" &&
        message.contains("toolCallId") &&
        message.at("toolCallId").is_string() &&
        message.contains("toolName") && message.at("toolName").is_string() &&
        message.contains("content") && message.at("content").is_array() &&
        message.contains("isError") && message.at("isError").is_boolean() &&
        is_known_upstream_message_entry_shape(json) &&
        has_only_allowed_keys(message,
                              {"role", "toolCallId", "toolName", "content",
                               "details", "isError", "timestamp"})) {
      auto content = tool_result_content_from_json(message.at("content"));
      if (content.has_value()) {
        std::optional<Json> details;
        if (message.contains("details")) {
          details = message.at("details");
        }
        entry.data = SessionToolResultMessageEntry{
            message.at("toolCallId").get<std::string>(),
            message.at("toolName").get<std::string>(),
            std::move(*content),
            std::move(details),
            message.at("isError").get<bool>(),
            optional_int64_field(message, "timestamp"),
            upstream_tree_fields_from_json(json),
        };
        return entry;
      }
    }

    if (message.value("role", std::string{}) == "custom" &&
        message.contains("customType") && message.at("customType").is_string() &&
        message.contains("content") && message.contains("display") &&
        message.at("display").is_boolean() &&
        is_known_upstream_message_entry_shape(json) &&
        has_only_allowed_keys(message,
                              {"role", "customType", "content", "details",
                               "display", "timestamp"})) {
      auto content = custom_message_content_from_json(message.at("content"));
      if (content.has_value()) {
        std::optional<Json> details;
        if (message.contains("details")) {
          details = message.at("details");
        }
        entry.data = SessionCustomMessageEntry{
            message.at("customType").get<std::string>(),
            std::move(*content),
            std::move(details),
            message.at("display").get<bool>(),
            true,
            optional_int64_field(message, "timestamp"),
            upstream_tree_fields_from_json(json),
        };
        return entry;
      }
    }

    if (message.value("role", std::string{}) == "assistant" &&
        message.contains("content") && message.at("content").is_array() &&
        message.contains("api") && message.at("api").is_string() &&
        message.contains("provider") && message.at("provider").is_string() &&
        message.contains("model") && message.at("model").is_string() &&
        message.contains("usage") && message.at("usage").is_object() &&
        message.contains("stopReason") &&
        message.at("stopReason").is_string() &&
        is_known_upstream_message_entry_shape(json) &&
        has_only_allowed_keys(
            message, {"role", "content", "api", "provider", "model",
                      "responseModel", "responseId", "diagnostics", "usage",
                      "stopReason", "errorMessage", "timestamp"})) {
      auto content = assistant_content_from_json(message.at("content"));
      if (content.has_value()) {
        std::optional<Json> diagnostics;
        if (message.contains("diagnostics") &&
            !message.at("diagnostics").is_null()) {
          diagnostics = message.at("diagnostics");
        }
        entry.data = SessionAssistantMessageEntry{
            std::move(*content),
            message.at("api").get<std::string>(),
            message.at("provider").get<std::string>(),
            message.at("model").get<std::string>(),
            optional_string_field(message, "responseModel"),
            optional_string_field(message, "responseId"),
            std::move(diagnostics),
            message.at("usage"),
            message.at("stopReason").get<std::string>(),
            optional_string_field(message, "errorMessage"),
            optional_int64_field(message, "timestamp"),
            upstream_tree_fields_from_json(json),
        };
        return entry;
      }
    }

    if (message.value("role", std::string{}) == "user" &&
        message.contains("content") &&
        is_known_upstream_message_entry_shape(json) &&
        has_only_allowed_keys(message, {"role", "content", "timestamp"})) {
      auto content = user_content_from_json(message.at("content"));
      if (content.has_value()) {
        SessionUserImageMessageEntry user_message;
        user_message.content = std::move(*content);
        user_message.message_timestamp = optional_int64_field(message, "timestamp");
        user_message.serialize_text_content_as_string =
            message.at("content").is_string();
        user_message.upstream = upstream_tree_fields_from_json(json);
        entry.data = std::move(user_message);
        return entry;
      }
    }

    if (message.value("role", std::string{}) == "bashExecution" &&
        message.contains("command") && message.at("command").is_string() &&
        message.contains("output") && message.at("output").is_string() &&
        message.contains("exitCode") &&
        (message.at("exitCode").is_number_integer() ||
         message.at("exitCode").is_null()) &&
        message.contains("cancelled") && message.at("cancelled").is_boolean() &&
        message.contains("truncated") && message.at("truncated").is_boolean()) {
      entry.data = SessionBashExecutionEntry{
          message.at("command").get<std::string>(),
          message.at("output").get<std::string>(),
          optional_int_field(message, "exitCode"),
          message.at("cancelled").get<bool>(),
          message.at("truncated").get<bool>(),
          optional_string_field(message, "fullOutputPath"),
          optional_bool_field(message, "excludeFromContext"),
          optional_int64_field(message, "timestamp"),
          upstream_tree_fields_from_json(json),
      };
      return entry;
    }
  }

  entry.data =
      SessionCustomEntry{kUpstreamPiMonoSessionEntryType, json,
                         SessionUpstreamTreeFields{}};
  return entry;
}

}  // namespace

SessionEntryKind SessionEntry::kind() const noexcept {
  return std::visit(
      [](const auto& value) -> SessionEntryKind {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, SessionMetadataEntry>) {
          return SessionEntryKind::kSessionMetadata;
        }
        if constexpr (std::is_same_v<Value, SessionLabelEntry>) {
          return SessionEntryKind::kLabel;
        }
        if constexpr (std::is_same_v<Value, SessionInfoEntry>) {
          return SessionEntryKind::kSessionInfo;
        }
        if constexpr (std::is_same_v<Value, SessionSummaryEntry>) {
          return SessionEntryKind::kSummary;
        }
        if constexpr (std::is_same_v<Value, SessionModelChangeEntry>) {
          return SessionEntryKind::kModelChange;
        }
        if constexpr (std::is_same_v<Value, SessionThinkingLevelChangeEntry>) {
          return SessionEntryKind::kThinkingLevelChange;
        }
        if constexpr (std::is_same_v<Value, SessionBashExecutionEntry>) {
          return SessionEntryKind::kBashExecution;
        }
        if constexpr (std::is_same_v<Value, SessionBranchSummaryEntry>) {
          return SessionEntryKind::kBranchSummary;
        }
        if constexpr (std::is_same_v<Value, SessionCompactionEntry>) {
          return SessionEntryKind::kCompaction;
        }
        if constexpr (std::is_same_v<Value, SessionUserImageMessageEntry>) {
          return SessionEntryKind::kUserImageMessage;
        }
        if constexpr (std::is_same_v<Value, SessionAssistantMessageEntry>) {
          return SessionEntryKind::kAssistantMessage;
        }
        if constexpr (std::is_same_v<Value, SessionToolResultMessageEntry>) {
          return SessionEntryKind::kToolResultMessage;
        }
        if constexpr (std::is_same_v<Value, SessionCustomMessageEntry>) {
          return SessionEntryKind::kCustomMessage;
        }
        if constexpr (std::is_same_v<Value, SessionExtensionStateEntry>) {
          return SessionEntryKind::kExtensionState;
        }
        return SessionEntryKind::kCustom;
      },
      data);
}

std::string to_string(SessionEntryKind kind) {
  switch (kind) {
    case SessionEntryKind::kSessionMetadata:
      return "session_metadata";
    case SessionEntryKind::kLabel:
      return "label";
    case SessionEntryKind::kSessionInfo:
      return "session_info";
    case SessionEntryKind::kSummary:
      return "summary";
    case SessionEntryKind::kModelChange:
      return "model_change";
    case SessionEntryKind::kThinkingLevelChange:
      return "thinking_level_change";
    case SessionEntryKind::kBashExecution:
      return "bash_execution";
    case SessionEntryKind::kBranchSummary:
      return "branch_summary";
    case SessionEntryKind::kCompaction:
      return "compaction";
    case SessionEntryKind::kUserImageMessage:
      return "user_image_message";
    case SessionEntryKind::kAssistantMessage:
      return "assistant_message";
    case SessionEntryKind::kToolResultMessage:
      return "tool_result_message";
    case SessionEntryKind::kCustomMessage:
      return "custom_message";
    case SessionEntryKind::kExtensionState:
      return "extension_state";
    case SessionEntryKind::kCustom:
      return "custom";
  }

  return "custom";
}

bool parse_session_entry_kind(std::string_view text, SessionEntryKind& kind) {
  if (text == "session_metadata") {
    kind = SessionEntryKind::kSessionMetadata;
    return true;
  }
  if (text == "label") {
    kind = SessionEntryKind::kLabel;
    return true;
  }
  if (text == "session_info") {
    kind = SessionEntryKind::kSessionInfo;
    return true;
  }
  if (text == "summary") {
    kind = SessionEntryKind::kSummary;
    return true;
  }
  if (text == "model_change") {
    kind = SessionEntryKind::kModelChange;
    return true;
  }
  if (text == "thinking_level_change") {
    kind = SessionEntryKind::kThinkingLevelChange;
    return true;
  }
  if (text == "bash_execution") {
    kind = SessionEntryKind::kBashExecution;
    return true;
  }
  if (text == "branch_summary") {
    kind = SessionEntryKind::kBranchSummary;
    return true;
  }
  if (text == "compaction") {
    kind = SessionEntryKind::kCompaction;
    return true;
  }
  if (text == "user_image_message") {
    kind = SessionEntryKind::kUserImageMessage;
    return true;
  }
  if (text == "assistant_message") {
    kind = SessionEntryKind::kAssistantMessage;
    return true;
  }
  if (text == "tool_result_message") {
    kind = SessionEntryKind::kToolResultMessage;
    return true;
  }
  if (text == "custom_message") {
    kind = SessionEntryKind::kCustomMessage;
    return true;
  }
  if (text == "extension_state") {
    kind = SessionEntryKind::kExtensionState;
    return true;
  }
  if (text == "custom") {
    kind = SessionEntryKind::kCustom;
    return true;
  }
  return false;
}

void to_json(Json& json, const SessionMetadata& metadata) {
  json = Json{
      {"session_id", metadata.session_id},
      {"working_directory", metadata.working_directory},
      {"persistent", metadata.persistent},
  };

  if (metadata.display_name) {
    json["display_name"] = *metadata.display_name;
  }
  if (metadata.parent_session_id) {
    json["parent_session_id"] = *metadata.parent_session_id;
  }
  if (metadata.created_at) {
    json["created_at"] = *metadata.created_at;
  }
  if (!metadata.prompt_history.empty()) {
    json["prompt_history"] = metadata.prompt_history;
  }
  if (metadata.source_format) {
    json["source_format"] = *metadata.source_format;
  }
  if (metadata.source_version) {
    json["source_version"] = *metadata.source_version;
  }
  if (!metadata.upstream_extra.empty()) {
    json["upstream_extra"] = metadata.upstream_extra;
  }
}

void from_json(const Json& json, SessionMetadata& metadata) {
  metadata.session_id = json.at("session_id").get<std::string>();
  metadata.working_directory = json.at("working_directory").get<std::string>();
  metadata.persistent = json.value("persistent", true);

  if (json.contains("display_name") && !json.at("display_name").is_null()) {
    metadata.display_name = json.at("display_name").get<std::string>();
  } else {
    metadata.display_name.reset();
  }

  if (json.contains("parent_session_id") &&
      !json.at("parent_session_id").is_null()) {
    metadata.parent_session_id =
        json.at("parent_session_id").get<std::string>();
  } else {
    metadata.parent_session_id.reset();
  }

  if (json.contains("created_at") && !json.at("created_at").is_null()) {
    metadata.created_at = json.at("created_at").get<std::string>();
  } else {
    metadata.created_at.reset();
  }

  metadata.prompt_history.clear();
  if (json.contains("prompt_history") && json.at("prompt_history").is_array()) {
    for (const auto& item : json.at("prompt_history")) {
      if (item.is_string()) {
        metadata.prompt_history.push_back(item.get<std::string>());
      }
    }
  }

  if (json.contains("source_format") && !json.at("source_format").is_null()) {
    metadata.source_format = json.at("source_format").get<std::string>();
  } else {
    metadata.source_format.reset();
  }

  if (json.contains("source_version") && !json.at("source_version").is_null()) {
    metadata.source_version = json.at("source_version").get<int>();
  } else {
    metadata.source_version.reset();
  }

  if (json.contains("upstream_extra") &&
      json.at("upstream_extra").is_object()) {
    metadata.upstream_extra = json.at("upstream_extra");
  } else {
    metadata.upstream_extra = Json::object();
  }
}

void to_json(Json& json, const SessionMetadataEntry& entry) {
  json = Json{{"metadata", entry.metadata}};
}

void from_json(const Json& json, SessionMetadataEntry& entry) {
  entry.metadata = json.at("metadata").get<SessionMetadata>();
}

void to_json(Json& json, const SessionLabelEntry& entry) {
  json = Json{{"label", entry.label}};
  if (entry.target_id) {
    json["target_id"] = *entry.target_id;
  }
  if (entry.clears_label) {
    json["clears_label"] = entry.clears_label;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionLabelEntry& entry) {
  entry.label = json.value("label", std::string{});
  entry.target_id = optional_string_field(json, "target_id");
  entry.clears_label = json.value("clears_label", false);
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionInfoEntry& entry) {
  json = Json::object();
  if (entry.name) {
    json["name"] = *entry.name;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionInfoEntry& entry) {
  entry.name = optional_string_field(json, "name");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionSummaryEntry& entry) {
  json = Json{{"summary", entry.summary}};
}

void from_json(const Json& json, SessionSummaryEntry& entry) {
  entry.summary = json.at("summary").get<std::string>();
}

void to_json(Json& json, const SessionUpstreamTreeFields& fields) {
  json = Json::object();
  if (fields.id) {
    json["id"] = *fields.id;
  }
  if (fields.parent_id) {
    json["parent_id"] = *fields.parent_id;
  }
  if (fields.timestamp) {
    json["timestamp"] = *fields.timestamp;
  }
  if (fields.source_format) {
    json["source_format"] = *fields.source_format;
  }
  if (fields.source_version) {
    json["source_version"] = *fields.source_version;
  }
}

void from_json(const Json& json, SessionUpstreamTreeFields& fields) {
  if (json.contains("id") && !json.at("id").is_null()) {
    fields.id = json.at("id").get<std::string>();
  } else {
    fields.id.reset();
  }

  if (json.contains("parent_id") && !json.at("parent_id").is_null()) {
    fields.parent_id = json.at("parent_id").get<std::string>();
  } else {
    fields.parent_id.reset();
  }

  if (json.contains("timestamp") && !json.at("timestamp").is_null()) {
    fields.timestamp = json.at("timestamp").get<std::string>();
  } else {
    fields.timestamp.reset();
  }

  if (json.contains("source_format") && !json.at("source_format").is_null()) {
    fields.source_format = json.at("source_format").get<std::string>();
  } else {
    fields.source_format.reset();
  }

  if (json.contains("source_version") && !json.at("source_version").is_null()) {
    fields.source_version = json.at("source_version").get<int>();
  } else {
    fields.source_version.reset();
  }
}

void to_json(Json& json, const SessionModelChangeEntry& entry) {
  json = Json{
      {"provider", entry.provider},
      {"model_id", entry.model_id},
  };
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionModelChangeEntry& entry) {
  entry.provider = json.at("provider").get<std::string>();
  entry.model_id = json.at("model_id").get<std::string>();
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionThinkingLevelChangeEntry& entry) {
  json = Json{{"thinking_level", entry.thinking_level}};
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionThinkingLevelChangeEntry& entry) {
  entry.thinking_level = json.at("thinking_level").get<std::string>();
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionBashExecutionEntry& entry) {
  json = Json{
      {"command", entry.command},
      {"output", entry.output},
      {"cancelled", entry.cancelled},
      {"truncated", entry.truncated},
  };
  if (entry.exit_code) {
    json["exit_code"] = *entry.exit_code;
  }
  if (entry.full_output_path) {
    json["full_output_path"] = *entry.full_output_path;
  }
  if (entry.exclude_from_context) {
    json["exclude_from_context"] = *entry.exclude_from_context;
  }
  if (entry.message_timestamp) {
    json["message_timestamp"] = *entry.message_timestamp;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionBashExecutionEntry& entry) {
  entry.command = json.at("command").get<std::string>();
  entry.output = json.at("output").get<std::string>();
  entry.exit_code = optional_int_field(json, "exit_code");
  entry.cancelled = json.value("cancelled", false);
  entry.truncated = json.value("truncated", false);
  entry.full_output_path = optional_string_field(json, "full_output_path");
  entry.exclude_from_context =
      optional_bool_field(json, "exclude_from_context");
  entry.message_timestamp = optional_int64_field(json, "message_timestamp");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionBranchSummaryEntry& entry) {
  json = Json{
      {"from_id", entry.from_id},
      {"summary", entry.summary},
  };
  if (entry.details) {
    json["details"] = *entry.details;
  }
  if (entry.from_hook) {
    json["from_hook"] = *entry.from_hook;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionBranchSummaryEntry& entry) {
  entry.from_id = json.at("from_id").get<std::string>();
  entry.summary = json.at("summary").get<std::string>();
  if (json.contains("details") && !json.at("details").is_null()) {
    entry.details = json.at("details");
  } else {
    entry.details.reset();
  }
  entry.from_hook = optional_bool_field(json, "from_hook");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionCompactionEntry& entry) {
  json = Json{
      {"summary", entry.summary},
      {"first_kept_entry_id", entry.first_kept_entry_id},
      {"tokens_before", entry.tokens_before},
  };
  if (entry.details) {
    json["details"] = *entry.details;
  }
  if (entry.from_hook) {
    json["from_hook"] = *entry.from_hook;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionCompactionEntry& entry) {
  entry.summary = json.at("summary").get<std::string>();
  entry.first_kept_entry_id =
      json.at("first_kept_entry_id").get<std::string>();
  entry.tokens_before = json.at("tokens_before").get<std::int64_t>();
  if (json.contains("details") && !json.at("details").is_null()) {
    entry.details = json.at("details");
  } else {
    entry.details.reset();
  }
  entry.from_hook = optional_bool_field(json, "from_hook");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionUserContentBlock& block) {
  if (block.kind == SessionUserContentBlockKind::kImage) {
    json = Json{
        {"type", "image"},
        {"data", block.data},
        {"mime_type", block.mime_type},
    };
    return;
  }

  json = Json{
      {"type", "text"},
      {"text", block.text},
  };
  if (block.text_signature) {
    json["text_signature"] = *block.text_signature;
  }
}

void from_json(const Json& json, SessionUserContentBlock& block) {
  const std::string type = json.at("type").get<std::string>();
  if (type == "image") {
    block.kind = SessionUserContentBlockKind::kImage;
    block.data = json.at("data").get<std::string>();
    block.mime_type = json.at("mime_type").get<std::string>();
    block.text.clear();
    block.text_signature.reset();
    return;
  }
  if (type == "text") {
    block.kind = SessionUserContentBlockKind::kText;
    block.text = json.at("text").get<std::string>();
    block.text_signature = optional_string_field(json, "text_signature");
    block.data.clear();
    block.mime_type.clear();
    return;
  }
  throw std::runtime_error("unsupported user content block type");
}

void to_json(Json& json, const SessionUserImageMessageEntry& entry) {
  json = Json{{"content", entry.content}};
  if (entry.message_timestamp) {
    json["message_timestamp"] = *entry.message_timestamp;
  }
  if (entry.serialize_text_content_as_string) {
    json["serialize_text_content_as_string"] = true;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionUserImageMessageEntry& entry) {
  entry.content = json.at("content").get<std::vector<SessionUserContentBlock>>();
  entry.message_timestamp = optional_int64_field(json, "message_timestamp");
  entry.serialize_text_content_as_string =
      json.value("serialize_text_content_as_string", false);
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionAssistantContentBlock& block) {
  if (block.kind == SessionAssistantContentBlockKind::kThinking) {
    json = Json{
        {"type", "thinking"},
        {"thinking", block.thinking},
    };
    if (block.thinking_signature) {
      json["thinking_signature"] = *block.thinking_signature;
    }
    if (block.redacted) {
      json["redacted"] = *block.redacted;
    }
    return;
  }

  if (block.kind == SessionAssistantContentBlockKind::kToolCall) {
    json = Json{
        {"type", "tool_call"},
        {"id", block.tool_call_id},
        {"name", block.tool_name},
        {"arguments", block.tool_arguments},
    };
    if (block.thought_signature) {
      json["thought_signature"] = *block.thought_signature;
    }
    return;
  }

  json = Json{
      {"type", "text"},
      {"text", block.text},
  };
  if (block.text_signature) {
    json["text_signature"] = *block.text_signature;
  }
}

void from_json(const Json& json, SessionAssistantContentBlock& block) {
  const std::string type = json.at("type").get<std::string>();
  if (type == "thinking") {
    block.kind = SessionAssistantContentBlockKind::kThinking;
    block.thinking = json.at("thinking").get<std::string>();
    block.thinking_signature =
        optional_string_field(json, "thinking_signature");
    block.redacted = optional_bool_field(json, "redacted");
    block.text.clear();
    block.text_signature.reset();
    block.tool_call_id.clear();
    block.tool_name.clear();
    block.tool_arguments = Json::object();
    block.thought_signature.reset();
    return;
  }
  if (type == "tool_call") {
    block.kind = SessionAssistantContentBlockKind::kToolCall;
    block.tool_call_id = json.at("id").get<std::string>();
    block.tool_name = json.at("name").get<std::string>();
    block.tool_arguments = json.at("arguments");
    block.thought_signature = optional_string_field(json, "thought_signature");
    block.text.clear();
    block.text_signature.reset();
    block.thinking.clear();
    block.thinking_signature.reset();
    block.redacted.reset();
    return;
  }
  if (type == "text") {
    block.kind = SessionAssistantContentBlockKind::kText;
    block.text = json.at("text").get<std::string>();
    block.text_signature = optional_string_field(json, "text_signature");
    block.thinking.clear();
    block.thinking_signature.reset();
    block.redacted.reset();
    block.tool_call_id.clear();
    block.tool_name.clear();
    block.tool_arguments = Json::object();
    block.thought_signature.reset();
    return;
  }
  throw std::runtime_error("unsupported assistant content block type");
}

void to_json(Json& json, const SessionAssistantMessageEntry& entry) {
  json = Json{
      {"content", entry.content},
      {"api", entry.api},
      {"provider", entry.provider},
      {"model", entry.model},
      {"usage", entry.usage},
      {"stop_reason", entry.stop_reason},
  };
  if (entry.response_model) {
    json["response_model"] = *entry.response_model;
  }
  if (entry.response_id) {
    json["response_id"] = *entry.response_id;
  }
  if (entry.diagnostics) {
    json["diagnostics"] = *entry.diagnostics;
  }
  if (entry.error_message) {
    json["error_message"] = *entry.error_message;
  }
  if (entry.message_timestamp) {
    json["message_timestamp"] = *entry.message_timestamp;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionAssistantMessageEntry& entry) {
  entry.content =
      json.at("content").get<std::vector<SessionAssistantContentBlock>>();
  entry.api = json.at("api").get<std::string>();
  entry.provider = json.at("provider").get<std::string>();
  entry.model = json.at("model").get<std::string>();
  entry.response_model = optional_string_field(json, "response_model");
  entry.response_id = optional_string_field(json, "response_id");
  if (json.contains("diagnostics") && !json.at("diagnostics").is_null()) {
    entry.diagnostics = json.at("diagnostics");
  } else {
    entry.diagnostics.reset();
  }
  entry.usage = json.at("usage");
  entry.stop_reason = json.at("stop_reason").get<std::string>();
  entry.error_message = optional_string_field(json, "error_message");
  entry.message_timestamp = optional_int64_field(json, "message_timestamp");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionToolResultMessageEntry& entry) {
  json = Json{
      {"tool_call_id", entry.tool_call_id},
      {"tool_name", entry.tool_name},
      {"content", entry.content},
      {"is_error", entry.is_error},
  };
  if (entry.details) {
    json["details"] = *entry.details;
  }
  if (entry.message_timestamp) {
    json["message_timestamp"] = *entry.message_timestamp;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionToolResultMessageEntry& entry) {
  entry.tool_call_id = json.at("tool_call_id").get<std::string>();
  entry.tool_name = json.at("tool_name").get<std::string>();
  entry.content = json.at("content").get<std::vector<SessionUserContentBlock>>();
  if (json.contains("details")) {
    entry.details = json.at("details");
  } else {
    entry.details.reset();
  }
  entry.is_error = json.at("is_error").get<bool>();
  entry.message_timestamp = optional_int64_field(json, "message_timestamp");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionCustomMessageEntry& entry) {
  json = Json{
      {"custom_type", entry.custom_type},
      {"display", entry.display},
  };
  std::visit([&json](const auto& content) { json["content"] = content; },
             entry.content);
  if (entry.details) {
    json["details"] = *entry.details;
  }
  if (entry.serialize_as_message_role) {
    json["serialize_as_message_role"] = true;
  }
  if (entry.message_timestamp) {
    json["message_timestamp"] = *entry.message_timestamp;
  }
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionCustomMessageEntry& entry) {
  entry.custom_type = json.at("custom_type").get<std::string>();
  if (json.at("content").is_string()) {
    entry.content = json.at("content").get<std::string>();
  } else {
    entry.content =
        json.at("content").get<std::vector<SessionUserContentBlock>>();
  }
  if (json.contains("details")) {
    entry.details = json.at("details");
  } else {
    entry.details.reset();
  }
  entry.display = json.at("display").get<bool>();
  entry.serialize_as_message_role =
      json.value("serialize_as_message_role", false);
  entry.message_timestamp = optional_int64_field(json, "message_timestamp");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionExtensionStateEntry& entry) {
  json = Json{{"custom_type", entry.custom_type}, {"data", entry.data}};
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionExtensionStateEntry& entry) {
  entry.custom_type = json.at("custom_type").get<std::string>();
  entry.data = json.contains("data") ? json.at("data") : Json::object();
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionCustomEntry& entry) {
  json = Json{
      {"entry_type", entry.entry_type},
      {"payload", entry.payload},
  };
  Json upstream = entry.upstream;
  if (!upstream.empty()) {
    json["upstream"] = std::move(upstream);
  }
}

void from_json(const Json& json, SessionCustomEntry& entry) {
  entry.entry_type = json.at("entry_type").get<std::string>();
  entry.payload = json.at("payload");
  if (json.contains("upstream") && json.at("upstream").is_object()) {
    entry.upstream = json.at("upstream").get<SessionUpstreamTreeFields>();
  } else {
    entry.upstream = SessionUpstreamTreeFields{};
  }
}

void to_json(Json& json, const SessionEntry& entry) {
  json = Json{
      {"version", entry.version},
      {"kind", to_string(entry.kind())},
  };

  std::visit([&json](const auto& value) { json["payload"] = value; },
             entry.data);
}

void from_json(const Json& json, SessionEntry& entry) {
  entry.version = json.at("version").get<int>();
  if (entry.version != kSessionEntryVersion) {
    throw std::runtime_error("unsupported session entry version");
  }

  SessionEntryKind kind = SessionEntryKind::kCustom;
  const auto kind_text = json.at("kind").get<std::string>();
  if (!parse_session_entry_kind(kind_text, kind)) {
    throw std::runtime_error("unsupported session entry kind");
  }

  switch (kind) {
    case SessionEntryKind::kSessionMetadata:
      entry.data = decode_payload<SessionMetadataEntry>(json);
      return;
    case SessionEntryKind::kLabel:
      entry.data = decode_payload<SessionLabelEntry>(json);
      return;
    case SessionEntryKind::kSessionInfo:
      entry.data = decode_payload<SessionInfoEntry>(json);
      return;
    case SessionEntryKind::kSummary:
      entry.data = decode_payload<SessionSummaryEntry>(json);
      return;
    case SessionEntryKind::kModelChange:
      entry.data = decode_payload<SessionModelChangeEntry>(json);
      return;
    case SessionEntryKind::kThinkingLevelChange:
      entry.data = decode_payload<SessionThinkingLevelChangeEntry>(json);
      return;
    case SessionEntryKind::kBashExecution:
      entry.data = decode_payload<SessionBashExecutionEntry>(json);
      return;
    case SessionEntryKind::kBranchSummary:
      entry.data = decode_payload<SessionBranchSummaryEntry>(json);
      return;
    case SessionEntryKind::kCompaction:
      entry.data = decode_payload<SessionCompactionEntry>(json);
      return;
    case SessionEntryKind::kUserImageMessage:
      entry.data = decode_payload<SessionUserImageMessageEntry>(json);
      return;
    case SessionEntryKind::kAssistantMessage:
      entry.data = decode_payload<SessionAssistantMessageEntry>(json);
      return;
    case SessionEntryKind::kToolResultMessage:
      entry.data = decode_payload<SessionToolResultMessageEntry>(json);
      return;
    case SessionEntryKind::kCustomMessage:
      entry.data = decode_payload<SessionCustomMessageEntry>(json);
      return;
    case SessionEntryKind::kExtensionState:
      entry.data = decode_payload<SessionExtensionStateEntry>(json);
      return;
    case SessionEntryKind::kCustom:
      entry.data = decode_payload<SessionCustomEntry>(json);
      return;
  }
}

namespace {

std::optional<std::int64_t> numeric_int64_field(const Json& json,
                                                const char* key) {
  if (!json.is_object() || !json.contains(key) || json.at(key).is_null() ||
      !json.at(key).is_number()) {
    return std::nullopt;
  }
  if (json.at(key).is_number_unsigned()) {
    return static_cast<std::int64_t>(json.at(key).get<std::uint64_t>());
  }
  if (json.at(key).is_number_integer()) {
    return json.at(key).get<std::int64_t>();
  }
  return static_cast<std::int64_t>(json.at(key).get<double>());
}

std::int64_t usage_field(const Json& usage, const char* camel_key,
                         const char* snake_key) {
  if (auto value = numeric_int64_field(usage, camel_key)) {
    return *value;
  }
  if (auto value = numeric_int64_field(usage, snake_key)) {
    return *value;
  }
  return 0;
}

std::int64_t usage_field(const Json& usage, const char* primary_key,
                         const char* secondary_key,
                         const char* tertiary_key) {
  if (auto value = numeric_int64_field(usage, primary_key)) {
    return *value;
  }
  if (auto value = numeric_int64_field(usage, secondary_key)) {
    return *value;
  }
  if (auto value = numeric_int64_field(usage, tertiary_key)) {
    return *value;
  }
  return 0;
}

std::optional<double> numeric_double_field(const Json& json, const char* key) {
  if (!json.is_object() || !json.contains(key) || json.at(key).is_null() ||
      !json.at(key).is_number()) {
    return std::nullopt;
  }
  return json.at(key).get<double>();
}

bool usage_has_known_values(const Json& usage) {
  if (!usage.is_object() || usage.empty()) {
    return false;
  }
  static constexpr const char* kKnownKeys[] = {
      "input",       "input_tokens",       "output",
      "output_tokens", "cacheRead",        "cache_read",
      "cache_read_tokens", "cacheWrite",   "cache_write",
      "cache_write_tokens", "totalTokens", "total_tokens",
      "cost",        "total_cost_usd"};
  for (const auto* key : kKnownKeys) {
    if (usage.contains(key) && !usage.at(key).is_null()) {
      return true;
    }
  }
  return false;
}

double usage_cost_total(const Json& usage) {
  if (auto value = numeric_double_field(usage, "total_cost_usd")) {
    return *value;
  }
  if (!usage.is_object() || !usage.contains("cost") ||
      !usage.at("cost").is_object()) {
    return 0.0;
  }
  const auto& cost = usage.at("cost");
  if (auto value = numeric_double_field(cost, "total")) {
    return *value;
  }
  double total = 0.0;
  for (const auto* key : {"input", "output", "cacheRead", "cacheWrite"}) {
    if (auto value = numeric_double_field(cost, key)) {
      total += *value;
    }
  }
  return total;
}

SessionTokenTotals token_totals_from_usage(const Json& usage) {
  SessionTokenTotals totals;
  totals.input = usage_field(usage, "input", "input_tokens", "input");
  totals.output = usage_field(usage, "output", "output_tokens", "output");
  totals.cache_read =
      usage_field(usage, "cacheRead", "cache_read_tokens", "cache_read");
  totals.cache_write =
      usage_field(usage, "cacheWrite", "cache_write_tokens", "cache_write");
  totals.total = usage_field(usage, "totalTokens", "total_tokens");
  if (totals.total == 0) {
    totals.total =
        totals.input + totals.output + totals.cache_read + totals.cache_write;
  }
  return totals;
}

void add_token_totals(SessionTokenTotals& accumulator,
                      const SessionTokenTotals& next) {
  accumulator.input += next.input;
  accumulator.output += next.output;
  accumulator.cache_read += next.cache_read;
  accumulator.cache_write += next.cache_write;
  accumulator.total += next.total;
}

std::optional<std::int64_t> compaction_tokens_before(
    const SessionEntry& entry) {
  const auto* compaction = std::get_if<SessionCompactionEntry>(&entry.data);
  if (compaction != nullptr) {
    return compaction->tokens_before;
  }

  const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
  if (custom == nullptr || custom->entry_type != kUpstreamPiMonoSessionEntryType ||
      !custom->payload.is_object() ||
      custom->payload.value("type", std::string{}) != "compaction") {
    return std::nullopt;
  }
  return numeric_int64_field(custom->payload, "tokensBefore");
}

std::string model_stats_key(const SessionAssistantMessageEntry& assistant) {
  if (!assistant.provider.empty() && !assistant.model.empty()) {
    return assistant.provider + "/" + assistant.model;
  }
  if (!assistant.model.empty()) {
    return assistant.model;
  }
  if (!assistant.provider.empty()) {
    return assistant.provider;
  }
  return "unknown";
}

}  // namespace

SessionStats compute_session_stats(
    const std::vector<SessionEntry>& entries,
    std::optional<std::int64_t> context_window) {
  SessionStats stats;
  std::optional<std::int64_t> current_context_tokens = 0;

  for (const auto& entry : entries) {
    if (const auto tokens_before = compaction_tokens_before(entry);
        tokens_before.has_value()) {
      stats.tokens = SessionTokenTotals{};
      stats.tokens.input = *tokens_before;
      stats.tokens.total = *tokens_before;
      current_context_tokens.reset();
      continue;
    }

    if (std::holds_alternative<SessionUserImageMessageEntry>(entry.data)) {
      ++stats.user_messages;
      ++stats.total_messages;
      continue;
    }
    if (std::holds_alternative<SessionToolResultMessageEntry>(entry.data)) {
      ++stats.tool_results;
      ++stats.total_messages;
      continue;
    }
    if (std::holds_alternative<SessionBashExecutionEntry>(entry.data) ||
        std::holds_alternative<SessionCustomMessageEntry>(entry.data)) {
      ++stats.total_messages;
      continue;
    }

    const auto* assistant =
        std::get_if<SessionAssistantMessageEntry>(&entry.data);
    if (assistant == nullptr) {
      continue;
    }

    ++stats.assistant_messages;
    ++stats.total_messages;
    for (const auto& block : assistant->content) {
      if (!block.tool_call_id.empty() || !block.tool_name.empty()) {
        ++stats.tool_calls;
      }
    }

    auto& model_stats = stats.models[model_stats_key(*assistant)];
    ++model_stats.assistant_messages;

    if (!usage_has_known_values(assistant->usage)) {
      stats.has_unknown_usage = true;
      model_stats.has_unknown_usage = true;
      current_context_tokens.reset();
      continue;
    }

    const SessionTokenTotals usage = token_totals_from_usage(assistant->usage);
    add_token_totals(stats.tokens, usage);
    add_token_totals(model_stats.tokens, usage);
    const auto cost = usage_cost_total(assistant->usage);
    stats.cost += cost;
    model_stats.cost += cost;
    // Provider-normalized usage separates cache hits/writes from uncached
    // input. All three categories still occupy the model's current prompt
    // context and must contribute to context-window pressure.
    current_context_tokens =
        usage.input + usage.cache_read + usage.cache_write;
  }

  if (context_window.has_value()) {
    SessionContextUsage usage;
    usage.context_window = *context_window;
    usage.tokens = current_context_tokens;
    if (current_context_tokens.has_value() && *context_window > 0) {
      usage.percent =
          (static_cast<double>(*current_context_tokens) /
           static_cast<double>(*context_window)) *
          100.0;
    }
    stats.context_usage = usage;
  }

  return stats;
}

std::string encode_session_entry_jsonl(const SessionEntry& entry) {
  if (const auto* metadata = std::get_if<SessionMetadataEntry>(&entry.data);
      metadata != nullptr &&
      metadata->metadata.source_format == kUpstreamPiMonoSessionEntryType) {
    Json json{
        {"type", "session"},
        {"id", metadata->metadata.session_id},
        {"timestamp", metadata->metadata.created_at.value_or(std::string{})},
        {"cwd", metadata->metadata.working_directory},
    };
    if (metadata->metadata.source_version.has_value()) {
      json["version"] = *metadata->metadata.source_version;
    }
    if (metadata->metadata.parent_session_id.has_value()) {
      json["parentSession"] = *metadata->metadata.parent_session_id;
    }
    if (!metadata->metadata.prompt_history.empty()) {
      json["promptHistory"] = metadata->metadata.prompt_history;
    }
    if (metadata->metadata.upstream_extra.is_object()) {
      for (const auto& item : metadata->metadata.upstream_extra.items()) {
        json[item.key()] = item.value();
      }
    }
    return json.dump() + "\n";
  }

  if (const auto* model = std::get_if<SessionModelChangeEntry>(&entry.data);
      model != nullptr &&
      model->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json json{
        {"type", "model_change"},
        {"provider", model->provider},
        {"modelId", model->model_id},
    };
    add_upstream_tree_fields(json, model->upstream);
    return json.dump() + "\n";
  }

  if (const auto* thinking =
          std::get_if<SessionThinkingLevelChangeEntry>(&entry.data);
      thinking != nullptr &&
      thinking->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json json{
        {"type", "thinking_level_change"},
        {"thinkingLevel", thinking->thinking_level},
    };
    add_upstream_tree_fields(json, thinking->upstream);
    return json.dump() + "\n";
  }

  if (const auto* label = std::get_if<SessionLabelEntry>(&entry.data);
      label != nullptr &&
      label->upstream.source_format == kUpstreamPiMonoSessionEntryType &&
      label->target_id.has_value()) {
    Json json{
        {"type", "label"},
        {"targetId", *label->target_id},
    };
    if (!label->clears_label) {
      json["label"] = label->label;
    }
    add_upstream_tree_fields(json, label->upstream);
    return json.dump() + "\n";
  }

  if (const auto* session_info = std::get_if<SessionInfoEntry>(&entry.data);
      session_info != nullptr &&
      session_info->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json json{
        {"type", "session_info"},
    };
    if (session_info->name) {
      json["name"] = *session_info->name;
    }
    add_upstream_tree_fields(json, session_info->upstream);
    return json.dump() + "\n";
  }

  if (const auto* bash = std::get_if<SessionBashExecutionEntry>(&entry.data);
      bash != nullptr &&
      bash->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json message{
        {"role", "bashExecution"},
        {"command", bash->command},
        {"output", bash->output},
        {"exitCode", bash->exit_code ? Json(*bash->exit_code) : Json(nullptr)},
        {"cancelled", bash->cancelled},
        {"truncated", bash->truncated},
    };
    if (bash->full_output_path) {
      message["fullOutputPath"] = *bash->full_output_path;
    }
    if (bash->exclude_from_context) {
      message["excludeFromContext"] = *bash->exclude_from_context;
    }
    if (bash->message_timestamp) {
      message["timestamp"] = *bash->message_timestamp;
    }

    Json json{
        {"type", "message"},
        {"message", std::move(message)},
    };
    add_upstream_tree_fields(json, bash->upstream);
    return json.dump() + "\n";
  }

  if (const auto* branch_summary =
          std::get_if<SessionBranchSummaryEntry>(&entry.data);
      branch_summary != nullptr && branch_summary->upstream.source_format ==
                                       kUpstreamPiMonoSessionEntryType) {
    Json json{
        {"type", "branch_summary"},
        {"fromId", branch_summary->from_id},
        {"summary", branch_summary->summary},
    };
    if (branch_summary->details) {
      json["details"] = *branch_summary->details;
    }
    if (branch_summary->from_hook) {
      json["fromHook"] = *branch_summary->from_hook;
    }
    add_upstream_tree_fields(json, branch_summary->upstream);
    return json.dump() + "\n";
  }

  if (const auto* compaction =
          std::get_if<SessionCompactionEntry>(&entry.data);
      compaction != nullptr &&
      compaction->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json json{
        {"type", "compaction"},
        {"summary", compaction->summary},
        {"firstKeptEntryId", compaction->first_kept_entry_id},
        {"tokensBefore", compaction->tokens_before},
    };
    if (compaction->details) {
      json["details"] = *compaction->details;
    }
    if (compaction->from_hook) {
      json["fromHook"] = *compaction->from_hook;
    }
    add_upstream_tree_fields(json, compaction->upstream);
    return json.dump() + "\n";
  }

  if (const auto* user_image =
          std::get_if<SessionUserImageMessageEntry>(&entry.data);
      user_image != nullptr &&
      user_image->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json content;
    if (user_image->serialize_text_content_as_string &&
        user_image->content.size() == 1U &&
        user_image->content.front().kind == SessionUserContentBlockKind::kText &&
        !user_image->content.front().text_signature) {
      content = user_image->content.front().text;
    } else {
      content = Json::array();
      for (const auto& block : user_image->content) {
        if (block.kind == SessionUserContentBlockKind::kImage) {
          content.push_back(Json{
              {"type", "image"},
              {"data", block.data},
              {"mimeType", block.mime_type},
          });
        } else {
          Json text_block{
              {"type", "text"},
              {"text", block.text},
          };
          if (block.text_signature) {
            text_block["textSignature"] = *block.text_signature;
          }
          content.push_back(std::move(text_block));
        }
      }
    }

    Json message{
        {"role", "user"},
        {"content", std::move(content)},
    };
    if (user_image->message_timestamp) {
      message["timestamp"] = *user_image->message_timestamp;
    }

    Json json{
        {"type", "message"},
        {"message", std::move(message)},
    };
    add_upstream_tree_fields(json, user_image->upstream);
    return json.dump() + "\n";
  }

  if (const auto* assistant =
          std::get_if<SessionAssistantMessageEntry>(&entry.data);
      assistant != nullptr &&
      assistant->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json content = Json::array();
    for (const auto& block : assistant->content) {
      if (block.kind == SessionAssistantContentBlockKind::kThinking) {
        Json thinking_block{
            {"type", "thinking"},
            {"thinking", block.thinking},
        };
        if (block.thinking_signature) {
          thinking_block["thinkingSignature"] = *block.thinking_signature;
        }
        if (block.redacted) {
          thinking_block["redacted"] = *block.redacted;
        }
        content.push_back(std::move(thinking_block));
      } else if (block.kind == SessionAssistantContentBlockKind::kToolCall) {
        Json tool_call_block{
            {"type", "toolCall"},
            {"id", block.tool_call_id},
            {"name", block.tool_name},
            {"arguments", block.tool_arguments},
        };
        if (block.thought_signature) {
          tool_call_block["thoughtSignature"] = *block.thought_signature;
        }
        content.push_back(std::move(tool_call_block));
      } else {
        Json text_block{
            {"type", "text"},
            {"text", block.text},
        };
        if (block.text_signature) {
          text_block["textSignature"] = *block.text_signature;
        }
        content.push_back(std::move(text_block));
      }
    }

    Json message{
        {"role", "assistant"},
        {"content", std::move(content)},
        {"api", assistant->api},
        {"provider", assistant->provider},
        {"model", assistant->model},
        {"usage", assistant->usage},
        {"stopReason", assistant->stop_reason},
    };
    if (assistant->response_model) {
      message["responseModel"] = *assistant->response_model;
    }
    if (assistant->response_id) {
      message["responseId"] = *assistant->response_id;
    }
    if (assistant->diagnostics) {
      message["diagnostics"] = *assistant->diagnostics;
    }
    if (assistant->error_message) {
      message["errorMessage"] = *assistant->error_message;
    }
    if (assistant->message_timestamp) {
      message["timestamp"] = *assistant->message_timestamp;
    }

    Json json{
        {"type", "message"},
        {"message", std::move(message)},
    };
    add_upstream_tree_fields(json, assistant->upstream);
    return json.dump() + "\n";
  }

  if (const auto* tool_result =
          std::get_if<SessionToolResultMessageEntry>(&entry.data);
      tool_result != nullptr &&
      tool_result->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json content = Json::array();
    for (const auto& block : tool_result->content) {
      if (block.kind == SessionUserContentBlockKind::kImage) {
        content.push_back(Json{
            {"type", "image"},
            {"data", block.data},
            {"mimeType", block.mime_type},
        });
      } else {
        Json text_block{
            {"type", "text"},
            {"text", block.text},
        };
        if (block.text_signature) {
          text_block["textSignature"] = *block.text_signature;
        }
        content.push_back(std::move(text_block));
      }
    }

    Json message{
        {"role", "toolResult"},
        {"toolCallId", tool_result->tool_call_id},
        {"toolName", tool_result->tool_name},
        {"content", std::move(content)},
        {"isError", tool_result->is_error},
    };
    if (tool_result->details) {
      message["details"] = *tool_result->details;
    }
    if (tool_result->message_timestamp) {
      message["timestamp"] = *tool_result->message_timestamp;
    }

    Json json{
        {"type", "message"},
        {"message", std::move(message)},
    };
    add_upstream_tree_fields(json, tool_result->upstream);
    return json.dump() + "\n";
  }

  if (const auto* custom_message =
          std::get_if<SessionCustomMessageEntry>(&entry.data);
      custom_message != nullptr && custom_message->upstream.source_format ==
                                       kUpstreamPiMonoSessionEntryType) {
    Json content;
    if (const auto* text =
            std::get_if<std::string>(&custom_message->content);
        text != nullptr) {
      content = *text;
    } else {
      content = Json::array();
      for (const auto& block :
           std::get<std::vector<SessionUserContentBlock>>(
               custom_message->content)) {
        if (block.kind == SessionUserContentBlockKind::kImage) {
          content.push_back(Json{
              {"type", "image"},
              {"data", block.data},
              {"mimeType", block.mime_type},
          });
        } else {
          Json text_block{
              {"type", "text"},
              {"text", block.text},
          };
          if (block.text_signature) {
            text_block["textSignature"] = *block.text_signature;
          }
          content.push_back(std::move(text_block));
        }
      }
    }

    if (custom_message->serialize_as_message_role) {
      Json message{
          {"role", "custom"},
          {"customType", custom_message->custom_type},
          {"content", std::move(content)},
          {"display", custom_message->display},
      };
      if (custom_message->details) {
        message["details"] = *custom_message->details;
      }
      if (custom_message->message_timestamp) {
        message["timestamp"] = *custom_message->message_timestamp;
      }

      Json json{
          {"type", "message"},
          {"message", std::move(message)},
      };
      add_upstream_tree_fields(json, custom_message->upstream);
      return json.dump() + "\n";
    }

    Json json{
        {"type", "custom_message"},
        {"customType", custom_message->custom_type},
        {"content", std::move(content)},
        {"display", custom_message->display},
    };
    if (custom_message->details) {
      json["details"] = *custom_message->details;
    }
    add_upstream_tree_fields(json, custom_message->upstream);
    return json.dump() + "\n";
  }

  if (const auto* extension_state =
          std::get_if<SessionExtensionStateEntry>(&entry.data);
      extension_state != nullptr &&
      extension_state->upstream.source_format == kUpstreamPiMonoSessionEntryType) {
    Json json{
        {"type", "custom"},
        {"customType", extension_state->custom_type},
    };
    if (!extension_state->data.is_null() && !extension_state->data.empty()) {
      json["data"] = extension_state->data;
    }
    add_upstream_tree_fields(json, extension_state->upstream);
    return json.dump() + "\n";
  }

  if (const auto* custom = std::get_if<SessionCustomEntry>(&entry.data);
      custom != nullptr &&
      custom->entry_type == kUpstreamPiMonoSessionEntryType &&
      is_upstream_pi_mono_entry(custom->payload)) {
    return custom->payload.dump() + "\n";
  }

  Json json = entry;
  return json.dump() + "\n";
}

SessionEntryDecodeResult decode_session_entry_jsonl(std::string_view line) {
  SessionEntryDecodeResult result;

  try {
    Json json = Json::parse(line);
    if (is_upstream_pi_mono_entry(json) && !json.contains("kind")) {
      result.entry = decode_upstream_pi_mono_entry(json);
    } else {
      result.entry = json.get<SessionEntry>();
    }
  } catch (const std::exception& error) {
    result.error = error.what();
  }

  return result;
}

}  // namespace qt::harness
