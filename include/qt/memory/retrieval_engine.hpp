#pragma once

#include "qt/memory/dense_retrieval.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace qt::memory {

struct RetrievalEngineOptions {
  bool enabled = false;
  bool background_indexing = true;
  bool dense_enabled = false;
  bool code_enabled = false;
  bool code_graph_enabled = false;
  std::size_t graph_max_seeds = 16;
  std::size_t graph_max_depth = 2;
  std::size_t graph_max_fanout = 16;
  std::size_t graph_max_edges = 256;
  std::size_t graph_max_results = 64;
  std::function<std::shared_ptr<const index::RetrievalSnapshot>()> code_snapshot;
  std::function<std::vector<RetrievalReference>(const std::string&, std::size_t)> code_lexical;
  std::size_t cache_entries = 64;
  RecallPolicy policy = RecallPolicy::Balanced;
  bool include_global_user = false;
  RetrievalEmbedder embedder;
  std::function<DenseIndexConfig(const RetrievalCancellation&)> configure;
  std::function<void()> validate_embedding;
};

// Effective code-index freshness. `enabled` reports a configured adapter;
// `available` reports a present snapshot and `stale` reports whether it must be
// rescanned. `generation` is the pinned whole-snapshot generation and changes
// only when a scan publishes.
struct RetrievalCodeStatus {
  bool enabled = false;
  bool available = false;
  bool stale = false;
  std::uint64_t generation = 0;
  std::string repository;
  std::string worktree;
};

struct RetrievalEngineStatus {
  bool enabled = false;
  bool running = false;
  std::string generation;
  std::string last_error;
  std::size_t completed_jobs = 0;
  RetrievalIndexStatus jobs;
  DenseRetrievalStats queries;
  RetrievalCodeStatus code;
};

// Owns indexing lifetime; all borrowed database access ends before stop returns.
class RetrievalEngine final {
 public:
  RetrievalEngine(SQLiteMemoryDatabase&, MemoryScope, RetrievalEngineOptions);
  ~RetrievalEngine();
  RetrievalEngine(const RetrievalEngine&) = delete;
  RetrievalEngine& operator=(const RetrievalEngine&) = delete;
  void stop();
  void set_scope(MemoryScope);
  void set_foreground_active(bool);
  // Row-only convenience query. Recall must use search_candidates to retain
  // the captured representation revision and evidence.
  std::vector<MemoryEntry> search(const MemoryScope&, const std::string&,
      RecallPolicy, bool include_global_user, std::size_t limit,
      const RetrievalCancellation& = {});
  std::vector<DenseMemoryCandidate> search_candidates(const MemoryScope&, const std::string&,
      RecallPolicy, bool include_global_user, std::size_t limit,
      const RetrievalCancellation& = {});
  RetrievalEngineStatus status() const;
  std::vector<RetrievalCandidate> search_code(const MemoryScope&, const std::string&,
      std::size_t limit, const RetrievalCancellation& = {},
      const std::vector<RetrievalReference>& memory_seeds = {},
      const std::vector<RetrievalCandidate>& contextual_seeds = {},
      std::optional<std::pair<RecallPolicy, bool>> access = std::nullopt);
  bool validate_code(const MemoryScope&, const RetrievalReference&) const;
  bool validate_relation(const MemoryScope&, const RetrievalPathEdge&,
                         RecallPolicy, bool include_global_user) const;

 private:
  void prepare_embedding(const RetrievalCancellation&);
  void run();
  MemoryScope code_scope(const MemoryScope&, const index::RetrievalSnapshot&) const;
  SQLiteMemoryDatabase& database_;
  RetrievalEngineOptions options_;
  RetrievalIndexer indexer_;
  DenseRetrievalService dense_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  MemoryScope scope_;
  RetrievalEngineStatus status_;
  bool foreground_active_ = false;
  std::atomic<bool> stopping_{false};
  std::thread worker_;
};

}  // namespace qt::memory
