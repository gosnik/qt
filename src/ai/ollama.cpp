#include "qt/ai/ollama.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace qt::ai {
namespace {

std::string trim_copy(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  return first >= last ? std::string{} : std::string(first, last);
}

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::optional<std::int64_t> parameter_context_window(const Json& parameters) {
  if (!parameters.is_object()) {
    return std::nullopt;
  }
  for (auto it = parameters.begin(); it != parameters.end(); ++it) {
    if (it.key().size() < 15U ||
        it.key().substr(it.key().size() - 15U) != ".context_length") {
      continue;
    }
    if (it.value().is_number_integer()) {
      return std::max<std::int64_t>(1, it.value().get<std::int64_t>());
    }
    if (it.value().is_string()) {
      try {
        return std::max<std::int64_t>(1, std::stoll(it.value().get<std::string>()));
      } catch (const std::exception&) {
      }
    }
  }
  return std::nullopt;
}

bool has_capability(const std::vector<std::string>& capabilities,
                    const std::string& value) {
  return std::any_of(capabilities.begin(), capabilities.end(), [&](const auto& item) {
    return lowercase_copy(item) == value;
  });
}

std::string response_error(const HttpResponse& response) {
  try {
    const auto body = Json::parse(response.body);
    if (body.is_object() && body.contains("error") && body.at("error").is_string()) {
      return body.at("error").get<std::string>();
    }
  } catch (const std::exception&) {
  }
  return response.body.empty() ? "HTTP " + std::to_string(response.status)
                               : response.body;
}

OllamaModelInfo model_from_json(const Json& tag, const Json& shown) {
  OllamaModelInfo result;
  result.name = tag.value("name", tag.value("model", std::string{}));
  result.modified_at = tag.value("modified_at", std::string{});
  result.digest = tag.value("digest", std::string{});
  if (tag.contains("size") && tag.at("size").is_number_unsigned()) {
    result.size = tag.at("size").get<std::uint64_t>();
  }
  result.details = shown.value("details", tag.value("details", Json::object()));
  if (!result.details.is_object()) {
    result.details = Json::object();
  }
  if (shown.contains("capabilities") && shown.at("capabilities").is_array()) {
    for (const auto& capability : shown.at("capabilities")) {
      if (capability.is_string()) {
        result.capabilities.push_back(capability.get<std::string>());
      }
    }
  }
  auto context = parameter_context_window(shown.value("model_info", Json::object()));
  if (!context) {
    context = parameter_context_window(shown.value("parameters", Json::object()));
  }
  result.context_window = context.value_or(kDefaultOllamaContextWindow);
  result.max_tokens = std::min(result.context_window, kDefaultOllamaMaxTokens);
  return result;
}

}  // namespace

std::string normalize_ollama_base_url(std::string value) {
  value = trim_copy(std::move(value));
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  if (value.size() >= 3U && value.compare(value.size() - 3U, 3U, "/v1") == 0) {
    value.resize(value.size() - 3U);
  }
  return value;
}

std::string ollama_provider_id(const std::string& instance_id) {
  return "ollama:" + instance_id;
}

bool is_ollama_provider_id(const std::string& provider_id) noexcept {
  return provider_id.size() > 7U && provider_id.compare(0U, 7U, "ollama:") == 0;
}

foundation::Result<Json> ollama_request(
    const OllamaInstanceSettings& instance, const std::string& method,
    const std::string& path, const Json& body, HttpTransport* transport) {
  const std::string base_url = normalize_ollama_base_url(instance.base_url);
  if (base_url.rfind("http://", 0U) != 0U && base_url.rfind("https://", 0U) != 0U) {
    return foundation::Result<Json>::failure(
        foundation::ErrorCode::invalid_argument,
        "Ollama base URL must start with http:// or https://");
  }

  BeastHttpTransport owned_transport;
  HttpTransport& active_transport = transport == nullptr ?
      static_cast<HttpTransport&>(owned_transport) : *transport;
  HttpRequest request;
  request.method = method;
  request.url = base_url + (path.empty() || path.front() == '/' ? path : "/" + path);
  request.timeout_ms = std::max<std::int64_t>(1, instance.timeout_ms);
  request.headers["Accept"] = "application/json";
  if (instance.api_key && !instance.api_key->empty()) {
    request.headers["Authorization"] = "Bearer " + *instance.api_key;
  }
  if (!body.is_null()) {
    request.headers["Content-Type"] = "application/json";
    request.body = body.dump();
  }

  try {
    const auto response = active_transport.send(request);
    if (response.status < 200 || response.status >= 300) {
      return foundation::Result<Json>::failure(
          foundation::ErrorCode::io_error,
          "Ollama " + method + " " + path + " failed: " + response_error(response));
    }
    if (response.body.empty()) {
      return foundation::Result<Json>::success(Json::object());
    }
    return foundation::Result<Json>::success(Json::parse(response.body));
  } catch (const std::exception& error) {
    return foundation::Result<Json>::failure(
        foundation::ErrorCode::io_error,
        "Ollama " + method + " " + path + " failed: " + error.what());
  }
}

foundation::Result<std::vector<OllamaModelInfo>> discover_ollama_models(
    const OllamaInstanceSettings& instance, HttpTransport* transport) {
  const auto listed = ollama_request(instance, "GET", "/api/tags", Json(), transport);
  if (!listed) {
    return foundation::Result<std::vector<OllamaModelInfo>>::failure(
        listed.error().code, listed.error().message, listed.error().system_error);
  }
  const auto models = listed.value().find("models");
  if (models == listed.value().end() || !models->is_array()) {
    return foundation::Result<std::vector<OllamaModelInfo>>::failure(
        foundation::ErrorCode::invalid_argument,
        "Ollama /api/tags response is missing models array");
  }

  std::vector<OllamaModelInfo> result;
  result.reserve(models->size());
  for (const auto& model : *models) {
    if (!model.is_object()) {
      continue;
    }
    const std::string name = model.value("name", model.value("model", std::string{}));
    if (name.empty()) {
      continue;
    }
    Json shown = Json::object();
    const auto info = ollama_request(
        instance, "POST", "/api/show", Json{{"model", name}, {"verbose", false}},
        transport);
    if (info) {
      shown = info.value();
    }
    result.push_back(model_from_json(model, shown));
  }
  std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
    return lowercase_copy(lhs.name) < lowercase_copy(rhs.name);
  });
  return foundation::Result<std::vector<OllamaModelInfo>>::success(std::move(result));
}

ModelCatalog model_catalog_from_ollama_settings(
    const std::vector<OllamaInstanceSettings>& instances) {
  ModelCatalog catalog;
  for (const auto& instance : instances) {
    if (!instance.enabled || instance.id.empty()) {
      continue;
    }
    const std::string provider_id = ollama_provider_id(instance.id);
    const std::string display_name = instance.name.empty() ? instance.id : instance.name;
    ModelCatalogProviderRecord provider;
    provider.descriptor.id = provider_id;
    provider.descriptor.display_name = "Ollama · " + display_name;
    provider.descriptor.api = "openai-completions";
    provider.metadata = Json{{"baseUrl", normalize_ollama_base_url(instance.base_url) + "/v1"},
                             {"requiresAuth", false},
                             {"direct", true},
                             {"ollama", true},
                             {"ollamaInstance", instance.id}};
    if (instance.api_key && !instance.api_key->empty()) {
      provider.metadata["apiKey"] = *instance.api_key;
    }
    catalog.providers.push_back(provider);

    for (const auto& model : instance.models) {
      if (model.name.empty()) {
        continue;
      }
      ModelCatalogModelRecord record;
      record.ref = ModelRef{provider_id, "openai-completions", model.name};
      record.display_name = model.name + " · " + display_name;
      record.capabilities.vision = has_capability(model.capabilities, "vision") ||
                                   has_capability(model.capabilities, "image");
      record.capabilities.reasoning = has_capability(model.capabilities, "thinking") ||
                                      has_capability(model.capabilities, "reasoning");
      record.metadata = Json{{"baseUrl", normalize_ollama_base_url(instance.base_url) + "/v1"},
                             {"timeoutMs", instance.timeout_ms},
                             {"contextWindow", model.context_window},
                             {"maxTokens", model.max_tokens},
                             {"input", record.capabilities.vision ? Json{"text", "image"} : Json{"text"}},
                             {"cost", Json{{"input", 0.0}, {"output", 0.0},
                                            {"cacheRead", 0.0}, {"cacheWrite", 0.0}}},
                             {"ollama", Json{{"instance", instance.id},
                                              {"digest", model.digest},
                                              {"size", model.size},
                                              {"modifiedAt", model.modified_at},
                                              {"capabilities", model.capabilities},
                                              {"details", model.details}}}};
      if (instance.api_key && !instance.api_key->empty()) {
        record.metadata["apiKey"] = *instance.api_key;
      }
      catalog.models.push_back(std::move(record));
    }
  }
  return normalized(catalog);
}

}  // namespace qt::ai
