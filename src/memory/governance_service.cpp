#include "qt/memory/governance_service.hpp"

#include "qt/memory/scope.hpp"

#include <stdexcept>

namespace qt::memory {

GovernanceService::GovernanceService(MemoryService& memories,
                                     AuditRepository& audit,
                                     GraphRepository& graph,
                                     LifecyclePolicy policy)
    : memories_(memories), audit_(audit), graph_(graph), policy_(policy) {}

bool GovernanceService::can_transition(MemoryStatus from, MemoryStatus to,
                                       const LifecyclePolicy& policy,
                                       bool has_evidence,
                                       bool has_replacement) noexcept {
  if (from == to) return true;
  if (from == MemoryStatus::Rejected) return false;
  if (from == MemoryStatus::Expired && to != MemoryStatus::Rejected) return false;
  if (to == MemoryStatus::Rejected) return true;
  if (to == MemoryStatus::Expired) return policy.allow_expiration;
  if (to == MemoryStatus::Superseded) return has_replacement;
  if (to == MemoryStatus::Deprecated) {
    return from == MemoryStatus::Candidate || from == MemoryStatus::Hypothesis ||
           from == MemoryStatus::Active || from == MemoryStatus::Validated;
  }
  if (to == MemoryStatus::Active) {
    return from == MemoryStatus::Candidate || from == MemoryStatus::Hypothesis;
  }
  if (to == MemoryStatus::Validated) {
    return (!policy.require_evidence_for_validation || has_evidence) &&
           (from == MemoryStatus::Candidate || from == MemoryStatus::Hypothesis ||
            from == MemoryStatus::Active);
  }
  if (to == MemoryStatus::Hypothesis) {
    return from == MemoryStatus::Candidate ||
           from == MemoryStatus::DreamArtifact;
  }
  return false;
}

bool GovernanceService::trusted_for_recall(MemoryStatus status) noexcept {
  return status == MemoryStatus::Active || status == MemoryStatus::Validated;
}

MemoryEntry GovernanceService::transition(const MemoryScope& scope,
                                          const LifecycleRequest& request) {
  if (request.actor.empty() || request.reason.empty()) {
    throw std::invalid_argument("governance transition requires actor and reason");
  }
  const auto before = memories_.get(scope, request.id);
  if (scope_key(before.scope) != scope_key(scope)) {
    throw std::invalid_argument(
        "governance transition subject is outside the exact authority scope");
  }
  const bool replacement = !request.replacement_id.empty();
  const bool has_evidence = request.has_evidence && !request.evidence_uri.empty();
  if (!can_transition(before.status, request.status, policy_, has_evidence,
                      replacement)) {
    throw std::invalid_argument("invalid memory lifecycle transition");
  }
  if (replacement) {
    const auto target = memories_.get(scope, request.replacement_id);
    if (scope_key(target.scope) != scope_key(scope)) {
      throw std::invalid_argument(
          "governance replacement is outside the exact authority scope");
    }
    if (target.id == before.id) throw std::invalid_argument("memory cannot replace itself");
  }
  MemoryPatch patch;
  patch.status = request.status;
  if (replacement) patch.replacement_id = request.replacement_id;
  if (request.confidence) patch.confidence = request.confidence;
  AuditEventWrite audit;
  audit.actor = request.actor;
  audit.action = std::string("memory.lifecycle.") + to_string(request.status);
  audit.reason = request.reason;
  audit.before_id = before.id;
  audit.after_id = replacement ? request.replacement_id : request.id;
  audit.evidence_uri = request.evidence_uri;
  std::optional<GraphEdge> edge;
  if (replacement) {
    edge = GraphEdge{ {}, before.id, request.replacement_id, "superseded_by",
                      1.0, "governance", {} };
  }
  return memories_.apply_governed_update(
      scope, request.id, before.status, std::move(patch), std::move(edge),
      std::move(audit));
}

MemoryEntry GovernanceService::validate(const MemoryScope& scope,
                                        const std::string& id,
                                        std::string actor,
                                        std::string reason,
                                        std::string evidence_uri) {
  LifecycleRequest request;
  request.id = id;
  request.status = MemoryStatus::Validated;
  request.actor = std::move(actor);
  request.reason = std::move(reason);
  request.evidence_uri = std::move(evidence_uri);
  request.has_evidence = !request.evidence_uri.empty();
  return transition(scope, request);
}

MemoryEntry GovernanceService::supersede(const MemoryScope& scope,
                                         const std::string& id,
                                         const std::string& replacement_id,
                                         std::string actor,
                                         std::string reason) {
  LifecycleRequest request;
  request.id = id;
  request.status = MemoryStatus::Superseded;
  request.replacement_id = replacement_id;
  request.actor = std::move(actor);
  request.reason = std::move(reason);
  return transition(scope, request);
}

}  // namespace qt::memory
