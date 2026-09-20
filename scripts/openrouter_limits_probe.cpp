#include "qt/ai/openrouter_model_discovery.hpp"
#include "qt/ai/provider_transport.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

qt::ai::CompletionRequest request_with_limit(std::int64_t max_tokens) {
  qt::ai::CompletionRequest request;
  request.model =
      qt::ai::ModelRef{"openrouter", "openai-completions", "qwen/qwen3.5-9b"};
  request.max_output_tokens = max_tokens;
  return request;
}

qt::ai::Json openrouter_options() {
  return qt::ai::Json{{"provider", "openrouter"},
                      {"model", "qwen/qwen3.5-9b"},
                      {"contextWindow", 262144},
                      {"maxTokens", 81920},
                      {"baseUrl", "https://openrouter.ai/api/v1"}};
}

void verify_request_shaping() {
  const auto capped = qt::ai::openai_completions_request_body(
      request_with_limit(32000), true, openrouter_options());
  require(capped.at("model") == "qwen/qwen3.5-9b",
          "OpenRouter slash-delimited model id was changed");
  require(capped.at("max_completion_tokens") == 32000,
          "OpenRouter safe default limit used the wrong request field or value");
  require(!capped.contains("max_tokens"),
          "OpenRouter emitted both max-token request fields");

  const auto lower = qt::ai::openai_completions_request_body(
      request_with_limit(4096), true, openrouter_options());
  require(lower.at("max_completion_tokens") == 4096,
          "an explicit lower model limit was not preserved");

  auto legacy_options = openrouter_options();
  legacy_options["compat"] = qt::ai::Json{{"maxTokensField", "max_tokens"}};
  const auto legacy = qt::ai::openai_completions_request_body(
      request_with_limit(4096), true, legacy_options);
  require(legacy.at("max_tokens") == 4096 &&
              !legacy.contains("max_completion_tokens"),
          "maxTokensField compatibility override was ignored");

  auto near_full = request_with_limit(4096);
  qt::ai::Message user;
  user.role = qt::ai::MessageRole::user;
  user.content = {qt::ai::TextContent{std::string(240, 'x'), std::nullopt}};
  near_full.messages.push_back(std::move(user));
  auto small_context_options = openrouter_options();
  small_context_options["contextWindow"] = 100;
  small_context_options["maxTokens"] = 4096;
  const auto near_full_body = qt::ai::openai_completions_request_body(
      near_full, true, small_context_options);
  const auto bounded =
      near_full_body.at("max_completion_tokens").get<std::int64_t>();
  const auto estimated_input = qt::ai::estimate_token_count(near_full);
  const auto remaining = std::max<std::int64_t>(1, 100 - estimated_input);
  require(bounded > 0 && bounded < 4096 && bounded <= remaining,
          "near-full context did not clamp output below the requested limit");
}

void verify_metadata_propagation() {
  qt::ai::ModelCatalogModelRecord model;
  model.ref =
      qt::ai::ModelRef{"openrouter", "openai-completions", "qwen/qwen3.5-9b"};
  model.display_name = "Qwen: Qwen3.5-9B";
  model.metadata = openrouter_options();

  const auto options = qt::ai::provider_options_from_model_metadata(model);
  require(options.metadata.at("model") == "qwen/qwen3.5-9b",
          "provider options changed the slash-delimited model id");
  require(options.metadata.at("contextWindow") == 262144,
          "provider options dropped the context window");
  require(options.metadata.at("maxTokens") == 81920,
          "provider options dropped the model output limit");
}

void verify_response_normalization() {
  const qt::ai::Json response = {
      {"data",
       qt::ai::Json::array(
           {{{"id", "valid/limited"},
             {"name", "Valid limited model"},
             {"context_length", 262144},
             {"top_provider",
              qt::ai::Json{{"context_length", 131072},
                           {"max_completion_tokens", 81920}}}},
            {{"id", "valid/defaulted"}},
            {{"id", "valid/malformed-limits"},
             {"context_length", "not-a-number"},
             {"top_provider",
              qt::ai::Json{{"context_length", 0},
                           {"max_completion_tokens", 7.5}}}},
            {{"id", ""}, {"context_length", 99999}},
            {{"id", 42}, {"context_length", 99999}},
            qt::ai::Json::array()})}};
  const auto parsed =
      qt::ai::model_catalog_from_openrouter_response(response);
  require(parsed.ok(), "valid OpenRouter response was rejected");
  require(parsed.value().models.size() == 3U,
          "malformed OpenRouter model entries were not skipped");

  const auto* limited = qt::ai::find_model_record(
      parsed.value(), "openrouter", "valid/limited");
  require(limited != nullptr, "valid OpenRouter model was dropped");
  require(limited->metadata.at("modelContextWindow") == 262144 &&
              limited->metadata.at("providerContextWindow") == 131072 &&
              limited->metadata.at("contextWindow") == 131072 &&
              limited->metadata.at("maxTokens") == 81920,
          "provider limits were not bounded by the model context window");

  for (const char* id : {"valid/defaulted", "valid/malformed-limits"}) {
    const auto* model =
        qt::ai::find_model_record(parsed.value(), "openrouter", id);
    require(model != nullptr, std::string("OpenRouter model was dropped: ") + id);
    require(model->metadata.at("contextWindow") == 4096 &&
                model->metadata.at("maxTokens") == 4096,
            std::string("safe limit defaults were not applied: ") + id);
  }

  require(!qt::ai::model_catalog_from_openrouter_response(qt::ai::Json::array()),
          "non-object OpenRouter response was accepted");
  require(!qt::ai::model_catalog_from_openrouter_response(
              qt::ai::Json{{"data", qt::ai::Json::object()}}),
          "OpenRouter response with non-array data was accepted");
}

void write_cache_fixture(const std::filesystem::path& home,
                         std::int64_t timestamp) {
  const auto directory = home / ".qt";
  std::filesystem::create_directories(directory);
  const qt::ai::Json fixture = {
      {"timestamp", timestamp},
      {"models",
       qt::ai::Json::array(
           {{{"id", "qwen/qwen3.5-9b"},
             {"display_name", "Qwen: Qwen3.5-9B"},
             {"reasoning", true},
             {"vision", true},
             {"metadata", openrouter_options()}},
            {{"id", "broken/missing-limits"},
             {"display_name", "Missing limits"},
             {"metadata", qt::ai::Json::object()}},
            {{"id", "broken/nonpositive-limits"},
             {"display_name", "Nonpositive limits"},
             {"metadata",
              qt::ai::Json{{"contextWindow", 0}, {"maxTokens", -1}}}}})}};

  std::ofstream output(directory / "openrouter_models.json");
  require(static_cast<bool>(output), "unable to create OpenRouter cache fixture");
  output << fixture.dump(2);
  require(static_cast<bool>(output), "unable to write OpenRouter cache fixture");
}

void verify_cache_loading(const std::filesystem::path& home) {
  require(::unsetenv("QT_CODING_AGENT_DIR") == 0,
          "unable to clear configured agent home for cache probe");
  require(::setenv("HOME", home.c_str(), 1) == 0,
          "unable to isolate HOME for cache probe");
  require(::setenv("PI_OFFLINE", "1", 1) == 0,
          "unable to force offline discovery");
  write_cache_fixture(home, 1);

  const auto cached = qt::ai::cached_openrouter_models();
  const auto* qwen =
      qt::ai::find_model_record(cached, "openrouter", "qwen/qwen3.5-9b");
  require(qwen != nullptr, "stale OpenRouter cache entry was not restored");
  require(qwen->metadata.at("contextWindow") == 262144 &&
              qwen->metadata.at("maxTokens") == 81920,
          "OpenRouter cache did not preserve token metadata");

  for (const auto& model : cached.models) {
    require(model.metadata.is_object() &&
                model.metadata.value("contextWindow", 0) > 0 &&
                model.metadata.value("maxTokens", 0) > 0,
            "cache loader returned a model with missing or nonpositive limits");
  }

  write_cache_fixture(home, static_cast<std::int64_t>(std::time(nullptr)));
  const auto offline =
      qt::ai::discover_openrouter_models(qt::ai::AuthStorage{});
  require(offline.ok(), "offline OpenRouter discovery rejected a valid cache");
  require(qt::ai::find_model_record(offline.value(), "openrouter",
                                    "qwen/qwen3.5-9b") != nullptr,
          "offline discovery did not use the OpenRouter cache");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "usage: openrouter_limits_probe <isolated-home>");
    verify_request_shaping();
    verify_metadata_propagation();
    verify_response_normalization();
    verify_cache_loading(std::filesystem::path(argv[1]));
    std::cout << "openrouter limits C++ probe passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "openrouter limits C++ probe failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
