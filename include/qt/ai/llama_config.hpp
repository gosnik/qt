#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/model_catalog.hpp"

namespace qt::ai {

constexpr const char* kLlamaProviderId = "llama";
constexpr const char* kLlamaProviderApi = "llama.cpp";

struct LlamaGenerationSettings {
  std::optional<double> temperature;
  std::optional<int> top_k;
  std::optional<double> top_p;
  std::optional<double> min_p;
  std::optional<int> min_keep;
  std::optional<std::uint32_t> seed;
  std::optional<std::int64_t> max_output_tokens;
  std::optional<double> repeat_penalty;
  std::optional<double> presence_penalty;
  std::optional<double> frequency_penalty;
};

struct LlamaBackendSettings {
  std::optional<bool> enabled;
  std::optional<int> gpu_layers;
  std::optional<int> main_gpu;
  std::optional<std::string> split_mode;
  std::vector<double> tensor_split;
  bool tensor_split_configured = false;
  std::optional<bool> use_mmap;
  std::optional<bool> use_mlock;
  std::optional<bool> offload_kqv;
};

struct LlamaContextSettings {
  std::optional<std::int64_t> context_size;
  std::optional<std::int64_t> batch_size;
  std::optional<std::int64_t> ubatch_size;
  std::optional<int> threads;
  std::optional<int> batch_threads;
};

struct LlamaKvCacheSettings {
  std::optional<std::string> policy;
  std::optional<int> max_sessions;
  std::optional<bool> clear_on_model_change;
};

struct LlamaGrammarSettings {
  std::optional<std::string> root;
  std::optional<std::string> default_grammar;
  std::map<std::string, std::string> files;
  bool files_configured = false;
};

struct LlamaEmbeddingSettings {
  std::optional<bool> enabled;
  std::optional<std::string> pooling;
  std::optional<bool> normalize;
};

struct LlamaModelSettings {
  std::string id;
  std::string name;
  std::string path;
  std::optional<std::int64_t> block_count;
  std::optional<std::int64_t> context_window;
  std::optional<std::int64_t> max_tokens;
  std::optional<bool> supports_embeddings;
  LlamaGenerationSettings generation;
};

struct LlamaSettings {
  LlamaBackendSettings backend;
  LlamaContextSettings context;
  LlamaGenerationSettings generation;
  LlamaKvCacheSettings kv_cache;
  LlamaGrammarSettings grammar;
  LlamaEmbeddingSettings embeddings;
  std::optional<std::string> selected_model_id;
  std::vector<LlamaModelSettings> models;
  bool models_configured = false;
};

bool empty(const LlamaGenerationSettings& settings) noexcept;
bool empty(const LlamaBackendSettings& settings) noexcept;
bool empty(const LlamaContextSettings& settings) noexcept;
bool empty(const LlamaKvCacheSettings& settings) noexcept;
bool empty(const LlamaGrammarSettings& settings) noexcept;
bool empty(const LlamaEmbeddingSettings& settings) noexcept;
bool empty(const LlamaModelSettings& settings) noexcept;
bool empty(const LlamaSettings& settings) noexcept;

LlamaGenerationSettings merge_llama_generation_settings(
    const LlamaGenerationSettings& base,
    const LlamaGenerationSettings& overlay);
LlamaSettings merge_llama_settings(const LlamaSettings& base,
                                   const LlamaSettings& overlay);

std::vector<std::string> validate(const LlamaGenerationSettings& settings,
                                  const std::string& field_prefix);
std::vector<std::string> validate(const LlamaSettings& settings);

ModelCatalog model_catalog_from_llama_settings(const LlamaSettings& settings);

void to_json(Json& json, const LlamaGenerationSettings& settings);
void to_json(Json& json, const LlamaBackendSettings& settings);
void to_json(Json& json, const LlamaContextSettings& settings);
void to_json(Json& json, const LlamaKvCacheSettings& settings);
void to_json(Json& json, const LlamaGrammarSettings& settings);
void to_json(Json& json, const LlamaEmbeddingSettings& settings);
void to_json(Json& json, const LlamaModelSettings& settings);
void to_json(Json& json, const LlamaSettings& settings);

}  // namespace qt::ai
