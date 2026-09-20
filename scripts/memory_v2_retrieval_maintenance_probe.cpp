#include "qt/memory/database.hpp"
#include "qt/memory/scope.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sqlite3.h>
#include <string>
#include <nlohmann/json.hpp>

using namespace qt::memory;

namespace {
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void exec_sql(const std::filesystem::path& path, const std::string& sql) {
  sqlite3* raw = nullptr;
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open sqlite probe connection");
  char* error = nullptr;
  const auto result = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, &error);
  const std::string detail = error ? error : "sqlite probe SQL failed";
  sqlite3_free(error); sqlite3_close(raw);
  if (result != SQLITE_OK) throw std::runtime_error(detail);
}
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("requires agent home");
    SQLiteMemoryDatabase database(argv[1]);
    MemoryScope scope; scope.kind = ScopeKind::Repository; scope.user = "maintenance";
    scope.project = "qt"; scope.repository = "rebuild";
    auto other = scope; other.repository = "other";

    MemoryWrite write; write.status = MemoryStatus::Validated; write.content = "rebuild me";
    const auto memory_id = database.add(scope, write);
    const auto other_id = database.add(other, write);
    auto chunk_job = database.claim_index_job(scope);
    check(chunk_job && chunk_job->representation == "chunks", "initial chunk job missing");
    RetrievalChunk chunk; chunk.text = "rebuild me"; chunk.source_span = "probe";
    check(database.publish_chunks(scope, *chunk_job, {chunk}), "initial chunks rejected");

    DenseIndexConfig config; config.generation = "probe-generation"; config.model_id = "probe-model";
    config.model_fingerprint = "model-fingerprint"; config.tokenizer_fingerprint = "tokenizer";
    config.pooling = "mean"; config.dimensions = 2;
    database.begin_dense_index(scope, config);
    auto dense_job = database.claim_index_job(scope, std::chrono::seconds{30}, config.generation);
    check(dense_job && dense_job->representation == "dense", "initial dense job missing");
    auto chunks = database.retrieval_chunks(scope, dense_job->owner_key);
    check(chunks.size() == 1, "initial chunk unavailable for dense publication");
    check(database.publish_dense_vectors(scope, *dense_job, {{chunks.front().id, {1.0F, 0.0F}}}),
          "initial vector publication rejected");
    check(database.activate_dense_index(scope, config.generation), "initial dense activation rejected");
    check(database.dense_index(scope, config.generation).has_value(), "dense config disappeared");

    MemoryWrite leased_write = write; leased_write.content = "leased rebuild";
    const auto leased_id = database.add(scope, leased_write);
    auto leased_job = database.claim_index_job(scope);
    check(leased_job && leased_job->representation == "chunks", "live leased job missing");
    const auto canonical_before = database.export_state(scope).dump();
    const auto canonical_revision = database.retrieval_revision(scope, memory_id);
    const auto rollback_status = database.retrieval_index_status(scope);
    const auto foreign_before = database.export_state(other).dump();
    const auto foreign_status = database.retrieval_index_status(other);

    exec_sql(database.path(),
        "CREATE TRIGGER probe_rebuild_failure BEFORE DELETE ON retrieval_jobs "
        "WHEN OLD.owner_key='memory:" + leased_id + "' BEGIN SELECT RAISE(ABORT,'probe failure'); END;");
    bool rolled_back = false;
    try { (void)database.rebuild_retrieval(scope, "memory:" + leased_id); }
    catch (const std::runtime_error&) { rolled_back = true; }
    exec_sql(database.path(), "DROP TRIGGER probe_rebuild_failure;");
    check(rolled_back && database.export_state(scope).dump() == canonical_before &&
              database.retrieval_index_status(scope).pending == rollback_status.pending &&
              database.retrieval_index_status(scope).leased == rollback_status.leased &&
              database.retrieval_revision(scope, memory_id) == canonical_revision,
          "failed rebuild did not roll back derived state");

    const auto report = database.rebuild_retrieval(scope, "memory:" + leased_id);
    check(report.owners == 1 && report.chunks_invalidated == 0 && report.vectors_invalidated == 0 &&
              report.jobs_enqueued == 2 && report.jobs_superseded >= 1,
          "rebuild report did not account for derived state");
    check(!database.publish_chunks(scope, *leased_job, {chunk}),
          "stale pre-rebuild worker was allowed to publish chunks");
    const auto status = database.retrieval_index_status(scope);
    check(status.chunks == 1 && status.pending == 2 && status.complete == 2 &&
              database.export_state(scope).dump() == canonical_before,
          "rebuild changed canonical state or did not enqueue current owner");
    check(database.dense_index(scope, config.generation).has_value(), "rebuild changed dense config");
    auto rebuilt_chunk_job = database.claim_index_job(scope);
    check(rebuilt_chunk_job && rebuilt_chunk_job->representation == "chunks",
          "rebuilt chunk job was not claimable");
    check(database.publish_chunks(scope, *rebuilt_chunk_job, {chunk}),
          "rebuilt chunk publication rejected");
    auto rebuilt_dense_job = database.claim_index_job(scope, std::chrono::seconds{30}, config.generation);
    check(rebuilt_dense_job && rebuilt_dense_job->representation == "dense",
          "rebuilt dense job was not claimable");
    const auto rebuilt_chunks = database.retrieval_chunks(scope, rebuilt_dense_job->owner_key);
    check(rebuilt_chunks.size() == 1, "rebuilt chunk unavailable for dense publication");
    std::vector<DenseChunkVector> rebuilt_vectors{{rebuilt_chunks.front().id, {0.0F, 1.0F}}};
    check(database.publish_dense_vectors(scope, *rebuilt_dense_job, rebuilt_vectors),
          "rebuilt dense publication rejected");
    const auto completed = database.retrieval_index_status(scope);
    check(completed.pending == 0 && completed.complete == 4 && completed.chunks == 2,
          "rebuilt chunk/dense jobs did not complete");
    check(database.retrieval_revision(scope, memory_id) == canonical_revision &&
              database.retrieval_index_status(other).complete == foreign_status.complete &&
              database.retrieval_index_status(other).pending == foreign_status.pending &&
              database.export_state(other).dump() == foreign_before &&
              database.retrieval_revision(other, other_id) > 0,
          "rebuild crossed scope boundary");
    bool rejected = false;
    try { (void)database.rebuild_retrieval(other, "memory:" + memory_id); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "foreign owner key was accepted");
    std::cout << "memory retrieval maintenance probe passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
