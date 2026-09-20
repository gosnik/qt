#include "qt/app/runtime/model_resolution.hpp"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#include "qt/app/runtime/provider_identity.hpp"
#include "qt/foundation/process.hpp"

namespace qt::app {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

std::size_t count_matching_models(const ai::ModelCatalog& catalog,
                                  const std::string& model_id) {
  const std::string wanted = lowercase_copy(trim_copy(model_id));
  if (wanted.empty()) {
    return 0;
  }

  std::size_t matches = 0;
  for (const auto& model : catalog.models) {
    if (lowercase_copy(trim_copy(model.ref.name)) == wanted) {
      ++matches;
    }
  }
  return matches;
}

std::string provider_alternatives(const ai::ModelCatalog& catalog) {
  std::ostringstream out;
  bool first = true;
  for (const auto& provider : catalog.providers) {
    if (!first) {
      out << ", ";
    }
    out << provider.descriptor.id;
    first = false;
  }
  return out.str();
}

std::string model_alternatives(const ai::ModelCatalog& catalog,
                               const std::optional<std::string>& provider) {
  std::ostringstream out;
  bool first = true;
  std::size_t count = 0;
  constexpr std::size_t max_alternatives = 8U;
  for (const auto& model : catalog.models) {
    if (provider && !provider_ids_equal(model.ref.provider, *provider)) {
      continue;
    }
    if (count >= max_alternatives) {
      out << ", ...";
      break;
    }
    if (!first) {
      out << ", ";
    }
    out << model.ref.provider << "/" << model.ref.name;
    first = false;
    ++count;
  }
  return out.str();
}

std::string unknown_provider_error(const ai::ModelCatalog& catalog,
                                   const std::string& provider) {
  const auto alternatives = provider_alternatives(catalog);
  if (alternatives.empty()) {
    return "unknown provider selection: " + provider;
  }
  return "unknown provider selection: " + provider +
         ". Available providers: " + alternatives;
}

std::string unknown_model_error(const ai::ModelCatalog& catalog,
                                const std::string& model,
                                const std::optional<std::string>& provider) {
  const auto alternatives = model_alternatives(catalog, provider);
  if (alternatives.empty()) {
    return "unknown model selection: " + model;
  }
  return "unknown model selection: " + model +
         ". Available models include: " + alternatives;
}

ai::Json synthetic_model_metadata(
    const ai::ModelCatalogProviderRecord& provider) {
  ai::Json metadata = ai::Json::object();
  if (!provider_ids_equal(provider.descriptor.id, "openrouter")) {
    return metadata;
  }

  // Match reference/pi's conservative custom-model defaults. Unknown
  // OpenRouter IDs must still have a positive context contract so compaction
  // and request budgeting remain enabled until catalog discovery catches up.
  metadata = ai::Json{{"input", ai::Json::array({"text"})},
                      {"contextWindow", 128000},
                      {"maxTokens", 16384},
                      {"baseUrl", "https://openrouter.ai/api/v1"}};
  return metadata;
}

std::optional<std::string> resolve_provider_config_value(
    const std::string& configured);

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

std::optional<std::string> auth_metadata_string(
    const ai::ProviderAuthRecord& auth, const std::string& key) {
  if (!auth.metadata.is_object() || !auth.metadata.contains(key) ||
      !auth.metadata.at(key).is_string()) {
    return std::nullopt;
  }
  const auto value = trim_copy(auth.metadata.at(key).get<std::string>());
  return value.empty() ? std::optional<std::string>{} : value;
}

void apply_oauth_model_modifications(ResolvedModelSelection& resolved) {
  if (!resolved.auth ||
      provider_key(resolved.provider.descriptor.id) != "github-copilot") {
    return;
  }
  const auto& auth = *resolved.auth;
  if (!auth.metadata.is_object() ||
      auth.metadata.value("type", std::string{}) != "oauth") {
    return;
  }
  const std::string access =
      auth_metadata_string(auth, "access").value_or(auth.token.value_or(""));
  if (trim_copy(access).empty()) {
    return;
  }
  const std::string enterprise =
      auth_metadata_string(auth, "enterpriseUrl").value_or("");
  if (!resolved.model.metadata.is_object()) {
    resolved.model.metadata = ai::Json::object();
  }
  resolved.model.metadata["baseUrl"] =
      github_copilot_base_url(access, enterprise);
}

bool provider_metadata_has_resolvable_api_key(
    const ai::ModelCatalogProviderRecord& provider) {
  if (!provider.metadata.is_object() || !provider.metadata.contains("apiKey") ||
      !provider.metadata.at("apiKey").is_string()) {
    return false;
  }
  const std::string configured =
      trim_copy(provider.metadata.at("apiKey").get<std::string>());
  if (configured.empty()) {
    return false;
  }
  if (configured.front() != '!') {
    return true;
  }
  return resolve_provider_config_value(configured).has_value();
}

std::optional<std::string> resolve_command_config_value(
    const std::string& configured) {
  static std::map<std::string, std::optional<std::string>> cache;
  static std::mutex cache_mutex;
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    const auto cached = cache.find(configured);
    if (cached != cache.end()) {
      return cached->second;
    }
  }

  const std::string command = configured.substr(1U);
  std::optional<std::string> resolved;
  const auto result = qt::foundation::run_shell_command(command, std::nullopt,
                                                        std::int64_t{10'000});
  if (result && !result.value().timed_out && result.value().exit_code == 0) {
    const std::string stdout_text = trim_copy(result.value().stdout_text);
    if (!stdout_text.empty()) {
      resolved = stdout_text;
    }
  }
  std::lock_guard<std::mutex> lock(cache_mutex);
  return cache.emplace(configured, resolved).first->second;
}

std::optional<std::string> resolve_provider_config_value(
    const std::string& configured) {
  const std::string trimmed = trim_copy(configured);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  if (trimmed.front() == '!') {
    return resolve_command_config_value(trimmed);
  }
  if (const char* env = std::getenv(trimmed.c_str());
      env != nullptr && *env != '\0') {
    return std::string(env);
  }
  return trimmed;
}

std::optional<ResolvedModelSelection> build_resolution(
    const ai::ModelCatalog& catalog, const ai::AuthStorage& auth_storage,
    const ai::ModelCatalogModelRecord* model, ModelResolutionSource source) {
  if (model == nullptr) {
    return std::nullopt;
  }

  const auto* provider = ai::find_provider_record(catalog, model->ref.provider);
  if (provider == nullptr) {
    return std::nullopt;
  }

  ResolvedModelSelection resolved;
  resolved.provider = *provider;
  resolved.model = *model;
  resolved.source = source;
  if (const auto* auth =
          ai::find_provider_auth_record(auth_storage, provider->descriptor.id);
      auth != nullptr) {
    resolved.auth = *auth;
  } else if (provider->metadata.is_object() &&
             provider->metadata.contains("apiKey") &&
             provider->metadata.at("apiKey").is_string()) {
    const std::string configured =
        trim_copy(provider->metadata.at("apiKey").get<std::string>());
    if (const auto resolved_key = resolve_provider_config_value(configured)) {
      ai::ProviderAuthRecord auth_record;
      auth_record.provider = provider->descriptor.id;
      auth_record.api_key = *resolved_key;
      resolved.auth = std::move(auth_record);
    }
  }
  return resolved;
}

Result<ResolvedModelSelection> resolve_explicit_model(
    const ai::ModelCatalog& catalog, const ai::AuthStorage& auth_storage,
    const std::optional<std::string>& requested_provider,
    const std::string& requested_model) {
  const std::string trimmed_model = trim_copy(requested_model);
  if (trimmed_model.empty()) {
    return Result<ResolvedModelSelection>::failure(
        ErrorCode::invalid_argument, "model selection must not be empty");
  }

  if (requested_provider) {
    const std::string trimmed_provider = trim_copy(*requested_provider);
    if (trimmed_provider.empty()) {
      return Result<ResolvedModelSelection>::failure(
          ErrorCode::invalid_argument, "provider selection must not be empty");
    }
    if (ai::find_provider_record(catalog, trimmed_provider) == nullptr) {
      return Result<ResolvedModelSelection>::failure(
          ErrorCode::invalid_argument,
          unknown_provider_error(catalog, trimmed_provider));
    }
    const auto parsed = ai::parse_model_catalog_reference(trimmed_model);
    const std::string provider_scoped_model =
        parsed.provider &&
                provider_ids_equal(*parsed.provider, trimmed_provider)
            ? parsed.model
            : trimmed_model;
    if (const auto* matched = ai::find_model_record(
            catalog, trimmed_provider, provider_scoped_model)) {
      auto resolved = build_resolution(catalog, auth_storage, matched,
                                       ModelResolutionSource::explicit_model);
      if (!resolved) {
        return Result<ResolvedModelSelection>::failure(
            ErrorCode::internal_error,
            "resolved model references unknown provider");
      }
      return *resolved;
    }

    // Provider-scoped model IDs are opaque. OpenRouter and several compatible
    // providers use slash-delimited IDs such as qwen/qwen3.5-9b; parsing the
    // first component as another provider silently changes the saved selection.
    const auto* provider =
        ai::find_provider_record(catalog, trimmed_provider);
    ResolvedModelSelection resolved;
    resolved.provider = *provider;
    resolved.model.ref = ai::ModelRef{trimmed_provider,
                                      provider->descriptor.api,
                                      provider_scoped_model};
    resolved.model.display_name = provider_scoped_model;
    resolved.model.capabilities = {false, false};
    resolved.model.metadata = synthetic_model_metadata(*provider);
    resolved.source = ModelResolutionSource::explicit_model;
    if (const auto* auth = ai::find_provider_auth_record(
            auth_storage, provider->descriptor.id)) {
      resolved.auth = *auth;
    } else if (provider->metadata.is_object() &&
               provider->metadata.contains("apiKey") &&
               provider->metadata.at("apiKey").is_string()) {
      const std::string configured = trim_copy(
          provider->metadata.at("apiKey").get<std::string>());
      if (const auto resolved_key =
              resolve_provider_config_value(configured)) {
        ai::ProviderAuthRecord auth_record;
        auth_record.provider = provider->descriptor.id;
        auth_record.api_key = *resolved_key;
        resolved.auth = std::move(auth_record);
      }
    }
    apply_oauth_model_modifications(resolved);
    return resolved;
  }

  const auto parsed = ai::parse_model_catalog_reference(trimmed_model);
  if (parsed.provider &&
      ai::find_provider_record(catalog, *parsed.provider) == nullptr) {
    return Result<ResolvedModelSelection>::failure(
        ErrorCode::invalid_argument,
        unknown_provider_error(catalog, *parsed.provider));
  }

  const auto* matched = ai::match_model_record(catalog, trimmed_model);
  if (matched == nullptr) {
    // Fallback: if the provider is known, create a synthetic model record.
    // This allows users to specify models that are not yet in the catalog
    // (e.g., newly released models or dynamically discovered models).
    std::string fallback_provider = parsed.provider.value_or("");
    if (!fallback_provider.empty()) {
      const auto* provider = ai::find_provider_record(catalog, fallback_provider);
      if (provider != nullptr) {
        ResolvedModelSelection resolved;
        resolved.provider = *provider;
        resolved.model.ref = ai::ModelRef{fallback_provider, provider->descriptor.api, parsed.model};
        resolved.model.display_name = parsed.model;
        resolved.model.capabilities = {false, false};
        resolved.model.metadata = synthetic_model_metadata(*provider);
        resolved.source = ModelResolutionSource::explicit_model;
        
        if (const auto* auth = ai::find_provider_auth_record(auth_storage, provider->descriptor.id);
            auth != nullptr) {
          resolved.auth = *auth;
        } else if (provider->metadata.is_object() &&
                   provider->metadata.contains("apiKey") &&
                   provider->metadata.at("apiKey").is_string()) {
          const std::string configured = trim_copy(provider->metadata.at("apiKey").get<std::string>());
          if (const auto resolved_key = resolve_provider_config_value(configured)) {
            ai::ProviderAuthRecord auth_record;
            auth_record.provider = provider->descriptor.id;
            auth_record.api_key = *resolved_key;
            resolved.auth = std::move(auth_record);
          }
        }
        
        apply_oauth_model_modifications(resolved);
        return resolved;
      }
    }
    
    if (!parsed.provider && count_matching_models(catalog, parsed.model) > 1U) {
      return Result<ResolvedModelSelection>::failure(
          ErrorCode::invalid_argument,
          "ambiguous model selection: " + parsed.model);
    }
    return Result<ResolvedModelSelection>::failure(
        ErrorCode::invalid_argument,
        unknown_model_error(catalog, trimmed_model, requested_provider));
  }

  if (requested_provider &&
      !provider_ids_equal(matched->ref.provider, *requested_provider)) {
    return Result<ResolvedModelSelection>::failure(
        ErrorCode::invalid_argument,
        "model selection provider does not match requested provider: " +
            trimmed_model);
  }

  auto resolved = build_resolution(catalog, auth_storage, matched,
                                   ModelResolutionSource::explicit_model);
  if (!resolved) {
    return Result<ResolvedModelSelection>::failure(
        ErrorCode::internal_error, "resolved model references unknown provider");
  }
  return *resolved;
}

Result<ResolvedModelSelection> resolve_explicit_provider(
    const ai::ModelCatalog& catalog, const ai::AuthStorage& auth_storage,
    const std::string& requested_provider) {
  const std::string trimmed_provider = trim_copy(requested_provider);
  if (trimmed_provider.empty()) {
    return Result<ResolvedModelSelection>::failure(
        ErrorCode::invalid_argument, "provider selection must not be empty");
  }

  if (ai::find_provider_record(catalog, trimmed_provider) == nullptr) {
    return Result<ResolvedModelSelection>::failure(
        ErrorCode::invalid_argument,
        "unknown provider selection: " + trimmed_provider);
  }

  for (const auto& model : catalog.models) {
    if (!provider_ids_equal(model.ref.provider, trimmed_provider)) {
      continue;
    }
    auto resolved = build_resolution(catalog, auth_storage, &model,
                                     ModelResolutionSource::explicit_provider);
    if (!resolved) {
      break;
    }
    return *resolved;
  }

  return Result<ResolvedModelSelection>::failure(
      ErrorCode::not_found, "provider has no models: " + trimmed_provider);
}

}  // namespace

std::string default_auth_storage_path(const AppSettings& settings) {
  if (trim_copy(settings.agent_home).empty()) {
    return "";
  }
  return (std::filesystem::path(settings.agent_home) / "auth.json").generic_string();
}

foundation::Result<ResolvedModelSelection> resolve_model_selection(
    const ModelResolutionRequest& request) {
  const ai::ModelCatalog catalog = ai::normalized(
      request.catalog.models.empty() && request.catalog.providers.empty()
          ? ai::default_model_catalog()
          : request.catalog);
  const ai::AuthStorage auth_storage = ai::normalized(request.auth_storage);

  const auto catalog_errors = ai::validate(catalog);
  if (!catalog_errors.empty()) {
    return Result<ResolvedModelSelection>::failure(ErrorCode::invalid_argument,
                                                   catalog_errors.front());
  }

  const auto auth_errors = ai::validate(auth_storage);
  if (!auth_errors.empty()) {
    return Result<ResolvedModelSelection>::failure(ErrorCode::invalid_argument,
                                                   auth_errors.front());
  }

  if (catalog.models.empty()) {
    return Result<ResolvedModelSelection>::failure(ErrorCode::not_found,
                                                   "model catalog is empty");
  }

  Result<ResolvedModelSelection> resolved =
      request.model ? resolve_explicit_model(catalog, auth_storage, request.provider,
                                             *request.model)
                    : request.provider
                          ? resolve_explicit_provider(catalog, auth_storage,
                                                      *request.provider)
                          : Result<ResolvedModelSelection>::failure(
                                ErrorCode::internal_error, "unreachable");

  if (!request.model && !request.provider) {
    if (request.settings.default_provider && request.settings.default_model) {
      resolved = resolve_explicit_model(catalog, auth_storage,
                                        request.settings.default_provider,
                                        *request.settings.default_model);
      if (!resolved) {
        return resolved.error();
      }
      resolved.value().source = ModelResolutionSource::settings_default;
    }

    for (const auto& model : catalog.models) {
      if (resolved) {
        break;
      }
      const auto* provider = ai::find_provider_record(catalog, model.ref.provider);
      const bool has_stored_auth =
          ai::find_provider_auth_record(auth_storage, model.ref.provider) !=
          nullptr;
      const bool has_provider_config_auth =
          provider != nullptr && provider_metadata_has_resolvable_api_key(*provider);
      if (!has_stored_auth && !has_provider_config_auth) {
        continue;
      }
      auto fallback = build_resolution(catalog, auth_storage, &model,
                                       ModelResolutionSource::authenticated_fallback);
      if (fallback) {
        resolved = *fallback;
        break;
      }
    }

    if (!resolved) {
      auto fallback = build_resolution(catalog, auth_storage, &catalog.models.front(),
                                       ModelResolutionSource::catalog_default);
      if (!fallback) {
        return Result<ResolvedModelSelection>::failure(
            ErrorCode::internal_error, "resolved model references unknown provider");
      }
      resolved = *fallback;
    }
  }

  if (!resolved) {
    return resolved.error();
  }

  apply_oauth_model_modifications(resolved.value());
  resolved.value().auth_storage_path = default_auth_storage_path(request.settings);
  return resolved.value();
}

}  // namespace qt::app
