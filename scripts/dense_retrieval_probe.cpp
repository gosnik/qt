#include "qt/memory/dense_retrieval.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace qt::memory;

static void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static void publish_chunks(SQLiteMemoryDatabase& db, const MemoryScope& scope,
                           const std::string& owner) {
  const auto job = db.claim_index_job(scope);
  check(job && job->owner_key == owner, "chunk job");
  auto chunks = RetrievalIndexer::render(job->content);
  check(!chunks.empty(), "retrieval chunks");
  check(db.publish_chunks(scope, *job, std::move(chunks)), "publish chunks");
}

int main(int argc, char** argv) {
  if (argc != 2) throw std::invalid_argument("requires probe directory");
  SQLiteMemoryDatabase db(argv[1]);
  MemoryScope scope;
  scope.kind = ScopeKind::Repository; scope.user = "dense-probe";
  scope.project = "qt"; scope.repository = "repo";
  MemoryWrite active; active.status = MemoryStatus::Active;
  active.content = "unique active retrieval content";
  const auto active_id = db.add(scope, active);
  MemoryWrite rejected; rejected.status = MemoryStatus::Rejected;
  rejected.content = "unique rejected retrieval content";
  const auto rejected_id = db.add(scope, rejected);
  publish_chunks(db, scope, "memory:" + active_id);
  publish_chunks(db, scope, "memory:" + rejected_id);

  DenseIndexConfig config;
  config.generation = "dense-generation-a"; config.model_id = "synthetic";
  config.model_fingerprint = "model-a"; config.tokenizer_fingerprint = "tokenizer-a";
  config.query_prefix = "query:"; config.dimensions = 3; config.pooling = "mean";
  db.begin_dense_index(scope, config);
  for (const auto& owner : {"memory:" + active_id, "memory:" + rejected_id}) {
    const auto job = db.claim_index_job(scope);
    check(job && job->owner_key == owner, "dense job");
    const auto chunks = db.retrieval_chunks(scope, owner);
    check(db.publish_dense_vectors(scope, *job, {{chunks.front().id, {1, 0, 0}}}),
          "publish vector");
  }
  check(db.activate_dense_index(scope, config.generation), "activate dense index");

  std::size_t calls = 0;
  DenseRetrievalService service(db, [&](const DenseIndexConfig& current,
                                        const std::string& text,
                                        const RetrievalCancellation&) {
    ++calls;
    check(current.query_prefix == "query:" && text == "query:term", "query prefix");
    return std::vector<float>{1, 0, 0};
  });
  auto hits = service.search(scope, "term", RecallPolicy::Strict, false, 10);
  check(hits.size() == 1 && hits.front().chunk.owner_key == "memory:" + active_id,
        "active dense retrieval excludes rejected memory");
  service.search(scope, "term", RecallPolicy::Strict, false, 10);
  check(calls == 1 && service.stats().cache_hits == 1, "query embedding cache");
  check(service.search(scope, "term", RecallPolicy::Strict, false, 10,
                       [] { return true; }).empty(), "cancellation");
  check(calls == 1, "cancelled query did not call provider");

  config.generation = "dense-generation-b"; config.model_fingerprint = "model-b";
  db.begin_dense_index(scope, config);
  for (const auto& owner : {"memory:" + active_id, "memory:" + rejected_id}) {
    const auto job = db.claim_index_job(scope);
    check(job && job->owner_key == owner, "second dense job");
    const auto chunks = db.retrieval_chunks(scope, owner);
    check(db.publish_dense_vectors(scope, *job, {{chunks.front().id, {1, 0, 0}}}), "second vector");
  }
  check(db.activate_dense_index(scope, config.generation), "activate second generation");
  service.search(scope, "term", RecallPolicy::Strict, false, 10);
  check(calls == 2, "generation changed cache key");

  std::size_t bounded_calls = 0;
  DenseRetrievalService bounded(db, [&](const DenseIndexConfig&, const std::string&,
                                       const RetrievalCancellation&) {
    ++bounded_calls;
    return std::vector<float>{1, 0, 0};
  }, 2);
  for (const auto* query : {"first", "second", "first", "third", "first"})
    check(!bounded.search(scope, query, RecallPolicy::Strict, false, 10).empty(),
          "bounded cache search");
  check(bounded_calls == 3, "recent query survives cache eviction");
  bounded.search(scope, "second", RecallPolicy::Strict, false, 10);
  check(bounded_calls == 4, "least recently used query is evicted");
  bounded.clear_cache();
  bounded.search(scope, "second", RecallPolicy::Strict, false, 10);
  check(bounded_calls == 5, "explicit cache invalidation forces re-embedding");

  bool cancel_after_embedding = false;
  std::size_t cancelled_calls = 0;
  DenseRetrievalService interrupted(db, [&](const DenseIndexConfig&, const std::string&,
                                           const RetrievalCancellation&) {
    ++cancelled_calls;
    cancel_after_embedding = true;
    return std::vector<float>{1, 0, 0};
  });
  check(interrupted.search(scope, "interrupted", RecallPolicy::Strict, false, 10,
                           [&] { return cancel_after_embedding; }).empty(),
        "cancellation after embedding suppresses retrieval");
  check(!interrupted.search(scope, "interrupted", RecallPolicy::Strict, false, 10).empty() &&
            cancelled_calls == 2,
        "cancelled embedding is not cached and retry succeeds");

  DenseRetrievalService failing(db, [](const DenseIndexConfig&, const std::string&,
                                       const RetrievalCancellation&) -> std::vector<float> {
    throw std::runtime_error("synthetic provider failure");
  });
  check(failing.search(scope, "term", RecallPolicy::Strict, false, 10).empty(),
        "provider failure fallback");
  check(failing.stats().embedding_failures == 1 &&
            !failing.stats().last_error.empty(), "provider diagnostic");

  config.generation = "dense-large-vectors";
  config.dimensions = 65536;
  std::vector<float> large_vector(config.dimensions, 0.0F);
  large_vector.front() = 1.0F;
  db.begin_dense_index(scope, config);
  for (const auto& owner : {"memory:" + active_id, "memory:" + rejected_id}) {
    const auto job = db.claim_index_job(scope);
    check(job && job->owner_key == owner, "large vector job");
    const auto chunks = db.retrieval_chunks(scope, owner);
    check(db.publish_dense_vectors(scope, *job, {{chunks.front().id, large_vector}}),
          "publish large vector");
  }
  check(db.activate_dense_index(scope, config.generation), "activate large vector index");
  std::size_t large_calls = 0;
  constexpr std::size_t byte_budget = 512U * 1024U;
  DenseRetrievalService byte_bounded(db, [&](const DenseIndexConfig&, const std::string&,
                                            const RetrievalCancellation&) {
    ++large_calls;
    return large_vector;
  }, 64, byte_budget);
  for (const auto* query : {"first", "first", "second", "first"}) {
    check(!byte_bounded.search(scope, query, RecallPolicy::Strict, false, 10).empty(),
          "large vector cache search");
    check(byte_bounded.stats().cache_bytes <= byte_budget &&
              byte_bounded.stats().cache_entries == 1,
          "byte limit evicts large vectors before entry limit");
  }
  check(large_calls == 3, "byte eviction requires re-embedding an evicted query");
  byte_bounded.clear_cache();
  check(byte_bounded.stats().cache_bytes == 0 && byte_bounded.stats().cache_entries == 0,
        "cache clear resets byte accounting");

  // Keep the owner-ranking contract isolated from the cache and lifecycle cases
  // above.  The long memory has enough chunks to crowd out another owner if the
  // database applies the limit before collapsing chunks by owner.
  MemoryScope owner_scope = scope;
  owner_scope.repository = "dense-owner-ranking";
  MemoryWrite long_owner_write;
  long_owner_write.status = MemoryStatus::Active;
  long_owner_write.content = std::string(5200, 'L');
  const auto long_owner_id = db.add(owner_scope, long_owner_write);
  MemoryWrite other_owner_write;
  other_owner_write.status = MemoryStatus::Active;
  other_owner_write.content = "a distinct owner that must remain in the result";
  const auto other_owner_id = db.add(owner_scope, other_owner_write);
  MemoryWrite tie_owner_write;
  tie_owner_write.status = MemoryStatus::Active;
  tie_owner_write.content = std::string(24000, 'T');
  const auto tie_owner_id = db.add(owner_scope, tie_owner_write);
  const auto long_owner = "memory:" + long_owner_id;
  const auto other_owner = "memory:" + other_owner_id;
  const auto tie_owner = "memory:" + tie_owner_id;
  publish_chunks(db, owner_scope, long_owner);
  publish_chunks(db, owner_scope, other_owner);
  publish_chunks(db, owner_scope, tie_owner);
  const auto long_chunks = db.retrieval_chunks(owner_scope, long_owner);
  check(long_chunks.size() >= 3, "owner ranking probe needs three long-owner chunks");

  DenseIndexConfig owner_config;
  owner_config.generation = "dense-owner-ranking";
  owner_config.model_id = "synthetic";
  owner_config.model_fingerprint = "owner-ranking-model";
  owner_config.tokenizer_fingerprint = "owner-ranking-tokenizer";
  owner_config.query_prefix = "query:";
  owner_config.dimensions = 3;
  owner_config.pooling = "mean";
  db.begin_dense_index(owner_scope, owner_config);
  for (std::size_t indexed = 0; indexed != 3U; ++indexed) {
    const auto job = db.claim_index_job(owner_scope);
    check(job && job->representation == "dense", "claim owner-ranking dense job");
    const auto chunks = db.retrieval_chunks(owner_scope, job->owner_key);
    std::vector<DenseChunkVector> vectors;
    vectors.reserve(chunks.size());
    for (const auto& chunk : chunks) {
      // The strongest long-owner chunk deliberately occurs later than its
      // earlier siblings, so owner collapse must replace the provisional hit.
      const auto values = job->owner_key == long_owner
          ? (chunk.ordinal == 0U ? std::vector<float>{0.6F, 0.8F, 0.0F}
             : chunk.ordinal == 1U ? std::vector<float>{0.8F, 0.6F, 0.0F}
                                  : std::vector<float>{1.0F, 0.0F, 0.0F})
          : job->owner_key == other_owner ? std::vector<float>{0.5F, 0.866F, 0.0F}
                                          : std::vector<float>{0.4F, 0.916F, 0.0F};
      vectors.push_back({chunk.id, values});
    }
    check(db.publish_dense_vectors(owner_scope, *job, vectors),
          "publish owner-ranking vectors");
  }
  check(db.activate_dense_index(owner_scope, owner_config.generation),
        "activate owner-ranking index");
  const auto raw_owner_hits = db.search_dense(owner_scope, owner_config.generation,
                                               {1, 0, 0}, RecallPolicy::Strict, false);
  check(raw_owner_hits.size() >= 4U, "raw dense search retains sibling chunks");
  check(raw_owner_hits[0].chunk.owner_key == long_owner &&
            raw_owner_hits[0].chunk.ordinal >= 2U,
        "raw ranking sees the later strongest chunk");
  check(std::count_if(raw_owner_hits.begin(), raw_owner_hits.end(), [&](const auto& hit) {
          return hit.chunk.owner_key == long_owner;
        }) >= 3U, "raw dense API remains chunk-oriented by default");
  std::size_t expected_tie_ordinal = 0;
  for (const auto& hit : raw_owner_hits) {
    if (hit.chunk.owner_key != tie_owner) continue;
    check(hit.chunk.ordinal == expected_tie_ordinal++,
          "equal-score chunks sort by numeric ordinal rather than chunk-id text");
  }
  check(expected_tie_ordinal > 10U, "tie-order probe needs a two-digit ordinal");
  DenseRetrievalService owner_aware(db, [](const DenseIndexConfig&, const std::string&,
                                           const RetrievalCancellation&) {
    return std::vector<float>{1, 0, 0};
  });
  const auto owner_hits = owner_aware.search(owner_scope, "term", RecallPolicy::Strict,
                                              false, 2);
  check(owner_hits.size() == 2U, "owner-aware service returns two owners");
  check(owner_hits[0].chunk.owner_key == long_owner && owner_hits[0].chunk.ordinal >= 2U,
        "later strongest chunk replaces an earlier sibling");
  check(owner_hits[1].chunk.owner_key == other_owner,
        "owner collapse happens before the service result limit");
  std::cout << "dense retrieval probe passed\n";
}
