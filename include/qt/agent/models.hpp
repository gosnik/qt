#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/models.hpp"
#include "qt/tools/models.hpp"

namespace qt::agent {

enum class QueueDisposition {
  pending,
  committed,
  cancelled,
};

enum class QueueMode {
  all,
  one_at_a_time,
};

enum class AgentEventKind {
  message_queued,
  turn_started,
  provider_request,
  provider_event,
  tool_execution_started,
  tool_execution_updated,
  tool_executed,
  subagent_event,
  message_committed,
  auto_retry_started,
  auto_retry_completed,
  turn_completed,
  turn_failed,
};

struct QueuedMessage {
  std::uint64_t sequence = 0;
  ai::Message message;
  QueueDisposition disposition = QueueDisposition::pending;
};

struct AgentState {
  std::string agent_id;
  std::uint64_t next_message_sequence = 1;
  std::uint64_t next_event_sequence = 1;
  std::uint64_t completed_turns = 0;
  std::uint64_t completed_tool_calls = 0;
  std::vector<ai::Message> transcript;
  std::vector<QueuedMessage> pending;
};

struct TurnResult {
  bool ok = false;
  std::uint64_t turn_index = 0;
  std::vector<std::uint64_t> committed_sequences;
  std::optional<ai::CompletionRequest> request;
  std::optional<ai::CompletionResponse> response;
  std::vector<ai::CompletionRequest> requests;
  std::vector<ai::CompletionResponse> responses;
  std::vector<ai::StreamEvent> provider_events;
  std::vector<tools::ToolRequest> tool_requests;
  std::vector<tools::ToolResult> tool_results;
  std::optional<std::string> error;
};

struct AgentEvent {
  std::uint64_t sequence = 0;
  AgentEventKind kind = AgentEventKind::message_queued;
  std::uint64_t turn_index = 0;
  std::optional<QueuedMessage> queued_message;
  std::optional<ai::CompletionRequest> request;
  std::optional<ai::StreamEvent> provider_event;
  std::optional<tools::ToolRequest> tool_request;
  std::optional<tools::ToolResult> tool_result;
  std::optional<ai::Json> subagent;
  std::optional<ai::Message> committed_message;
  std::optional<ai::CompletionResponse> response;
  std::optional<std::string> error;
  std::optional<std::uint32_t> retry_attempt;
  std::optional<std::uint32_t> retry_max_attempts;
  std::optional<std::int64_t> retry_delay_ms;
  std::optional<bool> retry_success;
};

const char* to_string(QueueDisposition value) noexcept;
const char* to_string(QueueMode value) noexcept;
const char* to_string(AgentEventKind value) noexcept;

bool is_retryable_agent_error(
    const ai::CompletionResponse& response,
    std::optional<std::int64_t> context_window_tokens = std::nullopt);

}  // namespace qt::agent
