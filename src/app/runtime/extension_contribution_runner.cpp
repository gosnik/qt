#include "qt/app/runtime/extension_contribution_runner.hpp"

#include <qt/foundation/filesystem.hpp>

#include <string>
#include <cctype>

namespace qt::app {
namespace {

using qt::extensions::ExtensionContributionDescriptor;
using qt::extensions::ExtensionContributionKind;
using qt::extensions::ExtensionContributionResult;
using qt::extensions::ExtensionHostRuntime;
using qt::extensions::ExtensionSubprocessHostBridge;
using qt::extensions::Json;
using qt::foundation::ErrorCode;
using qt::foundation::Result;

const char* contribution_kind_label(ExtensionContributionKind kind) noexcept {
  switch (kind) {
    case ExtensionContributionKind::kCommand:
      return "command";
    case ExtensionContributionKind::kTool:
      return "tool";
  }
  return "contribution";
}

std::string contribution_invocation_name(
    const ExtensionContributionDescriptor& contribution) {
  if (contribution.kind == ExtensionContributionKind::kCommand) {
    return contribution.invocation_name.empty() ? contribution.name
                                                : contribution.invocation_name;
  }
  return contribution.name;
}

std::string contribution_error_prefix(
    const ExtensionContributionDescriptor& contribution) {
  std::string output = "extension ";
  output += contribution_kind_label(contribution.kind);
  output += " '";
  output += contribution_invocation_name(contribution);
  output += "'";
  return output;
}

ExtensionHostRuntime runtime_for_contribution(
    const ExtensionContributionDescriptor& contribution) {
  ExtensionHostRuntime runtime;
  runtime.registry.loaded_extensions.push_back(contribution.extension);
  if (contribution.kind == ExtensionContributionKind::kCommand) {
    runtime.registry.commands.push_back(contribution);
  } else {
    runtime.registry.tools.push_back(contribution);
  }
  return runtime;
}

Result<ExtensionSubprocessHostBridge> make_bridge(
    ExtensionHostRuntime& runtime,
    const ExtensionContributionDescriptor& contribution, Json arguments) {
  if (contribution.kind == ExtensionContributionKind::kCommand) {
    return qt::extensions::make_extension_host_command_bridge(
        runtime, contribution_invocation_name(contribution), std::move(arguments));
  }
  return qt::extensions::make_extension_host_tool_bridge(
      runtime, contribution.name, std::move(arguments));
}

template <typename T>
Result<T> not_implemented_error(
    const ExtensionContributionDescriptor& contribution, std::string message) {
  return Result<T>::failure(
      ErrorCode::invalid_argument,
      "not_implemented: " + contribution_error_prefix(contribution) + ": " +
          std::move(message));
}

template <typename T>
Result<T> execution_error(
    const ExtensionContributionDescriptor& contribution, std::string message) {
  return Result<T>::failure(
      ErrorCode::io_error,
      contribution_error_prefix(contribution) + " execution failed: " +
          std::move(message));
}

}  // namespace

Result<ExtensionContributionResult> run_extension_contribution_subprocess(
    const ExtensionContributionDescriptor& contribution, Json arguments,
    ExtensionContributionRunOptions options) {
  if (contribution.extension.manifest.transport ==
      qt::extensions::ExtensionTransport::kAngelScript) {
    const auto& script = contribution.extension.script_runtime;
    if (!script) {
      return execution_error<ExtensionContributionResult>(contribution,
          "AngelScript runtime was not loaded");
    }
    if (options.cancellation_token && options.cancellation_token->is_cancelled()) {
      return execution_error<ExtensionContributionResult>(contribution, "cancelled");
    }
    std::string handler = contribution.kind == ExtensionContributionKind::kCommand
                              ? "command_" : "tool_";
    for (const unsigned char ch : contribution.name) {
      handler += std::isalnum(ch) || ch == '_' ? static_cast<char>(ch) : '_';
    }
    const auto& host_call = options.host_call ? options.host_call :
        (contribution.extension.host_call ? *contribution.extension.host_call :
                                           options.host_call);
    const auto invoked = script->invoke(handler, arguments, host_call);
    if (!invoked) return invoked.error();
    ExtensionContributionResult result;
    result.kind = contribution.kind;
    result.contribution_name = contribution.name;
    result.ok = true;
    result.value = invoked.value();
    return result;
  }
  auto runtime = runtime_for_contribution(contribution);
  auto bridge = make_bridge(runtime, contribution, std::move(arguments));
  if (!bridge) {
    return not_implemented_error<ExtensionContributionResult>(
        contribution, bridge.error().message);
  }

  const auto start =
      qt::extensions::request_extension_subprocess_host_launch(bridge.value());
  if (!start) {
    return not_implemented_error<ExtensionContributionResult>(
        contribution, start.error().message);
  }
  const auto launch =
      qt::extensions::plan_extension_subprocess_host_launch(bridge.value());
  if (!launch) {
    return not_implemented_error<ExtensionContributionResult>(
        contribution, launch.error().message);
  }
  const auto started =
      qt::extensions::confirm_extension_subprocess_host_launched(bridge.value(), 1);
  if (!started) {
    return not_implemented_error<ExtensionContributionResult>(
        contribution, started.error().message);
  }
  const auto stdin_text =
      qt::extensions::write_extension_subprocess_host_stdin(bridge.value());
  if (!stdin_text) {
    return not_implemented_error<ExtensionContributionResult>(
        contribution, stdin_text.error().message);
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
    return execution_error<ExtensionContributionResult>(
        contribution, process.error().message);
  }

  if (!process.value().stderr_text.empty()) {
    const auto stderr_update =
        qt::extensions::read_extension_subprocess_host_stderr(
            bridge.value(), process.value().stderr_text);
    if (!stderr_update) {
      return execution_error<ExtensionContributionResult>(
          contribution, stderr_update.error().message);
    }
  }

  const auto stdout_update = qt::extensions::read_extension_subprocess_host_stdout(
      bridge.value(), process.value().stdout_text);
  if (!stdout_update) {
    return execution_error<ExtensionContributionResult>(
        contribution, stdout_update.error().message);
  }

  if (!bridge.value().completed_result.has_value()) {
    std::string message = "did not return a JSON-RPC response";
    if (process.value().exit_code != 0) {
      message += "; exit code " + std::to_string(process.value().exit_code);
    }
    return execution_error<ExtensionContributionResult>(contribution,
                                                        std::move(message));
  }

  return *bridge.value().completed_result;
}

}  // namespace qt::app
