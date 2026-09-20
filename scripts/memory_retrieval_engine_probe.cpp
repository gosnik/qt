#include "qt/memory/database.hpp"
#include "qt/memory/retrieval_engine.hpp"

#include <algorithm>
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <sqlite3.h>

using namespace qt::memory;

static MemoryScope scope_for(const char* repository) {
  MemoryScope scope;
  scope.kind = ScopeKind::Repository;
  scope.user = "retrieval-engine-probe";
  scope.project = "qt";
  scope.repository = repository;
  return scope;
}

static std::string add_memory(SQLiteMemoryDatabase& db, const MemoryScope& scope,
                              const std::string& content) {
  MemoryWrite write;
  write.status = MemoryStatus::Active;
  write.content = content;
  return db.add(scope, std::move(write));
}

static void check(const bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <class Predicate>
static void wait_for(Predicate predicate, const char* message,
                     const std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

struct LeasedDenseJob {
  std::int64_t lease_until = 0;
  std::size_t attempts = 0;
};

static std::optional<LeasedDenseJob> leased_dense_job(const std::filesystem::path& path,
                                                       const std::string& owner_key) {
  sqlite3* raw = nullptr;
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open lease inspection connection");
  sqlite3_stmt* statement = nullptr;
  const int prepared = sqlite3_prepare_v2(raw,
      "SELECT lease_until,attempts FROM retrieval_jobs "
      "WHERE owner_key=? AND representation='dense' AND state='leased';", -1, &statement, nullptr);
  if (prepared != SQLITE_OK) {
    sqlite3_close(raw);
    throw std::runtime_error("prepare lease inspection query");
  }
  if (sqlite3_bind_text(statement, 1, owner_key.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    sqlite3_finalize(statement);
    sqlite3_close(raw);
    throw std::runtime_error("bind lease inspection owner");
  }
  std::optional<LeasedDenseJob> result;
  if (sqlite3_step(statement) == SQLITE_ROW) {
    result = LeasedDenseJob{sqlite3_column_int64(statement, 0),
                            static_cast<std::size_t>(sqlite3_column_int64(statement, 1))};
  }
  sqlite3_finalize(statement);
  sqlite3_close(raw);
  return result;
}

static std::int64_t shorten_dense_lease(const std::filesystem::path& path,
                                        const std::string& owner_key) {
  const auto short_lease = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count() + 3000;
  sqlite3* raw = nullptr;
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open lease shortening connection");
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(raw,
      "UPDATE retrieval_jobs SET lease_until=? WHERE owner_key=? AND representation='dense' "
      "AND state='leased';", -1, &statement, nullptr) == SQLITE_OK,
      "prepare lease shortening query");
  check(sqlite3_bind_int64(statement, 1, short_lease) == SQLITE_OK,
        "bind shortened lease deadline");
  check(sqlite3_bind_text(statement, 2, owner_key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK,
        "bind shortened lease owner");
  check(sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(raw) == 1,
        "shorten dense lease");
  sqlite3_finalize(statement);
  sqlite3_close(raw);
  return short_lease;
}

static std::size_t dense_vector_count(const std::filesystem::path& path,
                                      const std::string& owner_key) {
  sqlite3* raw = nullptr;
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open vector inspection connection");
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(raw,
      "SELECT COUNT(*) FROM retrieval_vectors v JOIN retrieval_chunks c ON c.id=v.chunk_id "
      "WHERE c.owner_key=?;", -1, &statement, nullptr) == SQLITE_OK,
      "prepare vector inspection query");
  check(sqlite3_bind_text(statement, 1, owner_key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK,
        "bind vector inspection owner");
  check(sqlite3_step(statement) == SQLITE_ROW, "read vector inspection count");
  const auto result = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
  sqlite3_finalize(statement);
  sqlite3_close(raw);
  return result;
}

int main(int argc, char** argv) {
  if (argc != 2) throw std::invalid_argument("requires probe directory");
  SQLiteMemoryDatabase db{std::filesystem::path(argv[1])};
  const auto scope_a = scope_for("repo-a");
  const auto scope_b = scope_for("repo-b");
  const auto id_a = add_memory(db, scope_a, "alpha dense retrieval document");
  add_memory(db, scope_b, "alpha belongs to another repository");
  auto exact_child = scope_a;
  exact_child.kind = ScopeKind::Exact;
  const auto child_id = add_memory(db, exact_child, "child alpha retrieval document");

  DenseIndexConfig config;
  config.generation = "probe-generation";
  config.model_id = "probe-model";
  config.model_fingerprint = "probe-model-fingerprint";
  config.tokenizer_fingerprint = "probe-tokenizer";
  config.query_prefix = "query: ";
  config.document_prefix = "document: ";
  config.pooling = "mean";
  config.dimensions = 2;

  RetrievalEngineOptions options;
  options.enabled = true;
  options.dense_enabled = true;
  options.configure = [&](const RetrievalCancellation& cancelled) {
    if (cancelled()) throw std::runtime_error("configuration cancelled");
    return config;
  };
  options.embedder = [](const DenseIndexConfig& current, const std::string& text,
                        const RetrievalCancellation& cancelled) {
    if (cancelled()) throw std::runtime_error("embedding cancelled");
    assert(current.dimensions == 2U);
    return std::vector<float>{(text.find("alpha") == std::string::npos &&
                               text.find("dense-query") == std::string::npos &&
                               text.find("child-query") == std::string::npos) ? 0.0F : 1.0F,
                              text.find("document") == std::string::npos ? 0.0F : 1.0F};
  };
  std::atomic<bool> reject_validation{false};
  options.validate_embedding = [&] {
    if (reject_validation.load()) throw std::runtime_error("embedding model changed");
  };

  RetrievalEngine engine(db, scope_a, options);
  bool indexed = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto status = engine.status();
    if (status.generation == config.generation && status.jobs.complete > 0U &&
        status.jobs.chunks > 0U && db.dense_index(scope_a, config.generation,
                                                   RecallPolicy::Strict, false)) {
      indexed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(indexed);
  const auto hits = engine.search(scope_a, "dense-query", RecallPolicy::Strict, false, 8);
  assert(!hits.empty() && hits.front().id == id_a);
  reject_validation.store(true);
  assert(engine.search(scope_a, "dense-query", RecallPolicy::Strict, false, 8).empty());
  assert(engine.status().last_error == "embedding model changed");
  reject_validation.store(false);
  MemoryQuery fts_query;
  fts_query.text = "dense-query";
  fts_query.policy = RecallPolicy::Balanced;
  assert(db.query(scope_a, fts_query).empty());
  assert(!engine.search(exact_child, "dense-query", RecallPolicy::Balanced, false, 8).empty());
  assert(engine.search(exact_child, "dense-query", RecallPolicy::Strict, false, 8).empty());
  engine.set_scope(exact_child);
  bool child_indexed = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto child_hits = engine.search(exact_child, "child-query",
                                          RecallPolicy::Balanced, false, 8);
    if (std::any_of(child_hits.begin(), child_hits.end(), [&](const auto& item) {
          return item.id == child_id;
        })) {
      child_indexed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(child_indexed);
  const auto old_dense_hits = db.search_dense(scope_a, config.generation, {1.0F, 1.0F},
                                              RecallPolicy::Strict, false, 8);
  assert(!old_dense_hits.empty());

  engine.set_foreground_active(true);
  MemoryPatch edit;
  edit.content = "edited beta retrieval document";
  db.update(scope_a, id_a, edit);
  const auto invalidated = db.retrieval_index_status(scope_a);
  assert(invalidated.pending > 0U || invalidated.chunks == 0U);
  assert(db.resolve_dense_memories(scope_a, old_dense_hits, RecallPolicy::Strict, false).empty());
  engine.set_foreground_active(false);
  engine.stop();

  // Standalone previews reuse published vectors without draining durable jobs.
  const auto pending_id = add_memory(db, exact_child, "unindexed preview document");
  const auto pending_before = db.retrieval_index_status(exact_child);
  auto query_options = options;
  query_options.background_indexing = false;
  std::size_t configuration_calls = 0;
  query_options.configure = [&](const RetrievalCancellation& cancelled) {
    assert(!cancelled());
    ++configuration_calls;
    return config;
  };
  RetrievalEngine query_engine(db, exact_child, query_options);
  assert(query_engine.search(exact_child, "child-query", RecallPolicy::Strict,
                             false, 8, [] { return true; }).empty());
  assert(configuration_calls == 0);
  const auto preview_hits = query_engine.search(exact_child, "child-query", RecallPolicy::Strict, false, 8);
  assert(configuration_calls == 1 && !query_engine.status().running);
  assert(std::any_of(preview_hits.begin(), preview_hits.end(), [&](const auto& item) { return item.id == child_id; }));
  assert(std::none_of(preview_hits.begin(), preview_hits.end(), [&](const auto& item) { return item.id == pending_id; }));
  const auto pending_after = db.retrieval_index_status(exact_child);
  assert(pending_before.pending == pending_after.pending &&
         pending_before.leased == pending_after.leased &&
         query_engine.status().completed_jobs == 0);
  query_engine.stop();

  // A foreground request that arrives while a multi-chunk owner is embedding
  // must pause at the next checkpoint without losing its durable dense lease.
  auto pause_scope = scope_for("foreground-pause");
  const std::string pause_content(6500, 'p');
  const auto pause_id = add_memory(db, pause_scope, pause_content);
  const auto pause_owner = "memory:" + pause_id;
  const auto expected_pause_embeddings = RetrievalIndexer::render(pause_content).size();
  check(expected_pause_embeddings > 1U, "foreground probe needs multiple chunks");
  std::mutex pause_mutex;
  std::condition_variable pause_condition;
  bool first_pause_embedding = false;
  bool release_pause_embedding = false;
  std::atomic<std::size_t> pause_embeddings{0};
  RetrievalEngineOptions pause_options = options;
  pause_options.embedder = [&](const DenseIndexConfig& current, const std::string&,
                               const RetrievalCancellation& cancelled) {
    check(current.dimensions == 2U, "foreground probe dimensions");
    if (pause_embeddings.fetch_add(1) == 0U) {
      std::unique_lock<std::mutex> lock(pause_mutex);
      first_pause_embedding = true;
      pause_condition.notify_all();
      if (!pause_condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return release_pause_embedding;
          })) throw std::runtime_error("foreground embedding release timed out");
    }
    if (cancelled()) throw std::runtime_error("embedding cancelled");
    return std::vector<float>{1.0F, 1.0F};
  };
  {
    RetrievalEngine pause_engine(db, pause_scope, pause_options);
    {
      std::unique_lock<std::mutex> lock(pause_mutex);
      const bool first_started = pause_condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return first_pause_embedding;
      });
      if (!first_started) {
        release_pause_embedding = true;
        lock.unlock();
        pause_condition.notify_all();
      }
      check(first_started, "first foreground embedding did not start");
    }
    pause_engine.set_foreground_active(true);
    const auto shortened_pause_lease = shorten_dense_lease(db.path(), pause_owner);
    {
      std::lock_guard<std::mutex> lock(pause_mutex);
      release_pause_embedding = true;
    }
    pause_condition.notify_all();
    const auto renewed_lease = [&] {
      const auto job = leased_dense_job(db.path(), pause_owner);
      return job && job->lease_until > shortened_pause_lease + 200000;
    };
    wait_for(renewed_lease, "foreground pause did not renew dense lease");
    const auto paused_job = leased_dense_job(db.path(), pause_owner);
    check(paused_job && paused_job->attempts == 1U, "foreground pause attempt count");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    const auto still_paused_job = leased_dense_job(db.path(), pause_owner);
    check(still_paused_job && still_paused_job->attempts == paused_job->attempts,
          "foreground pause changed dense attempts");
    check(still_paused_job->lease_until > paused_job->lease_until,
          "foreground pause stopped renewing dense lease");
    check(pause_embeddings.load() == 1U, "foreground pause started another embedding");
    pause_engine.set_foreground_active(false);
    wait_for([&] {
      const auto active = db.dense_index(pause_scope);
      return pause_embeddings.load() == expected_pause_embeddings && active &&
             active->generation == config.generation;
    }, "foreground pause did not resume dense indexing");
    check(dense_vector_count(db.path(), pause_owner) == expected_pause_embeddings,
          "foreground resume did not publish every vector once");
    check(pause_engine.status().last_error.empty(), "foreground pause reported an indexing error");
  }

  // Stopping while paused must wake the checkpoint and prevent both the next
  // model call and dense-vector publication.
  auto stop_scope = scope_for("foreground-pause-stop");
  const auto stop_id = add_memory(db, stop_scope, pause_content);
  const auto stop_owner = "memory:" + stop_id;
  std::mutex stop_mutex;
  std::condition_variable stop_condition;
  bool first_stop_embedding = false;
  bool release_stop_embedding = false;
  std::atomic<std::size_t> stop_embeddings{0};
  RetrievalEngineOptions stop_options = options;
  stop_options.embedder = [&](const DenseIndexConfig&, const std::string&,
                              const RetrievalCancellation& cancelled) {
    if (stop_embeddings.fetch_add(1) == 0U) {
      std::unique_lock<std::mutex> lock(stop_mutex);
      first_stop_embedding = true;
      stop_condition.notify_all();
      if (!stop_condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return release_stop_embedding;
          })) throw std::runtime_error("stop-pause embedding release timed out");
    }
    if (cancelled()) throw std::runtime_error("embedding cancelled");
    return std::vector<float>{1.0F, 1.0F};
  };
  RetrievalEngine stop_pause_engine(db, stop_scope, stop_options);
  {
    std::unique_lock<std::mutex> lock(stop_mutex);
    const bool first_started = stop_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return first_stop_embedding;
    });
    if (!first_started) {
      release_stop_embedding = true;
      lock.unlock();
      stop_condition.notify_all();
    }
    check(first_started, "first stop-pause embedding did not start");
  }
  stop_pause_engine.set_foreground_active(true);
  const auto shortened_stop_lease = shorten_dense_lease(db.path(), stop_owner);
  {
    std::lock_guard<std::mutex> lock(stop_mutex);
    release_stop_embedding = true;
  }
  stop_condition.notify_all();
  wait_for([&] {
    const auto job = leased_dense_job(db.path(), stop_owner);
    return job && job->lease_until > shortened_stop_lease + 200000;
  }, "stop-pause checkpoint did not renew dense lease");
  const auto paused_stop_started = std::chrono::steady_clock::now();
  stop_pause_engine.stop();
  check(std::chrono::steady_clock::now() - paused_stop_started < std::chrono::seconds(2),
        "stop did not wake a foreground-paused checkpoint");
  check(stop_embeddings.load() == 1U, "stop-pause started another embedding");
  check(!leased_dense_job(db.path(), stop_owner), "stop-pause left the dense job leased");
  check(dense_vector_count(db.path(), stop_owner) == 0U,
        "stop-pause published dense vectors");
  check(!db.dense_index(stop_scope), "stop-pause activated an incomplete dense index");

  std::atomic<bool> slow_started{false};
  RetrievalEngineOptions slow = options;
    slow.embedder = [&](const DenseIndexConfig&, const std::string&,
                      const RetrievalCancellation& cancelled) {
    slow_started.store(true);
    while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    throw std::runtime_error("embedding cancelled");
    return std::vector<float>{};
  };
  RetrievalEngine slow_engine(db, scope_b, slow);
  for (int attempt = 0; attempt < 100 && !slow_started.load(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(slow_started.load());
  const auto stop_started = std::chrono::steady_clock::now();
  slow_engine.stop();
  assert(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(2));

  RetrievalEngineOptions failing = options;
  failing.configure = [](const RetrievalCancellation&) -> DenseIndexConfig {
    throw std::runtime_error("model unavailable");
  };
  RetrievalEngine failed_engine(db, scope_b, failing);
  bool observed_failure = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (!failed_engine.status().last_error.empty()) {
      observed_failure = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(observed_failure);
  failed_engine.stop();
}
