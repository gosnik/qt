#include "qt/harness/agent_harness.hpp"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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

}  // namespace

int main(int argc, char** argv) {
  try {
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
