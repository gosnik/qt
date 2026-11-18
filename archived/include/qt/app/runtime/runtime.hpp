#pragma once

#include <memory>
#include <qt/agent/agent.hpp>
#include <qt/ai/provider.hpp>
#include <qt/app/cli/cli.hpp>
#include <qt/app/cli/startup.hpp>
#include <qt/app/runtime/runtime_host.hpp>
#include <qt/app/runtime/runtime_services.hpp>
#include <qt/app/config/settings.hpp>
#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>
#include <string>
#include <vector>

namespace qt::app {

struct ReferenceStartupRuntime {
  RuntimeServices services;
  RuntimeHost host;
  bool using_echo_provider = true;
};

struct ReferenceRuntimeProviderSelection {
  std::shared_ptr<qt::ai::Provider> provider;
  qt::ai::ProviderOptions options;
  ResolvedModelSelection model_selection;
  bool using_echo_provider = true;
};

std::shared_ptr<ai::Provider> make_echo_provider();
harness::SessionMetadata runtime_session_metadata(
    const ReferenceStartupSession& startup_session);
agent::AgentConfig make_agent_config(
    const ReferenceCliArgs& parsed,
    const ReferenceStartupSession& startup_session,
    const RuntimeServices& services, const ai::ModelRef& model,
    const std::optional<std::string>& thinking_level);
agent::AgentConfig make_demo_agent_config(
    const ReferenceCliArgs& parsed,
    const ReferenceStartupSession& startup_session);
std::vector<harness::SessionEntry> load_startup_session_entries(
    const ReferenceStartupSession& startup_session);
bool session_has_thinking_change(
    const std::vector<harness::SessionEntry>& entries);
foundation::Result<AppSettings> load_startup_settings(
    const ReferenceStartupSession& startup_session);
foundation::Result<ReferenceStartupRuntime> make_startup_runtime(
    const ReferenceCliArgs& parsed,
    const ReferenceStartupSession& startup_session);
foundation::Result<ReferenceRuntimeProviderSelection>
make_runtime_provider_selection(
    const RuntimeServices& services, const ReferenceCliArgs& parsed,
    const std::string& provider, const std::string& model);

}  // namespace qt::app
