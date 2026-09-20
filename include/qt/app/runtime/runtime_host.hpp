#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <qt/agent/agent.hpp>
#include <qt/ai/provider.hpp>
#include <qt/foundation/error.hpp>
#include <qt/harness/agent_harness.hpp>
#include <qt/harness/session.hpp>
#include <qt/harness/session_manager.hpp>

namespace qt::app {

struct RuntimeHostRequest {
  qt::harness::SessionMetadata session_metadata;
  std::vector<qt::harness::SessionEntry> session_entries;
  std::optional<std::string> selected_leaf_id;
  qt::agent::AgentConfig agent_config;
  std::shared_ptr<qt::ai::Provider> provider;
  qt::ai::ProviderOptions provider_options;
  std::string session_start_reason = "startup";
  std::optional<std::string> previous_session_file;
};

struct RuntimeHostSeedState {
  std::optional<std::string> context_leaf_id;
  std::size_t transcript_message_count = 0;
  bool restored_transcript = false;
  bool restored_model = false;
  bool restored_thinking_level = false;
};

class RuntimeHost {
 public:
  RuntimeHost(RuntimeHost&&) noexcept = default;
  RuntimeHost& operator=(RuntimeHost&&) noexcept = default;

  RuntimeHost(const RuntimeHost&) = delete;
  RuntimeHost& operator=(const RuntimeHost&) = delete;

  qt::harness::SessionState& session_state() noexcept { return session_state_; }
  const qt::harness::SessionState& session_state() const noexcept {
    return session_state_;
  }

  qt::harness::SessionManager& session_manager() noexcept {
    return session_manager_;
  }
  const qt::harness::SessionManager& session_manager() const noexcept {
    return session_manager_;
  }

  qt::harness::AgentHarness& harness() noexcept { return harness_; }
  const qt::harness::AgentHarness& harness() const noexcept { return harness_; }
  void sync_session_manager_from_harness();
  void sync_harness_from_session_manager();

  const qt::harness::SessionManagerContext& seeded_context() const noexcept {
    return seeded_context_;
  }

  const RuntimeHostSeedState& seed_state() const noexcept {
    return seed_state_;
  }

 private:
  friend foundation::Result<RuntimeHost> create_runtime_host(
      RuntimeHostRequest request);

  RuntimeHost(qt::harness::SessionState session_state,
              qt::harness::SessionManager session_manager,
              qt::harness::AgentHarness harness,
              qt::harness::SessionManagerContext seeded_context,
              RuntimeHostSeedState seed_state);

  qt::harness::SessionState session_state_;
  qt::harness::SessionManager session_manager_;
  qt::harness::AgentHarness harness_;
  qt::harness::SessionManagerContext seeded_context_;
  RuntimeHostSeedState seed_state_;
};

foundation::Result<RuntimeHost> create_runtime_host(RuntimeHostRequest request);

}  // namespace qt::app
