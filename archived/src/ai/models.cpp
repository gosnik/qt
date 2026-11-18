#include "qt/ai/models.hpp"

#include "qt/ai/images.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace qt::ai {

namespace {

template <typename Enum>
Enum parse_enum(const std::string& value,
                const std::vector<std::pair<const char*, Enum>>& table,
                const char* type_name) {
  for (const auto& item : table) {
    if (value == item.first) {
      return item.second;
    }
  }

  throw std::invalid_argument(std::string("invalid ") + type_name + ": " + value);
}

template <typename Enum>
const char* enum_to_string(
    Enum value, const std::vector<std::pair<const char*, Enum>>& table,
    const char* type_name) {
  for (const auto& item : table) {
    if (value == item.second) {
      return item.first;
    }
  }

  throw std::invalid_argument(std::string("unmapped ") + type_name);
}

const std::vector<std::pair<const char*, MessageRole>> kMessageRoles = {
    {"system", MessageRole::system},
    {"developer", MessageRole::developer},
    {"user", MessageRole::user},
    {"assistant", MessageRole::assistant},
    {"tool", MessageRole::tool},
};

const std::vector<std::pair<const char*, StopReason>> kStopReasons = {
    {"unknown", StopReason::unknown},
    {"end_turn", StopReason::end_turn},
    {"max_tokens", StopReason::max_tokens},
    {"tool_call", StopReason::tool_call},
    {"error", StopReason::error},
    {"aborted", StopReason::aborted},
    {"content_filter", StopReason::content_filter},
};

const std::vector<std::pair<const char*, EventKind>> kEventKinds = {
    {"message_start", EventKind::message_start},
    {"content_delta", EventKind::content_delta},
    {"content_done", EventKind::content_done},
    {"tool_call", EventKind::tool_call},
    {"tool_result", EventKind::tool_result},
    {"message_done", EventKind::message_done},
    {"usage", EventKind::usage},
    {"error", EventKind::error},
};

const std::vector<std::pair<const char*, ProviderTransport>> kTransports = {
    {"auto", ProviderTransport::auto_detect},
    {"sse", ProviderTransport::sse},
    {"websocket", ProviderTransport::websocket},
    {"http", ProviderTransport::http},
};

const std::vector<std::pair<const char*, ToolChoiceKind>> kToolChoiceKinds = {
    {"auto", ToolChoiceKind::auto_select},
    {"none", ToolChoiceKind::none},
    {"required", ToolChoiceKind::required},
    {"named", ToolChoiceKind::named},
};

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool contains_any(const std::string& value,
                  const std::vector<std::string>& needles) {
  return std::any_of(needles.begin(), needles.end(),
                     [&value](const std::string& needle) {
                       return value.find(needle) != std::string::npos;
                     });
}

std::optional<std::string> response_error_message(
    const CompletionResponse& response) {
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

Json normalized_object(const Json& value) {
  return value.is_object() ? value : Json::object();
}

bool json_matches_type(const Json& value, const std::string& type) {
  if (type == "number") {
    return value.is_number();
  }
  if (type == "integer") {
    return value.is_number_integer() ||
           (value.is_number_float() && std::floor(value.get<double>()) ==
                                         value.get<double>());
  }
  if (type == "boolean") {
    return value.is_boolean();
  }
  if (type == "string") {
    return value.is_string();
  }
  if (type == "null") {
    return value.is_null();
  }
  if (type == "object") {
    return value.is_object();
  }
  if (type == "array") {
    return value.is_array();
  }
  return true;
}

std::optional<double> parse_full_number(const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(value.c_str(), &end);
  if (errno != 0 || end == value.c_str() || *end != '\0' ||
      !std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

std::optional<std::int64_t> parse_full_integer(const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  errno = 0;
  const long long parsed = std::strtoll(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(parsed);
}

std::optional<Json> coerce_json_type(const Json& value,
                                     const std::string& type) {
  if (json_matches_type(value, type)) {
    return value;
  }

  if (type == "number") {
    if (value.is_string()) {
      if (const auto parsed = parse_full_number(value.get<std::string>())) {
        return Json(*parsed);
      }
    }
    if (value.is_boolean()) {
      return Json(value.get<bool>() ? 1 : 0);
    }
    if (value.is_null()) {
      return Json(0);
    }
    return std::nullopt;
  }

  if (type == "integer") {
    if (value.is_string()) {
      if (const auto parsed = parse_full_integer(value.get<std::string>())) {
        return Json(*parsed);
      }
    }
    if (value.is_number_float()) {
      const double number = value.get<double>();
      if (std::floor(number) == number) {
        return Json(static_cast<std::int64_t>(number));
      }
    }
    if (value.is_boolean()) {
      return Json(value.get<bool>() ? 1 : 0);
    }
    if (value.is_null()) {
      return Json(0);
    }
    return std::nullopt;
  }

  if (type == "boolean") {
    if (value.is_string()) {
      const auto text = value.get<std::string>();
      if (text == "true") {
        return Json(true);
      }
      if (text == "false") {
        return Json(false);
      }
    }
    if (value.is_number_integer()) {
      const auto number = value.get<std::int64_t>();
      if (number == 1) {
        return Json(true);
      }
      if (number == 0) {
        return Json(false);
      }
    }
    return std::nullopt;
  }

  if (type == "string") {
    if (value.is_null()) {
      return Json("");
    }
    if (value.is_boolean()) {
      return Json(value.get<bool>() ? "true" : "false");
    }
    if (value.is_number()) {
      return Json(value.dump());
    }
    return std::nullopt;
  }

  if (type == "null") {
    if (value.is_string() && value.get<std::string>().empty()) {
      return Json(nullptr);
    }
    if (value.is_number_integer() && value.get<std::int64_t>() == 0) {
      return Json(nullptr);
    }
    if (value.is_boolean() && !value.get<bool>()) {
      return Json(nullptr);
    }
    return std::nullopt;
  }

  return std::nullopt;
}

std::vector<std::string> schema_types(const Json& schema) {
  std::vector<std::string> types;
  if (!schema.is_object() || !schema.contains("type")) {
    return types;
  }

  const auto& type = schema.at("type");
  if (type.is_string()) {
    types.push_back(type.get<std::string>());
  } else if (type.is_array()) {
    for (const auto& item : type) {
      if (item.is_string()) {
        types.push_back(item.get<std::string>());
      }
    }
  }
  return types;
}

std::optional<Json> coerce_json_schema_value(const Json& value,
                                             const Json& schema) {
  const auto types = schema_types(schema);
  if (types.empty()) {
    return value;
  }

  for (const auto& type : types) {
    if (json_matches_type(value, type)) {
      return value;
    }
  }
  for (const auto& type : types) {
    if (const auto coerced = coerce_json_type(value, type)) {
      return coerced;
    }
  }
  return std::nullopt;
}

std::string json_schema_type_label(const Json& schema) {
  const auto types = schema_types(schema);
  if (types.empty()) {
    return "valid value";
  }
  std::ostringstream out;
  for (std::size_t index = 0; index < types.size(); ++index) {
    if (index != 0) {
      out << " or ";
    }
    out << types[index];
  }
  return out.str();
}

std::string json_value_type_label(const Json& value) {
  if (value.is_object()) return "object";
  if (value.is_array()) return "array";
  if (value.is_string()) return "string";
  if (value.is_boolean()) return "boolean";
  if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
  if (value.is_number()) return "number";
  if (value.is_null()) return "null";
  return "value";
}

std::string schema_child_path(std::string_view path, std::string_view child) {
  if (path.empty() || path == "root") return std::string(child);
  return std::string(path) + "." + std::string(child);
}

void validate_json_schema_value(const Json& schema, Json& value,
                                std::string_view path,
                                std::vector<std::string>& errors) {
  if (!schema.is_object()) return;
  const std::string display_path = path.empty() ? "root" : std::string(path);

  const auto coerced = coerce_json_schema_value(value, schema);
  if (!coerced) {
    errors.push_back(display_path + ": expected " +
                     json_schema_type_label(schema) + ", received " +
                     json_value_type_label(value));
    return;
  }
  value = *coerced;

  if (schema.contains("enum") && schema.at("enum").is_array() &&
      std::find(schema.at("enum").begin(), schema.at("enum").end(), value) ==
          schema.at("enum").end()) {
    errors.push_back(display_path + ": value must be one of " +
                     schema.at("enum").dump());
  }

  if (value.is_object()) {
    if (schema.contains("required") && schema.at("required").is_array()) {
      for (const auto& required : schema.at("required")) {
        if (required.is_string() &&
            !value.contains(required.get<std::string>())) {
          errors.push_back(
              schema_child_path(display_path, required.get<std::string>()) +
              ": required property is missing");
        }
      }
    }
    if (schema.contains("properties") &&
        schema.at("properties").is_object()) {
      const auto& properties = schema.at("properties");
      for (auto& item : value.items()) {
        const auto property = properties.find(item.key());
        if (property != properties.end()) {
          validate_json_schema_value(
              *property, item.value(),
              schema_child_path(display_path, item.key()), errors);
        } else if (schema.value("additionalProperties", true) == false) {
          errors.push_back(schema_child_path(display_path, item.key()) +
                           ": unknown property");
        }
      }
    }
  }

  if (value.is_array()) {
    if (schema.contains("minItems") &&
        schema.at("minItems").is_number_integer() &&
        schema.at("minItems").get<std::int64_t>() >= 0 &&
        value.size() < static_cast<std::size_t>(
                           schema.at("minItems").get<std::int64_t>())) {
      errors.push_back(display_path + ": expected at least " +
                       std::to_string(
                           schema.at("minItems").get<std::int64_t>()) +
                       " items");
    }
    if (schema.contains("maxItems") &&
        schema.at("maxItems").is_number_integer() &&
        schema.at("maxItems").get<std::int64_t>() >= 0 &&
        value.size() > static_cast<std::size_t>(
                           schema.at("maxItems").get<std::int64_t>())) {
      errors.push_back(display_path + ": expected at most " +
                       std::to_string(
                           schema.at("maxItems").get<std::int64_t>()) +
                       " items");
    }
    if (schema.contains("items") && schema.at("items").is_object()) {
      for (std::size_t index = 0; index < value.size(); ++index) {
        validate_json_schema_value(
            schema.at("items"), value[index],
            display_path + "[" + std::to_string(index) + "]", errors);
      }
    }
  }

  if (value.is_string() && schema.contains("minLength") &&
      schema.at("minLength").is_number_integer() &&
      schema.at("minLength").get<std::int64_t>() >= 0 &&
      value.get_ref<const std::string&>().size() <
          static_cast<std::size_t>(
              schema.at("minLength").get<std::int64_t>())) {
    errors.push_back(display_path + ": expected at least " +
                     std::to_string(
                         schema.at("minLength").get<std::int64_t>()) +
                     " characters");
  }

  if (value.is_number()) {
    const double number = value.get<double>();
    if (schema.contains("minimum") && schema.at("minimum").is_number() &&
        number < schema.at("minimum").get<double>()) {
      errors.push_back(display_path + ": value is below minimum " +
                       schema.at("minimum").dump());
    }
    if (schema.contains("maximum") && schema.at("maximum").is_number() &&
        number > schema.at("maximum").get<double>()) {
      errors.push_back(display_path + ": value is above maximum " +
                       schema.at("maximum").dump());
    }
  }
}

std::int64_t ceil_divide(std::int64_t numerator, std::int64_t denominator) {
  if (numerator <= 0) {
    return 0;
  }
  if (denominator <= 0) {
    throw std::invalid_argument("characters_per_token must be positive");
  }
  return (numerator + denominator - 1) / denominator;
}

std::int64_t clamped_size(const std::string& value) {
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(value.size());
}

std::int64_t text_tokens(const std::string& value,
                         const TokenEstimateOptions& options) {
  return ceil_divide(clamped_size(value), options.characters_per_token);
}

std::string content_text_for_estimate(const Content& content) {
  return std::visit(
      [](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;

        if constexpr (std::is_same<T, TextContent>::value) {
          return item.text;
        } else if constexpr (std::is_same<T, ThinkingContent>::value) {
          return item.text;
        } else if constexpr (std::is_same<T, ImageContent>::value) {
          return item.media_type;
        } else if constexpr (std::is_same<T, ToolCallContent>::value) {
          return item.name + ":" + item.arguments.dump();
        } else {
          return item.name + ":" + item.result.dump();
        }
      },
      content);
}

bool is_protected_for_trimming(MessageRole role) {
  return role == MessageRole::system || role == MessageRole::developer;
}

std::optional<std::size_t> oldest_trimmable_message_index(
    const std::vector<Message>& messages) {
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (!is_protected_for_trimming(messages[index].role)) {
      return index;
    }
  }
  return std::nullopt;
}

}  // namespace

const char* to_string(MessageRole value) {
  return enum_to_string(value, kMessageRoles, "message role");
}

const char* to_string(ContentKind value) {
  switch (value) {
    case ContentKind::text:
      return "text";
    case ContentKind::thinking:
      return "thinking";
    case ContentKind::image:
      return "image";
    case ContentKind::tool_call:
      return "tool_call";
    case ContentKind::tool_result:
      return "tool_result";
  }

  throw std::invalid_argument("unmapped content kind");
}

const char* to_string(StopReason value) {
  return enum_to_string(value, kStopReasons, "stop reason");
}

const char* to_string(EventKind value) {
  return enum_to_string(value, kEventKinds, "event kind");
}

const char* to_string(ProviderTransport value) {
  return enum_to_string(value, kTransports, "provider transport");
}

const char* to_string(ToolChoiceKind value) {
  return enum_to_string(value, kToolChoiceKinds, "tool choice kind");
}

ContentKind content_kind(const Content& content) {
  return std::visit(
      [](const auto& value) -> ContentKind {
        using T = std::decay_t<decltype(value)>;

        if constexpr (std::is_same<T, TextContent>::value) {
          return ContentKind::text;
        } else if constexpr (std::is_same<T, ThinkingContent>::value) {
          return ContentKind::thinking;
        } else if constexpr (std::is_same<T, ImageContent>::value) {
          return ContentKind::image;
        } else if constexpr (std::is_same<T, ToolCallContent>::value) {
          return ContentKind::tool_call;
        } else {
          return ContentKind::tool_result;
        }
      },
      content);
}

void normalize(Usage& usage) {
  const auto computed_total = usage.input_tokens + usage.output_tokens +
                              usage.cache_read_tokens + usage.cache_write_tokens;
  usage.total_tokens = computed_total;
  usage.total_cost_usd = usage.input_cost_usd + usage.output_cost_usd +
                         usage.cache_read_cost_usd +
                         usage.cache_write_cost_usd;
}

void apply_cost(Usage& usage, const TokenCostRates& rates) {
  usage.input_cost_usd =
      static_cast<double>(usage.input_tokens) * rates.input_usd_per_million /
      1'000'000.0;
  usage.output_cost_usd =
      static_cast<double>(usage.output_tokens) * rates.output_usd_per_million /
      1'000'000.0;
  usage.cache_read_cost_usd = static_cast<double>(usage.cache_read_tokens) *
                              rates.cache_read_usd_per_million / 1'000'000.0;
  usage.cache_write_cost_usd = static_cast<double>(usage.cache_write_tokens) *
                               rates.cache_write_usd_per_million / 1'000'000.0;
  normalize(usage);
}

bool is_context_overflow(const CompletionResponse& response,
                         std::optional<std::int64_t> context_window_tokens) {
  if (response.stop_reason == StopReason::error) {
    const auto error_message = response_error_message(response);
    if (error_message) {
      const auto lower = lowercase_copy(*error_message);
      const bool non_overflow =
          lower.rfind("throttling error:", 0U) == 0U ||
          lower.rfind("service unavailable:", 0U) == 0U ||
          lower.find("rate limit") != std::string::npos ||
          lower.find("too many requests") != std::string::npos;
      if (!non_overflow &&
          contains_any(lower,
                       {"prompt is too long",
                        "request_too_large",
                        "input is too long for requested model",
                        "exceeds the context window",
                        "input token count",
                        "maximum prompt length",
                        "reduce the length of the messages",
                        "maximum context length",
                        "longer than the model's context length",
                        "longer than the models context length",
                        "exceeds the limit of",
                        "exceeds the available context size",
                        "greater than the context length",
                        "context window exceeds limit",
                        "exceeded model token limit",
                        "too large for model with",
                        "model_context_window_exceeded",
                        "prompt too long; exceeded max context length",
                        "prompt too long; exceeded context length",
                        "context_length_exceeded",
                        "context length exceeded",
                        "too many tokens",
                        "token limit exceeded",
                        "400 status code (no body)",
                        "413 status code (no body)",
                        "429 status code (no body)",
                        "400 (no body)",
                        "413 (no body)",
                        "429 (no body)"})) {
        return true;
      }
    }
  }

  if (!context_window_tokens || *context_window_tokens <= 0) {
    return false;
  }

  const auto input_tokens =
      response.usage.input_tokens + response.usage.cache_read_tokens;
  if (response.stop_reason == StopReason::end_turn &&
      input_tokens > *context_window_tokens) {
    return true;
  }
  if (response.stop_reason == StopReason::max_tokens &&
      response.usage.output_tokens == 0 &&
      input_tokens >= (*context_window_tokens * 99) / 100) {
    return true;
  }
  return false;
}

std::int64_t estimate_token_count(std::string_view text,
                                  const TokenEstimateOptions& options) {
  if (text.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return ceil_divide(static_cast<std::int64_t>(text.size()),
                     options.characters_per_token);
}

std::int64_t estimate_token_count(const Content& content,
                                  const TokenEstimateOptions& options) {
  const auto kind = content_kind(content);
  auto tokens = text_tokens(content_text_for_estimate(content), options);
  if (kind == ContentKind::image) {
    tokens += options.image_tokens;
  }
  return tokens;
}

std::int64_t estimate_token_count(const Message& message,
                                  const TokenEstimateOptions& options) {
  std::int64_t tokens = estimate_token_count(to_string(message.role), options);
  if (message.name) {
    tokens += estimate_token_count(*message.name, options);
  }
  for (const auto& content : message.content) {
    tokens += estimate_token_count(content, options);
  }
  if (!message.metadata.empty()) {
    tokens += estimate_token_count(message.metadata.dump(), options);
  }
  return tokens;
}

std::int64_t estimate_token_count(const CompletionRequest& request,
                                  const TokenEstimateOptions& options) {
  std::int64_t tokens = estimate_token_count(request.model.provider, options) +
                        estimate_token_count(request.model.api, options) +
                        estimate_token_count(request.model.name, options);
  for (const auto& message : request.messages) {
    tokens += estimate_token_count(message, options);
  }
  for (const auto& tool : request.tools) {
    tokens += estimate_token_count(tool.name, options);
    tokens += estimate_token_count(tool.description, options);
    tokens += estimate_token_count(tool.input_schema.dump(), options);
  }
  if (!request.metadata.empty()) {
    tokens += estimate_token_count(request.metadata.dump(), options);
  }
  return tokens;
}

TrimmedCompletionRequest trim_to_token_budget(
    const CompletionRequest& request, std::int64_t context_window_tokens,
    std::int64_t reserved_output_tokens,
    const TokenEstimateOptions& options) {
  if (context_window_tokens < 0 || reserved_output_tokens < 0) {
    throw std::invalid_argument("token budgets must not be negative");
  }

  TrimmedCompletionRequest result;
  result.request = normalized(request);
  result.input_budget_tokens =
      std::max<std::int64_t>(0, context_window_tokens - reserved_output_tokens);
  result.estimated_input_tokens = estimate_token_count(result.request, options);

  while (result.estimated_input_tokens > result.input_budget_tokens) {
    const auto index = oldest_trimmable_message_index(result.request.messages);
    if (!index) {
      break;
    }
    result.request.messages.erase(result.request.messages.begin() +
                                  static_cast<std::ptrdiff_t>(*index));
    ++result.removed_messages;
    result.trimmed = true;
    result.estimated_input_tokens = estimate_token_count(result.request, options);
  }

  return result;
}

TextContent normalized(const TextContent& content) { return content; }

ThinkingContent normalized(const ThinkingContent& content) { return content; }

ImageContent normalized(const ImageContent& content) {
  ImageContent result = content;
  if (const auto parsed = parse_image_data_url(content.data)) {
    result.media_type = parsed->media_type;
    result.data = parsed->data;
  }
  return result;
}

ToolCallContent normalized(const ToolCallContent& content) {
  ToolCallContent result = content;
  result.arguments = normalized_object(content.arguments);
  return result;
}

ToolResultContent normalized(const ToolResultContent& content) {
  ToolResultContent result = content;
  result.result = normalized_object(content.result);
  return result;
}

Content normalized(const Content& content) {
  return std::visit(
      [](const auto& value) -> Content { return normalized(value); }, content);
}

Message normalized(const Message& message) {
  Message result = message;
  result.metadata = normalized_object(message.metadata);
  result.content.clear();
  result.content.reserve(message.content.size());

  for (const auto& item : message.content) {
    result.content.push_back(normalized(item));
  }

  return result;
}

ToolDefinition normalized(const ToolDefinition& tool) {
  ToolDefinition result = tool;
  result.input_schema = normalized_object(tool.input_schema);
  return result;
}

ToolChoice normalized(const ToolChoice& tool_choice,
                      const std::vector<ToolDefinition>& tools) {
  ToolChoice result = tool_choice;

  switch (result.kind) {
    case ToolChoiceKind::auto_select:
    case ToolChoiceKind::none:
    case ToolChoiceKind::required:
      result.tool_name.reset();
      break;
    case ToolChoiceKind::named:
      break;
  }

  if (tools.empty()) {
    result.kind = ToolChoiceKind::none;
    result.tool_name.reset();
    return result;
  }

  if (result.kind != ToolChoiceKind::named) {
    return result;
  }

  if (!result.tool_name || result.tool_name->empty()) {
    result.kind = ToolChoiceKind::auto_select;
    result.tool_name.reset();
    return result;
  }

  const auto matches_tool = [&result](const ToolDefinition& tool) {
    return tool.name == *result.tool_name;
  };

  if (std::find_if(tools.begin(), tools.end(), matches_tool) == tools.end()) {
    result.kind = ToolChoiceKind::auto_select;
    result.tool_name.reset();
  }

  return result;
}

CompletionRequest normalized(const CompletionRequest& request) {
  CompletionRequest result = request;
  result.metadata = normalized_object(request.metadata);

  result.messages.clear();
  result.messages.reserve(request.messages.size());
  for (const auto& message : request.messages) {
    result.messages.push_back(normalized(message));
  }

  result.tools.clear();
  result.tools.reserve(request.tools.size());
  for (const auto& tool : request.tools) {
    result.tools.push_back(normalized(tool));
  }

  std::sort(result.tools.begin(), result.tools.end(),
            [](const ToolDefinition& lhs, const ToolDefinition& rhs) {
              return std::tie(lhs.name, lhs.description, lhs.input_schema) <
                     std::tie(rhs.name, rhs.description, rhs.input_schema);
            });

  result.tool_choice = normalized(request.tool_choice, result.tools);
  return result;
}

std::vector<std::string> validate(const Message& message) {
  std::vector<std::string> errors;

  if (message.content.empty()) {
    errors.emplace_back("message must contain at least one content item");
  }

  for (std::size_t index = 0; index < message.content.size(); ++index) {
    const auto& item = message.content[index];
    switch (content_kind(item)) {
      case ContentKind::text: {
        const auto& block = std::get<TextContent>(item);
        if (block.text.empty()) {
          errors.emplace_back("text content must not be empty");
        }
        break;
      }
      case ContentKind::thinking: {
        const auto& block = std::get<ThinkingContent>(item);
        if (block.text.empty() && !block.redacted) {
          errors.emplace_back("thinking content must not be empty unless redacted");
        }
        break;
      }
      case ContentKind::image: {
        const auto& block = std::get<ImageContent>(item);
        if (block.media_type.empty()) {
          errors.emplace_back("image content media_type must not be empty");
        }
        if (block.data.empty()) {
          errors.emplace_back("image content data must not be empty");
        }
        break;
      }
      case ContentKind::tool_call: {
        const auto& block = std::get<ToolCallContent>(item);
        if (block.id.empty()) {
          errors.emplace_back("tool call id must not be empty");
        }
        if (block.name.empty()) {
          errors.emplace_back("tool call name must not be empty");
        }
        if (!block.arguments.is_object()) {
          errors.emplace_back("tool call arguments must be a JSON object");
        }
        break;
      }
      case ContentKind::tool_result: {
        const auto& block = std::get<ToolResultContent>(item);
        if (block.tool_call_id.empty()) {
          errors.emplace_back("tool result tool_call_id must not be empty");
        }
        if (block.name.empty()) {
          errors.emplace_back("tool result name must not be empty");
        }
        break;
      }
    }

    (void)index;
  }

  return errors;
}

std::vector<std::string> validate(const ToolDefinition& tool) {
  std::vector<std::string> errors;

  if (tool.name.empty()) {
    errors.emplace_back("tool name must not be empty");
  }
  if (tool.description.empty()) {
    errors.emplace_back("tool description must not be empty");
  }
  if (!tool.input_schema.is_object()) {
    errors.emplace_back("tool input_schema must be a JSON object");
  }

  return errors;
}

std::vector<std::string> validate(const StreamEvent& event) {
  std::vector<std::string> errors;

  switch (event.kind) {
    case EventKind::content_delta:
      if (!event.delta || event.delta->empty()) {
        errors.emplace_back("content_delta events require a non-empty delta");
      }
      break;
    case EventKind::tool_call:
      if (!event.content ||
          content_kind(*event.content) != ContentKind::tool_call) {
        errors.emplace_back("tool_call events require tool_call content");
      }
      break;
    case EventKind::tool_result:
      if (!event.content ||
          content_kind(*event.content) != ContentKind::tool_result) {
        errors.emplace_back("tool_result events require tool_result content");
      }
      break;
    case EventKind::usage:
      if (!event.usage.has_value()) {
        errors.emplace_back("usage events require usage payload");
      }
      break;
    case EventKind::error:
      if (!event.error || event.error->empty()) {
        errors.emplace_back("error events require an error message");
      }
      break;
    default:
      break;
  }

  return errors;
}

bool is_valid(const Message& message) { return validate(message).empty(); }

bool is_valid(const ToolDefinition& tool) { return validate(tool).empty(); }

bool is_valid(const StreamEvent& event) { return validate(event).empty(); }

qt::foundation::Result<Json> validate_tool_arguments(
    const ToolDefinition& tool, const ToolCallContent& tool_call) {
  using qt::foundation::ErrorCode;
  using qt::foundation::Result;

  if (tool.name != tool_call.name) {
    return Result<Json>::failure(
        ErrorCode::not_found,
        "Tool \"" + tool_call.name + "\" not found");
  }
  if (!tool_call.arguments.is_object()) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        "Validation failed for tool \"" + tool_call.name +
            "\":\n  - root: expected object\n\nReceived arguments:\n" +
            tool_call.arguments.dump(2));
  }

  const Json& schema = tool.input_schema;
  if (!schema.is_object()) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        "Validation failed for tool \"" + tool_call.name +
            "\":\n  - root: expected JSON schema object\n\nReceived arguments:\n" +
            tool_call.arguments.dump(2));
  }

  Json result = tool_call.arguments;
  std::vector<std::string> errors;
  validate_json_schema_value(schema, result, "root", errors);
  if (!errors.empty()) {
    std::ostringstream message;
    message << "Validation failed for tool \"" << tool_call.name << "\":";
    for (const auto& error : errors) message << "\n  - " << error;
    message << "\n\nReceived arguments:\n" << tool_call.arguments.dump(2);
    return Result<Json>::failure(ErrorCode::invalid_argument, message.str());
  }
  return Result<Json>::success(std::move(result));
}

void to_json(Json& json, const MessageRole& value) { json = to_string(value); }

void from_json(const Json& json, MessageRole& value) {
  value = parse_enum(json.get<std::string>(), kMessageRoles, "message role");
}

void to_json(Json& json, const StopReason& value) { json = to_string(value); }

void from_json(const Json& json, StopReason& value) {
  value = parse_enum(json.get<std::string>(), kStopReasons, "stop reason");
}

void to_json(Json& json, const EventKind& value) { json = to_string(value); }

void from_json(const Json& json, EventKind& value) {
  value = parse_enum(json.get<std::string>(), kEventKinds, "event kind");
}

void to_json(Json& json, const ProviderTransport& value) {
  json = to_string(value);
}

void from_json(const Json& json, ProviderTransport& value) {
  value = parse_enum(json.get<std::string>(), kTransports, "provider transport");
}

void to_json(Json& json, const ToolChoiceKind& value) {
  json = to_string(value);
}

void from_json(const Json& json, ToolChoiceKind& value) {
  value = parse_enum(json.get<std::string>(), kToolChoiceKinds,
                     "tool choice kind");
}

void to_json(Json& json, const TextContent& value) {
  json = Json{{"type", "text"}, {"text", value.text}};
  if (value.signature) {
    json["signature"] = *value.signature;
  }
}

void from_json(const Json& json, TextContent& value) {
  value.text = json.at("text").get<std::string>();
  if (json.contains("signature")) {
    value.signature = json.at("signature").get<std::string>();
  } else {
    value.signature.reset();
  }
}

void to_json(Json& json, const ThinkingContent& value) {
  json = Json{{"type", "thinking"},
              {"text", value.text},
              {"redacted", value.redacted}};
  if (value.signature) {
    json["signature"] = *value.signature;
  }
}

void from_json(const Json& json, ThinkingContent& value) {
  value.text = json.value("text", std::string());
  value.redacted = json.value("redacted", false);
  if (json.contains("signature")) {
    value.signature = json.at("signature").get<std::string>();
  } else {
    value.signature.reset();
  }
}

void to_json(Json& json, const ImageContent& value) {
  json = Json{{"type", "image"},
              {"media_type", value.media_type},
              {"data", value.data}};
}

void from_json(const Json& json, ImageContent& value) {
  if (json.contains("media_type")) {
    value.media_type = json.at("media_type").get<std::string>();
  } else {
    value.media_type = json.value("mimeType", std::string());
  }
  value.data = json.at("data").get<std::string>();
  value = normalized(value);
}

void to_json(Json& json, const ToolCallContent& value) {
  json = Json{{"type", "tool_call"},
              {"id", value.id},
              {"name", value.name},
              {"arguments", value.arguments}};
  if (value.thought_signature) {
    json["thought_signature"] = *value.thought_signature;
  }
}

void from_json(const Json& json, ToolCallContent& value) {
  value.id = json.at("id").get<std::string>();
  value.name = json.at("name").get<std::string>();
  value.arguments = json.value("arguments", Json::object());
  if (json.contains("thought_signature") &&
      !json.at("thought_signature").is_null()) {
    value.thought_signature = json.at("thought_signature").get<std::string>();
  } else if (json.contains("thoughtSignature") &&
             !json.at("thoughtSignature").is_null()) {
    value.thought_signature = json.at("thoughtSignature").get<std::string>();
  } else {
    value.thought_signature.reset();
  }
}

void to_json(Json& json, const ToolResultContent& value) {
  json = Json{{"type", "tool_result"},
              {"tool_call_id", value.tool_call_id},
              {"name", value.name},
              {"result", value.result},
              {"is_error", value.is_error}};
}

void from_json(const Json& json, ToolResultContent& value) {
  value.tool_call_id = json.at("tool_call_id").get<std::string>();
  value.name = json.at("name").get<std::string>();
  value.result = json.value("result", Json::object());
  value.is_error = json.value("is_error", false);
}

void to_json(Json& json, const Content& value) {
  std::visit([&json](const auto& item) { json = item; }, value);
}

void from_json(const Json& json, Content& value) {
  const auto type = json.at("type").get<std::string>();

  if (type == "text") {
    value = json.get<TextContent>();
  } else if (type == "thinking") {
    value = json.get<ThinkingContent>();
  } else if (type == "image") {
    value = json.get<ImageContent>();
  } else if (type == "tool_call") {
    value = json.get<ToolCallContent>();
  } else if (type == "tool_result") {
    value = json.get<ToolResultContent>();
  } else {
    throw std::invalid_argument("invalid content type: " + type);
  }
}

void to_json(Json& json, const Message& value) {
  json = Json{{"role", value.role},
              {"content", value.content},
              {"metadata", value.metadata}};

  if (value.id) {
    json["id"] = *value.id;
  }
  if (value.name) {
    json["name"] = *value.name;
  }
}

void from_json(const Json& json, Message& value) {
  value.role = json.at("role").get<MessageRole>();
  value.content = json.at("content").get<std::vector<Content>>();
  value.metadata = json.value("metadata", Json::object());

  if (json.contains("id")) {
    value.id = json.at("id").get<std::string>();
  } else {
    value.id.reset();
  }
  if (json.contains("name")) {
    value.name = json.at("name").get<std::string>();
  } else {
    value.name.reset();
  }
}

void to_json(Json& json, const Usage& value) {
  json = Json{{"input_tokens", value.input_tokens},
              {"output_tokens", value.output_tokens},
              {"cache_read_tokens", value.cache_read_tokens},
              {"cache_write_tokens", value.cache_write_tokens},
              {"total_tokens", value.total_tokens},
              {"input_cost_usd", value.input_cost_usd},
              {"output_cost_usd", value.output_cost_usd},
              {"cache_read_cost_usd", value.cache_read_cost_usd},
              {"cache_write_cost_usd", value.cache_write_cost_usd},
              {"total_cost_usd", value.total_cost_usd}};
}

void from_json(const Json& json, Usage& value) {
  value.input_tokens = json.value("input_tokens", std::int64_t{0});
  value.output_tokens = json.value("output_tokens", std::int64_t{0});
  value.cache_read_tokens = json.value("cache_read_tokens", std::int64_t{0});
  value.cache_write_tokens = json.value("cache_write_tokens", std::int64_t{0});
  value.total_tokens = json.value("total_tokens", std::int64_t{0});
  value.input_cost_usd = json.value("input_cost_usd", 0.0);
  value.output_cost_usd = json.value("output_cost_usd", 0.0);
  value.cache_read_cost_usd = json.value("cache_read_cost_usd", 0.0);
  value.cache_write_cost_usd = json.value("cache_write_cost_usd", 0.0);
  value.total_cost_usd = json.value("total_cost_usd", 0.0);
  normalize(value);
}

void to_json(Json& json, const ToolDefinition& value) {
  json = Json{{"name", value.name},
              {"description", value.description},
              {"input_schema", value.input_schema}};
}

void from_json(const Json& json, ToolDefinition& value) {
  value.name = json.at("name").get<std::string>();
  value.description = json.at("description").get<std::string>();
  value.input_schema = json.value("input_schema", Json::object());
}

void to_json(Json& json, const ToolChoice& value) {
  json = Json{{"kind", value.kind}};
  if (value.tool_name) {
    json["tool_name"] = *value.tool_name;
  }
}

void from_json(const Json& json, ToolChoice& value) {
  value.kind = json.value("kind", ToolChoiceKind::auto_select);
  if (json.contains("tool_name")) {
    value.tool_name = json.at("tool_name").get<std::string>();
  } else {
    value.tool_name.reset();
  }
}

void to_json(Json& json, const ModelRef& value) {
  json = Json{{"provider", value.provider},
              {"api", value.api},
              {"name", value.name}};
}

void from_json(const Json& json, ModelRef& value) {
  value.provider = json.at("provider").get<std::string>();
  value.api = json.at("api").get<std::string>();
  value.name = json.at("name").get<std::string>();
}

void to_json(Json& json, const CompletionRequest& value) {
  json = Json{{"model", value.model},
              {"messages", value.messages},
              {"tools", value.tools},
              {"tool_choice", value.tool_choice},
              {"metadata", value.metadata}};

  if (value.temperature) {
    json["temperature"] = *value.temperature;
  }
  if (value.max_output_tokens) {
    json["max_output_tokens"] = *value.max_output_tokens;
  }
}

void from_json(const Json& json, CompletionRequest& value) {
  value.model = json.at("model").get<ModelRef>();
  value.messages = json.value("messages", std::vector<Message>{});
  value.tools = json.value("tools", std::vector<ToolDefinition>{});
  value.tool_choice = json.value("tool_choice", ToolChoice{});
  value.metadata = json.value("metadata", Json::object());

  if (json.contains("temperature")) {
    value.temperature = json.at("temperature").get<double>();
  } else {
    value.temperature.reset();
  }
  if (json.contains("max_output_tokens")) {
    value.max_output_tokens = json.at("max_output_tokens").get<std::int64_t>();
  } else {
    value.max_output_tokens.reset();
  }
}

void to_json(Json& json, const CompletionResponse& value) {
  json = Json{{"message", value.message},
              {"stop_reason", value.stop_reason},
              {"usage", value.usage},
              {"metadata", value.metadata}};
  if (value.provider_response_id) {
    json["provider_response_id"] = *value.provider_response_id;
  }
}

void from_json(const Json& json, CompletionResponse& value) {
  value.message = json.at("message").get<Message>();
  value.stop_reason = json.value("stop_reason", StopReason::unknown);
  value.usage = json.value("usage", Usage{});
  value.metadata = json.value("metadata", Json::object());
  if (json.contains("provider_response_id")) {
    value.provider_response_id = json.at("provider_response_id").get<std::string>();
  } else {
    value.provider_response_id.reset();
  }
}

void to_json(Json& json, const StreamEvent& value) {
  json = Json{{"kind", value.kind}, {"metadata", value.metadata}};

  if (value.message_id) {
    json["message_id"] = *value.message_id;
  }
  if (value.role) {
    json["role"] = *value.role;
  }
  if (value.content) {
    json["content"] = *value.content;
  }
  if (value.delta) {
    json["delta"] = *value.delta;
  }
  if (value.usage) {
    json["usage"] = *value.usage;
  }
  if (value.stop_reason) {
    json["stop_reason"] = *value.stop_reason;
  }
  if (value.error) {
    json["error"] = *value.error;
  }
}

void from_json(const Json& json, StreamEvent& value) {
  value.kind = json.at("kind").get<EventKind>();
  value.metadata = json.value("metadata", Json::object());

  if (json.contains("message_id")) {
    value.message_id = json.at("message_id").get<std::string>();
  } else {
    value.message_id.reset();
  }
  if (json.contains("role")) {
    value.role = json.at("role").get<MessageRole>();
  } else {
    value.role.reset();
  }
  if (json.contains("content")) {
    value.content = json.at("content").get<Content>();
  } else {
    value.content.reset();
  }
  if (json.contains("delta")) {
    value.delta = json.at("delta").get<std::string>();
  } else {
    value.delta.reset();
  }
  if (json.contains("usage")) {
    value.usage = json.at("usage").get<Usage>();
  } else {
    value.usage.reset();
  }
  if (json.contains("stop_reason")) {
    value.stop_reason = json.at("stop_reason").get<StopReason>();
  } else {
    value.stop_reason.reset();
  }
  if (json.contains("error")) {
    value.error = json.at("error").get<std::string>();
  } else {
    value.error.reset();
  }
}

}  // namespace qt::ai
