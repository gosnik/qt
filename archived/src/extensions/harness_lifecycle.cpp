#include <qt/extensions/harness_lifecycle.hpp>

#include <limits>
#include <cassert>
#include <iterator>
#include <string>
#include <utility>
#include <variant>

namespace qt::extensions {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

struct GenericEventRoutingPlan {
  JsonRpcRequest request;
};

Json make_request_id(std::size_t request_id) {
  return "extension-host-" + std::to_string(request_id);
}

Result<std::int64_t> checked_index(std::uint64_t value, const char* context) {
  if (value > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
    return Result<std::int64_t>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " exceeds signed integer range");
  }
  return static_cast<std::int64_t>(value);
}

std::string fallback_tool_call_id(std::uint64_t turn_index,
                                  std::size_t tool_call_index) {
  return "turn-" + std::to_string(turn_index) + "-tool-" +
         std::to_string(tool_call_index);
}

std::string tool_call_id(const qt::harness::AgentHarness::ToolCallEvent& event) {
  if (event.request.call_id.has_value() && !event.request.call_id->empty()) {
    return *event.request.call_id;
  }
  return fallback_tool_call_id(event.turn_index, event.tool_call_index);
}

std::string tool_call_id(
    const qt::harness::AgentHarness::ToolResultEvent& event) {
  if (event.request.call_id.has_value() && !event.request.call_id->empty()) {
    return *event.request.call_id;
  }
  if (event.result.call_id.has_value() && !event.result.call_id->empty()) {
    return *event.result.call_id;
  }
  return fallback_tool_call_id(event.turn_index, event.tool_call_index);
}

Json text_content(std::string text) {
  return Json{{"type", "text"}, {"text", std::move(text)}};
}

Json messages_json(const std::vector<qt::ai::Message>& messages) {
  Json result = Json::array();
  for (const auto& message : messages) {
    result.push_back(Json(message));
  }
  return result;
}

Json session_entries_json(const std::vector<qt::harness::SessionEntry>& entries) {
  Json result = Json::array();
  for (const auto& entry : entries) {
    result.push_back(Json(entry));
  }
  return result;
}

Json compaction_preparation_json(
    const qt::harness::ModelBackedCompactionPlan& preparation) {
  Json result{{"request", Json(preparation.request)},
              {"firstKeptEntryId", preparation.first_kept_entry_id},
              {"firstKeptEntryIndex", preparation.first_kept_entry_index},
              {"tokensBefore", preparation.tokens_before},
              {"summarizedTokens", preparation.summarized_tokens},
              {"turnPrefixTokens", preparation.turn_prefix_tokens},
              {"keptTokens", preparation.kept_tokens},
              {"isSplitTurn", preparation.is_split_turn},
              {"details", preparation.details}};
  if (preparation.previous_summary.has_value()) {
    result["previousSummary"] = *preparation.previous_summary;
  }
  if (preparation.turn_prefix_request.has_value()) {
    result["turnPrefixRequest"] = Json(*preparation.turn_prefix_request);
  }
  return result;
}

Json compaction_entry_json(
    const qt::harness::AgentHarness::CompactEvent& event) {
  Json result{{"type", "compaction"},
              {"id", event.entry_id},
              {"summary", event.compaction.summary},
              {"firstKeptEntryId", event.compaction.first_kept_entry_id},
              {"tokensBefore", event.compaction.tokens_before}};
  if (event.compaction.details.has_value()) {
    result["details"] = *event.compaction.details;
  }
  return result;
}

Json skill_json(const qt::harness::LoadedSkillRecord& skill) {
  return Json{{"root", skill.root},
              {"directory", skill.directory},
              {"path", skill.path},
              {"name", skill.name},
              {"description", skill.description},
              {"content", skill.content},
              {"disableModelInvocation", skill.disable_model_invocation}};
}

Json prompt_template_json(
    const qt::harness::LoadedPromptTemplateRecord& prompt) {
  Json result{{"root", prompt.root},
              {"path", prompt.path},
              {"name", prompt.name},
              {"description", prompt.description},
              {"content", prompt.content}};
  if (prompt.argument_hint.has_value()) {
    result["argumentHint"] = *prompt.argument_hint;
  }
  return result;
}

const char* context_kind_json(qt::harness::ContextFileKind kind) {
  switch (kind) {
    case qt::harness::ContextFileKind::kAgents:
      return "AGENTS.md";
    case qt::harness::ContextFileKind::kClaude:
      return "CLAUDE.md";
  }
  return "context";
}

const char* context_scope_json(qt::harness::ContextFileScope scope) {
  switch (scope) {
    case qt::harness::ContextFileScope::kGlobal:
      return "global";
    case qt::harness::ContextFileScope::kAncestor:
      return "ancestor";
    case qt::harness::ContextFileScope::kCurrentDirectory:
      return "current";
  }
  return "unknown";
}

Json instruction_load_event_json(
    const qt::harness::InstructionLoadEvent& event) {
  return Json{{"path", event.path},
              {"directory", event.directory},
              {"kind", context_kind_json(event.kind)},
              {"scope", context_scope_json(event.scope)},
              {"contentBytes", event.content_bytes},
              {"reason", event.reason}};
}

Json resources_json(const qt::harness::AgentHarnessResources& resources) {
  Json skills = Json::array();
  for (const auto& skill : resources.skills) {
    skills.push_back(skill_json(skill));
  }

  Json prompt_templates = Json::array();
  for (const auto& prompt : resources.prompt_templates) {
    prompt_templates.push_back(prompt_template_json(prompt));
  }

  return Json{{"skills", std::move(skills)},
              {"promptTemplates", std::move(prompt_templates)}};
}

Result<ExtensionSessionStartReason> parse_harness_session_start_reason(
    std::string_view reason) {
  if (reason == "startup") {
    return ExtensionSessionStartReason::kStartup;
  }
  if (reason == "reload") {
    return ExtensionSessionStartReason::kReload;
  }
  if (reason == "new") {
    return ExtensionSessionStartReason::kNew;
  }
  if (reason == "resume") {
    return ExtensionSessionStartReason::kResume;
  }
  if (reason == "fork") {
    return ExtensionSessionStartReason::kFork;
  }
  return Result<ExtensionSessionStartReason>::failure(
      ErrorCode::invalid_argument,
      "extension session_start reason must be startup, reload, new, resume, "
      "or fork");
}

std::string prompt_text(const qt::ai::Message& prompt) {
  std::string text;
  for (const auto& content : prompt.content) {
    const auto* text_content = std::get_if<qt::ai::TextContent>(&content);
    if (!text_content) {
      continue;
    }
    if (!text.empty()) {
      text.push_back('\n');
    }
    text += text_content->text;
  }
  return text;
}

Json prompt_images_json(const qt::ai::Message& prompt) {
  Json images = Json::array();
  for (const auto& content : prompt.content) {
    const auto* image = std::get_if<qt::ai::ImageContent>(&content);
    if (!image) {
      continue;
    }
    images.push_back(Json(*image));
  }
  return images.empty() ? Json(nullptr) : images;
}

Json tool_result_content(const qt::tools::ToolResult& result) {
  if (result.output.is_array()) {
    return result.output;
  }
  if (result.output.is_string()) {
    return Json::array({text_content(result.output.get<std::string>())});
  }
  if (result.error_message.has_value() && !result.error_message->empty()) {
    return Json::array({text_content(*result.error_message)});
  }
  return Json::array({text_content(result.output.dump())});
}

bool tool_result_is_error(const qt::tools::ToolResult& result) {
  return result.status != qt::tools::ToolResultStatus::success;
}

Json tool_result_message_json(
    const qt::harness::AgentHarness::ToolResultEvent& event,
    std::int64_t timestamp) {
  return Json{{"role", "toolResult"},
              {"timestamp", timestamp},
              {"toolCallId", tool_call_id(event)},
              {"toolName", event.request.tool_name},
              {"content", tool_result_content(event.result)},
              {"details", event.result.output},
              {"isError", tool_result_is_error(event.result)}};
}

const char* extension_role(qt::ai::MessageRole role) {
  switch (role) {
    case qt::ai::MessageRole::system:
      return "system";
    case qt::ai::MessageRole::developer:
      return "developer";
    case qt::ai::MessageRole::user:
      return "user";
    case qt::ai::MessageRole::assistant:
      return "assistant";
    case qt::ai::MessageRole::tool:
      return "toolResult";
  }
  return "user";
}

Json message_json(const qt::ai::Message& message, std::int64_t timestamp) {
  Json json = message;
  json["role"] = extension_role(message.role);
  json["timestamp"] = timestamp;

  if (message.role == qt::ai::MessageRole::tool) {
    for (const auto& content : message.content) {
      const auto* tool_result =
          std::get_if<qt::ai::ToolResultContent>(&content);
      if (!tool_result) {
        continue;
      }
      json["toolCallId"] = tool_result->tool_call_id;
      json["toolName"] = tool_result->name;
      json["content"] = Json::array({tool_result->result});
      json["details"] = tool_result->result;
      json["isError"] = tool_result->is_error;
      break;
    }
  }

  return json;
}

Json lifecycle_messages_json(const std::vector<qt::ai::Message>& messages,
                             std::int64_t timestamp) {
  Json result = Json::array();
  for (const auto& message : messages) {
    result.push_back(message_json(message, timestamp));
  }
  return result;
}

std::optional<std::string> stop_reason_json(qt::ai::StopReason reason) {
  switch (reason) {
    case qt::ai::StopReason::end_turn:
      return "stop";
    case qt::ai::StopReason::max_tokens:
      return "length";
    case qt::ai::StopReason::tool_call:
      return "toolUse";
    case qt::ai::StopReason::error:
    case qt::ai::StopReason::aborted:
    case qt::ai::StopReason::content_filter:
    case qt::ai::StopReason::unknown:
      return std::nullopt;
  }
  return std::nullopt;
}

Json assistant_update_message(std::int64_t timestamp) {
  return Json{{"role", "assistant"}, {"timestamp", timestamp}};
}

Result<std::optional<ExtensionMessageUpdateEventRecord>>
make_provider_message_update_record(
    const qt::ai::StreamEvent& provider_event, std::int64_t timestamp) {
  if (provider_event.kind == qt::ai::EventKind::content_delta &&
      provider_event.delta.has_value()) {
    const std::string content_type =
        provider_event.metadata.value("content_type", std::string("text"));
    const bool thinking = content_type == "thinking";
    Json event = Json{{"type", thinking ? "thinking_delta" : "text_delta"},
                      {"contentIndex",
                       provider_event.metadata.value("content_index", 0)},
                      {"delta", *provider_event.delta}};
    if (provider_event.metadata.contains("partial")) {
      event["partial"] = provider_event.metadata.at("partial");
    }
    auto record = make_extension_message_update_event_record(
        assistant_update_message(timestamp), std::move(event));
    if (!record) {
      return Result<std::optional<ExtensionMessageUpdateEventRecord>>::failure(
          record.error().code, record.error().message);
    }
    return std::optional<ExtensionMessageUpdateEventRecord>{record.value()};
  }

  if (provider_event.kind == qt::ai::EventKind::message_done &&
      provider_event.stop_reason.has_value()) {
    const auto reason = stop_reason_json(*provider_event.stop_reason);
    if (!reason.has_value()) {
      return std::optional<ExtensionMessageUpdateEventRecord>{};
    }
    auto record = make_extension_message_update_event_record(
        assistant_update_message(timestamp),
        Json{{"type", "done"}, {"reason", *reason}});
    if (!record) {
      return Result<std::optional<ExtensionMessageUpdateEventRecord>>::failure(
          record.error().code, record.error().message);
    }
    return std::optional<ExtensionMessageUpdateEventRecord>{record.value()};
  }

  return std::optional<ExtensionMessageUpdateEventRecord>{};
}

bool metadata_handler_matches(const Json& handler, std::string_view event_type) {
  if (handler.is_string()) {
    return handler.get<std::string>() == event_type;
  }
  if (!handler.is_object() || !handler.contains("event") ||
      !handler.at("event").is_string()) {
    return false;
  }
  return handler.at("event").get<std::string>() == event_type;
}

bool metadata_handlers_match(const Json& metadata, const char* field,
                             std::string_view event_type) {
  if (!metadata.contains(field)) {
    return false;
  }
  const auto& handlers = metadata.at(field);
  if (!handlers.is_array()) {
    return false;
  }
  for (const auto& handler : handlers) {
    if (metadata_handler_matches(handler, event_type)) {
      return true;
    }
  }
  return false;
}

template <typename PlanFactory>
Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>> route_to_extensions(
    ExtensionHostRuntime& runtime, std::string event_type,
    PlanFactory make_plan) {
  std::vector<ExtensionHarnessLifecycleRoutingPlan> plans;

  for (const auto& extension : runtime.registry.loaded_extensions) {
    if (!extension_handles_harness_lifecycle_event(extension, event_type)) {
      continue;
    }

    auto typed_plan = make_plan(extension, make_request_id(runtime.next_request_id));
    if (!typed_plan) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          typed_plan.error().code, typed_plan.error().message);
    }

    ExtensionHarnessLifecycleRoutingPlan plan;
    plan.extension = extension;
    plan.event_type = event_type;
    plan.request = std::move(typed_plan.value().request);
    plans.push_back(std::move(plan));
    ++runtime.next_request_id;
  }

  return plans;
}

template <typename T>
void append_plans(std::vector<ExtensionHarnessLifecycleRoutingPlan>& output,
                  T&& plans) {
  output.insert(output.end(), std::make_move_iterator(plans.begin()),
                std::make_move_iterator(plans.end()));
}

Result<GenericEventRoutingPlan> make_generic_event_routing_plan(
    std::string method, Json params, Json request_id) {
  auto request = make_jsonrpc_request(std::move(method), std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<GenericEventRoutingPlan>::failure(request.error().code,
                                                    request.error().message);
  }

  GenericEventRoutingPlan plan;
  plan.request = std::move(request.value());
  return plan;
}

qt::foundation::Status require_dispatch_state(
    const ExtensionHarnessLifecycleDispatch& dispatch,
    ExtensionHarnessLifecycleDispatchState expected, const char* operation) {
  if (dispatch.state == expected) {
    return qt::foundation::Status::success();
  }
  return qt::foundation::Status::failure(
      ErrorCode::invalid_argument,
      std::string(operation) + " requires lifecycle dispatch state " +
          to_string(expected) + ", got " + to_string(dispatch.state));
}

qt::foundation::Status require_running(
    const ExtensionHarnessLifecycleDispatch& dispatch, const char* operation) {
  if (dispatch.runner.state != ExtensionRunnerState::kRunning) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires runner state running, got " +
            to_string(dispatch.runner.state));
  }
  if (dispatch.channel.state != SubprocessChannelState::kRunning) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires channel state running, got " +
            to_string(dispatch.channel.state));
  }
  return qt::foundation::Status::success();
}

bool matches_lifecycle_request(const ExtensionHarnessLifecycleDispatch& dispatch,
                               const Json& response_id) {
  return response_id == dispatch.routing_plan.request.id;
}

ExtensionHarnessLifecycleDispatchUpdate lifecycle_pending_update() {
  return ExtensionHarnessLifecycleDispatchUpdate{};
}

ExtensionHarnessLifecycleDispatchUpdate lifecycle_completed_success_update(
    JsonRpcResponse response) {
  ExtensionHarnessLifecycleDispatchUpdate update;
  update.kind = ExtensionContributionDispatchUpdateKind::kCompletedSuccess;
  update.response = std::move(response);
  return update;
}

ExtensionHarnessLifecycleDispatchUpdate lifecycle_completed_error_update(
    JsonRpcErrorResponse response) {
  ExtensionHarnessLifecycleDispatchUpdate update;
  update.kind = ExtensionContributionDispatchUpdateKind::kCompletedError;
  update.error_response = std::move(response);
  return update;
}

qt::foundation::Status enqueue_method_not_found_response(
    ExtensionHarnessLifecycleDispatch& dispatch,
    const JsonRpcRequest& request) {
  auto response = make_jsonrpc_error_response(
      request.id, -32601, "method not found", Json{{"method", request.method}});
  if (!response) {
    return qt::foundation::Status::failure(response.error().code,
                                           response.error().message);
  }
  return enqueue_subprocess_channel_message(dispatch.channel, response.value());
}

}  // namespace

std::string to_string(ExtensionHarnessLifecycleDispatchState state) {
  switch (state) {
    case ExtensionHarnessLifecycleDispatchState::kIdle:
      return "idle";
    case ExtensionHarnessLifecycleDispatchState::kDispatched:
      return "dispatched";
    case ExtensionHarnessLifecycleDispatchState::kCompletedSuccess:
      return "completed_success";
    case ExtensionHarnessLifecycleDispatchState::kCompletedError:
      return "completed_error";
  }
  return "idle";
}

bool extension_handles_harness_lifecycle_event(
    const LoadedExtensionRecord& extension, std::string_view event_type) {
  const auto& metadata = extension.manifest.metadata;
  if (!metadata.is_object()) {
    return false;
  }
  return metadata_handlers_match(metadata, "lifecycle_handlers", event_type) ||
         metadata_handlers_match(metadata, "lifecycleHandlers", event_type);
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_start_routing_plans(
    ExtensionHostRuntime& runtime, ExtensionSessionStartReason reason,
    std::optional<std::string> session_file) {
  auto record =
      make_extension_session_start_event_record(reason, std::move(session_file));
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "session_start",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_session_start_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_resource_discover_routing_plans(
    ExtensionHostRuntime& runtime, std::string cwd,
    ExtensionResourceDiscoverReason reason) {
  auto record =
      make_extension_resource_discover_event_record(std::move(cwd), reason);
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "resources_discover",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_resource_discover_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_instructions_loaded_routing_plans(
    ExtensionHostRuntime& runtime,
    const std::vector<qt::harness::InstructionLoadEvent>& events,
    std::string reason) {
  if (reason.empty()) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension instructions_loaded reason must not be empty");
  }

  Json files = Json::array();
  for (const auto& event : events) {
    files.push_back(instruction_load_event_json(event));
  }
  Json params{{"type", "instructions_loaded"},
              {"reason", std::move(reason)},
              {"files", std::move(files)}};
  return route_to_extensions(
      runtime, "instructions_loaded",
      [&](const LoadedExtensionRecord&, Json request_id) {
        return make_generic_event_routing_plan(
            "qt.event.instructions_loaded", params, std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_shutdown_routing_plans(
    ExtensionHostRuntime& runtime, ExtensionSessionShutdownReason reason,
    std::optional<std::string> target_session_file) {
  auto record = make_extension_session_shutdown_event_record(
      reason, std::move(target_session_file));
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "session_shutdown",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_session_shutdown_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_before_tree_routing_plans(ExtensionHostRuntime& runtime,
                                                 Json preparation) {
  auto record =
      make_extension_session_before_tree_event_record(std::move(preparation));
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "session_before_tree",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_session_before_tree_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_tree_routing_plans(
    ExtensionHostRuntime& runtime, std::optional<std::string> new_leaf_id,
    std::optional<std::string> old_leaf_id, std::optional<Json> summary_entry,
    std::optional<bool> from_extension) {
  auto record = make_extension_session_tree_event_record(
      std::move(new_leaf_id), std::move(old_leaf_id), std::move(summary_entry),
      std::move(from_extension));
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "session_tree",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_session_tree_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_input_routing_plans(ExtensionHostRuntime& runtime,
                                   std::string text, Json images,
                                   ExtensionInputSource source) {
  auto record = make_extension_input_event_record(std::move(text),
                                                  std::move(images), source);
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "input",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_input_event_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_model_select_routing_plans(
    ExtensionHostRuntime& runtime, Json model, std::optional<Json> previous_model,
    std::string source) {
  if (!model.is_object()) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension model_select model must be a JSON object");
  }
  if (previous_model.has_value() && !previous_model->is_object()) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension model_select previousModel must be a JSON object or null");
  }
  if (source != "set" && source != "cycle" && source != "restore") {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension model_select source must be set, cycle, or restore");
  }

  Json params{{"type", "model_select"},
              {"model", std::move(model)},
              {"previousModel", previous_model.has_value()
                                    ? Json(std::move(*previous_model))
                                    : Json(nullptr)},
              {"source", std::move(source)}};
  return route_to_extensions(
      runtime, "model_select",
      [&](const LoadedExtensionRecord&, Json request_id) {
        return make_generic_event_routing_plan("qt.event.model_select", params,
                                               std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_thinking_level_select_routing_plans(
    ExtensionHostRuntime& runtime, std::string level,
    std::string previous_level) {
  if (level.empty() || previous_level.empty()) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension thinking_level_select levels must not be empty");
  }

  Json params{{"type", "thinking_level_select"},
              {"level", std::move(level)},
              {"previousLevel", std::move(previous_level)}};
  return route_to_extensions(
      runtime, "thinking_level_select",
      [&](const LoadedExtensionRecord&, Json request_id) {
        return make_generic_event_routing_plan(
            "qt.event.thinking_level_select", params, std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_user_bash_routing_plans(ExtensionHostRuntime& runtime,
                                       std::string command,
                                       bool exclude_from_context,
                                       std::string cwd) {
  if (command.empty()) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension user_bash command must not be empty");
  }
  if (cwd.empty()) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension user_bash cwd must not be empty");
  }

  Json params{{"type", "user_bash"},
              {"command", std::move(command)},
              {"excludeFromContext", exclude_from_context},
              {"cwd", std::move(cwd)}};
  return route_to_extensions(
      runtime, "user_bash",
      [&](const LoadedExtensionRecord&, Json request_id) {
        return make_generic_event_routing_plan("qt.event.user_bash", params,
                                               std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_lifecycle_routing_plans(
    ExtensionHostRuntime& runtime, const qt::agent::AgentEvent& event,
    const ExtensionHarnessLifecycleRoutingOptions& options) {
  if (options.timestamp <= 0) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle timestamp must be a positive integer");
  }

  const auto turn_index = checked_index(event.turn_index, "agent turn index");
  if (!turn_index) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        turn_index.error().code, turn_index.error().message);
  }

  switch (event.kind) {
    case qt::agent::AgentEventKind::turn_started: {
      auto record = make_extension_turn_start_event_record(
          turn_index.value(), options.timestamp);
      if (!record) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            record.error().code, record.error().message);
      }
      return route_to_extensions(
          runtime, "turn_start",
          [&](const LoadedExtensionRecord& extension, Json request_id) {
            return make_extension_turn_start_routing_plan(
                extension, record.value(), std::move(request_id));
          });
    }

    case qt::agent::AgentEventKind::turn_completed: {
      if (!event.response.has_value()) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            ErrorCode::invalid_argument,
            "agent turn_completed event requires a response");
      }

      Json tool_results = Json::array();
      for (const auto& tool_result : options.tool_results) {
        tool_results.push_back(
            tool_result_message_json(tool_result, options.timestamp));
      }

      auto record = make_extension_turn_end_event_record(
          turn_index.value(),
          message_json(event.response->message, options.timestamp),
          std::move(tool_results));
      if (!record) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            record.error().code, record.error().message);
      }
      return route_to_extensions(
          runtime, "turn_end",
          [&](const LoadedExtensionRecord& extension, Json request_id) {
            return make_extension_turn_end_routing_plan(
                extension, record.value(), std::move(request_id));
          });
    }

    case qt::agent::AgentEventKind::message_committed: {
      if (!event.committed_message.has_value()) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            ErrorCode::invalid_argument,
            "agent message_committed event requires a committed message");
      }

      std::vector<ExtensionHarnessLifecycleRoutingPlan> plans;
      auto message =
          message_json(event.committed_message.value(), options.timestamp);

      auto start_record = make_extension_message_start_event_record(message);
      if (!start_record) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            start_record.error().code, start_record.error().message);
      }
      auto start_plans = route_to_extensions(
          runtime, "message_start",
          [&](const LoadedExtensionRecord& extension, Json request_id) {
            return make_extension_message_start_routing_plan(
                extension, start_record.value(), std::move(request_id));
          });
      if (!start_plans) {
        return start_plans;
      }
      append_plans(plans, std::move(start_plans.value()));

      auto end_record = make_extension_message_end_event_record(std::move(message));
      if (!end_record) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            end_record.error().code, end_record.error().message);
      }
      auto end_plans = route_to_extensions(
          runtime, "message_end",
          [&](const LoadedExtensionRecord& extension, Json request_id) {
            return make_extension_message_end_routing_plan(
                extension, end_record.value(), std::move(request_id));
          });
      if (!end_plans) {
        return end_plans;
      }
      append_plans(plans, std::move(end_plans.value()));
      return plans;
    }

    case qt::agent::AgentEventKind::provider_event: {
      if (!event.provider_event.has_value()) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            ErrorCode::invalid_argument,
            "agent provider_event requires a provider event");
      }

      const auto record = make_provider_message_update_record(
          *event.provider_event, options.timestamp);
      if (!record) {
        return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
            record.error().code, record.error().message);
      }
      if (!record.value().has_value()) {
        return std::vector<ExtensionHarnessLifecycleRoutingPlan>{};
      }
      return route_to_extensions(
          runtime, "message_update",
          [&](const LoadedExtensionRecord& extension, Json request_id) {
            return make_extension_message_update_routing_plan(
                extension, record.value().value(), std::move(request_id));
          });
    }

    default:
      return std::vector<ExtensionHarnessLifecycleRoutingPlan>{};
  }
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_lifecycle_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::SubscribedEvent& event,
    const ExtensionHarnessLifecycleRoutingOptions& options) {
  if (const auto* agent_event = std::get_if<qt::agent::AgentEvent>(&event)) {
    return make_extension_harness_lifecycle_routing_plans(
        runtime, *agent_event, options);
  }

  if (const auto* before_start =
          std::get_if<qt::harness::AgentHarness::BeforeAgentStartEvent>(
              &event)) {
    auto record = make_extension_before_agent_start_event_record(
        prompt_text(before_start->prompt), prompt_images_json(before_start->prompt),
        std::string(),
        Json{{"resources", resources_json(before_start->resources)}});
    if (!record) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          record.error().code, record.error().message);
    }

    return route_to_extensions(
        runtime, "before_agent_start",
        [&](const LoadedExtensionRecord& extension, Json request_id) {
          return make_extension_before_agent_start_routing_plan(
              extension, record.value(), std::move(request_id));
        });
  }

  if (std::get_if<qt::harness::AgentHarness::AgentStartEvent>(&event)) {
    const auto record = make_extension_agent_start_event_record();
    return route_to_extensions(
        runtime, "agent_start",
        [&](const LoadedExtensionRecord& extension, Json request_id) {
          return make_extension_agent_start_routing_plan(
              extension, record, std::move(request_id));
        });
  }

  if (const auto* end =
          std::get_if<qt::harness::AgentHarness::AgentEndEvent>(&event)) {
    auto record = make_extension_agent_end_event_record(
        lifecycle_messages_json(end->messages, options.timestamp));
    if (!record) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          record.error().code, record.error().message);
    }

    return route_to_extensions(
        runtime, "agent_end",
        [&](const LoadedExtensionRecord& extension, Json request_id) {
          return make_extension_agent_end_routing_plan(
              extension, record.value(), std::move(request_id));
        });
  }

  if (const auto* before_compact =
          std::get_if<qt::harness::AgentHarness::BeforeCompactEvent>(&event)) {
    auto record = make_extension_session_before_compact_event_record(
        compaction_preparation_json(before_compact->preparation),
        session_entries_json(before_compact->branch_entries),
        before_compact->custom_instructions);
    if (!record) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          record.error().code, record.error().message);
    }

    return route_to_extensions(
        runtime, "session_before_compact",
        [&](const LoadedExtensionRecord& extension, Json request_id) {
          return make_extension_session_before_compact_routing_plan(
              extension, record.value(), std::move(request_id));
        });
  }

  if (const auto* compact =
          std::get_if<qt::harness::AgentHarness::CompactEvent>(&event)) {
    auto record = make_extension_session_compact_event_record(
        compaction_entry_json(*compact), compact->from_hook);
    if (!record) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          record.error().code, record.error().message);
    }

    return route_to_extensions(
        runtime, "session_compact",
        [&](const LoadedExtensionRecord& extension, Json request_id) {
          return make_extension_session_compact_routing_plan(
              extension, record.value(), std::move(request_id));
        });
  }

  if (const auto* resources =
          std::get_if<qt::harness::AgentHarnessResourcesUpdate>(&event)) {
    Json params{{"type", "resources_update"},
                {"resources", resources_json(resources->resources)},
                {"previousResources",
                 resources_json(resources->previous_resources)}};
    return route_to_extensions(
        runtime, "resources_update",
        [&](const LoadedExtensionRecord&, Json request_id) {
          return make_generic_event_routing_plan(
              "qt.event.resources_update", params, std::move(request_id));
        });
  }

  if (const auto* start =
          std::get_if<qt::harness::AgentHarness::SessionStartEvent>(&event)) {
    const auto reason = parse_harness_session_start_reason(start->reason);
    if (!reason) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          reason.error().code, reason.error().message);
    }
    const auto record = make_extension_session_start_event_record(
        reason.value(), start->previous_session_file);
    if (!record) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          record.error().code, record.error().message);
    }
    return route_to_extensions(
        runtime, "session_start",
        [&](const LoadedExtensionRecord& extension, Json request_id) {
          return make_extension_session_start_routing_plan(
              extension, record.value(), std::move(request_id));
        });
  }

  if (const auto* shutdown =
          std::get_if<qt::harness::AgentHarness::SessionShutdownEvent>(&event)) {
    const auto reason =
        parse_extension_session_shutdown_reason(shutdown->reason);
    if (!reason) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          reason.error().code, reason.error().message);
    }
    const auto record = make_extension_session_shutdown_event_record(
        reason.value(), shutdown->target_session_file);
    if (!record) {
      return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
          record.error().code, record.error().message);
    }
    return route_to_extensions(
        runtime, "session_shutdown",
        [&](const LoadedExtensionRecord& extension, Json request_id) {
          return make_extension_session_shutdown_routing_plan(
              extension, record.value(), std::move(request_id));
        });
  }

  if (const auto* before_tree =
          std::get_if<qt::harness::AgentHarness::SessionBeforeTreeEvent>(
              &event)) {
    return make_extension_session_before_tree_routing_plans(
        runtime, before_tree->preparation);
  }

  if (const auto* tree =
          std::get_if<qt::harness::AgentHarness::SessionTreeEvent>(&event)) {
    return make_extension_session_tree_routing_plans(
        runtime, tree->new_leaf_id, tree->old_leaf_id, tree->summary_entry,
        tree->from_extension);
  }

  if (const auto* abort =
          std::get_if<qt::harness::AgentHarnessAbortEvent>(&event)) {
    Json params{{"type", "abort"},
                {"clearedSteer", messages_json(abort->cleared_steer)},
                {"clearedFollowUp", messages_json(abort->cleared_follow_up)}};
    return route_to_extensions(
        runtime, "abort",
        [&](const LoadedExtensionRecord&, Json request_id) {
          return make_generic_event_routing_plan("qt.event.abort", params,
                                                 std::move(request_id));
        });
  }

  if (const auto* settled =
          std::get_if<qt::harness::AgentHarnessSettledEvent>(&event)) {
    Json params{{"type", "settled"},
                {"nextTurnCount", settled->next_turn_count}};
    return route_to_extensions(
        runtime, "settled",
        [&](const LoadedExtensionRecord&, Json request_id) {
          return make_generic_event_routing_plan("qt.event.settled", params,
                                                 std::move(request_id));
        });
  }

  return std::vector<ExtensionHarnessLifecycleRoutingPlan>{};
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_call_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolCallEvent& event) {
  auto record = make_extension_tool_call_event_record(
      tool_call_id(event), event.request.tool_name, event.request.arguments);
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "tool_call",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_tool_call_event_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_result_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolResultEvent& event) {
  auto record = make_extension_tool_result_event_record(
      tool_call_id(event), event.request.tool_name, event.request.arguments,
      tool_result_content(event.result), event.result.output,
      tool_result_is_error(event.result));
  if (!record) {
    return Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>::failure(
        record.error().code, record.error().message);
  }

  return route_to_extensions(
      runtime, "tool_result",
      [&](const LoadedExtensionRecord& extension, Json request_id) {
        return make_extension_tool_result_event_routing_plan(
            extension, record.value(), std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_execution_start_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolCallEvent& event) {
  Json params{{"type", "tool_execution_start"},
              {"toolCallId", tool_call_id(event)},
              {"toolName", event.request.tool_name},
              {"args", event.request.arguments}};
  return route_to_extensions(
      runtime, "tool_execution_start",
      [&](const LoadedExtensionRecord&, Json request_id) {
        return make_generic_event_routing_plan(
            "qt.event.tool_execution_start", params, std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_execution_update_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolResultEvent& event) {
  Json params{{"type", "tool_execution_update"},
              {"toolCallId", tool_call_id(event)},
              {"toolName", event.request.tool_name},
              {"args", event.request.arguments},
              {"partialResult", event.result.output}};
  return route_to_extensions(
      runtime, "tool_execution_update",
      [&](const LoadedExtensionRecord&, Json request_id) {
        return make_generic_event_routing_plan(
            "qt.event.tool_execution_update", params, std::move(request_id));
      });
}

Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_execution_end_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolResultEvent& event) {
  Json params{{"type", "tool_execution_end"},
              {"toolCallId", tool_call_id(event)},
              {"toolName", event.request.tool_name},
              {"result", event.result.output},
              {"isError", tool_result_is_error(event.result)}};
  return route_to_extensions(
      runtime, "tool_execution_end",
      [&](const LoadedExtensionRecord&, Json request_id) {
        return make_generic_event_routing_plan(
            "qt.event.tool_execution_end", params, std::move(request_id));
      });
}

Result<ExtensionHarnessLifecycleDispatch>
make_extension_harness_lifecycle_dispatch(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan) {
  auto lifecycle = make_extension_runner_lifecycle(routing_plan.extension);
  if (!lifecycle) {
    return lifecycle.error();
  }
  auto channel = make_subprocess_channel();
  if (!channel) {
    return channel.error();
  }

  ExtensionHarnessLifecycleDispatch dispatch;
  dispatch.routing_plan = routing_plan;
  dispatch.runner = std::move(lifecycle.value());
  dispatch.channel = std::move(channel.value());
  return dispatch;
}

Result<ExtensionHarnessLifecycleResult>
decode_extension_harness_lifecycle_result(
    const ExtensionHarnessLifecycleDispatch& dispatch,
    const ExtensionHarnessLifecycleDispatchUpdate& update,
    const std::vector<std::string>& diagnostics) {
  if (!update.completed()) {
    return Result<ExtensionHarnessLifecycleResult>::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle result is not completed");
  }

  ExtensionHarnessLifecycleResult result;
  result.extension = dispatch.routing_plan.extension;
  result.event_type = dispatch.routing_plan.event_type;
  result.request_id = dispatch.routing_plan.request.id;
  result.diagnostics = diagnostics;

  if (update.kind == ExtensionContributionDispatchUpdateKind::kCompletedSuccess &&
      update.response.has_value()) {
    result.ok = true;
    result.value = update.response->result;
    return result;
  }

  if (update.kind == ExtensionContributionDispatchUpdateKind::kCompletedError &&
      update.error_response.has_value()) {
    const auto decoded_error =
        decode_jsonrpc_error(update.error_response->error);
    if (!decoded_error) {
      return Result<ExtensionHarnessLifecycleResult>::failure(
          decoded_error.error().code, decoded_error.error().message);
    }

    result.ok = false;
    result.error = decoded_error.value();
    return result;
  }

  return Result<ExtensionHarnessLifecycleResult>::failure(
      ErrorCode::invalid_argument,
      "extension harness lifecycle result is missing completed response payload");
}

qt::foundation::Status request_extension_harness_lifecycle_dispatch_start(
    ExtensionHarnessLifecycleDispatch& dispatch) {
  const auto runner = request_extension_start(dispatch.runner);
  if (!runner) {
    return runner;
  }
  const auto channel = request_subprocess_channel_start(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return qt::foundation::Status::success();
}

qt::foundation::Status confirm_extension_harness_lifecycle_dispatch_started(
    ExtensionHarnessLifecycleDispatch& dispatch, int process_id) {
  const auto runner = confirm_extension_started(dispatch.runner, process_id);
  if (!runner) {
    return runner;
  }
  const auto channel = confirm_subprocess_channel_started(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return qt::foundation::Status::success();
}

qt::foundation::Status dispatch_extension_harness_lifecycle_event(
    ExtensionHarnessLifecycleDispatch& dispatch) {
  const auto running =
      require_running(dispatch, "dispatch_extension_harness_lifecycle_event");
  if (!running) {
    return running;
  }
  const auto idle = require_dispatch_state(
      dispatch, ExtensionHarnessLifecycleDispatchState::kIdle,
      "dispatch_extension_harness_lifecycle_event");
  if (!idle) {
    return idle;
  }

  const auto enqueue = enqueue_subprocess_channel_message(
      dispatch.channel, dispatch.routing_plan.request);
  if (!enqueue) {
    return enqueue;
  }

  dispatch.state = ExtensionHarnessLifecycleDispatchState::kDispatched;
  ++dispatch.dispatch_count;
  return qt::foundation::Status::success();
}

Result<ExtensionHarnessLifecycleDispatchUpdate>
push_extension_harness_lifecycle_dispatch_stdout(
    ExtensionHarnessLifecycleDispatch& dispatch, std::string_view bytes) {
  const auto frames = push_subprocess_channel_stdout(dispatch.channel, bytes);
  if (!frames) {
    return frames.error();
  }

  for (const auto& frame : frames.value()) {
    switch (frame.kind) {
      case SubprocessChannelFrameKind::kJsonRpcResponse: {
        const auto& response = std::get<JsonRpcResponse>(frame.payload);
        if (matches_lifecycle_request(dispatch, response.id) &&
            dispatch.state ==
                ExtensionHarnessLifecycleDispatchState::kDispatched) {
          dispatch.state =
              ExtensionHarnessLifecycleDispatchState::kCompletedSuccess;
          return lifecycle_completed_success_update(response);
        }
        break;
      }
      case SubprocessChannelFrameKind::kJsonRpcErrorResponse: {
        const auto& response = std::get<JsonRpcErrorResponse>(frame.payload);
        if (matches_lifecycle_request(dispatch, response.id) &&
            dispatch.state ==
                ExtensionHarnessLifecycleDispatchState::kDispatched) {
          dispatch.state =
              ExtensionHarnessLifecycleDispatchState::kCompletedError;
          return lifecycle_completed_error_update(response);
        }
        break;
      }
      case SubprocessChannelFrameKind::kJsonRpcRequest: {
        const auto& request = std::get<JsonRpcRequest>(frame.payload);
        const auto response =
            enqueue_method_not_found_response(dispatch, request);
        if (!response) {
          return response.error();
        }
        break;
      }
      case SubprocessChannelFrameKind::kJsonRpcNotification:
      case SubprocessChannelFrameKind::kDiagnostic:
        break;
    }
  }

  return lifecycle_pending_update();
}

Result<ExtensionHarnessLifecycleDispatchUpdate>
push_extension_harness_lifecycle_dispatch_stderr(
    ExtensionHarnessLifecycleDispatch& dispatch, std::string_view bytes) {
  const auto frames = push_subprocess_channel_stderr(dispatch.channel, bytes);
  if (!frames) {
    return frames.error();
  }

  std::vector<std::string> diagnostics;
  diagnostics.reserve(frames.value().size());
  for (const auto& frame : frames.value()) {
    assert(frame.kind == SubprocessChannelFrameKind::kDiagnostic);
    diagnostics.push_back(frame.line);
  }

  ExtensionHarnessLifecycleDispatchUpdate update;
  if (!diagnostics.empty()) {
    update.kind = ExtensionContributionDispatchUpdateKind::kDiagnostics;
    update.diagnostics = std::move(diagnostics);
  }
  return update;
}

qt::foundation::Status request_extension_harness_lifecycle_dispatch_stop(
    ExtensionHarnessLifecycleDispatch& dispatch) {
  const auto runner = request_extension_stop(dispatch.runner);
  if (!runner) {
    return runner;
  }
  const auto channel = request_subprocess_channel_stop(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return qt::foundation::Status::success();
}

qt::foundation::Status confirm_extension_harness_lifecycle_dispatch_stopped(
    ExtensionHarnessLifecycleDispatch& dispatch) {
  const auto runner = confirm_extension_stopped(dispatch.runner);
  if (!runner) {
    return runner;
  }
  const auto channel = confirm_subprocess_channel_stopped(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return qt::foundation::Status::success();
}

Result<ExtensionHarnessLifecycleSubprocessBridge>
make_extension_harness_lifecycle_subprocess_bridge(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan) {
  auto dispatch = make_extension_harness_lifecycle_dispatch(routing_plan);
  if (!dispatch) {
    return dispatch.error();
  }

  ExtensionHarnessLifecycleSubprocessBridge bridge;
  bridge.dispatch = std::move(dispatch.value());
  return bridge;
}

Result<ExtensionSubprocessLaunchRequest>
plan_extension_harness_lifecycle_subprocess_launch(
    const ExtensionHarnessLifecycleSubprocessBridge& bridge) {
  if (bridge.dispatch.runner.launch_plan.transport !=
      ExtensionTransport::kSubprocessJsonRpc) {
    return Result<ExtensionSubprocessLaunchRequest>::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle subprocess bridge requires "
        "subprocess-jsonrpc transport");
  }

  ExtensionSubprocessLaunchRequest request;
  request.launch_plan = bridge.dispatch.runner.launch_plan;
  request.executable_path = request.launch_plan.executable_path;
  request.arguments = request.launch_plan.arguments;
  request.working_directory = request.launch_plan.working_directory;
  return request;
}

qt::foundation::Status request_extension_harness_lifecycle_subprocess_launch(
    ExtensionHarnessLifecycleSubprocessBridge& bridge) {
  const auto status =
      request_extension_harness_lifecycle_dispatch_start(bridge.dispatch);
  if (!status) {
    return status;
  }

  bridge.launch_requested = true;
  return qt::foundation::Status::success();
}

qt::foundation::Status confirm_extension_harness_lifecycle_subprocess_launched(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, int process_id) {
  return confirm_extension_harness_lifecycle_dispatch_started(bridge.dispatch,
                                                             process_id);
}

Result<std::string> write_extension_harness_lifecycle_subprocess_stdin(
    ExtensionHarnessLifecycleSubprocessBridge& bridge) {
  if (bridge.completed_result.has_value()) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle subprocess bridge event is already "
        "completed");
  }

  if (!bridge.event_written) {
    const auto dispatch =
        dispatch_extension_harness_lifecycle_event(bridge.dispatch);
    if (!dispatch) {
      return dispatch.error();
    }
    bridge.event_written = true;
  }

  return take_subprocess_channel_output(bridge.dispatch.channel);
}

Result<ExtensionHarnessLifecycleDispatchUpdate>
read_extension_harness_lifecycle_subprocess_stdout(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, std::string_view bytes) {
  auto update =
      push_extension_harness_lifecycle_dispatch_stdout(bridge.dispatch, bytes);
  if (!update) {
    return update.error();
  }

  if (update.value().completed()) {
    auto result = decode_extension_harness_lifecycle_result(
        bridge.dispatch, update.value(), bridge.diagnostics);
    if (!result) {
      return result.error();
    }
    bridge.completed_result = std::move(result.value());
  }

  return update.value();
}

Result<ExtensionHarnessLifecycleDispatchUpdate>
read_extension_harness_lifecycle_subprocess_stderr(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, std::string_view bytes) {
  auto update =
      push_extension_harness_lifecycle_dispatch_stderr(bridge.dispatch, bytes);
  if (!update) {
    return update.error();
  }

  bridge.diagnostics.insert(bridge.diagnostics.end(),
                            update.value().diagnostics.begin(),
                            update.value().diagnostics.end());
  return update.value();
}

qt::foundation::Status request_extension_harness_lifecycle_subprocess_teardown(
    ExtensionHarnessLifecycleSubprocessBridge& bridge) {
  const auto status =
      request_extension_harness_lifecycle_dispatch_stop(bridge.dispatch);
  if (!status) {
    return status;
  }

  bridge.teardown_requested = true;
  return qt::foundation::Status::success();
}

qt::foundation::Status confirm_extension_harness_lifecycle_subprocess_teardown(
    ExtensionHarnessLifecycleSubprocessBridge& bridge) {
  return confirm_extension_harness_lifecycle_dispatch_stopped(bridge.dispatch);
}

qt::foundation::Status record_extension_harness_lifecycle_subprocess_crashed(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, int exit_code,
    std::string diagnostic) {
  if (!bridge.launch_requested) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle subprocess crash requires launch to be "
        "requested");
  }
  if (bridge.completed_result.has_value()) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle subprocess bridge event is already "
        "completed");
  }
  if (!diagnostic.empty()) {
    bridge.diagnostics.push_back(std::move(diagnostic));
  }

  const auto runner =
      record_extension_crashed(bridge.dispatch.runner, exit_code);
  if (!runner) {
    return runner;
  }
  const auto channel = record_subprocess_channel_crashed(bridge.dispatch.channel);
  if (!channel) {
    return channel;
  }

  bridge.dispatch.state = ExtensionHarnessLifecycleDispatchState::kCompletedError;
  bridge.teardown_requested = true;

  ExtensionHarnessLifecycleResult result;
  result.extension = bridge.dispatch.routing_plan.extension;
  result.event_type = bridge.dispatch.routing_plan.event_type;
  result.request_id = bridge.dispatch.routing_plan.request.id;
  result.ok = false;
  result.error = JsonRpcError{
      -32050,
      "extension subprocess exited before completing lifecycle event",
      Json{{"event", bridge.dispatch.routing_plan.event_type},
           {"exitCode", exit_code}}};
  result.diagnostics = bridge.diagnostics;
  bridge.completed_result = std::move(result);
  return qt::foundation::Status::success();
}

qt::foundation::Status reset_extension_harness_lifecycle_subprocess_for_retry(
    ExtensionHarnessLifecycleSubprocessBridge& bridge) {
  if (!bridge.completed_result.has_value()) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle retry requires a completed result");
  }
  if (bridge.completed_result->ok) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle retry requires a failed result");
  }
  if (bridge.dispatch.runner.state != ExtensionRunnerState::kStopped) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle retry requires runner state stopped, got " +
            to_string(bridge.dispatch.runner.state));
  }
  if (bridge.dispatch.channel.state != SubprocessChannelState::kStopped) {
    return qt::foundation::Status::failure(
        ErrorCode::invalid_argument,
        "extension harness lifecycle retry requires channel state stopped, got " +
            to_string(bridge.dispatch.channel.state));
  }

  auto dispatch =
      make_extension_harness_lifecycle_dispatch(bridge.dispatch.routing_plan);
  if (!dispatch) {
    return qt::foundation::Status::failure(dispatch.error().code,
                                           dispatch.error().message);
  }

  bridge.dispatch = std::move(dispatch.value());
  bridge.diagnostics.clear();
  bridge.completed_result.reset();
  bridge.launch_requested = false;
  bridge.event_written = false;
  bridge.teardown_requested = false;
  ++bridge.retry_count;
  return qt::foundation::Status::success();
}

}  // namespace qt::extensions
