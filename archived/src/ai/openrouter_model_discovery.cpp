#include "qt/ai/openrouter_model_discovery.hpp"

#include "qt/ai/provider_transport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace qt::ai {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kDefaultOpenRouterTokenLimit = 4096;

std::optional<std::string> getenv_nonempty(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

bool offline_requested() {
  const auto value = getenv_nonempty("PI_OFFLINE");
  return value && (*value == "1" || *value == "true" || *value == "yes");
}

std::optional<double> json_number_or_string(const Json& object,
                                            const char* key) {
  if (!object.is_object() || !object.contains(key) || object.at(key).is_null()) {
    return std::nullopt;
  }
  const auto& value = object.at(key);
  if (value.is_number()) {
    return value.get<double>();
  }
  if (!value.is_string()) {
    return std::nullopt;
  }
  try {
    return std::stod(value.get<std::string>());
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::int64_t> json_positive_integer(const Json& object,
                                                  const char* key) {
  if (!object.is_object() || !object.contains(key) || object.at(key).is_null()) {
    return std::nullopt;
  }
  const auto& value = object.at(key);
  if (value.is_number_unsigned()) {
    const auto parsed = value.get<std::uint64_t>();
    if (parsed == 0 ||
        parsed > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(parsed);
  }
  if (value.is_number_integer()) {
    const auto parsed = value.get<std::int64_t>();
    return parsed > 0 ? std::optional<std::int64_t>(parsed) : std::nullopt;
  }
  if (!value.is_number_float()) {
    return std::nullopt;
  }
  const auto parsed = value.get<double>();
  if (!std::isfinite(parsed) || parsed < 1.0 || std::floor(parsed) != parsed ||
      parsed > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(parsed);
}

struct OpenRouterTokenLimits {
  std::int64_t model_context_window = kDefaultOpenRouterTokenLimit;
  std::optional<std::int64_t> provider_context_window;
  std::int64_t context_window = kDefaultOpenRouterTokenLimit;
  std::int64_t max_tokens = kDefaultOpenRouterTokenLimit;
};

OpenRouterTokenLimits token_limits_from_openrouter_model(const Json& model) {
  OpenRouterTokenLimits limits;
  limits.model_context_window =
      json_positive_integer(model, "context_length")
          .value_or(kDefaultOpenRouterTokenLimit);

  const auto top_provider = model.is_object()
                                ? model.value("top_provider", Json::object())
                                : Json::object();
  limits.provider_context_window =
      json_positive_integer(top_provider, "context_length");
  limits.context_window = limits.provider_context_window
                              ? std::min(limits.model_context_window,
                                         *limits.provider_context_window)
                              : limits.model_context_window;
  limits.max_tokens =
      json_positive_integer(top_provider, "max_completion_tokens")
          .value_or(kDefaultOpenRouterTokenLimit);
  limits.max_tokens = std::min(limits.max_tokens, limits.context_window);
  return limits;
}

Json normalized_cached_metadata(Json metadata) {
  if (!metadata.is_object()) {
    metadata = Json::object();
  }

  const auto cached_context = json_positive_integer(metadata, "contextWindow")
                                  .value_or(kDefaultOpenRouterTokenLimit);
  const auto model_context =
      json_positive_integer(metadata, "modelContextWindow")
          .value_or(cached_context);
  const auto provider_context =
      json_positive_integer(metadata, "providerContextWindow");
  std::int64_t context_window = std::min(cached_context, model_context);
  if (provider_context) {
    context_window = std::min(context_window, *provider_context);
  }
  const auto max_tokens =
      std::min(json_positive_integer(metadata, "maxTokens")
                   .value_or(kDefaultOpenRouterTokenLimit),
               context_window);

  metadata["contextWindow"] = context_window;
  metadata["maxTokens"] = max_tokens;
  metadata["modelContextWindow"] = model_context;
  if (provider_context) {
    metadata["providerContextWindow"] = *provider_context;
  } else {
    metadata.erase("providerContextWindow");
  }
  if (!metadata.contains("baseUrl") ||
      !metadata.at("baseUrl").is_string() ||
      metadata.at("baseUrl").get<std::string>().empty()) {
    metadata["baseUrl"] = "https://openrouter.ai/api/v1";
  }
  return metadata;
}

bool string_array_contains(const Json& array, std::string_view needle) {
  if (!array.is_array()) {
    return false;
  }
  return std::any_of(array.begin(), array.end(), [&](const Json& value) {
    return value.is_string() && value.get<std::string>() == needle;
  });
}

std::vector<std::string> input_modalities(const Json& model) {
  std::vector<std::string> input{"text"};
  const auto architecture = model.value("architecture", Json::object());
  const auto modalities =
      architecture.is_object()
          ? architecture.value("input_modalities", Json::array())
          : Json::array();
  if (string_array_contains(modalities, "image")) {
    input.push_back("image");
  }
  return input;
}

Json cost_metadata_from_openrouter_pricing(const Json& model) {
  const auto pricing = model.value("pricing", Json::object());
  const auto per_million = [](std::optional<double> per_token) {
    return per_token.value_or(0.0) * 1'000'000.0;
  };
  return Json{{"input_usd_per_million",
               per_million(json_number_or_string(pricing, "prompt"))},
              {"output_usd_per_million",
               per_million(json_number_or_string(pricing, "completion"))},
              {"cache_read_usd_per_million",
               per_million(json_number_or_string(pricing, "input_cache_read"))},
              {"cache_write_usd_per_million",
               per_million(json_number_or_string(pricing, "input_cache_write"))}};
}

bool supports_reasoning(const Json& model) {
  const auto parameters = model.value("supported_parameters", Json::array());
  return string_array_contains(parameters, "reasoning") ||
         string_array_contains(parameters, "include_reasoning") ||
         string_array_contains(parameters, "reasoning_effort");
}

std::optional<ModelCatalogModelRecord> model_from_openrouter_json(
    const Json& model) {
  if (!model.is_object() || !model.contains("id") ||
      !model.at("id").is_string()) {
    return std::nullopt;
  }

  const std::string id = model.at("id").get<std::string>();
  if (id.empty()) {
    return std::nullopt;
  }

  const auto architecture = model.value("architecture", Json::object());
  const auto output_modalities =
      architecture.is_object()
          ? architecture.value("output_modalities", Json::array())
          : Json::array();
  if (output_modalities.is_array() && !output_modalities.empty() &&
      !string_array_contains(output_modalities, "text")) {
    return std::nullopt;
  }

  const std::string display_name =
      model.contains("name") && model.at("name").is_string() &&
              !model.at("name").get<std::string>().empty()
          ? model.at("name").get<std::string>()
          : std::string("OpenRouter ") + id;
  const auto limits = token_limits_from_openrouter_model(model);

  ModelCatalogModelRecord record;
  record.ref = ModelRef{"openrouter", "openai-completions", id};
  record.display_name = display_name;
  record.capabilities.reasoning = supports_reasoning(model);
  const auto input = input_modalities(model);
  record.capabilities.vision =
      std::find(input.begin(), input.end(), "image") != input.end();
  record.metadata = Json{{"input", input},
                         {"cost", cost_metadata_from_openrouter_pricing(model)},
                         {"contextWindow", limits.context_window},
                         {"maxTokens", limits.max_tokens},
                         {"modelContextWindow", limits.model_context_window},
                         {"baseUrl", "https://openrouter.ai/api/v1"}};
  if (limits.provider_context_window) {
    record.metadata["providerContextWindow"] =
        *limits.provider_context_window;
  }
  return record;
}

std::optional<std::string> openrouter_cache_path() {
  if (const auto configured = getenv_nonempty("QT_CODING_AGENT_DIR")) {
    return (std::filesystem::path(*configured) / "openrouter_models.json")
        .generic_string();
  }
  auto home = getenv_nonempty("HOME");
  if (!home) {
    home = getenv_nonempty("USERPROFILE");
  }
  if (!home) {
    return std::nullopt;
  }
  return (std::filesystem::path(*home) / ".qt" / "openrouter_models.json")
      .generic_string();
}

bool save_openrouter_catalog_to_cache(const ModelCatalog& catalog) {
  const auto path = openrouter_cache_path();
  if (!path) {
    return false;
  }

  try {
    std::filesystem::create_directories(
        std::filesystem::path(*path).parent_path());

    Json cache_data = Json::object();
    cache_data["timestamp"] = std::time(nullptr);

    Json models_array = Json::array();
    for (const auto& model : catalog.models) {
      Json model_json = Json::object();
      model_json["id"] = model.ref.name;
      model_json["display_name"] = model.display_name;
      model_json["reasoning"] = model.capabilities.reasoning;
      model_json["vision"] = model.capabilities.vision;
      model_json["metadata"] = model.metadata;
      models_array.push_back(model_json);
    }
    cache_data["models"] = models_array;

    std::ofstream output(*path);
    if (!output) {
      return false;
    }
    output << cache_data.dump(2);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::optional<ModelCatalog> load_openrouter_catalog_from_cache(
    bool allow_stale = false) {
  const auto path = openrouter_cache_path();
  if (!path) {
    return std::nullopt;
  }

  std::ifstream input(*path);
  if (!input) {
    return std::nullopt;
  }

  try {
    const Json cache_data = Json::parse(input);
    if (!cache_data.is_object() || !cache_data.contains("models") ||
        !cache_data["models"].is_array()) {
      return std::nullopt;
    }

    // Network-fallback reads require a valid, recent timestamp. Startup reads
    // explicitly allow stale last-known metadata.
    const auto cache_timestamp =
        json_positive_integer(cache_data, "timestamp");
    if (!allow_stale) {
      if (!cache_timestamp) {
        return std::nullopt;
      }
      const auto cache_time = static_cast<std::time_t>(*cache_timestamp);
      const auto now = std::time(nullptr);
      const auto age_hours = (now - cache_time) / 3600;
      if (age_hours > 168) {  // 7 days = 168 hours
        return std::nullopt;
      }
    }

    const auto base_catalog = default_model_catalog();
    ModelCatalog catalog;
    if (auto provider = find_provider_record(base_catalog, "openrouter");
        provider != nullptr) {
      catalog.providers.push_back(*provider);
    }

    for (const auto& model_json : cache_data["models"]) {
      if (!model_json.is_object()) {
        continue;
      }

      if (!model_json.contains("id") || !model_json.at("id").is_string()) {
        continue;
      }
      const auto id = model_json.at("id").get<std::string>();
      if (id.empty()) {
        continue;
      }

      ModelCatalogModelRecord record;
      record.ref =
          ModelRef{"openrouter", "openai-completions", std::move(id)};
      record.display_name =
          model_json.contains("display_name") &&
                  model_json.at("display_name").is_string() &&
                  !model_json.at("display_name").get<std::string>().empty()
              ? model_json.at("display_name").get<std::string>()
              : std::string("OpenRouter ") + record.ref.name;
      record.capabilities.reasoning =
          model_json.contains("reasoning") &&
          model_json.at("reasoning").is_boolean() &&
          model_json.at("reasoning").get<bool>();
      record.capabilities.vision =
          model_json.contains("vision") &&
          model_json.at("vision").is_boolean() &&
          model_json.at("vision").get<bool>();
      record.metadata = normalized_cached_metadata(
          model_json.contains("metadata") ? model_json.at("metadata")
                                          : Json::object());
      catalog.models.push_back(std::move(record));
    }

    return normalized(catalog);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

foundation::Result<ModelCatalog> model_catalog_from_openrouter_response(
    const Json& response) {
  if (!response.is_object()) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenRouter models response must be an object");
  }
  const auto data = response.find("data");
  if (data == response.end() || !data->is_array()) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenRouter models response did not contain a data array");
  }

  const auto base_catalog = default_model_catalog();
  ModelCatalog catalog;
  if (const auto* provider = find_provider_record(base_catalog, "openrouter")) {
    catalog.providers.push_back(*provider);
  }
  for (const auto& item : *data) {
    if (auto model = model_from_openrouter_json(item)) {
      catalog.models.push_back(std::move(*model));
    }
  }
  return normalized(catalog);
}

ModelCatalog cached_openrouter_models() {
  return load_openrouter_catalog_from_cache(true).value_or(ModelCatalog{});
}

ModelCatalog replace_openrouter_models(
    const ModelCatalog& base, const ModelCatalog& previous_discovery,
    const ModelCatalog& refreshed_discovery) {
  ModelCatalog retained = base;
  retained.models.erase(
      std::remove_if(
          retained.models.begin(), retained.models.end(),
          [&](const ModelCatalogModelRecord& model) {
            const auto* previous = find_model_record(
                previous_discovery, model.ref.provider, model.ref.name);
            return previous != nullptr &&
                   model.ref.provider == previous->ref.provider &&
                   model.ref.api == previous->ref.api &&
                   model.ref.name == previous->ref.name &&
                   model.display_name == previous->display_name &&
                   model.capabilities.streaming ==
                       previous->capabilities.streaming &&
                   model.capabilities.tool_calls ==
                       previous->capabilities.tool_calls &&
                   model.capabilities.vision == previous->capabilities.vision &&
                   model.capabilities.reasoning ==
                       previous->capabilities.reasoning &&
                   model.capabilities.session_caching ==
                       previous->capabilities.session_caching &&
                   model.metadata == previous->metadata;
          }),
      retained.models.end());
  ModelCatalog overlay = refreshed_discovery;
  if (find_provider_record(retained, "openrouter") != nullptr) {
    overlay.providers.clear();
  }
  return merge_catalogs(retained, overlay);
}

foundation::Result<ModelCatalog> discover_openrouter_models(
    const AuthStorage& auth_storage, bool allow_cached_fallback) {
  if (offline_requested()) {
    // In offline mode, try to load from cache
    if (auto cached = load_openrouter_catalog_from_cache()) {
      return *cached;
    }
    return ModelCatalog{};
  }

  auto secret = get_stored_auth_secret(auth_storage, "openrouter");
  if (!secret || secret->empty()) {
    secret = get_env_api_key("openrouter");
  }

  HttpRequest request;
  request.method = "GET";
  request.url = "https://openrouter.ai/api/v1/models";
  request.timeout_ms = 10'000;
  request.headers.emplace("Accept", "application/json");
  if (secret && !secret->empty()) {
    request.headers.emplace("Authorization", "Bearer " + *secret);
  }

  HttpResponse response;
  bool api_succeeded = false;
  std::string api_error;
  try {
    BeastHttpTransport transport;
    response = transport.send(request);

    if (response.status >= 200 && response.status < 300) {
      api_succeeded = true;
    } else {
      api_error = "OpenRouter models request failed with HTTP " +
                  std::to_string(response.status);
    }
  } catch (const std::exception& error) {
    api_error = std::string("failed to fetch OpenRouter models: ") + error.what();
  }

  if (api_succeeded) {
    Json body;
    try {
      body = Json::parse(response.body);
    } catch (const Json::parse_error& error) {
      api_error = std::string("invalid OpenRouter models response: ") + error.what();
      api_succeeded = false;
    }

    if (api_succeeded) {
      auto parsed = model_catalog_from_openrouter_response(body);
      if (!parsed) {
        api_error = parsed.error().message;
        api_succeeded = false;
      } else {
        save_openrouter_catalog_to_cache(parsed.value());
        return parsed.value();
      }
    }
  }

  // General discovery may retain a recent cache. Explicit refresh callers
  // disable this fallback so they can report the network failure accurately.
  if (allow_cached_fallback) {
    if (auto cached = load_openrouter_catalog_from_cache()) {
      return *cached;
    }
  }

  // No cache available, return the API error
  return foundation::Result<ModelCatalog>::failure(
      foundation::ErrorCode::io_error, api_error);
}

foundation::Result<OpenRouterCredits> fetch_openrouter_credits(
    const std::string& api_key) {
  if (api_key.empty()) {
    return foundation::Result<OpenRouterCredits>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenRouter credits request requires an API key");
  }

  HttpRequest request;
  request.method = "GET";
  request.url = "https://openrouter.ai/api/v1/credits";
  request.timeout_ms = 10'000;
  request.headers.emplace("Accept", "application/json");
  request.headers.emplace("Authorization", "Bearer " + api_key);

  HttpResponse response;
  try {
    BeastHttpTransport transport;
    response = transport.send(request);
  } catch (const std::exception& error) {
    return foundation::Result<OpenRouterCredits>::failure(
        foundation::ErrorCode::io_error,
        std::string("failed to fetch OpenRouter credits: ") + error.what());
  }

  if (response.status < 200 || response.status >= 300) {
    return foundation::Result<OpenRouterCredits>::failure(
        foundation::ErrorCode::io_error,
        "OpenRouter credits request failed with HTTP " +
            std::to_string(response.status));
  }

  Json body;
  try {
    body = Json::parse(response.body);
  } catch (const Json::parse_error& error) {
    return foundation::Result<OpenRouterCredits>::failure(
        foundation::ErrorCode::invalid_argument,
        std::string("invalid OpenRouter credits response: ") + error.what());
  }

  const auto data = body.find("data");
  if (data == body.end() || !data->is_object()) {
    return foundation::Result<OpenRouterCredits>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenRouter credits response did not contain a data object");
  }

  OpenRouterCredits credits;
  credits.total_credits =
      json_number_or_string(*data, "total_credits").value_or(0.0);
  credits.total_usage =
      json_number_or_string(*data, "total_usage").value_or(0.0);
  return foundation::Result<OpenRouterCredits>::success(std::move(credits));
}

}  // namespace qt::ai
