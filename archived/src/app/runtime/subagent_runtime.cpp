#include "qt/app/runtime/subagent_runtime.hpp"
#include "qt/app/runtime/subagent_coordinator.hpp"

namespace qt::app {
void bind_subagent_runtime(
    const std::shared_ptr<ReferenceStartupRuntime> &runtime) {
  if (!runtime || !runtime->services.subagent_coordinator)
    return;
  const auto messages = runtime->host.harness().agent().state().transcript;
  runtime->services.subagent_coordinator->set_context_snapshot(
      [messages]() { return messages; });
}
} // namespace qt::app
