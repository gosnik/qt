#pragma once

#include "qt/memory/retrieval_evidence.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace qt::memory {

using TimePoint = std::chrono::system_clock::time_point;

enum class MemoryType {
  Semantic,
  Episodic,
  Procedural,
  Decision,
  Constraint,
  Bug,
  Preference,
  Skill,
  Observation,
  Todo,
  Unprocessed,
  Dream,
  Conversation,
  Tool,
  Pattern,
  Raw,
  Research,
};

enum class MemoryStatus {
  Candidate,
  Hypothesis,
  Active,
  Validated,
  Deprecated,
  Superseded,
  Rejected,
  Expired,
  DreamArtifact,
};

enum class SourceType {
  Chat,
  File,
  Commit,
  Issue,
  PullRequest,
  Document,
  Manual,
  ToolResult,
  TestLog,
  ProcessedText,
  Dreaming,
  Unknown,
};

enum class ScopeKind { Exact, Branch, Repository, Project, GlobalUser };
enum class RecallPolicy { Strict, Balanced, Broad };
enum class ReviewStatus { Pending, Accepted, Rejected, Deferred };
enum class ProcedureOutcome { Success, Failure, Partial, Unused, Unknown };

const char* to_string(MemoryType value) noexcept;
const char* to_string(MemoryStatus value) noexcept;
const char* to_string(SourceType value) noexcept;
const char* to_string(ScopeKind value) noexcept;
const char* to_string(ReviewStatus value) noexcept;
const char* to_string(ProcedureOutcome value) noexcept;
MemoryType memory_type_from_string(const std::string& value);
MemoryStatus memory_status_from_string(const std::string& value);
SourceType source_type_from_string(const std::string& value) noexcept;
ScopeKind scope_kind_from_string(const std::string& value);
ReviewStatus review_status_from_string(const std::string& value);
ProcedureOutcome procedure_outcome_from_string(const std::string& value) noexcept;

struct MemoryScope {
  ScopeKind kind = ScopeKind::Exact;
  std::string user;
  std::string organization;
  std::string project;
  std::string repository;
  std::string repository_display;
  std::string branch;
  std::string task;
  std::string agent;
  std::string session;
  std::string conversation;
};

struct Provenance {
  std::string source_id;
  SourceType source_type = SourceType::Unknown;
  std::string source_uri;
  std::string source_hash;
  std::string source_span;
  std::string created_by = "agent";
  double evidence_strength = 0.0;
};

struct MemoryEntry {
  std::string id;
  MemoryScope scope;
  MemoryType type = MemoryType::Observation;
  MemoryStatus status = MemoryStatus::Hypothesis;
  Provenance provenance;
  std::string source;
  std::string content;
  std::vector<std::string> tags;
  double confidence = 0.5;
  double quality_score = 0.0;
  std::size_t success_count = 0;
  std::size_t failure_count = 0;
  ProcedureOutcome last_outcome = ProcedureOutcome::Unknown;
  std::size_t access_count = 0;
  std::string superseded_by;
  std::string replacement_id;
  TimePoint created_at{};
  TimePoint updated_at{};
  TimePoint last_accessed_at{};
};

struct MemoryWrite {
  MemoryType type = MemoryType::Observation;
  MemoryStatus status = MemoryStatus::Hypothesis;
  Provenance provenance;
  std::string source;
  std::string content;
  std::vector<std::string> tags;
  double confidence = 0.5;
  double quality_score = 0.0;
};

struct MemoryPatch {
  std::optional<std::string> content;
  std::optional<MemoryType> type;
  std::optional<MemoryStatus> status;
  std::optional<std::string> replacement_id;
  std::optional<double> confidence;
  std::optional<double> quality_score;
  std::optional<Provenance> provenance;
  std::optional<std::vector<std::string>> tags;
};

struct MemoryStatistics {
  struct DreamingRuns {
    std::size_t attempted = 0U;
    std::size_t completed = 0U;
    std::size_t failed = 0U;
    std::size_t pending = 0U;
    std::size_t protocol_rejections = 0U;
    std::size_t timed_out = 0U;
    std::size_t cancelled = 0U;
    std::size_t start_failures = 0U;
    std::size_t cpu_fallbacks = 0U;
    std::size_t reviews_created = 0U;
    std::map<std::string, std::size_t> by_trigger;
    std::map<std::string, std::size_t> by_model;
    std::map<std::string, std::size_t> failures_by_reason;
  } dreaming;
  std::map<std::string, std::size_t> memory_counts;
  std::map<std::string, std::size_t> trust_states;
  std::size_t memory_entries = 0U;
  std::size_t hypotheses = 0U;
  std::size_t low_confidence = 0U;
  std::size_t unprovenanced = 0U;
  std::size_t deprecated = 0U;
  std::size_t failed_procedures = 0U;
  std::size_t high_use_hypotheses = 0U;
  std::size_t conflict_count = 0U;
  std::size_t unresolved_conflicts = 0U;
  std::size_t accessed_memories = 0U;
  std::size_t total_accesses = 0U;
  std::size_t successful_outcomes = 0U;
  std::size_t failed_outcomes = 0U;
  std::size_t pending_reviews = 0U;
};

struct MemoryQuery {
  std::string text;
  std::optional<MemoryType> type;
  std::vector<MemoryStatus> statuses;
  std::vector<std::string> tags;
  RecallPolicy policy = RecallPolicy::Balanced;
  bool include_global_user = false;
  bool include_superseded = false;
  std::size_t limit = 20;
  std::size_t offset = 0;
};

struct RecallScore {
  double total = 0.0;
  double lexical = 0.0;
  double hashed_similarity = 0.0;
  double confidence = 0.0;
  double quality = 0.0;
  double freshness = 0.0;
  double scope = 0.0;
  std::size_t lexical_rank = 0;
  std::size_t graph_rank = 0;
  std::size_t dense_rank = 0;
  double reciprocal_rank_fusion = 0.0;
};

struct RecalledAssertionStep {
  std::string memory_id;
  std::int64_t revision = 0;
  std::string from_entity_id;
  std::string to_entity_id;
  bool reverse = false;
};

struct RecalledMemory {
  MemoryEntry memory;
  RecallScore score;
  std::vector<std::string> warnings;
  bool eligible_for_context = true;
  std::vector<std::string> graph_path_ids;
  std::vector<std::string> graph_path_relations;
  std::vector<bool> graph_path_reverse;
  bool connecting_memory = false;
  bool historical = false;
  std::int64_t historical_revision = 0;
  std::optional<TimePoint> historical_recorded_at;
  std::optional<TimePoint> historical_as_of;
  std::vector<RecalledAssertionStep> assertion_path;
  std::optional<DenseSearchHit> dense_match;
};

struct MemoryRevision {
  std::string id;
  std::string memory_id;
  std::string kind;
  std::string entry_json;
  TimePoint created_at{};
};

struct GraphEdge {
  std::string id;
  std::string from_id;
  std::string to_id;
  std::string type;
  double confidence = 1.0;
  std::string source;
  TimePoint created_at{};
};

struct GraphQuery {
  std::string memory_id;
  std::string from_id;
  std::string to_id;
  std::string type;
  std::size_t limit = 50;
};

struct ProcedureFeedback {
  std::string procedure_id;
  std::string task_id;
  ProcedureOutcome outcome = ProcedureOutcome::Unknown;
  std::string notes;
  std::string evidence_uri;
  std::string deduplication_key;
};

struct ProcedureRun {
  std::string id;
  std::string procedure_id;
  std::string task_id;
  ProcedureOutcome outcome = ProcedureOutcome::Unknown;
  std::string notes;
  std::string evidence_uri;
  TimePoint created_at{};
  std::string deduplication_key;
};

struct ProcedureFeedbackResult {
  ProcedureRun run;
  MemoryEntry memory;
  bool inserted = false;
};

struct SourceRecord {
  std::string id;
  SourceType type = SourceType::Unknown;
  std::string uri;
  std::string hash;
  std::string display_name;
  std::string trust_level = "working";
  std::string hash_status = "unknown";
  std::size_t ingest_count = 0;
  TimePoint imported_at{};
  TimePoint last_ingested_at{};
  TimePoint last_checked_at{};
};

struct SourceEvent {
  std::string id;
  std::string source_id;
  std::string action;
  std::string status;
  std::string source_hash;
  std::string note;
  TimePoint created_at{};
};

struct ReviewItem {
  struct QueueSignals {
    bool critical_type = false;
    bool recent_context = false;
    double trust_risk = 0.0;
    std::size_t access_count = 0U;
    std::size_t procedure_failures = 0U;
  } queue_signals;
  std::string id;
  std::string kind = "candidate_memory";
  ReviewStatus status = ReviewStatus::Pending;
  std::string priority = "normal";
  std::string reason;
  std::string subject_id;
  Provenance provenance;
  std::string candidate_json;
  std::string decision;
  std::string decided_by;
  std::string decision_note;
  std::uint32_t candidate_version = 1U;
  std::uint64_t revision = 0U;
  TimePoint deferred_until{};
  std::string application_status = "not_started";
  std::string application_error;
  std::string action_key;
  std::vector<std::string> tags;
  TimePoint created_at{};
  TimePoint updated_at{};
  TimePoint decided_at{};
};

struct ReviewItemWrite {
  std::string kind = "candidate_memory";
  std::string priority = "normal";
  std::string reason;
  std::string subject_id;
  Provenance provenance;
  std::string candidate_json;
  std::uint32_t candidate_version = 1U;
  std::vector<std::string> tags;
};

struct ReviewDecision {
  ReviewStatus status = ReviewStatus::Deferred;
  std::string decision;
  std::string decided_by;
  std::string note;
  std::optional<TimePoint> deferred_until;
  std::uint64_t expected_revision = 0U;
  std::string action_key;
  std::string application_status = "applied";
  std::string application_error;
  std::string application_result_json = "{}";
};

struct AuditEvent {
  std::string id;
  std::string actor;
  std::string action;
  std::string reason;
  std::string before_id;
  std::string after_id;
  std::string evidence_uri;
  std::string metadata_json;
  TimePoint created_at{};
};

struct AuditEventWrite {
  std::string actor;
  std::string action;
  std::string reason;
  std::string before_id;
  std::string after_id;
  std::string evidence_uri;
  std::string metadata_json;
};

struct SessionEvent {
  std::string id;
  std::string event_type;
  std::string source;
  std::string payload_json;
  TimePoint created_at{};
  std::string deduplication_key;
};

struct SessionEventWrite {
  std::string event_type;
  std::string source;
  std::string payload_json;
  std::string deduplication_key;
};

struct SessionEventRecordResult {
  std::string id;
  bool inserted = false;
};

struct ContextBlock {
  std::string id;
  std::string content;
  TimePoint updated_at{};
};

struct ContextItemExplanation {
  std::string category;
  std::vector<std::string> memory_ids;
  std::vector<std::string> code_ids;
  std::vector<std::string> block_ids;
  bool graph_bundle = false;
  bool selected = false;
  bool eligible = true;
  // Item text only; shared headings and the context envelope are accounted for
  // by the pack's total. Unrenderable evidence has no known item cost.
  std::optional<std::size_t> rendered_bytes;
  std::string reason;
  std::vector<std::string> warnings;
};

struct ContextPack {
  std::string markdown;
  std::string required_context_markdown;
  std::vector<std::string> required_memory_ids;
  std::vector<std::string> memory_ids;
  std::vector<std::string> block_ids;
  std::vector<std::string> warnings;
  std::size_t estimated_tokens = 0;
  std::size_t omitted = 0;
  std::vector<std::string> code_ids;
  std::vector<ContextItemExplanation> item_explanations;
};

struct IngestionCandidate {
  std::string content;
  MemoryType type = MemoryType::Observation;
  std::vector<std::string> tags;
  double confidence = 0.5;
  std::string source_line;
};

struct IngestionRequest {
  std::string content;
  std::string source = "process:text-block";
  Provenance provenance;
  std::vector<std::string> tags;
  double minimum_confidence = 0.6;
  bool fallback_to_raw = false;
};

struct IngestionResult {
  std::vector<IngestionCandidate> candidates;
  std::vector<std::string> stored_ids;
};

struct ExportLimits {
  std::size_t memories = 10000;
  std::size_t graph_edges = 10000;
  std::size_t procedure_runs = 10000;
  std::size_t revisions = 10000;
  std::size_t sources = 10000;
  std::size_t source_events = 10000;
  std::size_t reviews = 10000;
  std::size_t audit_events = 10000;
  std::size_t session_events = 10000;
  std::size_t blocks = 10000;
};

struct RetentionPolicy {
  TimePoint older_than{};
  std::size_t keep_latest = 0;
  std::size_t batch_limit = 1000;
  bool raw_evidence = true;
  bool canonical_memories = false;
  bool audit_events = false;
  bool context_blocks = false;
  bool dry_run = true;
};

struct RetentionReport {
  bool dry_run = true;
  std::size_t session_events = 0;
  std::size_t source_events = 0;
  std::size_t orphan_sources = 0;
  std::size_t canonical_memories = 0;
  std::size_t audit_events = 0;
  std::size_t context_blocks = 0;

  std::size_t total() const noexcept {
    return session_events + source_events + orphan_sources + canonical_memories +
           audit_events + context_blocks;
  }
};

struct RedactionScrubPolicy {
  std::size_t batch_limit = 10000;
  bool dry_run = true;
};

struct RedactionScrubCollection {
  std::string name;
  std::size_t examined = 0;
  std::size_t changed = 0;
};

struct RedactionScrubReport {
  bool dry_run = true;
  bool truncated = false;
  std::vector<RedactionScrubCollection> collections;

  std::size_t examined() const noexcept {
    std::size_t result = 0;
    for (const auto& collection : collections) result += collection.examined;
    return result;
  }

  std::size_t changed() const noexcept {
    std::size_t result = 0;
    for (const auto& collection : collections) result += collection.changed;
    return result;
  }
};

}  // namespace qt::memory
