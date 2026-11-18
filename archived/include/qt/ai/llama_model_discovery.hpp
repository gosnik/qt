#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/llama_config.hpp"
#include "qt/ai/model_catalog.hpp"

namespace qt::ai {

enum class LlamaDiscoveredModelCapability {
  embedding,
  reranker,
  sentence_handler,
  chat,
  other,
};

enum class LlamaMemoryTask {
  document_embedding,
  short_embedding,
  worthiness,
  extraction,
  recall_prehandler,
  context_compilation,
};

struct LlamaModelDiscoveryOptions {
  std::vector<std::string> roots;
  bool recursive = true;
};

struct LlamaDiscoveredModel {
  std::string id;
  std::string name;
  std::string path;
  std::string format = "gguf";
  LlamaDiscoveredModelCapability capability =
      LlamaDiscoveredModelCapability::other;
  std::string best_usage;
  std::string quantization;
  std::string quantization_preference;
  std::optional<std::int64_t> block_count;
  std::optional<std::int64_t> context_length;
  std::optional<std::int64_t> embedding_length;
  std::optional<std::int64_t> gguf_file_type;
  Json gguf_metadata = Json::object();
  std::vector<std::string> matched_patterns;
  std::vector<std::string> usage_notes;
  std::vector<std::string> warnings;
};

struct LlamaModelDiscoveryResult {
  std::vector<LlamaDiscoveredModel> models;
  std::vector<std::string> diagnostics;
};

std::string to_string(LlamaDiscoveredModelCapability capability);
std::string to_string(LlamaMemoryTask task);

// Selects a configured, readable local GGUF for a memory-specific task. The
// ordering is deterministic and intentionally prefers quantized task models
// over larger general chat models.
const LlamaModelSettings* select_llama_model_for_memory_task(
    const LlamaSettings& settings, LlamaMemoryTask task);

LlamaModelDiscoveryResult discover_llama_models(
    const LlamaModelDiscoveryOptions& options);

LlamaModelSettings llama_model_settings_from_discovery(
    const LlamaDiscoveredModel& model);

ModelCatalog model_catalog_from_llama_discovery(
    const LlamaModelDiscoveryResult& discovery,
    const LlamaSettings* settings = nullptr);

void enrich_model_catalog_with_llama_discovery(
    ModelCatalog& catalog, const LlamaModelDiscoveryResult& discovery);

void to_json(Json& json, const LlamaDiscoveredModel& model);
void to_json(Json& json, const LlamaModelDiscoveryResult& result);

}  // namespace qt::ai
