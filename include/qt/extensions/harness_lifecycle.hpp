#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <qt/agent/models.hpp>
#include <qt/extensions/contributions.hpp>
#include <qt/foundation/error.hpp>
#include <qt/harness/agent_harness.hpp>
#include <qt/harness/context_resources.hpp>

namespace qt::extensions {

struct ExtensionHarnessLifecycleRoutingOptions {
  std::int64_t timestamp = 1;
  std::vector<qt::harness::AgentHarness::ToolResultEvent> tool_results;
};

struct ExtensionHarnessLifecycleRoutingPlan {
  LoadedExtensionRecord extension;
  std::string event_type;
  JsonRpcRequest request;
};

enum class ExtensionHarnessLifecycleDispatchState {
  kIdle,
  kDispatched,
  kCompletedSuccess,
  kCompletedError,
};

struct ExtensionHarnessLifecycleDispatch {
  ExtensionHarnessLifecycleRoutingPlan routing_plan;
  ExtensionRunnerLifecycle runner;
  SubprocessChannel channel;
  ExtensionHarnessLifecycleDispatchState state =
      ExtensionHarnessLifecycleDispatchState::kIdle;
  std::size_t dispatch_count = 0;
};

struct ExtensionHarnessLifecycleDispatchUpdate {
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

struct ExtensionHarnessLifecycleResult {
  LoadedExtensionRecord extension;
  std::string event_type;
  Json request_id = nullptr;
  bool ok = false;
  Json value = nullptr;
  std::optional<JsonRpcError> error;
  std::vector<std::string> diagnostics;
};

struct ExtensionHarnessLifecycleSubprocessBridge {
  ExtensionHarnessLifecycleDispatch dispatch;
  std::vector<std::string> diagnostics;
  std::optional<ExtensionHarnessLifecycleResult> completed_result;
  bool launch_requested = false;
  bool event_written = false;
  bool teardown_requested = false;
  std::size_t retry_count = 0;
};

std::string to_string(ExtensionHarnessLifecycleDispatchState state);

bool extension_handles_harness_lifecycle_event(
    const LoadedExtensionRecord& extension, std::string_view event_type);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_lifecycle_routing_plans(
    ExtensionHostRuntime& runtime, const qt::agent::AgentEvent& event,
    const ExtensionHarnessLifecycleRoutingOptions& options = {});

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_lifecycle_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::SubscribedEvent& event,
    const ExtensionHarnessLifecycleRoutingOptions& options = {});

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_start_routing_plans(
    ExtensionHostRuntime& runtime, ExtensionSessionStartReason reason,
    std::optional<std::string> session_file = std::nullopt);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_resource_discover_routing_plans(
    ExtensionHostRuntime& runtime, std::string cwd,
    ExtensionResourceDiscoverReason reason);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_instructions_loaded_routing_plans(
    ExtensionHostRuntime& runtime,
    const std::vector<qt::harness::InstructionLoadEvent>& events,
    std::string reason);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_shutdown_routing_plans(
    ExtensionHostRuntime& runtime, ExtensionSessionShutdownReason reason,
    std::optional<std::string> target_session_file = std::nullopt);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_before_tree_routing_plans(ExtensionHostRuntime& runtime,
                                                 Json preparation);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_session_tree_routing_plans(
    ExtensionHostRuntime& runtime, std::optional<std::string> new_leaf_id,
    std::optional<std::string> old_leaf_id,
    std::optional<Json> summary_entry = std::nullopt,
    std::optional<bool> from_extension = std::nullopt);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_input_routing_plans(ExtensionHostRuntime& runtime,
                                   std::string text, Json images,
                                   ExtensionInputSource source);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_model_select_routing_plans(
    ExtensionHostRuntime& runtime, Json model, std::optional<Json> previous_model,
    std::string source);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_thinking_level_select_routing_plans(
    ExtensionHostRuntime& runtime, std::string level,
    std::string previous_level);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_user_bash_routing_plans(ExtensionHostRuntime& runtime,
                                       std::string command,
                                       bool exclude_from_context,
                                       std::string cwd);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_call_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolCallEvent& event);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_result_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolResultEvent& event);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_execution_start_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolCallEvent& event);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_execution_update_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolResultEvent& event);

qt::foundation::Result<std::vector<ExtensionHarnessLifecycleRoutingPlan>>
make_extension_harness_tool_execution_end_routing_plans(
    ExtensionHostRuntime& runtime,
    const qt::harness::AgentHarness::ToolResultEvent& event);

qt::foundation::Result<ExtensionHarnessLifecycleDispatch>
make_extension_harness_lifecycle_dispatch(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan);

qt::foundation::Result<ExtensionHarnessLifecycleResult>
decode_extension_harness_lifecycle_result(
    const ExtensionHarnessLifecycleDispatch& dispatch,
    const ExtensionHarnessLifecycleDispatchUpdate& update,
    const std::vector<std::string>& diagnostics = {});

qt::foundation::Status request_extension_harness_lifecycle_dispatch_start(
    ExtensionHarnessLifecycleDispatch& dispatch);

qt::foundation::Status confirm_extension_harness_lifecycle_dispatch_started(
    ExtensionHarnessLifecycleDispatch& dispatch, int process_id);

qt::foundation::Status dispatch_extension_harness_lifecycle_event(
    ExtensionHarnessLifecycleDispatch& dispatch);

qt::foundation::Result<ExtensionHarnessLifecycleDispatchUpdate>
push_extension_harness_lifecycle_dispatch_stdout(
    ExtensionHarnessLifecycleDispatch& dispatch, std::string_view bytes);

qt::foundation::Result<ExtensionHarnessLifecycleDispatchUpdate>
push_extension_harness_lifecycle_dispatch_stderr(
    ExtensionHarnessLifecycleDispatch& dispatch, std::string_view bytes);

qt::foundation::Status request_extension_harness_lifecycle_dispatch_stop(
    ExtensionHarnessLifecycleDispatch& dispatch);

qt::foundation::Status confirm_extension_harness_lifecycle_dispatch_stopped(
    ExtensionHarnessLifecycleDispatch& dispatch);

qt::foundation::Result<ExtensionHarnessLifecycleSubprocessBridge>
make_extension_harness_lifecycle_subprocess_bridge(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan);

qt::foundation::Result<ExtensionSubprocessLaunchRequest>
plan_extension_harness_lifecycle_subprocess_launch(
    const ExtensionHarnessLifecycleSubprocessBridge& bridge);

qt::foundation::Status request_extension_harness_lifecycle_subprocess_launch(
    ExtensionHarnessLifecycleSubprocessBridge& bridge);

qt::foundation::Status confirm_extension_harness_lifecycle_subprocess_launched(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, int process_id);

qt::foundation::Result<std::string>
write_extension_harness_lifecycle_subprocess_stdin(
    ExtensionHarnessLifecycleSubprocessBridge& bridge);

qt::foundation::Result<ExtensionHarnessLifecycleDispatchUpdate>
read_extension_harness_lifecycle_subprocess_stdout(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, std::string_view bytes);

qt::foundation::Result<ExtensionHarnessLifecycleDispatchUpdate>
read_extension_harness_lifecycle_subprocess_stderr(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, std::string_view bytes);

qt::foundation::Status request_extension_harness_lifecycle_subprocess_teardown(
    ExtensionHarnessLifecycleSubprocessBridge& bridge);

qt::foundation::Status confirm_extension_harness_lifecycle_subprocess_teardown(
    ExtensionHarnessLifecycleSubprocessBridge& bridge);

qt::foundation::Status record_extension_harness_lifecycle_subprocess_crashed(
    ExtensionHarnessLifecycleSubprocessBridge& bridge, int exit_code,
    std::string diagnostic = {});

qt::foundation::Status reset_extension_harness_lifecycle_subprocess_for_retry(
    ExtensionHarnessLifecycleSubprocessBridge& bridge);

}  // namespace qt::extensions
