#include "qt/app/runtime/memory_preview.hpp"

#include "index/retrieval_snapshot.hpp"
#include "index/sqlite_store.hpp"
#include "qt/ai/llama_provider.hpp"
#include "qt/app/config/settings.hpp"
#include "qt/memory/llama_retrieval.hpp"
#include "qt/memory/runtime.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qt::app::runtime {
namespace {

std::filesystem::path index_database_path(const AppSettings& settings) {
  const std::filesystem::path project_root =
      settings.workspace_root.empty() ? settings.cwd : settings.workspace_root;
  const std::filesystem::path state_root =
      settings.agent_home.empty()
          ? project_root / ".index"
          : std::filesystem::path(settings.agent_home) / "index";
  const auto project_key = std::to_string(std::hash<std::string>{}(
      std::filesystem::absolute(project_root).generic_string()));
  return qt::index::sqlite_index_database_path(state_root / project_key);
}

std::string lexical_query(const std::string& query) {
  std::string result;
  std::string word;
  std::size_t words = 0;
  const auto flush = [&] {
    if (word.empty() || words >= 32U) {
      word.clear();
      return;
    }
    if (!result.empty()) result += " OR ";
    result += "\"" + word + "\"";
    word.clear();
    ++words;
  };
  for (const unsigned char ch : query.substr(0, 16384U)) {
    if (std::isalnum(ch) || ch == '_' || ch >= 128U)
      word.push_back(static_cast<char>(ch));
    else
      flush();
  }
  flush();
  return result;
}

}  // namespace

MemoryPreviewServices make_memory_preview_services(
    const AppSettings& settings, const qt::memory::MemoryScope& scope) {
  MemoryPreviewServices result;
  qt::memory::MemoryRuntimeOptions options;
  options.agent_home = settings.agent_home;
  options.cwd = settings.cwd;
  options.initial_scope = scope;
  options.background_processing = false;
  options.automatic_capture = false;
  options.dreaming.enabled = false;
  options.coordinator.enabled = true;
  options.coordinator.automatic_recall = false;
  options.coordinator.context_token_budget = settings.memory.context_token_budget;
  options.coordinator.candidate_limit = settings.memory.candidate_limit;
  options.coordinator.recall_policy =
      settings.memory.recall_policy == "strict"
          ? qt::memory::RecallPolicy::Strict
          : settings.memory.recall_policy == "exploratory" ||
                    settings.memory.recall_policy == "broad"
                ? qt::memory::RecallPolicy::Broad
                : qt::memory::RecallPolicy::Balanced;
  options.coordinator.include_global_user = false;

  const auto& retrieval = settings.memory.retrieval;
  options.coordinator.graph.enabled = retrieval.graph_enabled;
  options.coordinator.graph.max_depth = retrieval.max_depth;
  options.coordinator.graph.max_seeds = retrieval.max_seeds;
  options.coordinator.graph.max_edges = retrieval.max_edges;
  options.coordinator.graph.max_results = retrieval.max_graph_results;
  options.coordinator.graph.max_fanout = retrieval.max_fanout;
  options.coordinator.dense_enabled = retrieval.dense_enabled;
  options.retrieval.enabled = retrieval.graph_enabled || retrieval.dense_enabled ||
                              retrieval.code_enabled;
  options.retrieval.background_indexing = false;
  options.retrieval.dense_enabled = retrieval.dense_enabled;
  options.retrieval.code_enabled = false;
  options.retrieval.code_graph_enabled = false;
  options.retrieval.policy = options.coordinator.recall_policy;
  options.retrieval.include_global_user = false;
  options.retrieval.cache_entries = retrieval.query_cache_entries;
  options.retrieval.graph_max_seeds = retrieval.max_seeds;
  options.retrieval.graph_max_depth = retrieval.max_depth;
  options.retrieval.graph_max_edges = retrieval.max_edges;
  options.retrieval.graph_max_results = retrieval.max_graph_results;
  options.retrieval.graph_max_fanout = retrieval.max_fanout;

  if (retrieval.code_enabled) {
    std::string error;
    const auto loaded = qt::index::load_sqlite_index_snapshot(
        index_database_path(settings),
        settings.workspace_root.empty() ? std::filesystem::path(settings.cwd)
                                        : std::filesystem::path(settings.workspace_root),
        &error);
    bool complete = loaded && loaded->scan_fingerprints_complete;
    if (complete) {
      for (const auto& file : loaded->scan.files) {
        const auto fingerprint = loaded->scan_fingerprints.find(
            file.path.generic_string());
        if (fingerprint == loaded->scan_fingerprints.end() ||
            fingerprint->second.empty()) {
          complete = false;
          break;
        }
      }
    }
    if (!complete) {
      result.warnings.push_back(
          "code retrieval unavailable: persisted index fingerprints are incomplete; rescan needed");
    } else {
      const auto snapshot = qt::index::RetrievalSnapshot::build(
          loaded->scan, loaded->code_index, loaded->generation,
          loaded->scan_fingerprints, false);
      options.retrieval.code_enabled = true;
      options.retrieval.code_graph_enabled = retrieval.graph_enabled;
      options.retrieval.code_snapshot = [snapshot] { return snapshot; };
      const auto database_path = index_database_path(settings);
      const auto project_root = settings.workspace_root.empty()
                                    ? std::filesystem::path(settings.cwd)
                                    : std::filesystem::path(settings.workspace_root);
      options.retrieval.code_lexical =
          [snapshot, database_path, project_root](const std::string& query,
                                                 std::size_t limit) {
            std::vector<qt::memory::RetrievalReference> references;
            std::unordered_set<std::string> seen;
            const auto add_node = [&](const qt::index::RetrievalCodeNode& node) {
              if (references.size() >= limit || !seen.insert(node.id).second) return;
              try {
                if (qt::index::retrieval_source_fingerprint(
                        snapshot->root() / node.path) != node.source_fingerprint)
                  return;
              } catch (...) {
                return;
              }
              references.push_back({qt::memory::RetrievalOwnerKind::Code, node.id,
                  0, node.repository, node.worktree, node.path.generic_string(),
                  node.source_fingerprint, node.generation});
            };
            auto exact = snapshot->find_symbols(query);
            std::sort(exact.begin(), exact.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
            for (const auto& node : exact) add_node(node);
            const auto terms = lexical_query(query);
            if (!terms.empty()) {
              const auto payload = qt::index::sqlite_fts_search_payload_pinned(
                  database_path, project_root, terms, limit, snapshot->generation());
              if (payload.is_object() && payload.contains("matches")) {
                for (const auto& match : payload.at("matches")) {
                  const auto path = match.value("path", std::string{});
                  for (const auto& node : snapshot->nodes())
                    if (node.kind == qt::index::RetrievalCodeNodeKind::File &&
                        node.path.generic_string() == path) add_node(node);
                }
              }
            }
            if (references.size() > limit) references.resize(limit);
            return references;
          };
    }
    if (!loaded && !error.empty())
      result.warnings.push_back("code retrieval unavailable: " + error);
  }

  if (retrieval.dense_enabled) try {
    const auto provider = std::dynamic_pointer_cast<qt::ai::LlamaProvider>(
        qt::ai::make_llama_provider(settings.llama));
    if (!retrieval.embedding_model) {
      result.warnings.push_back(
          "dense retrieval unavailable: memory.retrieval.embeddingModel is not configured");
      options.retrieval.configure = [](const qt::memory::RetrievalCancellation&) -> qt::memory::DenseIndexConfig {
        throw std::runtime_error("dense retrieval embedding model is not configured");
      };
    } else {
      const auto model = std::find_if(
          settings.llama.models.begin(), settings.llama.models.end(),
          [&](const auto& candidate) { return candidate.id == *retrieval.embedding_model; });
      if (model == settings.llama.models.end()) {
        result.warnings.push_back("dense retrieval unavailable: configured embedding model '" +
                                 *retrieval.embedding_model + "' is missing");
        options.retrieval.configure = [](const qt::memory::RetrievalCancellation&) -> qt::memory::DenseIndexConfig {
          throw std::runtime_error("configured dense embedding model is missing");
        };
      } else if (!provider) {
        result.warnings.push_back(
            "dense retrieval unavailable: llama provider could not be initialized");
        options.retrieval.configure = [](const qt::memory::RetrievalCancellation&) -> qt::memory::DenseIndexConfig {
          throw std::runtime_error("llama provider could not be initialized");
        };
      } else {
        qt::memory::bind_llama_retrieval(
            options.retrieval, provider, *model, retrieval.query_prefix,
            retrieval.document_prefix,
            settings.llama.embeddings.normalize.value_or(true));
      }
    }
  }

  catch (const std::exception& error) {
    const std::string diagnostic = std::string("dense retrieval unavailable: ") + error.what();
    result.warnings.push_back(diagnostic);
    options.retrieval.configure = [diagnostic](const qt::memory::RetrievalCancellation&) -> qt::memory::DenseIndexConfig {
      throw std::runtime_error(diagnostic);
    };
  }

  result.runtime = std::make_unique<qt::memory::MemoryRuntime>(std::move(options));
  return result;
}

}  // namespace qt::app::runtime
