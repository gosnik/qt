#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "qt/agent/models.hpp"
#include "qt/ai/provider.hpp"
#include "qt/tools/registry.hpp"
#include "qt/tools/tool.hpp"

namespace qt::agent {

constexpr std::uint32_t kDefaultAutoRetryMaxAttempts = 3U;
constexpr std::int64_t kDefaultAutoRetryBaseDelayMs = 2000;

struct ToolHookContext {
  std::uint64_t turn_index = 0;
  std::size_t tool_call_index = 0;
  std::size_t completed_tool_calls_before = 0;
};

using BeforeToolHook =
    std::function<std::optional<tools::ToolResult>(
        const ToolHookContext&, const tools::ToolRequest&)>;
using TransformToolHook =
    std::function<std::optional<tools::ToolRequest>(
        const ToolHookContext&, const tools::ToolRequest&)>;
using AfterToolHook =
    std::function<void(const ToolHookContext&, const tools::ToolRequest&,
                       const tools::ToolResult&)>;
using ToolResultTransformHook =
    std::function<std::optional<tools::ToolResult>(
        const ToolHookContext&, const tools::ToolRequest&,
        const tools::ToolResult&)>;

enum class ToolExecutionMode {
  sequential,
  parallel,
};

struct AgentConfig {
  ai::ModelRef model;
  std::vector<ai::ToolDefinition> tools;
  ai::ToolChoice tool_choice;
  std::optional<double> temperature;
  std::optional<std::int64_t> max_output_tokens;
  ai::Json metadata = ai::Json::object();
  std::shared_ptr<tools::ToolRegistry> tool_registry;
  tools::ToolContext tool_context;
  std::vector<TransformToolHook> transform_tool_hooks;
  std::vector<BeforeToolHook> before_tool_hooks;
  std::vector<AfterToolHook> after_tool_hooks;
  std::vector<ToolResultTransformHook> tool_result_transforms;
  std::function<void()> prepare_next_turn_hook;
  std::function<std::vector<ai::Json>()> take_subagent_events;
  std::function<void(const std::vector<ai::Message>&)> publish_subagent_context;
  ToolExecutionMode tool_execution_mode = ToolExecutionMode::sequential;
  QueueMode steering_mode = QueueMode::one_at_a_time;
  QueueMode follow_up_mode = QueueMode::one_at_a_time;
  bool auto_retry_enabled = false;
  std::uint32_t auto_retry_max_attempts = kDefaultAutoRetryMaxAttempts;
  std::int64_t auto_retry_base_delay_ms = kDefaultAutoRetryBaseDelayMs;
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void push(const AgentEvent& event) = 0;
};

class Agent {
 public:
  Agent(AgentConfig config, std::shared_ptr<ai::Provider> provider,
        ai::ProviderOptions options = {});
  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;
  Agent(Agent&& other) noexcept;
  Agent& operator=(Agent&& other) noexcept;

  const AgentConfig& config() const noexcept { return config_; }
  const AgentState& state() const noexcept { return state_; }
  AgentState& state() noexcept { return state_; }
  std::shared_ptr<ai::Provider> provider() const noexcept { return provider_; }
  const ai::ProviderOptions& provider_options() const noexcept { return options_; }

  QueuedMessage enqueue(ai::Message message, EventSink* sink = nullptr);
  QueuedMessage steer(ai::Message message, EventSink* sink = nullptr);
  QueuedMessage follow_up(ai::Message message, EventSink* sink = nullptr);
  void clear_steering_queue() noexcept;
  void clear_follow_up_queue() noexcept;
  void clear_all_queues() noexcept;
  void reset() noexcept;
  bool has_queued_messages() const noexcept;
  QueueMode steering_mode() const noexcept { return config_.steering_mode; }
  void set_steering_mode(QueueMode mode) noexcept { config_.steering_mode = mode; }
  QueueMode follow_up_mode() const noexcept { return config_.follow_up_mode; }
  void set_follow_up_mode(QueueMode mode) noexcept {
    config_.follow_up_mode = mode;
  }
  void set_provider_options(ai::ProviderOptions options);
  void request_abort() noexcept;
  bool abort_requested() const noexcept {
    return abort_requested_.load(std::memory_order_acquire);
  }
  bool turn_running() const noexcept { return turn_running_; }
  TurnResult run_turn(EventSink& sink);
  TurnResult run_tool_call(
      tools::ToolRequest request,
      std::shared_ptr<qt::foundation::ProcessCancellationToken>
          cancellation_token,
      EventSink& sink);

 private:
  AgentEvent make_event(AgentEventKind kind, std::uint64_t turn_index) const;
  ai::CompletionRequest build_request() const;
  ai::CompletionResponse perform_completion(std::uint64_t turn_index,
                                            TurnResult& result,
                                            EventSink& sink);
  void record_provider_event(const ai::StreamEvent& provider_event,
                             std::uint64_t turn_index, TurnResult& result,
                             EventSink& sink);
  bool fail_if_abort_requested(std::uint64_t turn_index, TurnResult& result,
                               EventSink& sink);
  void cancel_active_pending_messages();
  void clear_runtime_queues() noexcept;
  void commit_pending_messages(std::uint64_t turn_index, TurnResult& result,
                               EventSink& sink);
  std::size_t drain_runtime_queue(std::vector<QueuedMessage>& queue,
                                  QueueMode mode, std::uint64_t turn_index,
                                  TurnResult& result, EventSink& sink);
  void commit_message(ai::Message message, std::uint64_t turn_index,
                      EventSink& sink,
                      std::optional<QueuedMessage> queued_message = std::nullopt);
  void remove_last_assistant_message() noexcept;
  std::int64_t auto_retry_delay_ms(std::uint32_t attempt) const noexcept;
  bool can_auto_retry(const ai::CompletionResponse& response,
                      std::uint32_t attempt) const;
  void emit_auto_retry_start(std::uint64_t turn_index, std::uint32_t attempt,
                             std::int64_t delay_ms, const std::string& error,
                             EventSink& sink);
  void emit_auto_retry_end(std::uint64_t turn_index, std::uint32_t attempt,
                           bool success, std::optional<std::string> final_error,
                           EventSink& sink);
  std::size_t execute_tool_calls(const ai::Message& message,
                                 std::uint64_t turn_index, TurnResult& result,
                                 EventSink& sink);
  void emit(EventSink* sink, AgentEvent event);

  AgentConfig config_;
  std::shared_ptr<ai::Provider> provider_;
  ai::ProviderOptions options_;
  AgentState state_;
  std::vector<QueuedMessage> steering_queue_;
  std::vector<QueuedMessage> follow_up_queue_;
  std::size_t active_pending_count_ = 0;
  std::atomic_bool abort_requested_{false};
  std::shared_ptr<std::atomic_bool> provider_abort_signal_ =
      std::make_shared<std::atomic_bool>(false);
  bool turn_running_ = false;
};

class VectorEventSink final : public EventSink {
 public:
  void push(const AgentEvent& event) override;
  const std::vector<AgentEvent>& events() const noexcept;

 private:
  std::vector<AgentEvent> events_;
};

}  // namespace qt::agent
