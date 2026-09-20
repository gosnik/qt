#include "qt/app/auth/auth_flow.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <sstream>
#include <set>
#include <tuple>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "qt/app/runtime/provider_identity.hpp"

namespace qt::app {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

constexpr std::int64_t kCopilotInitialPollMultiplierPerMille = 1200;
constexpr std::int64_t kCopilotSlowDownMultiplierPerMille = 1400;

std::string provider_env_name(const std::string& provider_id) {
  std::string name;
  name.reserve(provider_id.size() + 8);
  for (const char ch : provider_key(provider_id)) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    } else {
      name.push_back('_');
    }
  }
  name += "_API_KEY";
  return name;
}

bool getenv_nonempty(const std::string& name) {
  const char* value = std::getenv(name.c_str());
  return value != nullptr && *value != '\0';
}

std::optional<std::string> json_string_field(const ai::Json& json,
                                             const std::string& key);

std::optional<std::string> getenv_value(const std::string& name) {
  const char* value = std::getenv(name.c_str());
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

bool env_truthy(const std::string& value) {
  const std::string key = lowercase_copy(trim_copy(value));
  return key == "1" || key == "true" || key == "yes" || key == "on";
}

const char* built_in_provider_display_name(const std::string& provider) {
  const std::string key = provider_key(provider);
  static const std::vector<std::pair<const char*, const char*>> kDisplayNames = {
      {"anthropic", "Anthropic"},
      {"amazon-bedrock", "Amazon Bedrock"},
      {"azure-openai-responses", "Azure OpenAI Responses"},
      {"cerebras", "Cerebras"},
      {"cloudflare-ai-gateway", "Cloudflare AI Gateway"},
      {"cloudflare-workers-ai", "Cloudflare Workers AI"},
      {"deepseek", "DeepSeek"},
      {"fireworks", "Fireworks"},
      {"google", "Google Gemini"},
      {"google-vertex", "Google Vertex AI"},
      {"groq", "Groq"},
      {"huggingface", "Hugging Face"},
      {"kimi-coding", "Kimi For Coding"},
      {"mistral", "Mistral"},
      {"minimax", "MiniMax"},
      {"minimax-cn", "MiniMax (China)"},
      {"moonshotai", "Moonshot AI"},
      {"moonshotai-cn", "Moonshot AI (China)"},
      {"opencode", "OpenCode Zen"},
      {"opencode-go", "OpenCode Go"},
      {"openai", "OpenAI"},
      {"openrouter", "OpenRouter"},
      {"together", "Together AI"},
      {"vercel-ai-gateway", "Vercel AI Gateway"},
      {"xai", "xAI"},
      {"zai", "ZAI"},
      {"xiaomi", "Xiaomi MiMo"},
      {"xiaomi-token-plan-cn", "Xiaomi MiMo Token Plan (China)"},
      {"xiaomi-token-plan-ams", "Xiaomi MiMo Token Plan (Amsterdam)"},
      {"xiaomi-token-plan-sgp", "Xiaomi MiMo Token Plan (Singapore)"},
  };

  for (const auto& entry : kDisplayNames) {
    if (key == entry.first) {
      return entry.second;
    }
  }
  return nullptr;
}

std::string provider_display_name(
    const ai::ModelCatalog& catalog,
    const std::vector<AuthOAuthProviderOption>& oauth_providers,
    const std::string& provider) {
  const std::string wanted = provider_key(provider);
  for (const auto& record : catalog.providers) {
    if (provider_key(record.descriptor.id) == wanted &&
        !trim_copy(record.descriptor.display_name).empty()) {
      return trim_copy(record.descriptor.display_name);
    }
  }
  for (const auto& record : oauth_providers) {
    if (provider_key(record.id) == wanted && !trim_copy(record.name).empty()) {
      return trim_copy(record.name);
    }
  }
  if (const char* name = built_in_provider_display_name(provider);
      name != nullptr) {
    return name;
  }
  return trim_copy(provider);
}

std::vector<AuthOAuthProviderOption> catalog_oauth_provider_options(
    const ai::ModelCatalog& catalog,
    const std::vector<AuthOAuthProviderOption>& explicit_providers = {}) {
  std::vector<AuthOAuthProviderOption> providers = explicit_providers;
  std::set<std::string> seen;
  for (const auto& provider : providers) {
    const std::string key = provider_key(provider.id);
    if (!key.empty()) {
      seen.insert(key);
    }
  }

  for (const auto& provider : catalog.providers) {
    if (!provider.metadata.is_object() || !provider.metadata.contains("oauth") ||
        !provider.metadata.at("oauth").is_object()) {
      continue;
    }
    const std::string id = trim_copy(provider.descriptor.id);
    const std::string key = provider_key(id);
    if (id.empty() || key.empty() || seen.count(key) != 0U) {
      continue;
    }

    const auto& oauth = provider.metadata.at("oauth");
    AuthOAuthProviderOption option;
    option.id = id;
    option.name = json_string_field(oauth, "name")
                      .value_or(provider_display_name(catalog, providers, id));
    providers.push_back(std::move(option));
    seen.insert(key);
  }
  return providers;
}

bool has_auth_record(const ai::AuthStorage& storage,
                     const std::string& provider_id) {
  return ai::has_provider_auth_record(storage, provider_id);
}

ai::ProviderAuthRecord credential_record(const PlannedAuthCredential& credential,
                                         const std::string& secret) {
  ai::ProviderAuthRecord record;
  record.provider = credential.provider;
  if (credential.kind == AuthCredentialKind::api_key) {
    record.api_key = secret;
  } else {
    record.token = secret;
  }
  return record;
}

std::string percent_encode(const std::string& value) {
  std::ostringstream output;
  output << std::uppercase << std::hex;
  for (const unsigned char ch : value) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      output << static_cast<char>(ch);
    } else {
      output << '%' << std::setw(2) << std::setfill('0')
             << static_cast<int>(ch);
      output << std::setfill(' ');
    }
  }
  return output.str();
}

std::string form_body(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string body;
  for (const auto& field : fields) {
    if (!body.empty()) {
      body += "&";
    }
    body += percent_encode(field.first);
    body += "=";
    body += percent_encode(field.second);
  }
  return body;
}

std::vector<std::string> split_ascii_whitespace(const std::string& value) {
  std::vector<std::string> parts;
  std::string current;
  for (const unsigned char ch : value) {
    if (std::isspace(ch) != 0) {
      if (!current.empty()) {
        parts.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(static_cast<char>(ch));
  }
  if (!current.empty()) {
    parts.push_back(std::move(current));
  }
  return parts;
}

std::int64_t poll_wait_ms(std::int64_t interval_ms,
                          std::int64_t multiplier_per_mille,
                          std::int64_t remaining_ms) {
  const std::int64_t safe_interval = std::max<std::int64_t>(1000, interval_ms);
  const std::int64_t wait =
      (safe_interval * multiplier_per_mille + 999) / 1000;
  return std::min(wait, std::max<std::int64_t>(0, remaining_ms));
}

std::optional<std::string> json_string_field(const ai::Json& json,
                                             const std::string& key) {
  const auto it = json.find(key);
  if (it == json.end() || !it->is_string()) {
    return std::nullopt;
  }
  const auto value = trim_copy(it->get<std::string>());
  return value.empty() ? std::optional<std::string>{} : value;
}

std::optional<std::int64_t> json_int_field(const ai::Json& json,
                                           const std::string& key) {
  const auto it = json.find(key);
  if (it == json.end() || !it->is_number()) {
    return std::nullopt;
  }
  return it->get<std::int64_t>();
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
    const auto json = ai::Json::parse(text);
    const auto auth = json.find("https://api.openai.com/auth");
    if (auth == json.end() || !auth->is_object()) {
      return std::nullopt;
    }
    return json_string_field(*auth, "chatgpt_account_id");
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

Result<ai::ProviderAuthRecord> oauth_record_from_metadata(
    const std::string& provider, const ai::Json& metadata) {
  const auto access = json_string_field(metadata, "access");
  if (!access) {
    return Result<ai::ProviderAuthRecord>::failure(
        ErrorCode::invalid_argument,
        "OAuth token response for provider " + provider +
            " is missing access token");
  }

  ai::ProviderAuthRecord record;
  record.provider = provider;
  record.token = *access;
  record.metadata = metadata;
  const auto errors = ai::validate(record);
  if (!errors.empty()) {
    return Result<ai::ProviderAuthRecord>::failure(ErrorCode::invalid_argument,
                                                   errors.front());
  }
  return ai::normalized(record);
}

std::vector<std::pair<std::string, std::string>> parse_query_pairs(
    std::string value) {
  const auto hash = value.find('#');
  if (hash != std::string::npos) {
    value = value.substr(0, hash);
  }
  const auto question = value.find('?');
  if (question != std::string::npos) {
    value = value.substr(question + 1U);
  }

  std::vector<std::pair<std::string, std::string>> pairs;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find('&', start);
    const auto token = value.substr(start, end - start);
    if (!token.empty()) {
      const auto equals = token.find('=');
      if (equals == std::string::npos) {
        pairs.emplace_back(token, std::string());
      } else {
        pairs.emplace_back(token.substr(0, equals), token.substr(equals + 1U));
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1U;
  }
  return pairs;
}

std::optional<std::string> query_value(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    const std::string& key) {
  for (const auto& pair : pairs) {
    if (pair.first == key) {
      return pair.second;
    }
  }
  return std::nullopt;
}

std::optional<std::string> percent_decode(const std::string& value) {
  std::string output;
  output.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const unsigned char ch = static_cast<unsigned char>(value[index]);
    if (ch == '+') {
      output.push_back(' ');
      continue;
    }
    if (ch != '%') {
      output.push_back(static_cast<char>(ch));
      continue;
    }
    if (index + 2U >= value.size()) {
      return std::nullopt;
    }
    const auto hex_value = [](unsigned char digit) -> int {
      if (digit >= '0' && digit <= '9') {
        return digit - '0';
      }
      if (digit >= 'a' && digit <= 'f') {
        return 10 + digit - 'a';
      }
      if (digit >= 'A' && digit <= 'F') {
        return 10 + digit - 'A';
      }
      return -1;
    };
    const int hi = hex_value(static_cast<unsigned char>(value[index + 1U]));
    const int lo = hex_value(static_cast<unsigned char>(value[index + 2U]));
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    output.push_back(static_cast<char>((hi << 4) | lo));
    index += 2U;
  }
  return output;
}

std::optional<std::string> decoded_query_value(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    const std::string& key) {
  const auto value = query_value(pairs, key);
  if (!value) {
    return std::nullopt;
  }
  return percent_decode(*value).value_or(*value);
}

std::optional<std::string> normalize_github_domain(const std::string& input) {
  std::string value = trim_copy(input);
  if (value.empty()) {
    return std::nullopt;
  }
  const auto scheme = value.find("://");
  if (scheme != std::string::npos) {
    value = value.substr(scheme + 3U);
  }
  const auto slash = value.find('/');
  if (slash != std::string::npos) {
    value = value.substr(0, slash);
  }
  const auto question = value.find('?');
  if (question != std::string::npos) {
    value = value.substr(0, question);
  }
  const auto hash = value.find('#');
  if (hash != std::string::npos) {
    value = value.substr(0, hash);
  }
  const auto at = value.rfind('@');
  if (at != std::string::npos) {
    value = value.substr(at + 1U);
  }
  const auto colon = value.find(':');
  if (colon != std::string::npos) {
    value = value.substr(0, colon);
  }
  value = trim_copy(value);
  if (value.empty() || value.find('.') == std::string::npos ||
      value.find(' ') != std::string::npos) {
    return std::nullopt;
  }
  return value;
}

std::string github_copilot_domain(const std::string& input) {
  if (const auto normalized = normalize_github_domain(input)) {
    return *normalized;
  }
  return "github.com";
}

std::optional<std::string> github_copilot_base_url_from_token(
    const std::string& token) {
  const std::string marker = "proxy-ep=";
  const auto start = token.find(marker);
  if (start == std::string::npos) {
    return std::nullopt;
  }
  const auto host_start = start + marker.size();
  const auto host_end = token.find(';', host_start);
  std::string host = token.substr(host_start, host_end - host_start);
  host = trim_copy(host);
  if (host.empty()) {
    return std::nullopt;
  }
  const std::string proxy_prefix = "proxy.";
  if (host.find(proxy_prefix) == 0U) {
    host.replace(0, proxy_prefix.size(), "api.");
  }
  return "https://" + host;
}

std::string github_copilot_base_url(const std::string& token,
                                    const std::string& enterprise_domain) {
  if (const auto parsed = github_copilot_base_url_from_token(token)) {
    return *parsed;
  }
  if (const auto domain = normalize_github_domain(enterprise_domain)) {
    return "https://copilot-api." + *domain;
  }
  return "https://api.individual.githubcopilot.com";
}

std::string request_target_path(const std::string& request_target) {
  std::string value = trim_copy(request_target);
  const auto scheme = value.find("://");
  if (scheme != std::string::npos) {
    const auto slash = value.find('/', scheme + 3U);
    value = slash == std::string::npos ? "/" : value.substr(slash);
  }
  const auto question = value.find('?');
  if (question != std::string::npos) {
    value = value.substr(0, question);
  }
  const auto hash = value.find('#');
  if (hash != std::string::npos) {
    value = value.substr(0, hash);
  }
  return value.empty() ? "/" : value;
}

std::string escape_html(const std::string& value) {
  std::string output;
  output.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      case '\'':
        output += "&#39;";
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
  return output;
}

std::string oauth_callback_html(bool success, const std::string& message,
                                const std::string& details = {}) {
  std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>";
  html += success ? "Authentication successful" : "Authentication failed";
  html += "</title></head><body><h1>";
  html += success ? "Authentication successful" : "Authentication failed";
  html += "</h1><p>";
  html += escape_html(message);
  html += "</p>";
  if (!details.empty()) {
    html += "<pre>";
    html += escape_html(details);
    html += "</pre>";
  }
  html += "</body></html>";
  return html;
}

std::string oauth_http_status_text(int status_code) {
  switch (status_code) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    default:
      return "Error";
  }
}

std::string oauth_http_response_text(
    int status_code, const std::string& status_text,
    const std::map<std::string, std::string>& headers,
    const std::string& body) {
  std::ostringstream response;
  response << "HTTP/1.1 " << status_code << ' ' << status_text << "\r\n";
  for (const auto& [key, value] : headers) {
    response << key << ": " << value << "\r\n";
  }
  response << "\r\n" << body;
  return response.str();
}

}  // namespace

const char* to_string(AuthCredentialKind kind) noexcept {
  switch (kind) {
    case AuthCredentialKind::api_key:
      return "api_key";
    case AuthCredentialKind::token:
      return "token";
  }
  return "unknown";
}

const char* to_string(AuthLoginPlanAction action) noexcept {
  switch (action) {
    case AuthLoginPlanAction::use_existing_auth:
      return "use_existing_auth";
    case AuthLoginPlanAction::store_new_credential:
      return "store_new_credential";
  }
  return "unknown";
}

const char* to_string(AuthProviderStatusSource source) noexcept {
  switch (source) {
    case AuthProviderStatusSource::none:
      return "none";
    case AuthProviderStatusSource::auth_storage_api_key:
      return "auth_storage_api_key";
    case AuthProviderStatusSource::auth_storage_token:
      return "auth_storage_token";
    case AuthProviderStatusSource::environment_api_key:
      return "environment_api_key";
    case AuthProviderStatusSource::models_json_key:
      return "models_json_key";
    case AuthProviderStatusSource::models_json_command:
      return "models_json_command";
  }
  return "none";
}

const char* to_string(AuthSelectorAuthType auth_type) noexcept {
  switch (auth_type) {
    case AuthSelectorAuthType::oauth:
      return "oauth";
    case AuthSelectorAuthType::api_key:
      return "api_key";
  }
  return "api_key";
}

const char* to_string(OAuthDevicePollAction action) noexcept {
  switch (action) {
    case OAuthDevicePollAction::wait:
      return "wait";
    case OAuthDevicePollAction::access_token:
      return "access_token";
    case OAuthDevicePollAction::failed:
      return "failed";
    case OAuthDevicePollAction::timed_out:
      return "timed_out";
    case OAuthDevicePollAction::cancelled:
      return "cancelled";
  }
  return "failed";
}

std::vector<AuthOAuthProviderOption> built_in_oauth_provider_options() {
  return {
      AuthOAuthProviderOption{"anthropic", "Anthropic (Claude Pro/Max)"},
      AuthOAuthProviderOption{"github-copilot", "GitHub Copilot"},
      AuthOAuthProviderOption{"openai-codex",
                              "ChatGPT Plus/Pro (Codex Subscription)"},
  };
}

bool is_api_key_login_provider(
    const std::string& provider,
    const std::vector<std::string>& oauth_provider_ids,
    const std::vector<std::string>& built_in_model_provider_ids) {
  if (trim_copy(provider).empty()) {
    return false;
  }
  if (built_in_provider_display_name(provider) != nullptr) {
    return true;
  }
  if (contains_provider_id(built_in_model_provider_ids, provider)) {
    return false;
  }
  return !contains_provider_id(oauth_provider_ids, provider);
}

std::vector<AuthSelectorProvider> build_auth_login_provider_options(
    const AuthLoginSelectorRequest& request) {
  std::vector<AuthSelectorProvider> options;
  const auto oauth_providers =
      catalog_oauth_provider_options(request.catalog, request.oauth_providers);
  std::vector<std::string> oauth_provider_ids;
  oauth_provider_ids.reserve(oauth_providers.size());

  for (const auto& provider : oauth_providers) {
    const std::string id = trim_copy(provider.id);
    if (id.empty()) {
      continue;
    }
    oauth_provider_ids.push_back(id);
    if (request.auth_type &&
        *request.auth_type != AuthSelectorAuthType::oauth) {
      continue;
    }
    AuthSelectorProvider option;
    option.id = id;
    option.name = trim_copy(provider.name).empty() ? id : trim_copy(provider.name);
    option.auth_type = AuthSelectorAuthType::oauth;
    options.push_back(option);
  }

  std::set<std::string> model_provider_keys;
  std::vector<std::string> model_providers;
  for (const auto& model : request.catalog.models) {
    const std::string id = trim_copy(model.ref.provider);
    const std::string key = provider_key(id);
    if (key.empty() || model_provider_keys.count(key) != 0U) {
      continue;
    }
    model_provider_keys.insert(key);
    model_providers.push_back(id);
  }

  for (const auto& provider_id : model_providers) {
    if (!is_api_key_login_provider(provider_id, oauth_provider_ids,
                                   request.built_in_model_provider_ids)) {
      continue;
    }
    if (request.auth_type &&
        *request.auth_type != AuthSelectorAuthType::api_key) {
      continue;
    }

    AuthSelectorProvider option;
    option.id = provider_id;
    option.name =
        provider_display_name(request.catalog, oauth_providers, provider_id);
    option.auth_type = AuthSelectorAuthType::api_key;
    options.push_back(option);
  }

  std::sort(options.begin(), options.end(),
            [](const AuthSelectorProvider& lhs,
               const AuthSelectorProvider& rhs) {
              return std::make_tuple(lowercase_copy(lhs.name), lhs.name, lhs.id,
                                     std::string(to_string(lhs.auth_type))) <
                     std::make_tuple(lowercase_copy(rhs.name), rhs.name, rhs.id,
                                     std::string(to_string(rhs.auth_type)));
            });
  return options;
}

std::vector<AuthSelectorProvider> build_auth_logout_provider_options(
    const AuthLogoutSelectorRequest& request) {
  std::vector<AuthSelectorProvider> options;
  const ai::AuthStorage storage = ai::normalized(request.auth_storage);
  const auto oauth_providers =
      catalog_oauth_provider_options(request.catalog, request.oauth_providers);

  for (const auto& record : storage.providers) {
    const std::string id = trim_copy(record.provider);
    if (id.empty() || (!record.api_key && !record.token)) {
      continue;
    }

    AuthSelectorProvider option;
    option.id = id;
    option.name = provider_display_name(request.catalog, oauth_providers, id);
    option.auth_type =
        record.api_key ? AuthSelectorAuthType::api_key : AuthSelectorAuthType::oauth;
    options.push_back(std::move(option));
  }

  std::sort(options.begin(), options.end(),
            [](const AuthSelectorProvider& lhs,
               const AuthSelectorProvider& rhs) {
              return std::make_tuple(lowercase_copy(lhs.name), lhs.name, lhs.id,
                                     std::string(to_string(lhs.auth_type))) <
                     std::make_tuple(lowercase_copy(rhs.name), rhs.name, rhs.id,
                                     std::string(to_string(rhs.auth_type)));
            });
  return options;
}

AuthProviderStatus describe_auth_provider_status(
    const ai::AuthStorage& storage, const std::string& provider,
    const std::optional<std::string>& provider_request_api_key) {
  AuthProviderStatus status;
  status.provider = trim_copy(provider);
  if (provider_key(provider).empty()) {
    status.source = AuthProviderStatusSource::none;
    status.label = "unconfigured";
    return status;
  }

  if (const auto* record = ai::find_provider_auth_record(storage, provider);
      record != nullptr) {
    status.configured = true;
    if (record->api_key) {
      status.source = AuthProviderStatusSource::auth_storage_api_key;
      status.label = "auth.json api key";
    } else {
      status.source = AuthProviderStatusSource::auth_storage_token;
      status.label = "auth.json token";
    }
    return status;
  }

  const std::string env_name = provider_env_name(provider);
  const auto env_names = ai::find_auth_env_vars(provider);
  if (!env_names.empty()) {
    status.configured = true;
    status.source = AuthProviderStatusSource::environment_api_key;
    status.label = env_names.front();
    return status;
  }

  if (ai::auth_env_var_names_for_provider(provider).empty() && !env_name.empty() &&
      getenv_nonempty(env_name)) {
    status.configured = true;
    status.source = AuthProviderStatusSource::environment_api_key;
    status.label = env_name;
    return status;
  }

  const std::string request_api_key =
      provider_request_api_key ? trim_copy(*provider_request_api_key) : "";
  if (!request_api_key.empty()) {
    if (request_api_key.front() == '!') {
      status.configured = true;
      status.source = AuthProviderStatusSource::models_json_command;
      status.label = "command in models.json";
      return status;
    }

    if (getenv_nonempty(request_api_key)) {
      status.configured = true;
      status.source = AuthProviderStatusSource::environment_api_key;
      status.label = request_api_key;
      return status;
    }

    status.configured = true;
    status.source = AuthProviderStatusSource::models_json_key;
    status.label = "key in models.json";
    return status;
  }

  status.source = AuthProviderStatusSource::none;
  status.label = "unconfigured";
  return status;
}

Result<OAuthAuthorizationPlan> plan_oauth_authorization(
    const OAuthAuthorizationPlanRequest& request) {
  const std::string provider = provider_key(request.provider);
  const std::string verifier = trim_copy(request.verifier);
  const std::string challenge = trim_copy(request.challenge);
  const std::string state = trim_copy(request.state);
  if (provider.empty()) {
    return Result<OAuthAuthorizationPlan>::failure(ErrorCode::invalid_argument,
                                                   "OAuth provider is required");
  }
  if (provider != "github-copilot" &&
      (verifier.empty() || challenge.empty())) {
    return Result<OAuthAuthorizationPlan>::failure(
        ErrorCode::invalid_argument,
        "OAuth PKCE verifier and challenge are required");
  }

  OAuthAuthorizationPlan plan;
  plan.provider = provider;
  plan.uses_callback_server = provider != "github-copilot";

  if (provider == "anthropic") {
    plan.name = "Anthropic (Claude Pro/Max)";
    plan.token_url = "https://platform.claude.com/v1/oauth/token";
    plan.redirect_uri = "http://localhost:53692/callback";
    plan.callback_host =
        getenv_value("PI_OAUTH_CALLBACK_HOST").value_or("127.0.0.1");
    plan.callback_port = 53692;
    plan.callback_path = "/callback";
    plan.manual_prompt = "Paste the authorization code or full redirect URL:";
    plan.instructions =
        "Complete login in your browser. If the browser is on another machine, paste the final redirect URL here.";
    const std::string scope =
        "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
    plan.authorize_url =
        "https://claude.ai/oauth/authorize?" +
        form_body({{"code", "true"},
                   {"client_id", "9d1c250a-e61b-44d9-88ed-594d1962f5e"},
                   {"response_type", "code"},
                   {"redirect_uri", plan.redirect_uri},
                   {"scope", scope},
                   {"code_challenge", challenge},
                   {"code_challenge_method", "S256"},
                   {"state", verifier}});
    return plan;
  }

  if (provider == "openai-codex") {
    if (state.empty()) {
      return Result<OAuthAuthorizationPlan>::failure(
          ErrorCode::invalid_argument, "OpenAI Codex OAuth state is required");
    }
    const std::string originator =
        trim_copy(request.originator).empty() ? "pi" : trim_copy(request.originator);
    plan.name = "ChatGPT Plus/Pro (Codex Subscription)";
    plan.token_url = "https://auth.openai.com/oauth/token";
    plan.redirect_uri = "http://localhost:1455/auth/callback";
    plan.callback_host =
        getenv_value("PI_OAUTH_CALLBACK_HOST").value_or("127.0.0.1");
    plan.callback_port = 1455;
    plan.callback_path = "/auth/callback";
    plan.manual_prompt = "Paste the authorization code (or full redirect URL):";
    plan.instructions = "A browser window should open. Complete login to finish.";
    plan.authorize_url =
        "https://auth.openai.com/oauth/authorize?" +
        form_body({{"response_type", "code"},
                   {"client_id", "app_EMoamEEZ73f0CkXaXp7hrann"},
                   {"redirect_uri", plan.redirect_uri},
                   {"scope", "openid profile email offline_access"},
                   {"code_challenge", challenge},
                   {"code_challenge_method", "S256"},
                   {"state", state},
                   {"id_token_add_organizations", "true"},
                   {"codex_cli_simplified_flow", "true"},
                   {"originator", originator}});
    return plan;
  }

  if (provider == "github-copilot") {
    const std::string domain =
        github_copilot_domain(request.enterprise_domain);
    plan.name = "GitHub Copilot";
    plan.uses_callback_server = false;
    plan.token_url = "https://" + domain + "/login/oauth/access_token";
    plan.authorize_url = "https://" + domain + "/login/device/code";
    plan.manual_prompt = "Open the GitHub device URL and enter the user code.";
    plan.instructions =
        "Start GitHub's device-code flow, wait before the first poll, and honor slow_down poll intervals.";
    return plan;
  }

  return Result<OAuthAuthorizationPlan>::failure(
      ErrorCode::invalid_argument, "unknown OAuth provider: " + provider);
}

Result<OAuthAuthorizationCode> parse_oauth_authorization_input(
    const std::string& input,
    const std::optional<std::string>& expected_state) {
  const std::string value = trim_copy(input);
  if (value.empty()) {
    return Result<OAuthAuthorizationCode>::failure(ErrorCode::invalid_argument,
                                                   "Missing authorization code");
  }

  OAuthAuthorizationCode parsed;
  if (value.find('#') != std::string::npos &&
      value.find("://") == std::string::npos) {
    const auto hash = value.find('#');
    parsed.code = value.substr(0, hash);
    parsed.state = value.substr(hash + 1U);
  } else if (value.find("code=") != std::string::npos) {
    const auto pairs = parse_query_pairs(value);
    if (const auto code = query_value(pairs, "code")) {
      parsed.code = *code;
    }
    parsed.state = query_value(pairs, "state");
  } else {
    parsed.code = value;
  }

  parsed.code = trim_copy(parsed.code);
  if (parsed.state) {
    parsed.state = trim_copy(*parsed.state);
    if (parsed.state->empty()) {
      parsed.state.reset();
    }
  }

  if (parsed.code.empty()) {
    return Result<OAuthAuthorizationCode>::failure(ErrorCode::invalid_argument,
                                                   "Missing authorization code");
  }
  if (expected_state && parsed.state && *parsed.state != *expected_state) {
    return Result<OAuthAuthorizationCode>::failure(ErrorCode::invalid_argument,
                                                   "OAuth state mismatch");
  }
  return parsed;
}

Result<OAuthRefreshPlan> plan_oauth_refresh(
    const ai::ProviderAuthRecord& record, std::int64_t now_ms) {
  const ai::ProviderAuthRecord normalized_record = ai::normalized(record);
  const std::string provider = provider_key(normalized_record.provider);
  if (provider.empty()) {
    return Result<OAuthRefreshPlan>::failure(ErrorCode::invalid_argument,
                                             "OAuth provider is required");
  }
  if (normalized_record.metadata.value("type", std::string()) != "oauth") {
    return Result<OAuthRefreshPlan>::failure(
        ErrorCode::invalid_argument,
        "provider " + normalized_record.provider + " is not an OAuth record");
  }
  const auto refresh = json_string_field(normalized_record.metadata, "refresh");
  if (!refresh) {
    return Result<OAuthRefreshPlan>::failure(
        ErrorCode::invalid_argument,
        "provider " + normalized_record.provider +
            " OAuth record is missing refresh token");
  }

  OAuthRefreshPlan plan;
  plan.provider = provider;
  const auto expires = json_int_field(normalized_record.metadata, "expires");
  plan.refresh_required = !expires || now_ms >= *expires;

  if (provider == "anthropic") {
    plan.token_url = "https://platform.claude.com/v1/oauth/token";
    plan.content_type = "application/json";
    plan.request_body =
        ai::Json{{"grant_type", "refresh_token"},
                 {"client_id", "9d1c250a-e61b-44d9-88ed-594d1962f5e"},
                 {"refresh_token", *refresh}}
            .dump();
    plan.failure_message_prefix =
        "Anthropic token refresh request failed. url=" + plan.token_url;
    return plan;
  }

  if (provider == "openai-codex") {
    plan.token_url = "https://auth.openai.com/oauth/token";
    plan.content_type = "application/x-www-form-urlencoded";
    plan.request_body = form_body({{"grant_type", "refresh_token"},
                                   {"refresh_token", *refresh},
                                   {"client_id", "app_EMoamEEZ73f0CkXaXp7hrann"}});
    plan.failure_message_prefix = "OpenAI Codex token refresh failed";
    return plan;
  }

  if (provider == "github-copilot") {
    const std::string domain = github_copilot_domain(
        normalized_record.metadata.value("enterpriseUrl", std::string()));
    plan.method = "GET";
    plan.token_url = "https://api." + domain + "/copilot_internal/v2/token";
    plan.headers = {{"Accept", "application/json"},
                    {"Authorization", "Bearer " + *refresh},
                    {"User-Agent", "GitHubCopilotChat/0.35.0"},
                    {"Editor-Version", "vscode/1.107.0"},
                    {"Editor-Plugin-Version", "copilot-chat/0.35.0"},
                    {"Copilot-Integration-Id", "vscode-chat"}};
    plan.failure_message_prefix = "GitHub Copilot token refresh failed";
    return plan;
  }

  return Result<OAuthRefreshPlan>::failure(
      ErrorCode::invalid_argument, "unknown OAuth provider: " + provider);
}

Result<ai::ProviderAuthRecord> parse_oauth_token_response(
    const OAuthTokenResponseRequest& request) {
  const std::string provider = provider_key(request.provider);
  if (provider.empty()) {
    return Result<ai::ProviderAuthRecord>::failure(ErrorCode::invalid_argument,
                                                   "OAuth provider is required");
  }
  if (!request.response.is_object()) {
    return Result<ai::ProviderAuthRecord>::failure(
        ErrorCode::invalid_argument,
        "OAuth token response for provider " + provider + " must be an object");
  }

  if (provider == "anthropic") {
    const auto access = json_string_field(request.response, "access_token");
    const auto refresh = json_string_field(request.response, "refresh_token");
    const auto expires_in = json_int_field(request.response, "expires_in");
    if (!access || !refresh || !expires_in) {
      return Result<ai::ProviderAuthRecord>::failure(
          ErrorCode::invalid_argument,
          "Anthropic token response missing access_token, refresh_token, or expires_in");
    }

    return oauth_record_from_metadata(
        provider, ai::Json{{"type", "oauth"},
                           {"access", *access},
                           {"refresh", *refresh},
                           {"expires", request.now_ms +
                                           (*expires_in * 1000L) -
                                           (5L * 60L * 1000L)}});
  }

  if (provider == "openai-codex") {
    const auto access = json_string_field(request.response, "access_token");
    const auto refresh = json_string_field(request.response, "refresh_token");
    const auto expires_in = json_int_field(request.response, "expires_in");
    if (!access || !refresh || !expires_in) {
      return Result<ai::ProviderAuthRecord>::failure(
          ErrorCode::invalid_argument,
          "OpenAI Codex token exchange response missing fields: " +
              request.response.dump());
    }
    const auto account_id = openai_codex_account_id(*access);
    if (!account_id) {
      return Result<ai::ProviderAuthRecord>::failure(
          ErrorCode::invalid_argument,
          "Failed to extract accountId from token");
    }

    return oauth_record_from_metadata(
        provider, ai::Json{{"type", "oauth"},
                           {"access", *access},
                           {"refresh", *refresh},
                           {"expires", request.now_ms + (*expires_in * 1000)},
                           {"accountId", *account_id}});
  }

  if (provider == "github-copilot") {
    const auto token = json_string_field(request.response, "token");
    const auto expires_at = json_int_field(request.response, "expires_at");
    const auto refresh = json_string_field(request.response, "refresh_token")
                             .value_or(trim_copy(request.refresh_token));
    if (!token || refresh.empty() || !expires_at) {
      return Result<ai::ProviderAuthRecord>::failure(
          ErrorCode::invalid_argument,
          "Invalid Copilot token response fields");
    }

    ai::Json metadata = ai::Json{{"type", "oauth"},
                                 {"access", *token},
                                 {"refresh", refresh},
                                 {"expires", (*expires_at * 1000L) -
                                                 (5L * 60L * 1000L)}};
    if (!trim_copy(request.enterprise_domain).empty()) {
      metadata["enterpriseUrl"] = trim_copy(request.enterprise_domain);
    }
    return oauth_record_from_metadata(provider, metadata);
  }

  return Result<ai::ProviderAuthRecord>::failure(
      ErrorCode::invalid_argument, "unknown OAuth provider: " + provider);
}

Result<OAuthDeviceCodePlan> plan_oauth_device_code_polling(
    const OAuthDeviceCodePlanRequest& request) {
  const std::string provider = provider_key(request.provider);
  if (provider != "github-copilot") {
    return Result<OAuthDeviceCodePlan>::failure(
        ErrorCode::invalid_argument,
        provider.empty() ? "OAuth provider is required"
                         : "unknown OAuth device-code provider: " + provider);
  }
  if (!request.response.is_object()) {
    return Result<OAuthDeviceCodePlan>::failure(
        ErrorCode::invalid_argument, "Invalid device code response");
  }

  const auto device_code = json_string_field(request.response, "device_code");
  const auto user_code = json_string_field(request.response, "user_code");
  const auto verification_uri =
      json_string_field(request.response, "verification_uri");
  const auto interval = json_int_field(request.response, "interval");
  const auto expires_in = json_int_field(request.response, "expires_in");
  if (!device_code || !user_code || !verification_uri || !interval ||
      !expires_in) {
    return Result<OAuthDeviceCodePlan>::failure(
        ErrorCode::invalid_argument, "Invalid device code response fields");
  }

  const std::string domain = github_copilot_domain(request.enterprise_domain);
  OAuthDeviceCodePlan plan;
  plan.provider = provider;
  plan.device_code = *device_code;
  plan.user_code = *user_code;
  plan.verification_uri = *verification_uri;
  plan.instructions = "Enter code: " + plan.user_code;
  plan.access_token_url = "https://" + domain + "/login/oauth/access_token";
  plan.headers = {{"Accept", "application/json"},
                  {"Content-Type", "application/x-www-form-urlencoded"},
                  {"User-Agent", "GitHubCopilotChat/0.35.0"}};
  plan.request_body = form_body(
      {{"client_id", "Iv1.b507a08c87ecfe98"},
       {"device_code", plan.device_code},
       {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}});
  plan.poll_interval_ms = std::max<std::int64_t>(1000, *interval * 1000);
  plan.interval_multiplier_per_mille =
      kCopilotInitialPollMultiplierPerMille;
  plan.deadline_ms = request.now_ms + (*expires_in * 1000);
  plan.next_poll_at_ms =
      request.now_ms +
      poll_wait_ms(plan.poll_interval_ms, plan.interval_multiplier_per_mille,
                   plan.deadline_ms - request.now_ms);
  return plan;
}

Result<OAuthDevicePollDecision> plan_oauth_device_poll_response(
    const OAuthDevicePollDecisionRequest& request) {
  const std::string provider = provider_key(request.provider);
  if (provider != "github-copilot") {
    return Result<OAuthDevicePollDecision>::failure(
        ErrorCode::invalid_argument,
        provider.empty() ? "OAuth provider is required"
                         : "unknown OAuth device-code provider: " + provider);
  }

  OAuthDevicePollDecision decision;
  decision.provider = provider;
  decision.poll_interval_ms =
      std::max<std::int64_t>(1000, request.poll_interval_ms);
  decision.interval_multiplier_per_mille =
      request.interval_multiplier_per_mille <= 0
          ? kCopilotInitialPollMultiplierPerMille
          : request.interval_multiplier_per_mille;
  decision.slow_down_responses = request.slow_down_responses;

  if (request.cancelled) {
    decision.action = OAuthDevicePollAction::cancelled;
    decision.error_message = "Login cancelled";
    return decision;
  }

  if (request.now_ms >= request.deadline_ms) {
    decision.action = OAuthDevicePollAction::timed_out;
    decision.error_message =
        decision.slow_down_responses > 0
            ? "Device flow timed out after one or more slow_down responses. This is often caused by clock drift in WSL or VM environments. Please sync or restart the VM clock and try again."
            : "Device flow timed out";
    return decision;
  }

  if (!request.response.is_object()) {
    return Result<OAuthDevicePollDecision>::failure(
        ErrorCode::invalid_argument, "Invalid device token response");
  }

  if (const auto access = json_string_field(request.response, "access_token")) {
    decision.action = OAuthDevicePollAction::access_token;
    decision.access_token = *access;
    return decision;
  }

  const auto error = json_string_field(request.response, "error");
  if (!error) {
    return Result<OAuthDevicePollDecision>::failure(
        ErrorCode::invalid_argument, "Invalid device token response fields");
  }

  if (*error == "authorization_pending") {
    decision.action = OAuthDevicePollAction::wait;
  } else if (*error == "slow_down") {
    decision.action = OAuthDevicePollAction::wait;
    ++decision.slow_down_responses;
    if (const auto interval = json_int_field(request.response, "interval");
        interval && *interval > 0) {
      decision.poll_interval_ms = *interval * 1000;
    } else {
      decision.poll_interval_ms =
          std::max<std::int64_t>(1000, decision.poll_interval_ms + 5000);
    }
    decision.interval_multiplier_per_mille =
        kCopilotSlowDownMultiplierPerMille;
  } else {
    decision.action = OAuthDevicePollAction::failed;
    decision.error_message = "Device flow failed: " + *error;
    if (const auto description =
            json_string_field(request.response, "error_description")) {
      decision.error_message += ": " + *description;
    }
    return decision;
  }

  const std::int64_t wait =
      poll_wait_ms(decision.poll_interval_ms,
                   decision.interval_multiplier_per_mille,
                   request.deadline_ms - request.now_ms);
  decision.next_poll_at_ms = request.now_ms + wait;
  return decision;
}

Result<OAuthCopilotModelEnablementPlan>
plan_github_copilot_model_enablement(
    const OAuthCopilotModelEnablementPlanRequest& request) {
  const std::string access_token = trim_copy(request.access_token);
  if (access_token.empty()) {
    return Result<OAuthCopilotModelEnablementPlan>::failure(
        ErrorCode::invalid_argument,
        "GitHub Copilot access token is required");
  }

  OAuthCopilotModelEnablementPlan plan;
  plan.base_url =
      github_copilot_base_url(access_token, request.enterprise_domain);
  const ai::ModelCatalog catalog = ai::normalized(request.catalog);

  for (const auto& model : catalog.models) {
    if (provider_key(model.ref.provider) != "github-copilot") {
      continue;
    }

    OAuthCopilotModelEnablementRequest enable;
    enable.model_id = model.ref.name;
    enable.url = plan.base_url + "/models/" + model.ref.name + "/policy";
    enable.headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + access_token},
        {"User-Agent", "GitHubCopilotChat/0.35.0"},
        {"Editor-Version", "vscode/1.107.0"},
        {"Editor-Plugin-Version", "copilot-chat/0.35.0"},
        {"Copilot-Integration-Id", "vscode-chat"},
        {"openai-intent", "chat-policy"},
        {"x-interaction-type", "chat-policy"}};
    enable.request_body = ai::Json{{"state", "enabled"}}.dump();
    plan.requests.push_back(std::move(enable));
  }

  return plan;
}

Result<OAuthCallbackResponsePlan> plan_oauth_callback_response(
    const OAuthCallbackResponsePlanRequest& request) {
  const std::string provider = provider_key(request.provider);
  if (provider.empty()) {
    return Result<OAuthCallbackResponsePlan>::failure(
        ErrorCode::invalid_argument, "OAuth provider is required");
  }
  if (provider != "anthropic" && provider != "openai-codex") {
    return Result<OAuthCallbackResponsePlan>::failure(
        ErrorCode::invalid_argument,
        "unknown OAuth callback provider: " + provider);
  }

  OAuthCallbackResponsePlan plan;
  plan.provider = provider;
  const std::string expected_path =
      provider == "anthropic" ? "/callback" : "/auth/callback";
  if (request_target_path(request.request_target) != expected_path) {
    plan.status_code = 404;
    plan.message = "Callback route not found.";
    plan.response_body = oauth_callback_html(false, plan.message);
    return plan;
  }

  const auto pairs = parse_query_pairs(request.request_target);
  const auto code = decoded_query_value(pairs, "code");
  const auto state = decoded_query_value(pairs, "state");
  const auto error = decoded_query_value(pairs, "error");

  if (provider == "anthropic") {
    if (error && !trim_copy(*error).empty()) {
      plan.status_code = 400;
      plan.message = "Anthropic authentication did not complete.";
      plan.details = "Error: " + trim_copy(*error);
      plan.response_body = oauth_callback_html(false, plan.message, plan.details);
      return plan;
    }
    if (!code || trim_copy(*code).empty() || !state ||
        trim_copy(*state).empty()) {
      plan.status_code = 400;
      plan.message = "Missing code or state parameter.";
      plan.response_body = oauth_callback_html(false, plan.message);
      return plan;
    }
    if (trim_copy(*state) != trim_copy(request.expected_state)) {
      plan.status_code = 400;
      plan.message = "State mismatch.";
      plan.response_body = oauth_callback_html(false, plan.message);
      return plan;
    }

    plan.completed = true;
    plan.message =
        "Anthropic authentication completed. You can close this window.";
    plan.authorization = OAuthAuthorizationCode{trim_copy(*code), trim_copy(*state)};
    plan.response_body = oauth_callback_html(true, plan.message);
    return plan;
  }

  if (!state || trim_copy(*state) != trim_copy(request.expected_state)) {
    plan.status_code = 400;
    plan.message = "State mismatch.";
    plan.response_body = oauth_callback_html(false, plan.message);
    return plan;
  }
  if (!code || trim_copy(*code).empty()) {
    plan.status_code = 400;
    plan.message = "Missing authorization code.";
    plan.response_body = oauth_callback_html(false, plan.message);
    return plan;
  }

  plan.completed = true;
  plan.message = "OpenAI authentication completed. You can close this window.";
  plan.authorization = OAuthAuthorizationCode{trim_copy(*code), trim_copy(*state)};
  plan.response_body = oauth_callback_html(true, plan.message);
  return plan;
}

Result<OAuthCallbackHttpResponse> plan_oauth_callback_http_response(
    const OAuthCallbackHttpRequest& request) {
  const auto parts = split_ascii_whitespace(request.request_line);
  if (parts.size() < 3U || parts[2].rfind("HTTP/", 0) != 0) {
    return Result<OAuthCallbackHttpResponse>::failure(
        ErrorCode::invalid_argument, "Malformed OAuth callback HTTP request");
  }

  OAuthCallbackResponsePlanRequest callback_request;
  callback_request.provider = request.provider;
  callback_request.expected_state = request.expected_state;
  callback_request.request_target = parts[1];

  auto callback = plan_oauth_callback_response(callback_request);
  if (!callback) {
    return Result<OAuthCallbackHttpResponse>::failure(
        callback.error().code, callback.error().message,
        callback.error().system_error);
  }

  if (parts[0] != "GET") {
    auto rejected = callback.value();
    rejected.status_code = 405;
    rejected.completed = false;
    rejected.authorization.reset();
    rejected.message = "Method not allowed.";
    rejected.response_body = oauth_callback_html(false, rejected.message);
    callback = rejected;
  }

  OAuthCallbackHttpResponse response;
  response.callback = callback.value();
  response.status_text = oauth_http_status_text(response.callback.status_code);
  response.headers = {
      {"Connection", "close"},
      {"Content-Length", std::to_string(response.callback.response_body.size())},
      {"Content-Type", response.callback.content_type},
  };
  response.raw_response =
      oauth_http_response_text(response.callback.status_code,
                               response.status_text, response.headers,
                               response.callback.response_body);
  return response;
}

Result<OAuthAuthorizationResolution> resolve_oauth_authorization_result(
    const OAuthAuthorizationResolutionRequest& request) {
  const std::string provider = provider_key(request.provider);
  if (provider.empty()) {
    return Result<OAuthAuthorizationResolution>::failure(
        ErrorCode::invalid_argument, "OAuth provider is required");
  }
  if (provider != "anthropic" && provider != "openai-codex") {
    return Result<OAuthAuthorizationResolution>::failure(
        ErrorCode::invalid_argument,
        "unknown OAuth callback provider: " + provider);
  }

  OAuthAuthorizationResolution resolution;
  resolution.provider = provider;
  if (request.callback && request.callback->completed &&
      request.callback->authorization) {
    resolution.source = "callback";
    resolution.authorization = *request.callback->authorization;
    return resolution;
  }

  if (trim_copy(request.manual_input).empty()) {
    return Result<OAuthAuthorizationResolution>::failure(
        ErrorCode::invalid_argument, "Missing authorization code");
  }

  const auto parsed =
      parse_oauth_authorization_input(request.manual_input,
                                      request.expected_state);
  if (!parsed) {
    std::string message = parsed.error().message;
    if (provider == "openai-codex" && message == "OAuth state mismatch") {
      message = "State mismatch";
    }
    return Result<OAuthAuthorizationResolution>::failure(
        parsed.error().code, message, parsed.error().system_error);
  }

  resolution.source = "manual";
  resolution.authorization = parsed.value();
  if (!resolution.authorization.state && !trim_copy(request.expected_state).empty()) {
    resolution.authorization.state = trim_copy(request.expected_state);
  }
  return resolution;
}

OAuthLiveExecutionGate plan_oauth_live_execution_gate(
    const OAuthLiveExecutionGateRequest& request) {
  OAuthLiveExecutionGate gate;
  gate.provider = trim_copy(request.provider);
  gate.enable_env_var = trim_copy(request.enable_env_var).empty()
                            ? "QT_ENABLE_LIVE_OAUTH_FLOWS"
                            : trim_copy(request.enable_env_var);

  const auto value = getenv_value(gate.enable_env_var);
  gate.enabled = value && env_truthy(*value);
  if (gate.enabled) {
    gate.reason = "live OAuth execution enabled by " + gate.enable_env_var;
  } else {
    gate.reason = "live OAuth execution disabled; set " + gate.enable_env_var +
                  "=1 to enable";
  }
  if (!gate.provider.empty()) {
    gate.reason += " for provider " + gate.provider;
  }
  return gate;
}

Result<AuthLoginPlan> plan_auth_login(const AuthLoginPlanRequest& request) {
  const auto resolved = resolve_model_selection(request.model);
  if (!resolved) {
    return resolved.error();
  }

  AuthLoginPlan plan;
  plan.selection = resolved.value();

  if (plan.selection.auth) {
    plan.action = AuthLoginPlanAction::use_existing_auth;
    plan.steps.push_back("Use existing auth for provider " +
                         plan.selection.provider.descriptor.id + ".");
    return plan;
  }

  PlannedAuthCredential credential;
  credential.provider = plan.selection.provider.descriptor.id;
  credential.kind = request.credential_kind;
  credential.auth_storage_path = plan.selection.auth_storage_path;
  credential.replaces_existing_auth =
      has_auth_record(request.model.auth_storage, credential.provider);

  plan.action = AuthLoginPlanAction::store_new_credential;
  plan.credential = credential;
  plan.steps.push_back("Collect " + std::string(to_string(credential.kind)) +
                       " for provider " + credential.provider + ".");
  plan.steps.push_back("Store credential in " + credential.auth_storage_path + ".");
  plan.steps.push_back("Retry model " + plan.selection.model.ref.provider + "/" +
                       plan.selection.model.ref.name + ".");
  return plan;
}

Result<ai::AuthStorage> apply_auth_credential(
    const ai::AuthStorage& storage, const PlannedAuthCredential& credential,
    const std::string& secret) {
  const std::string trimmed_secret = trim_copy(secret);
  if (trimmed_secret.empty()) {
    return Result<ai::AuthStorage>::failure(ErrorCode::invalid_argument,
                                            "auth credential secret must not be empty");
  }
  if (trim_copy(credential.provider).empty()) {
    return Result<ai::AuthStorage>::failure(ErrorCode::invalid_argument,
                                            "auth credential provider is required");
  }

  ai::AuthStorage overlay;
  overlay.providers.push_back(credential_record(credential, trimmed_secret));

  ai::AuthStorage merged = ai::merge_auth_storage(storage, overlay);
  const auto errors = ai::validate(merged);
  if (!errors.empty()) {
    return Result<ai::AuthStorage>::failure(ErrorCode::invalid_argument,
                                            errors.front());
  }
  return merged;
}

}  // namespace qt::app
