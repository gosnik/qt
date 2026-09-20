#include "qt/memory/recall_service.hpp"
#include "qt/memory/context_service.hpp"
#include "qt/memory/scope.hpp"

#include <algorithm>
#include <cassert>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qt::memory;

class Memories final : public MemoryRepository {
 public:
  std::string add(const MemoryScope&, MemoryWrite) override { return {}; }
  std::vector<MemoryEntry> query(const MemoryScope& scope,
                                 const MemoryQuery& query) const override {
    std::vector<MemoryEntry> result;
    for (const auto& memory : values) {
      if (!scope_can_access(scope, memory.scope, query.policy,
                            query.include_global_user)) continue;
      if (!query.text.empty() && memory.content.find(query.text) == std::string::npos)
        continue;
      result.push_back(memory);
      if (query.limit != 0U && result.size() >= query.limit) break;
    }
    return result;
  }
  MemoryEntry get(const MemoryScope&, const std::string& id) const override {
    for (const auto& memory : values) if (memory.id == id) return memory;
    throw std::invalid_argument("missing memory");
  }
  std::optional<MemoryEntry> find_for_recall(const MemoryScope& scope,
                                              const std::string& id,
                                              RecallPolicy policy,
                                              bool include_global_user) const override {
    ++find_for_recall_calls;
    try {
      const auto value = get(scope, id);
      return scope_can_access(scope, value.scope, policy, include_global_user)
          ? std::optional<MemoryEntry>(value) : std::nullopt;
    } catch (const std::invalid_argument&) {
      return std::nullopt;
    }
  }
  MemoryEntry update(const MemoryScope&, const std::string&, const MemoryPatch&) override {
    throw std::logic_error("unused");
  }
  bool forget(const MemoryScope&, const std::string&) override { return false; }
  std::vector<MemoryRevision> revisions(const MemoryScope&, const std::string&) const override {
    return {};
  }
  std::int64_t retrieval_revision(const MemoryScope&, const std::string& id) const override {
    if (throw_retrieval_revision && id == "dense")
      throw std::runtime_error("dense revision authority unavailable");
    const auto found = retrieval_revisions.find(id);
    return found == retrieval_revisions.end() ? 0 : found->second;
  }
  void record_access(const MemoryScope&, const std::vector<std::string>&) override {}
  std::size_t size(const MemoryScope&) const override { return values.size(); }
  std::vector<MemoryEntry> values;
  std::map<std::string, std::int64_t> retrieval_revisions;
  bool throw_retrieval_revision = false;
  mutable std::size_t find_for_recall_calls = 0;
};

class Blocks final : public BlockRepository {
 public:
  ContextBlock get_block(const MemoryScope&, const std::string&) const override {
    throw std::invalid_argument("unused");
  }
  ContextBlock update_block(const MemoryScope&, const std::string&,
                            std::string) override {
    throw std::logic_error("unused");
  }
  std::vector<ContextBlock> blocks(const MemoryScope&) const override { return {}; }
};

static MemoryEntry memory(const std::string& id, const MemoryScope& scope,
                          const std::string& content, MemoryStatus status) {
  MemoryEntry value;
  value.id = id;
  value.scope = scope;
  value.content = content;
  value.status = status;
  return value;
}

static DenseMemoryCandidate dense_candidate(const MemoryEntry& memory,
                                            std::int64_t revision = 1,
                                            double similarity = 0.75) {
  DenseMemoryCandidate result;
  result.memory = memory;
  result.hit.chunk.id = "chunk:" + memory.id;
  result.hit.chunk.owner_key = "memory:" + memory.id;
  result.hit.chunk.owner_revision = revision;
  result.hit.chunk.text = memory.content;
  result.hit.chunk.content_hash = "hash:" + memory.id;
  result.hit.chunk.source_uri = "test://dense/" + memory.id;
  result.hit.chunk.source_span = "0:1";
  result.hit.generation = "test-generation";
  result.hit.similarity = similarity;
  return result;
}

int main() {
  MemoryScope scope;
  scope.user = "user";
  scope.project = "project";
  scope.repository = "repo";
  MemoryScope foreign = scope;
  foreign.user = "other";
  Memories memories;
  memories.values = {
      memory("lexical", scope, "lexical-token", MemoryStatus::Validated),
      memory("dense", scope, "semantic-only", MemoryStatus::Validated),
      memory("foreign", foreign, "semantic-only", MemoryStatus::Validated),
      memory("candidate", scope, "semantic-only", MemoryStatus::Candidate),
      memory("dense-other", scope, "another semantic result", MemoryStatus::Validated),
  };
  for (const auto& value : memories.values) memories.retrieval_revisions[value.id] = 1;

  int cancellation_checks = 0;
  DenseCandidateProvider provider = [&](const MemoryScope& requested,
                                         const RecallRequest& request) {
    assert(requested.user == scope.user && requested.project == scope.project &&
           requested.repository == scope.repository);
    assert(request.cancelled);
    ++cancellation_checks;
    assert(!request.cancelled());
    return std::vector<DenseMemoryCandidate>{dense_candidate(memories.values[1]),
        dense_candidate(memories.values[2]), dense_candidate(memories.values[3])};
  };
  RecallService service(memories, provider);
  RecallRequest request;
  request.query = "lexical-token";
  request.dense_enabled = true;
  request.include_candidates = false;
  request.candidate_limit = 8;
  request.result_limit = 8;
  request.cancelled = [] { return false; };
  const auto result = service.recall(scope, request);
  assert(cancellation_checks == 1);
  assert(result.size() == 2);
  assert(std::any_of(result.begin(), result.end(), [](const auto& item) {
    return item.memory.id == "lexical";
  }));
  const auto dense_result = std::find_if(result.begin(), result.end(), [](const auto& item) {
    return item.memory.id == "dense";
  });
  assert(dense_result != result.end() && dense_result->score.dense_rank == 1U &&
         dense_result->dense_match && dense_result->dense_match->chunk.id == "chunk:dense" &&
         dense_result->dense_match->chunk.owner_key == "memory:dense" &&
         dense_result->dense_match->chunk.owner_revision == 1 &&
         dense_result->dense_match->chunk.source_uri == "test://dense/dense" &&
         dense_result->dense_match->generation == "test-generation" &&
         dense_result->dense_match->similarity == 0.75);
  assert(std::none_of(result.begin(), result.end(), [](const auto& item) {
    return item.memory.id == "foreign" || item.memory.id == "candidate";
  }));

  // Dense providers return chunks, while recall ranks owners.  A stale chunk
  // must not reserve the owner before its current record is resolved, and
  // repeated valid chunks must neither replace that owner's first rank nor use
  // up the two-owner candidate budget.
  auto stale_dense = memories.values[1];
  stale_dense.content = "obsolete semantic payload";
  RecallService duplicate_dense(memories, DenseCandidateProvider{
      [&](const MemoryScope&, const RecallRequest&) {
        return std::vector<DenseMemoryCandidate>{dense_candidate(stale_dense),
            dense_candidate(memories.values[1]), dense_candidate(memories.values[1]),
            dense_candidate(memories.values[4]), dense_candidate(memories.values[4])};
      }});
  RecallRequest duplicate_request = request;
  duplicate_request.candidate_limit = 2;
  duplicate_request.result_limit = 8;
  const auto deduplicated = duplicate_dense.recall(scope, duplicate_request);
  const auto first_dense = std::find_if(deduplicated.begin(), deduplicated.end(), [](const auto& item) {
    return item.memory.id == "dense";
  });
  const auto second_dense = std::find_if(deduplicated.begin(), deduplicated.end(), [](const auto& item) {
    return item.memory.id == "dense-other";
  });
  assert(first_dense != deduplicated.end() && first_dense->score.dense_rank == 1U);
  assert(second_dense != deduplicated.end() && second_dense->score.dense_rank == 2U);
  assert(std::count_if(deduplicated.begin(), deduplicated.end(), [](const auto& item) {
    return item.memory.id == "dense";
  }) == 1);

  // The provider sees the same hard ceiling as the dense examination loop;
  // entries after the first thousand cannot cause extra repository resolution.
  bool bounded_provider = false;
  RecallService bounded_dense(memories, DenseCandidateProvider{
      [&](const MemoryScope&, const RecallRequest& bounded_request) {
        bounded_provider = bounded_request.candidate_limit == 1000U;
        std::vector<DenseMemoryCandidate> missing(1001);
        for (std::size_t i = 0; i < missing.size(); ++i)
          missing[i] = dense_candidate(memory("missing-dense-" + std::to_string(i), scope,
                                              "missing", MemoryStatus::Validated));
        return missing;
      }});
  RecallRequest oversized_request = request;
  oversized_request.candidate_limit = 1001;
  oversized_request.result_limit = 1001;
  memories.find_for_recall_calls = 0;
  const auto bounded_result = bounded_dense.recall(scope, oversized_request);
  assert(bounded_provider && memories.find_for_recall_calls == 1000U);
  assert(bounded_result.size() == 1 && bounded_result.front().memory.id == "lexical");

  // A provider may finish with a captured representation after its owner has changed.
  const auto captured_dense = memories.values[1];
  const auto captured_candidate = dense_candidate(captured_dense);
  RecallService stale(memories, DenseCandidateProvider{
      [&](const MemoryScope&, const RecallRequest&) {
        return std::vector<DenseMemoryCandidate>{captured_candidate};
      }});
  for (const bool graph : {false, true}) {
    request.graph.enabled = graph;
    memories.values[1].status = MemoryStatus::Superseded;
    auto current = stale.recall(scope, request);
    assert(current.size() == 1 && current.front().memory.id == "lexical");
    memories.values[1] = captured_dense;
    memories.values[1].content = "replacement prose without the old semantics";
    current = stale.recall(scope, request);
    assert(current.size() == 1 && current.front().memory.id == "lexical");
    memories.values[1] = captured_dense;
    // Revision changes invalidate captured dense evidence even if the complete
    // record snapshot has the same content and timestamp.
    memories.retrieval_revisions["dense"] = 2;
    current = stale.recall(scope, request);
    assert(current.size() == 1 && current.front().memory.id == "lexical");
    memories.retrieval_revisions["dense"] = 1;
    current = stale.recall(scope, request);
    assert(current.size() == 2);
  }
  request.graph.enabled = false;

  // A lexical owner that is also a verified dense candidate retains the chunk
  // evidence when the two retrievers merge into one result.
  RecallRequest merged_request = request;
  merged_request.query = "semantic-only";
  const auto merged = service.recall(scope, merged_request);
  const auto merged_dense = std::find_if(merged.begin(), merged.end(), [](const auto& item) {
    return item.memory.id == "dense";
  });
  assert(merged_dense != merged.end() && merged_dense->score.lexical_rank != 0U &&
         merged_dense->score.dense_rank == 1U && merged_dense->dense_match &&
         merged_dense->dense_match->chunk.id == "chunk:dense");

  // Repositories without revision authority cannot authorize dense evidence.
  memories.retrieval_revisions["dense"] = 0;
  const auto no_revision = service.recall(scope, request);
  assert(no_revision.size() == 1 && no_revision.front().memory.id == "lexical");
  memories.retrieval_revisions["dense"] = 1;

  // Authority failures are scoped to the candidate. A broken dense owner must
  // not prevent a later, independent lexical+dense owner from being recalled
  // or from retaining its matched chunk evidence.
  RecallService authority_race(memories, DenseCandidateProvider{
      [&](const MemoryScope&, const RecallRequest&) {
        return std::vector<DenseMemoryCandidate>{dense_candidate(memories.values[1]),
            dense_candidate(memories.values[0])};
      }});
  const auto previously_recalled = authority_race.recall(scope, request);
  memories.throw_retrieval_revision = true;
  const auto raced = authority_race.recall(scope, request);
  const auto raced_lexical = std::find_if(raced.begin(), raced.end(), [](const auto& item) {
    return item.memory.id == "lexical";
  });
  assert(raced.size() == 1 && raced_lexical != raced.end() &&
         raced_lexical->score.dense_rank == 1U && raced_lexical->dense_match &&
         raced_lexical->dense_match->chunk.id == "chunk:lexical");

  // Final context validation must independently omit the same failed owner
  // from a pre-existing recall snapshot, retaining the unaffected memory.
  Blocks blocks;
  ContextService context(blocks, nullptr, &memories);
  const auto revision_failure_pack = context.build(scope, previously_recalled);
  assert(std::find(revision_failure_pack.memory_ids.begin(), revision_failure_pack.memory_ids.end(),
                   "dense") == revision_failure_pack.memory_ids.end());
  assert(std::find(revision_failure_pack.memory_ids.begin(), revision_failure_pack.memory_ids.end(),
                   "lexical") != revision_failure_pack.memory_ids.end());
  memories.throw_retrieval_revision = false;

  // A validator exception follows the same per-item failure boundary.
  ContextService throwing_validator_context(
      blocks, nullptr, &memories, {}, nullptr,
      [](const MemoryScope&, const std::string& id, std::int64_t,
         RecallPolicy, bool) {
        if (id == "dense") throw std::runtime_error("dense validator unavailable");
        return true;
      });
  const auto validator_failure_pack =
      throwing_validator_context.build(scope, previously_recalled);
  assert(std::find(validator_failure_pack.memory_ids.begin(), validator_failure_pack.memory_ids.end(),
                   "dense") == validator_failure_pack.memory_ids.end());
  assert(std::find(validator_failure_pack.memory_ids.begin(), validator_failure_pack.memory_ids.end(),
                   "lexical") != validator_failure_pack.memory_ids.end());

  RecallService throwing(memories, DenseCandidateProvider{
      [](const MemoryScope&, const RecallRequest&) -> std::vector<DenseMemoryCandidate> {
        throw std::runtime_error("dense unavailable");
      }});
  const auto fallback = throwing.recall(scope, request);
  assert(fallback.size() == 1 && fallback.front().memory.id == "lexical");

  RecallRequest cancelled = request;
  cancelled.cancelled = [] { return true; };
  const auto cancelled_result = service.recall(scope, cancelled);
  assert(cancelled_result.size() == 1 && cancelled_result.front().memory.id == "lexical");
}
