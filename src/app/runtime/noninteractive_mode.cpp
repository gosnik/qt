#include "qt/app/runtime/noninteractive_mode.hpp"

#include <algorithm>
#include <utility>

#include "qt/app/rpc/rpc_protocol.hpp"
#include "qt/harness/agent_harness.hpp"
#include "qt/tools/models.hpp"

namespace qt::app {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

ai::Json normalize_metadata(const ai::Json& metadata) {
  return metadata.is_object() ? metadata : ai::Json::object();
}

void append_text_with_newline(std::string& output, const std::string& text) {
  if (text.empty()) {
    return;
  }

  output += text;
  if (output.empty() || output.back() != '\n') {
    output.push_back('\n');
  }
}

void append_printable_content(std::string& output, const ai::Message& message) {
  if (message.role != ai::MessageRole::assistant) {
    return;
  }

  for (const auto& content : message.content) {
    if (ai::content_kind(content) != ai::ContentKind::text) {
      continue;
    }

    append_text_with_newline(output, std::get<ai::TextContent>(content).text);
  }
}

std::optional<std::string> format_noninteractive_error_output(
    const agent::TurnResult& turn) {
  if (turn.ok || !turn.error.has_value() || turn.error->empty()) {
    return std::nullopt;
  }

  return "error: " + *turn.error + "\n";
}

std::string final_printable_content(const agent::TurnResult& turn) {
  if (!turn.ok) {
    return {};
  }

  const ai::CompletionResponse* response = nullptr;
  if (turn.response.has_value()) {
    response = &*turn.response;
  } else if (!turn.responses.empty()) {
    response = &turn.responses.back();
  }
  if (response == nullptr) {
    return {};
  }

  std::string output;
  append_printable_content(output, response->message);
  return output;
}

ai::Json queued_message_to_json(const agent::QueuedMessage& message) {
  return ai::Json{
      {"sequence", message.sequence},
      {"disposition", agent::to_string(message.disposition)},
      {"message", message.message},
  };
}

ai::Json agent_event_to_json(const agent::AgentEvent& event) {
  ai::Json json{
      {"type", agent::to_string(event.kind)},
      {"sequence", event.sequence},
      {"turn_index", event.turn_index},
  };

  if (event.queued_message) {
    json["queued_message"] = queued_message_to_json(*event.queued_message);
  }
  if (event.request) {
    json["request"] = *event.request;
  }
  if (event.provider_event) {
    json["provider_event"] = *event.provider_event;
  }
  if (event.tool_request) {
    json["tool_request"] = *event.tool_request;
  }
  if (event.tool_result) {
    json["tool_result"] = *event.tool_result;
  }
  if (event.subagent) json["subagent"] = *event.subagent;
  if (event.committed_message) {
    json["committed_message"] = *event.committed_message;
  }
  if (event.response) {
    json["response"] = *event.response;
  }
  if (event.error) {
    json["error"] = *event.error;
  }
  if (event.kind == agent::AgentEventKind::turn_failed) {
    json["qt"] = ai::Json{{"errorCode", "turn_failed"},
                          {"command", "noninteractive_prompt"}};
  }
  if (event.retry_attempt) {
    json["attempt"] = *event.retry_attempt;
  }
  if (event.retry_max_attempts) {
    json["max_attempts"] = *event.retry_max_attempts;
  }
  if (event.retry_delay_ms) {
    json["delay_ms"] = *event.retry_delay_ms;
  }
  if (event.retry_success) {
    json["success"] = *event.retry_success;
  }

  return json;
}

ai::Json auto_retry_start_event_to_json(const agent::AgentEvent& event) {
  return ai::Json{{"type", "auto_retry_start"},
                  {"attempt", event.retry_attempt.value_or(0U)},
                  {"maxAttempts", event.retry_max_attempts.value_or(0U)},
                  {"delayMs", event.retry_delay_ms.value_or(0U)},
                  {"errorMessage", event.error.value_or(std::string{})}};
}

ai::Json auto_retry_end_event_to_json(const agent::AgentEvent& event) {
  ai::Json json{{"type", "auto_retry_end"},
                {"success", event.retry_success.value_or(false)},
                {"attempt", event.retry_attempt.value_or(0U)}};
  if (event.error && !event.error->empty()) {
    json["finalError"] = *event.error;
  }
  return json;
}

std::string tool_result_output_text(const tools::ToolResult& result) {
  if (result.output.contains("output") && result.output.at("output").is_string()) {
    return result.output.at("output").get<std::string>();
  }
  return result.output.dump();
}

ai::Json tool_result_payload_json(const tools::ToolResult& result) {
  ai::Json payload{
      {"content",
       ai::Json::array(
           {ai::Json{{"type", "text"}, {"text", tool_result_output_text(result)}}})},
      {"details", result.output},
  };
  if (result.error_message.has_value()) {
    payload["errorMessage"] = *result.error_message;
  }
  return payload;
}

ai::Json tool_result_message_json(const tools::ToolRequest& request,
                                 const tools::ToolResult& result) {
  ai::Json message{
      {"role", "toolResult"},
      {"toolCallId", request.call_id.value_or(result.call_id.value_or(""))},
      {"toolName", request.tool_name},
      {"content",
       ai::Json::array(
           {ai::Json{{"type", "text"}, {"text", tool_result_output_text(result)}}})},
      {"isError", !tools::is_success(result)},
  };
  if (!result.output.is_null() && !result.output.empty()) {
    message["details"] = result.output;
  }
  return message;
}

std::vector<ai::Json> make_turn_tool_result_messages_json(
    const agent::TurnResult& turn) {
  std::vector<ai::Json> messages;
  const auto count = std::min(turn.tool_requests.size(), turn.tool_results.size());
  messages.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    messages.push_back(
        tool_result_message_json(turn.tool_requests[index], turn.tool_results[index]));
  }
  return messages;
}

std::vector<ai::Json> build_noninteractive_json_events(
    const agent::TurnResult& turn, const std::vector<agent::AgentEvent>& events) {
  std::vector<ai::Json> json_events;
  std::vector<ai::Json> assistant_messages;

  for (const auto& event : events) {
    switch (event.kind) {
      case agent::AgentEventKind::turn_started:
        json_events.push_back(ai::Json(AgentStartEvent{}));
        json_events.push_back(ai::Json(TurnStartEvent{}));
        break;

      case agent::AgentEventKind::provider_request:
      case agent::AgentEventKind::provider_event:
        break;

      case agent::AgentEventKind::auto_retry_started:
        json_events.push_back(auto_retry_start_event_to_json(event));
        break;

      case agent::AgentEventKind::auto_retry_completed:
        json_events.push_back(auto_retry_end_event_to_json(event));
        break;

      case agent::AgentEventKind::message_committed:
        if (!event.committed_message ||
            event.committed_message->role != ai::MessageRole::assistant) {
          break;
        }

        assistant_messages.push_back(ai::Json(*event.committed_message));
        json_events.push_back(
            ai::Json(MessageStartEvent{assistant_messages.back()}));
        for (std::size_t content_index = 0U;
             content_index < event.committed_message->content.size();
             ++content_index) {
          const auto& content = event.committed_message->content[content_index];
          switch (ai::content_kind(content)) {
            case ai::ContentKind::text:
              json_events.push_back(ai::Json(MessageUpdateEvent{
                  assistant_messages.back(),
                  ai::Json{{"type", "text_delta"},
                           {"delta", std::get<ai::TextContent>(content).text}},
              }));
              break;

            case ai::ContentKind::thinking:
              json_events.push_back(ai::Json(MessageUpdateEvent{
                  assistant_messages.back(),
                  ai::Json{{"type", "thinking_delta"},
                           {"delta",
                            std::get<ai::ThinkingContent>(content).text}},
              }));
              break;

            case ai::ContentKind::tool_call: {
              const auto& tool_call = std::get<ai::ToolCallContent>(content);
              json_events.push_back(ai::Json(MessageUpdateEvent{
                  assistant_messages.back(),
                  ai::Json{{"type", "toolcall_start"},
                           {"contentIndex", content_index}},
              }));
              json_events.push_back(ai::Json(MessageUpdateEvent{
                  assistant_messages.back(),
                  ai::Json{{"type", "toolcall_delta"},
                           {"contentIndex", content_index},
                           {"delta", tool_call.arguments.dump()}},
              }));
              json_events.push_back(ai::Json(MessageUpdateEvent{
                  assistant_messages.back(),
                  ai::Json{{"type", "toolcall_end"},
                           {"contentIndex", content_index},
                           {"toolCall", ai::Json(tool_call)}},
              }));
              break;
            }

            default:
              break;
          }
        }
        json_events.push_back(
            ai::Json(MessageEndEvent{assistant_messages.back()}));
        break;

      case agent::AgentEventKind::tool_execution_started:
        if (!event.tool_request) {
          break;
        }
        json_events.push_back(ai::Json(ToolExecutionStartEvent{
            event.tool_request->call_id.value_or(""),
            event.tool_request->tool_name,
            event.tool_request->arguments,
        }));
        break;

      case agent::AgentEventKind::tool_execution_updated:
        if (!event.tool_request || !event.tool_result) {
          break;
        }
        json_events.push_back(ai::Json(ToolExecutionUpdateEvent{
            event.tool_request->call_id.value_or(""),
            event.tool_request->tool_name,
            event.tool_request->arguments,
            tool_result_payload_json(*event.tool_result),
        }));
        break;

      case agent::AgentEventKind::tool_executed:
        if (!event.tool_request || !event.tool_result) {
          break;
        }
        json_events.push_back(ai::Json(ToolExecutionEndEvent{
            event.tool_request->call_id.value_or(event.tool_result->call_id.value_or("")),
            event.tool_request->tool_name,
            tool_result_payload_json(*event.tool_result),
            !tools::is_success(*event.tool_result),
        }));
        break;

      case agent::AgentEventKind::turn_completed: {
        TurnEndEvent turn_end;
        if (!assistant_messages.empty()) {
          turn_end.message = assistant_messages.back();
        } else if (turn.response.has_value()) {
          turn_end.message = ai::Json(turn.response->message);
        }
        turn_end.tool_results = ai::Json::array();
        for (auto& tool_result : make_turn_tool_result_messages_json(turn)) {
          turn_end.tool_results.push_back(std::move(tool_result));
        }
        json_events.push_back(ai::Json(turn_end));

        AgentEndEvent agent_end;
        agent_end.messages = ai::Json::array();
        for (auto& message : assistant_messages) {
          agent_end.messages.push_back(std::move(message));
        }
        json_events.push_back(ai::Json(agent_end));
        break;
      }

      case agent::AgentEventKind::turn_failed:
        json_events.push_back(agent_event_to_json(event));
        if (!assistant_messages.empty()) {
          AgentEndEvent agent_end;
          agent_end.messages = ai::Json::array();
          for (auto& message : assistant_messages) {
            agent_end.messages.push_back(std::move(message));
          }
          json_events.push_back(ai::Json(agent_end));
        }
        break;

      case agent::AgentEventKind::message_queued:
        break;
    }
  }

  return json_events;
}

ai::Json turn_result_to_json(const agent::TurnResult& turn) {
  ai::Json requests = ai::Json::array();
  for (const auto& request : turn.requests) {
    requests.push_back(request);
  }

  ai::Json responses = ai::Json::array();
  for (const auto& response : turn.responses) {
    responses.push_back(response);
  }

  ai::Json provider_events = ai::Json::array();
  for (const auto& event : turn.provider_events) {
    provider_events.push_back(event);
  }

  ai::Json tool_requests = ai::Json::array();
  for (const auto& request : turn.tool_requests) {
    tool_requests.push_back(request);
  }

  ai::Json tool_results = ai::Json::array();
  for (const auto& result : turn.tool_results) {
    tool_results.push_back(result);
  }

  ai::Json json{
      {"ok", turn.ok},
      {"turn_index", turn.turn_index},
      {"committed_sequences", turn.committed_sequences},
      {"requests", std::move(requests)},
      {"responses", std::move(responses)},
      {"provider_events", std::move(provider_events)},
      {"tool_requests", std::move(tool_requests)},
      {"tool_results", std::move(tool_results)},
  };

  if (turn.request) {
    json["request"] = *turn.request;
  }
  if (turn.response) {
    json["response"] = *turn.response;
  }
  if (turn.error) {
    json["error"] = *turn.error;
  }

  return json;
}

ai::Message build_noninteractive_message(
    const NoninteractivePromptRequest& request) {
  ai::Message message;
  message.role = ai::MessageRole::user;
  message.id = request.message_id;
  message.metadata = normalize_metadata(request.metadata);
  message.content = {ai::TextContent{request.text, std::nullopt}};
  for (const auto& image : request.images) {
    message.content.push_back(image);
  }
  return message;
}

class NoninteractiveEventSink final : public agent::EventSink {
 public:
  void push(const agent::AgentEvent& event) override { events.push_back(event); }

  std::vector<agent::AgentEvent> events;
};

NoninteractiveRunResult make_noninteractive_run_result(
    const agent::TurnResult& turn, const std::vector<agent::AgentEvent>& events) {
  NoninteractiveRunResult result;
  result.turn = turn;
  bool queued_message_recorded = false;

  for (const auto& event : events) {
    if (!queued_message_recorded && event.queued_message.has_value()) {
      result.queued_message = *event.queued_message;
      queued_message_recorded = true;
    }
  }
  result.json_events = build_noninteractive_json_events(result.turn, events);

  return result;
}

}  // namespace

Result<NoninteractiveRunResult> run_noninteractive_prompt(
    agent::Agent& agent, const NoninteractivePromptRequest& request) {
  if (request.text.empty()) {
    return Result<NoninteractiveRunResult>::failure(
        ErrorCode::invalid_argument, "non-interactive prompt text must not be empty");
  }

  ai::Message message = build_noninteractive_message(request);
  NoninteractiveEventSink sink;
  NoninteractiveRunResult result;
  result.queued_message = agent.enqueue(std::move(message), &sink);
  result.turn = agent.run_turn(sink);
  result.print_output = final_printable_content(result.turn);
  result.json_events = build_noninteractive_json_events(result.turn, sink.events);
  return result;
}

Result<NoninteractiveRunResult> run_noninteractive_prompt(
    harness::AgentHarness& harness, const NoninteractivePromptRequest& request) {
  if (request.text.empty()) {
    return Result<NoninteractiveRunResult>::failure(
        ErrorCode::invalid_argument, "non-interactive prompt text must not be empty");
  }

  harness.clear_events();
  const auto turn = harness.submit_prompt(build_noninteractive_message(request));
  auto result = make_noninteractive_run_result(turn, harness.events());
  result.print_output = final_printable_content(result.turn);
  return result;
}

ai::Json noninteractive_run_result_to_json(
    const NoninteractiveRunResult& result) {
  ai::Json events = ai::Json::array();
  for (const auto& event : result.json_events) {
    events.push_back(event);
  }

  const auto rendered_print =
      plan_noninteractive_output(result, NoninteractiveOutputMode::print);
  std::string stderr_output;
  for (const auto& chunk : rendered_print.stderr_chunks) {
    stderr_output += chunk;
  }

  return ai::Json{
      {"ok", result.turn.ok},
      {"exit_code", result.turn.ok ? 0 : 1},
      {"mode", "noninteractive"},
      {"queued_message", queued_message_to_json(result.queued_message)},
      {"turn", turn_result_to_json(result.turn)},
      {"print_output", result.print_output},
      {"stderr_output", stderr_output},
      {"events", std::move(events)},
  };
}

NoninteractiveRenderedOutput plan_noninteractive_output(
    const NoninteractiveRunResult& result, NoninteractiveOutputMode mode) {
  NoninteractiveRenderedOutput output;
  output.exit_code = result.turn.ok ? 0 : 1;

  switch (mode) {
    case NoninteractiveOutputMode::print:
      if (!result.turn.ok) {
        if (const auto error_output =
                format_noninteractive_error_output(result.turn)) {
          output.stderr_chunks.push_back(*error_output);
        }
      } else if (!result.print_output.empty()) {
        output.stdout_chunks.push_back(result.print_output);
      }
      break;

    case NoninteractiveOutputMode::json:
      for (const auto& event : result.json_events) {
        output.stdout_chunks.push_back(event.dump() + "\n");
      }
      break;
  }

  return output;
}

std::vector<std::string> render_noninteractive_output(
    const NoninteractiveRunResult& result, NoninteractiveOutputMode mode) {
  auto planned = plan_noninteractive_output(result, mode);
  if (!planned.stdout_chunks.empty()) {
    return planned.stdout_chunks;
  }

  // Preserve the legacy startup contract until callers adopt the stream-aware
  // output plan and route stderr separately.
  return planned.stderr_chunks;
}

}  // namespace qt::app
