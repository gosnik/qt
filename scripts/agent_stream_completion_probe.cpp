#include "qt/agent/agent.hpp"
#include "qt/ai/provider_transport.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using qt::ai::EventKind;
using qt::ai::MessageRole;
using qt::ai::StopReason;
using qt::ai::StreamEvent;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

StreamEvent done(StopReason reason) {
  StreamEvent event;
  event.kind = EventKind::message_done;
  event.stop_reason = reason;
  return event;
}

StreamEvent text(std::string value) {
  StreamEvent event;
  event.kind = EventKind::content_delta;
  event.delta = std::move(value);
  event.metadata["content_type"] = "text";
  return event;
}

StreamEvent tool_call(std::string id, std::string name) {
  StreamEvent event;
  event.kind = EventKind::tool_call;
  event.content = qt::ai::ToolCallContent{std::move(id), std::move(name),
                                           {{"value", "ok"}}};
  return event;
}

StreamEvent error(std::string value, StopReason reason = StopReason::error) {
  StreamEvent event;
  event.kind = EventKind::error;
  event.stop_reason = reason;
  event.error = std::move(value);
  return event;
}

class ScriptedStreamingProvider final : public qt::ai::Provider {
 public:
  explicit ScriptedStreamingProvider(std::vector<std::vector<StreamEvent>> steps)
      : steps_(std::move(steps)) {}

  const qt::ai::ProviderDescriptor& descriptor() const override {
    return descriptor_;
  }

  qt::ai::CompletionResponse complete(const qt::ai::CompletionRequest&,
                                      const qt::ai::ProviderOptions&) override {
    throw std::runtime_error("streaming probe unexpectedly called complete");
  }

  void stream(const qt::ai::CompletionRequest& request,
              const qt::ai::ProviderOptions&, qt::ai::EventSink& sink) override {
    requests_.push_back(request);
    require(next_ < steps_.size(), "streaming provider exhausted");
    for (const auto& event : steps_[next_++]) {
      sink.push(event);
    }
  }

  std::size_t request_count() const noexcept { return requests_.size(); }

 private:
  qt::ai::ProviderDescriptor descriptor_{"stream-probe", "Stream probe",
                                          "probe", qt::ai::ProviderTransport::sse,
                                          {true, true, false, false, false}};
  std::vector<std::vector<StreamEvent>> steps_;
  std::vector<qt::ai::CompletionRequest> requests_;
  std::size_t next_ = 0;
};

class CountingTool final : public qt::tools::Tool {
 public:
  explicit CountingTool(std::size_t& invocations) : invocations_(invocations) {
    descriptor_.name = "count";
    descriptor_.description = "Counts invocations";
    descriptor_.input_schema = {{"type", "object"},
                                {"properties", {{"value", {{"type", "string"}}}}},
                                {"required", qt::tools::Json::array({"value"})},
                                {"additionalProperties", false}};
  }

  const qt::tools::ToolDescriptor& descriptor() const override {
    return descriptor_;
  }

  qt::tools::ToolResult invoke(const qt::tools::ToolRequest& request,
                               const qt::tools::ToolContext&) const override {
    ++invocations_;
    qt::tools::ToolResult result;
    result.call_id = request.call_id;
    result.output = {{"count", invocations_}};
    return result;
  }

 private:
  std::size_t& invocations_;
  qt::tools::ToolDescriptor descriptor_;
};

qt::agent::AgentConfig agent_config(std::size_t& invocations, bool retry,
                                    std::uint32_t max_attempts = 3U) {
  qt::agent::AgentConfig config;
  config.model = {"stream-probe", "probe", "model"};
  config.auto_retry_enabled = retry;
  config.auto_retry_max_attempts = max_attempts;
  config.auto_retry_base_delay_ms = 0;
  config.tool_registry = std::make_shared<qt::tools::ToolRegistry>();
  require(config.tool_registry->register_tool(
              std::make_unique<CountingTool>(invocations)),
          "failed to register counting tool");
  config.tools.push_back({"count", "Counts invocations",
                          {{"type", "object"},
                           {"properties", {{"value", {{"type", "string"}}}}},
                           {"required", qt::ai::Json::array({"value"})},
                           {"additionalProperties", false}}});
  return config;
}

void enqueue_prompt(qt::agent::Agent& agent, qt::agent::EventSink& sink) {
  qt::ai::Message message;
  message.role = MessageRole::user;
  message.content.emplace_back(qt::ai::TextContent{"begin", std::nullopt});
  agent.enqueue(std::move(message), &sink);
}

bool transcript_contains(const qt::agent::Agent& agent, const std::string& needle) {
  for (const auto& message : agent.state().transcript) {
    for (const auto& content : message.content) {
      if (qt::ai::content_kind(content) == qt::ai::ContentKind::text &&
          std::get<qt::ai::TextContent>(content).text == needle) {
        return true;
      }
    }
  }
  return false;
}

qt::ai::CompletionResponse aggregate_chat_sse(const std::string& body) {
  auto transport = std::make_unique<qt::ai::RecordedHttpTransport>();
  transport->push_response({200, {}, body});
  qt::ai::OpenAICompletionsProvider provider(std::move(transport), "http://probe");
  qt::ai::CompletionRequest request;
  request.model = {"openai", "openai-completions", "probe"};
  qt::ai::VectorEventSink sink;
  provider.stream(request, {}, sink);
  return qt::ai::completion_response_from_events(sink.events());
}

void verify_aggregation_contract() {
  const auto empty = qt::ai::completion_response_from_events({});
  require(empty.stop_reason == StopReason::error,
          "empty event stream must become an error");

  const auto tool = qt::ai::completion_response_from_events(
      {tool_call("valid-tool", "count"), done(StopReason::tool_call)});
  require(tool.stop_reason == StopReason::tool_call,
          "tool-call completion must remain terminal");

  StreamEvent usage;
  usage.kind = EventKind::usage;
  usage.usage = qt::ai::Usage{3, 4, 0, 0, 7};
  const auto max_tokens = qt::ai::completion_response_from_events(
      {text("limited"), usage, done(StopReason::max_tokens)});
  require(max_tokens.stop_reason == StopReason::max_tokens &&
              max_tokens.usage.total_tokens == 7,
          "max-token completion or usage was lost");

  const auto sticky_error = qt::ai::completion_response_from_events(
      {error("provider failed"), done(StopReason::end_turn)});
  require(sticky_error.stop_reason == StopReason::error &&
              sticky_error.metadata.value("errorMessage", std::string()) ==
                  "provider failed",
          "error must remain sticky after message_done");

  const auto aborted = qt::ai::completion_response_from_events(
      {error("cancelled", StopReason::aborted), done(StopReason::end_turn)});
  require(aborted.stop_reason == StopReason::aborted,
          "aborted stream must remain aborted");

  const std::string chat_partial = R"(data: {"id":"chat-eof","choices":[{"delta":{"content":"partial"},"finish_reason":null}]}

data: [DONE]

)";
  const auto chat = aggregate_chat_sse(chat_partial);
  require(chat.stop_reason == StopReason::error &&
              chat.metadata.value("errorMessage", std::string()).find(
                  "ended without a completion signal") != std::string::npos,
          "chat EOF followed only by DONE must fail");

  const std::string responses_partial = R"(data: {"type":"response.created","response":{"id":"response-eof"}}

data: {"type":"response.output_text.delta","delta":"partial"}

data: [DONE]

)";
  const auto responses = qt::ai::completion_response_from_events(
      qt::ai::parse_openai_responses_sse(responses_partial));
  require(responses.stop_reason == StopReason::error &&
              responses.metadata.value("errorMessage", std::string()).find(
                  "ended without a completion signal") != std::string::npos,
          "responses EOF followed only by DONE must fail");
}

void verify_retry_recovers_after_tool_call() {
  auto provider = std::make_shared<ScriptedStreamingProvider>(
      std::vector<std::vector<StreamEvent>>{
          {tool_call("first", "count"), done(StopReason::tool_call)},
          {tool_call("incomplete", "count")},
          {text("recovered"), done(StopReason::end_turn)},
      });
  std::size_t invocations = 0;
  qt::agent::Agent agent(agent_config(invocations, true), provider);
  qt::agent::VectorEventSink sink;
  enqueue_prompt(agent, sink);
  const auto result = agent.run_turn(sink);

  require(result.ok && result.response &&
              result.response->stop_reason == StopReason::end_turn,
          "recovered stream must complete the turn");
  require(provider->request_count() == 3U && result.requests.size() == 3U,
          "tool loop plus retry must issue exactly three requests");
  require(invocations == 1U && result.tool_requests.size() == 1U,
          "incomplete tool call must never execute");
  for (const auto& message : result.requests.back().messages) {
    for (const auto& item : message.content) {
      if (qt::ai::content_kind(item) == qt::ai::ContentKind::tool_call) {
        require(std::get<qt::ai::ToolCallContent>(item).id != "incomplete",
                "retry request included the incomplete assistant tool call");
      }
    }
  }
  require(transcript_contains(agent, "recovered") &&
              !transcript_contains(agent, "partial"),
          "only recovered assistant response may remain after retry");
  for (const auto& message : agent.state().transcript) {
    for (const auto& item : message.content) {
      if (qt::ai::content_kind(item) == qt::ai::ContentKind::tool_call) {
        require(std::get<qt::ai::ToolCallContent>(item).id != "incomplete",
                "incomplete assistant response remained in transcript");
      }
    }
  }
}

void verify_retry_boundaries() {
  for (const bool enabled : {false, true}) {
    auto provider = std::make_shared<ScriptedStreamingProvider>(
        std::vector<std::vector<StreamEvent>>{{text("partial-a")},
                                               {text("partial-b")}});
    std::size_t invocations = 0;
    qt::agent::Agent agent(agent_config(invocations, enabled, 1U), provider);
    qt::agent::VectorEventSink sink;
    enqueue_prompt(agent, sink);
    const auto result = agent.run_turn(sink);
    require(!result.ok && result.error,
            "uncompleted stream must fail when retry is disabled or exhausted");
    require(provider->request_count() == (enabled ? 2U : 1U),
            "retry boundary issued an unexpected request count");
  }
}

void verify_nonstream_unknown() {
  for (const bool enabled : {false, true}) {
    qt::ai::ProviderDescriptor descriptor;
    descriptor.capabilities.streaming = false;
    auto provider = std::make_shared<qt::ai::ScriptedProvider>(descriptor);
    qt::ai::CompletionResponse incomplete;
    incomplete.message.role = MessageRole::assistant;
    incomplete.message.content.emplace_back(
        qt::ai::ToolCallContent{"incomplete", "count", {{"value", "ok"}}});
    incomplete.stop_reason = StopReason::unknown;
    provider->push_response(incomplete);
    provider->push_response(qt::ai::completion_response_from_events(
        {text("recovered"), done(StopReason::end_turn)}));
    std::size_t invocations = 0;
    qt::agent::Agent agent(agent_config(invocations, enabled, 1U), provider);
    qt::agent::VectorEventSink sink;
    enqueue_prompt(agent, sink);
    const auto result = agent.run_turn(sink);
    require(result.ok == enabled && invocations == 0U,
            "unknown nonstream completion bypassed failure/retry policy");
    require(provider->requests().size() == (enabled ? 2U : 1U),
            "unknown nonstream completion used incorrect retry count");
  }
}

}  // namespace

int main() {
  try {
    verify_aggregation_contract();
    verify_retry_recovers_after_tool_call();
    verify_retry_boundaries();
    verify_nonstream_unknown();
    std::cout << "agent stream completion probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "agent stream completion probe failed: " << error.what() << '\n';
    return 1;
  }
}
