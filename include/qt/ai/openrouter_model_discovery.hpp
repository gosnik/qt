#pragma once

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/model_catalog.hpp"
#include "qt/foundation/error.hpp"

namespace qt::ai {

struct OpenRouterCredits {
  double total_credits = 0.0;
  double total_usage = 0.0;
};

foundation::Result<ModelCatalog> discover_openrouter_models(
    const AuthStorage& auth_storage, bool allow_cached_fallback = true);
// Converts the OpenRouter GET /api/v1/models response into the normalized
// native catalog. Kept separate from network discovery so provider metadata
// normalization can be verified without making a live request.
foundation::Result<ModelCatalog> model_catalog_from_openrouter_response(
    const Json& response);
// Startup-safe cache read. This never performs network I/O and returns an
// empty catalog when no cache is available. Stale entries remain useful as
// last-known metadata and cannot delay startup.
ModelCatalog cached_openrouter_models();
// Replaces the previously discovered OpenRouter slice while preserving models
// contributed by built-ins, configuration, and extensions.
ModelCatalog replace_openrouter_models(
    const ModelCatalog& base, const ModelCatalog& previous_discovery,
    const ModelCatalog& refreshed_discovery);
foundation::Result<OpenRouterCredits> fetch_openrouter_credits(
    const std::string& api_key);

}  // namespace qt::ai
