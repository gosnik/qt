#pragma once

#include "qt/memory/models.hpp"

#include <nlohmann/json_fwd.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace qt::memory {

enum class ReviewCandidateKind {
  DreamArtifact,
  DreamResearchSuccessor,
  SemanticConsolidation,
  Compression,
  Conflict,
  ProcedureRevision,
  General,
  Staleness,
  ProvenanceRepair,
};

enum class ReviewActionKind {
  Approve,
  EditAndApprove,
  Reject,
  Defer,
  Reopen,
  Relate,
  Merge,
  Supersede,
  MarkContradiction,
  KeepBoth,
  Retain,
  NarrowScope,
  Revalidate,
  Deprecate,
  AttachEvidence,
  Compensate,
};

enum class ReviewApplicationStatus {
  NotStarted,
  Applied,
  NoOp,
  Failed,
  PartiallyApplied,
  Compensated,
};

const char* to_string(ReviewCandidateKind value) noexcept;
const char* to_string(ReviewActionKind value) noexcept;
const char* to_string(ReviewApplicationStatus value) noexcept;
ReviewCandidateKind review_candidate_kind_from_string(const std::string& value);
ReviewActionKind review_action_kind_from_string(const std::string& value);
ReviewApplicationStatus review_application_status_from_string(
    const std::string& value);
double review_queue_score(const ReviewItem& item, TimePoint now);

struct DreamArtifactCandidate {
  std::string artifact_id;
  std::string content;
  std::vector<std::string> source_memory_ids;
  std::string cycle_id;
  std::string model;
  bool research_required = false;
};

struct DreamResearchCandidate {
  std::string dream_id;
  std::string research_memory_id;
  std::string evidence_uri;
  std::vector<std::string> source_memory_ids;
};

struct ConsolidationCandidate {
  std::vector<std::string> memory_ids;
  std::string canonical_content;
  std::string proposed_relationship;
};

struct ConflictCandidate {
  std::vector<std::string> memory_ids;
  std::string severity = "normal";
  std::string explanation;
};

struct ProcedureRevisionCandidate {
  std::string current_memory_id;
  std::string proposed_content;
  std::vector<std::string> evidence_ids;
};

struct GeneralReviewCandidate {
  std::string content;
  std::optional<MemoryType> type;
  std::optional<MemoryStatus> target_status;
  std::vector<std::string> tags;
  std::vector<std::string> related_memory_ids;
  std::string reason;
};

using ReviewCandidatePayload =
    std::variant<DreamArtifactCandidate, DreamResearchCandidate,
                 ConsolidationCandidate, ConflictCandidate,
                 ProcedureRevisionCandidate, GeneralReviewCandidate>;

struct ReviewCandidate {
  std::uint32_t version = 1U;
  ReviewCandidateKind kind = ReviewCandidateKind::General;
  ReviewCandidatePayload payload = GeneralReviewCandidate{};
  bool legacy = false;
  std::vector<std::string> diagnostics;
};

struct ReviewQuery {
  enum class ScopeSelector { All, Exact, Durable };
  std::vector<ReviewStatus> statuses{ReviewStatus::Pending};
  std::optional<std::string> subject_id;
  std::vector<std::string> kinds;
  std::vector<std::string> priorities;
  std::vector<SourceType> source_types;
  std::vector<std::string> tags;
  std::vector<std::string> dream_cycles;
  std::optional<bool> stale;
  std::optional<bool> conflict;
  std::optional<TimePoint> created_after;
  std::optional<TimePoint> created_before;
  std::optional<TimePoint> decided_after;
  std::optional<TimePoint> deferred_due_before;
  bool include_due_deferred = true;
  bool oldest_first = false;
  ScopeSelector scope = ScopeSelector::All;
  std::size_t limit = 50U;
  std::size_t offset = 0U;
};

struct ReviewRisk {
  double score = 0.0;
  std::string risk_class = "normal";
  std::map<std::string, double> components;
  std::vector<std::string> explanations;
};

struct ReviewRecallImpact {
  bool strict = false;
  bool balanced = false;
  bool broad = false;
  std::vector<std::string> warnings;
};

struct HydratedReview {
  ReviewItem item;
  ReviewCandidate candidate;
  std::optional<MemoryEntry> subject;
  std::vector<MemoryEntry> sources;
  std::vector<MemoryEntry> related;
  std::vector<GraphEdge> edges;
  std::vector<ProcedureRun> procedure_runs;
  ReviewRisk risk;
  ReviewRecallImpact recall;
  std::vector<std::string> blocking_diagnostics;
};

struct ReviewEdit {
  std::optional<std::string> content;
  std::optional<MemoryType> type;
  std::optional<MemoryStatus> target_status;
  std::optional<double> confidence;
  std::optional<MemoryScope> scope;
  std::optional<std::vector<std::string>> tags;
  std::optional<std::string> replacement_id;
};

struct ApproveReviewAction {
  std::string evidence_uri;
  std::optional<ReviewEdit> edit;
};
struct RejectReviewAction {};
struct DeferReviewAction { std::optional<TimePoint> until; };
struct ReopenReviewAction {};
struct RelateReviewAction {};
struct MergeReviewAction { std::optional<ReviewEdit> edit; };
struct SupersedeReviewAction { std::string target_id; };
struct ContradictionReviewAction {};
struct KeepBothReviewAction {};
struct RetainReviewAction {};
struct NarrowScopeReviewAction { std::optional<ReviewEdit> edit; };
struct RevalidateReviewAction { std::string evidence_uri; };
struct DeprecateReviewAction {};
struct AttachEvidenceReviewAction { std::string evidence_uri; };
struct CompensateReviewAction {
  std::string target_id;
  std::optional<ReviewEdit> edit;
};

using ReviewActionPayload =
    std::variant<ApproveReviewAction, RejectReviewAction, DeferReviewAction,
                 ReopenReviewAction, RelateReviewAction, MergeReviewAction,
                 SupersedeReviewAction, ContradictionReviewAction,
                 KeepBothReviewAction, RetainReviewAction,
                 NarrowScopeReviewAction, RevalidateReviewAction,
                 DeprecateReviewAction, AttachEvidenceReviewAction,
                 CompensateReviewAction>;

struct ReviewAction {
  ReviewActionKind kind = ReviewActionKind::Approve;
  // New callers should supply a typed payload. The legacy flattened fields are
  // retained while older CLI/RPC/tool integrations migrate; normalization
  // makes both construction styles observe the same validated values.
  std::optional<ReviewActionPayload> payload;
  std::string actor;
  std::string note;
  std::string target_id;
  std::string evidence_uri;
  std::optional<TimePoint> deferred_until;
  std::optional<ReviewEdit> edit;
  std::uint64_t expected_revision = 0U;
  std::string idempotency_key;
  bool human_authority = false;
};

ReviewAction normalize_review_action(ReviewAction action);

struct ReviewRecordChange {
  std::string record_type;
  std::string record_id;
  std::string operation;
  std::string before_json;
  std::string after_json;
};

struct ReviewMutationPreview {
  std::string review_id;
  ReviewAction action;
  std::uint64_t review_revision = 0U;
  std::string idempotency_token;
  std::vector<ReviewRecordChange> changes;
  std::vector<std::string> will_do;
  std::vector<std::string> will_not_do;
  std::vector<std::string> warnings;
  bool requires_human_authority = true;
  bool applicable = false;
};

struct ReviewApplicationResult {
  std::string review_id;
  ReviewApplicationStatus status = ReviewApplicationStatus::NotStarted;
  bool decision_persisted = false;
  bool domain_mutations_applied = false;
  bool idempotent_replay = false;
  std::uint64_t revision = 0U;
  std::string action_id;
  std::vector<std::string> changed_ids;
  std::vector<std::string> failed_ids;
  std::string error;
};

struct ReviewMemoryMutation {
  enum class Operation { Create, Patch } operation = Operation::Patch;
  std::string memory_id;
  std::optional<MemoryScope> target_scope;
  MemoryWrite create;
  MemoryPatch patch;
  // Optional compare-and-swap snapshot for asynchronous verification.
  std::optional<MemoryEntry> expected_memory;
};

struct ReviewAtomicMutation {
  // Created records are referenced by zero-based "$created:N" placeholders in
  // patch memory_id/replacement_id fields and graph edge endpoints.
  std::vector<ReviewMemoryMutation> memories;
  std::vector<GraphEdge> edges;
  bool require_domain_mutation = false;
  std::optional<std::uint64_t> expected_review_revision;
  std::optional<ReviewStatus> expected_review_status;
  std::vector<MemoryEntry> expected_memories;
};

std::string review_created_reference(std::size_t index);

struct ReviewStatistics {
  std::size_t pending = 0U;
  std::size_t urgent = 0U;
  std::size_t due_deferred = 0U;
  std::size_t reviewed = 0U;
  std::size_t approved = 0U;
  std::size_t edited_and_approved = 0U;
  std::size_t rejected = 0U;
  std::size_t deferred = 0U;
  std::size_t resurfaced = 0U;
  std::size_t reopened = 0U;
  std::size_t merged = 0U;
  std::size_t superseded = 0U;
  std::size_t validated = 0U;
  std::size_t failed = 0U;
  std::size_t application_errors = 0U;
  std::size_t idempotent_replays = 0U;
  std::size_t compensated = 0U;
  std::size_t dreaming_pending = 0U;
  std::size_t dreaming_approved = 0U;
  std::size_t dreaming_rejected = 0U;
  std::int64_t oldest_pending_age_seconds = 0;
  std::int64_t median_pending_age_seconds = 0;
  std::int64_t time_to_first_decision_p50_seconds = 0;
  std::int64_t time_to_first_decision_p95_seconds = 0;
  std::int64_t review_duration_p50_seconds = 0;
  std::int64_t review_duration_p95_seconds = 0;
  std::size_t unresolved_critical_conflicts = 0U;
  std::size_t stale_validated_subjects = 0U;
  std::size_t unprovenanced_active_subjects = 0U;
  std::size_t high_use_low_trust_subjects = 0U;
  std::size_t batch_runs = 0U;
  std::size_t batch_items = 0U;
  std::size_t batch_applied_items = 0U;
  std::size_t batch_failed_items = 0U;
  std::size_t batch_partial_failures = 0U;
  std::size_t batch_retries = 0U;
  std::size_t batch_idempotent_replays = 0U;
  std::size_t batch_compensations = 0U;
  std::size_t batch_application_errors = 0U;
  std::map<std::string, std::size_t> pending_by_kind;
  std::map<std::string, std::size_t> pending_by_priority;
  std::map<std::string, std::size_t> pending_by_scope;
  std::map<std::string, std::size_t> pending_by_risk_class;
  std::map<std::string, std::size_t> by_kind;
  std::map<std::string, std::size_t> by_priority;
  std::map<std::string, std::size_t> by_status;
  std::map<std::string, std::size_t> by_source_type;
  std::map<std::string, std::size_t> by_application_status;
  std::map<std::string, std::size_t> by_outcome;
  std::map<std::string, std::int64_t> oldest_pending_age_by_kind_seconds;
  std::map<std::string, std::int64_t> median_pending_age_by_kind_seconds;
  std::map<std::string, std::int64_t> oldest_pending_age_by_priority_seconds;
  std::map<std::string, std::int64_t> median_pending_age_by_priority_seconds;
  std::map<std::string, std::int64_t> oldest_pending_age_by_scope_seconds;
  std::map<std::string, std::int64_t> median_pending_age_by_scope_seconds;
  std::map<std::string, std::int64_t> oldest_pending_age_by_risk_class_seconds;
  std::map<std::string, std::int64_t> median_pending_age_by_risk_class_seconds;
};

struct ReviewActionRecord {
  std::string id;
  std::string review_id;
  std::string action_key;
  std::string action;
  std::string actor;
  std::string note;
  ReviewStatus before_status = ReviewStatus::Pending;
  ReviewStatus after_status = ReviewStatus::Pending;
  std::uint64_t before_revision = 0U;
  std::uint64_t after_revision = 0U;
  ReviewApplicationStatus application_status =
      ReviewApplicationStatus::NotStarted;
  std::string application_error;
  std::string result_json;
  std::size_t replay_count = 0U;
  TimePoint created_at{};
};

struct ReviewViewState {
  ReviewQuery query;
  std::string selected_review_id;
  std::size_t queue_offset = 0U;
  TimePoint updated_at{};
};

struct ReviewBatchItem {
  std::string review_id;
  ReviewAction action;
};

struct ReviewBatchPreview {
  std::string idempotency_token;
  std::vector<ReviewMutationPreview> items;
  std::vector<std::string> warnings;
  bool applicable = false;
};

struct ReviewBatchItemResult {
  std::string review_id;
  std::optional<ReviewApplicationResult> result;
  std::string error_code;
  std::string error;
};

struct ReviewBatchResult {
  std::string idempotency_token;
  std::vector<ReviewBatchItemResult> items;
  std::size_t applied_count = 0U;
  std::size_t failed_count = 0U;
  bool partial_failure = false;
};

enum class ReviewBatchJobStatus { Queued, Running, Completed, Failed };

struct ReviewBatchJob {
  std::string id;
  ReviewBatchJobStatus status = ReviewBatchJobStatus::Queued;
  std::size_t total_count = 0U;
  std::size_t processed_count = 0U;
  std::optional<ReviewBatchResult> result;
  std::string error_code;
  std::string error;
};

const char* to_string(ReviewBatchJobStatus value) noexcept;

ReviewCandidate decode_review_candidate(const ReviewItem& item);
nlohmann::json review_candidate_json(const ReviewCandidate& candidate);

}  // namespace qt::memory
