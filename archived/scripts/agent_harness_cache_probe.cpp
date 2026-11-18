#include "qt/harness/agent_harness.hpp"
#include "qt/app/runtime/runtime_host.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class ImmediateProvider final : public qt::ai::Provider {
 public:
  const qt::ai::ProviderDescriptor& descriptor() const override {
    return descriptor_;
  }

  qt::ai::CompletionResponse complete(
      const qt::ai::CompletionRequest& request,
      const qt::ai::ProviderOptions& options) override {
    ++request_count_;
    last_model_ = request.model;
    last_tools_ = request.tools;
    last_max_output_tokens_ = request.max_output_tokens;
    last_options_ = options;

    qt::ai::CompletionResponse response;
    response.stop_reason = qt::ai::StopReason::end_turn;
    response.message.role = qt::ai::MessageRole::assistant;
    response.message.content = {
        qt::ai::TextContent{"ok", std::nullopt}};
    return response;
  }

  std::size_t request_count() const noexcept { return request_count_; }
  const qt::ai::ModelRef& last_model() const noexcept { return last_model_; }
  const std::vector<qt::ai::ToolDefinition>& last_tools() const noexcept {
    return last_tools_;
  }
  const std::optional<std::int64_t>& last_max_output_tokens() const noexcept {
    return last_max_output_tokens_;
  }
  const qt::ai::ProviderOptions& last_options() const noexcept {
    return last_options_;
  }

 private:
  qt::ai::ProviderDescriptor descriptor_{"immediate", "Immediate", "probe",
                                         qt::ai::ProviderTransport::auto_detect,
                                         {}};
  std::size_t request_count_ = 0U;
  qt::ai::ModelRef last_model_;
  std::vector<qt::ai::ToolDefinition> last_tools_;
  std::optional<std::int64_t> last_max_output_tokens_;
  qt::ai::ProviderOptions last_options_;
};

class ProbeTool final : public qt::tools::Tool {
 public:
  ProbeTool() {
    descriptor_.name = "tool_1";
    descriptor_.description = "Tracked direct-call probe";
    descriptor_.input_schema = {
        {"type", "object"},
        {"properties", {{"value", {{"type", "string"}}}}},
        {"required", qt::tools::Json::array({"value"})},
        {"additionalProperties", false},
    };
  }

  const qt::tools::ToolDescriptor& descriptor() const override {
    return descriptor_;
  }

  qt::tools::ToolResult invoke(
      const qt::tools::ToolRequest& request,
      const qt::tools::ToolContext& context) const override {
    if (context.update_callback) {
      qt::tools::ToolResult progress;
      progress.call_id = request.call_id;
      progress.output = {{"progress", "running"}};
      context.update_callback(progress);
    }
    qt::tools::ToolResult result;
    result.call_id = request.call_id;
    result.output = {{"value", request.arguments.at("value")}};
    return result;
  }

 private:
  qt::tools::ToolDescriptor descriptor_;
};

struct BlockingToolState {
  std::mutex mutex;
  std::condition_variable condition;
  std::shared_ptr<qt::foundation::ProcessCancellationToken> token;
  bool entered = false;
  bool release = false;
  bool finished = false;
};

class BlockingTool final : public qt::tools::Tool {
 public:
  explicit BlockingTool(std::shared_ptr<BlockingToolState> state)
      : state_(std::move(state)) {
    descriptor_.name = "blocking";
    descriptor_.description = "Waits for the shutdown cancellation token";
    descriptor_.input_schema = {{"type", "object"},
                                {"additionalProperties", false}};
  }

  const qt::tools::ToolDescriptor& descriptor() const override {
    return descriptor_;
  }

  qt::tools::ToolResult invoke(
      const qt::tools::ToolRequest& request,
      const qt::tools::ToolContext& context) const override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->token = context.cancellation_token;
    state_->entered = true;
    state_->condition.notify_all();
    const bool released = state_->condition.wait_for(
        lock, std::chrono::seconds(2), [this] {
          return state_->release ||
                 (state_->token && state_->token->is_cancelled());
        });
    state_->finished = true;
    state_->condition.notify_all();

    qt::tools::ToolResult result;
    result.call_id = request.call_id;
    result.status = released ? qt::tools::ToolResultStatus::success
                             : qt::tools::ToolResultStatus::execution_error;
    result.output = {{"cancelled", state_->token && state_->token->is_cancelled()}};
    return result;
  }

 private:
  std::shared_ptr<BlockingToolState> state_;
  qt::tools::ToolDescriptor descriptor_;
};

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

qt::agent::AgentConfig make_config(std::size_t tool_count) {
  qt::agent::AgentConfig config;
  config.model = {"probe", "probe", "model-a"};
  config.tool_registry = std::make_shared<qt::tools::ToolRegistry>();
  require(config.tool_registry->register_tool(std::make_unique<ProbeTool>()),
          "failed to register direct-call probe tool");
  config.tools.reserve(tool_count);
  for (std::size_t index = 0; index < tool_count; ++index) {
    qt::ai::ToolDefinition tool;
    tool.name = "tool_" + std::to_string(index);
    tool.description = "Probe tool " + std::to_string(index);
    tool.input_schema = {{"type", "object"},
                         {"properties", {{"value", {{"type", "string"}}}}}};
    config.tools.push_back(std::move(tool));
  }
  return config;
}

qt::agent::AgentConfig make_blocking_tool_config(
    const std::shared_ptr<BlockingToolState>& state) {
  qt::agent::AgentConfig config;
  config.model = {"probe", "probe", "model-a"};
  config.tool_registry = std::make_shared<qt::tools::ToolRegistry>();
  require(config.tool_registry->register_tool(std::make_unique<BlockingTool>(state)),
          "failed to register blocking probe tool");
  qt::ai::ToolDefinition definition;
  definition.name = "blocking";
  definition.description = "Waits for shutdown cancellation";
  definition.input_schema = {{"type", "object"},
                             {"additionalProperties", false}};
  config.tools.push_back(std::move(definition));
  return config;
}

void run_shutdown_abort_tool_cancellation_probe() {
  auto state = std::make_shared<BlockingToolState>();
  auto provider = std::make_shared<ImmediateProvider>();
  qt::harness::AgentHarness harness(make_blocking_tool_config(state), provider);
  qt::tools::ToolRequest request;
  request.tool_name = "blocking";
  request.arguments = qt::tools::Json::object();

  qt::agent::TurnResult result;
  std::thread submitter([&] {
    result = harness.submit_tool_call("run blocking tool", std::move(request));
  });

  std::shared_ptr<qt::foundation::ProcessCancellationToken> token;
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    const bool entered = state->condition.wait_for(
        lock, std::chrono::seconds(2), [&] { return state->entered; });
    if (entered) {
      token = state->token;
    }
    if (!entered || token == nullptr) {
      state->release = true;
      state->condition.notify_all();
      lock.unlock();
      submitter.join();
      require(false, "blocking tool did not receive a cancellation token");
    }
  }

  harness.request_shutdown_abort();
  state->condition.notify_all();
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    const bool finished = state->condition.wait_for(
        lock, std::chrono::seconds(2), [&] { return state->finished; });
    if (!finished) {
      state->release = true;
      state->condition.notify_all();
      const bool released = state->condition.wait_for(
          lock, std::chrono::seconds(2), [&] { return state->finished; });
      lock.unlock();
      submitter.join();
      require(released, "blocking tool fallback release did not complete");
      require(false, "shutdown abort did not release the blocking tool");
    }
  }
  submitter.join();
  require(token->is_cancelled(),
          "shutdown abort did not cancel the active tool token");
  require(!result.ok && result.error == "agent turn aborted",
          "shutdown abort did not propagate the agent abort result");
}

void run_bounded_diagnostic_history_probe() {
  constexpr std::size_t kTurns = 128U;
  auto provider = std::make_shared<ImmediateProvider>();
  qt::app::RuntimeHostRequest request;
  request.agent_config = make_config(0U);
  request.provider = provider;
  auto host = qt::app::create_runtime_host(std::move(request));
  require(host.ok(), "bounded diagnostic runtime creation failed");
  auto& harness = host.value().harness();

  std::size_t context_calls = 0U;
  std::size_t model_context_calls = 0U;
  std::size_t request_calls = 0U;
  std::size_t payload_calls = 0U;
  std::size_t response_calls = 0U;
  harness.add_context_hook([&](const qt::harness::AgentHarness::ContextEvent&) {
    ++context_calls;
    return std::nullopt;
  });
  harness.add_model_context_augmentor(
      [&](const qt::harness::AgentHarness::ModelContextEvent&) {
        ++model_context_calls;
        return std::nullopt;
      });
  harness.add_before_provider_request_hook(
      [&](const qt::harness::AgentHarness::BeforeProviderRequestEvent&) {
        ++request_calls;
        return std::nullopt;
      });
  harness.add_before_provider_payload_hook(
      [&](const qt::harness::AgentHarness::BeforeProviderPayloadEvent&) {
        ++payload_calls;
        return std::nullopt;
      });
  harness.add_after_provider_response_hook(
      [&](const qt::harness::AgentHarness::AfterProviderResponseEvent&) {
        ++response_calls;
      });

  std::uint64_t previous_save_point = 0U;
  for (std::size_t turn = 0; turn < kTurns; ++turn) {
    require(harness.submit_prompt("bounded " + std::to_string(turn)).ok,
            "bounded diagnostic prompt failed");
    require(harness.context_events().size() == 1U &&
                harness.model_context_events().size() == 1U &&
                harness.before_provider_request_events().size() == 1U &&
                harness.before_provider_payload_events().size() == 1U &&
                harness.after_provider_response_events().size() == 1U &&
                harness.save_points().size() == 1U,
            "bounded diagnostic history was not trimmed");
    const auto& save_point = harness.save_points().back();
    require(save_point.sequence > previous_save_point,
            "save-point sequence did not increase");
    previous_save_point = save_point.sequence;
  }
  require(context_calls == kTurns && model_context_calls == kTurns &&
              request_calls == kTurns && payload_calls == kTurns &&
              response_calls == kTurns,
          "bounded diagnostic hooks did not all execute");
  require(harness.agent().state().transcript.size() == kTurns * 2U,
          "bounded diagnostic history truncated the transcript");
  require(harness.session_manager().build_session_context().messages.size() ==
              kTurns * 2U,
          "bounded diagnostic history truncated the persisted session");

  harness.set_diagnostic_history_limit(0U);
  require(harness.submit_prompt("unlimited").ok,
          "unlimited diagnostic prompt failed");
  require(harness.context_events().size() == 2U &&
              harness.save_points().size() == 2U,
          "unlimited diagnostic history did not retain prior events");

  harness.set_diagnostic_history_limit(1U);
  require(harness.context_events().size() == 1U &&
              harness.save_points().size() == 1U &&
              harness.save_points().back().sequence > previous_save_point,
          "changing the diagnostic limit did not preserve the latest state");
  qt::harness::AgentHarness moved(std::move(harness));
  require(moved.submit_prompt("moved").ok &&
              moved.context_events().size() == 1U,
          "move constructor did not preserve diagnostic limit");
  qt::harness::AgentHarness assigned(make_config(0U), provider);
  assigned.set_diagnostic_history_limit(0U);
  assigned = std::move(moved);
  require(assigned.submit_prompt("assigned").ok &&
              assigned.context_events().size() == 1U,
          "move assignment did not preserve diagnostic limit");
}

void run_incremental_session_probe() {
  using namespace qt::harness;
  SessionManager source;
  const auto first = source.append_user_message("first");
  SessionAssistantMessageEntry assistant;
  assistant.provider = "probe";
  assistant.model = "model";
  assistant.usage = {{"input", 10}, {"output", 3}, {"cacheRead", 4},
                     {"cacheWrite", 2}, {"cost", {{"total", 0.25}}}};
  source.append_assistant_message(assistant);
  SessionManager mirror;
  mirror.synchronize_entries_from(source);
  require(mirror.entries().size() == 2 && mirror.get_entry(first) != nullptr,
          "initial session synchronization lost entries or tree IDs");
  auto stats = mirror.stats(100);
  require(stats.user_messages == 1 && stats.assistant_messages == 1 &&
              stats.tokens.total == 19 && stats.cost == 0.25 &&
              stats.context_usage->tokens == 16,
          "incremental known usage/context statistics differ");
  source.append_compaction("summary", first, 7);
  mirror.synchronize_entries_from(source);
  stats = mirror.stats(100);
  require(stats.tokens.total == 7 && !stats.context_usage->tokens &&
              stats.cost == 0.25 && stats.total_messages == 2,
          "compaction incorrectly reset historical counts/cost or context");
  assistant.usage = Json::object();
  source.append_assistant_message(assistant);
  mirror.synchronize_entries_from(source);
  require(mirror.stats(100).has_unknown_usage &&
              !mirror.stats(100).context_usage->tokens &&
              mirror.stats().models.at("probe/model").has_unknown_usage,
          "unknown usage was lost by incremental statistics");
  auto diverged = source;
  source.append_user_message("source fork");
  diverged.append_user_message("other fork");
  mirror.synchronize_entries_from(diverged);
  mirror.synchronize_entries_from(source);
  require(encode_session_entry_jsonl(mirror.entries().back()) ==
              encode_session_entry_jsonl(source.entries().back()) &&
              mirror.stats().user_messages == 2,
          "equal-length divergent sessions retained a stale prefix");
  SessionManager replacement;
  replacement.append_user_message("replacement");
  mirror.synchronize_entries_from(replacement);
  require(mirror.entries().size() == 1 && mirror.stats().assistant_messages == 0,
          "shorter replacement session retained old entries or statistics");
  SessionManager copied = replacement;
  copied.append_user_message("copy append");
  mirror.synchronize_entries_from(copied);
  require(mirror.get_leaf_id() == copied.get_leaf_id() &&
              mirror.stats().user_messages == 2,
          "shared-prefix append synchronization lost tree leaf or counts");
  copied.append_session_info("named");
  mirror.synchronize_entries_from(copied);
  require(mirror.get_session_name() == std::optional<std::string>{"named"},
          "session synchronization lost cached session name");
  copied.append_session_info("");
  mirror.synchronize_entries_from(copied);
  require(!mirror.get_session_name(), "empty latest session name was not cleared");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    run_incremental_session_probe();
    run_bounded_diagnostic_history_probe();
    run_shutdown_abort_tool_cancellation_probe();
    const std::size_t turn_count = argc > 1
                                       ? static_cast<std::size_t>(
                                             std::stoull(argv[1]))
                                       : 400U;
    const bool rebuild_each_turn = argc > 2 &&
                                   std::string(argv[2]) == "rebuild";
    auto provider = std::make_shared<ImmediateProvider>();
    qt::harness::AgentHarness harness(make_config(256U), provider);
    const std::vector<std::string> active_tools = {
        "tool_1", "tool_17", "tool_129", "tool_255"};
    harness.set_active_tool_names(active_tools);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t turn = 0; turn < turn_count; ++turn) {
      const std::string model_name =
          rebuild_each_turn && (turn % 2U) != 0U ? "model-b" : "model-a";
      harness.set_model({"probe", "probe", model_name});
      const auto result = harness.submit_prompt("turn " + std::to_string(turn));
      require(result.ok, "repeated prompt failed");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    require(provider->request_count() == turn_count,
            "provider request count changed");
    require(provider->last_tools().size() == active_tools.size(),
            "cached filtered tool count changed");
    for (std::size_t index = 0; index < active_tools.size(); ++index) {
      require(provider->last_tools()[index].name == active_tools[index],
              "cached filtered tool order changed");
    }

    harness.set_active_tool_names(active_tools);
    require(harness.submit_prompt("same configuration").ok,
            "same-configuration prompt failed");

    harness.set_active_tool_names({"tool_2", "tool_3"});
    require(harness.submit_prompt("changed tools").ok,
            "tool-change prompt failed");
    require(provider->last_tools().size() == 2U &&
                provider->last_tools()[0].name == "tool_2" &&
                provider->last_tools()[1].name == "tool_3",
            "tool cache was not invalidated");

    qt::ai::ProviderOptions large_model_options;
    large_model_options.metadata = {{"maxTokens", 65536},
                                    {"contextWindow", 131072}};
    harness.set_model({"probe", "probe", "model-b"}, provider,
                      large_model_options);
    require(harness.submit_prompt("changed model").ok,
            "model-change prompt failed");
    require(provider->last_model().name == "model-b",
            "effective config model was not invalidated");
    require(provider->last_max_output_tokens() == 32000,
            "large model output limit was not capped at the Pi default");
    require(provider->last_options().metadata.value("maxTokens", 0) == 65536 &&
                provider->last_options().metadata.value("contextWindow", 0) ==
                    131072,
            "selected model provider metadata was not switched atomically");

    qt::ai::ProviderOptions small_model_options;
    small_model_options.metadata = {{"maxTokens", 8192},
                                    {"contextWindow", 32768}};
    harness.set_model({"probe", "probe", "model-c"}, provider,
                      small_model_options);
    require(harness.submit_prompt("changed to small model").ok,
            "small model-change prompt failed");
    require(provider->last_model().name == "model-c" &&
                provider->last_max_output_tokens() == 8192,
            "small model output limit did not follow model metadata");

    qt::harness::AgentHarness moved(std::move(harness));
    require(moved.submit_prompt("after move").ok,
            "moved harness prompt failed");

    const auto provider_requests_before_tool_call = provider->request_count();
    moved.set_active_tool_names({"tool_1"});
    moved.clear_events();
    qt::tools::ToolRequest direct_request;
    direct_request.tool_name = "tool_1";
    direct_request.arguments = {{"value", "tracked"}};
    const auto direct_result = moved.submit_tool_call(
        "Run the tracked direct tool", std::move(direct_request));
    require(direct_result.ok, "direct tool call failed");
    require(provider->request_count() == provider_requests_before_tool_call,
            "direct tool call unexpectedly invoked the provider");
    require(direct_result.tool_requests.size() == 1U &&
                direct_result.tool_results.size() == 1U,
            "direct tool result was not attached to its turn");
    require(moved.tool_call_events().size() == 1U &&
                moved.tool_result_events().size() == 1U,
            "direct tool call did not traverse harness tracking hooks");
    const auto& direct_transcript = moved.agent().state().transcript;
    require(direct_transcript.size() >= 3U,
            "direct tool messages were not committed");
    const auto direct_start = direct_transcript.size() - 3U;
    require(direct_transcript[direct_start].role == qt::ai::MessageRole::user &&
                direct_transcript[direct_start + 1U].role ==
                    qt::ai::MessageRole::assistant &&
                direct_transcript[direct_start + 2U].role ==
                    qt::ai::MessageRole::tool,
            "direct tool transcript is not a user/call/result sequence");

    std::cout << "turns=" << turn_count
              << " mode=" << (rebuild_each_turn ? "rebuild" : "cached")
              << " elapsed_us=" << elapsed.count()
              << " average_us="
              << (turn_count == 0U ? 0U
                                   : static_cast<std::uint64_t>(
                                         elapsed.count()) /
                                         turn_count)
              << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "agent harness cache probe failed: " << error.what() << '\n';
    return 1;
  }
}
