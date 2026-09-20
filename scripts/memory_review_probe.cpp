#include "qt/memory/database.hpp"
#include "qt/memory/recall_service.hpp"
#include "qt/memory/review.hpp"
#include "qt/memory/review_json.hpp"
#include "qt/memory/review_service.hpp"
#include "qt/memory/scope.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace qt::memory;

void require(const bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

MemoryScope scope_for(const std::string& repository,
                      const std::string& branch = "main") {
  MemoryScope scope;
  scope.kind = ScopeKind::Branch;
  scope.user = "review-probe-user";
  scope.project = "review-probe-project";
  scope.repository = repository;
  scope.repository_display = repository;
  scope.branch = branch;
  return scope;
}

MemoryScope scope_fixture(const ScopeKind kind, const std::string& label) {
  auto scope = scope_for("review-probe-scope-" + label);
  scope.kind = kind;
  scope.user += "-" + label;
  scope.project += "-" + label;
  scope.task = "task-" + label;
  scope.agent = "agent-" + label;
  scope.session = "session-" + label;
  scope.conversation = "conversation-" + label;
  if (kind != ScopeKind::Exact) {
    scope.task.clear();
    scope.agent.clear();
    scope.session.clear();
    scope.conversation.clear();
  }
  if (kind == ScopeKind::Repository || kind == ScopeKind::Project ||
      kind == ScopeKind::GlobalUser) {
    scope.branch.clear();
  }
  if (kind == ScopeKind::Project || kind == ScopeKind::GlobalUser) {
    scope.repository.clear();
    scope.repository_display.clear();
  }
  if (kind == ScopeKind::GlobalUser) {
    scope.project.clear();
    scope.organization.clear();
  }
  return scope;
}

MemoryWrite memory_fixture(const MemoryType type, const MemoryStatus status,
                           const std::string& label) {
  MemoryWrite write;
  write.type = type;
  write.status = status;
  write.source = "memory-review-probe";
  write.content = label + " governed memory content";
  write.tags = {"review:fixture", "fixture:" + label};
  write.confidence = status == MemoryStatus::Validated ? 0.95 : 0.45;
  write.quality_score = 0.8;
  write.provenance.source_type = SourceType::Manual;
  write.provenance.source_uri = "test://review/" + label;
  write.provenance.source_hash = "sha256:" + label;
  write.provenance.source_span = "fixture";
  write.provenance.created_by = "user:review-probe";
  write.provenance.evidence_strength = 0.8;
  return write;
}

ReviewItemWrite review_fixture(const std::string& kind,
                               const std::string& subject_id,
                               const std::string& priority,
                               Json payload = Json::object()) {
  ReviewItemWrite write;
  write.kind = kind;
  write.priority = priority;
  write.reason = "Characterize " + kind + " review behavior";
  write.subject_id = subject_id;
  write.provenance.source_type =
      kind.find("dream") == 0U ? SourceType::Dreaming : SourceType::Manual;
  write.provenance.source_uri = "test://review/kind/" + kind;
  write.provenance.source_hash = "sha256:" + kind;
  write.provenance.created_by = "review-probe";
  write.provenance.evidence_strength = 0.5;
  write.tags = {"review:fixture", "review:kind:" + kind};
  write.candidate_json = Json{{"version", 1},
                              {"kind", kind},
                              {"payload", std::move(payload)}}.dump();
  return write;
}

const ReviewItem& find_review(const std::vector<ReviewItem>& reviews,
                              const std::string& id) {
  const auto found = std::find_if(reviews.begin(), reviews.end(),
                                  [&](const ReviewItem& item) {
                                    return item.id == id;
                                  });
  require(found != reviews.end(), "review was not found in expected status");
  return *found;
}

void validate_contract_examples(const fs::path& path) {
  std::ifstream input(path);
  require(static_cast<bool>(input), "unable to open review contract fixtures");
  const Json fixtures = Json::parse(input);
  require(fixtures.value("version", 0) == 1,
          "review contract fixture version changed unexpectedly");
  for (const char* key : {"list", "detail", "preview", "result", "history",
                          "batch_result", "errors"}) {
    require(fixtures.contains(key), std::string("missing target fixture: ") + key);
  }
  const auto& detail = fixtures.at("detail");
  require(detail.at("review").at("candidate").contains("kind") &&
              detail.contains("subject") && detail.contains("sources") &&
              detail.contains("recall_impact"),
          "detail fixture is not a hydrated typed review");
  const auto& preview = fixtures.at("preview");
  require(preview.contains("effects") && preview.contains("will_not") &&
              preview.contains("required_authority") &&
              preview.contains("idempotency_token"),
          "preview fixture omits governed application fields");
  require(fixtures.at("result").contains("decision_persisted") &&
              fixtures.at("result").contains("application_status") &&
              fixtures.at("batch_result").value("partial_failure", false),
          "application result fixtures do not distinguish durable outcomes");
  std::set<std::string> error_codes;
  for (const auto& error : fixtures.at("errors")) {
    error_codes.insert(error.at("code").get<std::string>());
  }
  for (const char* code : {"review_not_found", "invalid_candidate",
                           "stale_preview", "terminal_review", "scope_denied"}) {
    require(error_codes.count(code) == 1U,
            std::string("missing stable review error fixture: ") + code);
  }
}

void write_legacy_malformed_candidate(const fs::path& database_path,
                                      const std::string& review_id) {
  sqlite3* handle = nullptr;
  require(sqlite3_open(database_path.c_str(), &handle) == SQLITE_OK,
          "unable to open review database for legacy fixture");
  sqlite3_stmt* statement = nullptr;
  const char* sql = "UPDATE review_items SET candidate_json=? WHERE id=?;";
  const int prepared = sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr);
  require(prepared == SQLITE_OK, "unable to prepare malformed legacy fixture");
  sqlite3_bind_text(statement, 1, "{legacy-malformed", -1, SQLITE_STATIC);
  sqlite3_bind_text(statement, 2, review_id.c_str(), -1, SQLITE_TRANSIENT);
  const int stepped = sqlite3_step(statement);
  sqlite3_finalize(statement);
  sqlite3_close(handle);
  require(stepped == SQLITE_DONE,
          "unable to persist malformed legacy candidate fixture");
}

ReviewDecision decision(const ReviewStatus status, const std::string& action,
                        const std::string& actor = "user:review-probe") {
  ReviewDecision result;
  result.status = status;
  result.decision = action;
  result.decided_by = actor;
  result.note = "review probe decision";
  result.application_status = "applied";
  return result;
}

void shift_review_creation_times(const fs::path& database_path,
                                 const std::string& review_scope_key,
                                 const std::int64_t seconds) {
  sqlite3* handle = nullptr;
  require(sqlite3_open(database_path.c_str(), &handle) == SQLITE_OK,
          "unable to open review database for age fixtures");
  sqlite3_stmt* statement = nullptr;
  const char* sql =
      "UPDATE review_items SET created_at=created_at-? WHERE scope_key=?;";
  require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK,
          "unable to prepare review age fixtures");
  sqlite3_bind_int64(statement, 1, seconds * 1000);
  sqlite3_bind_text(statement, 2, review_scope_key.c_str(), -1,
                    SQLITE_TRANSIENT);
  const int stepped = sqlite3_step(statement);
  sqlite3_finalize(statement);
  sqlite3_close(handle);
  require(stepped == SQLITE_DONE, "unable to persist review age fixtures");
}

void inject_legacy_secret(const fs::path& database_path,
                          const std::string& review_id) {
  sqlite3* handle = nullptr;
  require(sqlite3_open(database_path.c_str(), &handle) == SQLITE_OK,
          "unable to open review database for redaction fixture");
  sqlite3_stmt* statement = nullptr;
  const char* sql =
      "UPDATE review_items SET reason=?,candidate_json=? WHERE id=?;";
  require(sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) == SQLITE_OK,
          "unable to prepare review redaction fixture");
  sqlite3_bind_text(statement, 1, "password=legacy-review-secret", -1,
                    SQLITE_STATIC);
  sqlite3_bind_text(
      statement, 2,
      R"({"version":1,"kind":"candidate_memory","payload":{"content":"sk-probe_review_secret_1234567890"}})",
      -1, SQLITE_STATIC);
  sqlite3_bind_text(statement, 3, review_id.c_str(), -1, SQLITE_TRANSIENT);
  const int stepped = sqlite3_step(statement);
  sqlite3_finalize(statement);
  sqlite3_close(handle);
  require(stepped == SQLITE_DONE,
          "unable to persist review redaction fixture");
}

Json characterize(const fs::path& home, const fs::path& contract_path) {
  validate_contract_examples(contract_path);
  const auto scope = scope_for("review-probe-repository");
  const auto foreign_scope = scope_for("review-probe-foreign");
  SQLiteMemoryDatabase database(home);

  const std::vector<std::pair<ScopeKind, std::string>> scope_levels = {
      {ScopeKind::Exact, "exact"},
      {ScopeKind::Branch, "branch"},
      {ScopeKind::Repository, "repository"},
      {ScopeKind::Project, "project"},
      {ScopeKind::GlobalUser, "global-user"},
  };
  for (const auto& level : scope_levels) {
    const auto fixture_scope = scope_fixture(level.first, level.second);
    const auto fixture_subject = database.add(
        fixture_scope,
        memory_fixture(MemoryType::Observation, MemoryStatus::Candidate,
                       "scope-" + level.second));
    (void)database.add_review(
        fixture_scope,
        review_fixture("candidate_memory", fixture_subject, "normal"));
    require(database.reviews(fixture_scope, ReviewStatus::Pending, 10U).size() ==
                1U,
            "review scope-level fixture was not exactly isolated: " +
                level.second);
  }

  struct SubjectSpec {
    MemoryType type;
    MemoryStatus status;
    const char* label;
  };
  const std::vector<SubjectSpec> subjects = {
      {MemoryType::Observation, MemoryStatus::Candidate, "candidate"},
      {MemoryType::Dream, MemoryStatus::DreamArtifact, "dream"},
      {MemoryType::Research, MemoryStatus::Hypothesis, "research"},
      {MemoryType::Procedural, MemoryStatus::Active, "procedure"},
      {MemoryType::Constraint, MemoryStatus::Validated, "critical"},
      {MemoryType::Semantic, MemoryStatus::Deprecated, "stale"},
      {MemoryType::Preference, MemoryStatus::Rejected, "rejected"},
      {MemoryType::Observation, MemoryStatus::Superseded, "superseded"},
      {MemoryType::Observation, MemoryStatus::Expired, "expired"},
  };
  std::vector<std::string> subject_ids;
  for (const auto& subject : subjects) {
    subject_ids.push_back(database.add(
        scope, memory_fixture(subject.type, subject.status, subject.label)));
  }
  const auto foreign_id = database.add(
      foreign_scope,
      memory_fixture(MemoryType::Constraint, MemoryStatus::Validated,
                     "cross-scope"));

  struct KindSpec {
    const char* kind;
    std::size_t subject;
    const char* priority;
  };
  const std::vector<KindSpec> kinds = {
      {"candidate_memory", 0U, "normal"},
      {"dream_artifact", 1U, "normal"},
      {"dream_research_successor", 2U, "high"},
      {"semantic_consolidation_candidate", 0U, "normal"},
      {"compression_candidate", 0U, "low"},
      {"conflict", 4U, "urgent"},
      {"procedure_revision", 3U, "high"},
      {"failed_procedure", 3U, "urgent"},
      {"staleness", 5U, "normal"},
      {"provenance_repair", 0U, "high"},
  };
  std::vector<std::string> pending_ids;
  for (const auto& spec : kinds) {
    Json payload{{"source_ids", subject_ids},
                 {"proposed_content", std::string("Proposed ") + spec.kind}};
    if (std::string_view(spec.kind) == "dream_artifact") {
      payload["cycle_id"] = "cycle-alpha";
    }
    pending_ids.push_back(database.add_review(
        scope, review_fixture(spec.kind, subject_ids.at(spec.subject),
                              spec.priority, std::move(payload))));
  }
  const auto missing_subject_id = database.add_review(
      scope, review_fixture("candidate_memory", "mem_missing", "high"));
  const auto cross_scope_id = database.add_review(
      scope, review_fixture("conflict", subject_ids.at(4), "urgent",
                            Json{{"left_id", subject_ids.at(4)},
                                 {"right_id", foreign_id},
                                 {"severity", "critical"}}));
  const auto malformed_id = database.add_review(
      scope, review_fixture("candidate_memory", subject_ids.front(), "normal"));
  write_legacy_malformed_candidate(database.path(), malformed_id);

  const auto initial = database.reviews(scope, ReviewStatus::Pending, 100U);
  require(initial.size() == kinds.size() + 3U,
          "pending review fixtures were not durably listed");
  require(database.reviews(foreign_scope, ReviewStatus::Pending, 100U).empty(),
          "review list leaked across repository scope");
  require(database.reviews(scope, ReviewStatus::Pending, 2U).size() == 2U,
          "review list limit was not honored");
  ReviewQuery paginated;
  paginated.limit = 4U;
  const auto first_page = database.query_reviews(scope, paginated);
  paginated.offset = first_page.size();
  const auto second_page = database.query_reviews(scope, paginated);
  require(first_page.size() == 4U && second_page.size() == 4U &&
              std::none_of(first_page.begin(), first_page.end(),
                           [&](const ReviewItem& left) {
                             return std::any_of(
                                 second_page.begin(), second_page.end(),
                                 [&](const ReviewItem& right) {
                                   return left.id == right.id;
                                 });
                           }),
          "review pagination repeated an item across adjacent pages");
  const auto repeated_initial =
      database.reviews(scope, ReviewStatus::Pending, 100U);
  require(std::equal(initial.begin(), initial.end(), repeated_initial.begin(),
                     repeated_initial.end(),
                     [](const ReviewItem& left, const ReviewItem& right) {
                       return left.id == right.id;
                     }),
          "review listing order was not deterministic");
  require(find_review(initial, missing_subject_id).subject_id == "mem_missing",
          "missing-subject review fixture was discarded");
  require(find_review(initial, cross_scope_id).kind == "conflict",
          "cross-scope review fixture was discarded");
  require(find_review(initial, malformed_id).candidate_json ==
              "{legacy-malformed",
          "malformed legacy payload was silently rewritten");
  for (const auto& item : initial) {
    const auto typed = decode_review_candidate(item);
    require(to_string(typed.kind)[0] != '\0',
            "typed review decoder returned an empty kind");
    if (item.id == malformed_id) {
      require(typed.legacy && !typed.diagnostics.empty(),
              "malformed legacy review lacks a typed blocking diagnostic");
    }
  }
  ReviewQuery urgent_conflicts;
  urgent_conflicts.kinds = {"conflict"};
  urgent_conflicts.priorities = {"urgent"};
  urgent_conflicts.tags = {"review:fixture"};
  urgent_conflicts.limit = 1U;
  require(database.query_reviews(scope, urgent_conflicts).size() == 1U,
          "indexed review kind/priority/tag query did not honor its limit");
  ReviewQuery one_kind;
  one_kind.kinds = {"provenance_repair"};
  one_kind.limit = 10U;
  require(database.query_reviews(scope, one_kind).size() == 1U,
          "single-result review query did not preserve its item");
  ReviewQuery dreaming_source;
  dreaming_source.source_types = {SourceType::Dreaming};
  dreaming_source.limit = 100U;
  const auto dreaming_items = database.query_reviews(scope, dreaming_source);
  require(dreaming_items.size() == 2U,
          "review source-type filter did not isolate Dreaming reviews");
  ReviewQuery dream_cycle;
  dream_cycle.dream_cycles = {"cycle-alpha"};
  dream_cycle.limit = 100U;
  const auto cycle_items = database.query_reviews(scope, dream_cycle);
  require(cycle_items.size() == 1U &&
              cycle_items.front().kind == "dream_artifact" &&
              std::get<DreamArtifactCandidate>(
                  decode_review_candidate(cycle_items.front()).payload)
                      .cycle_id == "cycle-alpha",
          "Dreaming cycle filter did not select its exact cycle");
  ReviewQuery stale_only;
  stale_only.stale = true;
  stale_only.limit = 100U;
  require(database.query_reviews(scope, stale_only).size() == 1U,
          "stale review flag did not isolate staleness reviews");
  ReviewQuery conflicts_only;
  conflicts_only.conflict = true;
  conflicts_only.limit = 100U;
  require(database.query_reviews(scope, conflicts_only).size() == 2U,
          "conflict review flag did not isolate conflict reviews");
  database.record_access(scope, {subject_ids.at(4)});
  ProcedureFeedback failed_run;
  failed_run.procedure_id = subject_ids.at(3);
  failed_run.task_id = "review-ranking-probe";
  failed_run.outcome = ProcedureOutcome::Failure;
  failed_run.notes = "durable ranking failure signal";
  failed_run.evidence_uri = "test://review/procedure-failure";
  (void)database.add_feedback(scope, std::move(failed_run));
  ReviewQuery ranking_query;
  ranking_query.limit = 100U;
  const auto ranked_inputs = database.query_reviews(scope, ranking_query);
  require(find_review(ranked_inputs, pending_ids.at(5))
                  .queue_signals.critical_type &&
              find_review(ranked_inputs, pending_ids.at(5))
                  .queue_signals.access_count > 0U &&
              find_review(ranked_inputs, pending_ids.at(5))
                  .queue_signals.recent_context &&
              find_review(ranked_inputs, pending_ids.at(6))
                  .queue_signals.procedure_failures > 0U &&
              find_review(ranked_inputs, pending_ids.at(0))
                  .queue_signals.trust_risk > 0.0,
          "durable review ranking signals were not hydrated in bounded queries");
  const auto fixed_now = std::chrono::system_clock::now();
  for (const auto& item : ranked_inputs) {
    require(review_queue_score(item, fixed_now) ==
                review_queue_score(item, fixed_now),
            "pure review score changed for identical input and time");
  }
  auto stable_rank = ranked_inputs;
  std::stable_sort(stable_rank.begin(), stable_rank.end(),
                   [&](const ReviewItem& left, const ReviewItem& right) {
                     const double left_score = review_queue_score(left, fixed_now);
                     const double right_score = review_queue_score(right, fixed_now);
                     if (left_score != right_score) return left_score > right_score;
                     return left.id < right.id;
                   });
  const auto first_rank = stable_rank;
  std::stable_sort(stable_rank.begin(), stable_rank.end(),
                   [&](const ReviewItem& left, const ReviewItem& right) {
                     const double left_score = review_queue_score(left, fixed_now);
                     const double right_score = review_queue_score(right, fixed_now);
                     if (left_score != right_score) return left_score > right_score;
                     return left.id < right.id;
                   });
  require(std::equal(first_rank.begin(), first_rank.end(), stable_rank.begin(),
                     stable_rank.end(),
                     [](const ReviewItem& left, const ReviewItem& right) {
                       return left.id == right.id;
                     }),
          "review score ordering was not stable under ID tie-breaking");

  RecallService recall(database, database, database, database);
  RecallRequest recall_request;
  recall_request.query = "critical governed memory content";
  recall_request.policy = RecallPolicy::Strict;
  recall_request.result_limit = 100U;
  const auto recalled = recall.recall(scope, recall_request);
  const auto gated = std::find_if(
      recalled.begin(), recalled.end(), [&](const RecalledMemory& item) {
        return item.memory.id == subject_ids.at(4);
      });
  require(gated != recalled.end() && !gated->eligible_for_context &&
              std::any_of(gated->warnings.begin(), gated->warnings.end(),
                          [](const std::string& warning) {
                            return warning.find("critical unresolved conflict") !=
                                   std::string::npos;
                          }),
          "critical unresolved conflict was eligible for recall context");

  MemoryService memory_service(database);
  ReviewInspectionService review_inspection(
      memory_service, database, database, database,
      ReviewGovernancePolicy{true});
  ReviewApplicationService review_application(review_inspection, database);
  const auto secret_subject = database.add(
      scope, memory_fixture(MemoryType::Observation, MemoryStatus::Active,
                            "secret-surface"));
  auto secret_review_fixture =
      review_fixture("provenance_repair", secret_subject, "high");
  secret_review_fixture.provenance.source_hash = "sha256:secret-surface";
  const auto secret_review =
      database.add_review(scope, std::move(secret_review_fixture));
  ReviewAction secret_action;
  secret_action.kind = ReviewActionKind::AttachEvidence;
  secret_action.actor = "user:security-probe";
  secret_action.note = "password=review-note-secret";
  secret_action.evidence_uri =
      "https://evidence.invalid/item?secret=review-evidence-secret";
  secret_action.human_authority = true;
  auto secret_preview =
      review_inspection.preview(scope, secret_review, secret_action);
  const auto preview_text = review_preview_json(secret_preview).dump();
  require(preview_text.find("review-note-secret") == std::string::npos &&
              preview_text.find("review-evidence-secret") ==
                  std::string::npos &&
              preview_text.find("[REDACTED]") != std::string::npos,
          "review preview leaked a note or evidence secret");
  secret_action = secret_preview.action;
  secret_action.expected_revision = secret_preview.review_revision;
  secret_action.idempotency_key = secret_preview.idempotency_token;
  secret_action.human_authority = true;
  const auto secret_result =
      review_application.apply(scope, secret_review, secret_action);
  const auto receipt_text = review_application_json(secret_result).dump();
  const auto secret_history = database.review_history(
      scope, std::optional<std::string>(secret_review), 20U);
  std::string secret_history_text;
  for (const auto& record : secret_history)
    secret_history_text += review_action_record_json(record).dump();
  require(receipt_text.find("review-note-secret") == std::string::npos &&
              receipt_text.find("review-evidence-secret") ==
                  std::string::npos &&
              secret_history_text.find("review-note-secret") ==
                  std::string::npos &&
              secret_history_text.find("review-evidence-secret") ==
                  std::string::npos,
          "review receipt or typed history leaked a secret");

  const auto accepted_id = pending_ids.at(0);
  const auto rejected_id = pending_ids.at(1);
  const auto deferred_id = pending_ids.at(2);
  database.decide_review(scope, accepted_id,
                         decision(ReviewStatus::Accepted, "approve_active"));
  database.decide_review(scope, rejected_id,
                         decision(ReviewStatus::Rejected, "reject"));
  database.decide_review(scope, deferred_id,
                         decision(ReviewStatus::Deferred, "defer"));
  require(find_review(database.reviews(scope, ReviewStatus::Accepted, 100U),
                      accepted_id)
                  .decision == "approve_active" &&
              find_review(database.reviews(scope, ReviewStatus::Rejected, 100U),
                          rejected_id)
                  .status == ReviewStatus::Rejected &&
              find_review(database.reviews(scope, ReviewStatus::Deferred, 100U),
                          deferred_id)
                  .decision_note == "review probe decision",
          "terminal review status or decision metadata was not persisted");

  bool scope_denied = false;
  try {
    database.decide_review(foreign_scope, accepted_id,
                           decision(ReviewStatus::Rejected, "reject"));
  } catch (const std::invalid_argument&) {
    scope_denied = true;
  }
  require(scope_denied, "review decision crossed repository scope");

  bool terminal_overwrite_allowed = false;
  try {
    const auto overwritten = database.decide_review(
        scope, accepted_id,
        decision(ReviewStatus::Rejected, "second_terminal_decision"));
    terminal_overwrite_allowed = overwritten.status == ReviewStatus::Rejected;
  } catch (const std::invalid_argument&) {
  }

  const auto race_id = database.add_review(
      scope, review_fixture("candidate_memory", subject_ids.front(), "high"));
  SQLiteMemoryDatabase second_client(home);
  bool first_applied = false;
  bool second_applied = false;
  try {
    database.decide_review(scope, race_id,
                           decision(ReviewStatus::Accepted, "race_accept",
                                    "user:first"));
    first_applied = true;
  } catch (const std::exception&) {
  }
  try {
    second_client.decide_review(scope, race_id,
                                decision(ReviewStatus::Rejected, "race_reject",
                                         "user:second"));
    second_applied = true;
  } catch (const std::exception&) {
  }
  const bool concurrent_single_winner = first_applied != second_applied;

  const auto idempotent_id = database.add_review(
      scope, review_fixture("candidate_memory", subject_ids.front(), "normal"));
  auto idempotent_decision = decision(ReviewStatus::Rejected, "reject");
  idempotent_decision.action_key = "review-probe-idempotency-key";
  const auto first_idempotent =
      database.decide_review(scope, idempotent_id, idempotent_decision);
  const auto replayed =
      database.decide_review(scope, idempotent_id, idempotent_decision);
  const bool preview_idempotency_available =
      first_idempotent.revision == replayed.revision &&
      replayed.action_key == idempotent_decision.action_key;

  const auto scheduled_id = database.add_review(
      scope, review_fixture("staleness", subject_ids.at(5), "normal"));
  auto defer = decision(ReviewStatus::Deferred, "defer");
  defer.action_key = "review-probe-defer";
  defer.deferred_until =
      std::chrono::system_clock::now() + std::chrono::minutes(30);
  const auto deferred = database.decide_review(scope, scheduled_id, defer);
  ReviewQuery due;
  due.statuses = {ReviewStatus::Pending};
  due.deferred_due_before =
      std::chrono::system_clock::now() + std::chrono::hours(1);
  due.include_due_deferred = true;
  const auto due_items = database.query_reviews(scope, due);
  const bool scheduled_deferral_available =
      deferred.deferred_until.time_since_epoch().count() > 0 &&
      std::any_of(due_items.begin(), due_items.end(),
                  [&](const ReviewItem& item) { return item.id == scheduled_id; });
  auto reopen = decision(ReviewStatus::Pending, "reopen");
  reopen.expected_revision = deferred.revision;
  reopen.action_key = "review-probe-reopen";
  const auto reopened = database.decide_review(scope, scheduled_id, reopen);
  const bool reopen_available = reopened.status == ReviewStatus::Pending &&
                                reopened.revision == deferred.revision + 1U;

  const auto atomic_id = database.add_review(
      scope, review_fixture("candidate_memory", subject_ids.front(), "high"));
  auto atomic_decision = decision(ReviewStatus::Accepted, "approve_active");
  atomic_decision.action_key = "review-probe-atomic-create";
  ReviewAtomicMutation atomic_mutation;
  atomic_mutation.require_domain_mutation = true;
  ReviewMemoryMutation create;
  create.operation = ReviewMemoryMutation::Operation::Create;
  create.create = memory_fixture(MemoryType::Semantic, MemoryStatus::Hypothesis,
                                 "atomic-created");
  atomic_mutation.memories.push_back(create);
  ReviewMemoryMutation replace_original;
  replace_original.memory_id = subject_ids.front();
  replace_original.patch.status = MemoryStatus::Superseded;
  replace_original.patch.replacement_id = review_created_reference(0U);
  atomic_mutation.memories.push_back(std::move(replace_original));
  const auto size_before_atomic = database.size(scope);
  const auto atomic_result = database.apply_review_mutation(
      scope, atomic_id, atomic_decision, atomic_mutation);
  require(atomic_result.status == ReviewApplicationStatus::Applied &&
              atomic_result.decision_persisted &&
              atomic_result.domain_mutations_applied &&
              atomic_result.changed_ids.size() == 2U &&
              database.size(scope) == size_before_atomic + 1U,
          "atomic review application did not commit its advertised mutation");
  const auto replay_result = database.apply_review_mutation(
      scope, atomic_id, atomic_decision, atomic_mutation);
  const bool atomic_idempotent_replay =
      replay_result.idempotent_replay &&
      database.size(scope) == size_before_atomic + 1U;
  const auto complete_audit = database.audit_events(scope, 1000U);
  const auto audit_contract = std::find_if(
      complete_audit.begin(), complete_audit.end(),
      [&](const AuditEvent& event) {
        if (event.action != "review_action" ||
            event.metadata_json.find(atomic_id) == std::string::npos)
          return false;
        const auto metadata = Json::parse(event.metadata_json, nullptr, false);
        return metadata.is_object() &&
               metadata.value("reviewer", std::string{}) ==
                   "user:review-probe" &&
               metadata.value("originating_review", std::string{}) == atomic_id &&
               metadata.contains("before") && metadata.contains("after") &&
               metadata.contains("decision") && metadata.contains("reason") &&
               metadata.contains("evidence_uris") &&
               metadata.contains("replacement_ids") &&
               metadata.at("replacement_ids").is_array() &&
               metadata.at("replacement_ids").size() == 1U &&
               metadata.contains("created_ids") &&
               metadata.at("created_ids").is_array() &&
               metadata.at("created_ids").size() == 1U &&
               metadata.contains("changed_ids") &&
               metadata.at("changed_ids").is_array() &&
               metadata.at("changed_ids").size() == 2U;
      });
  require(audit_contract != complete_audit.end(),
          "review audit omitted governed before/after, identity, evidence, "
          "replacement, or changed-record fields");

  const auto rollback_id = database.add_review(
      scope, review_fixture("compression_candidate", subject_ids.at(5), "high"));
  auto rollback_decision = decision(ReviewStatus::Accepted, "merge");
  rollback_decision.action_key = "review-probe-atomic-rollback";
  ReviewAtomicMutation rollback_mutation;
  rollback_mutation.require_domain_mutation = true;
  ReviewMemoryMutation valid_patch;
  valid_patch.memory_id = subject_ids.at(5);
  valid_patch.patch.content = "this write must roll back";
  rollback_mutation.memories.push_back(valid_patch);
  ReviewMemoryMutation invalid_patch;
  invalid_patch.memory_id = "mem_missing_for_rollback";
  invalid_patch.patch.status = MemoryStatus::Deprecated;
  rollback_mutation.memories.push_back(invalid_patch);
  const auto before_rollback = database.get(scope, subject_ids.at(5));
  bool rollback_failed = false;
  try {
    (void)database.apply_review_mutation(scope, rollback_id, rollback_decision,
                                         rollback_mutation);
  } catch (const std::exception&) {
    rollback_failed = true;
  }
  const auto after_rollback = database.get(scope, subject_ids.at(5));
  const bool atomic_rollback =
      rollback_failed && before_rollback.content == after_rollback.content &&
      database.review(scope, rollback_id).status == ReviewStatus::Pending;

  AuditEventWrite audit;
  audit.actor = "user:review-probe";
  audit.action = "review_action_fixture";
  audit.reason = "Characterize durable review history";
  audit.before_id = race_id;
  audit.after_id = race_id;
  audit.metadata_json = Json{{"review_id", race_id},
                             {"action", "race_winner"}}.dump();
  const auto audit_id = database.record_audit(scope, std::move(audit));
  const auto history = database.audit_events(scope, 100U);
  require(std::any_of(history.begin(), history.end(), [&](const AuditEvent& item) {
            return item.id == audit_id && item.before_id == race_id;
          }),
          "review-linked audit history did not persist");
  const bool review_history_audited =
      std::any_of(history.begin(), history.end(), [&](const AuditEvent& item) {
        return item.action == "review_action" &&
               item.metadata_json.find(scheduled_id) != std::string::npos;
      }) &&
      std::any_of(history.begin(), history.end(), [&](const AuditEvent& item) {
        return item.action == "review_reopen" &&
               item.metadata_json.find(scheduled_id) != std::string::npos;
      });

  database.decide_review(
      scope, pending_ids.at(3),
      decision(ReviewStatus::Accepted, "merged"));
  database.decide_review(
      scope, pending_ids.at(4),
      decision(ReviewStatus::Accepted, "superseded"));
  database.decide_review(
      scope, pending_ids.at(5),
      decision(ReviewStatus::Accepted, "approved_as_validated"));
  database.decide_review(
      scope, pending_ids.at(6),
      decision(ReviewStatus::Accepted,
               "edited_and_approved_as_hypothesis"));
  auto failed_application =
      decision(ReviewStatus::Rejected, "application_failed");
  failed_application.application_status = "failed";
  failed_application.application_error = "fixture application failure";
  database.decide_review(scope, pending_ids.at(7), failed_application);

  AuditEventWrite batch_audit;
  batch_audit.actor = "user:review-probe";
  batch_audit.action = "review_batch";
  batch_audit.reason = "Characterize durable batch statistics";
  batch_audit.metadata_json =
      Json{{"batch_size", 4U},
           {"applied_count", 3U},
           {"failed_count", 1U},
           {"partial_failure", true},
           {"retry_count", 2U},
           {"idempotent_replay_count", 2U},
           {"compensation_count", 1U},
           {"application_error_count", 1U}}
          .dump();
  (void)database.record_audit(scope, std::move(batch_audit));
  shift_review_creation_times(database.path(), scope_key(scope), 120);

  const auto statistics = database.review_statistics(scope);
  require(statistics.pending > 0U && statistics.reviewed >= 10U &&
              statistics.rejected > 0U && statistics.deferred > 0U &&
              statistics.reopened > 0U &&
              statistics.by_kind.count("dream_artifact") == 1U,
          "durable review statistics did not reconcile fixture outcomes");
  require(statistics.merged == 1U && statistics.superseded == 1U &&
              statistics.validated == 1U &&
              statistics.edited_and_approved == 1U &&
              statistics.application_errors == 1U &&
              statistics.by_outcome.at("merged") == 1U &&
              statistics.by_application_status.at("failed") == 1U,
          "review outcome or application statistics were collapsed");
  const auto pending_risk_total = std::accumulate(
      statistics.pending_by_risk_class.begin(),
      statistics.pending_by_risk_class.end(), std::size_t{0U},
      [](const std::size_t total, const auto& item) {
        return total + item.second;
      });
  require(statistics.pending_by_scope.at(scope_key(scope)) ==
                  statistics.pending &&
              pending_risk_total == statistics.pending &&
              !statistics.oldest_pending_age_by_risk_class_seconds.empty() &&
              !statistics.median_pending_age_by_risk_class_seconds.empty() &&
              statistics.oldest_pending_age_by_scope_seconds.at(
                  scope_key(scope)) >= 119 &&
              statistics.time_to_first_decision_p50_seconds >= 119 &&
              statistics.time_to_first_decision_p95_seconds >= 119 &&
              statistics.review_duration_p50_seconds >= 119 &&
              statistics.review_duration_p95_seconds >= 119,
          "review age, risk, scope, or duration statistics are incomplete");
  require(statistics.batch_runs == 1U && statistics.batch_items == 4U &&
              statistics.batch_applied_items == 3U &&
              statistics.batch_failed_items == 1U &&
              statistics.batch_partial_failures == 1U &&
              statistics.batch_retries == 2U &&
              statistics.batch_idempotent_replays == 2U &&
              statistics.batch_compensations == 1U &&
              statistics.batch_application_errors == 1U,
          "durable batch statistics did not preserve all outcomes");

  ReviewViewState view_state;
  view_state.query.kinds = {"conflict"};
  view_state.query.priorities = {"urgent"};
  view_state.query.tags = {"review:fixture"};
  view_state.query.oldest_first = true;
  view_state.selected_review_id = cross_scope_id;
  view_state.queue_offset = 1U;
  database.save_review_view_state(scope, view_state);

  const auto redaction_id = database.add_review(
      scope, review_fixture("candidate_memory", subject_ids.front(), "normal"));
  inject_legacy_secret(database.path(), redaction_id);
  RedactionScrubPolicy redaction;
  redaction.dry_run = false;
  const auto redaction_report = database.scrub_sensitive_data(scope, redaction);
  require(redaction_report.changed() > 0U,
          "review redaction fixture was not scrubbed");

  const Json exported = database.export_state(scope);
  require(exported.contains("review_items") &&
              exported.at("review_items").size() >= initial.size() &&
              exported.contains("review_actions") &&
              !exported.at("review_actions").empty() &&
              exported.contains("review_view_state") &&
              exported.at("review_view_state").is_object(),
          "memory export omitted review fixtures");
  const auto exported_text = exported.dump();
  require(exported_text.find("legacy-review-secret") == std::string::npos &&
              exported_text.find("sk-probe_review_secret") ==
                  std::string::npos &&
              exported_text.find("review-note-secret") == std::string::npos &&
              exported_text.find("review-evidence-secret") ==
                  std::string::npos,
          "review export retained a scrubbed secret");
  require(database.integrity_check() == "ok",
          "review database failed integrity check before restart");

  const auto backup_path = home / "review-backup.json";
  {
    std::ofstream backup(backup_path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(backup), "unable to create review backup");
    backup << exported.dump();
    require(static_cast<bool>(backup), "unable to write review backup");
  }
  Json restored_export;
  {
    std::ifstream backup(backup_path, std::ios::binary);
    require(static_cast<bool>(backup), "unable to reopen review backup");
    backup >> restored_export;
  }
  require(restored_export == exported,
          "review backup file did not preserve its export document");

  const auto imported_home = home / "imported";
  fs::create_directories(imported_home);
  const auto imported_scope = scope_for("review-probe-imported");
  {
    SQLiteMemoryDatabase imported(imported_home);
    imported.import_state(imported_scope, restored_export, true);
    for (const auto status : {ReviewStatus::Pending, ReviewStatus::Accepted,
                              ReviewStatus::Rejected,
                              ReviewStatus::Deferred}) {
      require(imported.reviews(imported_scope, status, 1000U).size() ==
                  database.reviews(scope, status, 1000U).size(),
              "review import changed a durable status population");
    }
    require(imported.review_history(imported_scope, std::nullopt, 1000U).size() ==
                database.review_history(scope, std::nullopt, 1000U).size(),
            "review import omitted action history");
    const auto imported_view = imported.load_review_view_state(imported_scope);
    require(imported_view && imported_view->query.kinds == view_state.query.kinds &&
                imported_view->query.priorities ==
                    view_state.query.priorities &&
                imported_view->query.tags == view_state.query.tags &&
                imported_view->query.oldest_first &&
                imported_view->queue_offset == view_state.queue_offset &&
                !imported_view->selected_review_id.empty() &&
                imported.review(imported_scope,
                                imported_view->selected_review_id)
                        .kind == "conflict",
            "review import omitted persisted Inbox view state");
    const auto imported_actions =
        imported.review_history(imported_scope, std::nullopt, 1000U);
    require(std::all_of(
                imported_actions.begin(), imported_actions.end(),
                [&](const ReviewActionRecord& action) {
                  const auto result =
                      Json::parse(action.result_json, nullptr, false);
                  if (!result.is_object()) return true;
                  for (const char* field : {"created_ids", "replacement_ids"}) {
                    for (const auto& id : result.value(
                             field, std::vector<std::string>{})) {
                      try {
                        (void)imported.get(imported_scope, id);
                      } catch (const std::exception&) {
                        return false;
                      }
                    }
                  }
                  return true;
                }),
            "review action result IDs were not remapped during restore");
    require(imported.integrity_check() == "ok",
            "imported review database failed integrity check");
  }
  {
    SQLiteMemoryDatabase restarted_import(imported_home);
    require(!restarted_import
                 .reviews(imported_scope, ReviewStatus::Pending, 1000U)
                 .empty() &&
                restarted_import.load_review_view_state(imported_scope)
                    .has_value() &&
                restarted_import.integrity_check() == "ok",
            "review import did not survive database restart");
  }

  const auto pending_before_retention =
      database.reviews(scope, ReviewStatus::Pending, 1000U).size();
  const auto actions_before_retention =
      database.review_history(scope, std::nullopt, 1000U).size();
  RetentionPolicy retention;
  retention.older_than =
      std::chrono::system_clock::now() + std::chrono::hours(24);
  retention.dry_run = false;
  (void)database.enforce_retention(scope, retention);
  require(database.reviews(scope, ReviewStatus::Pending, 1000U).size() ==
                  pending_before_retention &&
              database.review_history(scope, std::nullopt, 1000U).size() ==
                  actions_before_retention,
          "retention silently discarded governed review state");

  SQLiteMemoryDatabase restarted(home);
  require(restarted.integrity_check() == "ok" &&
              restarted.review_statistics(scope).reviewed ==
                  database.review_statistics(scope).reviewed &&
              restarted.load_review_view_state(scope).has_value(),
          "durable review state did not reconcile after restart");

  // The Review Inbox lists the exact scope plus its canonical durable scope.
  // Reported statistics must reconcile with that listing instead of reading the
  // turn scope alone.
  bool review_statistics_reconciled = false;
  {
    auto turn_scope = scope;
    turn_scope.kind = ScopeKind::Exact;
    turn_scope.task = "turn-task";
    turn_scope.agent = "turn-agent";
    turn_scope.session = "turn-session";
    turn_scope.conversation = "turn-conversation";
    const auto durable_scope = durable_memory_scope(turn_scope);
    const auto turn_only = review_inspection.statistics(turn_scope);
    const auto durable_only = review_inspection.statistics(durable_scope);
    const auto merged =
        review_inspection.statistics_including_durable(turn_scope);
    review_statistics_reconciled =
        turn_only.pending == 0U && durable_only.pending > 0U &&
        merged.pending == durable_only.pending &&
        merged.pending_by_kind == durable_only.pending_by_kind &&
        merged.dreaming_pending == durable_only.dreaming_pending;
    require(review_statistics_reconciled,
            "review statistics did not reconcile the durable review scope");
  }

  // Reviewing a dream artifact promotes it to a hypothesis under the default
  // review_only policy; only the explicit `disabled` policy blocks promotion.
  {
    auto dream_write = memory_fixture(MemoryType::Dream,
                                      MemoryStatus::DreamArtifact,
                                      "review-promotion-dream");
    dream_write.provenance.source_type = SourceType::Dreaming;
    dream_write.provenance.source_uri = "memory://dreaming/cycle-promotion";
    const auto dream_subject = database.add(scope, std::move(dream_write));
    auto dream_review_write = review_fixture(
        "dream_artifact", dream_subject, "normal",
        Json{{"cycle_id", "cycle-promotion"},
             {"source_ids", {subject_ids.front()}}});
    dream_review_write.provenance.source_hash =
        database.get(scope, dream_subject).provenance.source_hash;
    const auto dream_review =
        database.add_review(scope, std::move(dream_review_write));
    ReviewAction approve;
    approve.kind = ReviewActionKind::Approve;
    approve.actor = "user:review-probe";
    approve.note = "Promote the reviewed dream to an unvalidated hypothesis.";
    approve.human_authority = true;
    const auto preview =
        review_inspection.preview(scope, dream_review, approve);
    require(preview.applicable,
            "default review policy did not allow dream promotion");
    auto applied = approve;
    applied.expected_revision = preview.review_revision;
    applied.idempotency_key = preview.idempotency_token;
    const auto result =
        review_application.apply(scope, dream_review, applied);
    const auto promoted = database.get(scope, dream_subject);
    require(result.status == ReviewApplicationStatus::Applied &&
                result.domain_mutations_applied &&
                promoted.status == MemoryStatus::Hypothesis &&
                promoted.provenance.source_type == SourceType::Dreaming,
            "reviewed dream artifact was not promoted to hypothesis");
  }
  {
    auto blocked_write = memory_fixture(MemoryType::Dream,
                                        MemoryStatus::DreamArtifact,
                                        "disabled-promotion-dream");
    blocked_write.provenance.source_type = SourceType::Dreaming;
    blocked_write.provenance.source_uri = "memory://dreaming/cycle-disabled";
    const auto blocked_subject = database.add(scope, std::move(blocked_write));
    auto blocked_review_write = review_fixture(
        "dream_artifact", blocked_subject, "normal",
        Json{{"cycle_id", "cycle-disabled"},
             {"source_ids", {subject_ids.front()}}});
    blocked_review_write.provenance.source_hash =
        database.get(scope, blocked_subject).provenance.source_hash;
    const auto blocked_review =
        database.add_review(scope, std::move(blocked_review_write));
    ReviewInspectionService disabled_inspection(
        memory_service, database, database, database,
        ReviewGovernancePolicy{false});
    ReviewAction approve;
    approve.kind = ReviewActionKind::Approve;
    approve.actor = "user:review-probe";
    approve.human_authority = true;
    const auto preview =
        disabled_inspection.preview(scope, blocked_review, approve);
    require(!preview.applicable,
            "disabled promotion policy still allowed dream promotion");
    bool blocked = false;
    try {
      ReviewApplicationService disabled_application(disabled_inspection,
                                                    database);
      auto applied = approve;
      applied.expected_revision = preview.review_revision;
      applied.idempotency_key = preview.idempotency_token;
      (void)disabled_application.apply(scope, blocked_review, applied);
    } catch (const std::invalid_argument&) {
      blocked = true;
    }
    require(blocked && database.get(scope, blocked_subject).status ==
                           MemoryStatus::DreamArtifact,
            "disabled promotion policy mutated the dream artifact");
  }

  Json report = {
      {"schema_version", database.schema_version()},
      {"fixture_kinds", Json::array()},
      {"fixture_statuses", {"pending", "accepted", "rejected", "deferred"}},
      {"scope_isolation", scope_denied},
      {"malformed_legacy_payload_readable", true},
      {"missing_subject_readable", true},
      {"cross_scope_reference_readable_but_unhydrated", true},
      {"terminal_overwrite_allowed", terminal_overwrite_allowed},
      {"concurrent_single_winner", concurrent_single_winner},
      {"scheduled_deferral_available", scheduled_deferral_available},
      {"reopen_available", reopen_available},
      {"typed_history_available", review_history_audited},
      {"preview_idempotency_available", preview_idempotency_available},
      {"atomic_idempotent_replay", atomic_idempotent_replay},
      {"atomic_rollback", atomic_rollback},
      {"audit_completeness", true},
      {"queue_filters_complete", true},
      {"empty_single_mixed_pagination_complete", true},
      {"stable_score_ordering_complete", true},
      {"all_candidate_kinds_trust_scopes_complete", true},
      {"critical_conflict_recall_gated", true},
      {"durable_ranking_signals_complete", true},
      {"review_statistics_reconciled", review_statistics_reconciled},
      {"review_statistics_age_risk_duration_complete", true},
      {"review_statistics_outcomes_complete", true},
      {"review_statistics_batches_complete", true},
      {"export_import_redaction_integrity_restart_complete", true},
      {"retention_preserves_governance_complete", true},
      {"secret_surface_redaction_complete", true},
      {"target_contract_examples_valid", true},
  };
  for (const auto& spec : kinds) report["fixture_kinds"].push_back(spec.kind);
  return report;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      throw std::invalid_argument(
          "usage: memory_review_probe <runtime-dir> <contract-fixtures.json>");
    }
    const fs::path home(argv[1]);
    fs::create_directories(home);
    const Json report = characterize(home, fs::path(argv[2]));
    std::cout << report.dump() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "memory review probe failed: " << error.what() << '\n';
    return 1;
  }
}
