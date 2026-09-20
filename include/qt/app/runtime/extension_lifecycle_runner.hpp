#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <qt/extensions/harness_lifecycle.hpp>
#include <qt/extensions/angelscript_host.hpp>
#include <qt/foundation/error.hpp>
#include <qt/foundation/process.hpp>

namespace qt::app {

struct ExtensionLifecycleRunOptions {
  std::int64_t timeout_ms = 30'000;
  std::shared_ptr<qt::foundation::ProcessCancellationToken> cancellation_token;
  qt::extensions::ExtensionHostCall host_call;
};

qt::foundation::Result<qt::extensions::ExtensionHarnessLifecycleResult>
run_extension_harness_lifecycle_subprocess(
    const qt::extensions::ExtensionHarnessLifecycleRoutingPlan& routing_plan,
    ExtensionLifecycleRunOptions options = {});

qt::foundation::Result<qt::extensions::ExtensionInputEventRoutingState>
run_extension_input_hooks_subprocess(
    qt::extensions::ExtensionHostRuntime& runtime, std::string text,
    qt::extensions::Json images,
    qt::extensions::ExtensionInputSource source,
    ExtensionLifecycleRunOptions options = {});

qt::foundation::Result<qt::extensions::ExtensionSessionBeforeSwitchRoutingState>
run_extension_session_before_switch_hooks_subprocess(
    qt::extensions::ExtensionHostRuntime& runtime,
    qt::extensions::ExtensionSessionBeforeSwitchReason reason,
    std::optional<std::string> target_session_file,
    ExtensionLifecycleRunOptions options = {});

qt::foundation::Result<std::size_t>
run_extension_session_start_hooks_subprocess(
    qt::extensions::ExtensionHostRuntime& runtime,
    qt::extensions::ExtensionSessionStartReason reason,
    std::optional<std::string> previous_session_file = std::nullopt,
    ExtensionLifecycleRunOptions options = {});

qt::foundation::Result<std::size_t>
run_extension_session_shutdown_hooks_subprocess(
    qt::extensions::ExtensionHostRuntime& runtime,
    qt::extensions::ExtensionSessionShutdownReason reason,
    std::optional<std::string> target_session_file = std::nullopt,
    ExtensionLifecycleRunOptions options = {});

}  // namespace qt::app
