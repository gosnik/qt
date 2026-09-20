#include "qt/ai/openai_model_discovery.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>
#include <unordered_set>

namespace qt::ai {
namespace {

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

bool is_chat_model_id(const std::string& id) {
  const auto lower = lowercase(id);
  const auto fine_tune = lower.rfind("ft:", 0) == 0;
  if (fine_tune) {
    const auto base_end = lower.find(':', 3);
    return base_end != std::string::npos &&
           is_chat_model_id(id.substr(3, base_end - 3));
  }
  const bool gpt = lower.rfind("gpt-", 0) == 0;
  const bool chat_alias = lower == "chat-latest" ||
                          lower.rfind("chat-latest-", 0) == 0 ||
                          lower.rfind("chatgpt-", 0) == 0;
  const bool reasoning = lower == "o1" || lower.rfind("o1-", 0) == 0 ||
                         lower == "o3" || lower.rfind("o3-", 0) == 0 ||
                         lower == "o4" || lower.rfind("o4-", 0) == 0;
  if (!gpt && !chat_alias && !reasoning) {
    return false;
  }

  // The models endpoint also exposes modalities and specialized endpoints.
  // Keep this discovery limited to models usable by the chat/responses path.
  constexpr std::string_view excluded[] = {
      "-image", "image-", "-audio", "audio-", "-realtime",
      "realtime-", "-transcribe", "transcribe-", "-tts", "tts-",
      "embedding", "-instruct", "instruct-", "-search", "search-"};
  return std::none_of(std::begin(excluded), std::end(excluded),
                      [&](std::string_view token) {
                        return lower.find(token) != std::string::npos;
                      });
}

bool is_discovered_record(const ModelCatalogModelRecord& model) {
  return model.metadata.is_object() &&
         model.metadata.value("openaiDiscovery", false);
}

std::string trim_copy(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

}  // namespace

foundation::Result<ModelCatalog> model_catalog_from_openai_response(
    const Json& response, const ModelCatalog& known) {
  if (!response.is_object()) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenAI models response must be an object");
  }
  const auto data = response.find("data");
  if (data == response.end() || !data->is_array()) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenAI models response did not contain a data array");
  }

  const auto* provider = find_provider_record(known, "openai");
  if (provider == nullptr) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "known catalog does not contain the OpenAI provider");
  }

  ModelCatalog discovered;
  discovered.providers.push_back(*provider);
  std::unordered_set<std::string> seen;
  for (const auto& item : *data) {
    if (!item.is_object() || !item.contains("id") ||
        !item.at("id").is_string()) {
      continue;
    }
    const auto id = trim_copy(item.at("id").get<std::string>());
    if (id.empty()) {
      continue;
    }
    if (const auto* existing = find_model_record(known, "openai", id)) {
      if (!seen.insert(lowercase(id)).second) continue;
      discovered.models.push_back(*existing);
      continue;
    }
    if (!is_chat_model_id(id) || !seen.insert(lowercase(id)).second) {
      continue;
    }

    ModelCatalogModelRecord record;
    record.ref = ModelRef{"openai", "openai-responses", id};
    record.display_name = id;
    record.capabilities.streaming = true;
    record.capabilities.tool_calls = true;
    record.metadata = provider->metadata;
    if (!record.metadata.is_object()) record.metadata = Json::object();
    record.metadata["input"] = Json::array({"text"});
    record.metadata["contextWindow"] = 128'000;
    record.metadata["maxTokens"] = 16'384;
    record.metadata["openaiDiscovery"] = true;
    discovered.models.push_back(std::move(record));
  }
  return foundation::Result<ModelCatalog>::success(std::move(discovered));
}

ModelCatalog replace_openai_models(
    const ModelCatalog& base, const ModelCatalog& previous_discovery,
    const ModelCatalog& refreshed_discovery) {
  ModelCatalog retained = base;
  retained.models.erase(
      std::remove_if(
          retained.models.begin(), retained.models.end(),
          [&](const ModelCatalogModelRecord& model) {
            const auto* previous = find_model_record(
                previous_discovery, model.ref.provider, model.ref.name);
            return previous != nullptr && is_discovered_record(*previous) &&
                   model.ref.provider == previous->ref.provider &&
                   model.ref.api == previous->ref.api &&
                   model.ref.name == previous->ref.name &&
                   model.display_name == previous->display_name &&
                   model.capabilities.streaming ==
                       previous->capabilities.streaming &&
                   model.capabilities.tool_calls == previous->capabilities.tool_calls &&
                   model.capabilities.vision == previous->capabilities.vision &&
                   model.capabilities.reasoning == previous->capabilities.reasoning &&
                   model.capabilities.session_caching ==
                       previous->capabilities.session_caching &&
                   model.metadata == previous->metadata;
          }),
      retained.models.end());
  ModelCatalog overlay = refreshed_discovery;
  overlay.models.erase(
      std::remove_if(overlay.models.begin(), overlay.models.end(),
                     [&](const ModelCatalogModelRecord& model) {
                       return !is_discovered_record(model) ||
                              find_model_record(retained, model.ref.provider,
                                                model.ref.name) != nullptr;
                     }),
      overlay.models.end());
  if (find_provider_record(retained, "openai") != nullptr) {
    overlay.providers.clear();
  }
  return merge_catalogs(retained, overlay);
}

foundation::Result<ModelCatalog> discover_openai_models(
    HttpTransport& transport, const ModelCatalog& known,
    const std::string& api_key, std::string base_url,
    const std::map<std::string, std::string>& headers) {
  if (api_key.empty()) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenAI models request requires an API key");
  }
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }

  HttpRequest request;
  request.method = "GET";
  request.url = base_url + "/models";
  request.timeout_ms = 10'000;
  request.headers = headers;
  request.headers["Accept"] = "application/json";
  request.headers["Authorization"] = "Bearer " + api_key;

  HttpResponse response;
  try {
    response = transport.send(request);
  } catch (...) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::io_error, "OpenAI models request failed");
  }
  if (response.status < 200 || response.status >= 300) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::io_error,
        "OpenAI models request failed with HTTP " +
            std::to_string(response.status));
  }

  Json body;
  try {
    body = Json::parse(response.body);
  } catch (...) {
    return foundation::Result<ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "invalid OpenAI models response");
  }
  return model_catalog_from_openai_response(body, known);
}

}  // namespace qt::ai
