#include <qt/extensions/contributions.hpp>

#include <cassert>
#include <map>
#include <set>
#include <utility>

namespace qt::extensions {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

constexpr int kJsonRpcMethodNotFound = -32601;

Result<std::string> decode_required_string_field(const Json& json,
                                                 const char* field,
                                                 const char* context);

Json make_host_request_id(std::size_t request_id) {
  return "extension-host-" + std::to_string(request_id);
}

void append_command_contributions(
    std::vector<ExtensionContributionDescriptor>& output,
    const std::vector<LoadedExtensionRecord>& extensions) {
  std::map<std::string, std::size_t> counts;
  for (const auto& extension : extensions) {
    for (const auto& contribution : extension.manifest.commands) {
      ++counts[contribution.name];
    }
  }

  std::map<std::string, std::size_t> seen;
  for (std::size_t extension_index = 0; extension_index < extensions.size();
       ++extension_index) {
    const auto& extension = extensions[extension_index];
    for (std::size_t contribution_index = 0;
         contribution_index < extension.manifest.commands.size();
         ++contribution_index) {
      const auto& contribution = extension.manifest.commands[contribution_index];
      std::string invocation_name = contribution.name;
      if (counts[contribution.name] > 1U) {
        invocation_name += ":" + std::to_string(++seen[contribution.name]);
      }
      output.push_back(ExtensionContributionDescriptor{
          ExtensionContributionKind::kCommand,
          extension_index,
          contribution_index,
          extension,
          contribution.name,
          std::move(invocation_name),
          contribution.title,
          contribution.description,
      });
    }
  }
}

void append_tool_contributions(
    std::vector<ExtensionContributionDescriptor>& output,
    const std::vector<LoadedExtensionRecord>& extensions) {
  std::set<std::string> seen_names;
  for (std::size_t extension_index = 0; extension_index < extensions.size();
       ++extension_index) {
    const auto& extension = extensions[extension_index];
    for (std::size_t contribution_index = 0;
         contribution_index < extension.manifest.tools.size();
         ++contribution_index) {
      const auto& contribution = extension.manifest.tools[contribution_index];
      if (!seen_names.insert(contribution.name).second) {
        continue;
      }
      output.push_back(ExtensionContributionDescriptor{
          ExtensionContributionKind::kTool,
          extension_index,
          contribution_index,
          extension,
          contribution.name,
          contribution.name,
          contribution.title,
          contribution.description,
      });
    }
  }
}

void append_provider_contributions(
    std::vector<ExtensionProviderContributionDescriptor>& output,
    const std::vector<LoadedExtensionRecord>& extensions) {
  for (std::size_t extension_index = 0; extension_index < extensions.size();
       ++extension_index) {
    const auto& extension = extensions[extension_index];
    for (std::size_t contribution_index = 0;
         contribution_index < extension.manifest.providers.size();
         ++contribution_index) {
      const auto& contribution =
          extension.manifest.providers[contribution_index];
      output.push_back(ExtensionProviderContributionDescriptor{
          extension_index,
          contribution_index,
          extension,
          contribution.name,
          contribution.config,
      });
    }
  }
}

void append_flag_contributions(
    std::vector<ExtensionFlagContributionDescriptor>& output,
    const std::vector<LoadedExtensionRecord>& extensions) {
  std::set<std::string> seen_names;
  for (std::size_t extension_index = 0; extension_index < extensions.size();
       ++extension_index) {
    const auto& extension = extensions[extension_index];
    for (std::size_t contribution_index = 0;
         contribution_index < extension.manifest.flags.size();
         ++contribution_index) {
      const auto& contribution = extension.manifest.flags[contribution_index];
      if (!seen_names.insert(contribution.name).second) {
        continue;
      }
      output.push_back(ExtensionFlagContributionDescriptor{
          extension_index,
          contribution_index,
          extension,
          contribution.name,
          contribution.description,
          contribution.type,
          contribution.default_value,
      });
    }
  }
}

const std::vector<ExtensionContributionDescriptor>& descriptors_for(
    const ExtensionContributionRegistry& registry,
    ExtensionContributionKind kind) noexcept {
  switch (kind) {
    case ExtensionContributionKind::kCommand:
      return registry.commands;
    case ExtensionContributionKind::kTool:
      return registry.tools;
  }

  return registry.commands;
}

Result<ExtensionContributionDescriptor> find_contribution(
    const ExtensionContributionRegistry& registry, ExtensionContributionKind kind,
    std::string_view name) {
  if (name.empty()) {
    return Result<ExtensionContributionDescriptor>::failure(
        ErrorCode::invalid_argument, "contribution name is required");
  }

  const auto& descriptors = descriptors_for(registry, kind);
  for (const auto& descriptor : descriptors) {
    const bool matches =
        kind == ExtensionContributionKind::kCommand
            ? descriptor.invocation_name == name
            : descriptor.name == name;
    if (!matches) {
      continue;
    }
    return descriptor;
  }

  return Result<ExtensionContributionDescriptor>::failure(
      ErrorCode::not_found,
      "extension " + to_string(kind) + " contribution not found: " +
          std::string(name));
}

Result<ExtensionContributionInvocationPlan> make_invocation_plan(
    const ExtensionContributionRegistry& registry, ExtensionContributionKind kind,
    std::string_view name, Json arguments, Json request_id) {
  const auto descriptor = find_contribution(registry, kind, name);
  if (!descriptor) {
    return descriptor.error();
  }

  if (!is_valid_jsonrpc_params(arguments)) {
    return Result<ExtensionContributionInvocationPlan>::failure(
        ErrorCode::invalid_argument,
        "contribution invocation arguments must be a JSON object or array");
  }

  const std::string method =
      kind == ExtensionContributionKind::kCommand ? "qt.command.invoke"
                                                  : "qt.tool.invoke";

  Json params = Json::object();
  params["name"] = descriptor.value().name;
  params["arguments"] = arguments;

  const auto request = make_jsonrpc_request(method, std::move(params),
                                            std::move(request_id));
  if (!request) {
    return Result<ExtensionContributionInvocationPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionContributionInvocationPlan plan;
  plan.contribution = descriptor.value();
  plan.arguments = std::move(arguments);
  plan.request = std::move(request.value());
  return plan;
}

Status require_synced_stopped(const ExtensionContributionDispatch& dispatch,
                              const char* operation) {
  if (dispatch.runner.state != ExtensionRunnerState::kStopped) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires runner state stopped, got " +
            to_string(dispatch.runner.state));
  }
  if (dispatch.channel.state != SubprocessChannelState::kStopped) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires channel state stopped, got " +
            to_string(dispatch.channel.state));
  }
  return Status::success();
}

Status require_synced_starting(const ExtensionContributionDispatch& dispatch,
                               const char* operation) {
  if (dispatch.runner.state != ExtensionRunnerState::kStarting) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires runner state starting, got " +
            to_string(dispatch.runner.state));
  }
  if (dispatch.channel.state != SubprocessChannelState::kStarting) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires channel state starting, got " +
            to_string(dispatch.channel.state));
  }
  return Status::success();
}

Status require_synced_running(const ExtensionContributionDispatch& dispatch,
                              const char* operation) {
  if (dispatch.runner.state != ExtensionRunnerState::kRunning) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires runner state running, got " +
            to_string(dispatch.runner.state));
  }
  if (dispatch.channel.state != SubprocessChannelState::kRunning) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires channel state running, got " +
            to_string(dispatch.channel.state));
  }
  return Status::success();
}

Status require_synced_stopping(const ExtensionContributionDispatch& dispatch,
                               const char* operation) {
  if (dispatch.runner.state != ExtensionRunnerState::kStopping) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires runner state stopping, got " +
            to_string(dispatch.runner.state));
  }
  if (dispatch.channel.state != SubprocessChannelState::kStopping) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(operation) + " requires channel state stopping, got " +
            to_string(dispatch.channel.state));
  }
  return Status::success();
}

bool matches_invocation_request(
    const ExtensionContributionDispatch& dispatch,
    const Json& response_id) {
  return response_id == dispatch.invocation_plan.request.id;
}

ExtensionContributionDispatchUpdate pending_dispatch_update() {
  return ExtensionContributionDispatchUpdate{};
}

ExtensionContributionDispatchUpdate make_diagnostics_update(
    std::vector<std::string> diagnostics) {
  ExtensionContributionDispatchUpdate update;
  if (!diagnostics.empty()) {
    update.kind = ExtensionContributionDispatchUpdateKind::kDiagnostics;
    update.diagnostics = std::move(diagnostics);
  }
  return update;
}

ExtensionContributionDispatchUpdate make_completed_success_update(
    JsonRpcResponse response) {
  ExtensionContributionDispatchUpdate update;
  update.kind = ExtensionContributionDispatchUpdateKind::kCompletedSuccess;
  update.response = std::move(response);
  return update;
}

ExtensionContributionDispatchUpdate make_completed_error_update(
    JsonRpcErrorResponse response) {
  ExtensionContributionDispatchUpdate update;
  update.kind = ExtensionContributionDispatchUpdateKind::kCompletedError;
  update.error_response = std::move(response);
  return update;
}

ExtensionHostResourceEvent make_diagnostic_event(std::string message) {
  ExtensionHostResourceEvent event;
  event.kind = ExtensionHostResourceEventKind::kDiagnostic;
  event.message = std::move(message);
  return event;
}

ExtensionHostResourceEvent make_loaded_event(
    const LoadedExtensionRecord& extension, std::size_t extension_index) {
  ExtensionHostResourceEvent event;
  event.kind = ExtensionHostResourceEventKind::kExtensionLoaded;
  event.extension_index = extension_index;
  event.extension_name = extension.manifest.name;
  event.message = "loaded extension: " + extension.manifest.name;
  return event;
}

ExtensionHostResourceEvent make_contribution_event(
    const ExtensionContributionDescriptor& descriptor) {
  ExtensionHostResourceEvent event;
  event.kind = descriptor.kind == ExtensionContributionKind::kCommand
                   ? ExtensionHostResourceEventKind::kCommandDiscovered
                   : ExtensionHostResourceEventKind::kToolDiscovered;
  event.extension_index = descriptor.extension_index;
  event.contribution_index = descriptor.contribution_index;
  event.extension_name = descriptor.extension.manifest.name;
  event.contribution_name = descriptor.name;
  event.message = "discovered extension " + to_string(descriptor.kind) + ": " +
                  descriptor.name;
  return event;
}

ExtensionHostResourceEvent make_provider_contribution_event(
    const ExtensionProviderContributionDescriptor& descriptor) {
  ExtensionHostResourceEvent event;
  event.kind = ExtensionHostResourceEventKind::kProviderDiscovered;
  event.extension_index = descriptor.extension_index;
  event.contribution_index = descriptor.contribution_index;
  event.extension_name = descriptor.extension.manifest.name;
  event.contribution_name = descriptor.name;
  event.message = "discovered extension provider: " + descriptor.name;
  return event;
}

ExtensionHostResourceEvent make_flag_contribution_event(
    const ExtensionFlagContributionDescriptor& descriptor) {
  ExtensionHostResourceEvent event;
  event.kind = ExtensionHostResourceEventKind::kFlagDiscovered;
  event.extension_index = descriptor.extension_index;
  event.contribution_index = descriptor.contribution_index;
  event.extension_name = descriptor.extension.manifest.name;
  event.contribution_name = descriptor.name;
  event.message = "discovered extension flag: " + descriptor.name;
  return event;
}

Status require_dispatch_state(const ExtensionContributionDispatch& dispatch,
                              ExtensionContributionDispatchState expected,
                              const char* operation) {
  if (dispatch.state == expected) {
    return Status::success();
  }

  return Status::failure(
      ErrorCode::invalid_argument,
      std::string(operation) + " requires dispatch state " +
          to_string(expected) + ", got " + to_string(dispatch.state));
}

Status enqueue_method_not_found_response(ExtensionContributionDispatch& dispatch,
                                         const JsonRpcRequest& request) {
  auto response = make_jsonrpc_error_response(
      request.id, kJsonRpcMethodNotFound, "method not found",
      Json{{"method", request.method}});
  if (!response) {
    return Status::failure(response.error().code, response.error().message);
  }

  return enqueue_subprocess_channel_message(dispatch.channel, response.value());
}

bool is_valid_extension_input_images(const Json& images) noexcept {
  return images.is_null() || images.is_array();
}

bool is_valid_extension_tool_input(const Json& input) noexcept {
  return input.is_object();
}

bool is_valid_extension_tool_content(const Json& content) noexcept {
  return content.is_array();
}

Result<Json> decode_before_agent_start_message(const Json& json,
                                               const char* context) {
  if (!json.is_object()) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " must be an object");
  }

  const auto custom_type =
      decode_required_string_field(json, "customType", context);
  if (!custom_type) {
    return custom_type.error();
  }
  if (custom_type.value().empty()) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'customType' must not be empty");
  }

  const auto content = decode_required_string_field(json, "content", context);
  if (!content) {
    return content.error();
  }

  if (json.contains("display") && !json.at("display").is_boolean()) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'display' must be a boolean");
  }

  return json;
}

bool is_non_negative_json_integer(const Json& value) {
  if (value.is_number_unsigned()) {
    return true;
  }
  return value.is_number_integer() && value.get<long long>() >= 0;
}

Result<std::vector<ExtensionDiscoveredResourcePath>> decode_resource_path_array(
    const LoadedExtensionRecord& extension, const Json& result,
    const char* field) {
  std::vector<ExtensionDiscoveredResourcePath> paths;
  if (!result.contains(field) || result.at(field).is_null()) {
    return paths;
  }
  if (!result.at(field).is_array()) {
    return Result<std::vector<ExtensionDiscoveredResourcePath>>::failure(
        ErrorCode::invalid_argument,
        std::string("extension resources_discover result field '") + field +
            "' must be an array");
  }

  for (const auto& value : result.at(field)) {
    if (!value.is_string()) {
      return Result<std::vector<ExtensionDiscoveredResourcePath>>::failure(
          ErrorCode::invalid_argument,
          std::string("extension resources_discover result field '") + field +
              "' must contain string paths");
    }
    const auto path = value.get<std::string>();
    if (path.empty()) {
      return Result<std::vector<ExtensionDiscoveredResourcePath>>::failure(
          ErrorCode::invalid_argument,
          std::string("extension resources_discover result field '") + field +
              "' must not contain empty paths");
    }
    paths.push_back(ExtensionDiscoveredResourcePath{path, extension.path});
  }

  return paths;
}

Result<Json> decode_required_json_object_field(const Json& json,
                                               const char* field,
                                               const char* context) {
  if (!json.contains(field) || !json.at(field).is_object()) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field '" + field + "' must be an object");
  }
  return json.at(field);
}

Result<std::string> decode_required_string_field(const Json& json,
                                                 const char* field,
                                                 const char* context) {
  if (!json.contains(field) || !json.at(field).is_string()) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field '" + field + "' must be a string");
  }
  return json.at(field).get<std::string>();
}

Result<std::optional<std::string>> decode_nullable_string_field(
    const Json& json, const char* field, const char* context) {
  if (!json.contains(field) || json.at(field).is_null()) {
    return Result<std::optional<std::string>>::success(
        std::optional<std::string>{});
  }
  if (!json.at(field).is_string()) {
    return Result<std::optional<std::string>>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field '" + field + "' must be a string or "
        "null");
  }
  const auto value = json.at(field).get<std::string>();
  if (value.empty()) {
    return Result<std::optional<std::string>>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field '" + field + "' must not be empty");
  }
  return Result<std::optional<std::string>>::success(
      std::optional<std::string>(value));
}

Status validate_tree_preparation_object(const Json& preparation) {
  if (!preparation.is_object()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree preparation must be a JSON object");
  }

  const auto target_id = decode_required_string_field(
      preparation, "targetId", "extension session_before_tree preparation");
  if (!target_id) {
    return Status(target_id.error());
  }
  if (target_id.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree preparation field 'targetId' must not "
        "be empty");
  }

  const auto old_leaf_id = decode_nullable_string_field(
      preparation, "oldLeafId", "extension session_before_tree preparation");
  if (!old_leaf_id) {
    return Status(old_leaf_id.error());
  }

  const auto common_ancestor_id = decode_nullable_string_field(
      preparation, "commonAncestorId",
      "extension session_before_tree preparation");
  if (!common_ancestor_id) {
    return Status(common_ancestor_id.error());
  }

  if (!preparation.contains("entriesToSummarize") ||
      !preparation.at("entriesToSummarize").is_array()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree preparation field "
        "'entriesToSummarize' must be a JSON array");
  }

  if (!preparation.contains("userWantsSummary") ||
      !preparation.at("userWantsSummary").is_boolean()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree preparation field "
        "'userWantsSummary' must be a boolean");
  }

  if (preparation.contains("customInstructions") &&
      !preparation.at("customInstructions").is_null() &&
      !preparation.at("customInstructions").is_string()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree preparation field "
        "'customInstructions' must be a string");
  }

  if (preparation.contains("replaceInstructions") &&
      !preparation.at("replaceInstructions").is_boolean()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree preparation field "
        "'replaceInstructions' must be a boolean");
  }

  if (preparation.contains("label") &&
      !preparation.at("label").is_null() &&
      !preparation.at("label").is_string()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree preparation field 'label' must be a "
        "string");
  }

  return Status::success();
}

Result<ExtensionSessionBeforeTreeSummary> decode_before_tree_summary(
    const Json& json, const char* context) {
  if (!json.is_object()) {
    return Result<ExtensionSessionBeforeTreeSummary>::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " must be an object");
  }

  const auto summary =
      decode_required_string_field(json, "summary", context);
  if (!summary) {
    return summary.error();
  }

  ExtensionSessionBeforeTreeSummary decoded;
  decoded.summary = summary.value();
  if (json.contains("details") && !json.at("details").is_null()) {
    decoded.details = json.at("details");
  }
  return decoded;
}

Status validate_branch_summary_entry(const Json& entry, const char* context) {
  if (!entry.is_object()) {
    return Status::failure(ErrorCode::invalid_argument,
                           std::string(context) + " must be a JSON object");
  }

  const auto type = decode_required_string_field(entry, "type", context);
  if (!type) {
    return Status(type.error());
  }
  if (type.value() != "branch_summary") {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'type' must be 'branch_summary'");
  }

  const auto id = decode_required_string_field(entry, "id", context);
  if (!id) {
    return Status(id.error());
  }
  if (id.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'id' must not be empty");
  }

  const auto parent_id = decode_nullable_string_field(entry, "parentId", context);
  if (!parent_id) {
    return Status(parent_id.error());
  }

  const auto timestamp =
      decode_required_string_field(entry, "timestamp", context);
  if (!timestamp) {
    return Status(timestamp.error());
  }
  if (timestamp.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'timestamp' must not be empty");
  }

  const auto from_id = decode_required_string_field(entry, "fromId", context);
  if (!from_id) {
    return Status(from_id.error());
  }
  if (from_id.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'fromId' must not be empty");
  }

  const auto summary =
      decode_required_string_field(entry, "summary", context);
  if (!summary) {
    return Status(summary.error());
  }

  if (entry.contains("fromHook") && !entry.at("fromHook").is_boolean()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'fromHook' must be a boolean");
  }

  return Status::success();
}

Status validate_extension_agent_message_object(const Json& message,
                                              const char* context) {
  if (!message.is_object()) {
    return Status::failure(ErrorCode::invalid_argument,
                           std::string(context) + " must be a JSON object");
  }

  const auto role = decode_required_string_field(message, "role", context);
  if (!role) {
    return Status(role.error());
  }
  if (role.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'role' must not be empty");
  }

  if (!message.contains("timestamp") ||
      !is_non_negative_json_integer(message.at("timestamp")) ||
      message.at("timestamp").get<std::int64_t>() <= 0) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'timestamp' must be a positive integer");
  }

  return Status::success();
}

Status validate_extension_tool_result_message(const Json& message,
                                              const char* context) {
  const auto valid_message =
      validate_extension_agent_message_object(message, context);
  if (!valid_message) {
    return valid_message;
  }

  const auto role = decode_required_string_field(message, "role", context);
  if (!role) {
    return Status(role.error());
  }
  if (role.value() != "toolResult") {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'role' must be 'toolResult'");
  }

  const auto tool_call_id =
      decode_required_string_field(message, "toolCallId", context);
  if (!tool_call_id) {
    return Status(tool_call_id.error());
  }
  if (tool_call_id.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'toolCallId' must not be empty");
  }

  const auto tool_name =
      decode_required_string_field(message, "toolName", context);
  if (!tool_name) {
    return Status(tool_name.error());
  }
  if (tool_name.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'toolName' must not be empty");
  }

  if (!message.contains("content") || !message.at("content").is_array()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'content' must be a JSON array");
  }

  return Status::success();
}

Status validate_extension_tool_result_array(const Json& tool_results,
                                            const char* context) {
  if (!tool_results.is_array()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " must be a JSON array");
  }

  for (std::size_t index = 0; index < tool_results.size(); ++index) {
    const auto valid = validate_extension_tool_result_message(
        tool_results.at(index),
        (std::string(context) + "[" + std::to_string(index) + "]").c_str());
    if (!valid) {
      return valid;
    }
  }

  return Status::success();
}

Status validate_extension_agent_message_array(const Json& messages,
                                              const char* context) {
  if (!messages.is_array()) {
    return Status::failure(ErrorCode::invalid_argument,
                           std::string(context) + " must be a JSON array");
  }

  for (std::size_t index = 0; index < messages.size(); ++index) {
    const auto valid = validate_extension_agent_message_object(
        messages.at(index),
        (std::string(context) + "[" + std::to_string(index) + "]").c_str());
    if (!valid) {
      return valid;
    }
  }

  return Status::success();
}

Status validate_extension_assistant_message_event(
    const Json& event, const char* context) {
  if (!event.is_object()) {
    return Status::failure(ErrorCode::invalid_argument,
                           std::string(context) + " must be a JSON object");
  }

  const auto type = decode_required_string_field(event, "type", context);
  if (!type) {
    return Status(type.error());
  }
  if (type.value().empty()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'type' must not be empty");
  }

  const auto require_content_index = [&]() -> Status {
    if (!event.contains("contentIndex") ||
        !is_non_negative_json_integer(event.at("contentIndex"))) {
      return Status::failure(
          ErrorCode::invalid_argument,
          std::string(context) +
              " field 'contentIndex' must be a non-negative integer");
    }
    return Status::success();
  };

  const auto require_string_field = [&](const char* field) -> Status {
    const auto value = decode_required_string_field(event, field, context);
    if (!value) {
      return Status(value.error());
    }
    if (value.value().empty()) {
      return Status::failure(ErrorCode::invalid_argument,
                             std::string(context) + " field '" + field +
                                 "' must not be empty");
    }
    return Status::success();
  };

  const auto require_reason = [&](std::initializer_list<const char*> allowed)
      -> Status {
    const auto reason = decode_required_string_field(event, "reason", context);
    if (!reason) {
      return Status(reason.error());
    }
    for (const auto* value : allowed) {
      if (reason.value() == value) {
        return Status::success();
      }
    }

    std::string allowed_values;
    bool first = true;
    for (const auto* value : allowed) {
      if (!first) {
        allowed_values += ", ";
      }
      allowed_values += value;
      first = false;
    }
    return Status::failure(ErrorCode::invalid_argument,
                           std::string(context) + " field 'reason' must be " +
                               allowed_values);
  };

  if (event.contains("partial") && !event.at("partial").is_object()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'partial' must be a JSON object");
  }
  if (event.contains("usage") && !event.at("usage").is_object()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        std::string(context) + " field 'usage' must be a JSON object");
  }

  const auto& kind = type.value();
  if (kind == "start") {
    return Status::success();
  }
  if (kind == "text_start" || kind == "thinking_start") {
    return require_content_index();
  }
  if (kind == "text_delta" || kind == "thinking_delta" ||
      kind == "toolcall_delta") {
    const auto content_index = require_content_index();
    if (!content_index) {
      return content_index;
    }
    return require_string_field("delta");
  }
  if (kind == "text_end" || kind == "thinking_end" || kind == "toolcall_end") {
    const auto content_index = require_content_index();
    if (!content_index) {
      return content_index;
    }
    if (event.contains("content") && !event.at("content").is_string()) {
      return Status::failure(
          ErrorCode::invalid_argument,
          std::string(context) + " field 'content' must be a string");
    }
    if (event.contains("contentSignature") &&
        !event.at("contentSignature").is_string()) {
      return Status::failure(
          ErrorCode::invalid_argument,
          std::string(context) +
              " field 'contentSignature' must be a string");
    }
    if (event.contains("toolCall") && !event.at("toolCall").is_object()) {
      return Status::failure(
          ErrorCode::invalid_argument,
          std::string(context) + " field 'toolCall' must be a JSON object");
    }
    return Status::success();
  }
  if (kind == "toolcall_start") {
    const auto content_index = require_content_index();
    if (!content_index) {
      return content_index;
    }
    const auto id = require_string_field("id");
    if (!id) {
      return id;
    }
    return require_string_field("toolName");
  }
  if (kind == "done") {
    return require_reason({"stop", "length", "toolUse"});
  }
  if (kind == "error") {
    if (event.contains("errorMessage") && !event.at("errorMessage").is_string()) {
      return Status::failure(
          ErrorCode::invalid_argument,
          std::string(context) + " field 'errorMessage' must be a string");
    }
    return require_reason({"aborted", "error"});
  }

  return Status::failure(
      ErrorCode::invalid_argument,
      std::string(context) + " field 'type' must be one of start, text_start, "
      "text_delta, text_end, thinking_start, thinking_delta, thinking_end, "
      "toolcall_start, toolcall_delta, toolcall_end, done, or error");
}

}  // namespace

std::string to_string(ExtensionContributionKind kind) {
  switch (kind) {
    case ExtensionContributionKind::kCommand:
      return "command";
    case ExtensionContributionKind::kTool:
      return "tool";
  }

  return "command";
}

std::string to_string(ExtensionContributionDispatchState state) {
  switch (state) {
    case ExtensionContributionDispatchState::kIdle:
      return "idle";
    case ExtensionContributionDispatchState::kDispatched:
      return "dispatched";
    case ExtensionContributionDispatchState::kCompletedSuccess:
      return "completed_success";
    case ExtensionContributionDispatchState::kCompletedError:
      return "completed_error";
  }

  return "idle";
}

std::string to_string(ExtensionContributionDispatchUpdateKind kind) {
  switch (kind) {
    case ExtensionContributionDispatchUpdateKind::kPending:
      return "pending";
    case ExtensionContributionDispatchUpdateKind::kDiagnostics:
      return "diagnostics";
    case ExtensionContributionDispatchUpdateKind::kCompletedSuccess:
      return "completed_success";
    case ExtensionContributionDispatchUpdateKind::kCompletedError:
      return "completed_error";
  }

  return "pending";
}

std::string to_string(ExtensionHostResourceEventKind kind) {
  switch (kind) {
    case ExtensionHostResourceEventKind::kDiagnostic:
      return "diagnostic";
    case ExtensionHostResourceEventKind::kExtensionLoaded:
      return "extension_loaded";
    case ExtensionHostResourceEventKind::kCommandDiscovered:
      return "command_discovered";
    case ExtensionHostResourceEventKind::kToolDiscovered:
      return "tool_discovered";
    case ExtensionHostResourceEventKind::kProviderDiscovered:
      return "provider_discovered";
    case ExtensionHostResourceEventKind::kFlagDiscovered:
      return "flag_discovered";
  }

  return "extension_loaded";
}

std::string to_string(ExtensionInputSource source) {
  switch (source) {
    case ExtensionInputSource::kInteractive:
      return "interactive";
    case ExtensionInputSource::kRpc:
      return "rpc";
    case ExtensionInputSource::kExtension:
      return "extension";
  }

  return "interactive";
}

std::string to_string(ExtensionInputEventAction action) {
  switch (action) {
    case ExtensionInputEventAction::kContinue:
      return "continue";
    case ExtensionInputEventAction::kTransform:
      return "transform";
    case ExtensionInputEventAction::kHandled:
      return "handled";
  }

  return "continue";
}

std::string to_string(ExtensionResourceDiscoverReason reason) {
  switch (reason) {
    case ExtensionResourceDiscoverReason::kStartup:
      return "startup";
    case ExtensionResourceDiscoverReason::kReload:
      return "reload";
  }

  return "startup";
}

std::string to_string(ExtensionSessionCompactionEventKind kind) {
  switch (kind) {
    case ExtensionSessionCompactionEventKind::kBeforeCompact:
      return "session_before_compact";
    case ExtensionSessionCompactionEventKind::kCompact:
      return "session_compact";
  }

  return "session_before_compact";
}

std::string to_string(ExtensionSessionStartReason reason) {
  switch (reason) {
    case ExtensionSessionStartReason::kStartup:
      return "startup";
    case ExtensionSessionStartReason::kReload:
      return "reload";
    case ExtensionSessionStartReason::kNew:
      return "new";
    case ExtensionSessionStartReason::kResume:
      return "resume";
    case ExtensionSessionStartReason::kFork:
      return "fork";
  }

  return "startup";
}

std::string to_string(ExtensionSessionBeforeSwitchReason reason) {
  switch (reason) {
    case ExtensionSessionBeforeSwitchReason::kNew:
      return "new";
    case ExtensionSessionBeforeSwitchReason::kResume:
      return "resume";
  }

  return "new";
}

std::string to_string(ExtensionSessionBeforeForkPosition position) {
  switch (position) {
    case ExtensionSessionBeforeForkPosition::kBefore:
      return "before";
    case ExtensionSessionBeforeForkPosition::kAt:
      return "at";
  }

  return "before";
}

std::string to_string(ExtensionSessionShutdownReason reason) {
  switch (reason) {
    case ExtensionSessionShutdownReason::kQuit:
      return "quit";
    case ExtensionSessionShutdownReason::kReload:
      return "reload";
    case ExtensionSessionShutdownReason::kNew:
      return "new";
    case ExtensionSessionShutdownReason::kResume:
      return "resume";
    case ExtensionSessionShutdownReason::kFork:
      return "fork";
  }

  return "quit";
}

Result<ExtensionResourceDiscoverReason> parse_extension_resource_discover_reason(
    std::string_view reason) {
  if (reason == "startup") {
    return ExtensionResourceDiscoverReason::kStartup;
  }
  if (reason == "reload") {
    return ExtensionResourceDiscoverReason::kReload;
  }

  return Result<ExtensionResourceDiscoverReason>::failure(
      ErrorCode::invalid_argument,
      "extension resources_discover reason must be startup or reload");
}

Result<ExtensionResourceDiscoverEventRecord>
make_extension_resource_discover_event_record(
    std::string cwd, ExtensionResourceDiscoverReason reason) {
  if (cwd.empty()) {
    return Result<ExtensionResourceDiscoverEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension resources_discover cwd is required");
  }

  ExtensionResourceDiscoverEventRecord event;
  event.cwd = std::move(cwd);
  event.reason = reason;
  return event;
}

Result<ExtensionResourceDiscoverRoutingPlan>
make_extension_resource_discover_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionResourceDiscoverEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "resources_discover";
  params["cwd"] = event.cwd;
  params["reason"] = to_string(event.reason);

  auto request = make_jsonrpc_request("qt.event.resources_discover",
                                      std::move(params), std::move(request_id));
  if (!request) {
    return Result<ExtensionResourceDiscoverRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionResourceDiscoverRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionResourceDiscoverResult> decode_extension_resource_discover_result(
    const LoadedExtensionRecord& extension, const Json& result) {
  if (result.is_null()) {
    return ExtensionResourceDiscoverResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionResourceDiscoverResult>::failure(
        ErrorCode::invalid_argument,
        "extension resources_discover result must be an object or null");
  }

  auto skill_paths =
      decode_resource_path_array(extension, result, "skillPaths");
  if (!skill_paths) {
    return skill_paths.error();
  }
  auto prompt_paths =
      decode_resource_path_array(extension, result, "promptPaths");
  if (!prompt_paths) {
    return prompt_paths.error();
  }
  auto theme_paths =
      decode_resource_path_array(extension, result, "themePaths");
  if (!theme_paths) {
    return theme_paths.error();
  }

  ExtensionResourceDiscoverResult decoded;
  decoded.skill_paths = std::move(skill_paths.value());
  decoded.prompt_paths = std::move(prompt_paths.value());
  decoded.theme_paths = std::move(theme_paths.value());
  return decoded;
}

Result<ExtensionSessionStartEventRecord> make_extension_session_start_event_record(
    ExtensionSessionStartReason reason,
    std::optional<std::string> previous_session_file) {
  const bool requires_previous =
      reason == ExtensionSessionStartReason::kNew ||
      reason == ExtensionSessionStartReason::kResume ||
      reason == ExtensionSessionStartReason::kFork;

  if (previous_session_file.has_value() && previous_session_file->empty()) {
    return Result<ExtensionSessionStartEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_start previousSessionFile must not be empty");
  }
  if (requires_previous && !previous_session_file.has_value()) {
    return Result<ExtensionSessionStartEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_start previousSessionFile is required for new, "
        "resume, and fork reasons");
  }
  if (!requires_previous && previous_session_file.has_value()) {
    return Result<ExtensionSessionStartEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_start previousSessionFile is only valid for new, "
        "resume, and fork reasons");
  }

  ExtensionSessionStartEventRecord event;
  event.reason = reason;
  event.previous_session_file = std::move(previous_session_file);
  return event;
}

Result<ExtensionSessionStartRoutingPlan> make_extension_session_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionStartEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "session_start";
  params["reason"] = to_string(event.reason);
  if (event.previous_session_file.has_value()) {
    params["previousSessionFile"] = event.previous_session_file.value();
  }

  auto request = make_jsonrpc_request("qt.event.session_start",
                                      std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionStartRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionStartRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionSessionBeforeSwitchReason>
parse_extension_session_before_switch_reason(std::string_view reason) {
  if (reason == "new") {
    return ExtensionSessionBeforeSwitchReason::kNew;
  }
  if (reason == "resume") {
    return ExtensionSessionBeforeSwitchReason::kResume;
  }

  return Result<ExtensionSessionBeforeSwitchReason>::failure(
      ErrorCode::invalid_argument,
      "extension session_before_switch reason must be new or resume");
}

Result<ExtensionSessionBeforeSwitchEventRecord>
make_extension_session_before_switch_event_record(
    ExtensionSessionBeforeSwitchReason reason,
    std::optional<std::string> target_session_file) {
  if (target_session_file.has_value() && target_session_file->empty()) {
    return Result<ExtensionSessionBeforeSwitchEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_switch targetSessionFile must not be empty");
  }

  ExtensionSessionBeforeSwitchEventRecord event;
  event.reason = reason;
  event.target_session_file = std::move(target_session_file);
  return event;
}

Result<ExtensionSessionBeforeSwitchRoutingPlan>
make_extension_session_before_switch_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeSwitchEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "session_before_switch";
  params["reason"] = to_string(event.reason);
  if (event.target_session_file.has_value()) {
    params["targetSessionFile"] = event.target_session_file.value();
  }

  auto request = make_jsonrpc_request("qt.event.session_before_switch",
                                      std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionBeforeSwitchRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionBeforeSwitchRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionSessionBeforeSwitchResult>
decode_extension_session_before_switch_result(const Json& result) {
  if (result.is_null()) {
    return ExtensionSessionBeforeSwitchResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionSessionBeforeSwitchResult>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_switch result must be an object or null");
  }

  ExtensionSessionBeforeSwitchResult decoded;
  if (result.contains("cancel")) {
    if (!result.at("cancel").is_boolean()) {
      return Result<ExtensionSessionBeforeSwitchResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_switch result field 'cancel' must be a "
          "boolean");
    }
    decoded.cancel = result.at("cancel").get<bool>();
  }
  return decoded;
}

Result<ExtensionSessionBeforeSwitchRoutingState>
make_extension_session_before_switch_routing_state() {
  return ExtensionSessionBeforeSwitchRoutingState{};
}

Status apply_extension_session_before_switch_result(
    ExtensionSessionBeforeSwitchRoutingState& state,
    const ExtensionSessionBeforeSwitchResult& result) {
  if (state.cancelled) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_switch routing is already cancelled");
  }

  if (result.cancel) {
    state.cancelled = true;
  }
  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionSessionBeforeForkPosition>
parse_extension_session_before_fork_position(std::string_view position) {
  if (position == "before") {
    return ExtensionSessionBeforeForkPosition::kBefore;
  }
  if (position == "at") {
    return ExtensionSessionBeforeForkPosition::kAt;
  }

  return Result<ExtensionSessionBeforeForkPosition>::failure(
      ErrorCode::invalid_argument,
      "extension session_before_fork position must be before or at");
}

Result<ExtensionSessionBeforeForkEventRecord>
make_extension_session_before_fork_event_record(
    std::string entry_id, ExtensionSessionBeforeForkPosition position) {
  if (entry_id.empty()) {
    return Result<ExtensionSessionBeforeForkEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_fork entryId must not be empty");
  }

  ExtensionSessionBeforeForkEventRecord event;
  event.entry_id = std::move(entry_id);
  event.position = position;
  return event;
}

Result<ExtensionSessionBeforeForkRoutingPlan>
make_extension_session_before_fork_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeForkEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "session_before_fork";
  params["entryId"] = event.entry_id;
  params["position"] = to_string(event.position);

  auto request = make_jsonrpc_request("qt.event.session_before_fork",
                                      std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionBeforeForkRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionBeforeForkRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionSessionBeforeForkResult>
decode_extension_session_before_fork_result(const Json& result) {
  if (result.is_null()) {
    return ExtensionSessionBeforeForkResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionSessionBeforeForkResult>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_fork result must be an object or null");
  }

  ExtensionSessionBeforeForkResult decoded;
  if (result.contains("cancel")) {
    if (!result.at("cancel").is_boolean()) {
      return Result<ExtensionSessionBeforeForkResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_fork result field 'cancel' must be a "
          "boolean");
    }
    decoded.cancel = result.at("cancel").get<bool>();
  }
  if (result.contains("skipConversationRestore")) {
    if (!result.at("skipConversationRestore").is_boolean()) {
      return Result<ExtensionSessionBeforeForkResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_fork result field "
          "'skipConversationRestore' must be a boolean");
    }
    decoded.skip_conversation_restore =
        result.at("skipConversationRestore").get<bool>();
  }
  return decoded;
}

Result<ExtensionSessionBeforeForkRoutingState>
make_extension_session_before_fork_routing_state() {
  return ExtensionSessionBeforeForkRoutingState{};
}

Status apply_extension_session_before_fork_result(
    ExtensionSessionBeforeForkRoutingState& state,
    const ExtensionSessionBeforeForkResult& result) {
  if (state.cancelled) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_fork routing is already cancelled");
  }

  if (result.cancel) {
    state.cancelled = true;
  }
  if (result.skip_conversation_restore) {
    state.skip_conversation_restore = true;
  }
  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionSessionBeforeTreeEventRecord>
make_extension_session_before_tree_event_record(Json preparation) {
  const auto valid = validate_tree_preparation_object(preparation);
  if (!valid) {
    return Result<ExtensionSessionBeforeTreeEventRecord>::failure(
        valid.error().code, valid.error().message);
  }

  ExtensionSessionBeforeTreeEventRecord event;
  event.preparation = std::move(preparation);
  return event;
}

Result<ExtensionSessionBeforeTreeRoutingPlan>
make_extension_session_before_tree_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeTreeEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "session_before_tree";
  params["preparation"] = event.preparation;

  auto request = make_jsonrpc_request("qt.event.session_before_tree",
                                      std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionBeforeTreeRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionBeforeTreeRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionSessionBeforeTreeResult>
decode_extension_session_before_tree_result(const Json& result) {
  if (result.is_null()) {
    return ExtensionSessionBeforeTreeResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionSessionBeforeTreeResult>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree result must be an object or null");
  }

  ExtensionSessionBeforeTreeResult decoded;
  if (result.contains("cancel")) {
    if (!result.at("cancel").is_boolean()) {
      return Result<ExtensionSessionBeforeTreeResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_tree result field 'cancel' must be a "
          "boolean");
    }
    decoded.cancel = result.at("cancel").get<bool>();
  }

  if (result.contains("summary") && !result.at("summary").is_null()) {
    const auto summary = decode_before_tree_summary(
        result.at("summary"), "extension session_before_tree result field "
                              "'summary'");
    if (!summary) {
      return summary.error();
    }
    decoded.summary = summary.value();
  }

  if (result.contains("customInstructions")) {
    if (!result.at("customInstructions").is_string()) {
      return Result<ExtensionSessionBeforeTreeResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_tree result field 'customInstructions' "
          "must be a string");
    }
    decoded.custom_instructions =
        result.at("customInstructions").get<std::string>();
  }

  if (result.contains("replaceInstructions")) {
    if (!result.at("replaceInstructions").is_boolean()) {
      return Result<ExtensionSessionBeforeTreeResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_tree result field 'replaceInstructions' "
          "must be a boolean");
    }
    decoded.replace_instructions =
        result.at("replaceInstructions").get<bool>();
  }

  if (result.contains("label")) {
    if (!result.at("label").is_string()) {
      return Result<ExtensionSessionBeforeTreeResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_tree result field 'label' must be a "
          "string");
    }
    decoded.label = result.at("label").get<std::string>();
  }

  return decoded;
}

Result<ExtensionSessionBeforeTreeRoutingState>
make_extension_session_before_tree_routing_state() {
  return ExtensionSessionBeforeTreeRoutingState{};
}

Status apply_extension_session_before_tree_result(
    ExtensionSessionBeforeTreeRoutingState& state,
    const ExtensionSessionBeforeTreeResult& result) {
  if (state.cancelled) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_tree routing is already cancelled");
  }

  state.summary = result.summary;
  state.custom_instructions = result.custom_instructions;
  state.replace_instructions = result.replace_instructions;
  state.label = result.label;
  if (result.cancel) {
    state.cancelled = true;
  }
  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionSessionTreeEventRecord> make_extension_session_tree_event_record(
    std::optional<std::string> new_leaf_id,
    std::optional<std::string> old_leaf_id, std::optional<Json> summary_entry,
    std::optional<bool> from_extension) {
  if (new_leaf_id.has_value() && new_leaf_id->empty()) {
    return Result<ExtensionSessionTreeEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_tree newLeafId must not be empty");
  }
  if (old_leaf_id.has_value() && old_leaf_id->empty()) {
    return Result<ExtensionSessionTreeEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_tree oldLeafId must not be empty");
  }
  if (summary_entry.has_value()) {
    const auto valid = validate_branch_summary_entry(
        summary_entry.value(), "extension session_tree summaryEntry");
    if (!valid) {
      return Result<ExtensionSessionTreeEventRecord>::failure(
          valid.error().code, valid.error().message);
    }
  }
  if (from_extension.has_value() && !summary_entry.has_value()) {
    return Result<ExtensionSessionTreeEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_tree fromExtension is only valid when summaryEntry "
        "is present");
  }

  ExtensionSessionTreeEventRecord event;
  event.new_leaf_id = std::move(new_leaf_id);
  event.old_leaf_id = std::move(old_leaf_id);
  event.summary_entry = std::move(summary_entry);
  event.from_extension = std::move(from_extension);
  return event;
}

Result<ExtensionSessionTreeRoutingPlan> make_extension_session_tree_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionTreeEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "session_tree";
  params["newLeafId"] =
      event.new_leaf_id.has_value() ? Json(*event.new_leaf_id) : Json(nullptr);
  params["oldLeafId"] =
      event.old_leaf_id.has_value() ? Json(*event.old_leaf_id) : Json(nullptr);
  if (event.summary_entry.has_value()) {
    params["summaryEntry"] = event.summary_entry.value();
  }
  if (event.from_extension.has_value()) {
    params["fromExtension"] = event.from_extension.value();
  }

  auto request = make_jsonrpc_request("qt.event.session_tree",
                                      std::move(params), std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionTreeRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionTreeRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionSessionBeforeCompactEventRecord>
make_extension_session_before_compact_event_record(
    Json preparation, Json branch_entries,
    std::optional<std::string> custom_instructions) {
  if (!preparation.is_object()) {
    return Result<ExtensionSessionBeforeCompactEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_compact preparation must be a JSON object");
  }
  if (!branch_entries.is_array()) {
    return Result<ExtensionSessionBeforeCompactEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_compact branchEntries must be a JSON array");
  }

  ExtensionSessionBeforeCompactEventRecord event;
  event.preparation = std::move(preparation);
  event.branch_entries = std::move(branch_entries);
  event.custom_instructions = std::move(custom_instructions);
  return event;
}

Result<ExtensionSessionBeforeCompactRoutingPlan>
make_extension_session_before_compact_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeCompactEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = to_string(ExtensionSessionCompactionEventKind::kBeforeCompact);
  params["preparation"] = event.preparation;
  params["branchEntries"] = event.branch_entries;
  if (event.custom_instructions.has_value()) {
    params["customInstructions"] = event.custom_instructions.value();
  }

  auto request = make_jsonrpc_request(
      "qt.event.session_before_compact", std::move(params),
      std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionBeforeCompactRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionBeforeCompactRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionSessionBeforeCompactResult>
decode_extension_session_before_compact_result(const Json& result) {
  if (result.is_null()) {
    return ExtensionSessionBeforeCompactResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionSessionBeforeCompactResult>::failure(
        ErrorCode::invalid_argument,
        "extension session_before_compact result must be an object or null");
  }

  ExtensionSessionBeforeCompactResult decoded;
  if (result.contains("cancel")) {
    if (!result.at("cancel").is_boolean()) {
      return Result<ExtensionSessionBeforeCompactResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_compact result field 'cancel' must be a "
          "boolean");
    }
    decoded.cancel = result.at("cancel").get<bool>();
  }

  if (result.contains("compaction") && !result.at("compaction").is_null()) {
    const auto compaction = decode_required_json_object_field(
        result, "compaction", "extension session_before_compact result");
    if (!compaction) {
      return compaction.error();
    }
    const auto summary = decode_required_string_field(
        compaction.value(), "summary",
        "extension session_before_compact compaction");
    if (!summary) {
      return summary.error();
    }
    const auto first_kept_entry_id = decode_required_string_field(
        compaction.value(), "firstKeptEntryId",
        "extension session_before_compact compaction");
    if (!first_kept_entry_id) {
      return first_kept_entry_id.error();
    }
    if (!compaction.value().contains("tokensBefore") ||
        !is_non_negative_json_integer(
            compaction.value().at("tokensBefore"))) {
      return Result<ExtensionSessionBeforeCompactResult>::failure(
          ErrorCode::invalid_argument,
          "extension session_before_compact compaction field 'tokensBefore' "
          "must be a non-negative integer");
    }
    decoded.compaction = compaction.value();
  }

  return decoded;
}

Result<ExtensionSessionBeforeCompactRoutingState>
make_extension_session_before_compact_routing_state() {
  return ExtensionSessionBeforeCompactRoutingState{};
}

Status apply_extension_session_before_compact_result(
    ExtensionSessionBeforeCompactRoutingState& state,
    const ExtensionSessionBeforeCompactResult& result) {
  if (state.cancelled) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension session_before_compact routing is already cancelled");
  }

  if (result.cancel) {
    state.cancelled = true;
  }
  if (result.compaction.has_value()) {
    state.compaction = result.compaction.value();
  }
  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionSessionCompactEventRecord>
make_extension_session_compact_event_record(Json compaction_entry,
                                            bool from_extension) {
  if (!compaction_entry.is_object()) {
    return Result<ExtensionSessionCompactEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_compact compactionEntry must be a JSON object");
  }

  ExtensionSessionCompactEventRecord event;
  event.compaction_entry = std::move(compaction_entry);
  event.from_extension = from_extension;
  return event;
}

Result<ExtensionSessionCompactRoutingPlan>
make_extension_session_compact_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionCompactEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = to_string(ExtensionSessionCompactionEventKind::kCompact);
  params["compactionEntry"] = event.compaction_entry;
  params["fromExtension"] = event.from_extension;

  auto request = make_jsonrpc_request("qt.event.session_compact",
                                      std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionCompactRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionCompactRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionSessionShutdownReason> parse_extension_session_shutdown_reason(
    std::string_view reason) {
  if (reason == "quit") {
    return ExtensionSessionShutdownReason::kQuit;
  }
  if (reason == "reload") {
    return ExtensionSessionShutdownReason::kReload;
  }
  if (reason == "new") {
    return ExtensionSessionShutdownReason::kNew;
  }
  if (reason == "resume") {
    return ExtensionSessionShutdownReason::kResume;
  }
  if (reason == "fork") {
    return ExtensionSessionShutdownReason::kFork;
  }

  return Result<ExtensionSessionShutdownReason>::failure(
      ErrorCode::invalid_argument,
      "extension session_shutdown reason must be quit, reload, new, resume, "
      "or fork");
}

Result<ExtensionSessionShutdownEventRecord>
make_extension_session_shutdown_event_record(
    ExtensionSessionShutdownReason reason,
    std::optional<std::string> target_session_file) {
  const bool requires_target =
      reason == ExtensionSessionShutdownReason::kNew ||
      reason == ExtensionSessionShutdownReason::kResume ||
      reason == ExtensionSessionShutdownReason::kFork;

  if (target_session_file.has_value() && target_session_file->empty()) {
    return Result<ExtensionSessionShutdownEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_shutdown targetSessionFile must not be empty");
  }

  if (requires_target && !target_session_file.has_value()) {
    return Result<ExtensionSessionShutdownEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_shutdown targetSessionFile is required for new, "
        "resume, and fork reasons");
  }

  if (!requires_target && target_session_file.has_value()) {
    return Result<ExtensionSessionShutdownEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension session_shutdown targetSessionFile is only valid for new, "
        "resume, and fork reasons");
  }

  ExtensionSessionShutdownEventRecord event;
  event.reason = reason;
  event.target_session_file = std::move(target_session_file);
  return event;
}

Result<ExtensionSessionShutdownRoutingPlan>
make_extension_session_shutdown_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionShutdownEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "session_shutdown";
  params["reason"] = to_string(event.reason);
  if (event.target_session_file.has_value()) {
    params["targetSessionFile"] = event.target_session_file.value();
  }

  auto request = make_jsonrpc_request("qt.event.session_shutdown",
                                      std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionSessionShutdownRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionSessionShutdownRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionBeforeAgentStartEventRecord>
make_extension_before_agent_start_event_record(std::string prompt, Json images,
                                               std::string system_prompt,
                                               Json system_prompt_options) {
  if (!is_valid_extension_input_images(images)) {
    return Result<ExtensionBeforeAgentStartEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension before_agent_start images must be null or a JSON array");
  }
  if (!system_prompt_options.is_object()) {
    return Result<ExtensionBeforeAgentStartEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension before_agent_start systemPromptOptions must be a JSON "
        "object");
  }

  ExtensionBeforeAgentStartEventRecord event;
  event.prompt = std::move(prompt);
  event.images = std::move(images);
  event.system_prompt = std::move(system_prompt);
  event.system_prompt_options = std::move(system_prompt_options);
  return event;
}

Result<ExtensionBeforeAgentStartRoutingPlan>
make_extension_before_agent_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionBeforeAgentStartEventRecord& event, Json request_id) {
  if (!is_valid_extension_input_images(event.images)) {
    return Result<ExtensionBeforeAgentStartRoutingPlan>::failure(
        ErrorCode::invalid_argument,
        "extension before_agent_start images must be null or a JSON array");
  }
  if (!event.system_prompt_options.is_object()) {
    return Result<ExtensionBeforeAgentStartRoutingPlan>::failure(
        ErrorCode::invalid_argument,
        "extension before_agent_start systemPromptOptions must be a JSON "
        "object");
  }

  Json params = Json::object();
  params["type"] = "before_agent_start";
  params["prompt"] = event.prompt;
  if (!event.images.is_null()) {
    params["images"] = event.images;
  }
  params["systemPrompt"] = event.system_prompt;
  params["systemPromptOptions"] = event.system_prompt_options;

  auto request = make_jsonrpc_request("qt.event.before_agent_start",
                                      std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionBeforeAgentStartRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionBeforeAgentStartRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionBeforeAgentStartRoutingState>
make_extension_before_agent_start_routing_state(
    ExtensionBeforeAgentStartEventRecord event) {
  if (!is_valid_extension_input_images(event.images)) {
    return Result<ExtensionBeforeAgentStartRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension before_agent_start images must be null or a JSON array");
  }
  if (!event.system_prompt_options.is_object()) {
    return Result<ExtensionBeforeAgentStartRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension before_agent_start systemPromptOptions must be a JSON "
        "object");
  }

  ExtensionBeforeAgentStartRoutingState state;
  state.current_event = std::move(event);
  return state;
}

Result<ExtensionBeforeAgentStartEventResult>
decode_extension_before_agent_start_result(const Json& result) {
  if (result.is_null()) {
    return ExtensionBeforeAgentStartEventResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionBeforeAgentStartEventResult>::failure(
        ErrorCode::invalid_argument,
        "extension before_agent_start result must be an object or null");
  }

  ExtensionBeforeAgentStartEventResult decoded;
  if (result.contains("message") && !result.at("message").is_null()) {
    const auto message = decode_before_agent_start_message(
        result.at("message"),
        "extension before_agent_start result field 'message'");
    if (!message) {
      return message.error();
    }
    decoded.message = message.value();
  }

  if (result.contains("systemPrompt")) {
    if (!result.at("systemPrompt").is_string()) {
      return Result<ExtensionBeforeAgentStartEventResult>::failure(
          ErrorCode::invalid_argument,
          "extension before_agent_start result field 'systemPrompt' must be a "
          "string");
    }
    decoded.system_prompt = result.at("systemPrompt").get<std::string>();
  }

  return decoded;
}

Status apply_extension_before_agent_start_result(
    ExtensionBeforeAgentStartRoutingState& state,
    const ExtensionBeforeAgentStartEventResult& result) {
  if (result.message.has_value()) {
    const auto validated = decode_before_agent_start_message(
        result.message.value(),
        "extension before_agent_start result field 'message'");
    if (!validated) {
      return Status(validated.error());
    }
    state.injected_messages.push_back(validated.value());
  }

  if (result.system_prompt.has_value()) {
    state.current_event.system_prompt = result.system_prompt.value();
  }

  ++state.applied_result_count;
  return Status::success();
}

ExtensionAgentStartEventRecord make_extension_agent_start_event_record() {
  return ExtensionAgentStartEventRecord{};
}

Result<ExtensionAgentStartRoutingPlan> make_extension_agent_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionAgentStartEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "agent_start";

  auto request = make_jsonrpc_request("qt.event.agent_start", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionAgentStartRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionAgentStartRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionAgentEndEventRecord> make_extension_agent_end_event_record(
    Json messages) {
  const auto valid_messages = validate_extension_agent_message_array(
      messages, "extension agent_end messages");
  if (!valid_messages) {
    return Result<ExtensionAgentEndEventRecord>::failure(
        valid_messages.error().code, valid_messages.error().message);
  }

  ExtensionAgentEndEventRecord event;
  event.messages = std::move(messages);
  return event;
}

Result<ExtensionAgentEndRoutingPlan> make_extension_agent_end_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionAgentEndEventRecord& event, Json request_id) {
  const auto valid_messages = validate_extension_agent_message_array(
      event.messages, "extension agent_end messages");
  if (!valid_messages) {
    return Result<ExtensionAgentEndRoutingPlan>::failure(
        valid_messages.error().code, valid_messages.error().message);
  }

  Json params = Json::object();
  params["type"] = "agent_end";
  params["messages"] = event.messages;

  auto request = make_jsonrpc_request("qt.event.agent_end", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionAgentEndRoutingPlan>::failure(request.error().code,
                                                        request.error().message);
  }

  ExtensionAgentEndRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionTurnStartEventRecord> make_extension_turn_start_event_record(
    std::int64_t turn_index, std::int64_t timestamp) {
  if (turn_index < 0) {
    return Result<ExtensionTurnStartEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension turn_start turnIndex must be a non-negative integer");
  }
  if (timestamp <= 0) {
    return Result<ExtensionTurnStartEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension turn_start timestamp must be a positive integer");
  }

  ExtensionTurnStartEventRecord event;
  event.turn_index = static_cast<std::size_t>(turn_index);
  event.timestamp = timestamp;
  return event;
}

Result<ExtensionTurnStartRoutingPlan> make_extension_turn_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionTurnStartEventRecord& event, Json request_id) {
  if (event.timestamp <= 0) {
    return Result<ExtensionTurnStartRoutingPlan>::failure(
        ErrorCode::invalid_argument,
        "extension turn_start timestamp must be a positive integer");
  }

  Json params = Json::object();
  params["type"] = "turn_start";
  params["turnIndex"] = event.turn_index;
  params["timestamp"] = event.timestamp;

  auto request = make_jsonrpc_request("qt.event.turn_start", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionTurnStartRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionTurnStartRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionTurnEndEventRecord> make_extension_turn_end_event_record(
    std::int64_t turn_index, Json message, Json tool_results) {
  if (turn_index < 0) {
    return Result<ExtensionTurnEndEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension turn_end turnIndex must be a non-negative integer");
  }

  const auto valid_message = validate_extension_agent_message_object(
      message, "extension turn_end message");
  if (!valid_message) {
    return Result<ExtensionTurnEndEventRecord>::failure(
        valid_message.error().code, valid_message.error().message);
  }

  const auto valid_tool_results = validate_extension_tool_result_array(
      tool_results, "extension turn_end toolResults");
  if (!valid_tool_results) {
    return Result<ExtensionTurnEndEventRecord>::failure(
        valid_tool_results.error().code, valid_tool_results.error().message);
  }

  ExtensionTurnEndEventRecord event;
  event.turn_index = static_cast<std::size_t>(turn_index);
  event.message = std::move(message);
  event.tool_results = std::move(tool_results);
  return event;
}

Result<ExtensionTurnEndRoutingPlan> make_extension_turn_end_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionTurnEndEventRecord& event, Json request_id) {
  const auto valid_message = validate_extension_agent_message_object(
      event.message, "extension turn_end message");
  if (!valid_message) {
    return Result<ExtensionTurnEndRoutingPlan>::failure(
        valid_message.error().code, valid_message.error().message);
  }

  const auto valid_tool_results = validate_extension_tool_result_array(
      event.tool_results, "extension turn_end toolResults");
  if (!valid_tool_results) {
    return Result<ExtensionTurnEndRoutingPlan>::failure(
        valid_tool_results.error().code, valid_tool_results.error().message);
  }

  Json params = Json::object();
  params["type"] = "turn_end";
  params["turnIndex"] = event.turn_index;
  params["message"] = event.message;
  params["toolResults"] = event.tool_results;

  auto request = make_jsonrpc_request("qt.event.turn_end", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionTurnEndRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionTurnEndRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionMessageStartEventRecord> make_extension_message_start_event_record(
    Json message) {
  const auto valid_message = validate_extension_agent_message_object(
      message, "extension message_start message");
  if (!valid_message) {
    return Result<ExtensionMessageStartEventRecord>::failure(
        valid_message.error().code, valid_message.error().message);
  }

  ExtensionMessageStartEventRecord event;
  event.message = std::move(message);
  return event;
}

Result<ExtensionMessageStartRoutingPlan> make_extension_message_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionMessageStartEventRecord& event, Json request_id) {
  const auto valid_message = validate_extension_agent_message_object(
      event.message, "extension message_start message");
  if (!valid_message) {
    return Result<ExtensionMessageStartRoutingPlan>::failure(
        valid_message.error().code, valid_message.error().message);
  }

  Json params = Json::object();
  params["type"] = "message_start";
  params["message"] = event.message;

  auto request = make_jsonrpc_request("qt.event.message_start",
                                      std::move(params), std::move(request_id));
  if (!request) {
    return Result<ExtensionMessageStartRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionMessageStartRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionMessageUpdateEventRecord> make_extension_message_update_event_record(
    Json message, Json assistant_message_event) {
  const auto valid_message = validate_extension_agent_message_object(
      message, "extension message_update message");
  if (!valid_message) {
    return Result<ExtensionMessageUpdateEventRecord>::failure(
        valid_message.error().code, valid_message.error().message);
  }
  const auto role = decode_required_string_field(
      message, "role", "extension message_update message");
  if (!role) {
    return role.error();
  }
  if (role.value() != "assistant") {
    return Result<ExtensionMessageUpdateEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension message_update message field 'role' must be 'assistant'");
  }

  const auto valid_event = validate_extension_assistant_message_event(
      assistant_message_event, "extension message_update assistantMessageEvent");
  if (!valid_event) {
    return Result<ExtensionMessageUpdateEventRecord>::failure(
        valid_event.error().code, valid_event.error().message);
  }

  ExtensionMessageUpdateEventRecord event;
  event.message = std::move(message);
  event.assistant_message_event = std::move(assistant_message_event);
  return event;
}

Result<ExtensionMessageUpdateRoutingPlan>
make_extension_message_update_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionMessageUpdateEventRecord& event, Json request_id) {
  const auto valid_message = validate_extension_agent_message_object(
      event.message, "extension message_update message");
  if (!valid_message) {
    return Result<ExtensionMessageUpdateRoutingPlan>::failure(
        valid_message.error().code, valid_message.error().message);
  }
  const auto role = decode_required_string_field(
      event.message, "role", "extension message_update message");
  if (!role) {
    return role.error();
  }
  if (role.value() != "assistant") {
    return Result<ExtensionMessageUpdateRoutingPlan>::failure(
        ErrorCode::invalid_argument,
        "extension message_update message field 'role' must be 'assistant'");
  }

  const auto valid_event = validate_extension_assistant_message_event(
      event.assistant_message_event,
      "extension message_update assistantMessageEvent");
  if (!valid_event) {
    return Result<ExtensionMessageUpdateRoutingPlan>::failure(
        valid_event.error().code, valid_event.error().message);
  }

  Json params = Json::object();
  params["type"] = "message_update";
  params["message"] = event.message;
  params["assistantMessageEvent"] = event.assistant_message_event;

  auto request = make_jsonrpc_request("qt.event.message_update",
                                      std::move(params), std::move(request_id));
  if (!request) {
    return Result<ExtensionMessageUpdateRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionMessageUpdateRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionMessageEndEventRecord> make_extension_message_end_event_record(
    Json message) {
  const auto valid_message = validate_extension_agent_message_object(
      message, "extension message_end message");
  if (!valid_message) {
    return Result<ExtensionMessageEndEventRecord>::failure(
        valid_message.error().code, valid_message.error().message);
  }

  ExtensionMessageEndEventRecord event;
  event.message = std::move(message);
  return event;
}

Result<ExtensionMessageEndRoutingPlan> make_extension_message_end_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionMessageEndEventRecord& event, Json request_id) {
  const auto valid_message = validate_extension_agent_message_object(
      event.message, "extension message_end message");
  if (!valid_message) {
    return Result<ExtensionMessageEndRoutingPlan>::failure(
        valid_message.error().code, valid_message.error().message);
  }

  Json params = Json::object();
  params["type"] = "message_end";
  params["message"] = event.message;

  auto request = make_jsonrpc_request("qt.event.message_end", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionMessageEndRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionMessageEndRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionMessageEndRoutingState> make_extension_message_end_routing_state(
    ExtensionMessageEndEventRecord event) {
  const auto valid_message = validate_extension_agent_message_object(
      event.message, "extension message_end message");
  if (!valid_message) {
    return Result<ExtensionMessageEndRoutingState>::failure(
        valid_message.error().code, valid_message.error().message);
  }

  ExtensionMessageEndRoutingState state;
  state.current_event = std::move(event);
  return state;
}

Result<ExtensionMessageEndEventResult> decode_extension_message_end_result(
    const Json& result) {
  if (result.is_null()) {
    return ExtensionMessageEndEventResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionMessageEndEventResult>::failure(
        ErrorCode::invalid_argument,
        "extension message_end result must be a JSON object or null");
  }

  ExtensionMessageEndEventResult decoded;
  if (result.contains("message") && !result.at("message").is_null()) {
    const auto valid_message = validate_extension_agent_message_object(
        result.at("message"), "extension message_end result field 'message'");
    if (!valid_message) {
      return Result<ExtensionMessageEndEventResult>::failure(
          valid_message.error().code, valid_message.error().message);
    }
    decoded.message = result.at("message");
  }

  return decoded;
}

Status apply_extension_message_end_result(
    ExtensionMessageEndRoutingState& state,
    const ExtensionMessageEndEventResult& result) {
  if (result.message.has_value()) {
    const auto valid_message = validate_extension_agent_message_object(
        result.message.value(), "extension message_end result field 'message'");
    if (!valid_message) {
      return valid_message;
    }

    const auto current_role = decode_required_string_field(
        state.current_event.message, "role", "extension message_end message");
    if (!current_role) {
      return Status(current_role.error());
    }
    const auto replacement_role = decode_required_string_field(
        result.message.value(), "role",
        "extension message_end result field 'message'");
    if (!replacement_role) {
      return Status(replacement_role.error());
    }
    if (replacement_role.value() != current_role.value()) {
      return Status::failure(
          ErrorCode::invalid_argument,
          "extension message_end handlers must return a message with the same "
          "role");
    }

    state.current_event.message = result.message.value();
  }

  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionInputSource> parse_extension_input_source(
    std::string_view source) {
  if (source == "interactive") {
    return ExtensionInputSource::kInteractive;
  }
  if (source == "rpc") {
    return ExtensionInputSource::kRpc;
  }
  if (source == "extension") {
    return ExtensionInputSource::kExtension;
  }

  return Result<ExtensionInputSource>::failure(
      ErrorCode::invalid_argument,
      "extension input source must be interactive, rpc, or extension");
}

Result<ExtensionInputEventRecord> make_extension_input_event_record(
    std::string text, Json images, ExtensionInputSource source) {
  if (!is_valid_extension_input_images(images)) {
    return Result<ExtensionInputEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension input event images must be null or a JSON array");
  }

  ExtensionInputEventRecord event;
  event.text = std::move(text);
  event.images = std::move(images);
  event.source = source;
  return event;
}

Result<ExtensionInputEventRoutingPlan> make_extension_input_event_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionInputEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "input";
  params["text"] = event.text;
  params["source"] = to_string(event.source);
  if (!event.images.is_null()) {
    params["images"] = event.images;
  }

  auto request = make_jsonrpc_request("qt.event.input", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionInputEventRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionInputEventRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionInputEventRoutingState> make_extension_input_event_routing_state(
    ExtensionInputEventRecord event) {
  if (!is_valid_extension_input_images(event.images)) {
    return Result<ExtensionInputEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension input event images must be null or a JSON array");
  }

  ExtensionInputEventRoutingState state;
  state.current_event = std::move(event);
  return state;
}

Result<ExtensionInputEventResult> decode_extension_input_event_result(
    const Json& result) {
  if (!result.is_object()) {
    return Result<ExtensionInputEventResult>::failure(
        ErrorCode::invalid_argument,
        "extension input event result must be a JSON object");
  }

  auto action_text =
      decode_required_string_field(result, "action",
                                   "extension input event result");
  if (!action_text) {
    return action_text.error();
  }

  ExtensionInputEventResult decoded;
  if (action_text.value() == "continue") {
    decoded.action = ExtensionInputEventAction::kContinue;
    return decoded;
  }
  if (action_text.value() == "handled") {
    decoded.action = ExtensionInputEventAction::kHandled;
    return decoded;
  }
  if (action_text.value() != "transform") {
    return Result<ExtensionInputEventResult>::failure(
        ErrorCode::invalid_argument,
        "extension input event result action must be continue, transform, or "
        "handled");
  }

  decoded.action = ExtensionInputEventAction::kTransform;
  auto text =
      decode_required_string_field(result, "text",
                                   "extension input transform result");
  if (!text) {
    return text.error();
  }
  decoded.text = std::move(text.value());

  if (result.contains("images")) {
    if (!is_valid_extension_input_images(result.at("images"))) {
      return Result<ExtensionInputEventResult>::failure(
          ErrorCode::invalid_argument,
          "extension input transform result images must be null or a JSON array");
    }
    decoded.images = result.at("images");
  }

  return decoded;
}

Status apply_extension_input_event_result(
    ExtensionInputEventRoutingState& state,
    const ExtensionInputEventResult& result) {
  if (state.handled) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "extension input event routing is already handled");
  }

  switch (result.action) {
    case ExtensionInputEventAction::kContinue:
      break;
    case ExtensionInputEventAction::kHandled:
      state.handled = true;
      break;
    case ExtensionInputEventAction::kTransform:
      if (!result.text.has_value()) {
        return Status::failure(
            ErrorCode::invalid_argument,
            "extension input transform result requires text");
      }
      if (!is_valid_extension_input_images(result.images)) {
        return Status::failure(
            ErrorCode::invalid_argument,
            "extension input transform result images must be null or a JSON "
            "array");
      }
      state.current_event.text = result.text.value();
      if (!result.images.is_null()) {
        state.current_event.images = result.images;
      }
      break;
  }

  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionToolCallEventRecord> make_extension_tool_call_event_record(
    std::string tool_call_id, std::string tool_name, Json input) {
  if (tool_call_id.empty()) {
    return Result<ExtensionToolCallEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension tool_call event toolCallId is required");
  }
  if (tool_name.empty()) {
    return Result<ExtensionToolCallEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension tool_call event toolName is required");
  }
  if (!is_valid_extension_tool_input(input)) {
    return Result<ExtensionToolCallEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension tool_call event input must be a JSON object");
  }

  ExtensionToolCallEventRecord event;
  event.tool_call_id = std::move(tool_call_id);
  event.tool_name = std::move(tool_name);
  event.input = std::move(input);
  return event;
}

Result<ExtensionToolCallEventRoutingPlan>
make_extension_tool_call_event_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionToolCallEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "tool_call";
  params["toolCallId"] = event.tool_call_id;
  params["toolName"] = event.tool_name;
  params["input"] = event.input;

  auto request = make_jsonrpc_request("qt.event.tool_call", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionToolCallEventRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionToolCallEventRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionToolCallEventRoutingState>
make_extension_tool_call_event_routing_state(ExtensionToolCallEventRecord event) {
  if (event.tool_call_id.empty()) {
    return Result<ExtensionToolCallEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension tool_call event toolCallId is required");
  }
  if (event.tool_name.empty()) {
    return Result<ExtensionToolCallEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension tool_call event toolName is required");
  }
  if (!is_valid_extension_tool_input(event.input)) {
    return Result<ExtensionToolCallEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension tool_call event input must be a JSON object");
  }

  ExtensionToolCallEventRoutingState state;
  state.current_event = std::move(event);
  return state;
}

Result<ExtensionToolCallEventResult> decode_extension_tool_call_event_result(
    const Json& result) {
  if (result.is_null()) {
    return ExtensionToolCallEventResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionToolCallEventResult>::failure(
        ErrorCode::invalid_argument,
        "extension tool_call result must be a JSON object or null");
  }

  ExtensionToolCallEventResult decoded;
  if (result.contains("block")) {
    if (!result.at("block").is_boolean()) {
      return Result<ExtensionToolCallEventResult>::failure(
          ErrorCode::invalid_argument,
          "extension tool_call result field 'block' must be a boolean");
    }
    decoded.block = result.at("block").get<bool>();
  }
  if (result.contains("reason") && !result.at("reason").is_null()) {
    if (!result.at("reason").is_string()) {
      return Result<ExtensionToolCallEventResult>::failure(
          ErrorCode::invalid_argument,
          "extension tool_call result field 'reason' must be a string");
    }
    decoded.reason = result.at("reason").get<std::string>();
  }
  if (result.contains("input") && !result.at("input").is_null()) {
    if (!result.at("input").is_object()) {
      return Result<ExtensionToolCallEventResult>::failure(
          ErrorCode::invalid_argument,
          "extension tool_call result field 'input' must be an object");
    }
    decoded.input = result.at("input");
  }

  return decoded;
}

Status apply_extension_tool_call_event_result(
    ExtensionToolCallEventRoutingState& state,
    const ExtensionToolCallEventResult& result) {
  if (state.blocked) {
    return Status::failure(ErrorCode::invalid_argument,
                           "extension tool_call routing is already blocked");
  }

  if (result.block) {
    state.blocked = true;
    state.block_reason = result.reason;
  }
  if (result.input.has_value()) {
    state.current_event.input = *result.input;
  }
  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionToolResultEventRecord> make_extension_tool_result_event_record(
    std::string tool_call_id, std::string tool_name, Json input, Json content,
    Json details, bool is_error) {
  if (tool_call_id.empty()) {
    return Result<ExtensionToolResultEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event toolCallId is required");
  }
  if (tool_name.empty()) {
    return Result<ExtensionToolResultEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event toolName is required");
  }
  if (!is_valid_extension_tool_input(input)) {
    return Result<ExtensionToolResultEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event input must be a JSON object");
  }
  if (!is_valid_extension_tool_content(content)) {
    return Result<ExtensionToolResultEventRecord>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event content must be a JSON array");
  }

  ExtensionToolResultEventRecord event;
  event.tool_call_id = std::move(tool_call_id);
  event.tool_name = std::move(tool_name);
  event.input = std::move(input);
  event.content = std::move(content);
  event.details = std::move(details);
  event.is_error = is_error;
  return event;
}

Result<ExtensionToolResultEventRoutingPlan>
make_extension_tool_result_event_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionToolResultEventRecord& event, Json request_id) {
  Json params = Json::object();
  params["type"] = "tool_result";
  params["toolCallId"] = event.tool_call_id;
  params["toolName"] = event.tool_name;
  params["input"] = event.input;
  params["content"] = event.content;
  params["details"] = event.details;
  params["isError"] = event.is_error;

  auto request = make_jsonrpc_request("qt.event.tool_result", std::move(params),
                                      std::move(request_id));
  if (!request) {
    return Result<ExtensionToolResultEventRoutingPlan>::failure(
        request.error().code, request.error().message);
  }

  ExtensionToolResultEventRoutingPlan plan;
  plan.extension = extension;
  plan.event = event;
  plan.request = std::move(request.value());
  return plan;
}

Result<ExtensionToolResultEventRoutingState>
make_extension_tool_result_event_routing_state(
    ExtensionToolResultEventRecord event) {
  if (event.tool_call_id.empty()) {
    return Result<ExtensionToolResultEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event toolCallId is required");
  }
  if (event.tool_name.empty()) {
    return Result<ExtensionToolResultEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event toolName is required");
  }
  if (!is_valid_extension_tool_input(event.input)) {
    return Result<ExtensionToolResultEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event input must be a JSON object");
  }
  if (!is_valid_extension_tool_content(event.content)) {
    return Result<ExtensionToolResultEventRoutingState>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result event content must be a JSON array");
  }

  ExtensionToolResultEventRoutingState state;
  state.current_event = std::move(event);
  return state;
}

Result<ExtensionToolResultEventResult> decode_extension_tool_result_event_result(
    const Json& result) {
  if (result.is_null()) {
    return ExtensionToolResultEventResult{};
  }
  if (!result.is_object()) {
    return Result<ExtensionToolResultEventResult>::failure(
        ErrorCode::invalid_argument,
        "extension tool_result result must be a JSON object or null");
  }

  ExtensionToolResultEventResult decoded;
  if (result.contains("content")) {
    if (!is_valid_extension_tool_content(result.at("content"))) {
      return Result<ExtensionToolResultEventResult>::failure(
          ErrorCode::invalid_argument,
          "extension tool_result result field 'content' must be a JSON array");
    }
    decoded.content = result.at("content");
  }
  if (result.contains("details")) {
    decoded.details = result.at("details");
  }
  if (result.contains("isError")) {
    if (!result.at("isError").is_boolean()) {
      return Result<ExtensionToolResultEventResult>::failure(
          ErrorCode::invalid_argument,
          "extension tool_result result field 'isError' must be a boolean");
    }
    decoded.is_error = result.at("isError").get<bool>();
  }

  return decoded;
}

Status apply_extension_tool_result_event_result(
    ExtensionToolResultEventRoutingState& state,
    const ExtensionToolResultEventResult& result) {
  if (result.content.has_value()) {
    if (!is_valid_extension_tool_content(result.content.value())) {
      return Status::failure(
          ErrorCode::invalid_argument,
          "extension tool_result result field 'content' must be a JSON array");
    }
    state.current_event.content = result.content.value();
  }
  if (result.details.has_value()) {
    state.current_event.details = result.details.value();
  }
  if (result.is_error.has_value()) {
    state.current_event.is_error = result.is_error.value();
  }

  ++state.applied_result_count;
  return Status::success();
}

Result<ExtensionContributionRegistry> make_extension_contribution_registry(
    const std::vector<LoadedExtensionRecord>& loaded_extensions) {
  ExtensionContributionRegistry registry;
  registry.loaded_extensions = loaded_extensions;
  append_command_contributions(registry.commands, registry.loaded_extensions);
  append_tool_contributions(registry.tools, registry.loaded_extensions);
  append_provider_contributions(registry.providers, registry.loaded_extensions);
  append_flag_contributions(registry.flags, registry.loaded_extensions);
  return registry;
}

Result<ExtensionHostRuntime> make_extension_host_runtime(
    const ExtensionLoadResult& load_result) {
  auto registry =
      make_extension_contribution_registry(load_result.loaded_extensions);
  if (!registry) {
    return registry.error();
  }

  ExtensionHostRuntime runtime;
  runtime.registry = std::move(registry.value());
  runtime.resource_events.reserve(
      load_result.diagnostics.size() + runtime.registry.loaded_extensions.size() +
      runtime.registry.commands.size() + runtime.registry.tools.size() +
      runtime.registry.providers.size() + runtime.registry.flags.size());

  for (const auto& diagnostic : load_result.diagnostics) {
    runtime.resource_events.push_back(make_diagnostic_event(diagnostic));
  }

  for (std::size_t extension_index = 0;
       extension_index < runtime.registry.loaded_extensions.size();
       ++extension_index) {
    const auto& extension = runtime.registry.loaded_extensions[extension_index];
    runtime.resource_events.push_back(
        make_loaded_event(extension, extension_index));

    for (const auto& command : runtime.registry.commands) {
      if (command.extension_index == extension_index) {
        runtime.resource_events.push_back(make_contribution_event(command));
      }
    }
    for (const auto& tool : runtime.registry.tools) {
      if (tool.extension_index == extension_index) {
        runtime.resource_events.push_back(make_contribution_event(tool));
      }
    }
    for (const auto& provider : runtime.registry.providers) {
      if (provider.extension_index == extension_index) {
        runtime.resource_events.push_back(
            make_provider_contribution_event(provider));
      }
    }
    for (const auto& flag : runtime.registry.flags) {
      if (flag.extension_index == extension_index) {
        runtime.resource_events.push_back(make_flag_contribution_event(flag));
      }
    }
  }

  return runtime;
}

const std::vector<ExtensionHostResourceEvent>&
extension_host_resource_events(const ExtensionHostRuntime& runtime) noexcept {
  return runtime.resource_events;
}

Result<ExtensionContributionDescriptor> find_extension_command_contribution(
    const ExtensionContributionRegistry& registry, std::string_view name) {
  return find_contribution(registry, ExtensionContributionKind::kCommand, name);
}

Result<ExtensionContributionDescriptor> find_extension_tool_contribution(
    const ExtensionContributionRegistry& registry, std::string_view name) {
  return find_contribution(registry, ExtensionContributionKind::kTool, name);
}

Result<ExtensionContributionInvocationPlan> make_extension_command_invocation_plan(
    const ExtensionContributionRegistry& registry, std::string_view name,
    Json arguments, Json request_id) {
  return make_invocation_plan(registry, ExtensionContributionKind::kCommand, name,
                              std::move(arguments), std::move(request_id));
}

Result<ExtensionContributionInvocationPlan> make_extension_tool_invocation_plan(
    const ExtensionContributionRegistry& registry, std::string_view name,
    Json arguments, Json request_id) {
  return make_invocation_plan(registry, ExtensionContributionKind::kTool, name,
                              std::move(arguments), std::move(request_id));
}

Result<ExtensionContributionDispatch> make_extension_contribution_dispatch(
    const ExtensionContributionInvocationPlan& invocation_plan) {
  const auto runner = make_extension_runner_lifecycle(
      invocation_plan.contribution.extension);
  if (!runner) {
    return runner.error();
  }

  const auto channel = make_subprocess_channel();
  if (!channel) {
    return channel.error();
  }

  ExtensionContributionDispatch dispatch;
  dispatch.invocation_plan = invocation_plan;
  dispatch.runner = runner.value();
  dispatch.channel = channel.value();
  return dispatch;
}

Result<ExtensionContributionDispatch> make_extension_host_command_dispatch(
    ExtensionHostRuntime& runtime, std::string_view name, Json arguments) {
  auto plan = make_extension_command_invocation_plan(
      runtime.registry, name, std::move(arguments),
      make_host_request_id(runtime.next_request_id));
  if (!plan) {
    return plan.error();
  }

  auto dispatch = make_extension_contribution_dispatch(plan.value());
  if (!dispatch) {
    return dispatch.error();
  }

  ++runtime.next_request_id;
  return dispatch.value();
}

Result<ExtensionContributionDispatch> make_extension_host_tool_dispatch(
    ExtensionHostRuntime& runtime, std::string_view name, Json arguments) {
  auto plan = make_extension_tool_invocation_plan(
      runtime.registry, name, std::move(arguments),
      make_host_request_id(runtime.next_request_id));
  if (!plan) {
    return plan.error();
  }

  auto dispatch = make_extension_contribution_dispatch(plan.value());
  if (!dispatch) {
    return dispatch.error();
  }

  ++runtime.next_request_id;
  return dispatch.value();
}

Result<Json> extract_extension_contribution_result(
    const ExtensionContributionDispatchUpdate& update) {
  if (update.kind == ExtensionContributionDispatchUpdateKind::kCompletedSuccess &&
      update.response.has_value()) {
    return update.response->result;
  }

  if (update.kind == ExtensionContributionDispatchUpdateKind::kCompletedError &&
      update.error_response.has_value()) {
    const auto& error = update.error_response->error;
    const std::string message =
        error.value("message", std::string("extension contribution failed"));
    return Result<Json>::failure(ErrorCode::invalid_argument, message);
  }

  return Result<Json>::failure(
      ErrorCode::invalid_argument,
      "extension contribution result is not completed");
}

Result<ExtensionContributionResult> decode_extension_contribution_result(
    const ExtensionContributionDispatch& dispatch,
    const ExtensionContributionDispatchUpdate& update,
    const std::vector<std::string>& diagnostics) {
  if (!update.completed()) {
    return Result<ExtensionContributionResult>::failure(
        ErrorCode::invalid_argument,
        "extension contribution result is not completed");
  }

  ExtensionContributionResult result;
  result.kind = dispatch.invocation_plan.contribution.kind;
  result.contribution_name = dispatch.invocation_plan.contribution.name;
  result.request_id = dispatch.invocation_plan.request.id;
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
      return Result<ExtensionContributionResult>::failure(
          decoded_error.error().code, decoded_error.error().message);
    }

    result.ok = false;
    result.error = decoded_error.value();
    return result;
  }

  return Result<ExtensionContributionResult>::failure(
      ErrorCode::invalid_argument,
      "extension contribution result is missing completed response payload");
}

Status request_extension_contribution_dispatch_start(
    ExtensionContributionDispatch& dispatch) {
  const Status status = require_synced_stopped(
      dispatch, "request_extension_contribution_dispatch_start");
  if (!status) {
    return status;
  }

  const Status runner = request_extension_start(dispatch.runner);
  if (!runner) {
    return runner;
  }

  const Status channel = request_subprocess_channel_start(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return Status::success();
}

Status confirm_extension_contribution_dispatch_started(
    ExtensionContributionDispatch& dispatch, int process_id) {
  const Status status = require_synced_starting(
      dispatch, "confirm_extension_contribution_dispatch_started");
  if (!status) {
    return status;
  }

  const Status runner = confirm_extension_started(dispatch.runner, process_id);
  if (!runner) {
    return runner;
  }

  const Status channel = confirm_subprocess_channel_started(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return Status::success();
}

Status dispatch_extension_contribution_invocation(
    ExtensionContributionDispatch& dispatch) {
  const Status status = require_synced_running(
      dispatch, "dispatch_extension_contribution_invocation");
  if (!status) {
    return status;
  }
  const Status request_state = require_dispatch_state(
      dispatch, ExtensionContributionDispatchState::kIdle,
      "dispatch_extension_contribution_invocation");
  if (!request_state) {
    return request_state;
  }

  const Status enqueue = enqueue_subprocess_channel_message(
      dispatch.channel, dispatch.invocation_plan.request);
  if (!enqueue) {
    return enqueue;
  }

  dispatch.state = ExtensionContributionDispatchState::kDispatched;
  ++dispatch.dispatch_count;
  return Status::success();
}

Result<ExtensionContributionDispatchUpdate>
push_extension_contribution_dispatch_stdout(ExtensionContributionDispatch& dispatch,
                                            std::string_view bytes) {
  const auto frames = push_subprocess_channel_stdout(dispatch.channel, bytes);
  if (!frames) {
    return frames.error();
  }

  for (const auto& frame : frames.value()) {
    switch (frame.kind) {
      case SubprocessChannelFrameKind::kJsonRpcResponse: {
        const auto& response = std::get<JsonRpcResponse>(frame.payload);
        if (matches_invocation_request(dispatch, response.id)) {
          if (dispatch.state !=
              ExtensionContributionDispatchState::kDispatched) {
            break;
          }
          dispatch.state = ExtensionContributionDispatchState::kCompletedSuccess;
          return make_completed_success_update(response);
        }
        break;
      }
      case SubprocessChannelFrameKind::kJsonRpcErrorResponse: {
        const auto& response = std::get<JsonRpcErrorResponse>(frame.payload);
        if (matches_invocation_request(dispatch, response.id)) {
          if (dispatch.state !=
              ExtensionContributionDispatchState::kDispatched) {
            break;
          }
          dispatch.state = ExtensionContributionDispatchState::kCompletedError;
          return make_completed_error_update(response);
        }
        break;
      }
      case SubprocessChannelFrameKind::kJsonRpcRequest: {
        const auto& request = std::get<JsonRpcRequest>(frame.payload);
        const Status response =
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

  return pending_dispatch_update();
}

Result<ExtensionContributionDispatchUpdate>
push_extension_contribution_dispatch_stderr(ExtensionContributionDispatch& dispatch,
                                            std::string_view bytes) {
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

  return make_diagnostics_update(std::move(diagnostics));
}

Status request_extension_contribution_dispatch_stop(
    ExtensionContributionDispatch& dispatch) {
  if (!dispatch.runner.can_request_stop()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "request_extension_contribution_dispatch_stop requires runner state "
        "starting or running, got " +
            to_string(dispatch.runner.state));
  }
  if (!dispatch.channel.can_request_stop()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "request_extension_contribution_dispatch_stop requires channel state "
        "starting or running, got " +
            to_string(dispatch.channel.state));
  }

  const Status runner = request_extension_stop(dispatch.runner);
  if (!runner) {
    return runner;
  }

  const Status channel = request_subprocess_channel_stop(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return Status::success();
}

Status confirm_extension_contribution_dispatch_stopped(
    ExtensionContributionDispatch& dispatch) {
  const Status status = require_synced_stopping(
      dispatch, "confirm_extension_contribution_dispatch_stopped");
  if (!status) {
    return status;
  }

  const Status runner = confirm_extension_stopped(dispatch.runner);
  if (!runner) {
    return runner;
  }

  const Status channel = confirm_subprocess_channel_stopped(dispatch.channel);
  if (!channel) {
    return channel;
  }
  return Status::success();
}

Result<ExtensionSubprocessHostBridge> make_extension_subprocess_host_bridge(
    const ExtensionContributionInvocationPlan& invocation_plan) {
  auto dispatch = make_extension_contribution_dispatch(invocation_plan);
  if (!dispatch) {
    return dispatch.error();
  }

  ExtensionSubprocessHostBridge bridge;
  bridge.dispatch = std::move(dispatch.value());
  return bridge;
}

Result<ExtensionSubprocessHostBridge> make_extension_host_command_bridge(
    ExtensionHostRuntime& runtime, std::string_view name, Json arguments) {
  auto dispatch =
      make_extension_host_command_dispatch(runtime, name, std::move(arguments));
  if (!dispatch) {
    return dispatch.error();
  }

  ExtensionSubprocessHostBridge bridge;
  bridge.dispatch = std::move(dispatch.value());
  return bridge;
}

Result<ExtensionSubprocessHostBridge> make_extension_host_tool_bridge(
    ExtensionHostRuntime& runtime, std::string_view name, Json arguments) {
  auto dispatch =
      make_extension_host_tool_dispatch(runtime, name, std::move(arguments));
  if (!dispatch) {
    return dispatch.error();
  }

  ExtensionSubprocessHostBridge bridge;
  bridge.dispatch = std::move(dispatch.value());
  return bridge;
}

Result<ExtensionSubprocessLaunchRequest> plan_extension_subprocess_host_launch(
    const ExtensionSubprocessHostBridge& bridge) {
  if (bridge.dispatch.runner.launch_plan.transport !=
      ExtensionTransport::kSubprocessJsonRpc) {
    return Result<ExtensionSubprocessLaunchRequest>::failure(
        ErrorCode::invalid_argument,
        "extension subprocess host bridge requires subprocess-jsonrpc transport");
  }

  ExtensionSubprocessLaunchRequest request;
  request.launch_plan = bridge.dispatch.runner.launch_plan;
  request.executable_path = request.launch_plan.executable_path;
  request.arguments = request.launch_plan.arguments;
  request.working_directory = request.launch_plan.working_directory;
  return request;
}

Status request_extension_subprocess_host_launch(
    ExtensionSubprocessHostBridge& bridge) {
  const Status status =
      request_extension_contribution_dispatch_start(bridge.dispatch);
  if (!status) {
    return status;
  }

  bridge.launch_requested = true;
  return Status::success();
}

Status confirm_extension_subprocess_host_launched(
    ExtensionSubprocessHostBridge& bridge, int process_id) {
  return confirm_extension_contribution_dispatch_started(bridge.dispatch,
                                                        process_id);
}

Result<std::string> write_extension_subprocess_host_stdin(
    ExtensionSubprocessHostBridge& bridge) {
  if (bridge.completed_result.has_value()) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        "extension subprocess host bridge invocation is already completed");
  }

  if (!bridge.invocation_written) {
    const Status invoke =
        dispatch_extension_contribution_invocation(bridge.dispatch);
    if (!invoke) {
      return invoke.error();
    }
    bridge.invocation_written = true;
  }

  return take_subprocess_channel_output(bridge.dispatch.channel);
}

Result<ExtensionContributionDispatchUpdate> read_extension_subprocess_host_stdout(
    ExtensionSubprocessHostBridge& bridge, std::string_view bytes) {
  auto update =
      push_extension_contribution_dispatch_stdout(bridge.dispatch, bytes);
  if (!update) {
    return update.error();
  }

  if (update.value().completed()) {
    auto result = decode_extension_contribution_result(
        bridge.dispatch, update.value(), bridge.diagnostics);
    if (!result) {
      return result.error();
    }
    bridge.completed_result = std::move(result.value());
  }

  return update.value();
}

Result<ExtensionContributionDispatchUpdate> read_extension_subprocess_host_stderr(
    ExtensionSubprocessHostBridge& bridge, std::string_view bytes) {
  auto update =
      push_extension_contribution_dispatch_stderr(bridge.dispatch, bytes);
  if (!update) {
    return update.error();
  }

  bridge.diagnostics.insert(bridge.diagnostics.end(),
                            update.value().diagnostics.begin(),
                            update.value().diagnostics.end());
  return update.value();
}

}  // namespace qt::extensions
