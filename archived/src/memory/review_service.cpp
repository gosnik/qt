#include "qt/memory/review_service.hpp"

#include "qt/foundation/hash.hpp"
#include "qt/memory/governance_service.hpp"
#include "qt/memory/review_json.hpp"
#include "qt/memory/scope.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace qt::memory {
namespace {

std::vector<std::string> source_ids(const ReviewCandidate& candidate) {
  return std::visit(
      [](const auto& payload) -> std::vector<std::string> {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, DreamArtifactCandidate>) {
          return payload.source_memory_ids;
        } else if constexpr (std::is_same_v<Payload,
                                            DreamResearchCandidate>) {
          auto result = payload.source_memory_ids;
          if (!payload.dream_id.empty()) result.push_back(payload.dream_id);
          return result;
        } else if constexpr (std::is_same_v<Payload,
                                            ConsolidationCandidate>) {
          return payload.memory_ids;
        } else if constexpr (std::is_same_v<Payload, ConflictCandidate>) {
          return payload.memory_ids;
        } else if constexpr (std::is_same_v<Payload,
                                            ProcedureRevisionCandidate>) {
          auto result = payload.evidence_ids;
          if (!payload.current_memory_id.empty()) {
            result.push_back(payload.current_memory_id);
          }
          return result;
        } else {
          return payload.related_memory_ids;
        }
      },
      candidate.payload);
}

using ApprovalStatusHandler =
    std::optional<MemoryStatus> (*)(const HydratedReview&);
using ApprovalPreviewHandler = void (*)(const HydratedReview&,
                                        const ReviewGovernancePolicy&,
                                        ReviewMutationPreview&);

struct ReviewKindHandler {
  ReviewCandidateKind kind;
  ApprovalStatusHandler approval_status;
  ApprovalPreviewHandler append_approval_preview;
};

const ReviewKindHandler& review_kind_handler(ReviewCandidateKind kind);

std::optional<MemoryStatus> approval_status(
    const HydratedReview& review, const ReviewAction& action) {
  if (action.edit && action.edit->target_status) {
    return action.edit->target_status;
  }
  return review_kind_handler(review.candidate.kind).approval_status(review);
}

MemoryPatch edited_patch(const ReviewAction& action) {
  MemoryPatch patch;
  if (!action.edit) return patch;
  patch.content = action.edit->content;
  patch.type = action.edit->type;
  patch.status = action.edit->target_status;
  patch.confidence = action.edit->confidence;
  patch.replacement_id = action.edit->replacement_id;
  patch.tags = action.edit->tags;
  return patch;
}

ReviewMemoryMutation patch_memory(std::string id, MemoryPatch patch) {
  ReviewMemoryMutation mutation;
  mutation.operation = ReviewMemoryMutation::Operation::Patch;
  mutation.memory_id = std::move(id);
  mutation.patch = std::move(patch);
  return mutation;
}

bool exact_scope(const MemoryScope& lhs, const MemoryScope& rhs) {
  return scope_key(lhs) == scope_key(rhs);
}

bool mutates_review_subject(const ReviewActionKind kind) {
  return kind != ReviewActionKind::Defer && kind != ReviewActionKind::Reopen;
}

ReviewMemoryMutation create_memory(MemoryWrite write,
                                   std::optional<MemoryScope> target_scope =
                                       std::nullopt) {
  ReviewMemoryMutation mutation;
  mutation.operation = ReviewMemoryMutation::Operation::Create;
  mutation.target_scope = std::move(target_scope);
  mutation.create = std::move(write);
  return mutation;
}

const MemoryEntry* find_memory(const HydratedReview& review,
                               const std::string& id) {
  if (review.subject && review.subject->id == id) return &*review.subject;
  const auto source = std::find_if(
      review.sources.begin(), review.sources.end(),
      [&](const MemoryEntry& memory) { return memory.id == id; });
  if (source != review.sources.end()) return &*source;
  const auto related = std::find_if(
      review.related.begin(), review.related.end(),
      [&](const MemoryEntry& memory) { return memory.id == id; });
  return related == review.related.end() ? nullptr : &*related;
}

bool critical_type(const MemoryType type) {
  return type == MemoryType::Constraint || type == MemoryType::Decision ||
         type == MemoryType::Procedural || type == MemoryType::Preference;
}

double priority_score(std::string_view priority) {
  if (priority == "critical" || priority == "urgent") return 45.0;
  if (priority == "high") return 30.0;
  if (priority == "low") return 0.0;
  return 10.0;
}

double kind_score(const ReviewCandidateKind kind) {
  switch (kind) {
    case ReviewCandidateKind::Conflict:
      return 40.0;
    case ReviewCandidateKind::ProcedureRevision:
      return 28.0;
    case ReviewCandidateKind::ProvenanceRepair:
      return 24.0;
    case ReviewCandidateKind::Staleness:
      return 20.0;
    case ReviewCandidateKind::SemanticConsolidation:
    case ReviewCandidateKind::Compression:
      return 15.0;
    case ReviewCandidateKind::DreamResearchSuccessor:
      return 10.0;
    case ReviewCandidateKind::DreamArtifact:
      return 5.0;
    case ReviewCandidateKind::General:
      return 8.0;
  }
  return 0.0;
}

ReviewRisk review_risk(const HydratedReview& review) {
  ReviewRisk result;
  const double priority = priority_score(review.item.priority);
  const double kind = kind_score(review.candidate.kind);
  const double missing_provenance =
      review.item.provenance.source_type == SourceType::Unknown &&
              review.item.provenance.source_uri.empty() &&
              review.item.provenance.source_hash.empty()
          ? 18.0
          : 0.0;
  const double weak_evidence =
      std::max(0.0, 1.0 - review.item.provenance.evidence_strength) * 12.0;
  const double critical =
      review.subject && critical_type(review.subject->type) ? 25.0 : 0.0;
  const double usage = review.subject
                           ? std::min(20.0, static_cast<double>(
                                                review.subject->access_count) *
                                                2.0)
                           : 0.0;
  const auto now = std::chrono::system_clock::now();
  const double waiting_age = std::min(
      15.0, std::max(0.0,
                     static_cast<double>(std::chrono::duration_cast<
                                             std::chrono::hours>(
                                             now - review.item.created_at)
                                             .count()) /
                         48.0));
  const double stale_age = review.candidate.kind == ReviewCandidateKind::Staleness
                               ? waiting_age
                               : 0.0;
  double conflict_severity = 0.0;
  if (const auto* conflict =
          std::get_if<ConflictCandidate>(&review.candidate.payload)) {
    conflict_severity = conflict->severity == "critical" ? 25.0
                        : conflict->severity == "high" ? 15.0
                                                       : 5.0;
  }
  const double procedure_failures = review.subject &&
                                            review.subject->type ==
                                                MemoryType::Procedural
                                        ? std::min(
                                              20.0,
                                              static_cast<double>(
                                                  review.subject->failure_count) *
                                                  4.0)
                                        : 0.0;
  const double recent_context =
      review.subject && review.subject->last_accessed_at != TimePoint{} &&
              now - review.subject->last_accessed_at < std::chrono::hours(24 * 7)
          ? 8.0
          : 0.0;
  result.components = {{"priority", priority},
                       {"kind", kind},
                       {"conflict_severity", conflict_severity},
                       {"missing_provenance", missing_provenance},
                       {"weak_evidence", weak_evidence},
                       {"critical_type", critical},
                       {"usage", usage},
                       {"waiting_age", waiting_age},
                       {"stale_age", stale_age},
                       {"procedure_failures", procedure_failures},
                       {"recent_context", recent_context}};
  for (const auto& [name, value] : result.components) {
    result.score += value;
    if (value > 0.0) {
      result.explanations.push_back(name + " contributes " +
                                    std::to_string(
                                        static_cast<std::int64_t>(value)));
    }
  }
  result.score = std::min(100.0, result.score);
  result.risk_class = result.score >= 70.0 ? "critical"
                      : result.score >= 45.0 ? "high"
                      : result.score >= 20.0 ? "normal"
                                             : "low";
  return result;
}

ReviewRecallImpact recall_impact(const std::optional<MemoryEntry>& subject) {
  ReviewRecallImpact result;
  if (!subject) {
    result.warnings.push_back("review subject is unavailable");
    return result;
  }
  result.strict = subject->status == MemoryStatus::Validated;
  result.balanced = GovernanceService::trusted_for_recall(subject->status);
  result.broad = result.balanced || subject->status == MemoryStatus::Candidate ||
                 subject->status == MemoryStatus::Hypothesis;
  if (subject->status == MemoryStatus::DreamArtifact) {
    result.warnings.push_back("dream artifact is isolated from ordinary recall");
  } else if (subject->status == MemoryStatus::Rejected) {
    result.warnings.push_back("rejected memory is excluded from recall");
  } else if (subject->status == MemoryStatus::Superseded ||
             subject->status == MemoryStatus::Deprecated ||
             subject->status == MemoryStatus::Expired) {
    result.warnings.push_back("inactive lifecycle state is excluded by default");
  }
  return result;
}

std::optional<MemoryStatus> hypothesis_approval_status(
    const HydratedReview&) {
  return MemoryStatus::Hypothesis;
}

std::optional<MemoryStatus> general_approval_status(
    const HydratedReview& review) {
  const auto* candidate =
      std::get_if<GeneralReviewCandidate>(&review.candidate.payload);
  return candidate != nullptr && candidate->target_status
             ? candidate->target_status
             : std::optional<MemoryStatus>{MemoryStatus::Active};
}

std::optional<MemoryStatus> explicit_resolution_required(
    const HydratedReview&) {
  return std::nullopt;
}

void handle_dream_approval(const HydratedReview& review,
                           const ReviewGovernancePolicy& policy,
                           ReviewMutationPreview& preview) {
  preview.will_do.push_back(
      "promote the isolated dream artifact to a reviewable hypothesis");
  preview.will_do.push_back("preserve Dreaming provenance and source links");
  preview.will_not_do.push_back("mark the hypothesis as validated truth");
  preview.will_not_do.push_back("treat model output as external evidence");
  preview.applicable = review.subject.has_value() &&
                       policy.allow_dream_hypothesis_promotion &&
                       review.subject->status == MemoryStatus::DreamArtifact;
  if (!policy.allow_dream_hypothesis_promotion) {
    preview.warnings.push_back(
        "Dreaming promotion is disabled by the configured promotion policy");
  }
}

void handle_dream_research_approval(const HydratedReview& review,
                                    const ReviewGovernancePolicy&,
                                    ReviewMutationPreview& preview) {
  preview.will_do.push_back(
      "approve the separately sourced research memory as a hypothesis");
  preview.will_not_do.push_back("retroactively validate the dream artifact");
  preview.applicable = review.subject.has_value();
}

void handle_consolidation_approval(const HydratedReview&,
                                   const ReviewGovernancePolicy&,
                                   ReviewMutationPreview& preview) {
  preview.warnings.push_back(
      "choose merge, relate, supersede, or contradiction before applying");
}

void handle_compression_approval(const HydratedReview& review,
                                 const ReviewGovernancePolicy& policy,
                                 ReviewMutationPreview& preview) {
  handle_consolidation_approval(review, policy, preview);
}

void handle_conflict_approval(const HydratedReview&,
                              const ReviewGovernancePolicy&,
                              ReviewMutationPreview& preview) {
  preview.warnings.push_back(
      "choose an explicit conflict resolution before applying");
}

void handle_procedure_approval(const HydratedReview& review,
                               const ReviewGovernancePolicy&,
                               ReviewMutationPreview& preview) {
  preview.will_do.push_back(
      "store the previewed procedure revision as an unvalidated hypothesis");
  preview.will_not_do.push_back(
      "mark revised procedure steps as validated without outcome evidence");
  preview.applicable = review.subject.has_value();
}

void handle_general_approval(const HydratedReview& review,
                             const ReviewGovernancePolicy&,
                             ReviewMutationPreview& preview) {
  preview.will_do.push_back("advance the candidate to its previewed trust state");
  preview.will_not_do.push_back("validate a critical fact without evidence");
  preview.applicable = review.subject.has_value();
}

void handle_staleness_approval(const HydratedReview&,
                               const ReviewGovernancePolicy&,
                               ReviewMutationPreview& preview) {
  preview.warnings.push_back(
      "choose revalidate, deprecate, supersede, or retain before applying");
}

void handle_provenance_approval(const HydratedReview&,
                                const ReviewGovernancePolicy&,
                                ReviewMutationPreview& preview) {
  preview.warnings.push_back(
      "attach evidence or reduce trust before applying provenance repair");
}

const ReviewKindHandler& review_kind_handler(const ReviewCandidateKind kind) {
  static const std::array<ReviewKindHandler, 9U> handlers{{
      {ReviewCandidateKind::DreamArtifact, &hypothesis_approval_status,
       &handle_dream_approval},
      {ReviewCandidateKind::DreamResearchSuccessor,
       &hypothesis_approval_status,
       &handle_dream_research_approval},
      {ReviewCandidateKind::SemanticConsolidation,
       &explicit_resolution_required,
       &handle_consolidation_approval},
      {ReviewCandidateKind::Compression, &explicit_resolution_required,
       &handle_compression_approval},
      {ReviewCandidateKind::Conflict, &explicit_resolution_required,
       &handle_conflict_approval},
      {ReviewCandidateKind::ProcedureRevision, &hypothesis_approval_status,
       &handle_procedure_approval},
      {ReviewCandidateKind::General, &general_approval_status,
       &handle_general_approval},
      {ReviewCandidateKind::Staleness, &explicit_resolution_required,
       &handle_staleness_approval},
      {ReviewCandidateKind::ProvenanceRepair, &explicit_resolution_required,
       &handle_provenance_approval},
  }};
  const auto found = std::find_if(
      handlers.begin(), handlers.end(),
      [kind](const ReviewKindHandler& handler) { return handler.kind == kind; });
  if (found == handlers.end()) {
    throw std::logic_error("missing review-kind handler");
  }
  return *found;
}

void append_approval_effect(const HydratedReview& review,
                            const ReviewGovernancePolicy& policy,
                            ReviewMutationPreview& preview) {
  const auto& handler = review_kind_handler(review.candidate.kind);
  handler.append_approval_preview(review, policy, preview);
}

ReviewRecordChange record_change(std::string type, std::string id,
                                 std::string operation,
                                 const nlohmann::json& before,
                                 const nlohmann::json& after) {
  return ReviewRecordChange{std::move(type), std::move(id),
                            std::move(operation), before.dump(), after.dump()};
}

void append_preview_records(const HydratedReview& review,
                            ReviewMutationPreview& preview) {
  auto after_review = review.item;
  switch (preview.action.kind) {
    case ReviewActionKind::Reject: after_review.status = ReviewStatus::Rejected; break;
    case ReviewActionKind::Defer: after_review.status = ReviewStatus::Deferred; break;
    case ReviewActionKind::Reopen: after_review.status = ReviewStatus::Pending; break;
    case ReviewActionKind::Compensate: break;
    default: after_review.status = ReviewStatus::Accepted; break;
  }
  after_review.revision = review.item.revision + 1U;
  after_review.decision = to_string(preview.action.kind);
  after_review.decided_by = preview.action.actor;
  after_review.decision_note = preview.action.note;
  preview.changes.push_back(record_change(
      "review", review.item.id, "compare_and_set",
      review_item_json(review.item), review_item_json(after_review)));

  const auto append_memory = [&](const MemoryEntry& before,
                                 MemoryEntry after,
                                 const std::string& operation = "patch") {
    preview.changes.push_back(record_change(
        "memory", before.id, operation, review_memory_json(before),
        review_memory_json(after)));
  };
  if (review.subject) {
    auto after = *review.subject;
    bool changed = false;
    if ((preview.action.kind == ReviewActionKind::Approve ||
         preview.action.kind == ReviewActionKind::EditAndApprove) &&
        approval_status(review, preview.action)) {
      after.status = *approval_status(review, preview.action);
      if (after.status == MemoryStatus::Validated &&
          !preview.action.evidence_uri.empty()) {
        after.provenance.source_type = SourceType::Document;
        after.provenance.source_uri = preview.action.evidence_uri;
        after.provenance.created_by = preview.action.actor;
        after.provenance.evidence_strength = 1.0;
      }
      if (review.candidate.kind == ReviewCandidateKind::ProcedureRevision &&
          !(preview.action.edit && preview.action.edit->content)) {
        if (const auto* procedure =
                std::get_if<ProcedureRevisionCandidate>(
                    &review.candidate.payload)) {
          after.content = MemoryService::normalize_content(
              procedure->proposed_content);
          MemoryService::redact_secrets(after.content);
        }
      }
      changed = true;
    } else if (preview.action.kind == ReviewActionKind::Reject &&
               (after.status == MemoryStatus::Candidate ||
                after.status == MemoryStatus::Hypothesis ||
                after.status == MemoryStatus::DreamArtifact)) {
      after.status = MemoryStatus::Rejected;
      changed = true;
    } else if (preview.action.kind == ReviewActionKind::Deprecate) {
      after.status = MemoryStatus::Deprecated;
      changed = true;
    } else if (preview.action.kind == ReviewActionKind::Revalidate) {
      after.status = MemoryStatus::Validated;
      changed = true;
    } else if (preview.action.kind == ReviewActionKind::Retain) {
      if (std::find(after.tags.begin(), after.tags.end(),
                    "review:retained_current") == after.tags.end()) {
        after.tags.push_back("review:retained_current");
      }
      changed = true;
    }
    if (preview.action.edit) {
      const auto& edit = *preview.action.edit;
      if (edit.content) after.content = *edit.content;
      if (edit.type) after.type = *edit.type;
      if (edit.target_status) after.status = *edit.target_status;
      if (edit.confidence) after.confidence = *edit.confidence;
      if (edit.scope) after.scope = *edit.scope;
      if (edit.tags) after.tags = *edit.tags;
      if (edit.replacement_id) {
        after.replacement_id = *edit.replacement_id;
        after.superseded_by = *edit.replacement_id;
      }
      changed = true;
    }
    if (preview.action.kind == ReviewActionKind::AttachEvidence) {
      after.provenance.source_uri = preview.action.evidence_uri;
      after.provenance.source_type = SourceType::Document;
      after.provenance.created_by = preview.action.actor;
      after.provenance.evidence_strength = 1.0;
      changed = true;
    }
    if (changed) append_memory(*review.subject, std::move(after));
  }

  if (preview.action.kind == ReviewActionKind::Relate ||
      preview.action.kind == ReviewActionKind::MarkContradiction ||
      preview.action.kind == ReviewActionKind::KeepBoth) {
    const auto ids = source_ids(review.candidate);
    const char* type = preview.action.kind == ReviewActionKind::Relate
                           ? "related"
                       : preview.action.kind == ReviewActionKind::MarkContradiction
                           ? "contradicts"
                           : "conflict_warning";
    for (std::size_t index = 1U; index < ids.size(); ++index) {
      preview.changes.push_back(record_change(
          "graph_edge", ids.front() + ":" + ids[index] + ":" + type,
          "create", nlohmann::json(nullptr),
          nlohmann::json{{"from_id", ids.front()},
                         {"to_id", ids[index]}, {"type", type}}));
    }
  }
  if (preview.action.kind == ReviewActionKind::Merge) {
    const auto* candidate =
        std::get_if<ConsolidationCandidate>(&review.candidate.payload);
    const auto ids = source_ids(review.candidate);
    if (candidate != nullptr ||
        review.candidate.kind == ReviewCandidateKind::Conflict) {
      std::string content =
          preview.action.edit && preview.action.edit->content
              ? *preview.action.edit->content
              : candidate != nullptr ? candidate->canonical_content
                                     : std::string{};
      content = MemoryService::normalize_content(std::move(content));
      MemoryService::redact_secrets(content);
      preview.changes.push_back(record_change(
          "memory", review_created_reference(0U), "create",
          nlohmann::json(nullptr),
          nlohmann::json{{"content", content},
                         {"status", "hypothesis"},
                         {"source", "review:canonical_merge"},
                         {"source_memory_ids", ids}}));
      for (const auto& id : ids) {
        if (const auto* source = find_memory(review, id)) {
          auto after = *source;
          after.status = MemoryStatus::Superseded;
          after.replacement_id = review_created_reference(0U);
          after.superseded_by = review_created_reference(0U);
          append_memory(*source, std::move(after));
        }
        preview.changes.push_back(record_change(
            "graph_edge", id + ":canonical", "create",
            nlohmann::json(nullptr),
            nlohmann::json{{"from_id", id},
                           {"to_id", review_created_reference(0U)},
                           {"type", "superseded_by"}}));
      }
    }
  } else if (preview.action.kind == ReviewActionKind::Supersede) {
    for (const auto& id : source_ids(review.candidate)) {
      if (id == preview.action.target_id) continue;
      if (const auto* source = find_memory(review, id)) {
        auto after = *source;
        after.status = MemoryStatus::Superseded;
        after.replacement_id = preview.action.target_id;
        after.superseded_by = preview.action.target_id;
        append_memory(*source, std::move(after));
      }
      preview.changes.push_back(record_change(
          "graph_edge", id + ":" + preview.action.target_id,
          "create", nlohmann::json(nullptr),
          nlohmann::json{{"from_id", id},
                         {"to_id", preview.action.target_id},
                         {"type", "superseded_by"}}));
    }
  } else if (preview.action.kind == ReviewActionKind::NarrowScope &&
             review.subject && preview.action.edit &&
             preview.action.edit->scope) {
    auto narrowed = *review.subject;
    narrowed.id = review_created_reference(0U);
    narrowed.scope = *preview.action.edit->scope;
    if (preview.action.edit->content) narrowed.content = *preview.action.edit->content;
    if (preview.action.edit->type) narrowed.type = *preview.action.edit->type;
    if (preview.action.edit->target_status) {
      narrowed.status = *preview.action.edit->target_status;
    } else if (narrowed.status == MemoryStatus::Validated) {
      narrowed.status = MemoryStatus::Active;
    }
    if (preview.action.edit->confidence) {
      narrowed.confidence = *preview.action.edit->confidence;
    }
    if (preview.action.edit->tags) narrowed.tags = *preview.action.edit->tags;
    preview.changes.push_back(record_change(
        "memory", narrowed.id, "create_narrower_copy", nlohmann::json(nullptr),
        review_memory_json(narrowed)));
    auto superseded = *review.subject;
    superseded.status = MemoryStatus::Superseded;
    superseded.replacement_id = narrowed.id;
    superseded.superseded_by = narrowed.id;
    append_memory(*review.subject, std::move(superseded));
  }
}

}  // namespace

std::vector<ReviewItem> ReviewInspectionService::query(
    const MemoryScope& scope, const ReviewQuery& query) const {
  (void)reviews_.resurface_due_reviews(scope, std::chrono::system_clock::now());
  auto expanded = query;
  if (query.offset > std::numeric_limits<std::size_t>::max() - query.limit) {
    throw std::invalid_argument("review query offset is too large");
  }
  expanded.offset = 0U;
  // Rank against the complete bounded queue so durable subject signals cannot
  // be hidden by the storage layer's inexpensive preliminary ordering.
  expanded.limit = 1000U;
  auto items = reviews_.query_reviews(scope, expanded);
  std::map<std::string, double> scores;
  const auto now = std::chrono::system_clock::now();
  for (const auto& item : items) {
    scores[item.id] = review_queue_score(item, now);
  }
  std::sort(items.begin(), items.end(), [&](const ReviewItem& left,
                                             const ReviewItem& right) {
    if (scores[left.id] != scores[right.id]) {
      return scores[left.id] > scores[right.id];
    }
    if (left.created_at != right.created_at) {
      return query.oldest_first ? left.created_at < right.created_at
                                : left.created_at > right.created_at;
    }
    return left.id < right.id;
  });
  if (query.offset >= items.size()) return {};
  const auto begin = items.begin() + static_cast<std::ptrdiff_t>(query.offset);
  const auto count = std::min(query.limit,
                              static_cast<std::size_t>(items.end() - begin));
  return std::vector<ReviewItem>(begin,
                                 begin + static_cast<std::ptrdiff_t>(count));
}

HydratedReview ReviewInspectionService::hydrate(
    const MemoryScope& scope, const std::string& review_id) const {
  HydratedReview result;
  result.item = reviews_.review(scope, review_id);
  result.candidate = decode_review_candidate(result.item);
  result.blocking_diagnostics = result.candidate.diagnostics;
  if (!result.item.subject_id.empty()) {
    try {
      result.subject = memories_.get(scope, result.item.subject_id);
    } catch (const std::exception&) {
      result.blocking_diagnostics.push_back(
          "review subject is missing or outside the displayed scope");
    }
  }
  if (result.subject &&
      result.candidate.kind == ReviewCandidateKind::DreamArtifact) {
    if (auto* dream =
            std::get_if<DreamArtifactCandidate>(&result.candidate.payload);
        dream != nullptr && dream->content.empty()) {
      dream->content = result.subject->content;
    }
  }
  std::set<std::string> unique_ids;
  for (const auto& id : source_ids(result.candidate)) {
    if (id.empty() || id == result.item.subject_id || !unique_ids.insert(id).second) {
      continue;
    }
    try {
      auto source = memories_.get(scope, id);
      if (scope_key(source.scope) != scope_key(scope)) {
        result.blocking_diagnostics.push_back(
            "source memory " + id + " is outside the review scope");
        continue;
      }
      result.sources.push_back(std::move(source));
    } catch (const std::exception&) {
      result.blocking_diagnostics.push_back(
          "source memory " + id + " is missing or outside scope");
    }
  }
  if (result.subject) {
    if (!result.item.provenance.source_hash.empty() &&
        !result.subject->provenance.source_hash.empty() &&
        result.item.provenance.source_hash !=
            result.subject->provenance.source_hash) {
      result.blocking_diagnostics.push_back(
          "review source hash is stale relative to the current subject");
    }
    GraphQuery query;
    query.memory_id = result.subject->id;
    query.limit = 100U;
    result.edges = graph_.query_edges(scope, query);
    for (const auto& edge : result.edges) {
      const auto related_id = edge.from_id == result.subject->id
                                  ? edge.to_id
                                  : edge.from_id;
      if (related_id.empty() || related_id == result.subject->id ||
          !unique_ids.insert(related_id).second) {
        continue;
      }
      try {
        auto related = memories_.get(scope, related_id);
        if (scope_key(related.scope) == scope_key(scope)) {
          result.related.push_back(std::move(related));
        }
      } catch (const std::exception&) {
        result.blocking_diagnostics.push_back(
            "related memory " + related_id + " is unavailable");
      }
    }
    if (result.subject->type == MemoryType::Procedural ||
        result.candidate.kind == ReviewCandidateKind::ProcedureRevision) {
      result.procedure_runs = procedures_.runs(scope, result.subject->id);
    }
  }
  result.recall = recall_impact(result.subject);
  result.risk = review_risk(result);
  return result;
}

MemoryEntry ReviewInspectionService::get_memory(const MemoryScope& scope,
                                                const std::string& memory_id) const {
  return memories_.get(scope, memory_id);
}

ReviewMutationPreview ReviewInspectionService::preview(
    const MemoryScope& scope, const std::string& review_id,
    ReviewAction action) const {
  action = normalize_review_action(std::move(action));
  MemoryService::redact_secrets(action.note);
  MemoryService::redact_secrets(action.evidence_uri);
  if (action.edit) {
    if (action.edit->content) {
      *action.edit->content =
          MemoryService::normalize_content(std::move(*action.edit->content));
      MemoryService::redact_secrets(*action.edit->content);
    }
    if (action.edit->tags) {
      for (auto& tag : *action.edit->tags) MemoryService::redact_secrets(tag);
    }
  }
  // Rebuild the typed payload from the normalized and redacted compatibility
  // fields so previewed bytes are exactly the bytes later applied.
  action.payload.reset();
  action = normalize_review_action(std::move(action));
  auto review = hydrate(scope, review_id);
  ReviewMutationPreview result;
  result.review_id = review.item.id;
  result.review_revision = review.item.revision;
  action.expected_revision = review.item.revision;
  result.action = std::move(action);
  result.requires_human_authority = true;
  if (review.item.status != ReviewStatus::Pending &&
      result.action.kind != ReviewActionKind::Reopen &&
      result.action.kind != ReviewActionKind::Compensate) {
    result.warnings.push_back("review is no longer pending");
  } else if (result.action.kind == ReviewActionKind::Reopen) {
    result.will_do.push_back(
        "reopen the terminal review as pending without changing memory");
    result.will_not_do.push_back("erase the original decision or action history");
    result.applicable = review.item.status != ReviewStatus::Pending &&
                        !result.action.note.empty();
    if (result.action.note.empty()) {
      result.warnings.push_back("reopen requires an explicit reason");
    }
  } else if (result.action.kind == ReviewActionKind::Compensate) {
    result.will_do.push_back(
        "record a new governed compensation while preserving the original decision");
    result.will_not_do.push_back("rewrite or erase prior audit history");
    result.applicable = review.item.status != ReviewStatus::Pending &&
                        !result.action.note.empty() &&
                        result.action.edit.has_value() &&
                        !result.action.target_id.empty();
    if (!result.applicable) {
      result.warnings.push_back(
          "compensation requires a reason, target memory, and explicit replacement values");
    }
  } else if (result.action.kind == ReviewActionKind::Reject) {
    result.will_do.push_back("reject the proposal and exclude its subject from current recall");
    result.will_not_do.push_back("delete the subject or its audit history");
    result.applicable = true;
  } else if (result.action.kind == ReviewActionKind::Defer) {
    result.will_do.push_back("defer the review without mutating its subject");
    result.will_not_do.push_back("approve, reject, or change memory trust");
    result.applicable = result.action.deferred_until.has_value();
    if (!result.applicable) {
      result.warnings.push_back("defer requires a resurface time");
    }
  } else if (result.action.kind == ReviewActionKind::Approve ||
             result.action.kind == ReviewActionKind::EditAndApprove) {
    append_approval_effect(review, policy_, result);
    if (result.action.kind == ReviewActionKind::EditAndApprove &&
        !result.action.edit.has_value()) {
      result.applicable = false;
      result.warnings.push_back("edit-and-approve requires an edited candidate");
    }
  } else if (result.action.kind == ReviewActionKind::Relate ||
             result.action.kind == ReviewActionKind::MarkContradiction ||
             result.action.kind == ReviewActionKind::KeepBoth) {
    const auto ids = source_ids(review.candidate);
    const bool supported =
        review.candidate.kind == ReviewCandidateKind::SemanticConsolidation ||
        review.candidate.kind == ReviewCandidateKind::Compression ||
        review.candidate.kind == ReviewCandidateKind::Conflict;
    result.will_do.push_back(
        result.action.kind == ReviewActionKind::Relate
            ? "link the reviewed memories without changing their trust states"
        : result.action.kind == ReviewActionKind::MarkContradiction
            ? "record explicit contradiction edges and preserve both records"
            : "retain both memories with an unresolved-conflict warning edge");
    result.will_not_do.push_back("validate, merge, or delete either memory");
    result.applicable = supported && ids.size() >= 2U;
  } else if (result.action.kind == ReviewActionKind::Merge) {
    const auto* candidate =
        std::get_if<ConsolidationCandidate>(&review.candidate.payload);
    const auto ids = source_ids(review.candidate);
    const std::string content =
        result.action.edit && result.action.edit->content
            ? *result.action.edit->content
            : candidate ? candidate->canonical_content : std::string{};
    result.will_do.push_back(
        "create one hypothesis canonical memory, link every source, and supersede originals");
    result.will_not_do.push_back("mark the canonical memory as validated truth");
    result.applicable =
        (candidate != nullptr ||
         review.candidate.kind == ReviewCandidateKind::Conflict) &&
        ids.size() >= 2U && !content.empty();
  } else if (result.action.kind == ReviewActionKind::Supersede) {
    const auto ids = source_ids(review.candidate);
    result.will_do.push_back(
        "mark every other reviewed memory superseded by the selected current memory");
    result.will_not_do.push_back("delete superseded history");
    result.applicable = !result.action.target_id.empty() && ids.size() >= 2U &&
                        std::find(ids.begin(), ids.end(), result.action.target_id) !=
                            ids.end();
  } else if (result.action.kind == ReviewActionKind::NarrowScope) {
    result.will_do.push_back(
        "create a narrower-scope copy and supersede the broader record with lineage");
    result.will_not_do.push_back("broaden access or mutate unrelated scopes");
    result.applicable = review.subject.has_value() && result.action.edit &&
                        result.action.edit->scope.has_value();
  } else if (result.action.kind == ReviewActionKind::Revalidate) {
    result.will_do.push_back(
        "attach current independent evidence and transition the subject to validated");
    result.will_not_do.push_back("treat model output alone as evidence");
    result.applicable = review.subject.has_value() &&
                        !result.action.evidence_uri.empty();
  } else if (result.action.kind == ReviewActionKind::Deprecate) {
    result.will_do.push_back(
        "deprecate the subject while preserving history and provenance");
    result.will_not_do.push_back("delete the memory");
    result.applicable = review.subject.has_value();
  } else if (result.action.kind == ReviewActionKind::Retain) {
    result.will_do.push_back(
        "retain the current lifecycle state and record the review rationale on the subject");
    result.will_not_do.push_back("validate, supersede, or delete the memory");
    result.applicable = review.subject.has_value() &&
                        review.candidate.kind == ReviewCandidateKind::Staleness &&
                        !result.action.note.empty();
  } else if (result.action.kind == ReviewActionKind::AttachEvidence) {
    result.will_do.push_back(
        "attach the supplied evidence provenance without silently validating the subject");
    result.will_not_do.push_back("change lifecycle state or treat model output as evidence");
    result.applicable = review.subject.has_value() &&
                        !result.action.evidence_uri.empty();
  } else {
    result.warnings.push_back("unsupported review action");
  }
  if (!review.blocking_diagnostics.empty()) {
    result.warnings.insert(result.warnings.end(),
                           review.blocking_diagnostics.begin(),
                           review.blocking_diagnostics.end());
    if (result.action.kind != ReviewActionKind::Reject &&
        result.action.kind != ReviewActionKind::Defer &&
        result.action.kind != ReviewActionKind::Reopen) {
      result.applicable = false;
    }
  }
  if (result.applicable) append_preview_records(review, result);
  std::string token_material =
      result.review_id + ":" + std::to_string(result.review_revision) + ":" +
      to_string(result.action.kind) + ":" + result.action.target_id + ":" +
      result.action.note + ":" + result.action.evidence_uri;
  if (result.action.deferred_until) {
    token_material += ":defer=" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            result.action.deferred_until->time_since_epoch()).count());
  }
  if (result.action.edit) {
    const auto& edit = *result.action.edit;
    if (edit.content) token_material += ":content=" + *edit.content;
    if (edit.type) token_material += ":type=" + std::string(to_string(*edit.type));
    if (edit.target_status) {
      token_material += ":status=" + std::string(to_string(*edit.target_status));
    }
    if (edit.confidence) token_material += ":confidence=" + std::to_string(*edit.confidence);
    if (edit.replacement_id) token_material += ":replacement=" + *edit.replacement_id;
    if (edit.scope) token_material += ":scope=" + scope_key(*edit.scope);
    if (edit.tags) {
      for (const auto& tag : *edit.tags) token_material += ":tag=" + tag;
    }
  }
  const auto append_memory_fingerprint = [&](const MemoryEntry& memory) {
    token_material += ":memory=" + memory.id + ":" +
        qt::foundation::stable_hash_hex(memory.content) + ":" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           memory.updated_at.time_since_epoch()).count());
  };
  if (review.subject) append_memory_fingerprint(*review.subject);
  for (const auto& source : review.sources) append_memory_fingerprint(source);
  result.idempotency_token = qt::foundation::stable_hash_hex(token_material);
  return result;
}

ReviewStatistics ReviewInspectionService::statistics(
    const MemoryScope& scope) const {
  return reviews_.review_statistics(scope);
}

ReviewStatistics ReviewInspectionService::statistics_including_durable(
    const MemoryScope& exact_scope) const {
  auto result = statistics(exact_scope);
  const auto durable_scope = durable_memory_scope(exact_scope);
  if (scope_key(durable_scope) == scope_key(exact_scope)) return result;
  const auto durable = statistics(durable_scope);
  const auto exact_pending = result.pending;
  result.pending += durable.pending;
  result.urgent += durable.urgent;
  result.due_deferred += durable.due_deferred;
  result.reviewed += durable.reviewed;
  result.approved += durable.approved;
  result.edited_and_approved += durable.edited_and_approved;
  result.rejected += durable.rejected;
  result.deferred += durable.deferred;
  result.resurfaced += durable.resurfaced;
  result.reopened += durable.reopened;
  result.merged += durable.merged;
  result.superseded += durable.superseded;
  result.validated += durable.validated;
  result.failed += durable.failed;
  result.application_errors += durable.application_errors;
  result.idempotent_replays += durable.idempotent_replays;
  result.compensated += durable.compensated;
  result.dreaming_pending += durable.dreaming_pending;
  result.dreaming_approved += durable.dreaming_approved;
  result.dreaming_rejected += durable.dreaming_rejected;
  result.oldest_pending_age_seconds = std::max(
      result.oldest_pending_age_seconds, durable.oldest_pending_age_seconds);
  if (result.pending != 0U) {
    result.median_pending_age_seconds = static_cast<std::int64_t>(
        (static_cast<long double>(result.median_pending_age_seconds) *
             exact_pending +
         static_cast<long double>(durable.median_pending_age_seconds) *
             durable.pending) /
        result.pending);
  }
  result.time_to_first_decision_p50_seconds = std::max(
      result.time_to_first_decision_p50_seconds,
      durable.time_to_first_decision_p50_seconds);
  result.time_to_first_decision_p95_seconds = std::max(
      result.time_to_first_decision_p95_seconds,
      durable.time_to_first_decision_p95_seconds);
  result.review_duration_p50_seconds = std::max(
      result.review_duration_p50_seconds,
      durable.review_duration_p50_seconds);
  result.review_duration_p95_seconds = std::max(
      result.review_duration_p95_seconds,
      durable.review_duration_p95_seconds);
  result.unresolved_critical_conflicts += durable.unresolved_critical_conflicts;
  result.stale_validated_subjects += durable.stale_validated_subjects;
  result.unprovenanced_active_subjects +=
      durable.unprovenanced_active_subjects;
  result.high_use_low_trust_subjects += durable.high_use_low_trust_subjects;
  result.batch_runs += durable.batch_runs;
  result.batch_items += durable.batch_items;
  result.batch_applied_items += durable.batch_applied_items;
  result.batch_failed_items += durable.batch_failed_items;
  result.batch_partial_failures += durable.batch_partial_failures;
  result.batch_retries += durable.batch_retries;
  result.batch_idempotent_replays += durable.batch_idempotent_replays;
  result.batch_compensations += durable.batch_compensations;
  result.batch_application_errors += durable.batch_application_errors;
  const auto merge_count_map = [](auto& target, const auto& source) {
    for (const auto& [key, value] : source) target[key] += value;
  };
  merge_count_map(result.pending_by_kind, durable.pending_by_kind);
  merge_count_map(result.pending_by_priority, durable.pending_by_priority);
  merge_count_map(result.pending_by_scope, durable.pending_by_scope);
  merge_count_map(result.pending_by_risk_class,
                  durable.pending_by_risk_class);
  merge_count_map(result.by_kind, durable.by_kind);
  merge_count_map(result.by_priority, durable.by_priority);
  merge_count_map(result.by_status, durable.by_status);
  merge_count_map(result.by_source_type, durable.by_source_type);
  merge_count_map(result.by_application_status,
                  durable.by_application_status);
  merge_count_map(result.by_outcome, durable.by_outcome);
  const auto merge_age_map = [](auto& target, const auto& source) {
    for (const auto& [key, value] : source) {
      target[key] = std::max(target[key], value);
    }
  };
  merge_age_map(result.oldest_pending_age_by_kind_seconds,
                durable.oldest_pending_age_by_kind_seconds);
  merge_age_map(result.median_pending_age_by_kind_seconds,
                durable.median_pending_age_by_kind_seconds);
  merge_age_map(result.oldest_pending_age_by_priority_seconds,
                durable.oldest_pending_age_by_priority_seconds);
  merge_age_map(result.median_pending_age_by_priority_seconds,
                durable.median_pending_age_by_priority_seconds);
  merge_age_map(result.oldest_pending_age_by_scope_seconds,
                durable.oldest_pending_age_by_scope_seconds);
  merge_age_map(result.median_pending_age_by_scope_seconds,
                durable.median_pending_age_by_scope_seconds);
  merge_age_map(result.oldest_pending_age_by_risk_class_seconds,
                durable.oldest_pending_age_by_risk_class_seconds);
  merge_age_map(result.median_pending_age_by_risk_class_seconds,
                durable.median_pending_age_by_risk_class_seconds);
  return result;
}

ReviewApplicationResult ReviewApplicationService::apply(
    const MemoryScope& scope, const std::string& review_id,
    ReviewAction action) {
  if (action.actor.empty()) {
    throw std::invalid_argument("review action requires an actor");
  }
  auto hydrated = inspection_.hydrate(scope, review_id);
  if (!action.idempotency_key.empty() &&
      hydrated.item.status != ReviewStatus::Pending &&
      action.kind != ReviewActionKind::Reopen &&
      action.kind != ReviewActionKind::Compensate) {
    ReviewDecision replay;
    replay.status = hydrated.item.status;
    replay.decision = to_string(action.kind);
    replay.decided_by = action.actor;
    replay.note = action.note;
    replay.expected_revision = hydrated.item.revision;
    replay.action_key = action.idempotency_key;
    return reviews_.apply_review_mutation(scope, review_id, std::move(replay), {});
  }
  if (action.expected_revision != 0U &&
      action.expected_revision != hydrated.item.revision) {
    throw std::invalid_argument(
        "review or subject changed after preview: stale revision");
  }
  auto preview = inspection_.preview(scope, review_id, action);
  if (!preview.applicable) {
    throw std::invalid_argument(
        preview.warnings.empty() ? "review action is not applicable"
                                 : preview.warnings.front());
  }
  if (preview.requires_human_authority && !action.human_authority) {
    throw std::invalid_argument("review action requires explicit human authority");
  }
  action = preview.action;
  if (mutates_review_subject(action.kind) && hydrated.subject &&
      !exact_scope(hydrated.subject->scope, scope)) {
    throw std::invalid_argument(
        "review subject mutation requires the exact subject scope");
  }
  if (action.idempotency_key.empty()) {
    action.idempotency_key = preview.idempotency_token;
  } else if (action.idempotency_key != preview.idempotency_token) {
    throw std::invalid_argument(
        "review or subject changed after preview: stale idempotency token");
  }

  ReviewDecision decision;
  decision.decided_by = action.actor;
  decision.note = action.note;
  decision.expected_revision = preview.review_revision;
  decision.action_key = action.idempotency_key;
  decision.application_status = "applied";
  ReviewAtomicMutation mutation;

  switch (action.kind) {
    case ReviewActionKind::Reject: {
      decision.status = ReviewStatus::Rejected;
      decision.decision = "rejected";
      if (hydrated.subject &&
          (hydrated.subject->status == MemoryStatus::Candidate ||
           hydrated.subject->status == MemoryStatus::Hypothesis ||
           hydrated.subject->status == MemoryStatus::DreamArtifact) &&
          (hydrated.candidate.kind == ReviewCandidateKind::DreamArtifact ||
           hydrated.candidate.kind ==
               ReviewCandidateKind::DreamResearchSuccessor ||
           hydrated.candidate.kind == ReviewCandidateKind::General)) {
        MemoryPatch patch;
        patch.status = MemoryStatus::Rejected;
        mutation.memories.push_back(
            patch_memory(hydrated.subject->id, std::move(patch)));
      }
      break;
    }
    case ReviewActionKind::Defer:
      if (!action.deferred_until ||
          *action.deferred_until <= std::chrono::system_clock::now()) {
        throw std::invalid_argument(
            "deferred review requires a future resurface time");
      }
      decision.status = ReviewStatus::Deferred;
      decision.decision = "deferred";
      decision.deferred_until = action.deferred_until;
      break;
    case ReviewActionKind::Approve:
    case ReviewActionKind::EditAndApprove: {
      if (!hydrated.subject) {
        throw std::invalid_argument("review approval requires a subject memory");
      }
      const auto target = approval_status(hydrated, action);
      if (!target) {
        throw std::invalid_argument(
            "review kind requires an explicit typed resolution");
      }
      if (*target == MemoryStatus::Validated && action.evidence_uri.empty()) {
        throw std::invalid_argument(
            "validation requires independently supplied evidence");
      }
      auto patch = edited_patch(action);
      patch.status = *target;
      if (*target == MemoryStatus::Validated) {
        auto provenance = hydrated.subject->provenance;
        provenance.source_type = SourceType::Document;
        provenance.source_uri = action.evidence_uri;
        provenance.created_by = action.actor;
        provenance.evidence_strength = 1.0;
        patch.provenance = std::move(provenance);
      }
      if (hydrated.candidate.kind == ReviewCandidateKind::ProcedureRevision &&
          !patch.content) {
        const auto* candidate = std::get_if<ProcedureRevisionCandidate>(
            &hydrated.candidate.payload);
        if (candidate == nullptr || candidate->proposed_content.empty()) {
          throw std::invalid_argument(
              "procedure revision has no proposed content");
        }
        patch.content = MemoryService::normalize_content(
            candidate->proposed_content);
        MemoryService::redact_secrets(*patch.content);
      }
      mutation.memories.push_back(
          patch_memory(hydrated.subject->id, std::move(patch)));
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      const char* target_name = *target == MemoryStatus::Validated
                                    ? "validated"
                                : *target == MemoryStatus::Active
                                    ? "active"
                                    : "hypothesis";
      decision.decision =
          std::string(action.kind == ReviewActionKind::EditAndApprove
                          ? "edited_and_approved_as_"
                          : "approved_as_") +
          target_name;
      break;
    }
    case ReviewActionKind::Relate:
    case ReviewActionKind::MarkContradiction:
    case ReviewActionKind::KeepBoth: {
      const auto ids = source_ids(hydrated.candidate);
      if (ids.size() < 2U) {
        throw std::invalid_argument(
            "relationship review requires at least two source memories");
      }
      const std::string edge_type =
          action.kind == ReviewActionKind::Relate
              ? "related"
          : action.kind == ReviewActionKind::MarkContradiction
              ? "contradicts"
              : "conflict_warning";
      for (std::size_t index = 1U; index < ids.size(); ++index) {
        GraphEdge edge;
        edge.from_id = ids.front();
        edge.to_id = ids[index];
        edge.type = edge_type;
        edge.confidence = 1.0;
        edge.source = "memory_review";
        mutation.edges.push_back(std::move(edge));
      }
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = to_string(action.kind);
      break;
    }
    case ReviewActionKind::Supersede: {
      const auto ids = source_ids(hydrated.candidate);
      if (action.target_id.empty() ||
          std::find(ids.begin(), ids.end(), action.target_id) == ids.end()) {
        throw std::invalid_argument(
            "supersede requires a target from the reviewed source set");
      }
      for (const auto& id : ids) {
        if (id == action.target_id) continue;
        MemoryPatch patch;
        patch.status = MemoryStatus::Superseded;
        patch.replacement_id = action.target_id;
        mutation.memories.push_back(patch_memory(id, std::move(patch)));
        GraphEdge edge;
        edge.from_id = id;
        edge.to_id = action.target_id;
        edge.type = "superseded_by";
        edge.confidence = 1.0;
        edge.source = "memory_review";
        mutation.edges.push_back(std::move(edge));
      }
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = "superseded";
      break;
    }
    case ReviewActionKind::Merge: {
      const auto* candidate =
          std::get_if<ConsolidationCandidate>(&hydrated.candidate.payload);
      const auto ids = source_ids(hydrated.candidate);
      if ((candidate == nullptr &&
           hydrated.candidate.kind != ReviewCandidateKind::Conflict) ||
          ids.size() < 2U) {
        throw std::invalid_argument(
            "merge requires at least two reviewed source memories");
      }
      std::string content =
          action.edit && action.edit->content
              ? *action.edit->content
              : candidate != nullptr ? candidate->canonical_content
                                     : std::string{};
      content = MemoryService::normalize_content(std::move(content));
      MemoryService::redact_secrets(content);
      if (content.empty()) {
        throw std::invalid_argument("merge requires canonical content");
      }
      MemoryWrite canonical;
      canonical.content = content;
      canonical.type = action.edit && action.edit->type
                           ? *action.edit->type
                           : MemoryType::Observation;
      if (const auto* first = find_memory(hydrated, ids.front())) {
        if (!(action.edit && action.edit->type)) canonical.type = first->type;
      }
      canonical.status = MemoryStatus::Hypothesis;
      canonical.source = "review:canonical_merge";
      canonical.provenance.source_type = SourceType::ProcessedText;
      canonical.provenance.created_by = action.actor;
      canonical.provenance.evidence_strength = 0.0;
      canonical.confidence = 0.5;
      if (action.edit && action.edit->confidence) {
        canonical.confidence = *action.edit->confidence;
      }
      canonical.tags = {"reviewed_canonical"};
      if (action.edit && action.edit->tags) canonical.tags = *action.edit->tags;
      mutation.memories.push_back(create_memory(std::move(canonical)));
      const auto canonical_id = review_created_reference(0U);
      for (const auto& id : ids) {
        if (find_memory(hydrated, id) == nullptr) {
          throw std::invalid_argument("merge source is missing or outside scope: " + id);
        }
        MemoryPatch patch;
        patch.status = MemoryStatus::Superseded;
        patch.replacement_id = canonical_id;
        mutation.memories.push_back(patch_memory(id, std::move(patch)));
        GraphEdge edge;
        edge.from_id = id;
        edge.to_id = canonical_id;
        edge.type = "superseded_by";
        edge.confidence = 1.0;
        edge.source = "memory_review";
        mutation.edges.push_back(std::move(edge));
      }
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = "merged";
      break;
    }
    case ReviewActionKind::NarrowScope: {
      if (!hydrated.subject || !action.edit || !action.edit->scope) {
        throw std::invalid_argument(
            "narrow scope requires a subject and explicit target scope");
      }
      MemoryWrite narrowed;
      narrowed.content = action.edit->content.value_or(hydrated.subject->content);
      narrowed.type = action.edit->type.value_or(hydrated.subject->type);
      narrowed.status = action.edit->target_status.value_or(
          hydrated.subject->status == MemoryStatus::Validated
              ? MemoryStatus::Active
              : hydrated.subject->status);
      narrowed.source = "review:narrow_scope";
      narrowed.provenance = hydrated.subject->provenance;
      narrowed.provenance.created_by = action.actor;
      narrowed.tags = action.edit->tags.value_or(hydrated.subject->tags);
      narrowed.confidence = action.edit->confidence.value_or(
          hydrated.subject->confidence);
      narrowed.quality_score = hydrated.subject->quality_score;
      mutation.memories.push_back(
          create_memory(std::move(narrowed), *action.edit->scope));
      const auto replacement = review_created_reference(0U);
      MemoryPatch patch = edited_patch(action);
      patch.status = MemoryStatus::Superseded;
      patch.replacement_id = replacement;
      mutation.memories.push_back(
          patch_memory(hydrated.subject->id, std::move(patch)));
      GraphEdge edge;
      edge.from_id = hydrated.subject->id;
      edge.to_id = replacement;
      edge.type = "superseded_by";
      edge.confidence = 1.0;
      edge.source = "memory_review";
      mutation.edges.push_back(std::move(edge));
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = "narrowed_scope";
      break;
    }
    case ReviewActionKind::Deprecate: {
      if (!hydrated.subject) {
        throw std::invalid_argument("deprecate requires a subject memory");
      }
      MemoryPatch patch;
      patch.status = MemoryStatus::Deprecated;
      mutation.memories.push_back(
          patch_memory(hydrated.subject->id, std::move(patch)));
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = "deprecated";
      break;
    }
    case ReviewActionKind::Retain: {
      if (!hydrated.subject ||
          hydrated.candidate.kind != ReviewCandidateKind::Staleness ||
          action.note.empty()) {
        throw std::invalid_argument(
            "retain requires a staleness subject and explicit rationale");
      }
      auto tags = hydrated.subject->tags;
      if (std::find(tags.begin(), tags.end(), "review:retained_current") ==
          tags.end()) {
        tags.push_back("review:retained_current");
      }
      MemoryPatch patch;
      patch.tags = std::move(tags);
      mutation.memories.push_back(
          patch_memory(hydrated.subject->id, std::move(patch)));
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = "retained_current";
      break;
    }
    case ReviewActionKind::Revalidate: {
      if (!hydrated.subject || action.evidence_uri.empty()) {
        throw std::invalid_argument(
            "revalidation requires a subject and current evidence URI");
      }
      MemoryPatch patch;
      patch.status = MemoryStatus::Validated;
      auto provenance = hydrated.subject->provenance;
      provenance.source_type = SourceType::Document;
      provenance.source_uri = action.evidence_uri;
      provenance.created_by = action.actor;
      provenance.evidence_strength = 1.0;
      patch.provenance = std::move(provenance);
      mutation.memories.push_back(
          patch_memory(hydrated.subject->id, std::move(patch)));
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = "revalidated";
      break;
    }
    case ReviewActionKind::AttachEvidence: {
      if (!hydrated.subject || action.evidence_uri.empty()) {
        throw std::invalid_argument(
            "attach evidence requires a subject and evidence URI");
      }
      auto provenance = hydrated.subject->provenance;
      provenance.source_type = SourceType::Document;
      provenance.source_uri = action.evidence_uri;
      provenance.created_by = action.actor;
      provenance.evidence_strength = std::max(0.7, provenance.evidence_strength);
      MemoryPatch patch;
      patch.provenance = std::move(provenance);
      mutation.memories.push_back(
          patch_memory(hydrated.subject->id, std::move(patch)));
      mutation.require_domain_mutation = true;
      decision.status = ReviewStatus::Accepted;
      decision.decision = "evidence_attached";
      break;
    }
    case ReviewActionKind::Reopen:
      decision.status = ReviewStatus::Pending;
      decision.decision = "reopen";
      break;
    case ReviewActionKind::Compensate: {
      if (action.target_id.empty() || !action.edit) {
        throw std::invalid_argument(
            "compensation requires target memory and explicit replacement values");
      }
      const auto target_memory = [&]() {
        try {
          return inspection_.get_memory(scope, action.target_id);
        } catch (const std::exception&) {
          throw std::invalid_argument(
              "compensation target is missing or outside the authority scope");
        }
      }();
      if (!exact_scope(target_memory.scope, scope)) {
        throw std::invalid_argument(
            "compensation target requires the exact target scope");
      }
      if (action.edit->target_status || action.edit->replacement_id) {
        throw std::invalid_argument(
            "compensation cannot bypass lifecycle or replacement governance");
      }
      auto patch = edited_patch(action);
      mutation.memories.push_back(
          patch_memory(action.target_id, std::move(patch)));
      mutation.require_domain_mutation = true;
      decision.status = hydrated.item.status;
      decision.decision = "compensate";
      decision.application_status = "compensated";
      break;
    }
  }
  return reviews_.apply_review_mutation(scope, review_id, std::move(decision),
                                        std::move(mutation));
}

}  // namespace qt::memory
