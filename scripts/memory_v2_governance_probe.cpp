#include "qt/memory/database.hpp"
#include "qt/memory/governance_service.hpp"
#include "qt/memory/memory_service.hpp"
#include "qt/memory/review_service.hpp"
#include "qt/memory/scope.hpp"

#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
using namespace qt::memory;

void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function function, const char* message) {
  bool rejected = false;
  try { function(); } catch (const std::exception&) { rejected = true; }
  check(rejected, message);
}

void sql(const std::filesystem::path& path, const char* statement) {
  sqlite3* db = nullptr;
  check(sqlite3_open(path.c_str(), &db) == SQLITE_OK, "open sqlite probe connection");
  char* error = nullptr;
  const int result = sqlite3_exec(db, statement, nullptr, nullptr, &error);
  const std::string diagnostic = error ? error : "sqlite statement failed";
  sqlite3_free(error);
  sqlite3_close(db);
  if (result != SQLITE_OK) throw std::runtime_error(diagnostic);
}

MemoryScope scope(std::string repository) {
  MemoryScope value;
  value.kind = ScopeKind::Repository;
  value.user = "governance-probe";
  value.project = "qt";
  value.repository = std::move(repository);
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("requires probe directory");
    const std::filesystem::path home = argv[1];
    const auto exact = scope("repo-a");
    const auto other = scope("repo-b");
    SQLiteMemoryDatabase database(home);
    MemoryService memories(database);
    GovernanceService governance(memories, database, database);

    MemoryWrite candidate;
    candidate.content = "candidate governance memory";
    candidate.status = MemoryStatus::Candidate;
    candidate.provenance.created_by = "probe";
    const auto subject_id = memories.remember(exact, candidate);
    MemoryWrite replacement;
    replacement.content = "replacement governance memory";
    replacement.status = MemoryStatus::Active;
    const auto replacement_id = memories.remember(exact, replacement);
    MemoryWrite foreign;
    foreign.content = "foreign scope memory";
    foreign.status = MemoryStatus::Active;
    const auto foreign_id = memories.remember(other, foreign);

    rejects([&] { governance.validate(exact, subject_id, "probe", "missing evidence", ""); },
            "validation accepted empty evidence");
    const auto validated = governance.validate(
        exact, subject_id, "probe", "independent evidence", "file:proof.md");
    check(validated.status == MemoryStatus::Validated, "validation did not commit");
    rejects([&] {
      governance.supersede(exact, replacement_id, foreign_id, "probe", "cross scope");
    }, "cross-scope replacement was accepted");

    MemoryPatch wrong_status_patch;
    wrong_status_patch.content = "wrong expected status";
    rejects([&] {
      (void)database.apply_governed_update(
          exact, subject_id, MemoryStatus::Candidate, wrong_status_patch,
          std::nullopt, AuditEventWrite{"probe", "probe.cas", "wrong status"});
    }, "wrong expected status CAS was accepted");

    MemoryWrite active_candidate;
    active_candidate.content = "atomic rollback candidate";
    active_candidate.status = MemoryStatus::Candidate;
    const auto rollback_id = memories.remember(exact, active_candidate);
    sql(database.path(),
        "CREATE TRIGGER governance_audit_failure BEFORE INSERT ON audit_events "
        "WHEN NEW.action='memory.lifecycle.active' BEGIN SELECT RAISE(ABORT,'forced audit failure'); END;");
    LifecycleRequest transition;
    transition.id = rollback_id;
    transition.status = MemoryStatus::Active;
    transition.actor = "probe";
    transition.reason = "forced rollback";
    rejects([&] { (void)governance.transition(exact, transition); },
            "audit failure did not abort atomic transition");
    sql(database.path(), "DROP TRIGGER governance_audit_failure;");
    check(database.get(exact, rollback_id).status == MemoryStatus::Candidate,
          "atomic rollback left lifecycle mutation");
    GraphQuery rollback_edges;
    rollback_edges.memory_id = rollback_id;
    check(database.query_edges(exact, rollback_edges).empty(),
          "atomic rollback left graph mutation");

    ReviewItemWrite review;
    review.kind = "general";
    review.subject_id = replacement_id;
    review.reason = "compensation probe";
    review.candidate_json = nlohmann::json{
        {"version", 1}, {"kind", "general"},
        {"content", "compensation probe"}, {"related_memory_ids", nlohmann::json::array()}}
                                   .dump();
    const auto review_id = database.add_review(exact, review);
    ReviewInspectionService inspection(memories, database, database, database);
    ReviewApplicationService application(inspection, database);
    ReviewAction compensation;
    compensation.kind = ReviewActionKind::Compensate;
    compensation.actor = "probe";
    compensation.note = "corrected content";
    compensation.target_id = replacement_id;
    compensation.human_authority = true;
    compensation.edit = ReviewEdit{};
    compensation.edit->content = "corrected content";
    compensation.edit->target_status = MemoryStatus::Validated;
    rejects([&] { (void)application.apply(exact, review_id, compensation); },
            "compensation bypassed lifecycle validation");
    compensation.edit->target_status.reset();
    compensation.edit->replacement_id = foreign_id;
    rejects([&] { (void)application.apply(exact, review_id, compensation); },
            "compensation bypassed replacement scope validation");

    std::cout << nlohmann::json{{"exact_scope", true}, {"evidence_required", true},
        {"status_cas", true}, {"atomic_rollback", true},
        {"compensation_governed", true}}.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
