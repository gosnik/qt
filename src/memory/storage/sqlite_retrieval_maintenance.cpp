#include "qt/memory/database.hpp"

#include "qt/memory/scope.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace qt::memory {
namespace {

void check_sql(sqlite3* database, int result, const char* message) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW) {
    throw std::runtime_error(std::string(message) + ": " + sqlite3_errmsg(database));
  }
}

class Statement final {
 public:
  Statement(sqlite3* database, const char* sql) : database_(database) {
    check_sql(database_, sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr),
              "unable to prepare retrieval maintenance statement");
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind(int index, const std::string& value) {
    check_sql(database_, sqlite3_bind_text(statement_, index, value.c_str(), -1, SQLITE_TRANSIENT),
              "unable to bind retrieval maintenance text");
  }
  void bind(int index, std::int64_t value) {
    check_sql(database_, sqlite3_bind_int64(statement_, index, value),
              "unable to bind retrieval maintenance integer");
  }
  int step() { return sqlite3_step(statement_); }
  sqlite3_stmt* get() const { return statement_; }

 private:
  sqlite3* database_;
  sqlite3_stmt* statement_ = nullptr;
};

std::string column_text(sqlite3_stmt* statement, int index) {
  const auto* value = sqlite3_column_text(statement, index);
  return value ? reinterpret_cast<const char*>(value) : std::string{};
}

void exec(sqlite3* database, const char* sql) {
  char* error = nullptr;
  const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
  const std::string detail = error ? error : "sqlite error";
  sqlite3_free(error);
  if (result != SQLITE_OK) throw std::runtime_error(detail);
}

}  // namespace

RetrievalMaintenanceReport SQLiteMemoryDatabase::rebuild_retrieval(
    const MemoryScope& scope, std::optional<std::string> requested_owner) {
  validate_scope(scope);
  if (requested_owner && requested_owner->empty())
    throw std::invalid_argument("retrieval rebuild owner key cannot be empty");
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto key = scope_key(scope);
  std::vector<std::pair<std::string, std::int64_t>> owners;
  std::vector<std::string> dense_generations;
  RetrievalMaintenanceReport report;
  exec(database_, "BEGIN IMMEDIATE;");
  try {
    {
    const char* sql =
        "SELECT owner_key,revision FROM retrieval_owners WHERE scope_key=? AND deleted=0 "
        "AND (? IS NULL OR owner_key=?) ORDER BY owner_key;";
    Statement query(database_, sql);
    query.bind(1, key);
    if (requested_owner) {
      query.bind(2, *requested_owner);
      query.bind(3, *requested_owner);
    } else {
      // SQLite's NULL comparison keeps the owner filter disabled.
      check_sql(database_, sqlite3_bind_null(query.get(), 2), "unable to bind owner filter");
      check_sql(database_, sqlite3_bind_null(query.get(), 3), "unable to bind owner filter");
    }
    int result;
    while ((result = query.step()) == SQLITE_ROW)
      owners.emplace_back(column_text(query.get(), 0), sqlite3_column_int64(query.get(), 1));
    if (result != SQLITE_DONE)
      throw std::runtime_error("unable to enumerate retrieval rebuild owners");
    }
    if (requested_owner && owners.empty())
      throw std::invalid_argument("retrieval rebuild owner is not current in the requested scope");

    {
    Statement query(database_,
        "SELECT generation FROM retrieval_dense_indexes WHERE scope_key=? AND state='active' "
        "ORDER BY generation;");
    query.bind(1, key);
    int result;
    while ((result = query.step()) == SQLITE_ROW)
      dense_generations.push_back(column_text(query.get(), 0));
    if (result != SQLITE_DONE)
      throw std::runtime_error("unable to enumerate active dense generations");
    }
    report.owners = owners.size();
    for (const auto& [owner, revision] : owners) {
      {
        Statement count(database_,
            "SELECT COUNT(*) FROM retrieval_chunks WHERE owner_key=?;");
        count.bind(1, owner);
        check_sql(database_, count.step(), "unable to count retrieval chunks");
        report.chunks_invalidated += static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
      }
      {
        Statement count(database_,
            "SELECT COUNT(*) FROM retrieval_vectors v JOIN retrieval_chunks c ON c.id=v.chunk_id "
            "WHERE c.owner_key=?;");
        count.bind(1, owner);
        check_sql(database_, count.step(), "unable to count retrieval vectors");
        report.vectors_invalidated += static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
      }
      {
        Statement vectors(database_,
            "DELETE FROM retrieval_vectors WHERE chunk_id IN "
            "(SELECT id FROM retrieval_chunks WHERE owner_key=?);");
        vectors.bind(1, owner);
        check_sql(database_, vectors.step(), "unable to invalidate retrieval vectors");
      }
      {
        Statement chunks(database_, "DELETE FROM retrieval_chunks WHERE owner_key=?;");
        chunks.bind(1, owner);
        check_sql(database_, chunks.step(), "unable to invalidate retrieval chunks");
      }
      {
        Statement supersede(database_,
            "UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0 "
            "WHERE owner_key=? AND state<>'superseded';");
        supersede.bind(1, owner);
        check_sql(database_, supersede.step(), "unable to supersede retrieval jobs");
        report.jobs_superseded += static_cast<std::size_t>(sqlite3_changes(database_));
      }
      // Jobs are derived state and the uniqueness key includes the revision;
      // remove superseded rows so this rebuild can enqueue the same revision.
      {
        Statement remove(database_, "DELETE FROM retrieval_jobs WHERE owner_key=?;");
        remove.bind(1, owner);
        check_sql(database_, remove.step(), "unable to clear superseded retrieval jobs");
      }
      {
        Statement enqueue(database_,
            "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) "
            "VALUES(?,?, 'chunks','text-v1');");
        enqueue.bind(1, owner); enqueue.bind(2, revision);
        check_sql(database_, enqueue.step(), "unable to enqueue retrieval rebuild");
        ++report.jobs_enqueued;
      }
      for (const auto& generation : dense_generations) {
        Statement enqueue(database_,
            "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) "
            "VALUES(?,?, 'dense',?);");
        enqueue.bind(1, owner); enqueue.bind(2, revision); enqueue.bind(3, generation);
        check_sql(database_, enqueue.step(), "unable to enqueue dense retrieval rebuild");
        ++report.jobs_enqueued;
      }
    }
    exec(database_, "COMMIT;");
  } catch (...) {
    sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
  return report;
}

}  // namespace qt::memory
