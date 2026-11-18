#include "qt/ai/provider_transport.hpp"

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/images.hpp"
#include "qt/ai/stream_utils.hpp"
#include "qt/foundation/pi_user_agent.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace qt::ai {
namespace {

std::string sanitize_utf8(std::string_view input) {
  std::string result;
  result.reserve(input.size());

  for (std::size_t i = 0; i < input.size(); ) {
    const unsigned char c = static_cast<unsigned char>(input[i]);

    // Determine expected UTF-8 sequence length
    std::size_t seq_len = 0;
    if (c < 0x80) {
      seq_len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      seq_len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      seq_len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      seq_len = 4;
    } else {
      // Invalid start byte - replace with U+FFFD
      result += "\xEF\xBF\xBD";
      ++i;
      continue;
    }

    // Check if we have enough bytes
    if (i + seq_len > input.size()) {
      // Truncated sequence - replace with U+FFFD
      result += "\xEF\xBF\xBD";
      ++i;
      continue;
    }

    // Validate continuation bytes
    bool valid = true;
    for (std::size_t j = 1; j < seq_len; ++j) {
      if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80) {
        valid = false;
        break;
      }
    }

    if (valid) {
      // Copy the valid sequence
      result.append(input.data() + i, seq_len);
      i += seq_len;
    } else {
      // Invalid sequence - replace first byte with U+FFFD
      result += "\xEF\xBF\xBD";
      ++i;
    }
  }

  return result;
}

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

constexpr std::string_view kDefaultAzureOpenAIApiVersion = "v1";

struct ParsedHttpUrl {
  std::string scheme;
  std::string host;
  std::string port;
  std::string target = "/";
  std::string username;
  std::string password;
  bool https = false;
};

std::optional<std::string> string_metadata_value(const Json& metadata,
                                                 const std::string& key);
std::string cache_retention_from_options_metadata(const Json& metadata);
bool supports_long_cache_retention(const Json& compat);
bool compat_flag(const Json& compat, const std::string& key,
                 bool default_value);
Json openai_completions_compat_from_metadata(const Json& metadata);
Json anthropic_messages_compat_from_metadata(const Json& metadata);
std::optional<std::string> requested_thinking_level(const Json& metadata);
std::optional<std::string> mapped_thinking_level(
    const std::string& level, const Json& options_metadata);
std::optional<std::string> non_empty_string_field(const Json& object,
                                                  const std::string& key);

std::string lowercase_copy(std::string value) {
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

bool content_type_contains(const HttpResponse& response, const std::string& needle) {
  const auto content_type = header_value(response.headers, "content-type");
  return content_type &&
         lowercase_copy(*content_type).find(lowercase_copy(needle)) !=
             std::string::npos;
}

std::string trim_ascii(std::string_view value) {
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

std::string provider_http_error_detail(const HttpResponse& response) {
  auto body = trim_ascii(response.body);
  if (body.empty()) {
    return {};
  }

  try {
    const auto parsed = Json::parse(sanitize_utf8(body));
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

std::optional<std::string> service_tier_from_metadata(const Json& metadata) {
  if (!metadata.is_object()) {
    return std::nullopt;
  }
  if (const auto value = string_metadata_value(metadata, "serviceTier")) {
    return trim_ascii(*value);
  }
  if (const auto value = string_metadata_value(metadata, "service_tier")) {
    return trim_ascii(*value);
  }
  return std::nullopt;
}

std::string provider_key(std::string_view provider) {
  return lowercase_copy(trim_ascii(provider));
}

std::optional<std::vector<std::uint8_t>> decode_base64url(
    const std::string& value) {
  std::array<int, 256> table{};
  table.fill(-1);
  for (int index = 0; index < 26; ++index) {
    table[static_cast<std::size_t>('A') + static_cast<std::size_t>(index)] =
        index;
    table[static_cast<std::size_t>('a') + static_cast<std::size_t>(index)] =
        26 + index;
  }
  for (int index = 0; index < 10; ++index) {
    table[static_cast<std::size_t>('0') + static_cast<std::size_t>(index)] =
        52 + index;
  }
  table[static_cast<std::size_t>('-')] = 62;
  table[static_cast<std::size_t>('_')] = 63;

  std::vector<std::uint8_t> output;
  int accumulator = 0;
  int bits = -8;
  bool saw_padding = false;
  for (const unsigned char ch : value) {
    if (std::isspace(ch) != 0) {
      continue;
    }
    if (ch == '=') {
      saw_padding = true;
      continue;
    }
    if (saw_padding) {
      return std::nullopt;
    }
    const int decoded = table[ch];
    if (decoded < 0) {
      return std::nullopt;
    }
    accumulator = (accumulator << 6) | decoded;
    bits += 6;
    if (bits >= 0) {
      output.push_back(
          static_cast<std::uint8_t>((accumulator >> bits) & 0xff));
      bits -= 8;
    }
  }
  return output;
}

std::optional<std::string> openai_codex_account_id(
    const std::string& access_token) {
  const auto first = access_token.find('.');
  if (first == std::string::npos) {
    return std::nullopt;
  }
  const auto second = access_token.find('.', first + 1U);
  if (second == std::string::npos ||
      access_token.find('.', second + 1U) != std::string::npos) {
    return std::nullopt;
  }

  const auto payload = decode_base64url(
      access_token.substr(first + 1U, second - first - 1U));
  if (!payload) {
    return std::nullopt;
  }

  try {
    const std::string text(payload->begin(), payload->end());
    const auto json = Json::parse(text);
    const auto auth = json.find("https://api.openai.com/auth");
    if (auth == json.end() || !auth->is_object()) {
      return std::nullopt;
    }
    return string_metadata_value(*auth, "chatgpt_account_id");
  } catch (const std::exception&) {
    return std::nullopt;
  }
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

std::string string_field_or(const Json& object, const std::string& key,
                            std::string fallback = {}) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_string()) {
    return fallback;
  }
  return object.at(key).get<std::string>();
}

bool message_has_image_content(const Message& message) {
  for (const auto& content : message.content) {
    if (std::holds_alternative<ImageContent>(content)) {
      return true;
    }
  }
  return false;
}

bool assistant_message_is_incomplete_turn(const Message& message) {
  if (message.role != MessageRole::assistant || !message.metadata.is_object()) {
    return false;
  }

  const auto read_stop_reason = [&message](const char* key) {
    const auto it = message.metadata.find(key);
    if (it == message.metadata.end() || !it->is_string()) {
      return std::optional<std::string>{};
    }
    return std::optional<std::string>{lowercase_copy(trim_ascii(it->get<std::string>()))};
  };

  const auto stop_reason = read_stop_reason("stopReason")
                               .value_or(read_stop_reason("stop_reason")
                                             .value_or(std::string{}));
  return stop_reason == "error" || stop_reason == "aborted";
}

std::vector<Message> provider_replay_messages(const std::vector<Message>& messages) {
  std::vector<Message> result;
  std::vector<ToolCallContent> pending_tool_calls;
  std::vector<std::string> existing_tool_result_ids;

  const auto has_existing_tool_result = [&existing_tool_result_ids](
                                            const std::string& id) {
    return std::find(existing_tool_result_ids.begin(),
                     existing_tool_result_ids.end(),
                     id) != existing_tool_result_ids.end();
  };

  const auto insert_synthetic_tool_results = [&]() {
    for (const auto& call : pending_tool_calls) {
      if (has_existing_tool_result(call.id)) {
        continue;
      }
      result.push_back(Message{
          std::nullopt,
          MessageRole::tool,
          std::nullopt,
          {ToolResultContent{call.id,
                             call.name,
                             Json{{"content",
                                   Json::array({Json{{"type", "text"},
                                                      {"text",
                                                       "No result provided"}}})}},
                             true}},
          Json::object()});
    }
    pending_tool_calls.clear();
    existing_tool_result_ids.clear();
  };

  for (const auto& message : messages) {
    if (message.role == MessageRole::assistant) {
      insert_synthetic_tool_results();
      if (assistant_message_is_incomplete_turn(message)) {
        continue;
      }

      pending_tool_calls.clear();
      existing_tool_result_ids.clear();
      for (const auto& content : message.content) {
        if (content_kind(content) == ContentKind::tool_call) {
          pending_tool_calls.push_back(std::get<ToolCallContent>(content));
        }
      }
      result.push_back(message);
      continue;
    }

    if (message.role == MessageRole::tool) {
      for (const auto& content : message.content) {
        if (content_kind(content) == ContentKind::tool_result) {
          existing_tool_result_ids.push_back(
              std::get<ToolResultContent>(content).tool_call_id);
        }
      }
      result.push_back(message);
      continue;
    }

    if (message.role == MessageRole::user) {
      insert_synthetic_tool_results();
    }
    result.push_back(message);
  }

  insert_synthetic_tool_results();
  return result;
}

std::map<std::string, std::string> github_copilot_dynamic_headers(
    const CompletionRequest& request) {
  std::map<std::string, std::string> headers;
  headers.emplace("X-Initiator",
                  !request.messages.empty() &&
                          request.messages.back().role != MessageRole::user
                      ? "agent"
                      : "user");
  headers.emplace("Openai-Intent", "conversation-edits");

  for (const auto& message : request.messages) {
    if ((message.role == MessageRole::user || message.role == MessageRole::tool) &&
        message_has_image_content(message)) {
      headers.emplace("Copilot-Vision-Request", "true");
      break;
    }
  }
  return headers;
}

std::map<std::string, std::string> metadata_headers(const Json& metadata) {
  std::map<std::string, std::string> headers;
  if (!metadata.is_object() || !metadata.contains("headers") ||
      !metadata.at("headers").is_object()) {
    return headers;
  }
  for (const auto& entry : metadata.at("headers").items()) {
    if (entry.value().is_string()) {
      headers[entry.key()] = entry.value().get<std::string>();
    }
  }
  return headers;
}

void merge_overriding_headers(std::map<std::string, std::string>& into,
                              const std::map<std::string, std::string>& overlay) {
  for (const auto& entry : overlay) {
    into[entry.first] = entry.second;
  }
}

bool is_cloudflare_ai_gateway(std::string_view provider) {
  return provider_key(provider) == "cloudflare-ai-gateway";
}

void apply_openai_family_auth_headers(
    std::map<std::string, std::string>& headers, std::string_view provider,
    const std::optional<std::string>& api_key) {
  if (!api_key) {
    return;
  }
  if (is_cloudflare_ai_gateway(provider)) {
    headers["cf-aig-authorization"] = "Bearer " + *api_key;
    return;
  }
  headers.emplace("authorization", "Bearer " + *api_key);
}

bool metadata_bool_false(const Json& metadata, const std::string& key) {
  return metadata.is_object() && metadata.contains(key) &&
         metadata.at(key).is_boolean() && !metadata.at(key).get<bool>();
}

bool openrouter_attribution_enabled(const Json& metadata) {
  return !metadata_bool_false(metadata, "telemetryEnabled") &&
         !metadata_bool_false(metadata, "enableInstallTelemetry");
}

bool is_openrouter_routed_request(const CompletionRequest& request,
                                  const ProviderOptions& options,
                                  const std::string& url) {
  if (provider_key(request.model.provider) == "openrouter") {
    return true;
  }
  if (url.find("openrouter.ai") != std::string::npos) {
    return true;
  }
  if (const auto base_url = string_metadata_value(options.metadata, "baseUrl")) {
    return base_url->find("openrouter.ai") != std::string::npos;
  }
  return false;
}

void apply_openrouter_attribution_headers(
    std::map<std::string, std::string>& headers,
    const CompletionRequest& request, const ProviderOptions& options,
    const std::string& url) {
  if (!openrouter_attribution_enabled(options.metadata) ||
      !is_openrouter_routed_request(request, options, url)) {
    return;
  }
  headers.emplace("HTTP-Referer", "https://pi.dev");
  headers.emplace("X-OpenRouter-Title", "pi");
  headers.emplace("X-OpenRouter-Categories", "cli-agent");
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

void append_utf8_replacement(std::string& value) {
  value += "\xEF\xBF\xBD";
}

bool is_utf8_continuation(unsigned char ch) {
  return (ch & 0xC0U) == 0x80U;
}

std::string provider_safe_text(std::string_view value) {
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
        if (c0 == 0xEDU && c1 >= 0xA0U && c1 <= 0xBFU &&
            is_utf8_continuation(c2)) {
          i += 3;
          continue;
        }
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

std::string provider_safe_json_dump(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}

std::optional<Json> parse_optional_json(std::string_view value) {
  try {
    return Json::parse(value);
  } catch (const Json::parse_error&) {
    return std::nullopt;
  }
}

std::optional<std::string> env_string(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return std::nullopt;
  }
  auto trimmed = trim_ascii(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return trimmed;
}

bool ends_with(std::string_view value, std::string_view suffix) {
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

std::string format_url(const ParsedUrl& url) {
  return url.scheme + "://" + url.authority + url.path + url.suffix;
}

void strip_trailing_slashes(std::string& value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
}

std::string normalize_azure_openai_base_url(const std::string& base_url) {
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

std::string azure_default_base_url(const std::string& resource_name) {
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

std::string resolve_provider_url_placeholders(const std::string& value,
                                              const std::string& provider_id) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t offset = 0; offset < value.size();) {
    if (value[offset] != '{') {
      result.push_back(value[offset]);
      ++offset;
      continue;
    }
    const auto close = value.find('}', offset + 1);
    if (close == std::string::npos) {
      result.push_back(value[offset]);
      ++offset;
      continue;
    }
    const std::string name = value.substr(offset + 1, close - offset - 1);
    const char* env = std::getenv(name.c_str());
    if (env == nullptr || *env == '\0') {
      std::string message = name;
      message += " is required for provider ";
      message += provider_id;
      message += " but is not set.";
      throw std::runtime_error(message);
    }
    result += env;
    offset = close + 1;
  }
  return result;
}

std::string provider_endpoint_from_options(const ProviderOptions& options,
                                           const std::string& endpoint,
                                           const std::string& provider_id,
                                           std::string_view path) {
  if (!endpoint.empty()) {
    return resolve_provider_url_placeholders(endpoint, provider_id);
  }
  const auto base_url = string_metadata_value(options.metadata, "baseUrl");
  if (!base_url || trim_ascii(*base_url).empty()) {
    throw std::runtime_error("Provider base URL is required for provider " +
                             provider_id);
  }
  return append_path_before_url_suffix(
      resolve_provider_url_placeholders(trim_ascii(*base_url), provider_id),
      path);
}

std::string azure_openai_api_version(const ProviderOptions& options) {
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
  auto endpoint =
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
  if (const auto value = env_string("AZURE_OPENAI_DEPLOYMENT_NAME_MAP")) {
    std::size_t start = 0;
    while (start <= value->size()) {
      const auto end = value->find(',', start);
      const auto entry = trim_ascii(value->substr(start, end - start));
      const auto equals = entry.find('=');
      if (equals != std::string::npos) {
        const auto model_id = trim_ascii(entry.substr(0, equals));
        const auto deployment = trim_ascii(entry.substr(equals + 1U));
        if (model_id == request.model.name && !deployment.empty()) {
          return deployment;
        }
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1U;
    }
  }
  return request.model.name;
}

bool google_vertex_is_adc_marker(const std::string& value) {
  const std::string trimmed = trim_ascii(value);
  return trimmed == "gcp-vertex-credentials" ||
         (trimmed.size() > 2 && trimmed.front() == '<' &&
          trimmed.back() == '>');
}

std::optional<std::string> google_vertex_api_key(
    const ProviderOptions& options) {
  if (options.api_key) {
    auto trimmed = trim_ascii(*options.api_key);
    if (!trimmed.empty() && !google_vertex_is_adc_marker(trimmed)) {
      return trimmed;
    }
    return std::nullopt;
  }
  if (const auto value = string_metadata_value(options.metadata, "apiKey")) {
    auto trimmed = trim_ascii(*value);
    if (!trimmed.empty() && !google_vertex_is_adc_marker(trimmed)) {
      return trimmed;
    }
  }
  if (const auto value = env_string("GOOGLE_CLOUD_API_KEY")) {
    if (!google_vertex_is_adc_marker(*value)) {
      return value;
    }
  }
  return std::nullopt;
}

std::string google_vertex_required_option(const ProviderOptions& options,
                                          const std::string& metadata_key,
                                          const char* env_name,
                                          const char* alternate_env_name,
                                          const std::string& message) {
  if (const auto value = string_metadata_value(options.metadata, metadata_key)) {
    const auto trimmed = trim_ascii(*value);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  if (const auto value = env_string(env_name)) {
    return *value;
  }
  if (alternate_env_name != nullptr) {
    if (const auto value = env_string(alternate_env_name)) {
      return *value;
    }
  }
  throw std::runtime_error(message);
}

bool google_vertex_generated_base_url(const std::string& base_url) {
  const auto trimmed = trim_ascii(base_url);
  return trimmed.empty() ||
         trimmed == "https://{location}-aiplatform.googleapis.com";
}

bool google_vertex_path_segment_is_api_version(std::string_view segment) {
  if (segment.size() < 2 || segment.front() != 'v' ||
      !std::isdigit(static_cast<unsigned char>(segment[1]))) {
    return false;
  }
  std::size_t offset = 2;
  while (offset < segment.size() &&
         std::isdigit(static_cast<unsigned char>(segment[offset]))) {
    ++offset;
  }
  if (offset == segment.size()) {
    return true;
  }
  constexpr std::string_view beta = "beta";
  if (segment.substr(offset, beta.size()) != beta) {
    return false;
  }
  offset += beta.size();
  while (offset < segment.size() &&
         std::isdigit(static_cast<unsigned char>(segment[offset]))) {
    ++offset;
  }
  return offset == segment.size();
}

bool google_vertex_base_url_includes_api_version(
    const std::string& base_url) {
  const auto parsed = parse_absolute_url(
      base_url, "Invalid Google Vertex base URL: ");
  std::string_view path(parsed.path);
  while (!path.empty()) {
    if (path.front() == '/') {
      path.remove_prefix(1);
      continue;
    }
    const auto slash = path.find('/');
    const auto segment =
        slash == std::string_view::npos ? path : path.substr(0, slash);
    if (google_vertex_path_segment_is_api_version(segment)) {
      return true;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    path.remove_prefix(slash + 1);
  }
  return false;
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

std::pair<std::string_view, std::optional<std::string_view>> split_tool_call_id(
    std::string_view value) {
  const auto pipe = value.find('|');
  if (pipe == std::string_view::npos) {
    return {value, std::nullopt};
  }
  return {value.substr(0, pipe), value.substr(pipe + 1U)};
}

std::uint32_t js_imul(std::uint32_t left, std::uint32_t right) {
  return static_cast<std::uint32_t>(
      static_cast<std::uint64_t>(left) * static_cast<std::uint64_t>(right));
}

std::string base36_u32(std::uint32_t value) {
  constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  if (value == 0) {
    return "0";
  }
  std::string result;
  while (value > 0) {
    result.push_back(digits[value % 36U]);
    value /= 36U;
  }
  std::reverse(result.begin(), result.end());
  return result;
}

std::string short_hash(std::string_view value) {
  std::uint32_t h1 = 0xdeadbeefU;
  std::uint32_t h2 = 0x41c6ce57U;
  for (const unsigned char ch : value) {
    h1 = js_imul(h1 ^ ch, 2654435761U);
    h2 = js_imul(h2 ^ ch, 1597334677U);
  }
  h1 = js_imul(h1 ^ (h1 >> 16U), 2246822507U) ^
       js_imul(h2 ^ (h2 >> 13U), 3266489909U);
  h2 = js_imul(h2 ^ (h2 >> 16U), 2246822507U) ^
       js_imul(h1 ^ (h1 >> 13U), 3266489909U);
  return base36_u32(h2) + base36_u32(h1);
}

bool has_prefix(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

void normalize_openai_codex_input_ids(Json& input) {
  if (!input.is_array()) {
    return;
  }
  for (auto& item : input) {
    if (!item.is_object() || !item.contains("id") ||
        !item.at("id").is_string()) {
      continue;
    }
    const auto type = item.value("type", std::string());
    const auto id = item.at("id").get<std::string>();
    if (type == "message" && !has_prefix(id, "msg")) {
      item["id"] = "msg_" + short_hash(id);
    } else if (type == "function_call" && !has_prefix(id, "fc")) {
      item["id"] = "fc_" + short_hash(id);
    }
  }
}

bool openai_responses_target_preserves_item_ids(const ModelRef& model) {
  const auto provider = provider_key(model.provider);
  return provider == "openai" || provider == "openai-codex" ||
         provider == "opencode";
}

bool message_source_is_foreign(const Message& message, const ModelRef& target) {
  const auto source_provider = string_metadata_value(message.metadata, "provider");
  const auto source_api = string_metadata_value(message.metadata, "api");
  return (source_provider && provider_key(*source_provider) != provider_key(target.provider)) ||
         (source_api && *source_api != target.api);
}

bool message_source_is_different_model(const Message& message,
                                       const ModelRef& target) {
  const auto source_provider = string_metadata_value(message.metadata, "provider");
  const auto source_api = string_metadata_value(message.metadata, "api");
  const auto source_model = string_metadata_value(message.metadata, "model");
  return source_provider && source_api && source_model &&
         provider_key(*source_provider) == provider_key(target.provider) &&
         *source_api == target.api && *source_model != target.name;
}

bool message_source_is_same_model(const Message& message,
                                  const ModelRef& target) {
  const auto source_provider = string_metadata_value(message.metadata, "provider");
  const auto source_api = string_metadata_value(message.metadata, "api");
  const auto source_model = string_metadata_value(message.metadata, "model");
  return source_provider && source_api && source_model &&
         provider_key(*source_provider) == provider_key(target.provider) &&
         *source_api == target.api && *source_model == target.name;
}

struct OpenAIResponsesToolCallIds {
  std::string call_id;
  std::optional<std::string> item_id;
};

OpenAIResponsesToolCallIds openai_responses_tool_call_ids(
    std::string_view id, const ModelRef& target, const Message& source) {
  const auto [call_id, item_id] = split_tool_call_id(id);
  if (!item_id || !openai_responses_target_preserves_item_ids(target)) {
    return {sanitize_tool_call_id_part(id, 64), std::nullopt};
  }

  auto normalized_item_id =
      message_source_is_foreign(source, target)
          ? std::string("fc_") + short_hash(*item_id)
          : sanitize_tool_call_id_part(*item_id, 64);
  if (message_source_is_different_model(source, target) &&
      normalized_item_id.rfind("fc_", 0) == 0) {
    return {sanitize_tool_call_id_part(call_id, 64), std::nullopt};
  }
  if (normalized_item_id.rfind("fc_", 0) != 0) {
    normalized_item_id =
        sanitize_tool_call_id_part("fc_" + normalized_item_id, 64);
  }
  if (normalized_item_id.size() > 64U) {
    normalized_item_id.resize(64U);
  }
  return {sanitize_tool_call_id_part(call_id, 64), std::move(normalized_item_id)};
}

std::string openai_completions_tool_call_id(std::string_view id,
                                            const ModelRef& target) {
  if (id.find('|') == std::string_view::npos) {
    if (provider_key(target.provider) == "openai" && id.size() > 40U) {
      return std::string(id.substr(0, 40U));
    }
    return std::string(id);
  }
  return sanitize_tool_call_id_part(split_tool_call_id(id).first, 40);
}

std::string openai_responses_call_id(std::string_view id) {
  return sanitize_tool_call_id_part(split_tool_call_id(id).first, 64);
}

std::string openai_responses_call_id_for_target(std::string_view id,
                                                const ModelRef& target) {
  if (!openai_responses_target_preserves_item_ids(target)) {
    return sanitize_tool_call_id_part(id, 64);
  }
  return openai_responses_call_id(id);
}

std::string anthropic_tool_call_id(std::string_view id) {
  std::string result;
  result.reserve(std::min<std::size_t>(id.size(), 64U));
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

void dispatch_sse_event(std::vector<SseEvent>& events, SseEvent& current) {
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

std::optional<std::string> openai_text_signature(const Json& item);

void append_content_from_openai_message(StreamEvent& event, const Json& item) {
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
    } else if (type == "refusal" && part.contains("refusal") &&
               part.at("refusal").is_string()) {
      text += part.at("refusal").get<std::string>();
    }
  }

  if (!text.empty()) {
    event.content = TextContent{text, openai_text_signature(item)};
  }
}

std::optional<ToolCallContent> tool_call_from_openai_item(const Json& item) {
  if (!item.is_object() || item.value("type", std::string()) != "function_call") {
    return std::nullopt;
  }

  ToolCallContent call;
  const auto call_id = item.value("call_id", std::string());
  const auto item_id = item.value("id", std::string());
  if (!call_id.empty() && !item_id.empty() && call_id != item_id) {
    call.id = call_id + "|" + item_id;
  } else if (!call_id.empty()) {
    call.id = call_id;
  } else {
    call.id = item_id;
  }
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

Json parse_streamed_tool_arguments_or_raw(std::string_view arguments) {
  const auto text =
      arguments.empty() ? std::string("{}") : std::string(arguments);
  try {
    return Json::parse(text);
  } catch (const Json::parse_error&) {
    auto repaired = parse_streaming_json(text);
    if (!(repaired.is_object() && repaired.empty())) {
      return repaired;
    }
    return Json{{"raw", text}};
  }
}

std::string openai_reasoning_text(const Json& item) {
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

std::optional<std::string> openai_text_signature(const Json& item) {
  const auto id = non_empty_string_field(item, "id");
  if (!id) {
    return std::nullopt;
  }
  Json signature = Json{{"v", 1}, {"id", *id}};
  if (const auto phase = non_empty_string_field(item, "phase");
      phase && (*phase == "commentary" || *phase == "final_answer")) {
    signature["phase"] = *phase;
  }
  return signature.dump();
}

struct OpenAITextSignatureParts {
  std::string id;
  std::optional<std::string> phase;
};

std::optional<OpenAITextSignatureParts> parse_openai_text_signature(
    const std::optional<std::string>& signature) {
  if (!signature || signature->empty()) {
    return std::nullopt;
  }
  if (signature->front() == '{') {
    try {
      const auto parsed = Json::parse(*signature);
      if (parsed.is_object() && parsed.value("v", 0) == 1 &&
          parsed.contains("id") && parsed.at("id").is_string()) {
        OpenAITextSignatureParts parts{parsed.at("id").get<std::string>(),
                                       std::nullopt};
        if (const auto phase = non_empty_string_field(parsed, "phase");
            phase && (*phase == "commentary" || *phase == "final_answer")) {
          parts.phase = phase;
        }
        return parts;
      }
    } catch (const Json::parse_error& error) {
      (void)error;
    }
  }
  return OpenAITextSignatureParts{*signature, std::nullopt};
}

std::optional<Json> openai_responses_reasoning_replay_item(
    const Content& content) {
  if (content_kind(content) != ContentKind::thinking) {
    return std::nullopt;
  }
  const auto& thinking = std::get<ThinkingContent>(content);
  if (!thinking.signature || thinking.signature->empty()) {
    return std::nullopt;
  }
  try {
    auto parsed = Json::parse(*thinking.signature);
    if (parsed.is_object() &&
        parsed.value("type", std::string()) == "reasoning") {
      return parsed;
    }
  } catch (const Json::parse_error& error) {
    (void)error;
  }
  return std::nullopt;
}

StopReason openai_stop_reason(const Json& response) {
  const auto status = response.value("status", std::string());
  if (status == "incomplete") {
    return StopReason::max_tokens;
  }
  if (status == "failed" || status == "cancelled") {
    return StopReason::error;
  }
  return StopReason::end_turn;
}

StopReason openai_stop_reason(const Json& response, bool saw_tool_call) {
  if (saw_tool_call) {
    return StopReason::tool_call;
  }
  return openai_stop_reason(response);
}

StopReason openai_completions_stop_reason(const std::string& finish_reason) {
  if (finish_reason == "stop" || finish_reason == "end") {
    return StopReason::end_turn;
  }
  if (finish_reason == "length") {
    return StopReason::max_tokens;
  }
  if (finish_reason == "tool_calls" || finish_reason == "function_call") {
    return StopReason::tool_call;
  }
  if (finish_reason.empty()) {
    return StopReason::unknown;
  }
  return StopReason::error;
}

std::optional<std::string> openai_completions_finish_reason_error(
    const std::string& finish_reason) {
  if (finish_reason == "content_filter" || finish_reason == "network_error") {
    return "Provider finish_reason: " + finish_reason;
  }
  const auto reason = openai_completions_stop_reason(finish_reason);
  if (reason == StopReason::error) {
    return "Provider finish_reason: " + finish_reason;
  }
  return std::nullopt;
}

StopReason anthropic_stop_reason(const std::string& stop_reason) {
  if (stop_reason == "end_turn" || stop_reason == "stop_sequence") {
    return StopReason::end_turn;
  }
  if (stop_reason == "pause_turn") {
    return StopReason::end_turn;
  }
  if (stop_reason == "tool_use") {
    return StopReason::tool_call;
  }
  if (stop_reason == "max_tokens") {
    return StopReason::max_tokens;
  }
  if (stop_reason == "refusal" || stop_reason == "sensitive") {
    return StopReason::error;
  }
  if (stop_reason.empty()) {
    return StopReason::unknown;
  }
  return StopReason::error;
}

Usage openai_usage(const Json& response) {
  Usage usage;
  const auto source = response.value("usage", Json::object());
  const auto reported_input_tokens =
      source.value("input_tokens", std::int64_t{0});
  usage.output_tokens = source.value("output_tokens", std::int64_t{0});
  const auto input_details =
      source.value("input_tokens_details", Json::object());
  usage.cache_read_tokens =
      input_details.value("cached_tokens", std::int64_t{0});
  usage.input_tokens =
      std::max<std::int64_t>(0, reported_input_tokens - usage.cache_read_tokens);
  normalize(usage);
  return usage;
}

Usage openai_completions_usage(const Json& source) {
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

bool is_anthropic_message_sse_event(const std::optional<std::string>& event) {
  if (!event) {
    return false;
  }
  return *event == "message_start" || *event == "message_delta" ||
         *event == "message_stop" || *event == "content_block_start" ||
         *event == "content_block_delta" || *event == "content_block_stop";
}

struct ToolResultPayload {
  std::string text;
  std::vector<ImageContent> images;
  bool used_content_blocks = false;
};

ToolResultPayload tool_result_payload(const ToolResultContent& result) {
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

Json openai_tool_result_output(const ToolResultContent& result,
                               bool supports_image_input = true) {
  const auto payload = tool_result_payload(result);
  if (payload.images.empty()) {
    return payload.text;
  }
  if (!supports_image_input) {
    if (payload.text.empty()) {
      return std::string("(tool image omitted: model does not support images)");
    }
    return payload.text + "\n(tool image omitted: model does not support images)";
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
    const ToolResultContent& result, const ToolResultPayload& payload,
    bool include_name, const ModelRef& target,
    bool supports_image_input = true) {
  std::string text = payload.text;
  if (payload.used_content_blocks && !payload.images.empty()) {
    if (supports_image_input) {
      if (text.empty()) {
        text = "(see attached image)";
      }
    } else if (text.empty()) {
      text = "(tool image omitted: model does not support images)";
    } else {
      text += "\n(tool image omitted: model does not support images)";
    }
  }

  Json message = Json{{"role", "tool"},
                      {"tool_call_id",
                       openai_completions_tool_call_id(result.tool_call_id,
                                                       target)},
                      {"content", text}};
  if (include_name && !result.name.empty()) {
    message["name"] = result.name;
  }
  return message;
}

Json openai_completions_tool_result_image_part(const ImageContent& image) {
  return Json{{"type", "image_url"},
              {"image_url", {{"url", image_data_url(image)}}}};
}

bool metadata_declares_image_input(const Json& metadata) {
  if (!metadata.is_object() || !metadata.contains("input") ||
      !metadata.at("input").is_array()) {
    return true;
  }
  for (const auto& value : metadata.at("input")) {
    if (value.is_string() && value.get<std::string>() == "image") {
      return true;
    }
  }
  return false;
}

bool request_has_tool_history(const CompletionRequest& request) {
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

Json openai_content_part(const Content& content, MessageRole role) {
  return std::visit(
      [role](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;

        if constexpr (std::is_same<T, TextContent>::value) {
          Json part =
              Json{{"type", role == MessageRole::assistant ? "output_text"
                                                           : "input_text"},
                   {"text", provider_safe_text(item.text)}};
          if (role == MessageRole::assistant) {
            part["annotations"] = Json::array();
          }
          return part;
        } else if constexpr (std::is_same<T, ImageContent>::value) {
          return Json{{"type", "input_image"},
                      {"detail", "auto"},
                      {"image_url", image_data_url(item)}};
        } else {
          throw std::invalid_argument(
              "tool call content must be converted as a top-level input item");
        }
      },
      content);
}

Json openai_completions_content_part(const Content& content) {
  return std::visit(
      [](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;

        if constexpr (std::is_same<T, TextContent>::value) {
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

Json openai_input_item(
    const Message& message,
    const std::optional<std::string>& assistant_fallback_id = std::nullopt,
    bool supports_image_input = true) {
  Json item = Json{{"type", "message"}, {"role", to_string(message.role)}};
  if (message.id) {
    item["id"] = *message.id;
  } else if (message.role == MessageRole::assistant) {
    for (const auto& content : message.content) {
      if (content_kind(content) != ContentKind::text) {
        continue;
      }
      const auto parsed =
          parse_openai_text_signature(std::get<TextContent>(content).signature);
      if (parsed) {
        std::string id = parsed->id;
        if (id.size() > 64U) {
          id = "msg_" + short_hash(id);
        }
        item["id"] = id;
        if (parsed->phase) {
          item["phase"] = *parsed->phase;
        }
        break;
      }
    }
    if (!item.contains("id") && assistant_fallback_id &&
        !assistant_fallback_id->empty()) {
      item["id"] = *assistant_fallback_id;
    }
    item["status"] = "completed";
  }

  item["content"] = Json::array();
  bool previous_was_unsupported_image_placeholder = false;
  for (const auto& content : message.content) {
    if (content_kind(content) == ContentKind::tool_call ||
        content_kind(content) == ContentKind::tool_result ||
        content_kind(content) == ContentKind::thinking) {
      continue;
    }
    if (content_kind(content) == ContentKind::image && !supports_image_input) {
      if (!previous_was_unsupported_image_placeholder) {
        item["content"].push_back(Json{
            {"type", message.role == MessageRole::assistant ? "output_text"
                                                            : "input_text"},
            {"text", "(image omitted: model does not support images)"}});
      }
      previous_was_unsupported_image_placeholder = true;
      continue;
    }
    item["content"].push_back(openai_content_part(content, message.role));
    previous_was_unsupported_image_placeholder =
        content_kind(content) == ContentKind::text &&
        std::get<TextContent>(content).text ==
            "(image omitted: model does not support images)";
  }

  return item;
}

Json openai_assistant_text_input_item(
    const TextContent& text,
    const std::optional<std::string>& fallback_id) {
  Json item = Json{{"type", "message"},
                   {"role", "assistant"},
                   {"status", "completed"},
                   {"content",
                    Json::array({Json{{"type", "output_text"},
                                      {"text", provider_safe_text(text.text)},
                                      {"annotations", Json::array()}}})}};
  const auto parsed = parse_openai_text_signature(text.signature);
  if (parsed) {
    std::string id = parsed->id;
    if (id.size() > 64U) {
      id = "msg_" + short_hash(id);
    }
    item["id"] = id;
    if (parsed->phase) {
      item["phase"] = *parsed->phase;
    }
  } else if (fallback_id && !fallback_id->empty()) {
    item["id"] = *fallback_id;
  }
  return item;
}

void append_openai_responses_input_items(Json& input,
                                         const Message& message,
                                         const ModelRef& model,
                                         const std::string& fallback_id,
                                         bool supports_image_input,
                                         bool same_provider_model = true) {
  if (message.role == MessageRole::assistant) {
    for (const auto& content : message.content) {
      if (content_kind(content) == ContentKind::thinking) {
        const auto& thinking = std::get<ThinkingContent>(content);
        if (!same_provider_model) {
          const auto text = provider_safe_text(thinking.text);
          if (!thinking.redacted && !trim_ascii(text).empty()) {
            input.push_back(openai_assistant_text_input_item(
                TextContent{text, std::nullopt}, fallback_id));
          }
        } else if (auto reasoning =
                       openai_responses_reasoning_replay_item(content)) {
          input.push_back(std::move(*reasoning));
        }
      } else if (content_kind(content) == ContentKind::text) {
        input.push_back(openai_assistant_text_input_item(
            std::get<TextContent>(content), fallback_id));
      } else if (content_kind(content) == ContentKind::tool_call) {
        const auto& call = std::get<ToolCallContent>(content);
        const auto ids = openai_responses_tool_call_ids(call.id, model, message);
        Json item = Json{{"type", "function_call"},
                         {"call_id", ids.call_id},
                         {"name", call.name},
                         {"arguments", provider_safe_json_dump(call.arguments)}};
        if (ids.item_id) {
          item["id"] = *ids.item_id;
        }
        input.push_back(std::move(item));
      } else if (content_kind(content) == ContentKind::tool_result) {
        const auto& result = std::get<ToolResultContent>(content);
        input.push_back(
            Json{{"type", "function_call_output"},
                 {"call_id", openai_responses_call_id_for_target(
                                 result.tool_call_id, model)},
                 {"output",
                  openai_tool_result_output(result, supports_image_input)}});
      }
    }
    return;
  }

  auto input_item = openai_input_item(message, std::nullopt,
                                      supports_image_input);
  if (!input_item.at("content").empty()) {
    input.push_back(std::move(input_item));
  }
  for (const auto& content : message.content) {
    if (content_kind(content) == ContentKind::tool_call) {
      const auto& call = std::get<ToolCallContent>(content);
      const auto ids = openai_responses_tool_call_ids(call.id, model, message);
      Json item = Json{{"type", "function_call"},
                       {"call_id", ids.call_id},
                       {"name", call.name},
                       {"arguments", provider_safe_json_dump(call.arguments)}};
      if (ids.item_id) {
        item["id"] = *ids.item_id;
      }
      input.push_back(std::move(item));
    } else if (content_kind(content) == ContentKind::tool_result) {
      const auto& result = std::get<ToolResultContent>(content);
      input.push_back(
          Json{{"type", "function_call_output"},
               {"call_id", openai_responses_call_id_for_target(
                               result.tool_call_id, model)},
               {"output",
                openai_tool_result_output(result, supports_image_input)}});
    }
  }
}

std::string message_text_content(const Message& message) {
  std::string text;
  for (const auto& content : message.content) {
    if (content_kind(content) == ContentKind::text) {
      if (!text.empty()) {
        text.push_back('\n');
      }
      text += provider_safe_text(std::get<TextContent>(content).text);
    }
  }
  return text;
}

Json openai_completions_message(const Message& message, const Json& compat,
                                bool model_supports_reasoning,
                                const ModelRef& target,
                                bool model_supports_images = true,
                                bool same_provider_model = true) {
  Json item = Json{{"role", to_string(message.role)}};
  if (message.role == MessageRole::developer &&
      (!model_supports_reasoning ||
       !compat_flag(compat, "supportsDeveloperRole", true))) {
    item["role"] = "system";
  }
  if (message.name) {
    item["name"] = *message.name;
  }

  std::vector<ToolCallContent> tool_calls;
  std::vector<Json> parts;
  std::string text;
  const bool thinking_as_text =
      compat_flag(compat, "requiresThinkingAsText", false);
  bool previous_was_unsupported_image_placeholder = false;
  for (const auto& content : message.content) {
    switch (content_kind(content)) {
      case ContentKind::text:
        if (message.role != MessageRole::assistant ||
            !trim_ascii(std::get<TextContent>(content).text).empty()) {
          text += provider_safe_text(std::get<TextContent>(content).text);
          parts.push_back(openai_completions_content_part(content));
          previous_was_unsupported_image_placeholder =
              std::get<TextContent>(content).text ==
              "(image omitted: model does not support images)";
        }
        break;
      case ContentKind::image:
        if (model_supports_images) {
          parts.push_back(openai_completions_content_part(content));
          previous_was_unsupported_image_placeholder = false;
        } else if (!previous_was_unsupported_image_placeholder) {
          const std::string placeholder =
              "(image omitted: model does not support images)";
          text += placeholder;
          parts.push_back(Json{{"type", "text"}, {"text", placeholder}});
          previous_was_unsupported_image_placeholder = true;
        }
        break;
      case ContentKind::thinking: {
        const auto& thinking = std::get<ThinkingContent>(content);
        const auto safe_text = provider_safe_text(thinking.text);
        if (!trim_ascii(safe_text).empty()) {
          if (!same_provider_model) {
            if (!thinking.redacted) {
              text += safe_text;
              parts.push_back(Json{{"type", "text"}, {"text", safe_text}});
            }
          } else if (thinking_as_text) {
            text += safe_text;
            parts.push_back(Json{{"type", "text"}, {"text", safe_text}});
          }
        }
        break;
      }
      case ContentKind::tool_call:
        tool_calls.push_back(std::get<ToolCallContent>(content));
        break;
      case ContentKind::tool_result: {
        const auto& result = std::get<ToolResultContent>(content);
        return openai_completions_tool_result_message(
            result, tool_result_payload(result),
            compat_flag(compat, "requiresToolResultName", false), target);
      }
    }
  }

  if (message.role == MessageRole::assistant && !thinking_as_text &&
      same_provider_model) {
    std::vector<std::string> thinking_texts;
    std::optional<std::string> first_signature;
    for (const auto& content : message.content) {
      if (content_kind(content) != ContentKind::thinking) {
        continue;
      }
      const auto& thinking = std::get<ThinkingContent>(content);
      const auto safe_text = provider_safe_text(thinking.text);
      if (trim_ascii(safe_text).empty()) {
        continue;
      }
      if (thinking_texts.empty() && thinking.signature &&
          !thinking.signature->empty()) {
        first_signature = *thinking.signature;
      }
      thinking_texts.push_back(safe_text);
    }
    if (first_signature) {
      std::string joined_thinking;
      for (const auto& value : thinking_texts) {
        if (!joined_thinking.empty()) {
          joined_thinking += "\n";
        }
        joined_thinking += value;
      }
      item[*first_signature] = joined_thinking;
    }
  }

  if (message.role == MessageRole::assistant && thinking_as_text &&
      same_provider_model) {
    std::vector<std::string> thinking_texts;
    Json parts = Json::array();
    std::string assistant_text;
    for (const auto& content : message.content) {
      if (content_kind(content) == ContentKind::thinking) {
        const auto& thinking = std::get<ThinkingContent>(content);
        const auto safe_text = provider_safe_text(thinking.text);
        if (!trim_ascii(safe_text).empty()) {
          thinking_texts.push_back(safe_text);
        }
      } else if (content_kind(content) == ContentKind::text) {
        const auto& text_content = std::get<TextContent>(content);
        if (!trim_ascii(text_content.text).empty()) {
          const auto safe_text = provider_safe_text(text_content.text);
          assistant_text += safe_text;
          parts.push_back(Json{{"type", "text"}, {"text", safe_text}});
        }
      }
    }
    if (!thinking_texts.empty()) {
      std::string thinking_text;
      for (const auto& value : thinking_texts) {
        if (!thinking_text.empty()) {
          thinking_text += "\n\n";
        }
        thinking_text += value;
      }
      Json ordered_parts =
          Json::array({Json{{"type", "text"}, {"text", thinking_text}}});
      for (const auto& part : parts) {
        ordered_parts.push_back(part);
      }
      parts = std::move(ordered_parts);
      text = std::move(assistant_text);
    }
  }

  if (!tool_calls.empty()) {
    item["tool_calls"] = Json::array();
    Json reasoning_details = Json::array();
    for (const auto& call : tool_calls) {
      item["tool_calls"].push_back(
          Json{{"id", openai_completions_tool_call_id(call.id, target)},
               {"type", "function"},
               {"function",
                {{"name", call.name},
                 {"arguments", provider_safe_json_dump(call.arguments)}}}});
      if (same_provider_model && call.thought_signature &&
          !call.thought_signature->empty()) {
        if (const auto detail = parse_optional_json(*call.thought_signature)) {
          if (!detail->is_null()) {
            reasoning_details.push_back(*detail);
          }
        }
      }
    }
    if (!reasoning_details.empty()) {
      item["reasoning_details"] = std::move(reasoning_details);
    }
    if (thinking_as_text && !parts.empty()) {
      item["content"] = parts;
    } else if (text.empty()) {
      item["content"] = nullptr;
    } else {
      item["content"] = text;
    }
    return item;
  }

  if (parts.empty()) {
    item["content"] = "";
  } else if (message.role == MessageRole::user && parts.size() > 1) {
    item["content"] = parts;
  } else if (!thinking_as_text && parts.size() == 1 &&
             parts.front().value("type", std::string()) == "text") {
    item["content"] = text;
  } else {
    item["content"] = parts;
  }
  if (message.role == MessageRole::assistant &&
      compat_flag(compat, "requiresReasoningContentOnAssistantMessages",
                  false) &&
      model_supports_reasoning &&
      !item.contains("reasoning_content")) {
    item["reasoning_content"] = "";
  }
  return item;
}

bool openai_completions_should_skip_message(const Message& source,
                                            const Json& message) {
  if (source.role != MessageRole::assistant || message.contains("tool_calls")) {
    return false;
  }
  const auto content = message.find("content");
  if (content == message.end() || content->is_null()) {
    return true;
  }
  if (content->is_string()) {
    return content->get<std::string>().empty();
  }
  if (content->is_array()) {
    return content->empty();
  }
  return false;
}

Json openai_tool(const ToolDefinition& tool, Json strict = false) {
  return Json{{"type", "function"},
              {"name", tool.name},
              {"description", tool.description},
              {"parameters", tool.input_schema},
              {"strict", std::move(strict)}};
}

Json openai_completions_tool(const ToolDefinition& tool, const Json& compat) {
  Json function = Json{{"name", tool.name},
                       {"description", tool.description},
                       {"parameters", tool.input_schema}};
  if (compat.value("supportsStrictMode", true)) {
    function["strict"] = false;
  }
  return Json{{"type", "function"}, {"function", std::move(function)}};
}

bool anthropic_supports_eager_tool_input_streaming(const Json& metadata) {
  return compat_flag(anthropic_messages_compat_from_metadata(metadata),
                     "supportsEagerToolInputStreaming", true);
}

bool anthropic_uses_claude_code_oauth(const Json& metadata) {
  return metadata.is_object() && metadata.value("anthropicOAuth", false);
}

bool anthropic_supports_cache_control_on_tools(const Json& metadata) {
  return compat_flag(anthropic_messages_compat_from_metadata(metadata),
                     "supportsCacheControlOnTools", true);
}

bool anthropic_interleaved_thinking_enabled(const Json& metadata) {
  return !metadata.is_object() || !metadata.contains("interleavedThinking") ||
         !metadata.at("interleavedThinking").is_boolean() ||
         metadata.at("interleavedThinking").get<bool>();
}

std::string join_header_values(const std::vector<std::string>& values,
                               std::string_view separator) {
  std::string joined;
  for (const auto& value : values) {
    if (!joined.empty()) {
      joined += separator;
    }
    joined += value;
  }
  return joined;
}

bool anthropic_model_supports_reasoning(const std::string& model_id) {
  const auto lower = lowercase_copy(model_id);
  return lower.find("claude-") != std::string::npos &&
         (lower.find("-4-") != std::string::npos ||
          lower.find("-4.") != std::string::npos);
}

bool anthropic_supports_adaptive_thinking(const std::string& model_id) {
  const auto lower = lowercase_copy(model_id);
  return lower.find("opus-4-6") != std::string::npos ||
         lower.find("opus-4.6") != std::string::npos ||
         lower.find("opus-4-7") != std::string::npos ||
         lower.find("opus-4.7") != std::string::npos ||
         lower.find("sonnet-4-6") != std::string::npos ||
         lower.find("sonnet-4.6") != std::string::npos;
}

std::string anthropic_thinking_effort(const std::string& level,
                                      const Json& options_metadata) {
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

std::int64_t anthropic_thinking_budget(const Json& options_metadata,
                                       const std::string& level) {
  if (options_metadata.is_object() &&
      options_metadata.contains("thinkingBudgets") &&
      options_metadata.at("thinkingBudgets").is_object()) {
    const auto& budgets = options_metadata.at("thinkingBudgets");
    if (budgets.contains(level) && budgets.at(level).is_number_integer()) {
      return budgets.at(level).get<std::int64_t>();
    }
  }
  return 1024;
}

bool compat_sends_session_affinity_headers(const Json& metadata) {
  return compat_flag(anthropic_messages_compat_from_metadata(metadata),
                     "sendSessionAffinityHeaders", false);
}

std::string anthropic_claude_code_tool_name(const std::string& name) {
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

std::string anthropic_original_tool_name(
    const std::string& name, const std::vector<ToolDefinition>* tools) {
  if (tools == nullptr) {
    return name;
  }
  const auto lower = lowercase_copy(name);
  for (const auto& tool : *tools) {
    if (lowercase_copy(tool.name) == lower) {
      return tool.name;
    }
  }
  return name;
}

Json anthropic_text_content(const std::vector<Content>& content) {
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

Json anthropic_unsupported_image_text_content(
    const std::vector<Content>& content) {
  std::string text;
  bool previous_was_placeholder = false;
  const auto append_text = [&text](const std::string& value) {
    const auto safe = provider_safe_text(value);
    if (!text.empty()) {
      text += "\n";
    }
    text += safe;
  };

  for (const auto& item : content) {
    if (content_kind(item) == ContentKind::text) {
      const auto value = std::get<TextContent>(item).text;
      append_text(value);
      previous_was_placeholder =
          value == "(image omitted: model does not support images)";
    } else if (content_kind(item) == ContentKind::image) {
      if (!previous_was_placeholder) {
        append_text("(image omitted: model does not support images)");
      }
      previous_was_placeholder = true;
    }
  }
  return text;
}

Json anthropic_cache_control(const Json& metadata) {
  const bool explicit_cache_retention =
      metadata.is_object() && metadata.contains("cacheRetention");
  const auto retention = cache_retention_from_options_metadata(metadata);
  if (!explicit_cache_retention && retention != "long") {
    return Json(nullptr);
  }
  if (retention == "none") {
    return Json(nullptr);
  }

  const auto compat = anthropic_messages_compat_from_metadata(metadata);
  Json cache_control = Json{{"type", "ephemeral"}};
  if (retention == "long" &&
      compat_flag(compat, "supportsLongCacheRetention", true)) {
    cache_control["ttl"] = "1h";
  }
  return cache_control;
}

Json anthropic_image_block(const ImageContent& image) {
  const auto normalized_image = normalized(image);
  return Json{{"type", "image"},
              {"source",
               {{"type", "base64"},
                {"media_type", normalized_image.media_type},
                {"data", normalized_image.data}}}};
}

Json anthropic_user_content_blocks(const std::vector<Content>& content) {
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

Json anthropic_user_content(const std::vector<Content>& content,
                            bool supports_image_input = true) {
  if (!supports_image_input) {
    return anthropic_unsupported_image_text_content(content);
  }
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
                                 bool claude_code_oauth,
                                 std::map<std::string, std::string>& tool_call_id_map,
                                 bool same_provider_model = true) {
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
        if (!same_provider_model) {
          if (!thinking.redacted && !trim_ascii(text).empty()) {
            blocks.push_back(Json{{"type", "text"}, {"text", text}});
          }
          break;
        }
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
        auto id = call.id;
        if (!same_provider_model) {
          id = anthropic_tool_call_id(call.id);
          if (id != call.id) {
            tool_call_id_map[call.id] = id;
          }
        }
        blocks.push_back(
            Json{{"type", "tool_use"},
                 {"id", id},
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

Json anthropic_tool_result_content(const ToolResultContent& result,
                                   bool supports_image_input = true) {
  const auto payload = tool_result_payload(result);
  if (payload.images.empty()) {
    return payload.text;
  }
  if (!supports_image_input) {
    if (payload.text.empty()) {
      return std::string("(tool image omitted: model does not support images)");
    }
    return payload.text + "\n(tool image omitted: model does not support images)";
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

Json anthropic_tool_result_block(const ToolResultContent& result,
                                 bool supports_image_input = true,
                                 const std::map<std::string, std::string>* tool_call_id_map = nullptr) {
  const auto id =
      tool_call_id_map != nullptr &&
              tool_call_id_map->count(result.tool_call_id) > 0
          ? tool_call_id_map->at(result.tool_call_id)
          : result.tool_call_id;
  return Json{{"type", "tool_result"},
              {"tool_use_id", id},
              {"content",
               anthropic_tool_result_content(result, supports_image_input)},
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

std::optional<Json> anthropic_tool_choice(const ToolChoice& choice) {
  switch (choice.kind) {
    case ToolChoiceKind::auto_select:
      return std::nullopt;
    case ToolChoiceKind::required:
      return Json{{"type", "any"}};
    case ToolChoiceKind::named:
      return Json{{"type", "tool"},
                  {"name", choice.tool_name.value_or(std::string())}};
    case ToolChoiceKind::none:
      return Json{{"type", "none"}};
  }
  return std::nullopt;
}

std::optional<Json> anthropic_tool_choice_from_options_metadata(
    const Json& metadata) {
  if (!metadata.is_object() || !metadata.contains("toolChoice")) {
    return std::nullopt;
  }
  const auto& tool_choice = metadata.at("toolChoice");
  if (tool_choice.is_string()) {
    const auto type = tool_choice.get<std::string>();
    if (type.empty()) {
      return std::nullopt;
    }
    return Json{{"type", type}};
  }
  if (tool_choice.is_object()) {
    return tool_choice;
  }
  return std::nullopt;
}

bool is_google_schema_meta_key(const std::string& key) {
  return key == "$schema" || key == "$id" || key == "$anchor" ||
         key == "$dynamicAnchor" || key == "$vocabulary" ||
         key == "$comment" || key == "$defs" || key == "definitions";
}

bool google_requires_tool_call_id(const std::string& model_id) {
  return model_id.rfind("claude-", 0) == 0 || model_id.rfind("gpt-oss-", 0) == 0;
}

std::string lowercase_model_id(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool google_is_gemini3_pro_model(const std::string& model_id) {
  const auto id = lowercase_model_id(model_id);
  if (id.rfind("gemini-3", 0) != 0) {
    return false;
  }
  return id.find("-pro") != std::string::npos;
}

bool google_is_gemini3_flash_model(const std::string& model_id) {
  const auto id = lowercase_model_id(model_id);
  if (id.rfind("gemini-3", 0) != 0) {
    return false;
  }
  return id.find("-flash") != std::string::npos;
}

bool google_is_gemma4_model(const std::string& model_id) {
  const auto id = lowercase_model_id(model_id);
  return id.find("gemma-4") != std::string::npos ||
         id.find("gemma4") != std::string::npos;
}

Json google_disabled_thinking_config(const std::string& model_id) {
  if (google_is_gemini3_pro_model(model_id)) {
    return Json{{"thinkingLevel", "LOW"}};
  }
  if (google_is_gemini3_flash_model(model_id) ||
      google_is_gemma4_model(model_id)) {
    return Json{{"thinkingLevel", "MINIMAL"}};
  }
  return Json{{"thinkingBudget", 0}};
}

std::optional<std::string> google_mapped_thinking_level(
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

  if (level == "minimal") {
    return "MINIMAL";
  }
  if (level == "low") {
    return "LOW";
  }
  if (level == "medium") {
    return "MEDIUM";
  }
  if (level == "high" || level == "xhigh") {
    return "HIGH";
  }
  return level;
}

std::optional<std::int64_t> google_thinking_budget(
    const std::string& level, const Json& options_metadata) {
  if (!options_metadata.is_object() ||
      !options_metadata.contains("thinkingBudgets") ||
      !options_metadata.at("thinkingBudgets").is_object()) {
    return std::nullopt;
  }
  const auto& budgets = options_metadata.at("thinkingBudgets");
  if (budgets.contains(level) && budgets.at(level).is_number_integer()) {
    return budgets.at(level).get<std::int64_t>();
  }
  return std::nullopt;
}

std::string google_tool_call_id(std::string_view id) {
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

bool google_valid_thought_signature(const std::string& signature) {
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

std::optional<int> google_gemini_major_version(const std::string& model_id) {
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

std::string canonical_model_match_text(std::string value) {
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

std::optional<std::string> bedrock_standard_endpoint_region(
    const std::string& base_url) {
  const auto trimmed = trim_ascii(base_url);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  std::string host = trimmed;
  const auto scheme = host.find("://");
  if (scheme != std::string::npos) {
    host.erase(0, scheme + 3);
  }
  const auto path = host.find_first_of("/?#");
  if (path != std::string::npos) {
    host.erase(path);
  }
  const auto at = host.rfind('@');
  if (at != std::string::npos) {
    host.erase(0, at + 1);
  }
  const auto port = host.find(':');
  if (port != std::string::npos) {
    host.erase(port);
  }
  host = lowercase_copy(host);

  const std::string prefix = "bedrock-runtime";
  const std::string amazon = ".amazonaws.com";
  const std::string amazon_cn = ".amazonaws.com.cn";
  if (host.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  std::size_t region_start = prefix.size();
  if (host.compare(prefix.size(), 6, "-fips.") == 0) {
    region_start += 6;
  } else if (host.size() > region_start && host[region_start] == '.') {
    region_start += 1;
  } else {
    return std::nullopt;
  }

  std::optional<std::size_t> suffix_start;
  if (ends_with(host, amazon_cn)) {
    suffix_start = host.size() - amazon_cn.size();
  } else if (ends_with(host, amazon)) {
    suffix_start = host.size() - amazon.size();
  }
  if (!suffix_start || *suffix_start <= region_start) {
    return std::nullopt;
  }
  return host.substr(region_start, *suffix_start - region_start);
}

bool bedrock_has_proxy_env() {
  return env_string("HTTP_PROXY") || env_string("HTTPS_PROXY") ||
         env_string("NO_PROXY") || env_string("http_proxy") ||
         env_string("https_proxy") || env_string("no_proxy");
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

Json bedrock_cache_point(const std::string& retention) {
  Json cache_point = Json{{"type", "default"}};
  if (retention == "long") {
    cache_point["ttl"] = "1h";
  }
  return Json{{"cachePoint", cache_point}};
}

std::string bedrock_image_format(const std::string& media_type) {
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
                            MessageRole role,
                            bool supports_thinking_signature = false,
                            bool supports_images = true,
                            bool same_provider_model = true,
                            std::map<std::string, std::string>* tool_call_id_map =
                                nullptr) {
  Json blocks = Json::array();
  bool previous_was_unsupported_image_placeholder = false;
  for (const auto& item : content) {
    switch (content_kind(item)) {
      case ContentKind::text: {
        const auto text = provider_safe_text(std::get<TextContent>(item).text);
        if (!trim_ascii(text).empty()) {
          blocks.push_back(Json{{"text", text}});
          previous_was_unsupported_image_placeholder =
              std::get<TextContent>(item).text ==
              "(image omitted: model does not support images)";
        }
        break;
      }
      case ContentKind::image: {
        if (!supports_images) {
          if (!previous_was_unsupported_image_placeholder) {
            blocks.push_back(
                Json{{"text",
                      "(image omitted: model does not support images)"}});
          }
          previous_was_unsupported_image_placeholder = true;
          break;
        }
        const auto image = normalized(std::get<ImageContent>(item));
        blocks.push_back(Json{{"image",
                               {{"format", bedrock_image_format(image.media_type)},
                                {"source", {{"bytes", image.data}}}}}});
        previous_was_unsupported_image_placeholder = false;
        break;
      }
      case ContentKind::thinking: {
        const auto& thinking = std::get<ThinkingContent>(item);
        const auto text = provider_safe_text(thinking.text);
        if (!same_provider_model) {
          if (!thinking.redacted && !trim_ascii(text).empty()) {
            blocks.push_back(Json{{"text", text}});
            previous_was_unsupported_image_placeholder = false;
          }
          break;
        }
        if (!trim_ascii(text).empty()) {
          if (supports_thinking_signature &&
              (!thinking.signature || trim_ascii(*thinking.signature).empty())) {
            blocks.push_back(Json{{"text", text}});
          } else {
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
        }
        break;
      }
      case ContentKind::tool_call: {
        if (role != MessageRole::assistant) {
          break;
        }
        const auto& call = std::get<ToolCallContent>(item);
        std::string id = call.id;
        if (!same_provider_model) {
          id = google_tool_call_id(call.id);
          if (tool_call_id_map != nullptr && id != call.id) {
            (*tool_call_id_map)[call.id] = id;
          }
        }
        blocks.push_back(Json{{"toolUse",
                               {{"toolUseId", std::move(id)},
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
        if (!supports_images && !payload.images.empty()) {
          result_blocks.push_back(
              Json{{"text",
                    "(tool image omitted: model does not support images)"}});
        } else {
          for (const auto& image : payload.images) {
            result_blocks.push_back(
                Json{{"image",
                      {{"format", bedrock_image_format(image.media_type)},
                       {"source", {{"bytes", image.data}}}}}});
          }
        }
        const auto mapped_id =
            tool_call_id_map != nullptr &&
                    tool_call_id_map->count(result.tool_call_id) > 0
                ? tool_call_id_map->at(result.tool_call_id)
                : result.tool_call_id;
        blocks.push_back(Json{{"toolResult",
                               {{"toolUseId", mapped_id},
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

std::optional<Json> bedrock_tool_config_from_options_metadata(
    const std::vector<ToolDefinition>& tools, const Json& metadata) {
  if (!metadata.is_object() || !metadata.contains("toolChoice")) {
    return std::nullopt;
  }
  const auto& tool_choice = metadata.at("toolChoice");
  if (tools.empty()) {
    return Json(nullptr);
  }
  if (tool_choice.is_string() && tool_choice.get<std::string>() == "none") {
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
  if (tool_choice.is_string()) {
    const auto value = tool_choice.get<std::string>();
    if (value == "auto") {
      config["toolChoice"] = Json{{"auto", Json::object()}};
    } else if (value == "any") {
      config["toolChoice"] = Json{{"any", Json::object()}};
    }
  } else if (tool_choice.is_object() &&
             tool_choice.value("type", std::string()) == "tool" &&
             tool_choice.contains("name") && tool_choice.at("name").is_string()) {
    config["toolChoice"] =
        Json{{"tool", {{"name", tool_choice.at("name").get<std::string>()}}}};
  }
  return config;
}

Json google_sanitize_openapi_schema(const Json& schema) {
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

Json mistral_text_content(const std::vector<Content>& content) {
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

bool mistral_supports_images(const CompletionRequest& request,
                             const Json& options_metadata) {
  const auto has_image_input = [](const Json& metadata) {
    if (!metadata.is_object() || !metadata.contains("input") ||
        !metadata.at("input").is_array()) {
      return false;
    }
    for (const auto& value : metadata.at("input")) {
      if (value.is_string() && value.get<std::string>() == "image") {
        return true;
      }
    }
    return false;
  };
  return has_image_input(request.metadata) || has_image_input(options_metadata);
}

Json mistral_user_content(const std::vector<Content>& content,
                          bool supports_images) {
  Json parts = Json::array();
  bool saw_image = false;
  bool saw_non_text = false;
  std::string text_only;

  for (const auto& item : content) {
    switch (content_kind(item)) {
      case ContentKind::text: {
        const auto text = provider_safe_text(std::get<TextContent>(item).text);
        if (trim_ascii(text).empty()) {
          break;
        }
        if (!text_only.empty()) {
          text_only += "\n";
        }
        text_only += text;
        parts.push_back(Json{{"type", "text"}, {"text", text}});
        break;
      }
      case ContentKind::image: {
        saw_image = true;
        saw_non_text = true;
        if (supports_images) {
          const auto image = normalized(std::get<ImageContent>(item));
          parts.push_back(Json{{"type", "image_url"},
                               {"imageUrl", image_data_url(image)}});
        }
        break;
      }
      case ContentKind::thinking:
      case ContentKind::tool_call:
      case ContentKind::tool_result:
        saw_non_text = true;
        break;
    }
  }

  if (!parts.empty()) {
    if (!saw_non_text && parts.size() == 1U) {
      return text_only;
    }
    return parts;
  }
  if (saw_image && !supports_images) {
    return "(image omitted: model does not support images)";
  }
  return Json(nullptr);
}

Json mistral_assistant_content(const std::vector<Content>& content) {
  Json parts = Json::array();
  for (const auto& item : content) {
    switch (content_kind(item)) {
      case ContentKind::text: {
        const auto text = provider_safe_text(std::get<TextContent>(item).text);
        if (!trim_ascii(text).empty()) {
          parts.push_back(Json{{"type", "text"}, {"text", text}});
        }
        break;
      }
      case ContentKind::thinking: {
        const auto text =
            provider_safe_text(std::get<ThinkingContent>(item).text);
        if (!trim_ascii(text).empty()) {
          parts.push_back(
              Json{{"type", "thinking"},
                   {"thinking", Json::array({Json{{"type", "text"},
                                                   {"text", text}}})}});
        }
        break;
      }
      case ContentKind::image:
      case ContentKind::tool_call:
      case ContentKind::tool_result:
        break;
    }
  }
  return parts;
}

std::string mistral_alphanumeric(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if (std::isalnum(ch)) {
      result.push_back(static_cast<char>(ch));
    }
  }
  return result;
}

std::string derive_mistral_tool_call_id(std::string_view id, int attempt) {
  constexpr std::size_t kMistralToolCallIdLength = 9;
  auto normalized = mistral_alphanumeric(id);
  if (attempt == 0 && normalized.size() == kMistralToolCallIdLength) {
    return normalized;
  }
  const std::string seed_base = normalized.empty() ? std::string(id) : normalized;
  const auto seed =
      attempt == 0 ? seed_base : seed_base + ":" + std::to_string(attempt);
  const auto hashed = mistral_alphanumeric(short_hash(seed));
  return hashed.substr(0, kMistralToolCallIdLength);
}

class MistralToolCallIdNormalizer {
 public:
  std::string operator()(std::string_view id) {
    const std::string key(id);
    if (const auto existing = id_map_.find(key); existing != id_map_.end()) {
      return existing->second;
    }

    for (int attempt = 0;; ++attempt) {
      const auto candidate = derive_mistral_tool_call_id(key, attempt);
      const auto owner = reverse_map_.find(candidate);
      if (owner == reverse_map_.end() || owner->second == key) {
        id_map_[key] = candidate;
        reverse_map_[candidate] = key;
        return candidate;
      }
    }
  }

  std::optional<std::string> lookup(std::string_view id) const {
    const std::string key(id);
    if (const auto existing = id_map_.find(key); existing != id_map_.end()) {
      return existing->second;
    }
    return std::nullopt;
  }

 private:
  std::map<std::string, std::string> id_map_;
  std::map<std::string, std::string> reverse_map_;
};

Json mistral_tool_calls(const std::vector<Content>& content,
                        MistralToolCallIdNormalizer& normalize_id,
                        bool same_provider_model) {
  Json calls = Json::array();
  for (const auto& item : content) {
    if (content_kind(item) != ContentKind::tool_call) {
      continue;
    }
    const auto& call = std::get<ToolCallContent>(item);
    const auto id =
        same_provider_model ? call.id : normalize_id(call.id);
    calls.push_back(
        Json{{"id", id},
             {"type", "function"},
             {"function",
              {{"name", call.name},
               {"arguments", provider_safe_json_dump(call.arguments)}}}});
  }
  return calls;
}

std::string mistral_tool_result_text(const ToolResultPayload& payload,
                                     bool has_images,
                                     bool supports_images,
                                     bool is_error) {
  const std::string prefix = is_error ? "[tool error] " : "";
  const auto trimmed = trim_ascii(payload.text);
  if (!trimmed.empty()) {
    const std::string suffix =
        has_images && !supports_images
            ? "\n[tool image omitted: model does not support images]"
            : "";
    return prefix + payload.text + suffix;
  }
  if (has_images) {
    if (supports_images) {
      return is_error ? "[tool error] (see attached image)"
                      : "(see attached image)";
    }
    return is_error ? "[tool error] (image omitted: model does not support images)"
                    : "(image omitted: model does not support images)";
  }
  return is_error ? "[tool error] (no tool output)" : "(no tool output)";
}

Json mistral_tool_content(const ToolResultContent& result,
                          bool supports_images) {
  const auto payload = tool_result_payload(result);
  const bool has_images = !payload.images.empty();
  Json content = Json::array(
      {Json{{"type", "text"},
            {"text", mistral_tool_result_text(payload, has_images,
                                              supports_images,
                                              result.is_error)}}});
  if (supports_images) {
    for (const auto& image : payload.images) {
      content.push_back(Json{{"type", "image_url"},
                             {"imageUrl", image_data_url(image)}});
    }
  }
  return content;
}

Json mistral_tool(const ToolDefinition& tool) {
  return Json{{"type", "function"},
              {"function",
               {{"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.input_schema},
                {"strict", false}}}};
}

bool mistral_uses_reasoning_effort(const std::string& model_id) {
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

Json mistral_tool_choice(const ToolChoice& tool_choice) {
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

  throw std::invalid_argument("unmapped Mistral tool choice");
}

std::optional<Json> mistral_tool_choice_from_options_metadata(
    const Json& metadata) {
  if (!metadata.is_object() || !metadata.contains("toolChoice")) {
    return std::nullopt;
  }
  const auto& tool_choice = metadata.at("toolChoice");
  if (tool_choice.is_string()) {
    auto value = tool_choice.get<std::string>();
    if (value == "auto" || value == "none" || value == "any" ||
        value == "required") {
      return value;
    }
    return std::nullopt;
  }
  if (tool_choice.is_object()) {
    return tool_choice;
  }
  return std::nullopt;
}

Json openai_completions_tool_choice(const ToolChoice& tool_choice) {
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

std::optional<Json> openai_tool_choice_from_options_metadata(
    const Json& metadata) {
  if (!metadata.is_object() || !metadata.contains("toolChoice")) {
    return std::nullopt;
  }
  const auto& tool_choice = metadata.at("toolChoice");
  if (tool_choice.is_string()) {
    auto value = tool_choice.get<std::string>();
    if (value == "auto" || value == "none" || value == "required") {
      return value;
    }
    return std::nullopt;
  }
  if (tool_choice.is_object()) {
    return tool_choice;
  }
  return std::nullopt;
}

Json openai_metadata_object(const Json& metadata) {
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

Json openai_extra_options(const Json& metadata) {
  if (!metadata.is_object()) {
    return Json::object();
  }
  const auto extra = metadata.value("openai", Json::object());
  return extra.is_object() ? extra : Json::object();
}

std::string openai_responses_failed_message(const Json& response) {
  const auto error = response.value("error", Json::object());
  if (!error.empty()) {
    return error.value("code", std::string("unknown")) + ": " +
           error.value("message", std::string("no message"));
  }
  const auto details = response.value("incomplete_details", Json::object());
  if (details.contains("reason") && details.at("reason").is_string()) {
    return "incomplete: " + details.at("reason").get<std::string>();
  }
  return "Unknown error (no error details in response)";
}

std::string max_tokens_field_from_options_metadata(const Json& metadata) {
  const auto compat = openai_completions_compat_from_metadata(metadata);
  if (!compat.is_object()) {
    return "max_completion_tokens";
  }
  auto field = compat.value("maxTokensField", std::string());
  if (field == "max_tokens" || field == "max_completion_tokens") {
    return field;
  }
  return "max_completion_tokens";
}

std::optional<std::int64_t> positive_token_limit(const Json& metadata,
                                                 const char* key) {
  if (!metadata.is_object() || !metadata.contains(key) ||
      !metadata.at(key).is_number_integer()) {
    return std::nullopt;
  }
  const auto value = metadata.at(key).get<std::int64_t>();
  return value > 0 ? std::optional<std::int64_t>(value) : std::nullopt;
}

std::optional<std::int64_t> bounded_openai_completion_tokens(
    const CompletionRequest& request, const Json& options_metadata) {
  if (!request.max_output_tokens || *request.max_output_tokens <= 0) {
    return std::nullopt;
  }

  std::int64_t bounded = *request.max_output_tokens;
  if (const auto model_limit =
          positive_token_limit(options_metadata, "maxTokens")) {
    bounded = std::min(bounded, *model_limit);
  }
  if (const auto context_window =
          positive_token_limit(options_metadata, "contextWindow")) {
    const auto estimated_input = estimate_token_count(request);
    const auto remaining = std::max<std::int64_t>(1, *context_window -
                                                         estimated_input);
    bounded = std::min(bounded, remaining);
  }
  return std::max<std::int64_t>(1, bounded);
}

Json openai_completions_compat_from_metadata(const Json& metadata) {
  Json detected = Json{{"supportsStore", true},
                       {"supportsDeveloperRole", true},
                       {"supportsReasoningEffort", true},
                       {"supportsUsageInStreaming", true},
                       {"maxTokensField", "max_completion_tokens"},
                       {"requiresToolResultName", false},
                       {"requiresAssistantAfterToolResult", false},
                       {"requiresThinkingAsText", false},
                       {"requiresReasoningContentOnAssistantMessages", false},
                       {"thinkingFormat", "openai"},
                       {"supportsStrictMode", true},
                       {"supportsLongCacheRetention", true}};
  if (!metadata.is_object()) {
    return detected;
  }
  const auto provider = provider_key(
      string_metadata_value(metadata, "provider").value_or(std::string()));
  const auto base_url = string_metadata_value(metadata, "baseUrl")
                            .value_or(std::string());
  const auto contains_base = [&base_url](std::string_view needle) {
    return base_url.find(needle) != std::string::npos;
  };

  const bool is_zai = provider == "zai" || contains_base("api.z.ai");
  const bool is_together = provider == "together" ||
                           contains_base("api.together.ai") ||
                           contains_base("api.together.xyz");
  const bool is_moonshot = provider == "moonshotai" ||
                           provider == "moonshotai-cn" ||
                           contains_base("api.moonshot.");
  const bool is_cloudflare_workers =
      provider == "cloudflare-workers-ai" || contains_base("api.cloudflare.com");
  const bool is_cloudflare_gateway =
      provider == "cloudflare-ai-gateway" ||
      contains_base("gateway.ai.cloudflare.com");
  const bool is_grok = provider == "xai" || contains_base("api.x.ai");
  const bool is_deepseek =
      provider == "deepseek" || contains_base("deepseek.com");
  const bool is_openrouter =
      provider == "openrouter" || contains_base("openrouter.ai");
  const auto model_id =
      string_metadata_value(metadata, "model").value_or(std::string());
  const bool is_nonstandard =
      provider == "cerebras" || contains_base("cerebras.ai") || is_grok ||
      is_together || contains_base("chutes.ai") || is_deepseek || is_zai ||
      is_moonshot || provider == "opencode" || contains_base("opencode.ai") ||
      is_cloudflare_workers || is_cloudflare_gateway;

  if (is_nonstandard) {
    detected["supportsStore"] = false;
    detected["supportsDeveloperRole"] = false;
  }
  if (contains_base("chutes.ai") || is_moonshot || is_cloudflare_gateway ||
      is_together) {
    detected["maxTokensField"] = "max_tokens";
  }
  if (is_grok || is_zai || is_moonshot || is_together ||
      is_cloudflare_gateway) {
    detected["supportsReasoningEffort"] = false;
  }
  if (is_deepseek) {
    detected["requiresReasoningContentOnAssistantMessages"] = true;
    detected["thinkingFormat"] = "deepseek";
  } else if (is_zai) {
    detected["thinkingFormat"] = "zai";
  } else if (is_together) {
    detected["thinkingFormat"] = "together";
  } else if (is_openrouter) {
    detected["thinkingFormat"] = "openrouter";
  }
  if (is_moonshot || is_together || is_cloudflare_gateway) {
    detected["supportsStrictMode"] = false;
  }
  if (is_together || is_cloudflare_workers || is_cloudflare_gateway) {
    detected["supportsLongCacheRetention"] = false;
  }
  if (provider == "openrouter" && model_id.rfind("anthropic/", 0) == 0) {
    detected["cacheControlFormat"] = "anthropic";
  }

  const auto compat = metadata.value("compat", Json::object());
  if (compat.is_object()) {
    for (const auto& entry : compat.items()) {
      if (entry.value().is_null()) {
        continue;
      }
      detected[entry.key()] = entry.value();
    }
  }
  return detected;
}

Json openai_responses_compat_from_metadata(const Json& metadata) {
  return openai_completions_compat_from_metadata(metadata);
}

Json anthropic_messages_compat_from_metadata(const Json& metadata) {
  Json detected = Json{{"supportsEagerToolInputStreaming", true},
                       {"supportsLongCacheRetention", true},
                       {"sendSessionAffinityHeaders", false},
                       {"supportsCacheControlOnTools", true}};
  if (!metadata.is_object()) {
    return detected;
  }

  const auto provider = provider_key(
      string_metadata_value(metadata, "provider").value_or(std::string()));
  const auto base_url = string_metadata_value(metadata, "baseUrl")
                            .value_or(std::string());
  const bool is_fireworks = provider == "fireworks";
  const bool is_cloudflare_gateway_anthropic =
      provider == "cloudflare-ai-gateway" &&
      base_url.find("anthropic") != std::string::npos;

  if (is_fireworks) {
    detected["supportsEagerToolInputStreaming"] = false;
    detected["supportsLongCacheRetention"] = false;
    detected["supportsCacheControlOnTools"] = false;
  }
  if (is_fireworks || is_cloudflare_gateway_anthropic) {
    detected["sendSessionAffinityHeaders"] = true;
  }

  const auto compat = metadata.value("compat", Json::object());
  if (compat.is_object()) {
    for (const auto& entry : compat.items()) {
      if (entry.value().is_null()) {
        continue;
      }
      detected[entry.key()] = entry.value();
    }
  }
  return detected;
}

bool compat_flag(const Json& compat, const std::string& key,
                 bool default_value) {
  if (!compat.is_object()) {
    return default_value;
  }
  const auto it = compat.find(key);
  if (it == compat.end() || !it->is_boolean()) {
    return default_value;
  }
  return it->get<bool>();
}

bool openai_completions_supports_store(const Json& options_metadata,
                                       const Json& compat) {
  if (compat.is_object()) {
    const auto it = compat.find("supportsStore");
    if (it != compat.end() && it->is_boolean()) {
      return it->get<bool>();
    }
  }

  const auto base_url = string_metadata_value(options_metadata, "baseUrl");
  if (!base_url) {
    return true;
  }
  static const std::array<std::string_view, 10> nonstandard_hosts = {
      "cerebras.ai",
      "api.x.ai",
      "api.together.ai",
      "api.together.xyz",
      "chutes.ai",
      "deepseek.com",
      "api.z.ai",
      "api.moonshot.",
      "opencode.ai",
      "gateway.ai.cloudflare.com"};
  for (const auto host : nonstandard_hosts) {
    if (base_url->find(host) != std::string::npos) {
      return false;
    }
  }
  return base_url->find("api.cloudflare.com") == std::string::npos;
}

std::string cache_retention_from_options_metadata(const Json& metadata) {
  if (metadata.is_object() && metadata.contains("cacheRetention") &&
      metadata.at("cacheRetention").is_string()) {
    const auto retention = metadata.at("cacheRetention").get<std::string>();
    if (retention == "none" || retention == "long") {
      return retention;
    }
  }
  if (env_string("PI_CACHE_RETENTION") == std::optional<std::string>{"long"}) {
    return "long";
  }
  return "short";
}

bool supports_long_cache_retention(const Json& compat) {
  if (!compat.is_object()) {
    return true;
  }
  return compat.value("supportsLongCacheRetention", true);
}

bool metadata_reasoning_enabled(const Json& metadata) {
  return metadata.is_object() && metadata.contains("reasoning") &&
         metadata.at("reasoning").is_boolean() &&
         metadata.at("reasoning").get<bool>();
}

std::optional<std::string> off_thinking_level(const Json& options_metadata) {
  if (options_metadata.is_object() &&
      options_metadata.contains("thinkingLevelMap") &&
      options_metadata.at("thinkingLevelMap").is_object()) {
    const auto& map = options_metadata.at("thinkingLevelMap");
    if (map.contains("off")) {
      if (map.at("off").is_null()) {
        return std::nullopt;
      }
      if (map.at("off").is_string()) {
        return map.at("off").get<std::string>();
      }
    }
  }
  return std::string("none");
}

std::optional<std::string> string_metadata_value(const Json& metadata,
                                                 const std::string& key) {
  if (!metadata.is_object() || !metadata.contains(key) ||
      !metadata.at(key).is_string()) {
    return std::nullopt;
  }
  return metadata.at(key).get<std::string>();
}

std::optional<std::string> requested_thinking_level(const Json& metadata) {
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

  const bool azure_responses =
      provider_key(request.model.provider) == "azure-openai-responses";
  if (session_id && (retention != "none" || azure_responses)) {
    body["prompt_cache_key"] = *session_id;
  }
  if (!azure_responses && retention == "long" &&
      supports_long_cache_retention(compat)) {
    body["prompt_cache_retention"] = "24h";
  }

  auto level = requested_thinking_level(request.metadata);
  if (!level && metadata_reasoning_enabled(options_metadata)) {
    level = string_metadata_value(options_metadata, "reasoningEffort");
  }
  if (level && !level->empty() && *level != "off") {
    const auto effort =
        mapped_thinking_level(*level, options_metadata).value_or(*level);
    body["reasoning"] =
        Json{{"effort", effort},
             {"summary",
              options_metadata.value("reasoningSummary", std::string("auto"))}};
    body["include"] = Json::array({"reasoning.encrypted_content"});
  } else if (options_metadata.is_object() &&
             options_metadata.contains("reasoningSummary") &&
             metadata_reasoning_enabled(options_metadata)) {
    body["reasoning"] =
        Json{{"effort", "medium"},
             {"summary",
              options_metadata.value("reasoningSummary", std::string("auto"))}};
    body["include"] = Json::array({"reasoning.encrypted_content"});
  } else if (!level && metadata_reasoning_enabled(options_metadata) &&
             provider_key(request.model.provider) != "github-copilot") {
    if (const auto effort = off_thinking_level(options_metadata)) {
      body["reasoning"] = Json{{"effort", *effort}};
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
  auto level = requested_thinking_level(request.metadata);
  if (!level) {
    level = string_metadata_value(options_metadata, "reasoningEffort");
  }
  const auto compat = openai_completions_compat_from_metadata(options_metadata);
  const bool metadata_has_reasoning =
      options_metadata.is_object() && options_metadata.contains("reasoning") &&
      options_metadata.at("reasoning").is_boolean();
  const bool model_supports_reasoning =
      !metadata_has_reasoning || options_metadata.value("reasoning", false);
  if (!model_supports_reasoning) {
    return;
  }
  const bool default_reasoning_controls = metadata_has_reasoning &&
                                          options_metadata.value("reasoning", false);
  const auto thinking_format =
      compat.value("thinkingFormat", std::string("openai"));
  const bool thinking_enabled = level && !level->empty() && *level != "off";
  const auto effort = thinking_enabled
                          ? mapped_thinking_level(*level, options_metadata)
                                .value_or(*level)
                          : std::string();

  if (thinking_format == "zai" || thinking_format == "qwen") {
    body["enable_thinking"] = thinking_enabled;
    return;
  }
  if (thinking_format == "qwen-chat-template") {
    body["chat_template_kwargs"] =
        Json{{"enable_thinking", thinking_enabled},
             {"preserve_thinking", true}};
    return;
  }
  if (thinking_format == "deepseek") {
    if (thinking_enabled || default_reasoning_controls ||
        !metadata_has_reasoning) {
      body["thinking"] =
          Json{{"type", thinking_enabled ? "enabled" : "disabled"}};
    }
    if (thinking_enabled) {
      body["reasoning_effort"] = effort;
    }
    return;
  }

  if (!thinking_enabled) {
    if (!default_reasoning_controls) {
      return;
    }
    if (thinking_format == "openrouter") {
      if (const auto off = off_thinking_level(options_metadata)) {
        body["reasoning"] = Json{{"effort", *off}};
      }
    } else if (thinking_format == "together") {
      body["reasoning"] = Json{{"enabled", false}};
    } else if (compat.value("supportsReasoningEffort", false)) {
      if (const auto off = off_thinking_level(options_metadata)) {
        body["reasoning_effort"] = *off;
      }
    }
    return;
  }
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
  if (!base_url) {
    return;
  }
  const auto compat = options_metadata.value("compat", Json::object());
  if (!compat.is_object()) {
    return;
  }
  if (base_url->find("openrouter.ai") != std::string::npos &&
      compat.contains("openRouterRouting") &&
      compat.at("openRouterRouting").is_object()) {
    body["provider"] = compat.at("openRouterRouting");
  }
  if (base_url->find("ai-gateway.vercel.sh") != std::string::npos &&
      compat.contains("vercelGatewayRouting") &&
      compat.at("vercelGatewayRouting").is_object()) {
    Json gateway = Json::object();
    const auto& routing = compat.at("vercelGatewayRouting");
    if (routing.contains("only") && routing.at("only").is_array()) {
      gateway["only"] = routing.at("only");
    }
    if (routing.contains("order") && routing.at("order").is_array()) {
      gateway["order"] = routing.at("order");
    }
    if (!gateway.empty()) {
      body["providerOptions"] = Json{{"gateway", std::move(gateway)}};
    }
  }
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

double openai_service_tier_cost_multiplier(const std::string& model_id,
                                           const std::string& service_tier) {
  if (service_tier == "flex") {
    return 0.5;
  }
  if (service_tier == "priority") {
    return model_id == "gpt-5.5" ? 2.5 : 2.0;
  }
  return 1.0;
}

std::string resolve_openai_codex_service_tier(
    const std::optional<std::string>& response_tier,
    const std::optional<std::string>& request_tier) {
  if (response_tier == std::optional<std::string>{"default"} &&
      (request_tier == std::optional<std::string>{"flex"} ||
       request_tier == std::optional<std::string>{"priority"})) {
    return *request_tier;
  }
  return response_tier.value_or(request_tier.value_or(""));
}

void apply_cost_multiplier(Usage& usage, double multiplier) {
  if (multiplier == 1.0) {
    return;
  }
  usage.input_cost_usd *= multiplier;
  usage.output_cost_usd *= multiplier;
  usage.cache_read_cost_usd *= multiplier;
  usage.cache_write_cost_usd *= multiplier;
  usage.total_cost_usd = usage.input_cost_usd + usage.output_cost_usd +
                         usage.cache_read_cost_usd +
                         usage.cache_write_cost_usd;
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

void apply_openai_service_tier_accounting(CompletionResponse& response,
                                          const ProviderOptions& options,
                                          const std::string& model_id) {
  apply_accounting(response, options);
  const auto response_tier =
      non_empty_string_field(response.metadata, "service_tier");
  const auto request_tier = service_tier_from_metadata(options.metadata);
  const auto service_tier = response_tier.value_or(request_tier.value_or(""));
  apply_cost_multiplier(
      response.usage,
      openai_service_tier_cost_multiplier(model_id, service_tier));
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

void apply_openai_service_tier_accounting(std::vector<StreamEvent>& events,
                                          const ProviderOptions& options,
                                          const std::string& model_id) {
  const auto request_tier = service_tier_from_metadata(options.metadata);
  const auto rates = cost_rates_from_options(options);
  for (auto& event : events) {
    if (!event.usage) {
      continue;
    }
    if (rates) {
      apply_cost(*event.usage, *rates);
    } else {
      normalize(*event.usage);
    }
    const auto response_tier =
        non_empty_string_field(event.metadata, "service_tier");
    const auto service_tier = response_tier.value_or(request_tier.value_or(""));
    apply_cost_multiplier(
        *event.usage,
        openai_service_tier_cost_multiplier(model_id, service_tier));
  }
}

void apply_openai_codex_service_tier_accounting(std::vector<StreamEvent>& events,
                                                const ProviderOptions& options,
                                                const std::string& model_id) {
  const auto request_tier = service_tier_from_metadata(options.metadata);
  const auto rates = cost_rates_from_options(options);
  for (auto& event : events) {
    if (!event.usage) {
      continue;
    }
    if (rates) {
      apply_cost(*event.usage, *rates);
    } else {
      normalize(*event.usage);
    }
    const auto response_tier =
        non_empty_string_field(event.metadata, "service_tier");
    const auto service_tier =
        resolve_openai_codex_service_tier(response_tier, request_tier);
    apply_cost_multiplier(
        *event.usage,
        openai_service_tier_cost_multiplier(model_id, service_tier));
  }
}

HttpResponse send_with_retries(HttpTransport& transport, HttpRequest& http,
                               const ProviderOptions& options) {
  const int max_retries = std::max(0, options.max_retries.value_or(0));
  const auto max_retry_delay_ms = options.max_retry_delay_ms.value_or(60000);
  http.abort_signal = options.abort_signal;

  for (int attempt = 0;; ++attempt) {
    if (options.aborted ||
        (options.abort_signal &&
         options.abort_signal->load(std::memory_order_acquire))) {
      throw std::runtime_error("provider request aborted");
    }
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

BedrockThinkingTokenPlan bedrock_adjust_max_tokens_for_thinking(
    std::int64_t base_max_tokens,
    std::int64_t model_max_tokens,
    std::string_view thinking_level,
    const Json& custom_budgets) {
  const std::string level =
      thinking_level == "xhigh" ? "high" : std::string(thinking_level);

  auto budget_for = [&custom_budgets](const std::string& key,
                                      std::int64_t fallback) {
    if (custom_budgets.is_object() && custom_budgets.contains(key) &&
        custom_budgets.at(key).is_number_integer()) {
      return custom_budgets.at(key).get<std::int64_t>();
    }
    return fallback;
  };

  std::int64_t thinking_budget = 16384;
  if (level == "minimal") {
    thinking_budget = budget_for("minimal", 1024);
  } else if (level == "low") {
    thinking_budget = budget_for("low", 2048);
  } else if (level == "medium") {
    thinking_budget = budget_for("medium", 8192);
  } else {
    thinking_budget = budget_for("high", 16384);
  }

  const std::int64_t max_tokens =
      std::min(base_max_tokens + thinking_budget, model_max_tokens);
  if (max_tokens <= thinking_budget) {
    constexpr std::int64_t kMinOutputTokens = 1024;
    thinking_budget = std::max<std::int64_t>(0, max_tokens - kMinOutputTokens);
  }

  return BedrockThinkingTokenPlan{max_tokens, thinking_budget};
}

StopReason google_stop_reason(std::string_view reason) {
  if (reason == "STOP") {
    return StopReason::end_turn;
  }
  if (reason == "MAX_TOKENS") {
    return StopReason::max_tokens;
  }
  return StopReason::error;
}

Usage google_usage_metadata(const Json& usage_metadata) {
  Usage usage;
  if (!usage_metadata.is_object()) {
    return usage;
  }
  const auto prompt_tokens =
      usage_metadata.value("promptTokenCount", std::int64_t{0});
  usage.cache_read_tokens =
      usage_metadata.value("cachedContentTokenCount", std::int64_t{0});
  usage.input_tokens = prompt_tokens - usage.cache_read_tokens;
  usage.output_tokens =
      usage_metadata.value("candidatesTokenCount", std::int64_t{0}) +
      usage_metadata.value("thoughtsTokenCount", std::int64_t{0});
  usage.total_tokens =
      usage_metadata.value("totalTokenCount", std::int64_t{0});
  return usage;
}

std::vector<StreamEvent> parse_google_generate_content_stream(
    std::string_view transcript) {
  std::vector<StreamEvent> events;
  std::vector<Json> chunks;
  const auto text = trim_ascii(transcript);
  if (text.empty()) {
    return events;
  }

  try {
    const auto parsed = Json::parse(sanitize_utf8(text));
    if (parsed.is_array()) {
      for (const auto& item : parsed) {
        if (item.is_object()) {
          chunks.push_back(item);
        }
      }
    } else if (parsed.is_object()) {
      chunks.push_back(parsed);
    }
  } catch (const Json::parse_error&) {
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
      const auto line_end = text.find('\n', line_start);
      const auto line =
          trim_ascii(std::string_view(text).substr(
              line_start, line_end == std::string::npos
                              ? std::string::npos
                              : line_end - line_start));
      if (!line.empty()) {
        try {
          const auto parsed_line = Json::parse(sanitize_utf8(line));
          if (parsed_line.is_object()) {
            chunks.push_back(parsed_line);
          }
        } catch (const Json::parse_error&) {
          StreamEvent event;
          event.kind = EventKind::error;
          event.error = "invalid Google stream JSON payload";
          event.metadata = Json{{"data", line}};
          events.push_back(std::move(event));
        }
      }
      if (line_end == std::string::npos) {
        break;
      }
      line_start = line_end + 1;
    }
  }

  struct CurrentBlock {
    ContentKind kind = ContentKind::text;
    std::string text;
    std::optional<std::string> signature;
  };

  bool saw_start = false;
  bool saw_tool_call = false;
  std::optional<std::string> provider_response_id;
  std::optional<CurrentBlock> current_block;
  std::map<std::string, int> fallback_tool_id_counts;
  std::map<std::string, bool> used_tool_ids;
  std::optional<StopReason> final_stop_reason;

  const auto provider_metadata = [&provider_response_id]() {
    Json metadata = Json{{"provider_type", "google.generate_content.chunk"}};
    if (provider_response_id) {
      metadata["provider_response_id"] = *provider_response_id;
    }
    return metadata;
  };

  const auto ensure_start = [&events, &saw_start, &provider_response_id,
                             &provider_metadata]() {
    if (saw_start) {
      return;
    }
    StreamEvent event;
    event.kind = EventKind::message_start;
    event.role = MessageRole::assistant;
    if (provider_response_id) {
      event.message_id = provider_response_id;
    }
    event.metadata = provider_metadata();
    events.push_back(std::move(event));
    saw_start = true;
  };

  const auto finish_current_block = [&events, &current_block,
                                     &provider_metadata]() {
    if (!current_block) {
      return;
    }
    StreamEvent event;
    event.kind = EventKind::content_done;
    event.metadata = provider_metadata();
    if (current_block->kind == ContentKind::thinking) {
      event.content =
          ThinkingContent{current_block->text, current_block->signature, false};
      event.metadata["content_type"] = "thinking";
    } else {
      event.content = TextContent{current_block->text, current_block->signature};
      event.metadata["content_type"] = "text";
    }
    events.push_back(std::move(event));
    current_block.reset();
  };

  const auto fallback_tool_id = [&fallback_tool_id_counts,
                                 &used_tool_ids](std::string name) {
    if (name.empty()) {
      name = "tool";
    }
    std::string candidate;
    do {
      const auto count = ++fallback_tool_id_counts[name];
      candidate = name + "_" + std::to_string(count);
    } while (used_tool_ids.count(candidate) > 0);
    return candidate;
  };

  for (const auto& chunk : chunks) {
    if (const auto id = non_empty_string_field(chunk, "responseId");
        id && !provider_response_id) {
      provider_response_id = id;
    }
    ensure_start();

    if (chunk.contains("usageMetadata") &&
        chunk.at("usageMetadata").is_object()) {
      StreamEvent event;
      event.kind = EventKind::usage;
      event.usage = google_usage_metadata(chunk.at("usageMetadata"));
      event.metadata = provider_metadata();
      events.push_back(std::move(event));
    }

    const auto candidates = chunk.value("candidates", Json::array());
    if (candidates.empty() || !candidates.front().is_object()) {
      continue;
    }
    const auto& candidate = candidates.front();
    const auto parts = candidate.value("content", Json::object())
                           .value("parts", Json::array());
    for (const auto& part : parts) {
      if (!part.is_object()) {
        continue;
      }
      if (part.contains("text") && part.at("text").is_string()) {
        const bool is_thinking = google_is_thinking_part(part);
        const auto kind = is_thinking ? ContentKind::thinking
                                      : ContentKind::text;
        if (!current_block || current_block->kind != kind) {
          finish_current_block();
          current_block = CurrentBlock{kind, "", std::nullopt};
        }

        const auto delta = provider_safe_text(part.at("text").get<std::string>());
        current_block->text += delta;
        if (const auto signature =
                non_empty_string_field(part, "thoughtSignature")) {
          current_block->signature = google_retain_thought_signature(
              current_block->signature, signature);
        }
        if (!delta.empty()) {
          StreamEvent event;
          event.kind = EventKind::content_delta;
          event.delta = delta;
          event.metadata = provider_metadata();
          if (is_thinking) {
            event.content =
                ThinkingContent{delta, current_block->signature, false};
            event.metadata["content_type"] = "thinking";
          } else {
            event.content = TextContent{delta, current_block->signature};
            event.metadata["content_type"] = "text";
          }
          events.push_back(std::move(event));
        }
      }

      if (part.contains("functionCall") && part.at("functionCall").is_object()) {
        finish_current_block();
        const auto& function_call = part.at("functionCall");
        const auto name = function_call.value("name", std::string());
        const auto provided_id = non_empty_string_field(function_call, "id");
        const bool duplicate_id =
            provided_id && used_tool_ids.count(*provided_id) > 0;
        const auto id = provided_id && !duplicate_id
                            ? *provided_id
                            : fallback_tool_id(name);
        used_tool_ids[id] = true;
        auto arguments = function_call.value("args", Json::object());
        if (!arguments.is_object()) {
          arguments = Json::object();
        }

        std::optional<std::string> thought_signature;
        if (const auto signature =
                non_empty_string_field(part, "thoughtSignature")) {
          thought_signature = signature;
        }

        StreamEvent event;
        event.kind = EventKind::tool_call;
        event.content = ToolCallContent{id, name, arguments, thought_signature};
        event.delta = arguments.dump();
        event.metadata = provider_metadata();
        event.metadata["content_type"] = "tool_call";
        events.push_back(std::move(event));
        saw_tool_call = true;
      }
    }

    if (const auto finish_reason =
            non_empty_string_field(candidate, "finishReason")) {
      final_stop_reason =
          saw_tool_call ? StopReason::tool_call : google_stop_reason(*finish_reason);
    }
  }

  finish_current_block();
  if (final_stop_reason) {
    StreamEvent done;
    done.kind = EventKind::message_done;
    done.stop_reason = final_stop_reason;
    done.metadata = provider_metadata();
    if (provider_response_id) {
      done.message_id = provider_response_id;
    }
    events.push_back(std::move(done));
  }
  return events;
}

StopReason bedrock_stop_reason(std::string_view reason) {
  if (reason == "end_turn" || reason == "stop_sequence") {
    return StopReason::end_turn;
  }
  if (reason == "max_tokens" ||
      reason == "model_context_window_exceeded") {
    return StopReason::max_tokens;
  }
  if (reason == "tool_use") {
    return StopReason::tool_call;
  }
  return StopReason::error;
}

Usage bedrock_usage_metadata(const Json& usage_metadata) {
  Usage usage;
  if (!usage_metadata.is_object()) {
    return usage;
  }
  usage.input_tokens = usage_metadata.value("inputTokens", std::int64_t{0});
  usage.output_tokens = usage_metadata.value("outputTokens", std::int64_t{0});
  usage.cache_read_tokens =
      usage_metadata.value("cacheReadInputTokens", std::int64_t{0});
  usage.cache_write_tokens =
      usage_metadata.value("cacheWriteInputTokens", std::int64_t{0});
  normalize(usage);
  return usage;
}

std::vector<StreamEvent> parse_bedrock_converse_stream(
    std::string_view transcript) {
  std::vector<StreamEvent> events;
  std::vector<Json> items;
  const auto text = trim_ascii(transcript);
  if (text.empty()) {
    return events;
  }

  try {
    const auto parsed = Json::parse(sanitize_utf8(text));
    if (parsed.is_array()) {
      for (const auto& item : parsed) {
        if (item.is_object()) {
          items.push_back(item);
        }
      }
    } else if (parsed.is_object()) {
      items.push_back(parsed);
    }
  } catch (const Json::parse_error&) {
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
      const auto line_end = text.find('\n', line_start);
      const auto line =
          trim_ascii(std::string_view(text).substr(
              line_start, line_end == std::string::npos
                              ? std::string::npos
                              : line_end - line_start));
      if (!line.empty()) {
        try {
          const auto parsed_line = Json::parse(sanitize_utf8(line));
          if (parsed_line.is_object()) {
            items.push_back(parsed_line);
          }
        } catch (const Json::parse_error&) {
          StreamEvent event;
          event.kind = EventKind::error;
          event.error = "invalid Bedrock stream JSON payload";
          event.metadata = Json{{"data", line}};
          events.push_back(std::move(event));
        }
      }
      if (line_end == std::string::npos) {
        break;
      }
      line_start = line_end + 1;
    }
  }

  struct PendingBlock {
    ContentKind kind = ContentKind::text;
    std::string text;
    std::optional<std::string> signature;
    ToolCallContent tool_call;
    std::string partial_json;
  };

  std::map<int, PendingBlock> blocks;
  bool saw_start = false;
  std::optional<StopReason> final_stop_reason;

  const auto provider_metadata = [] {
    return Json{{"provider_type", "bedrock.converse_stream"}};
  };

  const auto ensure_start = [&events, &saw_start, &provider_metadata]() {
    if (saw_start) {
      return;
    }
    StreamEvent event;
    event.kind = EventKind::message_start;
    event.role = MessageRole::assistant;
    event.metadata = provider_metadata();
    events.push_back(std::move(event));
    saw_start = true;
  };

  const auto block_index = [](const Json& event) {
    return event.value("contentBlockIndex", 0);
  };

  for (const auto& item : items) {
    if (item.contains("messageStart") && item.at("messageStart").is_object()) {
      const auto role = item.at("messageStart").value("role", std::string());
      if (!role.empty() && role != "assistant") {
        StreamEvent event;
        event.kind = EventKind::error;
        event.error =
            "Unexpected assistant message start but got user message start instead";
        event.metadata = provider_metadata();
        events.push_back(std::move(event));
      } else {
        ensure_start();
      }
      continue;
    }

    ensure_start();

    if (item.contains("contentBlockStart") &&
        item.at("contentBlockStart").is_object()) {
      const auto& event = item.at("contentBlockStart");
      const int index = block_index(event);
      const auto start = event.value("start", Json::object());
      if (start.contains("toolUse") && start.at("toolUse").is_object()) {
        const auto& tool_use = start.at("toolUse");
        auto& block = blocks[index];
        block.kind = ContentKind::tool_call;
        block.tool_call.id =
            tool_use.value("toolUseId", std::string());
        block.tool_call.name = tool_use.value("name", std::string());
        block.tool_call.arguments = Json::object();
        block.partial_json.clear();
      }
      continue;
    }

    if (item.contains("contentBlockDelta") &&
        item.at("contentBlockDelta").is_object()) {
      const auto& event = item.at("contentBlockDelta");
      const int index = block_index(event);
      const auto delta = event.value("delta", Json::object());

      if (delta.contains("text") && delta.at("text").is_string()) {
        auto& block = blocks[index];
        block.kind = ContentKind::text;
        const auto text_delta =
            provider_safe_text(delta.at("text").get<std::string>());
        block.text += text_delta;
        if (!text_delta.empty()) {
          StreamEvent stream_event;
          stream_event.kind = EventKind::content_delta;
          stream_event.delta = text_delta;
          stream_event.content = TextContent{text_delta, std::nullopt};
          stream_event.metadata = provider_metadata();
          stream_event.metadata["content_type"] = "text";
          stream_event.metadata["content_block_index"] = index;
          events.push_back(std::move(stream_event));
        }
        continue;
      }

      if (delta.contains("toolUse") && delta.at("toolUse").is_object()) {
        auto& block = blocks[index];
        if (block.kind != ContentKind::tool_call) {
          continue;
        }
        const auto input =
            delta.at("toolUse").value("input", std::string());
        block.partial_json += input;
        block.tool_call.arguments =
            parse_streamed_tool_arguments_or_raw(block.partial_json);
        if (!input.empty()) {
          StreamEvent stream_event;
          stream_event.kind = EventKind::content_delta;
          stream_event.delta = input;
          stream_event.metadata = provider_metadata();
          stream_event.metadata["content_type"] = "tool_call_arguments";
          stream_event.metadata["content_block_index"] = index;
          if (!block.tool_call.id.empty()) {
            stream_event.metadata["tool_call_id"] = block.tool_call.id;
          }
          events.push_back(std::move(stream_event));
        }
        continue;
      }

      if (delta.contains("reasoningContent") &&
          delta.at("reasoningContent").is_object()) {
        auto& block = blocks[index];
        block.kind = ContentKind::thinking;
        const auto& reasoning = delta.at("reasoningContent");
        if (reasoning.contains("text") && reasoning.at("text").is_string()) {
          const auto text_delta =
              provider_safe_text(reasoning.at("text").get<std::string>());
          block.text += text_delta;
          if (!text_delta.empty()) {
            StreamEvent stream_event;
            stream_event.kind = EventKind::content_delta;
            stream_event.delta = text_delta;
            stream_event.content =
                ThinkingContent{text_delta, block.signature, false};
            stream_event.metadata = provider_metadata();
            stream_event.metadata["content_type"] = "thinking";
            stream_event.metadata["content_block_index"] = index;
            events.push_back(std::move(stream_event));
          }
        }
        if (reasoning.contains("signature") &&
            reasoning.at("signature").is_string()) {
          const auto signature = reasoning.at("signature").get<std::string>();
          block.signature = block.signature.value_or(std::string()) + signature;
        }
        continue;
      }
    }

    if (item.contains("contentBlockStop") &&
        item.at("contentBlockStop").is_object()) {
      const int index = block_index(item.at("contentBlockStop"));
      auto block_it = blocks.find(index);
      if (block_it == blocks.end()) {
        continue;
      }
      auto block = std::move(block_it->second);
      blocks.erase(block_it);

      if (block.kind == ContentKind::text) {
        StreamEvent event;
        event.kind = EventKind::content_done;
        event.content = TextContent{block.text, std::nullopt};
        event.metadata = provider_metadata();
        event.metadata["content_type"] = "text";
        event.metadata["content_block_index"] = index;
        events.push_back(std::move(event));
      } else if (block.kind == ContentKind::thinking) {
        StreamEvent event;
        event.kind = EventKind::content_done;
        event.content = ThinkingContent{block.text, block.signature, false};
        event.metadata = provider_metadata();
        event.metadata["content_type"] = "thinking";
        event.metadata["content_block_index"] = index;
        events.push_back(std::move(event));
      } else if (block.kind == ContentKind::tool_call) {
        block.tool_call.arguments =
            parse_streamed_tool_arguments_or_raw(block.partial_json);
        StreamEvent event;
        event.kind = EventKind::tool_call;
        event.content = block.tool_call;
        event.metadata = provider_metadata();
        event.metadata["content_type"] = "tool_call";
        event.metadata["content_block_index"] = index;
        events.push_back(std::move(event));
      }
      continue;
    }

    if (item.contains("messageStop") && item.at("messageStop").is_object()) {
      final_stop_reason = bedrock_stop_reason(
          item.at("messageStop").value("stopReason", std::string()));
      continue;
    }

    if (item.contains("metadata") && item.at("metadata").is_object()) {
      const auto metadata = item.at("metadata");
      if (metadata.contains("usage") && metadata.at("usage").is_object()) {
        StreamEvent event;
        event.kind = EventKind::usage;
        event.usage = bedrock_usage_metadata(metadata.at("usage"));
        event.metadata = provider_metadata();
        events.push_back(std::move(event));
      }
      continue;
    }

    const std::vector<std::string> error_names = {
        "internalServerException", "modelStreamErrorException",
        "validationException", "throttlingException",
        "serviceUnavailableException"};
    for (const auto& error_name : error_names) {
      if (!item.contains(error_name) || !item.at(error_name).is_object()) {
        continue;
      }
      const auto& error = item.at(error_name);
      const std::map<std::string, std::string> sdk_names = {
          {"internalServerException", "InternalServerException"},
          {"modelStreamErrorException", "ModelStreamErrorException"},
          {"validationException", "ValidationException"},
          {"throttlingException", "ThrottlingException"},
          {"serviceUnavailableException", "ServiceUnavailableException"}};
      StreamEvent event;
      event.kind = EventKind::error;
      event.error = bedrock_format_error(
          sdk_names.at(error_name),
          error.value("message", std::string("An unknown error occurred")),
          true);
      event.metadata = provider_metadata();
      events.push_back(std::move(event));
      final_stop_reason = StopReason::error;
      break;
    }
  }

  if (final_stop_reason) {
    StreamEvent done;
    done.kind = EventKind::message_done;
    done.stop_reason = final_stop_reason;
    done.metadata = provider_metadata();
    events.push_back(std::move(done));
  }
  return events;
}

StopReason mistral_chat_stop_reason(std::string_view reason) {
  if (reason.empty() || reason == "stop") {
    return StopReason::end_turn;
  }
  if (reason == "length" || reason == "model_length") {
    return StopReason::max_tokens;
  }
  if (reason == "tool_calls") {
    return StopReason::tool_call;
  }
  if (reason == "error") {
    return StopReason::error;
  }
  return StopReason::end_turn;
}

std::string mistral_format_error(std::optional<int> status_code,
                                 std::string_view message,
                                 std::optional<std::string_view> body) {
  constexpr std::size_t kMaxMistralErrorBodyChars = 4000;
  const auto trimmed_body = body ? trim_ascii(*body) : std::string();
  if (status_code && !trimmed_body.empty()) {
    std::string text = trimmed_body;
    if (text.size() > kMaxMistralErrorBodyChars) {
      const auto truncated = text.size() - kMaxMistralErrorBodyChars;
      text = text.substr(0, kMaxMistralErrorBodyChars) + "... [truncated " +
             std::to_string(truncated) + " chars]";
    }
    return "Mistral API error (" + std::to_string(*status_code) + "): " +
           text;
  }
  if (status_code) {
    return "Mistral API error (" + std::to_string(*status_code) + "): " +
           std::string(message);
  }
  return std::string(message);
}

Usage mistral_usage(const Json& source) {
  Usage usage;
  usage.input_tokens = source.value("promptTokens", std::int64_t{0});
  usage.output_tokens = source.value("completionTokens", std::int64_t{0});
  usage.total_tokens = source.value("totalTokens", std::int64_t{0});
  normalize(usage);
  return usage;
}

std::vector<StreamEvent> parse_mistral_conversations_sse(
    std::string_view transcript) {
  std::vector<StreamEvent> events;

  struct PendingToolCall {
    ToolCallContent call;
    std::string arguments;
  };
  std::map<std::string, PendingToolCall> pending_tool_calls;
  bool saw_start = false;
  std::optional<std::string> provider_response_id;

  const auto push_tool_call = [&events](const PendingToolCall& pending,
                                        std::string_view key) {
    auto call = pending.call;
    call.arguments = parse_streamed_tool_arguments_or_raw(pending.arguments);

    StreamEvent event;
    event.kind = EventKind::tool_call;
    event.content = std::move(call);
    event.metadata = Json{{"provider_type", "mistral.chat.chunk"},
                          {"content_type", "tool_call"},
                          {"tool_key", std::string(key)}};
    events.push_back(std::move(event));
  };

  for (const auto& sse : parse_sse_events(transcript)) {
    if (sse.data == "[DONE]") {
      continue;
    }

    Json payload;
    try {
      payload = Json::parse(sanitize_utf8(sse.data));
    } catch (const Json::parse_error&) {
      StreamEvent event;
      event.kind = EventKind::error;
      event.error = "invalid Mistral SSE JSON payload";
      event.metadata = Json{{"data", sse.data}};
      events.push_back(std::move(event));
      continue;
    }

    const auto chunk =
        payload.contains("data") && payload.at("data").is_object()
            ? payload.at("data")
            : payload;
    if (const auto id = non_empty_string_field(chunk, "id");
        id && !provider_response_id) {
      provider_response_id = id;
    }

    if (!saw_start) {
      StreamEvent event;
      event.kind = EventKind::message_start;
      event.role = MessageRole::assistant;
      if (provider_response_id) {
        event.message_id = provider_response_id;
        event.metadata["provider_response_id"] = *provider_response_id;
      }
      event.metadata["provider_type"] = "mistral.chat.chunk";
      events.push_back(std::move(event));
      saw_start = true;
    }

    if (chunk.contains("usage") && chunk.at("usage").is_object()) {
      StreamEvent event;
      event.kind = EventKind::usage;
      event.usage = mistral_usage(chunk.at("usage"));
      event.metadata = Json{{"provider_type", "mistral.chat.chunk"}};
      events.push_back(std::move(event));
    }

    const auto choices = chunk.value("choices", Json::array());
    if (choices.empty() || !choices.front().is_object()) {
      continue;
    }

    const auto& choice = choices.front();
    const auto delta = choice.value("delta", Json::object());
    const auto content_it = delta.find("content");
    if (content_it != delta.end() && !content_it->is_null()) {
      const auto content_items =
          content_it->is_array() ? *content_it : Json::array({*content_it});
      for (const auto& item : content_items) {
        if (item.is_string()) {
          const auto text = provider_safe_text(item.get<std::string>());
          if (!text.empty()) {
            StreamEvent event;
            event.kind = EventKind::content_delta;
            event.delta = text;
            event.content = TextContent{text, std::nullopt};
            event.metadata = Json{{"provider_type", "mistral.chat.chunk"},
                                  {"content_type", "text"}};
            events.push_back(std::move(event));
          }
          continue;
        }
        if (!item.is_object()) {
          continue;
        }
        const auto type = item.value("type", std::string());
        if (type == "text") {
          const auto text =
              provider_safe_text(item.value("text", std::string()));
          if (!text.empty()) {
            StreamEvent event;
            event.kind = EventKind::content_delta;
            event.delta = text;
            event.content = TextContent{text, std::nullopt};
            event.metadata = Json{{"provider_type", "mistral.chat.chunk"},
                                  {"content_type", "text"}};
            events.push_back(std::move(event));
          }
        } else if (type == "thinking") {
          std::string text;
          for (const auto& part : item.value("thinking", Json::array())) {
            if (part.is_object() && part.contains("text") &&
                part.at("text").is_string()) {
              text += part.at("text").get<std::string>();
            }
          }
          text = provider_safe_text(text);
          if (!text.empty()) {
            StreamEvent event;
            event.kind = EventKind::content_delta;
            event.delta = text;
            event.content = ThinkingContent{text, std::nullopt, false};
            event.metadata = Json{{"provider_type", "mistral.chat.chunk"},
                                  {"content_type", "thinking"}};
            events.push_back(std::move(event));
          }
        }
      }
    }

    for (const auto& tool_call : delta.value("toolCalls", Json::array())) {
      if (!tool_call.is_object()) {
        continue;
      }
      const auto raw_id = non_empty_string_field(tool_call, "id");
      const auto call_id = raw_id && *raw_id != "null"
                               ? *raw_id
                               : derive_mistral_tool_call_id(
                                     "toolcall:" +
                                         std::to_string(tool_call.value(
                                             "index", 0)),
                                     0);
      const auto index = tool_call.value("index", 0);
      const auto key = call_id + ":" + std::to_string(index);
      auto& pending = pending_tool_calls[key];
      pending.call.id = call_id;
      const auto function = tool_call.value("function", Json::object());
      if (const auto name = non_empty_string_field(function, "name")) {
        pending.call.name = *name;
      }
      if (function.contains("arguments")) {
        if (function.at("arguments").is_string()) {
          pending.arguments += function.at("arguments").get<std::string>();
        } else if (!function.at("arguments").is_null()) {
          pending.arguments += function.at("arguments").dump();
        }
      }
      push_tool_call(pending, key);
    }

    if (choice.contains("finishReason") && !choice.at("finishReason").is_null()) {
      StreamEvent done;
      done.kind = EventKind::message_done;
      done.stop_reason =
          mistral_chat_stop_reason(choice.value("finishReason", std::string()));
      if (provider_response_id) {
        done.message_id = provider_response_id;
        done.metadata["provider_response_id"] = *provider_response_id;
      }
      done.metadata["provider_type"] = "mistral.chat.chunk";
      events.push_back(std::move(done));
    }
  }

  return events;
}

bool is_retryable_http_status(int status) noexcept {
  return status == 408 || status == 409 || status == 425 || status == 429 ||
         (status >= 500 && status <= 599);
}

std::optional<std::int64_t> retry_after_ms(const HttpResponse& response) {
  const auto value = header_value(response.headers, "retry-after");
  if (!value || value->empty()) {
    return std::nullopt;
  }

  try {
    const auto seconds = std::stoll(*value);
    if (seconds < 0) {
      return std::nullopt;
    }
    return seconds * 1000;
  } catch (const std::exception&) {
    return parse_retry_after_http_date_ms(*value);
  }
}

int hex_digit_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

std::string percent_decode_url_component(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '%' && index + 2U < value.size()) {
      const int high = hex_digit_value(value[index + 1U]);
      const int low = hex_digit_value(value[index + 2U]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2U;
        continue;
      }
    }
    decoded.push_back(value[index]);
  }
  return decoded;
}

std::string base64_encode_bytes(std::string_view bytes) {
  constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t index = 0; index < bytes.size(); index += 3U) {
    const auto b0 = static_cast<unsigned char>(bytes[index]);
    const auto b1 = index + 1U < bytes.size()
                        ? static_cast<unsigned char>(bytes[index + 1U])
                        : 0U;
    const auto b2 = index + 2U < bytes.size()
                        ? static_cast<unsigned char>(bytes[index + 2U])
                        : 0U;
    encoded.push_back(kAlphabet[(b0 >> 2U) & 0x3FU]);
    encoded.push_back(kAlphabet[((b0 & 0x03U) << 4U) | ((b1 >> 4U) & 0x0FU)]);
    encoded.push_back(index + 1U < bytes.size()
                          ? kAlphabet[((b1 & 0x0FU) << 2U) |
                                      ((b2 >> 6U) & 0x03U)]
                          : '=');
    encoded.push_back(index + 2U < bytes.size() ? kAlphabet[b2 & 0x3FU] : '=');
  }
  return encoded;
}

ParsedHttpUrl parse_http_url_for_transport(const std::string& url,
                                           bool allow_userinfo = false) {
  ParsedHttpUrl parsed;
  if (url.rfind("https://", 0) == 0) {
    parsed.scheme = "https";
    parsed.port = "443";
    parsed.https = true;
  } else if (url.rfind("http://", 0) == 0) {
    parsed.scheme = "http";
    parsed.port = "80";
  } else {
    throw std::invalid_argument("HTTP transport URL must use http:// or https://");
  }

  const auto authority_start = parsed.scheme.size() + 3U;
  const auto authority_end = url.find_first_of("/?#", authority_start);
  const std::string authority =
      authority_end == std::string::npos
          ? url.substr(authority_start)
          : url.substr(authority_start, authority_end - authority_start);
  std::string host_port = authority;
  const auto userinfo_end = authority.rfind('@');
  if (userinfo_end != std::string::npos) {
    if (!allow_userinfo) {
      throw std::invalid_argument("HTTP transport URL must not include userinfo");
    }
    const auto userinfo = std::string_view(authority).substr(0, userinfo_end);
    const auto colon = userinfo.find(':');
    parsed.username = percent_decode_url_component(
        colon == std::string_view::npos ? userinfo : userinfo.substr(0, colon));
    if (colon != std::string_view::npos) {
      parsed.password = percent_decode_url_component(userinfo.substr(colon + 1U));
    }
    host_port = authority.substr(userinfo_end + 1U);
  }
  if (host_port.empty()) {
    throw std::invalid_argument("HTTP transport URL is missing a host");
  }

  const auto colon = host_port.rfind(':');
  if (colon != std::string::npos && colon + 1U < host_port.size() &&
      host_port.find(']') == std::string::npos) {
    parsed.host = host_port.substr(0, colon);
    parsed.port = host_port.substr(colon + 1U);
  } else {
    parsed.host = host_port;
  }

  parsed.target = authority_end == std::string::npos ? "/" : url.substr(authority_end);
  if (parsed.target.empty()) {
    parsed.target = "/";
  }
  if (parsed.target.front() == '#') {
    parsed.target.insert(parsed.target.begin(), '/');
  } else if (parsed.target.front() == '?') {
    parsed.target.insert(parsed.target.begin(), '/');
  }
  if (parsed.host.empty()) {
    throw std::invalid_argument("HTTP transport URL is missing a host");
  }
  return parsed;
}

std::string transport_host_header(const ParsedHttpUrl& parsed) {
  const bool default_port =
      (parsed.https && parsed.port == "443") ||
      (!parsed.https && parsed.port == "80");
  return default_port ? parsed.host : parsed.host + ":" + parsed.port;
}

std::optional<std::string> proxy_authorization_header(
    const ParsedHttpUrl& proxy) {
  if (proxy.username.empty() && proxy.password.empty()) {
    return std::nullopt;
  }
  return "Basic " + base64_encode_bytes(proxy.username + ":" + proxy.password);
}

http::verb beast_http_verb(std::string method) {
  std::transform(method.begin(), method.end(), method.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::toupper(ch));
                 });
  if (method == "GET") {
    return http::verb::get;
  }
  if (method == "POST") {
    return http::verb::post;
  }
  if (method == "PUT") {
    return http::verb::put;
  }
  if (method == "PATCH") {
    return http::verb::patch;
  }
  if (method == "DELETE") {
    return http::verb::delete_;
  }
  if (method == "HEAD") {
    return http::verb::head;
  }
  throw std::invalid_argument("unsupported HTTP method: " + method);
}

std::chrono::milliseconds transport_timeout(const HttpRequest& request) {
  if (!request.timeout_ms || *request.timeout_ms <= 0) {
    return std::chrono::milliseconds(30000);
  }
  return std::chrono::milliseconds(*request.timeout_ms);
}

bool request_abort_requested(const HttpRequest& request) noexcept {
  return request.abort_signal &&
         request.abort_signal->load(std::memory_order_acquire);
}

void throw_if_request_aborted(const HttpRequest& request) {
  if (request_abort_requested(request)) {
    throw std::runtime_error("provider request aborted");
  }
}

class ScopedAbortWatcher {
 public:
  ScopedAbortWatcher(std::shared_ptr<std::atomic_bool> signal,
                     std::function<void()> cancel)
      : signal_(std::move(signal)), cancel_(std::move(cancel)) {
    if (!signal_) {
      return;
    }
    worker_ = std::thread([this]() {
      while (!done_.load(std::memory_order_acquire)) {
        if (signal_->load(std::memory_order_acquire)) {
          cancel_();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    });
  }

  ScopedAbortWatcher(const ScopedAbortWatcher&) = delete;
  ScopedAbortWatcher& operator=(const ScopedAbortWatcher&) = delete;

  ~ScopedAbortWatcher() {
    done_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  std::shared_ptr<std::atomic_bool> signal_;
  std::function<void()> cancel_;
  std::atomic_bool done_{false};
  std::thread worker_;
};

template <typename Stream>
void cancel_beast_stream(Stream& stream) noexcept {
  beast::error_code ignored;
  auto& socket = beast::get_lowest_layer(stream).socket();
  socket.cancel(ignored);
  socket.shutdown(tcp::socket::shutdown_both, ignored);
  socket.close(ignored);
}

HttpResponse response_from_beast(
    const http::response<http::string_body>& response) {
  HttpResponse result;
  result.status = static_cast<int>(response.result_int());
  for (const auto& field : response.base()) {
    result.headers[std::string(field.name_string())] =
        std::string(field.value());
  }
  result.body = response.body();
  return result;
}

template <typename Stream>
http::response<http::string_body> read_beast_string_response(
    Stream& stream, beast::flat_buffer& buffer) {
  http::response_parser<http::string_body> parser;
  parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
  http::read(stream, buffer, parser);
  return parser.release();
}

http::request<http::string_body> make_beast_http_request(
    const HttpRequest& request, const ParsedHttpUrl& parsed) {
  http::request<http::string_body> outgoing{
      beast_http_verb(request.method), parsed.target, 11};
  outgoing.set(http::field::host, transport_host_header(parsed));
  outgoing.set(http::field::user_agent, qt::foundation::pi_user_agent());
  for (const auto& [name, value] : request.headers) {
    if (lowercase_copy(name) == "host") {
      continue;
    }
    outgoing.set(name, value);
  }
  outgoing.body() = request.body;
  outgoing.prepare_payload();
  return outgoing;
}

std::string absolute_http_proxy_target(const ParsedHttpUrl& parsed) {
  return parsed.scheme + "://" + transport_host_header(parsed) + parsed.target;
}

void apply_proxy_authorization(http::request<http::string_body>& outgoing,
                               const ParsedHttpUrl& proxy) {
  if (const auto authorization = proxy_authorization_header(proxy)) {
    outgoing.set(http::field::proxy_authorization, *authorization);
  }
}

http::request<http::empty_body> make_proxy_connect_request(
    const ParsedHttpUrl& target, const ParsedHttpUrl& proxy) {
  http::request<http::empty_body> connect{
      http::verb::connect, transport_host_header(target), 11};
  connect.set(http::field::host, transport_host_header(target));
  connect.set(http::field::user_agent, qt::foundation::pi_user_agent());
  if (const auto authorization = proxy_authorization_header(proxy)) {
    connect.set(http::field::proxy_authorization, *authorization);
  }
  return connect;
}

void verify_proxy_connect_response(
    const http::response<http::string_body>& response) {
  if (response.result_int() >= 200 && response.result_int() < 300) {
    return;
  }
  throw std::runtime_error("provider proxy CONNECT failed with HTTP status " +
                           std::to_string(response.result_int()) +
                           (response.body().empty() ? std::string()
                                                    : ": " + response.body()));
}

void shutdown_plain_stream(beast::tcp_stream& stream,
                           const std::string& label) {
  beast::error_code shutdown_error;
  stream.socket().shutdown(tcp::socket::shutdown_both, shutdown_error);
  if (shutdown_error == net::error::not_connected) {
    shutdown_error = {};
  }
  if (shutdown_error) {
    throw std::runtime_error("shutdown " + label + " HTTP stream: " +
                             shutdown_error.message());
  }
}

void shutdown_tls_stream(beast::ssl_stream<beast::tcp_stream>& stream,
                         const std::string& label) {
  beast::error_code shutdown_error;
  stream.shutdown(shutdown_error);
  if (shutdown_error == net::error::eof ||
      shutdown_error == ssl::error::stream_truncated) {
    shutdown_error = {};
  }
  if (shutdown_error) {
    throw std::runtime_error("shutdown " + label + " TLS stream: " +
                             shutdown_error.message());
  }
}

void configure_tls_stream(beast::ssl_stream<beast::tcp_stream>& stream,
                          const ParsedHttpUrl& parsed) {
  if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
    beast::error_code ec{static_cast<int>(::ERR_get_error()),
                         net::error::get_ssl_category()};
    throw std::runtime_error("configure provider TLS SNI: " + ec.message());
  }
  stream.set_verify_mode(ssl::verify_peer);
  stream.set_verify_callback(ssl::host_name_verification(parsed.host));
}

HttpResponse BeastHttpTransport::send(const HttpRequest& request) {
  throw_if_request_aborted(request);
  const auto parsed = parse_http_url_for_transport(request.url);
  const auto proxy =
      request.proxy_url && !request.proxy_url->empty()
          ? std::optional<ParsedHttpUrl>(
                parse_http_url_for_transport(*request.proxy_url, true))
          : std::nullopt;
  if (proxy && proxy->https) {
    throw std::runtime_error(
        "Boost.Beast HTTP transport supports http:// proxies only");
  }

  net::io_context ioc;
  tcp::resolver resolver(ioc);
  const auto connect_host = proxy ? proxy->host : parsed.host;
  const auto connect_port = proxy ? proxy->port : parsed.port;
  const auto results = resolver.resolve(connect_host, connect_port);
  auto outgoing = make_beast_http_request(request, parsed);
  if (proxy && !parsed.https) {
    outgoing.target(absolute_http_proxy_target(parsed));
    apply_proxy_authorization(outgoing, *proxy);
    outgoing.prepare_payload();
  }
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  const auto timeout = transport_timeout(request);

  if (parsed.https) {
    ssl::context ctx(ssl::context::tls_client);
    ctx.set_default_verify_paths();
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    ScopedAbortWatcher abort_watcher(
        request.abort_signal,
        [&stream]() { cancel_beast_stream(stream); });
    configure_tls_stream(stream, parsed);
    beast::get_lowest_layer(stream).expires_after(timeout);
    beast::get_lowest_layer(stream).connect(results);
    throw_if_request_aborted(request);
    if (proxy) {
      http::write(beast::get_lowest_layer(stream),
                  make_proxy_connect_request(parsed, *proxy));
      auto connect_response =
          read_beast_string_response(beast::get_lowest_layer(stream), buffer);
      verify_proxy_connect_response(connect_response);
      buffer.clear();
      throw_if_request_aborted(request);
    }
    stream.handshake(ssl::stream_base::client);
    throw_if_request_aborted(request);
    http::write(stream, outgoing);
    response = read_beast_string_response(stream, buffer);
    throw_if_request_aborted(request);
    shutdown_tls_stream(stream, "provider");
  } else {
    beast::tcp_stream stream(ioc);
    ScopedAbortWatcher abort_watcher(
        request.abort_signal,
        [&stream]() { cancel_beast_stream(stream); });
    stream.expires_after(timeout);
    stream.connect(results);
    throw_if_request_aborted(request);
    http::write(stream, outgoing);
    response = read_beast_string_response(stream, buffer);
    throw_if_request_aborted(request);
    shutdown_plain_stream(stream, "provider");
  }
  return response_from_beast(response);
}

HttpResponse RecordedHttpTransport::send(const HttpRequest& request) {
  requests_.push_back(request);

  if (next_step_index_ >= steps_.size()) {
    throw std::runtime_error("recorded HTTP transport has no remaining steps");
  }

  const Step& step = steps_[next_step_index_++];
  if (step.error) {
    throw std::runtime_error(*step.error);
  }

  assert(step.response.has_value());
  return *step.response;
}

void RecordedHttpTransport::push_response(HttpResponse response) {
  steps_.push_back(Step{std::move(response), std::nullopt});
}

void RecordedHttpTransport::push_error(std::string error) {
  steps_.push_back(Step{std::nullopt, std::move(error)});
}

const std::vector<HttpRequest>& RecordedHttpTransport::requests() const noexcept {
  return requests_;
}

std::size_t RecordedHttpTransport::pending_steps() const noexcept {
  return steps_.size() - next_step_index_;
}

std::size_t RecordedHttpTransport::completed_steps() const noexcept {
  return next_step_index_;
}

std::vector<SseEvent> parse_sse_events(std::string_view transcript) {
  std::vector<SseEvent> events;
  SseEvent current;
  std::size_t offset = 0;

  while (offset <= transcript.size()) {
    const auto newline = transcript.find('\n', offset);
    const auto end =
        newline == std::string_view::npos ? transcript.size() : newline;
    std::string_view line = transcript.substr(offset, end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    if (line.empty()) {
      // Sanitize the data field before dispatching
      current.data = sanitize_utf8(current.data);
      dispatch_sse_event(events, current);
    } else if (line.front() != ':') {
      const auto colon = line.find(':');
      const auto field =
          colon == std::string_view::npos ? line : line.substr(0, colon);
      std::string_view value =
          colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
      if (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1);
      }

      if (field == "event") {
        current.event = std::string(value);
      } else if (field == "data") {
        current.data += std::string(value);
        current.data.push_back('\n');
      } else if (field == "id") {
        current.id = std::string(value);
      } else if (field == "retry") {
        try {
          current.retry_ms = std::stoll(std::string(value));
        } catch (const std::exception&) {
          current.retry_ms.reset();
        }
      }
    }

    if (newline == std::string_view::npos) {
      break;
    }
    offset = newline + 1;
  }

  // Sanitize the last event's data field
  current.data = sanitize_utf8(current.data);
  dispatch_sse_event(events, current);
  return events;
}

std::vector<StreamEvent> parse_anthropic_messages_sse(
    std::string_view transcript, const std::vector<ToolDefinition>* tools,
    bool claude_code_oauth) {
  struct Block {
    std::string type;
    std::string text;
    std::string id;
    std::string name;
    Json arguments = Json::object();
    std::string partial_json;
    std::string signature;
    bool redacted = false;
  };

  std::vector<StreamEvent> events;
  std::map<int, Block> blocks;
  Usage usage;
  bool saw_message_start = false;
  bool saw_message_done = false;
  bool saw_message_stop = false;
  bool terminal_error = false;

  const auto push_usage = [&events, &usage]() {
    normalize(usage);
    StreamEvent event;
    event.kind = EventKind::usage;
    event.usage = usage;
    events.push_back(std::move(event));
  };

  const auto parse_payload = [](std::string_view data) -> std::optional<Json> {
    if (data == "[DONE]") {
      return std::nullopt;
    }
    try {
      return Json::parse(sanitize_utf8(data));
    } catch (const Json::parse_error&) {
      auto repaired = parse_streaming_json(data);
      if (repaired.is_object() && !repaired.empty()) {
        return repaired;
      }
    }
    return std::nullopt;
  };

  for (const auto& sse : parse_sse_events(transcript)) {
    if (saw_message_stop) {
      continue;
    }
    if (sse.event == std::optional<std::string>{"error"}) {
      StreamEvent event;
      event.kind = EventKind::error;
      event.error = sse.data;
      events.push_back(std::move(event));
      terminal_error = true;
      break;
    }
    if (!is_anthropic_message_sse_event(sse.event)) {
      continue;
    }
    const auto payload = parse_payload(sse.data);
    if (!payload || !payload->is_object()) {
      StreamEvent event;
      event.kind = EventKind::error;
      const auto event_name = sse.event.value_or(std::string());
      event.error = "Could not parse Anthropic SSE event " + event_name +
                    ": invalid JSON; data=" + sse.data;
      events.push_back(std::move(event));
      terminal_error = true;
      break;
    }

    const std::string type = payload->value("type", std::string());
    if (type == "message_start") {
      saw_message_start = true;
      const auto message = payload->value("message", Json::object());
      StreamEvent event;
      event.kind = EventKind::message_start;
      event.role = MessageRole::assistant;
      if (message.contains("id") && message.at("id").is_string()) {
        event.message_id = message.at("id").get<std::string>();
        event.metadata["provider_response_id"] = *event.message_id;
      }
      events.push_back(std::move(event));

      const auto raw_usage = message.value("usage", Json::object());
      usage.input_tokens =
          raw_usage.value("input_tokens", std::int64_t{usage.input_tokens});
      usage.output_tokens =
          raw_usage.value("output_tokens", std::int64_t{usage.output_tokens});
      usage.cache_read_tokens = raw_usage.value(
          "cache_read_input_tokens", std::int64_t{usage.cache_read_tokens});
      usage.cache_write_tokens = raw_usage.value(
          "cache_creation_input_tokens",
          std::int64_t{usage.cache_write_tokens});
      push_usage();
    } else if (type == "content_block_start") {
      const int index = payload->value("index", -1);
      if (index < 0) {
        continue;
      }
      const auto content_block =
          payload->value("content_block", Json::object());
      Block block;
      block.type = content_block.value("type", std::string());
      if (block.type == "text") {
        block.text = content_block.value("text", std::string());
      } else if (block.type == "thinking") {
        block.text = content_block.value("thinking", std::string());
      } else if (block.type == "redacted_thinking") {
        block.text = "[Reasoning redacted]";
        block.signature = content_block.value("data", std::string());
        block.redacted = true;
      } else if (block.type == "tool_use") {
        block.id = content_block.value("id", std::string());
        block.name = content_block.value("name", std::string());
        block.arguments = content_block.value("input", Json::object());
      }
      blocks[index] = std::move(block);
    } else if (type == "content_block_delta") {
      const int index = payload->value("index", -1);
      auto it = blocks.find(index);
      if (it == blocks.end()) {
        continue;
      }
      const auto delta = payload->value("delta", Json::object());
      const auto delta_type = delta.value("type", std::string());
      if (delta_type == "text_delta") {
        const auto text = delta.value("text", std::string());
        it->second.text += text;
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = text;
        event.metadata = Json{{"content_type", "text"}};
        events.push_back(std::move(event));
      } else if (delta_type == "thinking_delta") {
        const auto thinking = delta.value("thinking", std::string());
        it->second.text += thinking;
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = thinking;
        event.metadata = Json{{"content_type", "thinking"}};
        events.push_back(std::move(event));
      } else if (delta_type == "signature_delta") {
        it->second.signature += delta.value("signature", std::string());
      } else if (delta_type == "input_json_delta") {
        const auto partial = delta.value("partial_json", std::string());
        it->second.partial_json += partial;
        it->second.arguments =
            parse_streamed_tool_arguments_or_raw(it->second.partial_json);
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = partial;
        event.metadata = Json{{"content_type", "tool_call_arguments"}};
        events.push_back(std::move(event));
      }
    } else if (type == "content_block_stop") {
      const int index = payload->value("index", -1);
      auto it = blocks.find(index);
      if (it == blocks.end()) {
        continue;
      }
      StreamEvent event;
      if (it->second.type == "text") {
        event.kind = EventKind::content_done;
        event.content = TextContent{it->second.text, std::nullopt};
      } else if (it->second.type == "thinking" ||
                 it->second.type == "redacted_thinking") {
        event.kind = EventKind::content_done;
        event.content = ThinkingContent{
            it->second.text,
            it->second.signature.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{it->second.signature},
            it->second.redacted};
      } else if (it->second.type == "tool_use") {
        event.kind = EventKind::tool_call;
        event.content = ToolCallContent{
            it->second.id,
            claude_code_oauth
                ? anthropic_original_tool_name(it->second.name, tools)
                : it->second.name,
            it->second.arguments};
      } else {
        blocks.erase(it);
        continue;
      }
      events.push_back(std::move(event));
      blocks.erase(it);
    } else if (type == "message_delta") {
      const auto delta = payload->value("delta", Json::object());
      const auto raw_usage = payload->value("usage", Json::object());
      if (raw_usage.contains("input_tokens") &&
          !raw_usage.at("input_tokens").is_null()) {
        usage.input_tokens =
            raw_usage.at("input_tokens").get<std::int64_t>();
      }
      if (raw_usage.contains("output_tokens") &&
          !raw_usage.at("output_tokens").is_null()) {
        usage.output_tokens =
            raw_usage.at("output_tokens").get<std::int64_t>();
      }
      if (raw_usage.contains("cache_read_input_tokens") &&
          !raw_usage.at("cache_read_input_tokens").is_null()) {
        usage.cache_read_tokens =
            raw_usage.at("cache_read_input_tokens").get<std::int64_t>();
      }
      if (raw_usage.contains("cache_creation_input_tokens") &&
          !raw_usage.at("cache_creation_input_tokens").is_null()) {
        usage.cache_write_tokens =
            raw_usage.at("cache_creation_input_tokens").get<std::int64_t>();
      }
      push_usage();

      if (delta.contains("stop_reason") &&
          delta.at("stop_reason").is_string()) {
        const auto raw_stop_reason = delta.at("stop_reason").get<std::string>();
        const auto stop_reason = anthropic_stop_reason(raw_stop_reason);
        StreamEvent event;
        if (stop_reason == StopReason::error) {
          event.kind = EventKind::error;
          event.error = "Provider stop_reason: " + raw_stop_reason;
        } else {
          event.kind = EventKind::message_done;
          event.stop_reason = stop_reason;
        }
        events.push_back(std::move(event));
        saw_message_done = true;
      }
    } else if (type == "message_stop") {
      if (!saw_message_done) {
        StreamEvent event;
        event.kind = EventKind::message_done;
        event.stop_reason = StopReason::end_turn;
        events.push_back(std::move(event));
        saw_message_done = true;
      }
      saw_message_stop = true;
    } else if (type == "error") {
      StreamEvent event;
      event.kind = EventKind::error;
      const auto error = payload->value("error", Json::object());
      event.error = error.value("message", sse.data);
      events.push_back(std::move(event));
    }
  }

  if (saw_message_start && !saw_message_stop && !terminal_error) {
    StreamEvent event;
    event.kind = EventKind::error;
    event.error = "Anthropic stream ended before message_stop";
    events.push_back(std::move(event));
  }

  return events;
}

std::vector<StreamEvent> parse_anthropic_messages_sse(
    std::string_view transcript) {
  return parse_anthropic_messages_sse(transcript, nullptr, false);
}

std::vector<StreamEvent> parse_anthropic_messages_sse(
    std::string_view transcript, const std::vector<ToolDefinition>& tools,
    bool claude_code_oauth) {
  return parse_anthropic_messages_sse(transcript, &tools, claude_code_oauth);
}

Json openai_responses_request_body(const CompletionRequest& request,
                                   bool stream,
                                   const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  const bool azure_responses =
      provider_key(normalized_request.model.provider) ==
      "azure-openai-responses";
  Json body = azure_responses ? Json::object()
                              : openai_extra_options(normalized_request.metadata);
  body["model"] = normalized_request.model.name;
  body["input"] = Json::array();
  body["stream"] = stream;
  if (!azure_responses) {
    body["store"] = false;
  }

  if (normalized_request.temperature) {
    body["temperature"] = *normalized_request.temperature;
  }
  if (normalized_request.max_output_tokens &&
      *normalized_request.max_output_tokens != 0) {
    body["max_output_tokens"] = *normalized_request.max_output_tokens;
  }
  auto service_tier = service_tier_from_metadata(options_metadata);
  if (!service_tier) {
    service_tier = service_tier_from_metadata(normalized_request.metadata);
  }
  if (service_tier && !azure_responses) {
    body["service_tier"] = *service_tier;
  }

  const auto metadata = openai_metadata_object(normalized_request.metadata);
  if (!azure_responses && !metadata.empty()) {
    body["metadata"] = metadata;
  }

  const auto replay_messages = provider_replay_messages(normalized_request.messages);
  const bool supports_image_input =
      metadata_declares_image_input(options_metadata) &&
      metadata_declares_image_input(normalized_request.metadata);
  std::size_t openai_message_index = 0;
  for (const auto& message : replay_messages) {
    const bool has_source_model =
        string_metadata_value(message.metadata, "provider") &&
        string_metadata_value(message.metadata, "api") &&
        string_metadata_value(message.metadata, "model");
    const bool same_provider_model =
        !has_source_model ||
        message_source_is_same_model(message, normalized_request.model);
    append_openai_responses_input_items(
        body["input"], message, normalized_request.model,
        "msg_" + std::to_string(openai_message_index), supports_image_input,
        same_provider_model);
    ++openai_message_index;
  }

  if (!normalized_request.tools.empty()) {
    body["tools"] = Json::array();
    for (const auto& tool : normalized_request.tools) {
      body["tools"].push_back(openai_tool(tool, Json(false)));
    }
  }
  apply_openai_responses_options(body, normalized_request, options_metadata);
  return body;
}

Json openai_responses_request_body(const CompletionRequest& request,
                                   bool stream) {
  return openai_responses_request_body(request, stream, Json::object());
}

Json openai_codex_responses_request_body(const CompletionRequest& request,
                                         bool stream,
                                         const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  Json body = Json::object();
  body["model"] = normalized_request.model.name;
  body["store"] = false;
  body["stream"] = stream;
  body["instructions"] = "You are a helpful assistant.";
  body["input"] = Json::array();
  body["text"] = Json{{"verbosity",
                       options_metadata.value("textVerbosity",
                                              std::string("low"))}};
  body["include"] = Json::array({"reasoning.encrypted_content"});
  body["tool_choice"] = "auto";
  body["parallel_tool_calls"] = true;

  if (const auto session_id = string_metadata_value(options_metadata, "sessionId")) {
    body["prompt_cache_key"] = *session_id;
  }
  if (const auto previous_response_id =
          string_metadata_value(options_metadata, "previousResponseId")) {
    body["previous_response_id"] = *previous_response_id;
  } else if (const auto previous_response_id =
                 string_metadata_value(options_metadata,
                                       "previous_response_id")) {
    body["previous_response_id"] = *previous_response_id;
  }
  auto service_tier = service_tier_from_metadata(options_metadata);
  if (!service_tier) {
    service_tier = service_tier_from_metadata(normalized_request.metadata);
  }
  if (service_tier) {
    body["service_tier"] = *service_tier;
  }

  std::string instructions;
  const auto replay_messages = provider_replay_messages(normalized_request.messages);
  const bool supports_image_input =
      metadata_declares_image_input(options_metadata) &&
      metadata_declares_image_input(normalized_request.metadata);
  std::size_t openai_message_index = 0;
  for (const auto& message : replay_messages) {
    if (message.role == MessageRole::system ||
        message.role == MessageRole::developer) {
      const auto text = message_text_content(message);
      if (!text.empty()) {
        if (!instructions.empty()) {
          instructions.push_back('\n');
        }
        instructions += text;
      }
      ++openai_message_index;
      continue;
    }

    const bool has_source_model =
        string_metadata_value(message.metadata, "provider") &&
        string_metadata_value(message.metadata, "api") &&
        string_metadata_value(message.metadata, "model");
    const bool same_provider_model =
        !has_source_model ||
        message_source_is_same_model(message, normalized_request.model);
    append_openai_responses_input_items(
        body["input"], message, normalized_request.model,
        "msg_" + std::to_string(openai_message_index), supports_image_input,
        same_provider_model);
    ++openai_message_index;
  }
  if (!instructions.empty()) {
    body["instructions"] = instructions;
  }
  normalize_openai_codex_input_ids(body["input"]);

  if (!normalized_request.tools.empty()) {
    body["tools"] = Json::array();
    for (const auto& tool : normalized_request.tools) {
      body["tools"].push_back(openai_tool(tool, Json(nullptr)));
    }
  }

  auto level = requested_thinking_level(normalized_request.metadata);
  if (!level) {
    level = string_metadata_value(options_metadata, "reasoningEffort");
  }
  if (level && !level->empty()) {
    const auto effort = *level == "none"
                            ? off_thinking_level(options_metadata)
                            : mapped_thinking_level(*level, options_metadata);
    if (effort) {
      if (*effort != "none" && *effort != "off") {
        body["reasoning"] =
            Json{{"effort", *effort},
                 {"summary",
                  options_metadata.value("reasoningSummary",
                                         std::string("auto"))}};
      }
    }
  }

  return body;
}

Json openai_completions_request_body(const CompletionRequest& request,
                                     bool stream,
                                     const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  Json resolved_options_metadata =
      options_metadata.is_object() ? options_metadata : Json::object();
  if (!resolved_options_metadata.contains("provider")) {
    resolved_options_metadata["provider"] = normalized_request.model.provider;
  }
  if (!resolved_options_metadata.contains("model")) {
    resolved_options_metadata["model"] = normalized_request.model.name;
  }
  Json body = openai_extra_options(normalized_request.metadata);
  body["model"] = normalized_request.model.name;
  body["messages"] = Json::array();
  body["stream"] = stream;
  const auto compat =
      openai_completions_compat_from_metadata(resolved_options_metadata);
  const bool model_supports_reasoning =
      !resolved_options_metadata.contains("reasoning") ||
      resolved_options_metadata.value("reasoning", false);
  const bool model_supports_images =
      metadata_declares_image_input(normalized_request.metadata) &&
      metadata_declares_image_input(resolved_options_metadata);

  if (stream && compat_flag(compat, "supportsUsageInStreaming", true)) {
    body["stream_options"] = Json{{"include_usage", true}};
  }
  if (openai_completions_supports_store(resolved_options_metadata, compat)) {
    body["store"] = false;
  }
  if (normalized_request.temperature) {
    body["temperature"] = *normalized_request.temperature;
  }
  if (const auto max_output_tokens = bounded_openai_completion_tokens(
          normalized_request, resolved_options_metadata)) {
    body[max_tokens_field_from_options_metadata(resolved_options_metadata)] =
        *max_output_tokens;
  }

  const auto metadata = openai_metadata_object(normalized_request.metadata);
  if (!metadata.empty()) {
    body["metadata"] = metadata;
  }

  const auto replay_messages = provider_replay_messages(normalized_request.messages);
  bool previous_was_tool_result = false;
  for (std::size_t index = 0; index < replay_messages.size();
       ++index) {
    const auto& message = replay_messages[index];
    if (previous_was_tool_result &&
        compat_flag(compat, "requiresAssistantAfterToolResult", false) &&
        message.role == MessageRole::user) {
      body["messages"].push_back(
          Json{{"role", "assistant"},
               {"content", "I have processed the tool results."}});
    }
    if (message.role == MessageRole::tool && message.content.size() == 1U &&
        content_kind(message.content.front()) == ContentKind::tool_result) {
      Json image_parts = Json::array();
      std::size_t next = index;
      for (; next < replay_messages.size(); ++next) {
        const auto& tool_message = replay_messages[next];
        if (tool_message.role != MessageRole::tool ||
            tool_message.content.size() != 1U ||
            content_kind(tool_message.content.front()) !=
                ContentKind::tool_result) {
          break;
        }

        const auto& result =
            std::get<ToolResultContent>(tool_message.content.front());
        const auto payload = tool_result_payload(result);
        body["messages"].push_back(
            openai_completions_tool_result_message(
                result, payload,
                compat_flag(compat, "requiresToolResultName", false),
                normalized_request.model, model_supports_images));
        if (model_supports_images) {
          for (const auto& image : payload.images) {
            image_parts.push_back(
                openai_completions_tool_result_image_part(image));
          }
        }
      }

      if (!image_parts.empty()) {
        if (compat_flag(compat, "requiresAssistantAfterToolResult", false)) {
          body["messages"].push_back(
              Json{{"role", "assistant"},
                   {"content", "I have processed the tool results."}});
        }
        Json content = Json::array(
            {Json{{"type", "text"},
                  {"text", "Attached image(s) from tool result:"}}});
        for (const auto& image_part : image_parts) {
          content.push_back(image_part);
        }
        body["messages"].push_back(Json{{"role", "user"}, {"content", content}});
        previous_was_tool_result = false;
      } else {
        previous_was_tool_result = true;
      }

      index = next - 1;
      continue;
    }

    const bool has_source_model =
        string_metadata_value(message.metadata, "provider") &&
        string_metadata_value(message.metadata, "api") &&
        string_metadata_value(message.metadata, "model");
    const bool same_provider_model =
        !has_source_model ||
        message_source_is_same_model(message, normalized_request.model);
    auto converted = openai_completions_message(
        message, compat, model_supports_reasoning, normalized_request.model,
        model_supports_images, same_provider_model);
    if (!openai_completions_should_skip_message(message, converted)) {
      body["messages"].push_back(std::move(converted));
    }
    previous_was_tool_result = false;
  }

  if (!normalized_request.tools.empty()) {
    body["tools"] = Json::array();
    for (const auto& tool : normalized_request.tools) {
      body["tools"].push_back(openai_completions_tool(tool, compat));
    }
    if (normalized_request.tool_choice.kind != ToolChoiceKind::auto_select) {
      body["tool_choice"] =
          openai_completions_tool_choice(normalized_request.tool_choice);
    }
    if (compat.value("zaiToolStream", false)) {
      body["tool_stream"] = true;
    }
  } else if (request_has_tool_history(normalized_request)) {
    body["tools"] = Json::array();
  }
  if (const auto options_choice =
          openai_tool_choice_from_options_metadata(resolved_options_metadata)) {
    body["tool_choice"] = *options_choice;
  }

  apply_openai_completions_cache_options(body, resolved_options_metadata);
  apply_openai_completions_reasoning_options(body, normalized_request,
                                             resolved_options_metadata);
  apply_openai_completions_routing_options(body, resolved_options_metadata);

  return body;
}

Json openai_completions_request_body(const CompletionRequest& request,
                                     bool stream) {
  return openai_completions_request_body(request, stream, Json::object());
}

Json anthropic_messages_request_body(const CompletionRequest& request,
                                     bool stream,
                                     const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  const bool eager_tool_input =
      anthropic_supports_eager_tool_input_streaming(options_metadata);
  const bool claude_code_oauth =
      anthropic_uses_claude_code_oauth(options_metadata);
  const bool supports_image_input =
      metadata_declares_image_input(options_metadata) &&
      metadata_declares_image_input(normalized_request.metadata);
  const Json cache_control = anthropic_cache_control(options_metadata);

  Json body = Json{{"model", normalized_request.model.name},
                   {"messages", Json::array()},
                   {"stream", stream}};

  if (claude_code_oauth) {
    Json identity_block =
        Json{{"type", "text"},
             {"text", "You are Claude Code, Anthropic's official CLI for Claude."}};
    if (!cache_control.is_null()) {
      identity_block["cache_control"] = cache_control;
    }
    body["system"] = Json::array({std::move(identity_block)});
  }

  if (normalized_request.max_output_tokens &&
      *normalized_request.max_output_tokens != 0) {
    body["max_tokens"] = *normalized_request.max_output_tokens;
  } else if (options_metadata.is_object() &&
             options_metadata.contains("maxTokens") &&
             options_metadata.at("maxTokens").is_number_integer()) {
    const auto model_max_tokens =
        options_metadata.at("maxTokens").get<std::int64_t>();
    if (model_max_tokens > 0) {
      body["max_tokens"] = model_max_tokens / 3;
    }
  }

  const auto thinking_level =
      requested_thinking_level(normalized_request.metadata);
  const bool thinking_enabled = thinking_level && *thinking_level != "off";
  const bool thinking_disabled =
      options_metadata.is_object() &&
      options_metadata.contains("thinkingEnabled") &&
      options_metadata.at("thinkingEnabled").is_boolean() &&
      !options_metadata.at("thinkingEnabled").get<bool>();
  if (normalized_request.temperature && !thinking_enabled) {
    body["temperature"] = *normalized_request.temperature;
  }

  const auto replay_messages = provider_replay_messages(normalized_request.messages);
  std::map<std::string, std::string> anthropic_tool_call_id_map;
  for (std::size_t message_index = 0; message_index < replay_messages.size();
       ++message_index) {
    const auto& message = replay_messages[message_index];
    if (message.role == MessageRole::system ||
        message.role == MessageRole::developer) {
      const auto text = anthropic_text_content(message.content);
      if (!text.empty()) {
        Json system_block = Json{{"type", "text"}, {"text", text}};
        if (!cache_control.is_null()) {
          system_block["cache_control"] = cache_control;
        }
        body["system"].push_back(std::move(system_block));
      }
      continue;
    }

    if (message.role == MessageRole::assistant) {
      const bool has_source_model =
          string_metadata_value(message.metadata, "provider") &&
          string_metadata_value(message.metadata, "api") &&
          string_metadata_value(message.metadata, "model");
      const bool same_provider_model =
          !has_source_model ||
          message_source_is_same_model(message, normalized_request.model);
      const auto content = anthropic_assistant_content(
          message.content, claude_code_oauth, anthropic_tool_call_id_map,
          same_provider_model);
      if (!content.empty()) {
        body["messages"].push_back(
            Json{{"role", "assistant"}, {"content", content}});
      }
      continue;
    }

    if (message.role == MessageRole::tool) {
      Json tool_results = Json::array();
      std::size_t next = message_index;
      for (; next < replay_messages.size(); ++next) {
        const auto& tool_message = replay_messages[next];
        if (tool_message.role != MessageRole::tool) {
          break;
        }
        bool saw_tool_result = false;
        for (const auto& content : tool_message.content) {
          if (content_kind(content) == ContentKind::tool_result) {
            tool_results.push_back(anthropic_tool_result_block(
                std::get<ToolResultContent>(content), supports_image_input,
                &anthropic_tool_call_id_map));
            saw_tool_result = true;
          }
        }
        if (!saw_tool_result) {
          break;
        }
      }
      if (!tool_results.empty()) {
        body["messages"].push_back(
            Json{{"role", "user"}, {"content", tool_results}});
      }
      if (next > message_index) {
        message_index = next - 1;
      }
      continue;
    }

    const auto content =
        anthropic_user_content(message.content, supports_image_input);
    if ((content.is_string() && !trim_ascii(content.get<std::string>()).empty()) ||
        (content.is_array() && !content.empty())) {
      body["messages"].push_back(Json{{"role", "user"}, {"content", content}});
    }
  }

  if (!normalized_request.tools.empty()) {
    body["tools"] = Json::array();
    const bool cache_tools =
        !cache_control.is_null() &&
        anthropic_supports_cache_control_on_tools(options_metadata);
    for (std::size_t index = 0; index < normalized_request.tools.size();
         ++index) {
      const bool last_tool = index + 1 == normalized_request.tools.size();
      body["tools"].push_back(
          anthropic_tool(normalized_request.tools[index], eager_tool_input,
                         claude_code_oauth,
                         cache_tools && last_tool ? cache_control
                                                 : Json(nullptr)));
    }
    if (const auto choice = anthropic_tool_choice(normalized_request.tool_choice)) {
      body["tool_choice"] = *choice;
    }
  }
  if (const auto options_choice =
          anthropic_tool_choice_from_options_metadata(options_metadata)) {
    body["tool_choice"] = *options_choice;
  }

  anthropic_add_cache_control_to_last_user_message(body["messages"],
                                                   cache_control);

  if (anthropic_model_supports_reasoning(normalized_request.model.name) &&
      (thinking_enabled || thinking_disabled)) {
    if (thinking_enabled) {
      const std::string display =
          string_metadata_value(options_metadata, "thinkingDisplay")
              .value_or("summarized");
      if (anthropic_supports_adaptive_thinking(normalized_request.model.name)) {
        body["thinking"] = Json{{"type", "adaptive"}, {"display", display}};
        body["output_config"] =
            Json{{"effort", anthropic_thinking_effort(*thinking_level,
                                                       options_metadata)}};
      } else {
        body["thinking"] =
            Json{{"type", "enabled"},
                 {"budget_tokens",
                  anthropic_thinking_budget(options_metadata, *thinking_level)},
                 {"display", display}};
      }
    } else if (thinking_disabled) {
      body["thinking"] = Json{{"type", "disabled"}};
    }
  }

  if (const auto user_id = string_metadata_value(options_metadata, "user_id")) {
    body["metadata"] = Json{{"user_id", *user_id}};
  }

  return body;
}

HttpRequest anthropic_messages_http_request(const CompletionRequest& request,
                                            const ProviderOptions& options,
                                            bool stream,
                                            std::string endpoint) {
  HttpRequest http;
  http.method = "POST";
  http.url = std::move(endpoint);
  http.timeout_ms = options.timeout_ms;
  http.proxy_url = options.proxy_url;
  http.headers = metadata_headers(options.metadata);
  http.headers.emplace("content-type", "application/json");
  http.headers.emplace("anthropic-version", "2023-06-01");
  const bool github_copilot_provider =
      provider_key(request.model.provider) == "github-copilot";
  const bool cloudflare_ai_gateway_provider =
      is_cloudflare_ai_gateway(request.model.provider);
  if (github_copilot_provider) {
    merge_overriding_headers(http.headers,
                             github_copilot_dynamic_headers(request));
  }
  std::optional<std::string> effective_api_key = options.api_key;
  if (!effective_api_key) {
    effective_api_key = get_env_api_key(request.model.provider);
  }
  const bool eager_tool_input =
      anthropic_supports_eager_tool_input_streaming(options.metadata);
  const bool needs_fine_grained_tool_streaming_beta =
      !github_copilot_provider && !request.tools.empty() && !eager_tool_input;
  std::vector<std::string> beta_features;
  if (needs_fine_grained_tool_streaming_beta) {
    beta_features.emplace_back("fine-grained-tool-streaming-2025-05-14");
  }
  if (anthropic_interleaved_thinking_enabled(options.metadata) &&
      !anthropic_supports_adaptive_thinking(request.model.name)) {
    beta_features.emplace_back("interleaved-thinking-2025-05-14");
  }
  const bool uses_oauth_token =
      effective_api_key &&
      effective_api_key->find("sk-ant-oat") != std::string::npos;
  if (effective_api_key) {
    if (cloudflare_ai_gateway_provider) {
      http.headers["cf-aig-authorization"] = "Bearer " + *effective_api_key;
    } else if (github_copilot_provider) {
      http.headers.emplace("authorization", "Bearer " + *effective_api_key);
    } else if (uses_oauth_token) {
      http.headers.emplace("authorization", "Bearer " + *effective_api_key);
      std::string beta = "claude-code-20250219, oauth-2025-04-20";
      if (!beta_features.empty()) {
        beta += ", " + join_header_values(beta_features, ", ");
      }
      http.headers.emplace("anthropic-beta", std::move(beta));
      http.headers.emplace("user-agent", "claude-cli/2.1.75");
      http.headers.emplace("x-app", "cli");
    } else {
      http.headers.emplace("x-api-key", *effective_api_key);
    }
  }

  if (!beta_features.empty() &&
      http.headers.count("anthropic-beta") == 0U) {
    http.headers.emplace("anthropic-beta",
                         join_header_values(beta_features, ","));
  }

  const auto session_id = string_metadata_value(options.metadata, "sessionId");
  const auto cache_retention =
      cache_retention_from_options_metadata(options.metadata);
  if (session_id && cache_retention != "none" &&
      compat_sends_session_affinity_headers(options.metadata)) {
    http.headers["x-session-affinity"] = *session_id;
  }
  merge_overriding_headers(http.headers, options.headers);

  Json request_metadata =
      options.metadata.is_object() ? options.metadata : Json::object();
  if (uses_oauth_token) {
    request_metadata["anthropicOAuth"] = true;
  }
  http.body =
      provider_safe_json_dump(
          anthropic_messages_request_body(request, stream, request_metadata));
  return http;
}

Json google_tools_request_field(const std::vector<ToolDefinition>& tools,
                                bool use_parameters) {
  if (tools.empty()) {
    return Json(nullptr);
  }

  Json declarations = Json::array();
  for (const auto& tool : tools) {
    Json declaration = Json{{"name", tool.name},
                            {"description", tool.description}};
    if (use_parameters) {
      declaration["parameters"] =
          google_sanitize_openapi_schema(tool.input_schema);
    } else {
      declaration["parametersJsonSchema"] = tool.input_schema;
    }
    declarations.push_back(std::move(declaration));
  }

  return Json::array({Json{{"functionDeclarations", std::move(declarations)}}});
}

std::string google_tool_choice_mode(std::string_view choice) {
  if (choice == "none") {
    return "NONE";
  }
  if (choice == "any") {
    return "ANY";
  }
  return "AUTO";
}

std::optional<std::string> google_tool_choice_mode(const ToolChoice& choice) {
  switch (choice.kind) {
    case ToolChoiceKind::auto_select:
      return std::nullopt;
    case ToolChoiceKind::none:
      return google_tool_choice_mode("none");
    case ToolChoiceKind::required:
    case ToolChoiceKind::named:
      return google_tool_choice_mode("any");
  }
  return std::nullopt;
}

std::optional<std::string> google_tool_choice_mode_from_options_metadata(
    const Json& metadata) {
  if (!metadata.is_object() || !metadata.contains("toolChoice") ||
      !metadata.at("toolChoice").is_string()) {
    return std::nullopt;
  }
  return google_tool_choice_mode(metadata.at("toolChoice").get<std::string>());
}

Json google_contents_request_field(const CompletionRequest& request) {
  const auto normalized_request = normalized(request);
  Json contents = Json::array();
  const bool include_tool_call_id =
      google_requires_tool_call_id(normalized_request.model.name);
  const bool supports_images =
      metadata_declares_image_input(normalized_request.metadata);

  const auto replay_messages = provider_replay_messages(normalized_request.messages);
  std::map<std::string, std::string> google_tool_call_id_map;
  for (const auto& message : replay_messages) {
    if (message.role == MessageRole::user) {
      Json parts = Json::array();
      bool previous_was_unsupported_image_placeholder = false;
      for (const auto& content : message.content) {
        if (content_kind(content) == ContentKind::text) {
          const auto& text_content = std::get<TextContent>(content);
          const auto text = provider_safe_text(text_content.text);
          parts.push_back(Json{{"text", text}});
          previous_was_unsupported_image_placeholder =
              text_content.text == "(image omitted: model does not support images)";
        } else if (content_kind(content) == ContentKind::image) {
          if (!supports_images) {
            if (!previous_was_unsupported_image_placeholder) {
              parts.push_back(
                  Json{{"text",
                        "(image omitted: model does not support images)"}});
            }
            previous_was_unsupported_image_placeholder = true;
            continue;
          }
          const auto& image = std::get<ImageContent>(content);
          parts.push_back(Json{{"inlineData",
                                {{"mimeType", image.media_type},
                                 {"data", image.data}}}});
          previous_was_unsupported_image_placeholder = false;
        }
      }
      if (!parts.empty()) {
        contents.push_back(Json{{"role", "user"}, {"parts", std::move(parts)}});
      }
    } else if (message.role == MessageRole::assistant) {
      Json parts = Json::array();
      const bool same_provider_and_model =
          google_message_matches_model(message, normalized_request.model);

      for (const auto& content : message.content) {
        switch (content_kind(content)) {
          case ContentKind::text: {
            const auto& text_content = std::get<TextContent>(content);
            const auto text = provider_safe_text(text_content.text);
            if (trim_ascii(text).empty()) {
              break;
            }
            Json part = Json{{"text", text}};
            if (const auto signature = google_resolved_thought_signature(
                    same_provider_and_model, text_content.signature)) {
              part["thoughtSignature"] = *signature;
            }
            parts.push_back(std::move(part));
            break;
          }
          case ContentKind::thinking: {
            const auto& thinking = std::get<ThinkingContent>(content);
            const auto text = provider_safe_text(thinking.text);
            if (trim_ascii(text).empty()) {
              break;
            }
            Json part = Json{{"text", text}};
            if (same_provider_and_model) {
              part["thought"] = true;
              if (const auto signature = google_resolved_thought_signature(
                      true, thinking.signature)) {
                part["thoughtSignature"] = *signature;
              }
            }
            parts.push_back(std::move(part));
            break;
          }
          case ContentKind::tool_call: {
            const auto& call = std::get<ToolCallContent>(content);
            Json function_call = Json{{"name", call.name},
                                      {"args", call.arguments.is_null()
                                                   ? Json::object()
                                                   : call.arguments}};
            if (include_tool_call_id) {
              std::string id = call.id;
              if (!same_provider_and_model) {
                id = google_tool_call_id(call.id);
                if (id != call.id) {
                  google_tool_call_id_map[call.id] = id;
                }
              }
              function_call["id"] = std::move(id);
            }
            Json part = Json{{"functionCall", std::move(function_call)}};
            if (const auto signature = google_resolved_thought_signature(
                    same_provider_and_model, call.thought_signature)) {
              part["thoughtSignature"] = *signature;
            }
            parts.push_back(std::move(part));
            break;
          }
          case ContentKind::image:
          case ContentKind::tool_result:
            break;
        }
      }

      if (!parts.empty()) {
        contents.push_back(Json{{"role", "model"}, {"parts", std::move(parts)}});
      }
    } else if (message.role == MessageRole::tool) {
      Json parts = Json::array();
      Json image_user_parts = Json::array();
      for (const auto& content : message.content) {
        if (content_kind(content) != ContentKind::tool_result) {
          continue;
        }
        const auto& result = std::get<ToolResultContent>(content);
        const auto payload = tool_result_payload(result);
        const bool has_images = !payload.images.empty();
        const bool multimodal_function_response =
            google_supports_multimodal_function_response(
                normalized_request.model.name);
        std::string response_value = payload.text;
        if (has_images && !supports_images) {
          if (!response_value.empty()) {
            response_value += "\n";
          }
          response_value += "(tool image omitted: model does not support images)";
        } else if (response_value.empty() && has_images) {
          response_value = "(see attached image)";
        }

        Json function_response =
            Json{{"name", result.name},
                 {"response",
                  result.is_error ? Json{{"error", response_value}}
                                  : Json{{"output", response_value}}}};
        if (include_tool_call_id) {
          const auto mapped = google_tool_call_id_map.find(result.tool_call_id);
          function_response["id"] =
              mapped != google_tool_call_id_map.end()
                  ? mapped->second
                  : result.tool_call_id;
        }

        Json image_parts = Json::array();
        if (supports_images) {
          for (const auto& image : payload.images) {
            image_parts.push_back(Json{{"inlineData",
                                        {{"mimeType", image.media_type},
                                         {"data", image.data}}}});
          }
        }
        if (has_images && supports_images && multimodal_function_response) {
          function_response["parts"] = image_parts;
        } else if (has_images && supports_images) {
          for (const auto& image_part : image_parts) {
            image_user_parts.push_back(image_part);
          }
        }
        parts.push_back(Json{{"functionResponse", std::move(function_response)}});
      }

      if (!parts.empty()) {
        if (!contents.empty() && contents.back().is_object() &&
            contents.back().value("role", std::string()) == "user") {
          auto& prior_parts = contents.back()["parts"];
          if (prior_parts.is_array() &&
              std::any_of(prior_parts.begin(), prior_parts.end(),
                          [](const Json& part) {
                            return part.is_object() &&
                                   part.contains("functionResponse");
                          })) {
            for (auto& part : parts) {
              prior_parts.push_back(std::move(part));
            }
          } else {
            contents.push_back(
                Json{{"role", "user"}, {"parts", std::move(parts)}});
          }
        } else {
          contents.push_back(Json{{"role", "user"}, {"parts", std::move(parts)}});
        }
      }
      if (!image_user_parts.empty()) {
        image_user_parts.insert(image_user_parts.begin(),
                                Json{{"text", "Tool result image:"}});
        contents.push_back(
            Json{{"role", "user"}, {"parts", std::move(image_user_parts)}});
      }
    }
  }

  return contents;
}

Json google_generate_content_request_body(const CompletionRequest& request,
                                          const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  Json body = Json{{"model", normalized_request.model.name},
                   {"contents", google_contents_request_field(normalized_request)}};

  Json config = Json::object();
  if (normalized_request.temperature) {
    config["temperature"] = *normalized_request.temperature;
  }
  if (normalized_request.max_output_tokens) {
    config["maxOutputTokens"] = *normalized_request.max_output_tokens;
  }

  std::string system_instruction;
  for (const auto& message : provider_replay_messages(normalized_request.messages)) {
    if (message.role != MessageRole::system &&
        message.role != MessageRole::developer) {
      continue;
    }
    const auto text = message_text_content(message);
    if (text.empty()) {
      continue;
    }
    if (!system_instruction.empty()) {
      system_instruction.push_back('\n');
    }
    system_instruction += provider_safe_text(text);
  }
  if (!system_instruction.empty()) {
    config["systemInstruction"] = system_instruction;
  }

  if (!normalized_request.tools.empty()) {
    config["tools"] = google_tools_request_field(normalized_request.tools);
    if (const auto mode =
            google_tool_choice_mode(normalized_request.tool_choice)) {
      config["toolConfig"] =
          Json{{"functionCallingConfig", {{"mode", *mode}}}};
    }
    if (const auto options_mode =
            google_tool_choice_mode_from_options_metadata(options_metadata)) {
      config["toolConfig"] =
          Json{{"functionCallingConfig", {{"mode", *options_mode}}}};
    }
  }

  Json thinking_config =
      google_thinking_config_request_field(normalized_request, options_metadata);
  if (!thinking_config.is_null()) {
    config["thinkingConfig"] = std::move(thinking_config);
  }

  if (!config.empty()) {
    body["config"] = std::move(config);
  }
  return body;
}

Json google_thinking_config_request_field(
    const CompletionRequest& request, const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  const auto level = requested_thinking_level(normalized_request.metadata);
  const bool explicitly_disabled =
      (level && *level == "off") ||
      (options_metadata.is_object() &&
       options_metadata.contains("thinkingEnabled") &&
       options_metadata.at("thinkingEnabled").is_boolean() &&
       !options_metadata.at("thinkingEnabled").get<bool>());

  if (explicitly_disabled) {
    return google_disabled_thinking_config(normalized_request.model.name);
  }
  if (!level || level->empty()) {
    return Json(nullptr);
  }

  Json config = Json{{"includeThoughts", true}};
  if (const auto budget = google_thinking_budget(*level, options_metadata)) {
    config["thinkingBudget"] = *budget;
  } else if (const auto mapped =
                 google_mapped_thinking_level(*level, options_metadata)) {
    config["thinkingLevel"] = *mapped;
  }
  return config;
}

bool google_is_thinking_part(const Json& part) {
  return part.is_object() && part.value("thought", false);
}

std::optional<std::string> google_retain_thought_signature(
    const std::optional<std::string>& previous,
    const std::optional<std::string>& incoming) {
  if (incoming && !incoming->empty()) {
    return incoming;
  }
  return previous;
}

Json google_client_options(
    const ProviderOptions& options,
    const std::map<std::string, std::string>& model_headers) {
  Json config = Json::object();
  if (options.api_key) {
    config["apiKey"] = *options.api_key;
  }

  Json http_options = Json::object();
  if (const auto base_url = string_metadata_value(options.metadata, "baseUrl")) {
    const auto trimmed = trim_ascii(*base_url);
    if (!trimmed.empty()) {
      http_options["baseUrl"] = trimmed;
      http_options["apiVersion"] = "";
    }
  }

  Json headers = Json::object();
  for (const auto& [key, value] : model_headers) {
    headers[key] = value;
  }
  for (const auto& [key, value] : options.headers) {
    headers[key] = value;
  }
  if (!headers.empty()) {
    http_options["headers"] = std::move(headers);
  }
  if (!http_options.empty()) {
    config["httpOptions"] = std::move(http_options);
  }
  return config;
}

Json google_vertex_client_options(const ProviderOptions& options) {
  Json config = Json{{"vertexai", true}, {"apiVersion", "v1"}};
  if (const auto api_key = google_vertex_api_key(options)) {
    config["apiKey"] = *api_key;
  } else {
    config["project"] = google_vertex_required_option(
        options, "project", "GOOGLE_CLOUD_PROJECT", "GCLOUD_PROJECT",
        "Vertex AI requires a project ID. Set GOOGLE_CLOUD_PROJECT/"
        "GCLOUD_PROJECT or pass project in options.");
    config["location"] = google_vertex_required_option(
        options, "location", "GOOGLE_CLOUD_LOCATION", nullptr,
        "Vertex AI requires a location. Set GOOGLE_CLOUD_LOCATION or pass "
        "location in options.");
  }

  if (const auto base_url = string_metadata_value(options.metadata, "baseUrl")) {
    const auto trimmed = trim_ascii(*base_url);
    if (!google_vertex_generated_base_url(trimmed)) {
      Json http_options = Json{{"baseUrl", trimmed},
                               {"baseUrlResourceScope", "COLLECTION"}};
      if (google_vertex_base_url_includes_api_version(trimmed)) {
        http_options["apiVersion"] = "";
      }
      config["httpOptions"] = std::move(http_options);
    }
  }
  return config;
}

Json bedrock_runtime_client_config(const ProviderOptions& options) {
  Json config = Json::object();
  if (const auto profile = string_metadata_value(options.metadata, "profile")) {
    const auto trimmed = trim_ascii(*profile);
    if (!trimmed.empty()) {
      config["profile"] = trimmed;
    }
  }

  const auto base_url = string_metadata_value(options.metadata, "baseUrl");
  std::optional<std::string> configured_region;
  if (const auto region = string_metadata_value(options.metadata, "region")) {
    const auto trimmed = trim_ascii(*region);
    if (!trimmed.empty()) {
      configured_region = trimmed;
    }
  }
  if (!configured_region) {
    configured_region = env_string("AWS_REGION");
  }
  if (!configured_region) {
    configured_region = env_string("AWS_DEFAULT_REGION");
  }

  const bool has_profile = config.contains("profile") ||
                           env_string("AWS_PROFILE").has_value();
  const auto endpoint_region =
      base_url ? bedrock_standard_endpoint_region(*base_url) : std::nullopt;
  const bool use_explicit_endpoint =
      base_url && (!endpoint_region || (!configured_region && !has_profile));

  if (use_explicit_endpoint) {
    config["endpoint"] = trim_ascii(*base_url);
  }
  if (configured_region) {
    config["region"] = *configured_region;
  } else if (endpoint_region && use_explicit_endpoint) {
    config["region"] = *endpoint_region;
  } else if (!has_profile) {
    config["region"] = "us-east-1";
  }

  const bool skip_auth =
      env_string("AWS_BEDROCK_SKIP_AUTH") == std::optional<std::string>{"1"};
  if (skip_auth) {
    config["credentials"] =
        Json{{"accessKeyId", "dummy-access-key"},
             {"secretAccessKey", "dummy-secret-key"}};
  }

  std::optional<std::string> bearer_token;
  if (const auto token = string_metadata_value(options.metadata, "bearerToken")) {
    const auto trimmed = trim_ascii(*token);
    if (!trimmed.empty()) {
      bearer_token = trimmed;
    }
  }
  if (!bearer_token) {
    bearer_token = env_string("AWS_BEARER_TOKEN_BEDROCK");
  }
  if (bearer_token && !skip_auth) {
    config["token"] = Json{{"token", *bearer_token}};
    config["authSchemePreference"] = Json::array({"httpBearerAuth"});
  }

  if (options.proxy_url || bedrock_has_proxy_env()) {
    config["requestHandler"] = Json{{"type", "NodeHttpHandler"},
                                    {"proxyAgent", true}};
  } else if (env_string("AWS_BEDROCK_FORCE_HTTP1") ==
             std::optional<std::string>{"1"}) {
    config["requestHandler"] = Json{{"type", "NodeHttpHandler"}};
  }

  return config;
}

std::string bedrock_format_error(std::string_view name,
                                 std::string_view message,
                                 bool service_exception) {
  if (!service_exception) {
    return std::string(message);
  }

  std::string prefix(name);
  if (name == "InternalServerException") {
    prefix = "Internal server error";
  } else if (name == "ModelStreamErrorException") {
    prefix = "Model stream error";
  } else if (name == "ValidationException") {
    prefix = "Validation error";
  } else if (name == "ThrottlingException") {
    prefix = "Throttling error";
  } else if (name == "ServiceUnavailableException") {
    prefix = "Service unavailable";
  }
  return prefix + ": " + std::string(message);
}

Json bedrock_converse_request_body(const CompletionRequest& request,
                                   bool stream,
                                   const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  const auto retention = cache_retention_from_options_metadata(options_metadata);
  const bool supports_cache =
      retention != "none" &&
      bedrock_supports_prompt_caching(normalized_request, options_metadata);
  const bool supports_thinking_signature =
      bedrock_is_anthropic_claude(normalized_request, options_metadata);
  const bool supports_images =
      metadata_declares_image_input(options_metadata) &&
      metadata_declares_image_input(normalized_request.metadata);
  const auto level = requested_thinking_level(normalized_request.metadata);
  std::optional<BedrockThinkingTokenPlan> adjusted_thinking_tokens;
  if (normalized_request.max_output_tokens && level && !level->empty() &&
      *level != "off" &&
      bedrock_is_anthropic_claude(normalized_request, options_metadata) &&
      !bedrock_supports_adaptive_thinking(normalized_request,
                                          options_metadata) &&
      options_metadata.is_object() &&
      options_metadata.contains("maxTokens") &&
      options_metadata.at("maxTokens").is_number_integer()) {
    const auto model_max_tokens =
        options_metadata.at("maxTokens").get<std::int64_t>();
    if (model_max_tokens > 0) {
      const Json custom_budgets =
          options_metadata.contains("thinkingBudgets") &&
                  options_metadata.at("thinkingBudgets").is_object()
              ? options_metadata.at("thinkingBudgets")
              : Json::object();
      adjusted_thinking_tokens = bedrock_adjust_max_tokens_for_thinking(
          *normalized_request.max_output_tokens, model_max_tokens, *level,
          custom_budgets);
    }
  }

  Json body = Json{{"modelId", normalized_request.model.name},
                   {"stream", stream},
                   {"inferenceConfig", Json::object()},
                   {"messages", Json::array()}};

  if (normalized_request.max_output_tokens) {
    body["inferenceConfig"]["maxTokens"] =
        adjusted_thinking_tokens ? adjusted_thinking_tokens->max_tokens
                                 : *normalized_request.max_output_tokens;
  }
  if (normalized_request.temperature) {
    body["inferenceConfig"]["temperature"] = *normalized_request.temperature;
  }

  const auto replay_messages = provider_replay_messages(normalized_request.messages);
  std::map<std::string, std::string> bedrock_tool_call_id_map;
  for (std::size_t message_index = 0; message_index < replay_messages.size();
       ++message_index) {
    const auto& message = replay_messages[message_index];
    if (message.role == MessageRole::system ||
        message.role == MessageRole::developer) {
      const auto blocks = bedrock_content_blocks(
          message.content, message.role, supports_thinking_signature,
          supports_images, true);
      if (!blocks.empty()) {
        if (!body.contains("system")) {
          body["system"] = Json::array();
        }
        for (const auto& block : blocks) {
          body["system"].push_back(block);
        }
      }
      continue;
    }

    if (message.role == MessageRole::tool) {
      Json tool_results = Json::array();
      std::size_t next = message_index;
      for (; next < replay_messages.size(); ++next) {
        const auto& tool_message = replay_messages[next];
        if (tool_message.role != MessageRole::tool) {
          break;
        }
        const auto blocks =
            bedrock_content_blocks(tool_message.content, tool_message.role,
                                   supports_thinking_signature,
                                   supports_images, true,
                                   &bedrock_tool_call_id_map);
        for (const auto& block : blocks) {
          if (block.is_object() && block.contains("toolResult")) {
            tool_results.push_back(block);
          }
        }
      }
      if (!tool_results.empty()) {
        body["messages"].push_back(
            Json{{"role", "user"}, {"content", std::move(tool_results)}});
      }
      if (next > message_index) {
        message_index = next - 1;
      }
      continue;
    }

    const auto has_source_model =
        string_metadata_value(message.metadata, "provider").has_value() ||
        string_metadata_value(message.metadata, "api").has_value() ||
        string_metadata_value(message.metadata, "model").has_value();
    const bool same_provider_model =
        !has_source_model ||
        message_source_is_same_model(message, normalized_request.model);
    const auto blocks = bedrock_content_blocks(
        message.content, message.role, supports_thinking_signature,
        supports_images, same_provider_model, &bedrock_tool_call_id_map);
    if (blocks.empty()) {
      continue;
    }
    body["messages"].push_back(Json{
        {"role", message.role == MessageRole::assistant ? "assistant" : "user"},
        {"content", blocks}});
  }

  if (supports_cache) {
    if (body.contains("system") && body["system"].is_array() &&
        !body["system"].empty()) {
      body["system"].push_back(bedrock_cache_point(retention));
    }
    for (auto it = body["messages"].rbegin(); it != body["messages"].rend();
         ++it) {
      if (it->is_object() && it->value("role", std::string()) == "user" &&
          it->contains("content") && (*it)["content"].is_array()) {
        (*it)["content"].push_back(bedrock_cache_point(retention));
        break;
      }
    }
  }

  auto tool_config =
      bedrock_tool_config(normalized_request.tools, normalized_request.tool_choice);
  if (const auto override_tool_config =
          bedrock_tool_config_from_options_metadata(normalized_request.tools,
                                                    options_metadata)) {
    tool_config = *override_tool_config;
  }
  if (!tool_config.is_null()) {
    body["toolConfig"] = tool_config;
  }

  if (level && !level->empty() && *level != "off" &&
      bedrock_is_anthropic_claude(normalized_request, options_metadata)) {
    const bool govcloud =
        bedrock_is_govcloud_target(normalized_request, options_metadata);
    const auto display =
        govcloud ? std::optional<std::string>{}
                 : string_metadata_value(options_metadata, "thinkingDisplay")
                       .value_or("summarized");

    Json additional = Json::object();
    if (bedrock_supports_adaptive_thinking(normalized_request,
                                           options_metadata)) {
      additional["thinking"] = Json{{"type", "adaptive"}};
      if (display) {
        additional["thinking"]["display"] = *display;
      }
      additional["output_config"] =
          Json{{"effort", bedrock_thinking_effort(normalized_request,
                                                  options_metadata, *level)}};
    } else {
      additional["thinking"] =
          Json{{"type", "enabled"},
               {"budget_tokens",
                adjusted_thinking_tokens
                    ? adjusted_thinking_tokens->thinking_budget
                    : bedrock_thinking_budget(options_metadata, *level)}};
      if (display) {
        additional["thinking"]["display"] = *display;
      }
      if (options_metadata.value("interleavedThinking", true)) {
        additional["anthropic_beta"] =
            Json::array({"interleaved-thinking-2025-05-14"});
      }
    }
    body["additionalModelRequestFields"] = std::move(additional);
  }

  if (options_metadata.is_object() &&
      options_metadata.contains("requestMetadata") &&
      options_metadata.at("requestMetadata").is_object()) {
    body["requestMetadata"] = options_metadata.at("requestMetadata");
  }

  return body;
}

Json mistral_conversations_request_body(const CompletionRequest& request,
                                        bool stream,
                                        const Json& options_metadata) {
  const auto normalized_request = normalized(request);
  Json body = Json{{"model", normalized_request.model.name},
                   {"stream", stream},
                   {"messages", Json::array()}};

  if (normalized_request.temperature) {
    body["temperature"] = *normalized_request.temperature;
  }
  if (normalized_request.max_output_tokens) {
    body["maxTokens"] = *normalized_request.max_output_tokens;
  }

  const bool supports_images =
      mistral_supports_images(normalized_request, options_metadata);
  const auto replay_messages = provider_replay_messages(normalized_request.messages);
  MistralToolCallIdNormalizer normalize_tool_call_id;
  for (const auto& message : replay_messages) {
    if (message.role == MessageRole::system ||
        message.role == MessageRole::developer) {
      const auto content = mistral_text_content(message.content);
      if (content.empty()) {
        continue;
      }
      body["messages"].push_back(Json{{"role", "system"}, {"content", content}});
      continue;
    }

    if (message.role == MessageRole::assistant) {
      const bool same_provider_model =
          message_source_is_same_model(message, normalized_request.model);
      Json assistant = Json{{"role", "assistant"}};
      const auto content = mistral_assistant_content(message.content);
      const auto tool_calls =
          mistral_tool_calls(message.content, normalize_tool_call_id,
                             same_provider_model);
      if (!content.empty()) {
        assistant["content"] = content;
      }
      if (!tool_calls.empty()) {
        assistant["toolCalls"] = tool_calls;
      }
      if (assistant.contains("content") || assistant.contains("toolCalls")) {
        body["messages"].push_back(std::move(assistant));
      }
      continue;
    }

    if (message.role == MessageRole::tool) {
      for (const auto& item : message.content) {
        if (content_kind(item) != ContentKind::tool_result) {
          continue;
        }
        const auto& result = std::get<ToolResultContent>(item);
        const auto tool_call_id =
            normalize_tool_call_id.lookup(result.tool_call_id)
                .value_or(result.tool_call_id);
        body["messages"].push_back(
            Json{{"role", "tool"},
                 {"toolCallId", tool_call_id},
                 {"name", result.name},
                 {"content", mistral_tool_content(result, supports_images)}});
      }
      continue;
    }

    const auto content = mistral_user_content(message.content, supports_images);
    if (!content.is_null() && !(content.is_string() &&
                               trim_ascii(content.get<std::string>()).empty()) &&
        !(content.is_array() && content.empty())) {
      body["messages"].push_back(Json{{"role", "user"}, {"content", content}});
    }
  }

  if (!normalized_request.tools.empty()) {
    body["tools"] = Json::array();
    for (const auto& tool : normalized_request.tools) {
      body["tools"].push_back(mistral_tool(tool));
    }
    if (normalized_request.tool_choice.kind != ToolChoiceKind::auto_select) {
      body["toolChoice"] = mistral_tool_choice(normalized_request.tool_choice);
    }
  }
  if (const auto options_choice =
          mistral_tool_choice_from_options_metadata(options_metadata)) {
    body["toolChoice"] = *options_choice;
  }

  if (const auto level =
          mistral_requested_thinking_level(normalized_request.metadata)) {
    if (mistral_uses_reasoning_effort(normalized_request.model.name)) {
      if (const auto effort =
              mistral_reasoning_effort(*level, options_metadata)) {
        body["reasoningEffort"] = *effort;
      }
    } else {
      body["promptMode"] = "reasoning";
    }
  }

  return body;
}

Json mistral_request_options(
    const ProviderOptions& options,
    const std::map<std::string, std::string>& model_headers) {
  Json result = Json{{"retries", {{"strategy", "none"}}}};
  Json headers = Json::object();
  for (const auto& [key, value] : model_headers) {
    headers[key] = value;
  }
  for (const auto& [key, value] : options.headers) {
    headers[key] = value;
  }
  if (const auto session_id = string_metadata_value(options.metadata, "sessionId");
      session_id && !headers.contains("x-affinity")) {
    headers["x-affinity"] = *session_id;
  }
  if (!headers.empty()) {
    result["headers"] = std::move(headers);
  }
  return result;
}

CompletionResponse parse_openai_response(const Json& response_json) {
  CompletionResponse response;
  response.message.role = MessageRole::assistant;
  response.provider_response_id = response_json.value("id", std::string());
  response.response_model = non_empty_string_field(response_json, "model");
  if (const auto service_tier =
          non_empty_string_field(response_json, "service_tier")) {
    response.metadata["service_tier"] = *service_tier;
  }
  response.usage = openai_usage(response_json);

  bool saw_tool_call = false;
  for (const auto& item : response_json.value("output", Json::array())) {
    if (!item.is_object()) {
      continue;
    }

    if (item.value("type", std::string()) == "reasoning") {
      const auto text = openai_reasoning_text(item);
      if (!text.empty()) {
        response.message.content.emplace_back(
            ThinkingContent{text, item.dump(), false});
      }
    } else if (item.value("type", std::string()) == "message") {
      if (item.contains("id") && item.at("id").is_string()) {
        response.message.id = item.at("id").get<std::string>();
      }

      StreamEvent event;
      append_content_from_openai_message(event, item);
      if (event.content) {
        response.message.content.emplace_back(*event.content);
      }
    } else if (auto call = tool_call_from_openai_item(item)) {
      saw_tool_call = true;
      response.message.content.emplace_back(*call);
    }
  }

  response.stop_reason = openai_stop_reason(response_json, saw_tool_call);
  if (!response.message.id && response.provider_response_id) {
    response.message.id = response.provider_response_id;
  }
  return response;
}

std::optional<ToolCallContent> tool_call_from_openai_chat_item(
    const Json& item) {
  if (!item.is_object() || item.value("type", std::string()) != "function") {
    return std::nullopt;
  }
  const auto function = item.value("function", Json::object());

  ToolCallContent call;
  call.id = item.value("id", std::string());
  call.name = function.value("name", std::string());

  const auto arguments = function.value("arguments", std::string("{}"));
  try {
    call.arguments = Json::parse(arguments.empty() ? "{}" : arguments);
  } catch (const Json::parse_error&) {
    call.arguments = Json{{"raw", arguments}};
  }

  if (call.id.empty() || call.name.empty()) {
    return std::nullopt;
  }
  return call;
}

CompletionResponse parse_openai_completions_response(const Json& response_json) {
  CompletionResponse response;
  response.message.role = MessageRole::assistant;
  response.provider_response_id = response_json.value("id", std::string());
  response.response_model = non_empty_string_field(response_json, "model");
  response.usage =
      openai_completions_usage(response_json.value("usage", Json::object()));

  const auto choices = response_json.value("choices", Json::array());
  if (!choices.empty() && choices.front().is_object()) {
    const auto& choice = choices.front();
    const auto message = choice.value("message", Json::object());
    const auto content = message.value("content", std::string());
    if (!content.empty()) {
      response.message.content.emplace_back(TextContent{content, std::nullopt});
    }

    std::string reasoning;
    std::optional<std::string> reasoning_field;
    for (const auto& field :
         std::vector<std::string>{"reasoning_content", "reasoning",
                                  "reasoning_text"}) {
      const auto value = message.value(field, std::string());
      if (!value.empty()) {
        reasoning = value;
        reasoning_field = field;
        break;
      }
    }
    if (!reasoning.empty()) {
      response.message.content.emplace_back(
          ThinkingContent{reasoning, reasoning_field, false});
    }

    bool saw_tool_call = false;
    for (const auto& tool : message.value("tool_calls", Json::array())) {
      if (auto call = tool_call_from_openai_chat_item(tool)) {
        saw_tool_call = true;
        for (const auto& detail :
             message.value("reasoning_details", Json::array())) {
          if (!detail.is_object() ||
              detail.value("type", std::string()) != "reasoning.encrypted" ||
              !detail.contains("id") || !detail.at("id").is_string() ||
              detail.at("id").get<std::string>() != call->id ||
              !detail.contains("data") || !detail.at("data").is_string()) {
            continue;
          }
          call->thought_signature = detail.dump();
          break;
        }
        response.message.content.emplace_back(*call);
      }
    }
    response.stop_reason =
        saw_tool_call ? StopReason::tool_call
                      : openai_completions_stop_reason(
                            choice.value("finish_reason", std::string()));
    if (!saw_tool_call) {
      if (const auto error = openai_completions_finish_reason_error(
              choice.value("finish_reason", std::string()))) {
        response.metadata["error"] = *error;
      }
    }
  }

  if (!response.message.id && response.provider_response_id) {
    response.message.id = response.provider_response_id;
  }
  return response;
}

std::vector<StreamEvent> parse_openai_responses_sse(std::string_view transcript) {
  std::vector<StreamEvent> events;
  std::optional<ToolCallContent> pending_tool_call;
  std::string pending_tool_item_id;
  std::string pending_tool_arguments;
  bool pending_tool_arguments_finalized = false;
  std::string pending_reasoning_text;
  bool pending_reasoning_item = false;
  bool pending_reasoning_summary_part = false;
  std::optional<std::string> pending_message_content_part_type;

  const auto finish_pending_tool_call =
      [&events, &pending_tool_call, &pending_tool_item_id,
       &pending_tool_arguments, &pending_tool_arguments_finalized](
          const Json& item, const std::string& provider_type) {
        std::optional<ToolCallContent> call;
        if (pending_tool_call) {
          call = pending_tool_call;
          const auto arguments =
              pending_tool_arguments_finalized
                  ? pending_tool_arguments
                  : item.value("arguments", pending_tool_arguments);
          call->arguments = parse_streamed_tool_arguments_or_raw(arguments);
        } else {
          call = tool_call_from_openai_item(item);
        }

        if (!call) {
          return;
        }

        StreamEvent event;
        event.kind = EventKind::tool_call;
        event.content = *call;
        event.metadata = Json{{"provider_type", provider_type}};
        if (!pending_tool_item_id.empty()) {
          event.metadata["provider_item_id"] = pending_tool_item_id;
        } else if (item.contains("id") && item.at("id").is_string()) {
          event.metadata["provider_item_id"] = item.at("id").get<std::string>();
        }
        events.push_back(std::move(event));

        pending_tool_call.reset();
        pending_tool_item_id.clear();
        pending_tool_arguments.clear();
        pending_tool_arguments_finalized = false;
      };

  for (const auto& sse : parse_sse_events(transcript)) {
    if (sse.data == "[DONE]") {
      continue;
    }

    Json payload;
    try {
      payload = Json::parse(sanitize_utf8(sse.data));
    } catch (const Json::parse_error&) {
      StreamEvent event;
      event.kind = EventKind::error;
      event.error = "invalid SSE JSON payload";
      event.metadata = Json{{"data", sse.data}};
      events.push_back(std::move(event));
      continue;
    }

    const auto type = payload.value("type", std::string());
    if (type == "response.created") {
      const auto response = payload.value("response", Json::object());
      if (response.contains("id") && response.at("id").is_string()) {
        StreamEvent event;
        event.kind = EventKind::message_start;
        event.message_id = response.at("id").get<std::string>();
        event.role = MessageRole::assistant;
        event.metadata = Json{{"provider_type", type},
                              {"provider_response_id",
                               response.at("id").get<std::string>()}};
        events.push_back(std::move(event));
      }
    } else if (type == "response.output_item.added" &&
        payload.value("item", Json::object()).value("type", std::string()) ==
            "message") {
      StreamEvent event;
      event.kind = EventKind::message_start;
      const auto item = payload.value("item", Json::object());
      event.message_id = item.value("id", std::string());
      event.role = MessageRole::assistant;
      event.metadata = Json{{"provider_type", type}};
      events.push_back(std::move(event));
      pending_message_content_part_type.reset();
      pending_reasoning_item = false;
      pending_reasoning_summary_part = false;
    } else if (type == "response.output_item.added" &&
               payload.value("item", Json::object()).value("type", std::string()) ==
                   "reasoning") {
      pending_reasoning_text.clear();
      pending_reasoning_item = true;
      pending_reasoning_summary_part = false;
    } else if (type == "response.output_item.added" &&
               payload.value("item", Json::object()).value("type", std::string()) ==
                   "function_call") {
      const auto item = payload.value("item", Json::object());
      pending_tool_call = tool_call_from_openai_item(item).value_or(
          ToolCallContent{item.value("call_id", std::string()),
                          item.value("name", std::string()), Json::object()});
      pending_tool_item_id = item.value("id", std::string());
      pending_tool_arguments = item.value("arguments", std::string());
      pending_tool_arguments_finalized = false;
      pending_reasoning_item = false;
      pending_reasoning_summary_part = false;
      if (!pending_tool_arguments.empty()) {
        pending_tool_call->arguments =
            parse_streamed_tool_arguments_or_raw(pending_tool_arguments);
      }
    } else if (type == "response.reasoning_summary_part.added") {
      if (pending_reasoning_item) {
        pending_reasoning_summary_part = true;
      }
    } else if (type == "response.reasoning_summary_text.delta") {
      const auto delta = payload.value("delta", std::string());
      if (pending_reasoning_item && pending_reasoning_summary_part &&
          !delta.empty()) {
        pending_reasoning_text += delta;
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = delta;
        event.content = ThinkingContent{delta, std::nullopt, false};
        event.metadata = Json{{"provider_type", type}, {"content_type", "thinking"}};
        events.push_back(std::move(event));
      }
    } else if (type == "response.reasoning_text.delta") {
      const auto delta = payload.value("delta", std::string());
      if (pending_reasoning_item && !delta.empty()) {
        pending_reasoning_text += delta;
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = delta;
        event.content = ThinkingContent{delta, std::nullopt, false};
        event.metadata = Json{{"provider_type", type}, {"content_type", "thinking"}};
        events.push_back(std::move(event));
      }
    } else if (type == "response.reasoning_summary_part.done") {
      if (pending_reasoning_item && pending_reasoning_summary_part) {
        pending_reasoning_text += "\n\n";
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = "\n\n";
        event.content = ThinkingContent{"\n\n", std::nullopt, false};
        event.metadata =
            Json{{"provider_type", type}, {"content_type", "thinking"}};
        events.push_back(std::move(event));
      }
    } else if (type == "response.content_part.added") {
      const auto part = payload.value("part", Json::object());
      const auto part_type = part.value("type", std::string());
      if (part_type == "output_text" || part_type == "refusal") {
        pending_message_content_part_type = part_type;
      } else {
        pending_message_content_part_type = "ignored";
      }
    } else if (type == "response.output_text.delta") {
      if (pending_message_content_part_type &&
          *pending_message_content_part_type != "output_text") {
        continue;
      }
      const auto delta = payload.value("delta", std::string());
      if (!delta.empty()) {
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = delta;
        event.content = TextContent{delta, std::nullopt};
        event.metadata = Json{{"provider_type", type}, {"content_type", "text"}};
        events.push_back(std::move(event));
      }
    } else if (type == "response.refusal.delta") {
      if (pending_message_content_part_type &&
          *pending_message_content_part_type != "refusal") {
        continue;
      }
      const auto delta = payload.value("delta", std::string());
      if (!delta.empty()) {
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = delta;
        event.content = TextContent{delta, std::nullopt};
        event.metadata = Json{{"provider_type", type},
                              {"content_type", "text"},
                              {"refusal", true}};
        events.push_back(std::move(event));
      }
    } else if (type == "response.function_call_arguments.delta") {
      const auto delta = payload.value("delta", std::string());
      if (pending_tool_call && !delta.empty()) {
        pending_tool_arguments += delta;
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = delta;
        event.metadata = Json{{"provider_type", type},
                              {"content_type", "tool_call_arguments"}};
        if (!pending_tool_item_id.empty()) {
          event.metadata["provider_item_id"] = pending_tool_item_id;
        }
        events.push_back(std::move(event));
      }
    } else if (type == "response.function_call_arguments.done") {
      if (pending_tool_call) {
        const auto previous = pending_tool_arguments;
        pending_tool_arguments =
            payload.value("arguments", pending_tool_arguments);
        pending_tool_arguments_finalized = true;
        if (pending_tool_arguments.rfind(previous, 0) == 0 &&
            pending_tool_arguments.size() > previous.size()) {
          StreamEvent event;
          event.kind = EventKind::content_delta;
          event.delta = pending_tool_arguments.substr(previous.size());
          event.metadata = Json{{"provider_type", type},
                                {"content_type", "tool_call_arguments"}};
          if (!pending_tool_item_id.empty()) {
            event.metadata["provider_item_id"] = pending_tool_item_id;
          }
          events.push_back(std::move(event));
        }
      }
    } else if (type == "response.output_item.done") {
      const auto item = payload.value("item", Json::object());
      if (item.value("type", std::string()) == "reasoning") {
        const auto text = openai_reasoning_text(item);
        if (!text.empty()) {
          pending_reasoning_text = text;
        }
        if (!pending_reasoning_text.empty()) {
          StreamEvent event;
          event.kind = EventKind::content_done;
          event.content =
              ThinkingContent{pending_reasoning_text, item.dump(), false};
          event.metadata = Json{{"provider_type", type},
                                {"content_type", "thinking"}};
          if (item.contains("id") && item.at("id").is_string()) {
            event.metadata["provider_item_id"] = item.at("id").get<std::string>();
          }
          events.push_back(std::move(event));
        }
        pending_reasoning_text.clear();
        pending_reasoning_item = false;
        pending_reasoning_summary_part = false;
        continue;
      }

      if (auto call = tool_call_from_openai_item(payload.value("item", Json::object()))) {
        if (!pending_tool_call) {
          pending_tool_call = *call;
          pending_tool_item_id = item.value("id", std::string());
        }
        finish_pending_tool_call(item, type);
        continue;
      }

      StreamEvent event;
      append_content_from_openai_message(event, item);
      if (event.content) {
        event.metadata = Json{{"provider_type", type}};
        events.push_back(std::move(event));
      }
      pending_message_content_part_type.reset();
    } else if (type == "response.failed") {
      StreamEvent event;
      event.kind = EventKind::error;
      event.error =
          openai_responses_failed_message(payload.value("response", Json::object()));
      event.metadata = Json{{"provider_type", type}};
      events.push_back(std::move(event));
    } else if (type == "response.completed" || type == "response.done" ||
               type == "response.incomplete") {
      const auto response = payload.value("response", Json::object());
      if (response.contains("usage") && response.at("usage").is_object()) {
        StreamEvent usage_event;
        usage_event.kind = EventKind::usage;
        usage_event.usage = openai_usage(response);
        usage_event.metadata = Json{{"provider_type", type}};
        if (const auto service_tier =
                non_empty_string_field(response, "service_tier")) {
          usage_event.metadata["service_tier"] = *service_tier;
        }
        events.push_back(std::move(usage_event));
      }

      StreamEvent done;
      done.kind = EventKind::message_done;
      done.stop_reason = openai_stop_reason(response);
      if (const auto service_tier =
              non_empty_string_field(response, "service_tier")) {
        done.metadata["service_tier"] = *service_tier;
      }
      if (response.contains("id") && response.at("id").is_string()) {
        done.metadata["provider_response_id"] = response.at("id").get<std::string>();
      }
      done.metadata["provider_type"] = type;
      events.push_back(std::move(done));
    } else if (type == "error") {
      StreamEvent event;
      event.kind = EventKind::error;
      const auto code = payload.value("code", std::string());
      const auto message = payload.value("message", std::string());
      if (!code.empty() || !message.empty()) {
        event.error = "Error Code ";
        *event.error += code;
        *event.error += ": ";
        *event.error += message;
      } else {
        event.error = "Unknown error";
      }
      event.metadata = Json{{"provider_type", type}};
      events.push_back(std::move(event));
    }
  }

  return events;
}

std::vector<StreamEvent> parse_openai_completions_sse(
    std::string_view transcript) {
  std::vector<StreamEvent> events;

  struct PendingToolCall {
    ToolCallContent call;
    std::string arguments;
  };
  std::map<int, PendingToolCall> pending_tool_calls;
  std::map<std::string, int> pending_tool_call_ids;
  int next_synthetic_tool_index = 100000;
  bool saw_start = false;
  std::optional<std::string> latest_response_model;

  const auto emit_pending_tool_calls =
      [&events, &pending_tool_calls, &pending_tool_call_ids,
       &latest_response_model]() {
    for (auto& entry : pending_tool_calls) {
      auto& pending = entry.second;
      try {
        pending.call.arguments =
            Json::parse(pending.arguments.empty() ? "{}" : pending.arguments);
      } catch (const Json::parse_error&) {
        pending.call.arguments = Json{{"raw", pending.arguments}};
      }

      StreamEvent event;
      event.kind = EventKind::tool_call;
      event.content = pending.call;
      event.metadata = Json{{"provider_type", "chat.completion.chunk"},
                            {"content_type", "tool_call"},
                            {"tool_index", entry.first}};
      if (latest_response_model) {
        event.metadata["response_model"] = *latest_response_model;
      }
      events.push_back(std::move(event));
    }
    pending_tool_calls.clear();
    pending_tool_call_ids.clear();
  };

  for (const auto& sse : parse_sse_events(transcript)) {
    if (sse.data == "[DONE]") {
      continue;
    }

    Json payload;
    try {
      payload = Json::parse(sanitize_utf8(sse.data));
    } catch (const Json::parse_error&) {
      StreamEvent event;
      event.kind = EventKind::error;
      event.error = "invalid SSE JSON payload";
      event.metadata = Json{{"data", sse.data}};
      events.push_back(std::move(event));
      continue;
    }
    if (!payload.is_object()) {
      continue;
    }
    const auto response_model = non_empty_string_field(payload, "model");
    if (response_model) {
      latest_response_model = response_model;
    }

    if (!saw_start) {
      StreamEvent event;
      event.kind = EventKind::message_start;
      event.role = MessageRole::assistant;
      if (payload.contains("id") && payload.at("id").is_string()) {
        event.message_id = payload.at("id").get<std::string>();
        event.metadata["provider_response_id"] = *event.message_id;
      }
      event.metadata["provider_type"] = "chat.completion.chunk";
      if (latest_response_model) {
        event.metadata["response_model"] = *latest_response_model;
      }
      events.push_back(std::move(event));
      saw_start = true;
    }

    const auto has_payload_usage =
        payload.contains("usage") && payload.at("usage").is_object();
    if (has_payload_usage) {
      StreamEvent event;
      event.kind = EventKind::usage;
      event.usage = openai_completions_usage(payload.at("usage"));
      event.metadata = Json{{"provider_type", "chat.completion.chunk"}};
      if (latest_response_model) {
        event.metadata["response_model"] = *latest_response_model;
      }
      events.push_back(std::move(event));
    }

    const auto choices = payload.value("choices", Json::array());
    if (choices.empty() || !choices.front().is_object()) {
      continue;
    }

    const auto& choice = choices.front();
    if (!has_payload_usage && choice.contains("usage") &&
        choice.at("usage").is_object()) {
      StreamEvent event;
      event.kind = EventKind::usage;
      event.usage = openai_completions_usage(choice.at("usage"));
      event.metadata = Json{{"provider_type", "chat.completion.chunk"},
                            {"usage_source", "choice"}};
      if (latest_response_model) {
        event.metadata["response_model"] = *latest_response_model;
      }
      events.push_back(std::move(event));
    }

    const auto delta = choice.value("delta", Json::object());
    const auto text = string_field_or(delta, "content");
    if (!text.empty()) {
      StreamEvent event;
      event.kind = EventKind::content_delta;
      event.delta = text;
      event.content = TextContent{text, std::nullopt};
      event.metadata = Json{{"provider_type", "chat.completion.chunk"},
                            {"content_type", "text"}};
      if (latest_response_model) {
        event.metadata["response_model"] = *latest_response_model;
      }
      events.push_back(std::move(event));
    }

    const std::vector<std::string> reasoning_fields = {
        "reasoning_content", "reasoning", "reasoning_text"};
    for (const auto& field : reasoning_fields) {
      const auto reasoning = string_field_or(delta, field);
      if (!reasoning.empty()) {
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = reasoning;
        event.content = ThinkingContent{reasoning, field, false};
        event.metadata = Json{{"provider_type", "chat.completion.chunk"},
                              {"content_type", "thinking"},
                              {"reasoning_field", field}};
        if (latest_response_model) {
          event.metadata["response_model"] = *latest_response_model;
        }
        events.push_back(std::move(event));
        break;
      }
    }

    for (const auto& tool : delta.value("tool_calls", Json::array())) {
      if (!tool.is_object()) {
        continue;
      }
      int index = 0;
      const auto tool_id =
          tool.contains("id") && tool.at("id").is_string()
              ? std::optional<std::string>{tool.at("id").get<std::string>()}
              : std::nullopt;
      if (tool.contains("index") && tool.at("index").is_number_integer()) {
        index = tool.at("index").get<int>();
      } else if (tool_id && pending_tool_call_ids.count(*tool_id) > 0) {
        index = pending_tool_call_ids.at(*tool_id);
      } else {
        index = next_synthetic_tool_index++;
      }
      auto& pending = pending_tool_calls[index];
      if (tool_id) {
        if (pending.call.id.empty()) {
          pending.call.id = *tool_id;
        }
        pending_tool_call_ids[*tool_id] = index;
      }
      const auto function = tool.value("function", Json::object());
      if (function.contains("name") && function.at("name").is_string()) {
        pending.call.name = function.at("name").get<std::string>();
      }
      const auto arguments = string_field_or(function, "arguments");
      if (!arguments.empty()) {
        pending.arguments += arguments;
        StreamEvent event;
        event.kind = EventKind::content_delta;
        event.delta = arguments;
        event.metadata = Json{{"provider_type", "chat.completion.chunk"},
                              {"content_type", "tool_call_arguments"},
                              {"tool_index", index}};
        if (!pending.call.id.empty()) {
          event.metadata["tool_call_id"] = pending.call.id;
        }
        if (latest_response_model) {
          event.metadata["response_model"] = *latest_response_model;
        }
        events.push_back(std::move(event));
      }
    }

    for (const auto& detail : delta.value("reasoning_details", Json::array())) {
      if (!detail.is_object() ||
          string_field_or(detail, "type") != "reasoning.encrypted" ||
          !detail.contains("id") || !detail.at("id").is_string() ||
          !detail.contains("data") || !detail.at("data").is_string()) {
        continue;
      }
      const auto id = detail.at("id").get<std::string>();
      for (auto& [_, pending] : pending_tool_calls) {
        if (pending.call.id == id) {
          pending.call.thought_signature = detail.dump();
          break;
        }
      }
    }

    if (choice.contains("finish_reason") && !choice.at("finish_reason").is_null()) {
      emit_pending_tool_calls();
      const auto finish_reason = string_field_or(choice, "finish_reason");
      if (const auto error =
              openai_completions_finish_reason_error(finish_reason)) {
        StreamEvent event;
        event.kind = EventKind::error;
        event.error = *error;
        event.metadata = Json{{"provider_type", "chat.completion.chunk"}};
        if (payload.contains("id") && payload.at("id").is_string()) {
          event.message_id = payload.at("id").get<std::string>();
          event.metadata["provider_response_id"] = *event.message_id;
        }
        if (latest_response_model) {
          event.metadata["response_model"] = *latest_response_model;
        }
        events.push_back(std::move(event));
        continue;
      }
      StreamEvent done;
      done.kind = EventKind::message_done;
      done.stop_reason = openai_completions_stop_reason(finish_reason);
      if (payload.contains("id") && payload.at("id").is_string()) {
        done.message_id = payload.at("id").get<std::string>();
        done.metadata["provider_response_id"] = *done.message_id;
      }
      done.metadata["provider_type"] = "chat.completion.chunk";
      if (latest_response_model) {
        done.metadata["response_model"] = *latest_response_model;
      }
      events.push_back(std::move(done));
    }
  }

  if (!pending_tool_calls.empty()) {
    emit_pending_tool_calls();
  }
  return events;
}

CompletionResponse completion_response_from_events(
    const std::vector<StreamEvent>& events) {
  CompletionResponse response;
  bool saw_terminal = false;
  bool saw_error = false;
  response.message.role = MessageRole::assistant;
  std::string delta_text;
  std::string thinking_delta_text;
  std::optional<std::string> thinking_delta_signature;

  for (const auto& event : events) {
    if (const auto response_model =
            non_empty_string_field(event.metadata, "response_model")) {
      response.response_model = response_model;
    }
    if (const auto service_tier =
            non_empty_string_field(event.metadata, "service_tier")) {
      response.metadata["service_tier"] = *service_tier;
    }
    if (!response.provider_response_id) {
      if (const auto provider_response_id =
              non_empty_string_field(event.metadata, "provider_response_id")) {
        response.provider_response_id = provider_response_id;
      }
    }
    if (event.kind == EventKind::message_start) {
      if (event.message_id && !event.message_id->empty()) {
        response.message.id = event.message_id;
      }
      if (event.role) {
        response.message.role = *event.role;
      }
      if (event.metadata.contains("provider_response_id") &&
          event.metadata.at("provider_response_id").is_string()) {
        response.provider_response_id =
            event.metadata.at("provider_response_id").get<std::string>();
      }
    } else if (event.kind == EventKind::content_delta && event.delta) {
      const auto content_type =
          event.metadata.value("content_type", std::string("text"));
      if (content_type == "thinking") {
        thinking_delta_text += *event.delta;
        if (!thinking_delta_signature && event.content &&
            content_kind(*event.content) == ContentKind::thinking) {
          thinking_delta_signature =
              std::get<ThinkingContent>(*event.content).signature;
        }
      } else if (content_type == "text") {
        delta_text += *event.delta;
      }
    } else if (event.kind == EventKind::content_done && event.content) {
      response.message.content.push_back(*event.content);
      if (content_kind(*event.content) == ContentKind::text) {
        delta_text.clear();
      } else if (content_kind(*event.content) == ContentKind::thinking) {
        thinking_delta_text.clear();
        thinking_delta_signature.reset();
      }
    } else if (event.kind == EventKind::tool_call && event.content) {
      if (content_kind(*event.content) != ContentKind::tool_call) {
        continue;
      }
      const auto& incoming = std::get<ToolCallContent>(*event.content);
      auto existing = std::find_if(
          response.message.content.begin(), response.message.content.end(),
          [&incoming](const Content& content) {
            return content_kind(content) == ContentKind::tool_call &&
                   std::get<ToolCallContent>(content).id == incoming.id;
          });
      if (existing != response.message.content.end()) {
        *existing = *event.content;
        continue;
      }
      if (!delta_text.empty()) {
        response.message.content.emplace_back(
            TextContent{delta_text, std::nullopt});
        delta_text.clear();
      }
      if (!thinking_delta_text.empty()) {
        response.message.content.emplace_back(
            ThinkingContent{thinking_delta_text, thinking_delta_signature, false});
        thinking_delta_text.clear();
        thinking_delta_signature.reset();
      }
      response.message.content.emplace_back(*event.content);
    } else if (event.kind == EventKind::tool_result && event.content) {
      response.message.content.emplace_back(*event.content);
    } else if (event.kind == EventKind::usage && event.usage) {
      response.usage = *event.usage;
    } else if (event.kind == EventKind::message_done) {
      if (event.stop_reason && *event.stop_reason != StopReason::unknown) {
        saw_terminal = true;
      }
      if (event.stop_reason && !saw_error) {
        response.stop_reason = *event.stop_reason;
      }
      if (event.message_id && !event.message_id->empty() && !response.message.id) {
        response.message.id = event.message_id;
      }
      if (event.metadata.contains("provider_response_id") &&
          event.metadata.at("provider_response_id").is_string()) {
        response.provider_response_id =
            event.metadata.at("provider_response_id").get<std::string>();
      }
    } else if (event.kind == EventKind::error) {
      saw_error = true;
      response.stop_reason = event.stop_reason == StopReason::aborted
          ? StopReason::aborted : StopReason::error;
      const auto error = event.error.value_or("Provider stream error");
      response.metadata["error"] = error;
      response.metadata["errorMessage"] = error;
    }
  }

  if (!delta_text.empty()) {
    response.message.content.emplace_back(TextContent{delta_text, std::nullopt});
  }
  if (!thinking_delta_text.empty()) {
    response.message.content.emplace_back(
        ThinkingContent{thinking_delta_text, thinking_delta_signature, false});
  }
  if (!response.message.id && response.provider_response_id) {
    response.message.id = response.provider_response_id;
  }
  if (!saw_terminal && !saw_error) {
    response.stop_reason = StopReason::error;
    response.metadata["errorMessage"] = "Provider stream ended without a completion signal";
    response.metadata["error"] = response.metadata["errorMessage"];
  }
  normalize(response.usage);
  return response;
}

CompletionResponse aborted_provider_response(const CompletionRequest& request) {
  CompletionResponse response;
  response.message.role = MessageRole::assistant;
  response.message.metadata = Json{{"provider", request.model.provider},
                                   {"api", request.model.api},
                                   {"model", request.model.name}};
  response.stop_reason = StopReason::aborted;
  response.response_model = request.model.name;
  response.metadata["errorMessage"] = "Request was aborted";
  return response;
}

void push_aborted_provider_event(EventSink& sink) {
  StreamEvent event;
  event.kind = EventKind::error;
  event.stop_reason = StopReason::aborted;
  event.error = "Request was aborted";
  event.metadata["errorMessage"] = "Request was aborted";
  sink.push(event);
}

OpenAIResponsesProvider::OpenAIResponsesProvider(
    std::unique_ptr<HttpTransport> transport, std::string endpoint)
    : OpenAIResponsesProvider(ProviderDescriptor{}, std::move(transport),
                              std::move(endpoint)) {}

OpenAIResponsesProvider::OpenAIResponsesProvider(
    ProviderDescriptor descriptor, std::unique_ptr<HttpTransport> transport,
    std::string endpoint)
    : descriptor_(std::move(descriptor)),
      transport_(std::move(transport)),
      endpoint_(std::move(endpoint)) {
  if (!transport_) {
    throw std::invalid_argument("OpenAI Responses provider requires transport");
  }
  if (descriptor_.id.empty()) {
    descriptor_.id = "openai";
  }
  if (descriptor_.display_name.empty()) {
    descriptor_.display_name = "OpenAI";
  }
  if (descriptor_.api.empty()) {
    descriptor_.api = "openai-responses";
  }
  descriptor_.transport = ProviderTransport::sse;
  descriptor_.capabilities.streaming = true;
  descriptor_.capabilities.tool_calls = true;
  descriptor_.capabilities.vision = true;
  descriptor_.capabilities.reasoning = true;
}

const ProviderDescriptor& OpenAIResponsesProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse OpenAIResponsesProvider::complete(
    const CompletionRequest& request, const ProviderOptions& options) {
  if (options.aborted) {
    return aborted_provider_response(request);
  }
  const auto response = send_request(request, options, false);
  auto completion = parse_openai_response(Json::parse(sanitize_utf8(response.body)));
  apply_openai_service_tier_accounting(completion, options,
                                       request.model.name);
  return completion;
}

void OpenAIResponsesProvider::stream(const CompletionRequest& request,
                                     const ProviderOptions& options,
                                     EventSink& sink) {
  if (options.aborted) {
    push_aborted_provider_event(sink);
    return;
  }
  const auto response = send_request(request, options, true);
  auto events = parse_openai_responses_sse(response.body);
  apply_openai_service_tier_accounting(events, options, request.model.name);
  for (const auto& event : events) {
    sink.push(event);
  }
}

HttpResponse OpenAIResponsesProvider::send_request(
    const CompletionRequest& request, const ProviderOptions& options,
    bool stream) {
  HttpRequest http;
  http.method = "POST";
  http.url = provider_endpoint_from_options(options, endpoint_, descriptor_.id,
                                            "responses");
  http.headers = metadata_headers(options.metadata);
  apply_openrouter_attribution_headers(http.headers, request, options, http.url);
  if (provider_key(request.model.provider) == "github-copilot") {
    merge_overriding_headers(http.headers,
                             github_copilot_dynamic_headers(request));
  }
  http.headers.emplace("accept", stream ? "text/event-stream" : "application/json");
  http.headers.emplace("content-type", "application/json");
  http.timeout_ms = options.timeout_ms;
  http.proxy_url = options.proxy_url;
  if (options.api_key) {
    apply_openai_family_auth_headers(http.headers, request.model.provider,
                                     options.api_key);
  } else if (const auto env_api_key =
                 get_env_api_key(request.model.provider)) {
    apply_openai_family_auth_headers(http.headers, request.model.provider,
                                     env_api_key);
  }
  const auto session_id = string_metadata_value(options.metadata, "sessionId");
  const auto cache_retention = cache_retention_from_options_metadata(options.metadata);
  const auto compat = openai_responses_compat_from_metadata(options.metadata);
  if (session_id && cache_retention != "none") {
    if (compat.value("sendSessionIdHeader", true)) {
      http.headers["session_id"] = *session_id;
    }
    http.headers["x-client-request-id"] = *session_id;
  }
  merge_overriding_headers(http.headers, options.headers);
  http.body = provider_safe_json_dump(
      openai_responses_request_body(request, stream, options.metadata));

  return send_with_retries(*transport_, http, options);
}

std::shared_ptr<Provider> make_openai_responses_provider(
    std::unique_ptr<HttpTransport> transport, std::string endpoint) {
  return std::make_shared<OpenAIResponsesProvider>(std::move(transport),
                                                  std::move(endpoint));
}

OpenAICodexResponsesProvider::OpenAICodexResponsesProvider(
    std::unique_ptr<HttpTransport> transport, std::string endpoint)
    : OpenAICodexResponsesProvider(ProviderDescriptor{}, std::move(transport),
                                   std::move(endpoint)) {}

OpenAICodexResponsesProvider::OpenAICodexResponsesProvider(
    ProviderDescriptor descriptor, std::unique_ptr<HttpTransport> transport,
    std::string endpoint)
    : descriptor_(std::move(descriptor)),
      transport_(std::move(transport)),
      endpoint_(std::move(endpoint)) {
  if (!transport_) {
    throw std::invalid_argument("OpenAI Codex Responses provider requires transport");
  }
  if (descriptor_.id.empty()) {
    descriptor_.id = "openai-codex";
  }
  if (descriptor_.display_name.empty()) {
    descriptor_.display_name = "OpenAI Codex";
  }
  if (descriptor_.api.empty()) {
    descriptor_.api = "openai-codex-responses";
  }
  descriptor_.transport = ProviderTransport::sse;
  descriptor_.capabilities.streaming = true;
  descriptor_.capabilities.tool_calls = true;
  descriptor_.capabilities.vision = true;
  descriptor_.capabilities.reasoning = true;
  descriptor_.capabilities.session_caching = true;
}

const ProviderDescriptor& OpenAICodexResponsesProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse OpenAICodexResponsesProvider::complete(
    const CompletionRequest& request, const ProviderOptions& options) {
  if (options.aborted) {
    return aborted_provider_response(request);
  }
  const auto response = send_request(request, options, true);
  auto events = parse_openai_responses_sse(response.body);
  apply_openai_codex_service_tier_accounting(events, options,
                                             request.model.name);
  return completion_response_from_events(events);
}

void OpenAICodexResponsesProvider::stream(const CompletionRequest& request,
                                          const ProviderOptions& options,
                                          EventSink& sink) {
  if (options.aborted) {
    push_aborted_provider_event(sink);
    return;
  }
  const auto response = send_request(request, options, true);
  auto events = parse_openai_responses_sse(response.body);
  apply_openai_codex_service_tier_accounting(events, options,
                                             request.model.name);
  for (const auto& event : events) {
    sink.push(event);
  }
}

HttpResponse OpenAICodexResponsesProvider::send_request(
    const CompletionRequest& request, const ProviderOptions& options,
    bool stream) {
  const auto token = options.api_key
                         ? options.api_key
                         : get_env_api_key(request.model.provider);
  if (!token || token->empty()) {
    throw std::runtime_error(
        missing_provider_credential_message(request.model.provider));
  }
  const auto account_id = openai_codex_account_id(*token);
  if (!account_id) {
    throw std::runtime_error("Failed to extract accountId from token");
  }

  HttpRequest http;
  http.method = "POST";
  http.url = provider_endpoint_from_options(options, endpoint_, descriptor_.id,
                                            "codex/responses");
  http.headers = metadata_headers(options.metadata);
  merge_overriding_headers(http.headers, options.headers);
  http.headers["Authorization"] = "Bearer " + *token;
  http.headers["chatgpt-account-id"] = *account_id;
  http.headers["originator"] = "pi";
  http.headers["User-Agent"] = qt::foundation::pi_user_agent();
  http.headers["OpenAI-Beta"] = "responses=experimental";
  http.headers["accept"] = "text/event-stream";
  http.headers["content-type"] = "application/json";
  http.timeout_ms = options.timeout_ms;
  http.proxy_url = options.proxy_url;

  if (const auto session_id = string_metadata_value(options.metadata, "sessionId")) {
    http.headers["session_id"] = *session_id;
    http.headers["x-client-request-id"] = *session_id;
  }

  http.body = provider_safe_json_dump(
      openai_codex_responses_request_body(request, stream, options.metadata));

  return send_with_retries(*transport_, http, options);
}

std::shared_ptr<Provider> make_openai_codex_responses_provider(
    std::unique_ptr<HttpTransport> transport, std::string endpoint) {
  return std::make_shared<OpenAICodexResponsesProvider>(std::move(transport),
                                                       std::move(endpoint));
}

AzureOpenAIResponsesProvider::AzureOpenAIResponsesProvider(
    std::unique_ptr<HttpTransport> transport, std::string base_url)
    : AzureOpenAIResponsesProvider(ProviderDescriptor{}, std::move(transport),
                                   std::move(base_url)) {}

AzureOpenAIResponsesProvider::AzureOpenAIResponsesProvider(
    ProviderDescriptor descriptor, std::unique_ptr<HttpTransport> transport,
    std::string base_url)
    : descriptor_(std::move(descriptor)),
      transport_(std::move(transport)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw std::invalid_argument(
        "Azure OpenAI Responses provider requires transport");
  }
  if (descriptor_.id.empty()) {
    descriptor_.id = "azure-openai-responses";
  }
  if (descriptor_.display_name.empty()) {
    descriptor_.display_name = "Azure OpenAI Responses";
  }
  if (descriptor_.api.empty()) {
    descriptor_.api = "azure-openai-responses";
  }
  descriptor_.transport = ProviderTransport::sse;
  descriptor_.capabilities.streaming = true;
  descriptor_.capabilities.tool_calls = true;
  descriptor_.capabilities.vision = true;
  descriptor_.capabilities.reasoning = true;
}

const ProviderDescriptor& AzureOpenAIResponsesProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse AzureOpenAIResponsesProvider::complete(
    const CompletionRequest& request, const ProviderOptions& options) {
  if (options.aborted) {
    return aborted_provider_response(request);
  }
  const auto response = send_request(request, options, false);
  auto completion = parse_openai_response(Json::parse(sanitize_utf8(response.body)));
  apply_accounting(completion, options);
  return completion;
}

void AzureOpenAIResponsesProvider::stream(const CompletionRequest& request,
                                          const ProviderOptions& options,
                                          EventSink& sink) {
  if (options.aborted) {
    push_aborted_provider_event(sink);
    return;
  }
  const auto response = send_request(request, options, true);
  auto events = parse_openai_responses_sse(response.body);
  apply_accounting(events, options);
  for (const auto& event : events) {
    sink.push(event);
  }
}

HttpResponse AzureOpenAIResponsesProvider::send_request(
    const CompletionRequest& request, const ProviderOptions& options,
    bool stream) {
  auto azure_request = request;
  azure_request.model.provider = descriptor_.id;
  azure_request.model.api = descriptor_.api;
  azure_request.model.name = azure_openai_deployment_name(request, options);

  HttpRequest http;
  http.method = "POST";
  http.url = azure_openai_responses_endpoint(options, base_url_);
  http.headers = metadata_headers(options.metadata);
  merge_overriding_headers(http.headers, options.headers);
  http.headers.emplace("accept", stream ? "text/event-stream" : "application/json");
  http.headers.emplace("content-type", "application/json");
  http.timeout_ms = options.timeout_ms;
  http.proxy_url = options.proxy_url;
  if (options.api_key) {
    http.headers.emplace("api-key", *options.api_key);
  } else if (const auto env_api_key = env_string("AZURE_OPENAI_API_KEY")) {
    http.headers.emplace("api-key", *env_api_key);
  }
  http.body = provider_safe_json_dump(
      openai_responses_request_body(azure_request, stream, options.metadata));

  return send_with_retries(*transport_, http, options);
}

std::shared_ptr<Provider> make_azure_openai_responses_provider(
    std::unique_ptr<HttpTransport> transport, std::string base_url) {
  return std::make_shared<AzureOpenAIResponsesProvider>(std::move(transport),
                                                       std::move(base_url));
}

OpenAICompletionsProvider::OpenAICompletionsProvider(
    std::unique_ptr<HttpTransport> transport, std::string endpoint)
    : OpenAICompletionsProvider(ProviderDescriptor{}, std::move(transport),
                                std::move(endpoint)) {}

OpenAICompletionsProvider::OpenAICompletionsProvider(
    ProviderDescriptor descriptor, std::unique_ptr<HttpTransport> transport,
    std::string endpoint)
    : descriptor_(std::move(descriptor)),
      transport_(std::move(transport)),
      endpoint_(std::move(endpoint)) {
  if (!transport_) {
    throw std::invalid_argument(
        "OpenAI Chat Completions provider requires transport");
  }
  if (descriptor_.id.empty()) {
    descriptor_.id = "openai-completions";
  }
  if (descriptor_.display_name.empty()) {
    descriptor_.display_name = "OpenAI-compatible Chat Completions";
  }
  if (descriptor_.api.empty()) {
    descriptor_.api = "openai-completions";
  }
  descriptor_.transport = ProviderTransport::sse;
  descriptor_.capabilities.streaming = true;
  descriptor_.capabilities.tool_calls = true;
  descriptor_.capabilities.vision = true;
  descriptor_.capabilities.reasoning = true;
}

const ProviderDescriptor& OpenAICompletionsProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse OpenAICompletionsProvider::complete(
    const CompletionRequest& request, const ProviderOptions& options) {
  if (options.aborted) {
    return aborted_provider_response(request);
  }
  const auto response = send_request(request, options, false);
  auto completion = parse_openai_completions_response(Json::parse(sanitize_utf8(response.body)));
  retain_routed_response_model(completion, request.model.name);
  apply_accounting(completion, options);
  return completion;
}

void OpenAICompletionsProvider::stream(const CompletionRequest& request,
                                       const ProviderOptions& options,
                                       EventSink& sink) {
  if (options.aborted) {
    push_aborted_provider_event(sink);
    return;
  }
  const auto response = send_request(request, options, true);
  auto events = parse_openai_completions_sse(response.body);
  retain_routed_response_model(events, request.model.name);
  apply_accounting(events, options);
  for (const auto& event : events) {
    sink.push(event);
  }
}

HttpResponse OpenAICompletionsProvider::send_request(
    const CompletionRequest& request, const ProviderOptions& options,
    bool stream) {
  HttpRequest http;
  http.method = "POST";
  http.url = provider_endpoint_from_options(options, endpoint_, descriptor_.id,
                                            "chat/completions");
  http.headers = metadata_headers(options.metadata);
  apply_openrouter_attribution_headers(http.headers, request, options, http.url);
  if (provider_key(request.model.provider) == "github-copilot") {
    merge_overriding_headers(http.headers,
                             github_copilot_dynamic_headers(request));
  }
  http.headers.emplace("accept",
                       stream ? "text/event-stream" : "application/json");
  http.headers.emplace("content-type", "application/json");
  http.timeout_ms = options.timeout_ms;
  http.proxy_url = options.proxy_url;
  if (options.api_key) {
    apply_openai_family_auth_headers(http.headers, request.model.provider,
                                     options.api_key);
  } else if (const auto env_api_key =
                 get_env_api_key(request.model.provider)) {
    apply_openai_family_auth_headers(http.headers, request.model.provider,
                                     env_api_key);
  }
  if (const auto session_id = string_metadata_value(options.metadata, "sessionId");
      session_id && cache_retention_from_options_metadata(options.metadata) != "none" &&
      compat_sends_session_affinity_headers(options.metadata)) {
    http.headers["session_id"] = *session_id;
    http.headers["x-client-request-id"] = *session_id;
    http.headers["x-session-affinity"] = *session_id;
  }
  merge_overriding_headers(http.headers, options.headers);
  http.body = provider_safe_json_dump(
      openai_completions_request_body(request, stream, options.metadata));

  return send_with_retries(*transport_, http, options);
}

std::shared_ptr<Provider> make_openai_completions_provider(
    std::unique_ptr<HttpTransport> transport, std::string endpoint) {
  return std::make_shared<OpenAICompletionsProvider>(std::move(transport),
                                                    std::move(endpoint));
}

RecordedTranscriptProvider::RecordedTranscriptProvider(
    ProviderDescriptor descriptor, std::shared_ptr<HttpTransport> transport,
    std::string endpoint)
    : descriptor_(std::move(descriptor)),
      transport_(std::move(transport)),
      endpoint_(std::move(endpoint)) {
  if (!transport_) {
    throw std::invalid_argument("recorded transcript provider requires transport");
  }
  if (descriptor_.id.empty()) {
    descriptor_.id = "recorded";
  }
  if (descriptor_.display_name.empty()) {
    descriptor_.display_name = "Recorded Transcript Provider";
  }
  if (descriptor_.api.empty()) {
    descriptor_.api = "openai-responses";
  }
  descriptor_.transport = ProviderTransport::sse;
}

const ProviderDescriptor& RecordedTranscriptProvider::descriptor() const {
  return descriptor_;
}

CompletionResponse RecordedTranscriptProvider::complete(
    const CompletionRequest& request, const ProviderOptions& options) {
  if (options.aborted) {
    return aborted_provider_response(request);
  }
  const auto response = send_request(request, options);
  if (content_type_contains(response, "application/json")) {
    auto completion = Json::parse(sanitize_utf8(response.body)).get<CompletionResponse>();
    apply_accounting(completion, options);
    return completion;
  }
  auto events = decode_stream_events(response);
  apply_accounting(events, options);
  return completion_response_from_events(events);
}

void RecordedTranscriptProvider::stream(const CompletionRequest& request,
                                        const ProviderOptions& options,
                                        EventSink& sink) {
  if (options.aborted) {
    push_aborted_provider_event(sink);
    return;
  }
  auto events = decode_stream_events(send_request(request, options));
  apply_accounting(events, options);
  for (const auto& event : events) {
    sink.push(event);
  }
}

HttpResponse RecordedTranscriptProvider::send_request(
    const CompletionRequest& request, const ProviderOptions& options) {
  HttpRequest http;
  http.method = "POST";
  http.url = endpoint_;
  http.headers = options.headers;
  http.headers.emplace("accept", "text/event-stream");
  http.headers.emplace("content-type", "application/json");
  http.timeout_ms = options.timeout_ms;
  http.proxy_url = options.proxy_url;
  if (options.api_key) {
    http.headers.emplace("authorization", "Bearer " + *options.api_key);
  }
  http.body = provider_safe_json_dump(Json(normalized(request)));

  return send_with_retries(*transport_, http, options);
}

std::vector<StreamEvent> RecordedTranscriptProvider::decode_stream_events(
    const HttpResponse& response) const {
  if (content_type_contains(response, "application/x-qt-stream-events")) {
    std::vector<StreamEvent> events;
    for (const auto& item : Json::parse(sanitize_utf8(response.body))) {
      events.push_back(item.get<StreamEvent>());
    }
    return events;
  }

  return parse_openai_responses_sse(response.body);
}

}  // namespace qt::ai
