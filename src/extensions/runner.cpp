#include <qt/extensions/runner.hpp>

#include <filesystem>
#include <system_error>
#include <utility>

namespace qt::extensions {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;
using Path = std::filesystem::path;

std::string path_to_string(const Path& path) { return path.lexically_normal().generic_string(); }

bool path_is_regular_file(const Path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool path_is_directory(const Path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

Path resolve_from_directory(const std::string& base_directory,
                            const std::string& candidate) {
  const Path path(candidate);
  if (path.is_absolute()) {
    return path;
  }
  return Path(base_directory) / path;
}

Result<ExtensionLaunchPlan> validate_and_build_plan(
    const LoadedExtensionRecord& extension) {
  if (extension.directory.empty()) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::invalid_argument, "extension directory is required");
  }
  if (extension.path.empty()) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::invalid_argument, "extension manifest path is required");
  }
  if (extension.manifest.name.empty()) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::invalid_argument, "extension manifest name is required");
  }
  if (!supports_runner_transport(extension.manifest.transport)) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::invalid_argument,
        "unsupported extension runner transport: " +
            to_string(extension.manifest.transport));
  }
  if (extension.manifest.executable.empty()) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::invalid_argument, "extension executable is required");
  }

  const Path executable_path =
      resolve_from_directory(extension.directory, extension.manifest.executable);
  if (!path_is_regular_file(executable_path)) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::not_found,
        "extension executable is not a readable file: " +
            path_to_string(executable_path));
  }

  const std::string working_directory_text =
      extension.manifest.working_directory.value_or(extension.directory);
  if (working_directory_text.empty()) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::invalid_argument,
        "extension working_directory resolved to an empty path");
  }

  const Path working_directory_path =
      resolve_from_directory(extension.directory, working_directory_text);
  if (!path_is_directory(working_directory_path)) {
    return Result<ExtensionLaunchPlan>::failure(
        ErrorCode::not_found,
        "extension working_directory is not a directory: " +
            path_to_string(working_directory_path));
  }

  for (std::size_t index = 0; index < extension.manifest.args.size(); ++index) {
    if (extension.manifest.args[index].empty()) {
      return Result<ExtensionLaunchPlan>::failure(
          ErrorCode::invalid_argument,
          "extension argument at index " + std::to_string(index) +
              " must not be empty");
    }
  }

  ExtensionLaunchPlan plan;
  plan.extension = extension;
  plan.transport = extension.manifest.transport;
  plan.executable_path = path_to_string(executable_path);
  plan.arguments = extension.manifest.args;
  plan.working_directory = path_to_string(working_directory_path);
  return plan;
}

Status require_state(const ExtensionRunnerLifecycle& lifecycle,
                     ExtensionRunnerState expected, const char* operation) {
  if (lifecycle.state == expected) {
    return Status::success();
  }

  return Status::failure(
      ErrorCode::invalid_argument,
      std::string(operation) + " requires runner state " + to_string(expected) +
          ", got " + to_string(lifecycle.state));
}

}  // namespace

std::string to_string(ExtensionRunnerState state) {
  switch (state) {
    case ExtensionRunnerState::kStopped:
      return "stopped";
    case ExtensionRunnerState::kStarting:
      return "starting";
    case ExtensionRunnerState::kRunning:
      return "running";
    case ExtensionRunnerState::kStopping:
      return "stopping";
  }

  return "stopped";
}

bool supports_runner_transport(ExtensionTransport transport) noexcept {
  switch (transport) {
    case ExtensionTransport::kSubprocessJsonRpc:
      return true;
    case ExtensionTransport::kReferenceModule:
    case ExtensionTransport::kAngelScript:
      return false;
  }

  return false;
}

Result<ExtensionLaunchPlan> make_extension_launch_plan(
    const LoadedExtensionRecord& extension) {
  return validate_and_build_plan(extension);
}

Result<ExtensionRunnerLifecycle> make_extension_runner_lifecycle(
    const LoadedExtensionRecord& extension) {
  auto plan = make_extension_launch_plan(extension);
  if (!plan) {
    return plan.error();
  }

  ExtensionRunnerLifecycle lifecycle;
  lifecycle.launch_plan = std::move(plan.value());
  return lifecycle;
}

Status request_extension_start(ExtensionRunnerLifecycle& lifecycle) {
  const Status state = require_state(lifecycle, ExtensionRunnerState::kStopped,
                                     "request_extension_start");
  if (!state) {
    return state;
  }

  lifecycle.state = ExtensionRunnerState::kStarting;
  lifecycle.process_id.reset();
  ++lifecycle.start_count;
  return Status::success();
}

Status confirm_extension_started(ExtensionRunnerLifecycle& lifecycle,
                                 int process_id) {
  const Status state = require_state(lifecycle, ExtensionRunnerState::kStarting,
                                     "confirm_extension_started");
  if (!state) {
    return state;
  }
  if (process_id <= 0) {
    return Status::failure(ErrorCode::invalid_argument,
                           "process_id must be positive");
  }

  lifecycle.state = ExtensionRunnerState::kRunning;
  lifecycle.process_id = process_id;
  return Status::success();
}

Status request_extension_stop(ExtensionRunnerLifecycle& lifecycle) {
  if (!lifecycle.can_request_stop()) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "request_extension_stop requires runner state starting or running, got " +
            to_string(lifecycle.state));
  }

  lifecycle.state = ExtensionRunnerState::kStopping;
  return Status::success();
}

Status confirm_extension_stopped(ExtensionRunnerLifecycle& lifecycle) {
  const Status state = require_state(lifecycle, ExtensionRunnerState::kStopping,
                                     "confirm_extension_stopped");
  if (!state) {
    return state;
  }

  lifecycle.state = ExtensionRunnerState::kStopped;
  lifecycle.process_id.reset();
  ++lifecycle.stop_count;
  return Status::success();
}

Status record_extension_crashed(ExtensionRunnerLifecycle& lifecycle,
                                int exit_code) {
  (void)exit_code;
  if (lifecycle.state == ExtensionRunnerState::kStopped) {
    return Status::failure(
        ErrorCode::invalid_argument,
        "record_extension_crashed requires runner state starting, running, or "
        "stopping, got stopped");
  }

  lifecycle.state = ExtensionRunnerState::kStopped;
  lifecycle.process_id.reset();
  ++lifecycle.stop_count;
  return Status::success();
}

}  // namespace qt::extensions
