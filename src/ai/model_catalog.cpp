#include "qt/ai/model_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <tuple>
#include <utility>

namespace qt::ai {
namespace {

std::string trim_copy(const std::string& value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
      }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

std::string lowercase_copy(const std::string& value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

Json normalized_object(const Json& value) {
  return value.is_object() ? value : Json::object();
}

std::string provider_key(const std::string& provider_id) {
  return lowercase_copy(trim_copy(provider_id));
}

std::string model_key(const std::string& provider_id, const std::string& model_id) {
  return provider_key(provider_id) + "\n" + lowercase_copy(trim_copy(model_id));
}

Json cost_metadata(double input, double output, double cache_read,
                   double cache_write) {
  return Json{{"input", input},
              {"output", output},
              {"cacheRead", cache_read},
              {"cacheWrite", cache_write},
              {"input_usd_per_million", input},
              {"output_usd_per_million", output},
              {"cache_read_usd_per_million", cache_read},
              {"cache_write_usd_per_million", cache_write}};
}

ModelCatalogProviderRecord provider_record(std::string id, std::string api,
                                           std::string display_name,
                                           std::string base_url,
                                           const std::vector<std::string>&
                                               accepted_apis = {}) {
  ModelCatalogProviderRecord record;
  record.descriptor.id = std::move(id);
  record.descriptor.api = std::move(api);
  record.descriptor.display_name = std::move(display_name);
  record.metadata = Json{{"baseUrl", std::move(base_url)}};
  if (!accepted_apis.empty()) {
    record.metadata["acceptedApis"] = accepted_apis;
  }
  return record;
}

ModelCatalogProviderRecord unsupported_provider_record(std::string id,
                                                       std::string display_name,
                                                       std::string reason) {
  ModelCatalogProviderRecord record;
  record.descriptor.id = std::move(id);
  record.descriptor.api = "unsupported";
  record.descriptor.display_name = std::move(display_name);
  record.metadata = Json{{"unsupported", true},
                         {"unsupportedReason", std::move(reason)}};
  return record;
}

ModelCatalogModelRecord model_record(std::string provider, std::string api,
                                     std::string id, std::string name,
                                     bool reasoning,
                                     std::vector<std::string> input,
                                     std::int64_t context_window,
                                     std::int64_t max_tokens, Json cost,
                                     Json thinking_level_map = Json::object(),
                                     Json compat = Json::object(),
                                     std::string base_url = {},
                                     Json extra_metadata = Json::object()) {
  ModelCatalogModelRecord record;
  record.ref = ModelRef{std::move(provider), std::move(api), std::move(id)};
  record.display_name = std::move(name);
  record.capabilities.reasoning = reasoning;
  record.capabilities.vision =
      std::find(input.begin(), input.end(), "image") != input.end();
  record.metadata = Json{{"input", std::move(input)},
                         {"cost", std::move(cost)},
                         {"contextWindow", context_window},
                         {"maxTokens", max_tokens}};
  if (!thinking_level_map.empty()) {
    record.metadata["thinkingLevelMap"] = std::move(thinking_level_map);
  }
  if (!compat.empty()) {
    record.metadata["compat"] = std::move(compat);
  }
  if (!base_url.empty()) {
    record.metadata["baseUrl"] = std::move(base_url);
  }
  if (extra_metadata.is_object()) {
    for (auto it = extra_metadata.begin(); it != extra_metadata.end(); ++it) {
      record.metadata[it.key()] = it.value();
    }
  }
  return record;
}

bool provider_accepts_api(const ModelCatalogProviderRecord& provider,
                          const std::string& api) {
  if (provider_key(provider.descriptor.id) == "openrouter") {
    return !api.empty();
  }
  if (provider.descriptor.api == api) {
    return true;
  }
  if (!provider.metadata.is_object() || !provider.metadata.contains("acceptedApis") ||
      !provider.metadata.at("acceptedApis").is_array()) {
    return false;
  }
  for (const auto& accepted_api : provider.metadata.at("acceptedApis")) {
    if (accepted_api.is_string() && accepted_api.get<std::string>() == api) {
      return true;
    }
  }
  return false;
}

std::optional<double> metadata_number(const Json& object,
                                      const char* primary_key,
                                      const char* fallback_key) {
  if (!object.is_object()) {
    return std::nullopt;
  }
  if (object.contains(primary_key) && object.at(primary_key).is_number()) {
    return object.at(primary_key).get<double>();
  }
  if (object.contains(fallback_key) && object.at(fallback_key).is_number()) {
    return object.at(fallback_key).get<double>();
  }
  return std::nullopt;
}

template <typename T>
void append_messages(std::vector<std::string>& into, const T& more) {
  into.insert(into.end(), more.begin(), more.end());
}

std::optional<std::string> optional_string_field(const Json& object,
                                                 const char* key) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_string()) {
    return std::nullopt;
  }
  std::string value = trim_copy(object.at(key).get<std::string>());
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

bool optional_bool_field(const Json& object, const char* key,
                         bool fallback = false) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_boolean()) {
    return fallback;
  }
  return object.at(key).get<bool>();
}

std::optional<std::int64_t> positive_int_field(const Json& object,
                                               const char* key) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_number_integer()) {
    return std::nullopt;
  }
  const auto value = object.at(key).get<std::int64_t>();
  if (value <= 0) {
    return std::nullopt;
  }
  return value;
}

Json string_array_field(const Json& object, const char* key,
                        Json fallback = Json::array({"text"})) {
  if (!object.is_object() || !object.contains(key) ||
      !object.at(key).is_array()) {
    return fallback;
  }
  Json result = Json::array();
  for (const auto& item : object.at(key)) {
    if (item.is_string()) {
      result.push_back(item.get<std::string>());
    }
  }
  return result.empty() ? std::move(fallback) : result;
}

std::vector<std::string> strings_from_json_array(const Json& array) {
  std::vector<std::string> result;
  if (!array.is_array()) {
    return result;
  }
  result.reserve(array.size());
  for (const auto& item : array) {
    if (item.is_string()) {
      result.push_back(item.get<std::string>());
    }
  }
  return result;
}

Json merge_json_objects(Json base, const Json& overlay) {
  if (!base.is_object()) {
    base = Json::object();
  }
  if (!overlay.is_object()) {
    return base;
  }
  for (auto it = overlay.begin(); it != overlay.end(); ++it) {
    base[it.key()] = it.value();
  }
  return base;
}

Json merge_cost_metadata(Json base, const Json& override) {
  if (!base.is_object()) {
    base = Json{{"input", 0}, {"output", 0}, {"cacheRead", 0}, {"cacheWrite", 0}};
  }
  if (!override.is_object()) {
    return base;
  }
  for (const auto* field : {"input", "output", "cacheRead", "cacheWrite",
                            "input_usd_per_million",
                            "output_usd_per_million",
                            "cache_read_usd_per_million",
                            "cache_write_usd_per_million"}) {
    if (override.contains(field) && override.at(field).is_number()) {
      base[field] = override.at(field);
    }
  }
  return base;
}

void apply_model_override(ModelCatalogModelRecord& model,
                          const Json& override) {
  if (!override.is_object()) {
    return;
  }
  if (auto name = optional_string_field(override, "name")) {
    model.display_name = *name;
  }
  if (override.contains("reasoning") && override.at("reasoning").is_boolean()) {
    model.capabilities.reasoning = override.at("reasoning").get<bool>();
  }
  if (override.contains("thinkingLevelMap") &&
      override.at("thinkingLevelMap").is_object()) {
    model.metadata["thinkingLevelMap"] = merge_json_objects(
        model.metadata.value("thinkingLevelMap", Json::object()),
        override.at("thinkingLevelMap"));
  }
  if (override.contains("input") && override.at("input").is_array()) {
    model.metadata["input"] = string_array_field(override, "input");
    const auto input = strings_from_json_array(model.metadata.at("input"));
    model.capabilities.vision =
        std::find(input.begin(), input.end(), "image") != input.end();
  }
  if (const auto context_window = positive_int_field(override, "contextWindow")) {
    model.metadata["contextWindow"] = *context_window;
  }
  if (const auto max_tokens = positive_int_field(override, "maxTokens")) {
    model.metadata["maxTokens"] = *max_tokens;
  }
  if (override.contains("cost") && override.at("cost").is_object()) {
    model.metadata["cost"] =
        merge_cost_metadata(model.metadata.value("cost", Json::object()),
                            override.at("cost"));
  }
  if (override.contains("headers") && override.at("headers").is_object()) {
    model.metadata["headers"] = merge_json_objects(
        model.metadata.value("headers", Json::object()), override.at("headers"));
  }
  if (override.contains("compat") && override.at("compat").is_object()) {
    model.metadata["compat"] = merge_json_objects(
        model.metadata.value("compat", Json::object()), override.at("compat"));
  }
}

}  // namespace

ModelCatalogProviderRecord normalized(const ModelCatalogProviderRecord& record) {
  ModelCatalogProviderRecord result = record;
  result.descriptor.id = trim_copy(record.descriptor.id);
  result.descriptor.display_name = trim_copy(record.descriptor.display_name);
  result.descriptor.api = trim_copy(record.descriptor.api);
  result.metadata = normalized_object(record.metadata);
  return result;
}

ModelCatalogModelRecord normalized(const ModelCatalogModelRecord& record) {
  ModelCatalogModelRecord result = record;
  result.ref.provider = trim_copy(record.ref.provider);
  result.ref.api = trim_copy(record.ref.api);
  result.ref.name = trim_copy(record.ref.name);
  result.display_name = trim_copy(record.display_name);
  result.metadata = normalized_object(record.metadata);
  return result;
}

ImageModelCatalogRecord normalized(const ImageModelCatalogRecord& record) {
  ImageModelCatalogRecord result = record;
  result.provider = trim_copy(record.provider);
  result.api = trim_copy(record.api);
  result.id = trim_copy(record.id);
  result.display_name = trim_copy(record.display_name);
  result.base_url = trim_copy(record.base_url);
  result.metadata = normalized_object(record.metadata);
  return result;
}

ModelCatalog normalized(const ModelCatalog& catalog) {
  ModelCatalog result;
  result.providers.reserve(catalog.providers.size());
  result.models.reserve(catalog.models.size());

  for (const auto& provider : catalog.providers) {
    result.providers.push_back(normalized(provider));
  }
  for (const auto& model : catalog.models) {
    result.models.push_back(normalized(model));
  }

  std::sort(result.providers.begin(), result.providers.end(),
            [](const ModelCatalogProviderRecord& lhs,
               const ModelCatalogProviderRecord& rhs) {
              return std::make_tuple(provider_key(lhs.descriptor.id), lhs.descriptor.id,
                                     lhs.descriptor.display_name, lhs.descriptor.api) <
                     std::make_tuple(provider_key(rhs.descriptor.id), rhs.descriptor.id,
                                     rhs.descriptor.display_name, rhs.descriptor.api);
            });

  std::sort(result.models.begin(), result.models.end(),
            [](const ModelCatalogModelRecord& lhs,
               const ModelCatalogModelRecord& rhs) {
              return std::make_tuple(model_key(lhs.ref.provider, lhs.ref.name),
                                     provider_key(lhs.ref.provider), lhs.ref.provider,
                                     lowercase_copy(lhs.ref.name), lhs.ref.name,
                                     lhs.ref.api, lhs.display_name) <
                     std::make_tuple(model_key(rhs.ref.provider, rhs.ref.name),
                                     provider_key(rhs.ref.provider), rhs.ref.provider,
                                     lowercase_copy(rhs.ref.name), rhs.ref.name,
                                     rhs.ref.api, rhs.display_name);
            });

  return result;
}

ModelCatalog default_model_catalog() {
  ModelCatalog catalog;
  catalog.providers = {
      provider_record("anthropic", "anthropic-messages", "Anthropic",
                      "https://api.anthropic.com"),
      provider_record("amazon-bedrock", "bedrock-converse-stream", "Amazon Bedrock",
                      "https://bedrock-runtime.us-east-1.amazonaws.com"),
      unsupported_provider_record(
          "azure-openai-responses", "Azure OpenAI Responses",
          "Azure OpenAI Responses provider transport is not implemented yet."),
      unsupported_provider_record(
          "cerebras", "Cerebras",
          "Cerebras provider transport is not implemented yet."),
      provider_record("cloudflare-workers-ai", "openai-completions",
                      "Cloudflare Workers AI",
                      "https://api.cloudflare.com/client/v4/accounts/{CLOUDFLARE_ACCOUNT_ID}/ai/v1"),
      provider_record("cloudflare-ai-gateway", "openai-completions",
                      "Cloudflare AI Gateway",
                      "https://gateway.ai.cloudflare.com/v1/{CLOUDFLARE_ACCOUNT_ID}/{CLOUDFLARE_GATEWAY_ID}/compat",
                      {"anthropic-messages", "openai-completions",
                       "openai-responses"}),
      unsupported_provider_record(
          "deepseek", "DeepSeek",
          "DeepSeek provider transport is not implemented yet."),
      provider_record("fireworks", "anthropic-messages", "Fireworks",
                      "https://api.fireworks.ai/inference"),
      provider_record("github-copilot", "openai-responses", "GitHub Copilot",
                      "https://api.individual.githubcopilot.com",
                      {"anthropic-messages", "openai-completions",
                       "openai-responses"}),
      provider_record("google", "google-generative-ai", "Google",
                      "https://generativelanguage.googleapis.com"),
      provider_record("google-vertex", "google-vertex", "Google Vertex",
                      "https://{location}-aiplatform.googleapis.com"),
      provider_record("groq", "openai-completions", "Groq",
                      "https://api.groq.com/openai/v1"),
      unsupported_provider_record(
          "huggingface", "Hugging Face",
          "Hugging Face provider transport is not implemented yet."),
      unsupported_provider_record(
          "kimi-coding", "Kimi Coding",
          "Kimi Coding provider transport is not implemented yet."),
      unsupported_provider_record(
          "minimax", "MiniMax",
          "MiniMax provider transport is not implemented yet."),
      unsupported_provider_record(
          "minimax-cn", "MiniMax CN",
          "MiniMax CN provider transport is not implemented yet."),
      provider_record("mistral", "mistral-conversations", "Mistral",
                      "https://api.mistral.ai"),
      unsupported_provider_record(
          "moonshotai", "Moonshot AI",
          "Moonshot AI provider transport is not implemented yet."),
      unsupported_provider_record(
          "moonshotai-cn", "Moonshot AI CN",
          "Moonshot AI CN provider transport is not implemented yet."),
      provider_record("openai", "openai-responses", "OpenAI",
                      "https://api.openai.com/v1"),
      provider_record("openai-codex", "openai-codex-responses", "OpenAI Codex",
                      "https://chatgpt.com/backend-api"),
      provider_record("opencode", "openai-completions", "OpenCode Zen",
                      "https://opencode.ai/zen/v1",
                      {"anthropic-messages", "google-generative-ai",
                       "openai-completions", "openai-responses"}),
      provider_record("opencode-go", "openai-completions", "OpenCode Go",
                      "https://opencode.ai/zen/go/v1"),
      provider_record("openrouter", "openai-completions", "OpenRouter",
                      "https://openrouter.ai/api/v1"),
      provider_record("together", "openai-completions", "Together",
                      "https://api.together.ai/v1"),
      unsupported_provider_record(
          "vercel-ai-gateway", "Vercel AI Gateway",
          "Vercel AI Gateway provider transport is not implemented yet."),
      unsupported_provider_record(
          "xai", "xAI",
          "xAI provider transport is not implemented yet."),
      unsupported_provider_record(
          "xiaomi", "Xiaomi",
          "Xiaomi provider transport is not implemented yet."),
      unsupported_provider_record(
          "xiaomi-token-plan-ams", "Xiaomi Token Plan AMS",
          "Xiaomi token-plan provider transport is not implemented yet."),
      unsupported_provider_record(
          "xiaomi-token-plan-cn", "Xiaomi Token Plan CN",
          "Xiaomi token-plan provider transport is not implemented yet."),
      unsupported_provider_record(
          "xiaomi-token-plan-sgp", "Xiaomi Token Plan SGP",
          "Xiaomi token-plan provider transport is not implemented yet."),
      provider_record("zai", "openai-completions", "Z.ai",
                      "https://api.z.ai/api/paas/v4"),
  };

  catalog.models = {
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "amazon.nova-2-lite-v1:0", "Nova 2 Lite", false,
                   {"text", "image"}, 128'000, 4'096,
                   cost_metadata(0.33, 2.75, 0.0, 0.0), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "amazon.nova-lite-v1:0", "Nova Lite", false,
                   {"text", "image"}, 300'000, 8'192,
                   cost_metadata(0.06, 0.24, 0.015, 0.0), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "amazon.nova-micro-v1:0", "Nova Micro", false, {"text"},
                   128'000, 8'192, cost_metadata(0.035, 0.14, 0.00875, 0.0),
                   Json::object(), Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "amazon.nova-premier-v1:0", "Nova Premier", true,
                   {"text", "image"}, 1'000'000, 16'384,
                   cost_metadata(2.5, 12.5, 0.0, 0.0), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "amazon.nova-pro-v1:0", "Nova Pro", false,
                   {"text", "image"}, 300'000, 8'192,
                   cost_metadata(0.8, 3.2, 0.2, 0.0), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "anthropic.claude-haiku-4-5-20251001-v1:0",
                   "Claude Haiku 4.5", true, {"text", "image"}, 200'000,
                   64'000, cost_metadata(1.0, 5.0, 0.1, 1.25), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "anthropic.claude-opus-4-6-v1", "Claude Opus 4.6", true,
                   {"text", "image"}, 1'000'000, 128'000,
                   cost_metadata(5.0, 25.0, 0.5, 6.25),
                   Json{{"xhigh", "max"}}, Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "anthropic.claude-opus-4-7", "Claude Opus 4.7", true,
                   {"text", "image"}, 1'000'000, 128'000,
                   cost_metadata(5.0, 25.0, 0.5, 6.25),
                   Json{{"xhigh", "xhigh"}}, Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "anthropic.claude-sonnet-4-5-20250929-v1:0",
                   "Claude Sonnet 4.5", true, {"text", "image"}, 200'000,
                   64'000, cost_metadata(3.0, 15.0, 0.3, 3.75), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "anthropic.claude-sonnet-4-6", "Claude Sonnet 4.6", true,
                   {"text", "image"}, 1'000'000, 64'000,
                   cost_metadata(3.0, 15.0, 0.3, 3.75), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "au.anthropic.claude-opus-4-6-v1",
                   "AU Anthropic Claude Opus 4.6", true, {"text", "image"},
                   1'000'000, 128'000,
                   cost_metadata(16.5, 82.5, 0.5, 6.25),
                   Json{{"xhigh", "max"}}, Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "eu.anthropic.claude-opus-4-7",
                   "Claude Opus 4.7 (EU)", true, {"text", "image"},
                   1'000'000, 128'000,
                   cost_metadata(5.0, 25.0, 0.5, 6.25),
                   Json{{"xhigh", "xhigh"}}, Json::object(),
                   "https://bedrock-runtime.eu-central-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "global.anthropic.claude-sonnet-4-6",
                   "Claude Sonnet 4.6 (Global)", true, {"text", "image"},
                   1'000'000, 64'000, cost_metadata(3.0, 15.0, 0.3, 3.75),
                   Json::object(), Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "us.anthropic.claude-opus-4-6-v1",
                   "Claude Opus 4.6 (US)", true, {"text", "image"},
                   1'000'000, 128'000,
                   cost_metadata(5.0, 25.0, 0.5, 6.25),
                   Json{{"xhigh", "max"}}, Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "deepseek.v3.2", "DeepSeek-V3.2", true, {"text"},
                   163'840, 81'920, cost_metadata(0.62, 1.85, 0.0, 0.0),
                   Json::object(), Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "minimax.minimax-m2.5", "MiniMax M2.5", true, {"text"},
                   196'608, 98'304, cost_metadata(0.3, 1.2, 0.0, 0.0),
                   Json::object(), Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "qwen.qwen3-vl-235b-a22b",
                   "Qwen/Qwen3-VL-235B-A22B-Instruct", false,
                   {"text", "image"}, 262'000, 262'000,
                   cost_metadata(0.3, 1.5, 0.0, 0.0), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("amazon-bedrock", "bedrock-converse-stream",
                   "zai.glm-5", "GLM-5", true, {"text"}, 202'752, 101'376,
                   cost_metadata(1.0, 3.2, 0.0, 0.0), Json::object(),
                   Json::object(),
                   "https://bedrock-runtime.us-east-1.amazonaws.com"),
      model_record("anthropic", "anthropic-messages", "claude-sonnet-4-6",
                   "Claude Sonnet 4.6", true, {"text", "image"}, 1'000'000,
                   64'000, cost_metadata(3.0, 15.0, 0.3, 3.75)),
      model_record("anthropic", "anthropic-messages", "claude-opus-4-7",
                   "Claude Opus 4.7", true, {"text", "image"}, 1'000'000,
                   128'000, cost_metadata(5.0, 25.0, 0.5, 6.25),
                   Json{{"xhigh", "xhigh"}}),
      model_record("anthropic", "anthropic-messages", "claude-haiku-4-5",
                   "Claude Haiku 4.5 (latest)", true, {"text", "image"},
                   200'000, 64'000, cost_metadata(1.0, 5.0, 0.1, 1.25)),
      model_record("google", "google-generative-ai", "gemini-2.5-pro",
                   "Gemini 2.5 Pro", true, {"text", "image"}, 1'048'576,
                   65'536, cost_metadata(1.25, 10.0, 0.31, 0.0)),
      model_record("google", "google-generative-ai", "gemini-3-pro-preview",
                   "Gemini 3 Pro Preview", true, {"text", "image"},
                   1'000'000, 64'000, cost_metadata(2.0, 12.0, 0.2, 0.0),
                   Json{{"off", nullptr},
                        {"minimal", nullptr},
                        {"low", "LOW"},
                        {"medium", nullptr},
                        {"high", "HIGH"}}),
      model_record("google-vertex", "google-vertex", "gemini-3-flash-preview",
                   "Gemini 3 Flash Preview (Vertex)", true, {"text", "image"},
                   1'048'576, 65'536, cost_metadata(0.5, 3.0, 0.05, 0.0),
                   Json{{"off", nullptr}}, Json::object(),
                   "https://{location}-aiplatform.googleapis.com"),
      model_record("google-vertex", "google-vertex", "gemini-3-pro-preview",
                   "Gemini 3 Pro Preview (Vertex)", true, {"text", "image"},
                   1'000'000, 64'000, cost_metadata(2.0, 12.0, 0.2, 0.0),
                   Json{{"off", nullptr},
                        {"minimal", nullptr},
                        {"low", "LOW"},
                        {"medium", nullptr},
                        {"high", "HIGH"}},
                   Json::object(), "https://{location}-aiplatform.googleapis.com"),
      model_record("github-copilot", "anthropic-messages", "claude-opus-4.7",
                   "Claude Opus 4.7", true, {"text", "image"}, 144'000,
                   64'000, cost_metadata(0.0, 0.0, 0.0, 0.0),
                   Json{{"xhigh", "xhigh"}}, Json::object(),
                   "https://api.individual.githubcopilot.com",
                   Json{{"headers",
                         Json{{"User-Agent", "GitHubCopilotChat/0.35.0"},
                              {"Editor-Version", "vscode/1.107.0"},
                              {"Editor-Plugin-Version", "copilot-chat/0.35.0"},
                              {"Copilot-Integration-Id", "vscode-chat"}}}}),
      model_record("github-copilot", "openai-completions",
                   "gemini-3-pro-preview", "Gemini 3 Pro Preview", true,
                   {"text", "image"}, 128'000, 64'000,
                   cost_metadata(0.0, 0.0, 0.0, 0.0), Json::object(),
                   Json{{"supportsStore", false},
                        {"supportsDeveloperRole", false},
                        {"supportsReasoningEffort", false}},
                   "https://api.individual.githubcopilot.com",
                   Json{{"headers",
                         Json{{"User-Agent", "GitHubCopilotChat/0.35.0"},
                              {"Editor-Version", "vscode/1.107.0"},
                              {"Editor-Plugin-Version", "copilot-chat/0.35.0"},
                              {"Copilot-Integration-Id", "vscode-chat"}}}}),
      model_record("github-copilot", "openai-responses", "gpt-5.3-codex",
                   "GPT-5.3-Codex", true, {"text", "image"}, 400'000,
                   128'000, cost_metadata(0.0, 0.0, 0.0, 0.0),
                   Json{{"off", nullptr}, {"xhigh", "xhigh"}}, Json::object(),
                   "https://api.individual.githubcopilot.com",
                   Json{{"headers",
                         Json{{"User-Agent", "GitHubCopilotChat/0.35.0"},
                              {"Editor-Version", "vscode/1.107.0"},
                              {"Editor-Plugin-Version", "copilot-chat/0.35.0"},
                              {"Copilot-Integration-Id", "vscode-chat"}}}}),
      model_record("groq", "openai-completions", "qwen/qwen3-32b",
                   "Qwen3 32B", true, {"text"}, 131'072, 40'960,
                   cost_metadata(0.29, 0.59, 0.0, 0.0),
                   Json{{"minimal", nullptr},
                        {"low", nullptr},
                        {"medium", "default"},
                        {"high", "default"}},
                   Json{{"supportsReasoningEffort", true}},
                   "https://api.groq.com/openai/v1"),
      model_record("groq", "openai-completions", "openai/gpt-oss-20b",
                   "GPT OSS 20B", true, {"text"}, 131'072, 65'536,
                   cost_metadata(0.075, 0.3, 0.0, 0.0), Json::object(),
                   Json{{"supportsReasoningEffort", true}},
                   "https://api.groq.com/openai/v1"),
      model_record("mistral", "mistral-conversations", "mistral-large-latest",
                   "Mistral Large (latest)", false, {"text", "image"},
                   262'144, 262'144, cost_metadata(0.5, 1.5, 0.0, 0.0),
                   Json::object(), Json::object(), "https://api.mistral.ai"),
      model_record("mistral", "mistral-conversations",
                   "magistral-medium-latest", "Magistral Medium (latest)",
                   true, {"text"}, 128'000, 16'384,
                   cost_metadata(2.0, 5.0, 0.0, 0.0), Json::object(),
                   Json::object(), "https://api.mistral.ai"),
      model_record("mistral", "mistral-conversations", "mistral-medium-3.5",
                   "Mistral Medium 3.5", true, {"text", "image"}, 262'144,
                   262'144, cost_metadata(1.5, 7.5, 0.0, 0.0),
                   Json::object(), Json::object(), "https://api.mistral.ai"),
      model_record("mistral", "mistral-conversations", "mistral-small-2603",
                   "Mistral Small 4", true, {"text", "image"}, 256'000,
                   256'000, cost_metadata(0.15, 0.6, 0.0, 0.0),
                   Json::object(), Json::object(), "https://api.mistral.ai"),
      model_record("openai", "openai-responses", "gpt-5",
                   "GPT-5", true, {"text", "image"}, 400'000, 128'000,
                   cost_metadata(1.25, 10.0, 0.125, 0.0),
                   Json{{"off", nullptr}}),
      model_record("openai", "openai-responses", "gpt-5-codex",
                   "GPT-5-Codex", true, {"text", "image"}, 400'000,
                   128'000, cost_metadata(1.25, 10.0, 0.125, 0.0),
                   Json{{"off", nullptr}}),
      model_record("openai", "openai-responses", "gpt-5-mini",
                   "GPT-5 Mini", true, {"text", "image"}, 400'000, 128'000,
                   cost_metadata(0.25, 2.0, 0.025, 0.0),
                   Json{{"off", nullptr}}),
      model_record("openai", "openai-responses", "gpt-5-nano",
                   "GPT-5 Nano", true, {"text", "image"}, 400'000, 128'000,
                   cost_metadata(0.05, 0.4, 0.005, 0.0),
                   Json{{"off", nullptr}}),
      model_record("openai", "openai-responses", "gpt-5.1",
                   "GPT-5.1", true, {"text", "image"}, 400'000, 128'000,
                   cost_metadata(1.25, 10.0, 0.13, 0.0),
                   Json{{"off", "none"}}),
      model_record("openai", "openai-responses", "gpt-5.1-codex",
                   "GPT-5.1 Codex", true, {"text", "image"}, 400'000,
                   128'000, cost_metadata(1.25, 10.0, 0.125, 0.0),
                   Json{{"off", nullptr}}),
      model_record("openai", "openai-responses", "gpt-5.2",
                   "GPT-5.2", true, {"text", "image"}, 400'000, 128'000,
                   cost_metadata(1.75, 14.0, 0.175, 0.0),
                   Json{{"off", "none"}, {"xhigh", "xhigh"}}),
      model_record("openai", "openai-responses", "gpt-5.2-codex",
                   "GPT-5.2 Codex", true, {"text", "image"}, 400'000,
                   128'000, cost_metadata(1.75, 14.0, 0.175, 0.0),
                   Json{{"off", nullptr}, {"xhigh", "xhigh"}}),
      model_record("openai", "openai-responses", "gpt-5.3-codex",
                   "GPT-5.3 Codex", true, {"text", "image"}, 400'000,
                   128'000, cost_metadata(1.75, 14.0, 0.175, 0.0),
                   Json{{"off", "none"}, {"xhigh", "xhigh"}}),
      model_record("openai", "openai-responses", "gpt-6-astra",
                   "GPT-6 Astra", true, {"text", "image"}, 1'050'000,
                   128'000, cost_metadata(10.0, 50.0, 1.0, 12.5),
                   Json{{"off", nullptr}, {"minimal", nullptr},
                        {"xhigh", "xhigh"}}, Json::object(),
                   "https://api.openai.com/v1"),
      model_record("openai-codex", "openai-codex-responses", "gpt-5.3-codex",
                   "GPT-5.3 Codex", true, {"text", "image"}, 272'000,
                   128'000, cost_metadata(1.75, 14.0, 0.175, 0.0),
                   Json{{"minimal", "low"}, {"xhigh", "xhigh"}}, Json::object(),
                   "https://chatgpt.com/backend-api"),
      model_record("openai-codex", "openai-codex-responses", "gpt-5.4",
                   "GPT-5.4", true, {"text", "image"}, 272'000,
                   128'000, cost_metadata(2.5, 15.0, 0.25, 0.0),
                   Json{{"minimal", "low"}, {"xhigh", "xhigh"}},
                   Json::object(), "https://chatgpt.com/backend-api"),
      model_record("openai-codex", "openai-codex-responses", "gpt-5.4-mini",
                   "GPT-5.4 Mini", true, {"text", "image"}, 272'000,
                   128'000, cost_metadata(0.75, 4.5, 0.075, 0.0),
                   Json{{"minimal", "low"}, {"xhigh", "xhigh"}},
                   Json::object(), "https://chatgpt.com/backend-api"),
      model_record("openai-codex", "openai-codex-responses", "gpt-5.5",
                   "GPT-5.5", true, {"text", "image"}, 272'000,
                   128'000, cost_metadata(5.0, 30.0, 0.5, 0.0),
                   Json{{"minimal", "low"}, {"xhigh", "xhigh"}},
                   Json::object(), "https://chatgpt.com/backend-api"),
      model_record("opencode", "openai-completions", "big-pickle",
                   "Big Pickle", true, {"text"}, 200'000, 128'000,
                   cost_metadata(0.0, 0.0, 0.0, 0.0), Json::object(),
                   Json::object(), "https://opencode.ai/zen/v1"),
      model_record("opencode", "anthropic-messages", "claude-sonnet-4-5",
                   "Claude Sonnet 4.5", true, {"text", "image"}, 200'000,
                   64'000, cost_metadata(3.0, 15.0, 0.3, 3.75),
                   Json::object(), Json::object(),
                   "https://opencode.ai/zen"),
      model_record("opencode", "google-generative-ai", "gemini-3-flash",
                   "Gemini 3 Flash", true, {"text", "image"}, 1'048'576,
                   65'536, cost_metadata(0.5, 3.0, 0.05, 0.0),
                   Json{{"off", nullptr}}, Json::object(),
                   "https://opencode.ai/zen/v1"),
      model_record("opencode", "openai-responses", "gpt-5.2-codex",
                   "GPT-5.2 Codex", true, {"text", "image"}, 400'000,
                   128'000, cost_metadata(1.75, 14.0, 0.175, 0.0),
                   Json{{"off", nullptr}, {"xhigh", "xhigh"}},
                   Json::object(), "https://opencode.ai/zen/v1"),
      model_record("opencode", "anthropic-messages", "minimax-m2.5-free",
                   "MiniMax M2.5 Free", true, {"text"}, 204'800, 131'072,
                   cost_metadata(0.0, 0.0, 0.0, 0.0), Json::object(),
                   Json::object(), "https://opencode.ai/zen"),
      model_record("opencode-go", "openai-completions", "kimi-k2.5",
                   "Kimi K2.5", true, {"text", "image"}, 262'144, 65'536,
                   cost_metadata(0.6, 3.0, 0.1, 0.0), Json::object(),
                   Json::object(), "https://opencode.ai/zen/go/v1"),
      model_record("opencode-go", "openai-completions", "minimax-m2.5",
                   "MiniMax M2.5", true, {"text"}, 204'800, 65'536,
                   cost_metadata(0.3, 1.2, 0.03, 0.0), Json::object(),
                   Json::object(), "https://opencode.ai/zen/go/v1"),
      model_record("openrouter", "openai-completions", "openai/gpt-5",
                   "OpenAI GPT-5", true, {"text", "image"}, 400'000,
                   128'000, cost_metadata(1.25, 10.0, 0.125, 0.0)),
      model_record("cloudflare-workers-ai", "openai-completions",
                   "@cf/moonshotai/kimi-k2.6", "Kimi K2.6", true,
                   {"text", "image"}, 256'000, 256'000,
                   cost_metadata(0.95, 4.0, 0.16, 0.0), Json::object(),
                   Json{{"sendSessionAffinityHeaders", true}},
                   "https://api.cloudflare.com/client/v4/accounts/{CLOUDFLARE_ACCOUNT_ID}/ai/v1"),
      model_record("cloudflare-ai-gateway", "anthropic-messages",
                   "claude-sonnet-4-5", "Claude Sonnet 4.5 (latest)", true,
                   {"text", "image"}, 200'000, 64'000,
                   cost_metadata(3.0, 15.0, 0.3, 3.75), Json::object(),
                   Json::object(),
                   "https://gateway.ai.cloudflare.com/v1/{CLOUDFLARE_ACCOUNT_ID}/{CLOUDFLARE_GATEWAY_ID}/anthropic"),
      model_record("cloudflare-ai-gateway", "openai-responses", "gpt-5.1",
                   "GPT-5.1", true, {"text", "image"}, 400'000, 128'000,
                   cost_metadata(1.25, 10.0, 0.13, 0.0),
                   Json{{"off", nullptr}}, Json::object(),
                   "https://gateway.ai.cloudflare.com/v1/{CLOUDFLARE_ACCOUNT_ID}/{CLOUDFLARE_GATEWAY_ID}/openai"),
      model_record("cloudflare-ai-gateway", "openai-completions",
                   "workers-ai/@cf/moonshotai/kimi-k2.6", "Kimi K2.6", true,
                   {"text", "image"}, 256'000, 256'000,
                   cost_metadata(0.95, 4.0, 0.16, 0.0), Json::object(),
                   Json{{"sendSessionAffinityHeaders", true}},
                   "https://gateway.ai.cloudflare.com/v1/{CLOUDFLARE_ACCOUNT_ID}/{CLOUDFLARE_GATEWAY_ID}/compat"),
      model_record("fireworks", "anthropic-messages",
                   "accounts/fireworks/models/kimi-k2p6", "Kimi K2.6", true,
                   {"text", "image"}, 262'000, 262'000,
                   cost_metadata(0.95, 4.0, 0.16, 0.0), Json::object(),
                   Json{{"sendSessionAffinityHeaders", true},
                        {"supportsEagerToolInputStreaming", false},
                        {"supportsCacheControlOnTools", false},
                        {"supportsLongCacheRetention", false}},
                   "https://api.fireworks.ai/inference"),
      model_record("fireworks", "anthropic-messages",
                   "accounts/fireworks/routers/kimi-k2p5-turbo",
                   "Kimi K2.5 Turbo", true, {"text", "image"}, 256'000,
                   256'000, cost_metadata(0.0, 0.0, 0.0, 0.0),
                   Json::object(),
                   Json{{"sendSessionAffinityHeaders", true},
                        {"supportsEagerToolInputStreaming", false},
                        {"supportsCacheControlOnTools", false},
                        {"supportsLongCacheRetention", false}},
                   "https://api.fireworks.ai/inference"),
      model_record("together", "openai-completions", "moonshotai/Kimi-K2.6",
                   "Kimi K2.6", true, {"text", "image"}, 262'144, 131'000,
                   cost_metadata(1.2, 4.5, 0.2, 0.0),
                   Json{{"minimal", nullptr}, {"low", nullptr}, {"medium", nullptr}},
                   Json{{"supportsStore", false},
                        {"supportsDeveloperRole", false},
                        {"supportsReasoningEffort", false},
                        {"maxTokensField", "max_tokens"},
                        {"thinkingFormat", "together"},
                        {"supportsStrictMode", false},
                        {"supportsLongCacheRetention", false}}),
      model_record("together", "openai-completions", "openai/gpt-oss-120b",
                   "GPT OSS 120B", true, {"text"}, 131'072, 131'072,
                   cost_metadata(0.15, 0.6, 0.0, 0.0),
                   Json{{"off", nullptr}, {"minimal", nullptr}},
                   Json{{"supportsStore", false},
                        {"supportsDeveloperRole", false},
                        {"supportsReasoningEffort", true},
                        {"maxTokensField", "max_tokens"},
                        {"thinkingFormat", "openai"},
                        {"supportsStrictMode", false},
                        {"supportsLongCacheRetention", false}}),
      model_record("together", "openai-completions",
                   "deepseek-ai/DeepSeek-V4-Pro", "DeepSeek V4 Pro", true,
                   {"text"}, 512'000, 384'000,
                   cost_metadata(2.1, 4.4, 0.2, 0.0),
                   Json{{"minimal", nullptr},
                        {"low", nullptr},
                        {"medium", nullptr},
                        {"high", "high"},
                        {"xhigh", nullptr}},
                   Json{{"supportsStore", false},
                        {"supportsDeveloperRole", false},
                        {"supportsReasoningEffort", true},
                        {"maxTokensField", "max_tokens"},
                        {"thinkingFormat", "together"},
                        {"supportsStrictMode", false},
                        {"supportsLongCacheRetention", false}}),
      model_record("together", "openai-completions", "MiniMaxAI/MiniMax-M2.7",
                   "MiniMax-M2.7", true, {"text"}, 202'752, 131'072,
                   cost_metadata(0.3, 1.2, 0.06, 0.0),
                   Json{{"off", nullptr},
                        {"minimal", nullptr},
                        {"low", nullptr},
                        {"medium", nullptr}},
                   Json{{"supportsStore", false},
                        {"supportsDeveloperRole", false},
                        {"supportsReasoningEffort", false},
                        {"maxTokensField", "max_tokens"},
                        {"supportsStrictMode", false},
                        {"supportsLongCacheRetention", false}}),
      model_record("zai", "openai-completions", "glm-5", "GLM-5", true,
                   {"text"}, 128'000, 32'768,
                   cost_metadata(0.6, 2.2, 0.0, 0.0), Json::object(),
                   Json{{"supportsDeveloperRole", false},
                        {"thinkingFormat", "zai"},
                        {"zaiToolStream", true}},
                   "https://api.z.ai/api/coding/paas/v4"),
      model_record("zai", "openai-completions", "glm-4.7", "GLM-4.7", true,
                   {"text"}, 204'800, 131'072,
                   cost_metadata(0.0, 0.0, 0.0, 0.0), Json::object(),
                   Json{{"supportsDeveloperRole", false},
                        {"thinkingFormat", "zai"},
                        {"zaiToolStream", true}},
                   "https://api.z.ai/api/coding/paas/v4"),
      model_record("zai", "openai-completions", "glm-5-turbo",
                   "GLM-5-Turbo", true, {"text"}, 200'000, 131'072,
                   cost_metadata(0.0, 0.0, 0.0, 0.0), Json::object(),
                   Json{{"supportsDeveloperRole", false},
                        {"thinkingFormat", "zai"},
                        {"zaiToolStream", true}},
                   "https://api.z.ai/api/coding/paas/v4"),
      model_record("zai", "openai-completions", "glm-5.1", "GLM-5.1", true,
                   {"text"}, 200'000, 131'072,
                   cost_metadata(0.0, 0.0, 0.0, 0.0), Json::object(),
                   Json{{"supportsDeveloperRole", false},
                        {"thinkingFormat", "zai"},
                        {"zaiToolStream", true}},
                   "https://api.z.ai/api/coding/paas/v4"),
      model_record("zai", "openai-completions", "glm-4.5-air",
                   "GLM-4.5-Air", true, {"text"}, 131'072, 98'304,
                   cost_metadata(0.0, 0.0, 0.0, 0.0), Json::object(),
                   Json{{"supportsDeveloperRole", false},
                        {"thinkingFormat", "zai"}},
                   "https://api.z.ai/api/coding/paas/v4"),
  };

  return normalized(catalog);
}

std::vector<ImageModelCatalogRecord> default_image_model_catalog() {
  std::vector<ImageModelCatalogRecord> catalog = {
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "black-forest-labs/flux.2-flex",
          "Black Forest Labs: FLUX.2 Flex",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "black-forest-labs/flux.2-klein-4b",
          "Black Forest Labs: FLUX.2 Klein 4B",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "black-forest-labs/flux.2-max",
          "Black Forest Labs: FLUX.2 Max",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "black-forest-labs/flux.2-pro",
          "Black Forest Labs: FLUX.2 Pro",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "bytedance-seed/seedream-4.5",
          "ByteDance Seed: Seedream 4.5",
          "https://openrouter.ai/api/v1",
          {"image", "text"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "google/gemini-2.5-flash-image",
          "Google: Nano Banana (Gemini 2.5 Flash Image)",
          "https://openrouter.ai/api/v1",
          {"image", "text"},
          {"image", "text"},
          Json{{"cost", cost_metadata(0.3, 2.5, 0.03,
                                      0.08333333333333334)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "google/gemini-3-pro-image-preview",
          "Google: Nano Banana Pro (Gemini 3 Pro Image Preview)",
          "https://openrouter.ai/api/v1",
          {"image", "text"},
          {"image", "text"},
          Json{{"cost", cost_metadata(2.0, 12.0, 0.2, 0.375)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "google/gemini-3.1-flash-image-preview",
          "Google: Nano Banana 2 (Gemini 3.1 Flash Image Preview)",
          "https://openrouter.ai/api/v1",
          {"image", "text"},
          {"image", "text"},
          Json{{"cost", cost_metadata(0.5, 3.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "openai/gpt-5-image",
          "OpenAI: GPT-5 Image",
          "https://openrouter.ai/api/v1",
          {"image", "text"},
          {"image", "text"},
          Json{{"cost", cost_metadata(10.0, 10.0, 1.25, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "openai/gpt-5-image-mini",
          "OpenAI: GPT-5 Image Mini",
          "https://openrouter.ai/api/v1",
          {"image", "text"},
          {"image", "text"},
          Json{{"cost", cost_metadata(2.5, 2.0, 0.25, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "openai/gpt-5.4-image-2",
          "OpenAI: GPT-5.4 Image 2",
          "https://openrouter.ai/api/v1",
          {"image", "text"},
          {"image", "text"},
          Json{{"cost", cost_metadata(8.0, 15.0, 2.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "openrouter/auto",
          "Auto Router",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"text", "image"},
          Json{{"cost", cost_metadata(-1'000'000.0, -1'000'000.0, 0.0,
                                      0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "sourceful/riverflow-v2-fast",
          "Sourceful: Riverflow V2 Fast",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "sourceful/riverflow-v2-fast-preview",
          "Sourceful: Riverflow V2 Fast Preview",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "sourceful/riverflow-v2-max-preview",
          "Sourceful: Riverflow V2 Max Preview",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "sourceful/riverflow-v2-pro",
          "Sourceful: Riverflow V2 Pro",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
      ImageModelCatalogRecord{
          "openrouter",
          "openrouter-images",
          "sourceful/riverflow-v2-standard-preview",
          "Sourceful: Riverflow V2 Standard Preview",
          "https://openrouter.ai/api/v1",
          {"text", "image"},
          {"image"},
          Json{{"cost", cost_metadata(0.0, 0.0, 0.0, 0.0)}}},
  };

  for (auto& record : catalog) {
    record = normalized(record);
  }

  std::sort(catalog.begin(), catalog.end(),
            [](const ImageModelCatalogRecord& lhs,
               const ImageModelCatalogRecord& rhs) {
              return std::make_tuple(provider_key(lhs.provider),
                                     lowercase_copy(lhs.id), lhs.id,
                                     lhs.display_name, lhs.api) <
                     std::make_tuple(provider_key(rhs.provider),
                                     lowercase_copy(rhs.id), rhs.id,
                                     rhs.display_name, rhs.api);
            });
  return catalog;
}

std::vector<std::string> validate(const ModelCatalogProviderRecord& record) {
  const auto normalized_record = normalized(record);
  std::vector<std::string> errors;

  if (normalized_record.descriptor.id.empty()) {
    errors.emplace_back("provider descriptor.id is required");
  }
  if (normalized_record.descriptor.display_name.empty()) {
    errors.emplace_back("provider descriptor.display_name is required");
  }
  if (normalized_record.descriptor.api.empty()) {
    errors.emplace_back("provider descriptor.api is required");
  }
  return errors;
}

std::vector<std::string> validate(const ModelCatalogModelRecord& record) {
  const auto normalized_record = normalized(record);
  std::vector<std::string> errors;

  if (normalized_record.ref.provider.empty()) {
    errors.emplace_back("model ref.provider is required");
  }
  if (normalized_record.ref.api.empty()) {
    errors.emplace_back("model ref.api is required");
  }
  if (normalized_record.ref.name.empty()) {
    errors.emplace_back("model ref.name is required");
  }
  if (normalized_record.display_name.empty()) {
    errors.emplace_back("model display_name is required");
  }
  return errors;
}

std::vector<std::string> validate(const ModelCatalog& catalog) {
  const auto normalized_catalog = normalized(catalog);
  std::vector<std::string> errors;
  std::vector<std::string> provider_ids;
  std::vector<std::pair<std::string, std::string>> provider_apis;
  std::vector<std::string> model_ids;

  provider_ids.reserve(normalized_catalog.providers.size());
  provider_apis.reserve(normalized_catalog.providers.size());
  model_ids.reserve(normalized_catalog.models.size());

  for (const auto& provider : normalized_catalog.providers) {
    append_messages(errors, validate(provider));
    provider_ids.emplace_back(provider_key(provider.descriptor.id));
    provider_apis.emplace_back(provider_key(provider.descriptor.id),
                               provider.descriptor.api);
  }

  for (std::size_t i = 1; i < provider_ids.size(); ++i) {
    if (provider_ids[i] == provider_ids[i - 1]) {
      errors.push_back("duplicate provider id: " +
                       normalized_catalog.providers[i].descriptor.id);
    }
  }

  for (const auto& model : normalized_catalog.models) {
    append_messages(errors, validate(model));
    model_ids.push_back(model_key(model.ref.provider, model.ref.name));

    const auto* provider = find_provider_record(normalized_catalog, model.ref.provider);
    if (provider == nullptr) {
      errors.push_back("model references unknown provider: " + model.ref.provider +
                       "/" + model.ref.name);
      continue;
    }

    if (!provider_accepts_api(*provider, model.ref.api)) {
      errors.push_back("model api does not match provider api: " + model.ref.provider +
                       "/" + model.ref.name);
    }
  }

  for (std::size_t i = 1; i < model_ids.size(); ++i) {
    if (model_ids[i] == model_ids[i - 1]) {
      errors.push_back("duplicate model ref: " +
                       normalized_catalog.models[i].ref.provider + "/" +
                       normalized_catalog.models[i].ref.name);
    }
  }

  return errors;
}

bool is_valid(const ModelCatalogProviderRecord& record) {
  return validate(record).empty();
}

bool is_valid(const ModelCatalogModelRecord& record) {
  return validate(record).empty();
}

bool is_valid(const ModelCatalog& catalog) { return validate(catalog).empty(); }

ParsedModelCatalogReference parse_model_catalog_reference(
    const std::string& reference) {
  ParsedModelCatalogReference result;
  const std::string trimmed = trim_copy(reference);
  if (trimmed.empty()) {
    return result;
  }

  const auto slash = trimmed.find('/');
  if (slash == std::string::npos) {
    result.model = trimmed;
    return result;
  }

  const std::string provider = trim_copy(trimmed.substr(0, slash));
  const std::string model = trim_copy(trimmed.substr(slash + 1));
  if (provider.empty() || model.empty()) {
    result.model = trimmed;
    return result;
  }

  result.provider = provider;
  result.model = model;
  return result;
}

const ModelCatalogProviderRecord* find_provider_record(
    const ModelCatalog& catalog, const std::string& provider_id) {
  const std::string wanted = provider_key(provider_id);
  if (wanted.empty()) {
    return nullptr;
  }

  for (const auto& provider : catalog.providers) {
    if (provider_key(provider.descriptor.id) == wanted) {
      return &provider;
    }
  }
  return nullptr;
}

const ModelCatalogModelRecord* find_model_record(
    const ModelCatalog& catalog, const std::string& provider_id,
    const std::string& model_id) {
  const std::string wanted = model_key(provider_id, model_id);
  if (wanted == "\n") {
    return nullptr;
  }

  for (const auto& model : catalog.models) {
    if (model_key(model.ref.provider, model.ref.name) == wanted) {
      return &model;
    }
  }
  return nullptr;
}

const ModelCatalogModelRecord* match_model_record(
    const ModelCatalog& catalog, const std::string& reference) {
  const std::string trimmed = trim_copy(reference);
  if (trimmed.empty()) {
    return nullptr;
  }

  const std::string canonical = lowercase_copy(trimmed);
  for (const auto& model : catalog.models) {
    if (lowercase_copy(model.ref.provider + "/" + model.ref.name) == canonical) {
      return &model;
    }
  }

  const auto parsed = parse_model_catalog_reference(trimmed);
  if (parsed.provider) {
    const bool known_provider = find_provider_record(catalog, *parsed.provider) != nullptr;
    if (known_provider) {
      if (const auto* model =
              find_model_record(catalog, *parsed.provider, parsed.model);
          model != nullptr) {
        return model;
      }
      return find_model_record(catalog, *parsed.provider, trimmed);
    }
  }

  const std::string wanted_model = lowercase_copy(parsed.model);
  const ModelCatalogModelRecord* match = nullptr;
  for (const auto& model : catalog.models) {
    if (lowercase_copy(model.ref.name) != wanted_model) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &model;
  }
  return match;
}

const ImageModelCatalogRecord* find_image_model_record(
    const std::vector<ImageModelCatalogRecord>& catalog,
    const std::string& provider_id, const std::string& model_id) {
  const std::string wanted_provider = provider_key(provider_id);
  const std::string wanted_model = lowercase_copy(trim_copy(model_id));
  if (wanted_provider.empty() || wanted_model.empty()) {
    return nullptr;
  }

  for (const auto& model : catalog) {
    if (provider_key(model.provider) == wanted_provider &&
        lowercase_copy(trim_copy(model.id)) == wanted_model) {
      return &model;
    }
  }
  return nullptr;
}

std::vector<std::string> supported_thinking_levels(
    const ModelCatalogModelRecord& record) {
  static const std::vector<std::string> kExtendedLevels = {
      "off", "minimal", "low", "medium", "high", "xhigh"};
  if (!record.capabilities.reasoning) {
    return {"off"};
  }

  const Json map = record.metadata.is_object()
                       ? record.metadata.value("thinkingLevelMap", Json::object())
                       : Json::object();
  std::vector<std::string> result;
  for (const auto& level : kExtendedLevels) {
    if (map.is_object() && map.contains(level) && map.at(level).is_null()) {
      continue;
    }
    if (level == "xhigh" &&
        (!map.is_object() || !map.contains(level))) {
      continue;
    }
    result.push_back(level);
  }
  return result;
}

std::string clamp_thinking_level(const ModelCatalogModelRecord& record,
                                 const std::string& level) {
  static const std::vector<std::string> kExtendedLevels = {
      "off", "minimal", "low", "medium", "high", "xhigh"};
  const auto available = supported_thinking_levels(record);
  if (std::find(available.begin(), available.end(), level) !=
      available.end()) {
    return level;
  }

  const auto requested =
      std::find(kExtendedLevels.begin(), kExtendedLevels.end(), level);
  if (requested == kExtendedLevels.end()) {
    return available.empty() ? "off" : available.front();
  }

  for (auto it = requested; it != kExtendedLevels.end(); ++it) {
    if (std::find(available.begin(), available.end(), *it) !=
        available.end()) {
      return *it;
    }
  }
  for (auto it = std::make_reverse_iterator(requested);
       it != kExtendedLevels.rend(); ++it) {
    if (std::find(available.begin(), available.end(), *it) !=
        available.end()) {
      return *it;
    }
  }
  return available.empty() ? "off" : available.front();
}

ModelCatalog merge_catalogs(const ModelCatalog& base, const ModelCatalog& overlay) {
  ModelCatalog merged = normalized(base);
  const ModelCatalog normalized_overlay = normalized(overlay);

  for (const auto& provider : normalized_overlay.providers) {
    const auto it = std::lower_bound(
        merged.providers.begin(), merged.providers.end(), provider_key(provider.descriptor.id),
        [](const ModelCatalogProviderRecord& record, const std::string& key) {
          return provider_key(record.descriptor.id) < key;
        });

    if (it != merged.providers.end() &&
        provider_key(it->descriptor.id) == provider_key(provider.descriptor.id)) {
      *it = provider;
    } else {
      merged.providers.insert(it, provider);
    }
  }

  for (const auto& model : normalized_overlay.models) {
    const auto it =
        std::lower_bound(merged.models.begin(), merged.models.end(),
                         model_key(model.ref.provider, model.ref.name),
                         [](const ModelCatalogModelRecord& record,
                            const std::string& key) {
                           return model_key(record.ref.provider, record.ref.name) < key;
                         });

    if (it != merged.models.end() &&
        model_key(it->ref.provider, it->ref.name) ==
            model_key(model.ref.provider, model.ref.name)) {
      *it = model;
    } else {
      merged.models.insert(it, model);
    }
  }

  return normalized(merged);
}

qt::foundation::Result<ModelCatalog> model_catalog_from_provider_config(
    const std::string& provider_id, const Json& config,
    const ModelCatalog& base_catalog) {
  using qt::foundation::ErrorCode;
  using qt::foundation::Result;

  const std::string provider = trim_copy(provider_id);
  if (provider.empty()) {
    return Result<ModelCatalog>::failure(ErrorCode::invalid_argument,
                                         "provider name is required");
  }
  if (!config.is_object()) {
    return Result<ModelCatalog>::failure(
        ErrorCode::invalid_argument,
        "Provider " + provider + ": config must be a JSON object.");
  }
  if (config.contains("streamSimple") && !optional_string_field(config, "api")) {
    return Result<ModelCatalog>::failure(
        ErrorCode::invalid_argument,
        "Provider " + provider +
            ": \"api\" is required when registering streamSimple.");
  }

  const auto* base_provider = find_provider_record(base_catalog, provider);
  const auto models_it = config.find("models");
  if (models_it != config.end() && !models_it->is_array()) {
    return Result<ModelCatalog>::failure(
        ErrorCode::invalid_argument,
        "Provider " + provider + ": \"models\" must be an array.");
  }
  const bool has_models =
      models_it != config.end() && models_it->is_array() && !models_it->empty();
  const auto model_overrides_it = config.find("modelOverrides");
  if (model_overrides_it != config.end() && !model_overrides_it->is_object()) {
    return Result<ModelCatalog>::failure(
        ErrorCode::invalid_argument,
        "Provider " + provider + ": \"modelOverrides\" must be an object.");
  }
  const bool has_model_overrides =
      model_overrides_it != config.end() && model_overrides_it->is_object() &&
      !model_overrides_it->empty();

  ModelCatalog overlay;
  ModelCatalogProviderRecord provider_record;
  if (base_provider != nullptr) {
    provider_record = *base_provider;
  } else {
    provider_record.descriptor.id = provider;
    provider_record.descriptor.api =
        optional_string_field(config, "api").value_or(std::string("custom"));
    provider_record.descriptor.display_name =
        optional_string_field(config, "name").value_or(provider);
    provider_record.metadata = Json::object();
  }

  if (auto name = optional_string_field(config, "name")) {
    provider_record.descriptor.display_name = *name;
  }
  if (auto api = optional_string_field(config, "api")) {
    provider_record.descriptor.api = *api;
  }
  if (auto base_url = optional_string_field(config, "baseUrl")) {
    provider_record.metadata["baseUrl"] = *base_url;
  }
  if (config.contains("headers") && config.at("headers").is_object()) {
    provider_record.metadata["headers"] = config.at("headers");
  }
  if (config.contains("apiKey") && config.at("apiKey").is_string()) {
    provider_record.metadata["apiKey"] = config.at("apiKey");
  }
  if (config.contains("authHeader") && config.at("authHeader").is_boolean()) {
    provider_record.metadata["authHeader"] = config.at("authHeader");
  }
  if (config.contains("streamSimple")) {
    provider_record.metadata["streamSimple"] = config.at("streamSimple");
  }
  if (config.contains("oauth") && config.at("oauth").is_object()) {
    provider_record.metadata["oauth"] = config.at("oauth");
    if (!optional_string_field(config, "name")) {
      if (auto oauth_name = optional_string_field(config.at("oauth"), "name")) {
        provider_record.descriptor.display_name = *oauth_name;
      }
    }
  }
  if (config.contains("compat") && config.at("compat").is_object()) {
    provider_record.metadata["compat"] = config.at("compat");
  }

  if (!has_models) {
    if (base_provider == nullptr && !has_model_overrides &&
        !optional_string_field(config, "baseUrl") &&
        !(config.contains("headers") && config.at("headers").is_object()) &&
        !(config.contains("compat") && config.at("compat").is_object())) {
      return Result<ModelCatalog>::failure(
          ErrorCode::invalid_argument,
          "Provider " + provider +
              ": must specify \"baseUrl\", \"headers\", \"compat\", "
              "\"modelOverrides\", or \"models\".");
    }
    overlay.providers.push_back(std::move(provider_record));
    if (config.contains("baseUrl") || config.contains("headers") ||
        config.contains("compat") || config.contains("apiKey") ||
        config.contains("authHeader") || has_model_overrides) {
      for (const auto& model : base_catalog.models) {
        if (provider_key(model.ref.provider) != provider_key(provider)) {
          continue;
        }
        ModelCatalogModelRecord replacement = model;
        if (auto base_url = optional_string_field(config, "baseUrl")) {
          replacement.metadata["baseUrl"] = *base_url;
        }
        if (config.contains("headers") && config.at("headers").is_object()) {
          replacement.metadata["headers"] = merge_json_objects(
              replacement.metadata.value("headers", Json::object()),
              config.at("headers"));
        }
        if (config.contains("compat") && config.at("compat").is_object()) {
          replacement.metadata["compat"] = merge_json_objects(
              replacement.metadata.value("compat", Json::object()),
              config.at("compat"));
        }
        if (config.contains("apiKey") && config.at("apiKey").is_string()) {
          replacement.metadata["apiKey"] = config.at("apiKey");
        }
        if (config.contains("authHeader") &&
            config.at("authHeader").is_boolean()) {
          replacement.metadata["authHeader"] = config.at("authHeader");
        }
        if (has_model_overrides &&
            model_overrides_it->contains(model.ref.name)) {
          apply_model_override(replacement,
                               model_overrides_it->at(model.ref.name));
        }
        overlay.models.push_back(std::move(replacement));
      }
    }
    return overlay;
  }

  if (base_provider == nullptr && !optional_string_field(config, "baseUrl")) {
    return Result<ModelCatalog>::failure(
        ErrorCode::invalid_argument,
        "Provider " + provider +
            ": \"baseUrl\" is required when defining models.");
  }
  if (base_provider == nullptr && !optional_string_field(config, "apiKey") &&
      !(config.contains("oauth") && config.at("oauth").is_object())) {
    return Result<ModelCatalog>::failure(
        ErrorCode::invalid_argument,
        "Provider " + provider +
            ": \"apiKey\" or \"oauth\" is required when defining models.");
  }

  std::set<std::string> accepted_apis;
  for (const auto& model_json : *models_it) {
    const auto id = optional_string_field(model_json, "id");
    const auto name = optional_string_field(model_json, "name");
    if (!id) {
      return Result<ModelCatalog>::failure(
          ErrorCode::invalid_argument,
          "Provider " + provider + ": model missing \"id\".");
    }

    const auto api = optional_string_field(model_json, "api")
                         .value_or(optional_string_field(config, "api")
                                       .value_or(base_provider == nullptr
                                                     ? std::string{}
                                                     : provider_record
                                                           .descriptor.api));
    if (api.empty()) {
      return Result<ModelCatalog>::failure(
          ErrorCode::invalid_argument,
          "Provider " + provider + ", model " + *id +
              ": no \"api\" specified.");
    }
    accepted_apis.insert(api);

    const auto context_window_field =
        positive_int_field(model_json, "contextWindow");
    if (model_json.contains("contextWindow") && !context_window_field) {
      return Result<ModelCatalog>::failure(
          ErrorCode::invalid_argument,
          "Provider " + provider + ", model " + *id +
              ": invalid contextWindow.");
    }
    const auto max_tokens_field = positive_int_field(model_json, "maxTokens");
    if (model_json.contains("maxTokens") && !max_tokens_field) {
      return Result<ModelCatalog>::failure(
          ErrorCode::invalid_argument,
          "Provider " + provider + ", model " + *id + ": invalid maxTokens.");
    }
    const auto context_window = context_window_field.value_or(128000);
    const auto max_tokens = max_tokens_field.value_or(16384);

    Json metadata = Json::object();
    metadata["input"] = string_array_field(model_json, "input");
    metadata["cost"] =
        model_json.value("cost", Json{{"input", 0},
                                      {"output", 0},
                                      {"cacheRead", 0},
                                      {"cacheWrite", 0}});
    metadata["contextWindow"] = context_window;
    metadata["maxTokens"] = max_tokens;
    if (const auto base_url = optional_string_field(model_json, "baseUrl")) {
      metadata["baseUrl"] = *base_url;
    } else if (const auto config_base_url =
                   optional_string_field(config, "baseUrl")) {
      metadata["baseUrl"] = *config_base_url;
    } else if (base_provider != nullptr &&
               base_provider->metadata.contains("baseUrl") &&
               base_provider->metadata.at("baseUrl").is_string()) {
      metadata["baseUrl"] = base_provider->metadata.at("baseUrl");
    }
    if (config.contains("headers") && config.at("headers").is_object()) {
      metadata["headers"] = config.at("headers");
    }
    if (model_json.contains("headers") && model_json.at("headers").is_object()) {
      metadata["headers"] =
          merge_json_objects(metadata.value("headers", Json::object()),
                             model_json.at("headers"));
    }
    if (model_json.contains("thinkingLevelMap") &&
        model_json.at("thinkingLevelMap").is_object()) {
      metadata["thinkingLevelMap"] = model_json.at("thinkingLevelMap");
    }
    Json compat = config.value("compat", Json::object());
    compat = merge_json_objects(std::move(compat),
                                model_json.value("compat", Json::object()));
    if (!compat.empty()) {
      metadata["compat"] = std::move(compat);
    }
    if (config.contains("apiKey") && config.at("apiKey").is_string()) {
      metadata["apiKey"] = config.at("apiKey");
    }
    if (config.contains("authHeader") &&
        config.at("authHeader").is_boolean()) {
      metadata["authHeader"] = config.at("authHeader");
    }

    ModelCatalogModelRecord model;
    model.ref = ModelRef{provider, api, *id};
    model.display_name = name.value_or(*id);
    model.capabilities.reasoning =
        optional_bool_field(model_json, "reasoning", false);
    const auto input = strings_from_json_array(metadata.at("input"));
    model.capabilities.vision =
        std::find(input.begin(), input.end(), "image") != input.end();
    model.metadata = std::move(metadata);
    overlay.models.push_back(std::move(model));
  }

  if (!accepted_apis.empty()) {
    provider_record.descriptor.api = *accepted_apis.begin();
    if (accepted_apis.size() > 1U ||
        (base_provider != nullptr &&
         !provider_accepts_api(provider_record, *accepted_apis.begin()))) {
      provider_record.metadata["acceptedApis"] =
          std::vector<std::string>(accepted_apis.begin(), accepted_apis.end());
    }
  }
  overlay.providers.push_back(std::move(provider_record));
  return normalized(overlay);
}

qt::foundation::Result<ModelCatalog> apply_provider_config_to_model_catalog(
    const ModelCatalog& base_catalog, const std::string& provider_id,
    const Json& config) {
  auto overlay =
      model_catalog_from_provider_config(provider_id, config, base_catalog);
  if (!overlay) {
    return overlay.error();
  }

  ModelCatalog base = normalized(base_catalog);
  const std::string provider = provider_key(provider_id);
  const auto models_it = config.is_object() ? config.find("models") : config.end();
  const bool replaces_models =
      models_it != config.end() && models_it->is_array() && !models_it->empty();
  if (replaces_models) {
    base.models.erase(
        std::remove_if(base.models.begin(), base.models.end(),
                       [&](const ModelCatalogModelRecord& model) {
                         return provider_key(model.ref.provider) == provider;
                       }),
        base.models.end());
  }

  return merge_catalogs(base, overlay.value());
}

qt::foundation::Result<ModelCatalog> apply_models_config_to_model_catalog(
    const ModelCatalog& base_catalog, const Json& config) {
  using qt::foundation::ErrorCode;
  using qt::foundation::Result;

  if (!config.is_object()) {
    return Result<ModelCatalog>::failure(ErrorCode::invalid_argument,
                                         "models.json must be a JSON object");
  }
  const auto providers_it = config.find("providers");
  if (providers_it == config.end() || !providers_it->is_object()) {
    return Result<ModelCatalog>::failure(
        ErrorCode::invalid_argument,
        "models.json providers must be an object");
  }

  ModelCatalog catalog = base_catalog;
  for (auto it = providers_it->begin(); it != providers_it->end(); ++it) {
    const auto applied =
        apply_provider_config_to_model_catalog(catalog, it.key(), it.value());
    if (!applied) {
      return Result<ModelCatalog>::failure(
          applied.error().code,
          "models.json providers." + it.key() + ": " +
              applied.error().message,
          applied.error().system_error);
    }
    catalog = std::move(applied.value());
  }
  return catalog;
}

std::optional<TokenCostRates> token_cost_rates_from_model_metadata(
    const ModelCatalogModelRecord& record) {
  if (!record.metadata.is_object()) {
    return std::nullopt;
  }

  const auto cost = record.metadata.value("cost", Json::object());
  if (!cost.is_object()) {
    return std::nullopt;
  }

  const auto input =
      metadata_number(cost, "input_usd_per_million", "input");
  const auto output =
      metadata_number(cost, "output_usd_per_million", "output");
  const auto cache_read =
      metadata_number(cost, "cache_read_usd_per_million", "cacheRead");
  const auto cache_write =
      metadata_number(cost, "cache_write_usd_per_million", "cacheWrite");

  if (!input && !output && !cache_read && !cache_write) {
    return std::nullopt;
  }

  TokenCostRates rates;
  rates.input_usd_per_million = input.value_or(0.0);
  rates.output_usd_per_million = output.value_or(0.0);
  rates.cache_read_usd_per_million = cache_read.value_or(0.0);
  rates.cache_write_usd_per_million = cache_write.value_or(0.0);
  return rates;
}

std::optional<TokenCostRates> token_cost_rates_from_image_model_metadata(
    const ImageModelCatalogRecord& record) {
  if (!record.metadata.is_object()) {
    return std::nullopt;
  }

  ModelCatalogModelRecord as_model;
  as_model.metadata = record.metadata;
  return token_cost_rates_from_model_metadata(as_model);
}

ProviderOptions provider_options_from_model_metadata(
    const ModelCatalogModelRecord& record) {
  ProviderOptions options;
  if (const auto rates = token_cost_rates_from_model_metadata(record)) {
    options.metadata["cost"] = Json{
        {"input_usd_per_million", rates->input_usd_per_million},
        {"output_usd_per_million", rates->output_usd_per_million},
        {"cache_read_usd_per_million", rates->cache_read_usd_per_million},
        {"cache_write_usd_per_million", rates->cache_write_usd_per_million},
    };
  }
  if (record.metadata.is_object()) {
    options.metadata["provider"] = record.ref.provider;
    options.metadata["api"] = record.ref.api;
    options.metadata["model"] = record.ref.name;
    if (record.metadata.contains("maxTokens") &&
        record.metadata.at("maxTokens").is_number_integer()) {
      options.metadata["maxTokens"] =
          record.metadata.at("maxTokens").get<std::int64_t>();
    }
    if (record.metadata.contains("contextWindow") &&
        record.metadata.at("contextWindow").is_number_integer()) {
      options.metadata["contextWindow"] =
          record.metadata.at("contextWindow").get<std::int64_t>();
    }
    if (record.metadata.contains("timeoutMs") &&
        record.metadata.at("timeoutMs").is_number_integer()) {
      options.timeout_ms = std::max<std::int64_t>(
          1, record.metadata.at("timeoutMs").get<std::int64_t>());
    }
    if (record.metadata.contains("compat") &&
        record.metadata.at("compat").is_object()) {
      options.metadata["compat"] = record.metadata.at("compat");
    }
    if (record.metadata.contains("baseUrl") &&
        record.metadata.at("baseUrl").is_string()) {
      options.metadata["baseUrl"] = record.metadata.at("baseUrl");
    }
    if (record.metadata.contains("headers") &&
        record.metadata.at("headers").is_object()) {
      options.metadata["headers"] = record.metadata.at("headers");
    }
    if (record.metadata.contains("apiKey") &&
        record.metadata.at("apiKey").is_string()) {
      const auto api_key = trim_copy(record.metadata.at("apiKey").get<std::string>());
      if (!api_key.empty()) {
        options.api_key = api_key;
        options.metadata["apiKey"] = api_key;
      }
    }
    if (record.metadata.contains("authHeader") &&
        record.metadata.at("authHeader").is_boolean()) {
      options.metadata["authHeader"] = record.metadata.at("authHeader");
      if (record.metadata.at("authHeader").get<bool>() && options.api_key) {
        options.headers["Authorization"] = "Bearer " + *options.api_key;
      }
    }
    if (record.metadata.contains("input") && record.metadata.at("input").is_array()) {
      options.metadata["input"] = record.metadata.at("input");
    }
    if (record.metadata.contains("thinkingLevelMap") &&
        record.metadata.at("thinkingLevelMap").is_object()) {
      options.metadata["thinkingLevelMap"] = record.metadata.at("thinkingLevelMap");
    }
  }
  return options;
}

ProviderOptions provider_options_from_image_model_metadata(
    const ImageModelCatalogRecord& record) {
  ProviderOptions options;
  if (const auto rates = token_cost_rates_from_image_model_metadata(record)) {
    options.metadata["cost"] = Json{
        {"input_usd_per_million", rates->input_usd_per_million},
        {"output_usd_per_million", rates->output_usd_per_million},
        {"cache_read_usd_per_million", rates->cache_read_usd_per_million},
        {"cache_write_usd_per_million", rates->cache_write_usd_per_million},
    };
  }
  options.metadata["api"] = record.api;
  options.metadata["baseUrl"] = record.base_url;
  options.metadata["input"] = record.input;
  options.metadata["output"] = record.output;
  if (record.metadata.contains("headers") &&
      record.metadata.at("headers").is_object()) {
    options.metadata["headers"] = record.metadata.at("headers");
  }
  return options;
}

}  // namespace qt::ai
