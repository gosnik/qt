#include <qt/app/rpc/rpc_protocol.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace qt::app {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

void require_object(const Json& json, const char* context) {
  if (!json.is_object()) {
    throw std::runtime_error(std::string(context) + " must be a JSON object");
  }
}

std::optional<std::string> decode_optional_id(const Json& json) {
  if (!json.contains("id") || json.at("id").is_null()) {
    return std::nullopt;
  }
  if (!json.at("id").is_string()) {
    throw std::runtime_error("rpc id must be a string when present");
  }
  return json.at("id").get<std::string>();
}

void encode_optional_id(Json& json, const std::optional<std::string>& id) {
  if (id.has_value()) {
    json["id"] = *id;
  }
}

std::size_t require_size(const Json& json, const char* field) {
  const Json& value = json.at(field);
  if (!value.is_number_unsigned() && !value.is_number_integer()) {
    throw std::runtime_error(std::string(field) + " must be an integer");
  }
  const auto parsed = value.get<long long>();
  if (parsed < 0) {
    throw std::runtime_error(std::string(field) + " must not be negative");
  }
  return static_cast<std::size_t>(parsed);
}

std::string require_non_empty_string(const Json& json, const char* field) {
  const auto value = json.at(field).get<std::string>();
  if (value.empty()) {
    throw std::runtime_error(std::string(field) + " must not be empty");
  }
  return value;
}

Json require_object_field(const Json& json, const char* field) {
  const Json& value = json.at(field);
  if (!value.is_object()) {
    throw std::runtime_error(std::string(field) + " must be a JSON object");
  }
  return value;
}

Json require_array_field(const Json& json, const char* field) {
  const Json& value = json.at(field);
  if (!value.is_array()) {
    throw std::runtime_error(std::string(field) + " must be a JSON array");
  }
  return value;
}

void require_command_type(const Json& json,
                          const char* expected,
                          const char* context) {
  if (json.at("type").get<std::string>() != expected) {
    throw std::runtime_error(std::string(context) + " type must equal '" +
                             expected + "'");
  }
}

bool is_supported_thinking_level(std::string_view level) noexcept {
  return level == "off" || level == "minimal" || level == "low" ||
         level == "medium" || level == "high" || level == "xhigh";
}

bool rpc_success_data_is_forbidden(std::string_view command) noexcept {
  return command == "prompt" || command == "steer" ||
         command == "follow_up" || command == "abort" ||
         command == "set_thinking_level" ||
         command == "set_steering_mode" ||
         command == "set_follow_up_mode" ||
         command == "set_auto_compaction" ||
         command == "set_auto_retry" || command == "abort_retry" ||
         command == "abort_bash" || command == "set_session_name";
}

bool rpc_success_data_is_required(std::string_view command) noexcept {
  return command == "new_session" || command == "get_messages" ||
         command == "cycle_model" || command == "get_available_models" ||
         command == "compact" || command == "bash" ||
         command == "get_session_stats" || command == "export_html" ||
         command == "switch_session" || command == "fork" ||
         command == "clone" || command == "get_fork_messages" ||
         command == "get_last_assistant_text" || command == "get_commands" ||
         command == "memory" || command == "subagent";
}

bool is_valid_compaction_reason(std::string_view reason) noexcept {
  return reason == "manual" || reason == "threshold" || reason == "overflow";
}

Json preserve_extension_ui_payload(const Json& json) {
  Json payload = Json::object();
  for (auto it = json.begin(); it != json.end(); ++it) {
    if (it.key() != "type" && it.key() != "id" && it.key() != "method") {
      payload[it.key()] = it.value();
    }
  }
  return payload;
}

Json flatten_extension_ui_payload(Json payload,
                                  std::string type,
                                  std::string id,
                                  std::optional<std::string> method) {
  if (!payload.is_object()) {
    payload = Json::object();
  }
  payload["type"] = std::move(type);
  payload["id"] = std::move(id);
  if (method.has_value()) {
    payload["method"] = std::move(*method);
  }
  return payload;
}

std::string trim_single_line_ending(std::string_view line) {
  if (line.empty()) {
    throw std::runtime_error("rpc frame must not be empty");
  }

  std::string payload(line);
  if (!payload.empty() && payload.back() == '\n') {
    payload.pop_back();
    if (!payload.empty() && payload.back() == '\r') {
      payload.pop_back();
    }
  }

  if (payload.empty()) {
    throw std::runtime_error("rpc frame payload must not be empty");
  }
  if (payload.find('\n') != std::string::npos ||
      payload.find('\r') != std::string::npos) {
    throw std::runtime_error("rpc frame must contain exactly one LF-delimited record");
  }

  return payload;
}

Result<Json> parse_json_line(std::string_view line) {
  try {
    return Json::parse(trim_single_line_ending(line));
  } catch (const std::exception& error) {
    return Result<Json>::failure(ErrorCode::encoding_error, error.what());
  }
}

template <typename T>
Result<T> decode_json_value(const Json& json) {
  try {
    return json.get<T>();
  } catch (const std::exception& error) {
    return Result<T>::failure(ErrorCode::encoding_error, error.what());
  }
}

template <typename T>
Result<T> decode_json_line_value(std::string_view line) {
  const auto parsed = parse_json_line(line);
  if (!parsed) {
    return parsed.error();
  }
  return decode_json_value<T>(parsed.value());
}

Result<std::string> encode_json_line(const Json& json) {
  try {
    require_object(json, "rpc frame");
    return json.dump() + "\n";
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

}  // namespace

std::string to_string(RpcStreamingBehavior behavior) {
  switch (behavior) {
    case RpcStreamingBehavior::kSteer:
      return "steer";
    case RpcStreamingBehavior::kFollowUp:
      return "followUp";
  }

  return "steer";
}

bool parse_rpc_streaming_behavior(std::string_view text,
                                  RpcStreamingBehavior& behavior) noexcept {
  if (text == "steer") {
    behavior = RpcStreamingBehavior::kSteer;
    return true;
  }
  if (text == "followUp") {
    behavior = RpcStreamingBehavior::kFollowUp;
    return true;
  }
  return false;
}

std::string to_string(RpcCommandKind kind) {
  switch (kind) {
    case RpcCommandKind::kPrompt:
      return "prompt";
    case RpcCommandKind::kSteer:
      return "steer";
    case RpcCommandKind::kFollowUp:
      return "follow_up";
    case RpcCommandKind::kAbort:
      return "abort";
    case RpcCommandKind::kGetState:
      return "get_state";
    case RpcCommandKind::kGetMessages:
      return "get_messages";
    case RpcCommandKind::kSetModel:
      return "set_model";
    case RpcCommandKind::kCycleModel:
      return "cycle_model";
    case RpcCommandKind::kGetAvailableModels:
      return "get_available_models";
    case RpcCommandKind::kSetThinkingLevel:
      return "set_thinking_level";
    case RpcCommandKind::kCycleThinkingLevel:
      return "cycle_thinking_level";
    case RpcCommandKind::kSetSteeringMode:
      return "set_steering_mode";
    case RpcCommandKind::kSetFollowUpMode:
      return "set_follow_up_mode";
    case RpcCommandKind::kCompact:
      return "compact";
    case RpcCommandKind::kSetAutoCompaction:
      return "set_auto_compaction";
    case RpcCommandKind::kSetAutoRetry:
      return "set_auto_retry";
    case RpcCommandKind::kAbortRetry:
      return "abort_retry";
    case RpcCommandKind::kBash:
      return "bash";
    case RpcCommandKind::kAbortBash:
      return "abort_bash";
    case RpcCommandKind::kNewSession:
      return "new_session";
    case RpcCommandKind::kSwitchSession:
      return "switch_session";
    case RpcCommandKind::kFork:
      return "fork";
    case RpcCommandKind::kClone:
      return "clone";
    case RpcCommandKind::kGetSessionStats:
      return "get_session_stats";
    case RpcCommandKind::kGetForkMessages:
      return "get_fork_messages";
    case RpcCommandKind::kExportHtml:
      return "export_html";
    case RpcCommandKind::kGetLastAssistantText:
      return "get_last_assistant_text";
    case RpcCommandKind::kSetSessionName:
      return "set_session_name";
    case RpcCommandKind::kGetCommands:
      return "get_commands";
    case RpcCommandKind::kMemory:
      return "memory";
    case RpcCommandKind::kSubagent:
      return "subagent";
  }

  return "prompt";
}

bool parse_rpc_command_kind(std::string_view text, RpcCommandKind& kind) noexcept {
  if (text == "prompt") {
    kind = RpcCommandKind::kPrompt;
    return true;
  }
  if (text == "steer") {
    kind = RpcCommandKind::kSteer;
    return true;
  }
  if (text == "follow_up") {
    kind = RpcCommandKind::kFollowUp;
    return true;
  }
  if (text == "abort") {
    kind = RpcCommandKind::kAbort;
    return true;
  }
  if (text == "get_state") {
    kind = RpcCommandKind::kGetState;
    return true;
  }
  if (text == "get_messages") {
    kind = RpcCommandKind::kGetMessages;
    return true;
  }
  if (text == "set_model") {
    kind = RpcCommandKind::kSetModel;
    return true;
  }
  if (text == "cycle_model") {
    kind = RpcCommandKind::kCycleModel;
    return true;
  }
  if (text == "get_available_models") {
    kind = RpcCommandKind::kGetAvailableModels;
    return true;
  }
  if (text == "set_thinking_level") {
    kind = RpcCommandKind::kSetThinkingLevel;
    return true;
  }
  if (text == "cycle_thinking_level") {
    kind = RpcCommandKind::kCycleThinkingLevel;
    return true;
  }
  if (text == "set_steering_mode") {
    kind = RpcCommandKind::kSetSteeringMode;
    return true;
  }
  if (text == "set_follow_up_mode") {
    kind = RpcCommandKind::kSetFollowUpMode;
    return true;
  }
  if (text == "compact") {
    kind = RpcCommandKind::kCompact;
    return true;
  }
  if (text == "set_auto_compaction") {
    kind = RpcCommandKind::kSetAutoCompaction;
    return true;
  }
  if (text == "set_auto_retry") {
    kind = RpcCommandKind::kSetAutoRetry;
    return true;
  }
  if (text == "abort_retry") {
    kind = RpcCommandKind::kAbortRetry;
    return true;
  }
  if (text == "bash") {
    kind = RpcCommandKind::kBash;
    return true;
  }
  if (text == "abort_bash") {
    kind = RpcCommandKind::kAbortBash;
    return true;
  }
  if (text == "new_session") {
    kind = RpcCommandKind::kNewSession;
    return true;
  }
  if (text == "switch_session") {
    kind = RpcCommandKind::kSwitchSession;
    return true;
  }
  if (text == "fork") {
    kind = RpcCommandKind::kFork;
    return true;
  }
  if (text == "clone") {
    kind = RpcCommandKind::kClone;
    return true;
  }
  if (text == "get_session_stats") {
    kind = RpcCommandKind::kGetSessionStats;
    return true;
  }
  if (text == "get_fork_messages") {
    kind = RpcCommandKind::kGetForkMessages;
    return true;
  }
  if (text == "export_html") {
    kind = RpcCommandKind::kExportHtml;
    return true;
  }
  if (text == "get_last_assistant_text") {
    kind = RpcCommandKind::kGetLastAssistantText;
    return true;
  }
  if (text == "set_session_name") {
    kind = RpcCommandKind::kSetSessionName;
    return true;
  }
  if (text == "get_commands") {
    kind = RpcCommandKind::kGetCommands;
    return true;
  }
  if (text == "memory") {
    kind = RpcCommandKind::kMemory;
    return true;
  }
  if (text == "subagent") {
    kind = RpcCommandKind::kSubagent;
    return true;
  }
  return false;
}

RpcCommandKind rpc_command_kind(const RpcCommand& command) noexcept {
  return std::visit(
      [](const auto& value) -> RpcCommandKind {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, PromptCommand>) {
          return value.command;
        } else if constexpr (std::is_same_v<Value, AbortCommand>) {
          return RpcCommandKind::kAbort;
        } else if constexpr (std::is_same_v<Value, GetStateCommand>) {
          return RpcCommandKind::kGetState;
        } else if constexpr (std::is_same_v<Value, GetMessagesCommand>) {
          return RpcCommandKind::kGetMessages;
        } else if constexpr (std::is_same_v<Value, NewSessionCommand>) {
          return RpcCommandKind::kNewSession;
        } else if constexpr (std::is_same_v<Value, SwitchSessionCommand>) {
          return RpcCommandKind::kSwitchSession;
        } else if constexpr (std::is_same_v<Value, ForkCommand>) {
          return RpcCommandKind::kFork;
        } else if constexpr (std::is_same_v<Value, CloneCommand>) {
          return RpcCommandKind::kClone;
        } else if constexpr (std::is_same_v<Value, SetModelCommand>) {
          return RpcCommandKind::kSetModel;
        } else if constexpr (std::is_same_v<Value, CycleModelCommand>) {
          return RpcCommandKind::kCycleModel;
        } else if constexpr (std::is_same_v<Value, GetAvailableModelsCommand>) {
          return RpcCommandKind::kGetAvailableModels;
        } else if constexpr (std::is_same_v<Value, SetThinkingLevelCommand>) {
          return RpcCommandKind::kSetThinkingLevel;
        } else if constexpr (std::is_same_v<Value, CycleThinkingLevelCommand>) {
          return RpcCommandKind::kCycleThinkingLevel;
        } else if constexpr (std::is_same_v<Value, CompactCommand>) {
          return RpcCommandKind::kCompact;
        } else if constexpr (std::is_same_v<Value, AbortRetryCommand>) {
          return RpcCommandKind::kAbortRetry;
        } else if constexpr (std::is_same_v<Value, BashCommand>) {
          return RpcCommandKind::kBash;
        } else if constexpr (std::is_same_v<Value, AbortBashCommand>) {
          return RpcCommandKind::kAbortBash;
        } else if constexpr (std::is_same_v<Value, GetSessionStatsCommand>) {
          return RpcCommandKind::kGetSessionStats;
        } else if constexpr (std::is_same_v<Value, GetForkMessagesCommand>) {
          return RpcCommandKind::kGetForkMessages;
        } else if constexpr (std::is_same_v<Value, ExportHtmlCommand>) {
          return RpcCommandKind::kExportHtml;
        } else if constexpr (std::is_same_v<Value,
                                             GetLastAssistantTextCommand>) {
          return RpcCommandKind::kGetLastAssistantText;
        } else if constexpr (std::is_same_v<Value, SetSessionNameCommand>) {
          return RpcCommandKind::kSetSessionName;
        } else if constexpr (std::is_same_v<Value, GetCommandsCommand>) {
          return RpcCommandKind::kGetCommands;
        } else if constexpr (std::is_same_v<Value, MemoryCommand>) {
          return RpcCommandKind::kMemory;
        } else if constexpr (std::is_same_v<Value, SubagentCommand>) {
          return RpcCommandKind::kSubagent;
        } else {
          return value.command;
        }
      },
      command);
}

std::string to_string(RpcResponseKind kind) {
  switch (kind) {
    case RpcResponseKind::kSuccess:
      return "success";
    case RpcResponseKind::kError:
      return "error";
    case RpcResponseKind::kGetState:
      return "get_state";
    case RpcResponseKind::kSetModel:
      return "set_model";
  }

  return "success";
}

RpcResponseKind rpc_response_kind(const RpcResponse& response) noexcept {
  return std::visit(
      [](const auto& value) -> RpcResponseKind {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, RpcSuccessResponse>) {
          return RpcResponseKind::kSuccess;
        }
        if constexpr (std::is_same_v<Value, RpcErrorResponse>) {
          return RpcResponseKind::kError;
        }
        if constexpr (std::is_same_v<Value, GetStateResponse>) {
          return RpcResponseKind::kGetState;
        }
        return RpcResponseKind::kSetModel;
      },
      response);
}

std::string to_string(RpcEventKind kind) {
  switch (kind) {
    case RpcEventKind::kAgentStart:
      return "agent_start";
    case RpcEventKind::kAgentEnd:
      return "agent_end";
    case RpcEventKind::kTurnStart:
      return "turn_start";
    case RpcEventKind::kTurnEnd:
      return "turn_end";
    case RpcEventKind::kMessageStart:
      return "message_start";
    case RpcEventKind::kMessageUpdate:
      return "message_update";
    case RpcEventKind::kToolExecutionStart:
      return "tool_execution_start";
    case RpcEventKind::kToolExecutionUpdate:
      return "tool_execution_update";
    case RpcEventKind::kToolExecutionEnd:
      return "tool_execution_end";
    case RpcEventKind::kQueueUpdate:
      return "queue_update";
    case RpcEventKind::kCompactionStart:
      return "compaction_start";
    case RpcEventKind::kCompactionEnd:
      return "compaction_end";
    case RpcEventKind::kAutoRetryStart:
      return "auto_retry_start";
    case RpcEventKind::kAutoRetryEnd:
      return "auto_retry_end";
    case RpcEventKind::kExtensionError:
      return "extension_error";
    case RpcEventKind::kExtensionUiRequest:
      return "extension_ui_request";
    case RpcEventKind::kMessageEnd:
      return "message_end";
  }

  return "agent_start";
}

bool parse_rpc_event_kind(std::string_view text, RpcEventKind& kind) noexcept {
  if (text == "agent_start") {
    kind = RpcEventKind::kAgentStart;
    return true;
  }
  if (text == "agent_end") {
    kind = RpcEventKind::kAgentEnd;
    return true;
  }
  if (text == "turn_start") {
    kind = RpcEventKind::kTurnStart;
    return true;
  }
  if (text == "turn_end") {
    kind = RpcEventKind::kTurnEnd;
    return true;
  }
  if (text == "message_start") {
    kind = RpcEventKind::kMessageStart;
    return true;
  }
  if (text == "message_update") {
    kind = RpcEventKind::kMessageUpdate;
    return true;
  }
  if (text == "tool_execution_start") {
    kind = RpcEventKind::kToolExecutionStart;
    return true;
  }
  if (text == "tool_execution_update") {
    kind = RpcEventKind::kToolExecutionUpdate;
    return true;
  }
  if (text == "tool_execution_end") {
    kind = RpcEventKind::kToolExecutionEnd;
    return true;
  }
  if (text == "queue_update") {
    kind = RpcEventKind::kQueueUpdate;
    return true;
  }
  if (text == "compaction_start") {
    kind = RpcEventKind::kCompactionStart;
    return true;
  }
  if (text == "compaction_end") {
    kind = RpcEventKind::kCompactionEnd;
    return true;
  }
  if (text == "auto_retry_start") {
    kind = RpcEventKind::kAutoRetryStart;
    return true;
  }
  if (text == "auto_retry_end") {
    kind = RpcEventKind::kAutoRetryEnd;
    return true;
  }
  if (text == "extension_error") {
    kind = RpcEventKind::kExtensionError;
    return true;
  }
  if (text == "extension_ui_request") {
    kind = RpcEventKind::kExtensionUiRequest;
    return true;
  }
  if (text == "message_end") {
    kind = RpcEventKind::kMessageEnd;
    return true;
  }
  return false;
}

RpcEventKind rpc_event_kind(const RpcEvent& event) noexcept {
  return std::visit(
      [](const auto& value) -> RpcEventKind {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, AgentStartEvent>) {
          return RpcEventKind::kAgentStart;
        }
        if constexpr (std::is_same_v<Value, AgentEndEvent>) {
          return RpcEventKind::kAgentEnd;
        }
        if constexpr (std::is_same_v<Value, TurnStartEvent>) {
          return RpcEventKind::kTurnStart;
        }
        if constexpr (std::is_same_v<Value, TurnEndEvent>) {
          return RpcEventKind::kTurnEnd;
        }
        if constexpr (std::is_same_v<Value, MessageStartEvent>) {
          return RpcEventKind::kMessageStart;
        }
        if constexpr (std::is_same_v<Value, MessageUpdateEvent>) {
          return RpcEventKind::kMessageUpdate;
        }
        if constexpr (std::is_same_v<Value, ToolExecutionStartEvent>) {
          return RpcEventKind::kToolExecutionStart;
        }
        if constexpr (std::is_same_v<Value, ToolExecutionUpdateEvent>) {
          return RpcEventKind::kToolExecutionUpdate;
        }
        if constexpr (std::is_same_v<Value, ToolExecutionEndEvent>) {
          return RpcEventKind::kToolExecutionEnd;
        }
        if constexpr (std::is_same_v<Value, QueueUpdateEvent>) {
          return RpcEventKind::kQueueUpdate;
        }
        if constexpr (std::is_same_v<Value, CompactionStartEvent>) {
          return RpcEventKind::kCompactionStart;
        }
        if constexpr (std::is_same_v<Value, CompactionEndEvent>) {
          return RpcEventKind::kCompactionEnd;
        }
        if constexpr (std::is_same_v<Value, AutoRetryStartEvent>) {
          return RpcEventKind::kAutoRetryStart;
        }
        if constexpr (std::is_same_v<Value, AutoRetryEndEvent>) {
          return RpcEventKind::kAutoRetryEnd;
        }
        if constexpr (std::is_same_v<Value, ExtensionErrorEvent>) {
          return RpcEventKind::kExtensionError;
        }
        if constexpr (std::is_same_v<Value, ExtensionUiRequestEvent>) {
          return RpcEventKind::kExtensionUiRequest;
        }
        return RpcEventKind::kMessageEnd;
      },
      event);
}

Result<RpcCommand> decode_rpc_command(const Json& json) {
  try {
    require_object(json, "rpc command");
    const auto type = json.at("type").get<std::string>();
    RpcCommandKind kind = RpcCommandKind::kPrompt;
    if (!parse_rpc_command_kind(type, kind)) {
      throw std::runtime_error("unsupported rpc command type");
    }

    switch (kind) {
      case RpcCommandKind::kPrompt:
      case RpcCommandKind::kSteer:
      case RpcCommandKind::kFollowUp:
        return RpcCommand(json.get<PromptCommand>());
      case RpcCommandKind::kAbort:
        return RpcCommand(json.get<AbortCommand>());
      case RpcCommandKind::kGetState:
        return RpcCommand(json.get<GetStateCommand>());
      case RpcCommandKind::kGetMessages:
        return RpcCommand(json.get<GetMessagesCommand>());
      case RpcCommandKind::kNewSession:
        return RpcCommand(json.get<NewSessionCommand>());
      case RpcCommandKind::kSwitchSession:
        return RpcCommand(json.get<SwitchSessionCommand>());
      case RpcCommandKind::kFork:
        return RpcCommand(json.get<ForkCommand>());
      case RpcCommandKind::kClone:
        return RpcCommand(json.get<CloneCommand>());
      case RpcCommandKind::kSetModel:
        return RpcCommand(json.get<SetModelCommand>());
      case RpcCommandKind::kCycleModel:
        return RpcCommand(json.get<CycleModelCommand>());
      case RpcCommandKind::kGetAvailableModels:
        return RpcCommand(json.get<GetAvailableModelsCommand>());
      case RpcCommandKind::kSetThinkingLevel:
        return RpcCommand(json.get<SetThinkingLevelCommand>());
      case RpcCommandKind::kCycleThinkingLevel:
        return RpcCommand(json.get<CycleThinkingLevelCommand>());
      case RpcCommandKind::kSetSteeringMode:
      case RpcCommandKind::kSetFollowUpMode:
        return RpcCommand(json.get<SetQueueModeCommand>());
      case RpcCommandKind::kCompact:
        return RpcCommand(json.get<CompactCommand>());
      case RpcCommandKind::kSetAutoCompaction:
      case RpcCommandKind::kSetAutoRetry:
        return RpcCommand(json.get<SetBooleanModeCommand>());
      case RpcCommandKind::kAbortRetry:
        return RpcCommand(json.get<AbortRetryCommand>());
      case RpcCommandKind::kBash:
        return RpcCommand(json.get<BashCommand>());
      case RpcCommandKind::kAbortBash:
        return RpcCommand(json.get<AbortBashCommand>());
      case RpcCommandKind::kGetSessionStats:
        return RpcCommand(json.get<GetSessionStatsCommand>());
      case RpcCommandKind::kGetForkMessages:
        return RpcCommand(json.get<GetForkMessagesCommand>());
      case RpcCommandKind::kExportHtml:
        return RpcCommand(json.get<ExportHtmlCommand>());
      case RpcCommandKind::kGetLastAssistantText:
        return RpcCommand(json.get<GetLastAssistantTextCommand>());
      case RpcCommandKind::kSetSessionName:
        return RpcCommand(json.get<SetSessionNameCommand>());
      case RpcCommandKind::kGetCommands:
        return RpcCommand(json.get<GetCommandsCommand>());
      case RpcCommandKind::kMemory:
        return RpcCommand(json.get<MemoryCommand>());
      case RpcCommandKind::kSubagent:
        return RpcCommand(json.get<SubagentCommand>());
    }
  } catch (const std::exception& error) {
    return Result<RpcCommand>::failure(ErrorCode::encoding_error, error.what());
  }

  return Result<RpcCommand>::failure(ErrorCode::encoding_error,
                                     "unsupported rpc command type");
}

Result<RpcResponse> decode_rpc_response(const Json& json) {
  try {
    require_object(json, "rpc response");
    if (json.at("type").get<std::string>() != "response") {
      throw std::runtime_error("rpc response type must equal 'response'");
    }

    const std::string command = json.at("command").get<std::string>();
    const bool success = json.at("success").get<bool>();

    if (!success) {
      return RpcResponse(json.get<RpcErrorResponse>());
    }
    if (command == "get_state") {
      return RpcResponse(json.get<GetStateResponse>());
    }
    if (command == "set_model") {
      return RpcResponse(json.get<SetModelResponse>());
    }
    return RpcResponse(json.get<RpcSuccessResponse>());
  } catch (const std::exception& error) {
    return Result<RpcResponse>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

Result<RpcEvent> decode_rpc_event(const Json& json) {
  try {
    require_object(json, "rpc event");
    const auto type = json.at("type").get<std::string>();
    RpcEventKind kind = RpcEventKind::kAgentStart;
    if (!parse_rpc_event_kind(type, kind)) {
      throw std::runtime_error("unsupported rpc event type");
    }

    switch (kind) {
      case RpcEventKind::kAgentStart:
        return RpcEvent(json.get<AgentStartEvent>());
      case RpcEventKind::kAgentEnd:
        return RpcEvent(json.get<AgentEndEvent>());
      case RpcEventKind::kTurnStart:
        return RpcEvent(json.get<TurnStartEvent>());
      case RpcEventKind::kTurnEnd:
        return RpcEvent(json.get<TurnEndEvent>());
      case RpcEventKind::kMessageStart:
        return RpcEvent(json.get<MessageStartEvent>());
      case RpcEventKind::kMessageUpdate:
        return RpcEvent(json.get<MessageUpdateEvent>());
      case RpcEventKind::kToolExecutionStart:
        return RpcEvent(json.get<ToolExecutionStartEvent>());
      case RpcEventKind::kToolExecutionUpdate:
        return RpcEvent(json.get<ToolExecutionUpdateEvent>());
      case RpcEventKind::kToolExecutionEnd:
        return RpcEvent(json.get<ToolExecutionEndEvent>());
      case RpcEventKind::kQueueUpdate:
        return RpcEvent(json.get<QueueUpdateEvent>());
      case RpcEventKind::kCompactionStart:
        return RpcEvent(json.get<CompactionStartEvent>());
      case RpcEventKind::kCompactionEnd:
        return RpcEvent(json.get<CompactionEndEvent>());
      case RpcEventKind::kAutoRetryStart:
        return RpcEvent(json.get<AutoRetryStartEvent>());
      case RpcEventKind::kAutoRetryEnd:
        return RpcEvent(json.get<AutoRetryEndEvent>());
      case RpcEventKind::kExtensionError:
        return RpcEvent(json.get<ExtensionErrorEvent>());
      case RpcEventKind::kExtensionUiRequest:
        return RpcEvent(json.get<ExtensionUiRequestEvent>());
      case RpcEventKind::kMessageEnd:
        return RpcEvent(json.get<MessageEndEvent>());
    }
  } catch (const std::exception& error) {
    return Result<RpcEvent>::failure(ErrorCode::encoding_error, error.what());
  }

  return Result<RpcEvent>::failure(ErrorCode::encoding_error,
                                   "unsupported rpc event type");
}

Result<ExtensionUiResponseFrame> decode_extension_ui_response_frame(
    const Json& json) {
  try {
    return json.get<ExtensionUiResponseFrame>();
  } catch (const std::exception& error) {
    return Result<ExtensionUiResponseFrame>::failure(ErrorCode::encoding_error,
                                                     error.what());
  }
}

Result<RpcDecodedFrame> decode_rpc_frame(const Json& json) {
  try {
    require_object(json, "rpc frame");
  } catch (const std::exception& error) {
    return Result<RpcDecodedFrame>::failure(ErrorCode::encoding_error,
                                            error.what());
  }

  if (!json.contains("type")) {
    return Result<RpcDecodedFrame>::failure(ErrorCode::encoding_error,
                                            "rpc frame must contain type");
  }

  const auto type = json.at("type").get<std::string>();
  if (type == "response") {
    const auto response = decode_rpc_response(json);
    if (!response) {
      return response.error();
    }
    return RpcDecodedFrame{response.value()};
  }
  if (type == "extension_ui_response") {
    const auto response = decode_extension_ui_response_frame(json);
    if (!response) {
      return response.error();
    }
    return RpcDecodedFrame{response.value()};
  }

  RpcCommandKind command_kind = RpcCommandKind::kPrompt;
  if (parse_rpc_command_kind(type, command_kind)) {
    const auto command = decode_rpc_command(json);
    if (!command) {
      return command.error();
    }
    return RpcDecodedFrame{command.value()};
  }

  RpcEventKind event_kind = RpcEventKind::kAgentStart;
  if (parse_rpc_event_kind(type, event_kind)) {
    const auto event = decode_rpc_event(json);
    if (!event) {
      return event.error();
    }
    return RpcDecodedFrame{event.value()};
  }

  return Result<RpcDecodedFrame>::failure(ErrorCode::encoding_error,
                                          "unsupported rpc frame type");
}

Result<RpcCommand> decode_rpc_command_line(std::string_view line) {
  const auto parsed = decode_rpc_json_line(line);
  if (!parsed) {
    return parsed.error();
  }
  return decode_rpc_command(parsed.value());
}

Result<RpcResponse> decode_rpc_response_line(std::string_view line) {
  const auto parsed = decode_rpc_json_line(line);
  if (!parsed) {
    return parsed.error();
  }
  return decode_rpc_response(parsed.value());
}

Result<RpcEvent> decode_rpc_event_line(std::string_view line) {
  const auto parsed = decode_rpc_json_line(line);
  if (!parsed) {
    return parsed.error();
  }
  return decode_rpc_event(parsed.value());
}

Result<RpcDecodedFrame> decode_rpc_frame_line(std::string_view line) {
  const auto parsed = decode_rpc_json_line(line);
  if (!parsed) {
    return parsed.error();
  }
  return decode_rpc_frame(parsed.value());
}

Result<Json> decode_rpc_json_line(std::string_view line) {
  return parse_json_line(line);
}

Result<std::string> encode_rpc_command_line(const RpcCommand& command) {
  try {
    Json json = std::visit([](const auto& value) { return Json(value); }, command);
    return encode_json_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

Result<std::string> encode_rpc_response_line(const RpcResponse& response) {
  try {
    Json json = std::visit([](const auto& value) { return Json(value); }, response);
    return encode_json_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

Result<std::string> encode_rpc_event_line(const RpcEvent& event) {
  try {
    Json json = std::visit([](const auto& value) { return Json(value); }, event);
    return encode_json_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

Result<std::string> encode_rpc_frame_line(const RpcDecodedFrame& frame) {
  if (const auto* command = std::get_if<RpcCommand>(&frame.payload)) {
    return encode_rpc_command_line(*command);
  }
  if (const auto* response = std::get_if<RpcResponse>(&frame.payload)) {
    return encode_rpc_response_line(*response);
  }
  if (const auto* event = std::get_if<RpcEvent>(&frame.payload)) {
    return encode_rpc_event_line(*event);
  }
  try {
    Json json = std::get<ExtensionUiResponseFrame>(frame.payload);
    return encode_json_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

void to_json(Json& json, const PromptCommand& command) {
  json = Json{
      {"type", to_string(command.command)},
      {"message", command.message},
  };
  encode_optional_id(json, command.id);
  if (command.images.has_value()) {
    json["images"] = *command.images;
  }
  if (command.streaming_behavior.has_value() &&
      command.command == RpcCommandKind::kPrompt) {
    json["streamingBehavior"] = to_string(*command.streaming_behavior);
  }
}

void from_json(const Json& json, PromptCommand& command) {
  require_object(json, "prompt command");
  const auto type = json.at("type").get<std::string>();
  RpcCommandKind kind = RpcCommandKind::kPrompt;
  if (!parse_rpc_command_kind(type, kind) ||
      (kind != RpcCommandKind::kPrompt && kind != RpcCommandKind::kSteer &&
       kind != RpcCommandKind::kFollowUp)) {
    throw std::runtime_error(
        "prompt command type must equal 'prompt', 'steer', or 'follow_up'");
  }
  command.id = decode_optional_id(json);
  command.command = kind;
  command.message = json.at("message").get<std::string>();
  if (command.message.empty()) {
    throw std::runtime_error("prompt command message must not be empty");
  }
  if (json.contains("images") && !json.at("images").is_null()) {
    command.images = require_array_field(json, "images");
  } else {
    command.images.reset();
  }
  if (json.contains("streamingBehavior") &&
      !json.at("streamingBehavior").is_null()) {
    if (kind != RpcCommandKind::kPrompt) {
      throw std::runtime_error(
          "streamingBehavior is only supported on prompt commands");
    }
    RpcStreamingBehavior behavior = RpcStreamingBehavior::kSteer;
    const auto text = json.at("streamingBehavior").get<std::string>();
    if (!parse_rpc_streaming_behavior(text, behavior)) {
      throw std::runtime_error("unsupported prompt streamingBehavior");
    }
    command.streaming_behavior = behavior;
  } else if (kind == RpcCommandKind::kSteer) {
    command.streaming_behavior = RpcStreamingBehavior::kSteer;
  } else if (kind == RpcCommandKind::kFollowUp) {
    command.streaming_behavior = RpcStreamingBehavior::kFollowUp;
  } else {
    command.streaming_behavior.reset();
  }
}

void to_json(Json& json, const AbortCommand& command) {
  json = Json{{"type", "abort"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, AbortCommand& command) {
  require_object(json, "abort command");
  if (json.at("type").get<std::string>() != "abort") {
    throw std::runtime_error("abort command type must equal 'abort'");
  }
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const GetStateCommand& command) {
  json = Json{{"type", "get_state"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, GetStateCommand& command) {
  require_object(json, "get_state command");
  if (json.at("type").get<std::string>() != "get_state") {
    throw std::runtime_error("get_state command type must equal 'get_state'");
  }
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const GetMessagesCommand& command) {
  json = Json{{"type", "get_messages"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, GetMessagesCommand& command) {
  require_object(json, "get_messages command");
  require_command_type(json, "get_messages", "get_messages command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const NewSessionCommand& command) {
  json = Json{{"type", "new_session"}};
  encode_optional_id(json, command.id);
  if (command.parent_session.has_value()) {
    json["parentSession"] = *command.parent_session;
  }
}

void from_json(const Json& json, NewSessionCommand& command) {
  require_object(json, "new_session command");
  require_command_type(json, "new_session", "new_session command");
  command.id = decode_optional_id(json);
  if (json.contains("parentSession") && !json.at("parentSession").is_null()) {
    command.parent_session = json.at("parentSession").get<std::string>();
    if (command.parent_session->empty()) {
      throw std::runtime_error("new_session parentSession must not be empty");
    }
  } else {
    command.parent_session.reset();
  }
}

void to_json(Json& json, const SwitchSessionCommand& command) {
  json = Json{{"type", "switch_session"}, {"sessionPath", command.session_path}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, SwitchSessionCommand& command) {
  require_object(json, "switch_session command");
  require_command_type(json, "switch_session", "switch_session command");
  command.id = decode_optional_id(json);
  command.session_path = json.at("sessionPath").get<std::string>();
  if (command.session_path.empty()) {
    throw std::runtime_error("switch_session sessionPath must not be empty");
  }
}

void to_json(Json& json, const ForkCommand& command) {
  json = Json{{"type", "fork"}, {"entryId", command.entry_id}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, ForkCommand& command) {
  require_object(json, "fork command");
  require_command_type(json, "fork", "fork command");
  command.id = decode_optional_id(json);
  command.entry_id = json.at("entryId").get<std::string>();
  if (command.entry_id.empty()) {
    throw std::runtime_error("fork entryId must not be empty");
  }
}

void to_json(Json& json, const CloneCommand& command) {
  json = Json{{"type", "clone"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, CloneCommand& command) {
  require_object(json, "clone command");
  require_command_type(json, "clone", "clone command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const SetModelCommand& command) {
  json = Json{
      {"type", "set_model"},
      {"provider", command.provider},
      {"modelId", command.model_id},
  };
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, SetModelCommand& command) {
  require_object(json, "set_model command");
  if (json.at("type").get<std::string>() != "set_model") {
    throw std::runtime_error("set_model command type must equal 'set_model'");
  }
  command.id = decode_optional_id(json);
  command.provider = json.at("provider").get<std::string>();
  command.model_id = json.at("modelId").get<std::string>();
  if (command.provider.empty() || command.model_id.empty()) {
    throw std::runtime_error("set_model command provider and modelId must not be empty");
  }
}

void to_json(Json& json, const CycleModelCommand& command) {
  json = Json{{"type", "cycle_model"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, CycleModelCommand& command) {
  require_object(json, "cycle_model command");
  require_command_type(json, "cycle_model", "cycle_model command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const GetAvailableModelsCommand& command) {
  json = Json{{"type", "get_available_models"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, GetAvailableModelsCommand& command) {
  require_object(json, "get_available_models command");
  require_command_type(json, "get_available_models", "get_available_models command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const SetThinkingLevelCommand& command) {
  json = Json{{"type", "set_thinking_level"}, {"level", command.level}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, SetThinkingLevelCommand& command) {
  require_object(json, "set_thinking_level command");
  require_command_type(json, "set_thinking_level", "set_thinking_level command");
  command.id = decode_optional_id(json);
  command.level = json.at("level").get<std::string>();
  if (!is_supported_thinking_level(command.level)) {
    throw std::runtime_error("thinking level must be off, minimal, low, medium, high, or xhigh");
  }
}

void to_json(Json& json, const CycleThinkingLevelCommand& command) {
  json = Json{{"type", "cycle_thinking_level"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, CycleThinkingLevelCommand& command) {
  require_object(json, "cycle_thinking_level command");
  require_command_type(json, "cycle_thinking_level",
                       "cycle_thinking_level command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const SetQueueModeCommand& command) {
  json = Json{
      {"type", to_string(command.command)},
      {"mode", command.mode},
  };
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, SetQueueModeCommand& command) {
  require_object(json, "queue mode command");
  const auto type = json.at("type").get<std::string>();
  RpcCommandKind kind = RpcCommandKind::kPrompt;
  if (!parse_rpc_command_kind(type, kind) ||
      (kind != RpcCommandKind::kSetSteeringMode &&
       kind != RpcCommandKind::kSetFollowUpMode)) {
    throw std::runtime_error(
        "queue mode command type must equal 'set_steering_mode' or 'set_follow_up_mode'");
  }
  command.id = decode_optional_id(json);
  command.command = kind;
  command.mode = json.at("mode").get<std::string>();
  if (command.mode != "all" && command.mode != "one-at-a-time") {
    throw std::runtime_error("queue mode must equal 'all' or 'one-at-a-time'");
  }
}

void to_json(Json& json, const CompactCommand& command) {
  json = Json{{"type", "compact"}};
  encode_optional_id(json, command.id);
  if (command.custom_instructions.has_value()) {
    json["customInstructions"] = *command.custom_instructions;
  }
}

void from_json(const Json& json, CompactCommand& command) {
  require_object(json, "compact command");
  require_command_type(json, "compact", "compact command");
  command.id = decode_optional_id(json);
  if (json.contains("customInstructions") &&
      !json.at("customInstructions").is_null()) {
    command.custom_instructions =
        json.at("customInstructions").get<std::string>();
    if (command.custom_instructions->empty()) {
      throw std::runtime_error("compact customInstructions must not be empty");
    }
  } else {
    command.custom_instructions.reset();
  }
}

void to_json(Json& json, const SetBooleanModeCommand& command) {
  json = Json{{"type", to_string(command.command)}, {"enabled", command.enabled}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, SetBooleanModeCommand& command) {
  require_object(json, "boolean mode command");
  const auto type = json.at("type").get<std::string>();
  RpcCommandKind kind = RpcCommandKind::kPrompt;
  if (!parse_rpc_command_kind(type, kind) ||
      (kind != RpcCommandKind::kSetAutoCompaction &&
       kind != RpcCommandKind::kSetAutoRetry)) {
    throw std::runtime_error(
        "boolean mode command type must equal 'set_auto_compaction' or 'set_auto_retry'");
  }
  command.id = decode_optional_id(json);
  command.command = kind;
  command.enabled = json.at("enabled").get<bool>();
}

void to_json(Json& json, const AbortRetryCommand& command) {
  json = Json{{"type", "abort_retry"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, AbortRetryCommand& command) {
  require_object(json, "abort_retry command");
  require_command_type(json, "abort_retry", "abort_retry command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const BashCommand& command) {
  json = Json{{"type", "bash"}, {"command", command.command}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, BashCommand& command) {
  require_object(json, "bash command");
  require_command_type(json, "bash", "bash command");
  command.id = decode_optional_id(json);
  command.command = json.at("command").get<std::string>();
  if (command.command.empty()) {
    throw std::runtime_error("bash command must not be empty");
  }
}

void to_json(Json& json, const AbortBashCommand& command) {
  json = Json{{"type", "abort_bash"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, AbortBashCommand& command) {
  require_object(json, "abort_bash command");
  require_command_type(json, "abort_bash", "abort_bash command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const GetSessionStatsCommand& command) {
  json = Json{{"type", "get_session_stats"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, GetSessionStatsCommand& command) {
  require_object(json, "get_session_stats command");
  require_command_type(json, "get_session_stats", "get_session_stats command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const GetForkMessagesCommand& command) {
  json = Json{{"type", "get_fork_messages"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, GetForkMessagesCommand& command) {
  require_object(json, "get_fork_messages command");
  require_command_type(json, "get_fork_messages",
                       "get_fork_messages command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const ExportHtmlCommand& command) {
  json = Json{{"type", "export_html"}};
  encode_optional_id(json, command.id);
  if (command.output_path.has_value()) {
    json["outputPath"] = *command.output_path;
  }
}

void from_json(const Json& json, ExportHtmlCommand& command) {
  require_object(json, "export_html command");
  require_command_type(json, "export_html", "export_html command");
  command.id = decode_optional_id(json);
  if (json.contains("outputPath") && !json.at("outputPath").is_null()) {
    command.output_path = json.at("outputPath").get<std::string>();
    if (command.output_path->empty()) {
      throw std::runtime_error("export_html outputPath must not be empty");
    }
  } else {
    command.output_path.reset();
  }
}

void to_json(Json& json, const GetLastAssistantTextCommand& command) {
  json = Json{{"type", "get_last_assistant_text"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, GetLastAssistantTextCommand& command) {
  require_object(json, "get_last_assistant_text command");
  require_command_type(json, "get_last_assistant_text",
                       "get_last_assistant_text command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const SetSessionNameCommand& command) {
  json = Json{{"type", "set_session_name"}, {"name", command.name}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, SetSessionNameCommand& command) {
  require_object(json, "set_session_name command");
  require_command_type(json, "set_session_name", "set_session_name command");
  command.id = decode_optional_id(json);
  command.name = json.at("name").get<std::string>();
  if (command.name.empty()) {
    throw std::runtime_error("set_session_name name must not be empty");
  }
}

void to_json(Json& json, const GetCommandsCommand& command) {
  json = Json{{"type", "get_commands"}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, GetCommandsCommand& command) {
  require_object(json, "get_commands command");
  require_command_type(json, "get_commands", "get_commands command");
  command.id = decode_optional_id(json);
}

void to_json(Json& json, const MemoryCommand& command) {
  json = Json{{"type", "memory"},
              {"action", command.action},
              {"params", command.params},
              {"confirm", command.confirm}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, MemoryCommand& command) {
  require_object(json, "memory command");
  require_command_type(json, "memory", "memory command");
  command.id = decode_optional_id(json);
  command.action = json.at("action").get<std::string>();
  static const std::array<std::string_view, 9> actions = {
      "status", "dream", "recall", "explain", "remember", "review", "feedback",
      "forget", "retrieval_rebuild"};
  if (std::find(actions.begin(), actions.end(), command.action) ==
      actions.end()) {
    throw std::runtime_error("memory command action is unsupported");
  }
  command.params = json.value("params", Json::object());
  if (!command.params.is_object()) {
    throw std::runtime_error("memory command params must be an object");
  }
  command.confirm = json.value("confirm", false);
}

void to_json(Json& json, const SubagentCommand& command) {
  json = Json{{"type", "subagent"}, {"action", command.action},
              {"arguments", command.arguments}};
  encode_optional_id(json, command.id);
}

void from_json(const Json& json, SubagentCommand& command) {
  require_object(json, "subagent command");
  require_command_type(json, "subagent", "subagent command");
  command.id = decode_optional_id(json);
  command.action = require_non_empty_string(json, "action");
  command.arguments = json.value("arguments", Json::object());
  if (!command.arguments.is_object()) {
    throw std::runtime_error("subagent command arguments must be an object");
  }
  static const std::array<std::string_view, 6> actions = {
      "spawn", "send", "follow_up", "wait", "interrupt", "list_workers"};
  if (std::find(actions.begin(), actions.end(), command.action) == actions.end()) {
    throw std::runtime_error("subagent command action is unsupported");
  }
  command.arguments["action"] = command.action;
}

void to_json(Json& json, const RpcSessionState& state) {
  json = Json{
      {"model", state.model},
      {"thinkingLevel", state.thinking_level},
      {"isStreaming", state.is_streaming},
      {"isCompacting", state.is_compacting},
      {"steeringMode", state.steering_mode},
      {"followUpMode", state.follow_up_mode},
      {"sessionId", state.session_id},
      {"autoCompactionEnabled", state.auto_compaction_enabled},
      {"messageCount", state.message_count},
      {"pendingMessageCount", state.pending_message_count},
  };
  if (state.session_file.has_value()) {
    json["sessionFile"] = *state.session_file;
  }
  if (state.session_name.has_value()) {
    json["sessionName"] = *state.session_name;
  }
}

void from_json(const Json& json, RpcSessionState& state) {
  require_object(json, "rpc session state");
  if (json.contains("model")) {
    state.model = json.at("model");
  } else {
    state.model = nullptr;
  }
  state.thinking_level = json.at("thinkingLevel").get<std::string>();
  state.is_streaming = json.at("isStreaming").get<bool>();
  state.is_compacting = json.at("isCompacting").get<bool>();
  state.steering_mode = json.at("steeringMode").get<std::string>();
  state.follow_up_mode = json.at("followUpMode").get<std::string>();
  state.session_id = json.at("sessionId").get<std::string>();
  state.auto_compaction_enabled = json.at("autoCompactionEnabled").get<bool>();
  state.message_count = require_size(json, "messageCount");
  state.pending_message_count = require_size(json, "pendingMessageCount");

  if (json.contains("sessionFile") && !json.at("sessionFile").is_null()) {
    state.session_file = json.at("sessionFile").get<std::string>();
  } else {
    state.session_file.reset();
  }
  if (json.contains("sessionName") && !json.at("sessionName").is_null()) {
    state.session_name = json.at("sessionName").get<std::string>();
  } else {
    state.session_name.reset();
  }
}

void to_json(Json& json, const RpcSuccessResponse& response) {
  json = Json{
      {"type", "response"},
      {"command", response.command},
      {"success", true},
  };
  encode_optional_id(json, response.id);
  if (response.data.has_value() &&
      !rpc_success_data_is_forbidden(response.command)) {
    json["data"] = *response.data;
  }
}

void from_json(const Json& json, RpcSuccessResponse& response) {
  require_object(json, "rpc success response");
  if (json.at("type").get<std::string>() != "response") {
    throw std::runtime_error("rpc success response type must equal 'response'");
  }
  if (!json.at("success").get<bool>()) {
    throw std::runtime_error("rpc success response success must be true");
  }
  response.id = decode_optional_id(json);
  response.command = json.at("command").get<std::string>();
  if (response.command.empty()) {
    throw std::runtime_error("rpc success response command must not be empty");
  }
  if (rpc_success_data_is_forbidden(response.command) &&
      json.contains("data")) {
    throw std::runtime_error("rpc success response must not include data");
  }
  if (rpc_success_data_is_required(response.command) && !json.contains("data")) {
    throw std::runtime_error("rpc success response must include data");
  }
  if (json.contains("data")) {
    response.data = json.at("data");
  } else {
    response.data.reset();
  }
}

void to_json(Json& json, const RpcErrorResponse& response) {
  json = Json{
      {"type", "response"},
      {"command", response.command},
      {"success", false},
      {"error", response.error},
      {"error_code", response.error_code},
  };
  encode_optional_id(json, response.id);
}

void from_json(const Json& json, RpcErrorResponse& response) {
  require_object(json, "rpc error response");
  if (json.at("type").get<std::string>() != "response") {
    throw std::runtime_error("rpc error response type must equal 'response'");
  }
  if (json.at("success").get<bool>()) {
    throw std::runtime_error("rpc error response success must be false");
  }
  response.id = decode_optional_id(json);
  response.command = json.at("command").get<std::string>();
  response.error = json.at("error").get<std::string>();
  response.error_code = json.value("error_code", std::string("internal_error"));
  if (response.command.empty() || response.error.empty() ||
      response.error_code.empty()) {
    throw std::runtime_error("rpc error response command and error must not be empty");
  }
}

void to_json(Json& json, const GetStateResponse& response) {
  json = Json{
      {"type", "response"},
      {"command", "get_state"},
      {"success", true},
      {"data", response.data},
  };
  encode_optional_id(json, response.id);
}

void from_json(const Json& json, GetStateResponse& response) {
  require_object(json, "get_state response");
  if (json.at("type").get<std::string>() != "response") {
    throw std::runtime_error("get_state response type must equal 'response'");
  }
  if (!json.at("success").get<bool>()) {
    throw std::runtime_error("get_state response success must be true");
  }
  if (json.at("command").get<std::string>() != "get_state") {
    throw std::runtime_error("get_state response command must equal 'get_state'");
  }
  response.id = decode_optional_id(json);
  response.data = json.at("data").get<RpcSessionState>();
}

void to_json(Json& json, const SetModelResponse& response) {
  json = Json{
      {"type", "response"},
      {"command", "set_model"},
      {"success", true},
      {"data", response.data},
  };
  encode_optional_id(json, response.id);
}

void from_json(const Json& json, SetModelResponse& response) {
  require_object(json, "set_model response");
  if (json.at("type").get<std::string>() != "response") {
    throw std::runtime_error("set_model response type must equal 'response'");
  }
  if (!json.at("success").get<bool>()) {
    throw std::runtime_error("set_model response success must be true");
  }
  if (json.at("command").get<std::string>() != "set_model") {
    throw std::runtime_error("set_model response command must equal 'set_model'");
  }
  response.id = decode_optional_id(json);
  response.data = json.at("data");
}

void to_json(Json& json, const AgentStartEvent&) {
  json = Json{{"type", "agent_start"}};
}

void from_json(const Json& json, AgentStartEvent&) {
  require_object(json, "agent_start event");
  if (json.at("type").get<std::string>() != "agent_start") {
    throw std::runtime_error("agent_start event type must equal 'agent_start'");
  }
}

void to_json(Json& json, const AgentEndEvent& event) {
  json = Json{
      {"type", "agent_end"},
      {"messages", event.messages},
  };
}

void from_json(const Json& json, AgentEndEvent& event) {
  require_object(json, "agent_end event");
  if (json.at("type").get<std::string>() != "agent_end") {
    throw std::runtime_error("agent_end event type must equal 'agent_end'");
  }
  event.messages = require_array_field(json, "messages");
}

void to_json(Json& json, const TurnStartEvent&) {
  json = Json{{"type", "turn_start"}};
}

void from_json(const Json& json, TurnStartEvent&) {
  require_object(json, "turn_start event");
  if (json.at("type").get<std::string>() != "turn_start") {
    throw std::runtime_error("turn_start event type must equal 'turn_start'");
  }
}

void to_json(Json& json, const TurnEndEvent& event) {
  json = Json{
      {"type", "turn_end"},
      {"message", event.message},
      {"toolResults", event.tool_results},
  };
}

void from_json(const Json& json, TurnEndEvent& event) {
  require_object(json, "turn_end event");
  if (json.at("type").get<std::string>() != "turn_end") {
    throw std::runtime_error("turn_end event type must equal 'turn_end'");
  }
  event.message = require_object_field(json, "message");
  if (json.contains("toolResults")) {
    event.tool_results = require_array_field(json, "toolResults");
  } else {
    event.tool_results = Json::array();
  }
}

void to_json(Json& json, const MessageStartEvent& event) {
  json = Json{
      {"type", "message_start"},
      {"message", event.message},
  };
}

void from_json(const Json& json, MessageStartEvent& event) {
  require_object(json, "message_start event");
  if (json.at("type").get<std::string>() != "message_start") {
    throw std::runtime_error("message_start event type must equal 'message_start'");
  }
  event.message = require_object_field(json, "message");
}

void to_json(Json& json, const MessageUpdateEvent& event) {
  json = Json{
      {"type", "message_update"},
      {"message", event.message},
      {"assistantMessageEvent", event.assistant_message_event},
  };
}

void from_json(const Json& json, MessageUpdateEvent& event) {
  require_object(json, "message_update event");
  if (json.at("type").get<std::string>() != "message_update") {
    throw std::runtime_error("message_update event type must equal 'message_update'");
  }
  event.message = require_object_field(json, "message");
  event.assistant_message_event =
      require_object_field(json, "assistantMessageEvent");
  (void)require_non_empty_string(event.assistant_message_event, "type");
}

void to_json(Json& json, const ToolExecutionStartEvent& event) {
  json = Json{
      {"type", "tool_execution_start"},
      {"toolCallId", event.tool_call_id},
      {"toolName", event.tool_name},
      {"args", event.args},
  };
}

void from_json(const Json& json, ToolExecutionStartEvent& event) {
  require_object(json, "tool_execution_start event");
  require_command_type(json,
                       "tool_execution_start",
                       "tool_execution_start event");
  event.tool_call_id = require_non_empty_string(json, "toolCallId");
  event.tool_name = require_non_empty_string(json, "toolName");
  event.args = require_object_field(json, "args");
}

void to_json(Json& json, const ToolExecutionUpdateEvent& event) {
  json = Json{
      {"type", "tool_execution_update"},
      {"toolCallId", event.tool_call_id},
      {"toolName", event.tool_name},
      {"args", event.args},
      {"partialResult", event.partial_result},
  };
}

void from_json(const Json& json, ToolExecutionUpdateEvent& event) {
  require_object(json, "tool_execution_update event");
  require_command_type(json,
                       "tool_execution_update",
                       "tool_execution_update event");
  event.tool_call_id = require_non_empty_string(json, "toolCallId");
  event.tool_name = require_non_empty_string(json, "toolName");
  event.args = require_object_field(json, "args");
  event.partial_result = require_object_field(json, "partialResult");
}

void to_json(Json& json, const ToolExecutionEndEvent& event) {
  json = Json{
      {"type", "tool_execution_end"},
      {"toolCallId", event.tool_call_id},
      {"toolName", event.tool_name},
      {"result", event.result},
      {"isError", event.is_error},
  };
}

void from_json(const Json& json, ToolExecutionEndEvent& event) {
  require_object(json, "tool_execution_end event");
  require_command_type(json, "tool_execution_end", "tool_execution_end event");
  event.tool_call_id = require_non_empty_string(json, "toolCallId");
  event.tool_name = require_non_empty_string(json, "toolName");
  event.result = require_object_field(json, "result");
  event.is_error = json.at("isError").get<bool>();
}

void to_json(Json& json, const QueueUpdateEvent& event) {
  json = Json{
      {"type", "queue_update"},
      {"steering", event.steering},
      {"followUp", event.follow_up},
  };
}

void from_json(const Json& json, QueueUpdateEvent& event) {
  require_object(json, "queue_update event");
  if (json.at("type").get<std::string>() != "queue_update") {
    throw std::runtime_error("queue_update event type must equal 'queue_update'");
  }
  if (json.contains("steering") && !json.at("steering").is_null()) {
    event.steering = json.at("steering").get<std::vector<std::string>>();
  } else {
    event.steering.clear();
  }
  if (json.contains("followUp") && !json.at("followUp").is_null()) {
    event.follow_up = json.at("followUp").get<std::vector<std::string>>();
  } else {
    event.follow_up.clear();
  }
}

void to_json(Json& json, const CompactionStartEvent& event) {
  json = Json{
      {"type", "compaction_start"},
      {"reason", event.reason},
  };
}

void from_json(const Json& json, CompactionStartEvent& event) {
  require_object(json, "compaction_start event");
  if (json.at("type").get<std::string>() != "compaction_start") {
    throw std::runtime_error(
        "compaction_start event type must equal 'compaction_start'");
  }
  event.reason = json.at("reason").get<std::string>();
  if (!is_valid_compaction_reason(event.reason)) {
    throw std::runtime_error(
        "compaction_start reason must be manual, threshold, or overflow");
  }
}

void to_json(Json& json, const CompactionEndEvent& event) {
  json = Json{
      {"type", "compaction_end"},
      {"reason", event.reason},
      {"aborted", event.aborted},
      {"willRetry", event.will_retry},
  };
  if (event.result.has_value()) {
    json["result"] = *event.result;
  }
  if (event.error_message.has_value()) {
    json["errorMessage"] = *event.error_message;
  }
}

void from_json(const Json& json, CompactionEndEvent& event) {
  require_object(json, "compaction_end event");
  if (json.at("type").get<std::string>() != "compaction_end") {
    throw std::runtime_error(
        "compaction_end event type must equal 'compaction_end'");
  }
  event.reason = json.at("reason").get<std::string>();
  if (!is_valid_compaction_reason(event.reason)) {
    throw std::runtime_error(
        "compaction_end reason must be manual, threshold, or overflow");
  }
  if (json.contains("result") && !json.at("result").is_null()) {
    event.result = json.at("result");
  } else {
    event.result.reset();
  }
  event.aborted = json.at("aborted").get<bool>();
  event.will_retry = json.at("willRetry").get<bool>();
  if (json.contains("errorMessage") && !json.at("errorMessage").is_null()) {
    event.error_message = json.at("errorMessage").get<std::string>();
  } else {
    event.error_message.reset();
  }
}

void to_json(Json& json, const AutoRetryStartEvent& event) {
  json = Json{
      {"type", "auto_retry_start"},
      {"attempt", event.attempt},
      {"maxAttempts", event.max_attempts},
      {"delayMs", event.delay_ms},
      {"errorMessage", event.error_message},
  };
}

void from_json(const Json& json, AutoRetryStartEvent& event) {
  require_object(json, "auto_retry_start event");
  require_command_type(json, "auto_retry_start", "auto_retry_start event");
  event.attempt = require_size(json, "attempt");
  event.max_attempts = require_size(json, "maxAttempts");
  event.delay_ms = require_size(json, "delayMs");
  event.error_message = require_non_empty_string(json, "errorMessage");
  if (event.attempt == 0) {
    throw std::runtime_error("attempt must be at least 1");
  }
  if (event.max_attempts == 0) {
    throw std::runtime_error("maxAttempts must be at least 1");
  }
  if (event.attempt > event.max_attempts) {
    throw std::runtime_error("attempt must not exceed maxAttempts");
  }
}

void to_json(Json& json, const AutoRetryEndEvent& event) {
  json = Json{
      {"type", "auto_retry_end"},
      {"success", event.success},
      {"attempt", event.attempt},
  };
  if (event.final_error.has_value()) {
    json["finalError"] = *event.final_error;
  }
}

void from_json(const Json& json, AutoRetryEndEvent& event) {
  require_object(json, "auto_retry_end event");
  require_command_type(json, "auto_retry_end", "auto_retry_end event");
  event.success = json.at("success").get<bool>();
  event.attempt = require_size(json, "attempt");
  if (event.attempt == 0) {
    throw std::runtime_error("attempt must be at least 1");
  }
  if (json.contains("finalError") && !json.at("finalError").is_null()) {
    event.final_error = require_non_empty_string(json, "finalError");
  } else {
    event.final_error.reset();
  }
  if (!event.success && !event.final_error.has_value()) {
    throw std::runtime_error(
        "auto_retry_end finalError is required when success is false");
  }
}

void to_json(Json& json, const ExtensionErrorEvent& event) {
  json = Json{
      {"type", "extension_error"},
      {"extensionPath", event.extension_path},
      {"event", event.event},
      {"error", event.error},
  };
}

void from_json(const Json& json, ExtensionErrorEvent& event) {
  require_object(json, "extension_error event");
  require_command_type(json, "extension_error", "extension_error event");
  event.extension_path = require_non_empty_string(json, "extensionPath");
  event.event = require_non_empty_string(json, "event");
  event.error = require_non_empty_string(json, "error");
}

void to_json(Json& json, const ExtensionUiRequestEvent& event) {
  json = flatten_extension_ui_payload(event.payload,
                                      "extension_ui_request",
                                      event.id,
                                      event.method);
}

void from_json(const Json& json, ExtensionUiRequestEvent& event) {
  require_object(json, "extension_ui_request event");
  require_command_type(json,
                       "extension_ui_request",
                       "extension_ui_request event");
  event.id = require_non_empty_string(json, "id");
  event.method = require_non_empty_string(json, "method");
  event.payload = preserve_extension_ui_payload(json);
}

void to_json(Json& json, const ExtensionUiResponseFrame& frame) {
  json = flatten_extension_ui_payload(frame.payload,
                                      "extension_ui_response",
                                      frame.id,
                                      std::nullopt);
}

void from_json(const Json& json, ExtensionUiResponseFrame& frame) {
  require_object(json, "extension_ui_response frame");
  require_command_type(json,
                       "extension_ui_response",
                       "extension_ui_response frame");
  frame.id = require_non_empty_string(json, "id");
  frame.payload = preserve_extension_ui_payload(json);
}

void to_json(Json& json, const MessageEndEvent& event) {
  json = Json{
      {"type", "message_end"},
      {"message", event.message},
  };
}

void from_json(const Json& json, MessageEndEvent& event) {
  require_object(json, "message_end event");
  if (json.at("type").get<std::string>() != "message_end") {
    throw std::runtime_error("message_end event type must equal 'message_end'");
  }
  event.message = require_object_field(json, "message");
}

}  // namespace qt::app
