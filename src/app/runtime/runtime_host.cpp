#include "qt/app/runtime/runtime_host.hpp"

#include <stdexcept>
#include <utility>

namespace qt::app {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

enum class MetadataBoolDefault {
  disabled,
  enabled,
};

std::string configured_thinking_level(const qt::agent::AgentConfig& config) {
  if (!config.metadata.is_object()) {
    return "off";
  }

  const auto it = config.metadata.find("thinkingLevel");
  if (it == config.metadata.end() || !it->is_string()) {
    return "off";
  }
  return it->get<std::string>();
}

bool metadata_bool(const qt::agent::AgentConfig& config, const char* key,
                   MetadataBoolDefault default_value) {
  const bool fallback = default_value == MetadataBoolDefault::enabled;
  if (!config.metadata.is_object()) {
    return fallback;
  }

  const auto it = config.metadata.find(key);
  if (it == config.metadata.end() || !it->is_boolean()) {
    return fallback;
  }
  return it->get<bool>();
}

std::optional<qt::ai::ModelRef> restore_model_ref(
    const qt::ai::ModelRef& fallback,
    const std::optional<qt::harness::SessionManagerModelState>& persisted) {
  if (!persisted.has_value() || persisted->provider.empty() ||
      persisted->model_id.empty()) {
    return std::nullopt;
  }

  if (persisted->provider == fallback.provider) {
    qt::ai::ModelRef restored = fallback;
    restored.name = persisted->model_id;
    return restored;
  }

  if (fallback.provider.empty() && fallback.api.empty()) {
    return qt::ai::ModelRef{persisted->provider, std::string{},
                            persisted->model_id};
  }

  return std::nullopt;
}

std::optional<std::string> resolved_context_leaf_id(
    const qt::harness::SessionManager& session_manager,
    const std::optional<std::string>& requested_leaf_id) {
  if (requested_leaf_id.has_value() &&
      session_manager.get_entry(*requested_leaf_id) != nullptr) {
    return requested_leaf_id;
  }
  return session_manager.get_leaf_id();
}

bool same_model_ref(const qt::ai::ModelRef& left, const qt::ai::ModelRef& right) {
  return left.provider == right.provider && left.api == right.api &&
         left.name == right.name;
}

}  // namespace

RuntimeHost::RuntimeHost(qt::harness::SessionState session_state,
                         qt::harness::SessionManager session_manager,
                         qt::harness::AgentHarness harness,
                         qt::harness::SessionManagerContext seeded_context,
                         RuntimeHostSeedState seed_state)
    : session_state_(std::move(session_state)),
      session_manager_(std::move(session_manager)),
      harness_(std::move(harness)),
      seeded_context_(std::move(seeded_context)),
      seed_state_(std::move(seed_state)) {}

void RuntimeHost::sync_session_manager_from_harness() {
  session_manager_ =
      qt::harness::SessionManager(harness_.session_manager().entries());
  const auto selected_leaf_id = harness_.session_manager().get_leaf_id();
  if (!selected_leaf_id.has_value()) {
    session_manager_.reset_leaf();
  } else if (session_manager_.get_entry(*selected_leaf_id) != nullptr) {
    const auto status = session_manager_.branch(*selected_leaf_id);
    if (!status.ok()) {
      throw std::logic_error(
          "Failed to synchronize runtime host session leaf from harness");
    }
  }
}

void RuntimeHost::sync_harness_from_session_manager() {
  const auto selected_leaf_id = session_manager_.get_leaf_id();
  harness_.restore_session_entries(session_manager_.entries(), selected_leaf_id,
                                   !selected_leaf_id.has_value());

  const auto context = selected_leaf_id.has_value()
                           ? session_manager_.build_session_context(
                                 *selected_leaf_id)
                           : qt::harness::SessionManagerContext{};
  auto& agent_state = harness_.agent().state();
  agent_state.transcript = context.messages;
  agent_state.pending.clear();

  seeded_context_ = context;
  seed_state_.context_leaf_id = selected_leaf_id;
  seed_state_.transcript_message_count = context.messages.size();
  seed_state_.restored_transcript = !context.messages.empty();
}

Result<RuntimeHost> create_runtime_host(RuntimeHostRequest request) {
  if (request.provider == nullptr) {
    return Result<RuntimeHost>::failure(ErrorCode::invalid_argument,
                                        "runtime host provider is required");
  }

  qt::harness::SessionState session_state(std::move(request.session_metadata));
  qt::harness::SessionManager session_manager(std::move(request.session_entries));
  qt::harness::SessionManagerContext context =
      request.selected_leaf_id.has_value()
          ? session_manager.build_session_context(*request.selected_leaf_id)
          : session_manager.build_session_context();
  if (request.selected_leaf_id.has_value() &&
      session_manager.get_entry(*request.selected_leaf_id) != nullptr) {
    const auto selected = session_manager.branch(*request.selected_leaf_id);
    if (!selected.ok()) {
      return Result<RuntimeHost>::failure(selected.error().code,
                                          selected.error().message,
                                          selected.error().system_error);
    }
  }
  RuntimeHostSeedState seed_state;
  seed_state.context_leaf_id =
      resolved_context_leaf_id(session_manager, request.selected_leaf_id);
  seed_state.transcript_message_count = context.messages.size();
  seed_state.restored_transcript = !context.messages.empty();

  qt::harness::AgentHarness harness(std::move(request.agent_config),
                                    std::move(request.provider),
                                    std::move(request.provider_options));

  auto& agent_state = harness.agent().state();
  agent_state.agent_id = session_state.metadata().session_id;
  agent_state.transcript = context.messages;
  agent_state.pending.clear();

  const bool allow_session_model_restore = metadata_bool(
      harness.agent().config(), "allowSessionModelRestore",
      MetadataBoolDefault::enabled);
  const auto restored_model = restore_model_ref(harness.model(), context.model);
  if (allow_session_model_restore && restored_model.has_value() &&
      !same_model_ref(*restored_model, harness.model())) {
    harness.set_model(*restored_model);
    seed_state.restored_model = true;
  }

  const bool allow_session_thinking_restore = metadata_bool(
      harness.agent().config(), "allowSessionThinkingRestore",
      MetadataBoolDefault::enabled);
  const std::string current_thinking = configured_thinking_level(
      harness.agent().config());
  if (allow_session_thinking_restore && !context.thinking_level.empty() &&
      context.thinking_level != current_thinking) {
    harness.set_thinking_level(context.thinking_level);
    seed_state.restored_thinking_level = true;
  }
  harness.restore_session_entries(session_manager.entries(),
                                  seed_state.context_leaf_id);
  harness.emit_session_start(std::move(request.session_start_reason),
                             std::move(request.previous_session_file));

  return RuntimeHost(std::move(session_state), std::move(session_manager),
                     std::move(harness), std::move(context),
                     std::move(seed_state));
}

}  // namespace qt::app
