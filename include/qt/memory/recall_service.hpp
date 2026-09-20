#pragma once

#include "qt/memory/governance_service.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace qt::memory {

enum class GraphDirection { Outgoing, Incoming, Both };

struct GraphRecallOptions {
  bool enabled = false;
  std::vector<std::string> seed_memory_ids;
  std::size_t max_depth = 2;
  std::size_t max_fanout = 16;
  std::size_t max_seeds = 16;
  std::size_t max_edges = 256;
  std::size_t max_results = 64;
  GraphDirection direction = GraphDirection::Both;
  std::vector<std::string> allowed_relation_types;
  bool include_connecting_memories = true;
};

struct StructuredRecallOptions {
  bool enabled = false;
  std::string subject;
  std::string predicate;
  std::string object;
  std::optional<TimePoint> as_of;
  std::size_t limit = 32;
};

struct RecallRequest {
  std::string query;
  std::vector<std::string> tags;
  RecallPolicy policy = RecallPolicy::Balanced;
  bool include_global_user = false;
  bool include_candidates = true;
  std::size_t candidate_limit = 50;
  std::size_t result_limit = 20;
  GraphRecallOptions graph;
  StructuredRecallOptions structured;
  bool dense_enabled = false;
  std::function<bool()> cancelled;
};

using DenseCandidateProvider =
    std::function<std::vector<DenseMemoryCandidate>(const MemoryScope&, const RecallRequest&)>;

class RecallService {
 public:
  explicit RecallService(MemoryRepository& memories) : memories_(memories) {}
  RecallService(MemoryRepository& memories, GraphRepository& graph)
      : memories_(memories), graph_(&graph) {}
  RecallService(MemoryRepository& memories, GraphRepository& graph,
                SourceRepository& sources)
      : memories_(memories), graph_(&graph), sources_(&sources) {}
  RecallService(MemoryRepository& memories, GraphRepository& graph,
                SourceRepository& sources, ReviewRepository& reviews)
      : memories_(memories),
        graph_(&graph),
        sources_(&sources),
        reviews_(&reviews) {}
  RecallService(MemoryRepository& memories, GraphRepository& graph,
                SourceRepository& sources, ReviewRepository& reviews,
                StructuredMemoryRepository& structured,
                DenseCandidateProvider dense = {})
      : memories_(memories),
        graph_(&graph),
        sources_(&sources),
        reviews_(&reviews),
        structured_(&structured), dense_(std::move(dense)) {}
  RecallService(MemoryRepository& memories, DenseCandidateProvider dense)
      : memories_(memories), dense_(std::move(dense)) {}

  std::vector<RecalledMemory> recall(const MemoryScope& scope,
                                     const RecallRequest& request) const;
  bool eligible_for_context(const MemoryScope& scope,
                            const std::string& memory_id,
                            RecallPolicy policy,
                            bool include_global_user) const;
  void record_access(const MemoryScope& scope,
                     const std::vector<RecalledMemory>& memories);

 private:
  static double hashed_similarity(const std::string& lhs,
                                  const std::string& rhs);
  std::vector<std::string> warnings_for(const MemoryScope& scope,
                                        const MemoryEntry& memory) const;
  std::optional<std::string> source_drift_warning(
      const MemoryEntry& memory) const;

  MemoryRepository& memories_;
  GraphRepository* graph_ = nullptr;
  SourceRepository* sources_ = nullptr;
  ReviewRepository* reviews_ = nullptr;
  StructuredMemoryRepository* structured_ = nullptr;
  DenseCandidateProvider dense_;
};

}  // namespace qt::memory
