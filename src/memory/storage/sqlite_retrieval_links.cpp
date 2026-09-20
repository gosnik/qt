#include "qt/memory/database.hpp"

#include "qt/memory/scope.hpp"

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace qt::memory {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumLinkText = 1024U;

class LinkStatement final {
 public:
  LinkStatement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK)
      throw std::runtime_error("unable to prepare retrieval link statement");
  }
  ~LinkStatement() { sqlite3_finalize(statement_); }
  LinkStatement(const LinkStatement&) = delete;
  LinkStatement& operator=(const LinkStatement&) = delete;
  sqlite3_stmt* get() const { return statement_; }
  void bind(int index, const std::string& value) {
    if (sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
      throw std::runtime_error("unable to bind retrieval link text");
  }
  void bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
      throw std::runtime_error("unable to bind retrieval link integer");
  }
  int step() { return sqlite3_step(statement_); }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

void link_exec(sqlite3* db, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error == nullptr ? sqlite3_errmsg(db) : error;
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

std::string kind_name(RetrievalOwnerKind kind) {
  switch (kind) {
    case RetrievalOwnerKind::Memory: return "memory";
    case RetrievalOwnerKind::Code: return "code";
    case RetrievalOwnerKind::Entity: return "entity";
  }
  return "unknown";
}

std::string random_link_id(sqlite3* db) {
  LinkStatement query(db, "SELECT lower(hex(randomblob(16)));");
  if (query.step() != SQLITE_ROW) throw std::runtime_error("unable to generate retrieval link id");
  return reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 0));
}

RetrievalOwnerKind owner_kind(const std::string& value) {
  if (value == "memory") return RetrievalOwnerKind::Memory;
  if (value == "code") return RetrievalOwnerKind::Code;
  if (value == "entity") return RetrievalOwnerKind::Entity;
  throw std::runtime_error("unknown retrieval link endpoint kind");
}

Json scope_json(const MemoryScope& scope) {
  return {{"kind", to_string(scope.kind)}, {"user", scope.user},
          {"organization", scope.organization}, {"project", scope.project},
          {"repository", scope.repository}, {"repository_display", scope.repository_display},
          {"branch", scope.branch}, {"task", scope.task}, {"agent", scope.agent},
          {"session", scope.session}, {"conversation", scope.conversation}};
}

MemoryScope parse_scope(const std::string& encoded) {
  const auto value = Json::parse(encoded);
  MemoryScope scope;
  scope.kind = scope_kind_from_string(value.at("kind").get<std::string>());
  scope.user = value.value("user", ""); scope.organization = value.value("organization", "");
  scope.project = value.value("project", ""); scope.repository = value.value("repository", "");
  scope.repository_display = value.value("repository_display", ""); scope.branch = value.value("branch", "");
  scope.task = value.value("task", ""); scope.agent = value.value("agent", "");
  scope.session = value.value("session", ""); scope.conversation = value.value("conversation", "");
  return scope;
}

std::vector<std::string> readable_scope_keys(const MemoryScope& caller,
                                             RecallPolicy policy,
                                             bool include_global_user) {
  std::vector<std::string> keys;
  const auto add = [&](const MemoryScope& value) {
    const auto encoded = scope_key(value);
    if (std::find(keys.begin(), keys.end(), encoded) == keys.end()) keys.push_back(encoded);
  };
  for (const auto& visible : recall_scopes(caller, policy, include_global_user)) add(visible);
  return keys;
}

void validate_endpoint(RetrievalReference& endpoint, const MemoryScope& scope,
                       SQLiteMemoryDatabase& database, sqlite3* db) {
  if (endpoint.id.empty() || endpoint.id.size() > kMaximumLinkText)
    throw std::invalid_argument("retrieval link endpoint id is invalid");
  if (endpoint.revision < 0 || endpoint.snapshot_generation >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw std::invalid_argument("retrieval link endpoint numeric pin is invalid");
  if (endpoint.repository.size() > kMaximumLinkText || endpoint.worktree.size() > kMaximumLinkText ||
      endpoint.path.size() > kMaximumLinkText || endpoint.source_fingerprint.size() > kMaximumLinkText)
    throw std::invalid_argument("retrieval link endpoint metadata is too large");
  if (endpoint.kind == RetrievalOwnerKind::Memory) {
    const auto memory = database.get(scope, endpoint.id);
    if (scope_key(memory.scope) != scope_key(scope))
      throw std::invalid_argument("memory link endpoint requires exact write scope");
    const auto revision = database.retrieval_revision(scope, endpoint.id);
    if (endpoint.revision != 0 && endpoint.revision != revision)
      throw std::invalid_argument("memory link endpoint revision is stale");
    endpoint.revision = revision;
  } else if (endpoint.kind == RetrievalOwnerKind::Code) {
    if (endpoint.repository.empty() || endpoint.worktree.empty() ||
        endpoint.source_fingerprint.empty() || endpoint.snapshot_generation == 0)
      throw std::invalid_argument("code link endpoint requires repository, worktree, fingerprint, and generation");
  } else if (endpoint.kind == RetrievalOwnerKind::Entity) {
    LinkStatement query(db, "SELECT 1 FROM memory_entities WHERE id=? AND scope_key=?;");
    query.bind(1, endpoint.id); query.bind(2, scope_key(scope));
    if (query.step() != SQLITE_ROW)
      throw std::invalid_argument("entity link endpoint is not visible in exact scope");
  }
}

RetrievalReference read_endpoint(sqlite3_stmt* row, int offset) {
  RetrievalReference endpoint;
  endpoint.kind = owner_kind(reinterpret_cast<const char*>(sqlite3_column_text(row, offset)));
  endpoint.id = reinterpret_cast<const char*>(sqlite3_column_text(row, offset + 1));
  endpoint.revision = sqlite3_column_int64(row, offset + 2);
  endpoint.repository = reinterpret_cast<const char*>(sqlite3_column_text(row, offset + 3));
  endpoint.worktree = reinterpret_cast<const char*>(sqlite3_column_text(row, offset + 4));
  endpoint.path = reinterpret_cast<const char*>(sqlite3_column_text(row, offset + 5));
  endpoint.source_fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(row, offset + 6));
  endpoint.snapshot_generation = static_cast<std::uint64_t>(sqlite3_column_int64(row, offset + 7));
  return endpoint;
}

}  // namespace

std::string SQLiteMemoryDatabase::add_cross_store_link(const MemoryScope& scope,
                                                        CrossStoreLink link) {
  validate_scope(scope);
  if (link.id.size() > kMaximumLinkText || link.relation.empty() || link.relation.size() > kMaximumLinkText || link.evidence_id.empty() ||
      link.evidence_id.size() > kMaximumLinkText ||
      link.evidence_id.rfind("uri:", 0) != 0 || link.evidence_id.size() == 4)
    throw std::invalid_argument("cross-store link requires bounded relation and uri evidence");
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  link_exec(database_, "BEGIN IMMEDIATE;");
  try {
    validate_endpoint(link.from, scope, *this, database_);
    validate_endpoint(link.to, scope, *this, database_);
    if (link.id.empty()) link.id = random_link_id(database_);
    LinkStatement insert(database_,
        "INSERT INTO retrieval_cross_store_links"
        "(id,scope_key,scope_json,from_kind,from_id,from_revision,from_repository,from_worktree,from_path,"
        "from_fingerprint,from_generation,to_kind,to_id,to_revision,to_repository,to_worktree,to_path,"
        "to_fingerprint,to_generation,relation,evidence_id,created_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
    int i = 1;
    insert.bind(i++, link.id); insert.bind(i++, scope_key(scope)); insert.bind(i++, scope_json(scope).dump());
    const auto bind_endpoint = [&](const RetrievalReference& endpoint) {
      insert.bind(i++, kind_name(endpoint.kind)); insert.bind(i++, endpoint.id); insert.bind(i++, endpoint.revision);
      insert.bind(i++, endpoint.repository); insert.bind(i++, endpoint.worktree); insert.bind(i++, endpoint.path);
      insert.bind(i++, endpoint.source_fingerprint); insert.bind(i++, static_cast<std::int64_t>(endpoint.snapshot_generation));
    };
    bind_endpoint(link.from); bind_endpoint(link.to); insert.bind(i++, link.relation); insert.bind(i++, link.evidence_id);
    insert.bind(i, static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()));
    if (insert.step() != SQLITE_DONE) throw std::runtime_error("unable to persist cross-store link");
    link_exec(database_, "COMMIT;");
    return link.id;
  } catch (...) {
    try { link_exec(database_, "ROLLBACK;"); } catch (...) {}
    throw;
  }
}

std::vector<CrossStoreLink> SQLiteMemoryDatabase::links(
    const MemoryScope& scope, const RetrievalReference& endpoint, RecallPolicy policy,
    bool include_global_user, std::size_t limit) const {
  validate_scope(scope);
  if (endpoint.id.empty() || limit == 0) return {};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto scope_keys = readable_scope_keys(scope, policy, include_global_user);
  std::string sql =
      "SELECT id,scope_json,from_kind,from_id,from_revision,from_repository,from_worktree,from_path,"
      "from_fingerprint,from_generation,to_kind,to_id,to_revision,to_repository,to_worktree,to_path,"
      "to_fingerprint,to_generation,relation,evidence_id FROM retrieval_cross_store_links WHERE scope_key IN (";
  for (std::size_t i = 0; i < scope_keys.size(); ++i) {
    if (i != 0) sql += ',';
    sql += '?';
  }
  sql += ") AND ((from_kind=? AND from_id=?) OR (to_kind=? AND to_id=?)) ORDER BY created_at,id LIMIT ?;";
  LinkStatement query(database_, sql.c_str());
  int parameter = 1;
  for (const auto& key : scope_keys) query.bind(parameter++, key);
  const auto endpoint_kind = kind_name(endpoint.kind);
  query.bind(parameter++, endpoint_kind); query.bind(parameter++, endpoint.id);
  query.bind(parameter++, endpoint_kind); query.bind(parameter++, endpoint.id);
  query.bind(parameter, static_cast<std::int64_t>(std::min<std::size_t>(limit, 1024U)));
  std::vector<CrossStoreLink> result;
  int query_status = SQLITE_ROW;
  while ((query_status = query.step()) == SQLITE_ROW) {
    const auto stored_scope = parse_scope(reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 1)));
    if (!scope_can_access(scope, stored_scope, policy, include_global_user)) continue;
    auto from = read_endpoint(query.get(), 2);
    auto to = read_endpoint(query.get(), 10);
    const bool matches_from = from.kind == endpoint.kind && from.id == endpoint.id;
    const bool matches_to = to.kind == endpoint.kind && to.id == endpoint.id;
    if (!matches_from && !matches_to) continue;
    const auto valid_memory = [&](const RetrievalReference& ref) {
      if (ref.kind != RetrievalOwnerKind::Memory) return true;
      const auto memory = find_for_recall(scope, ref.id, policy, include_global_user);
      if (!memory || !scope_can_access(scope, memory->scope, policy, include_global_user)) return false;
      try { return retrieval_revision(memory->scope, ref.id) == ref.revision; }
      catch (...) { return false; }
    };
    if (!valid_memory(from) || !valid_memory(to)) continue;
    CrossStoreLink link;
    link.id = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 0));
    link.from = std::move(from); link.to = std::move(to);
    link.relation = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 18));
    link.evidence_id = reinterpret_cast<const char*>(sqlite3_column_text(query.get(), 19));
    result.push_back(std::move(link));
    if (result.size() >= limit) break;
  }
  if (query_status != SQLITE_DONE) throw std::runtime_error("unable to read retrieval links");
  return result;
}

}  // namespace qt::memory
