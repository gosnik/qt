#include "qt/memory/database.hpp"
#include "qt/memory/retrieval_indexer.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace qt::memory;

namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

std::int64_t vector_count(const std::filesystem::path& path, const std::string& generation) {
  sqlite3* connection = nullptr;
  check(sqlite3_open_v2(path.c_str(), &connection, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
        "open vector database read-only");
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(connection,
                           "SELECT COUNT(*) FROM retrieval_vectors WHERE generation=?;",
                           -1, &statement, nullptr) == SQLITE_OK, "prepare vector count");
  sqlite3_bind_text(statement, 1, generation.c_str(), -1, SQLITE_TRANSIENT);
  check(sqlite3_step(statement) == SQLITE_ROW, "read vector count");
  const auto result = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  sqlite3_close(connection);
  return result;
}

std::size_t rss_kib() {
  std::ifstream status("/proc/self/status");
  std::string key;
  std::size_t value = 0;
  while (status >> key) {
    if (key == "VmRSS:") { status >> value; return value; }
    std::string ignored;
    std::getline(status, ignored);
  }
  return 0;
}

MemoryScope scope_for(const char* repository) {
  MemoryScope scope;
  scope.kind = ScopeKind::Repository;
  scope.user = "dense-stream-probe";
  scope.project = "qt";
  scope.repository = repository;
  return scope;
}

DenseIndexConfig config_for(const char* generation, std::size_t dimensions) {
  DenseIndexConfig config;
  config.generation = generation;
  config.model_id = "probe-model";
  config.model_fingerprint = std::string(generation) + "-model";
  config.tokenizer_fingerprint = std::string(generation) + "-tokenizer";
  config.pooling = "mean";
  config.dimensions = dimensions;
  config.query_prefix = "query:";
  config.document_prefix = "document:";
  return config;
}

std::string long_content() {
  std::string content;
  for (int i = 0; i != 160; ++i)
    content += "stream publication chunk " + std::to_string(i) + " carries durable content.\n";
  return content;
}

void publish_chunks(SQLiteMemoryDatabase& db, const MemoryScope& scope,
                    const std::string& owner) {
  const auto job = db.claim_index_job(scope);
  check(job && job->owner_key == owner, "claim chunk job");
  check(db.publish_chunks(scope, *job, RetrievalIndexer::render(job->content)),
        "publish chunks");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("requires probe directory");
    SQLiteMemoryDatabase db(argv[1]);
    const auto scope = scope_for("stream-repo");
    MemoryWrite write;
    write.status = MemoryStatus::Active;
    write.content = long_content();
    const auto memory_id = db.add(scope, write);
    const auto owner = "memory:" + memory_id;
    publish_chunks(db, scope, owner);
    const auto chunks = db.retrieval_chunks(scope, owner);
    check(chunks.size() >= 2, "probe needs multiple chunks");

    const auto generation = config_for("stream-rollback", 4);
    db.begin_dense_index(scope, generation);
    const auto leased = db.claim_index_job(scope);
    check(leased && leased->representation == "dense", "claim stream job");
    std::size_t calls = 0;
    bool threw = false;
    try {
      (void)db.publish_dense_vectors(scope, *leased, chunks.size(), [&] {
        ++calls;
        if (calls == 2) throw std::runtime_error("injected stream failure");
        return DenseChunkVector{chunks[calls - 1].id, {1, 0, 0, 0}};
      });
    } catch (const std::runtime_error&) {
      threw = true;
    }
    check(threw && calls == 2, "reader exception and exact bounded calls");
    check(vector_count(db.path(), generation.generation) == 0,
          "failed stream left partial rows");
    check(db.search_dense(scope, generation.generation, {1, 0, 0, 0},
                          RecallPolicy::Strict, false).empty(),
          "failed stream left searchable vectors");
    check(!db.activate_dense_index(scope, generation.generation),
          "failed stream generation activated");

    calls = 0;
    check(db.publish_dense_vectors(scope, *leased, chunks.size(), [&] {
      ++calls;
      return DenseChunkVector{chunks[calls - 1].id, {1, 0, 0, 0}};
    }), "retry same leased job");
    check(calls == chunks.size(), "retry consumed all chunks");
    check(db.activate_dense_index(scope, generation.generation), "activate retry generation");
    check(db.search_dense(scope, generation.generation, {1, 0, 0, 0},
                          RecallPolicy::Strict, false).size() == chunks.size(),
          "retry vectors searchable");

    const auto malformed = config_for("stream-malformed", 4);
    db.begin_dense_index(scope, malformed);
    const auto malformed_job = db.claim_index_job(scope);
    check(static_cast<bool>(malformed_job), "claim malformed stream job");
    calls = 0;
    threw = false;
    try {
      (void)db.publish_dense_vectors(scope, *malformed_job, chunks.size(), [&] {
        ++calls;
        if (calls == chunks.size()) return DenseChunkVector{chunks[calls - 1].id, {1, 2}};
        return DenseChunkVector{chunks[calls - 1].id, {1, 0, 0, 0}};
      });
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    check(threw && calls == chunks.size(), "late malformed vector consumed exact count");
    check(vector_count(db.path(), malformed.generation) == 0,
          "malformed stream left partial rows");
    check(db.search_dense(scope, malformed.generation, {1, 0, 0, 0},
                          RecallPolicy::Strict, false).empty(),
          "malformed stream rolled back vectors");
    check(!db.activate_dense_index(scope, malformed.generation),
          "malformed generation activated");

    const auto stale_scope = scope_for("stale-repo");
    MemoryWrite stale_write; stale_write.status = MemoryStatus::Active;
    stale_write.content = "stale lease revision content";
    const auto stale_id = db.add(stale_scope, stale_write);
    publish_chunks(db, stale_scope, "memory:" + stale_id);
    const auto stale_generation = config_for("stream-stale", 4);
    db.begin_dense_index(stale_scope, stale_generation);
    const auto stale_job = db.claim_index_job(stale_scope);
    check(static_cast<bool>(stale_job), "claim stale job");
    MemoryPatch edit; edit.content = "revision changed before callback";
    db.update(stale_scope, stale_id, edit);
    std::size_t stale_calls = 0;
    check(!db.publish_dense_vectors(stale_scope, *stale_job, 1, [&] {
      ++stale_calls; return DenseChunkVector{"unexpected", {1, 0, 0, 0}};
    }), "stale revision rejected");
    check(stale_calls == 0, "stale revision called reader");

    const auto cancelled_scope = scope_for("cancelled-spool-repo");
    MemoryWrite cancelled_write; cancelled_write.status = MemoryStatus::Active;
    cancelled_write.content = long_content();
    const auto cancelled_id = db.add(cancelled_scope, cancelled_write);
    publish_chunks(db, cancelled_scope, "memory:" + cancelled_id);
    const auto cancelled_chunks = db.retrieval_chunks(cancelled_scope, "memory:" + cancelled_id);
    check(cancelled_chunks.size() > 2, "cancellation fixture requires remaining chunks");
    const auto cancelled_config = config_for("stream-cancelled", 4);
    db.begin_dense_index(cancelled_scope, cancelled_config);
    bool cancel = false;
    std::size_t cancelled_calls = 0;
    RetrievalIndexer interrupted(db, [&](const DenseIndexConfig&, const std::string&,
                                         const RetrievalCancellation&) {
      if (++cancelled_calls == 2) cancel = true;
      return std::vector<float>{1, 0, 0, 0};
    });
    const auto interrupted_step = interrupted.step(cancelled_scope, [&] { return cancel; },
                                                    cancelled_config.generation);
    const auto interrupted_status = db.retrieval_index_status(cancelled_scope);
    check(interrupted_step.worked && !interrupted_step.completed && cancelled_calls == 2 &&
              vector_count(db.path(), cancelled_config.generation) == 0 &&
              interrupted_status.pending == 1 && interrupted_status.leased == 0 &&
              interrupted_status.failed == 0,
          "cancelled spool publishes no vectors and leaves a retryable job");
    cancel = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    check(interrupted.step(cancelled_scope, {}, cancelled_config.generation).completed &&
              cancelled_calls == 2 + cancelled_chunks.size() &&
              vector_count(db.path(), cancelled_config.generation) ==
                  static_cast<std::int64_t>(cancelled_chunks.size()),
          "retry after backoff recomputes every unpublished vector");

    // Exercise the production indexer on a maximum-sized record and 65,536-D vectors.
    const auto high_scope = scope_for("high-dimensional-repo");
    MemoryWrite high_write; high_write.status = MemoryStatus::Active;
    high_write.content = std::string(1024U * 1024U, 'x');
    const auto high_id = db.add(high_scope, high_write);
    const auto high_chunks_job = db.claim_index_job(high_scope);
    check(static_cast<bool>(high_chunks_job), "claim high-dimensional chunks");
    check(db.publish_chunks(high_scope, *high_chunks_job,
                            RetrievalIndexer::render(high_chunks_job->content)),
          "publish high-dimensional chunks");
    const auto high_chunks = db.retrieval_chunks(high_scope, "memory:" + high_id);
    check(high_chunks.size() > 400, "maximum-sized record chunk count");
    const auto high_config = config_for("stream-high-dimensional", 65536);
    db.begin_dense_index(high_scope, high_config);
    std::size_t high_calls = 0;
    const auto baseline_rss = rss_kib();
    check(baseline_rss > 0, "RSS measurement unavailable");
    std::size_t peak_rss = baseline_rss;
    RetrievalIndexer indexer(db, [&](const DenseIndexConfig& config, const std::string&,
                                     const RetrievalCancellation&) {
      ++high_calls;
      const auto current_rss = rss_kib();
      check(current_rss > 0, "RSS measurement unavailable during embedding");
      peak_rss = std::max(peak_rss, current_rss);
      std::vector<float> vector(config.dimensions, 0.0F);
      vector.front() = 1.0F;
      return vector;
    });
    const auto step = indexer.step(high_scope, {}, high_config.generation);
    check(step.worked && step.completed, "production indexer high-dimensional step");
    check(high_calls == high_chunks.size() &&
              vector_count(db.path(), high_config.generation) ==
                  static_cast<std::int64_t>(high_chunks.size()),
          "high-dimensional stream covered every chunk");
    check(peak_rss <= baseline_rss + 64U * 1024U,
          "high-dimensional stream stayed bounded in memory");
    check(db.activate_dense_index(high_scope, high_config.generation),
          "activate high-dimensional index");
    check(db.search_dense(high_scope, high_config.generation,
                           std::vector<float>(high_config.dimensions, 1.0F),
                           RecallPolicy::Strict, false, 1000).size() == high_chunks.size(),
          "all high-dimensional vectors searchable");
    std::cout << "memory dense stream probe passed: chunks=" << high_chunks.size()
              << " dimensions=" << high_config.dimensions
              << " embedding_rss_growth_kib=" << (peak_rss - baseline_rss) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "memory dense stream probe failed: " << error.what() << '\n';
    return 1;
  }
}
