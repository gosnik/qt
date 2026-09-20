#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <qt/extensions/loader.hpp>
#include <qt/foundation/error.hpp>

namespace qt::extensions {

enum class ExtensionRunnerState {
  kStopped,
  kStarting,
  kRunning,
  kStopping,
};

struct ExtensionLaunchPlan {
  LoadedExtensionRecord extension;
  ExtensionTransport transport = ExtensionTransport::kSubprocessJsonRpc;
  std::string executable_path;
  std::vector<std::string> arguments;
  std::string working_directory;
};

struct ExtensionRunnerLifecycle {
  ExtensionLaunchPlan launch_plan;
  ExtensionRunnerState state = ExtensionRunnerState::kStopped;
  std::optional<int> process_id;
  std::size_t start_count = 0;
  std::size_t stop_count = 0;

  bool can_request_start() const noexcept {
    return state == ExtensionRunnerState::kStopped;
  }

  bool can_request_stop() const noexcept {
    return state == ExtensionRunnerState::kStarting ||
           state == ExtensionRunnerState::kRunning;
  }
};

std::string to_string(ExtensionRunnerState state);
bool supports_runner_transport(ExtensionTransport transport) noexcept;

qt::foundation::Result<ExtensionLaunchPlan> make_extension_launch_plan(
    const LoadedExtensionRecord& extension);

qt::foundation::Result<ExtensionRunnerLifecycle> make_extension_runner_lifecycle(
    const LoadedExtensionRecord& extension);

qt::foundation::Status request_extension_start(
    ExtensionRunnerLifecycle& lifecycle);

qt::foundation::Status confirm_extension_started(
    ExtensionRunnerLifecycle& lifecycle, int process_id);

qt::foundation::Status request_extension_stop(
    ExtensionRunnerLifecycle& lifecycle);

qt::foundation::Status confirm_extension_stopped(
    ExtensionRunnerLifecycle& lifecycle);

qt::foundation::Status record_extension_crashed(
    ExtensionRunnerLifecycle& lifecycle, int exit_code);

}  // namespace qt::extensions
