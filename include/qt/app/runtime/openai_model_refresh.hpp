#pragma once

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/model_catalog.hpp"
#include "qt/foundation/error.hpp"

namespace qt::app {

// Performs network I/O. Interactive callers must run this off the UI thread.
foundation::Result<ai::ModelCatalog> refresh_openai_models(
    const ai::ModelCatalog& catalog, const ai::AuthStorage& auth_storage,
    const std::optional<std::string>& runtime_api_key = std::nullopt);

}  // namespace qt::app
