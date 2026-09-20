#include "qt/app/runtime/model_resolution.hpp"
#include "qt/ai/images.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

// models.cpp accepts data-URL image blocks during JSON decoding. The focused
// model-resolution probe does not exercise image parsing, so avoid linking the
// full image transport stack.
namespace qt::ai {
std::optional<ImageDataUrl> parse_image_data_url(const std::string&) {
  return std::nullopt;
}
}  // namespace qt::ai

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

qt::ai::AuthStorage authenticated_providers() {
  qt::ai::AuthStorage auth;
  qt::ai::ProviderAuthRecord codex;
  codex.provider = "openai-codex";
  codex.api_key = "codex-key";
  auth.providers.push_back(std::move(codex));
  qt::ai::ProviderAuthRecord openrouter;
  openrouter.provider = "openrouter";
  openrouter.api_key = "router-key";
  auth.providers.push_back(std::move(openrouter));
  return auth;
}

qt::app::ResolvedModelSelection resolve(
    std::optional<std::string> provider, std::optional<std::string> model,
    std::optional<std::string> default_provider = std::nullopt,
    std::optional<std::string> default_model = std::nullopt) {
  qt::app::ModelResolutionRequest request;
  request.catalog = qt::ai::default_model_catalog();
  request.auth_storage = authenticated_providers();
  request.provider = std::move(provider);
  request.model = std::move(model);
  request.settings.default_provider = std::move(default_provider);
  request.settings.default_model = std::move(default_model);
  const auto result = qt::app::resolve_model_selection(request);
  require(static_cast<bool>(result),
          result ? std::string{} : result.error().message);
  return result.value();
}

void require_openrouter(const qt::app::ResolvedModelSelection& selection,
                        const std::string& model) {
  require(selection.provider.descriptor.id == "openrouter",
          "OpenRouter selection changed provider");
  require(selection.model.ref.provider == "openrouter" &&
              selection.model.ref.name == model,
          "OpenRouter slash-delimited model ID was not preserved");
  require(selection.model.metadata.value("contextWindow", 0) > 0,
          "OpenRouter selection lost its context window");
  require(selection.model.metadata.value("maxTokens", 0) > 0,
          "OpenRouter selection lost its output limit");
}

}  // namespace

int main() {
  try {
    const auto persisted = resolve(std::nullopt, std::nullopt, "openrouter",
                                   "qwen/qwen3.5-9b");
    require_openrouter(persisted, "qwen/qwen3.5-9b");
    require(persisted.source == qt::app::ModelResolutionSource::settings_default,
            "persisted OpenRouter selection used authenticated fallback");

    require_openrouter(resolve("openrouter", "qwen/qwen3.5-9b"),
                       "qwen/qwen3.5-9b");
    require_openrouter(resolve("openrouter", "anthropic/claude-sonnet-4.6"),
                       "anthropic/claude-sonnet-4.6");
    require_openrouter(resolve("openrouter", "openrouter/qwen/qwen3.5-9b"),
                       "qwen/qwen3.5-9b");
    require_openrouter(resolve(std::nullopt,
                               "openrouter/qwen/qwen3.5-9b"),
                       "qwen/qwen3.5-9b");

    std::cout << "model resolution probe passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "model resolution probe failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
