#include "qt/agent/agent.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "qt/ai/provider_transport.hpp"

namespace qt::agent {

namespace {

constexpr const char* kAbortedError = "agent turn aborted";

tools::ToolRequest make_tool_request(const ai::ToolCallContent& call) {
  tools::ToolRequest request;
  request.tool_name = call.name;
  request.arguments = call.arguments;
  if (!call.id.empty()) {
    request.call_id = call.id;
  }
  return request;
}

tools::ToolResult make_invalid_tool_result(const tools::ToolRequest& request,
                                           tools::ToolResultStatus status,
                                           std::string error_message) {
  tools::ToolResult result;
  result.status = status;
  result.call_id = request.call_id;
  result.error_message = std::move(error_message);
  return result;
}

tools::ToolResult make_aborted_tool_result(const tools::ToolRequest& request) {
  tools::ToolResult result = make_invalid_tool_result(
      request, tools::ToolResultStatus::execution_error, "Operation aborted");
  result.output = ai::Json{{"aborted", true}};
  return result;
}

tools::ToolResult make_interrupted_tool_result(
    const tools::ToolRequest& request, std::string reason) {
  tools::ToolResult result = make_invalid_tool_result(
      request, tools::ToolResultStatus::execution_error, std::move(reason));
  result.output = ai::Json{{"interrupted", true}};
  return result;
}

ai::Message make_tool_result_message(const tools::ToolRequest& request,
                                     const tools::ToolResult& result) {
  ai::ToolResultContent tool_result;
  tool_result.tool_call_id = request.call_id.value_or("");
  tool_result.name = request.tool_name;
  tool_result.is_error = !tools::is_success(result);
  tool_result.result = ai::Json{
      {"status", tools::to_string(result.status)},
      {"output", result.output},
  };
  if (result.error_message) {
    tool_result.result["error"] = *result.error_message;
  }

  ai::Message message;
  message.role = ai::MessageRole::tool;
  message.name = request.tool_name;
  message.content.push_back(std::move(tool_result));
  return message;
}

bool has_tool_calls(const ai::Message& message) {
  for (const auto& content : message.content) {
    if (ai::content_kind(content) == ai::ContentKind::tool_call) {
      return true;
    }
  }
  return false;
}

bool is_error_stop_reason(ai::StopReason reason) {
  return reason == ai::StopReason::error || reason == ai::StopReason::aborted;
}

std::string make_stop_reason_error(ai::StopReason reason) {
  return std::string{"provider turn ended with stop reason "} +
         ai::to_string(reason);
}

std::vector<tools::ToolRequest> plan_tool_requests(const ai::Message& message) {
  std::vector<tools::ToolRequest> requests;
  for (const auto& content : message.content) {
    if (ai::content_kind(content) != ai::ContentKind::tool_call) {
      continue;
    }

    requests.push_back(make_tool_request(std::get<ai::ToolCallContent>(content)));
  }
  return requests;
}

bool tool_is_active(const tools::ToolRequest& request,
                    const std::vector<ai::ToolDefinition>& active_tools) {
  if (active_tools.empty()) {
    return true;
  }
  return std::any_of(active_tools.begin(), active_tools.end(),
                     [&](const ai::ToolDefinition& tool) {
                       return tool.name == request.tool_name;
      });
}

const ai::ToolDefinition* active_tool_definition(
    const tools::ToolRequest& request,
    const std::vector<ai::ToolDefinition>& active_tools) {
  const auto match = std::find_if(
      active_tools.begin(), active_tools.end(),
      [&](const ai::ToolDefinition& tool) {
        return tool.name == request.tool_name;
      });
  return match == active_tools.end() ? nullptr : &*match;
}

bool tool_batch_requires_sequential_execution(
    const std::vector<tools::ToolRequest>& requests,
    const std::shared_ptr<tools::ToolRegistry>& tool_registry) {
  if (!tool_registry) {
    return true;
  }
  return std::any_of(requests.begin(), requests.end(),
                     [&](const tools::ToolRequest& request) {
                       const auto prepared = tool_registry->prepare_request(request);
                       const auto* tool = tool_registry->find(prepared.tool_name);
                       return tool == nullptr || tool->descriptor().destructive;
                     });
}

tools::ToolResult make_inactive_tool_result(
    const tools::ToolRequest& request) {
  return make_invalid_tool_result(
      request, tools::ToolResultStatus::not_found,
      "requested tool is not active for this turn");
}

struct ToolPreflightResult {
  tools::ToolRequest request;
  std::optional<tools::ToolResult> result;
};

tools::ToolRequest apply_transform_tool_hooks(
    const std::vector<TransformToolHook>& hooks,
    const ToolHookContext& hook_context,
    const tools::ToolRequest& request) {
  tools::ToolRequest current = request;
  for (const auto& hook : hooks) {
    auto replacement = hook(hook_context, current);
    if (!replacement) {
      continue;
    }
    if (replacement->tool_name.empty()) {
      replacement->tool_name = current.tool_name;
    }
    if (!replacement->call_id.has_value()) {
      replacement->call_id = current.call_id;
    }
    current = std::move(*replacement);
  }
  return current;
}

std::optional<tools::ToolResult> apply_before_tool_hooks(
    const std::vector<BeforeToolHook>& hooks,
    const ToolHookContext& hook_context,
    const tools::ToolRequest& request) {
  for (const auto& hook : hooks) {
    auto blocked = hook(hook_context, request);
    if (!blocked) {
      continue;
    }
    blocked->call_id = request.call_id;
    if (blocked->status == tools::ToolResultStatus::success) {
      blocked->status = tools::ToolResultStatus::blocked_by_extension;
    }
    if (blocked->output.is_null()) {
      blocked->output = ai::Json::object();
    }
    if (!blocked->output.is_object()) {
      blocked->output = ai::Json{{"value", blocked->output}};
    }
    blocked->output["blocked"] = true;
    blocked->output["blocked_by"] = "extension";
    return blocked;
  }
  return std::nullopt;
}

ToolPreflightResult preflight_tool_request(
    const std::vector<ai::ToolDefinition>& active_tools,
    const std::shared_ptr<tools::ToolRegistry>& tool_registry,
    const std::vector<TransformToolHook>& transform_tool_hooks,
    const std::vector<BeforeToolHook>& before_tool_hooks,
    const ToolHookContext& hook_context,
    const tools::ToolRequest& request) {
  ToolPreflightResult preflight;
  preflight.request = request;
  if (!tool_is_active(request, active_tools)) {
    preflight.result = make_inactive_tool_result(request);
    return preflight;
  }

  tools::ToolRequest normalized_request = request;
  if (tool_registry) {
    normalized_request = tool_registry->prepare_request(normalized_request);
    // Registry preparation may resolve a public alias to its implementation
    // name. Keep the provider-facing identity stable for hooks, events, and
    // the paired tool-result message; invoke_tool_request resolves it again.
    normalized_request.tool_name = request.tool_name;
  }
  preflight.request = apply_transform_tool_hooks(
      transform_tool_hooks, hook_context, normalized_request);
  if (!tool_is_active(preflight.request, active_tools)) {
    preflight.result = make_inactive_tool_result(preflight.request);
    return preflight;
  }
  if (const auto* definition =
          active_tool_definition(preflight.request, active_tools)) {
    ai::ToolCallContent call;
    call.id = request.call_id.value_or("");
    call.name = preflight.request.tool_name;
    call.arguments = preflight.request.arguments;
    const auto validated = ai::validate_tool_arguments(*definition, call);
    if (!validated) {
      preflight.result = make_invalid_tool_result(
          preflight.request, tools::ToolResultStatus::invalid_request,
          validated.error().message);
      preflight.result->output =
          ai::Json{{"code", "invalid_arguments"},
                   {"tool", preflight.request.tool_name},
                   {"retryable", true},
                   {"received_arguments", preflight.request.arguments},
                   {"expected_schema", definition->input_schema}};
    } else {
      preflight.request.arguments = validated.value();
    }
  }
  if (!preflight.result) {
    preflight.result = apply_before_tool_hooks(before_tool_hooks, hook_context,
                                               preflight.request);
  }
  return preflight;
}

tools::ToolResult invoke_tool_request(
    const tools::ToolRequest& request,
    const std::shared_ptr<tools::ToolRegistry>& tool_registry,
    const tools::ToolContext& tool_context) {
  if (!tools::is_valid(request)) {
    return make_invalid_tool_result(
        request, tools::ToolResultStatus::invalid_request,
        "provider returned an invalid tool call request");
  }

  if (!tool_registry) {
    return make_invalid_tool_result(request, tools::ToolResultStatus::not_found,
                                    "agent tool registry is not configured");
  }

  const auto tool = tool_registry->find(request.tool_name);
  if (!tool) {
    return make_invalid_tool_result(request, tools::ToolResultStatus::not_found,
                                    "requested tool is not registered");
  }

  const auto prepared_request = tool_registry->prepare_request(request);
  tools::ToolResult tool_result = tool->invoke(prepared_request, tool_context);
  tool_result.call_id = request.call_id;
  return tool_result;
}

std::vector<ai::StreamEvent> build_provider_events(
    const ai::CompletionResponse& response) {
  std::vector<ai::StreamEvent> events;

  ai::StreamEvent start;
  start.kind = ai::EventKind::message_start;
  start.role = ai::MessageRole::assistant;
  if (response.message.id) {
    start.message_id = response.message.id;
  }
  events.push_back(start);

  for (const auto& content : response.message.content) {
    ai::StreamEvent content_event;
    const auto kind = ai::content_kind(content);
    content_event.kind = kind == ai::ContentKind::tool_call
                             ? ai::EventKind::tool_call
                             : kind == ai::ContentKind::tool_result
                                   ? ai::EventKind::tool_result
                                   : ai::EventKind::content_done;
    content_event.content = content;
    if (response.message.id) {
      content_event.message_id = response.message.id;
    }
    events.push_back(content_event);
  }

  ai::StreamEvent usage;
  usage.kind = ai::EventKind::usage;
  usage.usage = response.usage;
  if (response.message.id) {
    usage.message_id = response.message.id;
  }
  events.push_back(usage);

  ai::StreamEvent done;
  done.kind = ai::EventKind::message_done;
  done.stop_reason = response.stop_reason;
  if (response.message.id) {
    done.message_id = response.message.id;
  }
  events.push_back(done);

  return events;
}

}  // namespace

namespace {

void prepare_next_turn(AgentConfig& config) {
  if (config.prepare_next_turn_hook) {
    config.prepare_next_turn_hook();
  }
}

}  // namespace

Agent::Agent(AgentConfig config, std::shared_ptr<ai::Provider> provider,
             ai::ProviderOptions options)
    : config_(std::move(config)),
      provider_(std::move(provider)),
      options_(std::move(options)) {
  if (!provider_) {
    throw std::invalid_argument("agent provider is required");
  }
  options_.abort_signal = provider_abort_signal_;
}

Agent::Agent(Agent&& other) noexcept
    : config_(std::move(other.config_)),
      provider_(std::move(other.provider_)),
      options_(std::move(other.options_)),
      state_(std::move(other.state_)),
      steering_queue_(std::move(other.steering_queue_)),
      follow_up_queue_(std::move(other.follow_up_queue_)),
      active_pending_count_(other.active_pending_count_),
      abort_requested_(
          other.abort_requested_.load(std::memory_order_acquire)),
      provider_abort_signal_(std::move(other.provider_abort_signal_)),
      turn_running_(other.turn_running_) {
  if (!provider_abort_signal_) {
    provider_abort_signal_ = std::make_shared<std::atomic_bool>(false);
  }
  options_.abort_signal = provider_abort_signal_;
  other.active_pending_count_ = 0;
  other.abort_requested_.store(false, std::memory_order_release);
  other.provider_abort_signal_ = std::make_shared<std::atomic_bool>(false);
  other.options_.abort_signal = other.provider_abort_signal_;
  other.turn_running_ = false;
}

Agent& Agent::operator=(Agent&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  config_ = std::move(other.config_);
  provider_ = std::move(other.provider_);
  options_ = std::move(other.options_);
  state_ = std::move(other.state_);
  steering_queue_ = std::move(other.steering_queue_);
  follow_up_queue_ = std::move(other.follow_up_queue_);
  active_pending_count_ = other.active_pending_count_;
  abort_requested_.store(
      other.abort_requested_.load(std::memory_order_acquire),
      std::memory_order_release);
  provider_abort_signal_ = std::move(other.provider_abort_signal_);
  if (!provider_abort_signal_) {
    provider_abort_signal_ = std::make_shared<std::atomic_bool>(false);
  }
  options_.abort_signal = provider_abort_signal_;
  turn_running_ = other.turn_running_;

  other.active_pending_count_ = 0;
  other.abort_requested_.store(false, std::memory_order_release);
  other.provider_abort_signal_ = std::make_shared<std::atomic_bool>(false);
  other.options_.abort_signal = other.provider_abort_signal_;
  other.turn_running_ = false;
  return *this;
}

QueuedMessage Agent::enqueue(ai::Message message, EventSink* sink) {
  QueuedMessage queued;
  queued.sequence = state_.next_message_sequence++;
  queued.message = std::move(message);

  state_.pending.push_back(queued);

  AgentEvent event = make_event(AgentEventKind::message_queued,
                                state_.completed_turns + 1);
  event.queued_message = queued;
  emit(sink, std::move(event));
  return queued;
}

QueuedMessage Agent::steer(ai::Message message, EventSink* sink) {
  QueuedMessage queued;
  queued.sequence = state_.next_message_sequence++;
  queued.message = std::move(message);

  steering_queue_.push_back(queued);

  AgentEvent event = make_event(AgentEventKind::message_queued,
                                state_.completed_turns + 1);
  event.queued_message = queued;
  emit(sink, std::move(event));
  return queued;
}

QueuedMessage Agent::follow_up(ai::Message message, EventSink* sink) {
  QueuedMessage queued;
  queued.sequence = state_.next_message_sequence++;
  queued.message = std::move(message);

  follow_up_queue_.push_back(queued);

  AgentEvent event = make_event(AgentEventKind::message_queued,
                                state_.completed_turns + 1);
  event.queued_message = queued;
  emit(sink, std::move(event));
  return queued;
}

void Agent::clear_steering_queue() noexcept { steering_queue_.clear(); }

void Agent::clear_follow_up_queue() noexcept { follow_up_queue_.clear(); }

void Agent::clear_all_queues() noexcept {
  clear_steering_queue();
  clear_follow_up_queue();
}

void Agent::reset() noexcept {
  state_ = AgentState{};
  steering_queue_.clear();
  follow_up_queue_.clear();
  active_pending_count_ = 0;
  abort_requested_.store(false, std::memory_order_release);
  provider_abort_signal_->store(false, std::memory_order_release);
}

bool Agent::has_queued_messages() const noexcept {
  return !steering_queue_.empty() || !follow_up_queue_.empty();
}

void Agent::request_abort() noexcept {
  abort_requested_.store(true, std::memory_order_release);
  provider_abort_signal_->store(true, std::memory_order_release);
}

void Agent::set_provider_options(ai::ProviderOptions options) {
  options.abort_signal = provider_abort_signal_;
  options_ = std::move(options);
}

TurnResult Agent::run_turn(EventSink& sink) {
  TurnResult result;
  result.turn_index = state_.completed_turns + 1;

  AgentEvent started = make_event(AgentEventKind::turn_started, result.turn_index);
  emit(&sink, std::move(started));

  if (turn_running_) {
    result.error = "agent turn is already running";

    AgentEvent failed = make_event(AgentEventKind::turn_failed, result.turn_index);
    failed.error = result.error;
    emit(&sink, std::move(failed));
    return result;
  }

  turn_running_ = true;
  active_pending_count_ = state_.pending.size();
  struct TurnReset final {
    Agent* agent;
    ~TurnReset() {
      agent->turn_running_ = false;
      agent->active_pending_count_ = 0;
      agent->abort_requested_.store(false, std::memory_order_release);
      agent->provider_abort_signal_->store(false, std::memory_order_release);
    }
  } turn_reset{this};

  if (active_pending_count_ == 0U && !has_queued_messages()) {
    result.error = "agent turn requires at least one queued message";

    AgentEvent failed = make_event(AgentEventKind::turn_failed, result.turn_index);
    failed.error = result.error;
    emit(&sink, std::move(failed));
    return result;
  }

  try {
    if (fail_if_abort_requested(result.turn_index, result, sink)) {
      return result;
    }

    if (active_pending_count_ == 0U) {
      const auto starter_messages =
          drain_runtime_queue(steering_queue_, config_.steering_mode,
                              result.turn_index, result, sink);
      if (starter_messages == 0U) {
        drain_runtime_queue(follow_up_queue_, config_.follow_up_mode,
                            result.turn_index, result, sink);
      }

      if (fail_if_abort_requested(result.turn_index, result, sink)) {
        return result;
      }
    }

    ai::CompletionResponse response =
        perform_completion(result.turn_index, result, sink);

    if (fail_if_abort_requested(result.turn_index, result, sink)) {
      return result;
    }

    commit_pending_messages(result.turn_index, result, sink);

    std::uint32_t auto_retry_attempt = 0;
    while (true) {
      response.message.metadata["usage"] = response.usage;
      response.message.metadata["stopReason"] = response.stop_reason;
      commit_message(response.message, result.turn_index, sink);

      if (fail_if_abort_requested(result.turn_index, result, sink)) {
        return result;
      }

      if (is_error_stop_reason(response.stop_reason)) {
        const auto error_message =
            response.metadata.value("errorMessage", std::string());
        if (can_auto_retry(response, auto_retry_attempt)) {
          ++auto_retry_attempt;
          const auto delay_ms = auto_retry_delay_ms(auto_retry_attempt);
          emit_auto_retry_start(result.turn_index, auto_retry_attempt, delay_ms,
                                error_message.empty() ? "Unknown error"
                                                      : error_message,
                                sink);
          remove_last_assistant_message();
          response = perform_completion(result.turn_index, result, sink);
          continue;
        }

        if (auto_retry_attempt > 0U) {
          emit_auto_retry_end(result.turn_index, auto_retry_attempt, false,
                              error_message.empty()
                                  ? std::optional<std::string>{"Unknown error"}
                                  : std::optional<std::string>{error_message},
                              sink);
        }

        result.error = make_stop_reason_error(response.stop_reason);

        AgentEvent failed =
            make_event(AgentEventKind::turn_failed, result.turn_index);
        failed.error = result.error;
        emit(&sink, std::move(failed));
        return result;
      }

      const bool response_has_tool_calls = has_tool_calls(response.message);
      if (auto_retry_attempt > 0U) {
        emit_auto_retry_end(result.turn_index, auto_retry_attempt, true,
                            std::nullopt, sink);
        auto_retry_attempt = 0;
      }

      if (response_has_tool_calls) {
        execute_tool_calls(response.message, result.turn_index, result, sink);

        if (fail_if_abort_requested(result.turn_index, result, sink)) {
          return result;
        }
      }

      if (config_.take_subagent_events) {
        for (auto& notification : config_.take_subagent_events()) {
          auto event = make_event(AgentEventKind::subagent_event, result.turn_index);
          event.subagent = notification;
          emit(&sink, std::move(event));
          ai::Message message;
          message.role = ai::MessageRole::user;
          message.content = {ai::TextContent{notification.dump(), std::nullopt}};
          message.metadata = ai::Json{{"role", "custom"},
              {"customType", "subagent_event"}, {"display", true},
              {"details", notification}};
          follow_up(std::move(message), &sink);
        }
      }
      const auto steered = drain_runtime_queue(
          steering_queue_, config_.steering_mode, result.turn_index, result, sink);
      if (steered != 0U) {
        if (fail_if_abort_requested(result.turn_index, result, sink)) {
          return result;
        }

        prepare_next_turn(config_);
        response = perform_completion(result.turn_index, result, sink);

        if (fail_if_abort_requested(result.turn_index, result, sink)) {
          return result;
        }
        continue;
      }

      if (response_has_tool_calls) {
        prepare_next_turn(config_);
        response = perform_completion(result.turn_index, result, sink);

        if (fail_if_abort_requested(result.turn_index, result, sink)) {
          return result;
        }
        continue;
      }

      const auto followed_up =
          drain_runtime_queue(follow_up_queue_, config_.follow_up_mode,
                              result.turn_index, result, sink);
      if (followed_up == 0U) {
        break;
      }

      if (fail_if_abort_requested(result.turn_index, result, sink)) {
        return result;
      }

      prepare_next_turn(config_);
      response = perform_completion(result.turn_index, result, sink);

      if (fail_if_abort_requested(result.turn_index, result, sink)) {
        return result;
      }
    }
  } catch (const std::exception& error) {
    if (abort_requested() &&
        fail_if_abort_requested(result.turn_index, result, sink)) {
      return result;
    }

    result.error = error.what();

    AgentEvent failed = make_event(AgentEventKind::turn_failed, result.turn_index);
    failed.error = result.error;
    emit(&sink, std::move(failed));
    return result;
  }

  state_.completed_turns = result.turn_index;
  result.ok = true;

  AgentEvent completed =
      make_event(AgentEventKind::turn_completed, result.turn_index);
  completed.response = result.response;
  emit(&sink, std::move(completed));

  return result;
}

TurnResult Agent::run_tool_call(
    tools::ToolRequest request,
    std::shared_ptr<qt::foundation::ProcessCancellationToken>
        cancellation_token,
    EventSink& sink) {
  TurnResult result;
  result.turn_index = state_.completed_turns + 1;

  AgentEvent started = make_event(AgentEventKind::turn_started, result.turn_index);
  emit(&sink, std::move(started));

  if (turn_running_) {
    result.error = "agent turn is already running";
    AgentEvent failed = make_event(AgentEventKind::turn_failed, result.turn_index);
    failed.error = result.error;
    emit(&sink, std::move(failed));
    return result;
  }
  if (request.tool_name.empty()) {
    result.error = "direct tool call requires a tool name";
    AgentEvent failed = make_event(AgentEventKind::turn_failed, result.turn_index);
    failed.error = result.error;
    emit(&sink, std::move(failed));
    return result;
  }

  turn_running_ = true;
  active_pending_count_ = state_.pending.size();
  auto previous_cancellation_token = config_.tool_context.cancellation_token;
  config_.tool_context.cancellation_token = std::move(cancellation_token);
  struct TurnReset final {
    Agent* agent;
    std::shared_ptr<qt::foundation::ProcessCancellationToken>
        previous_cancellation_token;
    ~TurnReset() {
      agent->config_.tool_context.cancellation_token =
          std::move(previous_cancellation_token);
      agent->turn_running_ = false;
      agent->active_pending_count_ = 0;
      agent->abort_requested_.store(false, std::memory_order_release);
      agent->provider_abort_signal_->store(false, std::memory_order_release);
    }
  } turn_reset{this, std::move(previous_cancellation_token)};

  if (active_pending_count_ == 0U) {
    result.error = "direct tool call requires a queued user message";
    AgentEvent failed = make_event(AgentEventKind::turn_failed, result.turn_index);
    failed.error = result.error;
    emit(&sink, std::move(failed));
    return result;
  }

  try {
    if (fail_if_abort_requested(result.turn_index, result, sink)) {
      return result;
    }

    commit_pending_messages(result.turn_index, result, sink);

    ai::Message tool_call_message;
    tool_call_message.role = ai::MessageRole::assistant;
    tool_call_message.metadata =
        ai::Json{{"origin", "user_initiated_tool_call"}};
    tool_call_message.content.push_back(ai::ToolCallContent{
        request.call_id.value_or(""), request.tool_name, request.arguments});
    commit_message(std::move(tool_call_message), result.turn_index, sink);

    execute_tool_calls(state_.transcript.back(), result.turn_index, result, sink);
    if (fail_if_abort_requested(result.turn_index, result, sink)) {
      return result;
    }
  } catch (const std::exception& error) {
    if (abort_requested() &&
        fail_if_abort_requested(result.turn_index, result, sink)) {
      return result;
    }
    result.error = error.what();
    AgentEvent failed = make_event(AgentEventKind::turn_failed, result.turn_index);
    failed.error = result.error;
    emit(&sink, std::move(failed));
    return result;
  }

  state_.completed_turns = result.turn_index;
  result.ok = true;
  AgentEvent completed =
      make_event(AgentEventKind::turn_completed, result.turn_index);
  emit(&sink, std::move(completed));
  return result;
}

AgentEvent Agent::make_event(AgentEventKind kind, std::uint64_t turn_index) const {
  AgentEvent event;
  event.sequence = state_.next_event_sequence;
  event.kind = kind;
  event.turn_index = turn_index;
  return event;
}

ai::CompletionRequest Agent::build_request() const {
  ai::CompletionRequest request;
  request.model = config_.model;
  request.tools = config_.tools;
  request.tool_choice = config_.tool_choice;
  request.temperature = config_.temperature;
  request.max_output_tokens = config_.max_output_tokens;
  request.metadata = config_.metadata;

  request.messages.reserve(state_.transcript.size() + active_pending_count_);
  request.messages.insert(request.messages.end(), state_.transcript.begin(),
                          state_.transcript.end());

  const auto pending_count =
      std::min(active_pending_count_, state_.pending.size());
  for (std::size_t index = 0; index < pending_count; ++index) {
    request.messages.push_back(state_.pending[index].message);
  }

  return request;
}

ai::CompletionResponse Agent::perform_completion(std::uint64_t turn_index,
                                                 TurnResult& result,
                                                 EventSink& sink) {
  ai::CompletionRequest request = build_request();
  if (!result.request) {
    result.request = request;
  }
  result.requests.push_back(request);

  AgentEvent request_event = make_event(AgentEventKind::provider_request, turn_index);
  request_event.request = request;
  emit(&sink, std::move(request_event));

  ai::CompletionResponse response;
  if (provider_->descriptor().capabilities.streaming) {
    class StreamRecorder final : public ai::EventSink {
     public:
      StreamRecorder(Agent& agent, std::uint64_t turn_index, TurnResult& result,
                     qt::agent::EventSink& sink)
          : agent_(agent),
            turn_index_(turn_index),
            result_(result),
            sink_(sink) {}

      void push(const ai::StreamEvent& event) override {
        events_.push_back(event);
        agent_.record_provider_event(events_.back(), turn_index_, result_, sink_);
      }

      const std::vector<ai::StreamEvent>& events() const noexcept {
        return events_;
      }

     private:
      Agent& agent_;
      std::uint64_t turn_index_;
      TurnResult& result_;
      qt::agent::EventSink& sink_;
      std::vector<ai::StreamEvent> events_;
    } recorder{*this, turn_index, result, sink};

    provider_->stream(request, options_, recorder);
    response = ai::completion_response_from_events(recorder.events());
  } else {
    response = provider_->complete(request, options_);
    for (const auto& provider_event : build_provider_events(response)) {
      record_provider_event(provider_event, turn_index, result, sink);
    }
  }

  if (response.stop_reason == ai::StopReason::unknown) {
    response.stop_reason = ai::StopReason::error;
    response.metadata["errorMessage"] = "Provider response ended without a completion signal";
  }
  result.responses.push_back(response);
  result.response = response;
  return response;
}

void Agent::record_provider_event(const ai::StreamEvent& provider_event,
                                  std::uint64_t turn_index, TurnResult& result,
                                  EventSink& sink) {
  result.provider_events.push_back(provider_event);

  AgentEvent event = make_event(AgentEventKind::provider_event, turn_index);
  event.provider_event = provider_event;
  emit(&sink, std::move(event));
}

bool Agent::fail_if_abort_requested(std::uint64_t turn_index, TurnResult& result,
                                    EventSink& sink) {
  if (!abort_requested()) {
    return false;
  }

  cancel_active_pending_messages();
  clear_runtime_queues();
  result.error = kAbortedError;

  AgentEvent failed = make_event(AgentEventKind::turn_failed, turn_index);
  failed.error = result.error;
  emit(&sink, std::move(failed));
  return true;
}

void Agent::cancel_active_pending_messages() {
  const auto pending_count =
      std::min(active_pending_count_, state_.pending.size());
  if (pending_count == 0U) {
    active_pending_count_ = 0;
    return;
  }

  state_.pending.erase(state_.pending.begin(),
                       state_.pending.begin() +
                           static_cast<std::ptrdiff_t>(pending_count));
  active_pending_count_ = 0;
}

void Agent::clear_runtime_queues() noexcept {
  steering_queue_.clear();
  follow_up_queue_.clear();
}

void Agent::commit_pending_messages(std::uint64_t turn_index, TurnResult& result,
                                    EventSink& sink) {
  const auto pending_count =
      std::min(active_pending_count_, state_.pending.size());
  for (std::size_t index = 0; index < pending_count; ++index) {
    auto& queued = state_.pending[index];
    queued.disposition = QueueDisposition::committed;
    result.committed_sequences.push_back(queued.sequence);
    commit_message(queued.message, turn_index, sink, queued);
  }

  if (pending_count != 0U) {
    state_.pending.erase(state_.pending.begin(),
                         state_.pending.begin() +
                             static_cast<std::ptrdiff_t>(pending_count));
  }
  active_pending_count_ = 0;
}

std::size_t Agent::drain_runtime_queue(std::vector<QueuedMessage>& queue,
                                       QueueMode mode,
                                       std::uint64_t turn_index,
                                       TurnResult& result, EventSink& sink) {
  const auto drain_count =
      mode == QueueMode::all ? queue.size() : std::min<std::size_t>(queue.size(), 1U);
  for (std::size_t index = 0; index < drain_count; ++index) {
    auto& queued = queue[index];
    queued.disposition = QueueDisposition::committed;
    result.committed_sequences.push_back(queued.sequence);
    commit_message(queued.message, turn_index, sink, queued);
  }

  if (drain_count != 0U) {
    queue.erase(queue.begin(),
                queue.begin() + static_cast<std::ptrdiff_t>(drain_count));
  }
  return drain_count;
}

void Agent::commit_message(ai::Message message, std::uint64_t turn_index,
                           EventSink& sink,
                           std::optional<QueuedMessage> queued_message) {
  state_.transcript.push_back(message);

  AgentEvent committed = make_event(AgentEventKind::message_committed, turn_index);
  committed.queued_message = std::move(queued_message);
  committed.committed_message = std::move(message);
  emit(&sink, std::move(committed));
}

void Agent::remove_last_assistant_message() noexcept {
  if (!state_.transcript.empty() &&
      state_.transcript.back().role == ai::MessageRole::assistant) {
    state_.transcript.pop_back();
  }
}

std::int64_t Agent::auto_retry_delay_ms(std::uint32_t attempt) const noexcept {
  if (attempt == 0U || config_.auto_retry_base_delay_ms <= 0) {
    return 0;
  }

  std::int64_t delay = config_.auto_retry_base_delay_ms;
  for (std::uint32_t step = 1; step < attempt; ++step) {
    if (delay > std::numeric_limits<std::int64_t>::max() / 2) {
      return std::numeric_limits<std::int64_t>::max();
    }
    delay *= 2;
  }
  return delay;
}

bool Agent::can_auto_retry(const ai::CompletionResponse& response,
                           std::uint32_t attempt) const {
  return config_.auto_retry_enabled &&
         attempt < config_.auto_retry_max_attempts &&
         is_retryable_agent_error(response);
}

void Agent::emit_auto_retry_start(std::uint64_t turn_index, std::uint32_t attempt,
                                  std::int64_t delay_ms,
                                  const std::string& error, EventSink& sink) {
  AgentEvent event = make_event(AgentEventKind::auto_retry_started, turn_index);
  event.retry_attempt = attempt;
  event.retry_max_attempts = config_.auto_retry_max_attempts;
  event.retry_delay_ms = delay_ms;
  event.error = error;
  emit(&sink, std::move(event));
}

void Agent::emit_auto_retry_end(std::uint64_t turn_index, std::uint32_t attempt,
                                bool success,
                                std::optional<std::string> final_error,
                                EventSink& sink) {
  AgentEvent event = make_event(AgentEventKind::auto_retry_completed, turn_index);
  event.retry_attempt = attempt;
  event.retry_success = success;
  event.error = std::move(final_error);
  emit(&sink, std::move(event));
}

std::size_t Agent::execute_tool_calls(const ai::Message& message,
                                      std::uint64_t turn_index,
                                      TurnResult& result, EventSink& sink) {
  if (config_.publish_subagent_context) {
    config_.publish_subagent_context(state_.transcript);
  }

  const auto planned_requests = plan_tool_requests(message);
  if (planned_requests.empty()) {
    return 0;
  }

  if (config_.tool_execution_mode != ToolExecutionMode::parallel ||
      planned_requests.size() == 1U ||
      tool_batch_requires_sequential_execution(planned_requests,
                                               config_.tool_registry)) {
    std::size_t executed = 0;
    const auto commit_synthetic_result =
        [&](const tools::ToolRequest& request, tools::ToolResult tool_result) {
          AgentEvent tool_start =
              make_event(AgentEventKind::tool_execution_started, turn_index);
          tool_start.tool_request = request;
          emit(&sink, std::move(tool_start));

          result.tool_requests.push_back(request);
          result.tool_results.push_back(tool_result);
          ++state_.completed_tool_calls;

          AgentEvent tool_event =
              make_event(AgentEventKind::tool_executed, turn_index);
          tool_event.tool_request = request;
          tool_event.tool_result = tool_result;
          emit(&sink, std::move(tool_event));

          ai::Message tool_message =
              make_tool_result_message(request, tool_result);
          commit_message(std::move(tool_message), turn_index, sink);
          ++executed;
        };

    for (std::size_t index = 0; index < planned_requests.size(); ++index) {
      const auto& planned_request = planned_requests[index];
      if (abort_requested()) {
        for (std::size_t skipped = index; skipped < planned_requests.size();
             ++skipped) {
          commit_synthetic_result(planned_requests[skipped],
                                  make_aborted_tool_result(
                                      planned_requests[skipped]));
        }
        break;
      }

      ToolHookContext hook_context;
      hook_context.turn_index = turn_index;
      hook_context.tool_call_index = executed;
      hook_context.completed_tool_calls_before = state_.completed_tool_calls;

      const auto preflight = preflight_tool_request(
          config_.tools, config_.tool_registry, config_.transform_tool_hooks,
          config_.before_tool_hooks, hook_context, planned_request);
      const auto& request = preflight.request;

      AgentEvent tool_start =
          make_event(AgentEventKind::tool_execution_started, turn_index);
      tool_start.tool_request = request;
      emit(&sink, std::move(tool_start));

      auto tool_context = config_.tool_context;
      tool_context.metadata["model"] = ai::Json{{"provider", config_.model.provider}, {"id", config_.model.name}};
      tool_context.metadata["active_tools"] = ai::Json::array();
      for (const auto& tool : config_.tools) tool_context.metadata["active_tools"].push_back(tool.name);
      tool_context.update_callback = [&](const tools::ToolResult& partial) {
        if (abort_requested()) {
          return;
        }
        AgentEvent update =
            make_event(AgentEventKind::tool_execution_updated, turn_index);
        update.tool_request = request;
        update.tool_result = partial;
        emit(&sink, std::move(update));
      };

      tools::ToolResult tool_result =
          preflight.result ? *preflight.result
                           : invoke_tool_request(request, config_.tool_registry,
                                                 tool_context);

      for (const auto& transform : config_.tool_result_transforms) {
        if (auto replacement = transform(hook_context, request, tool_result)) {
          replacement->call_id = tool_result.call_id;
          tool_result = std::move(*replacement);
        }
      }
      for (const auto& hook : config_.after_tool_hooks) {
        hook(hook_context, request, tool_result);
      }

      result.tool_requests.push_back(request);
      result.tool_results.push_back(tool_result);
      ++state_.completed_tool_calls;

      AgentEvent tool_event = make_event(AgentEventKind::tool_executed, turn_index);
      tool_event.tool_request = request;
      tool_event.tool_result = tool_result;
      emit(&sink, std::move(tool_event));

      ai::Message tool_message = make_tool_result_message(request, tool_result);
      commit_message(std::move(tool_message), turn_index, sink);
      ++executed;

      if (abort_requested() || !steering_queue_.empty()) {
        const std::string reason =
            abort_requested()
                ? "Operation aborted"
                : "Tool execution interrupted by queued steering message";
        for (std::size_t skipped = index + 1U; skipped < planned_requests.size();
             ++skipped) {
          commit_synthetic_result(
              planned_requests[skipped],
              abort_requested()
                  ? make_aborted_tool_result(planned_requests[skipped])
                  : make_interrupted_tool_result(planned_requests[skipped],
                                                 reason));
        }
        break;
      }
    }

    return executed;
  }

  const std::uint64_t completed_before_batch = state_.completed_tool_calls;
  std::vector<ToolHookContext> hook_contexts;
  hook_contexts.reserve(planned_requests.size());
  std::vector<ToolPreflightResult> preflight_results;
  preflight_results.reserve(planned_requests.size());
  std::vector<tools::ToolRequest> effective_requests;
  effective_requests.reserve(planned_requests.size());

  for (std::size_t index = 0; index < planned_requests.size(); ++index) {
    ToolHookContext hook_context;
    hook_context.turn_index = turn_index;
    hook_context.tool_call_index = index;
    hook_context.completed_tool_calls_before =
        completed_before_batch + static_cast<std::uint64_t>(index);
    hook_contexts.push_back(hook_context);

    preflight_results.push_back(preflight_tool_request(
        config_.tools, config_.tool_registry, config_.transform_tool_hooks,
        config_.before_tool_hooks, hook_contexts.back(),
        planned_requests[index]));
    effective_requests.push_back(preflight_results.back().request);

    AgentEvent tool_start =
        make_event(AgentEventKind::tool_execution_started, turn_index);
    tool_start.tool_request = effective_requests.back();
    emit(&sink, std::move(tool_start));
  }

  std::vector<tools::ToolResult> tool_results;
  tool_results.reserve(planned_requests.size());

  // Invoke concurrently, then replay hooks/events/messages in provider plan order.
  struct CompletionQueue {
    struct Entry {
      enum class Kind {
        update,
        complete,
      };

      Kind kind = Kind::complete;
      std::size_t index = 0;
      std::optional<tools::ToolResult> result;
    };

    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Entry> entries;
  };
  auto completion_queue = std::make_shared<CompletionQueue>();

  std::vector<std::future<tools::ToolResult>> futures;
  futures.reserve(planned_requests.size());
  const auto tool_registry = config_.tool_registry;
  for (std::size_t index = 0; index < planned_requests.size(); ++index) {
    const auto request = effective_requests[index];
    auto tool_context = config_.tool_context;
    tool_context.metadata["model"] = ai::Json{{"provider", config_.model.provider}, {"id", config_.model.name}};
    tool_context.metadata["active_tools"] = ai::Json::array();
    for (const auto& tool : config_.tools) tool_context.metadata["active_tools"].push_back(tool.name);
    tool_context.update_callback = [this,
                                    completion_queue,
                                    index](const tools::ToolResult& partial) {
      if (abort_requested()) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(completion_queue->mutex);
        completion_queue->entries.push_back(
            CompletionQueue::Entry{CompletionQueue::Entry::Kind::update,
                                   index,
                                   partial});
      }
      completion_queue->ready.notify_one();
    };
    const auto preflight_result = preflight_results[index].result;
    futures.push_back(std::async(
        std::launch::async,
        [this,
         request,
         tool_registry,
         tool_context,
         completion_queue,
         index,
         preflight_result]() {
          tools::ToolResult result;
          if (abort_requested()) {
            result = make_aborted_tool_result(request);
          } else if (preflight_result) {
            result = *preflight_result;
          } else {
            result = invoke_tool_request(request, tool_registry, tool_context);
          }
          {
            std::lock_guard<std::mutex> lock(completion_queue->mutex);
            completion_queue->entries.push_back(
                CompletionQueue::Entry{
                    CompletionQueue::Entry::Kind::complete,
                    index,
                    std::nullopt});
          }
          completion_queue->ready.notify_one();
          return result;
        }));
  }

  tool_results.resize(planned_requests.size());
  std::size_t remaining = planned_requests.size();
  while (remaining > 0U) {
    CompletionQueue::Entry entry;
    {
      std::unique_lock<std::mutex> lock(completion_queue->mutex);
      completion_queue->ready.wait(lock, [&] {
        return !completion_queue->entries.empty();
      });
      entry = std::move(completion_queue->entries.front());
      completion_queue->entries.pop_front();
    }

    const auto& request = effective_requests[entry.index];
    if (entry.kind == CompletionQueue::Entry::Kind::update) {
      if (abort_requested()) {
        continue;
      }
      AgentEvent update =
          make_event(AgentEventKind::tool_execution_updated, turn_index);
      update.tool_request = request;
      update.tool_result = std::move(entry.result);
      emit(&sink, std::move(update));
      continue;
    }

    --remaining;
    const auto index = entry.index;
    tool_results[index] = futures[index].get();
    if (abort_requested()) {
      tool_results[index] = make_aborted_tool_result(request);
    }

    auto& tool_result = tool_results[index];
    const auto& hook_context = hook_contexts[index];

    for (const auto& transform : config_.tool_result_transforms) {
      if (auto replacement = transform(hook_context, request, tool_result)) {
        replacement->call_id = tool_result.call_id;
        tool_result = std::move(*replacement);
      }
    }
    for (const auto& hook : config_.after_tool_hooks) {
      hook(hook_context, request, tool_result);
    }

    ++state_.completed_tool_calls;

    AgentEvent tool_event =
        make_event(AgentEventKind::tool_executed, turn_index);
    tool_event.tool_request = request;
    tool_event.tool_result = tool_result;
    emit(&sink, std::move(tool_event));
  }

  for (std::size_t index = 0; index < planned_requests.size(); ++index) {
    const auto& request = effective_requests[index];
    const auto& tool_result = tool_results[index];

    result.tool_requests.push_back(request);
    result.tool_results.push_back(tool_result);

    ai::Message tool_message = make_tool_result_message(request, tool_result);
    commit_message(std::move(tool_message), turn_index, sink);
  }

  return planned_requests.size();
}

void Agent::emit(EventSink* sink, AgentEvent event) {
  if (!sink) {
    return;
  }

  sink->push(event);
  ++state_.next_event_sequence;
}

void VectorEventSink::push(const AgentEvent& event) { events_.push_back(event); }

const std::vector<AgentEvent>& VectorEventSink::events() const noexcept {
  return events_;
}

}  // namespace qt::agent
