#include "qt/app/runtime/openai_model_refresh.hpp"

#include "qt/ai/openai_model_discovery.hpp"
#include "qt/ai/provider_transport.hpp"
#include "qt/app/runtime/model_resolution.hpp"

#include <cstdlib>

namespace qt::app {

foundation::Result<ai::ModelCatalog> refresh_openai_models(
    const ai::ModelCatalog& catalog, const ai::AuthStorage& auth_storage,
    const std::optional<std::string>& runtime_api_key) {
  const char* offline = std::getenv("PI_OFFLINE");
  if (offline != nullptr && (std::string(offline) == "1" ||
                            std::string(offline) == "true" ||
                            std::string(offline) == "yes")) {
    return foundation::Result<ai::ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenAI model discovery is unavailable in offline mode");
  }
  ModelResolutionRequest request;
  request.catalog = catalog;
  request.auth_storage = auth_storage;
  request.provider = "openai";
  const auto selection = resolve_model_selection(request);
  if (!selection) {
    return selection.error();
  }

  ai::ProviderCredentialResolutionRequest credential_request;
  credential_request.provider = "openai";
  credential_request.auth_storage = auth_storage;
  credential_request.auth_record = selection.value().auth;
  credential_request.runtime_api_key = runtime_api_key;
  const auto credential = ai::resolve_provider_credential(credential_request);
  if (!credential.secret || credential.secret->empty()) {
    return foundation::Result<ai::ModelCatalog>::failure(
        foundation::ErrorCode::invalid_argument,
        "OpenAI model discovery requires an OpenAI API key");
  }

  const auto& metadata = selection.value().provider.metadata;
  const auto base_url = metadata.value("baseUrl", std::string("https://api.openai.com/v1"));
  std::map<std::string, std::string> headers;
  if (metadata.contains("headers") && metadata.at("headers").is_object()) {
    for (const auto& item : metadata.at("headers").items()) {
      if (item.value().is_string()) {
        headers.emplace(item.key(), item.value().get<std::string>());
      }
    }
  }
  ai::BeastHttpTransport transport;
  return ai::discover_openai_models(transport, catalog, *credential.secret,
                                    base_url, headers);
}

}  // namespace qt::app
