#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <qt/extensions/jsonrpc_transport.hpp>
#include <qt/extensions/loader.hpp>
#include <qt/extensions/runner.hpp>
#include <qt/extensions/subprocess_channel.hpp>
#include <qt/foundation/error.hpp>

namespace qt::extensions {

enum class ExtensionContributionKind {
  kCommand,
  kTool,
};

struct ExtensionContributionDescriptor {
  ExtensionContributionKind kind = ExtensionContributionKind::kCommand;
  std::size_t extension_index = 0;
  std::size_t contribution_index = 0;
  LoadedExtensionRecord extension;
  std::string name;
  std::string invocation_name;
  std::optional<std::string> title;
  std::optional<std::string> description;
};

struct ExtensionProviderContributionDescriptor {
  std::size_t extension_index = 0;
  std::size_t contribution_index = 0;
  LoadedExtensionRecord extension;
  std::string name;
  Json config = Json::object();
};

struct ExtensionFlagContributionDescriptor {
  std::size_t extension_index = 0;
  std::size_t contribution_index = 0;
  LoadedExtensionRecord extension;
  std::string name;
  std::optional<std::string> description;
  std::string type = "boolean";
  Json default_value = Json(nullptr);
};

struct ExtensionContributionRegistry {
  std::vector<LoadedExtensionRecord> loaded_extensions;
  std::vector<ExtensionContributionDescriptor> commands;
  std::vector<ExtensionContributionDescriptor> tools;
  std::vector<ExtensionProviderContributionDescriptor> providers;
  std::vector<ExtensionFlagContributionDescriptor> flags;
};

enum class ExtensionHostResourceEventKind {
  kDiagnostic,
  kExtensionLoaded,
  kCommandDiscovered,
  kToolDiscovered,
  kProviderDiscovered,
  kFlagDiscovered,
};

struct ExtensionHostResourceEvent {
  ExtensionHostResourceEventKind kind =
      ExtensionHostResourceEventKind::kExtensionLoaded;
  std::size_t extension_index = 0;
  std::optional<std::size_t> contribution_index;
  std::string extension_name;
  std::string contribution_name;
  std::string message;
};

struct ExtensionHostRuntime {
  ExtensionContributionRegistry registry;
  std::vector<ExtensionHostResourceEvent> resource_events;
  std::size_t next_request_id = 1;
  std::shared_ptr<void> runtime_binding_state;

  bool has_loaded_extensions() const noexcept {
    return !registry.loaded_extensions.empty();
  }
};

struct ExtensionContributionInvocationPlan {
  ExtensionContributionDescriptor contribution;
  Json arguments = Json::object();
  JsonRpcRequest request;
};

enum class ExtensionContributionDispatchState {
  kIdle,
  kDispatched,
  kCompletedSuccess,
  kCompletedError,
};

struct ExtensionContributionDispatch {
  ExtensionContributionInvocationPlan invocation_plan;
  ExtensionRunnerLifecycle runner;
  SubprocessChannel channel;
  ExtensionContributionDispatchState state =
      ExtensionContributionDispatchState::kIdle;
  std::size_t dispatch_count = 0;
};

enum class ExtensionContributionDispatchUpdateKind {
  kPending,
  kDiagnostics,
  kCompletedSuccess,
  kCompletedError,
};

enum class ExtensionInputSource {
  kInteractive,
  kRpc,
  kExtension,
};

enum class ExtensionInputEventAction {
  kContinue,
  kTransform,
  kHandled,
};

enum class ExtensionResourceDiscoverReason {
  kStartup,
  kReload,
};

enum class ExtensionSessionCompactionEventKind {
  kBeforeCompact,
  kCompact,
};

enum class ExtensionSessionStartReason {
  kStartup,
  kReload,
  kNew,
  kResume,
  kFork,
};

enum class ExtensionSessionBeforeSwitchReason {
  kNew,
  kResume,
};

enum class ExtensionSessionBeforeForkPosition {
  kBefore,
  kAt,
};

enum class ExtensionSessionShutdownReason {
  kQuit,
  kReload,
  kNew,
  kResume,
  kFork,
};

struct ExtensionResourceDiscoverEventRecord {
  std::string cwd;
  ExtensionResourceDiscoverReason reason =
      ExtensionResourceDiscoverReason::kStartup;
};

struct ExtensionDiscoveredResourcePath {
  std::string path;
  std::string extension_path;
};

struct ExtensionResourceDiscoverResult {
  std::vector<ExtensionDiscoveredResourcePath> skill_paths;
  std::vector<ExtensionDiscoveredResourcePath> prompt_paths;
  std::vector<ExtensionDiscoveredResourcePath> theme_paths;
};

struct ExtensionResourceDiscoverRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionResourceDiscoverEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionStartEventRecord {
  ExtensionSessionStartReason reason = ExtensionSessionStartReason::kStartup;
  std::optional<std::string> previous_session_file;
};

struct ExtensionSessionStartRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionStartEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionBeforeSwitchEventRecord {
  ExtensionSessionBeforeSwitchReason reason =
      ExtensionSessionBeforeSwitchReason::kNew;
  std::optional<std::string> target_session_file;
};

struct ExtensionSessionBeforeSwitchResult {
  bool cancel = false;
};

struct ExtensionSessionBeforeSwitchRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionBeforeSwitchEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionBeforeSwitchRoutingState {
  bool cancelled = false;
  std::size_t applied_result_count = 0;
};

struct ExtensionSessionBeforeForkEventRecord {
  std::string entry_id;
  ExtensionSessionBeforeForkPosition position =
      ExtensionSessionBeforeForkPosition::kBefore;
};

struct ExtensionSessionBeforeForkResult {
  bool cancel = false;
  bool skip_conversation_restore = false;
};

struct ExtensionSessionBeforeForkRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionBeforeForkEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionBeforeForkRoutingState {
  bool cancelled = false;
  bool skip_conversation_restore = false;
  std::size_t applied_result_count = 0;
};

struct ExtensionSessionBeforeTreeEventRecord {
  Json preparation = Json::object();
};

struct ExtensionSessionBeforeTreeSummary {
  std::string summary;
  std::optional<Json> details;
};

struct ExtensionSessionBeforeTreeResult {
  bool cancel = false;
  std::optional<ExtensionSessionBeforeTreeSummary> summary;
  std::optional<std::string> custom_instructions;
  std::optional<bool> replace_instructions;
  std::optional<std::string> label;
};

struct ExtensionSessionBeforeTreeRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionBeforeTreeEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionBeforeTreeRoutingState {
  bool cancelled = false;
  std::optional<ExtensionSessionBeforeTreeSummary> summary;
  std::optional<std::string> custom_instructions;
  std::optional<bool> replace_instructions;
  std::optional<std::string> label;
  std::size_t applied_result_count = 0;
};

struct ExtensionSessionTreeEventRecord {
  std::optional<std::string> new_leaf_id;
  std::optional<std::string> old_leaf_id;
  std::optional<Json> summary_entry;
  std::optional<bool> from_extension;
};

struct ExtensionSessionTreeRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionTreeEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionBeforeCompactEventRecord {
  Json preparation = Json::object();
  Json branch_entries = Json::array();
  std::optional<std::string> custom_instructions;
};

struct ExtensionSessionBeforeCompactResult {
  bool cancel = false;
  std::optional<Json> compaction;
};

struct ExtensionSessionBeforeCompactRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionBeforeCompactEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionBeforeCompactRoutingState {
  bool cancelled = false;
  std::optional<Json> compaction;
  std::size_t applied_result_count = 0;
};

struct ExtensionSessionCompactEventRecord {
  Json compaction_entry = Json::object();
  bool from_extension = false;
};

struct ExtensionSessionCompactRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionCompactEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionSessionShutdownEventRecord {
  ExtensionSessionShutdownReason reason =
      ExtensionSessionShutdownReason::kQuit;
  std::optional<std::string> target_session_file;
};

struct ExtensionSessionShutdownRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionSessionShutdownEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionBeforeAgentStartEventRecord {
  std::string prompt;
  Json images = nullptr;
  std::string system_prompt;
  Json system_prompt_options = Json::object();
};

struct ExtensionBeforeAgentStartEventResult {
  std::optional<Json> message;
  std::optional<std::string> system_prompt;
};

struct ExtensionBeforeAgentStartRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionBeforeAgentStartEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionBeforeAgentStartRoutingState {
  ExtensionBeforeAgentStartEventRecord current_event;
  std::vector<Json> injected_messages;
  std::size_t applied_result_count = 0;
};

struct ExtensionAgentStartEventRecord {};

struct ExtensionAgentStartRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionAgentStartEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionAgentEndEventRecord {
  Json messages = Json::array();
};

struct ExtensionAgentEndRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionAgentEndEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionTurnStartEventRecord {
  std::size_t turn_index = 0;
  std::int64_t timestamp = 0;
};

struct ExtensionTurnStartRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionTurnStartEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionTurnEndEventRecord {
  std::size_t turn_index = 0;
  Json message = Json::object();
  Json tool_results = Json::array();
};

struct ExtensionTurnEndRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionTurnEndEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionMessageStartEventRecord {
  Json message = Json::object();
};

struct ExtensionMessageStartRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionMessageStartEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionMessageUpdateEventRecord {
  Json message = Json::object();
  Json assistant_message_event = Json::object();
};

struct ExtensionMessageUpdateRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionMessageUpdateEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionMessageEndEventRecord {
  Json message = Json::object();
};

struct ExtensionMessageEndEventResult {
  std::optional<Json> message;
};

struct ExtensionMessageEndRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionMessageEndEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionMessageEndRoutingState {
  ExtensionMessageEndEventRecord current_event;
  std::size_t applied_result_count = 0;
};

struct ExtensionInputEventRecord {
  std::string text;
  Json images = nullptr;
  ExtensionInputSource source = ExtensionInputSource::kInteractive;
};

struct ExtensionInputEventResult {
  ExtensionInputEventAction action = ExtensionInputEventAction::kContinue;
  std::optional<std::string> text;
  Json images = nullptr;
};

struct ExtensionInputEventRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionInputEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionInputEventRoutingState {
  ExtensionInputEventRecord current_event;
  bool handled = false;
  std::size_t applied_result_count = 0;
};

struct ExtensionToolCallEventRecord {
  std::string tool_call_id;
  std::string tool_name;
  Json input = Json::object();
};

struct ExtensionToolCallEventResult {
  bool block = false;
  std::optional<std::string> reason;
  std::optional<Json> input;
};

struct ExtensionToolCallEventRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionToolCallEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionToolCallEventRoutingState {
  ExtensionToolCallEventRecord current_event;
  bool blocked = false;
  std::optional<std::string> block_reason;
  std::size_t applied_result_count = 0;
};

struct ExtensionToolResultEventRecord {
  std::string tool_call_id;
  std::string tool_name;
  Json input = Json::object();
  Json content = Json::array();
  Json details = nullptr;
  bool is_error = false;
};

struct ExtensionToolResultEventResult {
  std::optional<Json> content;
  std::optional<Json> details;
  std::optional<bool> is_error;
};

struct ExtensionToolResultEventRoutingPlan {
  LoadedExtensionRecord extension;
  ExtensionToolResultEventRecord event;
  JsonRpcRequest request;
};

struct ExtensionToolResultEventRoutingState {
  ExtensionToolResultEventRecord current_event;
  std::size_t applied_result_count = 0;
};

struct ExtensionContributionDispatchUpdate {
  ExtensionContributionDispatchUpdateKind kind =
      ExtensionContributionDispatchUpdateKind::kPending;
  std::vector<std::string> diagnostics;
  std::optional<JsonRpcResponse> response;
  std::optional<JsonRpcErrorResponse> error_response;

  bool completed() const noexcept {
    return kind == ExtensionContributionDispatchUpdateKind::kCompletedSuccess ||
           kind == ExtensionContributionDispatchUpdateKind::kCompletedError;
  }
};

struct ExtensionContributionResult {
  ExtensionContributionKind kind = ExtensionContributionKind::kCommand;
  std::string contribution_name;
  Json request_id = nullptr;
  bool ok = false;
  Json value = nullptr;
  std::optional<JsonRpcError> error;
  std::vector<std::string> diagnostics;
};

struct ExtensionSubprocessLaunchRequest {
  ExtensionLaunchPlan launch_plan;
  std::string executable_path;
  std::vector<std::string> arguments;
  std::string working_directory;
};

struct ExtensionSubprocessHostBridge {
  ExtensionContributionDispatch dispatch;
  std::vector<std::string> diagnostics;
  std::optional<ExtensionContributionResult> completed_result;
  bool launch_requested = false;
  bool invocation_written = false;
};

std::string to_string(ExtensionContributionKind kind);
std::string to_string(ExtensionContributionDispatchState state);
std::string to_string(ExtensionContributionDispatchUpdateKind kind);
std::string to_string(ExtensionHostResourceEventKind kind);
std::string to_string(ExtensionInputSource source);
std::string to_string(ExtensionInputEventAction action);
std::string to_string(ExtensionResourceDiscoverReason reason);
std::string to_string(ExtensionSessionCompactionEventKind kind);
std::string to_string(ExtensionSessionStartReason reason);
std::string to_string(ExtensionSessionBeforeSwitchReason reason);
std::string to_string(ExtensionSessionBeforeForkPosition position);
std::string to_string(ExtensionSessionShutdownReason reason);

qt::foundation::Result<ExtensionResourceDiscoverReason>
parse_extension_resource_discover_reason(std::string_view reason);

qt::foundation::Result<ExtensionResourceDiscoverEventRecord>
make_extension_resource_discover_event_record(
    std::string cwd, ExtensionResourceDiscoverReason reason);

qt::foundation::Result<ExtensionResourceDiscoverRoutingPlan>
make_extension_resource_discover_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionResourceDiscoverEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionResourceDiscoverResult>
decode_extension_resource_discover_result(const LoadedExtensionRecord& extension,
                                          const Json& result);

qt::foundation::Result<ExtensionSessionStartEventRecord>
make_extension_session_start_event_record(
    ExtensionSessionStartReason reason,
    std::optional<std::string> previous_session_file);

qt::foundation::Result<ExtensionSessionStartRoutingPlan>
make_extension_session_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionStartEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionSessionBeforeSwitchReason>
parse_extension_session_before_switch_reason(std::string_view reason);

qt::foundation::Result<ExtensionSessionBeforeSwitchEventRecord>
make_extension_session_before_switch_event_record(
    ExtensionSessionBeforeSwitchReason reason,
    std::optional<std::string> target_session_file);

qt::foundation::Result<ExtensionSessionBeforeSwitchRoutingPlan>
make_extension_session_before_switch_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeSwitchEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionSessionBeforeSwitchResult>
decode_extension_session_before_switch_result(const Json& result);

qt::foundation::Result<ExtensionSessionBeforeSwitchRoutingState>
make_extension_session_before_switch_routing_state();

qt::foundation::Status apply_extension_session_before_switch_result(
    ExtensionSessionBeforeSwitchRoutingState& state,
    const ExtensionSessionBeforeSwitchResult& result);

qt::foundation::Result<ExtensionSessionBeforeForkPosition>
parse_extension_session_before_fork_position(std::string_view position);

qt::foundation::Result<ExtensionSessionBeforeForkEventRecord>
make_extension_session_before_fork_event_record(
    std::string entry_id, ExtensionSessionBeforeForkPosition position);

qt::foundation::Result<ExtensionSessionBeforeForkRoutingPlan>
make_extension_session_before_fork_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeForkEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionSessionBeforeForkResult>
decode_extension_session_before_fork_result(const Json& result);

qt::foundation::Result<ExtensionSessionBeforeForkRoutingState>
make_extension_session_before_fork_routing_state();

qt::foundation::Status apply_extension_session_before_fork_result(
    ExtensionSessionBeforeForkRoutingState& state,
    const ExtensionSessionBeforeForkResult& result);

qt::foundation::Result<ExtensionSessionBeforeTreeEventRecord>
make_extension_session_before_tree_event_record(Json preparation);

qt::foundation::Result<ExtensionSessionBeforeTreeRoutingPlan>
make_extension_session_before_tree_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeTreeEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionSessionBeforeTreeResult>
decode_extension_session_before_tree_result(const Json& result);

qt::foundation::Result<ExtensionSessionBeforeTreeRoutingState>
make_extension_session_before_tree_routing_state();

qt::foundation::Status apply_extension_session_before_tree_result(
    ExtensionSessionBeforeTreeRoutingState& state,
    const ExtensionSessionBeforeTreeResult& result);

qt::foundation::Result<ExtensionSessionTreeEventRecord>
make_extension_session_tree_event_record(
    std::optional<std::string> new_leaf_id,
    std::optional<std::string> old_leaf_id, std::optional<Json> summary_entry,
    std::optional<bool> from_extension);

qt::foundation::Result<ExtensionSessionTreeRoutingPlan>
make_extension_session_tree_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionTreeEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionSessionBeforeCompactEventRecord>
make_extension_session_before_compact_event_record(
    Json preparation, Json branch_entries,
    std::optional<std::string> custom_instructions);

qt::foundation::Result<ExtensionSessionBeforeCompactRoutingPlan>
make_extension_session_before_compact_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionBeforeCompactEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionSessionBeforeCompactResult>
decode_extension_session_before_compact_result(const Json& result);

qt::foundation::Result<ExtensionSessionBeforeCompactRoutingState>
make_extension_session_before_compact_routing_state();

qt::foundation::Status apply_extension_session_before_compact_result(
    ExtensionSessionBeforeCompactRoutingState& state,
    const ExtensionSessionBeforeCompactResult& result);

qt::foundation::Result<ExtensionSessionCompactEventRecord>
make_extension_session_compact_event_record(Json compaction_entry,
                                            bool from_extension);

qt::foundation::Result<ExtensionSessionCompactRoutingPlan>
make_extension_session_compact_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionCompactEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionSessionShutdownReason>
parse_extension_session_shutdown_reason(std::string_view reason);

qt::foundation::Result<ExtensionSessionShutdownEventRecord>
make_extension_session_shutdown_event_record(
    ExtensionSessionShutdownReason reason,
    std::optional<std::string> target_session_file);

qt::foundation::Result<ExtensionSessionShutdownRoutingPlan>
make_extension_session_shutdown_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionSessionShutdownEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionBeforeAgentStartEventRecord>
make_extension_before_agent_start_event_record(std::string prompt, Json images,
                                               std::string system_prompt,
                                               Json system_prompt_options);

qt::foundation::Result<ExtensionBeforeAgentStartRoutingPlan>
make_extension_before_agent_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionBeforeAgentStartEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionBeforeAgentStartRoutingState>
make_extension_before_agent_start_routing_state(
    ExtensionBeforeAgentStartEventRecord event);

qt::foundation::Result<ExtensionBeforeAgentStartEventResult>
decode_extension_before_agent_start_result(const Json& result);

qt::foundation::Status apply_extension_before_agent_start_result(
    ExtensionBeforeAgentStartRoutingState& state,
    const ExtensionBeforeAgentStartEventResult& result);

ExtensionAgentStartEventRecord make_extension_agent_start_event_record();

qt::foundation::Result<ExtensionAgentStartRoutingPlan>
make_extension_agent_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionAgentStartEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionAgentEndEventRecord>
make_extension_agent_end_event_record(Json messages);

qt::foundation::Result<ExtensionAgentEndRoutingPlan>
make_extension_agent_end_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionAgentEndEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionTurnStartEventRecord>
make_extension_turn_start_event_record(std::int64_t turn_index,
                                       std::int64_t timestamp);

qt::foundation::Result<ExtensionTurnStartRoutingPlan>
make_extension_turn_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionTurnStartEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionTurnEndEventRecord>
make_extension_turn_end_event_record(std::int64_t turn_index, Json message,
                                     Json tool_results);

qt::foundation::Result<ExtensionTurnEndRoutingPlan>
make_extension_turn_end_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionTurnEndEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionMessageStartEventRecord>
make_extension_message_start_event_record(Json message);

qt::foundation::Result<ExtensionMessageStartRoutingPlan>
make_extension_message_start_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionMessageStartEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionMessageUpdateEventRecord>
make_extension_message_update_event_record(Json message,
                                           Json assistant_message_event);

qt::foundation::Result<ExtensionMessageUpdateRoutingPlan>
make_extension_message_update_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionMessageUpdateEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionMessageEndEventRecord>
make_extension_message_end_event_record(Json message);

qt::foundation::Result<ExtensionMessageEndRoutingPlan>
make_extension_message_end_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionMessageEndEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionMessageEndRoutingState>
make_extension_message_end_routing_state(ExtensionMessageEndEventRecord event);

qt::foundation::Result<ExtensionMessageEndEventResult>
decode_extension_message_end_result(const Json& result);

qt::foundation::Status apply_extension_message_end_result(
    ExtensionMessageEndRoutingState& state,
    const ExtensionMessageEndEventResult& result);

qt::foundation::Result<ExtensionInputSource>
parse_extension_input_source(std::string_view source);

qt::foundation::Result<ExtensionInputEventRecord>
make_extension_input_event_record(std::string text, Json images,
                                  ExtensionInputSource source);

qt::foundation::Result<ExtensionInputEventRoutingPlan>
make_extension_input_event_routing_plan(const LoadedExtensionRecord& extension,
                                        const ExtensionInputEventRecord& event,
                                        Json request_id);

qt::foundation::Result<ExtensionInputEventRoutingState>
make_extension_input_event_routing_state(ExtensionInputEventRecord event);

qt::foundation::Result<ExtensionInputEventResult>
decode_extension_input_event_result(const Json& result);

qt::foundation::Status apply_extension_input_event_result(
    ExtensionInputEventRoutingState& state,
    const ExtensionInputEventResult& result);

qt::foundation::Result<ExtensionToolCallEventRecord>
make_extension_tool_call_event_record(std::string tool_call_id,
                                      std::string tool_name, Json input);

qt::foundation::Result<ExtensionToolCallEventRoutingPlan>
make_extension_tool_call_event_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionToolCallEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionToolCallEventRoutingState>
make_extension_tool_call_event_routing_state(
    ExtensionToolCallEventRecord event);

qt::foundation::Result<ExtensionToolCallEventResult>
decode_extension_tool_call_event_result(const Json& result);

qt::foundation::Status apply_extension_tool_call_event_result(
    ExtensionToolCallEventRoutingState& state,
    const ExtensionToolCallEventResult& result);

qt::foundation::Result<ExtensionToolResultEventRecord>
make_extension_tool_result_event_record(std::string tool_call_id,
                                        std::string tool_name, Json input,
                                        Json content, Json details,
                                        bool is_error);

qt::foundation::Result<ExtensionToolResultEventRoutingPlan>
make_extension_tool_result_event_routing_plan(
    const LoadedExtensionRecord& extension,
    const ExtensionToolResultEventRecord& event, Json request_id);

qt::foundation::Result<ExtensionToolResultEventRoutingState>
make_extension_tool_result_event_routing_state(
    ExtensionToolResultEventRecord event);

qt::foundation::Result<ExtensionToolResultEventResult>
decode_extension_tool_result_event_result(const Json& result);

qt::foundation::Status apply_extension_tool_result_event_result(
    ExtensionToolResultEventRoutingState& state,
    const ExtensionToolResultEventResult& result);

qt::foundation::Result<ExtensionContributionRegistry>
make_extension_contribution_registry(
    const std::vector<LoadedExtensionRecord>& loaded_extensions);

qt::foundation::Result<ExtensionHostRuntime> make_extension_host_runtime(
    const ExtensionLoadResult& load_result);

const std::vector<ExtensionHostResourceEvent>&
extension_host_resource_events(const ExtensionHostRuntime& runtime) noexcept;

qt::foundation::Result<ExtensionContributionDescriptor>
find_extension_command_contribution(const ExtensionContributionRegistry& registry,
                                    std::string_view name);

qt::foundation::Result<ExtensionContributionDescriptor>
find_extension_tool_contribution(const ExtensionContributionRegistry& registry,
                                 std::string_view name);

qt::foundation::Result<ExtensionContributionInvocationPlan>
make_extension_command_invocation_plan(
    const ExtensionContributionRegistry& registry, std::string_view name,
    Json arguments, Json request_id);

qt::foundation::Result<ExtensionContributionInvocationPlan>
make_extension_tool_invocation_plan(
    const ExtensionContributionRegistry& registry, std::string_view name,
    Json arguments, Json request_id);

qt::foundation::Result<ExtensionContributionDispatch>
make_extension_contribution_dispatch(
    const ExtensionContributionInvocationPlan& invocation_plan);

qt::foundation::Result<ExtensionContributionDispatch>
make_extension_host_command_dispatch(ExtensionHostRuntime& runtime,
                                     std::string_view name, Json arguments);

qt::foundation::Result<ExtensionContributionDispatch>
make_extension_host_tool_dispatch(ExtensionHostRuntime& runtime,
                                  std::string_view name, Json arguments);

qt::foundation::Result<Json> extract_extension_contribution_result(
    const ExtensionContributionDispatchUpdate& update);

qt::foundation::Result<ExtensionContributionResult>
decode_extension_contribution_result(
    const ExtensionContributionDispatch& dispatch,
    const ExtensionContributionDispatchUpdate& update,
    const std::vector<std::string>& diagnostics = {});

qt::foundation::Status request_extension_contribution_dispatch_start(
    ExtensionContributionDispatch& dispatch);

qt::foundation::Status confirm_extension_contribution_dispatch_started(
    ExtensionContributionDispatch& dispatch, int process_id);

qt::foundation::Status dispatch_extension_contribution_invocation(
    ExtensionContributionDispatch& dispatch);

qt::foundation::Result<ExtensionContributionDispatchUpdate>
push_extension_contribution_dispatch_stdout(ExtensionContributionDispatch& dispatch,
                                            std::string_view bytes);

qt::foundation::Result<ExtensionContributionDispatchUpdate>
push_extension_contribution_dispatch_stderr(ExtensionContributionDispatch& dispatch,
                                            std::string_view bytes);

qt::foundation::Status request_extension_contribution_dispatch_stop(
    ExtensionContributionDispatch& dispatch);

qt::foundation::Status confirm_extension_contribution_dispatch_stopped(
    ExtensionContributionDispatch& dispatch);

qt::foundation::Result<ExtensionSubprocessHostBridge>
make_extension_subprocess_host_bridge(
    const ExtensionContributionInvocationPlan& invocation_plan);

qt::foundation::Result<ExtensionSubprocessHostBridge>
make_extension_host_command_bridge(ExtensionHostRuntime& runtime,
                                   std::string_view name, Json arguments);

qt::foundation::Result<ExtensionSubprocessHostBridge>
make_extension_host_tool_bridge(ExtensionHostRuntime& runtime,
                                std::string_view name, Json arguments);

qt::foundation::Result<ExtensionSubprocessLaunchRequest>
plan_extension_subprocess_host_launch(
    const ExtensionSubprocessHostBridge& bridge);

qt::foundation::Status request_extension_subprocess_host_launch(
    ExtensionSubprocessHostBridge& bridge);

qt::foundation::Status confirm_extension_subprocess_host_launched(
    ExtensionSubprocessHostBridge& bridge, int process_id);

qt::foundation::Result<std::string> write_extension_subprocess_host_stdin(
    ExtensionSubprocessHostBridge& bridge);

qt::foundation::Result<ExtensionContributionDispatchUpdate>
read_extension_subprocess_host_stdout(ExtensionSubprocessHostBridge& bridge,
                                      std::string_view bytes);

qt::foundation::Result<ExtensionContributionDispatchUpdate>
read_extension_subprocess_host_stderr(ExtensionSubprocessHostBridge& bridge,
                                      std::string_view bytes);

}  // namespace qt::extensions
