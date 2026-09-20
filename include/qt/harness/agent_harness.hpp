#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "qt/agent/agent.hpp"
#include "qt/foundation/process.hpp"
#include "qt/harness/compaction.hpp"
#include "qt/harness/prompt_templates.hpp"
#include "qt/harness/session_manager.hpp"
#include "qt/harness/skills.hpp"

namespace qt::harness {

enum class AgentHarnessPhase {
  idle,
  running,
  compaction,
  branch_summary,
  aborted,
};

enum class AgentHarnessWriteKind {
  committed_message,
  pending_message,
};

enum class LogicalInputKind {
  unknown,
  prompt,
  tool_call,
  steer,
  follow_up,
  next_turn,
};

struct AgentHarnessWrite {
  AgentHarnessWriteKind kind = AgentHarnessWriteKind::committed_message;
  std::optional<std::uint64_t> sequence;
  qt::ai::Message message;
};

struct AgentHarnessSavePoint {
  std::uint64_t sequence = 0;
  std::uint64_t turn_index = 0;
  bool had_pending_writes = false;
  std::vector<AgentHarnessWrite> writes;
};

struct AgentHarnessQueueUpdate {
  std::vector<qt::ai::Message> steer;
  std::vector<qt::ai::Message> follow_up;
  std::vector<qt::ai::Message> next_turn;
};

struct AgentHarnessResources {
  std::vector<LoadedSkillRecord> skills;
  std::vector<LoadedPromptTemplateRecord> prompt_templates;
};

struct AgentHarnessResourcesUpdate {
  AgentHarnessResources resources;
  AgentHarnessResources previous_resources;
};

struct AgentHarnessToolsUpdate {
  std::vector<qt::ai::ToolDefinition> tools;
  std::vector<qt::ai::ToolDefinition> previous_tools;
  std::vector<std::string> active_tool_names;
  std::vector<std::string> previous_active_tool_names;
};

struct AgentHarnessSavePointEvent {
  bool had_pending_mutations = false;
};

struct AgentHarnessAbortEvent {
  std::vector<qt::ai::Message> cleared_steer;
  std::vector<qt::ai::Message> cleared_follow_up;
};

struct AgentHarnessSettledEvent {
  std::size_t next_turn_count = 0;
};

struct AgentHarnessProvidedCompaction {
  std::string summary;
  std::string first_kept_entry_id;
  std::int64_t tokens_before = 0;
  std::optional<Json> details;
};

class AgentHarness final : public qt::agent::EventSink {
 public:
  struct BeforeProviderRequestEvent {
    qt::ai::ModelRef model;
    qt::ai::CompletionRequest request;
    qt::ai::ProviderOptions stream_options;
  };

  struct BeforeAgentStartEvent {
    qt::ai::Message prompt;
    AgentHarnessResources resources;
  };

  struct BeforeAgentStartResult {
    std::vector<qt::ai::Message> messages;
  };

  struct AgentStartEvent {};

  struct AgentEndEvent {
    std::vector<qt::ai::Message> messages;
    bool ok = false;
    std::uint64_t turn_index = 0U;
    std::size_t tool_request_count = 0U;
    std::size_t tool_error_count = 0U;
    std::optional<std::string> error;
    std::optional<qt::ai::ModelRef> model;
    std::optional<qt::ai::Usage> usage;
  };

  struct BeforeProviderPayloadEvent {
    qt::ai::ModelRef model;
    qt::ai::Json payload;
  };

  struct ContextEvent {
    std::vector<qt::ai::Message> messages;
    std::uint64_t model_context_sequence = 0;
    std::uint64_t projection_epoch = 0;
    std::uint64_t turn_index = 0;
    std::optional<std::uint64_t> logical_input_sequence;
    LogicalInputKind logical_input_kind = LogicalInputKind::unknown;
  };

  struct ModelContextEvent {
    qt::ai::CompletionRequest request;
    std::uint64_t model_context_sequence = 0;
    std::uint64_t projection_epoch = 0;
    std::uint64_t turn_index = 0;
    std::optional<std::uint64_t> logical_input_sequence;
    LogicalInputKind logical_input_kind = LogicalInputKind::unknown;
    std::size_t call_index_for_logical_input = 0;
    std::shared_ptr<qt::foundation::ProcessCancellationToken>
        cancellation_token;
  };

  struct AfterProviderResponseEvent {
    qt::ai::ModelRef model;
    qt::ai::CompletionRequest request;
    qt::ai::ProviderOptions stream_options;
    qt::ai::CompletionResponse response;
  };

  struct ToolCallEvent {
    std::uint64_t turn_index = 0;
    std::size_t tool_call_index = 0;
    std::size_t completed_tool_calls_before = 0;
    qt::tools::ToolRequest request;
  };

  struct ToolResultEvent {
    std::uint64_t turn_index = 0;
    std::size_t tool_call_index = 0;
    std::size_t completed_tool_calls_before = 0;
    qt::tools::ToolRequest request;
    qt::tools::ToolResult result;
  };

  struct BeforeCompactEvent {
    ModelBackedCompactionPlan preparation;
    std::vector<SessionEntry> branch_entries;
    std::optional<std::string> custom_instructions;
  };

  struct BeforeCompactResult {
    bool cancel = false;
    std::optional<AgentHarnessProvidedCompaction> compaction;
  };

  struct CompactEvent {
    std::string entry_id;
    SessionCompactionEntry compaction;
    bool from_hook = false;
  };

  struct CompactionStartEvent {
    std::string reason = "manual";
  };

  struct CompactionEndEvent {
    std::string reason = "manual";
    std::optional<ModelBackedCompactionResult> result;
    bool aborted = false;
    bool will_retry = false;
    std::optional<std::string> error_message;
  };

  struct SessionStartEvent {
    std::string reason = "startup";
    std::optional<std::string> previous_session_file;
  };

  struct SessionShutdownEvent {
    std::string reason = "quit";
    std::optional<std::string> target_session_file;
  };

  struct SessionBeforeTreeEvent {
    qt::ai::Json preparation = qt::ai::Json::object();
  };

  struct SessionTreeEvent {
    std::optional<std::string> new_leaf_id;
    std::optional<std::string> old_leaf_id;
    std::optional<qt::ai::Json> summary_entry;
    std::optional<bool> from_extension;
  };

  using EventListener = std::function<void(const qt::agent::AgentEvent&)>;
  using SubscriptionId = std::uint64_t;
  using SubscribedEvent =
      std::variant<qt::agent::AgentEvent,
                   AgentHarnessQueueUpdate,
                   AgentHarnessResourcesUpdate,
                   AgentHarnessToolsUpdate,
                   BeforeAgentStartEvent,
                   AgentStartEvent,
                   AgentEndEvent,
                   AgentHarnessSavePointEvent,
                   AgentHarnessAbortEvent,
                   AgentHarnessSettledEvent,
                   BeforeCompactEvent,
                   CompactEvent,
                   CompactionStartEvent,
                   CompactionEndEvent,
                   SessionStartEvent,
                   SessionShutdownEvent,
                   SessionBeforeTreeEvent,
                   SessionTreeEvent>;
  using Subscriber = std::function<void(const SubscribedEvent&)>;
  using BeforeAgentStartHook =
      std::function<std::optional<BeforeAgentStartResult>(
          const BeforeAgentStartEvent&)>;
  using BeforeProviderRequestHook =
      std::function<std::optional<qt::ai::ProviderOptions>(
          const BeforeProviderRequestEvent&)>;
  using BeforeProviderPayloadHook =
      std::function<std::optional<qt::ai::Json>(
          const BeforeProviderPayloadEvent&)>;
  using ContextHook =
      std::function<std::optional<std::vector<qt::ai::Message>>(
          const ContextEvent&)>;
  using ModelContextAugmentor =
      std::function<std::optional<qt::ai::CompletionRequest>(
          const ModelContextEvent&)>;
  using AfterProviderResponseHook =
      std::function<void(const AfterProviderResponseEvent&)>;
  using ToolCallHook = std::function<void(const ToolCallEvent&)>;
  using ToolResultHook = std::function<void(const ToolResultEvent&)>;
  using BeforeCompactHook =
      std::function<std::optional<BeforeCompactResult>(
          const BeforeCompactEvent&)>;

  AgentHarness(qt::agent::AgentConfig config,
               std::shared_ptr<qt::ai::Provider> provider,
               qt::ai::ProviderOptions options = {});
  ~AgentHarness() noexcept;
  AgentHarness(AgentHarness&& other) noexcept;
  AgentHarness& operator=(AgentHarness&& other) noexcept;

  AgentHarness(const AgentHarness&) = delete;
  AgentHarness& operator=(const AgentHarness&) = delete;

  qt::agent::Agent& agent() noexcept { return agent_; }
  const qt::agent::Agent& agent() const noexcept { return agent_; }
  std::shared_ptr<qt::ai::Provider> provider() const noexcept {
    return provider_;
  }

  AgentHarnessPhase phase() const noexcept { return phase_; }
  bool busy() const noexcept { return phase_ == AgentHarnessPhase::running; }
  const qt::ai::ModelRef& model() const noexcept { return config_.model; }
  const std::string& thinking_level() const noexcept { return thinking_level_; }
  qt::ai::ProviderOptions stream_options() const { return options_; }

  const std::vector<qt::agent::AgentEvent>& events() const noexcept {
    return events_;
  }
  const SessionManager& session_manager() const noexcept {
    return session_manager_;
  }
  const std::vector<AgentHarnessWrite>& committed_writes() const noexcept {
    return committed_writes_;
  }
  std::vector<AgentHarnessWrite> pending_writes() const;
  std::vector<AgentHarnessWrite> session_writes() const;
  std::vector<SessionEntry> pending_session_entries() const {
    return pending_session_entries_;
  }
  const std::vector<AgentHarnessSavePoint>& save_points() const noexcept {
    return save_points_;
  }
  const std::vector<AgentHarnessQueueUpdate>& queue_update_events()
      const noexcept {
    return queue_update_events_;
  }
  const AgentHarnessResources& resources() const noexcept { return resources_; }
  const std::vector<AgentHarnessResourcesUpdate>& resource_update_events()
      const noexcept {
    return resource_update_events_;
  }
  const std::vector<AgentHarnessToolsUpdate>& tool_update_events() const noexcept {
    return tool_update_events_;
  }
  const std::vector<qt::ai::ToolDefinition>& tool_definitions() const noexcept {
    return tool_definitions_;
  }
  const std::vector<std::string>& active_tool_names() const noexcept {
    return active_tool_names_;
  }
  qt::agent::QueueMode steering_mode() const noexcept {
    return config_.steering_mode;
  }
  qt::agent::QueueMode follow_up_mode() const noexcept {
    return config_.follow_up_mode;
  }
  const std::vector<qt::agent::QueuedMessage>& steering_queue() const noexcept {
    return steering_queue_;
  }
  const std::vector<qt::agent::QueuedMessage>& follow_up_queue()
      const noexcept {
    return follow_up_queue_;
  }
  const std::vector<qt::ai::Message>& next_turn_queue() const noexcept {
    return next_turn_queue_;
  }
  const std::vector<BeforeAgentStartEvent>& before_agent_start_events()
      const noexcept {
    return before_agent_start_events_;
  }
  const std::vector<BeforeProviderRequestEvent>&
  before_provider_request_events() const noexcept {
    return before_provider_request_events_;
  }
  const std::vector<BeforeProviderPayloadEvent>&
  before_provider_payload_events() const noexcept {
    return before_provider_payload_events_;
  }
  const std::vector<ContextEvent>& context_events() const noexcept {
    return context_events_;
  }
  const std::vector<ModelContextEvent>& model_context_events() const noexcept {
    return model_context_events_;
  }
  std::uint64_t model_context_projection_epoch() const noexcept {
    return model_context_projection_epoch_;
  }
  const std::vector<AfterProviderResponseEvent>&
  after_provider_response_events() const noexcept {
    return after_provider_response_events_;
  }
  const std::vector<ToolCallEvent>& tool_call_events() const noexcept {
    return tool_call_events_;
  }
  const std::vector<ToolResultEvent>& tool_result_events() const noexcept {
    return tool_result_events_;
  }
  const std::vector<BeforeCompactEvent>& before_compact_events()
      const noexcept {
    return before_compact_events_;
  }
  const std::vector<CompactEvent>& compact_events() const noexcept {
    return compact_events_;
  }
  const std::vector<SessionStartEvent>& session_start_events() const noexcept {
    return session_start_events_;
  }
  const std::vector<SessionShutdownEvent>& session_shutdown_events()
      const noexcept {
    return session_shutdown_events_;
  }
  const std::vector<SessionBeforeTreeEvent>& session_before_tree_events()
      const noexcept {
    return session_before_tree_events_;
  }
  const std::vector<SessionTreeEvent>& session_tree_events() const noexcept {
    return session_tree_events_;
  }
  const AgentHarnessSavePoint* last_save_point() const noexcept;
  const std::optional<qt::agent::TurnResult>& last_result() const noexcept {
    return last_result_;
  }

  void set_event_listener(EventListener listener);
  SubscriptionId subscribe(Subscriber listener);
  bool unsubscribe(SubscriptionId id);
  void add_before_agent_start_hook(BeforeAgentStartHook hook);
  void clear_before_agent_start_hooks();
  void add_before_provider_request_hook(BeforeProviderRequestHook hook);
  void clear_before_provider_request_hooks();
  void add_before_provider_payload_hook(BeforeProviderPayloadHook hook);
  void clear_before_provider_payload_hooks();
  void add_context_hook(ContextHook hook);
  void clear_context_hooks();
  // Augmentors are chained after ordinary context hooks and before provider
  // request/payload hooks. Their requests are provider-only and are not
  // committed to the agent or session transcript.
  void add_model_context_augmentor(ModelContextAugmentor augmentor);
  void clear_model_context_augmentors();
  // Invalidates caches keyed by (projection_epoch, logical_input_sequence).
  // Restored history deliberately has no logical input sequence.
  void reset_model_context_projection() noexcept;
  void add_after_provider_response_hook(AfterProviderResponseHook hook);
  void clear_after_provider_response_hooks();
  void add_tool_call_hook(ToolCallHook hook);
  void add_before_tool_hook(qt::agent::BeforeToolHook hook);
  void add_tool_result_transform(qt::agent::ToolResultTransformHook hook);
  void clear_tool_call_hooks();
  void add_tool_result_hook(ToolResultHook hook);
  void clear_tool_result_hooks();
  void add_before_compact_hook(BeforeCompactHook hook);
  void clear_before_compact_hooks();
  void clear_events();
  void reset_phase() noexcept;
  void set_model(qt::ai::ModelRef model);
  void set_model(qt::ai::ModelRef model,
                 std::shared_ptr<qt::ai::Provider> provider,
                 qt::ai::ProviderOptions options);
  void set_provider(std::shared_ptr<qt::ai::Provider> provider,
                    qt::ai::ProviderOptions options);
  void set_thinking_level(std::string level);
  void set_stream_options(qt::ai::ProviderOptions options);
  void set_resources(AgentHarnessResources resources);
  void set_tool_definitions(
      std::vector<qt::ai::ToolDefinition> tools,
      std::optional<std::vector<std::string>> active_tool_names = std::nullopt);
  void set_active_tool_names(std::vector<std::string> active_tool_names);
  void set_external_filesystem_access(bool allowed);
  void restore_session_entries(
      std::vector<SessionEntry> entries,
      std::optional<std::string> selected_leaf_id = std::nullopt,
      bool reset_leaf_to_root = false);
  void set_steering_mode(qt::agent::QueueMode mode) noexcept;
  void set_follow_up_mode(qt::agent::QueueMode mode) noexcept;
  std::optional<std::string> set_session_name(
      std::optional<std::string> name = std::nullopt);
  std::optional<std::string> append_extension_state(
      std::string custom_type, Json data = Json::object());
  std::optional<std::string> append_message(qt::ai::Message message);

  qt::agent::TurnResult submit_prompt(std::string text);
  qt::agent::TurnResult submit_prompt(qt::ai::Message message);
  qt::agent::TurnResult submit_tool_call(std::string user_text,
                                         qt::tools::ToolRequest request);
  qt::agent::QueuedMessage steer(std::string text);
  qt::agent::QueuedMessage steer(qt::ai::Message message);
  qt::agent::QueuedMessage follow_up(std::string text);
  qt::agent::QueuedMessage follow_up(qt::ai::Message message);
  void next_turn(std::string text);
  void next_turn(qt::ai::Message message);
  AgentHarnessAbortEvent request_abort();
  qt::foundation::Result<ModelBackedCompactionResult> compact_session(
      ModelBackedCompactionRequest request = {});
  qt::foundation::Result<ModelBackedCompactionResult> compact_session(
      std::string custom_instructions);
  qt::foundation::Result<ModelBackedBranchSummaryResult> summarize_branch(
      const std::vector<SessionEntry>& entries,
      ModelBackedBranchSummaryRequest request = {});
  void emit_session_start(std::string reason,
                          std::optional<std::string> previous_session_file =
                              std::nullopt);
  void emit_session_shutdown(std::string reason,
                             std::optional<std::string> target_session_file =
                                 std::nullopt);
  void emit_session_before_tree(qt::ai::Json preparation);
  void emit_session_tree(std::optional<std::string> new_leaf_id,
                         std::optional<std::string> old_leaf_id,
                         std::optional<qt::ai::Json> summary_entry =
                             std::nullopt,
                         std::optional<bool> from_extension = std::nullopt);

  void push(const qt::agent::AgentEvent& event) override;

 private:
  friend class AgentHarnessProviderProxy;
  struct ProviderProxyState;

  static qt::ai::Message make_user_message(std::string text);
  void bind_provider_proxy_state() noexcept;
  void clear_provider_proxy_state() noexcept;
  qt::ai::CompletionRequest apply_runtime_request_snapshot(
      const qt::ai::CompletionRequest& request) const;
  qt::ai::CompletionRequest apply_context_hooks(
      const qt::ai::CompletionRequest& request);
  qt::ai::CompletionRequest apply_model_context_augmentors(
      const qt::ai::CompletionRequest& request);
  qt::ai::ProviderOptions apply_before_provider_request_hooks(
      const qt::ai::CompletionRequest& request,
      const qt::ai::ProviderOptions& options);
  qt::ai::CompletionRequest apply_before_provider_payload_hooks(
      const qt::ai::CompletionRequest& request);
  void record_after_provider_response(
      const qt::ai::CompletionRequest& request,
      const qt::ai::ProviderOptions& options,
      const qt::ai::CompletionResponse& response);
  void record_tool_call(const qt::agent::ToolHookContext& context,
                        const qt::tools::ToolRequest& request);
  void record_tool_result(const qt::agent::ToolHookContext& context,
                          const qt::tools::ToolRequest& request,
                          const qt::tools::ToolResult& result);
  std::optional<BeforeCompactResult> apply_before_compact_hooks(
      const BeforeCompactEvent& event);
  void record_compact_event(const std::string& entry_id,
                            const SessionCompactionEntry& compaction,
                            bool from_hook);
  void maybe_auto_compact_after_turn(const qt::agent::TurnResult& result);
  void emit_subscribed_event(const SubscribedEvent& event);
  void require_can_submit() const;
  std::vector<qt::ai::Message> apply_before_agent_start_hooks(
      const qt::ai::Message& prompt);
  void initialize_cached_agent_hooks();
  void invalidate_agent_config() noexcept;
  void invalidate_filtered_tool_definitions() noexcept;
  void sync_agent_for_next_turn();
  void apply_pending_runtime_updates_at_save_point();
  void refresh_applied_turn_snapshot();
  std::optional<std::string> persist_or_queue_session_entry(SessionEntry entry);
  void flush_pending_session_entries();
  void record_committed_session_entry(const qt::ai::Message& message);
  std::optional<SessionEntry> make_session_message_entry(
      const qt::ai::Message& message) const;
  void apply_thinking_level_metadata();
  const std::vector<qt::ai::ToolDefinition>& filtered_tool_definitions();
  void validate_active_tool_names(
      const std::vector<std::string>& active_tool_names) const;
  void require_can_queue_runtime_message(const char* operation) const;
  bool remove_runtime_queue_message(std::uint64_t sequence);
  void record_queue_update();
  void drain_next_turn_queue();
  void register_logical_input(const qt::agent::QueuedMessage& queued,
                              LogicalInputKind kind);
  void update_model_context_input(const qt::agent::AgentEvent& event);
  void record_committed_write(const qt::agent::AgentEvent& event);
  void record_save_point(const qt::agent::AgentEvent& event);
  AgentHarnessAbortEvent record_abort();
  void record_settled();

  qt::agent::AgentConfig config_;
  std::shared_ptr<qt::ai::Provider> provider_;
  qt::ai::ProviderOptions options_;
  std::shared_ptr<ProviderProxyState> provider_proxy_state_;
  std::shared_ptr<qt::ai::Provider> routed_provider_;
  qt::agent::Agent agent_;
  qt::agent::AgentConfig effective_config_;
  std::vector<qt::ai::ToolDefinition> filtered_tool_definitions_cache_;
  qt::agent::BeforeToolHook harness_before_tool_hook_;
  qt::agent::AfterToolHook harness_after_tool_hook_;
  std::function<void()> harness_prepare_next_turn_hook_;
  bool agent_config_dirty_ = true;
  bool filtered_tool_definitions_dirty_ = true;
  std::string thinking_level_ = "off";
  std::optional<qt::ai::ProviderOptions> pending_stream_options_;
  qt::ai::ModelRef applied_model_;
  qt::ai::Json applied_metadata_ = qt::ai::Json::object();
  std::vector<qt::ai::ToolDefinition> applied_tool_definitions_;
  SessionManager session_manager_;
  std::vector<SessionEntry> pending_session_entries_;
  std::vector<qt::ai::Message> pending_session_transcript_messages_;
  bool pending_session_entries_flushed_at_turn_end_ = false;
  AgentHarnessPhase phase_ = AgentHarnessPhase::idle;
  std::vector<qt::agent::AgentEvent> events_;
  std::vector<AgentHarnessWrite> committed_writes_;
  std::vector<AgentHarnessSavePoint> save_points_;
  std::vector<qt::ai::Message> next_turn_queue_;
  std::vector<AgentHarnessQueueUpdate> queue_update_events_;
  AgentHarnessResources resources_;
  std::vector<AgentHarnessResourcesUpdate> resource_update_events_;
  std::vector<AgentHarnessToolsUpdate> tool_update_events_;
  std::vector<qt::ai::ToolDefinition> tool_definitions_;
  std::vector<std::string> active_tool_names_;
  std::vector<qt::agent::QueuedMessage> steering_queue_;
  std::vector<qt::agent::QueuedMessage> follow_up_queue_;
  mutable std::recursive_mutex queue_mutex_;
  std::vector<BeforeAgentStartEvent> before_agent_start_events_;
  std::vector<BeforeAgentStartHook> before_agent_start_hooks_;
  std::vector<ContextEvent> context_events_;
  std::vector<ContextHook> context_hooks_;
  std::vector<ModelContextEvent> model_context_events_;
  std::vector<ModelContextAugmentor> model_context_augmentors_;
  std::map<std::uint64_t, LogicalInputKind> logical_input_kinds_;
  std::map<std::uint64_t,
           std::shared_ptr<qt::foundation::ProcessCancellationToken>>
      logical_input_cancellation_;
  std::optional<std::uint64_t> current_logical_input_sequence_;
  LogicalInputKind current_logical_input_kind_ = LogicalInputKind::unknown;
  std::uint64_t current_model_context_turn_index_ = 0;
  std::uint64_t model_context_projection_epoch_ = 1U;
  std::uint64_t next_model_context_sequence_ = 1U;
  std::optional<std::uint64_t> last_augmented_logical_input_sequence_;
  std::uint64_t last_augmented_projection_epoch_ = 0;
  std::size_t call_index_for_logical_input_ = 0U;
  std::vector<BeforeProviderRequestEvent> before_provider_request_events_;
  std::vector<BeforeProviderRequestHook> before_provider_request_hooks_;
  std::vector<BeforeProviderPayloadEvent> before_provider_payload_events_;
  std::vector<BeforeProviderPayloadHook> before_provider_payload_hooks_;
  std::vector<AfterProviderResponseEvent> after_provider_response_events_;
  std::vector<AfterProviderResponseHook> after_provider_response_hooks_;
  std::vector<ToolCallEvent> tool_call_events_;
  std::vector<ToolCallHook> tool_call_hooks_;
  std::vector<ToolResultEvent> tool_result_events_;
  std::vector<ToolResultHook> tool_result_hooks_;
  std::vector<BeforeCompactEvent> before_compact_events_;
  std::vector<BeforeCompactHook> before_compact_hooks_;
  std::vector<CompactEvent> compact_events_;
  std::vector<SessionStartEvent> session_start_events_;
  std::vector<SessionShutdownEvent> session_shutdown_events_;
  std::vector<SessionBeforeTreeEvent> session_before_tree_events_;
  std::vector<SessionTreeEvent> session_tree_events_;
  SubscriptionId next_subscription_id_ = 1U;
  std::vector<std::pair<SubscriptionId, Subscriber>> subscribers_;
  std::optional<qt::agent::TurnResult> last_result_;
  EventListener listener_;
};

const char* to_string(AgentHarnessPhase value) noexcept;
const char* to_string(AgentHarnessWriteKind value) noexcept;
const char* to_string(LogicalInputKind value) noexcept;

}  // namespace qt::harness
