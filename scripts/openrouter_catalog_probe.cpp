#include "qt/ai/openrouter_model_discovery.hpp"
#include "qt/ai/provider_transport.hpp"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace qt::ai {

HttpResponse BeastHttpTransport::send(const HttpRequest&) {
  throw std::runtime_error("unexpected live HTTP request in OpenRouter probe");
}

std::optional<std::string> get_stored_auth_secret(const AuthStorage&,
                                                  const std::string&) {
  return std::nullopt;
}

std::optional<std::string> get_env_api_key(const std::string&) {
  return std::nullopt;
}

}  // namespace qt::ai

namespace {

using qt::ai::Json;

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

qt::ai::ModelCatalogModelRecord require_model(
    const qt::ai::ModelCatalog& catalog, const std::string& id) {
  const auto* model = qt::ai::find_model_record(catalog, "openrouter", id);
  require(model != nullptr, "missing OpenRouter model: " + id);
  return *model;
}

std::int64_t token_metadata(const qt::ai::ModelCatalogModelRecord& model,
                            const char* key) {
  require(model.metadata.contains(key) &&
              model.metadata.at(key).is_number_integer(),
          std::string("missing integer metadata: ") + key);
  return model.metadata.at(key).get<std::int64_t>();
}

void verify_response_normalization() {
  const Json response = {
      {"data",
       Json::array(
           {{{"id", "probe/conservative"},
             {"name", "Conservative"},
             {"context_length", 100000},
             {"architecture", {{"output_modalities", {"text"}}}},
             {"top_provider",
              {{"context_length", 64000},
               {"max_completion_tokens", 80000}}}},
            {{"id", "probe/defaults"},
             {"top_provider", Json::object()}},
            {{"id", "probe/missing-max"},
             {"context_length", 128000},
             {"architecture", {{"output_modalities", {"text"}}}},
             {"top_provider", Json::object()}},
            {{"id", "probe/invalid"},
             {"context_length", -1},
             {"architecture", {{"output_modalities", {"text"}}}},
             {"top_provider",
              {{"context_length", "64000"},
               {"max_completion_tokens", 5000.5}}}},
            {{"id", "probe/image-only"},
             {"architecture", {{"output_modalities", {"image"}}}}}})}};

  const auto parsed = qt::ai::model_catalog_from_openrouter_response(response);
  require(static_cast<bool>(parsed),
          parsed ? std::string{} : parsed.error().message);

  const auto conservative = require_model(parsed.value(), "probe/conservative");
  require(token_metadata(conservative, "modelContextWindow") == 100000,
          "model context was not retained");
  require(token_metadata(conservative, "providerContextWindow") == 64000,
          "provider context was not retained");
  require(token_metadata(conservative, "contextWindow") == 64000,
          "effective context was not conservative");
  require(token_metadata(conservative, "maxTokens") == 64000,
          "completion limit was not clamped to effective context");

  const auto defaults = require_model(parsed.value(), "probe/defaults");
  require(token_metadata(defaults, "contextWindow") == 4096 &&
              token_metadata(defaults, "maxTokens") == 4096,
          "reference 4096 token defaults were not applied");

  const auto missing_max = require_model(parsed.value(), "probe/missing-max");
  require(token_metadata(missing_max, "contextWindow") == 128000 &&
              token_metadata(missing_max, "maxTokens") == 4096,
          "missing completion limit did not use its independent default");

  const auto invalid = require_model(parsed.value(), "probe/invalid");
  require(token_metadata(invalid, "contextWindow") == 4096 &&
              token_metadata(invalid, "maxTokens") == 4096,
          "invalid token metadata escaped positive normalization");
  require(qt::ai::find_model_record(parsed.value(), "openrouter",
                                    "probe/image-only") == nullptr,
          "non-text output model was not filtered");

  const auto malformed = qt::ai::model_catalog_from_openrouter_response(
      Json{{"data", Json::object()}});
  require(!malformed, "malformed data collection was accepted");
}

void verify_cache_normalization(const std::filesystem::path& agent_home) {
  std::filesystem::create_directories(agent_home);
  const Json cache = {
      {"timestamp", static_cast<std::int64_t>(std::time(nullptr))},
      {"models",
       Json::array(
           {{{"id", "probe/cache-clamp"},
             {"display_name", "Cache clamp"},
             {"reasoning", true},
             {"vision", false},
             {"metadata",
              {{"contextWindow", 32768}, {"maxTokens", 99999}}}},
            {{"id", "probe/cache-provider"},
             {"metadata",
              {{"contextWindow", 100000},
               {"modelContextWindow", 100000},
               {"providerContextWindow", 48000},
               {"maxTokens", 64000}}}},
            {{"id", "probe/cache-invalid"},
             {"metadata",
              {{"contextWindow", -1}, {"maxTokens", "32000"}}}},
            {{"id", 7}, {"metadata", Json::object()}}})}};
  const auto path = agent_home / "openrouter_models.json";
  std::ofstream output(path);
  require(static_cast<bool>(output), "unable to write OpenRouter cache fixture");
  output << cache.dump(2);
  output.close();

  require(::setenv("QT_CODING_AGENT_DIR", agent_home.c_str(), 1) == 0,
          "unable to set probe agent home");
  const auto catalog = qt::ai::cached_openrouter_models();
  require(catalog.models.size() == 3,
          "cache loader did not isolate malformed records");

  const auto clamped = require_model(catalog, "probe/cache-clamp");
  require(token_metadata(clamped, "contextWindow") == 32768 &&
              token_metadata(clamped, "maxTokens") == 32768,
          "cached maxTokens was not clamped to contextWindow");
  require(clamped.metadata.value("baseUrl", std::string{}) ==
              "https://openrouter.ai/api/v1",
          "cached metadata did not restore the OpenRouter base URL");

  const auto provider = require_model(catalog, "probe/cache-provider");
  require(token_metadata(provider, "contextWindow") == 48000 &&
              token_metadata(provider, "maxTokens") == 48000,
          "cached provider context was not reconciled conservatively");

  const auto invalid = require_model(catalog, "probe/cache-invalid");
  require(token_metadata(invalid, "contextWindow") == 4096 &&
              token_metadata(invalid, "maxTokens") == 4096,
          "invalid cached token metadata was not repaired");

  require(::unsetenv("PI_OFFLINE") == 0,
          "unable to enable online discovery behavior");
  const auto explicit_refresh =
      qt::ai::discover_openrouter_models(qt::ai::AuthStorage{}, false);
  require(!explicit_refresh,
          "explicit refresh concealed a network failure with cached data");
  const auto fallback_refresh =
      qt::ai::discover_openrouter_models(qt::ai::AuthStorage{});
  require(fallback_refresh && fallback_refresh.value().models.size() == 3,
          "default discovery did not retain cache fallback behavior");
}

void verify_discovery_replacement() {
  const auto previous = qt::ai::model_catalog_from_openrouter_response(
      Json{{"data", Json::array({Json{{"id", "probe/removed"}},
                                 Json{{"id", "probe/overridden"}}})}});
  const auto refreshed = qt::ai::model_catalog_from_openrouter_response(
      Json{{"data", Json::array({Json{{"id", "probe/added"}}})}});
  require(previous && refreshed, "unable to construct replacement fixtures");

  auto base = qt::ai::merge_catalogs(qt::ai::default_model_catalog(),
                                     previous.value());
  qt::ai::ModelCatalog configured;
  configured.providers = previous.value().providers;
  auto retained = previous.value().models.front();
  retained.ref.name = "probe/configured";
  configured.models.push_back(std::move(retained));
  auto overridden = require_model(previous.value(), "probe/overridden");
  overridden.display_name = "Extension override";
  configured.models.push_back(std::move(overridden));
  base = qt::ai::merge_catalogs(base, configured);

  const auto replaced = qt::ai::replace_openrouter_models(
      base, previous.value(), refreshed.value());
  require(qt::ai::find_model_record(replaced, "openrouter",
                                    "probe/removed") == nullptr,
          "removed discovery model survived refresh");
  require(qt::ai::find_model_record(replaced, "openrouter", "probe/added") !=
              nullptr,
          "new discovery model was not installed");
  require(qt::ai::find_model_record(replaced, "openrouter",
                                    "probe/configured") != nullptr,
          "configured OpenRouter model was removed during refresh");
  const auto extension = require_model(replaced, "probe/overridden");
  require(extension.display_name == "Extension override",
          "extension OpenRouter override was removed during refresh");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: openrouter_catalog_probe <temporary-agent-home>");
    verify_response_normalization();
    verify_cache_normalization(argv[1]);
    verify_discovery_replacement();
    std::cout << "OpenRouter catalog probe passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "OpenRouter catalog probe failed: " << error.what() << '\n';
    return 1;
  }
}
