#include <qt/web_core/contracts.hpp>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace qt::web_core {
namespace {

Json normalized_object(const Json& value) {
  return value.is_object() ? value : Json::object();
}

Json optional_json_alias(const Json& json,
                         const char* primary,
                         const char* legacy) {
  if (json.contains(primary)) {
    return json.at(primary);
  }
  if (json.contains(legacy)) {
    return json.at(legacy);
  }
  return Json(nullptr);
}

bool is_one_of(const std::string& value,
               std::initializer_list<const char*> allowed) {
  for (const char* candidate : allowed) {
    if (value == candidate) {
      return true;
    }
  }
  return false;
}

bool is_supported_thinking_level(const std::string& level) {
  return is_one_of(level, {"off", "minimal", "low", "medium", "high", "xhigh"});
}

std::string image_fallback_label(std::string_view mime_type,
                                 std::optional<std::string_view> filename =
                                     std::nullopt) {
  std::string label = "[Image:";
  if (filename.has_value() && !filename->empty()) {
    label.push_back(' ');
    label.append(filename->data(), filename->size());
  }
  label += " [";
  label.append(mime_type.data(), mime_type.size());
  label += "]]";
  return label;
}

bool is_browser_session_open_command(const std::string& type) {
  return is_one_of(type, {"open_session", "switch_session"});
}

bool is_runtime_session_mutation_event(const std::string& type) {
  return is_one_of(type,
                   {"session_create_start", "session_create_end",
                    "session_open_start", "session_open_end",
                    "session_fork_start", "session_fork_end",
                    "session_clone_start", "session_clone_end"});
}

bool is_runtime_session_mutation_start_event(const std::string& type) {
  return is_one_of(type,
                   {"session_create_start", "session_open_start",
                    "session_fork_start", "session_clone_start"});
}

bool is_runtime_session_mutation_end_event(const std::string& type) {
  return is_one_of(type,
                   {"session_create_end", "session_open_end",
                    "session_fork_end", "session_clone_end"});
}

void require_non_empty_optional(const std::optional<std::string>& value,
                                const char* field,
                                const char* contract) {
  if (!value.has_value() || value->empty()) {
    throw std::runtime_error(std::string(contract) + " requires " + field);
  }
}

void require_session_mutation_shape(const SessionMutationRequest& request) {
  if (!is_one_of(request.command,
                 {"rename", "delete", "open", "new", "fork", "clone"})) {
    throw std::runtime_error("session mutation command is not supported");
  }
  if (request.command == "rename") {
    require_non_empty_optional(request.session_id, "session_id",
                              "rename session mutation");
    require_non_empty_optional(request.title, "title",
                              "rename session mutation");
  } else if (request.command == "delete" || request.command == "open") {
    require_non_empty_optional(request.session_id, "session_id",
                              "session mutation");
  } else if (request.command == "fork") {
    require_non_empty_optional(request.source_session_id, "source_session_id",
                              "fork session mutation");
  }
  if (request.thinking_level.has_value() &&
      !is_supported_thinking_level(*request.thinking_level)) {
    throw std::runtime_error("session mutation thinking_level is not supported");
  }
}

void require_session_mutation_shape(const SessionMutationResponse& response) {
  if (!is_one_of(response.command,
                 {"rename", "delete", "open", "new", "fork", "clone"})) {
    throw std::runtime_error("session mutation response command is not supported");
  }
  if (!response.ok) {
    require_non_empty_optional(response.error_message, "error_message",
                              "failed session mutation response");
    return;
  }
  require_non_empty_optional(response.session_id, "session_id",
                            "successful session mutation response");
}

void require_browser_session_mutation_command_shape(
    const BrowserSessionMutationCommand& command) {
  if (!is_one_of(command.type,
                 {"new_session", "open_session", "switch_session", "fork",
                  "clone"})) {
    throw std::runtime_error(
        "browser session mutation command type is not supported");
  }
  if (command.id.has_value() && command.id->empty()) {
    throw std::runtime_error(
        "browser session mutation command id must not be empty");
  }
  if (command.type == "open_session" || command.type == "switch_session") {
    if ((!command.session_path.has_value() || command.session_path->empty()) &&
        (!command.session_id.has_value() || command.session_id->empty())) {
      throw std::runtime_error(
          "browser session open mutation requires sessionPath or sessionId");
    }
  }
  if (command.type == "fork") {
    require_non_empty_optional(command.entry_id, "entryId",
                              "browser session fork mutation");
  }
  if (!command.options.is_object()) {
    throw std::runtime_error(
        "browser session mutation command options must be an object");
  }
}

void require_browser_session_mutation_result_shape(
    const BrowserSessionMutationResult& result) {
  if (result.type != "response") {
    throw std::runtime_error(
        "browser session mutation result type must be response");
  }
  if (!is_one_of(result.command,
                 {"new_session", "open_session", "switch_session", "fork",
                  "clone"})) {
    throw std::runtime_error(
        "browser session mutation result command is not supported");
  }
  if (result.id.has_value() && result.id->empty()) {
    throw std::runtime_error(
        "browser session mutation result id must not be empty");
  }
  if (!result.data.is_object()) {
    throw std::runtime_error(
        "browser session mutation result data must be an object");
  }
  if (!result.success) {
    require_non_empty_optional(result.error, "error",
                              "failed browser session mutation result");
  }
}

void require_web_session_storage_mutation_shape(
    const WebSessionStorageMutation& mutation) {
  if (!is_one_of(mutation.operation, {"save", "delete", "update_title"})) {
    throw std::runtime_error(
        "web session storage mutation operation is not supported");
  }

  if (mutation.operation == "save") {
    if (!mutation.data.has_value()) {
      throw std::runtime_error("web session storage save requires data");
    }
    if (!mutation.metadata.has_value()) {
      throw std::runtime_error("web session storage save requires metadata");
    }
    if (mutation.data->id != mutation.metadata->id) {
      throw std::runtime_error(
          "web session storage save requires matching data and metadata ids");
    }
    if (mutation.session_id.has_value() &&
        *mutation.session_id != mutation.data->id) {
      throw std::runtime_error(
          "web session storage save sessionId must match data id");
    }
    return;
  }

  require_non_empty_optional(mutation.session_id, "sessionId",
                            "web session storage mutation");
  if (mutation.operation == "update_title") {
    require_non_empty_optional(mutation.title, "title",
                              "web session storage update_title mutation");
  }
}

void require_web_export_tool_definition_shape(
    const WebExportToolDefinition& tool) {
  if (tool.name.empty()) {
    throw std::runtime_error("web export tool definition name must not be empty");
  }
  if (!tool.parameters.is_object()) {
    throw std::runtime_error(
        "web export tool definition parameters must be an object");
  }
}

void require_web_export_rendered_tool_html_shape(
    const WebExportRenderedToolHtml& html) {
  const bool has_any = html.call_html.has_value() ||
                       html.result_html_collapsed.has_value() ||
                       html.result_html_expanded.has_value();
  if (!has_any) {
    throw std::runtime_error(
        "web export rendered tool html requires at least one html field");
  }
}

void require_web_export_session_data_shape(
    const WebExportSessionData& data) {
  if (!data.header.is_object()) {
    throw std::runtime_error("web export session data header must be an object");
  }
  if (data.header.empty()) {
    throw std::runtime_error("web export session data header must not be empty");
  }
  if (data.leaf_id.has_value() && data.leaf_id->empty()) {
    throw std::runtime_error("web export session data leafId must not be empty");
  }
  if (data.system_prompt.has_value() && data.system_prompt->empty()) {
    throw std::runtime_error(
        "web export session data systemPrompt must not be empty");
  }
  for (const Json& entry : data.entries) {
    if (!entry.is_object()) {
      throw std::runtime_error(
          "web export session data entries must contain objects");
    }
    if (!entry.contains("type") || !entry.at("type").is_string() ||
        entry.at("type").get<std::string>().empty()) {
      throw std::runtime_error(
          "web export session data entry requires non-empty type");
    }
  }
  for (const auto& tool : data.tools) {
    require_web_export_tool_definition_shape(tool);
  }
  for (const auto& rendered : data.rendered_tools) {
    if (rendered.first.empty()) {
      throw std::runtime_error(
          "web export renderedTools keys must not be empty");
    }
    require_web_export_rendered_tool_html_shape(rendered.second);
  }
}

bool is_web_auto_discovery_provider_type(const std::string& type) {
  return is_one_of(type, {"ollama", "llama.cpp", "vllm", "lmstudio"});
}

bool is_web_custom_provider_type(const std::string& type) {
  return is_web_auto_discovery_provider_type(type) ||
         is_one_of(type,
                   {"openai-completions", "openai-responses",
                    "anthropic-messages"});
}

void require_web_custom_provider_shape(const WebCustomProvider& provider) {
  if (provider.id.empty()) {
    throw std::runtime_error("web custom provider id must not be empty");
  }
  if (provider.name.empty()) {
    throw std::runtime_error("web custom provider name must not be empty");
  }
  if (!is_web_custom_provider_type(provider.type)) {
    throw std::runtime_error("web custom provider type is not supported");
  }
  if (provider.base_url.empty()) {
    throw std::runtime_error("web custom provider baseUrl must not be empty");
  }
  if (provider.api_key.has_value() && provider.api_key->empty()) {
    throw std::runtime_error("web custom provider apiKey must not be empty");
  }
  for (const Json& model : provider.models) {
    if (!model.is_object()) {
      throw std::runtime_error("web custom provider models must be objects");
    }
  }
  if (is_web_auto_discovery_provider_type(provider.type) &&
      !provider.models.empty()) {
    throw std::runtime_error(
        "web custom provider auto-discovery types must not store models");
  }
}

void require_web_dialog_request_shape(const WebDialogRequest& request) {
  if (!is_one_of(request.type,
                 {"api_key_prompt", "persistent_storage", "model_selector",
                  "session_list", "prompt"})) {
    throw std::runtime_error("web dialog request type is not supported");
  }
  if (request.id.has_value() && request.id->empty()) {
    throw std::runtime_error("web dialog request id must not be empty");
  }
  if (request.type == "api_key_prompt") {
    require_non_empty_optional(request.provider, "provider",
                              "web api_key_prompt dialog request");
  }
  if (request.type == "prompt") {
    require_non_empty_optional(request.title, "title",
                              "web prompt dialog request");
  }
  if (request.type == "model_selector" && !request.current_model.is_null() &&
      !request.current_model.is_object()) {
    throw std::runtime_error(
        "web model_selector dialog currentModel must be an object");
  }
  if (request.type == "session_list" && !request.sessions.is_array()) {
    throw std::runtime_error(
        "web session_list dialog sessions must be an array");
  }
  for (const std::string& provider : request.allowed_providers) {
    if (provider.empty()) {
      throw std::runtime_error(
          "web dialog request allowedProviders must not contain empty values");
    }
  }
}

void require_web_dialog_result_shape(const WebDialogResult& result) {
  if (!is_one_of(result.type,
                 {"api_key_prompt", "persistent_storage", "model_selector",
                  "session_list", "prompt"})) {
    throw std::runtime_error("web dialog result type is not supported");
  }
  if (result.id.has_value() && result.id->empty()) {
    throw std::runtime_error("web dialog result id must not be empty");
  }
  if (!result.accepted) {
    return;
  }
  if (result.type == "api_key_prompt") {
    require_non_empty_optional(result.provider, "provider",
                              "web api_key_prompt dialog result");
  } else if (result.type == "persistent_storage") {
    if (!result.persistent_granted.has_value()) {
      throw std::runtime_error(
          "web persistent_storage dialog result requires persistentGranted");
    }
  } else if (result.type == "model_selector") {
    if (!result.model.is_object() || result.model.empty()) {
      throw std::runtime_error(
          "web model_selector dialog result requires model object");
    }
  } else if (result.type == "session_list") {
    require_non_empty_optional(result.session_id, "sessionId",
                              "web session_list dialog result");
  } else if (result.type == "prompt") {
    require_non_empty_optional(result.value, "value",
                              "web prompt dialog result");
  }
  for (const std::string& session_id : result.deleted_session_ids) {
    if (session_id.empty()) {
      throw std::runtime_error(
          "web dialog result deletedSessionIds must not contain empty values");
    }
  }
}

void require_web_auth_token_mutation_shape(
    const WebAuthTokenMutation& mutation) {
  if (!is_one_of(mutation.operation, {"get", "set", "clear"})) {
    throw std::runtime_error("web auth token mutation operation is not supported");
  }
  if (mutation.storage_key.empty()) {
    throw std::runtime_error("web auth token mutation storageKey must not be empty");
  }
  if (mutation.operation == "set") {
    require_non_empty_optional(mutation.token, "token",
                              "web auth token set mutation");
  }
}

void require_web_storage_quota_info_shape(const WebStorageQuotaInfo& info) {
  if (info.usage < 0 || info.quota < 0) {
    throw std::runtime_error("web storage quota info values must be non-negative");
  }
  if (info.percent < 0.0 || info.percent > 100.0) {
    throw std::runtime_error("web storage quota info percent is out of range");
  }
}

void require_web_storage_backend_request_shape(
    const WebStorageBackendRequest& request) {
  if (!is_one_of(request.operation,
                 {"get", "set", "delete", "keys", "getAllFromIndex",
                  "clear", "has", "transaction", "quota",
                  "requestPersistence"})) {
    throw std::runtime_error(
        "web storage backend request operation is not supported");
  }
  if (!is_one_of(request.direction, {"asc", "desc"})) {
    throw std::runtime_error(
        "web storage backend request direction is not supported");
  }
  if (!is_one_of(request.mode, {"readonly", "readwrite"})) {
    throw std::runtime_error(
        "web storage backend request mode is not supported");
  }
  const bool needs_store = is_one_of(request.operation,
                                    {"get", "set", "delete", "keys",
                                     "getAllFromIndex", "clear", "has"});
  if (needs_store) {
    require_non_empty_optional(request.store_name, "storeName",
                              "web storage backend request");
  }
  if (is_one_of(request.operation, {"get", "set", "delete", "has"})) {
    require_non_empty_optional(request.key, "key",
                              "web storage backend request");
  }
  if (request.operation == "set" && request.value.is_null()) {
    throw std::runtime_error("web storage backend set request requires value");
  }
  if (request.operation == "getAllFromIndex") {
    require_non_empty_optional(request.index_name, "indexName",
                              "web storage backend request");
  }
  if (request.operation == "transaction") {
    if (request.store_names.empty()) {
      throw std::runtime_error(
          "web storage backend transaction request requires storeNames");
    }
    for (const std::string& store_name : request.store_names) {
      if (store_name.empty()) {
        throw std::runtime_error(
            "web storage backend transaction storeNames must not contain empty values");
      }
    }
  }
}

void require_web_storage_backend_result_shape(
    const WebStorageBackendResult& result) {
  if (!is_one_of(result.operation,
                 {"get", "set", "delete", "keys", "getAllFromIndex",
                  "clear", "has", "transaction", "quota",
                  "requestPersistence"})) {
    throw std::runtime_error(
        "web storage backend result operation is not supported");
  }
  if (!result.success) {
    require_non_empty_optional(result.error_message, "errorMessage",
                              "failed web storage backend result");
    return;
  }
  if (result.operation == "has" && !result.exists.has_value()) {
    throw std::runtime_error("web storage backend has result requires exists");
  }
  if (result.operation == "quota") {
    if (!result.quota_info.has_value()) {
      throw std::runtime_error("web storage backend quota result requires quotaInfo");
    }
    require_web_storage_quota_info_shape(*result.quota_info);
  }
  if (result.operation == "requestPersistence" &&
      !result.persistent_granted.has_value()) {
    throw std::runtime_error(
        "web storage backend requestPersistence result requires persistentGranted");
  }
}

void require_web_session_command_shape(const WebSessionCommand& command) {
  if (!is_one_of(command.type,
                 {"prompt", "abort", "set_model", "set_thinking_level",
                  "retry", "compact"})) {
    throw std::runtime_error("web session command type is not supported");
  }
  if (command.session_id.empty()) {
    throw std::runtime_error("web session command session_id must not be empty");
  }
  if (command.type == "prompt") {
    const bool has_input = command.input.has_value() && !command.input->empty();
    if (!has_input && command.attachments.empty()) {
      throw std::runtime_error(
          "web session prompt command requires input or attachments");
    }
  } else if (command.type == "set_model") {
    if (!command.model.is_object() || command.model.empty()) {
      throw std::runtime_error("web session set_model command requires model");
    }
  } else if (command.type == "set_thinking_level") {
    require_non_empty_optional(command.thinking_level, "thinking_level",
                              "web session set_thinking_level command");
  } else if (command.type == "retry") {
    require_non_empty_optional(command.message_id, "message_id",
                              "web session retry command");
  }
  if (command.thinking_level.has_value() &&
      !is_supported_thinking_level(*command.thinking_level)) {
    throw std::runtime_error(
        "web session command thinking_level is not supported");
  }
}

void require_streaming_delta_shape(const StreamingDelta& delta) {
  if (!is_one_of(delta.type,
                 {"start", "text_start", "text_delta", "text_end",
                  "thinking_start", "thinking_delta", "thinking_end",
                  "toolcall_start", "toolcall_delta", "toolcall_end", "done",
                  "error", "agent_start", "agent_end", "turn_start",
                  "turn_end", "message_start", "message_update",
                  "message_end", "tool_execution_start",
                  "tool_execution_update", "tool_execution_end"})) {
    throw std::runtime_error("streaming delta type is not supported");
  }
  if (delta.type == "text_delta" || delta.type == "thinking_delta" ||
      delta.type == "toolcall_delta") {
    require_non_empty_optional(delta.delta, "delta", "streaming delta");
  }
  if (delta.type == "error") {
    require_non_empty_optional(delta.error_message, "error_message",
                              "streaming delta");
  }
}

void require_streaming_state_sync_shape(const StreamingStateSync& sync) {
  if (!sync.model.is_null() && !sync.model.is_object()) {
    throw std::runtime_error("streaming state sync model must be an object");
  }
  if (sync.thinking_level.has_value() &&
      !is_supported_thinking_level(*sync.thinking_level)) {
    throw std::runtime_error(
        "streaming state sync thinkingLevel is not supported");
  }
  if (!sync.messages.is_array()) {
    throw std::runtime_error("streaming state sync messages must be an array");
  }
  if (!sync.tools.is_array()) {
    throw std::runtime_error("streaming state sync tools must be an array");
  }
  for (const std::string& id : sync.pending_tool_calls) {
    if (id.empty()) {
      throw std::runtime_error(
          "streaming state sync pendingToolCalls must not contain empty ids");
    }
  }
  if (sync.streaming_message.has_value() && !sync.is_streaming) {
    throw std::runtime_error(
        "streaming state sync streamingMessage requires isStreaming");
  }
}

void require_web_content_block_shape(const WebContentBlock& block) {
  if (!is_one_of(block.type, {"text", "thinking", "image", "toolCall"})) {
    throw std::runtime_error("web content block type is not supported");
  }
  if (block.type == "text") {
    require_non_empty_optional(block.text, "text", "web text content block");
    return;
  }
  if (block.type == "thinking") {
    require_non_empty_optional(block.thinking, "thinking",
                              "web thinking content block");
    return;
  }
  if (block.type == "image") {
    require_non_empty_optional(block.data, "data", "web image content block");
    require_non_empty_optional(block.mime_type, "mimeType",
                              "web image content block");
    return;
  }
  require_non_empty_optional(block.id, "id", "web toolCall content block");
  require_non_empty_optional(block.name, "name", "web toolCall content block");
  if (!block.arguments.is_object()) {
    throw std::runtime_error(
        "web toolCall content block arguments must be an object");
  }
}

std::string expected_tool_render_snapshot_state(const Json& result,
                                                bool pending,
                                                bool aborted,
                                                bool is_streaming,
                                                bool is_error) {
  if (!result.is_null()) {
    if (result.is_object() && result.contains("isError") &&
        result.at("isError").is_boolean()) {
      return result.at("isError").get<bool>() ? "error" : "complete";
    }
    return is_error ? "error" : "complete";
  }
  if (aborted || is_error) {
    return "error";
  }
  if (pending || is_streaming) {
    return "inprogress";
  }
  return "complete";
}

void require_tool_render_snapshot_shape(const ToolRenderSnapshot& snapshot) {
  if (snapshot.call_id.empty() || snapshot.name.empty()) {
    throw std::runtime_error("tool render snapshot requires call_id and name");
  }
  if (!snapshot.arguments.is_object()) {
    throw std::runtime_error(
        "tool render snapshot arguments must be an object");
  }
  if (snapshot.result.is_array()) {
    throw std::runtime_error(
        "tool render snapshot result must be object, scalar, or null");
  }
  if (snapshot.renderer.empty()) {
    throw std::runtime_error(
        "tool render snapshot renderer must not be empty");
  }
  if (!snapshot.state.empty() &&
      !is_one_of(snapshot.state, {"inprogress", "complete", "error"})) {
    throw std::runtime_error("tool render snapshot state is not supported");
  }
  const std::string expected = expected_tool_render_snapshot_state(
      snapshot.result, snapshot.pending, snapshot.aborted,
      snapshot.is_streaming, snapshot.is_error);
  if (!snapshot.state.empty() && snapshot.state != expected) {
    throw std::runtime_error(
        "tool render snapshot state does not match result status");
  }
}

void require_web_assistant_message_event_shape(
    const WebAssistantMessageEvent& event) {
  if (!is_one_of(event.type,
                 {"start", "text_start", "text_delta", "text_end",
                  "thinking_start", "thinking_delta", "thinking_end",
                  "toolcall_start", "toolcall_delta", "toolcall_end", "done",
                  "error"})) {
    throw std::runtime_error(
        "web assistant message event type is not supported");
  }
  if (event.type != "start" && event.type != "done" &&
      event.type != "error" && !event.content_index.has_value()) {
    throw std::runtime_error(
        "web assistant message event requires contentIndex");
  }
  if (event.type == "text_delta" || event.type == "thinking_delta" ||
      event.type == "toolcall_delta") {
    require_non_empty_optional(event.delta, "delta",
                              "web assistant message event");
  }
  if (event.type == "text_end" || event.type == "thinking_end") {
    require_non_empty_optional(event.content, "content",
                              "web assistant message event");
  }
  if (event.type == "toolcall_end" && !event.tool_call.is_object()) {
    throw std::runtime_error(
        "web assistant message event toolcall_end requires toolCall object");
  }
  if (event.type == "done") {
    if (!event.reason.has_value() ||
        !is_one_of(*event.reason, {"stop", "length", "toolUse"})) {
      throw std::runtime_error(
          "web assistant message event done requires supported reason");
    }
    if (!event.message.is_object()) {
      throw std::runtime_error(
          "web assistant message event done requires message object");
    }
  }
  if (event.type == "error") {
    if (!event.reason.has_value() ||
        !is_one_of(*event.reason, {"aborted", "error"})) {
      throw std::runtime_error(
          "web assistant message event error requires supported reason");
    }
    if (!event.error.is_object()) {
      throw std::runtime_error(
          "web assistant message event error requires error object");
    }
  }
}

void require_web_agent_event_shape(const WebAgentEvent& event) {
  if (!is_one_of(event.type,
                 {"agent_start", "agent_end", "turn_start", "turn_end",
                  "message_start", "message_update", "message_end",
                  "tool_execution_start", "tool_execution_update",
                  "tool_execution_end", "queue_update",
                  "extension_error"})) {
    throw std::runtime_error("web agent event type is not supported");
  }

  if (event.type == "queue_update") {
    if (!event.steer.is_array() || !event.follow_up.is_array() ||
        !event.next_turn.is_array()) {
      throw std::runtime_error(
          "web queue_update event requires steer, followUp, and nextTurn arrays");
    }
    return;
  }

  if (event.type == "extension_error") {
    require_non_empty_optional(event.extension_path, "extensionPath",
                              "web extension_error event");
    require_non_empty_optional(event.extension_event, "event",
                              "web extension_error event");
    require_non_empty_optional(event.error, "error",
                              "web extension_error event");
    return;
  }

  if (event.type == "agent_end") {
    if (!event.messages.is_array()) {
      throw std::runtime_error("web agent_end event requires messages array");
    }
  } else if (event.type == "message_start" ||
             event.type == "message_update" ||
             event.type == "message_end" || event.type == "turn_end") {
    if (!event.message.is_object()) {
      throw std::runtime_error("web agent event requires message object");
    }
  }

  if (event.type == "message_update" &&
      !event.assistant_message_event.is_object()) {
    throw std::runtime_error(
        "web message_update event requires assistantMessageEvent object");
  }

  if (event.type == "turn_end" && !event.tool_results.is_array()) {
    throw std::runtime_error("web turn_end event requires toolResults array");
  }

  if (event.type == "tool_execution_start" ||
      event.type == "tool_execution_update" ||
      event.type == "tool_execution_end") {
    require_non_empty_optional(event.tool_call_id, "toolCallId",
                              "web tool execution event");
    require_non_empty_optional(event.tool_name, "toolName",
                              "web tool execution event");
  }

  if (event.type == "tool_execution_update" &&
      event.partial_result.is_null()) {
    throw std::runtime_error(
        "web tool_execution_update event requires partialResult");
  }

  if (event.type == "tool_execution_end") {
    if (event.result.is_null()) {
      throw std::runtime_error("web tool_execution_end event requires result");
    }
    if (!event.is_error.has_value()) {
      throw std::runtime_error("web tool_execution_end event requires isError");
    }
  }
}

void require_extension_ui_update_data_shape(const Json& data) {
  if (!data.is_object()) {
    throw std::runtime_error(
        "web runtime sync extension_ui_update event requires data object");
  }
  if (!data.contains("extensionPath") || !data.at("extensionPath").is_string() ||
      data.at("extensionPath").get<std::string>().empty()) {
    throw std::runtime_error(
        "web runtime sync extension_ui_update event requires extensionPath");
  }

  if (!data.contains("contribution") ||
      !data.at("contribution").is_string()) {
    throw std::runtime_error(
        "web runtime sync extension_ui_update event requires contribution");
  }
  const std::string contribution =
      data.at("contribution").get<std::string>();
  if (!is_one_of(contribution,
                 {"status", "widget", "footer", "header", "working",
                  "title"})) {
    throw std::runtime_error(
        "web runtime sync extension_ui_update contribution is not supported");
  }

  const std::string action = data.value("action", std::string("set"));
  if (!is_one_of(action, {"set", "clear", "replace"})) {
    throw std::runtime_error(
        "web runtime sync extension_ui_update action is not supported");
  }

  if (contribution == "status" || contribution == "widget") {
    if (!data.contains("key") || !data.at("key").is_string() ||
        data.at("key").get<std::string>().empty()) {
      throw std::runtime_error(
          "web runtime sync extension_ui_update requires key");
    }
  }

  if (contribution == "status") {
    if (data.contains("text") && !data.at("text").is_null() &&
        !data.at("text").is_string()) {
      throw std::runtime_error(
          "web runtime sync extension_ui_update status text must be string");
    }
    return;
  }

  if (contribution == "widget") {
    const std::string placement =
        data.value("placement", std::string("aboveEditor"));
    if (!is_one_of(placement, {"aboveEditor", "belowEditor"})) {
      throw std::runtime_error(
          "web runtime sync extension_ui_update widget placement is not supported");
    }
    if (data.contains("content") && !data.at("content").is_null() &&
        !data.at("content").is_array()) {
      throw std::runtime_error(
          "web runtime sync extension_ui_update widget content must be an array");
    }
  }
}

void require_web_runtime_sync_event_shape(const WebRuntimeSyncEvent& event) {
  if (!is_one_of(event.type,
                 {"agent_start", "agent_end", "turn_start", "turn_end",
                  "message_start", "message_update", "message_end",
                  "tool_execution_start", "tool_execution_update",
                  "tool_execution_end", "queue_update", "model_select",
                  "thinking_level_select", "thinking_level_changed",
                  "compaction_start", "compaction_end", "session_info_changed",
                  "auto_retry_start", "auto_retry_end",
                  "extension_ui_update", "state-update", "session_create_start",
                  "session_create_end", "session_open_start",
                  "session_open_end", "session_fork_start",
                  "session_fork_end", "session_clone_start",
                  "session_clone_end"})) {
    throw std::runtime_error("web runtime sync event type is not supported");
  }
  if (event.session_id.has_value() && event.session_id->empty()) {
    throw std::runtime_error("web runtime sync event sessionId must not be empty");
  }
  if (event.request_id.has_value() && event.request_id->empty()) {
    throw std::runtime_error("web runtime sync event requestId must not be empty");
  }
  if (event.type == "queue_update") {
    if (!event.steer.is_array() || !event.follow_up.is_array() ||
        !event.next_turn.is_array()) {
      throw std::runtime_error(
          "web runtime sync queue_update event requires steer, followUp, and nextTurn arrays");
    }
  }
  if (event.type == "state-update" && !event.state.has_value()) {
    throw std::runtime_error(
        "web runtime sync state-update event requires state");
  }
  if (event.type == "model_select") {
    if (!event.model.is_object()) {
      throw std::runtime_error(
          "web runtime sync model_select event requires model object");
    }
  }
  if (is_runtime_session_mutation_event(event.type)) {
    if (is_runtime_session_mutation_start_event(event.type)) {
      if (!event.session_command.has_value()) {
        throw std::runtime_error(
            "web runtime sync session mutation start event requires sessionCommand");
      }
      const std::string& command_type = event.session_command->type;
      if ((event.type == "session_create_start" &&
           command_type != "new_session") ||
          (event.type == "session_open_start" &&
           !is_browser_session_open_command(command_type)) ||
          (event.type == "session_fork_start" && command_type != "fork") ||
          (event.type == "session_clone_start" && command_type != "clone")) {
        throw std::runtime_error(
            "web runtime sync session mutation start command does not match event type");
      }
    }
    if (is_runtime_session_mutation_end_event(event.type)) {
      if (!event.session_result.has_value()) {
        throw std::runtime_error(
            "web runtime sync session mutation end event requires sessionResult");
      }
      const std::string& command = event.session_result->command;
      if ((event.type == "session_create_end" &&
           command != "new_session") ||
          (event.type == "session_open_end" &&
           !is_browser_session_open_command(command)) ||
          (event.type == "session_fork_end" && command != "fork") ||
          (event.type == "session_clone_end" && command != "clone")) {
        throw std::runtime_error(
            "web runtime sync session mutation result command does not match event type");
      }
      if (event.session_result->success &&
          !event.session_result->session_id.has_value() &&
          !event.state.has_value()) {
        throw std::runtime_error(
            "web runtime sync successful session mutation end event requires sessionId or state");
      }
    }
  }
  if (event.type == "thinking_level_select" ||
      event.type == "thinking_level_changed") {
    require_non_empty_optional(event.thinking_level, "level",
                              "web runtime sync thinking event");
    if (!is_supported_thinking_level(*event.thinking_level)) {
      throw std::runtime_error(
          "web runtime sync thinking event level is not supported");
    }
  }
  if (event.previous_thinking_level.has_value() &&
      !is_supported_thinking_level(*event.previous_thinking_level)) {
    throw std::runtime_error(
        "web runtime sync thinking event previousLevel is not supported");
  }
  if (event.type == "compaction_start") {
    require_non_empty_optional(event.reason, "reason",
                              "web runtime sync compaction_start event");
  }
  if (event.type == "compaction_end") {
    require_non_empty_optional(event.reason, "reason",
                              "web runtime sync compaction_end event");
    if (!event.aborted.has_value()) {
      throw std::runtime_error(
          "web runtime sync compaction_end event requires aborted");
    }
    if (!event.will_retry.has_value()) {
      throw std::runtime_error(
          "web runtime sync compaction_end event requires willRetry");
    }
  }
  if (event.reason.has_value() &&
      event.type.find("compaction_") == 0 &&
      !is_one_of(*event.reason, {"manual", "threshold", "overflow"})) {
    throw std::runtime_error(
        "web runtime sync compaction event reason is not supported");
  }
  if (event.type == "tool_execution_start" ||
      event.type == "tool_execution_update" ||
      event.type == "tool_execution_end") {
    require_non_empty_optional(event.tool_call_id, "toolCallId",
                              "web runtime sync tool execution event");
    require_non_empty_optional(event.tool_name, "toolName",
                              "web runtime sync tool execution event");
  }
  if (event.type == "tool_execution_update" &&
      event.partial_result.is_null()) {
    throw std::runtime_error(
        "web runtime sync tool_execution_update event requires partialResult");
  }
  if (event.type == "tool_execution_end") {
    if (event.result.is_null()) {
      throw std::runtime_error(
          "web runtime sync tool_execution_end event requires result");
    }
    if (!event.is_error.has_value()) {
      throw std::runtime_error(
          "web runtime sync tool_execution_end event requires isError");
    }
  }
  if (event.type == "extension_ui_update") {
    require_extension_ui_update_data_shape(event.data);
  }
}

void require_tool_result_content_block_shape(const Json& block) {
  if (!block.is_object()) {
    throw std::runtime_error("web tool result content block must be an object");
  }
  const std::string type = block.at("type").get<std::string>();
  if (type == "text") {
    if (!block.contains("text") || !block.at("text").is_string()) {
      throw std::runtime_error(
          "web tool result text content requires text string");
    }
    return;
  }
  if (type == "image") {
    if (!block.contains("data") || !block.at("data").is_string() ||
        block.at("data").get<std::string>().empty()) {
      throw std::runtime_error(
          "web tool result image content requires data string");
    }
    if (!block.contains("mimeType") || !block.at("mimeType").is_string() ||
        block.at("mimeType").get<std::string>().empty()) {
      throw std::runtime_error(
          "web tool result image content requires mimeType string");
    }
    return;
  }
  throw std::runtime_error("web tool result content type is not supported");
}

void require_web_artifact_runtime_operation_shape(
    const WebArtifactRuntimeOperation& operation) {
  if (operation.type != "artifact-operation") {
    throw std::runtime_error(
        "web artifact runtime operation type must be artifact-operation");
  }
  if (!is_one_of(operation.action,
                 {"list", "get", "createOrUpdate", "delete"})) {
    throw std::runtime_error(
        "web artifact runtime operation action is not supported");
  }
  if (operation.action == "get" || operation.action == "createOrUpdate" ||
      operation.action == "delete") {
    require_non_empty_optional(operation.filename, "filename",
                              "web artifact runtime operation");
  }
  if (operation.action == "createOrUpdate") {
    require_non_empty_optional(operation.content, "content",
                              "web artifact runtime operation");
  }
  if (operation.mime_type.has_value() && operation.mime_type->empty()) {
    throw std::runtime_error(
        "web artifact runtime operation mimeType must not be empty");
  }
}

void require_web_artifact_runtime_response_shape(
    const WebArtifactRuntimeResponse& response) {
  if (!response.success) {
    require_non_empty_optional(response.error, "error",
                              "failed web artifact runtime response");
    return;
  }
  if (response.error.has_value() && response.error->empty()) {
    throw std::runtime_error(
        "web artifact runtime response error must not be empty");
  }
  if (response.result.is_object()) {
    throw std::runtime_error(
        "web artifact runtime response result must be array, string, scalar, or null");
  }
  if (response.result.is_array()) {
    for (const Json& item : response.result) {
      if (!item.is_string() || item.get<std::string>().empty()) {
        throw std::runtime_error(
            "web artifact runtime response list result must contain non-empty filenames");
      }
    }
  }
}

std::string expected_tool_execution_display_state(
    const std::optional<WebToolResultMessage>& result,
    bool pending,
    bool aborted,
    bool is_streaming) {
  if (result.has_value()) {
    return result->is_error ? "error" : "complete";
  }
  if (aborted) {
    return "error";
  }
  if (pending || is_streaming) {
    return "inprogress";
  }
  return "complete";
}

void require_web_tool_execution_display_shape(
    const WebToolExecutionDisplay& display) {
  if (display.tool_call_id.empty()) {
    throw std::runtime_error(
        "web tool execution display toolCall id must not be empty");
  }
  if (display.tool_name.empty()) {
    throw std::runtime_error(
        "web tool execution display toolCall name must not be empty");
  }
  if (display.renderer.empty()) {
    throw std::runtime_error(
        "web tool execution display renderer must not be empty");
  }
  if (!is_one_of(display.state, {"inprogress", "complete", "error"})) {
    throw std::runtime_error(
        "web tool execution display state is not supported");
  }
  const std::string expected = expected_tool_execution_display_state(
      display.result, display.pending, display.aborted, display.is_streaming);
  if (display.state != expected) {
    throw std::runtime_error(
        "web tool execution display state does not match result status");
  }
  if (display.result.has_value()) {
    if (display.result->tool_call_id != display.tool_call_id) {
      throw std::runtime_error(
          "web tool execution display result toolCallId must match toolCall id");
    }
    if (display.result->tool_name != display.tool_name) {
      throw std::runtime_error(
          "web tool execution display result toolName must match toolCall name");
    }
  }
}

Json payload_without_routing_fields(const Json& json) {
  Json payload = Json::object();
  if (!json.is_object()) {
    return payload;
  }
  for (auto it = json.begin(); it != json.end(); ++it) {
    if (it.key() == "sandboxId" || it.key() == "messageId" ||
        it.key() == "type" || it.key() == "success" || it.key() == "error") {
      continue;
    }
    payload[it.key()] = it.value();
  }
  return payload;
}

Json tool_call_json(const WebToolExecutionDisplay& display) {
  return Json{{"type", "toolCall"},
              {"id", display.tool_call_id},
              {"name", display.tool_name},
              {"arguments", display.arguments}};
}

std::string text_content_from_message(const Json& message) {
  if (!message.is_object()) {
    return {};
  }

  const std::string role = message.value("role", std::string{});
  if (role != "user" && role != "assistant") {
    return {};
  }

  if (!message.contains("content") || message.at("content").is_null()) {
    return {};
  }

  const Json& content = message.at("content");
  if (content.is_string()) {
    return content.get<std::string>();
  }
  if (!content.is_array()) {
    return {};
  }

  std::string text;
  for (const Json& block : content) {
    if (!block.is_object() || block.value("type", std::string{}) != "text" ||
        !block.contains("text") || !block.at("text").is_string()) {
      continue;
    }
    text += block.at("text").get<std::string>();
  }
  return text;
}

}  // namespace

std::string build_session_preview(const std::vector<Json>& messages,
                                  std::size_t max_bytes) {
  std::string preview;
  for (const Json& message : messages) {
    const std::string text = text_content_from_message(message);
    if (text.empty()) {
      continue;
    }
    if (!preview.empty()) {
      preview += '\n';
    }
    preview += text;
    if (preview.size() >= max_bytes) {
      preview.resize(max_bytes);
      return preview;
    }
  }
  if (preview.size() > max_bytes) {
    preview.resize(max_bytes);
  }
  return preview;
}

std::vector<RenderBlock> build_render_blocks_from_content(
    const std::vector<WebContentBlock>& content) {
  std::vector<RenderBlock> blocks;
  blocks.reserve(content.size());
  for (const WebContentBlock& block : content) {
    require_web_content_block_shape(block);
    if (block.type == "text") {
      blocks.push_back(RenderBlock{"markdown", *block.text,
                                   Json{{"content", Json(block)}}});
    } else if (block.type == "thinking") {
      Json data{{"content", Json(block)}};
      if (block.redacted.has_value()) {
        data["redacted"] = *block.redacted;
      }
      blocks.push_back(RenderBlock{"thinking", *block.thinking, data});
    } else if (block.type == "image") {
      Json data{{"content", Json(block)},
                {"fallbackText", image_fallback_label(*block.mime_type)}};
      blocks.push_back(RenderBlock{"image", std::string{},
                                   std::move(data)});
    } else if (block.type == "toolCall") {
      ToolRenderSnapshot snapshot;
      snapshot.call_id = *block.id;
      snapshot.name = *block.name;
      snapshot.arguments = block.arguments;
      snapshot.pending = true;
      blocks.push_back(RenderBlock{"tool", std::string{},
                                   Json{{"tool", snapshot},
                                        {"content", Json(block)}}});
    }
  }
  return blocks;
}

void to_json(Json& json, const SessionSummary& summary) {
  json = Json{
      {"session_id", summary.session_id},
      {"working_directory", summary.working_directory},
      {"message_count", summary.message_count},
  };
  if (summary.display_name.has_value()) {
    json["display_name"] = *summary.display_name;
  }
  if (summary.latest_summary.has_value()) {
    json["latest_summary"] = *summary.latest_summary;
  }
}

void from_json(const Json& json, SessionSummary& summary) {
  summary.session_id = json.at("session_id").get<std::string>();
  summary.working_directory = json.at("working_directory").get<std::string>();
  summary.message_count = json.value("message_count", 0U);
  if (json.contains("display_name") && !json.at("display_name").is_null()) {
    summary.display_name = json.at("display_name").get<std::string>();
  } else {
    summary.display_name.reset();
  }
  if (json.contains("latest_summary") && !json.at("latest_summary").is_null()) {
    summary.latest_summary = json.at("latest_summary").get<std::string>();
  } else {
    summary.latest_summary.reset();
  }
}

void to_json(Json& json, const MessagePreview& preview) {
  json = Json{{"role", preview.role}, {"text", preview.text}};
}

void from_json(const Json& json, MessagePreview& preview) {
  preview.role = json.at("role").get<std::string>();
  preview.text = json.at("text").get<std::string>();
}

void to_json(Json& json, const UsageSummary& usage) {
  json = Json{
      {"input_tokens", usage.input_tokens},
      {"output_tokens", usage.output_tokens},
      {"cache_read_tokens", usage.cache_read_tokens},
      {"cache_write_tokens", usage.cache_write_tokens},
      {"total_tokens", usage.total_tokens},
      {"total_cost_usd", usage.total_cost_usd},
  };
}

void from_json(const Json& json, UsageSummary& usage) {
  usage.input_tokens = json.value("input_tokens", 0);
  usage.output_tokens = json.value("output_tokens", 0);
  usage.cache_read_tokens = json.value("cache_read_tokens", 0);
  usage.cache_write_tokens = json.value("cache_write_tokens", 0);
  usage.total_tokens = json.value("total_tokens", 0);
  usage.total_cost_usd = json.value("total_cost_usd", 0.0);
}

void to_json(Json& json, const WebUsageCostBreakdown& cost) {
  json = Json{
      {"input", cost.input},
      {"output", cost.output},
      {"cacheRead", cost.cache_read},
      {"cacheWrite", cost.cache_write},
      {"total", cost.total},
  };
}

void from_json(const Json& json, WebUsageCostBreakdown& cost) {
  cost.input = json.value("input", 0.0);
  cost.output = json.value("output", 0.0);
  cost.cache_read = json.value("cacheRead", 0.0);
  cost.cache_write = json.value("cacheWrite", 0.0);
  cost.total = json.value("total", 0.0);
}

void to_json(Json& json, const WebSessionUsage& usage) {
  json = Json{
      {"input", usage.input},
      {"output", usage.output},
      {"cacheRead", usage.cache_read},
      {"cacheWrite", usage.cache_write},
      {"totalTokens", usage.total_tokens},
      {"cost", usage.cost},
  };
}

void from_json(const Json& json, WebSessionUsage& usage) {
  usage.input = json.value("input", 0);
  usage.output = json.value("output", 0);
  usage.cache_read = json.value("cacheRead", 0);
  usage.cache_write = json.value("cacheWrite", 0);
  usage.total_tokens = json.value("totalTokens", 0);
  usage.cost = json.value("cost", Json::object()).get<WebUsageCostBreakdown>();
}

void to_json(Json& json, const WebSessionMetadata& metadata) {
  json = Json{
      {"id", metadata.id},
      {"title", metadata.title},
      {"createdAt", metadata.created_at},
      {"lastModified", metadata.last_modified},
      {"messageCount", metadata.message_count},
      {"usage", metadata.usage},
      {"thinkingLevel", metadata.thinking_level},
      {"preview", metadata.preview},
  };
}

void from_json(const Json& json, WebSessionMetadata& metadata) {
  metadata.id = json.at("id").get<std::string>();
  metadata.title = json.at("title").get<std::string>();
  metadata.created_at = json.at("createdAt").get<std::string>();
  metadata.last_modified = json.at("lastModified").get<std::string>();
  metadata.message_count = json.value("messageCount", 0U);
  metadata.usage = json.value("usage", Json::object()).get<WebSessionUsage>();
  metadata.thinking_level = json.value("thinkingLevel", std::string("off"));
  metadata.preview = json.value("preview", std::string{});
  if (metadata.id.empty()) {
    throw std::runtime_error("web session metadata id must not be empty");
  }
  if (!is_supported_thinking_level(metadata.thinking_level)) {
    throw std::runtime_error(
        "web session metadata thinkingLevel is not supported");
  }
}

void to_json(Json& json, const WebSessionData& data) {
  json = Json{
      {"id", data.id},
      {"title", data.title},
      {"model", data.model},
      {"thinkingLevel", data.thinking_level},
      {"messages", data.messages},
      {"createdAt", data.created_at},
      {"lastModified", data.last_modified},
  };
}

void from_json(const Json& json, WebSessionData& data) {
  data.id = json.at("id").get<std::string>();
  data.title = json.at("title").get<std::string>();
  data.model = normalized_object(json.at("model"));
  data.thinking_level = json.value("thinkingLevel", std::string("off"));
  data.messages = json.value("messages", std::vector<Json>{});
  data.created_at = json.at("createdAt").get<std::string>();
  data.last_modified = json.at("lastModified").get<std::string>();
  if (data.id.empty()) {
    throw std::runtime_error("web session data id must not be empty");
  }
  if (data.title.empty()) {
    throw std::runtime_error("web session data title must not be empty");
  }
  if (!is_supported_thinking_level(data.thinking_level)) {
    throw std::runtime_error("web session data thinkingLevel is not supported");
  }
}

void to_json(Json& json, const WebSessionStorageMutation& mutation) {
  json = Json{{"operation", mutation.operation}};
  if (mutation.session_id.has_value()) {
    json["sessionId"] = *mutation.session_id;
  }
  if (mutation.data.has_value()) {
    json["data"] = *mutation.data;
  }
  if (mutation.metadata.has_value()) {
    json["metadata"] = *mutation.metadata;
  }
  if (mutation.title.has_value()) {
    json["title"] = *mutation.title;
  }
}

void from_json(const Json& json, WebSessionStorageMutation& mutation) {
  mutation.operation = json.at("operation").get<std::string>();
  mutation.session_id =
      json.contains("sessionId") && !json.at("sessionId").is_null()
          ? std::optional<std::string>(json.at("sessionId").get<std::string>())
          : std::nullopt;
  mutation.data = json.contains("data") && !json.at("data").is_null()
                      ? std::optional<WebSessionData>(
                            json.at("data").get<WebSessionData>())
                      : std::nullopt;
  mutation.metadata =
      json.contains("metadata") && !json.at("metadata").is_null()
          ? std::optional<WebSessionMetadata>(
                json.at("metadata").get<WebSessionMetadata>())
          : std::nullopt;
  mutation.title = json.contains("title") && !json.at("title").is_null()
                       ? std::optional<std::string>(
                             json.at("title").get<std::string>())
                       : std::nullopt;
  require_web_session_storage_mutation_shape(mutation);
}

void to_json(Json& json, const WebExportToolDefinition& tool) {
  json = Json{{"name", tool.name},
              {"description", tool.description},
              {"parameters", tool.parameters}};
}

void from_json(const Json& json, WebExportToolDefinition& tool) {
  tool.name = json.at("name").get<std::string>();
  tool.description = json.value("description", std::string{});
  tool.parameters = normalized_object(json.value("parameters", Json::object()));
  require_web_export_tool_definition_shape(tool);
}

void to_json(Json& json, const WebExportRenderedToolHtml& html) {
  json = Json::object();
  if (html.call_html.has_value()) {
    json["callHtml"] = *html.call_html;
  }
  if (html.result_html_collapsed.has_value()) {
    json["resultHtmlCollapsed"] = *html.result_html_collapsed;
  }
  if (html.result_html_expanded.has_value()) {
    json["resultHtmlExpanded"] = *html.result_html_expanded;
  }
}

void from_json(const Json& json, WebExportRenderedToolHtml& html) {
  html.call_html =
      json.contains("callHtml") && !json.at("callHtml").is_null()
          ? std::optional<std::string>(json.at("callHtml").get<std::string>())
          : std::nullopt;
  html.result_html_collapsed =
      json.contains("resultHtmlCollapsed") &&
              !json.at("resultHtmlCollapsed").is_null()
          ? std::optional<std::string>(
                json.at("resultHtmlCollapsed").get<std::string>())
          : std::nullopt;
  html.result_html_expanded =
      json.contains("resultHtmlExpanded") &&
              !json.at("resultHtmlExpanded").is_null()
          ? std::optional<std::string>(
                json.at("resultHtmlExpanded").get<std::string>())
          : std::nullopt;
  require_web_export_rendered_tool_html_shape(html);
}

void to_json(Json& json, const WebExportSessionData& data) {
  json = Json{{"header", data.header}, {"entries", data.entries}};
  if (data.leaf_id.has_value()) {
    json["leafId"] = *data.leaf_id;
  }
  if (data.system_prompt.has_value()) {
    json["systemPrompt"] = *data.system_prompt;
  }
  if (!data.tools.empty()) {
    json["tools"] = data.tools;
  }
  if (!data.rendered_tools.empty()) {
    Json rendered_tools = Json::object();
    for (const auto& rendered : data.rendered_tools) {
      rendered_tools[rendered.first] = rendered.second;
    }
    json["renderedTools"] = rendered_tools;
  }
}

void from_json(const Json& json, WebExportSessionData& data) {
  data.header = normalized_object(json.at("header"));
  data.entries = json.value("entries", std::vector<Json>{});
  data.leaf_id =
      json.contains("leafId") && !json.at("leafId").is_null()
          ? std::optional<std::string>(json.at("leafId").get<std::string>())
          : std::nullopt;
  data.system_prompt =
      json.contains("systemPrompt") && !json.at("systemPrompt").is_null()
          ? std::optional<std::string>(
                json.at("systemPrompt").get<std::string>())
          : std::nullopt;
  data.tools = json.contains("tools") && !json.at("tools").is_null()
                   ? json.at("tools").get<std::vector<WebExportToolDefinition>>()
                   : std::vector<WebExportToolDefinition>{};
  data.rendered_tools.clear();
  if (json.contains("renderedTools") && !json.at("renderedTools").is_null()) {
    if (!json.at("renderedTools").is_object()) {
      throw std::runtime_error(
          "web export session data renderedTools must be an object");
    }
    for (const auto& item : json.at("renderedTools").items()) {
      data.rendered_tools.emplace(
          item.key(), item.value().get<WebExportRenderedToolHtml>());
    }
  }
  require_web_export_session_data_shape(data);
}

void to_json(Json& json, const ProviderKeyStatus& status) {
  json = Json{
      {"provider", status.provider},
      {"hasKey", status.has_key},
      {"testing", status.testing},
      {"failed", status.failed},
      {"inputChanged", status.input_changed},
  };
}

void from_json(const Json& json, ProviderKeyStatus& status) {
  status.provider = json.at("provider").get<std::string>();
  status.has_key = json.value("hasKey", false);
  status.testing = json.value("testing", false);
  status.failed = json.value("failed", false);
  status.input_changed = json.value("inputChanged", false);
  if (status.provider.empty()) {
    throw std::runtime_error("provider key status provider must not be empty");
  }
  if (status.testing && status.failed) {
    throw std::runtime_error(
        "provider key status cannot be testing and failed simultaneously");
  }
}

void to_json(Json& json, const WebCustomProvider& provider) {
  json = Json{{"id", provider.id},
              {"name", provider.name},
              {"type", provider.type},
              {"baseUrl", provider.base_url}};
  if (provider.api_key.has_value()) {
    json["apiKey"] = *provider.api_key;
  }
  if (!provider.models.empty()) {
    json["models"] = provider.models;
  }
}

void from_json(const Json& json, WebCustomProvider& provider) {
  provider.id = json.at("id").get<std::string>();
  provider.name = json.at("name").get<std::string>();
  provider.type = json.at("type").get<std::string>();
  provider.base_url = json.at("baseUrl").get<std::string>();
  provider.api_key =
      json.contains("apiKey") && !json.at("apiKey").is_null()
          ? std::optional<std::string>(json.at("apiKey").get<std::string>())
          : std::nullopt;
  provider.models = json.contains("models") && !json.at("models").is_null()
                        ? json.at("models").get<std::vector<Json>>()
                        : std::vector<Json>{};
  require_web_custom_provider_shape(provider);
}

void to_json(Json& json, const WebDialogRequest& request) {
  json = Json{{"type", request.type},
              {"secret", request.secret},
              {"currentModel", request.current_model},
              {"allowedProviders", request.allowed_providers},
              {"sessions", request.sessions}};
  if (request.id.has_value()) {
    json["id"] = *request.id;
  }
  if (request.title.has_value()) {
    json["title"] = *request.title;
  }
  if (request.description.has_value()) {
    json["description"] = *request.description;
  }
  if (request.provider.has_value()) {
    json["provider"] = *request.provider;
  }
  if (request.default_value.has_value()) {
    json["defaultValue"] = *request.default_value;
  }
}

void from_json(const Json& json, WebDialogRequest& request) {
  request.id = json.contains("id") && !json.at("id").is_null()
                   ? std::optional<std::string>(json.at("id").get<std::string>())
                   : std::nullopt;
  request.type = json.at("type").get<std::string>();
  request.title =
      json.contains("title") && !json.at("title").is_null()
          ? std::optional<std::string>(json.at("title").get<std::string>())
          : std::nullopt;
  request.description =
      json.contains("description") && !json.at("description").is_null()
          ? std::optional<std::string>(
                json.at("description").get<std::string>())
          : std::nullopt;
  request.provider =
      json.contains("provider") && !json.at("provider").is_null()
          ? std::optional<std::string>(json.at("provider").get<std::string>())
          : std::nullopt;
  request.secret = json.value("secret", false);
  request.default_value =
      json.contains("defaultValue") && !json.at("defaultValue").is_null()
          ? std::optional<std::string>(
                json.at("defaultValue").get<std::string>())
          : std::nullopt;
  request.current_model =
      json.contains("currentModel") ? json.at("currentModel") : Json(nullptr);
  request.allowed_providers =
      json.value("allowedProviders", std::vector<std::string>{});
  request.sessions = json.contains("sessions") ? json.at("sessions") : Json::array();
  require_web_dialog_request_shape(request);
}

void to_json(Json& json, const WebDialogResult& result) {
  json = Json{{"type", result.type},
              {"accepted", result.accepted},
              {"deletedSessionIds", result.deleted_session_ids},
              {"model", result.model}};
  if (result.id.has_value()) {
    json["id"] = *result.id;
  }
  if (result.value.has_value()) {
    json["value"] = *result.value;
  }
  if (result.provider.has_value()) {
    json["provider"] = *result.provider;
  }
  if (result.session_id.has_value()) {
    json["sessionId"] = *result.session_id;
  }
  if (result.persistent_granted.has_value()) {
    json["persistentGranted"] = *result.persistent_granted;
  }
  if (result.error_message.has_value()) {
    json["errorMessage"] = *result.error_message;
  }
}

void from_json(const Json& json, WebDialogResult& result) {
  result.id = json.contains("id") && !json.at("id").is_null()
                  ? std::optional<std::string>(json.at("id").get<std::string>())
                  : std::nullopt;
  result.type = json.at("type").get<std::string>();
  result.accepted = json.value("accepted", false);
  result.value =
      json.contains("value") && !json.at("value").is_null()
          ? std::optional<std::string>(json.at("value").get<std::string>())
          : std::nullopt;
  result.provider =
      json.contains("provider") && !json.at("provider").is_null()
          ? std::optional<std::string>(json.at("provider").get<std::string>())
          : std::nullopt;
  result.session_id =
      json.contains("sessionId") && !json.at("sessionId").is_null()
          ? std::optional<std::string>(json.at("sessionId").get<std::string>())
          : std::nullopt;
  result.deleted_session_ids =
      json.value("deletedSessionIds", std::vector<std::string>{});
  result.model = json.contains("model") ? json.at("model") : Json(nullptr);
  result.persistent_granted =
      json.contains("persistentGranted") &&
              !json.at("persistentGranted").is_null()
          ? std::optional<bool>(json.at("persistentGranted").get<bool>())
          : std::nullopt;
  result.error_message =
      json.contains("errorMessage") && !json.at("errorMessage").is_null()
          ? std::optional<std::string>(
                json.at("errorMessage").get<std::string>())
          : std::nullopt;
  require_web_dialog_result_shape(result);
}

void to_json(Json& json, const WebAuthTokenMutation& mutation) {
  json = Json{{"operation", mutation.operation},
              {"storageKey", mutation.storage_key},
              {"requestedWithPrompt", mutation.requested_with_prompt}};
  if (mutation.token.has_value()) {
    json["token"] = *mutation.token;
  }
}

void from_json(const Json& json, WebAuthTokenMutation& mutation) {
  mutation.operation = json.at("operation").get<std::string>();
  mutation.storage_key = json.value("storageKey", std::string("auth-token"));
  mutation.token =
      json.contains("token") && !json.at("token").is_null()
          ? std::optional<std::string>(json.at("token").get<std::string>())
          : std::nullopt;
  mutation.requested_with_prompt = json.value("requestedWithPrompt", false);
  require_web_auth_token_mutation_shape(mutation);
}

void to_json(Json& json, const WebStorageQuotaInfo& info) {
  json = Json{{"usage", info.usage},
              {"quota", info.quota},
              {"percent", info.percent}};
}

void from_json(const Json& json, WebStorageQuotaInfo& info) {
  info.usage = json.value("usage", 0);
  info.quota = json.value("quota", 0);
  info.percent = json.value("percent", 0.0);
  require_web_storage_quota_info_shape(info);
}

void to_json(Json& json, const WebStorageBackendRequest& request) {
  json = Json{{"operation", request.operation},
              {"value", request.value},
              {"direction", request.direction},
              {"storeNames", request.store_names},
              {"mode", request.mode}};
  if (request.store_name.has_value()) {
    json["storeName"] = *request.store_name;
  }
  if (request.key.has_value()) {
    json["key"] = *request.key;
  }
  if (request.prefix.has_value()) {
    json["prefix"] = *request.prefix;
  }
  if (request.index_name.has_value()) {
    json["indexName"] = *request.index_name;
  }
}

void from_json(const Json& json, WebStorageBackendRequest& request) {
  request.operation = json.at("operation").get<std::string>();
  request.store_name =
      json.contains("storeName") && !json.at("storeName").is_null()
          ? std::optional<std::string>(json.at("storeName").get<std::string>())
          : std::nullopt;
  request.key = json.contains("key") && !json.at("key").is_null()
                    ? std::optional<std::string>(json.at("key").get<std::string>())
                    : std::nullopt;
  request.value = json.contains("value") ? json.at("value") : Json(nullptr);
  request.prefix =
      json.contains("prefix") && !json.at("prefix").is_null()
          ? std::optional<std::string>(json.at("prefix").get<std::string>())
          : std::nullopt;
  request.index_name =
      json.contains("indexName") && !json.at("indexName").is_null()
          ? std::optional<std::string>(json.at("indexName").get<std::string>())
          : std::nullopt;
  request.direction = json.value("direction", std::string("asc"));
  request.store_names = json.value("storeNames", std::vector<std::string>{});
  request.mode = json.value("mode", std::string("readonly"));
  require_web_storage_backend_request_shape(request);
}

void to_json(Json& json, const WebStorageBackendResult& result) {
  json = Json{{"operation", result.operation},
              {"success", result.success},
              {"value", result.value},
              {"keys", result.keys},
              {"values", result.values}};
  if (result.exists.has_value()) {
    json["exists"] = *result.exists;
  }
  if (result.quota_info.has_value()) {
    json["quotaInfo"] = *result.quota_info;
  }
  if (result.persistent_granted.has_value()) {
    json["persistentGranted"] = *result.persistent_granted;
  }
  if (result.error_message.has_value()) {
    json["errorMessage"] = *result.error_message;
  }
}

void from_json(const Json& json, WebStorageBackendResult& result) {
  result.operation = json.at("operation").get<std::string>();
  result.success = json.value("success", true);
  result.value = json.contains("value") ? json.at("value") : Json(nullptr);
  result.keys = json.value("keys", std::vector<std::string>{});
  result.values = json.contains("values") ? json.at("values") : Json::array();
  result.exists = json.contains("exists") && !json.at("exists").is_null()
                      ? std::optional<bool>(json.at("exists").get<bool>())
                      : std::nullopt;
  result.quota_info =
      json.contains("quotaInfo") && !json.at("quotaInfo").is_null()
          ? std::optional<WebStorageQuotaInfo>(
                json.at("quotaInfo").get<WebStorageQuotaInfo>())
          : std::nullopt;
  result.persistent_granted =
      json.contains("persistentGranted") &&
              !json.at("persistentGranted").is_null()
          ? std::optional<bool>(json.at("persistentGranted").get<bool>())
          : std::nullopt;
  result.error_message =
      json.contains("errorMessage") && !json.at("errorMessage").is_null()
          ? std::optional<std::string>(
                json.at("errorMessage").get<std::string>())
          : std::nullopt;
  require_web_storage_backend_result_shape(result);
}

void to_json(Json& json, const WebStorageIndexConfig& config) {
  json = Json{{"name", config.name}, {"keyPath", config.key_path}};
  if (config.unique) {
    json["unique"] = true;
  }
}

void from_json(const Json& json, WebStorageIndexConfig& config) {
  config.name = json.at("name").get<std::string>();
  config.key_path = json.at("keyPath").get<std::string>();
  config.unique = json.value("unique", false);

  if (config.name.empty()) {
    throw std::runtime_error("web storage index config name must not be empty");
  }
  if (config.key_path.empty()) {
    throw std::runtime_error(
        "web storage index config keyPath must not be empty");
  }
}

void to_json(Json& json, const WebStorageStoreConfig& config) {
  json = Json{{"name", config.name}};
  if (config.key_path.has_value()) {
    json["keyPath"] = *config.key_path;
  }
  if (config.auto_increment) {
    json["autoIncrement"] = true;
  }
  if (!config.indices.empty()) {
    json["indices"] = config.indices;
  }
}

void from_json(const Json& json, WebStorageStoreConfig& config) {
  config.name = json.at("name").get<std::string>();
  config.key_path =
      json.contains("keyPath") && !json.at("keyPath").is_null()
          ? std::optional<std::string>(json.at("keyPath").get<std::string>())
          : std::nullopt;
  config.auto_increment = json.value("autoIncrement", false);
  config.indices = json.contains("indices") && !json.at("indices").is_null()
                       ? json.at("indices")
                             .get<std::vector<WebStorageIndexConfig>>()
                       : std::vector<WebStorageIndexConfig>{};

  if (config.name.empty()) {
    throw std::runtime_error("web storage store config name must not be empty");
  }
  if (config.key_path.has_value() && config.key_path->empty()) {
    throw std::runtime_error(
        "web storage store config keyPath must not be empty");
  }
}

void to_json(Json& json, const WebIndexedDBConfig& config) {
  json = Json{{"dbName", config.db_name},
              {"version", config.version},
              {"stores", config.stores}};
}

void from_json(const Json& json, WebIndexedDBConfig& config) {
  config.db_name = json.at("dbName").get<std::string>();
  config.version = json.at("version").get<int>();
  config.stores = json.at("stores").get<std::vector<WebStorageStoreConfig>>();

  if (config.db_name.empty()) {
    throw std::runtime_error("web indexeddb config dbName must not be empty");
  }
  if (config.version <= 0) {
    throw std::runtime_error("web indexeddb config version must be positive");
  }
  if (config.stores.empty()) {
    throw std::runtime_error("web indexeddb config stores must not be empty");
  }
}

void to_json(Json& json, const RenderBlock& block) {
  json = Json{
      {"type", block.type},
      {"text", block.text},
      {"data", block.data},
  };
}

void from_json(const Json& json, RenderBlock& block) {
  block.type = json.at("type").get<std::string>();
  if (block.type.empty()) {
    throw std::runtime_error("render block type must not be empty");
  }
  block.text = json.value("text", std::string{});
  block.data = normalized_object(json.value("data", Json::object()));
}

void to_json(Json& json, const WebContentBlock& block) {
  json = Json{{"type", block.type}};
  if (block.text.has_value()) {
    json["text"] = *block.text;
  }
  if (block.text_signature.has_value()) {
    json["textSignature"] = *block.text_signature;
  }
  if (block.thinking.has_value()) {
    json["thinking"] = *block.thinking;
  }
  if (block.thinking_signature.has_value()) {
    json["thinkingSignature"] = *block.thinking_signature;
  }
  if (block.redacted.has_value()) {
    json["redacted"] = *block.redacted;
  }
  if (block.data.has_value()) {
    json["data"] = *block.data;
  }
  if (block.mime_type.has_value()) {
    json["mimeType"] = *block.mime_type;
  }
  if (block.id.has_value()) {
    json["id"] = *block.id;
  }
  if (block.name.has_value()) {
    json["name"] = *block.name;
  }
  if (block.type == "toolCall") {
    json["arguments"] = block.arguments;
  }
  if (block.thought_signature.has_value()) {
    json["thoughtSignature"] = *block.thought_signature;
  }
}

void from_json(const Json& json, WebContentBlock& block) {
  block.type = json.at("type").get<std::string>();
  block.text = json.contains("text") && !json.at("text").is_null()
                   ? std::optional<std::string>(json.at("text").get<std::string>())
                   : std::nullopt;
  block.text_signature =
      json.contains("textSignature") && !json.at("textSignature").is_null()
          ? std::optional<std::string>(
                json.at("textSignature").get<std::string>())
          : std::nullopt;
  block.thinking =
      json.contains("thinking") && !json.at("thinking").is_null()
          ? std::optional<std::string>(json.at("thinking").get<std::string>())
          : std::nullopt;
  block.thinking_signature =
      json.contains("thinkingSignature") &&
              !json.at("thinkingSignature").is_null()
          ? std::optional<std::string>(
                json.at("thinkingSignature").get<std::string>())
          : std::nullopt;
  block.redacted = json.contains("redacted") && !json.at("redacted").is_null()
                       ? std::optional<bool>(json.at("redacted").get<bool>())
                       : std::nullopt;
  block.data = json.contains("data") && !json.at("data").is_null()
                   ? std::optional<std::string>(json.at("data").get<std::string>())
                   : std::nullopt;
  block.mime_type =
      json.contains("mimeType") && !json.at("mimeType").is_null()
          ? std::optional<std::string>(json.at("mimeType").get<std::string>())
          : std::nullopt;
  block.id = json.contains("id") && !json.at("id").is_null()
                 ? std::optional<std::string>(json.at("id").get<std::string>())
                 : std::nullopt;
  block.name =
      json.contains("name") && !json.at("name").is_null()
          ? std::optional<std::string>(json.at("name").get<std::string>())
          : std::nullopt;
  block.arguments =
      json.contains("arguments") ? json.at("arguments") : Json::object();
  block.thought_signature =
      json.contains("thoughtSignature") &&
              !json.at("thoughtSignature").is_null()
          ? std::optional<std::string>(
                json.at("thoughtSignature").get<std::string>())
          : std::nullopt;
  require_web_content_block_shape(block);
}

void to_json(Json& json, const ToolRenderSnapshot& snapshot) {
  const std::string state =
      snapshot.state.empty()
          ? expected_tool_render_snapshot_state(
                snapshot.result, snapshot.pending, snapshot.aborted,
                snapshot.is_streaming, snapshot.is_error)
          : snapshot.state;
  const Json tool_call{{"type", "toolCall"},
                       {"id", snapshot.call_id},
                       {"name", snapshot.name},
                       {"arguments", snapshot.arguments}};
  json = Json{
      {"call_id", snapshot.call_id},
      {"name", snapshot.name},
      {"arguments", snapshot.arguments},
      {"toolCall", tool_call},
      {"result", snapshot.result},
      {"pending", snapshot.pending},
      {"aborted", snapshot.aborted},
      {"isStreaming", snapshot.is_streaming},
      {"is_error", snapshot.is_error},
      {"state", state},
      {"renderer", snapshot.renderer},
      {"isCustom", snapshot.is_custom},
  };
}

void from_json(const Json& json, ToolRenderSnapshot& snapshot) {
  const Json tool_call =
      json.contains("toolCall") ? json.at("toolCall") : Json(nullptr);
  if (!tool_call.is_null()) {
    if (!tool_call.is_object()) {
      throw std::runtime_error(
          "tool render snapshot toolCall must be an object");
    }
    const std::string type = tool_call.value("type", std::string("toolCall"));
    if (type != "toolCall") {
      throw std::runtime_error(
          "tool render snapshot toolCall type must be toolCall");
    }
    snapshot.call_id = tool_call.at("id").get<std::string>();
    snapshot.name = tool_call.at("name").get<std::string>();
    snapshot.arguments =
        normalized_object(tool_call.value("arguments", Json::object()));
  } else {
    snapshot.call_id = json.at("call_id").get<std::string>();
    snapshot.name = json.at("name").get<std::string>();
    snapshot.arguments =
        normalized_object(json.value("arguments", Json::object()));
  }
  snapshot.result = json.contains("result") ? json.at("result") : Json(nullptr);
  snapshot.pending = json.value("pending", false);
  snapshot.aborted = json.value("aborted", false);
  snapshot.is_streaming = json.value("isStreaming", false);
  snapshot.is_error = json.value("is_error", false);
  if (snapshot.result.is_object() && snapshot.result.contains("isError") &&
      snapshot.result.at("isError").is_boolean()) {
    snapshot.is_error = snapshot.result.at("isError").get<bool>();
  }
  snapshot.state =
      json.value("state", expected_tool_render_snapshot_state(
                              snapshot.result, snapshot.pending,
                              snapshot.aborted, snapshot.is_streaming,
                              snapshot.is_error));
  snapshot.renderer = json.value("renderer", std::string("default"));
  snapshot.is_custom = json.value("isCustom", false);
  require_tool_render_snapshot_shape(snapshot);
}

void to_json(Json& json, const WebAttachment& attachment) {
  json = Json{
      {"id", attachment.id},
      {"type", attachment.type},
      {"fileName", attachment.file_name},
      {"mimeType", attachment.mime_type},
      {"size", attachment.size},
      {"content", attachment.content},
  };
  if (attachment.extracted_text.has_value()) {
    json["extractedText"] = *attachment.extracted_text;
  }
  if (attachment.preview.has_value()) {
    json["preview"] = *attachment.preview;
  }
}

void from_json(const Json& json, WebAttachment& attachment) {
  attachment.id = json.at("id").get<std::string>();
  attachment.type = json.at("type").get<std::string>();
  attachment.file_name = json.at("fileName").get<std::string>();
  attachment.mime_type = json.at("mimeType").get<std::string>();
  attachment.size = json.value("size", std::uint64_t{0});
  attachment.content = json.value("content", std::string{});
  attachment.extracted_text =
      json.contains("extractedText") && !json.at("extractedText").is_null()
          ? std::optional<std::string>(
                json.at("extractedText").get<std::string>())
          : std::nullopt;
  attachment.preview =
      json.contains("preview") && !json.at("preview").is_null()
          ? std::optional<std::string>(json.at("preview").get<std::string>())
          : std::nullopt;

  if (attachment.id.empty()) {
    throw std::runtime_error("web attachment id must not be empty");
  }
  if (!is_one_of(attachment.type, {"image", "document"})) {
    throw std::runtime_error("web attachment type is not supported");
  }
  if (attachment.file_name.empty()) {
    throw std::runtime_error("web attachment fileName must not be empty");
  }
  if (attachment.mime_type.empty()) {
    throw std::runtime_error("web attachment mimeType must not be empty");
  }
}

void to_json(Json& json, const WebArtifactMessage& message) {
  json = Json{
      {"role", message.role},
      {"action", message.action},
      {"filename", message.filename},
      {"timestamp", message.timestamp},
  };
  if (message.content.has_value()) {
    json["content"] = *message.content;
  }
  if (message.title.has_value()) {
    json["title"] = *message.title;
  }
}

void from_json(const Json& json, WebArtifactMessage& message) {
  message.role = json.value("role", std::string("artifact"));
  message.action = json.at("action").get<std::string>();
  message.filename = json.at("filename").get<std::string>();
  message.content =
      json.contains("content") && !json.at("content").is_null()
          ? std::optional<std::string>(json.at("content").get<std::string>())
          : std::nullopt;
  message.title = json.contains("title") && !json.at("title").is_null()
                      ? std::optional<std::string>(json.at("title").get<std::string>())
                      : std::nullopt;
  message.timestamp = json.at("timestamp").get<std::string>();

  if (message.role != "artifact") {
    throw std::runtime_error("web artifact message role must be artifact");
  }
  if (!is_one_of(message.action, {"create", "update", "delete"})) {
    throw std::runtime_error("web artifact message action is not supported");
  }
  if (message.filename.empty()) {
    throw std::runtime_error("web artifact message filename must not be empty");
  }
  if (message.timestamp.empty()) {
    throw std::runtime_error("web artifact message timestamp must not be empty");
  }
  if ((message.action == "create" || message.action == "update") &&
      !message.content.has_value()) {
    throw std::runtime_error(
        "web artifact message content is required for create/update");
  }
}

void to_json(Json& json, const WebArtifactRuntimeOperation& operation) {
  json = Json{{"type", operation.type}, {"action", operation.action}};
  if (operation.filename.has_value()) {
    json["filename"] = *operation.filename;
  }
  if (operation.content.has_value()) {
    json["content"] = *operation.content;
  }
  if (operation.mime_type.has_value()) {
    json["mimeType"] = *operation.mime_type;
  }
}

void from_json(const Json& json, WebArtifactRuntimeOperation& operation) {
  operation.type = json.value("type", std::string("artifact-operation"));
  operation.action = json.at("action").get<std::string>();
  operation.filename =
      json.contains("filename") && !json.at("filename").is_null()
          ? std::optional<std::string>(json.at("filename").get<std::string>())
          : std::nullopt;
  operation.content =
      json.contains("content") && !json.at("content").is_null()
          ? std::optional<std::string>(json.at("content").get<std::string>())
          : std::nullopt;
  operation.mime_type =
      json.contains("mimeType") && !json.at("mimeType").is_null()
          ? std::optional<std::string>(json.at("mimeType").get<std::string>())
          : std::nullopt;

  require_web_artifact_runtime_operation_shape(operation);
}

void to_json(Json& json, const WebArtifactRuntimeResponse& response) {
  json = Json{{"success", response.success}};
  if (!response.result.is_null()) {
    json["result"] = response.result;
  }
  if (response.error.has_value()) {
    json["error"] = *response.error;
  }
}

void from_json(const Json& json, WebArtifactRuntimeResponse& response) {
  response.success = json.value("success", true);
  response.result = json.contains("result") ? json.at("result") : Json(nullptr);
  response.error = json.contains("error") && !json.at("error").is_null()
                       ? std::optional<std::string>(
                             json.at("error").get<std::string>())
                       : std::nullopt;

  require_web_artifact_runtime_response_shape(response);
}

void to_json(Json& json, const WebArtifactsToolParams& params) {
  json = Json{{"command", params.command}, {"filename", params.filename}};
  if (params.content.has_value()) {
    json["content"] = *params.content;
  }
  if (params.old_str.has_value()) {
    json["old_str"] = *params.old_str;
  }
  if (params.new_str.has_value()) {
    json["new_str"] = *params.new_str;
  }
}

void from_json(const Json& json, WebArtifactsToolParams& params) {
  params.command = json.at("command").get<std::string>();
  params.filename = json.at("filename").get<std::string>();
  params.content =
      json.contains("content") && !json.at("content").is_null()
          ? std::optional<std::string>(json.at("content").get<std::string>())
          : std::nullopt;
  params.old_str =
      json.contains("old_str") && !json.at("old_str").is_null()
          ? std::optional<std::string>(json.at("old_str").get<std::string>())
          : std::nullopt;
  params.new_str =
      json.contains("new_str") && !json.at("new_str").is_null()
          ? std::optional<std::string>(json.at("new_str").get<std::string>())
          : std::nullopt;

  if (!is_one_of(params.command,
                 {"create", "update", "rewrite", "get", "delete", "logs"})) {
    throw std::runtime_error("web artifacts tool command is not supported");
  }
  if (params.filename.empty()) {
    throw std::runtime_error("web artifacts tool filename must not be empty");
  }
  if (is_one_of(params.command, {"create", "rewrite"}) &&
      (!params.content.has_value() || params.content->empty())) {
    throw std::runtime_error(
        "web artifacts tool create/rewrite requires content");
  }
  if (params.command == "update") {
    if (!params.old_str.has_value() || params.old_str->empty()) {
      throw std::runtime_error("web artifacts tool update requires old_str");
    }
    if (!params.new_str.has_value()) {
      throw std::runtime_error("web artifacts tool update requires new_str");
    }
  }
}

void to_json(Json& json, const WebToolResultMessage& message) {
  json = Json{
      {"role", message.role},
      {"toolCallId", message.tool_call_id},
      {"toolName", message.tool_name},
      {"content", message.content},
      {"isError", message.is_error},
      {"timestamp", message.timestamp},
  };
  if (!message.details.is_null()) {
    json["details"] = message.details;
  }
}

void from_json(const Json& json, WebToolResultMessage& message) {
  message.role = json.value("role", std::string("toolResult"));
  message.tool_call_id = json.at("toolCallId").get<std::string>();
  message.tool_name = json.at("toolName").get<std::string>();
  message.content = json.at("content").get<std::vector<Json>>();
  message.details = json.contains("details") ? json.at("details") : Json(nullptr);
  message.is_error = json.value("isError", false);
  message.timestamp = json.at("timestamp").get<std::int64_t>();

  if (message.role != "toolResult") {
    throw std::runtime_error("web tool result message role must be toolResult");
  }
  if (message.tool_call_id.empty()) {
    throw std::runtime_error(
        "web tool result message toolCallId must not be empty");
  }
  if (message.tool_name.empty()) {
    throw std::runtime_error(
        "web tool result message toolName must not be empty");
  }
  if (message.timestamp <= 0) {
    throw std::runtime_error(
        "web tool result message timestamp must be positive");
  }
  for (const Json& block : message.content) {
    require_tool_result_content_block_shape(block);
  }
}

void to_json(Json& json, const WebToolExecutionDisplay& display) {
  const std::string state = display.state.empty()
                                ? expected_tool_execution_display_state(
                                      display.result, display.pending,
                                      display.aborted, display.is_streaming)
                                : display.state;
  json = Json{
      {"toolCall", tool_call_json(display)},
      {"pending", display.pending},
      {"aborted", display.aborted},
      {"isStreaming", display.is_streaming},
      {"state", state},
      {"renderer", display.renderer},
      {"isCustom", display.is_custom},
  };
  if (display.result.has_value()) {
    json["result"] = *display.result;
  }
}

void from_json(const Json& json, WebToolExecutionDisplay& display) {
  const Json& tool_call = json.at("toolCall");
  if (!tool_call.is_object()) {
    throw std::runtime_error(
        "web tool execution display toolCall must be an object");
  }
  const auto type = tool_call.value("type", std::string("toolCall"));
  if (type != "toolCall") {
    throw std::runtime_error(
        "web tool execution display toolCall type must be toolCall");
  }

  display.tool_call_id = tool_call.at("id").get<std::string>();
  display.tool_name = tool_call.at("name").get<std::string>();
  display.arguments = normalized_object(tool_call.value("arguments", Json::object()));
  display.result =
      json.contains("result") && !json.at("result").is_null()
          ? std::optional<WebToolResultMessage>(
                json.at("result").get<WebToolResultMessage>())
          : std::nullopt;
  display.pending = json.value("pending", false);
  display.aborted = json.value("aborted", false);
  display.is_streaming = json.value("isStreaming", false);
  display.state =
      json.value("state", expected_tool_execution_display_state(
                              display.result, display.pending,
                              display.aborted, display.is_streaming));
  display.renderer = json.value("renderer", std::string("default"));
  display.is_custom = json.value("isCustom", false);
  require_web_tool_execution_display_shape(display);
}

void to_json(Json& json, const WebMessage& message) {
  json = Json{
      {"id", message.id},
      {"role", message.role},
      {"blocks", message.blocks},
      {"metadata", message.metadata},
  };
  if (message.usage.has_value()) {
    json["usage"] = *message.usage;
  }
  if (message.stop_reason.has_value()) {
    json["stop_reason"] = *message.stop_reason;
  }
  if (message.error_message.has_value()) {
    json["error_message"] = *message.error_message;
  }
}

void from_json(const Json& json, WebMessage& message) {
  message.id = json.at("id").get<std::string>();
  message.role = json.at("role").get<std::string>();
  if (message.id.empty() || message.role.empty()) {
    throw std::runtime_error("web message requires id and role");
  }
  message.blocks = json.value("blocks", std::vector<RenderBlock>{});
  if (json.contains("usage") && !json.at("usage").is_null()) {
    message.usage = json.at("usage").get<UsageSummary>();
  } else {
    message.usage.reset();
  }
  if (json.contains("stop_reason") && !json.at("stop_reason").is_null()) {
    message.stop_reason = json.at("stop_reason").get<std::string>();
  } else {
    message.stop_reason.reset();
  }
  if (json.contains("error_message") && !json.at("error_message").is_null()) {
    message.error_message = json.at("error_message").get<std::string>();
  } else {
    message.error_message.reset();
  }
  message.metadata = normalized_object(json.value("metadata", Json::object()));
}

void to_json(Json& json, const WebSessionState& state) {
  json = Json{
      {"session", state.session},
      {"model", state.model},
      {"thinking_level", state.thinking_level},
      {"is_streaming", state.is_streaming},
      {"is_compacting", state.is_compacting},
      {"messages", state.messages},
  };
}

void from_json(const Json& json, WebSessionState& state) {
  state.session = json.at("session").get<SessionSummary>();
  state.model = normalized_object(json.value("model", Json::object()));
  state.thinking_level = json.value("thinking_level", std::string{});
  state.is_streaming = json.value("is_streaming", false);
  state.is_compacting = json.value("is_compacting", false);
  state.messages = json.value("messages", std::vector<WebMessage>{});
}

void to_json(Json& json, const WebSessionTreeNode& node) {
  json = Json{
      {"id", node.id},
      {"title", node.title},
      {"workingDirectory", node.working_directory},
      {"lastModified", node.last_modified},
      {"messageCount", node.message_count},
      {"active", node.active},
      {"children", node.children},
  };
  if (node.parent_id.has_value()) {
    json["parentId"] = *node.parent_id;
  }
  if (node.branch_from_entry_id.has_value()) {
    json["branchFromEntryId"] = *node.branch_from_entry_id;
  }
}

void from_json(const Json& json, WebSessionTreeNode& node) {
  node.id = json.at("id").get<std::string>();
  node.title = json.value("title", std::string{});
  node.working_directory = json.at("workingDirectory").get<std::string>();
  node.parent_id =
      json.contains("parentId") && !json.at("parentId").is_null()
          ? std::optional<std::string>(json.at("parentId").get<std::string>())
          : std::nullopt;
  node.branch_from_entry_id =
      json.contains("branchFromEntryId") &&
              !json.at("branchFromEntryId").is_null()
          ? std::optional<std::string>(
                json.at("branchFromEntryId").get<std::string>())
          : std::nullopt;
  node.last_modified = json.at("lastModified").get<std::string>();
  node.message_count = json.value("messageCount", 0U);
  node.active = json.value("active", false);
  node.children = json.value("children", std::vector<WebSessionTreeNode>{});
  if (node.id.empty()) {
    throw std::runtime_error("web session tree node id must not be empty");
  }
  if (node.working_directory.empty()) {
    throw std::runtime_error(
        "web session tree node workingDirectory must not be empty");
  }
  if (node.last_modified.empty()) {
    throw std::runtime_error(
        "web session tree node lastModified must not be empty");
  }
  if (node.parent_id.has_value() && node.parent_id->empty()) {
    throw std::runtime_error("web session tree node parentId must not be empty");
  }
  if (node.branch_from_entry_id.has_value() &&
      node.branch_from_entry_id->empty()) {
    throw std::runtime_error(
        "web session tree node branchFromEntryId must not be empty");
  }
}

void to_json(Json& json, const SessionMutationRequest& request) {
  json = Json{
      {"command", request.command},
      {"model", request.model},
  };
  if (request.session_id.has_value()) {
    json["session_id"] = *request.session_id;
  }
  if (request.title.has_value()) {
    json["title"] = *request.title;
  }
  if (request.working_directory.has_value()) {
    json["working_directory"] = *request.working_directory;
  }
  if (request.source_session_id.has_value()) {
    json["source_session_id"] = *request.source_session_id;
  }
  if (request.branch_from_entry_id.has_value()) {
    json["branch_from_entry_id"] = *request.branch_from_entry_id;
  }
  if (request.thinking_level.has_value()) {
    json["thinking_level"] = *request.thinking_level;
  }
}

void from_json(const Json& json, SessionMutationRequest& request) {
  request.command = json.at("command").get<std::string>();
  request.session_id =
      json.contains("session_id") && !json.at("session_id").is_null()
          ? std::optional<std::string>(json.at("session_id").get<std::string>())
          : std::nullopt;
  request.title = json.contains("title") && !json.at("title").is_null()
                      ? std::optional<std::string>(json.at("title").get<std::string>())
                      : std::nullopt;
  request.working_directory =
      json.contains("working_directory") && !json.at("working_directory").is_null()
          ? std::optional<std::string>(
                json.at("working_directory").get<std::string>())
          : std::nullopt;
  request.source_session_id =
      json.contains("source_session_id") && !json.at("source_session_id").is_null()
          ? std::optional<std::string>(
                json.at("source_session_id").get<std::string>())
          : std::nullopt;
  request.branch_from_entry_id =
      json.contains("branch_from_entry_id") &&
              !json.at("branch_from_entry_id").is_null()
          ? std::optional<std::string>(
                json.at("branch_from_entry_id").get<std::string>())
          : std::nullopt;
  request.model = normalized_object(json.value("model", Json::object()));
  request.thinking_level =
      json.contains("thinking_level") && !json.at("thinking_level").is_null()
          ? std::optional<std::string>(
                json.at("thinking_level").get<std::string>())
          : std::nullopt;
  require_session_mutation_shape(request);
}

void to_json(Json& json, const SessionMutationResponse& response) {
  json = Json{
      {"command", response.command},
      {"ok", response.ok},
  };
  if (response.session_id.has_value()) {
    json["session_id"] = *response.session_id;
  }
  if (response.session.has_value()) {
    json["session"] = *response.session;
  }
  if (response.state.has_value()) {
    json["state"] = *response.state;
  }
  if (response.error_message.has_value()) {
    json["error_message"] = *response.error_message;
  }
}

void from_json(const Json& json, SessionMutationResponse& response) {
  response.command = json.at("command").get<std::string>();
  response.ok = json.value("ok", false);
  response.session_id =
      json.contains("session_id") && !json.at("session_id").is_null()
          ? std::optional<std::string>(json.at("session_id").get<std::string>())
          : std::nullopt;
  response.session =
      json.contains("session") && !json.at("session").is_null()
          ? std::optional<SessionSummary>(json.at("session").get<SessionSummary>())
          : std::nullopt;
  response.state =
      json.contains("state") && !json.at("state").is_null()
          ? std::optional<WebSessionState>(json.at("state").get<WebSessionState>())
          : std::nullopt;
  response.error_message =
      json.contains("error_message") && !json.at("error_message").is_null()
          ? std::optional<std::string>(
                json.at("error_message").get<std::string>())
          : std::nullopt;
  require_session_mutation_shape(response);
}

void to_json(Json& json, const BrowserSessionMutationCommand& command) {
  json = Json{{"type", command.type}};
  if (command.id.has_value()) {
    json["id"] = *command.id;
  }
  if (command.session_id.has_value()) {
    json["sessionId"] = *command.session_id;
  }
  if (command.session_path.has_value()) {
    json["sessionPath"] = *command.session_path;
  }
  if (command.parent_session.has_value()) {
    json["parentSession"] = *command.parent_session;
  }
  if (command.entry_id.has_value()) {
    json["entryId"] = *command.entry_id;
  }
  if (command.working_directory.has_value()) {
    json["workingDirectory"] = *command.working_directory;
  }
  if (!command.options.empty()) {
    json["options"] = command.options;
  }
}

void from_json(const Json& json, BrowserSessionMutationCommand& command) {
  command.id = json.contains("id") && !json.at("id").is_null()
                   ? std::optional<std::string>(json.at("id").get<std::string>())
                   : std::nullopt;
  command.type = json.at("type").get<std::string>();
  command.session_id =
      json.contains("sessionId") && !json.at("sessionId").is_null()
          ? std::optional<std::string>(json.at("sessionId").get<std::string>())
          : std::nullopt;
  command.session_path =
      json.contains("sessionPath") && !json.at("sessionPath").is_null()
          ? std::optional<std::string>(
                json.at("sessionPath").get<std::string>())
          : std::nullopt;
  command.parent_session =
      json.contains("parentSession") && !json.at("parentSession").is_null()
          ? std::optional<std::string>(
                json.at("parentSession").get<std::string>())
          : std::nullopt;
  command.entry_id =
      json.contains("entryId") && !json.at("entryId").is_null()
          ? std::optional<std::string>(json.at("entryId").get<std::string>())
          : std::nullopt;
  command.working_directory =
      json.contains("workingDirectory") &&
              !json.at("workingDirectory").is_null()
          ? std::optional<std::string>(
                json.at("workingDirectory").get<std::string>())
          : std::nullopt;
  command.options = normalized_object(json.value("options", Json::object()));
  require_browser_session_mutation_command_shape(command);
}

void to_json(Json& json, const BrowserSessionMutationResult& result) {
  json = Json{{"type", result.type},
              {"command", result.command},
              {"success", result.success},
              {"data", result.data}};
  if (result.id.has_value()) {
    json["id"] = *result.id;
  }
  if (result.error.has_value()) {
    json["error"] = *result.error;
  }
  if (result.session_id.has_value()) {
    json["sessionId"] = *result.session_id;
  }
  if (result.state.has_value()) {
    json["state"] = *result.state;
  }
}

void from_json(const Json& json, BrowserSessionMutationResult& result) {
  result.id = json.contains("id") && !json.at("id").is_null()
                  ? std::optional<std::string>(json.at("id").get<std::string>())
                  : std::nullopt;
  result.type = json.value("type", std::string("response"));
  result.command = json.at("command").get<std::string>();
  result.success = json.value("success", false);
  result.data = normalized_object(json.value("data", Json::object()));
  result.error = json.contains("error") && !json.at("error").is_null()
                     ? std::optional<std::string>(
                           json.at("error").get<std::string>())
                     : std::nullopt;
  result.session_id =
      json.contains("sessionId") && !json.at("sessionId").is_null()
          ? std::optional<std::string>(json.at("sessionId").get<std::string>())
          : std::nullopt;
  result.state = json.contains("state") && !json.at("state").is_null()
                     ? std::optional<WebSessionState>(
                           json.at("state").get<WebSessionState>())
                     : std::nullopt;
  require_browser_session_mutation_result_shape(result);
}

void to_json(Json& json, const WebSessionCommand& command) {
  json = Json{
      {"type", command.type},
      {"session_id", command.session_id},
      {"attachments", command.attachments},
      {"model", command.model},
  };
  if (command.input.has_value()) {
    json["input"] = *command.input;
  }
  if (command.thinking_level.has_value()) {
    json["thinking_level"] = *command.thinking_level;
  }
  if (command.message_id.has_value()) {
    json["message_id"] = *command.message_id;
  }
  if (command.instructions.has_value()) {
    json["instructions"] = *command.instructions;
  }
}

void from_json(const Json& json, WebSessionCommand& command) {
  command.type = json.at("type").get<std::string>();
  command.session_id = json.at("session_id").get<std::string>();
  command.input = json.contains("input") && !json.at("input").is_null()
                      ? std::optional<std::string>(json.at("input").get<std::string>())
                      : std::nullopt;
  command.attachments =
      json.value("attachments", std::vector<WebAttachment>{});
  command.model = normalized_object(json.value("model", Json::object()));
  command.thinking_level =
      json.contains("thinking_level") && !json.at("thinking_level").is_null()
          ? std::optional<std::string>(
                json.at("thinking_level").get<std::string>())
          : std::nullopt;
  command.message_id =
      json.contains("message_id") && !json.at("message_id").is_null()
          ? std::optional<std::string>(json.at("message_id").get<std::string>())
          : std::nullopt;
  command.instructions =
      json.contains("instructions") && !json.at("instructions").is_null()
          ? std::optional<std::string>(
                json.at("instructions").get<std::string>())
          : std::nullopt;
  require_web_session_command_shape(command);
}

void to_json(Json& json, const StreamingDelta& delta) {
  json = Json{{"type", delta.type}, {"tool_call", delta.tool_call},
              {"message", delta.message}};
  if (delta.message_id.has_value()) {
    json["message_id"] = *delta.message_id;
  }
  if (delta.content_index.has_value()) {
    json["content_index"] = *delta.content_index;
  }
  if (delta.delta.has_value()) {
    json["delta"] = *delta.delta;
  }
  if (delta.content.has_value()) {
    json["content"] = *delta.content;
  }
  if (delta.reason.has_value()) {
    json["reason"] = *delta.reason;
  }
  if (delta.error_message.has_value()) {
    json["error_message"] = *delta.error_message;
  }
}

void from_json(const Json& json, StreamingDelta& delta) {
  delta.type = json.at("type").get<std::string>();
  delta.message_id =
      json.contains("message_id") && !json.at("message_id").is_null()
          ? std::optional<std::string>(json.at("message_id").get<std::string>())
          : std::nullopt;
  delta.content_index =
      json.contains("content_index") && !json.at("content_index").is_null()
          ? std::optional<int>(json.at("content_index").get<int>())
          : std::nullopt;
  delta.delta = json.contains("delta") && !json.at("delta").is_null()
                    ? std::optional<std::string>(json.at("delta").get<std::string>())
                    : std::nullopt;
  delta.content =
      json.contains("content") && !json.at("content").is_null()
          ? std::optional<std::string>(json.at("content").get<std::string>())
          : std::nullopt;
  delta.tool_call = json.contains("tool_call") ? json.at("tool_call") : Json(nullptr);
  delta.message = json.contains("message") ? json.at("message") : Json(nullptr);
  delta.reason =
      json.contains("reason") && !json.at("reason").is_null()
          ? std::optional<std::string>(json.at("reason").get<std::string>())
          : std::nullopt;
  delta.error_message =
      json.contains("error_message") && !json.at("error_message").is_null()
          ? std::optional<std::string>(
                json.at("error_message").get<std::string>())
          : std::nullopt;
  require_streaming_delta_shape(delta);
}

void to_json(Json& json, const WebAssistantMessageEvent& event) {
  json = Json{{"type", event.type}};
  if (event.content_index.has_value()) {
    json["contentIndex"] = *event.content_index;
  }
  if (event.delta.has_value()) {
    json["delta"] = *event.delta;
  }
  if (event.content.has_value()) {
    json["content"] = *event.content;
  }
  if (!event.partial.is_null()) {
    json["partial"] = event.partial;
  }
  if (!event.tool_call.is_null()) {
    json["toolCall"] = event.tool_call;
  }
  if (event.reason.has_value()) {
    json["reason"] = *event.reason;
  }
  if (!event.message.is_null()) {
    json["message"] = event.message;
  }
  if (!event.error.is_null()) {
    json["error"] = event.error;
  }
}

void from_json(const Json& json, WebAssistantMessageEvent& event) {
  event.type = json.at("type").get<std::string>();
  event.content_index =
      json.contains("contentIndex") && !json.at("contentIndex").is_null()
          ? std::optional<int>(json.at("contentIndex").get<int>())
          : std::nullopt;
  event.delta =
      json.contains("delta") && !json.at("delta").is_null()
          ? std::optional<std::string>(json.at("delta").get<std::string>())
          : std::nullopt;
  event.content =
      json.contains("content") && !json.at("content").is_null()
          ? std::optional<std::string>(json.at("content").get<std::string>())
          : std::nullopt;
  event.partial = json.contains("partial") ? json.at("partial") : Json(nullptr);
  event.tool_call =
      json.contains("toolCall") ? json.at("toolCall") : Json(nullptr);
  event.reason =
      json.contains("reason") && !json.at("reason").is_null()
          ? std::optional<std::string>(json.at("reason").get<std::string>())
          : std::nullopt;
  event.message = json.contains("message") ? json.at("message") : Json(nullptr);
  event.error = json.contains("error") ? json.at("error") : Json(nullptr);
  require_web_assistant_message_event_shape(event);
}

void to_json(Json& json, const StreamingStateSync& sync) {
  json = Json{
      {"isStreaming", sync.is_streaming},
      {"pendingToolCalls", sync.pending_tool_calls},
  };
  if (sync.system_prompt.has_value()) {
    json["systemPrompt"] = *sync.system_prompt;
  }
  if (!sync.model.is_null()) {
    json["model"] = sync.model;
  }
  if (sync.thinking_level.has_value()) {
    json["thinkingLevel"] = *sync.thinking_level;
  }
  if (sync.messages.is_array() && !sync.messages.empty()) {
    json["messages"] = sync.messages;
  }
  if (sync.tools.is_array() && !sync.tools.empty()) {
    json["tools"] = sync.tools;
  }
  if (sync.streaming_message.has_value()) {
    json["streamingMessage"] = *sync.streaming_message;
  }
  if (sync.error_message.has_value()) {
    json["errorMessage"] = *sync.error_message;
  }
}

void from_json(const Json& json, StreamingStateSync& sync) {
  sync.system_prompt =
      json.contains("systemPrompt") && !json.at("systemPrompt").is_null()
          ? std::optional<std::string>(
                json.at("systemPrompt").get<std::string>())
          : std::nullopt;
  sync.model = json.contains("model") ? json.at("model") : Json(nullptr);
  if (json.contains("thinkingLevel") && !json.at("thinkingLevel").is_null()) {
    sync.thinking_level = json.at("thinkingLevel").get<std::string>();
  } else if (json.contains("thinking_level") &&
             !json.at("thinking_level").is_null()) {
    sync.thinking_level = json.at("thinking_level").get<std::string>();
  } else {
    sync.thinking_level.reset();
  }
  sync.messages = json.contains("messages") ? json.at("messages") : Json::array();
  sync.tools = json.contains("tools") ? json.at("tools") : Json::array();
  if (json.contains("isStreaming")) {
    sync.is_streaming = json.value("isStreaming", false);
  } else {
    sync.is_streaming = json.value("is_streaming", false);
  }
  const Json streaming_message =
      optional_json_alias(json, "streamingMessage", "streaming_message");
  sync.streaming_message =
      !streaming_message.is_null()
          ? std::optional<WebMessage>(streaming_message.get<WebMessage>())
          : std::nullopt;
  if (json.contains("pendingToolCalls")) {
    sync.pending_tool_calls =
        json.value("pendingToolCalls", std::vector<std::string>{});
  } else {
    sync.pending_tool_calls =
        json.value("pending_tool_calls", std::vector<std::string>{});
  }
  if (json.contains("errorMessage") && !json.at("errorMessage").is_null()) {
    sync.error_message = json.at("errorMessage").get<std::string>();
  } else if (json.contains("error_message") &&
             !json.at("error_message").is_null()) {
    sync.error_message = json.at("error_message").get<std::string>();
  } else {
    sync.error_message.reset();
  }
  require_streaming_state_sync_shape(sync);
}

void to_json(Json& json, const WebAgentEvent& event) {
  json = Json{{"type", event.type}};
  if (!event.message.is_null()) {
    json["message"] = event.message;
  }
  if (event.messages.is_array() && !event.messages.empty()) {
    json["messages"] = event.messages;
  }
  if (!event.assistant_message_event.is_null()) {
    json["assistantMessageEvent"] = event.assistant_message_event;
  }
  if (event.tool_results.is_array() && !event.tool_results.empty()) {
    json["toolResults"] = event.tool_results;
  }
  if (event.type == "queue_update") {
    json["steer"] = event.steer;
    json["followUp"] = event.follow_up;
    json["nextTurn"] = event.next_turn;
  }
  if (event.extension_path.has_value()) {
    json["extensionPath"] = *event.extension_path;
  }
  if (event.extension_event.has_value()) {
    json["event"] = *event.extension_event;
  }
  if (event.error.has_value()) {
    json["error"] = *event.error;
  }
  if (event.tool_call_id.has_value()) {
    json["toolCallId"] = *event.tool_call_id;
  }
  if (event.tool_name.has_value()) {
    json["toolName"] = *event.tool_name;
  }
  if (!event.args.is_null()) {
    json["args"] = event.args;
  }
  if (!event.partial_result.is_null()) {
    json["partialResult"] = event.partial_result;
  }
  if (!event.result.is_null()) {
    json["result"] = event.result;
  }
  if (event.is_error.has_value()) {
    json["isError"] = *event.is_error;
  }
  if (event.state.has_value()) {
    json["state"] = *event.state;
  }
}

void from_json(const Json& json, WebAgentEvent& event) {
  event.type = json.at("type").get<std::string>();
  event.message = json.contains("message") ? json.at("message") : Json(nullptr);
  event.messages = json.contains("messages") ? json.at("messages") : Json::array();
  event.assistant_message_event =
      json.contains("assistantMessageEvent") ? json.at("assistantMessageEvent")
                                             : Json(nullptr);
  event.tool_results =
      json.contains("toolResults") ? json.at("toolResults") : Json::array();
  if (json.contains("steer")) {
    event.steer = json.at("steer");
  } else if (json.contains("steering")) {
    event.steer = json.at("steering");
  } else {
    event.steer = Json::array();
  }
  event.follow_up =
      json.contains("followUp") ? json.at("followUp") : Json::array();
  event.next_turn =
      json.contains("nextTurn") ? json.at("nextTurn") : Json::array();
  event.extension_path =
      json.contains("extensionPath") && !json.at("extensionPath").is_null()
          ? std::optional<std::string>(
                json.at("extensionPath").get<std::string>())
          : std::nullopt;
  event.extension_event =
      json.contains("event") && !json.at("event").is_null()
          ? std::optional<std::string>(json.at("event").get<std::string>())
          : std::nullopt;
  event.error = json.contains("error") && !json.at("error").is_null()
                    ? std::optional<std::string>(
                          json.at("error").get<std::string>())
                    : std::nullopt;
  event.tool_call_id =
      json.contains("toolCallId") && !json.at("toolCallId").is_null()
          ? std::optional<std::string>(json.at("toolCallId").get<std::string>())
          : std::nullopt;
  event.tool_name =
      json.contains("toolName") && !json.at("toolName").is_null()
          ? std::optional<std::string>(json.at("toolName").get<std::string>())
          : std::nullopt;
  event.args = json.contains("args") ? json.at("args") : Json(nullptr);
  event.partial_result =
      json.contains("partialResult") ? json.at("partialResult") : Json(nullptr);
  event.result = json.contains("result") ? json.at("result") : Json(nullptr);
  event.is_error = json.contains("isError") && !json.at("isError").is_null()
                       ? std::optional<bool>(json.at("isError").get<bool>())
                       : std::nullopt;
  event.state = json.contains("state") && !json.at("state").is_null()
                    ? std::optional<StreamingStateSync>(
                          json.at("state").get<StreamingStateSync>())
                    : std::nullopt;
  require_web_agent_event_shape(event);
}

void to_json(Json& json, const ModelStateChange& change) {
  json = Json{
      {"type", "model_select"},
      {"model", change.model},
      {"source", change.source},
  };
  if (!change.previous_model.is_null()) {
    json["previousModel"] = change.previous_model;
  }
}

void from_json(const Json& json, ModelStateChange& change) {
  const auto type = json.value("type", std::string("model_select"));
  if (type != "model_select") {
    throw std::runtime_error("model state change type must be model_select");
  }
  change.model = normalized_object(json.at("model"));
  if (json.contains("previousModel")) {
    change.previous_model = json.at("previousModel");
  } else if (json.contains("previous_model")) {
    change.previous_model = json.at("previous_model");
  } else {
    change.previous_model = Json(nullptr);
  }
  change.source = json.value("source", std::string("set"));
  if (!is_one_of(change.source, {"set", "restore", "cycle"})) {
    throw std::runtime_error("model state change source is not supported");
  }
}

void to_json(Json& json, const ThinkingStateChange& change) {
  json = Json{{"type", "thinking_level_select"}, {"level", change.thinking_level}};
  if (change.previous_thinking_level.has_value()) {
    json["previousLevel"] = *change.previous_thinking_level;
  }
}

void from_json(const Json& json, ThinkingStateChange& change) {
  const auto type = json.value("type", std::string("thinking_level_select"));
  if (type != "thinking_level_select") {
    throw std::runtime_error(
        "thinking state change type must be thinking_level_select");
  }
  if (json.contains("level") && !json.at("level").is_null()) {
    change.thinking_level = json.at("level").get<std::string>();
  } else {
    change.thinking_level = json.at("thinking_level").get<std::string>();
  }
  if (!is_supported_thinking_level(change.thinking_level)) {
    throw std::runtime_error("thinking state change level is not supported");
  }
  if (json.contains("previousLevel") && !json.at("previousLevel").is_null()) {
    change.previous_thinking_level =
        json.at("previousLevel").get<std::string>();
  } else if (json.contains("previous_thinking_level") &&
             !json.at("previous_thinking_level").is_null()) {
    change.previous_thinking_level =
        json.at("previous_thinking_level").get<std::string>();
  } else {
    change.previous_thinking_level.reset();
  }
  if (change.previous_thinking_level.has_value() &&
      !is_supported_thinking_level(*change.previous_thinking_level)) {
    throw std::runtime_error(
        "thinking state change previous level is not supported");
  }
}

void to_json(Json& json, const WebRuntimeSyncEvent& event) {
  json = Json{{"type", event.type}};
  if (event.session_id.has_value()) {
    json["sessionId"] = *event.session_id;
  }
  if (event.request_id.has_value()) {
    json["requestId"] = *event.request_id;
  }
  if (event.session_command.has_value()) {
    json["sessionCommand"] = *event.session_command;
  }
  if (event.session_result.has_value()) {
    json["sessionResult"] = *event.session_result;
  }
  if (!event.message.is_null()) {
    json["message"] = event.message;
  }
  if (event.messages.is_array() && !event.messages.empty()) {
    json["messages"] = event.messages;
  }
  if (!event.assistant_message_event.is_null()) {
    json["assistantMessageEvent"] = event.assistant_message_event;
  }
  if (event.tool_results.is_array() && !event.tool_results.empty()) {
    json["toolResults"] = event.tool_results;
  }
  if (event.tool_call_id.has_value()) {
    json["toolCallId"] = *event.tool_call_id;
  }
  if (event.tool_name.has_value()) {
    json["toolName"] = *event.tool_name;
  }
  if (!event.args.is_null()) {
    json["args"] = event.args;
  }
  if (!event.partial_result.is_null()) {
    json["partialResult"] = event.partial_result;
  }
  if (!event.result.is_null()) {
    json["result"] = event.result;
  }
  if (event.is_error.has_value()) {
    json["isError"] = *event.is_error;
  }
  if (!event.model.is_null()) {
    json["model"] = event.model;
  }
  if (!event.previous_model.is_null()) {
    json["previousModel"] = event.previous_model;
  }
  if (event.thinking_level.has_value()) {
    json["level"] = *event.thinking_level;
  }
  if (event.previous_thinking_level.has_value()) {
    json["previousLevel"] = *event.previous_thinking_level;
  }
  if (event.type == "queue_update") {
    json["steer"] = event.steer;
    json["followUp"] = event.follow_up;
    json["nextTurn"] = event.next_turn;
  }
  if (event.reason.has_value()) {
    json["reason"] = *event.reason;
  }
  if (event.aborted.has_value()) {
    json["aborted"] = *event.aborted;
  }
  if (event.will_retry.has_value()) {
    json["willRetry"] = *event.will_retry;
  }
  if (event.error_message.has_value()) {
    json["errorMessage"] = *event.error_message;
  }
  if (!event.data.is_null()) {
    json["data"] = event.data;
  }
  if (event.state.has_value()) {
    json["state"] = *event.state;
  }
}

void from_json(const Json& json, WebRuntimeSyncEvent& event) {
  event.type = json.at("type").get<std::string>();
  event.session_id =
      json.contains("sessionId") && !json.at("sessionId").is_null()
          ? std::optional<std::string>(json.at("sessionId").get<std::string>())
          : std::nullopt;
  event.request_id =
      json.contains("requestId") && !json.at("requestId").is_null()
          ? std::optional<std::string>(json.at("requestId").get<std::string>())
          : std::nullopt;
  event.session_command =
      json.contains("sessionCommand") && !json.at("sessionCommand").is_null()
          ? std::optional<BrowserSessionMutationCommand>(
                json.at("sessionCommand").get<BrowserSessionMutationCommand>())
          : std::nullopt;
  event.session_result =
      json.contains("sessionResult") && !json.at("sessionResult").is_null()
          ? std::optional<BrowserSessionMutationResult>(
                json.at("sessionResult").get<BrowserSessionMutationResult>())
          : std::nullopt;
  event.message = json.contains("message") ? json.at("message") : Json(nullptr);
  event.messages = json.contains("messages") ? json.at("messages") : Json::array();
  event.assistant_message_event =
      json.contains("assistantMessageEvent") ? json.at("assistantMessageEvent")
                                             : Json(nullptr);
  event.tool_results =
      json.contains("toolResults") ? json.at("toolResults") : Json::array();
  event.tool_call_id =
      json.contains("toolCallId") && !json.at("toolCallId").is_null()
          ? std::optional<std::string>(json.at("toolCallId").get<std::string>())
          : std::nullopt;
  event.tool_name =
      json.contains("toolName") && !json.at("toolName").is_null()
          ? std::optional<std::string>(json.at("toolName").get<std::string>())
          : std::nullopt;
  event.args = json.contains("args") ? json.at("args") : Json(nullptr);
  event.partial_result =
      json.contains("partialResult") ? json.at("partialResult") : Json(nullptr);
  event.result = json.contains("result") ? json.at("result") : Json(nullptr);
  event.is_error = json.contains("isError") && !json.at("isError").is_null()
                       ? std::optional<bool>(json.at("isError").get<bool>())
                       : std::nullopt;
  event.model = json.contains("model") ? json.at("model") : Json(nullptr);
  event.previous_model =
      json.contains("previousModel") ? json.at("previousModel") : Json(nullptr);
  if (json.contains("level") && !json.at("level").is_null()) {
    event.thinking_level = json.at("level").get<std::string>();
  } else if (json.contains("thinkingLevel") &&
             !json.at("thinkingLevel").is_null()) {
    event.thinking_level = json.at("thinkingLevel").get<std::string>();
  } else {
    event.thinking_level.reset();
  }
  event.previous_thinking_level =
      json.contains("previousLevel") && !json.at("previousLevel").is_null()
          ? std::optional<std::string>(
                json.at("previousLevel").get<std::string>())
          : std::nullopt;
  if (json.contains("steer")) {
    event.steer = json.at("steer");
  } else if (json.contains("steering")) {
    event.steer = json.at("steering");
  } else {
    event.steer = Json::array();
  }
  event.follow_up =
      json.contains("followUp") ? json.at("followUp") : Json::array();
  event.next_turn =
      json.contains("nextTurn") ? json.at("nextTurn") : Json::array();
  event.reason = json.contains("reason") && !json.at("reason").is_null()
                     ? std::optional<std::string>(
                           json.at("reason").get<std::string>())
                     : std::nullopt;
  event.aborted = json.contains("aborted") && !json.at("aborted").is_null()
                      ? std::optional<bool>(json.at("aborted").get<bool>())
                      : std::nullopt;
  event.will_retry =
      json.contains("willRetry") && !json.at("willRetry").is_null()
          ? std::optional<bool>(json.at("willRetry").get<bool>())
          : std::nullopt;
  event.error_message =
      json.contains("errorMessage") && !json.at("errorMessage").is_null()
          ? std::optional<std::string>(
                json.at("errorMessage").get<std::string>())
          : std::nullopt;
  event.data = json.contains("data") ? json.at("data") : Json(nullptr);
  event.state = json.contains("state") && !json.at("state").is_null()
                    ? std::optional<StreamingStateSync>(
                          json.at("state").get<StreamingStateSync>())
                    : std::nullopt;
  require_web_runtime_sync_event_shape(event);
}

void to_json(Json& json, const RuntimeMessageEnvelope& envelope) {
  json = normalized_object(envelope.payload);
  json["sandboxId"] = envelope.sandbox_id;
  if (envelope.message_id.has_value()) {
    json["messageId"] = *envelope.message_id;
  }
  if (envelope.type.has_value()) {
    json["type"] = *envelope.type;
  }
}

void from_json(const Json& json, RuntimeMessageEnvelope& envelope) {
  envelope.sandbox_id = json.at("sandboxId").get<std::string>();
  envelope.message_id =
      json.contains("messageId") && !json.at("messageId").is_null()
          ? std::optional<std::string>(json.at("messageId").get<std::string>())
          : std::nullopt;
  envelope.type = json.contains("type") && !json.at("type").is_null()
                      ? std::optional<std::string>(json.at("type").get<std::string>())
                      : std::nullopt;
  envelope.payload = payload_without_routing_fields(json);
  if (envelope.sandbox_id.empty()) {
    throw std::runtime_error("runtime message sandboxId must not be empty");
  }
  if (envelope.message_id.has_value() && envelope.message_id->empty()) {
    throw std::runtime_error("runtime message messageId must not be empty");
  }
}

void to_json(Json& json, const RuntimeResponseEnvelope& envelope) {
  json = normalized_object(envelope.payload);
  json["type"] = "runtime-response";
  json["sandboxId"] = envelope.sandbox_id;
  json["messageId"] = envelope.message_id;
  json["success"] = envelope.success;
  if (envelope.error_message.has_value()) {
    json["error"] = *envelope.error_message;
  }
}

void from_json(const Json& json, RuntimeResponseEnvelope& envelope) {
  const auto type = json.value("type", std::string{});
  if (type != "runtime-response") {
    throw std::runtime_error("runtime response type must be runtime-response");
  }
  envelope.sandbox_id = json.at("sandboxId").get<std::string>();
  envelope.message_id = json.at("messageId").get<std::string>();
  envelope.success = json.value("success", false);
  envelope.error_message =
      json.contains("error") && !json.at("error").is_null()
          ? std::optional<std::string>(json.at("error").get<std::string>())
          : std::nullopt;
  envelope.payload = payload_without_routing_fields(json);
  if (envelope.sandbox_id.empty()) {
    throw std::runtime_error("runtime response sandboxId must not be empty");
  }
  if (envelope.message_id.empty()) {
    throw std::runtime_error("runtime response messageId must not be empty");
  }
  if (!envelope.success && (!envelope.error_message.has_value() ||
                            envelope.error_message->empty())) {
    throw std::runtime_error("runtime response error is required on failure");
  }
}

void to_json(Json& json, const WebReturnedFile& file) {
  json = Json{{"type", file.type},
              {"fileName", file.file_name},
              {"content", file.content},
              {"mimeType", file.mime_type}};
}

void from_json(const Json& json, WebReturnedFile& file) {
  file.type = json.value("type", std::string("file-returned"));
  file.file_name = json.at("fileName").get<std::string>();
  file.content = json.contains("content") ? json.at("content") : Json(nullptr);
  file.mime_type = json.at("mimeType").get<std::string>();

  if (file.type != "file-returned") {
    throw std::runtime_error("web returned file type must be file-returned");
  }
  if (file.file_name.empty()) {
    throw std::runtime_error("web returned file fileName must not be empty");
  }
  if (file.content.is_null()) {
    throw std::runtime_error("web returned file content is required");
  }
  if (file.mime_type.empty()) {
    throw std::runtime_error("web returned file mimeType must not be empty");
  }
}

void to_json(Json& json, const WebSandboxAttachment& attachment) {
  json = Json{{"id", attachment.id},
              {"fileName", attachment.file_name},
              {"mimeType", attachment.mime_type},
              {"size", attachment.size},
              {"content", attachment.content}};
  if (attachment.extracted_text.has_value()) {
    json["extractedText"] = *attachment.extracted_text;
  }
}

void from_json(const Json& json, WebSandboxAttachment& attachment) {
  attachment.id = json.at("id").get<std::string>();
  attachment.file_name = json.at("fileName").get<std::string>();
  attachment.mime_type = json.at("mimeType").get<std::string>();
  attachment.size = json.value("size", std::uint64_t{0});
  attachment.content = json.value("content", std::string{});
  attachment.extracted_text =
      json.contains("extractedText") && !json.at("extractedText").is_null()
          ? std::optional<std::string>(
                json.at("extractedText").get<std::string>())
          : std::nullopt;

  if (attachment.id.empty()) {
    throw std::runtime_error("web sandbox attachment id must not be empty");
  }
  if (attachment.file_name.empty()) {
    throw std::runtime_error(
        "web sandbox attachment fileName must not be empty");
  }
  if (attachment.mime_type.empty()) {
    throw std::runtime_error(
        "web sandbox attachment mimeType must not be empty");
  }
}

void to_json(Json& json, const WebSandboxAttachmentSummary& attachment) {
  json = Json{{"id", attachment.id},
              {"fileName", attachment.file_name},
              {"mimeType", attachment.mime_type},
              {"size", attachment.size}};
}

void from_json(const Json& json, WebSandboxAttachmentSummary& attachment) {
  attachment.id = json.at("id").get<std::string>();
  attachment.file_name = json.at("fileName").get<std::string>();
  attachment.mime_type = json.at("mimeType").get<std::string>();
  attachment.size = json.value("size", std::uint64_t{0});

  if (attachment.id.empty()) {
    throw std::runtime_error(
        "web sandbox attachment summary id must not be empty");
  }
  if (attachment.file_name.empty()) {
    throw std::runtime_error(
        "web sandbox attachment summary fileName must not be empty");
  }
  if (attachment.mime_type.empty()) {
    throw std::runtime_error(
        "web sandbox attachment summary mimeType must not be empty");
  }
}

void to_json(Json& json, const WebConsoleMessage& message) {
  json = Json{{"type", message.type},
              {"method", message.method},
              {"text", message.text}};
  if (!message.args.empty()) {
    json["args"] = message.args;
  }
}

void from_json(const Json& json, WebConsoleMessage& message) {
  message.type = json.value("type", std::string("console"));
  message.method = json.at("method").get<std::string>();
  message.text = json.at("text").get<std::string>();
  message.args = json.contains("args") ? json.at("args") : Json::array();

  if (message.type != "console") {
    throw std::runtime_error("web console message type must be console");
  }
  if (!is_one_of(message.method, {"log", "error", "warn", "info"})) {
    throw std::runtime_error("web console message method is not supported");
  }
  if (!message.args.is_array()) {
    throw std::runtime_error("web console message args must be an array");
  }
}

void to_json(Json& json, const WebExecutionCompletionMessage& message) {
  json = Json{{"type", message.type}};
  if (message.type == "execution-error") {
    json["error"] = message.error;
  } else if (message.type == "execution-complete" &&
             !message.return_value.is_null()) {
    json["returnValue"] = message.return_value;
  }
}

void from_json(const Json& json, WebExecutionCompletionMessage& message) {
  message.type = json.at("type").get<std::string>();
  message.error = json.contains("error") ? json.at("error") : Json(nullptr);
  message.return_value =
      json.contains("returnValue") ? json.at("returnValue") : Json(nullptr);

  if (!is_one_of(message.type, {"execution-error", "execution-complete"})) {
    throw std::runtime_error(
        "web execution completion message type is not supported");
  }
  if (message.type == "execution-error") {
    if (!message.error.is_object()) {
      throw std::runtime_error(
          "web execution-error message requires error object");
    }
    if (!message.error.contains("message") ||
        !message.error.at("message").is_string() ||
        message.error.at("message").get<std::string>().empty()) {
      throw std::runtime_error(
          "web execution-error message requires error.message");
    }
    if (!message.error.contains("stack") ||
        !message.error.at("stack").is_string()) {
      throw std::runtime_error(
          "web execution-error message requires error.stack");
    }
  }
}

void to_json(Json& json, const WebSandboxFile& file) {
  json = Json{{"fileName", file.file_name},
              {"content", file.content},
              {"mimeType", file.mime_type}};
}

void from_json(const Json& json, WebSandboxFile& file) {
  file.file_name = json.at("fileName").get<std::string>();
  file.content = json.contains("content") ? json.at("content") : Json(nullptr);
  file.mime_type = json.at("mimeType").get<std::string>();
  if (file.file_name.empty()) {
    throw std::runtime_error("web sandbox file fileName must not be empty");
  }
  if (file.content.is_null()) {
    throw std::runtime_error("web sandbox file content is required");
  }
  if (file.mime_type.empty()) {
    throw std::runtime_error("web sandbox file mimeType must not be empty");
  }
}

void to_json(Json& json, const WebSandboxConsoleLog& log) {
  json = Json{{"type", log.type}, {"text", log.text}};
  if (!log.args.empty()) {
    json["args"] = log.args;
  }
}

void from_json(const Json& json, WebSandboxConsoleLog& log) {
  log.type = json.at("type").get<std::string>();
  log.text = json.at("text").get<std::string>();
  log.args = json.contains("args") ? json.at("args") : Json::array();
  if (!is_one_of(log.type, {"log", "warn", "error", "info"})) {
    throw std::runtime_error("web sandbox console log type is not supported");
  }
  if (!log.args.is_array()) {
    throw std::runtime_error("web sandbox console log args must be an array");
  }
}

void to_json(Json& json, const WebSandboxResult& result) {
  json = Json{{"success", result.success}, {"console", result.console}};
  if (!result.files.empty()) {
    json["files"] = result.files;
  }
  if (!result.error.is_null()) {
    json["error"] = result.error;
  }
  if (!result.return_value.is_null()) {
    json["returnValue"] = result.return_value;
  }
}

void from_json(const Json& json, WebSandboxResult& result) {
  result.success = json.at("success").get<bool>();
  result.console = json.contains("console") && !json.at("console").is_null()
                       ? json.at("console")
                             .get<std::vector<WebSandboxConsoleLog>>()
                       : std::vector<WebSandboxConsoleLog>{};
  result.files = json.contains("files") && !json.at("files").is_null()
                     ? json.at("files").get<std::vector<WebSandboxFile>>()
                     : std::vector<WebSandboxFile>{};
  result.error = json.contains("error") ? json.at("error") : Json(nullptr);
  result.return_value =
      json.contains("returnValue") ? json.at("returnValue") : Json(nullptr);

  if (!result.success) {
    if (!result.error.is_object()) {
      throw std::runtime_error("web sandbox failed result requires error object");
    }
    if (!result.error.contains("message") ||
        !result.error.at("message").is_string() ||
        result.error.at("message").get<std::string>().empty()) {
      throw std::runtime_error("web sandbox failed result requires error.message");
    }
    if (!result.error.contains("stack") ||
        !result.error.at("stack").is_string()) {
      throw std::runtime_error("web sandbox failed result requires error.stack");
    }
  }
}

void to_json(Json& json, const WebJavaScriptReplFile& file) {
  json = Json{{"fileName", file.file_name},
              {"mimeType", file.mime_type},
              {"size", file.size},
              {"contentBase64", file.content_base64}};
}

void from_json(const Json& json, WebJavaScriptReplFile& file) {
  file.file_name = json.value("fileName", std::string("file"));
  file.mime_type = json.value("mimeType", std::string("application/octet-stream"));
  file.size = json.value("size", std::uint64_t{0});
  file.content_base64 = json.at("contentBase64").get<std::string>();

  if (file.file_name.empty()) {
    throw std::runtime_error("web javascript repl file fileName must not be empty");
  }
  if (file.mime_type.empty()) {
    throw std::runtime_error("web javascript repl file mimeType must not be empty");
  }
  if (file.content_base64.empty() && file.size > 0) {
    throw std::runtime_error(
        "web javascript repl file contentBase64 must not be empty when size is non-zero");
  }
}

void to_json(Json& json, const WebJavaScriptReplParams& params) {
  json = Json{{"title", params.title}, {"code", params.code}};
}

void from_json(const Json& json, WebJavaScriptReplParams& params) {
  params.title = json.at("title").get<std::string>();
  params.code = json.at("code").get<std::string>();

  if (params.title.empty()) {
    throw std::runtime_error("web javascript repl params title must not be empty");
  }
  if (params.code.empty()) {
    throw std::runtime_error("web javascript repl params code must not be empty");
  }
}

void to_json(Json& json, const WebJavaScriptReplResult& result) {
  json = Json{{"content", result.content},
              {"details", Json{{"files", result.files}}}};
}

void from_json(const Json& json, WebJavaScriptReplResult& result) {
  result.content = json.at("content").get<std::vector<WebContentBlock>>();
  const Json details =
      json.contains("details") && !json.at("details").is_null()
          ? json.at("details")
          : Json::object();
  result.files = details.contains("files") && !details.at("files").is_null()
                     ? details.at("files")
                           .get<std::vector<WebJavaScriptReplFile>>()
                     : std::vector<WebJavaScriptReplFile>{};

  if (result.content.empty()) {
    throw std::runtime_error("web javascript repl result content must not be empty");
  }
  for (const WebContentBlock& block : result.content) {
    if (block.type != "text") {
      throw std::runtime_error(
          "web javascript repl result content must contain text blocks");
    }
  }
}

void to_json(Json& json, const WebExtractDocumentParams& params) {
  json = Json{{"url", params.url}};
}

void from_json(const Json& json, WebExtractDocumentParams& params) {
  params.url = json.at("url").get<std::string>();
  if (params.url.empty()) {
    throw std::runtime_error("web extract document params url must not be empty");
  }
}

void to_json(Json& json, const WebExtractDocumentDetails& details) {
  json = Json{{"extractedText", details.extracted_text},
              {"format", details.format},
              {"fileName", details.file_name},
              {"size", details.size}};
}

void from_json(const Json& json, WebExtractDocumentDetails& details) {
  details.extracted_text = json.at("extractedText").get<std::string>();
  details.format = json.at("format").get<std::string>();
  details.file_name = json.at("fileName").get<std::string>();
  details.size = json.value("size", std::uint64_t{0});

  if (details.extracted_text.empty()) {
    throw std::runtime_error(
        "web extract document details extractedText must not be empty");
  }
  if (!is_one_of(details.format, {"pdf", "docx", "xlsx", "pptx", "unknown"})) {
    throw std::runtime_error(
        "web extract document details format is not supported");
  }
  if (details.file_name.empty()) {
    throw std::runtime_error(
        "web extract document details fileName must not be empty");
  }
}

void to_json(Json& json, const WebExtractDocumentResult& result) {
  json = Json{{"content", result.content}, {"details", result.details}};
}

void from_json(const Json& json, WebExtractDocumentResult& result) {
  result.content = json.at("content").get<std::vector<WebContentBlock>>();
  result.details = json.at("details").get<WebExtractDocumentDetails>();

  if (result.content.empty()) {
    throw std::runtime_error("web extract document result content must not be empty");
  }
  for (const WebContentBlock& block : result.content) {
    if (block.type != "text") {
      throw std::runtime_error(
          "web extract document result content must contain text blocks");
    }
  }
}

void to_json(Json& json, const WebSandboxControlMessage& message) {
  json = Json{{"type", message.type}};
  if (message.sandbox_id.has_value()) {
    json["sandboxId"] = *message.sandbox_id;
  }
  if (message.code.has_value()) {
    json["code"] = *message.code;
  }
  if (message.error.has_value()) {
    json["error"] = *message.error;
  }
  if (message.stack.has_value()) {
    json["stack"] = *message.stack;
  }
  if (message.url.has_value()) {
    json["url"] = *message.url;
  }
}

void from_json(const Json& json, WebSandboxControlMessage& message) {
  message.type = json.at("type").get<std::string>();
  message.sandbox_id =
      json.contains("sandboxId") && !json.at("sandboxId").is_null()
          ? std::optional<std::string>(json.at("sandboxId").get<std::string>())
          : std::nullopt;
  message.code = json.contains("code") && !json.at("code").is_null()
                     ? std::optional<std::string>(
                           json.at("code").get<std::string>())
                     : std::nullopt;
  message.error = json.contains("error") && !json.at("error").is_null()
                      ? std::optional<std::string>(
                            json.at("error").get<std::string>())
                      : std::nullopt;
  message.stack = json.contains("stack") && !json.at("stack").is_null()
                      ? std::optional<std::string>(
                            json.at("stack").get<std::string>())
                      : std::nullopt;
  message.url = json.contains("url") && !json.at("url").is_null()
                    ? std::optional<std::string>(
                          json.at("url").get<std::string>())
                    : std::nullopt;

  if (!is_one_of(message.type, {"sandbox-ready", "sandbox-load",
                                "sandbox-error", "open-external-url"})) {
    throw std::runtime_error("web sandbox control message type is not supported");
  }
  if (message.type == "sandbox-load") {
    if (!message.sandbox_id.has_value() || message.sandbox_id->empty()) {
      throw std::runtime_error(
          "web sandbox-load message requires sandboxId");
    }
    if (!message.code.has_value() || message.code->empty()) {
      throw std::runtime_error("web sandbox-load message requires code");
    }
  }
  if (message.type == "sandbox-error") {
    if (!message.error.has_value() || message.error->empty()) {
      throw std::runtime_error(
          "web sandbox-error message requires error");
    }
    if (!message.stack.has_value()) {
      throw std::runtime_error("web sandbox-error message requires stack");
    }
  }
  if (message.type == "open-external-url" &&
      (!message.url.has_value() || message.url->empty())) {
    throw std::runtime_error(
        "web open-external-url message requires url");
  }
}

void to_json(Json& json, const TransportEnvelope& envelope) {
  json = Json{
      {"jsonrpc", envelope.jsonrpc},
      {"method", envelope.method},
      {"params", envelope.params},
      {"id", envelope.id},
      {"meta", envelope.meta},
  };
}

void from_json(const Json& json, TransportEnvelope& envelope) {
  envelope.jsonrpc = json.value("jsonrpc", std::string("2.0"));
  if (envelope.jsonrpc != "2.0") {
    throw std::runtime_error("transport envelope jsonrpc must equal '2.0'");
  }

  envelope.method = json.at("method").get<std::string>();
  if (envelope.method.empty()) {
    throw std::runtime_error("transport envelope method must not be empty");
  }

  envelope.params = normalized_object(json.value("params", Json::object()));
  envelope.id = json.contains("id") ? json.at("id") : Json(nullptr);
  envelope.meta = normalized_object(json.value("meta", Json::object()));
}

void to_json(Json& json, const SessionTranscriptSnapshot& snapshot) {
  json = Json{
      {"session", snapshot.session},
      {"messages", snapshot.messages},
  };
}

void from_json(const Json& json, SessionTranscriptSnapshot& snapshot) {
  snapshot.session = json.at("session").get<SessionSummary>();
  snapshot.messages = json.value("messages", std::vector<MessagePreview>{});
}

}  // namespace qt::web_core
