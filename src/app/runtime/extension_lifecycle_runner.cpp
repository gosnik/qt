#include "qt/app/runtime/extension_lifecycle_runner.hpp"

#include <qt/foundation/filesystem.hpp>

#include <string>

namespace qt::app {
namespace {

using qt::extensions::ExtensionHarnessLifecycleResult;
using qt::extensions::ExtensionHarnessLifecycleRoutingPlan;
using qt::extensions::ExtensionInputEventRoutingState;
using qt::extensions::ExtensionInputSource;
using qt::extensions::ExtensionHostRuntime;
using qt::extensions::Json;
using qt::foundation::ErrorCode;
using qt::foundation::Result;

std::string extension_lifecycle_error_prefix(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan) {
  std::string output = "extension lifecycle event '";
  output += routing_plan.event_type;
  output += "'";
  if (!routing_plan.extension.manifest.name.empty()) {
    output += " for '";
    output += routing_plan.extension.manifest.name;
    output += "'";
  }
  return output;
}

template <typename T>
Result<T> not_implemented_error(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan,
    std::string message) {
  return Result<T>::failure(
      ErrorCode::invalid_argument,
      "not_implemented: " + extension_lifecycle_error_prefix(routing_plan) +
          ": " + std::move(message));
}

template <typename T>
Result<T> execution_error(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan,
    std::string message) {
  return Result<T>::failure(
      ErrorCode::io_error,
      extension_lifecycle_error_prefix(routing_plan) + " execution failed: " +
          std::move(message));
}

std::string jsonrpc_error_message(
    const qt::extensions::ExtensionHarnessLifecycleResult& result) {
  if (!result.error.has_value()) {
    return "handler returned an error";
  }
  if (!result.error->message.empty()) {
    return result.error->message;
  }
  return "handler returned JSON-RPC error " + std::to_string(result.error->code);
}

Result<std::size_t> run_lifecycle_plans(
    const std::vector<ExtensionHarnessLifecycleRoutingPlan>& plans,
    ExtensionLifecycleRunOptions options) {
  std::size_t completed = 0;
  for (const auto& plan : plans) {
    auto result = run_extension_harness_lifecycle_subprocess(plan, options);
    if (!result) {
      return Result<std::size_t>::failure(
          result.error().code, result.error().message,
          result.error().system_error);
    }
    if (!result.value().ok) {
      return Result<std::size_t>::failure(
          ErrorCode::io_error,
          extension_lifecycle_error_prefix(plan) + " failed: " +
              jsonrpc_error_message(result.value()));
    }
    ++completed;
  }
  return completed;
}

}  // namespace

Result<ExtensionHarnessLifecycleResult>
run_extension_harness_lifecycle_subprocess(
    const ExtensionHarnessLifecycleRoutingPlan& routing_plan,
    ExtensionLifecycleRunOptions options) {
  if (routing_plan.extension.manifest.transport ==
      qt::extensions::ExtensionTransport::kAngelScript) {
    const auto& script = routing_plan.extension.script_runtime;
    if (!script) {
      return execution_error<ExtensionHarnessLifecycleResult>(routing_plan,
          "AngelScript runtime was not loaded");
    }
    if (options.cancellation_token && options.cancellation_token->is_cancelled()) {
      return execution_error<ExtensionHarnessLifecycleResult>(routing_plan, "cancelled");
    }
    ExtensionHarnessLifecycleResult result;
    result.extension = routing_plan.extension;
    result.event_type = routing_plan.event_type;
    result.request_id = routing_plan.request.id;
    result.ok = true;
    result.value = Json::object();
    for (const auto& registration : script->registration().lifecycle_handlers) {
      if (registration.event != routing_plan.event_type) continue;
      const auto& host_call = options.host_call ? options.host_call :
          (routing_plan.extension.host_call ? *routing_plan.extension.host_call :
                                             options.host_call);
      const auto value = script->invoke(registration.handler,
          routing_plan.request.params, host_call);
      if (!value) return value.error();
      if (value.value().is_object()) {
        result.value.update(value.value());
      } else if (!value.value().is_null()) {
        return execution_error<ExtensionHarnessLifecycleResult>(routing_plan,
            "lifecycle handler must return an object or null");
      }
    }
    return result;
  }
  auto bridge =
      qt::extensions::make_extension_harness_lifecycle_subprocess_bridge(
          routing_plan);
  if (!bridge) {
    return not_implemented_error<ExtensionHarnessLifecycleResult>(
        routing_plan, bridge.error().message);
  }

  const auto start =
      qt::extensions::request_extension_harness_lifecycle_subprocess_launch(
          bridge.value());
  if (!start) {
    return not_implemented_error<ExtensionHarnessLifecycleResult>(
        routing_plan, start.error().message);
  }
  const auto launch =
      qt::extensions::plan_extension_harness_lifecycle_subprocess_launch(
          bridge.value());
  if (!launch) {
    return not_implemented_error<ExtensionHarnessLifecycleResult>(
        routing_plan, launch.error().message);
  }
  const auto started =
      qt::extensions::confirm_extension_harness_lifecycle_subprocess_launched(
          bridge.value(), 1);
  if (!started) {
    return not_implemented_error<ExtensionHarnessLifecycleResult>(
        routing_plan, started.error().message);
  }
  const auto stdin_text =
      qt::extensions::write_extension_harness_lifecycle_subprocess_stdin(
          bridge.value());
  if (!stdin_text) {
    return not_implemented_error<ExtensionHarnessLifecycleResult>(
        routing_plan, stdin_text.error().message);
  }

  qt::foundation::ProcessRequest process_request;
  process_request.mode = qt::foundation::ProcessLaunchMode::argv;
  process_request.argv.push_back(launch.value().executable_path);
  process_request.argv.insert(process_request.argv.end(),
                              launch.value().arguments.begin(),
                              launch.value().arguments.end());
  process_request.cwd = qt::foundation::Path(launch.value().working_directory);
  process_request.timeout_ms = options.timeout_ms;
  process_request.cancellation_token = std::move(options.cancellation_token);
  process_request.stdin_text = stdin_text.value();

  const auto process = qt::foundation::run_process(process_request);
  if (!process) {
    return execution_error<ExtensionHarnessLifecycleResult>(
        routing_plan, process.error().message);
  }

  if (!process.value().stderr_text.empty()) {
    const auto stderr_update =
        qt::extensions::read_extension_harness_lifecycle_subprocess_stderr(
            bridge.value(), process.value().stderr_text);
    if (!stderr_update) {
      return execution_error<ExtensionHarnessLifecycleResult>(
          routing_plan, stderr_update.error().message);
    }
  }

  const auto stdout_update =
      qt::extensions::read_extension_harness_lifecycle_subprocess_stdout(
          bridge.value(), process.value().stdout_text);
  if (!stdout_update) {
    return execution_error<ExtensionHarnessLifecycleResult>(
        routing_plan, stdout_update.error().message);
  }

  if (!bridge.value().completed_result.has_value()) {
    std::string message = "did not return a JSON-RPC response";
    if (process.value().exit_code != 0) {
      message += "; exit code " + std::to_string(process.value().exit_code);
    }
    return execution_error<ExtensionHarnessLifecycleResult>(routing_plan,
                                                            std::move(message));
  }

  return *bridge.value().completed_result;
}

Result<ExtensionInputEventRoutingState> run_extension_input_hooks_subprocess(
    ExtensionHostRuntime& runtime, std::string text, Json images,
    ExtensionInputSource source, ExtensionLifecycleRunOptions options) {
  auto record = qt::extensions::make_extension_input_event_record(
      std::move(text), std::move(images), source);
  if (!record) {
    return Result<ExtensionInputEventRoutingState>::failure(
        record.error().code, record.error().message);
  }

  auto state = qt::extensions::make_extension_input_event_routing_state(
      std::move(record.value()));
  if (!state) {
    return Result<ExtensionInputEventRoutingState>::failure(
        state.error().code, state.error().message);
  }

  for (const auto& extension : runtime.registry.loaded_extensions) {
    if (!qt::extensions::extension_handles_harness_lifecycle_event(extension,
                                                                   "input")) {
      continue;
    }

    auto plan = qt::extensions::make_extension_input_event_routing_plan(
        extension, state.value().current_event, Json(runtime.next_request_id));
    if (!plan) {
      return Result<ExtensionInputEventRoutingState>::failure(
          plan.error().code, plan.error().message);
    }
    ++runtime.next_request_id;

    qt::extensions::ExtensionHarnessLifecycleRoutingPlan routing_plan;
    routing_plan.extension = extension;
    routing_plan.event_type = "input";
    routing_plan.request = std::move(plan.value().request);

    auto result = run_extension_harness_lifecycle_subprocess(
        routing_plan, options);
    if (!result) {
      return Result<ExtensionInputEventRoutingState>::failure(
          result.error().code, result.error().message);
    }
    if (!result.value().ok) {
      return Result<ExtensionInputEventRoutingState>::failure(
          ErrorCode::io_error,
          extension_lifecycle_error_prefix(routing_plan) + " failed: " +
              jsonrpc_error_message(result.value()));
    }

    auto decoded =
        qt::extensions::decode_extension_input_event_result(result.value().value);
    if (!decoded) {
      return Result<ExtensionInputEventRoutingState>::failure(
          decoded.error().code, decoded.error().message);
    }

    const auto applied = qt::extensions::apply_extension_input_event_result(
        state.value(), decoded.value());
    if (!applied) {
      return Result<ExtensionInputEventRoutingState>::failure(
          applied.error().code, applied.error().message);
    }
    if (state.value().handled) {
      break;
    }
  }

  return state.value();
}

Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>
run_extension_session_before_switch_hooks_subprocess(
    ExtensionHostRuntime& runtime,
    qt::extensions::ExtensionSessionBeforeSwitchReason reason,
    std::optional<std::string> target_session_file,
    ExtensionLifecycleRunOptions options) {
  auto record =
      qt::extensions::make_extension_session_before_switch_event_record(
          reason, std::move(target_session_file));
  if (!record) {
    return Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>::
        failure(record.error().code, record.error().message);
  }

  auto state = qt::extensions::make_extension_session_before_switch_routing_state();
  if (!state) {
    return Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>::
        failure(state.error().code, state.error().message);
  }

  for (const auto& extension : runtime.registry.loaded_extensions) {
    if (!qt::extensions::extension_handles_harness_lifecycle_event(
            extension, "session_before_switch")) {
      continue;
    }

    auto plan =
        qt::extensions::make_extension_session_before_switch_routing_plan(
            extension, record.value(), Json(runtime.next_request_id));
    if (!plan) {
      return Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>::
          failure(plan.error().code, plan.error().message);
    }
    ++runtime.next_request_id;

    qt::extensions::ExtensionHarnessLifecycleRoutingPlan routing_plan;
    routing_plan.extension = extension;
    routing_plan.event_type = "session_before_switch";
    routing_plan.request = std::move(plan.value().request);

    auto result = run_extension_harness_lifecycle_subprocess(
        routing_plan, options);
    if (!result) {
      return Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>::
          failure(result.error().code, result.error().message);
    }
    if (!result.value().ok) {
      return Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>::
          failure(ErrorCode::io_error,
                  extension_lifecycle_error_prefix(routing_plan) +
                      " failed: " + jsonrpc_error_message(result.value()));
    }

    auto decoded =
        qt::extensions::decode_extension_session_before_switch_result(
            result.value().value);
    if (!decoded) {
      return Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>::
          failure(decoded.error().code, decoded.error().message);
    }

    const auto applied =
        qt::extensions::apply_extension_session_before_switch_result(
            state.value(), decoded.value());
    if (!applied) {
      return Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>::
          failure(applied.error().code, applied.error().message);
    }
    if (state.value().cancelled) {
      break;
    }
  }

  return state.value();
}

Result<std::size_t> run_extension_session_start_hooks_subprocess(
    ExtensionHostRuntime& runtime,
    qt::extensions::ExtensionSessionStartReason reason,
    std::optional<std::string> previous_session_file,
    ExtensionLifecycleRunOptions options) {
  auto plans = qt::extensions::make_extension_session_start_routing_plans(
      runtime, reason, std::move(previous_session_file));
  if (!plans) {
    return Result<std::size_t>::failure(plans.error().code,
                                        plans.error().message,
                                        plans.error().system_error);
  }
  return run_lifecycle_plans(plans.value(), std::move(options));
}

Result<std::size_t> run_extension_session_shutdown_hooks_subprocess(
    ExtensionHostRuntime& runtime,
    qt::extensions::ExtensionSessionShutdownReason reason,
    std::optional<std::string> target_session_file,
    ExtensionLifecycleRunOptions options) {
  auto plans = qt::extensions::make_extension_session_shutdown_routing_plans(
      runtime, reason, std::move(target_session_file));
  if (!plans) {
    return Result<std::size_t>::failure(plans.error().code,
                                        plans.error().message,
                                        plans.error().system_error);
  }
  return run_lifecycle_plans(plans.value(), std::move(options));
}

}  // namespace qt::app
