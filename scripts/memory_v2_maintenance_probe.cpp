#include "qt/memory/database.hpp"
#include "qt/memory/memory_service.hpp"
#include "qt/memory/scope.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

using namespace qt::memory;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void exec_sql(const std::filesystem::path& path, const std::string& sql) {
  sqlite3* db = nullptr; check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open sqlite");
  char* error = nullptr; const auto rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  const std::string detail = error ? error : "sql failed"; sqlite3_free(error); sqlite3_close(db);
  if (rc != SQLITE_OK) throw std::runtime_error(detail);
}
int count_sql(const std::filesystem::path& path, const std::string& sql) {
  sqlite3* db = nullptr; check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open sqlite");
  sqlite3_stmt* statement = nullptr; check(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK, "prepare count");
  const int count = sqlite3_step(statement) == SQLITE_ROW ? sqlite3_column_int(statement, 0) : -1;
  sqlite3_finalize(statement); sqlite3_close(db); return count;
}
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("requires agent home");
    SQLiteMemoryDatabase db(argv[1]);
    MemoryScope scope; scope.kind = ScopeKind::Repository; scope.user = "maintenance";
    scope.project = "qt"; scope.repository = "repo-maintenance";
    MemoryWrite old_write; old_write.status = MemoryStatus::Validated; old_write.content = "old";
    const auto old_id = db.add(scope, old_write);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    old_write.content = "kept"; const auto kept_id = db.add(scope, old_write);
    MemoryEntity entity; entity.kind = "test"; entity.name = "kept-entity";
    entity = db.add_entity(scope, std::move(entity));
    MemoryEntity orphan; orphan.kind = "test"; orphan.name = "orphan-entity";
    orphan = db.add_entity(scope, std::move(orphan));
    MemoryEntity orphan_two; orphan_two.kind = "test"; orphan_two.name = "orphan-entity-two";
    orphan_two = db.add_entity(scope, std::move(orphan_two));
    MemoryEntity orphan_three; orphan_three.kind = "test"; orphan_three.name = "orphan-entity-three";
    orphan_three = db.add_entity(scope, std::move(orphan_three));
    MemoryAssertion kept_assertion;
    kept_assertion.memory_id = kept_id;
    kept_assertion.revision = 1;
    kept_assertion.subject_id = entity.id;
    kept_assertion.predicate = "retention-kept";
    kept_assertion.literal_json = "\"yes\"";
    kept_assertion.evidence.push_back({"", "retention", "uri://retention",
                                       "retention-fingerprint", "", "probe"});
    db.add_assertion(scope, kept_assertion);
    CrossStoreLink link; link.from.kind = RetrievalOwnerKind::Memory; link.from.id = old_id;
    link.to.kind = RetrievalOwnerKind::Code; link.to.id = "code";
    link.to.repository = "repo"; link.to.worktree = "worktree";
    link.to.source_fingerprint = "fingerprint"; link.to.snapshot_generation = 1;
    link.relation = "supports"; link.evidence_id = "uri://maintenance";
    db.add_cross_store_link(scope, link);
    exec_sql(db.path(), "INSERT INTO memory_entity_aliases(entity_id,alias) VALUES('" + entity.id + "','password=alias-one'),('" + entity.id + "','password=alias-two');");
    exec_sql(db.path(), "UPDATE memory_assertions SET predicate='password=assertion-secret', literal_json='{\"token\":\"assertion-secret\"}', qualifiers_json='{\"secret\":\"assertion-secret\"}', evidence_json='[{\"authorization\":\"assertion-secret\"}]', historical_json=json_set(historical_json,'$.content','password=history-secret') WHERE memory_id='" + kept_id + "';");
    exec_sql(db.path(), "UPDATE retrieval_cross_store_links SET from_path='password=link-secret', evidence_id='uri://password=link-secret' WHERE scope_key='" + scope_key(scope) + "';");
    exec_sql(db.path(), "UPDATE memory_entities SET name='password=maintenance-secret' WHERE id='" + orphan.id + "';");
    auto foreign_scope = scope; foreign_scope.repository = "repo-foreign";
    MemoryWrite foreign_write; foreign_write.status = MemoryStatus::Validated; foreign_write.content = "foreign";
    const auto foreign_id = db.add(foreign_scope, foreign_write);
    MemoryEntity foreign_entity; foreign_entity.kind = "test"; foreign_entity.name = "foreign-password=must-remain";
    foreign_entity = db.add_entity(foreign_scope, std::move(foreign_entity));
    exec_sql(db.path(), "INSERT INTO memory_entity_aliases(entity_id,alias) VALUES('" + foreign_entity.id + "','foreign-password=alias');");
    (void)foreign_id;
    const auto foreign_before = db.export_state(foreign_scope).dump();
    const auto before_scrub = db.export_state(scope).dump();
    RedactionScrubPolicy scrub; scrub.batch_limit = 100; scrub.dry_run = true;
    const auto dry_scrub = db.scrub_sensitive_data(scope, scrub);
    check(dry_scrub.changed() != 0 && db.export_state(scope).dump() == before_scrub &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memory_entities WHERE id='" + foreign_entity.id + "'") == 1,
          "scrub dry-run changed persisted state");
    scrub.dry_run = false; db.scrub_sensitive_data(scope, scrub);
    const auto scrubbed = db.export_state(scope).dump();
    check(scrubbed.find("assertion-secret") == std::string::npos &&
              scrubbed.find("history-secret") == std::string::npos &&
              scrubbed.find("link-secret") == std::string::npos &&
              db.find_entities(scope, "password=maintenance-secret").empty() &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memory_entities WHERE id='" + foreign_entity.id + "'") == 1 &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memory_entity_aliases WHERE entity_id='" + entity.id + "'") == 2 &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memory_entity_aliases WHERE entity_id='" + entity.id + "' AND alias='[REDACTED]'") == 1 &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memory_entity_aliases WHERE entity_id='" + entity.id + "' AND alias LIKE '%alias-%'") == 0,
            "structured secrets or alias collisions survived scoped scrub");
    const auto scrubbed_assertions = db.query_assertions(
        scope, entity.id, "", "", std::nullopt, RecallPolicy::Strict, false, 10);
    check(scrubbed_assertions.size() == 1 &&
              nlohmann::json::parse(scrubbed_assertions.front().literal_json).is_object() &&
              nlohmann::json::parse(scrubbed_assertions.front().qualifiers_json).is_object() &&
              scrubbed_assertions.front().evidence.size() == 1,
          "scrub produced an invalid retained assertion");
    RetentionPolicy retention; retention.older_than = std::chrono::system_clock::now() + std::chrono::seconds(1);
    retention.canonical_memories = true; retention.batch_limit = 1;
    const auto before_retention = db.export_state(scope).dump();
    const auto dry_retention = db.enforce_retention(scope, retention);
    check(dry_retention.canonical_memories == 1 && db.get(scope, old_id).id == old_id &&
              db.export_state(scope).dump() == before_retention,
          "retention dry-run changed persisted state");
    retention.dry_run = false; db.enforce_retention(scope, retention);
    const auto old_remaining = db.find_for_recall(scope, old_id, RecallPolicy::Strict, false);
    const auto kept_remaining = db.find_for_recall(scope, kept_id, RecallPolicy::Strict, false);
    const auto kept_entities = db.find_entities(scope, "kept-entity");
    const auto orphan_entities = db.find_entities(scope, "orphan-entity");
    const auto orphan_two_remaining = db.find_entities(scope, "orphan-entity-two");
    const auto orphan_three_remaining = db.find_entities(scope, "orphan-entity-three");
    const auto links = db.links(scope, link.from, RecallPolicy::Strict, false, 10);
    const auto orphan_ids_remaining = count_sql(
        db.path(), "SELECT COUNT(*) FROM memory_entities WHERE id IN ('" + orphan.id + "','" +
                       orphan_two.id + "','" + orphan_three.id + "')");
    check(!old_remaining && kept_remaining && kept_entities.size() == 1 &&
              orphan_entities.empty() && orphan_two_remaining.size() == 1 &&
              orphan_three_remaining.size() == 1 && orphan_ids_remaining == 2 && links.empty() &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memories WHERE scope_key='" + scope_key(foreign_scope) + "'") == 1 &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memory_entities WHERE scope_key='" + scope_key(foreign_scope) + "'") == 1 &&
              count_sql(db.path(), "SELECT COUNT(*) FROM memory_entity_aliases WHERE entity_id='" + foreign_entity.id + "'") == 2 &&
              db.export_state(foreign_scope).dump() == foreign_before,
          "retention did not enforce scoped graph cleanup");
    std::cout << "memory maintenance probe passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
