#include "qt/memory/review.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <type_traits>

namespace qt::memory {
namespace {

using Json = nlohmann::json;

template <typename T>
T required_string_array(const Json& value, const char* key) {
  if (!value.contains(key) || !value.at(key).is_array()) return {};
  T result;
  for (const auto& item : value.at(key)) {
    if (!item.is_string()) return {};
    result.push_back(item.get<std::string>());
  }
  return result;
}

std::vector<std::string> memory_ids(const Json& value) {
  auto ids = required_string_array<std::vector<std::string>>(
      value, "source_memory_ids");
  if (ids.empty())
    ids = required_string_array<std::vector<std::string>>(value, "source_ids");
  if (ids.empty())
    ids = required_string_array<std::vector<std::string>>(value, "memory_ids");
  return ids;
}

ReviewCandidateKind kind_for(const std::string& kind) {
  if (kind == "dream_artifact") return ReviewCandidateKind::DreamArtifact;
  if (kind == "dream_research_successor")
    return ReviewCandidateKind::DreamResearchSuccessor;
  if (kind == "semantic_consolidation_candidate" ||
      kind == "semantic_consolidation")
    return ReviewCandidateKind::SemanticConsolidation;
  if (kind == "compression_candidate" || kind == "compression")
    return ReviewCandidateKind::Compression;
  if (kind == "conflict") return ReviewCandidateKind::Conflict;
  if (kind == "procedure_revision" || kind == "failed_procedure")
    return ReviewCandidateKind::ProcedureRevision;
  if (kind == "staleness") return ReviewCandidateKind::Staleness;
  if (kind == "provenance_repair") return ReviewCandidateKind::ProvenanceRepair;
  return ReviewCandidateKind::General;
}

}  // namespace

const char* to_string(const ReviewCandidateKind value) noexcept {
  switch (value) {
    case ReviewCandidateKind::DreamArtifact: return "dream_artifact";
    case ReviewCandidateKind::DreamResearchSuccessor: return "dream_research_successor";
    case ReviewCandidateKind::SemanticConsolidation: return "semantic_consolidation";
    case ReviewCandidateKind::Compression: return "compression";
    case ReviewCandidateKind::Conflict: return "conflict";
    case ReviewCandidateKind::ProcedureRevision: return "procedure_revision";
    case ReviewCandidateKind::General: return "general";
    case ReviewCandidateKind::Staleness: return "staleness";
    case ReviewCandidateKind::ProvenanceRepair: return "provenance_repair";
  }
  return "general";
}

const char* to_string(const ReviewActionKind value) noexcept {
  switch (value) {
    case ReviewActionKind::Approve: return "approve";
    case ReviewActionKind::EditAndApprove: return "edit_and_approve";
    case ReviewActionKind::Reject: return "reject";
    case ReviewActionKind::Defer: return "defer";
    case ReviewActionKind::Reopen: return "reopen";
    case ReviewActionKind::Relate: return "relate";
    case ReviewActionKind::Merge: return "merge";
    case ReviewActionKind::Supersede: return "supersede";
    case ReviewActionKind::MarkContradiction: return "mark_contradiction";
    case ReviewActionKind::KeepBoth: return "keep_both";
    case ReviewActionKind::Retain: return "retain";
    case ReviewActionKind::NarrowScope: return "narrow_scope";
    case ReviewActionKind::Revalidate: return "revalidate";
    case ReviewActionKind::Deprecate: return "deprecate";
    case ReviewActionKind::AttachEvidence: return "attach_evidence";
    case ReviewActionKind::Compensate: return "compensate";
  }
  return "approve";
}

const char* to_string(const ReviewApplicationStatus value) noexcept {
  switch (value) {
    case ReviewApplicationStatus::NotStarted: return "not_started";
    case ReviewApplicationStatus::Applied: return "applied";
    case ReviewApplicationStatus::NoOp: return "no_op";
    case ReviewApplicationStatus::Failed: return "failed";
    case ReviewApplicationStatus::PartiallyApplied: return "partially_applied";
    case ReviewApplicationStatus::Compensated: return "compensated";
  }
  return "not_started";
}

ReviewCandidateKind review_candidate_kind_from_string(const std::string& value) {
  const auto kind = kind_for(value);
  if (kind == ReviewCandidateKind::General && value != "general" &&
      value != "candidate_memory" && value != "candidate") {
    throw std::invalid_argument("invalid review candidate kind: " + value);
  }
  return kind;
}

ReviewActionKind review_action_kind_from_string(const std::string& value) {
#define QT_REVIEW_ACTION(name, value_name) if (value == value_name) return ReviewActionKind::name
  QT_REVIEW_ACTION(Approve, "approve");
  QT_REVIEW_ACTION(EditAndApprove, "edit_and_approve");
  QT_REVIEW_ACTION(Reject, "reject");
  QT_REVIEW_ACTION(Defer, "defer");
  QT_REVIEW_ACTION(Reopen, "reopen");
  QT_REVIEW_ACTION(Relate, "relate");
  QT_REVIEW_ACTION(Merge, "merge");
  QT_REVIEW_ACTION(Supersede, "supersede");
  QT_REVIEW_ACTION(MarkContradiction, "mark_contradiction");
  QT_REVIEW_ACTION(KeepBoth, "keep_both");
  QT_REVIEW_ACTION(Retain, "retain");
  QT_REVIEW_ACTION(NarrowScope, "narrow_scope");
  QT_REVIEW_ACTION(Revalidate, "revalidate");
  QT_REVIEW_ACTION(Deprecate, "deprecate");
  QT_REVIEW_ACTION(AttachEvidence, "attach_evidence");
  QT_REVIEW_ACTION(Compensate, "compensate");
#undef QT_REVIEW_ACTION
  throw std::invalid_argument("invalid review action: " + value);
}

ReviewAction normalize_review_action(ReviewAction action) {
  const auto mismatch = [&]() -> std::invalid_argument {
    return std::invalid_argument(
        "review action payload does not match action kind " +
        std::string(to_string(action.kind)));
  };

  if (!action.payload) {
    switch (action.kind) {
      case ReviewActionKind::Approve:
      case ReviewActionKind::EditAndApprove:
        action.payload = ApproveReviewAction{action.evidence_uri, action.edit};
        break;
      case ReviewActionKind::Reject:
        action.payload = RejectReviewAction{};
        break;
      case ReviewActionKind::Defer:
        action.payload = DeferReviewAction{action.deferred_until};
        break;
      case ReviewActionKind::Reopen:
        action.payload = ReopenReviewAction{};
        break;
      case ReviewActionKind::Relate:
        action.payload = RelateReviewAction{};
        break;
      case ReviewActionKind::Merge:
        action.payload = MergeReviewAction{action.edit};
        break;
      case ReviewActionKind::Supersede:
        action.payload = SupersedeReviewAction{action.target_id};
        break;
      case ReviewActionKind::MarkContradiction:
        action.payload = ContradictionReviewAction{};
        break;
      case ReviewActionKind::KeepBoth:
        action.payload = KeepBothReviewAction{};
        break;
      case ReviewActionKind::Retain:
        action.payload = RetainReviewAction{};
        break;
      case ReviewActionKind::NarrowScope:
        action.payload = NarrowScopeReviewAction{action.edit};
        break;
      case ReviewActionKind::Revalidate:
        action.payload = RevalidateReviewAction{action.evidence_uri};
        break;
      case ReviewActionKind::Deprecate:
        action.payload = DeprecateReviewAction{};
        break;
      case ReviewActionKind::AttachEvidence:
        action.payload = AttachEvidenceReviewAction{action.evidence_uri};
        break;
      case ReviewActionKind::Compensate:
        action.payload = CompensateReviewAction{action.target_id, action.edit};
        break;
    }
    return action;
  }

  switch (action.kind) {
    case ReviewActionKind::Approve:
    case ReviewActionKind::EditAndApprove: {
      const auto* payload = std::get_if<ApproveReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.evidence_uri = payload->evidence_uri;
      action.edit = payload->edit;
      break;
    }
    case ReviewActionKind::Reject:
      if (!std::holds_alternative<RejectReviewAction>(*action.payload)) throw mismatch();
      break;
    case ReviewActionKind::Defer: {
      const auto* payload = std::get_if<DeferReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.deferred_until = payload->until;
      break;
    }
    case ReviewActionKind::Reopen:
      if (!std::holds_alternative<ReopenReviewAction>(*action.payload)) throw mismatch();
      break;
    case ReviewActionKind::Relate:
      if (!std::holds_alternative<RelateReviewAction>(*action.payload)) throw mismatch();
      break;
    case ReviewActionKind::Merge: {
      const auto* payload = std::get_if<MergeReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.edit = payload->edit;
      break;
    }
    case ReviewActionKind::Supersede: {
      const auto* payload = std::get_if<SupersedeReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.target_id = payload->target_id;
      break;
    }
    case ReviewActionKind::MarkContradiction:
      if (!std::holds_alternative<ContradictionReviewAction>(*action.payload)) throw mismatch();
      break;
    case ReviewActionKind::KeepBoth:
      if (!std::holds_alternative<KeepBothReviewAction>(*action.payload)) throw mismatch();
      break;
    case ReviewActionKind::Retain:
      if (!std::holds_alternative<RetainReviewAction>(*action.payload)) throw mismatch();
      break;
    case ReviewActionKind::NarrowScope: {
      const auto* payload = std::get_if<NarrowScopeReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.edit = payload->edit;
      break;
    }
    case ReviewActionKind::Revalidate: {
      const auto* payload = std::get_if<RevalidateReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.evidence_uri = payload->evidence_uri;
      break;
    }
    case ReviewActionKind::Deprecate:
      if (!std::holds_alternative<DeprecateReviewAction>(*action.payload)) throw mismatch();
      break;
    case ReviewActionKind::AttachEvidence: {
      const auto* payload = std::get_if<AttachEvidenceReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.evidence_uri = payload->evidence_uri;
      break;
    }
    case ReviewActionKind::Compensate: {
      const auto* payload = std::get_if<CompensateReviewAction>(&*action.payload);
      if (payload == nullptr) throw mismatch();
      action.target_id = payload->target_id;
      action.edit = payload->edit;
      break;
    }
  }
  return action;
}

const char* to_string(const ReviewBatchJobStatus value) noexcept {
  switch (value) {
    case ReviewBatchJobStatus::Queued: return "queued";
    case ReviewBatchJobStatus::Running: return "running";
    case ReviewBatchJobStatus::Completed: return "completed";
    case ReviewBatchJobStatus::Failed: return "failed";
  }
  return "failed";
}

ReviewApplicationStatus review_application_status_from_string(
    const std::string& value) {
  if (value == "not_started") return ReviewApplicationStatus::NotStarted;
  if (value == "applied") return ReviewApplicationStatus::Applied;
  if (value == "no_op") return ReviewApplicationStatus::NoOp;
  if (value == "failed") return ReviewApplicationStatus::Failed;
  if (value == "partially_applied") return ReviewApplicationStatus::PartiallyApplied;
  if (value == "compensated") return ReviewApplicationStatus::Compensated;
  throw std::invalid_argument("invalid review application status: " + value);
}

double review_queue_score(const ReviewItem& item, const TimePoint now) {
  double score = item.priority == "critical" || item.priority == "urgent"
                     ? 45.0
                 : item.priority == "high" ? 30.0
                 : item.priority == "low" ? 0.0
                                           : 10.0;
  const auto kind = kind_for(item.kind);
  switch (kind) {
    case ReviewCandidateKind::Conflict: score += 40.0; break;
    case ReviewCandidateKind::ProcedureRevision: score += 28.0; break;
    case ReviewCandidateKind::ProvenanceRepair: score += 24.0; break;
    case ReviewCandidateKind::Staleness: score += 20.0; break;
    case ReviewCandidateKind::SemanticConsolidation:
    case ReviewCandidateKind::Compression: score += 15.0; break;
    case ReviewCandidateKind::DreamResearchSuccessor: score += 10.0; break;
    case ReviewCandidateKind::General: score += 8.0; break;
    case ReviewCandidateKind::DreamArtifact: score += 5.0; break;
  }
  if (item.provenance.source_type == SourceType::Unknown &&
      item.provenance.source_uri.empty() && item.provenance.source_hash.empty()) {
    score += 18.0;
  }
  score += std::max(0.0, 1.0 - item.provenance.evidence_strength) * 12.0;
  if (item.queue_signals.critical_type) score += 25.0;
  score += std::min(20.0, item.queue_signals.trust_risk);
  score += std::min(20.0, static_cast<double>(item.queue_signals.access_count) * 2.0);
  score += std::min(20.0,
                    static_cast<double>(item.queue_signals.procedure_failures) *
                        4.0);
  if (item.queue_signals.recent_context) score += 8.0;
  const auto waiting = std::min(
      15.0, std::max(0.0,
                     static_cast<double>(std::chrono::duration_cast<
                                             std::chrono::hours>(
                                             now - item.created_at)
                                             .count()) /
                         48.0));
  score += waiting;
  if (kind == ReviewCandidateKind::Staleness) score += waiting;
  const auto candidate = decode_review_candidate(item);
  if (const auto* conflict =
          std::get_if<ConflictCandidate>(&candidate.payload)) {
    if (conflict->severity == "critical") {
      // A producer-provided low priority must never bury a critical conflict.
      // Keep a little headroom so explicit urgency and waiting age can still
      // provide stable ordering between otherwise critical items.
      score = std::max(score + 25.0, 95.0);
    } else {
      score += conflict->severity == "high" ? 15.0 : 5.0;
    }
  }
  return std::min(100.0, score);
}

ReviewCandidate decode_review_candidate(const ReviewItem& item) {
  ReviewCandidate result;
  result.kind = kind_for(item.kind);
  result.version = item.candidate_version == 0U ? 1U : item.candidate_version;
  const auto value = Json::parse(item.candidate_json, nullptr, false);
  if (value.is_discarded() || !value.is_object()) {
    result.legacy = true;
    result.diagnostics.push_back("candidate payload is not a JSON object");
    result.payload = GeneralReviewCandidate{item.candidate_json, std::nullopt,
                                            std::nullopt, item.tags, {}, item.reason};
    return result;
  }
  if (value.contains("version") && value["version"].is_number_integer() &&
      value["version"].get<std::int64_t>() > 0) {
    result.version = value["version"].get<std::uint32_t>();
  }
  result.legacy = !value.contains("version") || !value.contains("kind");
  if (result.version != 1U) {
    result.diagnostics.push_back("candidate envelope version is unsupported");
  }
  if (value.contains("kind") && value["kind"].is_string() &&
      kind_for(value["kind"].get<std::string>()) != result.kind) {
    result.diagnostics.push_back(
        "candidate envelope kind does not match the review kind");
  }
  const Json& payload = value.contains("payload") && value["payload"].is_object()
                            ? value["payload"]
                            : value;
  for (const char* key : {"source_memory_ids", "source_ids", "memory_ids",
                          "evidence_ids", "tags"}) {
    if (!payload.contains(key)) continue;
    if (!payload[key].is_array() ||
        !std::all_of(payload[key].begin(), payload[key].end(),
                     [](const Json& item) { return item.is_string(); })) {
      result.diagnostics.push_back(std::string("candidate field ") + key +
                                   " must contain only strings");
    }
  }
  switch (result.kind) {
    case ReviewCandidateKind::DreamArtifact:
      result.payload = DreamArtifactCandidate{
          payload.value("artifact_id", payload.value("memory_id", item.subject_id)),
          payload.value("content", std::string{}),
          memory_ids(payload),
          payload.value("cycle_id", std::string{}),
          payload.value("model", std::string{}),
          payload.value("research_required", false)};
      break;
    case ReviewCandidateKind::DreamResearchSuccessor:
      result.payload = DreamResearchCandidate{
          payload.value("dream_id", std::string{}),
          payload.value("research_memory_id", item.subject_id),
          payload.value("evidence_uri", item.provenance.source_uri),
          memory_ids(payload)};
      break;
    case ReviewCandidateKind::SemanticConsolidation:
    case ReviewCandidateKind::Compression:
      result.payload = ConsolidationCandidate{
          memory_ids(payload),
          payload.value("canonical_content", payload.value("content", std::string{})),
          payload.value("proposal", payload.value("relationship", std::string{}))};
      if (std::get<ConsolidationCandidate>(result.payload).memory_ids.size() < 2U) {
        result.diagnostics.push_back(
            "consolidation candidate requires at least two source memories");
      }
      break;
    case ReviewCandidateKind::Conflict:
      result.payload = ConflictCandidate{
          memory_ids(payload),
          payload.value("severity", std::string("normal")),
          payload.value("explanation", item.reason)};
      if (std::get<ConflictCandidate>(result.payload).memory_ids.size() < 2U) {
        result.diagnostics.push_back(
            "conflict candidate requires at least two memories");
      }
      break;
    case ReviewCandidateKind::ProcedureRevision:
      result.payload = ProcedureRevisionCandidate{
          payload.value("current_memory_id", item.subject_id),
          payload.value("proposed_content", payload.value("content", std::string{})),
          required_string_array<std::vector<std::string>>(payload, "evidence_ids")};
      break;
    default: {
      GeneralReviewCandidate general;
      general.content = payload.value("content", std::string{});
      general.tags = payload.value("tags", item.tags);
      general.related_memory_ids =
          required_string_array<std::vector<std::string>>(payload, "memory_ids");
      general.reason = payload.value("reason", item.reason);
      if (payload.contains("type") && payload["type"].is_string()) {
        try {
          general.type =
              memory_type_from_string(payload["type"].get<std::string>());
        } catch (const std::invalid_argument&) {
          result.diagnostics.push_back("candidate contains an invalid memory type");
        }
      }
      if (payload.contains("target_status") &&
          payload["target_status"].is_string()) {
        try {
          general.target_status = memory_status_from_string(
              payload["target_status"].get<std::string>());
        } catch (const std::invalid_argument&) {
          result.diagnostics.push_back(
              "candidate contains an invalid target status");
        }
      }
      result.payload = std::move(general);
      break;
    }
  }
  return result;
}

nlohmann::json review_candidate_json(const ReviewCandidate& candidate) {
  Json payload;
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, DreamArtifactCandidate>) {
      payload = {{"artifact_id", value.artifact_id}, {"content", value.content},
                 {"source_memory_ids", value.source_memory_ids},
                 {"cycle_id", value.cycle_id}, {"model", value.model},
                 {"research_required", value.research_required}};
    } else if constexpr (std::is_same_v<T, DreamResearchCandidate>) {
      payload = {{"dream_id", value.dream_id},
                 {"research_memory_id", value.research_memory_id},
                 {"evidence_uri", value.evidence_uri},
                 {"source_memory_ids", value.source_memory_ids}};
    } else if constexpr (std::is_same_v<T, ConsolidationCandidate>) {
      payload = {{"memory_ids", value.memory_ids},
                 {"canonical_content", value.canonical_content},
                 {"relationship", value.proposed_relationship}};
    } else if constexpr (std::is_same_v<T, ConflictCandidate>) {
      payload = {{"memory_ids", value.memory_ids}, {"severity", value.severity},
                 {"explanation", value.explanation}};
    } else if constexpr (std::is_same_v<T, ProcedureRevisionCandidate>) {
      payload = {{"current_memory_id", value.current_memory_id},
                 {"proposed_content", value.proposed_content},
                 {"evidence_ids", value.evidence_ids}};
    } else {
      payload = {{"content", value.content}, {"tags", value.tags},
                 {"memory_ids", value.related_memory_ids}, {"reason", value.reason}};
      if (value.type) payload["type"] = to_string(*value.type);
      if (value.target_status) payload["target_status"] = to_string(*value.target_status);
    }
  }, candidate.payload);
  return Json{{"version", candidate.version},
              {"kind", to_string(candidate.kind)}, {"payload", std::move(payload)}};
}

std::string review_created_reference(const std::size_t index) {
  return "$created:" + std::to_string(index);
}

}  // namespace qt::memory
