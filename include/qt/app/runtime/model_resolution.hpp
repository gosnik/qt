#pragma once

#include <optional>
#include <string>

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/model_catalog.hpp"
#include "qt/app/config/settings.hpp"
#include "qt/foundation/error.hpp"

namespace qt::app {

enum class ModelResolutionSource {
  explicit_model,
  explicit_provider,
  settings_default,
  authenticated_fallback,
  catalog_default,
};

struct ModelResolutionRequest {
  AppSettings settings;
  ai::ModelCatalog catalog;
  ai::AuthStorage auth_storage;
  std::optional<std::string> provider;
  std::optional<std::string> model;
};

struct ResolvedModelSelection {
  ai::ModelCatalogProviderRecord provider;
  ai::ModelCatalogModelRecord model;
  std::optional<ai::ProviderAuthRecord> auth;
  std::string auth_storage_path;
  ModelResolutionSource source = ModelResolutionSource::catalog_default;
};

std::string default_auth_storage_path(const AppSettings& settings);

foundation::Result<ResolvedModelSelection> resolve_model_selection(
    const ModelResolutionRequest& request);

}  // namespace qt::app
