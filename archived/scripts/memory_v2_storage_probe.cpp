#include "qt/memory/database.hpp"
#include "qt/memory/scope.hpp"
#include "qt/memory/recall_service.hpp"
#include "qt/memory/context_service.hpp"

#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <nlohmann/json.hpp>

namespace {
using namespace qt::memory;
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Action> void rejects(Action action, const char* message) {
  bool rejected = false;
  try { action(); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, message);
}
void sql(const std::filesystem::path& path, const char* command) {
  sqlite3* raw = nullptr;
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open probe connection");
  char* error = nullptr;
  const int result = sqlite3_exec(raw, command, nullptr, nullptr, &error);
  const std::string diagnostic = error ? error : "SQL failed";
  sqlite3_free(error);
  sqlite3_close(raw);
  if (result != SQLITE_OK) throw std::runtime_error(diagnostic);
}
RetrievalChunk chunk(const std::string& value) {
  RetrievalChunk result; result.text = value; return result;
}
struct JobMetadata {
  std::string state;
  std::string lease_token;
  std::int64_t lease_until = 0;
  std::size_t max_attempts = 0;
  std::string error;
};
JobMetadata job_metadata(const std::filesystem::path& path, const std::int64_t id) {
  sqlite3* raw = nullptr;
  check(sqlite3_open(path.c_str(), &raw) == SQLITE_OK, "open job metadata connection");
  sqlite3_stmt* statement = nullptr;
  check(sqlite3_prepare_v2(raw,
      "SELECT state,lease_token,lease_until,max_attempts,error FROM retrieval_jobs WHERE id=?;", -1,
      &statement, nullptr) == SQLITE_OK, "prepare job metadata query");
  check(sqlite3_bind_int64(statement, 1, id) == SQLITE_OK, "bind job metadata id");
  check(sqlite3_step(statement) == SQLITE_ROW, "read job metadata");
  JobMetadata result;
  result.state = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
  result.lease_token = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
  result.lease_until = sqlite3_column_int64(statement, 2);
  result.max_attempts = static_cast<std::size_t>(sqlite3_column_int64(statement, 3));
  result.error = reinterpret_cast<const char*>(sqlite3_column_text(statement, 4));
  sqlite3_finalize(statement);
  sqlite3_close(raw);
  return result;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("requires empty probe directory");
    const std::filesystem::path home = argv[1];
    MemoryScope scope;
    scope.kind = ScopeKind::Repository; scope.user = "v2-probe";
    scope.project = "qt"; scope.repository = "repo-a";
    auto other = scope; other.repository = "repo-b";
    std::string id;
    std::filesystem::path db_path;
    {
      SQLiteMemoryDatabase db(home);
      db_path = db.path();
      MemoryWrite write;
      write.content = "RecallService uses SQLite to persist governed memory.";
      write.status = MemoryStatus::Active;
      id = db.add(scope, write);
      check(db.retrieval_revision(scope, id) == 1, "initial revision");
      check(db.retrieval_index_status(scope).pending == 1, "atomic outbox insert");
      check(!db.claim_index_job(other), "job scope isolation");
      const auto first = db.claim_index_job(scope);
      check(first.has_value(), "initial lease");
      SQLiteMemoryDatabase concurrent(home);
      check(!concurrent.claim_index_job(scope), "exclusive lease");
      sql(db.path(), "UPDATE retrieval_jobs SET lease_until=0 WHERE state='leased';");
      const auto recovered = concurrent.claim_index_job(scope);
      check(recovered && recovered->id == first->id && recovered->lease_token != first->lease_token,
            "expired lease recovery");
      check(!db.publish_chunks(scope, *first, {chunk("obsolete worker")}), "old worker rejected");
      check(concurrent.publish_chunks(scope, *recovered, {chunk(write.content)}), "publish recovered job");
      check(db.retrieval_chunks(scope, "memory:" + id).size() == 1, "chunk read");
      check(db.retrieval_chunks(other, "memory:" + id).empty(), "chunk scope isolation");
      // A crash happens after the claim increment, before fail_index_job().
      // Reopening another handle repeatedly must consume the persisted default
      // policy and leave unrelated eligible work claimable.
      auto retry_scope = scope;
      retry_scope.repository = "retry-recovery";
      MemoryWrite crash_write;
      crash_write.content = "Bounded lease recovery must stop after the default ceiling.";
      crash_write.status = MemoryStatus::Active;
      const auto crash_id = db.add(retry_scope, crash_write);
      MemoryWrite independent_write;
      independent_write.content = "Independent indexing work remains available.";
      independent_write.status = MemoryStatus::Active;
      const auto independent_id = db.add(retry_scope, independent_write);
      std::optional<RetrievalIndexJob> previous_crash;
      std::int64_t crash_job_id = 0;
      for (std::size_t attempt = 1; attempt <= 5; ++attempt) {
        SQLiteMemoryDatabase restarted(home);
        const auto crashed = restarted.claim_index_job(retry_scope);
        check(crashed && crashed->owner_key == "memory:" + crash_id && crashed->attempts == attempt,
              "expired lease recovery did not retain the default retry bound");
        if (previous_crash) {
          check(!restarted.publish_chunks(retry_scope, *previous_crash, {chunk("stale lease")}),
                "reclaimed lease allowed a stale worker to publish");
        }
        crash_job_id = crashed->id;
        sql(db.path(), ("UPDATE retrieval_jobs SET lease_until=0 WHERE id=" +
                        std::to_string(crashed->id) + ";").c_str());
        previous_crash = crashed;
      }
      SQLiteMemoryDatabase recovery_after_crashes(home);
      const auto independent = recovery_after_crashes.claim_index_job(retry_scope);
      check(independent && independent->owner_key == "memory:" + independent_id,
            "exhausted crash recovery blocked independent work");
      const auto exhausted = job_metadata(db.path(), crash_job_id);
      check(exhausted.state == "failed" && exhausted.max_attempts == 5 &&
                exhausted.lease_token.empty() && exhausted.lease_until == 0 &&
                exhausted.error.find("retry limit reached") != std::string::npos,
            "expired default-policy job was not terminally exhausted");
      check(recovery_after_crashes.publish_chunks(retry_scope, *independent,
                                                  {chunk(independent_write.content)}),
            "independent job could not publish after crash recovery");
      MemoryWrite custom_write;
      custom_write.content = "Custom retry policy must survive a worker crash.";
      custom_write.status = MemoryStatus::Active;
      const auto custom_id = db.add(retry_scope, custom_write);
      const auto custom_first = db.claim_index_job(retry_scope);
      check(custom_first && custom_first->owner_key == "memory:" + custom_id,
            "custom retry job claim");
      check(db.fail_index_job(retry_scope, *custom_first, "transient encoder failure", 3),
            "custom retry policy persisted with failure");
      check(job_metadata(db.path(), custom_first->id).max_attempts == 3,
            "custom retry policy was not stored on the job");
      sql(db.path(), ("UPDATE retrieval_jobs SET next_attempt=0 WHERE id=" +
                      std::to_string(custom_first->id) + ";").c_str());
      for (std::size_t attempt = 2; attempt <= 3; ++attempt) {
        SQLiteMemoryDatabase restarted(home);
        const auto crashed = restarted.claim_index_job(retry_scope);
        check(crashed && crashed->id == custom_first->id && crashed->attempts == attempt,
              "persisted custom retry policy was not applied after reopen");
        sql(db.path(), ("UPDATE retrieval_jobs SET lease_until=0 WHERE id=" +
                        std::to_string(crashed->id) + ";").c_str());
      }
      SQLiteMemoryDatabase custom_recovery(home);
      check(!custom_recovery.claim_index_job(retry_scope), "custom retry ceiling allowed another claim");
      const auto custom_exhausted = job_metadata(db.path(), custom_first->id);
      check(custom_exhausted.state == "failed" && custom_exhausted.max_attempts == 3 &&
                custom_exhausted.lease_token.empty() && custom_exhausted.lease_until == 0,
            "custom retry ceiling did not survive crash recovery");
      DenseIndexConfig dense;
      dense.generation = "embedding-space-a"; dense.model_id = "probe/model";
      dense.model_fingerprint = "model-bytes-a"; dense.tokenizer_fingerprint = "tokenizer-a";
      dense.pooling = "mean"; dense.dimensions = 3;
      db.begin_dense_index(scope, dense);
      check(!db.activate_dense_index(scope, dense.generation), "incomplete index not activated");
      const auto dense_job = db.claim_index_job(scope);
      check(dense_job && dense_job->representation == "dense", "dense backfill job");
      const auto indexed = db.retrieval_chunks(scope, "memory:" + id);
      rejects([&] { db.publish_dense_vectors(scope, *dense_job, {{indexed[0].id, {1, 0}}}); },
              "dimension mismatch rejected");
      rejects([&] { db.publish_dense_vectors(scope, *dense_job,
          {{indexed[0].id, {std::numeric_limits<float>::quiet_NaN(), 0, 1}}}); },
              "non-finite embedding rejected");
      check(db.publish_dense_vectors(scope, *dense_job, {{indexed[0].id, {2, 0, 0}}}),
            "publish dense vectors");
      check(db.activate_dense_index(scope, dense.generation), "complete dense generation activated");
      const auto hits = db.search_dense(scope, dense.generation, {1, 0, 0}, RecallPolicy::Strict, false);
      check(hits.size() == 1 && hits[0].similarity > 0.999, "normalized exact cosine retrieval");
      check(db.search_dense(other, dense.generation, {1, 0, 0}, RecallPolicy::Strict, false).empty(),
            "dense scope isolation");
      auto incompatible = dense; incompatible.model_fingerprint = "different-model";
      rejects([&] { db.begin_dense_index(scope, incompatible); }, "mixed embedding space rejected");
      rejects([&] { db.publish_chunks(scope, *recovered, {}); }, "empty publication rejected");
      MemoryEntity entity; entity.kind = "project"; entity.name = "qt"; entity.aliases = {"QT"};
      const auto project = db.add_entity(scope, entity);
      const auto ambiguous = db.add_entity(scope, entity);
      check(project.id != ambiguous.id && db.find_entities(scope, "qt").size() == 2,
            "ambiguous aliases retained");
      check(db.find_entities(other, "qt").empty(), "entity scope isolation");
      MemoryAssertion assertion;
      assertion.memory_id = id; assertion.revision = 1; assertion.subject_id = project.id;
      assertion.predicate = "uses"; assertion.literal_json = "\"SQLite\"";
      assertion.positive = false;
      assertion.qualifiers_json = "{\"environment\":\"production\"}";
      assertion.evidence.push_back({"", "revision-1", "file:src/memory/recall_service.cpp", "source-fingerprint", "1:20", "probe"});
      db.add_assertion(scope, assertion);
      const auto recorded = db.assertion(scope, id);
      check(recorded.has_value(), "qualified assertion roundtrip");
      const auto historical_cutoff = recorded->recorded_at;
      rejects([&] { db.add_assertion(scope, assertion); }, "immutable assertion revision");
      auto branch = scope; branch.kind = ScopeKind::Branch; branch.branch = "topic";
      rejects([&] { db.add_assertion(branch, assertion); }, "exact-scope assertion write");
      sql(db.path(), "BEGIN IMMEDIATE; UPDATE memories SET content='uncommitted'; ROLLBACK;");
      check(db.retrieval_revision(scope, id) == 1 && db.retrieval_index_status(scope).complete == 2,
            "canonical rollback rolls back index invalidation");
      MemoryPatch validation; validation.status = MemoryStatus::Validated;
      db.update(scope, id, validation);
      check(db.assertion(scope, id) && db.assertion(scope, id)->revision == 2,
            "lifecycle transition preserves canonical assertion in new revision");
      RecalledMemory current_claim; current_claim.memory = db.get(scope, id);
      ContextService current_context(db, nullptr, &db, {}, &db);
      ContextOptions current_options; current_options.token_budget = 2000;
      const auto current_pack = current_context.build(scope, {current_claim}, current_options);
      check(current_pack.memory_ids.size() == 1 &&
                current_pack.markdown.find("\"polarity\":\"negative\"") != std::string::npos &&
                current_pack.markdown.find("\"revision\":2") != std::string::npos,
            "current assertion context must preserve qualifications after a lifecycle revision");
      const auto historical = db.assertion(scope, id, historical_cutoff);
      check(historical && historical->revision == 1 &&
                historical->historical_content == write.content &&
                historical->recorded_at <= historical_cutoff,
            "as-of assertion query did not retain the recorded revision snapshot");
      const auto historical_rows = db.query_assertions(
          scope, project.id, "uses", {}, historical_cutoff,
          RecallPolicy::Strict, false, 10U);
      check(historical_rows.size() == 1U && historical_rows.front().revision == 1 &&
                historical_rows.front().historical_content == write.content,
            "structured as-of query rebound to current memory content");
      MemoryPatch patch; patch.content = "Changed evidence for the recall architecture.";
      db.update(scope, id, patch);
      check(db.retrieval_revision(scope, id) == 3, "revision advances");
      check(db.retrieval_chunks(scope, "memory:" + id).empty(), "immediate invalidation");
      check(db.search_dense(scope, dense.generation, {1, 0, 0}, RecallPolicy::Strict, false).empty(),
            "edit removes stale vector immediately");
      check(!db.assertion(scope, id), "old assertion not rebound to new content");
      RecallService recall(db, db, db, db, db);
      RecallRequest historical_request;
      historical_request.query = "no lexical overlap";
      historical_request.policy = RecallPolicy::Strict;
      historical_request.structured.enabled = true;
      historical_request.structured.subject = project.id;
      historical_request.structured.predicate = "uses";
      historical_request.structured.as_of = historical_cutoff;
      const auto recalled_history = recall.recall(scope, historical_request);
      check(recalled_history.size() == 1 && recalled_history.front().historical &&
                recalled_history.front().memory.content == write.content,
            "production recall must recover historical prose without lexical overlap");
      ContextService context(db, nullptr, &db, {}, &db);
      ContextOptions context_options;
      context_options.policy = RecallPolicy::Strict;
      context_options.token_budget = 2000;
      const auto historical_context = context.build(scope, recalled_history, context_options);
      check(historical_context.memory_ids.size() == 1 &&
                historical_context.markdown.find(write.content) != std::string::npos &&
                historical_context.markdown.find("historical revision=1") != std::string::npos &&
                historical_context.markdown.find(*patch.content) == std::string::npos,
            "context must label and preserve historical prose rather than substitute current text");
      check(historical_context.markdown.find("\"polarity\":\"negative\"") != std::string::npos &&
                historical_context.markdown.find("\"environment\":\"production\"") != std::string::npos &&
                historical_context.markdown.find("revision-1") != std::string::npos &&
                historical_context.markdown.find("source-fingerprint") != std::string::npos,
            "structured context lost negative polarity, qualifications or revision evidence");
      auto compact_options = context_options;
      compact_options.include_evidence_links = false;
      compact_options.include_provenance = false;
      const auto mandatory_evidence = context.build(scope, recalled_history, compact_options);
      check(mandatory_evidence.markdown.find("revision-1") != std::string::npos,
            "assertion evidence cannot be detached from the claim by display options");
      compact_options.token_budget = 100;
      const auto insufficient = context.build(scope, recalled_history, compact_options);
      check(insufficient.memory_ids.empty() &&
                insufficient.markdown.find(write.content) == std::string::npos,
            "small budgets must omit the entire assertion rather than only its qualifications");
      auto later_request = historical_request;
      later_request.structured.as_of = std::chrono::system_clock::now() + std::chrono::seconds(1);
      check(recall.recall(scope, later_request).empty(),
            "removed assertions must not reappear at a newer cutoff");
      const auto updated = db.claim_index_job(scope);
      check(updated && updated->owner_revision == 3, "new revision job");
      check(!db.publish_chunks(scope, *recovered, {chunk("stale content")}), "stale revision publication rejected");
      rejects([&] { db.publish_chunks(scope, *updated, {chunk("valid"), chunk("")}); },
              "partial chunk publication rolls back");
      check(db.retrieval_chunks(scope, "memory:" + id).empty(), "publication atomicity");
      check(db.fail_index_job(scope, *updated, "encoder unavailable", 1), "failure persisted");
      check(db.retrieval_index_status(scope).failed == 1, "bounded retry exhausted");
      patch.status = MemoryStatus::Rejected;
      db.update(scope, id, patch);
      check(context.build(scope, recalled_history, context_options).memory_ids.empty(),
            "final context must reject historical guidance after current rejection");
      sql(db.path(), ("UPDATE memory_assertions SET evidence_json='"+
          std::string("{\"token\":\"probe-secret\"}")+
          "',historical_json='{\"content\":\"password=probe-secret\"}' "
          "WHERE memory_id='" + id + "';").c_str());
      sql(db.path(), ("UPDATE memory_entities SET name='password=probe-secret' WHERE id='" +
          project.id + "';").c_str());
      RedactionScrubPolicy scrub;
      scrub.batch_limit = 100U;
      scrub.dry_run = false;
      check(db.scrub_sensitive_data(scope, scrub).changed() != 0U,
            "sensitive structured records were not scrubbed");
      const auto scrubbed_export = db.export_state(scope).dump();
      check(scrubbed_export.find("probe-secret") == std::string::npos,
            "sensitive structured data survived scrub/export");
      const auto deleted_worker = db.claim_index_job(scope);
      check(deleted_worker.has_value(), "status revision queued");
      db.forget(scope, id);
      check(!db.publish_chunks(scope, *deleted_worker, {chunk("deleted")}), "deleted owner cannot resurrect");
      check(db.retrieval_index_status(scope).chunks == 0, "delete invalidates chunks");
      MemoryWrite captured;
      captured.content = "Decision: preserve C++ punctuation in captured memories.";
      captured.provenance.created_by = "probe";
      SessionEventWrite claim{"candidate_ingestion_claim", "probe", "{}", "candidate-a"};
      const auto captured_once = db.add_capture_candidate_once(scope, captured, claim);
      const auto captured_again = concurrent.add_capture_candidate_once(scope, captured, claim);
      check(captured_once.second && !captured_again.second && captured_once.first == captured_again.first,
            "capture receipt prevents duplicate effects across database handles");
      db.forget(scope, captured_once.first);
      check(!db.add_capture_candidate_once(scope, captured, claim).second,
            "capture replay does not resurrect forgotten memory");
      const auto before_failure = db.size(scope);
      sql(db.path(), "CREATE TRIGGER capture_failure BEFORE INSERT ON session_events "
          "WHEN NEW.source='force-failure' BEGIN SELECT RAISE(ABORT,'probe fault'); END;");
      claim.source = "force-failure"; claim.deduplication_key = "candidate-b";
      captured.content = "Decision: test atomic capture rollback.";
      bool rolled_back = false;
      try { (void)db.add_capture_candidate_once(scope, captured, claim); }
      catch (const std::exception&) { rolled_back = true; }
      check(rolled_back && db.size(scope) == before_failure, "capture effect rolls back when receipt publication fails");
      sql(db.path(), "DROP TRIGGER capture_failure;");
      claim.source = "probe";
      const auto retried_capture = db.add_capture_candidate_once(scope, captured, claim);
      check(retried_capture.second, "failed capture leaves no suppressing receipt");
      db.forget(scope, retried_capture.first);
      auto retention_scope = scope;
      retention_scope.repository = "repo-retention";
      MemoryWrite old_write;
      old_write.content = "Retention old memory";
      old_write.status = MemoryStatus::Validated;
      const auto old_id = db.add(retention_scope, old_write);
      MemoryEntity shared_entity;
      shared_entity.kind = "retention";
      shared_entity.name = "shared-retention-entity";
      shared_entity = db.add_entity(retention_scope, std::move(shared_entity));
      MemoryEntity orphan_entity;
      orphan_entity.kind = "retention";
      orphan_entity.name = "orphan-retention-entity";
      orphan_entity = db.add_entity(retention_scope, std::move(orphan_entity));
      MemoryAssertion retention_assertion;
      retention_assertion.memory_id = old_id;
      retention_assertion.revision = 1;
      retention_assertion.subject_id = shared_entity.id;
      retention_assertion.predicate = "retention-test";
      retention_assertion.literal_json = "\"old\"";
      retention_assertion.evidence.push_back({"", "retention-old",
          "session://retention", "retention-hash", "", "probe"});
      db.add_assertion(retention_scope, retention_assertion);
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      MemoryWrite new_write = old_write;
      new_write.content = "Retention kept memory";
      const auto new_id = db.add(retention_scope, new_write);
      retention_assertion.memory_id = new_id;
      retention_assertion.literal_json = "\"new\"";
      retention_assertion.evidence[0].source_revision = "retention-new";
      db.add_assertion(retention_scope, retention_assertion);
      CrossStoreLink retention_link;
      retention_link.from.kind = RetrievalOwnerKind::Memory;
      retention_link.from.id = old_id;
      retention_link.to.kind = RetrievalOwnerKind::Code;
      retention_link.to.id = "code-retention-test";
      retention_link.to.repository = "repo-retention";
      retention_link.to.worktree = "worktree-retention";
      retention_link.to.source_fingerprint = "fingerprint-retention";
      retention_link.to.snapshot_generation = 1;
      retention_link.relation = "supports";
      retention_link.evidence_id = "uri://retention-link-evidence";
      db.add_cross_store_link(retention_scope, retention_link);
      RetentionPolicy retention;
      retention.older_than = std::chrono::system_clock::now() + std::chrono::seconds(1);
      retention.canonical_memories = true;
      retention.batch_limit = 1;
      const auto dry_retention = db.enforce_retention(retention_scope, retention);
      check(dry_retention.canonical_memories == 1 &&
                db.get(retention_scope, old_id).id == old_id,
            "retention dry-run mutated or miscounted structured memory");
      retention.dry_run = false;
      (void)db.enforce_retention(retention_scope, retention);
      const auto old_remaining = db.find_for_recall(retention_scope, old_id, RecallPolicy::Strict, false);
      const auto new_remaining = db.find_for_recall(retention_scope, new_id, RecallPolicy::Strict, false);
      const auto shared_remaining = db.find_entities(retention_scope, "shared-retention-entity");
      const auto orphan_remaining = db.find_entities(retention_scope, "orphan-retention-entity");
      check(!old_remaining && new_remaining && shared_remaining.size() == 1U &&
                orphan_remaining.empty(),
            "retention did not preserve shared or remove orphan entities correctly");
      check(db.links(retention_scope, retention_link.from, RecallPolicy::Strict, false, 10U).empty(),
            "retention left a link to deleted memory");
      auto assertion_scope = scope; assertion_scope.repository = "assertion-paths";
      const auto entity_for = [&](const std::string& name) {
        MemoryEntity value; value.name = name; value.kind = "component";
        return db.add_entity(assertion_scope, value).id;
      };
      const auto a = entity_for("alpha"), b = entity_for("beta"),
                 c = entity_for("gamma"), d = entity_for("delta");
      const auto add_claim = [&](const std::string& text, const std::string& subject,
                                 const std::string& object, const std::string& predicate,
                                 bool positive) {
        MemoryWrite claim_write; claim_write.content = text;
        claim_write.status = MemoryStatus::Validated;
        const auto owner = db.add(assertion_scope, claim_write);
        MemoryAssertion claim; claim.memory_id = owner; claim.revision = 1;
        claim.subject_id = subject; claim.object_id = object; claim.predicate = predicate;
        claim.positive = positive; claim.qualifiers_json = "{\"environment\":\"production\"}";
        claim.evidence.push_back({"", "1", "test://assertion-evidence", "hash", "span", "probe"});
        db.add_assertion(assertion_scope, claim); return owner;
      };
      const auto first_claim = add_claim("amberanchor ownership", a, b, "uses", true);
      const auto negative_claim = add_claim("Deployment permission is denied", b, c, "supported_by", false);
      const auto forbidden_bridge = add_claim("Hidden onward relationship", c, d, "depends_on", true);
      RecallRequest path_request; path_request.query = "amberanchor";
      path_request.graph.enabled = true; path_request.graph.max_seeds = 1;
      path_request.graph.max_depth = 3;
      const auto path_results = recall.recall(assertion_scope, path_request);
      const auto negative_result = std::find_if(path_results.begin(), path_results.end(),
          [&](const auto& value) { return value.memory.id == negative_claim; });
      check(negative_result != path_results.end() && negative_result->assertion_path.size() == 2,
            "assertion graph must recover a nonlexical negative claim with its connecting owner");
      check(std::none_of(path_results.begin(), path_results.end(), [&](const auto& value) {
              return value.memory.id == forbidden_bridge;
            }), "negative assertion must not become a transitive bridge");
      auto reverse_request = path_request;
      reverse_request.query = "unmatchedtoken";
      reverse_request.structured.enabled = true;
      reverse_request.structured.object = c;
      reverse_request.graph.direction = GraphDirection::Incoming;
      const auto reverse_results = recall.recall(assertion_scope, reverse_request);
      check(reverse_results.size() == 1 && reverse_results.front().memory.id == negative_claim &&
                reverse_results.front().assertion_path.size() == 1 &&
                reverse_results.front().assertion_path.front().reverse,
            "incoming entity query must preserve a negative edge direction without traversing it");
      const auto path_context = context.build(assertion_scope, path_results, context_options);
      check(path_context.markdown.find("Deployment permission is denied") != std::string::npos &&
                path_context.markdown.find("amberanchor ownership") != std::string::npos &&
                path_context.markdown.find("\"polarity\":\"negative\"") != std::string::npos &&
                path_context.markdown.find("test://assertion-evidence") != std::string::npos,
            "assertion context must include all claims, polarity and evidence atomically");
      auto forged_path = path_results;
      for (auto& value : forged_path)
        if (value.memory.id == negative_claim) value.assertion_path.back().reverse = true;
      check(context.build(assertion_scope, forged_path, context_options).markdown.find(
                "Deployment permission is denied") == std::string::npos,
            "context accepted a forged assertion direction");
      forged_path = path_results;
      for (auto& value : forged_path)
        if (value.memory.id == negative_claim) value.assertion_path.pop_back();
      check(context.build(assertion_scope, forged_path, context_options).markdown.find(
                "Deployment permission is denied") == std::string::npos,
            "context accepted an assertion path that does not reach its attributed owner");
      auto bounded_request = path_request; bounded_request.graph.max_edges = 1;
      const auto bounded_results = recall.recall(assertion_scope, bounded_request);
      check(std::none_of(bounded_results.begin(), bounded_results.end(), [&](const auto& value) {
              return value.memory.id == negative_claim;
            }), "assertion traversal exceeded its shared edge budget");
      // A globally visible seed keeps its review in the GlobalUser scope.  A
      // repository-scope review query must not let that conflicted memory
      // enter graph traversal as a connecting bridge.
      auto global_scope = scope;
      global_scope.kind = ScopeKind::GlobalUser;
      global_scope.organization.clear();
      global_scope.project.clear();
      global_scope.repository.clear();
      global_scope.branch.clear();
      global_scope.task.clear();
      global_scope.agent.clear();
      global_scope.session.clear();
      global_scope.conversation.clear();
      MemoryWrite global_write;
      global_write.content = "Global bridge with unresolved contradictory guidance";
      global_write.status = MemoryStatus::Validated;
      const auto global_bridge = db.add(global_scope, global_write);
      MemoryWrite downstream_write;
      downstream_write.content = "Global downstream guidance";
      downstream_write.status = MemoryStatus::Validated;
      const auto global_downstream = db.add(global_scope, downstream_write);
      MemoryWrite distractor_write;
      distractor_write.content = "Unrelated global conflict distractor";
      distractor_write.status = MemoryStatus::Validated;
      const auto global_distractor = db.add(global_scope, distractor_write);
      distractor_write.content = "Second unrelated global conflict distractor";
      const auto global_distractor_peer = db.add(global_scope, distractor_write);
      const auto global_entity = [&](const std::string& name) {
        MemoryEntity entity; entity.name = name; entity.kind = "component";
        return db.add_entity(global_scope, entity).id;
      };
      const auto global_a = global_entity("global-alpha");
      const auto global_b = global_entity("global-beta");
      const auto global_c = global_entity("global-gamma");
      MemoryAssertion global_first;
      global_first.memory_id = global_bridge; global_first.revision = 1;
      global_first.subject_id = global_a; global_first.object_id = global_b;
      global_first.predicate = "depends_on"; global_first.positive = true;
      global_first.evidence.push_back({"", "1", "test://global-assertion", "hash", "span", "probe"});
      db.add_assertion(global_scope, global_first);
      MemoryAssertion global_second;
      global_second.memory_id = global_downstream; global_second.revision = 1;
      global_second.subject_id = global_b; global_second.object_id = global_c;
      global_second.predicate = "depends_on"; global_second.positive = true;
      global_second.evidence.push_back({"", "1", "test://global-assertion", "hash", "span", "probe"});
      db.add_assertion(global_scope, global_second);
      GraphEdge global_edge;
      global_edge.from_id = global_bridge;
      global_edge.to_id = global_downstream;
      global_edge.type = "depends_on";
      db.add_edge(global_scope, std::move(global_edge));
      RecallRequest global_positive_request;
      global_positive_request.query = "no lexical overlap";
      global_positive_request.policy = RecallPolicy::Strict;
      global_positive_request.include_global_user = true;
      global_positive_request.graph.enabled = true;
      global_positive_request.graph.seed_memory_ids = {global_bridge};
      global_positive_request.graph.max_depth = 2;
      global_positive_request.structured.enabled = true;
      global_positive_request.structured.subject = "global-alpha";
      const auto global_positive_results = recall.recall(global_scope, global_positive_request);
      check(std::any_of(global_positive_results.begin(), global_positive_results.end(),
                        [&](const auto& value) { return value.memory.id == global_downstream; }),
            "global graph positive control did not reach downstream memory");
      auto repository_positive_request = global_positive_request;
      repository_positive_request.policy = RecallPolicy::Broad;
      const auto repository_positive_results =
          recall.recall(assertion_scope, repository_positive_request);
      check(std::any_of(repository_positive_results.begin(), repository_positive_results.end(),
                        [&](const auto& value) { return value.memory.id == global_downstream; }),
            "repository graph positive control did not reach global downstream memory");
      check(recall.eligible_for_context(assertion_scope, global_bridge,
                                         RecallPolicy::Broad, true),
            "global bridge positive control failed final context authority");
      ReviewItemWrite global_conflict;
      global_conflict.kind = "conflict";
      global_conflict.subject_id = global_bridge;
      global_conflict.reason = "critical global conflict";
      global_conflict.candidate_json = nlohmann::json{
          {"memory_ids", nlohmann::json::array({global_bridge, first_claim})},
          {"severity", "critical"}, {"explanation", "conflicting global guidance"}}
          .dump();
      const auto target_conflict_id = db.add_review(global_scope, std::move(global_conflict));
      check(!recall.eligible_for_context(assertion_scope, global_bridge,
                                         RecallPolicy::Broad, true),
            "canonical global conflict must fail final context authority");
      // Keep the target conflict older than the query window.  Critical
      // reviews sort ahead of ordinary work, so this deliberately uses more
      // than the legacy 1000-row limit to prove that authority cannot depend
      // on a truncated conflict query.
      for (std::size_t index = 0; index < 1001U; ++index) {
        ReviewItemWrite overflow_conflict;
        overflow_conflict.kind = "conflict";
        overflow_conflict.priority = "urgent";
        overflow_conflict.subject_id = global_distractor;
        overflow_conflict.reason = "critical overflow conflict " + std::to_string(index);
        overflow_conflict.candidate_json = nlohmann::json{
            {"memory_ids", nlohmann::json::array({global_distractor, global_distractor_peer})},
            {"severity", "critical"}, {"explanation", "query window overflow"}}
            .dump();
        db.add_review(global_scope, std::move(overflow_conflict));
      }
      ReviewQuery bounded_conflicts;
      bounded_conflicts.statuses = {ReviewStatus::Pending, ReviewStatus::Deferred};
      bounded_conflicts.kinds = {"conflict"};
      bounded_conflicts.limit = 1000U;
      const auto bounded_conflict_rows = db.query_reviews(global_scope, bounded_conflicts);
      check(std::none_of(bounded_conflict_rows.begin(), bounded_conflict_rows.end(),
                         [&](const auto& row) { return row.id == target_conflict_id; }),
            "target conflict unexpectedly appeared inside bounded review query");
      check(std::all_of(bounded_conflict_rows.begin(), bounded_conflict_rows.end(),
                        [&](const auto& row) {
                          return row.subject_id != global_bridge &&
                                 row.candidate_json.find(global_bridge) == std::string::npos;
                        }),
            "overflow distractor review referenced the protected bridge");
      check(!recall.eligible_for_context(assertion_scope, global_bridge,
                                         RecallPolicy::Broad, true),
            "critical conflict beyond query window was treated as eligible");
      const auto conflicted_global_request = repository_positive_request;
      const auto conflicted_global_results =
          recall.recall(assertion_scope, conflicted_global_request);
      check(std::none_of(conflicted_global_results.begin(), conflicted_global_results.end(),
                         [&](const auto& value) { return value.memory.id == global_bridge; }),
            "graph traversal admitted a globally conflicted bridge before context validation");
      check(std::none_of(conflicted_global_results.begin(), conflicted_global_results.end(),
                         [&](const auto& value) { return value.memory.id == global_downstream; }),
            "graph traversal continued through a globally conflicted bridge");
      ContextService authoritative_context(db, nullptr, &db, {}, &db,
          [&](const MemoryScope& requested, const std::string& owner, std::int64_t,
              RecallPolicy policy, bool global) {
            return recall.eligible_for_context(requested, owner, policy, global);
          });
      SourceRecord evidence_source; evidence_source.id = "assertion-source";
      evidence_source.uri = "test://assertion-evidence"; evidence_source.hash = "changed";
      evidence_source.hash_status = "changed";
      SourceEvent evidence_event; evidence_event.action = "recheck";
      db.record_event(assertion_scope, evidence_source, evidence_event);
      check(recall.recall(assertion_scope, path_request).empty() &&
                authoritative_context.build(assertion_scope, path_results, context_options).memory_ids.empty(),
            "assertion evidence drift must block fresh traversal and captured context");
      evidence_source.hash = "hash"; evidence_source.hash_status = "current";
      db.record_event(assertion_scope, evidence_source, evidence_event);
      MemoryPatch changed_bridge; changed_bridge.content = "ownership changed";
      db.update(assertion_scope, first_claim, changed_bridge);
      check(context.build(assertion_scope, path_results, context_options).markdown.find(
                "Deployment permission is denied") == std::string::npos,
            "context accepted an assertion path after its connecting revision changed");
      // Keep one legacy record for a genuine schema-5-to-6 backfill test.
      id = db.add(scope, write);
      check(db.integrity_check() == "ok", "integrity before migration test");
    }
    sql(db_path, R"SQL(
DROP TRIGGER retrieval_memory_insert;
DROP TRIGGER retrieval_memory_update;
DROP TRIGGER retrieval_memory_delete;
DROP TRIGGER retrieval_chunk_insert;
DROP TRIGGER retrieval_chunk_delete;
DROP TABLE memory_assertions;
DROP TABLE memory_assertion_revisions;
DROP TABLE memory_entity_aliases;
DROP TABLE memory_entities;
DROP TABLE retrieval_vectors;
DROP TABLE retrieval_code_sources;
DROP TABLE retrieval_dense_indexes;
DROP TABLE retrieval_fts;
DROP TABLE retrieval_chunks;
DROP TABLE retrieval_jobs;
DROP TABLE retrieval_owners;
DROP TABLE retrieval_cross_store_links;
DROP TABLE capture_candidate_receipts;
DROP INDEX graph_edges_reverse_idx;
DELETE FROM memory_schema_migrations WHERE version>=6;
)SQL");
    {
      SQLiteMemoryDatabase migrated(home);
      check(migrated.schema_version() == 7 && migrated.retrieval_revision(scope, id) == 1,
            "schema 5 backfill preserves memory IDs");
      check(migrated.retrieval_index_status(scope).pending == 1, "backfill schedules exactly one job");
    }
    SQLiteMemoryDatabase reopened(home);
    check(reopened.retrieval_index_status(scope).pending == 1, "idempotent reopen/backfill");
    check(reopened.integrity_check() == "ok", "final SQLite integrity");
    std::cout << nlohmann::json{{"schema", reopened.schema_version()},
        {"lease_recovery", true}, {"stale_publication_rejected", true},
        {"transactional_invalidation", true}, {"scope_isolation", true},
        {"immutable_assertions", true}, {"legacy_backfill", true},
        {"historical_context_revalidation", true},
        {"dense_generation_validation", true}, {"atomic_capture_replay", true}}.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
