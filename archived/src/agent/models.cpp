#include "qt/agent/models.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace qt::agent {

namespace {

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string compact_alnum(std::string_view value) {
  std::string compacted;
  compacted.reserve(value.size());
  for (const unsigned char ch : value) {
    if (std::isalnum(ch)) {
      compacted.push_back(static_cast<char>(std::tolower(ch)));
    }
  }
  return compacted;
}

bool contains_any(const std::string& value,
                  const std::vector<std::string>& needles) {
  return std::any_of(needles.begin(), needles.end(),
                     [&value](const std::string& needle) {
                       return value.find(needle) != std::string::npos;
                     });
}

std::optional<std::string> response_error_message(
    const ai::CompletionResponse& response) {
  if (!response.metadata.is_object()) {
    return std::nullopt;
  }
  for (const auto& key : {"errorMessage", "error_message"}) {
    if (response.metadata.contains(key) && response.metadata.at(key).is_string()) {
      return response.metadata.at(key).get<std::string>();
    }
  }
  return std::nullopt;
}

}  // namespace

const char* to_string(QueueDisposition value) noexcept {
  switch (value) {
    case QueueDisposition::pending:
      return "pending";
    case QueueDisposition::committed:
      return "committed";
    case QueueDisposition::cancelled:
      return "cancelled";
  }

  return "pending";
}

const char* to_string(QueueMode value) noexcept {
  switch (value) {
    case QueueMode::all:
      return "all";
    case QueueMode::one_at_a_time:
      return "one-at-a-time";
  }

  return "one-at-a-time";
}

const char* to_string(AgentEventKind value) noexcept {
  switch (value) {
    case AgentEventKind::message_queued:
      return "message_queued";
    case AgentEventKind::turn_started:
      return "turn_started";
    case AgentEventKind::provider_request:
      return "provider_request";
    case AgentEventKind::provider_event:
      return "provider_event";
    case AgentEventKind::tool_execution_started:
      return "tool_execution_start";
    case AgentEventKind::tool_execution_updated:
      return "tool_execution_update";
    case AgentEventKind::tool_executed:
      return "tool_executed";
    case AgentEventKind::subagent_event:
      return "subagent_event";
    case AgentEventKind::message_committed:
      return "message_committed";
    case AgentEventKind::auto_retry_started:
      return "auto_retry_start";
    case AgentEventKind::auto_retry_completed:
      return "auto_retry_end";
    case AgentEventKind::turn_completed:
      return "turn_completed";
    case AgentEventKind::turn_failed:
      return "turn_failed";
  }

  return "message_queued";
}

bool is_retryable_agent_error(
    const ai::CompletionResponse& response,
    std::optional<std::int64_t> context_window_tokens) {
  if (response.stop_reason != ai::StopReason::error) {
    return false;
  }

  const auto error_message = response_error_message(response);
  if (!error_message || error_message->empty()) {
    return false;
  }
  if (ai::is_context_overflow(response, context_window_tokens)) {
    return false;
  }

  const auto lower = lowercase_copy(*error_message);
  if (contains_any(lower, {"overloaded",
                          "too many requests",
                          "429",
                          "500",
                          "502",
                          "503",
                          "504",
                          "other side closed",
                          "fetch failed",
                          "upstream connect",
                          "reset before headers",
                          "socket hang up",
                          "ended without",
                          "http2 request did not get a response",
                          "timed out",
                          "timeout",
                          "terminated",
                          "retry delay"})) {
    return true;
  }

  const auto compacted = compact_alnum(*error_message);
  return contains_any(compacted, {"providerreturnederror",
                                  "ratelimit",
                                  "serviceunavailable",
                                  "servererror",
                                  "internalerror",
                                  "networkerror",
                                  "connectionerror",
                                  "connectionrefused",
                                  "connectionlost",
                                  "websocketclosed",
                                  "websocketerror",
                                  "http2requestdidnotgetaresponse"});
}

}  // namespace qt::agent
