#include "qt/memory/database.hpp"

#include "qt/memory/memory_service.hpp"
#include "qt/memory/scope.hpp"
#include "qt/foundation/hash.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace qt::memory {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumDurableText = 1024U * 1024U;
constexpr std::size_t kMaximumDurableJson = 1024U * 1024U;
constexpr std::size_t kMaximumDurableMetadata = 32768U;
constexpr std::size_t kMaximumDurableUri = 4096U;

std::string sanitize_text(std::string value,
                          const std::size_t limit = kMaximumDurableMetadata) {
  MemoryService::redact_secrets(value);
  if (value.size() > limit) value.resize(limit);
  return value;
}

bool sensitive_json_key(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return key.find("password") != std::string::npos ||
         key.find("secret") != std::string::npos ||
         key.find("api_key") != std::string::npos ||
         key.find("apikey") != std::string::npos ||
         key.find("access_token") != std::string::npos ||
         key.find("refresh_token") != std::string::npos ||
         key == "token" || key == "authorization";
}

void sanitize_json_value(Json& value) {
  if (value.is_object()) {
    for (auto item = value.begin(); item != value.end(); ++item) {
      if (sensitive_json_key(item.key())) {
        *item = "[REDACTED]";
      } else {
        sanitize_json_value(*item);
      }
    }
  } else if (value.is_array()) {
    for (auto& item : value) sanitize_json_value(item);
  } else if (value.is_string()) {
    value = sanitize_text(value.get<std::string>(), kMaximumDurableMetadata);
  }
}

std::string sanitize_json(std::string value,
                          const std::size_t limit = kMaximumDurableJson) {
  auto parsed = Json::parse(value, nullptr, false);
  if (parsed.is_discarded()) {
    value = sanitize_text(std::move(value), limit / 2U);
    return Json{{"_qt_unparsed", sanitize_text(std::move(value), limit / 2U)}}.dump();
  }
  sanitize_json_value(parsed);
  value = parsed.dump();
  if (value.size() <= limit) return value;
  return Json{{"_qt_truncated", true},
              {"original_bytes", value.size()}}.dump();
}

void remap_json_ids(
    Json& value,
    const std::initializer_list<const std::unordered_map<std::string,
                                                         std::string>*>& maps) {
  if (value.is_object()) {
    for (auto& item : value.items()) remap_json_ids(item.value(), maps);
    return;
  }
  if (value.is_array()) {
    for (auto& item : value) remap_json_ids(item, maps);
    return;
  }
  if (!value.is_string()) return;
  const auto original = value.get<std::string>();
  for (const auto* map : maps) {
    const auto found = map->find(original);
    if (found != map->end()) {
      value = found->second;
      return;
    }
  }
}

std::string remapped_json(
    std::string value,
    const std::initializer_list<const std::unordered_map<std::string,
                                                         std::string>*>& maps) {
  auto parsed = Json::parse(value, nullptr, false);
  if (parsed.is_discarded()) return sanitize_json(std::move(value));
  remap_json_ids(parsed, maps);
  return sanitize_json(parsed.dump());
}

class Statement final {
 public:
  Statement(sqlite3* database, const std::string& sql) {
    const int rc = sqlite3_prepare_v2(database, sql.c_str(), -1, &statement_, nullptr);
    if (rc != SQLITE_OK) {
      throw std::runtime_error("memory sqlite prepare failed: " +
                               std::string(sqlite3_errmsg(database)));
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  sqlite3_stmt* get() const noexcept { return statement_; }
  void bind(int index, const std::string& value) {
    if (sqlite3_bind_text(statement_, index, value.c_str(),
                          static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
      throw std::runtime_error("memory sqlite text bind failed");
  }
  void bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
      throw std::runtime_error("memory sqlite integer bind failed");
  }
  void bind(int index, double value) {
    if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK)
      throw std::runtime_error("memory sqlite real bind failed");
  }
  int step() {
    for (std::size_t attempt = 0; attempt <= 6; ++attempt) {
      const int rc = sqlite3_step(statement_);
      if (rc != SQLITE_LOCKED || attempt == 6) return rc;
      std::this_thread::sleep_for(std::chrono::milliseconds(4 * (attempt + 1)));
    }
    return SQLITE_LOCKED;
  }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

void exec(sqlite3* database, const std::string& sql) {
  for (std::size_t attempt = 0; attempt <= 8; ++attempt) {
    char* error = nullptr;
    const int rc = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
    if (rc == SQLITE_OK) return;
    const std::string message = error != nullptr ? error : sqlite3_errmsg(database);
    sqlite3_free(error);
    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && attempt < 8) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5 * (attempt + 1)));
      continue;
    }
    throw std::runtime_error("memory sqlite execution failed: " + message);
  }
}

class Transaction final {
 public:
  explicit Transaction(sqlite3* database, bool immediate = false) : database_(database) {
    if (sqlite3_get_autocommit(database_) != 0) {
      exec(database_, immediate ? "BEGIN IMMEDIATE;" : "BEGIN;");
    } else {
      static std::atomic<std::uint64_t> next{0};
      savepoint_ = "qt_memory_" + std::to_string(++next);
      exec(database_, "SAVEPOINT " + savepoint_ + ";");
    }
  }
  ~Transaction() {
    if (finished_) return;
    if (savepoint_.empty()) {
      sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
    } else {
      sqlite3_exec(database_, ("ROLLBACK TO " + savepoint_ + ";").c_str(), nullptr, nullptr, nullptr);
      sqlite3_exec(database_, ("RELEASE " + savepoint_ + ";").c_str(), nullptr, nullptr, nullptr);
    }
  }
  void commit() {
    exec(database_, savepoint_.empty() ? "COMMIT;" : "RELEASE " + savepoint_ + ";");
    finished_ = true;
  }

 private:
  sqlite3* database_;
  std::string savepoint_;
  bool finished_ = false;
};

std::int64_t timestamp(TimePoint value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             value.time_since_epoch()).count();
}

TimePoint timepoint(sqlite3_stmt* statement, int column) {
  return TimePoint(std::chrono::milliseconds(sqlite3_column_int64(statement, column)));
}

std::string text(sqlite3_stmt* statement, int column) {
  const auto* value = sqlite3_column_text(statement, column);
  return value == nullptr ? std::string{} :
      std::string(reinterpret_cast<const char*>(value));
}

std::string make_id(const char* prefix) {
  static thread_local std::mt19937_64 generator(std::random_device{}());
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  std::ostringstream out;
  out << prefix << std::hex << now << generator();
  return out.str();
}

std::vector<std::string> allowed_keys(const MemoryScope& scope,
                                      RecallPolicy policy,
                                      bool include_global) {
  std::vector<std::string> result;
  for (const auto& visible : recall_scopes(scope, policy, include_global))
    result.push_back(scope_key(visible));
  return result;
}

std::string placeholders(std::size_t count) {
  std::string result;
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) result += ',';
    result += '?';
  }
  return result;
}

std::string fts_query(const std::string& value) {
  std::vector<std::string> words;
  std::string word;
  for (unsigned char ch : value) {
    if (std::isalnum(ch) != 0 || ch == '_' || ch == '-') {
      word.push_back(static_cast<char>(ch));
    } else if (!word.empty()) {
      words.push_back(std::move(word));
      word.clear();
    }
  }
  if (!word.empty()) words.push_back(std::move(word));
  std::string result;
  for (const auto& item : words) {
    if (!result.empty()) result += " OR ";
    result += '"' + item + '"';
  }
  return result;
}

int busy_retry(void* context, int prior_attempts) {
  const auto& options = *static_cast<const DatabaseOptions*>(context);
  if (prior_attempts >= static_cast<int>(options.busy_retries)) return 0;
  const auto base = options.retry_base_delay.count() * (prior_attempts + 1);
  const auto cap = std::max<std::int64_t>(1, options.busy_timeout.count());
  static thread_local std::minstd_rand generator(std::random_device{}());
  const auto jitter = static_cast<std::int64_t>(generator() %
      static_cast<unsigned long>(std::max<std::int64_t>(1, options.retry_base_delay.count() + 1)));
  std::this_thread::sleep_for(std::chrono::milliseconds(std::min(cap, base + jitter)));
  return 1;
}

void bind_keys(Statement& statement, int& index,
               const std::vector<std::string>& keys) {
  for (const auto& key : keys) statement.bind(index++, key);
}

std::string memory_columns(const std::string& prefix = {}) {
  static const char* columns[] = {
      "id", "scope_key", "scope_kind", "user_id", "organization_id", "project_id",
      "repository_id", "repository_display", "branch_id", "task_id", "agent_id",
      "session_id", "conversation_id", "memory_type", "status", "source", "source_id",
      "source_type", "source_uri", "source_hash", "source_span", "created_by",
      "evidence_strength", "content", "confidence", "quality_score", "success_count",
      "failure_count", "last_outcome", "access_count", "superseded_by", "replacement_id",
      "created_at", "updated_at", "last_accessed_at"};
  std::string result;
  for (const auto* column : columns) {
    if (!result.empty()) result += ',';
    result += prefix;
    result += column;
  }
  return result;
}

MemoryAssertion read_assertion(sqlite3_stmt* row, int column) {
  MemoryAssertion value;
  value.memory_id = text(row, column++);
  value.revision = sqlite3_column_int64(row, column++);
  value.subject_id = text(row, column++);
  value.predicate = text(row, column++);
  value.object_id = text(row, column++);
  value.literal_json = text(row, column++);
  value.qualifiers_json = text(row, column++);
  value.positive = sqlite3_column_int(row, column++) != 0;
  if (sqlite3_column_type(row, column) != SQLITE_NULL)
    value.valid_from = timepoint(row, column);
  ++column;
  if (sqlite3_column_type(row, column) != SQLITE_NULL)
    value.valid_to = timepoint(row, column);
  ++column;
  const auto evidence = Json::parse(text(row, column++), nullptr, false);
  if (evidence.is_array()) {
    for (const auto& item : evidence) {
      value.evidence.push_back({item.value("source_id", ""),
                                item.value("source_revision", ""),
                                item.value("source_uri", ""),
                                item.value("source_hash", ""),
                                item.value("source_span", ""),
                                item.value("created_by", "")});
    }
  }
  value.recorded_at = timepoint(row, column++);
  const auto historical = Json::parse(text(row, column), nullptr, false);
  if (historical.is_object()) {
    value.historical_content = historical.value("content", "");
    value.historical_source = historical.value("source", "");
    value.historical_type = memory_type_from_string(
        historical.value("type", "observation"));
    value.historical_status = memory_status_from_string(
        historical.value("status", "hypothesis"));
    value.historical_provenance.source_id = historical.value("source_id", "");
    value.historical_provenance.source_type = source_type_from_string(
        historical.value("source_type", "unknown"));
    value.historical_provenance.source_uri = historical.value("source_uri", "");
    value.historical_provenance.source_hash = historical.value("source_hash", "");
    value.historical_provenance.source_span = historical.value("source_span", "");
    value.historical_provenance.created_by = historical.value("created_by", "agent");
    value.historical_provenance.evidence_strength =
        historical.value("evidence_strength", 0.0);
    value.historical_created_at = TimePoint(std::chrono::milliseconds(
        historical.value("created_at", static_cast<std::int64_t>(0))));
    value.historical_updated_at = TimePoint(std::chrono::milliseconds(
        historical.value("updated_at", static_cast<std::int64_t>(0))));
  }
  return value;
}

std::vector<std::string> load_tags(sqlite3* database, const std::string& id) {
  Statement statement(database,
      "SELECT tag FROM memory_tags WHERE memory_id=? ORDER BY tag;");
  statement.bind(1, id);
  std::vector<std::string> result;
  while (statement.step() == SQLITE_ROW)
    result.push_back(sanitize_text(text(statement.get(), 0), 256U));
  return result;
}

MemoryEntry read_memory(sqlite3* database, sqlite3_stmt* row) {
  MemoryEntry result;
  int column = 0;
  result.id = text(row, column++);
  ++column;  // scope_key
  result.scope.kind = scope_kind_from_string(text(row, column++));
  result.scope.user = text(row, column++);
  result.scope.organization = text(row, column++);
  result.scope.project = text(row, column++);
  result.scope.repository = text(row, column++);
  result.scope.repository_display = text(row, column++);
  result.scope.branch = text(row, column++);
  result.scope.task = text(row, column++);
  result.scope.agent = text(row, column++);
  result.scope.session = text(row, column++);
  result.scope.conversation = text(row, column++);
  result.type = memory_type_from_string(text(row, column++));
  result.status = memory_status_from_string(text(row, column++));
  result.source = text(row, column++);
  result.provenance.source_id = text(row, column++);
  result.provenance.source_type = source_type_from_string(text(row, column++));
  result.provenance.source_uri = text(row, column++);
  result.provenance.source_hash = text(row, column++);
  result.provenance.source_span = text(row, column++);
  result.provenance.created_by = text(row, column++);
  result.provenance.evidence_strength = sqlite3_column_double(row, column++);
  result.content = text(row, column++);
  result.confidence = sqlite3_column_double(row, column++);
  result.quality_score = sqlite3_column_double(row, column++);
  result.success_count = static_cast<std::size_t>(sqlite3_column_int64(row, column++));
  result.failure_count = static_cast<std::size_t>(sqlite3_column_int64(row, column++));
  result.last_outcome = procedure_outcome_from_string(text(row, column++));
  result.access_count = static_cast<std::size_t>(sqlite3_column_int64(row, column++));
  result.superseded_by = text(row, column++);
  result.replacement_id = text(row, column++);
  result.created_at = timepoint(row, column++);
  result.updated_at = timepoint(row, column++);
  result.last_accessed_at = timepoint(row, column++);
  result.tags = load_tags(database, result.id);
  result.scope.repository_display =
      sanitize_text(std::move(result.scope.repository_display), kMaximumDurableUri);
  result.source = sanitize_text(std::move(result.source));
  result.provenance.source_id = sanitize_text(std::move(result.provenance.source_id));
  result.provenance.source_uri =
      sanitize_text(std::move(result.provenance.source_uri), kMaximumDurableUri);
  result.provenance.source_hash = sanitize_text(std::move(result.provenance.source_hash));
  result.provenance.source_span = sanitize_text(std::move(result.provenance.source_span));
  result.provenance.created_by = sanitize_text(std::move(result.provenance.created_by));
  result.content = sanitize_text(std::move(result.content), kMaximumDurableText);
  return result;
}

constexpr const char* kReviewColumns =
    "id,kind,status,priority,reason,subject_id,source_id,source_type,source_uri,"
    "source_hash,source_span,created_by,evidence_strength,candidate_json,"
    "decision,decided_by,decision_note,tags_json,created_at,updated_at,decided_at,"
    "candidate_version,revision,deferred_until,application_status,"
    "application_error,action_key";

ReviewItem read_review(sqlite3_stmt* row) {
  ReviewItem item;
  int c = 0;
  item.id = text(row, c++);
  item.kind = text(row, c++);
  item.status = review_status_from_string(text(row, c++));
  item.priority = text(row, c++);
  item.reason = text(row, c++);
  item.subject_id = text(row, c++);
  item.provenance.source_id = text(row, c++);
  item.provenance.source_type = source_type_from_string(text(row, c++));
  item.provenance.source_uri = text(row, c++);
  item.provenance.source_hash = text(row, c++);
  item.provenance.source_span = text(row, c++);
  item.provenance.created_by = text(row, c++);
  item.provenance.evidence_strength = sqlite3_column_double(row, c++);
  item.candidate_json = text(row, c++);
  item.decision = text(row, c++);
  item.decided_by = text(row, c++);
  item.decision_note = text(row, c++);
  const auto tags = Json::parse(text(row, c++), nullptr, false);
  if (tags.is_array()) item.tags = tags.get<std::vector<std::string>>();
  item.created_at = timepoint(row, c++);
  item.updated_at = timepoint(row, c++);
  item.decided_at = timepoint(row, c++);
  item.candidate_version =
      static_cast<std::uint32_t>(sqlite3_column_int64(row, c++));
  item.revision = static_cast<std::uint64_t>(sqlite3_column_int64(row, c++));
  item.deferred_until = timepoint(row, c++);
  item.application_status = text(row, c++);
  item.application_error = text(row, c++);
  item.action_key = text(row, c++);
  return item;
}

Json memory_json(const MemoryEntry& entry) {
  return Json{
      {"id", entry.id}, {"scope_kind", to_string(entry.scope.kind)},
      {"user", entry.scope.user}, {"organization", entry.scope.organization},
      {"project", entry.scope.project}, {"repository", entry.scope.repository},
      {"repository_display", entry.scope.repository_display},
      {"branch", entry.scope.branch}, {"task", entry.scope.task},
      {"agent", entry.scope.agent}, {"session", entry.scope.session},
      {"conversation", entry.scope.conversation}, {"type", to_string(entry.type)},
      {"status", to_string(entry.status)}, {"source", entry.source},
      {"source_id", entry.provenance.source_id},
      {"source_type", to_string(entry.provenance.source_type)},
      {"source_uri", entry.provenance.source_uri},
      {"source_hash", entry.provenance.source_hash},
      {"source_span", entry.provenance.source_span},
      {"created_by", entry.provenance.created_by},
      {"evidence_strength", entry.provenance.evidence_strength},
      {"content", entry.content}, {"tags", entry.tags},
      {"confidence", entry.confidence}, {"quality_score", entry.quality_score},
      {"success_count", entry.success_count}, {"failure_count", entry.failure_count},
      {"last_outcome", to_string(entry.last_outcome)},
      {"access_count", entry.access_count}, {"superseded_by", entry.superseded_by},
      {"replacement_id", entry.replacement_id},
      {"created_at", timestamp(entry.created_at)},
      {"updated_at", timestamp(entry.updated_at)},
      {"last_accessed_at", timestamp(entry.last_accessed_at)}};
}

void secure_directory(const std::filesystem::path& path) {
  std::filesystem::create_directories(path);
#if !defined(_WIN32)
  if (::chmod(path.c_str(), S_IRWXU) != 0)
    throw std::runtime_error("unable to secure memory directory: " + path.string());
#endif
}

void secure_file(const std::filesystem::path& path) {
#if !defined(_WIN32)
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
    throw std::runtime_error("unable to secure memory database: " + path.string());
#else
  (void)path;
#endif
}

bool table_exists(sqlite3* database, const char* name) {
  Statement statement(database,
      "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;");
  statement.bind(1, std::string(name));
  return statement.step() == SQLITE_ROW;
}

std::size_t current_version(sqlite3* database) {
  if (table_exists(database, "memory_schema_migrations")) {
    Statement statement(database,
        "SELECT COALESCE(MAX(version),0) FROM memory_schema_migrations;");
    if (statement.step() == SQLITE_ROW)
      return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
  }
  if (table_exists(database, "meta")) {
    Statement statement(database,
        "SELECT value FROM meta WHERE key='schema_version' LIMIT 1;");
    if (statement.step() == SQLITE_ROW)
      return static_cast<std::size_t>(std::stoull(text(statement.get(), 0)));
  }
  return 0;
}

void migration_one(sqlite3* db) {
  exec(db,
      "CREATE TABLE memories("
      "id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,scope_kind TEXT NOT NULL,"
      "user_id TEXT NOT NULL,organization_id TEXT NOT NULL,project_id TEXT NOT NULL,"
      "repository_id TEXT NOT NULL,repository_display TEXT NOT NULL,branch_id TEXT NOT NULL,"
      "task_id TEXT NOT NULL,agent_id TEXT NOT NULL,session_id TEXT NOT NULL,"
      "conversation_id TEXT NOT NULL,memory_type TEXT NOT NULL,status TEXT NOT NULL,"
      "source TEXT NOT NULL,source_id TEXT NOT NULL,source_type TEXT NOT NULL,"
      "source_uri TEXT NOT NULL,source_hash TEXT NOT NULL,source_span TEXT NOT NULL,"
      "created_by TEXT NOT NULL,evidence_strength REAL NOT NULL,content TEXT NOT NULL,"
      "confidence REAL NOT NULL,quality_score REAL NOT NULL,success_count INTEGER NOT NULL,"
      "failure_count INTEGER NOT NULL,last_outcome TEXT NOT NULL,access_count INTEGER NOT NULL,"
      "superseded_by TEXT NOT NULL,replacement_id TEXT NOT NULL,created_at INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL,last_accessed_at INTEGER NOT NULL);"
      "CREATE INDEX memories_scope_idx ON memories(scope_key,status,updated_at DESC);"
      "CREATE TABLE memory_tags(memory_id TEXT NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
      "tag TEXT NOT NULL,PRIMARY KEY(memory_id,tag));"
      "CREATE INDEX memory_tags_tag_idx ON memory_tags(tag,memory_id);"
      "CREATE VIRTUAL TABLE memory_fts USING fts5(memory_id UNINDEXED,content,source,"
      "memory_type UNINDEXED,source_uri,tokenize='unicode61');");
}

void migration_two(sqlite3* db) {
  exec(db,
      "CREATE TABLE graph_edges(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,from_id TEXT NOT NULL,"
      "to_id TEXT NOT NULL,type TEXT NOT NULL,confidence REAL NOT NULL,source TEXT NOT NULL,"
      "created_at INTEGER NOT NULL,UNIQUE(scope_key,from_id,to_id,type));"
      "CREATE INDEX graph_edges_scope_idx ON graph_edges(scope_key,from_id,to_id,type);"
      "CREATE TABLE procedure_runs(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,procedure_id TEXT NOT NULL,"
      "task_id TEXT NOT NULL,outcome TEXT NOT NULL,notes TEXT NOT NULL,evidence_uri TEXT NOT NULL,"
      "created_at INTEGER NOT NULL);"
      "CREATE INDEX procedure_runs_scope_idx ON procedure_runs(scope_key,procedure_id,created_at DESC);"
      "CREATE TABLE memory_revisions(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,memory_id TEXT NOT NULL,"
      "revision_kind TEXT NOT NULL,entry_json TEXT NOT NULL,created_at INTEGER NOT NULL);"
      "CREATE INDEX memory_revisions_scope_idx ON memory_revisions(scope_key,memory_id,created_at DESC);"
      "CREATE TABLE sources(id TEXT NOT NULL,scope_key TEXT NOT NULL,source_type TEXT NOT NULL,"
      "source_uri TEXT NOT NULL,source_hash TEXT NOT NULL,display_name TEXT NOT NULL,trust_level TEXT NOT NULL,"
      "hash_status TEXT NOT NULL,ingest_count INTEGER NOT NULL,imported_at INTEGER NOT NULL,"
      "last_ingested_at INTEGER NOT NULL,last_checked_at INTEGER NOT NULL,PRIMARY KEY(scope_key,id));"
      "CREATE TABLE source_events(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,source_id TEXT NOT NULL,"
      "action TEXT NOT NULL,status TEXT NOT NULL,source_hash TEXT NOT NULL,note TEXT NOT NULL,created_at INTEGER NOT NULL);"
      "CREATE INDEX source_events_scope_idx ON source_events(scope_key,source_id,created_at DESC);");
}

void migration_three(sqlite3* db) {
  exec(db,
      "CREATE TABLE review_items(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,kind TEXT NOT NULL,status TEXT NOT NULL,"
      "priority TEXT NOT NULL,reason TEXT NOT NULL,subject_id TEXT NOT NULL,source_id TEXT NOT NULL,"
      "source_type TEXT NOT NULL,source_uri TEXT NOT NULL,source_hash TEXT NOT NULL,source_span TEXT NOT NULL,"
      "created_by TEXT NOT NULL,evidence_strength REAL NOT NULL,candidate_json TEXT NOT NULL,decision TEXT NOT NULL,"
      "decided_by TEXT NOT NULL,decision_note TEXT NOT NULL,tags_json TEXT NOT NULL,created_at INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL,decided_at INTEGER NOT NULL);"
      "CREATE INDEX review_items_scope_idx ON review_items(scope_key,status,created_at DESC);"
      "CREATE TABLE audit_events(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,actor TEXT NOT NULL,action TEXT NOT NULL,"
      "reason TEXT NOT NULL,before_id TEXT NOT NULL,after_id TEXT NOT NULL,evidence_uri TEXT NOT NULL,"
      "metadata_json TEXT NOT NULL,created_at INTEGER NOT NULL);"
      "CREATE INDEX audit_events_scope_idx ON audit_events(scope_key,created_at DESC);"
      "CREATE TABLE session_events(id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,event_type TEXT NOT NULL,"
      "source TEXT NOT NULL,payload_json TEXT NOT NULL,created_at INTEGER NOT NULL);"
      "CREATE INDEX session_events_scope_idx ON session_events(scope_key,event_type,created_at DESC);"
      "CREATE TABLE context_blocks(scope_key TEXT NOT NULL,id TEXT NOT NULL,content TEXT NOT NULL,"
      "updated_at INTEGER NOT NULL,PRIMARY KEY(scope_key,id));");
}

void migration_four(sqlite3* db) {
  exec(db,
      "ALTER TABLE session_events ADD COLUMN deduplication_key TEXT NOT NULL DEFAULT '';"
      "CREATE UNIQUE INDEX session_events_dedup_idx ON "
      "session_events(scope_key,event_type,deduplication_key) "
      "WHERE deduplication_key<>'';"
      "ALTER TABLE procedure_runs ADD COLUMN deduplication_key TEXT NOT NULL DEFAULT '';"
      "CREATE UNIQUE INDEX procedure_runs_dedup_idx ON "
      "procedure_runs(scope_key,procedure_id,deduplication_key) "
      "WHERE deduplication_key<>'';");
}

void migration_five(sqlite3* db) {
  exec(db,
      "ALTER TABLE review_items ADD COLUMN candidate_version INTEGER NOT NULL DEFAULT 1;"
      "ALTER TABLE review_items ADD COLUMN revision INTEGER NOT NULL DEFAULT 0;"
      "ALTER TABLE review_items ADD COLUMN deferred_until INTEGER NOT NULL DEFAULT 0;"
      "ALTER TABLE review_items ADD COLUMN application_status TEXT NOT NULL DEFAULT 'not_started';"
      "ALTER TABLE review_items ADD COLUMN application_error TEXT NOT NULL DEFAULT '';"
      "ALTER TABLE review_items ADD COLUMN action_key TEXT NOT NULL DEFAULT '';"
      "CREATE INDEX review_items_queue_idx ON review_items("
      "scope_key,status,priority,kind,deferred_until,created_at,id);"
      "CREATE UNIQUE INDEX review_items_action_key_idx ON review_items(scope_key,action_key) "
      "WHERE action_key<>'';"
      "CREATE TABLE review_actions("
      "id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,review_id TEXT NOT NULL,"
      "action_key TEXT NOT NULL,action TEXT NOT NULL,actor TEXT NOT NULL,note TEXT NOT NULL,"
      "before_status TEXT NOT NULL,after_status TEXT NOT NULL,"
      "before_revision INTEGER NOT NULL,after_revision INTEGER NOT NULL,"
      "application_status TEXT NOT NULL,application_error TEXT NOT NULL,"
      "result_json TEXT NOT NULL DEFAULT '{}',replay_count INTEGER NOT NULL DEFAULT 0,"
      "created_at INTEGER NOT NULL,"
      "FOREIGN KEY(review_id) REFERENCES review_items(id));"
      "CREATE UNIQUE INDEX review_actions_key_idx ON review_actions(scope_key,action_key) "
      "WHERE action_key<>'';"
      "CREATE INDEX review_actions_history_idx ON review_actions(scope_key,created_at DESC,id);"
      "CREATE TABLE review_view_state(scope_key TEXT PRIMARY KEY,state_json TEXT NOT NULL,"
      "updated_at INTEGER NOT NULL);");
}

void migration_six(sqlite3* db) {
  exec(db, R"SQL(
CREATE TABLE retrieval_owners(
 owner_key TEXT PRIMARY KEY,scope_key TEXT NOT NULL,kind TEXT NOT NULL,
 owner_id TEXT NOT NULL,revision INTEGER NOT NULL CHECK(revision>0),
 content TEXT NOT NULL,source_uri TEXT NOT NULL,deleted INTEGER NOT NULL DEFAULT 0);
CREATE INDEX retrieval_owners_scope ON retrieval_owners(scope_key,kind,deleted);
CREATE TABLE capture_candidate_receipts(
 scope_key TEXT NOT NULL,deduplication_key TEXT NOT NULL,canonical_content TEXT NOT NULL,
 memory_id TEXT NOT NULL,PRIMARY KEY(scope_key,deduplication_key));
CREATE TABLE retrieval_jobs(
 id INTEGER PRIMARY KEY AUTOINCREMENT,owner_key TEXT NOT NULL REFERENCES retrieval_owners(owner_key) ON DELETE CASCADE,
 owner_revision INTEGER NOT NULL,representation TEXT NOT NULL,generation TEXT NOT NULL,
 state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','leased','complete','failed','superseded')),
 lease_token TEXT NOT NULL DEFAULT '',lease_until INTEGER NOT NULL DEFAULT 0,
 attempts INTEGER NOT NULL DEFAULT 0,next_attempt INTEGER NOT NULL DEFAULT 0,
 error TEXT NOT NULL DEFAULT '',UNIQUE(owner_key,owner_revision,representation,generation));
CREATE INDEX retrieval_jobs_claim ON retrieval_jobs(state,next_attempt,lease_until,id);
CREATE TABLE retrieval_chunks(
 id TEXT PRIMARY KEY,owner_key TEXT NOT NULL REFERENCES retrieval_owners(owner_key) ON DELETE CASCADE,
 owner_revision INTEGER NOT NULL,ordinal INTEGER NOT NULL,content TEXT NOT NULL,
 content_hash TEXT NOT NULL,source_uri TEXT NOT NULL,source_span TEXT NOT NULL,
 render_version TEXT NOT NULL,UNIQUE(owner_key,owner_revision,ordinal,render_version));
CREATE INDEX retrieval_chunks_owner ON retrieval_chunks(owner_key,owner_revision);
CREATE TABLE retrieval_dense_indexes(
 scope_key TEXT NOT NULL,generation TEXT NOT NULL,config_json TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('building','active','retired')),
 PRIMARY KEY(scope_key,generation));
CREATE UNIQUE INDEX retrieval_dense_active ON retrieval_dense_indexes(scope_key) WHERE state='active';
CREATE TABLE retrieval_vectors(
 chunk_id TEXT NOT NULL REFERENCES retrieval_chunks(id) ON DELETE CASCADE,
 generation TEXT NOT NULL,dimensions INTEGER NOT NULL,vector BLOB NOT NULL,
 PRIMARY KEY(chunk_id,generation));
CREATE INDEX retrieval_vectors_generation ON retrieval_vectors(generation,chunk_id);
CREATE VIRTUAL TABLE retrieval_fts USING fts5(chunk_id UNINDEXED,content,source_uri,tokenize='unicode61');
CREATE TRIGGER retrieval_chunk_insert AFTER INSERT ON retrieval_chunks BEGIN
 INSERT INTO retrieval_fts(chunk_id,content,source_uri) VALUES(NEW.id,NEW.content,NEW.source_uri);
END;
CREATE TRIGGER retrieval_chunk_delete AFTER DELETE ON retrieval_chunks BEGIN
 DELETE FROM retrieval_fts WHERE chunk_id=OLD.id;
END;
CREATE TABLE memory_entities(
 id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,scope_json TEXT NOT NULL,
 kind TEXT NOT NULL,name TEXT NOT NULL);
CREATE INDEX memory_entities_scope ON memory_entities(scope_key,name);
CREATE TABLE memory_entity_aliases(
 entity_id TEXT NOT NULL REFERENCES memory_entities(id) ON DELETE CASCADE,
 alias TEXT NOT NULL COLLATE NOCASE,PRIMARY KEY(entity_id,alias));
CREATE INDEX memory_entity_alias_lookup ON memory_entity_aliases(alias,entity_id);
CREATE TABLE retrieval_code_sources(
 owner_key TEXT PRIMARY KEY REFERENCES retrieval_owners(owner_key) ON DELETE CASCADE,
 repository TEXT NOT NULL,worktree TEXT NOT NULL,path TEXT NOT NULL,
 fingerprint TEXT NOT NULL,snapshot_generation INTEGER NOT NULL,sync_token TEXT NOT NULL);
CREATE TABLE memory_assertions(
 memory_id TEXT NOT NULL REFERENCES memories(id) ON DELETE CASCADE,revision INTEGER NOT NULL,
 subject_id TEXT NOT NULL REFERENCES memory_entities(id),predicate TEXT NOT NULL,
 object_id TEXT NOT NULL,literal_json TEXT NOT NULL,qualifiers_json TEXT NOT NULL,
 positive INTEGER NOT NULL,valid_from INTEGER,valid_to INTEGER,evidence_json TEXT NOT NULL,
 recorded_at INTEGER NOT NULL,historical_json TEXT NOT NULL,
 PRIMARY KEY(memory_id,revision));
CREATE INDEX memory_assertions_subject ON memory_assertions(subject_id,predicate,valid_from,valid_to);
CREATE INDEX memory_assertions_object ON memory_assertions(object_id,predicate);
CREATE TABLE memory_assertion_revisions(
 memory_id TEXT NOT NULL,revision INTEGER NOT NULL,recorded_at INTEGER NOT NULL,
 PRIMARY KEY(memory_id,revision));
CREATE TABLE retrieval_cross_store_links(
 id TEXT PRIMARY KEY,scope_key TEXT NOT NULL,scope_json TEXT NOT NULL,
 from_kind TEXT NOT NULL,from_id TEXT NOT NULL,from_revision INTEGER NOT NULL DEFAULT 0,
 from_repository TEXT NOT NULL,from_worktree TEXT NOT NULL,from_path TEXT NOT NULL,
 from_fingerprint TEXT NOT NULL,from_generation INTEGER NOT NULL DEFAULT 0,
 to_kind TEXT NOT NULL,to_id TEXT NOT NULL,to_revision INTEGER NOT NULL DEFAULT 0,
 to_repository TEXT NOT NULL,to_worktree TEXT NOT NULL,to_path TEXT NOT NULL,
 to_fingerprint TEXT NOT NULL,to_generation INTEGER NOT NULL DEFAULT 0,
 relation TEXT NOT NULL,evidence_id TEXT NOT NULL,created_at INTEGER NOT NULL);
CREATE INDEX retrieval_cross_store_links_from
 ON retrieval_cross_store_links(scope_key,from_kind,from_id,relation,id);
CREATE INDEX retrieval_cross_store_links_to
 ON retrieval_cross_store_links(scope_key,to_kind,to_id,relation,id);
CREATE INDEX graph_edges_reverse_idx ON graph_edges(scope_key,to_id,type,from_id);
INSERT INTO retrieval_owners(owner_key,scope_key,kind,owner_id,revision,content,source_uri)
 SELECT 'memory:'||id,scope_key,'memory',id,1,content,source_uri FROM memories;
INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation)
 SELECT owner_key,revision,'chunks','text-v1' FROM retrieval_owners;
INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at)
 SELECT id,1,created_at FROM memories;
CREATE TRIGGER retrieval_memory_insert AFTER INSERT ON memories BEGIN
 INSERT INTO retrieval_owners(owner_key,scope_key,kind,owner_id,revision,content,source_uri)
 VALUES('memory:'||NEW.id,NEW.scope_key,'memory',NEW.id,1,NEW.content,NEW.source_uri);
 INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation)
 VALUES('memory:'||NEW.id,1,'chunks','text-v1');
 INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at)
 VALUES(NEW.id,1,NEW.created_at);
END;
CREATE TRIGGER retrieval_memory_update AFTER UPDATE OF
 content,memory_type,status,source_id,source_type,source_uri,source_hash,source_span,
 created_by,evidence_strength,superseded_by,replacement_id,scope_key ON memories BEGIN
 UPDATE retrieval_owners SET revision=revision+1,content=NEW.content,
 source_uri=NEW.source_uri,scope_key=NEW.scope_key WHERE owner_key='memory:'||NEW.id;
 INSERT INTO memory_assertions
 SELECT a.memory_id,o.revision,a.subject_id,a.predicate,a.object_id,a.literal_json,
        a.qualifiers_json,a.positive,a.valid_from,a.valid_to,a.evidence_json,NEW.updated_at,
        json_object('content',NEW.content,'source',NEW.source,'type',NEW.memory_type,
          'status',NEW.status,'source_id',NEW.source_id,'source_type',NEW.source_type,
          'source_uri',NEW.source_uri,'source_hash',NEW.source_hash,'source_span',NEW.source_span,
          'created_by',NEW.created_by,'evidence_strength',NEW.evidence_strength,
          'created_at',NEW.created_at,'updated_at',NEW.updated_at)
 FROM memory_assertions a JOIN retrieval_owners o ON o.owner_key='memory:'||a.memory_id
 WHERE a.memory_id=NEW.id AND a.revision=o.revision-1
   AND NEW.content=OLD.content AND NEW.scope_key=OLD.scope_key
   AND NEW.source_id=OLD.source_id AND NEW.source_type=OLD.source_type
   AND NEW.source_uri=OLD.source_uri AND NEW.source_hash=OLD.source_hash
   AND NEW.source_span=OLD.source_span AND NEW.created_by=OLD.created_by;
 INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at)
 SELECT NEW.id,o.revision,NEW.updated_at FROM retrieval_owners o
 WHERE o.owner_key='memory:'||NEW.id;
 DELETE FROM retrieval_chunks WHERE owner_key='memory:'||NEW.id;
 UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0
 WHERE owner_key='memory:'||NEW.id AND state IN ('pending','leased','failed');
 INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation)
 SELECT owner_key,revision,'chunks','text-v1' FROM retrieval_owners WHERE owner_key='memory:'||NEW.id;
END;
CREATE TRIGGER retrieval_memory_delete AFTER DELETE ON memories BEGIN
 UPDATE retrieval_owners SET revision=revision+1,deleted=1,content='',source_uri=''
 WHERE owner_key='memory:'||OLD.id;
 DELETE FROM retrieval_chunks WHERE owner_key='memory:'||OLD.id;
 UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0
 WHERE owner_key='memory:'||OLD.id;
END;
)SQL");
}

void migration_seven(sqlite3* db) {
  // The retry policy is part of the durable job contract: a worker can crash
  // after claiming a lease and before it has a chance to report a failure.
  // Existing jobs retain the historical default ceiling.
  exec(db, "ALTER TABLE retrieval_jobs ADD COLUMN max_attempts INTEGER NOT NULL DEFAULT 5 "
           "CHECK(max_attempts BETWEEN 1 AND 100);");
}

}  // namespace

SQLiteMemoryDatabase::SQLiteMemoryDatabase(std::filesystem::path agent_home,
                                           DatabaseOptions options)
    : root_(std::move(agent_home) / "memory"),
      path_(root_ / "memory.sqlite3"), options_(options) {
  try {
    initialize();
  } catch (...) {
    if (database_ != nullptr) sqlite3_close_v2(database_);
    database_ = nullptr;
    throw;
  }
}

SQLiteMemoryDatabase::~SQLiteMemoryDatabase() {
  if (database_ != nullptr) sqlite3_close_v2(database_);
}

std::string SQLiteMemoryDatabase::integrity_check() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_, "PRAGMA quick_check;");
  if (statement.step() != SQLITE_ROW) {
    throw std::runtime_error("memory database integrity check returned no result");
  }
  return text(statement.get(), 0);
}

void SQLiteMemoryDatabase::initialize() {
  secure_directory(root_);
  secure_directory(root_ / "blocks");
  secure_directory(root_ / "exports");
  if (sqlite3_open_v2(path_.c_str(), &database_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const auto error = database_ == nullptr ? "unable to allocate sqlite handle" :
                                               sqlite3_errmsg(database_);
    if (database_ != nullptr) sqlite3_close_v2(database_);
    database_ = nullptr;
    throw std::runtime_error("unable to open memory database: " + std::string(error));
  }
  secure_file(path_);
  sqlite3_busy_timeout(database_, static_cast<int>(options_.busy_timeout.count()));
  exec(database_, "PRAGMA foreign_keys=ON;");
  const bool has_migrations = table_exists(database_, "memory_schema_migrations");
  const bool has_legacy_meta = table_exists(database_, "meta");
  schema_version_ = current_version(database_);
  if (schema_version_ > kSchemaVersion) {
    throw std::runtime_error("unsupported memory schema version " +
                             std::to_string(schema_version_));
  }
  if (!has_migrations && has_legacy_meta) {
    throw std::runtime_error(
        "standalone qtmem database requires explicit offline import");
  }
  sqlite3_busy_handler(database_, busy_retry, &options_);
  exec(database_, "PRAGMA journal_mode=WAL;");
  exec(database_, "PRAGMA synchronous=FULL;");
  if (!table_exists(database_, "memory_schema_migrations")) {
    exec(database_, "CREATE TABLE IF NOT EXISTS memory_schema_migrations(version INTEGER PRIMARY KEY,applied_at INTEGER NOT NULL);");
  }
  using Migration = void (*)(sqlite3*);
  const Migration migrations[] = {migration_one, migration_two, migration_three,
                                  migration_four, migration_five, migration_six,
                                  migration_seven};
  for (std::size_t version = schema_version_ + 1; version <= kSchemaVersion; ++version) {
    Transaction transaction(database_, true);
    const auto locked_version = current_version(database_);
    if (locked_version >= version) {
      transaction.commit();
      schema_version_ = locked_version;
      continue;
    }
    migrations[version - 1](database_);
    Statement record(database_,
        "INSERT INTO memory_schema_migrations(version,applied_at) VALUES(?,?);");
    record.bind(1, static_cast<std::int64_t>(version));
    record.bind(2, timestamp(std::chrono::system_clock::now()));
    if (record.step() != SQLITE_DONE) throw std::runtime_error("unable to record memory migration");
    transaction.commit();
    schema_version_ = version;
  }
}

std::string SQLiteMemoryDatabase::add(const MemoryScope& scope, MemoryWrite write) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  write.source = sanitize_text(std::move(write.source));
  write.provenance.source_id = sanitize_text(std::move(write.provenance.source_id));
  write.provenance.source_uri =
      sanitize_text(std::move(write.provenance.source_uri), kMaximumDurableUri);
  write.provenance.source_hash = sanitize_text(std::move(write.provenance.source_hash));
  write.provenance.source_span = sanitize_text(std::move(write.provenance.source_span));
  write.provenance.created_by = sanitize_text(std::move(write.provenance.created_by));
  write.content = sanitize_text(std::move(write.content), kMaximumDurableText);
  for (auto& tag : write.tags) tag = sanitize_text(std::move(tag), 256U);
  const auto id = make_id("mem_");
  const auto now = std::chrono::system_clock::now();
  Transaction transaction(database_);
  Statement insert(database_,
      "INSERT INTO memories(" + memory_columns() + ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
  int i = 1;
  insert.bind(i++, id); insert.bind(i++, scope_key(scope)); insert.bind(i++, std::string(to_string(scope.kind)));
  insert.bind(i++, scope.user); insert.bind(i++, scope.organization); insert.bind(i++, scope.project);
  insert.bind(i++, scope.repository);
  insert.bind(i++, sanitize_text(scope.repository_display, kMaximumDurableUri));
  insert.bind(i++, scope.branch);
  insert.bind(i++, scope.task); insert.bind(i++, scope.agent); insert.bind(i++, scope.session); insert.bind(i++, scope.conversation);
  insert.bind(i++, std::string(to_string(write.type))); insert.bind(i++, std::string(to_string(write.status)));
  insert.bind(i++, write.source); insert.bind(i++, write.provenance.source_id);
  insert.bind(i++, std::string(to_string(write.provenance.source_type))); insert.bind(i++, write.provenance.source_uri);
  insert.bind(i++, write.provenance.source_hash); insert.bind(i++, write.provenance.source_span);
  insert.bind(i++, write.provenance.created_by); insert.bind(i++, write.provenance.evidence_strength);
  insert.bind(i++, write.content); insert.bind(i++, write.confidence); insert.bind(i++, write.quality_score);
  insert.bind(i++, std::int64_t{0}); insert.bind(i++, std::int64_t{0}); insert.bind(i++, std::string("unknown"));
  insert.bind(i++, std::int64_t{0}); insert.bind(i++, std::string{}); insert.bind(i++, std::string{});
  insert.bind(i++, timestamp(now)); insert.bind(i++, timestamp(now)); insert.bind(i++, std::int64_t{0});
  if (insert.step() != SQLITE_DONE) throw std::runtime_error("unable to insert memory");
  for (const auto& tag : write.tags) {
    Statement tag_insert(database_, "INSERT OR IGNORE INTO memory_tags(memory_id,tag) VALUES(?,?);");
    tag_insert.bind(1, id); tag_insert.bind(2, tag);
    if (tag_insert.step() != SQLITE_DONE) throw std::runtime_error("unable to insert memory tag");
  }
  Statement fts(database_, "INSERT INTO memory_fts(memory_id,content,source,memory_type,source_uri) VALUES(?,?,?,?,?);");
  fts.bind(1, id); fts.bind(2, write.content); fts.bind(3, write.source);
  fts.bind(4, std::string(to_string(write.type))); fts.bind(5, write.provenance.source_uri);
  if (fts.step() != SQLITE_DONE) throw std::runtime_error("unable to index memory");
  transaction.commit();
  return id;
}

std::vector<MemoryEntry> SQLiteMemoryDatabase::query(
    const MemoryScope& scope, const MemoryQuery& query) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, query.policy, query.include_global_user);
  const auto search = fts_query(query.text);
  const bool has_search = !search.empty();
  std::string sql = "SELECT " + memory_columns("m.") + " FROM memories m ";
  if (has_search) sql += "JOIN memory_fts f ON f.memory_id=m.id ";
  sql += "WHERE m.scope_key IN (" + placeholders(keys.size()) + ") ";
  if (has_search) sql += "AND memory_fts MATCH ? ";
  if (query.type) sql += "AND m.memory_type=? ";
  if (!query.statuses.empty()) sql += "AND m.status IN (" + placeholders(query.statuses.size()) + ") ";
  else if (!query.include_superseded)
    sql += "AND m.status NOT IN ('rejected','expired','superseded','deprecated','dream_artifact') ";
  for (std::size_t tag = 0; tag < query.tags.size(); ++tag)
    sql += "AND EXISTS(SELECT 1 FROM memory_tags mt WHERE mt.memory_id=m.id AND mt.tag=?) ";
  sql += "ORDER BY CASE m.scope_key ";
  for (std::size_t rank = 0; rank < keys.size(); ++rank)
    sql += "WHEN ? THEN " + std::to_string(rank) + " ";
  sql += "ELSE " + std::to_string(keys.size()) + " END,";
  sql += has_search ? "bm25(memory_fts),m.updated_at DESC,m.id " :
                      "m.updated_at DESC,m.id ";
  if (query.limit != 0) sql += "LIMIT ? OFFSET ?";
  Statement statement(database_, sql);
  int index = 1;
  bind_keys(statement, index, keys);
  if (has_search) statement.bind(index++, search);
  if (query.type) statement.bind(index++, std::string(to_string(*query.type)));
  for (auto status : query.statuses) statement.bind(index++, std::string(to_string(status)));
  for (const auto& tag : query.tags) statement.bind(index++, tag);
  bind_keys(statement, index, keys);
  if (query.limit != 0) {
    statement.bind(index++, static_cast<std::int64_t>(query.limit));
    statement.bind(index++, static_cast<std::int64_t>(query.offset));
  }
  std::vector<MemoryEntry> result;
  int rc;
  while ((rc = statement.step()) == SQLITE_ROW) result.push_back(read_memory(database_, statement.get()));
  if (rc != SQLITE_DONE) throw std::runtime_error("memory query failed: " + std::string(sqlite3_errmsg(database_)));
  return result;
}

MemoryEntry SQLiteMemoryDatabase::get(const MemoryScope& scope,
                                      const std::string& id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, false);
  Statement statement(database_, "SELECT " + memory_columns() +
      " FROM memories WHERE id=? AND scope_key IN (" + placeholders(keys.size()) + ") LIMIT 1;");
  int index = 1;
  statement.bind(index++, id);
  bind_keys(statement, index, keys);
  if (statement.step() != SQLITE_ROW) throw std::invalid_argument("memory not found in scope");
  return read_memory(database_, statement.get());
}

std::optional<MemoryEntry> SQLiteMemoryDatabase::find_for_recall(
    const MemoryScope& scope, const std::string& id, RecallPolicy policy,
    bool include_global_user) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, policy, include_global_user);
  Statement statement(database_, "SELECT " + memory_columns() +
      " FROM memories WHERE id=? AND scope_key IN (" + placeholders(keys.size()) + ") LIMIT 1;");
  int index = 1; statement.bind(index++, id); bind_keys(statement, index, keys);
  const auto rc = statement.step();
  if (rc == SQLITE_DONE) return std::nullopt;
  if (rc != SQLITE_ROW) throw std::runtime_error("unable to resolve memory for recall");
  return read_memory(database_, statement.get());
}

MemoryEntry SQLiteMemoryDatabase::update(const MemoryScope& scope,
                                         const std::string& id,
                                         const MemoryPatch& patch) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto before = get(scope, id);
  auto after = before;
  if (patch.content)
    after.content = sanitize_text(*patch.content, kMaximumDurableText);
  if (patch.type) after.type = *patch.type;
  if (patch.status) after.status = *patch.status;
  if (patch.replacement_id) {
    after.replacement_id = *patch.replacement_id;
    after.superseded_by = *patch.replacement_id;
  }
  if (patch.confidence) after.confidence = *patch.confidence;
  if (patch.quality_score) after.quality_score = *patch.quality_score;
  if (patch.provenance) {
    after.provenance = *patch.provenance;
    after.provenance.source_id =
        sanitize_text(std::move(after.provenance.source_id));
    after.provenance.source_uri = sanitize_text(
        std::move(after.provenance.source_uri), kMaximumDurableUri);
    after.provenance.source_hash =
        sanitize_text(std::move(after.provenance.source_hash));
    after.provenance.source_span =
        sanitize_text(std::move(after.provenance.source_span));
    after.provenance.created_by =
        sanitize_text(std::move(after.provenance.created_by));
    after.provenance.evidence_strength = std::max(
        0.0, std::min(1.0, after.provenance.evidence_strength));
  }
  if (patch.tags) {
    after.tags = *patch.tags;
    for (auto& tag : after.tags) tag = sanitize_text(std::move(tag), 256U);
    after.tags.erase(
        std::remove_if(after.tags.begin(), after.tags.end(),
                       [](const std::string& tag) { return tag.empty(); }),
        after.tags.end());
    std::sort(after.tags.begin(), after.tags.end());
    after.tags.erase(std::unique(after.tags.begin(), after.tags.end()),
                     after.tags.end());
  }
  after.updated_at = std::chrono::system_clock::now();
  Transaction transaction(database_);
  Statement revision(database_,
      "INSERT INTO memory_revisions(id,scope_key,memory_id,revision_kind,entry_json,created_at) VALUES(?,?,?,?,?,?);");
  revision.bind(1, make_id("rev_")); revision.bind(2, scope_key(before.scope));
  revision.bind(3, id); revision.bind(4, std::string("update"));
  revision.bind(5, memory_json(before).dump()); revision.bind(6, timestamp(after.updated_at));
  if (revision.step() != SQLITE_DONE) throw std::runtime_error("unable to record memory revision");
  Statement statement(database_,
      "UPDATE memories SET content=?,memory_type=?,status=?,replacement_id=?,superseded_by=?,"
      "confidence=?,quality_score=?,source_id=?,source_type=?,source_uri=?,source_hash=?,"
      "source_span=?,created_by=?,evidence_strength=?,updated_at=? WHERE id=? AND scope_key=?;");
  int i = 1;
  statement.bind(i++, after.content); statement.bind(i++, std::string(to_string(after.type)));
  statement.bind(i++, std::string(to_string(after.status))); statement.bind(i++, after.replacement_id);
  statement.bind(i++, after.superseded_by); statement.bind(i++, after.confidence);
  statement.bind(i++, after.quality_score);
  statement.bind(i++, after.provenance.source_id);
  statement.bind(i++, std::string(to_string(after.provenance.source_type)));
  statement.bind(i++, after.provenance.source_uri);
  statement.bind(i++, after.provenance.source_hash);
  statement.bind(i++, after.provenance.source_span);
  statement.bind(i++, after.provenance.created_by);
  statement.bind(i++, after.provenance.evidence_strength);
  statement.bind(i++, timestamp(after.updated_at));
  statement.bind(i++, id); statement.bind(i++, scope_key(before.scope));
  if (statement.step() != SQLITE_DONE || sqlite3_changes(database_) != 1)
    throw std::runtime_error("unable to update memory");
  if (patch.tags) {
    Statement remove_tags(database_, "DELETE FROM memory_tags WHERE memory_id=?;");
    remove_tags.bind(1, id);
    if (remove_tags.step() != SQLITE_DONE)
      throw std::runtime_error("unable to refresh memory tags");
    for (const auto& tag : after.tags) {
      Statement insert_tag(database_,
          "INSERT INTO memory_tags(memory_id,tag) VALUES(?,?);");
      insert_tag.bind(1, id);
      insert_tag.bind(2, tag);
      if (insert_tag.step() != SQLITE_DONE)
        throw std::runtime_error("unable to refresh memory tag");
    }
  }
  Statement remove_fts(database_, "DELETE FROM memory_fts WHERE memory_id=?;");
  remove_fts.bind(1, id); remove_fts.step();
  Statement fts(database_, "INSERT INTO memory_fts(memory_id,content,source,memory_type,source_uri) VALUES(?,?,?,?,?);");
  fts.bind(1, id); fts.bind(2, after.content); fts.bind(3, after.source);
  fts.bind(4, std::string(to_string(after.type))); fts.bind(5, after.provenance.source_uri);
  if (fts.step() != SQLITE_DONE) throw std::runtime_error("unable to refresh memory index");
  transaction.commit();
  return after;
}

bool SQLiteMemoryDatabase::forget(const MemoryScope& scope, const std::string& id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  MemoryEntry before;
  try { before = get(scope, id); } catch (const std::invalid_argument&) { return false; }
  Transaction transaction(database_);
  Statement revision(database_,
      "INSERT INTO memory_revisions(id,scope_key,memory_id,revision_kind,entry_json,created_at) VALUES(?,?,?,?,?,?);");
  revision.bind(1, make_id("rev_")); revision.bind(2, scope_key(before.scope));
  revision.bind(3, id); revision.bind(4, std::string("delete"));
  revision.bind(5, memory_json(before).dump()); revision.bind(6, timestamp(std::chrono::system_clock::now()));
  if (revision.step() != SQLITE_DONE) throw std::runtime_error("unable to record deletion revision");
  Statement edges(database_, "DELETE FROM graph_edges WHERE scope_key=? AND (from_id=? OR to_id=?);");
  edges.bind(1, scope_key(before.scope)); edges.bind(2, id); edges.bind(3, id); edges.step();
  Statement fts(database_, "DELETE FROM memory_fts WHERE memory_id=?;");
  fts.bind(1, id); fts.step();
  Statement memory(database_, "DELETE FROM memories WHERE id=? AND scope_key=?;");
  memory.bind(1, id); memory.bind(2, scope_key(before.scope));
  if (memory.step() != SQLITE_DONE) throw std::runtime_error("unable to delete memory");
  const bool removed = sqlite3_changes(database_) == 1;
  transaction.commit();
  return removed;
}

MemoryEntry SQLiteMemoryDatabase::apply_governed_update(
    const MemoryScope& scope, const std::string& id, MemoryStatus expected_status,
    const MemoryPatch& patch, std::optional<GraphEdge> edge, AuditEventWrite audit) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  Transaction transaction(database_, true);
  const auto before = get(scope, id);
  if (scope_key(before.scope) != scope_key(scope) || before.status != expected_status)
    throw std::invalid_argument("governance subject changed or is outside the exact scope");
  if (audit.actor.empty() || audit.reason.empty())
    throw std::invalid_argument("governance requires an actor and reason");
  const auto require_exact = [&](const std::string& target) {
    if (scope_key(get(scope, target).scope) != scope_key(scope))
      throw std::invalid_argument("governance endpoint is outside the exact scope");
  };
  if (patch.replacement_id && !patch.replacement_id->empty()) {
    if (*patch.replacement_id == id) throw std::invalid_argument("memory cannot replace itself");
    require_exact(*patch.replacement_id);
  }
  if (edge) { require_exact(edge->from_id); require_exact(edge->to_id); }
  const auto after = update(scope, id, patch);
  if (edge) (void)add_edge(scope, std::move(*edge));
  (void)record_audit(scope, std::move(audit));
  transaction.commit();
  return after;
}

std::vector<MemoryRevision> SQLiteMemoryDatabase::revisions(
    const MemoryScope& scope, const std::string& memory_id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, false);
  Statement statement(database_,
      "SELECT id,memory_id,revision_kind,entry_json,created_at FROM memory_revisions "
      "WHERE memory_id=? AND scope_key IN (" + placeholders(keys.size()) + ") ORDER BY created_at DESC,id;");
  int index = 1;
  statement.bind(index++, memory_id); bind_keys(statement, index, keys);
  std::vector<MemoryRevision> result;
  while (statement.step() == SQLITE_ROW) {
    result.push_back({text(statement.get(), 0), text(statement.get(), 1),
                      text(statement.get(), 2), text(statement.get(), 3),
                      timepoint(statement.get(), 4)});
  }
  return result;
}

void SQLiteMemoryDatabase::record_access(
    const MemoryScope& scope, const std::vector<std::string>& memory_ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (memory_ids.empty()) return;
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, true);
  Transaction transaction(database_);
  for (const auto& id : memory_ids) {
    Statement statement(database_, "UPDATE memories SET access_count=access_count+1,last_accessed_at=? "
        "WHERE id=? AND scope_key IN (" + placeholders(keys.size()) + ");");
    int index = 1;
    statement.bind(index++, timestamp(std::chrono::system_clock::now()));
    statement.bind(index++, id); bind_keys(statement, index, keys);
    if (statement.step() != SQLITE_DONE) throw std::runtime_error("unable to record memory access");
  }
  transaction.commit();
}

std::size_t SQLiteMemoryDatabase::size(const MemoryScope& scope) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_, "SELECT COUNT(*) FROM memories WHERE scope_key=?;");
  statement.bind(1, scope_key(scope));
  if (statement.step() != SQLITE_ROW) throw std::runtime_error("unable to count memories");
  return static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 0));
}

MemoryStatistics SQLiteMemoryDatabase::statistics(
    const MemoryScope& scope, const RecallPolicy policy,
    const bool include_global_user) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, policy, include_global_user);
  MemoryStatistics stats;

  Statement totals(
      database_,
      "SELECT COUNT(*),"
      "COALESCE(SUM(CASE WHEN status='hypothesis' THEN 1 ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN confidence<0.35 THEN 1 ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN source='' AND source_type='unknown' AND "
      "source_uri='' AND source_hash='' THEN 1 ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN status='deprecated' THEN 1 ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN memory_type='procedural' AND failure_count>"
      "success_count THEN 1 ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN status='hypothesis' AND access_count>=3 THEN 1 "
      "ELSE 0 END),0),"
      "COALESCE(SUM(CASE WHEN access_count>0 THEN 1 ELSE 0 END),0),"
      "COALESCE(SUM(access_count),0),COALESCE(SUM(success_count),0),"
      "COALESCE(SUM(failure_count),0) FROM memories WHERE scope_key IN (" +
          placeholders(keys.size()) + ");");
  int index = 1;
  bind_keys(totals, index, keys);
  if (totals.step() != SQLITE_ROW) {
    throw std::runtime_error("unable to aggregate memory statistics");
  }
  stats.memory_entries =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 0));
  stats.hypotheses =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 1));
  stats.low_confidence =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 2));
  stats.unprovenanced =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 3));
  stats.deprecated =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 4));
  stats.failed_procedures =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 5));
  stats.high_use_hypotheses =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 6));
  stats.accessed_memories =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 7));
  stats.total_accesses =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 8));
  stats.successful_outcomes =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 9));
  stats.failed_outcomes =
      static_cast<std::size_t>(sqlite3_column_int64(totals.get(), 10));

  Statement dream_events(
      database_,
      "SELECT event_type,payload_json FROM session_events WHERE scope_key IN (" +
          placeholders(keys.size()) +
          ") AND event_type IN ('memory_dream_claim','memory_dream_receipt');");
  index = 1;
  bind_keys(dream_events, index, keys);
  while (dream_events.step() == SQLITE_ROW) {
    const std::string event_type = text(dream_events.get(), 0);
    const auto payload =
        nlohmann::json::parse(text(dream_events.get(), 1), nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) continue;
    if (event_type == "memory_dream_claim") {
      ++stats.dreaming.attempted;
      const std::string trigger = payload.value("trigger", std::string{});
      const std::string model = payload.value("model", std::string{});
      if (!trigger.empty()) ++stats.dreaming.by_trigger[trigger];
      if (!model.empty()) ++stats.dreaming.by_model[model];
      continue;
    }
    const std::string error = payload.value("error", std::string{});
    if (error.empty()) {
      ++stats.dreaming.completed;
    } else {
      ++stats.dreaming.failed;
      std::string reason = payload.value("failure_reason", std::string{});
      if (reason.empty()) {
        if (error.find("rejected by the Dreaming protocol") !=
            std::string::npos) {
          reason = "protocol_rejection";
        } else if (error.find("timed out") != std::string::npos) {
          reason = "timeout";
        } else if (error.find("cancelled") != std::string::npos) {
          reason = "cancelled";
        } else if (error.find("failed to start") != std::string::npos) {
          reason = "start_failure";
        } else {
          reason = "unspecified";
        }
      }
      ++stats.dreaming.failures_by_reason[reason];
      if (reason == "protocol_rejection") {
        ++stats.dreaming.protocol_rejections;
      } else if (reason == "timeout") {
        ++stats.dreaming.timed_out;
      } else if (reason == "cancelled") {
        ++stats.dreaming.cancelled;
      } else if (reason == "start_failure") {
        ++stats.dreaming.start_failures;
      }
    }
    if (payload.value("backend_fallback", std::string{}) == "cpu") {
      ++stats.dreaming.cpu_fallbacks;
    }
    if (payload.value("review_created", false)) {
      ++stats.dreaming.reviews_created;
    }
  }
  stats.dreaming.pending =
      stats.dreaming.attempted >
              stats.dreaming.completed + stats.dreaming.failed
          ? stats.dreaming.attempted - stats.dreaming.completed -
                stats.dreaming.failed
          : 0U;

  Statement types(database_,
                  "SELECT memory_type,COUNT(*) FROM memories WHERE scope_key "
                  "IN (" +
                      placeholders(keys.size()) +
                      ") GROUP BY memory_type ORDER BY memory_type;");
  index = 1;
  bind_keys(types, index, keys);
  while (types.step() == SQLITE_ROW) {
    stats.memory_counts[text(types.get(), 0)] =
        static_cast<std::size_t>(sqlite3_column_int64(types.get(), 1));
  }

  Statement statuses(database_,
                     "SELECT status,COUNT(*) FROM memories WHERE scope_key IN "
                     "(" +
                         placeholders(keys.size()) +
                         ") GROUP BY status ORDER BY status;");
  index = 1;
  bind_keys(statuses, index, keys);
  while (statuses.step() == SQLITE_ROW) {
    stats.trust_states[text(statuses.get(), 0)] =
        static_cast<std::size_t>(sqlite3_column_int64(statuses.get(), 1));
  }

  Statement conflicts(
      database_,
      "SELECT COUNT(*),COALESCE(SUM(CASE WHEN lhs.status='superseded' OR "
      "rhs.status='superseded' OR lhs.replacement_id<>'' OR "
      "rhs.replacement_id<>'' THEN 0 ELSE 1 END),0) FROM graph_edges edge "
      "JOIN memories lhs ON lhs.id=edge.from_id JOIN memories rhs ON "
      "rhs.id=edge.to_id WHERE edge.scope_key IN (" +
          placeholders(keys.size()) + ") AND edge.type='conflicts_with';");
  index = 1;
  bind_keys(conflicts, index, keys);
  if (conflicts.step() != SQLITE_ROW) {
    throw std::runtime_error("unable to aggregate memory conflict statistics");
  }
  stats.conflict_count =
      static_cast<std::size_t>(sqlite3_column_int64(conflicts.get(), 0));
  stats.unresolved_conflicts =
      static_cast<std::size_t>(sqlite3_column_int64(conflicts.get(), 1));

  Statement reviews(database_,
                    "SELECT COUNT(*) FROM review_items WHERE scope_key IN (" +
                        placeholders(keys.size()) +
                        ") AND status='pending';");
  index = 1;
  bind_keys(reviews, index, keys);
  if (reviews.step() != SQLITE_ROW) {
    throw std::runtime_error("unable to aggregate memory review statistics");
  }
  stats.pending_reviews =
      static_cast<std::size_t>(sqlite3_column_int64(reviews.get(), 0));
  return stats;
}

std::string SQLiteMemoryDatabase::add_edge(const MemoryScope& scope, GraphEdge edge) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (edge.from_id.empty() || edge.to_id.empty() || edge.type.empty())
    throw std::invalid_argument("graph edge requires endpoints and type");
  get(scope, edge.from_id);
  get(scope, edge.to_id);
  if (edge.id.empty()) edge.id = make_id("edge_");
  edge.type = sanitize_text(std::move(edge.type), 256U);
  edge.source = sanitize_text(std::move(edge.source));
  edge.created_at = std::chrono::system_clock::now();
  Statement statement(database_,
      "INSERT INTO graph_edges(id,scope_key,from_id,to_id,type,confidence,source,created_at) "
      "VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(scope_key,from_id,to_id,type) DO UPDATE SET "
      "confidence=excluded.confidence,source=excluded.source;");
  statement.bind(1, edge.id); statement.bind(2, scope_key(scope));
  statement.bind(3, edge.from_id); statement.bind(4, edge.to_id);
  statement.bind(5, edge.type); statement.bind(6, edge.confidence);
  statement.bind(7, edge.source); statement.bind(8, timestamp(edge.created_at));
  if (statement.step() != SQLITE_DONE) throw std::runtime_error("unable to add graph edge");
  return edge.id;
}

std::vector<GraphEdge> SQLiteMemoryDatabase::query_edges(
    const MemoryScope& scope, const GraphQuery& query) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, true);
  std::string sql = "SELECT id,from_id,to_id,type,confidence,source,created_at FROM graph_edges "
                    "WHERE scope_key IN (" + placeholders(keys.size()) + ") ";
  if (!query.memory_id.empty()) sql += "AND (from_id=? OR to_id=?) ";
  if (!query.from_id.empty()) sql += "AND from_id=? ";
  if (!query.to_id.empty()) sql += "AND to_id=? ";
  if (!query.type.empty()) sql += "AND type=? ";
  sql += "ORDER BY created_at DESC,id LIMIT ?;";
  Statement statement(database_, sql);
  int index = 1;
  bind_keys(statement, index, keys);
  if (!query.memory_id.empty()) {
    statement.bind(index++, query.memory_id); statement.bind(index++, query.memory_id);
  }
  if (!query.from_id.empty()) statement.bind(index++, query.from_id);
  if (!query.to_id.empty()) statement.bind(index++, query.to_id);
  if (!query.type.empty()) statement.bind(index++, query.type);
  statement.bind(index++, static_cast<std::int64_t>(query.limit));
  std::vector<GraphEdge> result;
  while (statement.step() == SQLITE_ROW) {
    result.push_back({text(statement.get(), 0), text(statement.get(), 1),
                      text(statement.get(), 2), text(statement.get(), 3),
                      sqlite3_column_double(statement.get(), 4), text(statement.get(), 5),
                      timepoint(statement.get(), 6)});
  }
  return result;
}

std::pair<ProcedureRun, MemoryEntry> SQLiteMemoryDatabase::add_feedback(
    const MemoryScope& scope, ProcedureFeedback feedback) {
  const auto result = add_feedback_once(scope, std::move(feedback));
  return {result.run, result.memory};
}

ProcedureFeedbackResult SQLiteMemoryDatabase::add_feedback_once(
    const MemoryScope& scope, ProcedureFeedback feedback) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto memory = get(scope, feedback.procedure_id);
  if (memory.type != MemoryType::Procedural)
    throw std::invalid_argument("procedure feedback target is not procedural memory");
  ProcedureRun run;
  run.id = make_id("run_");
  run.procedure_id = feedback.procedure_id;
  run.task_id = sanitize_text(std::move(feedback.task_id));
  run.outcome = feedback.outcome;
  run.notes = sanitize_text(std::move(feedback.notes), kMaximumDurableMetadata);
  run.evidence_uri =
      sanitize_text(std::move(feedback.evidence_uri), kMaximumDurableUri);
  run.deduplication_key =
      sanitize_text(std::move(feedback.deduplication_key), 256U);
  run.created_at = std::chrono::system_clock::now();
  Transaction transaction(database_);
  Statement insert(database_, "INSERT INTO procedure_runs(id,scope_key,procedure_id,task_id,outcome,notes,evidence_uri,created_at,deduplication_key) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT DO NOTHING;");
  insert.bind(1, run.id); insert.bind(2, scope_key(memory.scope)); insert.bind(3, run.procedure_id);
  insert.bind(4, run.task_id); insert.bind(5, std::string(to_string(run.outcome)));
  insert.bind(6, run.notes); insert.bind(7, run.evidence_uri); insert.bind(8, timestamp(run.created_at));
  insert.bind(9, run.deduplication_key);
  if (insert.step() != SQLITE_DONE) throw std::runtime_error("unable to add procedure feedback");
  if (sqlite3_changes(database_) == 0) {
    if (run.deduplication_key.empty())
      throw std::runtime_error("unable to add procedure feedback");
    Statement existing(
        database_,
        "SELECT id,task_id,outcome,notes,evidence_uri,created_at "
        "FROM procedure_runs WHERE scope_key=? AND procedure_id=? AND "
        "deduplication_key=? LIMIT 1;");
    existing.bind(1, scope_key(memory.scope));
    existing.bind(2, run.procedure_id);
    existing.bind(3, run.deduplication_key);
    if (existing.step() != SQLITE_ROW)
      throw std::runtime_error("unable to resolve deduplicated procedure feedback");
    run.id = text(existing.get(), 0);
    run.task_id = text(existing.get(), 1);
    run.outcome = procedure_outcome_from_string(text(existing.get(), 2));
    run.notes = text(existing.get(), 3);
    run.evidence_uri = text(existing.get(), 4);
    run.created_at = timepoint(existing.get(), 5);
    transaction.commit();
    return {run, memory, false};
  }
  if (run.outcome == ProcedureOutcome::Success) ++memory.success_count;
  else if (run.outcome == ProcedureOutcome::Failure) ++memory.failure_count;
  memory.last_outcome = run.outcome;
  const auto attempts = memory.success_count + memory.failure_count;
  if (attempts != 0) {
    const double observed = static_cast<double>(memory.success_count) / attempts;
    memory.confidence = std::max(0.0, std::min(1.0, 0.7 * memory.confidence + 0.3 * observed));
  }
  Statement update(database_, "UPDATE memories SET success_count=?,failure_count=?,last_outcome=?,confidence=?,updated_at=? WHERE id=? AND scope_key=?;");
  update.bind(1, static_cast<std::int64_t>(memory.success_count));
  update.bind(2, static_cast<std::int64_t>(memory.failure_count));
  update.bind(3, std::string(to_string(memory.last_outcome))); update.bind(4, memory.confidence);
  memory.updated_at = std::chrono::system_clock::now();
  update.bind(5, timestamp(memory.updated_at)); update.bind(6, memory.id); update.bind(7, scope_key(memory.scope));
  if (update.step() != SQLITE_DONE) throw std::runtime_error("unable to update procedure outcome");
  transaction.commit();
  return {run, memory, true};
}

std::vector<ProcedureRun> SQLiteMemoryDatabase::runs(
    const MemoryScope& scope, const std::string& procedure_id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, false);
  Statement statement(database_, "SELECT id,procedure_id,task_id,outcome,notes,evidence_uri,created_at,deduplication_key FROM procedure_runs "
      "WHERE procedure_id=? AND scope_key IN (" + placeholders(keys.size()) + ") ORDER BY created_at DESC,id;");
  int index = 1;
  statement.bind(index++, procedure_id); bind_keys(statement, index, keys);
  std::vector<ProcedureRun> result;
  while (statement.step() == SQLITE_ROW) {
    result.push_back({text(statement.get(), 0), text(statement.get(), 1), text(statement.get(), 2),
                      procedure_outcome_from_string(text(statement.get(), 3)), text(statement.get(), 4),
                      text(statement.get(), 5), timepoint(statement.get(), 6),
                      text(statement.get(), 7)});
  }
  return result;
}

void SQLiteMemoryDatabase::record_event(const MemoryScope& scope,
                                        SourceRecord source,
                                        SourceEvent event) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (source.id.empty()) source.id = make_id("src_");
  if (event.id.empty()) event.id = make_id("sevt_");
  source.id = sanitize_text(std::move(source.id));
  source.uri = sanitize_text(std::move(source.uri), kMaximumDurableUri);
  source.hash = sanitize_text(std::move(source.hash));
  source.display_name = sanitize_text(std::move(source.display_name));
  source.trust_level = sanitize_text(std::move(source.trust_level), 256U);
  source.hash_status = sanitize_text(std::move(source.hash_status), 256U);
  event.action = sanitize_text(std::move(event.action), 256U);
  event.status = sanitize_text(std::move(event.status), 256U);
  event.source_hash = sanitize_text(std::move(event.source_hash));
  event.note = sanitize_text(std::move(event.note), kMaximumDurableMetadata);
  event.source_id = source.id;
  const auto now = std::chrono::system_clock::now();
  if (source.imported_at == TimePoint{}) source.imported_at = now;
  source.last_ingested_at = now;
  source.last_checked_at = now;
  ++source.ingest_count;
  event.created_at = now;
  Transaction transaction(database_);
  Statement upsert(database_, "INSERT INTO sources(id,scope_key,source_type,source_uri,source_hash,display_name,trust_level,hash_status,ingest_count,imported_at,last_ingested_at,last_checked_at) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(scope_key,id) DO UPDATE SET source_type=excluded.source_type,source_uri=excluded.source_uri,"
      "source_hash=excluded.source_hash,display_name=excluded.display_name,trust_level=excluded.trust_level,hash_status=excluded.hash_status,"
      "ingest_count=sources.ingest_count+1,last_ingested_at=excluded.last_ingested_at,last_checked_at=excluded.last_checked_at;");
  int i = 1;
  upsert.bind(i++, source.id); upsert.bind(i++, scope_key(scope)); upsert.bind(i++, std::string(to_string(source.type)));
  upsert.bind(i++, source.uri); upsert.bind(i++, source.hash); upsert.bind(i++, source.display_name);
  upsert.bind(i++, source.trust_level); upsert.bind(i++, source.hash_status);
  upsert.bind(i++, static_cast<std::int64_t>(source.ingest_count)); upsert.bind(i++, timestamp(source.imported_at));
  upsert.bind(i++, timestamp(source.last_ingested_at)); upsert.bind(i++, timestamp(source.last_checked_at));
  if (upsert.step() != SQLITE_DONE) throw std::runtime_error("unable to record source");
  Statement insert(database_, "INSERT INTO source_events(id,scope_key,source_id,action,status,source_hash,note,created_at) VALUES(?,?,?,?,?,?,?,?);");
  insert.bind(1, event.id); insert.bind(2, scope_key(scope)); insert.bind(3, event.source_id);
  insert.bind(4, event.action); insert.bind(5, event.status); insert.bind(6, event.source_hash);
  insert.bind(7, event.note); insert.bind(8, timestamp(event.created_at));
  if (insert.step() != SQLITE_DONE) throw std::runtime_error("unable to record source event");
  transaction.commit();
}

std::vector<SourceRecord> SQLiteMemoryDatabase::sources(const MemoryScope& scope) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_, "SELECT id,source_type,source_uri,source_hash,display_name,trust_level,hash_status,ingest_count,imported_at,last_ingested_at,last_checked_at FROM sources WHERE scope_key=? ORDER BY last_checked_at DESC,id;");
  statement.bind(1, scope_key(scope));
  std::vector<SourceRecord> result;
  while (statement.step() == SQLITE_ROW) {
    result.push_back({text(statement.get(), 0), source_type_from_string(text(statement.get(), 1)),
                      text(statement.get(), 2), text(statement.get(), 3), text(statement.get(), 4),
                      text(statement.get(), 5), text(statement.get(), 6),
                      static_cast<std::size_t>(sqlite3_column_int64(statement.get(), 7)),
                      timepoint(statement.get(), 8), timepoint(statement.get(), 9), timepoint(statement.get(), 10)});
  }
  return result;
}

std::vector<SourceEvent> SQLiteMemoryDatabase::source_events(
    const MemoryScope& scope, const std::string& source_id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_, "SELECT id,source_id,action,status,source_hash,note,created_at FROM source_events WHERE scope_key=? AND source_id=? ORDER BY created_at DESC,id;");
  statement.bind(1, scope_key(scope)); statement.bind(2, source_id);
  std::vector<SourceEvent> result;
  while (statement.step() == SQLITE_ROW) {
    result.push_back({text(statement.get(), 0), text(statement.get(), 1), text(statement.get(), 2),
                      text(statement.get(), 3), text(statement.get(), 4), text(statement.get(), 5),
                      timepoint(statement.get(), 6)});
  }
  return result;
}

std::string SQLiteMemoryDatabase::add_review(const MemoryScope& scope,
                                             ReviewItemWrite write) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Transaction transaction(database_, true);
  validate_scope(scope);
  write.kind = sanitize_text(std::move(write.kind), 256U);
  write.priority = sanitize_text(std::move(write.priority), 256U);
  write.reason = sanitize_text(std::move(write.reason), kMaximumDurableMetadata);
  write.subject_id = sanitize_text(std::move(write.subject_id));
  write.provenance.source_id = sanitize_text(std::move(write.provenance.source_id));
  write.provenance.source_uri =
      sanitize_text(std::move(write.provenance.source_uri), kMaximumDurableUri);
  write.provenance.source_hash = sanitize_text(std::move(write.provenance.source_hash));
  write.provenance.source_span = sanitize_text(std::move(write.provenance.source_span));
  write.provenance.created_by = sanitize_text(std::move(write.provenance.created_by));
  write.candidate_json = sanitize_json(std::move(write.candidate_json));
  for (auto& tag : write.tags) tag = sanitize_text(std::move(tag), 256U);
  ReviewItem candidate_item;
  candidate_item.kind = write.kind;
  candidate_item.subject_id = write.subject_id;
  candidate_item.provenance = write.provenance;
  candidate_item.candidate_json = write.candidate_json;
  candidate_item.candidate_version = write.candidate_version;
  candidate_item.reason = write.reason;
  candidate_item.tags = write.tags;
  const auto candidate = decode_review_candidate(candidate_item);
  if (candidate.diagnostics.empty()) {
    write.candidate_version = candidate.version;
    write.candidate_json = review_candidate_json(candidate).dump();
  }
  const auto id = make_id("review_");
  const auto now = std::chrono::system_clock::now();
  Statement statement(database_, "INSERT INTO review_items(id,scope_key,kind,status,priority,reason,subject_id,source_id,source_type,source_uri,source_hash,source_span,created_by,evidence_strength,candidate_json,decision,decided_by,decision_note,tags_json,created_at,updated_at,decided_at,candidate_version,revision,deferred_until,application_status,application_error,action_key) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
  int i = 1;
  statement.bind(i++, id); statement.bind(i++, scope_key(scope)); statement.bind(i++, write.kind);
  statement.bind(i++, std::string("pending")); statement.bind(i++, write.priority);
  statement.bind(i++, write.reason); statement.bind(i++, write.subject_id);
  statement.bind(i++, write.provenance.source_id); statement.bind(i++, std::string(to_string(write.provenance.source_type)));
  statement.bind(i++, write.provenance.source_uri); statement.bind(i++, write.provenance.source_hash);
  statement.bind(i++, write.provenance.source_span); statement.bind(i++, write.provenance.created_by);
  statement.bind(i++, write.provenance.evidence_strength); statement.bind(i++, write.candidate_json);
  statement.bind(i++, std::string{}); statement.bind(i++, std::string{}); statement.bind(i++, std::string{});
  statement.bind(i++, Json(write.tags).dump()); statement.bind(i++, timestamp(now));
  statement.bind(i++, timestamp(now)); statement.bind(i++, std::int64_t{0});
  statement.bind(i++, static_cast<std::int64_t>(write.candidate_version));
  statement.bind(i++, std::int64_t{0});
  statement.bind(i++, std::int64_t{0});
  statement.bind(i++, std::string("not_started"));
  statement.bind(i++, std::string{});
  statement.bind(i++, std::string{});
  if (statement.step() != SQLITE_DONE) throw std::runtime_error("unable to add review item");
  AuditEventWrite audit;
  audit.actor = write.provenance.created_by.empty() ? "memory" : write.provenance.created_by;
  audit.action = "review_create";
  audit.reason = write.reason.empty() ? "review item created" : write.reason;
  audit.after_id = id;
  audit.evidence_uri = write.provenance.source_uri;
  audit.metadata_json = Json{{"review_id", id}, {"kind", write.kind},
                             {"candidate_version", write.candidate_version}}.dump();
  (void)record_audit(scope, std::move(audit));
  transaction.commit();
  return id;
}

std::vector<ReviewItem> SQLiteMemoryDatabase::reviews(
    const MemoryScope& scope, ReviewStatus status, std::size_t limit) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_, std::string("SELECT ") + kReviewColumns +
      " FROM review_items WHERE scope_key=? AND status=? ORDER BY created_at DESC,id LIMIT ?;");
  statement.bind(1, scope_key(scope)); statement.bind(2, std::string(to_string(status)));
  statement.bind(3, static_cast<std::int64_t>(limit));
  std::vector<ReviewItem> result;
  while (statement.step() == SQLITE_ROW) result.push_back(read_review(statement.get()));
  return result;
}

ReviewItem SQLiteMemoryDatabase::review(const MemoryScope& scope,
                                        const std::string& id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_, std::string("SELECT ") + kReviewColumns +
      " FROM review_items WHERE scope_key=? AND id=? LIMIT 1;");
  statement.bind(1, scope_key(scope));
  statement.bind(2, id);
  if (statement.step() != SQLITE_ROW)
    throw std::invalid_argument("review item not found in scope");
  return read_review(statement.get());
}

std::vector<std::string> SQLiteMemoryDatabase::critical_conflict_memory_ids(
    const MemoryScope& scope) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  // A single streaming statement pins a SQLite read snapshot and avoids both
  // UI pagination and loading an unbounded vector of complete review records.
  Statement statement(database_, std::string("SELECT ") + kReviewColumns +
      " FROM review_items WHERE scope_key=? AND kind='conflict'"
      " AND status IN ('pending','deferred');");
  statement.bind(1, scope_key(scope));
  std::set<std::string> ids;
  while (statement.step() == SQLITE_ROW) {
    const auto item = read_review(statement.get());
    const auto candidate = decode_review_candidate(item);
    const auto* conflict = std::get_if<ConflictCandidate>(&candidate.payload);
    if (conflict == nullptr || conflict->severity != "critical") continue;
    if (!item.subject_id.empty()) ids.insert(item.subject_id);
    ids.insert(conflict->memory_ids.begin(), conflict->memory_ids.end());
  }
  return {ids.begin(), ids.end()};
}

std::vector<ReviewItem> SQLiteMemoryDatabase::query_reviews(
    const MemoryScope& scope, const ReviewQuery& query) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  std::string sql = std::string("SELECT ") + kReviewColumns +
                    " FROM review_items WHERE scope_key=?";
  if (query.subject_id) sql += " AND subject_id=?";
  if (!query.statuses.empty()) {
    sql += " AND (status IN (" + placeholders(query.statuses.size()) + ")";
    if (query.include_due_deferred && query.deferred_due_before)
      sql += " OR (status='deferred' AND deferred_until>0 AND deferred_until<=?)";
    sql += ")";
  }
  if (!query.kinds.empty())
    sql += " AND kind IN (" + placeholders(query.kinds.size()) + ")";
  if (!query.priorities.empty())
    sql += " AND priority IN (" + placeholders(query.priorities.size()) + ")";
  if (!query.source_types.empty())
    sql += " AND source_type IN (" + placeholders(query.source_types.size()) + ")";
  if (!query.dream_cycles.empty()) {
    sql += " AND CASE WHEN json_valid(candidate_json) THEN "
           "COALESCE(json_extract(candidate_json,'$.payload.cycle_id'),"
           "json_extract(candidate_json,'$.cycle_id'),'') ELSE '' END IN (" +
           placeholders(query.dream_cycles.size()) + ")";
  }
  if (query.stale)
    sql += *query.stale ? " AND kind='staleness'" : " AND kind<>'staleness'";
  if (query.conflict)
    sql += *query.conflict ? " AND kind='conflict'" : " AND kind<>'conflict'";
  if (query.created_after) sql += " AND created_at>=?";
  if (query.created_before) sql += " AND created_at<=?";
  if (query.decided_after) sql += " AND decided_at>=?";
  if (query.deferred_due_before && query.statuses.empty())
    sql += " AND deferred_until>0 AND deferred_until<=?";
  for (std::size_t i = 0; i < query.tags.size(); ++i)
    sql += " AND EXISTS (SELECT 1 FROM json_each(review_items.tags_json) WHERE value=?)";
  sql += " ORDER BY CASE "
         "WHEN kind='conflict' AND CASE WHEN json_valid(candidate_json) THEN "
         "COALESCE(json_extract(candidate_json,'$.severity'),"
         "json_extract(candidate_json,'$.payload.severity')) ELSE '' END="
         "'critical' THEN 0 "
         "WHEN kind='conflict' THEN 1 "
         "WHEN EXISTS (SELECT 1 FROM json_each(review_items.tags_json) "
         "WHERE value IN ('memory:critical','review:critical')) THEN 2 ELSE 3 END,"
         " CASE priority WHEN 'urgent' THEN 0 WHEN 'high' THEN 1 "
         "WHEN 'normal' THEN 2 WHEN 'low' THEN 3 ELSE 4 END,";
  sql += query.oldest_first ? " created_at ASC,id ASC" : " created_at DESC,id ASC";
  sql += " LIMIT ? OFFSET ?;";
  Statement statement(database_, sql);
  int index = 1;
  statement.bind(index++, scope_key(scope));
  if (query.subject_id) statement.bind(index++, *query.subject_id);
  for (const auto status : query.statuses)
    statement.bind(index++, std::string(to_string(status)));
  if (!query.statuses.empty() && query.include_due_deferred &&
      query.deferred_due_before)
    statement.bind(index++, timestamp(*query.deferred_due_before));
  for (const auto& kind : query.kinds) statement.bind(index++, kind);
  for (const auto& priority : query.priorities) statement.bind(index++, priority);
  for (const auto source_type : query.source_types)
    statement.bind(index++, std::string(to_string(source_type)));
  for (const auto& cycle : query.dream_cycles) statement.bind(index++, cycle);
  if (query.created_after) statement.bind(index++, timestamp(*query.created_after));
  if (query.created_before) statement.bind(index++, timestamp(*query.created_before));
  if (query.decided_after) statement.bind(index++, timestamp(*query.decided_after));
  if (query.deferred_due_before && query.statuses.empty())
    statement.bind(index++, timestamp(*query.deferred_due_before));
  for (const auto& tag : query.tags) statement.bind(index++, tag);
  statement.bind(index++, static_cast<std::int64_t>(query.limit));
  statement.bind(index++, static_cast<std::int64_t>(query.offset));
  std::vector<ReviewItem> result;
  while (statement.step() == SQLITE_ROW) result.push_back(read_review(statement.get()));
  if (!result.empty()) {
    std::vector<std::string> subject_ids;
    subject_ids.reserve(result.size());
    for (const auto& item : result) {
      if (!item.subject_id.empty()) subject_ids.push_back(item.subject_id);
    }
    std::sort(subject_ids.begin(), subject_ids.end());
    subject_ids.erase(std::unique(subject_ids.begin(), subject_ids.end()),
                      subject_ids.end());
    if (!subject_ids.empty()) {
      Statement signals(
          database_,
          "SELECT m.id,m.memory_type,m.status,m.confidence,m.quality_score,"
          "m.access_count,m.failure_count,m.last_accessed_at,"
          "COALESCE((SELECT s.trust_level FROM sources s WHERE "
          "s.scope_key=m.scope_key AND s.id=m.source_id LIMIT 1),'') "
          "FROM memories m WHERE m.scope_key=? AND m.id IN (" +
              placeholders(subject_ids.size()) + ");");
      int signal_index = 1;
      signals.bind(signal_index++, scope_key(scope));
      for (const auto& id : subject_ids) signals.bind(signal_index++, id);
      std::map<std::string, ReviewItem::QueueSignals> by_subject;
      const auto recent_cutoff = timestamp(std::chrono::system_clock::now() -
                                           std::chrono::hours(24 * 7));
      while (signals.step() == SQLITE_ROW) {
        ReviewItem::QueueSignals value;
        const auto type = text(signals.get(), 1);
        value.critical_type = type == "constraint" || type == "decision" ||
                              type == "procedural" || type == "preference";
        const auto status = text(signals.get(), 2);
        const double confidence = sqlite3_column_double(signals.get(), 3);
        const double quality = sqlite3_column_double(signals.get(), 4);
        const auto source_trust = text(signals.get(), 8);
        value.trust_risk =
            (status == "candidate" || status == "hypothesis" ||
                     status == "dream_artifact"
                 ? 10.0
                 : 0.0) +
            std::max(0.0, 0.6 - confidence) * 15.0 +
            std::max(0.0, 0.5 - quality) * 10.0 +
            (source_trust == "untrusted" || source_trust == "low" ? 10.0
             : source_trust == "working" ? 4.0
                                          : 0.0);
        value.access_count = static_cast<std::size_t>(
            std::max<std::int64_t>(0, sqlite3_column_int64(signals.get(), 5)));
        value.procedure_failures = static_cast<std::size_t>(
            std::max<std::int64_t>(0, sqlite3_column_int64(signals.get(), 6)));
        value.recent_context =
            sqlite3_column_int64(signals.get(), 7) >= recent_cutoff;
        by_subject.emplace(text(signals.get(), 0), value);
      }
      for (auto& item : result) {
        const auto found = by_subject.find(item.subject_id);
        if (found != by_subject.end()) item.queue_signals = found->second;
      }
    }
  }
  return result;
}

ReviewItem SQLiteMemoryDatabase::decide_review(const MemoryScope& scope,
                                               const std::string& id,
                                               ReviewDecision decision) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (decision.decided_by.empty()) throw std::invalid_argument("review decision requires actor");
  decision.decision = sanitize_text(std::move(decision.decision));
  decision.decided_by = sanitize_text(std::move(decision.decided_by));
  decision.note = sanitize_text(std::move(decision.note), kMaximumDurableMetadata);
  decision.action_key = sanitize_text(std::move(decision.action_key));
  decision.application_status =
      sanitize_text(std::move(decision.application_status), 64U);
  decision.application_error =
      sanitize_text(std::move(decision.application_error), kMaximumDurableMetadata);
  decision.application_result_json =
      sanitize_json(std::move(decision.application_result_json));
  const auto now = std::chrono::system_clock::now();
  Transaction transaction(database_, true);
  if (!decision.action_key.empty()) {
    Statement replay(database_,
        "SELECT review_id FROM review_actions WHERE scope_key=? AND action_key=? LIMIT 1;");
    replay.bind(1, scope_key(scope));
    replay.bind(2, decision.action_key);
    if (replay.step() == SQLITE_ROW) {
      if (text(replay.get(), 0) != id)
        throw std::invalid_argument("review action key belongs to another review");
      Statement count_replay(database_,
          "UPDATE review_actions SET replay_count=replay_count+1 "
          "WHERE scope_key=? AND action_key=?;");
      count_replay.bind(1, scope_key(scope));
      count_replay.bind(2, decision.action_key);
      if (count_replay.step() != SQLITE_DONE)
        throw std::runtime_error("unable to record idempotent review replay");
      transaction.commit();
      return review(scope, id);
    }
  }
  const auto before = review(scope, id);
  const bool resurfacing = decision.decision == "resurface";
  const bool reopening = decision.decision == "reopen" || resurfacing;
  const bool compensating = decision.decision == "compensate";
  if ((reopening || compensating) && decision.note.empty())
    throw std::invalid_argument(
        "review reopen or compensation requires an explicit reason");
  if ((!reopening && !compensating && before.status != ReviewStatus::Pending) ||
      ((reopening || compensating) && before.status == ReviewStatus::Pending))
    throw std::invalid_argument("review decision rejected: item is not in the required state");
  if (reopening && decision.status != ReviewStatus::Pending)
    throw std::invalid_argument("reopen must transition the review to pending");
  if (compensating && decision.status != before.status)
    throw std::invalid_argument(
        "compensation records a new action without rewriting the terminal decision");
  if (before.revision != decision.expected_revision)
    throw std::invalid_argument("review decision rejected: stale revision");
  const auto deferred_until = decision.status == ReviewStatus::Deferred &&
                                      decision.deferred_until
                                  ? timestamp(*decision.deferred_until)
                                  : std::int64_t{0};
  const std::string update_sql = compensating
      ? "UPDATE review_items SET updated_at=?,revision=revision+1,application_status=?,application_error=?,action_key=? WHERE id=? AND scope_key=? AND revision=? AND status=?;"
      : "UPDATE review_items SET status=?,decision=?,decided_by=?,decision_note=?,updated_at=?,decided_at=?,deferred_until=?,revision=revision+1,application_status=?,application_error=?,action_key=? WHERE id=? AND scope_key=? AND revision=? AND status=?;";
  Statement update(database_, update_sql);
  int update_index = 1;
  if (!compensating) {
    update.bind(update_index++, std::string(to_string(decision.status)));
    update.bind(update_index++, decision.decision);
    update.bind(update_index++, decision.decided_by);
    update.bind(update_index++, decision.note);
  }
  update.bind(update_index++, timestamp(now));
  if (!compensating) {
    update.bind(update_index++, decision.status == ReviewStatus::Pending
                                      ? std::int64_t{0}
                                      : timestamp(now));
    update.bind(update_index++, deferred_until);
  }
  update.bind(update_index++, decision.application_status);
  update.bind(update_index++, decision.application_error);
  update.bind(update_index++, decision.action_key);
  update.bind(update_index++, id);
  update.bind(update_index++, scope_key(scope));
  update.bind(update_index++,
              static_cast<std::int64_t>(decision.expected_revision));
  update.bind(update_index++, std::string(to_string(before.status)));
  if (update.step() != SQLITE_DONE || sqlite3_changes(database_) != 1)
    throw std::invalid_argument("review decision rejected by concurrent update");
  const auto action_id = make_id("review_action_");
  Statement action(database_,
      "INSERT INTO review_actions(id,scope_key,review_id,action_key,action,actor,note,before_status,after_status,before_revision,after_revision,application_status,application_error,result_json,replay_count,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
  int i = 1;
  action.bind(i++, action_id);
  action.bind(i++, scope_key(scope));
  action.bind(i++, id);
  action.bind(i++, decision.action_key);
  action.bind(i++, decision.decision);
  action.bind(i++, decision.decided_by);
  action.bind(i++, decision.note);
  action.bind(i++, std::string(to_string(before.status)));
  action.bind(i++, std::string(to_string(decision.status)));
  action.bind(i++, static_cast<std::int64_t>(before.revision));
  action.bind(i++, static_cast<std::int64_t>(before.revision + 1U));
  action.bind(i++, decision.application_status);
  action.bind(i++, decision.application_error);
  action.bind(i++, decision.application_result_json);
  action.bind(i++, std::int64_t{0});
  action.bind(i++, timestamp(now));
  if (action.step() != SQLITE_DONE)
    throw std::runtime_error("unable to record review action");
  const auto after = review(scope, id);
  auto application_result =
      Json::parse(decision.application_result_json, nullptr, false);
  if (!application_result.is_object()) application_result = Json::object();
  const auto evidence_uris = application_result.value(
      "evidence_uris", std::vector<std::string>{});
  AuditEventWrite audit;
  audit.actor = decision.decided_by;
  audit.action = resurfacing ? "review_resurface"
                           : reopening ? "review_reopen"
                           : compensating ? "review_compensation"
                                          : "review_action";
  audit.reason = decision.note.empty() ? decision.decision : decision.note;
  audit.before_id = id;
  audit.after_id = id;
  if (!evidence_uris.empty()) audit.evidence_uri = evidence_uris.front();
  audit.metadata_json = Json{{"originating_review", id},
                             {"review_id", id}, {"review_action_id", action_id},
                             {"reviewer", decision.decided_by},
                             {"action", decision.decision},
                             {"decision", decision.decision},
                             {"reason", audit.reason},
                             {"before_status", to_string(before.status)},
                             {"after_status", to_string(decision.status)},
                             {"before_revision", before.revision},
                             {"after_revision", before.revision + 1U},
                             {"before", {{"status", to_string(before.status)},
                                         {"revision", before.revision},
                                         {"decision", before.decision},
                                         {"application_status", before.application_status}}},
                             {"after", {{"status", to_string(after.status)},
                                        {"revision", after.revision},
                                        {"decision", after.decision},
                                        {"application_status", after.application_status}}},
                             {"evidence_uris", evidence_uris},
                             {"replacement_ids", application_result.value(
                                 "replacement_ids", std::vector<std::string>{})},
                             {"created_ids", application_result.value(
                                 "created_ids", std::vector<std::string>{})},
                             {"changed_ids", application_result.value(
                                 "changed_ids", std::vector<std::string>{})},
                             {"application_status", decision.application_status},
                             {"application_error", decision.application_error},
                             {"action_key", decision.action_key}}.dump();
  (void)record_audit(scope, std::move(audit));
  transaction.commit();
  return after;
}

std::vector<ReviewActionRecord> SQLiteMemoryDatabase::review_history(
    const MemoryScope& scope, const std::optional<std::string>& review_id,
    const std::size_t limit) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (limit == 0U || limit > 10000U)
    throw std::invalid_argument("review history limit must be from 1 to 10000");
  std::string sql =
      "SELECT id,review_id,action_key,action,actor,note,before_status,after_status,"
      "before_revision,after_revision,application_status,application_error,"
      "result_json,replay_count,created_at FROM review_actions WHERE scope_key=?";
  if (review_id) sql += " AND review_id=?";
  sql += " ORDER BY created_at DESC,id LIMIT ?;";
  Statement statement(database_, sql);
  int index = 1;
  statement.bind(index++, scope_key(scope));
  if (review_id) statement.bind(index++, *review_id);
  statement.bind(index++, static_cast<std::int64_t>(limit));
  std::vector<ReviewActionRecord> result;
  while (statement.step() == SQLITE_ROW) {
    ReviewActionRecord action;
    int column = 0;
    action.id = text(statement.get(), column++);
    action.review_id = text(statement.get(), column++);
    action.action_key = text(statement.get(), column++);
    action.action = text(statement.get(), column++);
    action.actor = text(statement.get(), column++);
    action.note = text(statement.get(), column++);
    action.before_status =
        review_status_from_string(text(statement.get(), column++));
    action.after_status =
        review_status_from_string(text(statement.get(), column++));
    action.before_revision =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), column++));
    action.after_revision =
        static_cast<std::uint64_t>(sqlite3_column_int64(statement.get(), column++));
    action.application_status = review_application_status_from_string(
        text(statement.get(), column++));
    action.application_error = text(statement.get(), column++);
    action.result_json = text(statement.get(), column++);
    action.replay_count =
        static_cast<std::size_t>(sqlite3_column_int64(statement.get(), column++));
    action.created_at = timepoint(statement.get(), column++);
    result.push_back(std::move(action));
  }
  return result;
}

std::size_t SQLiteMemoryDatabase::resurface_due_reviews(
    const MemoryScope& scope, const TimePoint now) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  Transaction transaction(database_, true);
  Statement due(database_,
      "SELECT id,revision FROM review_items WHERE scope_key=? AND "
      "status='deferred' AND deferred_until>0 AND deferred_until<=? "
      "ORDER BY deferred_until,id;");
  due.bind(1, scope_key(scope));
  due.bind(2, timestamp(now));
  std::vector<std::pair<std::string, std::uint64_t>> rows;
  while (due.step() == SQLITE_ROW) {
    rows.emplace_back(
        text(due.get(), 0),
        static_cast<std::uint64_t>(sqlite3_column_int64(due.get(), 1)));
  }
  for (const auto& [id, revision] : rows) {
    ReviewDecision decision;
    decision.status = ReviewStatus::Pending;
    decision.decision = "resurface";
    decision.decided_by = "qt:memory_review_scheduler";
    decision.note = "scheduled review deferral elapsed";
    decision.expected_revision = revision;
    decision.action_key =
        "review:resurface:" + id + ":" + std::to_string(revision);
    decision.application_status = "no_op";
    (void)decide_review(scope, id, std::move(decision));
  }
  transaction.commit();
  return rows.size();
}

std::optional<ReviewViewState> SQLiteMemoryDatabase::load_review_view_state(
    const MemoryScope& scope) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_,
      "SELECT state_json,updated_at FROM review_view_state WHERE scope_key=? LIMIT 1;");
  statement.bind(1, scope_key(scope));
  if (statement.step() != SQLITE_ROW) return std::nullopt;
  const auto value = Json::parse(text(statement.get(), 0), nullptr, false);
  if (!value.is_object()) return std::nullopt;
  ReviewViewState state;
  if (value.contains("statuses")) {
    state.query.statuses.clear();
    for (const auto& status :
         value.value("statuses", std::vector<std::string>{})) {
      try {
        state.query.statuses.push_back(review_status_from_string(status));
      } catch (const std::invalid_argument&) {
      }
    }
  }
  state.query.kinds = value.value("kinds", std::vector<std::string>{});
  state.query.priorities = value.value("priorities", std::vector<std::string>{});
  for (const auto& source_type :
       value.value("source_types", std::vector<std::string>{})) {
    try {
      state.query.source_types.push_back(source_type_from_string(source_type));
    } catch (const std::invalid_argument&) {
    }
  }
  state.query.tags = value.value("tags", std::vector<std::string>{});
  state.query.dream_cycles =
      value.value("dream_cycles", std::vector<std::string>{});
  if (value.contains("stale") && value["stale"].is_boolean()) {
    state.query.stale = value["stale"].get<bool>();
  }
  if (value.contains("conflict") && value["conflict"].is_boolean()) {
    state.query.conflict = value["conflict"].get<bool>();
  }
  const auto scope_selector = value.value("scope", std::string("all"));
  state.query.scope = scope_selector == "exact"
                          ? ReviewQuery::ScopeSelector::Exact
                      : scope_selector == "durable"
                          ? ReviewQuery::ScopeSelector::Durable
                          : ReviewQuery::ScopeSelector::All;
  const auto time_value = [&](const char* key) -> std::optional<TimePoint> {
    const auto stored = value.value(key, std::int64_t{0});
    return stored > 0 ? std::optional<TimePoint>(
                            TimePoint(std::chrono::milliseconds(stored)))
                      : std::nullopt;
  };
  state.query.created_after = time_value("created_after");
  state.query.created_before = time_value("created_before");
  state.query.decided_after = time_value("decided_after");
  state.query.deferred_due_before = time_value("deferred_due_before");
  state.query.include_due_deferred = value.value("include_due_deferred", true);
  state.query.oldest_first = value.value("oldest_first", false);
  state.query.limit = std::min<std::size_t>(
      10000U, std::max<std::size_t>(1U, value.value("limit", std::size_t{50U})));
  state.query.offset = value.value("offset", std::size_t{0U});
  state.selected_review_id = value.value("selected_review_id", std::string{});
  state.queue_offset = value.value("queue_offset", std::size_t{0U});
  state.updated_at = timepoint(statement.get(), 1);
  return state;
}

void SQLiteMemoryDatabase::save_review_view_state(
    const MemoryScope& scope, ReviewViewState state) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  state.selected_review_id = sanitize_text(std::move(state.selected_review_id));
  const auto optional_timestamp = [](const std::optional<TimePoint>& value) {
    return value ? timestamp(*value) : std::int64_t{0};
  };
  std::vector<std::string> statuses;
  statuses.reserve(state.query.statuses.size());
  for (const auto status : state.query.statuses) statuses.emplace_back(to_string(status));
  std::vector<std::string> source_types;
  source_types.reserve(state.query.source_types.size());
  for (const auto source_type : state.query.source_types) {
    source_types.emplace_back(to_string(source_type));
  }
  const char* scope_selector =
      state.query.scope == ReviewQuery::ScopeSelector::Exact
          ? "exact"
      : state.query.scope == ReviewQuery::ScopeSelector::Durable ? "durable"
                                                                 : "all";
  auto value = Json{{"statuses", statuses},
                    {"kinds", state.query.kinds},
                    {"priorities", state.query.priorities},
                    {"source_types", source_types},
                    {"tags", state.query.tags},
                    {"dream_cycles", state.query.dream_cycles},
                    {"stale", state.query.stale ? Json(*state.query.stale)
                                                : Json(nullptr)},
                    {"conflict", state.query.conflict
                                     ? Json(*state.query.conflict)
                                     : Json(nullptr)},
                    {"scope", scope_selector},
                    {"created_after", optional_timestamp(state.query.created_after)},
                    {"created_before", optional_timestamp(state.query.created_before)},
                    {"decided_after", optional_timestamp(state.query.decided_after)},
                    {"deferred_due_before",
                     optional_timestamp(state.query.deferred_due_before)},
                    {"include_due_deferred", state.query.include_due_deferred},
                    {"oldest_first", state.query.oldest_first},
                    {"limit", std::min<std::size_t>(10000U,
                                                    std::max<std::size_t>(1U, state.query.limit))},
                    {"offset", state.query.offset},
                    {"selected_review_id", state.selected_review_id},
                    {"queue_offset", state.queue_offset}};
  const auto now = std::chrono::system_clock::now();
  Statement statement(database_,
      "INSERT INTO review_view_state(scope_key,state_json,updated_at) VALUES(?,?,?) "
      "ON CONFLICT(scope_key) DO UPDATE SET state_json=excluded.state_json,"
      "updated_at=excluded.updated_at;");
  statement.bind(1, scope_key(scope));
  statement.bind(2, sanitize_json(value.dump()));
  statement.bind(3, timestamp(now));
  if (statement.step() != SQLITE_DONE)
    throw std::runtime_error("unable to save review view state");
}

void SQLiteMemoryDatabase::clear_review_view_state(const MemoryScope& scope) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_,
      "DELETE FROM review_view_state WHERE scope_key=?;");
  statement.bind(1, scope_key(scope));
  if (statement.step() != SQLITE_DONE)
    throw std::runtime_error("unable to clear review view state");
}

ReviewStatistics SQLiteMemoryDatabase::review_statistics(
    const MemoryScope& scope) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ReviewStatistics result;
  const auto key = scope_key(scope);
  const auto now_time = std::chrono::system_clock::now();
  const auto now = timestamp(now_time);
  Statement items(database_,
      "SELECT kind,priority,status,created_at,deferred_until,source_type,candidate_json,"
      "tags_json,subject_id,evidence_strength,source_uri,source_hash FROM review_items "
      "WHERE scope_key=? ORDER BY created_at;");
  items.bind(1, key);
  std::vector<std::int64_t> pending_ages;
  std::map<std::string, std::vector<std::int64_t>> pending_ages_by_kind;
  std::map<std::string, std::vector<std::int64_t>> pending_ages_by_priority;
  std::map<std::string, std::vector<std::int64_t>> pending_ages_by_scope;
  std::map<std::string, std::vector<std::int64_t>> pending_ages_by_risk;
  while (items.step() == SQLITE_ROW) {
    const auto kind = text(items.get(), 0);
    const auto priority = text(items.get(), 1);
    const auto status = text(items.get(), 2);
    const auto created_at = sqlite3_column_int64(items.get(), 3);
    const auto deferred_until = sqlite3_column_int64(items.get(), 4);
    const auto source_type = text(items.get(), 5);
    const auto candidate = Json::parse(text(items.get(), 6), nullptr, false);
    const auto tags = Json::parse(text(items.get(), 7), nullptr, false);
    const auto subject_id = text(items.get(), 8);
    ++result.by_kind[kind];
    ++result.by_priority[priority];
    ++result.by_status[status];
    ++result.by_source_type[source_type];
    if (status == "pending") {
      ++result.pending;
      ++result.pending_by_kind[kind];
      ++result.pending_by_priority[priority];
      ++result.pending_by_scope[key];
      if (priority == "urgent" || priority == "high") ++result.urgent;
      const auto age = std::max<std::int64_t>(0, (now - created_at) / 1000);
      pending_ages.push_back(age);
      pending_ages_by_kind[kind].push_back(age);
      pending_ages_by_priority[priority].push_back(age);
      pending_ages_by_scope[key].push_back(age);
      ReviewItem risk_item;
      risk_item.kind = kind;
      risk_item.priority = priority;
      risk_item.created_at = TimePoint(
          std::chrono::duration_cast<TimePoint::duration>(
              std::chrono::milliseconds(created_at)));
      risk_item.candidate_json = text(items.get(), 6);
      risk_item.provenance.source_type = source_type_from_string(source_type);
      risk_item.provenance.evidence_strength =
          sqlite3_column_double(items.get(), 9);
      risk_item.provenance.source_uri = text(items.get(), 10);
      risk_item.provenance.source_hash = text(items.get(), 11);
      const auto risk_score = review_queue_score(risk_item, now_time);
      const std::string risk_class = risk_score >= 70.0 ? "critical"
                                     : risk_score >= 45.0 ? "high"
                                     : risk_score >= 20.0 ? "normal"
                                                          : "low";
      ++result.pending_by_risk_class[risk_class];
      pending_ages_by_risk[risk_class].push_back(age);
      if (kind == "dream_artifact" || kind == "dream_research_successor")
        ++result.dreaming_pending;
      const bool critical_candidate = candidate.is_object() &&
          (candidate.value("severity", std::string{}) == "critical" ||
           (candidate.contains("payload") && candidate["payload"].is_object() &&
            candidate["payload"].value("severity", std::string{}) == "critical"));
      const bool critical_tag = tags.is_array() &&
          std::any_of(tags.begin(), tags.end(), [](const Json& tag) {
            return tag.is_string() &&
                   (tag.get<std::string>() == "memory:critical" ||
                    tag.get<std::string>() == "review:critical");
          });
      if (kind == "conflict" && (critical_candidate || critical_tag))
        ++result.unresolved_critical_conflicts;
      if (!subject_id.empty() && kind == "staleness") {
        Statement subject(database_,
            "SELECT status "
            "FROM memories WHERE scope_key=? AND id=? LIMIT 1;");
        subject.bind(1, key);
        subject.bind(2, subject_id);
        if (subject.step() == SQLITE_ROW) {
          const auto memory_status = text(subject.get(), 0);
          if (kind == "staleness" && memory_status == "validated")
            ++result.stale_validated_subjects;
        }
      }
    } else if (status == "deferred") {
      if (deferred_until > 0 && deferred_until <= now) ++result.due_deferred;
    }
  }
  if (!pending_ages.empty()) {
    std::sort(pending_ages.begin(), pending_ages.end());
    result.oldest_pending_age_seconds = pending_ages.back();
    result.median_pending_age_seconds =
        pending_ages[(pending_ages.size() - 1U) / 2U];
  }
  const auto fill_age_distribution = [](
      std::map<std::string, std::vector<std::int64_t>>& values,
      std::map<std::string, std::int64_t>& oldest,
      std::map<std::string, std::int64_t>& median) {
    for (auto& [key_name, ages] : values) {
      std::sort(ages.begin(), ages.end());
      oldest[key_name] = ages.back();
      median[key_name] = ages[(ages.size() - 1U) / 2U];
    }
  };
  fill_age_distribution(pending_ages_by_kind,
                        result.oldest_pending_age_by_kind_seconds,
                        result.median_pending_age_by_kind_seconds);
  fill_age_distribution(pending_ages_by_priority,
                        result.oldest_pending_age_by_priority_seconds,
                        result.median_pending_age_by_priority_seconds);
  fill_age_distribution(pending_ages_by_scope,
                        result.oldest_pending_age_by_scope_seconds,
                        result.median_pending_age_by_scope_seconds);
  fill_age_distribution(pending_ages_by_risk,
                        result.oldest_pending_age_by_risk_class_seconds,
                        result.median_pending_age_by_risk_class_seconds);
  Statement actions(database_,
      "SELECT action,after_status,application_status,COUNT(*) FROM review_actions "
      "WHERE scope_key=? GROUP BY action,after_status,application_status;");
  actions.bind(1, key);
  while (actions.step() == SQLITE_ROW) {
    const auto action = text(actions.get(), 0);
    const auto status = text(actions.get(), 1);
    const auto application = text(actions.get(), 2);
    const auto count = static_cast<std::size_t>(sqlite3_column_int64(actions.get(), 3));
    if (action != "reopen" && action != "resurface" &&
        action != "compensate")
      result.reviewed += count;
    result.by_application_status[application] += count;
    result.by_outcome[action] += count;
    if (status == "accepted" && action != "compensate")
      result.approved += count;
    if (status == "rejected" && action != "compensate")
      result.rejected += count;
    if (status == "deferred" && action != "compensate")
      result.deferred += count;
    if (action == "edited_and_approved" ||
        action.rfind("edited_and_approved_as_", 0U) == 0U)
      result.edited_and_approved += count;
    if (action == "reopen") result.reopened += count;
    if (action == "resurface") result.resurfaced += count;
    if (action == "compensate") result.compensated += count;
    if (action == "merged" || action == "merge") result.merged += count;
    if (action == "superseded" || action == "supersede")
      result.superseded += count;
    if (action == "revalidated" || action == "validated" ||
        action == "approved_as_validated" ||
        action == "edited_and_approved_as_validated")
      result.validated += count;
    if (application == "failed" || application == "partially_applied") {
      result.failed += count;
      result.application_errors += count;
    }
  }
  Statement replays(database_,
      "SELECT COALESCE(SUM(replay_count),0) FROM review_actions WHERE scope_key=?;");
  replays.bind(1, key);
  if (replays.step() == SQLITE_ROW)
    result.idempotent_replays =
        static_cast<std::size_t>(sqlite3_column_int64(replays.get(), 0));
  Statement dream_actions(database_,
      "SELECT r.status,COUNT(*) FROM review_items r WHERE r.scope_key=? AND "
      "r.kind IN ('dream_artifact','dream_research_successor') AND r.status IN "
      "('accepted','rejected') GROUP BY r.status;");
  dream_actions.bind(1, key);
  while (dream_actions.step() == SQLITE_ROW) {
    const auto count = static_cast<std::size_t>(sqlite3_column_int64(dream_actions.get(), 1));
    if (text(dream_actions.get(), 0) == "accepted") result.dreaming_approved = count;
    else result.dreaming_rejected = count;
  }
  Statement first_decisions(database_,
      "SELECT (MIN(a.created_at)-r.created_at)/1000 FROM review_items r "
      "JOIN review_actions a ON a.review_id=r.id AND a.scope_key=r.scope_key "
      "WHERE r.scope_key=? GROUP BY r.id HAVING MIN(a.created_at)>=r.created_at "
      "ORDER BY 1;");
  first_decisions.bind(1, key);
  std::vector<std::int64_t> decision_times;
  while (first_decisions.step() == SQLITE_ROW)
    decision_times.push_back(sqlite3_column_int64(first_decisions.get(), 0));
  if (!decision_times.empty()) {
    const auto percentile = [&](const double fraction) {
      const auto index = static_cast<std::size_t>(
          std::ceil(fraction * static_cast<double>(decision_times.size()))) - 1U;
      return decision_times[std::min(index, decision_times.size() - 1U)];
    };
    result.time_to_first_decision_p50_seconds = percentile(0.50);
    result.time_to_first_decision_p95_seconds = percentile(0.95);
  }
  Statement review_durations(database_,
      "SELECT (MIN(a.created_at)-r.created_at)/1000 FROM review_items r "
      "JOIN review_actions a ON a.review_id=r.id AND a.scope_key=r.scope_key "
      "WHERE r.scope_key=? AND a.after_status IN ('accepted','rejected') "
      "GROUP BY r.id HAVING MIN(a.created_at)>=r.created_at ORDER BY 1;");
  review_durations.bind(1, key);
  std::vector<std::int64_t> review_durations_seconds;
  while (review_durations.step() == SQLITE_ROW) {
    review_durations_seconds.push_back(
        sqlite3_column_int64(review_durations.get(), 0));
  }
  if (!review_durations_seconds.empty()) {
    const auto percentile = [&](const double fraction) {
      const auto index = static_cast<std::size_t>(
          std::ceil(fraction *
                    static_cast<double>(review_durations_seconds.size()))) -
                         1U;
      return review_durations_seconds[
          std::min(index, review_durations_seconds.size() - 1U)];
    };
    result.review_duration_p50_seconds = percentile(0.50);
    result.review_duration_p95_seconds = percentile(0.95);
  }
  Statement trust_risk(database_,
      "SELECT status,source_id,source_uri,evidence_strength,access_count FROM memories "
      "WHERE scope_key=? AND status IN ('active','hypothesis');");
  trust_risk.bind(1, key);
  while (trust_risk.step() == SQLITE_ROW) {
    const auto status = text(trust_risk.get(), 0);
    const bool low_trust = text(trust_risk.get(), 1).empty() &&
                           text(trust_risk.get(), 2).empty() &&
                           sqlite3_column_double(trust_risk.get(), 3) <= 0.0;
    const auto accesses = sqlite3_column_int64(trust_risk.get(), 4);
    if (status == "active" && low_trust)
      ++result.unprovenanced_active_subjects;
    if (accesses >= 5 && low_trust)
      ++result.high_use_low_trust_subjects;
  }
  Statement batch_events(database_,
      "SELECT metadata_json FROM audit_events WHERE scope_key=? AND action='review_batch';");
  batch_events.bind(1, key);
  while (batch_events.step() == SQLITE_ROW) {
    const auto payload = Json::parse(text(batch_events.get(), 0), nullptr, false);
    if (!payload.is_object()) continue;
    ++result.batch_runs;
    result.batch_items += payload.value(
        "batch_size",
        payload.value("size", payload.value("item_count", std::size_t{0U})));
    result.batch_applied_items +=
        payload.value("applied_count", std::size_t{0U});
    result.batch_failed_items +=
        payload.value("failed_count", std::size_t{0U});
    if (payload.value("partial_failure", false)) ++result.batch_partial_failures;
    result.batch_retries += payload.value("retry_count", std::size_t{0U});
    result.batch_idempotent_replays +=
        payload.value("idempotent_replay_count", std::size_t{0U});
    result.batch_compensations +=
        payload.value("compensation_count", std::size_t{0U});
    result.batch_application_errors +=
        payload.value("application_error_count",
                      payload.value("failed_count", std::size_t{0U}));
  }
  return result;
}

ReviewApplicationResult SQLiteMemoryDatabase::apply_review_mutation(
    const MemoryScope& scope, const std::string& id, ReviewDecision decision,
    ReviewAtomicMutation mutation) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Transaction transaction(database_, true);
  ReviewApplicationResult result;
  result.review_id = id;
  if (!decision.action_key.empty()) {
    Statement replay(database_,
        "SELECT id,review_id,result_json,after_revision,application_status FROM review_actions "
        "WHERE scope_key=? AND action_key=? LIMIT 1;");
    replay.bind(1, scope_key(scope));
    replay.bind(2, decision.action_key);
    if (replay.step() == SQLITE_ROW) {
      if (text(replay.get(), 1) != id)
        throw std::invalid_argument(
            "review action key belongs to another review");
      const auto prior = Json::parse(text(replay.get(), 2), nullptr, false);
      result.action_id = text(replay.get(), 0);
      result.revision =
          static_cast<std::uint64_t>(sqlite3_column_int64(replay.get(), 3));
      result.status = review_application_status_from_string(text(replay.get(), 4));
      result.decision_persisted = true;
      result.idempotent_replay = true;
      if (prior.is_object()) {
        result.domain_mutations_applied =
            prior.value("domain_mutations_applied", false);
        result.changed_ids =
            prior.value("changed_ids", std::vector<std::string>{});
        result.failed_ids = prior.value("failed_ids", std::vector<std::string>{});
        result.error = prior.value("error", std::string{});
      }
      Statement count_replay(database_,
          "UPDATE review_actions SET replay_count=replay_count+1 WHERE id=?;");
      count_replay.bind(1, result.action_id);
      if (count_replay.step() != SQLITE_DONE)
        throw std::runtime_error("unable to record idempotent review replay");
      transaction.commit();
      return result;
    }
  }
  if (mutation.require_domain_mutation && mutation.memories.empty() &&
      mutation.edges.empty())
    throw std::invalid_argument("review action requires a domain mutation");
  if (mutation.expected_review_revision || mutation.expected_review_status) {
    const auto current_review = review(scope, id);
    if ((mutation.expected_review_revision &&
         current_review.revision != *mutation.expected_review_revision) ||
        (mutation.expected_review_status &&
         current_review.status != *mutation.expected_review_status)) {
      throw std::invalid_argument("review changed during verification");
    }
  }
  for (const auto& expected : mutation.expected_memories) {
    const auto current = get(scope, expected.id);
    if (scope_key(current.scope) != scope_key(scope) ||
        memory_json(current) != memory_json(expected)) {
      throw std::invalid_argument("review evidence subject changed during verification");
    }
  }
  std::vector<std::string> created_ids;
  std::vector<std::string> replacement_ids;
  std::vector<std::string> evidence_uris;
  for (auto& memory : mutation.memories) {
    if (memory.operation != ReviewMemoryMutation::Operation::Create) continue;
    const auto expected_reference = review_created_reference(created_ids.size());
    if (!memory.memory_id.empty() && memory.memory_id != expected_reference)
      throw std::invalid_argument(
          "review create memory_id must be empty or its $created:N reference");
    const auto& create_scope = memory.target_scope ? *memory.target_scope : scope;
    validate_scope(create_scope);
    if (!scope_can_access(scope, create_scope, RecallPolicy::Broad, true))
      throw std::invalid_argument(
          "review create target scope is unrelated to the review authority scope");
    if (static_cast<int>(create_scope.kind) > static_cast<int>(scope.kind))
      throw std::invalid_argument(
          "review create target scope cannot be broader than the review authority scope");
    const auto created = add(create_scope, std::move(memory.create));
    created_ids.push_back(created);
    result.changed_ids.push_back(created);
    const auto created_entry = get(create_scope, created);
    if (!created_entry.provenance.source_uri.empty())
      evidence_uris.push_back(created_entry.provenance.source_uri);
    AuditEventWrite audit;
    audit.actor = decision.decided_by;
    audit.action = "review_memory_create";
    audit.reason = decision.note.empty() ? decision.decision : decision.note;
    audit.after_id = created;
    audit.evidence_uri = created_entry.provenance.source_uri;
    audit.metadata_json =
        Json{{"originating_review", id},
             {"review_id", id},
             {"action_key", decision.action_key},
             {"before", nullptr},
             {"after", memory_json(created_entry)},
             {"created_ids", Json::array({created})},
             {"changed_ids", Json::array({created})}}.dump();
    (void)record_audit(scope, std::move(audit));
  }
  const auto resolve_created = [&](const std::string& reference) {
    constexpr std::string_view prefix = "$created:";
    if (reference.compare(0U, prefix.size(), prefix) != 0) return reference;
    const auto suffix = reference.substr(prefix.size());
    if (suffix.empty() ||
        !std::all_of(suffix.begin(), suffix.end(), [](const unsigned char ch) {
          return std::isdigit(ch) != 0;
        }))
      throw std::invalid_argument("invalid review created-memory reference");
    const auto index = static_cast<std::size_t>(std::stoull(suffix));
    if (index >= created_ids.size())
      throw std::invalid_argument("review created-memory reference is out of range");
    return created_ids[index];
  };
  for (auto& memory : mutation.memories) {
    if (memory.operation == ReviewMemoryMutation::Operation::Patch) {
      memory.memory_id = resolve_created(memory.memory_id);
      if (memory.patch.replacement_id)
        memory.patch.replacement_id =
            resolve_created(*memory.patch.replacement_id);
      if (memory.memory_id.empty())
        throw std::invalid_argument("review memory patch requires an id");
      const auto before_memory = get(scope, memory.memory_id);
      if (memory.expected_memory &&
          memory_json(before_memory) != memory_json(*memory.expected_memory)) {
        throw std::invalid_argument("review subject changed during verification");
      }
      if (scope_key(before_memory.scope) != scope_key(scope))
        throw std::invalid_argument("review mutation subject is outside the exact scope");
      if (memory.patch.replacement_id &&
          !memory.patch.replacement_id->empty())
        replacement_ids.push_back(*memory.patch.replacement_id);
      if (memory.patch.provenance &&
          !memory.patch.provenance->source_uri.empty())
        evidence_uris.push_back(memory.patch.provenance->source_uri);
      (void)update(scope, memory.memory_id, memory.patch);
      const auto after_memory = get(scope, memory.memory_id);
      result.changed_ids.push_back(memory.memory_id);
      AuditEventWrite audit;
      audit.actor = decision.decided_by;
      audit.action = "review_memory_patch";
      audit.reason = decision.note.empty() ? decision.decision : decision.note;
      audit.before_id = memory.memory_id;
      audit.after_id = memory.memory_id;
      audit.evidence_uri = after_memory.provenance.source_uri;
      audit.metadata_json =
          Json{{"originating_review", id},
               {"review_id", id},
               {"action_key", decision.action_key},
               {"before", memory_json(before_memory)},
               {"after", memory_json(after_memory)},
               {"replacement_ids", replacement_ids},
               {"changed_ids", Json::array({memory.memory_id})}}.dump();
      (void)record_audit(scope, std::move(audit));
    }
  }
  for (auto& edge : mutation.edges) {
    edge.from_id = resolve_created(edge.from_id);
    edge.to_id = resolve_created(edge.to_id);
    const auto edge_before_write = edge;
    const auto edge_id = add_edge(scope, std::move(edge));
    result.changed_ids.push_back(edge_id);
    AuditEventWrite audit;
    audit.actor = decision.decided_by;
    audit.action = "review_graph_change";
    audit.reason = decision.note.empty() ? decision.decision : decision.note;
    audit.after_id = edge_id;
    audit.metadata_json =
        Json{{"originating_review", id},
             {"review_id", id},
             {"action_key", decision.action_key},
             {"before", nullptr},
             {"after", {{"id", edge_id},
                         {"from_id", edge_before_write.from_id},
                         {"to_id", edge_before_write.to_id},
                         {"type", edge_before_write.type},
                         {"confidence", edge_before_write.confidence},
                         {"source", edge_before_write.source}}},
             {"changed_ids", Json::array({edge_id})}}.dump();
    (void)record_audit(scope, std::move(audit));
  }
  const bool compensation = decision.decision == "compensate" ||
                            decision.application_status == "compensated";
  decision.application_status =
      compensation ? "compensated"
                   : mutation.memories.empty() && mutation.edges.empty()
                         ? "no_op"
                         : "applied";
  decision.application_result_json =
      Json{{"domain_mutations_applied", !result.changed_ids.empty()},
           {"created_ids", created_ids},
           {"changed_ids", result.changed_ids},
           {"replacement_ids", replacement_ids},
           {"evidence_uris", evidence_uris},
           {"failed_ids", Json::array()},
           {"error", ""}}.dump();
  const auto decided = decide_review(scope, id, std::move(decision));
  result.revision = decided.revision;
  if (!decided.action_key.empty()) {
    Statement action(database_,
        "SELECT id FROM review_actions WHERE scope_key=? AND action_key=? LIMIT 1;");
    action.bind(1, scope_key(scope));
    action.bind(2, decided.action_key);
    if (action.step() == SQLITE_ROW) result.action_id = text(action.get(), 0);
  }
  result.decision_persisted = true;
  result.domain_mutations_applied = !result.changed_ids.empty();
  result.status = compensation
                      ? ReviewApplicationStatus::Compensated
                      : result.domain_mutations_applied
                            ? ReviewApplicationStatus::Applied
                            : ReviewApplicationStatus::NoOp;
  transaction.commit();
  return result;
}

std::string SQLiteMemoryDatabase::record_audit(const MemoryScope& scope,
                                               AuditEventWrite write) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (write.actor.empty() || write.action.empty() || write.reason.empty())
    throw std::invalid_argument("audit event requires actor, action, and reason");
  write.actor = sanitize_text(std::move(write.actor));
  write.action = sanitize_text(std::move(write.action));
  write.reason = sanitize_text(std::move(write.reason), kMaximumDurableMetadata);
  write.before_id = sanitize_text(std::move(write.before_id));
  write.after_id = sanitize_text(std::move(write.after_id));
  write.evidence_uri =
      sanitize_text(std::move(write.evidence_uri), kMaximumDurableUri);
  write.metadata_json = sanitize_json(std::move(write.metadata_json));
  const auto id = make_id("audit_");
  Statement statement(database_, "INSERT INTO audit_events(id,scope_key,actor,action,reason,before_id,after_id,evidence_uri,metadata_json,created_at) VALUES(?,?,?,?,?,?,?,?,?,?);");
  statement.bind(1, id); statement.bind(2, scope_key(scope)); statement.bind(3, write.actor);
  statement.bind(4, write.action); statement.bind(5, write.reason); statement.bind(6, write.before_id);
  statement.bind(7, write.after_id); statement.bind(8, write.evidence_uri);
  statement.bind(9, write.metadata_json); statement.bind(10, timestamp(std::chrono::system_clock::now()));
  if (statement.step() != SQLITE_DONE) throw std::runtime_error("unable to record audit event");
  return id;
}

std::vector<AuditEvent> SQLiteMemoryDatabase::audit_events(
    const MemoryScope& scope, std::size_t limit) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Statement statement(database_, "SELECT id,actor,action,reason,before_id,after_id,evidence_uri,metadata_json,created_at FROM audit_events WHERE scope_key=? ORDER BY created_at DESC,id LIMIT ?;");
  statement.bind(1, scope_key(scope)); statement.bind(2, static_cast<std::int64_t>(limit));
  std::vector<AuditEvent> result;
  while (statement.step() == SQLITE_ROW) {
    result.push_back({text(statement.get(), 0), text(statement.get(), 1), text(statement.get(), 2),
                      text(statement.get(), 3), text(statement.get(), 4), text(statement.get(), 5),
                      text(statement.get(), 6), text(statement.get(), 7), timepoint(statement.get(), 8)});
  }
  return result;
}

std::string SQLiteMemoryDatabase::record_session_event(
    const MemoryScope& scope, SessionEventWrite write) {
  write.deduplication_key.clear();
  return record_session_event_once(scope, std::move(write)).id;
}

SessionEventRecordResult SQLiteMemoryDatabase::record_session_event_once(
    const MemoryScope& scope, SessionEventWrite write) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (write.event_type.empty()) throw std::invalid_argument("session event type is required");
  write.event_type = sanitize_text(std::move(write.event_type), 256U);
  write.source = sanitize_text(std::move(write.source));
  write.payload_json = sanitize_json(std::move(write.payload_json));
  write.deduplication_key =
      sanitize_text(std::move(write.deduplication_key), 256U);
  const auto id = make_id("session_evt_");
  Statement statement(database_, "INSERT INTO session_events(id,scope_key,event_type,source,payload_json,created_at,deduplication_key) VALUES(?,?,?,?,?,?,?) ON CONFLICT DO NOTHING;");
  statement.bind(1, id); statement.bind(2, scope_key(scope)); statement.bind(3, write.event_type);
  statement.bind(4, write.source); statement.bind(5, write.payload_json);
  statement.bind(6, timestamp(std::chrono::system_clock::now()));
  statement.bind(7, write.deduplication_key);
  if (statement.step() != SQLITE_DONE) throw std::runtime_error("unable to record session event");
  if (sqlite3_changes(database_) != 0) return {id, true};
  if (write.deduplication_key.empty())
    throw std::runtime_error("unable to record session event");
  Statement existing(
      database_,
      "SELECT id FROM session_events WHERE scope_key=? AND event_type=? AND "
      "deduplication_key=? LIMIT 1;");
  existing.bind(1, scope_key(scope));
  existing.bind(2, write.event_type);
  existing.bind(3, write.deduplication_key);
  if (existing.step() != SQLITE_ROW)
    throw std::runtime_error("unable to resolve deduplicated session event");
  return {text(existing.get(), 0), false};
}

std::vector<SessionEvent> SQLiteMemoryDatabase::session_events(
    const MemoryScope& scope, const std::string& event_type,
    std::size_t limit, std::size_t offset) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::string sql = "SELECT id,event_type,source,payload_json,created_at,deduplication_key FROM session_events WHERE scope_key=? ";
  if (!event_type.empty()) sql += "AND event_type=? ";
  sql += "ORDER BY created_at DESC,id LIMIT ? OFFSET ?;";
  Statement statement(database_, sql);
  int index = 1;
  statement.bind(index++, scope_key(scope));
  if (!event_type.empty()) statement.bind(index++, event_type);
  statement.bind(index++, static_cast<std::int64_t>(limit));
  statement.bind(index++, static_cast<std::int64_t>(offset));
  std::vector<SessionEvent> result;
  while (statement.step() == SQLITE_ROW) {
    result.push_back({text(statement.get(), 0), text(statement.get(), 1),
                      text(statement.get(), 2), text(statement.get(), 3),
                      timepoint(statement.get(), 4), text(statement.get(), 5)});
  }
  return result;
}

ContextBlock SQLiteMemoryDatabase::get_block(const MemoryScope& scope,
                                             const std::string& id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, false);
  std::string ordering = "CASE scope_key ";
  for (std::size_t i = 0; i < keys.size(); ++i)
    ordering += "WHEN ? THEN " + std::to_string(i) + " ";
  ordering += "ELSE 999 END";
  Statement statement(database_, "SELECT id,content,updated_at FROM context_blocks WHERE id=? AND scope_key IN (" +
      placeholders(keys.size()) + ") ORDER BY " + ordering + " LIMIT 1;");
  int index = 1;
  statement.bind(index++, id); bind_keys(statement, index, keys); bind_keys(statement, index, keys);
  if (statement.step() != SQLITE_ROW) return {id, {}, {}};
  return {text(statement.get(), 0), text(statement.get(), 1), timepoint(statement.get(), 2)};
}

ContextBlock SQLiteMemoryDatabase::update_block(const MemoryScope& scope,
                                                const std::string& id,
                                                std::string content) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (id.empty()) throw std::invalid_argument("context block id is required");
  content = sanitize_text(std::move(content), kMaximumDurableText);
  const auto now = std::chrono::system_clock::now();
  Statement statement(database_, "INSERT INTO context_blocks(scope_key,id,content,updated_at) VALUES(?,?,?,?) ON CONFLICT(scope_key,id) DO UPDATE SET content=excluded.content,updated_at=excluded.updated_at;");
  statement.bind(1, scope_key(scope)); statement.bind(2, id);
  statement.bind(3, content); statement.bind(4, timestamp(now));
  if (statement.step() != SQLITE_DONE) throw std::runtime_error("unable to update context block");
  return {id, std::move(content), now};
}

std::vector<ContextBlock> SQLiteMemoryDatabase::blocks(const MemoryScope& scope) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, false);
  std::string ordering = "CASE scope_key ";
  for (std::size_t i = 0; i < keys.size(); ++i)
    ordering += "WHEN ? THEN " + std::to_string(i) + " ";
  ordering += "ELSE 999 END,id";
  Statement statement(database_, "SELECT id,content,updated_at FROM context_blocks WHERE scope_key IN (" +
      placeholders(keys.size()) + ") ORDER BY " + ordering + ";");
  int index = 1;
  bind_keys(statement, index, keys); bind_keys(statement, index, keys);
  std::vector<ContextBlock> result;
  std::unordered_map<std::string, bool> seen;
  while (statement.step() == SQLITE_ROW) {
    auto id = text(statement.get(), 0);
    if (seen.emplace(id, true).second)
      result.push_back({std::move(id), text(statement.get(), 1), timepoint(statement.get(), 2)});
  }
  return result;
}

nlohmann::json SQLiteMemoryDatabase::export_state(const MemoryScope& scope) const {
  return export_state(scope, ExportLimits{});
}

nlohmann::json SQLiteMemoryDatabase::export_state(
    const MemoryScope& scope, const ExportLimits& limits) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto limit : {
           limits.memories, limits.graph_edges, limits.procedure_runs,
           limits.revisions, limits.sources, limits.source_events,
           limits.reviews, limits.audit_events, limits.session_events,
           limits.blocks}) {
    if (limit == 0 || limit > 100000) {
      throw std::invalid_argument(
          "memory export collection limits must be from 1 to 100000");
    }
  }
  Json state{{"schema_version", kSchemaVersion},
             {"scope_key", scope_key(scope)},
             {"scope", Json{{"kind", to_string(scope.kind)}, {"user", scope.user},
                              {"organization", scope.organization}, {"project", scope.project},
                              {"repository", scope.repository}, {"repository_display", scope.repository_display},
                              {"branch", scope.branch}, {"task", scope.task}, {"agent", scope.agent},
                              {"session", scope.session}, {"conversation", scope.conversation}}},
             {"export_limits",
              Json{{"memories", limits.memories},
                   {"graph_edges", limits.graph_edges},
                   {"procedure_runs", limits.procedure_runs},
                   {"revisions", limits.revisions},
                   {"sources", limits.sources},
                   {"source_events", limits.source_events},
                   {"reviews", limits.reviews},
                   {"audit_events", limits.audit_events},
                   {"session_events", limits.session_events},
                   {"blocks", limits.blocks}}}};
  const auto count_rows = [&](const char* table, bool broad_scope) {
    const auto keys = broad_scope
        ? allowed_keys(scope, RecallPolicy::Broad, true)
        : std::vector<std::string>{scope_key(scope)};
    Statement count(database_, "SELECT COUNT(*) FROM " + std::string(table) +
                                   " WHERE scope_key IN (" +
                                   placeholders(keys.size()) + ");");
    int index = 1;
    bind_keys(count, index, keys);
    if (count.step() != SQLITE_ROW)
      throw std::runtime_error("unable to count bounded memory export rows");
    return static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
  };
  state["truncated"] =
      Json{{"memories", count_rows("memories", false) > limits.memories},
           {"graph_edges", count_rows("graph_edges", false) > limits.graph_edges},
           {"procedure_runs", count_rows("procedure_runs", false) > limits.procedure_runs},
           {"revisions", count_rows("memory_revisions", false) > limits.revisions},
           {"sources", count_rows("sources", false) > limits.sources},
           {"source_events", count_rows("source_events", false) > limits.source_events},
           {"reviews", count_rows("review_items", false) > limits.reviews},
           {"review_actions", count_rows("review_actions", false) >
                                  limits.audit_events},
           {"audit_events", count_rows("audit_events", false) > limits.audit_events},
           {"session_events", count_rows("session_events", false) > limits.session_events},
           {"blocks", count_rows("context_blocks", false) > limits.blocks},
           {"entities", false}, {"assertions", false},
           {"assertion_revision_boundaries", false}, {"cross_store_links", false}};
  auto bounded_count = [&](const char* sql) {
    Statement count(database_, sql);
    count.bind(1, scope_key(scope));
    if (count.step() != SQLITE_ROW) throw std::runtime_error("unable to count structured export rows");
    return static_cast<std::size_t>(sqlite3_column_int64(count.get(), 0));
  };
  state["truncated"]["entities"] = bounded_count(
      "SELECT COUNT(*) FROM memory_entities WHERE scope_key=?") > limits.memories;
  state["truncated"]["assertions"] = bounded_count(
      "SELECT COUNT(*) FROM memory_assertions a JOIN memories m ON m.id=a.memory_id WHERE m.scope_key=?") > limits.revisions;
  state["truncated"]["cross_store_links"] = bounded_count(
      "SELECT COUNT(*) FROM retrieval_cross_store_links WHERE scope_key=?") > limits.revisions;
  state["truncated"]["assertion_revision_boundaries"] = bounded_count(
      "SELECT COUNT(*) FROM memory_assertion_revisions r JOIN memories m ON m.id=r.memory_id WHERE m.scope_key=?") > limits.revisions;
  MemoryQuery query;
  query.policy = RecallPolicy::Strict;
  query.include_superseded = true;
  query.limit = limits.memories;
  state["memories"] = Json::array();
  const auto exported_memories = this->query(scope, query);
  for (const auto& memory : exported_memories) {
    auto encoded = memory_json(memory);
    Statement revision(database_, "SELECT revision FROM retrieval_owners WHERE owner_key=?;");
    revision.bind(1, "memory:" + memory.id);
    if (revision.step() == SQLITE_ROW)
      encoded["retrieval_revision"] = sqlite3_column_int64(revision.get(), 0);
    state["memories"].push_back(std::move(encoded));
  }
  state["entities"] = Json::array();
  Statement entity_rows(database_,
      "SELECT id,kind,name,scope_json FROM memory_entities WHERE scope_key=? "
      "ORDER BY id LIMIT ?;");
  entity_rows.bind(1, scope_key(scope));
  entity_rows.bind(2, static_cast<std::int64_t>(limits.memories));
  while (entity_rows.step() == SQLITE_ROW) {
    const auto id = text(entity_rows.get(), 0);
    Json aliases = Json::array();
    Statement alias_rows(database_,
                         "SELECT alias FROM memory_entity_aliases WHERE entity_id=? ORDER BY alias;");
    alias_rows.bind(1, id);
    while (alias_rows.step() == SQLITE_ROW) aliases.push_back(text(alias_rows.get(), 0));
    state["entities"].push_back(Json{{"id", id}, {"kind", text(entity_rows.get(), 1)},
                                      {"name", text(entity_rows.get(), 2)},
                                      {"scope", Json::parse(text(entity_rows.get(), 3))},
                                      {"aliases", aliases}});
  }
  state["assertions"] = Json::array();
  state["assertion_revision_boundaries"] = Json::array();
  Statement boundary_rows(database_,
      "SELECT r.memory_id,r.revision,r.recorded_at FROM memory_assertion_revisions r "
      "JOIN memories m ON m.id=r.memory_id WHERE m.scope_key=? ORDER BY r.memory_id,r.revision LIMIT ?;");
  boundary_rows.bind(1, scope_key(scope));
  boundary_rows.bind(2, static_cast<std::int64_t>(limits.revisions));
  while (boundary_rows.step() == SQLITE_ROW)
    state["assertion_revision_boundaries"].push_back(Json{{"memory_id", text(boundary_rows.get(), 0)},
        {"revision", sqlite3_column_int64(boundary_rows.get(), 1)},
        {"recorded_at", sqlite3_column_int64(boundary_rows.get(), 2)}});
  Statement assertion_rows(database_,
      "SELECT a.memory_id,a.revision,a.subject_id,a.predicate,a.object_id,a.literal_json,"
      "a.qualifiers_json,a.positive,a.valid_from,a.valid_to,a.evidence_json,a.recorded_at,a.historical_json "
      "FROM memory_assertions a JOIN memories m ON m.id=a.memory_id "
      "WHERE m.scope_key=? ORDER BY a.memory_id,a.revision LIMIT ?;");
  assertion_rows.bind(1, scope_key(scope));
  assertion_rows.bind(2, static_cast<std::int64_t>(limits.revisions));
  while (assertion_rows.step() == SQLITE_ROW) {
    Json item{{"memory_id", text(assertion_rows.get(), 0)},
              {"revision", sqlite3_column_int64(assertion_rows.get(), 1)},
              {"subject_id", text(assertion_rows.get(), 2)},
              {"predicate", text(assertion_rows.get(), 3)},
              {"object_id", text(assertion_rows.get(), 4)},
              {"literal_json", text(assertion_rows.get(), 5)},
              {"qualifiers_json", text(assertion_rows.get(), 6)},
              {"positive", sqlite3_column_int(assertion_rows.get(), 7) != 0},
              {"evidence", Json::parse(text(assertion_rows.get(), 10))},
              {"recorded_at", sqlite3_column_int64(assertion_rows.get(), 11)},
              {"historical_json", text(assertion_rows.get(), 12)}};
    if (sqlite3_column_type(assertion_rows.get(), 8) != SQLITE_NULL)
      item["valid_from"] = sqlite3_column_int64(assertion_rows.get(), 8);
    if (sqlite3_column_type(assertion_rows.get(), 9) != SQLITE_NULL)
      item["valid_to"] = sqlite3_column_int64(assertion_rows.get(), 9);
    state["assertions"].push_back(std::move(item));
  }
  state["cross_store_links"] = Json::array();
  Statement link_rows(database_,
      "SELECT id,scope_json,from_kind,from_id,from_revision,from_repository,from_worktree,"
      "from_path,from_fingerprint,from_generation,to_kind,to_id,to_revision,to_repository,"
      "to_worktree,to_path,to_fingerprint,to_generation,relation,evidence_id,created_at "
      "FROM retrieval_cross_store_links WHERE scope_key=? ORDER BY id LIMIT ?;");
  link_rows.bind(1, scope_key(scope));
  link_rows.bind(2, static_cast<std::int64_t>(limits.revisions));
  while (link_rows.step() == SQLITE_ROW) {
    const auto endpoint = [&](int offset) {
      return Json{{"kind", text(link_rows.get(), offset)},
                  {"id", text(link_rows.get(), offset + 1)},
                  {"revision", sqlite3_column_int64(link_rows.get(), offset + 2)},
                  {"repository", text(link_rows.get(), offset + 3)},
                  {"worktree", text(link_rows.get(), offset + 4)},
                  {"path", text(link_rows.get(), offset + 5)},
                  {"source_fingerprint", text(link_rows.get(), offset + 6)},
                  {"snapshot_generation", sqlite3_column_int64(link_rows.get(), offset + 7)}};
    };
    state["cross_store_links"].push_back(Json{
        {"id", text(link_rows.get(), 0)}, {"scope", Json::parse(text(link_rows.get(), 1))},
        {"from", endpoint(2)}, {"to", endpoint(10)},
        {"relation", text(link_rows.get(), 18)}, {"evidence_id", text(link_rows.get(), 19)},
        {"created_at", sqlite3_column_int64(link_rows.get(), 20)}});
  }
  state["graph_edges"] = Json::array();
  Statement edge_rows(
      database_,
      "SELECT id,from_id,to_id,type,confidence,source,created_at FROM "
      "graph_edges WHERE scope_key=? ORDER BY created_at DESC,id LIMIT ?;");
  edge_rows.bind(1, scope_key(scope));
  edge_rows.bind(2, static_cast<std::int64_t>(limits.graph_edges));
  while (edge_rows.step() == SQLITE_ROW) {
    state["graph_edges"].push_back(
        Json{{"id", text(edge_rows.get(), 0)},
             {"from_id", text(edge_rows.get(), 1)},
             {"to_id", text(edge_rows.get(), 2)},
             {"type", sanitize_text(text(edge_rows.get(), 3), 256U)},
             {"confidence", sqlite3_column_double(edge_rows.get(), 4)},
             {"source", sanitize_text(text(edge_rows.get(), 5))},
             {"created_at", sqlite3_column_int64(edge_rows.get(), 6)}});
  }
  state["blocks"] = Json::array();
  Statement block_rows(
      database_,
      "SELECT id,content,updated_at FROM context_blocks WHERE scope_key=? "
      "ORDER BY id LIMIT ?;");
  block_rows.bind(1, scope_key(scope));
  block_rows.bind(2, static_cast<std::int64_t>(limits.blocks));
  while (block_rows.step() == SQLITE_ROW) {
    state["blocks"].push_back(
        Json{{"id", text(block_rows.get(), 0)},
             {"content", sanitize_text(text(block_rows.get(), 1),
                                       kMaximumDurableText)},
             {"updated_at", sqlite3_column_int64(block_rows.get(), 2)}});
  }
  state["sources"] = Json::array();
  auto exported_sources = sources(scope);
  if (exported_sources.size() > limits.sources)
    exported_sources.resize(limits.sources);
  for (const auto& source : exported_sources) {
    state["sources"].push_back(Json{{"id", source.id}, {"type", to_string(source.type)},
        {"uri", source.uri}, {"hash", source.hash}, {"display_name", source.display_name},
        {"trust_level", source.trust_level}, {"hash_status", source.hash_status},
        {"ingest_count", source.ingest_count}, {"imported_at", timestamp(source.imported_at)},
        {"last_ingested_at", timestamp(source.last_ingested_at)},
        {"last_checked_at", timestamp(source.last_checked_at)}});
  }
  state["source_events"] = Json::array();
  std::size_t exported_source_events = 0;
  for (const auto& source : exported_sources) {
    for (const auto& event : source_events(scope, source.id)) {
      if (exported_source_events++ >= limits.source_events) break;
      state["source_events"].push_back(
          Json{{"id", event.id},
               {"source_id", event.source_id},
               {"action", event.action},
               {"status", event.status},
               {"source_hash", event.source_hash},
               {"note", event.note},
               {"created_at", timestamp(event.created_at)}});
    }
    if (exported_source_events >= limits.source_events) break;
  }
  state["memory_revisions"] = Json::array();
  state["procedure_runs"] = Json::array();
  std::size_t exported_revisions = 0;
  std::size_t exported_runs = 0;
  for (const auto& memory : exported_memories) {
    for (const auto& revision : revisions(scope, memory.id)) {
      if (exported_revisions++ >= limits.revisions) break;
      state["memory_revisions"].push_back(
          Json{{"id", revision.id},
               {"memory_id", revision.memory_id},
               {"kind", revision.kind},
               {"entry_json", revision.entry_json},
               {"created_at", timestamp(revision.created_at)}});
    }
    if (memory.type == MemoryType::Procedural) {
      for (const auto& run : runs(scope, memory.id)) {
        if (exported_runs++ >= limits.procedure_runs) break;
        state["procedure_runs"].push_back(
            Json{{"id", run.id},
                 {"procedure_id", run.procedure_id},
                 {"task_id", run.task_id},
                 {"outcome", to_string(run.outcome)},
                 {"notes", run.notes},
                 {"evidence_uri", run.evidence_uri},
                 {"deduplication_key", run.deduplication_key},
                 {"created_at", timestamp(run.created_at)}});
      }
    }
  }
  state["review_items"] = Json::array();
  std::size_t exported_reviews = 0;
  for (const auto status : {ReviewStatus::Pending, ReviewStatus::Accepted,
                            ReviewStatus::Rejected, ReviewStatus::Deferred}) {
    for (const auto& review : reviews(scope, status, limits.reviews)) {
      if (exported_reviews++ >= limits.reviews) break;
      state["review_items"].push_back(
          Json{{"id", review.id},
               {"kind", review.kind},
               {"status", to_string(review.status)},
               {"priority", review.priority},
               {"reason", review.reason},
               {"subject_id", review.subject_id},
               {"source_id", review.provenance.source_id},
               {"source_type", to_string(review.provenance.source_type)},
               {"source_uri", review.provenance.source_uri},
               {"source_hash", review.provenance.source_hash},
               {"source_span", review.provenance.source_span},
               {"created_by", review.provenance.created_by},
               {"evidence_strength", review.provenance.evidence_strength},
               {"candidate_json", review.candidate_json},
               {"candidate_version", review.candidate_version},
               {"revision", review.revision},
               {"deferred_until", timestamp(review.deferred_until)},
               {"application_status", review.application_status},
               {"application_error", review.application_error},
               {"action_key", review.action_key},
               {"decision", review.decision},
               {"decided_by", review.decided_by},
               {"decision_note", review.decision_note},
               {"tags", review.tags},
               {"created_at", timestamp(review.created_at)},
               {"updated_at", timestamp(review.updated_at)},
               {"decided_at", timestamp(review.decided_at)}});
    }
    if (exported_reviews >= limits.reviews) break;
  }
  state["review_actions"] = Json::array();
  for (const auto& action :
       review_history(scope, std::nullopt, limits.audit_events)) {
    state["review_actions"].push_back(
        Json{{"id", action.id},
             {"review_id", action.review_id},
             {"action_key", action.action_key},
             {"action", action.action},
             {"actor", action.actor},
             {"note", action.note},
             {"before_status", to_string(action.before_status)},
             {"after_status", to_string(action.after_status)},
             {"before_revision", action.before_revision},
             {"after_revision", action.after_revision},
             {"application_status", to_string(action.application_status)},
             {"application_error", action.application_error},
             {"result_json", action.result_json},
             {"replay_count", action.replay_count},
             {"created_at", timestamp(action.created_at)}});
  }
  state["review_view_state"] = nullptr;
  {
    Statement view(database_,
        "SELECT state_json,updated_at FROM review_view_state WHERE scope_key=? LIMIT 1;");
    view.bind(1, scope_key(scope));
    if (view.step() == SQLITE_ROW) {
      const auto value = Json::parse(text(view.get(), 0), nullptr, false);
      if (value.is_object())
        state["review_view_state"] =
            Json{{"state", value},
                 {"updated_at", sqlite3_column_int64(view.get(), 1)}};
    }
  }
  state["audit_events"] = Json::array();
  for (const auto& audit : audit_events(scope, limits.audit_events)) {
    state["audit_events"].push_back(
        Json{{"id", audit.id},
             {"actor", audit.actor},
             {"action", audit.action},
             {"reason", audit.reason},
             {"before_id", audit.before_id},
             {"after_id", audit.after_id},
             {"evidence_uri", audit.evidence_uri},
             {"metadata_json", audit.metadata_json},
             {"created_at", timestamp(audit.created_at)}});
  }
  state["session_events"] = Json::array();
  for (const auto& event : session_events(scope, "", limits.session_events, 0U)) {
    state["session_events"].push_back(
        Json{{"id", event.id},
             {"event_type", event.event_type},
             {"source", event.source},
             {"payload_json", event.payload_json},
             {"deduplication_key", event.deduplication_key},
             {"created_at", timestamp(event.created_at)}});
  }
  return state;
}

RetentionReport SQLiteMemoryDatabase::enforce_retention(
    const MemoryScope& scope, const RetentionPolicy& policy) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (policy.batch_limit == 0 || policy.batch_limit > 10000) {
    throw std::invalid_argument("retention batch_limit must be from 1 to 10000");
  }
  if (policy.older_than == TimePoint{} && policy.keep_latest == 0) {
    throw std::invalid_argument(
        "retention requires an age cutoff or non-zero keep_latest");
  }
  const auto key = scope_key(scope);
  const auto cutoff = timestamp(policy.older_than);
  const auto select_ids = [&](const char* table, const char* time_column,
                              const std::string& extra = {}) {
    std::string sql = "SELECT id FROM " + std::string(table) +
                      " WHERE scope_key=? ";
    if (policy.older_than != TimePoint{})
      sql += "AND " + std::string(time_column) + "<? ";
    if (!extra.empty()) sql += "AND " + extra + " ";
    if (policy.keep_latest != 0) {
      sql += "AND id NOT IN (SELECT id FROM " + std::string(table) +
             " WHERE scope_key=? ORDER BY " + std::string(time_column) +
             " DESC,id DESC LIMIT ?) ";
    }
    sql += "ORDER BY " + std::string(time_column) + " ASC,id ASC LIMIT ?;";
    Statement statement(database_, sql);
    int index = 1;
    statement.bind(index++, key);
    if (policy.older_than != TimePoint{}) statement.bind(index++, cutoff);
    if (policy.keep_latest != 0) {
      statement.bind(index++, key);
      statement.bind(index++, static_cast<std::int64_t>(policy.keep_latest));
    }
    statement.bind(index++, static_cast<std::int64_t>(policy.batch_limit));
    std::vector<std::string> ids;
    while (statement.step() == SQLITE_ROW) ids.push_back(text(statement.get(), 0));
    return ids;
  };
  const auto session_ids = policy.raw_evidence
      ? select_ids("session_events", "created_at") : std::vector<std::string>{};
  const auto source_event_ids = policy.raw_evidence
      ? select_ids("source_events", "created_at") : std::vector<std::string>{};
  const auto memory_ids = policy.canonical_memories
      ? select_ids("memories", "updated_at") : std::vector<std::string>{};
  const auto audit_ids = policy.audit_events
      ? select_ids("audit_events", "created_at") : std::vector<std::string>{};
  const auto block_ids = policy.context_blocks
      ? select_ids("context_blocks", "updated_at") : std::vector<std::string>{};
  RetentionReport report;
  report.dry_run = policy.dry_run;
  report.session_events = session_ids.size();
  report.source_events = source_event_ids.size();
  report.canonical_memories = memory_ids.size();
  report.audit_events = audit_ids.size();
  report.context_blocks = block_ids.size();
  if (policy.dry_run) return report;

  Transaction transaction(database_);
  const auto remove_ids = [&](const char* table,
                              const std::vector<std::string>& ids) {
    Statement remove(database_, "DELETE FROM " + std::string(table) +
                                    " WHERE scope_key=? AND id=?;");
    for (const auto& id : ids) {
      remove.bind(1, key);
      remove.bind(2, id);
      if (remove.step() != SQLITE_DONE)
        throw std::runtime_error("retention deletion failed for " +
                                 std::string(table));
      sqlite3_reset(remove.get());
      sqlite3_clear_bindings(remove.get());
    }
  };
  remove_ids("session_events", session_ids);
  remove_ids("source_events", source_event_ids);
  remove_ids("audit_events", audit_ids);
  remove_ids("context_blocks", block_ids);
  for (const auto& id : memory_ids) {
    if (!forget(scope, id))
      throw std::runtime_error("retention memory disappeared during deletion");
    Statement links(database_,
        "DELETE FROM retrieval_cross_store_links WHERE scope_key=? AND "
        "((from_kind='memory' AND from_id=?) OR (to_kind='memory' AND to_id=?));");
    links.bind(1, key); links.bind(2, id); links.bind(3, id);
    if (links.step() != SQLITE_DONE)
      throw std::runtime_error("retention cross-store link deletion failed");
    Statement assertion_revisions(database_,
        "DELETE FROM memory_assertion_revisions WHERE memory_id=?;");
    assertion_revisions.bind(1, id);
    if (assertion_revisions.step() != SQLITE_DONE)
      throw std::runtime_error("retention assertion revision deletion failed");
  }
  // Entity rows are scoped canonical structure. Remove only entities that no
  // longer participate in a retained assertion or cross-store link.
  Statement orphan_query(database_,
      "SELECT e.id FROM memory_entities e WHERE e.scope_key=? AND "
      "NOT EXISTS(SELECT 1 FROM memory_assertions a WHERE a.subject_id=e.id OR a.object_id=e.id) AND "
      "NOT EXISTS(SELECT 1 FROM retrieval_cross_store_links l WHERE l.scope_key=e.scope_key AND "
      "((l.from_kind='entity' AND l.from_id=e.id) OR (l.to_kind='entity' AND l.to_id=e.id))) "
      "ORDER BY e.id LIMIT ?;");
  orphan_query.bind(1, key);
  orphan_query.bind(2, static_cast<std::int64_t>(policy.batch_limit));
  std::vector<std::string> orphan_entity_ids;
  while (orphan_query.step() == SQLITE_ROW)
    orphan_entity_ids.push_back(text(orphan_query.get(), 0));
  Statement orphan_entities(database_, "DELETE FROM memory_entities WHERE scope_key=? AND id=?;");
  for (const auto& id : orphan_entity_ids) {
    orphan_entities.bind(1, key); orphan_entities.bind(2, id);
    if (orphan_entities.step() != SQLITE_DONE)
      throw std::runtime_error("retention entity deletion failed");
    sqlite3_reset(orphan_entities.get()); sqlite3_clear_bindings(orphan_entities.get());
  }
  if (policy.raw_evidence) {
    std::string sql =
        "SELECT id FROM sources s WHERE scope_key=? AND NOT EXISTS(SELECT 1 "
        "FROM source_events e WHERE e.scope_key=s.scope_key AND e.source_id=s.id) "
        "AND NOT EXISTS(SELECT 1 FROM memories m WHERE m.scope_key=s.scope_key "
        "AND m.source_id=s.id) ";
    if (policy.older_than != TimePoint{}) sql += "AND last_checked_at<? ";
    sql += "ORDER BY last_checked_at ASC,id ASC LIMIT ?;";
    Statement orphan_query(database_, sql);
    int index = 1;
    orphan_query.bind(index++, key);
    if (policy.older_than != TimePoint{}) orphan_query.bind(index++, cutoff);
    orphan_query.bind(index++, static_cast<std::int64_t>(policy.batch_limit));
    std::vector<std::string> orphan_ids;
    while (orphan_query.step() == SQLITE_ROW)
      orphan_ids.push_back(text(orphan_query.get(), 0));
    remove_ids("sources", orphan_ids);
    report.orphan_sources = orphan_ids.size();
  }
  transaction.commit();
  return report;
}

RedactionScrubReport SQLiteMemoryDatabase::scrub_sensitive_data(
    const MemoryScope& scope, const RedactionScrubPolicy& policy) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (policy.batch_limit == 0U || policy.batch_limit > 10000U) {
    throw std::invalid_argument(
        "redaction scrub batch_limit must be from 1 to 10000");
  }
  const auto key = scope_key(scope);
  RedactionScrubReport report;
  report.dry_run = policy.dry_run;
  Transaction transaction(database_, !policy.dry_run);

  using Column = std::tuple<std::string, bool, std::size_t>;
  const auto scrub_collection = [&](const std::string& table,
                                    const std::vector<Column>& columns) {
    RedactionScrubCollection collection;
    collection.name = table;
    std::string select = "SELECT id";
    for (const auto& column : columns) select += "," + std::get<0>(column);
    select += " FROM " + table +
              " WHERE scope_key=? ORDER BY id LIMIT ?;";
    struct LoadedRow {
      std::string id;
      std::vector<std::string> values;
    };
    std::vector<LoadedRow> loaded;
    {
      Statement rows(database_, select);
      rows.bind(1, key);
      rows.bind(2, static_cast<std::int64_t>(policy.batch_limit + 1U));
      while (rows.step() == SQLITE_ROW) {
        LoadedRow row;
        row.id = text(rows.get(), 0);
        row.values.reserve(columns.size());
        for (std::size_t index = 0; index < columns.size(); ++index) {
          row.values.push_back(
              text(rows.get(), static_cast<int>(index + 1U)));
        }
        loaded.push_back(std::move(row));
      }
    }
    if (loaded.size() > policy.batch_limit) {
      report.truncated = true;
      loaded.resize(policy.batch_limit);
    }
    for (auto& row : loaded) {
      std::vector<std::string> values;
      values.reserve(columns.size());
      bool changed = false;
      for (std::size_t index = 0; index < columns.size(); ++index) {
        const auto& original = row.values[index];
        auto sanitized = std::get<1>(columns[index])
                             ? sanitize_json(original, std::get<2>(columns[index]))
                             : sanitize_text(original, std::get<2>(columns[index]));
        changed = changed || sanitized != original;
        values.push_back(std::move(sanitized));
      }
      ++collection.examined;
      if (!changed) continue;
      ++collection.changed;
      if (policy.dry_run) continue;
      std::string update = "UPDATE " + table + " SET ";
      for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0U) update += ",";
        update += std::get<0>(columns[index]) + "=?";
      }
      update += " WHERE scope_key=? AND id=?;";
      Statement write(database_, update);
      int bind = 1;
      for (const auto& value : values) write.bind(bind++, value);
      write.bind(bind++, key);
      write.bind(bind, row.id);
      if (write.step() != SQLITE_DONE || sqlite3_changes(database_) != 1) {
        throw std::runtime_error("redaction scrub update failed for " + table);
      }
      if (table == "memories") {
        Statement remove_fts(database_,
                             "DELETE FROM memory_fts WHERE memory_id=?;");
        remove_fts.bind(1, row.id);
        if (remove_fts.step() != SQLITE_DONE) {
          throw std::runtime_error("redaction scrub FTS deletion failed");
        }
        Statement restore_fts(
            database_,
            "INSERT INTO memory_fts(memory_id,content,source,memory_type,source_uri) "
            "SELECT id,content,source,memory_type,source_uri FROM memories "
            "WHERE scope_key=? AND id=?;");
        restore_fts.bind(1, key);
        restore_fts.bind(2, row.id);
        if (restore_fts.step() != SQLITE_DONE) {
          throw std::runtime_error("redaction scrub FTS restoration failed");
        }
      }
    }
    report.collections.push_back(std::move(collection));
  };

  scrub_collection("memories",
                   {{"repository_display", false, kMaximumDurableUri},
                    {"source", false, kMaximumDurableMetadata},
                    {"source_id", false, kMaximumDurableMetadata},
                    {"source_uri", false, kMaximumDurableUri},
                    {"source_hash", false, kMaximumDurableMetadata},
                    {"source_span", false, kMaximumDurableMetadata},
                    {"created_by", false, kMaximumDurableMetadata},
                    {"content", false, kMaximumDurableText}});
  {
    RedactionScrubCollection collection;
    collection.name = "memory_assertions";
    Statement rows(database_,
        "SELECT a.memory_id,a.revision,a.predicate,a.literal_json,a.qualifiers_json,"
        "a.evidence_json,a.historical_json "
        "FROM memory_assertions a JOIN memories m ON m.id=a.memory_id "
        "WHERE m.scope_key=? ORDER BY a.memory_id,a.revision LIMIT ?;");
    rows.bind(1, key);
    rows.bind(2, static_cast<std::int64_t>(policy.batch_limit + 1U));
    struct AssertionRow { std::string memory_id; std::int64_t revision; std::string predicate;
      std::string literal; std::string qualifiers; std::string evidence; std::string historical; };
    std::vector<AssertionRow> loaded;
    while (rows.step() == SQLITE_ROW)
      loaded.push_back({text(rows.get(), 0), sqlite3_column_int64(rows.get(), 1),
                        text(rows.get(), 2), text(rows.get(), 3), text(rows.get(), 4),
                        text(rows.get(), 5), text(rows.get(), 6)});
    if (loaded.size() > policy.batch_limit) {
      report.truncated = true;
      loaded.resize(policy.batch_limit);
    }
    for (const auto& row : loaded) {
      ++collection.examined;
      const auto predicate = sanitize_text(row.predicate, 128U);
      const auto literal = sanitize_json(row.literal, kMaximumDurableMetadata);
      const auto qualifiers = sanitize_json(row.qualifiers, kMaximumDurableMetadata);
      const auto evidence = sanitize_json(row.evidence, kMaximumDurableJson);
      const auto historical = sanitize_json(row.historical, kMaximumDurableJson);
      if (predicate == row.predicate && literal == row.literal &&
          qualifiers == row.qualifiers && evidence == row.evidence &&
          historical == row.historical) continue;
      ++collection.changed;
      if (policy.dry_run) continue;
      Statement update(database_,
          "UPDATE memory_assertions SET predicate=?,literal_json=?,qualifiers_json=?,"
          "evidence_json=?,historical_json=? "
          "WHERE memory_id=? AND revision=?;");
      update.bind(1, predicate); update.bind(2, literal); update.bind(3, qualifiers);
      update.bind(4, evidence); update.bind(5, historical);
      update.bind(6, row.memory_id); update.bind(7, row.revision);
      if (update.step() != SQLITE_DONE)
        throw std::runtime_error("redaction assertion scrub update failed");
    }
    report.collections.push_back(std::move(collection));
  }
  // Scope JSON is an identity serialization. Redacting it independently of
  // scope_key would make the row inaccessible, so only scrub entity payload.
  scrub_collection("memory_entities", { {"kind", false, 256U},
                                         {"name", false, kMaximumDurableMetadata}});
  {
    RedactionScrubCollection collection;
    collection.name = "memory_entity_aliases";
    Statement rows(database_,
        "SELECT a.entity_id,a.alias FROM memory_entity_aliases a "
        "JOIN memory_entities e ON e.id=a.entity_id WHERE e.scope_key=? "
        "ORDER BY a.entity_id,a.alias LIMIT ?;");
    rows.bind(1, key);
    rows.bind(2, static_cast<std::int64_t>(policy.batch_limit + 1U));
    struct AliasRow { std::string id; std::string alias; };
    std::vector<AliasRow> loaded;
    while (rows.step() == SQLITE_ROW)
      loaded.push_back({text(rows.get(), 0), text(rows.get(), 1)});
    if (loaded.size() > policy.batch_limit) { report.truncated = true; loaded.resize(policy.batch_limit); }
    for (const auto& row : loaded) {
      ++collection.examined;
      const auto sanitized = sanitize_text(row.alias, kMaximumDurableMetadata);
      if (sanitized == row.alias) continue;
      ++collection.changed;
      if (policy.dry_run) continue;
      Statement remove(database_, "DELETE FROM memory_entity_aliases WHERE entity_id=? AND alias=?;");
      remove.bind(1, row.id); remove.bind(2, row.alias);
      if (remove.step() != SQLITE_DONE) throw std::runtime_error("redaction alias scrub deletion failed");
      Statement insert(database_, "INSERT OR IGNORE INTO memory_entity_aliases(entity_id,alias) VALUES(?,?);");
      insert.bind(1, row.id); insert.bind(2, sanitized);
      if (insert.step() != SQLITE_DONE) throw std::runtime_error("redaction alias scrub restoration failed");
    }
    report.collections.push_back(std::move(collection));
  }
  scrub_collection("retrieval_cross_store_links",
                   {{"from_repository", false, kMaximumDurableMetadata},
                    {"from_worktree", false, kMaximumDurableUri},
                    {"from_path", false, kMaximumDurableUri},
                    {"from_fingerprint", false, kMaximumDurableMetadata},
                    {"to_repository", false, kMaximumDurableMetadata},
                    {"to_worktree", false, kMaximumDurableUri},
                    {"to_path", false, kMaximumDurableUri},
                    {"to_fingerprint", false, kMaximumDurableMetadata},
                    {"relation", false, 256U},
                    {"evidence_id", false, kMaximumDurableMetadata}});
  {
    RedactionScrubCollection collection;
    collection.name = "memory_tags";
    std::vector<std::pair<std::string, std::string>> tags;
    Statement rows(
        database_,
        "SELECT t.memory_id,t.tag FROM memory_tags t JOIN memories m ON "
        "m.id=t.memory_id WHERE m.scope_key=? ORDER BY t.memory_id,t.tag "
        "LIMIT ?;");
    rows.bind(1, key);
    rows.bind(2, static_cast<std::int64_t>(policy.batch_limit + 1U));
    while (rows.step() == SQLITE_ROW) {
      tags.emplace_back(text(rows.get(), 0), text(rows.get(), 1));
    }
    if (tags.size() > policy.batch_limit) {
      report.truncated = true;
      tags.resize(policy.batch_limit);
    }
    for (const auto& [memory_id, original] : tags) {
      ++collection.examined;
      const auto sanitized = sanitize_text(original, 256U);
      if (sanitized == original) continue;
      ++collection.changed;
      if (policy.dry_run) continue;
      Statement remove(
          database_,
          "DELETE FROM memory_tags WHERE memory_id=? AND tag=?;");
      remove.bind(1, memory_id);
      remove.bind(2, original);
      if (remove.step() != SQLITE_DONE) {
        throw std::runtime_error("redaction scrub tag deletion failed");
      }
      Statement insert(
          database_,
          "INSERT OR IGNORE INTO memory_tags(memory_id,tag) VALUES(?,?);");
      insert.bind(1, memory_id);
      insert.bind(2, sanitized);
      if (insert.step() != SQLITE_DONE) {
        throw std::runtime_error("redaction scrub tag restoration failed");
      }
    }
    report.collections.push_back(std::move(collection));
  }
  scrub_collection("graph_edges", {{"type", false, 256U},
                                     {"source", false, kMaximumDurableMetadata}});
  scrub_collection("procedure_runs",
                   {{"task_id", false, kMaximumDurableMetadata},
                    {"outcome", false, 256U},
                    {"notes", false, kMaximumDurableMetadata},
                    {"evidence_uri", false, kMaximumDurableUri}});
  scrub_collection("memory_revisions",
                   {{"revision_kind", false, 256U},
                    {"entry_json", true, kMaximumDurableJson}});
  scrub_collection("sources",
                   {{"source_type", false, 256U},
                    {"source_uri", false, kMaximumDurableUri},
                    {"source_hash", false, kMaximumDurableMetadata},
                    {"display_name", false, kMaximumDurableMetadata},
                    {"trust_level", false, 256U},
                    {"hash_status", false, 256U}});
  scrub_collection("source_events",
                   {{"action", false, 256U}, {"status", false, 256U},
                    {"source_hash", false, kMaximumDurableMetadata},
                    {"note", false, kMaximumDurableMetadata}});
  scrub_collection("review_items",
                   {{"kind", false, 256U}, {"priority", false, 256U},
                    {"reason", false, kMaximumDurableMetadata},
                    {"source_id", false, kMaximumDurableMetadata},
                    {"source_uri", false, kMaximumDurableUri},
                    {"source_hash", false, kMaximumDurableMetadata},
                    {"source_span", false, kMaximumDurableMetadata},
                    {"created_by", false, kMaximumDurableMetadata},
                    {"candidate_json", true, kMaximumDurableJson},
                    {"decision", false, kMaximumDurableMetadata},
                    {"decided_by", false, kMaximumDurableMetadata},
                    {"decision_note", false, kMaximumDurableMetadata},
                    {"application_status", false, 64U},
                    {"application_error", false, kMaximumDurableMetadata},
                    {"action_key", false, kMaximumDurableMetadata},
                    {"tags_json", true, kMaximumDurableJson}});
  scrub_collection("review_actions",
                   {{"action_key", false, kMaximumDurableMetadata},
                    {"action", false, 256U},
                    {"actor", false, kMaximumDurableMetadata},
                    {"note", false, kMaximumDurableMetadata},
                    {"application_status", false, 64U},
                    {"application_error", false, kMaximumDurableMetadata},
                    {"result_json", true, kMaximumDurableJson}});
  scrub_collection("audit_events",
                   {{"actor", false, kMaximumDurableMetadata},
                    {"action", false, kMaximumDurableMetadata},
                    {"reason", false, kMaximumDurableMetadata},
                    {"evidence_uri", false, kMaximumDurableUri},
                    {"metadata_json", true, kMaximumDurableJson}});
  scrub_collection("session_events",
                   {{"event_type", false, 256U},
                    {"source", false, kMaximumDurableMetadata},
                    {"payload_json", true, kMaximumDurableJson}});
  scrub_collection("context_blocks", {{"content", false, kMaximumDurableText}});

  if (!policy.dry_run && report.changed() != 0U) {
    AuditEventWrite audit;
    audit.actor = "qt:memory_maintenance";
    audit.action = "redaction_scrub";
    audit.reason = "explicit historical redaction maintenance";
    audit.metadata_json =
        Json{{"changed_rows", report.changed()},
             {"examined_rows", report.examined()},
             {"truncated", report.truncated}}
            .dump();
    (void)record_audit(scope, std::move(audit));
  }
  transaction.commit();
  return report;
}

void SQLiteMemoryDatabase::import_state(const MemoryScope& scope,
                                        const nlohmann::json& state,
                                        bool replace_existing) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto version = state.value("schema_version", std::size_t{0});
  if (version > kSchemaVersion)
    throw std::invalid_argument("unsupported imported memory schema version");
  validate_scope(scope);
  MemoryScope declared_scope = scope;
  if (state.contains("scope") && state["scope"].is_object()) {
    const auto encoded = state["scope"];
    declared_scope.kind = scope_kind_from_string(encoded.value("kind", to_string(scope.kind)));
    declared_scope.user = encoded.value("user", "");
    declared_scope.organization = encoded.value("organization", "");
    declared_scope.project = encoded.value("project", "");
    declared_scope.repository = encoded.value("repository", "");
    declared_scope.repository_display = encoded.value("repository_display", "");
    declared_scope.branch = encoded.value("branch", "");
    declared_scope.task = encoded.value("task", "");
    declared_scope.agent = encoded.value("agent", "");
    declared_scope.session = encoded.value("session", "");
    declared_scope.conversation = encoded.value("conversation", "");
    validate_scope(declared_scope);
    if (state.value("scope_key", std::string{}) != scope_key(declared_scope))
      throw std::invalid_argument("exported scope declaration is inconsistent");
  }
  if (state.contains("truncated") && state["truncated"].is_object()) {
    for (auto it = state["truncated"].begin(); it != state["truncated"].end(); ++it)
      if (it.value().is_boolean() && it.value().get<bool>())
        throw std::invalid_argument("cannot import a truncated canonical export");
  }
  Transaction transaction(database_);
  if (replace_existing) {
    const auto key = scope_key(scope);
    Statement assertions(database_, "DELETE FROM memory_assertions WHERE subject_id IN (SELECT id FROM memory_entities WHERE scope_key=?) OR object_id IN (SELECT id FROM memory_entities WHERE scope_key=?);");
    assertions.bind(1, key); assertions.bind(2, key);
    if (assertions.step() != SQLITE_DONE) throw std::runtime_error("unable to clear imported assertions");
    Statement aliases(database_, "DELETE FROM memory_entity_aliases WHERE entity_id IN (SELECT id FROM memory_entities WHERE scope_key=?);");
    aliases.bind(1, key);
    if (aliases.step() != SQLITE_DONE) throw std::runtime_error("unable to clear imported entity aliases");
    for (const char* table : {"retrieval_cross_store_links", "retrieval_owners",
                              "memory_entities", "graph_edges",
                              "procedure_runs", "memory_revisions", "source_events",
                              "sources", "review_actions", "review_items", "audit_events",
                              "session_events", "context_blocks", "review_view_state"}) {
      Statement remove(database_, std::string("DELETE FROM ") + table + " WHERE scope_key=?;");
      remove.bind(1, key);
      if (remove.step() != SQLITE_DONE)
        throw std::runtime_error("unable to clear imported memory state: " + std::string(table));
    }
    Statement fts(database_, "DELETE FROM memory_fts WHERE memory_id IN (SELECT id FROM memories WHERE scope_key=?);");
    fts.bind(1, key); if (fts.step() != SQLITE_DONE) throw std::runtime_error("unable to clear memory index");
    Statement memories(database_, "DELETE FROM memories WHERE scope_key=?;");
    memories.bind(1, key); if (memories.step() != SQLITE_DONE) throw std::runtime_error("unable to clear memories");
  }
  std::unordered_map<std::string, std::string> memory_ids;
  const auto import_memory = [&](const Json& item, MemoryWrite write) {
    const auto original = item.value("id", std::string{});
    const bool safe_id = !original.empty() && original.size() <= 256U &&
        std::all_of(original.begin(), original.end(), [](const unsigned char value) {
          return std::isalnum(value) != 0 || value == '_' || value == '-' || value == '.';
        });
    if (!safe_id || scope_key(scope) != scope_key(declared_scope))
      return add(scope, std::move(write));
    Statement existing(database_, "SELECT 1 FROM memories WHERE id=?;");
    existing.bind(1, original);
    if (existing.step() == SQLITE_ROW) return add(scope, std::move(write));
    const auto now = std::chrono::system_clock::now();
    Statement insert(database_, "INSERT INTO memories(" + memory_columns() + ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    int index = 1;
    insert.bind(index++, original); insert.bind(index++, scope_key(scope));
    insert.bind(index++, std::string(to_string(scope.kind))); insert.bind(index++, scope.user);
    insert.bind(index++, scope.organization); insert.bind(index++, scope.project); insert.bind(index++, scope.repository);
    insert.bind(index++, scope.repository_display); insert.bind(index++, scope.branch); insert.bind(index++, scope.task);
    insert.bind(index++, scope.agent); insert.bind(index++, scope.session); insert.bind(index++, scope.conversation);
    insert.bind(index++, std::string(to_string(write.type))); insert.bind(index++, std::string(to_string(write.status)));
    insert.bind(index++, write.source); insert.bind(index++, write.provenance.source_id);
    insert.bind(index++, std::string(to_string(write.provenance.source_type))); insert.bind(index++, write.provenance.source_uri);
    insert.bind(index++, write.provenance.source_hash); insert.bind(index++, write.provenance.source_span);
    insert.bind(index++, write.provenance.created_by); insert.bind(index++, write.provenance.evidence_strength);
    insert.bind(index++, write.content); insert.bind(index++, write.confidence); insert.bind(index++, write.quality_score);
    insert.bind(index++, static_cast<std::int64_t>(item.value("success_count", std::size_t{0})));
    insert.bind(index++, static_cast<std::int64_t>(item.value("failure_count", std::size_t{0})));
    insert.bind(index++, item.value("last_outcome", std::string("unknown")));
    insert.bind(index++, static_cast<std::int64_t>(item.value("access_count", std::size_t{0})));
    insert.bind(index++, std::string{}); insert.bind(index++, std::string{});
    insert.bind(index++, item.value("created_at", timestamp(now)));
    insert.bind(index++, item.value("updated_at", timestamp(now)));
    insert.bind(index++, item.value("last_accessed_at", timestamp(now)));
    if (insert.step() != SQLITE_DONE) throw std::invalid_argument("unable to preserve imported memory identity");
    for (const auto& tag : write.tags) {
      Statement tag_insert(database_, "INSERT OR IGNORE INTO memory_tags(memory_id,tag) VALUES(?,?);");
      tag_insert.bind(1, original); tag_insert.bind(2, sanitize_text(tag, 256U));
      if (tag_insert.step() != SQLITE_DONE) throw std::runtime_error("unable to restore imported memory tag");
    }
    Statement fts(database_, "INSERT INTO memory_fts(memory_id,content,source,memory_type,source_uri) VALUES(?,?,?,?,?);");
    fts.bind(1, original); fts.bind(2, write.content); fts.bind(3, write.source);
    fts.bind(4, std::string(to_string(write.type))); fts.bind(5, write.provenance.source_uri);
    if (fts.step() != SQLITE_DONE) throw std::runtime_error("unable to index imported memory");
    return original;
  };
  for (const auto& item : state.value("memories", Json::array())) {
    if (state.contains("scope") &&
        (item.value("scope_kind", std::string{}) != to_string(declared_scope.kind) ||
         item.value("user", std::string{}) != declared_scope.user ||
         item.value("organization", std::string{}) != declared_scope.organization ||
         item.value("project", std::string{}) != declared_scope.project ||
         item.value("repository", std::string{}) != declared_scope.repository ||
         item.value("branch", std::string{}) != declared_scope.branch ||
         item.value("task", std::string{}) != declared_scope.task ||
         item.value("agent", std::string{}) != declared_scope.agent ||
         item.value("session", std::string{}) != declared_scope.session ||
         item.value("conversation", std::string{}) != declared_scope.conversation))
      throw std::invalid_argument("imported memory scope does not match export declaration");
    MemoryWrite write;
    write.type = memory_type_from_string(item.value("type", "observation"));
    write.status = memory_status_from_string(item.value("status", "hypothesis"));
    write.source = item.value("source", "import");
    write.provenance.source_id = item.value("source_id", "");
    write.provenance.source_type = source_type_from_string(item.value("source_type", "unknown"));
    write.provenance.source_uri = item.value("source_uri", "");
    write.provenance.source_hash = item.value("source_hash", "");
    write.provenance.source_span = item.value("source_span", "");
    write.provenance.created_by = item.value("created_by", std::string("import"));
    write.provenance.evidence_strength = item.value("evidence_strength", 0.0);
    write.content = item.value("content", "");
    write.tags = item.value("tags", std::vector<std::string>{});
    write.confidence = item.value("confidence", 0.5);
    write.quality_score = item.value("quality_score", 0.0);
    write.source = sanitize_text(std::move(write.source));
    write.content = sanitize_text(std::move(write.content), kMaximumDurableText);
    write.provenance.source_id = sanitize_text(std::move(write.provenance.source_id));
    write.provenance.source_uri = sanitize_text(std::move(write.provenance.source_uri), kMaximumDurableUri);
    write.provenance.source_hash = sanitize_text(std::move(write.provenance.source_hash));
    write.provenance.source_span = sanitize_text(std::move(write.provenance.source_span));
    write.provenance.created_by = sanitize_text(std::move(write.provenance.created_by));
    const auto original_id = item.value("id", std::string{});
    if (original_id.empty() || memory_ids.count(original_id))
      throw std::invalid_argument("imported memory identity is empty or duplicated");
    memory_ids.emplace(original_id, import_memory(item, std::move(write)));
  }
  for (const auto& item : state.value("memories", Json::array())) {
    const auto mapped = memory_ids.find(item.value("id", ""));
    if (mapped == memory_ids.end()) continue;
    const auto replacement = memory_ids.find(item.value("replacement_id", ""));
    const auto superseded = memory_ids.find(item.value("superseded_by", ""));
    Statement update(
        database_,
        "UPDATE memories SET success_count=?,failure_count=?,last_outcome=?,"
        "access_count=?,replacement_id=?,superseded_by=?,created_at=?,updated_at=?,"
        "last_accessed_at=? WHERE id=? AND scope_key=?;");
    update.bind(1, static_cast<std::int64_t>(item.value("success_count", std::size_t{0})));
    update.bind(2, static_cast<std::int64_t>(item.value("failure_count", std::size_t{0})));
    update.bind(3, item.value("last_outcome", std::string("unknown")));
    update.bind(4, static_cast<std::int64_t>(item.value("access_count", std::size_t{0})));
    update.bind(5, replacement == memory_ids.end() ? std::string{} : replacement->second);
    update.bind(6, superseded == memory_ids.end() ? std::string{} : superseded->second);
    update.bind(7, item.value("created_at", std::int64_t{0}));
    update.bind(8, item.value("updated_at", std::int64_t{0}));
    update.bind(9, item.value("last_accessed_at", std::int64_t{0}));
    update.bind(10, mapped->second);
    update.bind(11, scope_key(scope));
    if (update.step() != SQLITE_DONE)
      throw std::runtime_error("unable to restore imported memory metadata: " +
                               std::string(sqlite3_errmsg(database_)));
  }
  // Metadata restoration fires normal invalidation triggers. Reconcile only
  // the newly imported owners afterwards, before restoring immutable history.
  for (const auto& item : state.value("memories", Json::array())) {
    const auto& id = memory_ids.at(item.value("id", std::string{}));
    const auto revision = item.value("retrieval_revision", std::int64_t{1});
    if (revision < 1) throw std::invalid_argument("imported revision must be positive");
    Statement owner(database_, "UPDATE retrieval_owners SET revision=? WHERE owner_key=? AND scope_key=?;");
    owner.bind(1, revision); owner.bind(2, "memory:" + id); owner.bind(3, scope_key(scope));
    if (owner.step() != SQLITE_DONE || sqlite3_changes(database_) != 1)
      throw std::runtime_error("unable to restore imported retrieval revision");
    Statement jobs(database_, "DELETE FROM retrieval_jobs WHERE owner_key=?;");
    jobs.bind(1, "memory:" + id);
    if (jobs.step() != SQLITE_DONE) throw std::runtime_error("unable to clear imported derived jobs");
    Statement schedule(database_, "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) VALUES(?,?,'chunks','text-v1');");
    schedule.bind(1, "memory:" + id); schedule.bind(2, revision);
    if (schedule.step() != SQLITE_DONE) throw std::runtime_error("unable to schedule imported memory indexing");
    Statement clear(database_, "DELETE FROM memory_assertion_revisions WHERE memory_id=?;");
    clear.bind(1, id);
    if (clear.step() != SQLITE_DONE) throw std::runtime_error("unable to reset imported revision boundaries");
    if (!state.contains("assertion_revision_boundaries")) {
      Statement fallback(database_, "INSERT INTO memory_assertion_revisions VALUES(?,?,?);");
      fallback.bind(1, id); fallback.bind(2, revision);
      fallback.bind(3, item.value("updated_at", timestamp(std::chrono::system_clock::now())));
      if (fallback.step() != SQLITE_DONE) throw std::runtime_error("unable to initialize imported revision boundary");
    }
  }
  for (const auto& item : state.value("assertion_revision_boundaries", Json::array())) {
    const auto memory = memory_ids.find(item.value("memory_id", std::string{}));
    const auto revision = item.value("revision", std::int64_t{0});
    if (memory == memory_ids.end() || revision < 1 || revision > retrieval_revision(scope, memory->second))
      throw std::invalid_argument("imported assertion boundary is unresolved or invalid");
    Statement boundary(database_, "INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at) VALUES(?,?,?);");
    boundary.bind(1, memory->second); boundary.bind(2, revision);
    boundary.bind(3, item.value("recorded_at", std::int64_t{0}));
    if (boundary.step() != SQLITE_DONE) throw std::invalid_argument("imported assertion boundary conflicts");
  }
  if (state.contains("assertion_revision_boundaries")) {
    for (const auto& [original, imported] : memory_ids) {
      (void)original;
      Statement current(database_,
          "SELECT 1 FROM memory_assertion_revisions b JOIN retrieval_owners o "
          "ON o.owner_id=b.memory_id AND o.kind='memory' AND o.scope_key=? "
          "WHERE b.memory_id=? AND b.revision=o.revision;");
      current.bind(1, scope_key(scope)); current.bind(2, imported);
      if (current.step() != SQLITE_ROW)
        throw std::invalid_argument("imported current revision boundary is missing");
    }
  }
  std::unordered_map<std::string, std::string> entity_ids;
  for (const auto& item : state.value("entities", Json::array())) {
    const auto original = item.value("id", std::string{});
    if (original.empty() || original.size() > 256U || entity_ids.count(original))
      throw std::invalid_argument("imported entity id is invalid");
    const auto encoded_scope = item.value("scope", Json::object());
    if (!encoded_scope.is_object() ||
        encoded_scope.value("kind", std::string{}) != to_string(declared_scope.kind) ||
        encoded_scope.value("user", std::string{}) != declared_scope.user ||
        encoded_scope.value("organization", std::string{}) != declared_scope.organization ||
        encoded_scope.value("project", std::string{}) != declared_scope.project ||
        encoded_scope.value("repository", std::string{}) != declared_scope.repository ||
        encoded_scope.value("branch", std::string{}) != declared_scope.branch ||
        encoded_scope.value("task", std::string{}) != declared_scope.task ||
        encoded_scope.value("agent", std::string{}) != declared_scope.agent ||
        encoded_scope.value("session", std::string{}) != declared_scope.session ||
        encoded_scope.value("conversation", std::string{}) != declared_scope.conversation)
      throw std::invalid_argument("imported entity scope is invalid");
    const auto kind = sanitize_text(item.value("kind", std::string{}), 128U);
    const auto name = sanitize_text(item.value("name", std::string{}), 1024U);
    if (kind.empty() || name.empty()) throw std::invalid_argument("imported entity is incomplete");
    auto imported_id = original;
    Statement collision(database_, "SELECT 1 FROM memory_entities WHERE id=?;");
    collision.bind(1, imported_id);
    if (collision.step() == SQLITE_ROW) imported_id = make_id("entity_");
    Json target_scope_json{{"kind", to_string(scope.kind)}, {"user", scope.user},
        {"organization", scope.organization}, {"project", scope.project},
        {"repository", scope.repository}, {"repository_display", scope.repository_display},
        {"branch", scope.branch}, {"task", scope.task}, {"agent", scope.agent},
        {"session", scope.session}, {"conversation", scope.conversation}};
    Statement insert(database_, "INSERT INTO memory_entities(id,scope_key,scope_json,kind,name) VALUES(?,?,?,?,?);");
    insert.bind(1, imported_id); insert.bind(2, scope_key(scope)); insert.bind(3, target_scope_json.dump());
    insert.bind(4, kind); insert.bind(5, name);
    if (insert.step() != SQLITE_DONE) throw std::invalid_argument("imported entity id conflicts");
    entity_ids[original] = imported_id;
    const auto aliases = item.value("aliases", Json::array());
    if (!aliases.is_array() || aliases.size() > 128U) throw std::invalid_argument("imported entity aliases are invalid");
    for (const auto& alias : aliases) {
      const auto value = sanitize_text(alias.get<std::string>(), 1024U);
      if (value.empty()) continue;
      Statement add_alias(database_, "INSERT OR IGNORE INTO memory_entity_aliases VALUES(?,?);");
      add_alias.bind(1, imported_id); add_alias.bind(2, value);
      if (add_alias.step() != SQLITE_DONE) throw std::runtime_error("unable to restore entity alias");
    }
  }
  std::unordered_map<std::string, std::int64_t> source_current_revisions;
  for (const auto& item : state.value("memories", Json::array()))
    source_current_revisions[item.value("id", std::string{})] =
        item.value("retrieval_revision", std::int64_t{1});
  for (const auto& item : state.value("assertions", Json::array())) {
    const auto memory = memory_ids.find(item.value("memory_id", std::string{}));
    const auto subject = entity_ids.find(item.value("subject_id", std::string{}));
    const auto object_id = item.value("object_id", std::string{});
    const auto object = object_id.empty() ? entity_ids.end() : entity_ids.find(object_id);
    if (memory == memory_ids.end() || subject == entity_ids.end() ||
        (!object_id.empty() && object == entity_ids.end()))
      throw std::invalid_argument("imported assertion has an unresolved endpoint");
    const auto literal = Json::parse(item.value("literal_json", std::string("null")), nullptr, false);
    const auto qualifiers = Json::parse(item.value("qualifiers_json", std::string("{}")), nullptr, false);
    const auto evidence = item.value("evidence", Json::array());
    if (literal.is_discarded() || qualifiers.is_discarded() || !qualifiers.is_object() ||
        !evidence.is_array() || evidence.empty() || item.value("predicate", std::string{}).empty())
      throw std::invalid_argument("imported assertion evidence or JSON is invalid");
    Statement insert(database_, "INSERT INTO memory_assertions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);");
    const auto source_revision = item.value("revision", std::int64_t{0});
    const auto source_current = source_current_revisions[item.value("memory_id", std::string{})];
    if (source_revision < 1 || source_revision > source_current)
      throw std::invalid_argument("imported assertion revision is invalid");
    const auto target_revision = source_revision;
    insert.bind(1, memory->second); insert.bind(2, target_revision);
    insert.bind(3, subject->second); insert.bind(4, sanitize_text(item.value("predicate", std::string{}), 128U));
    insert.bind(5, object_id.empty() ? std::string{} : object->second);
    insert.bind(6, sanitize_json(literal.dump())); insert.bind(7, sanitize_json(qualifiers.dump()));
    insert.bind(8, static_cast<std::int64_t>(item.value("positive", true)));
    if (item.contains("valid_from")) insert.bind(9, item.value("valid_from", std::int64_t{0}));
    else if (sqlite3_bind_null(insert.get(), 9) != SQLITE_OK) throw std::runtime_error("unable to bind assertion start");
    if (item.contains("valid_to")) insert.bind(10, item.value("valid_to", std::int64_t{0}));
    else if (sqlite3_bind_null(insert.get(), 10) != SQLITE_OK) throw std::runtime_error("unable to bind assertion end");
    insert.bind(11, sanitize_json(evidence.dump()));
    insert.bind(12, item.value("recorded_at", static_cast<std::int64_t>(0)));
    insert.bind(13, sanitize_json(item.value("historical_json", std::string("{}"))));
    if (insert.step() != SQLITE_DONE) throw std::invalid_argument("imported assertion conflicts or is invalid");
  }
  const auto import_endpoint = [&](const Json& value) {
    RetrievalReference endpoint;
    const auto kind = value.value("kind", std::string{});
    if (kind == "memory") endpoint.kind = RetrievalOwnerKind::Memory;
    else if (kind == "code") endpoint.kind = RetrievalOwnerKind::Code;
    else if (kind == "entity") endpoint.kind = RetrievalOwnerKind::Entity;
    else throw std::invalid_argument("imported link endpoint kind is invalid");
    endpoint.id = value.value("id", std::string{});
    if (endpoint.kind == RetrievalOwnerKind::Memory) {
      const auto found = memory_ids.find(endpoint.id);
      if (found == memory_ids.end()) throw std::invalid_argument("imported link memory endpoint is unresolved");
      endpoint.id = found->second;
    } else if (endpoint.kind == RetrievalOwnerKind::Entity) {
      const auto found = entity_ids.find(endpoint.id);
      if (found == entity_ids.end()) throw std::invalid_argument("imported link entity endpoint is unresolved");
      endpoint.id = found->second;
    }
    endpoint.revision = value.value("revision", std::int64_t{0});
    endpoint.repository = value.value("repository", std::string{});
    endpoint.worktree = value.value("worktree", std::string{});
    endpoint.path = value.value("path", std::string{});
    endpoint.source_fingerprint = value.value("source_fingerprint", std::string{});
    endpoint.snapshot_generation = value.value("snapshot_generation", std::uint64_t{0});
    return endpoint;
  };
  for (const auto& item : state.value("cross_store_links", Json::array())) {
    CrossStoreLink link;
    link.id = item.value("id", std::string{});
    link.from = import_endpoint(item.value("from", Json::object()));
    link.to = import_endpoint(item.value("to", Json::object()));
    link.relation = item.value("relation", std::string{});
    link.evidence_id = item.value("evidence_id", std::string{});
    if (link.id.empty() || link.relation.empty() || link.evidence_id.rfind("uri:", 0) != 0)
      throw std::invalid_argument("imported cross-store link evidence is invalid");
    const auto validate_endpoint = [&](RetrievalReference& endpoint) {
      if (endpoint.kind == RetrievalOwnerKind::Memory) {
        const auto memory = get(scope, endpoint.id);
        if (scope_key(memory.scope) != scope_key(scope))
          throw std::invalid_argument("imported memory link endpoint is outside target scope");
        if (endpoint.revision < 0 || endpoint.revision > retrieval_revision(scope, endpoint.id))
          throw std::invalid_argument("imported memory link revision is invalid");
      } else if (endpoint.kind == RetrievalOwnerKind::Entity) {
        Statement entity(database_, "SELECT 1 FROM memory_entities WHERE id=? AND scope_key=?;");
        entity.bind(1, endpoint.id); entity.bind(2, scope_key(scope));
        if (entity.step() != SQLITE_ROW) throw std::invalid_argument("imported entity link endpoint is unresolved");
      } else if (endpoint.repository.empty() || endpoint.worktree.empty() ||
                 endpoint.source_fingerprint.empty() || endpoint.snapshot_generation == 0) {
        throw std::invalid_argument("imported code link endpoint is incomplete");
      }
    };
    validate_endpoint(link.from); validate_endpoint(link.to);
    Statement collision(database_, "SELECT 1 FROM retrieval_cross_store_links WHERE id=?;");
    collision.bind(1, link.id);
    if (collision.step() == SQLITE_ROW) link.id = make_id("link_");
    const auto encode_kind = [](RetrievalOwnerKind kind) {
      if (kind == RetrievalOwnerKind::Memory) return std::string("memory");
      if (kind == RetrievalOwnerKind::Code) return std::string("code");
      return std::string("entity");
    };
    Statement insert(database_,
        "INSERT INTO retrieval_cross_store_links VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    int index = 1;
    insert.bind(index++, link.id); insert.bind(index++, scope_key(scope));
    insert.bind(index++, Json{{"kind", to_string(scope.kind)}, {"user", scope.user},
                              {"organization", scope.organization}, {"project", scope.project},
                              {"repository", scope.repository}, {"repository_display", scope.repository_display},
                              {"branch", scope.branch}, {"task", scope.task}, {"agent", scope.agent},
                              {"session", scope.session}, {"conversation", scope.conversation}}.dump());
    const auto bind_endpoint = [&](const RetrievalReference& endpoint) {
      insert.bind(index++, encode_kind(endpoint.kind)); insert.bind(index++, endpoint.id);
      insert.bind(index++, endpoint.revision); insert.bind(index++, endpoint.repository);
      insert.bind(index++, endpoint.worktree); insert.bind(index++, endpoint.path);
      insert.bind(index++, endpoint.source_fingerprint);
      insert.bind(index++, static_cast<std::int64_t>(endpoint.snapshot_generation));
    };
    bind_endpoint(link.from); bind_endpoint(link.to);
    insert.bind(index++, link.relation); insert.bind(index++, link.evidence_id);
    insert.bind(index, item.value("created_at", static_cast<std::int64_t>(0)));
    if (insert.step() != SQLITE_DONE) throw std::invalid_argument("imported cross-store link conflicts");
  }
  for (const auto& item : state.value("blocks", Json::array())) {
    update_block(scope, item.value("id", ""), item.value("content", ""));
  }
  std::unordered_map<std::string, std::string> graph_ids;
  for (const auto& item : state.value("graph_edges", Json::array())) {
    const auto from = memory_ids.find(item.value("from_id", ""));
    const auto to = memory_ids.find(item.value("to_id", ""));
    if (from == memory_ids.end() || to == memory_ids.end()) continue;
    GraphEdge edge;
    edge.from_id = from->second;
    edge.to_id = to->second;
    edge.type = item.value("type", "related");
    edge.confidence = item.value("confidence", 1.0);
    edge.source = item.value("source", "import");
    const auto edge_id = add_edge(scope, std::move(edge));
    graph_ids[item.value("id", edge_id)] = edge_id;
  }
  for (const auto& item : state.value("memory_revisions", Json::array())) {
    const auto memory = memory_ids.find(item.value("memory_id", ""));
    if (memory == memory_ids.end()) continue;
    Statement insert(
        database_,
        "INSERT INTO memory_revisions(id,scope_key,memory_id,revision_kind,"
        "entry_json,created_at) VALUES(?,?,?,?,?,?);");
    insert.bind(1, make_id("rev_"));
    insert.bind(2, scope_key(scope));
    insert.bind(3, memory->second);
    insert.bind(4, sanitize_text(item.value("kind", std::string("import")), 256U));
    insert.bind(5, sanitize_json(item.value("entry_json", std::string("{}"))));
    insert.bind(6, item.value("created_at", std::int64_t{0}));
    if (insert.step() != SQLITE_DONE)
      throw std::runtime_error("unable to restore imported memory revision");
  }
  for (const auto& item : state.value("procedure_runs", Json::array())) {
    const auto procedure = memory_ids.find(item.value("procedure_id", ""));
    if (procedure == memory_ids.end()) continue;
    Statement insert(
        database_,
        "INSERT INTO procedure_runs(id,scope_key,procedure_id,task_id,outcome,"
        "notes,evidence_uri,created_at,deduplication_key) "
        "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT DO NOTHING;");
    insert.bind(1, make_id("run_"));
    insert.bind(2, scope_key(scope));
    insert.bind(3, procedure->second);
    insert.bind(4, sanitize_text(item.value("task_id", std::string{})));
    insert.bind(5, sanitize_text(item.value("outcome", std::string("unknown")), 256U));
    insert.bind(6, sanitize_text(item.value("notes", std::string{}),
                                 kMaximumDurableMetadata));
    insert.bind(7, sanitize_text(item.value("evidence_uri", std::string{}),
                                 kMaximumDurableUri));
    insert.bind(8, item.value("created_at", std::int64_t{0}));
    insert.bind(9, sanitize_text(
                       item.value("deduplication_key", std::string{}), 256U));
    if (insert.step() != SQLITE_DONE)
      throw std::runtime_error("unable to restore imported procedure run");
  }
  const auto source_values = state.value("sources", Json::array());
  const auto source_event_values = state.value("source_events", Json::array());
  for (const auto& item : source_values) {
    SourceRecord source;
    source.id = item.value("id", std::string{});
    source.type = source_type_from_string(item.value("type", std::string("unknown")));
    source.uri = item.value("uri", std::string{});
    source.hash = item.value("hash", std::string{});
    source.display_name = item.value("display_name", std::string{});
    source.trust_level = item.value("trust_level", std::string("working"));
    source.hash_status = item.value("hash_status", std::string("unknown"));
    bool found_event = false;
    for (const auto& event_value : source_event_values) {
      if (event_value.value("source_id", std::string{}) != source.id) continue;
      SourceEvent event;
      event.action = event_value.value("action", std::string("import"));
      event.status = event_value.value("status", std::string("ok"));
      event.source_hash = event_value.value("source_hash", std::string{});
      event.note = event_value.value("note", std::string{});
      record_event(scope, source, std::move(event));
      found_event = true;
    }
    if (!found_event) {
      SourceEvent event;
      event.action = "import";
      event.status = "ok";
      event.source_hash = source.hash;
      event.note = "source restored from native memory export";
      record_event(scope, std::move(source), std::move(event));
    }
  }
  const bool has_review_actions = state.contains("review_actions") &&
                                  state["review_actions"].is_array();
  std::unordered_map<std::string, std::string> review_ids;
  for (const auto& item : state.value("review_items", Json::array())) {
    ReviewItemWrite write;
    write.kind = item.value("kind", std::string("candidate_memory"));
    write.priority = item.value("priority", std::string("normal"));
    write.reason = item.value("reason", std::string("imported review"));
    const auto subject = memory_ids.find(item.value("subject_id", ""));
    write.subject_id = subject == memory_ids.end()
                           ? item.value("subject_id", std::string{})
                           : subject->second;
    write.provenance.source_id = item.value("source_id", std::string{});
    write.provenance.source_type = source_type_from_string(
        item.value("source_type", std::string("unknown")));
    write.provenance.source_uri = item.value("source_uri", std::string{});
    write.provenance.source_hash = item.value("source_hash", std::string{});
    write.provenance.source_span = item.value("source_span", std::string{});
    write.provenance.created_by = item.value("created_by", std::string("import"));
    write.provenance.evidence_strength = item.value("evidence_strength", 0.0);
    write.candidate_json = remapped_json(
        item.value("candidate_json", std::string("{}")), {&memory_ids});
    write.candidate_version = item.value("candidate_version", std::uint32_t{1});
    write.tags = item.value("tags", std::vector<std::string>{});
    const auto review_id = add_review(scope, std::move(write));
    review_ids[item.value("id", review_id)] = review_id;
    const auto imported_status = review_status_from_string(
        item.value("status", std::string("pending")));
    if (has_review_actions) {
      Statement restore(database_,
          "UPDATE review_items SET status=?,decision=?,decided_by=?,decision_note=?,"
          "created_at=?,updated_at=?,decided_at=?,revision=?,deferred_until=?,"
          "application_status=?,application_error=?,action_key=? WHERE scope_key=? AND id=?;");
      int index = 1;
      restore.bind(index++, std::string(to_string(imported_status)));
      restore.bind(index++, sanitize_text(item.value("decision", std::string{})));
      restore.bind(index++, sanitize_text(item.value("decided_by", std::string{})));
      restore.bind(index++, sanitize_text(item.value("decision_note", std::string{}),
                                          kMaximumDurableMetadata));
      restore.bind(index++, item.value("created_at", std::int64_t{0}));
      restore.bind(index++, item.value("updated_at", std::int64_t{0}));
      restore.bind(index++, item.value("decided_at", std::int64_t{0}));
      restore.bind(index++, item.value("revision", std::int64_t{0}));
      restore.bind(index++, item.value("deferred_until", std::int64_t{0}));
      restore.bind(index++, sanitize_text(
                                item.value("application_status",
                                           std::string("not_started")), 64U));
      restore.bind(index++, sanitize_text(
                                item.value("application_error", std::string{}),
                                kMaximumDurableMetadata));
      restore.bind(index++, sanitize_text(item.value("action_key", std::string{})));
      restore.bind(index++, scope_key(scope));
      restore.bind(index++, review_id);
      if (restore.step() != SQLITE_DONE || sqlite3_changes(database_) != 1)
        throw std::runtime_error("unable to restore imported review state");
    } else if (imported_status != ReviewStatus::Pending) {
      ReviewDecision decision;
      decision.status = imported_status;
      decision.decision = item.value("decision", to_string(imported_status));
      decision.decided_by = item.value("decided_by", std::string("import"));
      if (decision.decided_by.empty()) decision.decided_by = "import";
      decision.note = item.value("decision_note", std::string{});
      const auto deferred_until = item.value("deferred_until", std::int64_t{0});
      if (deferred_until > 0)
        decision.deferred_until = TimePoint(std::chrono::milliseconds(deferred_until));
      decision.action_key = item.value("action_key", std::string{});
      decision.application_status =
          item.value("application_status", std::string("applied"));
      decision.application_error = item.value("application_error", std::string{});
      (void)decide_review(scope, review_id, std::move(decision));
    }
  }
  std::unordered_map<std::string, std::string> action_ids;
  for (const auto& item : state.value("review_actions", Json::array())) {
    const auto mapped = review_ids.find(item.value("review_id", std::string{}));
    if (mapped == review_ids.end()) continue;
    Statement insert(database_,
        "INSERT INTO review_actions(id,scope_key,review_id,action_key,action,actor,note,"
        "before_status,after_status,before_revision,after_revision,application_status,"
        "application_error,result_json,replay_count,created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    int index = 1;
    const auto action_id = make_id("review_action_");
    action_ids[item.value("id", action_id)] = action_id;
    insert.bind(index++, action_id);
    insert.bind(index++, scope_key(scope));
    insert.bind(index++, mapped->second);
    insert.bind(index++, sanitize_text(item.value("action_key", std::string{})));
    insert.bind(index++, sanitize_text(item.value("action", std::string{}), 256U));
    insert.bind(index++, sanitize_text(item.value("actor", std::string("import"))));
    insert.bind(index++, sanitize_text(item.value("note", std::string{}),
                                       kMaximumDurableMetadata));
    insert.bind(index++, sanitize_text(
                              item.value("before_status", std::string("pending")),
                              64U));
    insert.bind(index++, sanitize_text(
                              item.value("after_status", std::string("pending")),
                              64U));
    insert.bind(index++, item.value("before_revision", std::int64_t{0}));
    insert.bind(index++, item.value("after_revision", std::int64_t{0}));
    insert.bind(index++, sanitize_text(
                              item.value("application_status",
                                         std::string("not_started")), 64U));
    insert.bind(index++, sanitize_text(
                              item.value("application_error", std::string{}),
                              kMaximumDurableMetadata));
    insert.bind(index++, remapped_json(
                              item.value("result_json", std::string("{}")),
                              {&memory_ids, &graph_ids, &review_ids}));
    insert.bind(index++, item.value("replay_count", std::int64_t{0}));
    insert.bind(index++, item.value("created_at", std::int64_t{0}));
    if (insert.step() != SQLITE_DONE)
      throw std::runtime_error("unable to restore imported review action");
  }
  if (state.contains("review_view_state") &&
      state["review_view_state"].is_object() &&
      state["review_view_state"].contains("state") &&
      state["review_view_state"]["state"].is_object()) {
    Statement restore(database_,
        "INSERT INTO review_view_state(scope_key,state_json,updated_at) VALUES(?,?,?) "
        "ON CONFLICT(scope_key) DO UPDATE SET state_json=excluded.state_json,"
        "updated_at=excluded.updated_at;");
    restore.bind(1, scope_key(scope));
    auto imported_view_state = state["review_view_state"]["state"];
    remap_json_ids(imported_view_state,
                   {&memory_ids, &graph_ids, &review_ids});
    restore.bind(2, sanitize_json(imported_view_state.dump()));
    restore.bind(3, state["review_view_state"].value(
                            "updated_at", std::int64_t{0}));
    if (restore.step() != SQLITE_DONE)
      throw std::runtime_error("unable to restore review view state");
  }
  for (const auto& item : state.value("audit_events", Json::array())) {
    AuditEventWrite write;
    write.actor = item.value("actor", std::string("import"));
    write.action = item.value("action", std::string("import"));
    write.reason = item.value("reason", std::string("imported audit event"));
    const auto remap_id = [&](const std::string& original) {
      for (const auto* ids : {&memory_ids, &graph_ids, &review_ids,
                              &action_ids}) {
        const auto found = ids->find(original);
        if (found != ids->end()) return found->second;
      }
      return original;
    };
    write.before_id = remap_id(item.value("before_id", std::string{}));
    write.after_id = remap_id(item.value("after_id", std::string{}));
    write.evidence_uri = item.value("evidence_uri", std::string{});
    write.metadata_json = remapped_json(
        item.value("metadata_json", std::string("{}")),
        {&memory_ids, &graph_ids, &review_ids, &action_ids});
    (void)record_audit(scope, std::move(write));
  }
  for (const auto& item : state.value("session_events", Json::array())) {
    SessionEventWrite write;
    write.event_type = item.value("event_type", std::string("import"));
    write.source = item.value("source", std::string("import"));
    write.payload_json = item.value("payload_json", std::string("{}"));
    write.deduplication_key =
        item.value("deduplication_key", std::string{});
    if (write.deduplication_key.empty())
      (void)record_session_event(scope, std::move(write));
    else
      (void)record_session_event_once(scope, std::move(write));
  }
  transaction.commit();
}

std::pair<std::string, bool> SQLiteMemoryDatabase::add_capture_candidate_once(
    const MemoryScope& scope, MemoryWrite write, SessionEventWrite claim) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (claim.deduplication_key.empty() || claim.event_type != "candidate_ingestion_claim")
    throw std::invalid_argument("capture candidate requires a deduplicated ingestion claim");
  write.content = MemoryService::normalize_content(sanitize_text(std::move(write.content), kMaximumDurableText));
  if (write.content.empty()) throw std::invalid_argument("capture candidate must not be empty");
  const auto canonicalize = [](const std::string& value) {
    auto canonical = MemoryService::normalize_content(value);
    for (char& c : canonical) {
      const auto byte = static_cast<unsigned char>(c);
      if (byte < 128) c = static_cast<char>(std::tolower(byte));
    }
    return canonical;
  };
  const auto canonical = canonicalize(write.content);
  Transaction transaction(database_, true);
  Statement prior(database_, "SELECT memory_id,canonical_content FROM capture_candidate_receipts WHERE scope_key=? AND deduplication_key=?;");
  prior.bind(1, scope_key(scope)); prior.bind(2, claim.deduplication_key);
  const int prior_rc = prior.step();
  if (prior_rc == SQLITE_ROW) {
    if (text(prior.get(), 1) != canonical)
      throw std::invalid_argument("capture receipt key reused for different content");
    const auto id = text(prior.get(), 0);
    transaction.commit();
    return {id, false};
  }
  if (prior_rc != SQLITE_DONE) throw std::runtime_error("unable to read capture receipt");
  std::string id;
  Statement existing(database_, "SELECT id,content FROM memories WHERE scope_key=? ORDER BY id;");
  existing.bind(1, scope_key(scope));
  int rc;
  while ((rc = existing.step()) == SQLITE_ROW) {
    if (canonicalize(text(existing.get(), 1)) == canonical) {
      id = text(existing.get(), 0);
      break;
    }
  }
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) throw std::runtime_error("unable to inspect capture duplicates");
  const bool inserted = id.empty();
  if (inserted) {
    write.status = MemoryStatus::Candidate;
    MemoryService service(*this);
    id = service.remember(scope, std::move(write));
  }
  Statement receipt(database_, "INSERT INTO capture_candidate_receipts VALUES(?,?,?,?);");
  receipt.bind(1, scope_key(scope)); receipt.bind(2, claim.deduplication_key);
  receipt.bind(3, canonical); receipt.bind(4, id);
  if (receipt.step() != SQLITE_DONE) throw std::runtime_error("unable to publish capture receipt");
  (void)record_session_event_once(scope, std::move(claim));
  transaction.commit();
  return {id, inserted};
}

MemoryEntity SQLiteMemoryDatabase::add_entity(const MemoryScope& scope,
                                             MemoryEntity entity) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  entity.name = sanitize_text(std::move(entity.name), 1024);
  entity.kind = sanitize_text(std::move(entity.kind), 128);
  if (entity.name.empty() || entity.kind.empty() || entity.aliases.size() > 128)
    throw std::invalid_argument("entity requires a name, kind and bounded aliases");
  if (!entity.id.empty())
    throw std::invalid_argument("new entity identity is assigned by storage");
  entity.id = make_id("entity_");
  entity.scope = scope;
  Json encoded{{"kind", to_string(scope.kind)}, {"user", scope.user},
      {"organization", scope.organization}, {"project", scope.project},
      {"repository", scope.repository}, {"repository_display", scope.repository_display},
      {"branch", scope.branch}, {"task", scope.task}, {"agent", scope.agent},
      {"session", scope.session}, {"conversation", scope.conversation}};
  Transaction transaction(database_, true);
  Statement insert(database_, "INSERT INTO memory_entities VALUES(?,?,?,?,?);");
  insert.bind(1, entity.id); insert.bind(2, scope_key(scope));
  insert.bind(3, encoded.dump()); insert.bind(4, entity.kind); insert.bind(5, entity.name);
  if (insert.step() != SQLITE_DONE) throw std::runtime_error("unable to create entity");
  entity.aliases.push_back(entity.name);
  for (auto& alias : entity.aliases) alias = sanitize_text(std::move(alias), 1024);
  entity.aliases.erase(std::remove(entity.aliases.begin(), entity.aliases.end(), ""), entity.aliases.end());
  std::sort(entity.aliases.begin(), entity.aliases.end());
  entity.aliases.erase(std::unique(entity.aliases.begin(), entity.aliases.end()), entity.aliases.end());
  for (const auto& alias : entity.aliases) {
    Statement add_alias(database_, "INSERT OR IGNORE INTO memory_entity_aliases VALUES(?,?);");
    add_alias.bind(1, entity.id); add_alias.bind(2, alias);
    if (add_alias.step() != SQLITE_DONE) throw std::runtime_error("unable to create entity alias");
  }
  transaction.commit();
  return entity;
}

std::vector<MemoryEntity> SQLiteMemoryDatabase::find_entities(
    const MemoryScope& scope, const std::string& name, RecallPolicy policy,
    bool include_global_user, std::size_t limit) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, policy, include_global_user);
  Statement query(database_, "SELECT e.id,e.kind,e.name,e.scope_json FROM memory_entities e "
      "WHERE e.scope_key IN (" + placeholders(keys.size()) + ") AND "
      "(e.id=? OR EXISTS(SELECT 1 FROM memory_entity_aliases a WHERE a.entity_id=e.id AND a.alias=?)) "
      "ORDER BY e.id LIMIT ?;");
  int i = 1; bind_keys(query, i, keys); query.bind(i++, name); query.bind(i++, name);
  query.bind(i, static_cast<std::int64_t>(std::min<std::size_t>(limit, 1000)));
  std::vector<MemoryEntity> result;
  int rc;
  while ((rc = query.step()) == SQLITE_ROW) {
    MemoryEntity entity;
    entity.id = text(query.get(), 0); entity.kind = text(query.get(), 1); entity.name = text(query.get(), 2);
    const auto s = Json::parse(text(query.get(), 3));
    entity.scope.kind = scope_kind_from_string(s.at("kind").get<std::string>());
    entity.scope.user = s.value("user", ""); entity.scope.organization = s.value("organization", "");
    entity.scope.project = s.value("project", ""); entity.scope.repository = s.value("repository", "");
    entity.scope.repository_display = s.value("repository_display", ""); entity.scope.branch = s.value("branch", "");
    entity.scope.task = s.value("task", ""); entity.scope.agent = s.value("agent", "");
    entity.scope.session = s.value("session", ""); entity.scope.conversation = s.value("conversation", "");
    Statement aliases(database_, "SELECT alias FROM memory_entity_aliases WHERE entity_id=? ORDER BY alias;");
    aliases.bind(1, entity.id);
    int alias_rc;
    while ((alias_rc = aliases.step()) == SQLITE_ROW) entity.aliases.push_back(text(aliases.get(), 0));
    if (alias_rc != SQLITE_DONE) throw std::runtime_error("unable to read entity aliases");
    result.push_back(std::move(entity));
  }
  if (rc != SQLITE_DONE) throw std::runtime_error("unable to resolve entity");
  return result;
}

std::int64_t SQLiteMemoryDatabase::retrieval_revision(
    const MemoryScope& scope, const std::string& memory_id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto memory = get(scope, memory_id);
  Statement query(database_, "SELECT revision FROM retrieval_owners WHERE owner_key=? AND scope_key=? AND deleted=0;");
  query.bind(1, "memory:" + memory_id); query.bind(2, scope_key(memory.scope));
  if (query.step() != SQLITE_ROW) throw std::invalid_argument("missing retrieval owner");
  return sqlite3_column_int64(query.get(), 0);
}

void SQLiteMemoryDatabase::add_assertion(const MemoryScope& scope,
                                        MemoryAssertion assertion) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Transaction transaction(database_, true);
  const auto memory = get(scope, assertion.memory_id);
  if (scope_key(memory.scope) != scope_key(scope))
    throw std::invalid_argument("assertion writes require the memory's exact scope");
  if (assertion.revision != retrieval_revision(scope, assertion.memory_id))
    throw std::invalid_argument("assertion revision is stale");
  if (assertion.predicate.empty() || assertion.predicate.size() > 128 ||
      assertion.evidence.empty() || assertion.evidence.size() > 128 ||
      (assertion.valid_from && assertion.valid_to && *assertion.valid_from >= *assertion.valid_to))
    throw std::invalid_argument("assertion requires predicate, evidence and a valid time interval");
  const auto require_entity = [&](const std::string& id) {
    if (id.empty() || find_entities(scope, id, RecallPolicy::Broad, false, 1).empty())
      throw std::invalid_argument("assertion endpoint is not a visible entity");
  };
  require_entity(assertion.subject_id);
  if (!assertion.object_id.empty()) require_entity(assertion.object_id);
  auto literal = Json::parse(assertion.literal_json, nullptr, false);
  auto qualifiers = Json::parse(assertion.qualifiers_json, nullptr, false);
  if (literal.is_discarded() || qualifiers.is_discarded() || !qualifiers.is_object() ||
      (assertion.object_id.empty() && literal.is_null()) ||
      (!assertion.object_id.empty() && !literal.is_null()) ||
      assertion.literal_json.size() > kMaximumDurableMetadata ||
      assertion.qualifiers_json.size() > kMaximumDurableMetadata)
    throw std::invalid_argument("assertion requires one object and bounded JSON qualifiers");
  Json evidence = Json::array();
  for (const auto& e : assertion.evidence) {
    if ((e.source_id.empty() && e.source_uri.empty()) || e.source_hash.empty())
      throw std::invalid_argument("assertion evidence requires a locator and fingerprint");
    evidence.push_back({{"source_id", e.source_id}, {"source_revision", e.source_revision},
        {"source_uri", e.source_uri}, {"source_hash", e.source_hash},
        {"source_span", e.source_span}, {"created_by", e.created_by}});
  }
  if (evidence.dump().size() > kMaximumDurableJson)
    throw std::invalid_argument("assertion evidence exceeds storage limit");
  const Json historical{{"content", memory.content},
                        {"source", memory.source},
                        {"type", to_string(memory.type)},
                        {"status", to_string(memory.status)},
                        {"source_id", memory.provenance.source_id},
                        {"source_type", to_string(memory.provenance.source_type)},
                        {"source_uri", memory.provenance.source_uri},
                        {"source_hash", memory.provenance.source_hash},
                        {"source_span", memory.provenance.source_span},
                        {"created_by", memory.provenance.created_by},
                        {"evidence_strength", memory.provenance.evidence_strength},
                        {"created_at", timestamp(memory.created_at)},
                        {"updated_at", timestamp(memory.updated_at)}};
  const auto recorded_at = std::chrono::system_clock::now();
  Statement insert(database_, "INSERT INTO memory_assertions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);");
  insert.bind(1, assertion.memory_id); insert.bind(2, assertion.revision);
  insert.bind(3, assertion.subject_id); insert.bind(4, sanitize_text(assertion.predicate, 128));
  insert.bind(5, assertion.object_id); insert.bind(6, sanitize_json(literal.dump()));
  insert.bind(7, sanitize_json(qualifiers.dump()));
  insert.bind(8, static_cast<std::int64_t>(assertion.positive));
  if (assertion.valid_from) insert.bind(9, timestamp(*assertion.valid_from));
  if (assertion.valid_to) insert.bind(10, timestamp(*assertion.valid_to));
  insert.bind(11, sanitize_json(evidence.dump()));
  insert.bind(12, timestamp(recorded_at));
  insert.bind(13, sanitize_json(historical.dump()));
  if (insert.step() != SQLITE_DONE)
    throw std::invalid_argument("assertion revision already exists or is invalid");
  transaction.commit();
}

std::optional<MemoryAssertion> SQLiteMemoryDatabase::assertion(
    const MemoryScope& scope, const std::string& memory_id,
    std::optional<TimePoint> as_of) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto memory = get(scope, memory_id);
  if (memory.status != MemoryStatus::Active &&
      memory.status != MemoryStatus::Validated)
    return std::nullopt;
  const auto at = as_of.value_or(std::chrono::system_clock::now());
  Statement query(database_,
      "SELECT memory_id,revision,subject_id,predicate,object_id,literal_json,"
      "qualifiers_json,positive,valid_from,valid_to,evidence_json,recorded_at,historical_json "
      "FROM memory_assertions WHERE memory_id=? AND recorded_at<=? "
      "AND (valid_from IS NULL OR valid_from<=?) AND (valid_to IS NULL OR valid_to>?) "
      + std::string(as_of
                        ? "AND revision=(SELECT MAX(revision) FROM memory_assertion_revisions "
                          "WHERE memory_id=? AND recorded_at<=?) "
                        : "AND revision=? ") +
      "ORDER BY revision DESC LIMIT 1;");
  query.bind(1, memory_id);
  query.bind(2, timestamp(at));
  query.bind(3, timestamp(at));
  query.bind(4, timestamp(at));
  if (!as_of) {
    query.bind(5, retrieval_revision(scope, memory_id));
  } else {
    query.bind(5, memory_id);
    query.bind(6, timestamp(at));
  }
  const int rc = query.step();
  if (rc == SQLITE_DONE) return std::nullopt;
  if (rc != SQLITE_ROW) throw std::runtime_error("unable to read assertion");
  return read_assertion(query.get(), 0);
}

std::optional<RetrievalIndexJob> SQLiteMemoryDatabase::claim_index_job(
    const MemoryScope& scope, std::chrono::seconds lease,
    std::optional<std::string> dense_generation) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (lease.count() < 1 || lease.count() > 300)
    throw std::invalid_argument("index lease must be between one and 300 seconds");
  const auto now = std::chrono::system_clock::now();
  Transaction transaction(database_, true);
  const std::string generation_filter = dense_generation
      ? "AND (representation='chunks' OR (representation='dense' AND generation=?)) "
      : "";
  // A crash cannot call fail_index_job().  Retire only jobs that this claimant
  // could otherwise recover: same exact scope/current revision, an eligible
  // pending retry or expired lease, and the requested generation set.
  Statement exhaust(database_, "UPDATE retrieval_jobs SET state='failed',lease_token='',lease_until=0,"
      "next_attempt=0,error=CASE WHEN state='leased' THEN 'retry limit reached after lease expiry' "
      "ELSE 'retry limit reached before retry' END WHERE attempts>=max_attempts "
      "AND ((state='pending' AND next_attempt<=?) OR (state='leased' AND lease_until<=?)) "
      "AND EXISTS(SELECT 1 FROM retrieval_owners o WHERE o.owner_key=retrieval_jobs.owner_key "
      "AND o.scope_key=? AND o.deleted=0 AND o.revision=retrieval_jobs.owner_revision) " +
      generation_filter);
  int exhaust_parameter = 1;
  exhaust.bind(exhaust_parameter++, timestamp(now));
  exhaust.bind(exhaust_parameter++, timestamp(now));
  exhaust.bind(exhaust_parameter++, scope_key(scope));
  if (dense_generation) exhaust.bind(exhaust_parameter++, *dense_generation);
  if (exhaust.step() != SQLITE_DONE) throw std::runtime_error("unable to exhaust index retries");
  Statement query(database_, std::string("SELECT j.id,j.owner_key,j.owner_revision,j.representation,j.generation,") +
      "j.attempts,o.content,o.source_uri FROM retrieval_jobs j JOIN retrieval_owners o ON o.owner_key=j.owner_key "
      "WHERE o.scope_key=? AND o.deleted=0 AND o.revision=j.owner_revision "
      "AND ((j.state='pending' AND j.next_attempt<=?) OR (j.state='leased' AND j.lease_until<=?)) " +
      "AND j.attempts<j.max_attempts " + generation_filter +
      "ORDER BY j.id LIMIT 1;");
  query.bind(1, scope_key(scope)); query.bind(2, timestamp(now)); query.bind(3, timestamp(now));
  if (dense_generation) query.bind(4, *dense_generation);
  const int rc = query.step();
  if (rc == SQLITE_DONE) { transaction.commit(); return std::nullopt; }
  if (rc != SQLITE_ROW) throw std::runtime_error("unable to claim index job");
  RetrievalIndexJob job;
  job.id = sqlite3_column_int64(query.get(), 0); job.owner_key = text(query.get(), 1);
  job.owner_revision = sqlite3_column_int64(query.get(), 2); job.representation = text(query.get(), 3);
  job.generation = text(query.get(), 4); job.attempts = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 5)) + 1;
  job.content = text(query.get(), 6); job.source_uri = text(query.get(), 7);
  job.lease_token = make_id("lease_");
  Statement claim(database_, "UPDATE retrieval_jobs SET state='leased',lease_token=?,lease_until=?,attempts=attempts+1 "
      "WHERE id=? AND attempts<max_attempts AND EXISTS(SELECT 1 FROM retrieval_owners o "
      "WHERE o.owner_key=retrieval_jobs.owner_key AND o.scope_key=? AND o.deleted=0 "
      "AND o.revision=retrieval_jobs.owner_revision);");
  claim.bind(1, job.lease_token); claim.bind(2, timestamp(now + lease)); claim.bind(3, job.id);
  claim.bind(4, scope_key(scope));
  if (claim.step() != SQLITE_DONE || sqlite3_changes(database_) != 1)
    throw std::runtime_error("unable to lease index job");
  transaction.commit();
  return job;
}

std::vector<MemoryAssertion> SQLiteMemoryDatabase::query_assertions(
    const MemoryScope& scope, const std::string& subject_id, const std::string& predicate,
    const std::string& object_id, std::optional<TimePoint> as_of, RecallPolicy policy,
    bool include_global_user, std::size_t limit) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, policy, include_global_user);
  if (subject_id.empty() && predicate.empty() && object_id.empty()) return {};
  std::string sql = "SELECT a.memory_id,a.revision,a.subject_id,a.predicate,a.object_id,"
      "a.literal_json,a.qualifiers_json,a.positive,a.valid_from,a.valid_to,a.evidence_json,"
      "a.recorded_at,a.historical_json FROM memory_assertions a "
      "JOIN memories m ON m.id=a.memory_id JOIN retrieval_owners o ON o.owner_key='memory:'||m.id "
      "JOIN memory_entities s ON s.id=a.subject_id LEFT JOIN memory_entities t ON t.id=a.object_id "
      "WHERE m.scope_key IN (" + placeholders(keys.size()) + ") "
      "AND s.scope_key IN (" + placeholders(keys.size()) + ") "
      "AND (a.object_id='' OR t.scope_key IN (" + placeholders(keys.size()) + ")) "
      "AND o.deleted=0 ";
  sql += "AND m.status IN ('active','validated') ";
  if (!as_of) sql += "AND o.revision=a.revision ";
  sql += "AND a.recorded_at<=? "
      "AND (a.valid_from IS NULL OR a.valid_from<=?) AND (a.valid_to IS NULL OR a.valid_to>?) ";
  if (as_of) {
    sql += "AND a.revision=(SELECT MAX(r.revision) FROM memory_assertion_revisions r "
           "WHERE r.memory_id=a.memory_id AND r.recorded_at<=?) ";
  }
  if (!subject_id.empty()) sql += "AND a.subject_id=? ";
  if (!predicate.empty()) sql += "AND a.predicate=? ";
  if (!object_id.empty()) sql += "AND a.object_id=? ";
  sql += "ORDER BY a.memory_id,a.revision DESC LIMIT ?;";
  Statement query(database_, sql);
  int i = 1;
  bind_keys(query, i, keys); bind_keys(query, i, keys); bind_keys(query, i, keys);
  const auto when = timestamp(as_of.value_or(std::chrono::system_clock::now()));
  query.bind(i++, when); query.bind(i++, when); query.bind(i++, when);
  if (as_of) query.bind(i++, when);
  if (!subject_id.empty()) query.bind(i++, subject_id);
  if (!predicate.empty()) query.bind(i++, predicate);
  if (!object_id.empty()) query.bind(i++, object_id);
  query.bind(i, static_cast<std::int64_t>(std::min<std::size_t>(limit, 1000)));
  std::vector<MemoryAssertion> result;
  int rc;
  while ((rc = query.step()) == SQLITE_ROW) {
    result.push_back(read_assertion(query.get(), 0));
  }
  if (rc != SQLITE_DONE) throw std::runtime_error("unable to query structured memory");
  return result;
}

bool SQLiteMemoryDatabase::publish_chunks(const MemoryScope& scope,
    const RetrievalIndexJob& job, std::vector<RetrievalChunk> chunks) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (chunks.empty() || chunks.size() > 4096 || job.lease_token.empty())
    throw std::invalid_argument("chunk publication requires a lease and bounded nonempty chunks");
  Transaction transaction(database_, true);
  Statement valid(database_, "SELECT o.source_uri FROM retrieval_jobs j JOIN retrieval_owners o ON o.owner_key=j.owner_key "
      "WHERE j.id=? AND j.owner_key=? AND j.owner_revision=? AND j.representation='chunks' AND j.generation=? "
      "AND j.state='leased' AND j.lease_token=? AND j.lease_until>? AND o.scope_key=? "
      "AND o.deleted=0 AND o.revision=j.owner_revision;");
  valid.bind(1, job.id); valid.bind(2, job.owner_key); valid.bind(3, job.owner_revision);
  valid.bind(4, job.generation); valid.bind(5, job.lease_token);
  valid.bind(6, timestamp(std::chrono::system_clock::now())); valid.bind(7, scope_key(scope));
  const int rc = valid.step();
  if (rc == SQLITE_DONE) return false;
  if (rc != SQLITE_ROW) throw std::runtime_error("unable to validate chunk publication");
  const auto source_uri = text(valid.get(), 0);
  Statement remove(database_, "DELETE FROM retrieval_chunks WHERE owner_key=?;");
  remove.bind(1, job.owner_key);
  if (remove.step() != SQLITE_DONE) throw std::runtime_error("unable to replace chunks");
  std::size_t total_bytes = 0;
  for (std::size_t ordinal = 0; ordinal < chunks.size(); ++ordinal) {
    auto& chunk = chunks[ordinal];
    total_bytes += chunk.text.size();
    if (chunk.text.empty() || chunk.text.size() > 32768 || total_bytes > 4 * kMaximumDurableText)
      throw std::invalid_argument("chunk content exceeds publication budget");
    chunk.text = sanitize_text(std::move(chunk.text), 32768);
    const auto id = job.owner_key + ":" + std::to_string(job.owner_revision) + ":" +
        job.generation + ":" + std::to_string(ordinal);
    Statement insert(database_, "INSERT INTO retrieval_chunks VALUES(?,?,?,?,?,?,?,?,?);");
    insert.bind(1, id); insert.bind(2, job.owner_key); insert.bind(3, job.owner_revision);
    insert.bind(4, static_cast<std::int64_t>(ordinal)); insert.bind(5, chunk.text);
    insert.bind(6, foundation::stable_hash_hex(chunk.text)); insert.bind(7, source_uri);
    insert.bind(8, sanitize_text(chunk.source_span, 1024)); insert.bind(9, job.generation);
    if (insert.step() != SQLITE_DONE) throw std::runtime_error("unable to publish chunk");
  }
  Statement done(database_, "UPDATE retrieval_jobs SET state='complete',lease_token='',lease_until=0,error='' WHERE id=?;");
  done.bind(1, job.id);
  if (done.step() != SQLITE_DONE) throw std::runtime_error("unable to complete index job");
  Statement dense_jobs(database_, "INSERT OR IGNORE INTO retrieval_jobs(owner_key,owner_revision,representation,generation) "
      "SELECT ?,?,'dense',generation FROM retrieval_dense_indexes WHERE scope_key=? AND state IN ('building','active');");
  dense_jobs.bind(1, job.owner_key); dense_jobs.bind(2, job.owner_revision); dense_jobs.bind(3, scope_key(scope));
  if (dense_jobs.step() != SQLITE_DONE) throw std::runtime_error("unable to schedule dense indexing");
  transaction.commit();
  return true;
}

bool SQLiteMemoryDatabase::fail_index_job(const MemoryScope& scope,
    const RetrievalIndexJob& job, const std::string& error, std::size_t max_attempts) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (max_attempts < 1 || max_attempts > 100 || job.lease_token.empty())
    throw std::invalid_argument("invalid index retry policy or lease");
  const auto now = std::chrono::system_clock::now();
  Statement fail(database_, "UPDATE retrieval_jobs SET max_attempts=?,"
      "state=CASE WHEN attempts>=? THEN 'failed' ELSE 'pending' END,"
      "error=?,next_attempt=?,lease_token='',lease_until=0 WHERE id=? AND owner_key=? AND owner_revision=? "
      "AND state='leased' AND lease_token=? AND lease_until>? AND EXISTS(SELECT 1 FROM retrieval_owners o "
      "WHERE o.owner_key=retrieval_jobs.owner_key AND o.scope_key=? AND o.deleted=0 AND o.revision=owner_revision);");
  fail.bind(1, static_cast<std::int64_t>(max_attempts));
  fail.bind(2, static_cast<std::int64_t>(max_attempts)); fail.bind(3, sanitize_text(error, 2048));
  fail.bind(4, timestamp(now + std::chrono::seconds{std::min<std::size_t>(60, job.attempts * 2)}));
  fail.bind(5, job.id); fail.bind(6, job.owner_key); fail.bind(7, job.owner_revision);
  fail.bind(8, job.lease_token); fail.bind(9, timestamp(now)); fail.bind(10, scope_key(scope));
  if (fail.step() != SQLITE_DONE) throw std::runtime_error("unable to record indexing failure");
  return sqlite3_changes(database_) == 1;
}

void SQLiteMemoryDatabase::sync_code_snapshot(const MemoryScope& scope,
    const index::RetrievalSnapshot& snapshot, const std::function<bool()>& cancelled) {
  validate_scope(scope);
  if (snapshot.stale()) throw std::invalid_argument("cannot publish stale code snapshot");
  if (snapshot.generation() > static_cast<std::uint64_t>(INT64_MAX))
    throw std::invalid_argument("code snapshot generation is out of range");
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Transaction transaction(database_, true);
  const auto token = make_id("code_sync_");
  const auto key = scope_key(scope);
  std::unordered_map<std::string, const index::RetrievalCodeNode*> nodes;
  for (const auto& node : snapshot.nodes()) nodes.emplace(node.id, &node);
  std::unordered_set<std::string> seen;
  for (const auto& chunk : snapshot.chunks()) {
    if (cancelled && cancelled()) throw std::runtime_error("code publication cancelled");
    const auto found = nodes.find(chunk.owner_id);
    if (found == nodes.end() || !seen.insert(chunk.owner_id).second) continue;
    const auto& node = *found->second;
    if (chunk.generation != snapshot.generation() || chunk.source_fingerprint != node.source_fingerprint)
      throw std::invalid_argument("inconsistent code snapshot chunk");
    const auto content = "Path: " + node.path.generic_string() + "\nSymbol: " +
        node.qualified_name + "\n" + chunk.text;
    if (content.empty() || content.size() > kMaximumDurableText)
      throw std::invalid_argument("code chunk exceeds indexing budget");
    const auto owner = "code:" + key + ":" + node.id;
    Statement old(database_, "SELECT revision,content,deleted FROM retrieval_owners WHERE owner_key=?;");
    old.bind(1, owner);
    const auto rc = old.step();
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) throw std::runtime_error("unable to inspect code owner");
    const bool exists = rc == SQLITE_ROW;
    const bool changed = !exists || text(old.get(), 1) != content || sqlite3_column_int(old.get(), 2) != 0;
    const std::int64_t revision = exists ? sqlite3_column_int64(old.get(), 0) + (changed ? 1 : 0) : 1;
    Statement upsert(database_, "INSERT INTO retrieval_owners(owner_key,scope_key,kind,owner_id,revision,content,source_uri,deleted) "
        "VALUES(?,?,'code',?,?,?,?,0) ON CONFLICT(owner_key) DO UPDATE SET revision=excluded.revision,"
        "content=excluded.content,source_uri=excluded.source_uri,deleted=0;");
    upsert.bind(1, owner); upsert.bind(2, key); upsert.bind(3, node.id);
    upsert.bind(4, static_cast<std::int64_t>(revision)); upsert.bind(5, content);
    upsert.bind(6, (snapshot.root() / node.path).generic_string());
    if (upsert.step() != SQLITE_DONE) throw std::runtime_error("unable to publish code owner");
    Statement metadata(database_, "INSERT INTO retrieval_code_sources VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(owner_key) DO UPDATE SET repository=excluded.repository,worktree=excluded.worktree,"
        "path=excluded.path,fingerprint=excluded.fingerprint,snapshot_generation=excluded.snapshot_generation,sync_token=excluded.sync_token;");
    metadata.bind(1, owner); metadata.bind(2, node.repository); metadata.bind(3, node.worktree);
    metadata.bind(4, node.path.generic_string()); metadata.bind(5, node.source_fingerprint);
    metadata.bind(6, static_cast<std::int64_t>(snapshot.generation())); metadata.bind(7, token);
    if (metadata.step() != SQLITE_DONE) throw std::runtime_error("unable to publish code source metadata");
    if (changed) {
      Statement erase(database_, "DELETE FROM retrieval_chunks WHERE owner_key=?;");
      erase.bind(1, owner);
      if (erase.step() != SQLITE_DONE) throw std::runtime_error("unable to invalidate code chunks");
      Statement supersede(database_, "UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0 "
          "WHERE owner_key=? AND state IN ('pending','leased','failed');");
      supersede.bind(1, owner);
      if (supersede.step() != SQLITE_DONE) throw std::runtime_error("unable to supersede code jobs");
      Statement job(database_, "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) VALUES(?,?,'chunks','text-v1');");
      job.bind(1, owner); job.bind(2, static_cast<std::int64_t>(revision));
      if (job.step() != SQLITE_DONE) throw std::runtime_error("unable to schedule code indexing");
    }
  }
  Statement retire(database_, "UPDATE retrieval_owners SET deleted=1,revision=revision+1,content='',source_uri='' "
      "WHERE kind='code' AND scope_key=? AND deleted=0 AND owner_key IN "
      "(SELECT owner_key FROM retrieval_code_sources WHERE sync_token<>?);");
  retire.bind(1, key); retire.bind(2, token);
  if (retire.step() != SQLITE_DONE) throw std::runtime_error("unable to retire removed code");
  Statement chunks(database_, "DELETE FROM retrieval_chunks WHERE owner_key IN "
      "(SELECT owner_key FROM retrieval_owners WHERE kind='code' AND scope_key=? AND deleted=1);");
  chunks.bind(1, key);
  if (chunks.step() != SQLITE_DONE) throw std::runtime_error("unable to invalidate removed code chunks");
  Statement jobs(database_, "UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0 WHERE owner_key IN "
      "(SELECT owner_key FROM retrieval_owners WHERE kind='code' AND scope_key=? AND deleted=1);");
  jobs.bind(1, key);
  if (jobs.step() != SQLITE_DONE) throw std::runtime_error("unable to retire removed code jobs");
  if (cancelled && cancelled()) throw std::runtime_error("code publication cancelled");
  transaction.commit();
}

std::vector<RetrievalCandidate> SQLiteMemoryDatabase::resolve_code_hits(
    const MemoryScope& scope, const std::vector<DenseSearchHit>& hits) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto key = scope_key(scope);
  std::vector<RetrievalCandidate> result;
  std::unordered_set<std::string> seen;
  for (const auto& hit : hits) {
    if (result.size() >= 1000) break;
    if (seen.count(hit.chunk.owner_key) != 0U) continue;
    Statement row(database_, "SELECT o.owner_id,o.revision,s.repository,s.worktree,s.path,s.fingerprint,s.snapshot_generation "
        "FROM retrieval_owners o JOIN retrieval_code_sources s ON s.owner_key=o.owner_key "
        "JOIN retrieval_chunks c ON c.owner_key=o.owner_key AND c.owner_revision=o.revision "
        "WHERE o.owner_key=? AND o.revision=? AND o.scope_key=? AND o.kind='code' AND o.deleted=0 "
        "AND c.id=? AND c.content_hash=?;");
    row.bind(1, hit.chunk.owner_key); row.bind(2, hit.chunk.owner_revision); row.bind(3, key);
    row.bind(4, hit.chunk.id); row.bind(5, hit.chunk.content_hash);
    const auto rc = row.step();
    if (rc == SQLITE_DONE) continue;
    if (rc != SQLITE_ROW) throw std::runtime_error("unable to resolve code hit");
    seen.insert(hit.chunk.owner_key);
    RetrievalCandidate candidate;
    candidate.owner = {RetrievalOwnerKind::Code, text(row.get(), 0), sqlite3_column_int64(row.get(), 1),
        text(row.get(), 2), text(row.get(), 3), text(row.get(), 4), text(row.get(), 5),
        static_cast<std::uint64_t>(sqlite3_column_int64(row.get(), 6))};
    // Retain the matched representation chunk as transient supporting
    // evidence. The engine renders the canonical source window separately.
    candidate.matched_chunks.push_back(hit);
    candidate.ranks.push_back({"dense", result.size() + 1, hit.similarity});
    result.push_back(std::move(candidate));
  }
  return result;
}

RetrievalIndexStatus SQLiteMemoryDatabase::retrieval_index_status(const MemoryScope& scope) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  RetrievalIndexStatus status;
  Statement jobs(database_, "SELECT j.state,COUNT(*) FROM retrieval_jobs j JOIN retrieval_owners o "
      "ON o.owner_key=j.owner_key WHERE o.scope_key=? AND o.deleted=0 AND o.revision=j.owner_revision GROUP BY j.state;");
  jobs.bind(1, scope_key(scope));
  int rc;
  while ((rc = jobs.step()) == SQLITE_ROW) {
    const auto state = text(jobs.get(), 0);
    const auto count = static_cast<std::size_t>(sqlite3_column_int64(jobs.get(), 1));
    if (state == "pending") status.pending = count;
    if (state == "leased") status.leased = count;
    if (state == "failed") status.failed = count;
    if (state == "complete") status.complete = count;
  }
  if (rc != SQLITE_DONE) throw std::runtime_error("unable to read index status");
  Statement chunks(database_, "SELECT COUNT(*) FROM retrieval_chunks c JOIN retrieval_owners o ON o.owner_key=c.owner_key "
      "WHERE o.scope_key=? AND o.deleted=0 AND o.revision=c.owner_revision;");
  chunks.bind(1, scope_key(scope));
  if (chunks.step() != SQLITE_ROW) throw std::runtime_error("unable to count chunks");
  status.chunks = static_cast<std::size_t>(sqlite3_column_int64(chunks.get(), 0));
  return status;
}

bool SQLiteMemoryDatabase::renew_index_job(const MemoryScope& scope,
    const RetrievalIndexJob& job, std::chrono::seconds lease) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (lease.count() < 1 || lease.count() > 300 || job.lease_token.empty())
    throw std::invalid_argument("invalid indexing lease renewal");
  const auto now = std::chrono::system_clock::now();
  Statement renew(database_, "UPDATE retrieval_jobs SET lease_until=? WHERE id=? AND owner_key=? "
      "AND owner_revision=? AND state='leased' AND lease_token=? AND lease_until>? "
      "AND EXISTS(SELECT 1 FROM retrieval_owners o WHERE o.owner_key=retrieval_jobs.owner_key "
      "AND o.scope_key=? AND o.deleted=0 AND o.revision=owner_revision);");
  renew.bind(1, timestamp(now + lease)); renew.bind(2, job.id); renew.bind(3, job.owner_key);
  renew.bind(4, job.owner_revision); renew.bind(5, job.lease_token);
  renew.bind(6, timestamp(now)); renew.bind(7, scope_key(scope));
  if (renew.step() != SQLITE_DONE) throw std::runtime_error("unable to renew indexing lease");
  return sqlite3_changes(database_) == 1;
}

std::vector<RetrievalChunk> SQLiteMemoryDatabase::retrieval_chunks(
    const MemoryScope& scope, const std::string& owner_key) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, RecallPolicy::Broad, false);
  Statement query(database_, "SELECT c.id,c.owner_key,c.owner_revision,c.ordinal,c.content,c.content_hash,"
      "c.source_uri,c.source_span,c.render_version FROM retrieval_chunks c JOIN retrieval_owners o "
      "ON o.owner_key=c.owner_key WHERE c.owner_key=? AND o.deleted=0 AND o.revision=c.owner_revision "
      "AND o.scope_key IN (" + placeholders(keys.size()) + ") ORDER BY c.ordinal;");
  int i = 1; query.bind(i++, owner_key); bind_keys(query, i, keys);
  std::vector<RetrievalChunk> chunks;
  int rc;
  while ((rc = query.step()) == SQLITE_ROW) {
    RetrievalChunk c;
    c.id = text(query.get(), 0); c.owner_key = text(query.get(), 1);
    c.owner_revision = sqlite3_column_int64(query.get(), 2);
    c.ordinal = static_cast<std::size_t>(sqlite3_column_int64(query.get(), 3));
    c.text = text(query.get(), 4); c.content_hash = text(query.get(), 5);
    c.source_uri = text(query.get(), 6); c.source_span = text(query.get(), 7); c.render_version = text(query.get(), 8);
    chunks.push_back(std::move(c));
  }
  if (rc != SQLITE_DONE) throw std::runtime_error("unable to read chunks");
  return chunks;
}

namespace {
Json dense_config_json(const DenseIndexConfig& config) {
  return {{"model_id", config.model_id}, {"model_fingerprint", config.model_fingerprint},
      {"tokenizer_fingerprint", config.tokenizer_fingerprint}, {"query_prefix", config.query_prefix},
      {"document_prefix", config.document_prefix}, {"pooling", config.pooling},
      {"dimensions", config.dimensions}, {"normalize", config.normalize}, {"encoding", "f32le-v1"}};
}

double vector_norm(const std::vector<float>& values, std::size_t dimensions) {
  if (values.size() != dimensions || dimensions == 0 || dimensions > 65536)
    throw std::invalid_argument("embedding dimension mismatch");
  double sum = 0;
  for (float value : values) {
    if (!std::isfinite(value)) throw std::invalid_argument("embedding contains a non-finite value");
    sum += static_cast<double>(value) * value;
  }
  if (!std::isfinite(sum) || sum <= 0) throw std::invalid_argument("embedding has invalid norm");
  return std::sqrt(sum);
}

std::vector<unsigned char> encode_vector(const std::vector<float>& values, bool normalize) {
  static_assert(sizeof(float) == 4, "dense storage requires 32-bit floats");
  const auto norm = vector_norm(values, values.size());
  std::vector<unsigned char> bytes(values.size() * 4);
  for (std::size_t i = 0; i < values.size(); ++i) {
    const float value = normalize ? static_cast<float>(values[i] / norm) : values[i];
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = static_cast<unsigned char>(bits >> (8 * j));
  }
  return bytes;
}

std::vector<float> decode_vector(sqlite3_stmt* row, int column, std::size_t dimensions) {
  const int count = sqlite3_column_bytes(row, column);
  const auto* bytes = static_cast<const unsigned char*>(sqlite3_column_blob(row, column));
  if (dimensions == 0 || dimensions > 65536 || count != static_cast<int>(dimensions * 4) || bytes == nullptr)
    throw std::runtime_error("corrupt dense vector payload");
  std::vector<float> result(dimensions);
  for (std::size_t i = 0; i < dimensions; ++i) {
    std::uint32_t bits = 0;
    for (unsigned j = 0; j < 4; ++j) bits |= static_cast<std::uint32_t>(bytes[i * 4 + j]) << (8 * j);
    std::memcpy(&result[i], &bits, sizeof(bits));
  }
  (void)vector_norm(result, dimensions);
  return result;
}
}  // namespace

void SQLiteMemoryDatabase::begin_dense_index(const MemoryScope& scope,
                                             const DenseIndexConfig& config) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (config.generation.empty() || config.generation.size() > 256 || config.model_id.empty() ||
      config.model_fingerprint.empty() || config.tokenizer_fingerprint.empty() || config.pooling.empty() ||
      config.dimensions == 0 || config.dimensions > 65536)
    throw std::invalid_argument("dense index requires a complete embedding-space contract");
  const auto encoded = dense_config_json(config).dump();
  if (encoded.size() > kMaximumDurableMetadata)
    throw std::invalid_argument("dense index configuration too large");
  Transaction transaction(database_, true);
  Statement existing(database_, "SELECT config_json FROM retrieval_dense_indexes WHERE generation=?;");
  existing.bind(1, config.generation);
  int rc;
  while ((rc = existing.step()) == SQLITE_ROW) {
    if (text(existing.get(), 0) != encoded)
      throw std::invalid_argument("dense generation already identifies another embedding space");
  }
  if (rc != SQLITE_DONE) throw std::runtime_error("unable to inspect dense generation");
  Statement insert(database_, "INSERT OR IGNORE INTO retrieval_dense_indexes VALUES(?,?,?,'building');");
  insert.bind(1, scope_key(scope)); insert.bind(2, config.generation); insert.bind(3, encoded);
  if (insert.step() != SQLITE_DONE) throw std::runtime_error("unable to create dense generation");
  Statement jobs(database_, "INSERT OR IGNORE INTO retrieval_jobs(owner_key,owner_revision,representation,generation) "
      "SELECT o.owner_key,o.revision,'dense',? FROM retrieval_owners o WHERE o.scope_key=? AND o.deleted=0 "
      "AND EXISTS(SELECT 1 FROM retrieval_chunks c WHERE c.owner_key=o.owner_key AND c.owner_revision=o.revision);");
  jobs.bind(1, config.generation); jobs.bind(2, scope_key(scope));
  if (jobs.step() != SQLITE_DONE) throw std::runtime_error("unable to schedule dense backfill");
  transaction.commit();
}

std::optional<DenseIndexConfig> SQLiteMemoryDatabase::dense_index(
    const MemoryScope& scope, const std::string& generation, RecallPolicy policy,
    bool include_global_user) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  const auto keys = allowed_keys(scope, policy, include_global_user);
  std::string precedence = " CASE scope_key";
  for (std::size_t index = 0; index < keys.size(); ++index)
    precedence += " WHEN ? THEN " + std::to_string(index);
  precedence += " END";
  Statement query(database_, "SELECT generation,config_json FROM retrieval_dense_indexes WHERE scope_key IN (" +
      placeholders(keys.size()) + ") AND " +
      std::string(generation.empty() ? "state='active'" : "generation=?") +
      " ORDER BY" + precedence + " LIMIT 1;");
  int parameter = 1;
  bind_keys(query, parameter, keys);
  if (!generation.empty()) query.bind(parameter++, generation);
  bind_keys(query, parameter, keys);
  const int rc = query.step();
  if (rc == SQLITE_DONE) return std::nullopt;
  if (rc != SQLITE_ROW) throw std::runtime_error("unable to read dense configuration");
  const auto j = Json::parse(text(query.get(), 1));
  DenseIndexConfig config;
  config.generation = text(query.get(), 0); config.model_id = j.at("model_id").get<std::string>();
  config.model_fingerprint = j.at("model_fingerprint").get<std::string>();
  config.tokenizer_fingerprint = j.at("tokenizer_fingerprint").get<std::string>();
  config.query_prefix = j.at("query_prefix").get<std::string>();
  config.document_prefix = j.at("document_prefix").get<std::string>();
  config.pooling = j.at("pooling").get<std::string>(); config.dimensions = j.at("dimensions").get<std::size_t>();
  config.normalize = j.at("normalize").get<bool>();
  return config;
}

bool SQLiteMemoryDatabase::publish_dense_vectors(const MemoryScope& scope,
    const RetrievalIndexJob& job, const std::vector<DenseChunkVector>& vectors) {
  std::size_t next = 0;
  return publish_dense_vectors(scope, job, vectors.size(), [&] { return vectors.at(next++); });
}

bool SQLiteMemoryDatabase::publish_dense_vectors(const MemoryScope& scope,
    const RetrievalIndexJob& job, std::size_t vector_count,
    const std::function<DenseChunkVector()>& next_vector) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  if (job.lease_token.empty() || vector_count == 0 || vector_count > 4096 || !next_vector)
    throw std::invalid_argument("dense publication requires a lease and bounded vectors");
  Transaction transaction(database_, true);
  Statement valid(database_, "SELECT 1 FROM retrieval_jobs j JOIN retrieval_owners o ON o.owner_key=j.owner_key "
      "JOIN retrieval_dense_indexes d ON d.scope_key=o.scope_key AND d.generation=j.generation "
      "WHERE j.id=? AND j.owner_key=? AND j.owner_revision=? AND j.representation='dense' AND j.generation=? "
      "AND j.state='leased' AND j.lease_token=? AND j.lease_until>? AND o.scope_key=? "
      "AND o.deleted=0 AND o.revision=j.owner_revision AND d.state IN ('building','active');");
  valid.bind(1, job.id); valid.bind(2, job.owner_key); valid.bind(3, job.owner_revision);
  valid.bind(4, job.generation); valid.bind(5, job.lease_token);
  valid.bind(6, timestamp(std::chrono::system_clock::now())); valid.bind(7, scope_key(scope));
  const int rc = valid.step();
  if (rc == SQLITE_DONE) return false;
  if (rc != SQLITE_ROW) throw std::runtime_error("unable to validate dense publication");
  const auto config = dense_index(scope, job.generation);
  if (!config) throw std::runtime_error("missing dense configuration");
  const auto chunks = retrieval_chunks(scope, job.owner_key);
  if (chunks.size() != vector_count) throw std::invalid_argument("dense publication must cover every owner chunk");
  std::unordered_map<std::string, bool> expected;
  for (const auto& c : chunks) expected.emplace(c.id, false);
  for (std::size_t index = 0; index < vector_count; ++index) {
    const auto vector = next_vector();
    const auto found = expected.find(vector.chunk_id);
    if (found == expected.end() || found->second)
      throw std::invalid_argument("dense publication contains an unexpected or duplicate chunk");
    found->second = true;
    (void)vector_norm(vector.values, config->dimensions);
    const auto bytes = encode_vector(vector.values, config->normalize);
    Statement insert(database_, "INSERT OR REPLACE INTO retrieval_vectors VALUES(?,?,?,?);");
    insert.bind(1, vector.chunk_id); insert.bind(2, job.generation);
    insert.bind(3, static_cast<std::int64_t>(config->dimensions));
    if (sqlite3_bind_blob(insert.get(), 4, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
        insert.step() != SQLITE_DONE) throw std::runtime_error("unable to write dense vector");
  }
  Statement done(database_, "UPDATE retrieval_jobs SET state='complete',lease_token='',lease_until=0,error='' WHERE id=?;");
  done.bind(1, job.id);
  if (done.step() != SQLITE_DONE) throw std::runtime_error("unable to complete dense indexing");
  transaction.commit();
  return true;
}

bool SQLiteMemoryDatabase::activate_dense_index(const MemoryScope& scope,
                                                const std::string& generation) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  validate_scope(scope);
  Transaction transaction(database_, true);
  if (!dense_index(scope, generation)) throw std::invalid_argument("unknown dense generation");
  // A new model is published only after both rendering and vector backfill have
  // caught up. Later edits invalidate individual vectors immediately.
  Statement missing(database_, "SELECT COUNT(*) FROM retrieval_owners o WHERE o.scope_key=? AND o.deleted=0 AND "
      "(NOT EXISTS(SELECT 1 FROM retrieval_chunks c WHERE c.owner_key=o.owner_key AND c.owner_revision=o.revision) "
      "OR EXISTS(SELECT 1 FROM retrieval_chunks c WHERE c.owner_key=o.owner_key AND c.owner_revision=o.revision AND "
      "NOT EXISTS(SELECT 1 FROM retrieval_vectors v WHERE v.chunk_id=c.id AND v.generation=?)));");
  missing.bind(1, scope_key(scope)); missing.bind(2, generation);
  if (missing.step() != SQLITE_ROW) throw std::runtime_error("unable to inspect dense completeness");
  if (sqlite3_column_int64(missing.get(), 0) != 0) return false;
  Statement retire(database_, "UPDATE retrieval_dense_indexes SET state='retired' WHERE scope_key=? AND state='active';");
  retire.bind(1, scope_key(scope));
  if (retire.step() != SQLITE_DONE) throw std::runtime_error("unable to retire dense index");
  Statement activate(database_, "UPDATE retrieval_dense_indexes SET state='active' WHERE scope_key=? AND generation=?;");
  activate.bind(1, scope_key(scope)); activate.bind(2, generation);
  if (activate.step() != SQLITE_DONE) throw std::runtime_error("unable to activate dense index");
  transaction.commit();
  return true;
}

std::vector<MemoryEntry> SQLiteMemoryDatabase::resolve_dense_memories(
    const MemoryScope& scope, const std::vector<DenseSearchHit>& hits,
    RecallPolicy policy, bool include_global_user) const {
  auto candidates = resolve_dense_memory_candidates(scope, hits, policy, include_global_user);
  std::vector<MemoryEntry> result;
  result.reserve(candidates.size());
  for (auto& candidate : candidates) result.push_back(std::move(candidate.memory));
  return result;
}

std::vector<DenseMemoryCandidate> SQLiteMemoryDatabase::resolve_dense_memory_candidates(
    const MemoryScope& scope, const std::vector<DenseSearchHit>& hits,
    RecallPolicy policy, bool include_global_user) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, policy, include_global_user);
  std::vector<DenseMemoryCandidate> result;
  std::unordered_set<std::string> seen;
  for (std::size_t index = 0; index < std::min<std::size_t>(1000U, hits.size()); ++index) {
    const auto& hit = hits[index];
    if (!std::isfinite(hit.similarity) || hit.similarity < -1.0 || hit.similarity > 1.0 ||
        hit.generation.empty()) continue;
    if (seen.count(hit.chunk.owner_key) != 0U) continue;
    Statement row(database_, "SELECT " + memory_columns("m.") +
        " FROM memories m JOIN retrieval_owners o ON o.owner_id=m.id AND o.kind='memory' "
        "JOIN retrieval_chunks c ON c.owner_key=o.owner_key AND c.owner_revision=o.revision "
        "JOIN retrieval_vectors v ON v.chunk_id=c.id AND v.generation=? "
        "WHERE o.owner_key=? AND o.revision=? AND c.id=? AND c.content_hash=? AND o.deleted=0 "
        "AND c.content=? AND c.ordinal=? AND c.source_uri=? AND c.source_span=? AND c.render_version=? "
        "AND m.status IN ('active','validated') AND m.scope_key IN (" + placeholders(keys.size()) + ");");
    int parameter = 1;
    row.bind(parameter++, hit.generation);
    row.bind(parameter++, hit.chunk.owner_key); row.bind(parameter++, hit.chunk.owner_revision);
    row.bind(parameter++, hit.chunk.id); row.bind(parameter++, hit.chunk.content_hash);
    row.bind(parameter++, hit.chunk.text);
    row.bind(parameter++, static_cast<std::int64_t>(hit.chunk.ordinal));
    row.bind(parameter++, hit.chunk.source_uri); row.bind(parameter++, hit.chunk.source_span);
    row.bind(parameter++, hit.chunk.render_version);
    bind_keys(row, parameter, keys);
    const auto rc = row.step();
    if (rc == SQLITE_ROW) {
      seen.insert(hit.chunk.owner_key);
      result.push_back({read_memory(database_, row.get()), hit});
    }
    else if (rc != SQLITE_DONE) throw std::runtime_error("unable to resolve dense memory");
  }
  return result;
}

std::vector<DenseSearchHit> SQLiteMemoryDatabase::search_dense(const MemoryScope& scope,
    const std::string& generation, const std::vector<float>& query, RecallPolicy policy,
    bool include_global_user, std::size_t limit, bool distinct_owners) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto keys = allowed_keys(scope, policy, include_global_user);
  const auto config = dense_index(scope, {}, policy, include_global_user);
  if (!config || config->generation != generation) return {};
  const double query_norm = vector_norm(query, config->dimensions);
  limit = std::min<std::size_t>(limit, 1000);
  if (limit == 0) return {};
  Statement rows(database_, "SELECT c.id,c.owner_key,c.owner_revision,c.ordinal,c.content,c.content_hash,"
      "c.source_uri,c.source_span,c.render_version,v.dimensions,v.vector FROM retrieval_vectors v "
      "JOIN retrieval_chunks c ON c.id=v.chunk_id JOIN retrieval_owners o ON o.owner_key=c.owner_key "
      "JOIN retrieval_dense_indexes d ON d.scope_key=o.scope_key AND d.generation=v.generation "
      "LEFT JOIN memories m ON o.kind='memory' AND m.id=o.owner_id "
      "WHERE v.generation=? AND d.state='active' AND o.deleted=0 AND o.revision=c.owner_revision "
      "AND o.scope_key IN (" + placeholders(keys.size()) + ") "
      "AND (o.kind='code' OR m.status IN ('active','validated')) ORDER BY c.id;");
  int i = 1; rows.bind(i++, generation); bind_keys(rows, i, keys);
  std::vector<DenseSearchHit> hits;
  const auto better = [](const DenseSearchHit& a, const DenseSearchHit& b) {
    if (a.similarity != b.similarity) return a.similarity > b.similarity;
    if (a.chunk.owner_key != b.chunk.owner_key) return a.chunk.owner_key < b.chunk.owner_key;
    if (a.chunk.ordinal != b.chunk.ordinal) return a.chunk.ordinal < b.chunk.ordinal;
    return a.chunk.id < b.chunk.id;
  };
  int rc;
  while ((rc = rows.step()) == SQLITE_ROW) {
    const auto dimensions = static_cast<std::size_t>(sqlite3_column_int64(rows.get(), 9));
    if (dimensions != config->dimensions) throw std::runtime_error("dense index contains mixed dimensions");
    const auto vector = decode_vector(rows.get(), 10, dimensions);
    const double norm = vector_norm(vector, dimensions);
    double dot = 0;
    for (std::size_t j = 0; j < dimensions; ++j) dot += static_cast<double>(query[j]) * vector[j];
    DenseSearchHit hit;
    hit.generation = generation;
    hit.similarity = std::max(-1.0, std::min(1.0, dot / (query_norm * norm)));
    hit.chunk.id = text(rows.get(), 0); hit.chunk.owner_key = text(rows.get(), 1);
    hit.chunk.owner_revision = sqlite3_column_int64(rows.get(), 2);
    hit.chunk.ordinal = static_cast<std::size_t>(sqlite3_column_int64(rows.get(), 3));
    hit.chunk.text = text(rows.get(), 4); hit.chunk.content_hash = text(rows.get(), 5);
    hit.chunk.source_uri = text(rows.get(), 6); hit.chunk.source_span = text(rows.get(), 7);
    hit.chunk.render_version = text(rows.get(), 8);
    if (distinct_owners) {
      // Aggregate before applying the owner budget. Keep only the best chunk
      // for each retained owner, so repeated text cannot crowd out other owners.
      // The retained set stays bounded by limit even for a very large corpus.
      const auto prior = std::find_if(hits.begin(), hits.end(), [&](const auto& value) {
        return value.chunk.owner_key == hit.chunk.owner_key;
      });
      if (prior != hits.end()) {
        if (!better(hit, *prior)) continue;
        hits.erase(prior);
      }
    }
    const auto position = std::lower_bound(hits.begin(), hits.end(), hit, better);
    hits.insert(position, std::move(hit));
    if (hits.size() > limit) hits.pop_back();
  }
  if (rc != SQLITE_DONE) throw std::runtime_error("unable to search dense index");
  return hits;
}

}  // namespace qt::memory
