#pragma once

#include <map>
#include <string>

#include "qt/ai/model_catalog.hpp"
#include "qt/ai/provider_transport.hpp"
#include "qt/foundation/error.hpp"

namespace qt::ai {

foundation::Result<ModelCatalog> model_catalog_from_openai_response(
    const Json& response, const ModelCatalog& known);

foundation::Result<ModelCatalog> discover_openai_models(
    HttpTransport& transport, const ModelCatalog& known,
    const std::string& api_key,
    std::string base_url = "https://api.openai.com/v1",
    const std::map<std::string, std::string>& headers = {});

ModelCatalog replace_openai_models(
    const ModelCatalog& base, const ModelCatalog& previous_discovery,
    const ModelCatalog& refreshed_discovery);

}  // namespace qt::ai
