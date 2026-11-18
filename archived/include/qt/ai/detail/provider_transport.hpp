#pragma once
#include "qt/ai/provider_transport.hpp"

#include <algorithm>

namespace qt::ai {
namespace detail {
namespace {

constexpr std::string_view kDefaultAzureOpenAIApiVersion = "v1";

std::optional<std::string> string_metadata_value(const Json& metadata,
                                                 const std::string& key);
std::string cache_retention_from_options_metadata(const Json& metadata);
bool supports_long_cache_retention(const Json& compat);
std::optional<std::string> requested_thinking_level(const Json& metadata);
std::optional<std::string> mapped_thinking_level(
    const std::string& level, const Json& options_metadata);
std::optional<std::string> non_empty_string_field(const Json& object,
                                                  const std::string& key);

inline std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::optional<std::string> header_value(
    const std::map<std::string, std::string>& headers, const std::string& name) {
  const std::string wanted = lowercase_copy(name);
  for (const auto& entry : headers) {
    if (lowercase_copy(entry.first) == wanted) {
      return entry.second;
    }
  }
  return std::nullopt;
}

inline bool content_type_contains(const HttpResponse& response, const std::string& needle) {
  const auto content_type = header_value(response.headers, "content-type");
  return content_type &&
         lowercase_copy(*content_type).find(lowercase_copy(needle)) !=
             std::string::npos;
}

inline std::string trim_ascii(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return std::string(value);
}

inline std::string provider_http_error_detail(const HttpResponse& response) {
  auto body = trim_ascii(response.body);
  if (body.empty()) {
    return {};
  }

  try {
    const auto parsed = Json::parse(body);
    if (parsed.is_object()) {
      if (parsed.contains("error")) {
        const auto& error = parsed.at("error");
        if (error.is_string()) {
          body = error.get<std::string>();
        } else if (error.is_object()) {
          if (const auto message = non_empty_string_field(error, "message")) {
            body = *message;
          } else if (const auto code = non_empty_string_field(error, "code")) {
            body = *code;
          }
        }
      } else if (const auto message = non_empty_string_field(parsed, "message")) {
        body = *message;
      }
    }
  } catch (const Json::exception&) {
  }

  body = trim_ascii(body);
  constexpr std::size_t kMaxErrorBody = 2000;
  if (body.size() > kMaxErrorBody) {
    body.resize(kMaxErrorBody);
    body += "...";
  }
  return body;
}

std::optional<std::string> non_empty_string_field(const Json& object,
                                                  const std::string& key) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_string()) {
    return std::nullopt;
  }
  auto value = trim_ascii(object.at(key).get<std::string>());
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

void retain_routed_response_model(CompletionResponse& response,
                                  const std::string& requested_model) {
  if (response.response_model && *response.response_model == requested_model) {
    response.response_model.reset();
  }
}

void retain_routed_response_model(std::vector<StreamEvent>& events,
                                  const std::string& requested_model) {
  for (auto& event : events) {
    const auto model = non_empty_string_field(event.metadata, "response_model");
    if (model && *model == requested_model) {
      event.metadata.erase("response_model");
    }
  }
}

inline void append_utf8_replacement(std::string& value) {
  value += "\xEF\xBF\xBD";
}

inline bool is_utf8_continuation(unsigned char ch) {
  return (ch & 0xC0U) == 0x80U;
}

inline std::string provider_safe_text(std::string_view value) {
  std::string result;
  result.reserve(value.size());

  for (std::size_t i = 0; i < value.size();) {
    const auto c0 = static_cast<unsigned char>(value[i]);
    if (c0 < 0x80U) {
      result.push_back(static_cast<char>(c0));
      ++i;
      continue;
    }

    std::size_t length = 0;
    bool valid = false;
    if (c0 >= 0xC2U && c0 <= 0xDFU) {
      length = 2;
      valid = i + 1 < value.size() &&
              is_utf8_continuation(static_cast<unsigned char>(value[i + 1]));
    } else if (c0 >= 0xE0U && c0 <= 0xEFU) {
      length = 3;
      if (i + 2 < value.size()) {
        const auto c1 = static_cast<unsigned char>(value[i + 1]);
        const auto c2 = static_cast<unsigned char>(value[i + 2]);
        valid = is_utf8_continuation(c1) && is_utf8_continuation(c2) &&
                (c0 != 0xE0U || c1 >= 0xA0U) &&
                (c0 != 0xEDU || c1 < 0xA0U);
      }
    } else if (c0 >= 0xF0U && c0 <= 0xF4U) {
      length = 4;
      if (i + 3 < value.size()) {
        const auto c1 = static_cast<unsigned char>(value[i + 1]);
        const auto c2 = static_cast<unsigned char>(value[i + 2]);
        const auto c3 = static_cast<unsigned char>(value[i + 3]);
        valid = is_utf8_continuation(c1) && is_utf8_continuation(c2) &&
                is_utf8_continuation(c3) &&
                (c0 != 0xF0U || c1 >= 0x90U) &&
                (c0 != 0xF4U || c1 <= 0x8FU);
      }
    }

    if (valid) {
      result.append(value.substr(i, length));
      i += length;
    } else {
      append_utf8_replacement(result);
      bool consumed_invalid_sequence = false;
      if (length > 1 && i + length <= value.size()) {
        consumed_invalid_sequence = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
          if (!is_utf8_continuation(
                  static_cast<unsigned char>(value[i + offset]))) {
            consumed_invalid_sequence = false;
            break;
          }
        }
      }
      i += consumed_invalid_sequence ? length : 1;
    }
  }

  return result;
}

inline std::string provider_safe_json_dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

inline std::optional<std::string> env_string(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  const auto trimmed = trim_ascii(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return trimmed;
}

inline bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

struct ParsedUrl {
  std::string scheme;
  std::string authority;
  std::string host;
  std::string path;
  std::string suffix;
};

ParsedUrl parse_absolute_url(const std::string& value,
                             const std::string& error_prefix) {
  const auto scheme_end = value.find("://");
  if (scheme_end == std::string::npos || scheme_end == 0) {
    throw std::runtime_error(error_prefix + value);
  }

  const auto authority_start = scheme_end + 3;
  const auto path_start = value.find_first_of("/?#", authority_start);
  const auto authority =
      value.substr(authority_start, path_start - authority_start);
  if (authority.empty()) {
    throw std::runtime_error(error_prefix + value);
  }

  std::string host = authority;
  const auto at = host.rfind('@');
  if (at != std::string::npos) {
    host = host.substr(at + 1);
  }
  if (!host.empty() && host.front() == '[') {
    const auto bracket = host.find(']');
    if (bracket != std::string::npos) {
      host = host.substr(1, bracket - 1);
    }
  } else {
    const auto colon = host.find(':');
    if (colon != std::string::npos) {
      host = host.substr(0, colon);
    }
  }
  host = lowercase_copy(host);
  if (host.empty()) {
    throw std::runtime_error(error_prefix + value);
  }

  ParsedUrl parsed;
  parsed.scheme = value.substr(0, scheme_end);
  parsed.authority = authority;
  parsed.host = host;
  if (path_start == std::string::npos) {
    parsed.path = "";
    parsed.suffix = "";
  } else if (value[path_start] == '/') {
    const auto suffix_start = value.find_first_of("?#", path_start);
    parsed.path = value.substr(path_start, suffix_start - path_start);
    parsed.suffix =
        suffix_start == std::string::npos ? "" : value.substr(suffix_start);
  } else {
    parsed.path = "";
    parsed.suffix = value.substr(path_start);
  }
  return parsed;
}

inline std::string format_url(const ParsedUrl& url) {
  return url.scheme + "://" + url.authority + url.path + url.suffix;
}

inline void strip_trailing_slashes(std::string& value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
}

inline std::string normalize_azure_openai_base_url(const std::string& base_url) {
  auto trimmed = trim_ascii(base_url);
  strip_trailing_slashes(trimmed);
  auto parsed =
      parse_absolute_url(trimmed, "Invalid Azure OpenAI base URL: ");
  const bool is_azure_host =
      ends_with(parsed.host, ".openai.azure.com") ||
      ends_with(parsed.host, ".cognitiveservices.azure.com");

  auto normalized_path = parsed.path;
  strip_trailing_slashes(normalized_path);
  if (is_azure_host &&
      (normalized_path.empty() || normalized_path == "/" ||
       normalized_path == "/openai")) {
    parsed.path = "/openai/v1";
    parsed.suffix.clear();
  }

  auto result = format_url(parsed);
  strip_trailing_slashes(result);
  return result;
}

inline std::string azure_default_base_url(const std::string& resource_name) {
  return "https://" + resource_name + ".openai.azure.com/openai/v1";
}

std::string append_path_before_url_suffix(const std::string& base_url,
                                          std::string_view segment) {
  auto parsed =
      parse_absolute_url(base_url, "Invalid Azure OpenAI base URL: ");
  strip_trailing_slashes(parsed.path);
  if (parsed.path.empty()) {
    parsed.path = "/";
  }
  if (parsed.path.back() != '/') {
    parsed.path += "/";
  }
  parsed.path += segment;
  return format_url(parsed);
}

inline std::string azure_openai_api_version(const ProviderOptions& options) {
  if (const auto value = string_metadata_value(options.metadata,
                                              "azureApiVersion")) {
    return trim_ascii(*value);
  }
  if (const auto value = string_metadata_value(options.metadata, "apiVersion")) {
    return trim_ascii(*value);
  }
  if (const auto value = env_string("AZURE_OPENAI_API_VERSION")) {
    return *value;
  }
  return std::string(kDefaultAzureOpenAIApiVersion);
}

std::string azure_openai_base_url(const ProviderOptions& options,
                                  const std::string& constructor_base_url) {
  std::optional<std::string> base_url;
  if (const auto value = string_metadata_value(options.metadata,
                                              "azureBaseUrl")) {
    base_url = trim_ascii(*value);
  } else if (const auto value = string_metadata_value(options.metadata,
                                                     "baseUrl")) {
    base_url = trim_ascii(*value);
  } else if (const auto value = env_string("AZURE_OPENAI_BASE_URL")) {
    base_url = *value;
  }

  if (base_url && !base_url->empty()) {
    return normalize_azure_openai_base_url(*base_url);
  }

  if (const auto resource_name =
          string_metadata_value(options.metadata, "azureResourceName")) {
    const auto trimmed = trim_ascii(*resource_name);
    if (!trimmed.empty()) {
      return azure_default_base_url(trimmed);
    }
  }
  if (const auto resource_name = env_string("AZURE_OPENAI_RESOURCE_NAME")) {
    return azure_default_base_url(*resource_name);
  }
  if (!constructor_base_url.empty()) {
    return normalize_azure_openai_base_url(constructor_base_url);
  }
  throw std::runtime_error(
      "Azure OpenAI base URL is required. Set AZURE_OPENAI_BASE_URL or "
      "AZURE_OPENAI_RESOURCE_NAME, or pass azureBaseUrl, azureResourceName, "
      "baseUrl metadata, or a provider base URL.");
}

std::string azure_openai_responses_endpoint(const ProviderOptions& options,
                                            const std::string& base_url) {
  const auto normalized_base_url = azure_openai_base_url(options, base_url);
  const auto endpoint =
      append_path_before_url_suffix(normalized_base_url, "responses");
  const auto api_version = azure_openai_api_version(options);
  if (api_version.empty()) {
    return endpoint;
  }
  return endpoint + (endpoint.find('?') == std::string::npos ? "?" : "&") +
         "api-version=" + api_version;
}

std::string azure_openai_deployment_name(const CompletionRequest& request,
                                         const ProviderOptions& options) {
  if (const auto value = string_metadata_value(options.metadata,
                                              "azureDeploymentName")) {
    const auto trimmed = trim_ascii(*value);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  if (const auto value =
          string_metadata_value(options.metadata, "deploymentName")) {
    const auto trimmed = trim_ascii(*value);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  return request.model.name;
}

std::string sanitize_tool_call_id_part(std::string_view value,
                                       std::size_t max_size) {
  std::string result;
  result.reserve(std::min(value.size(), max_size));
  for (const unsigned char ch : value) {
    if (result.size() >= max_size) {
      break;
    }
    if (std::isalnum(ch) || ch == '_' || ch == '-') {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('_');
    }
  }
  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  return result;
}

inline std::string first_pipe_component(std::string_view value) {
  const auto pipe = value.find('|');
  if (pipe == std::string_view::npos) {
    return std::string(value);
  }
  return std::string(value.substr(0, pipe));
}

inline std::string openai_completions_tool_call_id(std::string_view id) {
  if (id.find('|') == std::string_view::npos) {
    return std::string(id);
  }
  return sanitize_tool_call_id_part(first_pipe_component(id), 40);
}

inline std::string openai_responses_call_id(std::string_view id) {
  return sanitize_tool_call_id_part(first_pipe_component(id), 64);
}

inline std::optional<int> parse_http_status_line(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  if (line.rfind("HTTP/", 0) != 0) {
    return std::nullopt;
  }

  const auto first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return std::nullopt;
  }
  const auto status_start = first_space + 1;
  const auto status_end = line.find(' ', status_start);
  const auto status = line.substr(status_start, status_end - status_start);
  try {
    return std::stoi(std::string(status));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::int64_t> parse_retry_after_http_date_ms(
    const std::string& value) {
  std::tm parsed {};
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  input >> std::get_time(&parsed, "%a, %d %b %Y %H:%M:%S GMT");
  if (input.fail()) {
    return std::nullopt;
  }

  const std::time_t retry_at = timegm(&parsed);
  if (retry_at == static_cast<std::time_t>(-1)) {
    return std::nullopt;
  }

  const std::time_t now = std::time(nullptr);
  if (retry_at <= now) {
    return std::int64_t{0};
  }
  return static_cast<std::int64_t>(retry_at - now) * 1000;
}

inline std::string timeout_seconds(std::int64_t timeout_ms) {
  const auto clamped = std::max<std::int64_t>(1, timeout_ms);
  std::ostringstream output;
  output << std::fixed << std::setprecision(3)
         << (static_cast<double>(clamped) / 1000.0);
  return output.str();
}

inline void throw_errno(const std::string& action) {
  throw std::runtime_error(action + ": " + std::strerror(errno));
}

inline void close_if_open(int& fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

class ScopedSignalIgnore {
 public:
  inline explicit ScopedSignalIgnore(int signal) : signal_(signal) {
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    sigaction(signal_, &action, &previous_);
  }

  ~ScopedSignalIgnore() { sigaction(signal_, &previous_, nullptr); }

 private:
  int signal_;
  struct sigaction previous_ {};
};

inline bool write_some(int fd, std::string_view& remaining) {
  const auto written = write(fd, remaining.data(), remaining.size());
  if (written > 0) {
    remaining.remove_prefix(static_cast<std::size_t>(written));
    return true;
  }
  if (written == 0 || errno == EAGAIN || errno == EWOULDBLOCK ||
      errno == EINTR) {
    return false;
  }
  if (errno == EPIPE) {
    remaining = {};
    return false;
  }
  throw_errno("write command stdin");
  return false;
}

inline bool read_some(int fd, std::string& output) {
  char buffer[4096];
  const auto read_count = read(fd, buffer, sizeof(buffer));
  if (read_count > 0) {
    output.append(buffer, static_cast<std::size_t>(read_count));
    return true;
  }
  if (read_count == 0) {
    return false;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return true;
  }
  throw_errno("read command output");
  return false;
}

inline void dispatch_sse_event(std::vector<SseEvent>& events, SseEvent& current) {
  if (current.data.empty()) {
    current = SseEvent{};
    return;
  }

  if (!current.data.empty() && current.data.back() == '\n') {
    current.data.pop_back();
  }
  events.push_back(current);
  current = SseEvent{};
}

inline void append_content_from_openai_message(StreamEvent& event, const Json& item) {
  if (!item.is_object() || item.value("type", std::string()) != "message") {
    return;
  }

  event.kind = EventKind::content_done;
  if (item.contains("id") && item.at("id").is_string()) {
    event.message_id = item.at("id").get<std::string>();
  }

  const auto content = item.value("content", Json::array());
  std::string text;
  for (const auto& part : content) {
    if (!part.is_object()) {
      continue;
    }
    const auto type = part.value("type", std::string());
    if ((type == "output_text" || type == "text") && part.contains("text") &&
        part.at("text").is_string()) {
      text += part.at("text").get<std::string>();
    }
  }

  if (!text.empty()) {
    event.content = TextContent{text, std::nullopt};
  }
}

inline std::optional<ToolCallContent> tool_call_from_openai_item(const Json& item) {
  if (!item.is_object() || item.value("type", std::string()) != "function_call") {
    return std::nullopt;
  }

  ToolCallContent call;
  call.id = item.value("call_id", item.value("id", std::string()));
  call.name = item.value("name", std::string());

  const auto arguments = item.value("arguments", std::string("{}"));
  try {
    call.arguments = Json::parse(arguments);
  } catch (const Json::parse_error&) {
    call.arguments = Json{{"raw", arguments}};
  }

  if (call.id.empty() || call.name.empty()) {
    return std::nullopt;
  }
  return call;
}

inline Json parse_streamed_tool_arguments_or_raw(std::string_view arguments) {
  const auto text =
      arguments.empty() ? std::string("{}") : std::string(arguments);
  try {
    return Json::parse(text);
  } catch (const Json::parse_error&) {
    const auto repaired = parse_streaming_json(text);
    if (!(repaired.is_object() && repaired.empty())) {
      return repaired;
    }
    return Json{{"raw", text}};
  }
}

inline std::string openai_reasoning_text(const Json& item) {
  if (!item.is_object() || item.value("type", std::string()) != "reasoning") {
    return "";
  }

  const auto collect_text = [](const Json& parts) {
    std::string text;
    if (!parts.is_array()) {
      return text;
    }
    for (const auto& part : parts) {
      if (!part.is_object()) {
        continue;
      }
      if (part.contains("text") && part.at("text").is_string()) {
        if (!text.empty()) {
          text += "\n\n";
        }
        text += part.at("text").get<std::string>();
      }
    }
    return text;
  };

  auto text = collect_text(item.value("summary", Json::array()));
  if (!text.empty()) {
    return text;
  }
  return collect_text(item.value("content", Json::array()));
}

inline StopReason openai_stop_reason(const Json& response) {
  const auto status = response.value("status", std::string());
  if (status == "incomplete") {
    const auto details = response.value("incomplete_details", Json::object());
    if (details.value("reason", std::string()) == "max_output_tokens") {
      return StopReason::max_tokens;
    }
    return StopReason::unknown;
  }
  if (status == "cancelled") {
    return StopReason::aborted;
  }
  if (status == "failed") {
    return StopReason::error;
  }
  return StopReason::end_turn;
}

inline StopReason openai_stop_reason(const Json& response, bool saw_tool_call) {
  if (saw_tool_call) {
    return StopReason::tool_call;
  }
  return openai_stop_reason(response);
}

inline StopReason openai_completions_stop_reason(const std::string& finish_reason) {
  if (finish_reason == "stop") {
    return StopReason::end_turn;
  }
  if (finish_reason == "length") {
    return StopReason::max_tokens;
  }
  if (finish_reason == "tool_calls" || finish_reason == "function_call") {
    return StopReason::tool_call;
  }
  if (finish_reason == "content_filter") {
    return StopReason::content_filter;
  }
  if (finish_reason.empty()) {
    return StopReason::unknown;
  }
  return StopReason::error;
}

inline Usage openai_usage(const Json& response) {
  Usage usage;
  const auto source = response.value("usage", Json::object());
  usage.input_tokens = source.value("input_tokens", std::int64_t{0});
  usage.output_tokens = source.value("output_tokens", std::int64_t{0});
  usage.total_tokens = source.value("total_tokens", std::int64_t{0});
  const auto input_details =
      source.value("input_tokens_details", Json::object());
  usage.cache_read_tokens =
      input_details.value("cached_tokens", std::int64_t{0});
  normalize(usage);
  return usage;
}

inline Usage openai_completions_usage(const Json& source) {
  Usage usage;
  const auto prompt_tokens = source.value("prompt_tokens", std::int64_t{0});
  usage.output_tokens = source.value("completion_tokens", std::int64_t{0});
  const auto prompt_details =
      source.value("prompt_tokens_details", Json::object());
  const auto reported_cached_tokens =
      prompt_details.contains("cached_tokens")
          ? prompt_details.value("cached_tokens", std::int64_t{0})
          : source.value("prompt_cache_hit_tokens", std::int64_t{0});
  usage.cache_write_tokens =
      prompt_details.value("cache_write_tokens", std::int64_t{0});
  usage.cache_read_tokens =
      usage.cache_write_tokens > 0
          ? std::max<std::int64_t>(0, reported_cached_tokens -
                                          usage.cache_write_tokens)
          : reported_cached_tokens;
  usage.input_tokens = std::max<std::int64_t>(
      0, prompt_tokens - usage.cache_read_tokens - usage.cache_write_tokens);
  normalize(usage);
  return usage;
}

struct ToolResultPayload {
  std::string text;
  std::vector<ImageContent> images;
  bool used_content_blocks = false;
};

inline ToolResultPayload tool_result_payload(const ToolResultContent& result) {
  ToolResultPayload payload;
  const auto content = result.result.find("content");
  if (content == result.result.end() || !content->is_array()) {
    payload.text = provider_safe_json_dump(result.result);
    return payload;
  }

  payload.used_content_blocks = true;
  for (const auto& block : *content) {
    if (!block.is_object()) {
      continue;
    }
    const auto type = block.value("type", std::string());
    if (type == "text" && block.contains("text") &&
        block.at("text").is_string()) {
      if (!payload.text.empty()) {
        payload.text += "\n";
      }
      payload.text += provider_safe_text(block.at("text").get<std::string>());
    } else if (type == "image" && block.contains("data") &&
               block.at("data").is_string()) {
      ImageContent image;
      if (block.contains("media_type") && block.at("media_type").is_string()) {
        image.media_type = block.at("media_type").get<std::string>();
      } else {
        image.media_type = block.value("mimeType", std::string());
      }
      image.data = block.at("data").get<std::string>();
      payload.images.push_back(normalized(image));
    }
  }

  if (payload.text.empty() && payload.images.empty()) {
    payload.text = provider_safe_json_dump(result.result);
    payload.used_content_blocks = false;
  }
  return payload;
}

inline Json openai_tool_result_output(const ToolResultContent& result) {
  const auto payload = tool_result_payload(result);
  if (payload.images.empty()) {
    return payload.text;
  }

  Json output = Json::array();
  if (!payload.text.empty()) {
    output.push_back(Json{{"type", "input_text"}, {"text", payload.text}});
  }
  for (const auto& image : payload.images) {
    output.push_back(Json{{"type", "input_image"},
                          {"detail", "auto"},
                          {"image_url", image_data_url(image)}});
  }
  return output;
}

Json openai_completions_tool_result_message(
    const ToolResultContent& result, const ToolResultPayload& payload) {
  std::string text = payload.text;
  if (payload.used_content_blocks && text.empty() && !payload.images.empty()) {
    text = "(see attached image)";
  }

  return Json{{"role", "tool"},
              {"tool_call_id",
               openai_completions_tool_call_id(result.tool_call_id)},
              {"content", text}};
}

inline Json openai_completions_tool_result_image_part(const ImageContent& image) {
  return Json{{"type", "image_url"},
              {"image_url", {{"url", image_data_url(image)}}}};
}

inline bool request_has_tool_history(const CompletionRequest& request) {
  for (const auto& message : request.messages) {
    for (const auto& content : message.content) {
      const auto kind = content_kind(content);
      if (kind == ContentKind::tool_call || kind == ContentKind::tool_result) {
        return true;
      }
    }
  }
  return false;
}

inline Json openai_content_part(const Content& content, MessageRole role) {
  return std::visit(
      [role](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;

        inline if constexpr (std::is_same<T, TextContent>::value) {
          return Json{{"type", role == MessageRole::assistant ? "output_text"
                                                              : "input_text"},
                      {"text", provider_safe_text(item.text)}};
        } else if constexpr (std::is_same<T, ImageContent>::value) {
          return Json{{"type", "input_image"},
                      {"image_url", image_data_url(item)}};
        } else {
          throw std::invalid_argument(
              "tool call content must be converted as a top-level input item");
        }
      },
      content);
}

inline Json openai_completions_content_part(const Content& content) {
  return std::visit(
      [](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;

        inline if constexpr (std::is_same<T, TextContent>::value) {
          return Json{{"type", "text"},
                      {"text", provider_safe_text(item.text)}};
        } else if constexpr (std::is_same<T, ImageContent>::value) {
          return Json{{"type", "image_url"},
                      {"image_url", {{"url", image_data_url(item)}}}};
        } else {
          throw std::invalid_argument(
              "tool content must be converted as a chat message field");
        }
      },
      content);
}

inline Json openai_input_item(const Message& message) {
  Json item = Json{{"type", "message"}, {"role", to_string(message.role)}};
  if (message.id) {
    item["id"] = *message.id;
  }

  item["content"] = Json::array();
  for (const auto& content : message.content) {
    if (content_kind(content) == ContentKind::tool_call ||
        content_kind(content) == ContentKind::tool_result) {
      continue;
    }
    item["content"].push_back(openai_content_part(content, message.role));
  }

  return item;
}

inline Json openai_completions_message(const Message& message) {
  Json item = Json{{"role", to_string(message.role)}};
  if (message.name) {
    item["name"] = *message.name;
  }

  std::vector<ToolCallContent> tool_calls;
  std::vector<Json> parts;
  std::string text;
  for (const auto& content : message.content) {
    switch (content_kind(content)) {
      case ContentKind::text:
        text += provider_safe_text(std::get<TextContent>(content).text);
        parts.push_back(openai_completions_content_part(content));
        break;
      case ContentKind::image:
        parts.push_back(openai_completions_content_part(content));
        break;
      case ContentKind::thinking: {
        const auto& thinking = std::get<ThinkingContent>(content);
        if (!thinking.text.empty()) {
          const auto safe_text = provider_safe_text(thinking.text);
          text += safe_text;
          parts.push_back(Json{{"type", "text"}, {"text", safe_text}});
        }
        break;
      }
      case ContentKind::tool_call:
        tool_calls.push_back(std::get<ToolCallContent>(content));
        break;
      case ContentKind::tool_result: {
        const auto& result = std::get<ToolResultContent>(content);
        return openai_completions_tool_result_message(
            result, tool_result_payload(result));
      }
    }
  }

  if (!tool_calls.empty()) {
    item["tool_calls"] = Json::array();
    for (const auto& call : tool_calls) {
      item["tool_calls"].push_back(
          Json{{"id", openai_completions_tool_call_id(call.id)},
               {"type", "function"},
               {"function",
                {{"name", call.name},
                 {"arguments", provider_safe_json_dump(call.arguments)}}}});
    }
    item["content"] = text.empty() ? Json(nullptr) : Json(text);
    return item;
  }

  if (parts.empty()) {
    item["content"] = "";
  } else if (message.role == MessageRole::user && parts.size() > 1) {
    item["content"] = parts;
  } else if (parts.size() == 1 && content_kind(message.content.front()) == ContentKind::text) {
    item["content"] = text;
  } else {
    item["content"] = parts;
  }
  return item;
}

inline Json openai_tool(const ToolDefinition& tool) {
  return Json{{"type", "function"},
              {"name", tool.name},
              {"description", tool.description},
              {"parameters", tool.input_schema}};
}

inline Json openai_completions_tool(const ToolDefinition& tool,
                                    const Json& compat) {
  Json function = Json{{"name", tool.name},
                       {"description", tool.description},
                       {"parameters", tool.input_schema}};
  if (compat.value("supportsStrictMode", true)) {
    function["strict"] = false;
  }
  return Json{{"type", "function"}, {"function", std::move(function)}};
}

inline bool anthropic_supports_eager_tool_input_streaming(const Json& metadata) {
  if (!metadata.is_object()) {
    return true;
  }
  const auto compat = metadata.value("compat", Json::object());
  if (!compat.is_object()) {
    return true;
  }
  return compat.value("supportsEagerToolInputStreaming", true);
}

inline bool anthropic_uses_claude_code_oauth(const Json& metadata) {
  return metadata.is_object() && metadata.value("anthropicOAuth", false);
}

inline bool anthropic_supports_cache_control_on_tools(const Json& metadata) {
  if (!metadata.is_object()) {
    return true;
  }
  const auto compat = metadata.value("compat", Json::object());
  if (!compat.is_object()) {
    return true;
  }
  return compat.value("supportsCacheControlOnTools", true);
}

inline bool compat_sends_session_affinity_headers(const Json& metadata) {
  if (!metadata.is_object()) {
    return false;
  }
  const auto compat = metadata.value("compat", Json::object());
  if (!compat.is_object()) {
    return false;
  }
  return compat.value("sendSessionAffinityHeaders", false);
}

inline std::string anthropic_claude_code_tool_name(const std::string& name) {
  static const std::vector<std::string> canonical_names = {
      "Read",          "Write",       "Edit",       "Bash",
      "Grep",          "Glob",        "AskUserQuestion",
      "EnterPlanMode", "ExitPlanMode", "KillShell", "NotebookEdit",
      "Skill",         "Task",        "TaskOutput", "TodoWrite",
      "WebFetch",      "WebSearch"};
  const std::string lower = lowercase_copy(name);
  for (const auto& candidate : canonical_names) {
    if (lowercase_copy(candidate) == lower) {
      return candidate;
    }
  }
  return name;
}

inline Json anthropic_text_content(const std::vector<Content>& content) {
  std::string text;
  for (const auto& item : content) {
    if (content_kind(item) != ContentKind::text) {
      continue;
    }
    if (!text.empty()) {
      text += "\n";
    }
    text += provider_safe_text(std::get<TextContent>(item).text);
  }
  return text;
}

inline Json anthropic_cache_control(const Json& metadata) {
  if (!metadata.is_object() || !metadata.contains("cacheRetention")) {
    return Json(nullptr);
  }
  const auto retention = cache_retention_from_options_metadata(metadata);
  if (retention == "none") {
    return Json(nullptr);
  }

  const auto compat = metadata.is_object()
                          ? metadata.value("compat", Json::object())
                          : Json::object();
  Json cache_control = Json{{"type", "ephemeral"}};
  if (retention == "long" && supports_long_cache_retention(compat)) {
    cache_control["ttl"] = "1h";
  }
  return cache_control;
}

inline Json anthropic_image_block(const ImageContent& image) {
  const auto normalized_image = normalized(image);
  return Json{{"type", "image"},
              {"source",
               {{"type", "base64"},
                {"media_type", normalized_image.media_type},
                {"data", normalized_image.data}}}};
}

inline Json anthropic_user_content_blocks(const std::vector<Content>& content) {
  Json blocks = Json::array();
  for (const auto& item : content) {
    switch (content_kind(item)) {
      case ContentKind::text: {
        const auto text = provider_safe_text(std::get<TextContent>(item).text);
        if (!trim_ascii(text).empty()) {
          blocks.push_back(Json{{"type", "text"}, {"text", text}});
        }
        break;
      }
      case ContentKind::image:
        blocks.push_back(anthropic_image_block(std::get<ImageContent>(item)));
        break;
      case ContentKind::thinking:
      case ContentKind::tool_call:
      case ContentKind::tool_result:
        break;
    }
  }
  return blocks;
}

inline Json anthropic_user_content(const std::vector<Content>& content) {
  bool has_image = false;
  for (const auto& item : content) {
    if (content_kind(item) == ContentKind::image) {
      has_image = true;
      break;
    }
  }
  if (!has_image) {
    return anthropic_text_content(content);
  }

  auto blocks = anthropic_user_content_blocks(content);
  const bool has_text =
      std::any_of(blocks.begin(), blocks.end(), [](const Json& block) {
        return block.is_object() && block.value("type", std::string()) == "text";
      });
  if (!has_text) {
    blocks.insert(blocks.begin(),
                  Json{{"type", "text"}, {"text", "(see attached image)"}});
  }
  return blocks;
}

Json anthropic_assistant_content(const std::vector<Content>& content,
                                 bool claude_code_oauth) {
  Json blocks = Json::array();
  for (const auto& item : content) {
    switch (content_kind(item)) {
      case ContentKind::text: {
        const auto text = provider_safe_text(std::get<TextContent>(item).text);
        if (!trim_ascii(text).empty()) {
          blocks.push_back(Json{{"type", "text"}, {"text", text}});
        }
        break;
      }
      case ContentKind::thinking: {
        const auto& thinking = std::get<ThinkingContent>(item);
        const auto text = provider_safe_text(thinking.text);
        if (thinking.redacted && thinking.signature) {
          blocks.push_back(
              Json{{"type", "redacted_thinking"}, {"data", *thinking.signature}});
        } else if (!trim_ascii(text).empty() && thinking.signature &&
                   !trim_ascii(*thinking.signature).empty()) {
          blocks.push_back(Json{{"type", "thinking"},
                                {"thinking", text},
                                {"signature", *thinking.signature}});
        } else if (!trim_ascii(text).empty()) {
          blocks.push_back(Json{{"type", "text"}, {"text", text}});
        }
        break;
      }
      case ContentKind::tool_call: {
        const auto& call = std::get<ToolCallContent>(item);
        blocks.push_back(
            Json{{"type", "tool_use"},
                 {"id", call.id},
                 {"name", claude_code_oauth
                              ? anthropic_claude_code_tool_name(call.name)
                              : call.name},
                 {"input", call.arguments.is_null() ? Json::object()
                                                     : call.arguments}});
        break;
      }
      case ContentKind::image:
      case ContentKind::tool_result:
        break;
    }
  }
  return blocks;
}

inline Json anthropic_tool_result_content(const ToolResultContent& result) {
  const auto payload = tool_result_payload(result);
  if (payload.images.empty()) {
    return payload.text;
  }

  Json blocks = Json::array();
  if (!payload.text.empty()) {
    blocks.push_back(Json{{"type", "text"}, {"text", payload.text}});
  } else {
    blocks.push_back(Json{{"type", "text"}, {"text", "(see attached image)"}});
  }
  for (const auto& image : payload.images) {
    blocks.push_back(anthropic_image_block(image));
  }
  return blocks;
}

inline Json anthropic_tool_result_block(const ToolResultContent& result) {
  return Json{{"type", "tool_result"},
              {"tool_use_id", result.tool_call_id},
              {"content", anthropic_tool_result_content(result)},
              {"is_error", result.is_error}};
}

bool anthropic_add_cache_control_to_last_user_message(Json& messages,
                                                      const Json& cache_control) {
  if (!messages.is_array() || cache_control.is_null()) {
    return false;
  }
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (!it->is_object() || it->value("role", std::string()) != "user") {
      continue;
    }
    auto& content = (*it)["content"];
    if (content.is_string()) {
      const auto text = content.get<std::string>();
      if (trim_ascii(text).empty()) {
        return false;
      }
      content = Json::array({Json{{"type", "text"},
                                  {"text", text},
                                  {"cache_control", cache_control}}});
      return true;
    }
    if (!content.is_array() || content.empty()) {
      return false;
    }
    for (auto block = content.rbegin(); block != content.rend(); ++block) {
      const auto type = block->value("type", std::string());
      if (type == "text" || type == "image" || type == "tool_result") {
        (*block)["cache_control"] = cache_control;
        return true;
      }
    }
    return false;
  }
  return false;
}

Json anthropic_tool(const ToolDefinition& tool,
                    bool eager_tool_input,
                    bool claude_code_oauth,
                    const Json& cache_control = Json(nullptr)) {
  Json schema =
      tool.input_schema.is_object() ? tool.input_schema : Json::object();
  Json input_schema = Json{{"type", "object"},
                           {"properties",
                            schema.value("properties", Json::object())},
                           {"required", schema.value("required", Json::array())}};
  Json result = Json{{"name", claude_code_oauth
                                 ? anthropic_claude_code_tool_name(tool.name)
                                 : tool.name},
                     {"description", tool.description},
                     {"input_schema", std::move(input_schema)}};
  if (eager_tool_input) {
    result["eager_input_streaming"] = true;
  }
  if (!cache_control.is_null()) {
    result["cache_control"] = cache_control;
  }
  return result;
}

inline bool is_google_schema_meta_key(const std::string& key) {
  return key == "$schema" || key == "$id" || key == "$anchor" ||
         key == "$dynamicAnchor" || key == "$vocabulary" ||
         key == "$comment" || key == "$defs" || key == "definitions";
}

inline bool google_requires_tool_call_id(const std::string& model_id) {
  return model_id.rfind("claude-", 0) == 0 || model_id.rfind("gpt-oss-", 0) == 0;
}

inline std::string google_tool_call_id(std::string_view id) {
  std::string result;
  result.reserve(std::min<std::size_t>(id.size(), 64));
  for (const unsigned char ch : id) {
    if (result.size() >= 64U) {
      break;
    }
    if (std::isalnum(ch) || ch == '_' || ch == '-') {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('_');
    }
  }
  return result;
}

inline bool google_valid_thought_signature(const std::string& signature) {
  if (signature.empty() || signature.size() % 4U != 0U) {
    return false;
  }
  return std::all_of(signature.begin(), signature.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '+' || ch == '/' || ch == '=';
  });
}

std::optional<std::string> google_resolved_thought_signature(
    bool same_provider_and_model, const std::optional<std::string>& signature) {
  if (!same_provider_and_model || !signature ||
      !google_valid_thought_signature(*signature)) {
    return std::nullopt;
  }
  return signature;
}

bool google_message_matches_model(const Message& message,
                                  const ModelRef& model) {
  if (!message.metadata.is_object()) {
    return false;
  }
  return message.metadata.value("provider", std::string()) == model.provider &&
         message.metadata.value("model", std::string()) == model.name;
}

inline std::optional<int> google_gemini_major_version(const std::string& model_id) {
  const std::string prefix = "gemini-";
  const std::string live_prefix = "gemini-live-";
  std::size_t offset = std::string::npos;
  if (model_id.rfind(prefix, 0) == 0) {
    offset = prefix.size();
  } else if (model_id.rfind(live_prefix, 0) == 0) {
    offset = live_prefix.size();
  }
  if (offset == std::string::npos || offset >= model_id.size() ||
      !std::isdigit(static_cast<unsigned char>(model_id[offset]))) {
    return std::nullopt;
  }
  return model_id[offset] - '0';
}

bool google_supports_multimodal_function_response(
    const std::string& model_id) {
  const auto major = google_gemini_major_version(lowercase_copy(model_id));
  return !major || *major >= 3;
}

inline std::string canonical_model_match_text(std::string value) {
  value = lowercase_copy(std::move(value));
  for (char& ch : value) {
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == '_' ||
        ch == '.' || ch == ':') {
      ch = '-';
    }
  }
  return value;
}

std::vector<std::string> bedrock_model_match_candidates(
    const CompletionRequest& request, const Json& options_metadata) {
  std::vector<std::string> result;
  const auto add = [&result](const std::string& value) {
    if (value.empty()) {
      return;
    }
    const auto lower = lowercase_copy(value);
    result.push_back(lower);
    result.push_back(canonical_model_match_text(value));
  };

  add(request.model.name);
  if (const auto display_name =
          string_metadata_value(options_metadata, "modelDisplayName")) {
    add(*display_name);
  } else if (const auto display_name =
                 string_metadata_value(options_metadata, "displayName")) {
    add(*display_name);
  }
  return result;
}

bool candidate_contains(const std::vector<std::string>& candidates,
                        std::string_view needle) {
  return std::any_of(candidates.begin(), candidates.end(),
                     [needle](const std::string& candidate) {
                       return candidate.find(needle) != std::string::npos;
                     });
}

bool bedrock_is_anthropic_claude(const CompletionRequest& request,
                                 const Json& options_metadata) {
  const auto candidates =
      bedrock_model_match_candidates(request, options_metadata);
  return candidate_contains(candidates, "anthropic.claude") ||
         candidate_contains(candidates, "anthropic/claude") ||
         candidate_contains(candidates, "claude");
}

bool bedrock_supports_adaptive_thinking(const CompletionRequest& request,
                                        const Json& options_metadata) {
  const auto candidates =
      bedrock_model_match_candidates(request, options_metadata);
  return candidate_contains(candidates, "opus-4-6") ||
         candidate_contains(candidates, "opus-4-7") ||
         candidate_contains(candidates, "sonnet-4-6");
}

bool bedrock_supports_native_xhigh_effort(const CompletionRequest& request,
                                          const Json& options_metadata) {
  return candidate_contains(
      bedrock_model_match_candidates(request, options_metadata), "opus-4-7");
}

bool bedrock_supports_prompt_caching(const CompletionRequest& request,
                                     const Json& options_metadata) {
  const auto candidates =
      bedrock_model_match_candidates(request, options_metadata);
  if (!candidate_contains(candidates, "claude")) {
    return options_metadata.is_object() &&
           options_metadata.value("forcePromptCaching", false);
  }
  return candidate_contains(candidates, "-4-") ||
         candidate_contains(candidates, "claude-3-7-sonnet") ||
         candidate_contains(candidates, "claude-3-5-haiku");
}

bool bedrock_is_govcloud_target(const CompletionRequest& request,
                                const Json& options_metadata) {
  if (const auto region = string_metadata_value(options_metadata, "region")) {
    if (lowercase_copy(*region).rfind("us-gov-", 0) == 0) {
      return true;
    }
  }
  const auto model_id = lowercase_copy(request.model.name);
  return model_id.rfind("us-gov.", 0) == 0 ||
         model_id.rfind("arn:aws-us-gov:", 0) == 0;
}

std::string bedrock_thinking_effort(const CompletionRequest& request,
                                    const Json& options_metadata,
                                    const std::string& level) {
  if (level == "xhigh" &&
      bedrock_supports_native_xhigh_effort(request, options_metadata)) {
    return "xhigh";
  }
  if (const auto mapped = mapped_thinking_level(level, options_metadata)) {
    return *mapped;
  }
  if (level == "minimal" || level == "low") {
    return "low";
  }
  if (level == "medium") {
    return "medium";
  }
  return "high";
}

std::int64_t bedrock_thinking_budget(const Json& options_metadata,
                                     const std::string& requested_level) {
  const std::string level = requested_level == "xhigh" ? "high" : requested_level;
  if (options_metadata.is_object() &&
      options_metadata.contains("thinkingBudgets") &&
      options_metadata.at("thinkingBudgets").is_object()) {
    const auto& budgets = options_metadata.at("thinkingBudgets");
    if (budgets.contains(level) && budgets.at(level).is_number_integer()) {
      return budgets.at(level).get<std::int64_t>();
    }
  }
  if (requested_level == "minimal") {
    return 1024;
  }
  if (requested_level == "low") {
    return 2048;
  }
  if (requested_level == "medium") {
    return 8192;
  }
  return 16384;
}

inline Json bedrock_cache_point(const std::string& retention) {
  Json cache_point = Json{{"type", "default"}};
  if (retention == "long") {
    cache_point["ttl"] = "1h";
  }
  return Json{{"cachePoint", cache_point}};
}

inline std::string bedrock_image_format(const std::string& media_type) {
  const auto lower = lowercase_copy(media_type);
  if (lower == "image/jpeg" || lower == "image/jpg") {
    return "jpeg";
  }
  if (lower == "image/gif") {
    return "gif";
  }
  if (lower == "image/webp") {
    return "webp";
  }
  return "png";
}

Json bedrock_content_blocks(const std::vector<Content>& content,
                            MessageRole role) {
  Json blocks = Json::array();
  for (const auto& item : content) {
    switch (content_kind(item)) {
      case ContentKind::text: {
        const auto text = provider_safe_text(std::get<TextContent>(item).text);
        if (!trim_ascii(text).empty()) {
          blocks.push_back(Json{{"text", text}});
        }
        break;
      }
      case ContentKind::image: {
        const auto image = normalized(std::get<ImageContent>(item));
        blocks.push_back(Json{{"image",
                               {{"format", bedrock_image_format(image.media_type)},
                                {"source", {{"bytes", image.data}}}}}});
        break;
      }
      case ContentKind::thinking: {
        const auto& thinking = std::get<ThinkingContent>(item);
        const auto text = provider_safe_text(thinking.text);
        if (!trim_ascii(text).empty()) {
          blocks.push_back(Json{{"reasoningContent",
                                 {{"reasoningText",
                                   {{"text", text},
                                    {"signature",
                                     thinking.signature.value_or(
                                         std::string())}}}}}});
          if (!thinking.signature || trim_ascii(*thinking.signature).empty()) {
            blocks.back()["reasoningContent"]["reasoningText"].erase(
                "signature");
          }
        }
        break;
      }
      case ContentKind::tool_call: {
        if (role != MessageRole::assistant) {
          break;
        }
        const auto& call = std::get<ToolCallContent>(item);
        blocks.push_back(Json{{"toolUse",
                               {{"toolUseId", google_tool_call_id(call.id)},
                                {"name", call.name},
                                {"input", call.arguments.is_null()
                                              ? Json::object()
                                              : call.arguments}}}});
        break;
      }
      case ContentKind::tool_result: {
        const auto& result = std::get<ToolResultContent>(item);
        const auto payload = tool_result_payload(result);
        Json result_blocks = Json::array();
        if (!payload.text.empty()) {
          result_blocks.push_back(Json{{"text", payload.text}});
        }
        for (const auto& image : payload.images) {
          result_blocks.push_back(
              Json{{"image",
                    {{"format", bedrock_image_format(image.media_type)},
                     {"source", {{"bytes", image.data}}}}}});
        }
        blocks.push_back(Json{{"toolResult",
                               {{"toolUseId",
                                 google_tool_call_id(result.tool_call_id)},
                                {"content", result_blocks},
                                {"status",
                                 result.is_error ? "error" : "success"}}}});
        break;
      }
    }
  }
  return blocks;
}

Json bedrock_tool_config(const std::vector<ToolDefinition>& tools,
                         const ToolChoice& choice) {
  if (tools.empty() || choice.kind == ToolChoiceKind::none) {
    return Json(nullptr);
  }
  Json config = Json{{"tools", Json::array()}};
  for (const auto& tool : tools) {
    config["tools"].push_back(
        Json{{"toolSpec",
              {{"name", tool.name},
               {"description", tool.description},
               {"inputSchema", {{"json", tool.input_schema}}}}}});
  }
  switch (choice.kind) {
    case ToolChoiceKind::auto_select:
      config["toolChoice"] = Json{{"auto", Json::object()}};
      break;
    case ToolChoiceKind::required:
      config["toolChoice"] = Json{{"any", Json::object()}};
      break;
    case ToolChoiceKind::named:
      config["toolChoice"] =
          Json{{"tool", {{"name", choice.tool_name.value_or(std::string())}}}};
      break;
    case ToolChoiceKind::none:
      break;
  }
  return config;
}

inline Json google_sanitize_openapi_schema(const Json& schema) {
  if (!schema.is_object()) {
    return schema;
  }

  Json result = Json::object();
  for (const auto& entry : schema.items()) {
    if (is_google_schema_meta_key(entry.key())) {
      continue;
    }
    result[entry.key()] = google_sanitize_openapi_schema(entry.value());
  }
  return result;
}

inline Json mistral_text_content(const std::vector<Content>& content) {
  std::string text;
  for (const auto& item : content) {
    if (content_kind(item) != ContentKind::text) {
      continue;
    }
    if (!text.empty()) {
      text += "\n";
    }
    text += provider_safe_text(std::get<TextContent>(item).text);
  }
  return text;
}

inline Json mistral_tool(const ToolDefinition& tool) {
  return Json{{"type", "function"},
              {"function",
               {{"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.input_schema},
                {"strict", false}}}};
}

inline bool mistral_uses_reasoning_effort(const std::string& model_id) {
  return model_id == "mistral-small-2603" ||
         model_id == "mistral-small-latest" ||
         model_id == "mistral-medium-3.5";
}

std::optional<std::string> mistral_requested_thinking_level(
    const Json& metadata) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }

  std::string level;
  if (metadata.contains("thinkingLevel") &&
      metadata.at("thinkingLevel").is_string()) {
    level = metadata.at("thinkingLevel").get<std::string>();
  } else if (metadata.contains("thinking_level") &&
             metadata.at("thinking_level").is_string()) {
    level = metadata.at("thinking_level").get<std::string>();
  }

  if (level.empty() || level == "off") {
    return std::nullopt;
  }
  return level;
}

std::optional<std::string> mistral_reasoning_effort(
    const std::string& level, const Json& options_metadata) {
  if (options_metadata.is_object() &&
      options_metadata.contains("thinkingLevelMap") &&
      options_metadata.at("thinkingLevelMap").is_object()) {
    const auto& map = options_metadata.at("thinkingLevelMap");
    if (map.contains(level)) {
      if (map.at(level).is_null()) {
        return std::nullopt;
      }
      if (map.at(level).is_string()) {
        return map.at(level).get<std::string>();
      }
    }
  }
  return "high";
}

inline Json openai_tool_choice(const ToolChoice& tool_choice) {
  switch (tool_choice.kind) {
    case ToolChoiceKind::auto_select:
      return "auto";
    case ToolChoiceKind::none:
      return "none";
    case ToolChoiceKind::required:
      return "required";
    case ToolChoiceKind::named:
      return Json{{"type", "function"},
                  {"name", tool_choice.tool_name.value_or(std::string())}};
  }

  throw std::invalid_argument("unmapped tool choice");
}

inline Json openai_completions_tool_choice(const ToolChoice& tool_choice) {
  switch (tool_choice.kind) {
    case ToolChoiceKind::auto_select:
      return "auto";
    case ToolChoiceKind::none:
      return "none";
    case ToolChoiceKind::required:
      return "required";
    case ToolChoiceKind::named:
      return Json{{"type", "function"},
                  {"function",
                   {{"name", tool_choice.tool_name.value_or(std::string())}}}};
  }

  throw std::invalid_argument("unmapped chat completions tool choice");
}

inline Json openai_metadata_object(const Json& metadata) {
  if (!metadata.is_object()) {
    return Json::object();
  }

  Json result = metadata;
  result.erase("cost");
  result.erase("openai");
  result.erase("thinkingLevel");
  result.erase("thinking_level");
  result.erase("reasoningSummary");
  result.erase("sessionId");
  result.erase("cacheRetention");
  result.erase("compat");
  result.erase("thinkingLevelMap");
  return result.is_object() ? result : Json::object();
}

inline Json openai_extra_options(const Json& metadata) {
  if (!metadata.is_object()) {
    return Json::object();
  }
  const auto extra = metadata.value("openai", Json::object());
  return extra.is_object() ? extra : Json::object();
}

inline std::string max_tokens_field_from_options_metadata(const Json& metadata) {
  if (!metadata.is_object()) {
    return "max_completion_tokens";
  }
  const auto compat = metadata.value("compat", Json::object());
  if (!compat.is_object()) {
    return "max_completion_tokens";
  }
  const auto field = compat.value("maxTokensField", std::string());
  if (field == "max_tokens" || field == "max_completion_tokens") {
    return field;
  }
  return "max_completion_tokens";
}

inline Json openai_completions_compat_from_metadata(const Json& metadata) {
  if (!metadata.is_object()) {
    return Json::object();
  }
  const auto compat = metadata.value("compat", Json::object());
  return compat.is_object() ? compat : Json::object();
}

inline Json openai_responses_compat_from_metadata(const Json& metadata) {
  return openai_completions_compat_from_metadata(metadata);
}

inline std::string cache_retention_from_options_metadata(const Json& metadata) {
  if (!metadata.is_object()) {
    return "short";
  }
  const auto retention = metadata.value("cacheRetention", std::string("short"));
  if (retention == "none" || retention == "long") {
    return retention;
  }
  return "short";
}

inline bool supports_long_cache_retention(const Json& compat) {
  if (!compat.is_object()) {
    return true;
  }
  return compat.value("supportsLongCacheRetention", true);
}

std::optional<std::string> string_metadata_value(const Json& metadata,
                                                 const std::string& key) {
  if (!metadata.is_object() || !metadata.contains(key) ||
      !metadata.at(key).is_string()) {
    return std::nullopt;
  }
  return metadata.at(key).get<std::string>();
}

inline std::optional<std::string> requested_thinking_level(const Json& metadata) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  if (metadata.contains("thinkingLevel") &&
      metadata.at("thinkingLevel").is_string()) {
    return metadata.at("thinkingLevel").get<std::string>();
  }
  if (metadata.contains("thinking_level") &&
      metadata.at("thinking_level").is_string()) {
    return metadata.at("thinking_level").get<std::string>();
  }
  return std::nullopt;
}

std::optional<std::string> mapped_thinking_level(
    const std::string& level, const Json& options_metadata) {
  if (options_metadata.is_object() &&
      options_metadata.contains("thinkingLevelMap") &&
      options_metadata.at("thinkingLevelMap").is_object()) {
    const auto& map = options_metadata.at("thinkingLevelMap");
    if (map.contains(level)) {
      if (map.at(level).is_null()) {
        return std::nullopt;
      }
      if (map.at(level).is_string()) {
        return map.at(level).get<std::string>();
      }
    }
  }
  return level;
}

std::optional<Json> openai_completions_cache_control(const Json& compat,
                                                     const std::string& retention) {
  if (!compat.is_object() ||
      compat.value("cacheControlFormat", std::string()) != "anthropic" ||
      retention == "none") {
    return std::nullopt;
  }

  Json cache_control = Json{{"type", "ephemeral"}};
  if (retention == "long" && supports_long_cache_retention(compat)) {
    cache_control["ttl"] = "1h";
  }
  return cache_control;
}

bool add_cache_control_to_text_content(Json& message,
                                       const Json& cache_control) {
  if (!message.is_object() || !message.contains("content")) {
    return false;
  }

  auto& content = message["content"];
  if (content.is_string()) {
    const auto text = content.get<std::string>();
    if (text.empty()) {
      return false;
    }
    content = Json::array({Json{{"type", "text"},
                                {"text", text},
                                {"cache_control", cache_control}}});
    return true;
  }

  if (!content.is_array()) {
    return false;
  }
  for (auto it = content.rbegin(); it != content.rend(); ++it) {
    if (it->is_object() && it->value("type", std::string()) == "text") {
      (*it)["cache_control"] = cache_control;
      return true;
    }
  }
  return false;
}

void apply_openai_completions_anthropic_cache_control(
    Json& messages, Json* tools, const Json& cache_control) {
  if (messages.is_array()) {
    for (auto& message : messages) {
      const auto role = message.value("role", std::string());
      if (role == "system" || role == "developer") {
        add_cache_control_to_text_content(message, cache_control);
        break;
      }
    }

    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
      const auto role = it->value("role", std::string());
      if ((role == "user" || role == "assistant") &&
          add_cache_control_to_text_content(*it, cache_control)) {
        break;
      }
    }
  }

  if (tools != nullptr && tools->is_array() && !tools->empty()) {
    tools->back()["cache_control"] = cache_control;
  }
}

void apply_openai_responses_options(Json& body, const CompletionRequest& request,
                                    const Json& options_metadata) {
  const auto compat = openai_responses_compat_from_metadata(options_metadata);
  const auto retention = cache_retention_from_options_metadata(options_metadata);
  const auto session_id = string_metadata_value(options_metadata, "sessionId");

  if (session_id && retention != "none") {
    body["prompt_cache_key"] = *session_id;
  }
  if (retention == "long" && supports_long_cache_retention(compat)) {
    body["prompt_cache_retention"] = "24h";
  }

  const auto level = requested_thinking_level(request.metadata);
  if (level && !level->empty() && *level != "off") {
    if (const auto effort = mapped_thinking_level(*level, options_metadata)) {
      body["reasoning"] =
          Json{{"effort", *effort},
               {"summary",
                options_metadata.value("reasoningSummary", std::string("auto"))}};
      body["include"] = Json::array({"reasoning.encrypted_content"});
    }
  }
}

void apply_openai_completions_cache_options(Json& body,
                                            const Json& options_metadata) {
  const auto compat = openai_completions_compat_from_metadata(options_metadata);
  const auto retention = cache_retention_from_options_metadata(options_metadata);
  const auto supports_long = supports_long_cache_retention(compat);
  const auto session_id = string_metadata_value(options_metadata, "sessionId");
  const auto base_url = string_metadata_value(options_metadata, "baseUrl");

  if (session_id &&
      ((base_url && base_url->find("api.openai.com") != std::string::npos &&
        retention != "none") ||
       (retention == "long" && supports_long))) {
    body["prompt_cache_key"] = *session_id;
  }
  if (retention == "long" && supports_long) {
    body["prompt_cache_retention"] = "24h";
  }

  if (const auto cache_control =
          openai_completions_cache_control(compat, retention)) {
    Json* tools = body.contains("tools") ? &body["tools"] : nullptr;
    apply_openai_completions_anthropic_cache_control(body["messages"], tools,
                                                     *cache_control);
  }
}

void apply_openai_completions_reasoning_options(
    Json& body, const CompletionRequest& request, const Json& options_metadata) {
  const auto level = requested_thinking_level(request.metadata);
  if (!level || level->empty() || *level == "off") {
    return;
  }

  const auto compat = openai_completions_compat_from_metadata(options_metadata);
  const auto effort = mapped_thinking_level(*level, options_metadata)
                          .value_or(*level);
  const auto thinking_format =
      compat.value("thinkingFormat", std::string("openai"));
  if (thinking_format == "openrouter") {
    body["reasoning"] = Json{{"effort", effort}};
  } else if (thinking_format == "together") {
    body["reasoning"] = Json{{"enabled", true}};
    if (compat.value("supportsReasoningEffort", false)) {
      body["reasoning_effort"] = effort;
    }
  } else if (compat.value("supportsReasoningEffort", false)) {
    body["reasoning_effort"] = effort;
  }
}

void apply_openai_completions_routing_options(
    Json& body, const Json& options_metadata) {
  if (!options_metadata.is_object()) {
    return;
  }
  const auto base_url = string_metadata_value(options_metadata, "baseUrl");
  if (!base_url || base_url->find("openrouter.ai") == std::string::npos) {
    return;
  }
  const auto compat = options_metadata.value("compat", Json::object());
  if (!compat.is_object() || !compat.contains("openRouterRouting") ||
      !compat.at("openRouterRouting").is_object()) {
    return;
  }
  body["provider"] = compat.at("openRouterRouting");
}

std::optional<TokenCostRates> cost_rates_from_options(
    const ProviderOptions& options) {
  if (!options.metadata.is_object()) {
    return std::nullopt;
  }

  const auto cost = options.metadata.value("cost", Json::object());
  if (!cost.is_object()) {
    return std::nullopt;
  }

  TokenCostRates rates;
  rates.input_usd_per_million =
      cost.value("input_usd_per_million", cost.value("input", 0.0));
  rates.output_usd_per_million =
      cost.value("output_usd_per_million", cost.value("output", 0.0));
  rates.cache_read_usd_per_million =
      cost.value("cache_read_usd_per_million", cost.value("cacheRead", 0.0));
  rates.cache_write_usd_per_million =
      cost.value("cache_write_usd_per_million", cost.value("cacheWrite", 0.0));
  return rates;
}

void apply_accounting(CompletionResponse& response,
                      const ProviderOptions& options) {
  const auto rates = cost_rates_from_options(options);
  if (rates) {
    apply_cost(response.usage, *rates);
  } else {
    normalize(response.usage);
  }
}

void apply_accounting(std::vector<StreamEvent>& events,
                      const ProviderOptions& options) {
  const auto rates = cost_rates_from_options(options);
  for (auto& event : events) {
    if (event.usage) {
      if (rates) {
        apply_cost(*event.usage, *rates);
      } else {
        normalize(*event.usage);
      }
    }
  }
}

HttpResponse send_with_retries(HttpTransport& transport, HttpRequest& http,
                               const ProviderOptions& options) {
  const int max_retries = std::max(0, options.max_retries.value_or(0));
  const auto max_retry_delay_ms = options.max_retry_delay_ms.value_or(60000);

  for (int attempt = 0;; ++attempt) {
    http.attempt = attempt;
    HttpResponse response;
    try {
      response = transport.send(http);
    } catch (const std::runtime_error&) {
      if (attempt < max_retries) {
        continue;
      }
      throw;
    }

    if (response.status >= 200 && response.status < 300) {
      return response;
    }

    if (!is_retryable_http_status(response.status) || attempt >= max_retries) {
      auto message = "provider transport returned HTTP status " +
                     std::to_string(response.status);
      const auto detail = provider_http_error_detail(response);
      if (!detail.empty()) {
        message += ": " + detail;
      }
      throw std::runtime_error(message);
    }

    const auto retry_delay_ms = retry_after_ms(response);
    if (retry_delay_ms && max_retry_delay_ms > 0 &&
        *retry_delay_ms > max_retry_delay_ms) {
      throw std::runtime_error("provider retry-after delay " +
                               std::to_string(*retry_delay_ms) +
                               "ms exceeds max_retry_delay_ms " +
                               std::to_string(max_retry_delay_ms));
    }
  }

  throw std::runtime_error("provider transport retry loop exited unexpectedly");
}

}  // namespace
}  // namespace detail
}  // namespace qt::ai
