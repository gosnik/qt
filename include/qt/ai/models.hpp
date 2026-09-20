#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "qt/foundation/error.hpp"

namespace qt::ai {

using Json = nlohmann::json;

enum class MessageRole {
  system,
  developer,
  user,
  assistant,
  tool,
};

enum class ContentKind {
  text,
  thinking,
  image,
  tool_call,
  tool_result,
};

enum class StopReason {
  unknown,
  end_turn,
  max_tokens,
  tool_call,
  error,
  aborted,
  content_filter,
};

enum class EventKind {
  message_start,
  content_delta,
  content_done,
  tool_call,
  tool_result,
  message_done,
  usage,
  error,
};

enum class ProviderTransport {
  auto_detect,
  sse,
  websocket,
  http,
};

enum class ToolChoiceKind {
  auto_select,
  none,
  required,
  named,
};

struct TextContent {
  std::string text;
  std::optional<std::string> signature;
};

struct ThinkingContent {
  std::string text;
  std::optional<std::string> signature;
  bool redacted = false;
};

struct ImageContent {
  std::string media_type;
  std::string data;
};

struct ToolCallContent {
  ToolCallContent() = default;
  ToolCallContent(std::string id_, std::string name_, Json arguments_,
                  std::optional<std::string> thought_signature_ = std::nullopt)
      : id(std::move(id_)),
        name(std::move(name_)),
        arguments(std::move(arguments_)),
        thought_signature(std::move(thought_signature_)) {}

  std::string id;
  std::string name;
  Json arguments = Json::object();
  std::optional<std::string> thought_signature;
};

struct ToolResultContent {
  std::string tool_call_id;
  std::string name;
  Json result = Json::object();
  bool is_error = false;
};

using Content = std::variant<TextContent, ThinkingContent, ImageContent,
                             ToolCallContent, ToolResultContent>;

struct Message {
  std::optional<std::string> id;
  MessageRole role = MessageRole::user;
  std::optional<std::string> name;
  std::vector<Content> content;
  Json metadata = Json::object();
};

struct Usage {
  std::int64_t input_tokens = 0;
  std::int64_t output_tokens = 0;
  std::int64_t cache_read_tokens = 0;
  std::int64_t cache_write_tokens = 0;
  std::int64_t total_tokens = 0;
  double input_cost_usd = 0.0;
  double output_cost_usd = 0.0;
  double cache_read_cost_usd = 0.0;
  double cache_write_cost_usd = 0.0;
  double total_cost_usd = 0.0;
};

struct TokenCostRates {
  double input_usd_per_million = 0.0;
  double output_usd_per_million = 0.0;
  double cache_read_usd_per_million = 0.0;
  double cache_write_usd_per_million = 0.0;
};

struct TokenEstimateOptions {
  std::int64_t characters_per_token = 4;
  std::int64_t image_tokens = 1500;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  Json input_schema = Json::object();
};

struct ToolChoice {
  ToolChoiceKind kind = ToolChoiceKind::auto_select;
  std::optional<std::string> tool_name;
};

struct ModelRef {
  std::string provider;
  std::string api;
  std::string name;
};

struct CompletionRequest {
  ModelRef model;
  std::vector<Message> messages;
  std::vector<ToolDefinition> tools;
  ToolChoice tool_choice;
  std::optional<double> temperature;
  std::optional<std::int64_t> max_output_tokens;
  Json metadata = Json::object();
};

struct CompletionResponse {
  Message message;
  StopReason stop_reason = StopReason::unknown;
  Usage usage;
  std::optional<std::string> provider_response_id;
  std::optional<std::string> response_model;
  Json metadata = Json::object();
};

struct TrimmedCompletionRequest {
  CompletionRequest request;
  std::int64_t estimated_input_tokens = 0;
  std::int64_t input_budget_tokens = 0;
  std::size_t removed_messages = 0;
  bool trimmed = false;
};

struct StreamEvent {
  EventKind kind = EventKind::message_start;
  std::optional<std::string> message_id;
  std::optional<MessageRole> role;
  std::optional<Content> content;
  std::optional<std::string> delta;
  std::optional<Usage> usage;
  std::optional<StopReason> stop_reason;
  std::optional<std::string> error;
  Json metadata = Json::object();
};

const char* to_string(MessageRole value);
const char* to_string(ContentKind value);
const char* to_string(StopReason value);
const char* to_string(EventKind value);
const char* to_string(ProviderTransport value);
const char* to_string(ToolChoiceKind value);

ContentKind content_kind(const Content& content);

void normalize(Usage& usage);
void apply_cost(Usage& usage, const TokenCostRates& rates);
bool is_context_overflow(const CompletionResponse& response,
                         std::optional<std::int64_t> context_window_tokens =
                             std::nullopt);
std::int64_t estimate_token_count(std::string_view text,
                                  const TokenEstimateOptions& options = {});
std::int64_t estimate_token_count(const Content& content,
                                  const TokenEstimateOptions& options = {});
std::int64_t estimate_token_count(const Message& message,
                                  const TokenEstimateOptions& options = {});
std::int64_t estimate_token_count(const CompletionRequest& request,
                                  const TokenEstimateOptions& options = {});
TrimmedCompletionRequest trim_to_token_budget(
    const CompletionRequest& request, std::int64_t context_window_tokens,
    std::int64_t reserved_output_tokens,
    const TokenEstimateOptions& options = {});
TextContent normalized(const TextContent& content);
ThinkingContent normalized(const ThinkingContent& content);
ImageContent normalized(const ImageContent& content);
ToolCallContent normalized(const ToolCallContent& content);
ToolResultContent normalized(const ToolResultContent& content);
Content normalized(const Content& content);
Message normalized(const Message& message);
ToolDefinition normalized(const ToolDefinition& tool);
ToolChoice normalized(const ToolChoice& tool_choice,
                      const std::vector<ToolDefinition>& tools);
CompletionRequest normalized(const CompletionRequest& request);
bool is_valid(const Message& message);
bool is_valid(const ToolDefinition& tool);
bool is_valid(const StreamEvent& event);

std::vector<std::string> validate(const Message& message);
std::vector<std::string> validate(const ToolDefinition& tool);
std::vector<std::string> validate(const StreamEvent& event);
qt::foundation::Result<Json> validate_tool_arguments(
    const ToolDefinition& tool, const ToolCallContent& tool_call);

void to_json(Json& json, const MessageRole& value);
void from_json(const Json& json, MessageRole& value);

void to_json(Json& json, const StopReason& value);
void from_json(const Json& json, StopReason& value);

void to_json(Json& json, const EventKind& value);
void from_json(const Json& json, EventKind& value);

void to_json(Json& json, const ProviderTransport& value);
void from_json(const Json& json, ProviderTransport& value);

void to_json(Json& json, const ToolChoiceKind& value);
void from_json(const Json& json, ToolChoiceKind& value);

void to_json(Json& json, const TextContent& value);
void from_json(const Json& json, TextContent& value);

void to_json(Json& json, const ThinkingContent& value);
void from_json(const Json& json, ThinkingContent& value);

void to_json(Json& json, const ImageContent& value);
void from_json(const Json& json, ImageContent& value);

void to_json(Json& json, const ToolCallContent& value);
void from_json(const Json& json, ToolCallContent& value);

void to_json(Json& json, const ToolResultContent& value);
void from_json(const Json& json, ToolResultContent& value);

void to_json(Json& json, const Content& value);
void from_json(const Json& json, Content& value);

void to_json(Json& json, const Message& value);
void from_json(const Json& json, Message& value);

void to_json(Json& json, const Usage& value);
void from_json(const Json& json, Usage& value);

void to_json(Json& json, const ToolDefinition& value);
void from_json(const Json& json, ToolDefinition& value);

void to_json(Json& json, const ToolChoice& value);
void from_json(const Json& json, ToolChoice& value);

void to_json(Json& json, const ModelRef& value);
void from_json(const Json& json, ModelRef& value);

void to_json(Json& json, const CompletionRequest& value);
void from_json(const Json& json, CompletionRequest& value);

void to_json(Json& json, const CompletionResponse& value);
void from_json(const Json& json, CompletionResponse& value);

void to_json(Json& json, const StreamEvent& value);
void from_json(const Json& json, StreamEvent& value);

}  // namespace qt::ai
