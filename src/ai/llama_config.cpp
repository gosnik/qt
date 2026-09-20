#include "qt/ai/llama_config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool is_one_of(const std::string& value, const std::vector<const char*>& allowed) {
  return std::find_if(allowed.begin(), allowed.end(), [&](const char* candidate) {
           return value == candidate;
         }) != allowed.end();
}

void append_messages(std::vector<std::string>& into,
                     const std::vector<std::string>& more) {
  into.insert(into.end(), more.begin(), more.end());
}

template <typename T>
void overlay_optional(std::optional<T>& target, const std::optional<T>& value) {
  if (value) {
    target = value;
  }
}

void optional_to_json(Json& json, const char* key,
                      const std::optional<std::string>& value) {
  if (value) {
    json[key] = *value;
  }
}

void optional_to_json(Json& json, const char* key,
                      const std::optional<bool>& value) {
  if (value) {
    json[key] = *value;
  }
}

void optional_to_json(Json& json, const char* key, const std::optional<int>& value) {
  if (value) {
    json[key] = *value;
  }
}

void optional_to_json(Json& json, const char* key,
                      const std::optional<std::uint32_t>& value) {
  if (value) {
    json[key] = *value;
  }
}

void optional_to_json(Json& json, const char* key,
                      const std::optional<std::int64_t>& value) {
  if (value) {
    json[key] = *value;
  }
}

void optional_to_json(Json& json, const char* key,
                      const std::optional<double>& value) {
  if (value) {
    json[key] = *value;
  }
}

Json zero_cost_metadata() {
  return Json{{"input", 0.0},
              {"output", 0.0},
              {"cacheRead", 0.0},
              {"cacheWrite", 0.0},
              {"input_usd_per_million", 0.0},
              {"output_usd_per_million", 0.0},
              {"cache_read_usd_per_million", 0.0},
              {"cache_write_usd_per_million", 0.0}};
}

ProviderCapabilities llama_capabilities(const LlamaSettings& settings,
                                         const LlamaModelSettings* model = nullptr) {
  ProviderCapabilities capabilities;
  capabilities.streaming = true;
  capabilities.tool_calls = true;
  capabilities.vision = false;
  capabilities.reasoning = false;
  capabilities.session_caching =
      settings.kv_cache.policy.value_or("session") != "off";
  (void)model;
  return capabilities;
}

std::optional<std::int64_t> effective_context_window(
    const LlamaSettings& settings, const LlamaModelSettings& model) {
  if (model.context_window) {
    return model.context_window;
  }
  return settings.context.context_size;
}

std::optional<std::int64_t> effective_max_tokens(
    const LlamaSettings& settings, const LlamaModelSettings& model) {
  if (model.max_tokens) {
    return model.max_tokens;
  }
  return settings.generation.max_output_tokens;
}

Json model_generation_json(const LlamaSettings& settings,
                           const LlamaModelSettings& model) {
  Json json;
  to_json(json, merge_llama_generation_settings(settings.generation,
                                                model.generation));
  return json;
}

}  // namespace

bool empty(const LlamaGenerationSettings& settings) noexcept {
  return !settings.temperature && !settings.top_k && !settings.top_p &&
         !settings.min_p && !settings.min_keep && !settings.seed &&
         !settings.max_output_tokens && !settings.repeat_penalty &&
         !settings.presence_penalty && !settings.frequency_penalty;
}

bool empty(const LlamaBackendSettings& settings) noexcept {
  return !settings.enabled && !settings.gpu_layers && !settings.main_gpu &&
         !settings.split_mode && !settings.tensor_split_configured &&
         !settings.use_mmap && !settings.use_mlock && !settings.offload_kqv;
}

bool empty(const LlamaContextSettings& settings) noexcept {
  return !settings.context_size && !settings.batch_size && !settings.ubatch_size &&
         !settings.threads && !settings.batch_threads;
}

bool empty(const LlamaKvCacheSettings& settings) noexcept {
  return !settings.policy && !settings.max_sessions &&
         !settings.clear_on_model_change;
}

bool empty(const LlamaGrammarSettings& settings) noexcept {
  return !settings.root && !settings.default_grammar && !settings.files_configured;
}

bool empty(const LlamaEmbeddingSettings& settings) noexcept {
  return !settings.enabled && !settings.pooling && !settings.normalize;
}

bool empty(const LlamaModelSettings& settings) noexcept {
  return trim_copy(settings.id).empty() && trim_copy(settings.name).empty() &&
         trim_copy(settings.path).empty() && !settings.block_count &&
         !settings.context_window &&
         !settings.max_tokens && !settings.supports_embeddings &&
         empty(settings.generation);
}

bool empty(const LlamaSettings& settings) noexcept {
  return empty(settings.backend) && empty(settings.context) &&
         empty(settings.generation) && empty(settings.kv_cache) &&
         empty(settings.grammar) && empty(settings.embeddings) &&
         !settings.selected_model_id &&
         !settings.models_configured && settings.models.empty();
}

LlamaGenerationSettings merge_llama_generation_settings(
    const LlamaGenerationSettings& base,
    const LlamaGenerationSettings& overlay) {
  LlamaGenerationSettings result = base;
  overlay_optional(result.temperature, overlay.temperature);
  overlay_optional(result.top_k, overlay.top_k);
  overlay_optional(result.top_p, overlay.top_p);
  overlay_optional(result.min_p, overlay.min_p);
  overlay_optional(result.min_keep, overlay.min_keep);
  overlay_optional(result.seed, overlay.seed);
  overlay_optional(result.max_output_tokens, overlay.max_output_tokens);
  overlay_optional(result.repeat_penalty, overlay.repeat_penalty);
  overlay_optional(result.presence_penalty, overlay.presence_penalty);
  overlay_optional(result.frequency_penalty, overlay.frequency_penalty);
  return result;
}

LlamaSettings merge_llama_settings(const LlamaSettings& base,
                                   const LlamaSettings& overlay) {
  LlamaSettings result = base;
  overlay_optional(result.backend.enabled, overlay.backend.enabled);
  overlay_optional(result.backend.gpu_layers, overlay.backend.gpu_layers);
  overlay_optional(result.backend.main_gpu, overlay.backend.main_gpu);
  overlay_optional(result.backend.split_mode, overlay.backend.split_mode);
  if (overlay.backend.tensor_split_configured) {
    result.backend.tensor_split = overlay.backend.tensor_split;
    result.backend.tensor_split_configured = true;
  }
  overlay_optional(result.backend.use_mmap, overlay.backend.use_mmap);
  overlay_optional(result.backend.use_mlock, overlay.backend.use_mlock);
  overlay_optional(result.backend.offload_kqv, overlay.backend.offload_kqv);

  overlay_optional(result.context.context_size, overlay.context.context_size);
  overlay_optional(result.context.batch_size, overlay.context.batch_size);
  overlay_optional(result.context.ubatch_size, overlay.context.ubatch_size);
  overlay_optional(result.context.threads, overlay.context.threads);
  overlay_optional(result.context.batch_threads, overlay.context.batch_threads);

  result.generation =
      merge_llama_generation_settings(result.generation, overlay.generation);

  overlay_optional(result.kv_cache.policy, overlay.kv_cache.policy);
  overlay_optional(result.kv_cache.max_sessions, overlay.kv_cache.max_sessions);
  overlay_optional(result.kv_cache.clear_on_model_change,
                   overlay.kv_cache.clear_on_model_change);

  overlay_optional(result.grammar.root, overlay.grammar.root);
  overlay_optional(result.grammar.default_grammar,
                   overlay.grammar.default_grammar);
  if (overlay.grammar.files_configured) {
    result.grammar.files = overlay.grammar.files;
    result.grammar.files_configured = true;
  }

  overlay_optional(result.embeddings.enabled, overlay.embeddings.enabled);
  overlay_optional(result.embeddings.pooling, overlay.embeddings.pooling);
  overlay_optional(result.embeddings.normalize, overlay.embeddings.normalize);

  overlay_optional(result.selected_model_id, overlay.selected_model_id);

  if (overlay.models_configured) {
    result.models = overlay.models;
    result.models_configured = true;
  }

  return result;
}

std::vector<std::string> validate(const LlamaGenerationSettings& settings,
                                  const std::string& field_prefix) {
  std::vector<std::string> errors;
  const auto finite = [&](std::optional<double> value, const char* field) {
    if (value && !std::isfinite(*value)) {
      errors.push_back(field_prefix + field + " must be finite");
    }
  };
  finite(settings.temperature, "temperature");
  finite(settings.top_p, "topP");
  finite(settings.min_p, "minP");
  finite(settings.repeat_penalty, "repeatPenalty");
  finite(settings.presence_penalty, "presencePenalty");
  finite(settings.frequency_penalty, "frequencyPenalty");

  if (settings.temperature && *settings.temperature < 0.0) {
    errors.push_back(field_prefix + "temperature must be >= 0");
  }
  if (settings.top_k && *settings.top_k < 0) {
    errors.push_back(field_prefix + "topK must be >= 0");
  }
  if (settings.top_p && (*settings.top_p < 0.0 || *settings.top_p > 1.0)) {
    errors.push_back(field_prefix + "topP must be between 0 and 1");
  }
  if (settings.min_p && (*settings.min_p < 0.0 || *settings.min_p > 1.0)) {
    errors.push_back(field_prefix + "minP must be between 0 and 1");
  }
  if (settings.min_keep && *settings.min_keep < 1) {
    errors.push_back(field_prefix + "minKeep must be >= 1");
  }
  if (settings.max_output_tokens && *settings.max_output_tokens < 0) {
    errors.push_back(field_prefix + "maxOutputTokens must be >= 0");
  }
  if (settings.repeat_penalty && *settings.repeat_penalty < 0.0) {
    errors.push_back(field_prefix + "repeatPenalty must be >= 0");
  }
  return errors;
}

std::vector<std::string> validate(const LlamaSettings& settings) {
  std::vector<std::string> errors;

  if (settings.backend.main_gpu && *settings.backend.main_gpu < 0) {
    errors.push_back("llama.backend.mainGpu must be >= 0");
  }
  if (settings.backend.split_mode) {
    const std::string value = lowercase_copy(trim_copy(*settings.backend.split_mode));
    if (!is_one_of(value, {"none", "layer", "row"})) {
      errors.push_back(
          "llama.backend.splitMode must be one of: none, layer, row");
    }
  }
  if (settings.backend.tensor_split_configured) {
    for (std::size_t index = 0; index < settings.backend.tensor_split.size();
         ++index) {
      const double value = settings.backend.tensor_split[index];
      if (!std::isfinite(value) || value < 0.0) {
        errors.push_back("llama.backend.tensorSplit[" +
                         std::to_string(index) + "] must be finite and >= 0");
      }
    }
  }

  const auto validate_nonnegative_int64 =
      [&](std::optional<std::int64_t> value, const char* field) {
        if (value && *value < 0) {
          errors.push_back(std::string("llama.context.") + field + " must be >= 0");
        }
      };
  validate_nonnegative_int64(settings.context.context_size, "contextSize");
  validate_nonnegative_int64(settings.context.batch_size, "batchSize");
  validate_nonnegative_int64(settings.context.ubatch_size, "ubatchSize");
  if (settings.context.threads && *settings.context.threads < 0) {
    errors.push_back("llama.context.threads must be >= 0");
  }
  if (settings.context.batch_threads && *settings.context.batch_threads < 0) {
    errors.push_back("llama.context.batchThreads must be >= 0");
  }

  append_messages(errors, validate(settings.generation, "llama.generation."));

  if (settings.kv_cache.policy) {
    const std::string value = lowercase_copy(trim_copy(*settings.kv_cache.policy));
    if (!is_one_of(value, {"off", "turn", "session"})) {
      errors.push_back("llama.kvCache.policy must be one of: off, turn, session");
    }
  }
  if (settings.kv_cache.max_sessions && *settings.kv_cache.max_sessions < 0) {
    errors.push_back("llama.kvCache.maxSessions must be >= 0");
  }

  if (settings.grammar.root && trim_copy(*settings.grammar.root).empty()) {
    errors.push_back("llama.grammar.root must not be empty");
  }
  if (settings.grammar.default_grammar &&
      trim_copy(*settings.grammar.default_grammar).empty()) {
    errors.push_back("llama.grammar.default must not be empty when set");
  }
  for (const auto& [name, path] : settings.grammar.files) {
    if (trim_copy(name).empty()) {
      errors.push_back("llama.grammar.files keys must not be empty");
    }
    if (trim_copy(path).empty()) {
      errors.push_back("llama.grammar.files." + name + " must not be empty");
    }
  }

  if (settings.embeddings.pooling) {
    const std::string value = lowercase_copy(trim_copy(*settings.embeddings.pooling));
    if (!is_one_of(value, {"none", "mean", "cls", "last", "rank"})) {
      errors.push_back(
          "llama.embeddings.pooling must be one of: none, mean, cls, last, rank");
    }
  }
  if (settings.selected_model_id &&
      trim_copy(*settings.selected_model_id).empty()) {
    errors.push_back("llama.selectedModel must not be empty when set");
  }

  std::set<std::string> model_ids;
  for (std::size_t index = 0; index < settings.models.size(); ++index) {
    const auto& model = settings.models[index];
    const std::string prefix =
        "llama.models[" + std::to_string(index) + "].";
    const std::string id = trim_copy(model.id);
    if (id.empty()) {
      errors.push_back(prefix + "id is required");
    } else {
      const std::string normalized_id = lowercase_copy(id);
      if (!model_ids.insert(normalized_id).second) {
        errors.push_back("duplicate llama model id: " + id);
      }
    }
    if (trim_copy(model.name).empty()) {
      errors.push_back(prefix + "name is required");
    }
    if (trim_copy(model.path).empty()) {
      errors.push_back(prefix + "path is required");
    }
    if (model.block_count && *model.block_count <= 0) {
      errors.push_back(prefix + "blockCount must be > 0");
    }
    if (model.context_window && *model.context_window < 0) {
      errors.push_back(prefix + "contextWindow must be >= 0");
    }
    if (model.max_tokens && *model.max_tokens < 0) {
      errors.push_back(prefix + "maxTokens must be >= 0");
    }
    append_messages(errors, validate(model.generation, prefix + "generation."));
  }

  return errors;
}

ModelCatalog model_catalog_from_llama_settings(const LlamaSettings& settings) {
  ModelCatalog catalog;
  if (settings.models.empty()) {
    return catalog;
  }

  ModelCatalogProviderRecord provider;
  provider.descriptor.id = kLlamaProviderId;
  provider.descriptor.display_name = "llama.cpp";
  provider.descriptor.api = kLlamaProviderApi;
  provider.descriptor.transport = ProviderTransport::auto_detect;
  provider.descriptor.capabilities = llama_capabilities(settings);
  provider.metadata = Json{{"local", true},
                           {"direct", true},
                           {"source", "settings.llama"},
                           {"api", kLlamaProviderApi},
                           {"backend", settings.backend},
                           {"context", settings.context},
                           {"generation", settings.generation},
                           {"kvCache", settings.kv_cache},
                           {"grammar", settings.grammar},
                           {"embeddings", settings.embeddings}};
  catalog.providers.push_back(std::move(provider));

  for (const auto& model : settings.models) {
    ModelCatalogModelRecord record;
    record.ref = ModelRef{kLlamaProviderId, kLlamaProviderApi, model.id};
    record.display_name = model.name;
    record.capabilities = llama_capabilities(settings, &model);
    record.metadata = Json{{"input", Json::array({"text"})},
                           {"cost", zero_cost_metadata()},
                           {"path", model.path},
                           {"local", true},
                           {"direct", true},
                           {"llama", Json{{"model", model},
                                           {"generation",
                                            model_generation_json(settings, model)}}}};
    if (const auto context_window = effective_context_window(settings, model)) {
      record.metadata["contextWindow"] = *context_window;
    }
    if (const auto max_tokens = effective_max_tokens(settings, model)) {
      record.metadata["maxTokens"] = *max_tokens;
    }
    if (model.supports_embeddings.value_or(
            settings.embeddings.enabled.value_or(false))) {
      record.metadata["supportsEmbeddings"] = true;
    }
    catalog.models.push_back(std::move(record));
  }

  return normalized(catalog);
}

void to_json(Json& json, const LlamaGenerationSettings& settings) {
  json = Json::object();
  optional_to_json(json, "temperature", settings.temperature);
  optional_to_json(json, "topK", settings.top_k);
  optional_to_json(json, "topP", settings.top_p);
  optional_to_json(json, "minP", settings.min_p);
  optional_to_json(json, "minKeep", settings.min_keep);
  optional_to_json(json, "seed", settings.seed);
  optional_to_json(json, "maxOutputTokens", settings.max_output_tokens);
  optional_to_json(json, "repeatPenalty", settings.repeat_penalty);
  optional_to_json(json, "presencePenalty", settings.presence_penalty);
  optional_to_json(json, "frequencyPenalty", settings.frequency_penalty);
}

void to_json(Json& json, const LlamaBackendSettings& settings) {
  json = Json::object();
  optional_to_json(json, "enabled", settings.enabled);
  optional_to_json(json, "gpuLayers", settings.gpu_layers);
  optional_to_json(json, "mainGpu", settings.main_gpu);
  optional_to_json(json, "splitMode", settings.split_mode);
  if (settings.tensor_split_configured) {
    json["tensorSplit"] = settings.tensor_split;
  }
  optional_to_json(json, "useMmap", settings.use_mmap);
  optional_to_json(json, "useMlock", settings.use_mlock);
  optional_to_json(json, "offloadKqv", settings.offload_kqv);
}

void to_json(Json& json, const LlamaContextSettings& settings) {
  json = Json::object();
  optional_to_json(json, "contextSize", settings.context_size);
  optional_to_json(json, "batchSize", settings.batch_size);
  optional_to_json(json, "ubatchSize", settings.ubatch_size);
  optional_to_json(json, "threads", settings.threads);
  optional_to_json(json, "batchThreads", settings.batch_threads);
}

void to_json(Json& json, const LlamaKvCacheSettings& settings) {
  json = Json::object();
  optional_to_json(json, "policy", settings.policy);
  optional_to_json(json, "maxSessions", settings.max_sessions);
  optional_to_json(json, "clearOnModelChange", settings.clear_on_model_change);
}

void to_json(Json& json, const LlamaGrammarSettings& settings) {
  json = Json::object();
  optional_to_json(json, "root", settings.root);
  optional_to_json(json, "default", settings.default_grammar);
  if (settings.files_configured) {
    json["files"] = settings.files;
  }
}

void to_json(Json& json, const LlamaEmbeddingSettings& settings) {
  json = Json::object();
  optional_to_json(json, "enabled", settings.enabled);
  optional_to_json(json, "pooling", settings.pooling);
  optional_to_json(json, "normalize", settings.normalize);
}

void to_json(Json& json, const LlamaModelSettings& settings) {
  json = Json::object();
  json["id"] = settings.id;
  json["name"] = settings.name;
  json["path"] = settings.path;
  optional_to_json(json, "blockCount", settings.block_count);
  optional_to_json(json, "contextWindow", settings.context_window);
  optional_to_json(json, "maxTokens", settings.max_tokens);
  optional_to_json(json, "supportsEmbeddings", settings.supports_embeddings);
  if (!empty(settings.generation)) {
    json["generation"] = settings.generation;
  }
}

void to_json(Json& json, const LlamaSettings& settings) {
  json = Json::object();
  optional_to_json(json, "selectedModel", settings.selected_model_id);
  if (!empty(settings.backend)) {
    json["backend"] = settings.backend;
  }
  if (!empty(settings.context)) {
    json["context"] = settings.context;
  }
  if (!empty(settings.generation)) {
    json["generation"] = settings.generation;
  }
  if (!empty(settings.kv_cache)) {
    json["kvCache"] = settings.kv_cache;
  }
  if (!empty(settings.grammar)) {
    json["grammar"] = settings.grammar;
  }
  if (!empty(settings.embeddings)) {
    json["embeddings"] = settings.embeddings;
  }
  if (settings.models_configured) {
    json["models"] = settings.models;
  }
}

}  // namespace qt::ai
