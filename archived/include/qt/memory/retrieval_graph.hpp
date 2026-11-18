#pragma once

#include "index/retrieval_snapshot.hpp"
#include "qt/memory/repositories.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qt::memory {

struct RetrievalGraphIdentity {
  std::string repository;
  std::string worktree;
};

struct CrossStoreLink {
  RetrievalReference from;
  RetrievalReference to;
  std::string relation;
  std::string evidence_id;
  std::string id;
};

class CrossStoreLinkRepository {
 public:
  virtual ~CrossStoreLinkRepository() = default;
  virtual std::vector<CrossStoreLink> links(const MemoryScope&,
                                            const RetrievalReference&,
                                            RecallPolicy, bool,
                                            std::size_t limit) const = 0;
};

enum class RetrievalGraphDirection { Outgoing, Incoming, Both };

struct RetrievalGraphSeed {
  RetrievalReference reference;
  std::size_t depth = 0;
};

struct RetrievalGraphQuery {
  std::vector<RetrievalGraphSeed> seeds;
  RecallPolicy policy = RecallPolicy::Balanced;
  bool include_global_user = false;
  std::size_t max_depth = 2;
  std::size_t max_fanout = 16;
  std::size_t max_edges = 256;
  std::size_t max_results = 64;
  RetrievalGraphDirection direction = RetrievalGraphDirection::Both;
  std::vector<std::string> allowed_relations;
  std::optional<TimePoint> as_of;
};

struct RetrievalGraphEdge {
  RetrievalReference from;
  std::vector<RetrievalReference> candidates;
  std::string relation;
  std::string evidence_id;
  std::string resolution = "resolved";
  // True when traversal reached this relation through its target endpoint.
  // The persisted relation remains oriented from its original source.
  bool incoming = false;
  bool assertion_positive = true;
  std::optional<TimePoint> valid_from;
  std::optional<TimePoint> valid_to;
  std::vector<std::string> assertion_evidence;
  std::optional<MemoryAssertion> assertion;
  bool heuristic = false;
};

struct RetrievalGraphResult {
  std::vector<RetrievalReference> nodes;
  std::vector<RetrievalGraphEdge> edges;
  std::vector<std::string> omissions;
};

class RetrievalGraphAdapter {
 public:
  using SnapshotGetter =
      std::function<std::shared_ptr<const index::RetrievalSnapshot>()>;
  using EndpointAuthorizer =
      std::function<bool(const MemoryScope&, const RetrievalReference&)>;

  RetrievalGraphAdapter(GraphRepository& graph, MemoryRepository& memories,
                        SnapshotGetter snapshot, RetrievalGraphIdentity identity,
                        CrossStoreLinkRepository* links = nullptr,
                        EndpointAuthorizer authorizer = {},
                        StructuredMemoryRepository* structured = nullptr)
      : graph_(graph), memories_(memories), snapshot_(std::move(snapshot)),
        identity_(std::move(identity)), links_(links), authorizer_(std::move(authorizer)),
        structured_(structured) {}

  RetrievalGraphResult traverse(const MemoryScope&, const RetrievalGraphQuery&) const;

 private:
  GraphRepository& graph_;
  MemoryRepository& memories_;
  SnapshotGetter snapshot_;
  RetrievalGraphIdentity identity_;
  CrossStoreLinkRepository* links_ = nullptr;
  EndpointAuthorizer authorizer_;
  StructuredMemoryRepository* structured_ = nullptr;
};

}  // namespace qt::memory
