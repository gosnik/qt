#pragma once

#include <optional>
#include <string>
#include <vector>

#include "qt/agent/agent.hpp"
#include "qt/ai/models.hpp"
#include "qt/foundation/error.hpp"
#include "qt/app/export/stream_output.hpp"

namespace qt::harness {
class AgentHarness;
}

namespace qt::app {

enum class NoninteractiveOutputMode {
  print,
  json,
};

struct NoninteractivePromptRequest {
  std::string text;
  std::optional<std::string> message_id;
  std::vector<ai::ImageContent> images;
  ai::Json metadata = ai::Json::object();
};

struct NoninteractiveRunResult {
  agent::QueuedMessage queued_message;
  agent::TurnResult turn;
  std::string print_output;
  std::vector<ai::Json> json_events;
};

using NoninteractiveRenderedOutput = StreamOutputPlan;

foundation::Result<NoninteractiveRunResult> run_noninteractive_prompt(
    agent::Agent& agent, const NoninteractivePromptRequest& request);
foundation::Result<NoninteractiveRunResult> run_noninteractive_prompt(
    harness::AgentHarness& harness, const NoninteractivePromptRequest& request);

ai::Json noninteractive_run_result_to_json(
    const NoninteractiveRunResult& result);

NoninteractiveRenderedOutput plan_noninteractive_output(
    const NoninteractiveRunResult& result, NoninteractiveOutputMode mode);

std::vector<std::string> render_noninteractive_output(
    const NoninteractiveRunResult& result, NoninteractiveOutputMode mode);

}  // namespace qt::app
