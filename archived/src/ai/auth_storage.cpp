#include "qt/ai/auth_storage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <tuple>
#include <utility>

#if defined(__unix__)
extern char** environ;
#endif

namespace qt::ai {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using qt::foundation::Status;

constexpr int kAuthStorageVersion = 1;

std::string trim_copy(const std::string& value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
      }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

std::string lowercase_copy(const std::string& value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

std::string provider_key(const std::string& provider) {
  return lowercase_copy(trim_copy(provider));
}

Json normalized_object(const Json& value) {
  return value.is_object() ? value : Json::object();
}

std::optional<std::string> getenv_nonempty(const std::string& name) {
  const char* value = std::getenv(name.c_str());
  if (value != nullptr && *value != '\0') {
    return std::string(value);
  }

#if defined(__unix__)
  if (environ != nullptr && environ[0] != nullptr) {
    return std::nullopt;
  }
#else
  return std::nullopt;
#endif

  std::ifstream env_file("/proc/self/environ", std::ios::binary);
  if (!env_file) {
    return std::nullopt;
  }
  const std::string prefix = name + "=";
  std::string entry;
  while (std::getline(env_file, entry, '\0')) {
    if (entry.rfind(prefix, 0) != 0) {
      continue;
    }
    std::string fallback = entry.substr(prefix.size());
    return fallback.empty() ? std::optional<std::string>{}
                            : std::optional<std::string>{std::move(fallback)};
  }
  return std::nullopt;
}

bool is_google_vertex_adc_marker(const std::string& value) {
  const std::string trimmed = trim_copy(value);
  return trimmed == "gcp-vertex-credentials" ||
         (trimmed.size() > 2 && trimmed.front() == '<' &&
          trimmed.back() == '>');
}

bool file_exists(const std::string& path) {
  std::error_code error;
  return !path.empty() && std::filesystem::exists(path, error);
}

bool has_google_vertex_adc_credentials() {
  const bool has_project = getenv_nonempty("GOOGLE_CLOUD_PROJECT") ||
                           getenv_nonempty("GCLOUD_PROJECT");
  if (!has_project || !getenv_nonempty("GOOGLE_CLOUD_LOCATION")) {
    return false;
  }

  if (const auto gac_path = getenv_nonempty("GOOGLE_APPLICATION_CREDENTIALS")) {
    return file_exists(*gac_path);
  }

  if (const auto home = getenv_nonempty("HOME")) {
    const std::filesystem::path default_path =
        std::filesystem::path(*home) / ".config" / "gcloud" /
        "application_default_credentials.json";
    return file_exists(default_path.string());
  }

  return false;
}

bool has_amazon_bedrock_ambient_credentials() {
  if (getenv_nonempty("AWS_PROFILE")) {
    return true;
  }
  if (getenv_nonempty("AWS_ACCESS_KEY_ID") &&
      getenv_nonempty("AWS_SECRET_ACCESS_KEY")) {
    return true;
  }
  return getenv_nonempty("AWS_BEARER_TOKEN_BEDROCK") ||
         getenv_nonempty("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI") ||
         getenv_nonempty("AWS_CONTAINER_CREDENTIALS_FULL_URI") ||
         getenv_nonempty("AWS_WEB_IDENTITY_TOKEN_FILE");
}

std::string join_names(const std::vector<std::string>& names) {
  std::string result;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index > 0U) {
      result += index + 1U == names.size() ? " or " : ", ";
    }
    result += names[index];
  }
  return result;
}

Result<std::optional<std::string>> parse_optional_secret(const Json& json,
                                                         const std::string& field_name) {
  if (json.is_null()) {
    return std::optional<std::string>{};
  }
  if (!json.is_string()) {
    return Result<std::optional<std::string>>::failure(
        ErrorCode::invalid_argument, field_name + " must be a string or null");
  }

  const std::string value = trim_copy(json.get<std::string>());
  if (value.empty()) {
    return Result<std::optional<std::string>>::failure(
        ErrorCode::invalid_argument, field_name + " must not be empty");
  }
  return std::optional<std::string>{value};
}

Result<ProviderAuthRecord> parse_provider_auth_record(const std::string& provider_name,
                                                      const Json& json) {
  if (!json.is_object()) {
    return Result<ProviderAuthRecord>::failure(
        ErrorCode::invalid_argument, "providers." + provider_name + " must be an object");
  }

  ProviderAuthRecord record;
  record.provider = provider_name;

  if (const auto it = json.find("apiKey"); it != json.end()) {
    const auto value = parse_optional_secret(*it, "providers." + provider_name + ".apiKey");
    if (!value) {
      return value.error();
    }
    record.api_key = value.value();
  }

  if (const auto it = json.find("token"); it != json.end()) {
    const auto value = parse_optional_secret(*it, "providers." + provider_name + ".token");
    if (!value) {
      return value.error();
    }
    record.token = value.value();
  }

  if (const auto it = json.find("metadata"); it != json.end()) {
    if (!it->is_object()) {
      return Result<ProviderAuthRecord>::failure(
          ErrorCode::invalid_argument,
          "providers." + provider_name + ".metadata must be an object");
    }
    record.metadata = *it;
  }

  const auto errors = validate(record);
  if (!errors.empty()) {
    return Result<ProviderAuthRecord>::failure(ErrorCode::invalid_argument, errors.front());
  }

  return normalized(record);
}

Result<ProviderAuthRecord> parse_legacy_provider_auth_record(
    const std::string& provider_name, const Json& json) {
  if (!json.is_object()) {
    return Result<ProviderAuthRecord>::failure(
        ErrorCode::invalid_argument, "providers." + provider_name + " must be an object");
  }

  const std::string type = json.value("type", std::string());
  ProviderAuthRecord record;
  record.provider = provider_name;

  if (type == "api_key") {
    const auto it = json.find("key");
    if (it == json.end()) {
      return Result<ProviderAuthRecord>::failure(
          ErrorCode::invalid_argument, "providers." + provider_name + ".key is required");
    }
    const auto value = parse_optional_secret(*it, "providers." + provider_name + ".key");
    if (!value) {
      return value.error();
    }
    record.api_key = value.value();
  } else if (type == "oauth") {
    const auto it = json.find("access");
    if (it == json.end()) {
      return Result<ProviderAuthRecord>::failure(
          ErrorCode::invalid_argument, "providers." + provider_name + ".access is required");
    }
    const auto value =
        parse_optional_secret(*it, "providers." + provider_name + ".access");
    if (!value) {
      return value.error();
    }
    record.token = value.value();
    record.metadata = json;
  } else {
    return Result<ProviderAuthRecord>::failure(
        ErrorCode::invalid_argument,
        "providers." + provider_name + ".type must be api_key or oauth");
  }

  const auto errors = validate(record);
  if (!errors.empty()) {
    return Result<ProviderAuthRecord>::failure(ErrorCode::invalid_argument, errors.front());
  }
  return normalized(record);
}

bool looks_like_legacy_auth_record(const Json& json) {
  return json.is_object() && json.find("type") != json.end();
}

template <typename T>
void append_messages(std::vector<std::string>& into, const T& more) {
  into.insert(into.end(), more.begin(), more.end());
}

}  // namespace

ProviderAuthRecord normalized(const ProviderAuthRecord& record) {
  ProviderAuthRecord result = record;
  result.provider = trim_copy(record.provider);

  if (record.api_key) {
    const auto value = trim_copy(*record.api_key);
    result.api_key = value.empty() ? std::optional<std::string>{} : value;
  }

  if (record.token) {
    const auto value = trim_copy(*record.token);
    result.token = value.empty() ? std::optional<std::string>{} : value;
  }

  result.metadata = normalized_object(record.metadata);
  return result;
}

AuthStorage normalized(const AuthStorage& storage) {
  AuthStorage result;
  result.providers.reserve(storage.providers.size());
  for (const auto& record : storage.providers) {
    result.providers.push_back(normalized(record));
  }

  std::sort(result.providers.begin(), result.providers.end(),
            [](const ProviderAuthRecord& lhs, const ProviderAuthRecord& rhs) {
              return std::make_tuple(provider_key(lhs.provider), lhs.provider) <
                     std::make_tuple(provider_key(rhs.provider), rhs.provider);
            });
  return result;
}

std::vector<std::string> validate(const ProviderAuthRecord& record) {
  const auto normalized_record = normalized(record);
  std::vector<std::string> errors;

  if (normalized_record.provider.empty()) {
    errors.push_back("provider is required");
  }
  if (!normalized_record.api_key && !normalized_record.token) {
    errors.push_back("provider " + normalized_record.provider +
                     " must define api_key or token");
  }
  return errors;
}

std::vector<std::string> validate(const AuthStorage& storage) {
  const auto normalized_storage = normalized(storage);
  std::vector<std::string> errors;
  std::vector<std::string> provider_ids;
  provider_ids.reserve(normalized_storage.providers.size());

  for (const auto& record : normalized_storage.providers) {
    append_messages(errors, validate(record));
    provider_ids.push_back(provider_key(record.provider));
  }

  for (std::size_t index = 1; index < provider_ids.size(); ++index) {
    if (provider_ids[index] == provider_ids[index - 1]) {
      errors.push_back("duplicate provider auth entry: " +
                       normalized_storage.providers[index].provider);
    }
  }

  return errors;
}

bool is_valid(const ProviderAuthRecord& record) {
  return validate(record).empty();
}

bool is_valid(const AuthStorage& storage) { return validate(storage).empty(); }

const ProviderAuthRecord* find_provider_auth_record(
    const AuthStorage& storage, const std::string& provider) {
  const std::string wanted = provider_key(provider);
  if (wanted.empty()) {
    return nullptr;
  }

  for (const auto& record : storage.providers) {
    if (provider_key(record.provider) == wanted) {
      return &record;
    }
  }
  return nullptr;
}

std::optional<std::string> get_stored_auth_secret(const AuthStorage& storage,
                                                  const std::string& provider) {
  const auto* record = find_provider_auth_record(storage, provider);
  if (record == nullptr) {
    return std::nullopt;
  }
  if (record->api_key) {
    return record->api_key;
  }
  return record->token;
}

const char* to_string(ProviderCredentialSource source) noexcept {
  switch (source) {
    case ProviderCredentialSource::none:
      return "none";
    case ProviderCredentialSource::runtime_api_key:
      return "runtime_api_key";
    case ProviderCredentialSource::auth_storage_api_key:
      return "auth_storage_api_key";
    case ProviderCredentialSource::auth_storage_token:
      return "auth_storage_token";
    case ProviderCredentialSource::environment_api_key:
      return "environment_api_key";
  }
  return "none";
}

ProviderCredentialResolution resolve_provider_credential(
    const ProviderCredentialResolutionRequest& request) {
  ProviderCredentialResolution resolution;
  resolution.provider = trim_copy(request.provider);

  if (request.runtime_api_key) {
    const std::string runtime_key = trim_copy(*request.runtime_api_key);
    if (!runtime_key.empty()) {
      resolution.source = ProviderCredentialSource::runtime_api_key;
      resolution.secret = runtime_key;
      resolution.label = "CLI --api-key";
      return resolution;
    }
  }

  const auto apply_record = [&](const ProviderAuthRecord& record)
      -> std::optional<ProviderCredentialResolution> {
    const ProviderAuthRecord normalized_record = normalized(record);
    if (!provider_key(resolution.provider).empty() &&
        !provider_key(normalized_record.provider).empty() &&
        provider_key(normalized_record.provider) !=
            provider_key(resolution.provider)) {
      return std::nullopt;
    }
    ProviderCredentialResolution from_record;
    from_record.provider = resolution.provider.empty()
                               ? normalized_record.provider
                               : resolution.provider;
    if (normalized_record.api_key) {
      from_record.source = ProviderCredentialSource::auth_storage_api_key;
      from_record.secret = normalized_record.api_key;
      from_record.label = "auth.json api key";
      return from_record;
    }
    if (normalized_record.token) {
      from_record.source = ProviderCredentialSource::auth_storage_token;
      from_record.secret = normalized_record.token;
      from_record.label = "auth.json token";
      return from_record;
    }
    return std::nullopt;
  };

  if (request.auth_record) {
    if (auto record_resolution = apply_record(*request.auth_record)) {
      return *record_resolution;
    }
  }
  if (const auto* stored =
          find_provider_auth_record(request.auth_storage, request.provider);
      stored != nullptr) {
    if (auto stored_resolution = apply_record(*stored)) {
      return *stored_resolution;
    }
  }

  if (const auto env_secret = get_env_api_key(request.provider)) {
    resolution.source = ProviderCredentialSource::environment_api_key;
    resolution.secret = env_secret;
    const auto env_names = find_auth_env_vars(request.provider);
    resolution.label =
        env_names.empty() ? "ambient environment credentials" : env_names.front();
    return resolution;
  }

  return resolution;
}

std::string missing_provider_credential_message(const std::string& provider) {
  const std::string key = provider_key(provider);
  const std::string provider_display =
      key.empty() ? std::string("the selected provider") : trim_copy(provider);

  std::string hint;
  if (key == "google-vertex") {
    hint =
        "set GOOGLE_CLOUD_API_KEY or configure Google Application Default "
        "Credentials with GOOGLE_CLOUD_PROJECT and GOOGLE_CLOUD_LOCATION";
  } else if (key == "amazon-bedrock") {
    hint =
        "set AWS_PROFILE, AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY, or "
        "AWS_BEARER_TOKEN_BEDROCK";
  } else {
    const auto names = auth_env_var_names_for_provider(provider);
    if (!names.empty()) {
      hint = "set " + join_names(names);
    }
  }

  std::string message = "No API key found for " + provider_display + ".";
  if (!hint.empty()) {
    message += " " + hint + ", or run /login";
  } else {
    message += " Run /login";
  }
  if (!key.empty()) {
    message += " " + trim_copy(provider);
  }
  message += ".";
  return message;
}

std::vector<std::string> list_auth_provider_ids(const AuthStorage& storage) {
  const AuthStorage normalized_storage = normalized(storage);
  std::vector<std::string> providers;
  providers.reserve(normalized_storage.providers.size());
  for (const auto& record : normalized_storage.providers) {
    providers.push_back(record.provider);
  }
  return providers;
}

bool has_provider_auth_record(const AuthStorage& storage,
                              const std::string& provider) {
  return find_provider_auth_record(storage, provider) != nullptr;
}

AuthStorage upsert_provider_auth_record(const AuthStorage& storage,
                                        const ProviderAuthRecord& record) {
  AuthStorage overlay;
  overlay.providers.push_back(record);
  return merge_auth_storage(storage, overlay);
}

AuthStorage remove_provider_auth_record(const AuthStorage& storage,
                                        const std::string& provider) {
  const AuthStorage normalized_storage = normalized(storage);
  const std::string wanted = provider_key(provider);
  if (wanted.empty()) {
    return normalized_storage;
  }

  AuthStorage result;
  result.providers.reserve(normalized_storage.providers.size());
  for (const auto& record : normalized_storage.providers) {
    if (provider_key(record.provider) != wanted) {
      result.providers.push_back(record);
    }
  }
  return normalized(result);
}

AuthStorage merge_auth_storage(const AuthStorage& base, const AuthStorage& overlay) {
  AuthStorage merged = normalized(base);
  const AuthStorage normalized_overlay = normalized(overlay);

  for (const auto& record : normalized_overlay.providers) {
    const std::string key = provider_key(record.provider);
    const auto it = std::lower_bound(
        merged.providers.begin(), merged.providers.end(), key,
        [](const ProviderAuthRecord& candidate, const std::string& wanted) {
          return provider_key(candidate.provider) < wanted;
        });

    if (it != merged.providers.end() && provider_key(it->provider) == key) {
      if (record.api_key) {
        it->api_key = record.api_key;
      }
      if (record.token) {
        it->token = record.token;
      }
      if (!record.metadata.empty()) {
        it->metadata = record.metadata;
      }
      it->provider = record.provider;
    } else {
      merged.providers.insert(it, record);
    }
  }

  return normalized(merged);
}

std::vector<std::string> auth_env_var_names_for_provider(
    const std::string& provider) {
  const std::string key = provider_key(provider);
  if (key == "github-copilot") {
    return {"COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"};
  }
  if (key == "anthropic") {
    return {"ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"};
  }

  static const std::vector<std::pair<const char*, const char*>> kEnvVars = {
      {"openai", "OPENAI_API_KEY"},
      {"azure-openai-responses", "AZURE_OPENAI_API_KEY"},
      {"deepseek", "DEEPSEEK_API_KEY"},
      {"google", "GEMINI_API_KEY"},
      {"google-vertex", "GOOGLE_CLOUD_API_KEY"},
      {"groq", "GROQ_API_KEY"},
      {"cerebras", "CEREBRAS_API_KEY"},
      {"xai", "XAI_API_KEY"},
      {"openrouter", "OPENROUTER_API_KEY"},
      {"vercel-ai-gateway", "AI_GATEWAY_API_KEY"},
      {"zai", "ZAI_API_KEY"},
      {"mistral", "MISTRAL_API_KEY"},
      {"minimax", "MINIMAX_API_KEY"},
      {"minimax-cn", "MINIMAX_CN_API_KEY"},
      {"moonshotai", "MOONSHOT_API_KEY"},
      {"moonshotai-cn", "MOONSHOT_API_KEY"},
      {"huggingface", "HF_TOKEN"},
      {"fireworks", "FIREWORKS_API_KEY"},
      {"together", "TOGETHER_API_KEY"},
      {"opencode", "OPENCODE_API_KEY"},
      {"opencode-go", "OPENCODE_API_KEY"},
      {"kimi-coding", "KIMI_API_KEY"},
      {"cloudflare-workers-ai", "CLOUDFLARE_API_KEY"},
      {"cloudflare-ai-gateway", "CLOUDFLARE_API_KEY"},
      {"xiaomi", "XIAOMI_API_KEY"},
      {"xiaomi-token-plan-cn", "XIAOMI_TOKEN_PLAN_CN_API_KEY"},
      {"xiaomi-token-plan-ams", "XIAOMI_TOKEN_PLAN_AMS_API_KEY"},
      {"xiaomi-token-plan-sgp", "XIAOMI_TOKEN_PLAN_SGP_API_KEY"},
  };

  for (const auto& entry : kEnvVars) {
    if (key == entry.first) {
      return {entry.second};
    }
  }
  return {};
}

std::vector<std::string> find_auth_env_vars(const std::string& provider) {
  std::vector<std::string> found;
  for (const auto& name : auth_env_var_names_for_provider(provider)) {
    if (getenv_nonempty(name)) {
      found.push_back(name);
    }
  }
  return found;
}

std::optional<std::string> get_env_api_key(const std::string& provider) {
  const std::string key = provider_key(provider);
  for (const auto& name : auth_env_var_names_for_provider(provider)) {
    if (const auto value = getenv_nonempty(name)) {
      if (key == "google-vertex" && is_google_vertex_adc_marker(*value)) {
        continue;
      }
      return value;
    }
  }
  if (key == "google-vertex" && has_google_vertex_adc_credentials()) {
    return "<authenticated>";
  }
  if (key == "amazon-bedrock" && has_amazon_bedrock_ambient_credentials()) {
    return "<authenticated>";
  }
  return std::nullopt;
}

Result<AuthStorage> parse_auth_storage_json(const Json& json) {
  if (!json.is_object()) {
    return Result<AuthStorage>::failure(ErrorCode::invalid_argument,
                                        "auth storage document must be a JSON object");
  }

  if (const auto it = json.find("version"); it != json.end()) {
    if (!it->is_number_integer()) {
      return Result<AuthStorage>::failure(ErrorCode::invalid_argument,
                                          "version must be an integer");
    }
    if (it->get<int>() != kAuthStorageVersion) {
      return Result<AuthStorage>::failure(
          ErrorCode::invalid_argument,
          "unsupported auth storage version: " + std::to_string(it->get<int>()));
    }
  }

  const auto providers_it = json.find("providers");
  if (providers_it == json.end()) {
    AuthStorage storage;
    for (auto it = json.begin(); it != json.end(); ++it) {
      if (!looks_like_legacy_auth_record(it.value())) {
        continue;
      }
      const auto record = parse_legacy_provider_auth_record(it.key(), it.value());
      if (!record) {
        return Result<AuthStorage>::failure(record.error().code,
                                            record.error().message);
      }
      storage.providers.push_back(record.value());
    }

    const auto errors = validate(storage);
    if (!errors.empty()) {
      return Result<AuthStorage>::failure(ErrorCode::invalid_argument, errors.front());
    }
    return normalized(storage);
  }
  if (!providers_it->is_object()) {
    return Result<AuthStorage>::failure(ErrorCode::invalid_argument,
                                        "providers must be an object");
  }

  AuthStorage storage;
  for (auto it = providers_it->begin(); it != providers_it->end(); ++it) {
    const auto record = parse_provider_auth_record(it.key(), it.value());
    if (!record) {
      return Result<AuthStorage>::failure(record.error().code, record.error().message);
    }
    storage.providers.push_back(record.value());
  }

  const auto errors = validate(storage);
  if (!errors.empty()) {
    return Result<AuthStorage>::failure(ErrorCode::invalid_argument, errors.front());
  }

  return normalized(storage);
}

Json to_json(const AuthStorage& storage) {
  const AuthStorage normalized_storage = normalized(storage);

  Json json = Json::object();
  json["version"] = kAuthStorageVersion;
  json["providers"] = Json::object();

  for (const auto& record : normalized_storage.providers) {
    Json provider_json = Json::object();
    if (record.api_key) {
      provider_json["apiKey"] = *record.api_key;
    }
    if (record.token) {
      provider_json["token"] = *record.token;
    }
    if (!record.metadata.empty()) {
      provider_json["metadata"] = record.metadata;
    }
    json["providers"][record.provider] = std::move(provider_json);
  }

  return json;
}

Result<AuthStorage> load_auth_storage(const foundation::Path& path) {
  const auto content = foundation::read_text_file(path);
  if (!content) {
    if (content.error().code == ErrorCode::not_found) {
      return AuthStorage{};
    }
    return content.error();
  }

  Json json;
  try {
    json = Json::parse(content.value());
  } catch (const Json::parse_error& error) {
    return Result<AuthStorage>::failure(
        ErrorCode::invalid_argument,
        "invalid auth storage JSON in " + foundation::path_for_display(path) + ": " +
            error.what());
  }

  return parse_auth_storage_json(json);
}

Status save_auth_storage(const foundation::Path& path, const AuthStorage& storage) {
  const auto errors = validate(storage);
  if (!errors.empty()) {
    return Status::failure(ErrorCode::invalid_argument, errors.front());
  }

  const Json json = to_json(storage);
  return foundation::write_text_file(path, json.dump(2) + "\n");
}

}  // namespace qt::ai
