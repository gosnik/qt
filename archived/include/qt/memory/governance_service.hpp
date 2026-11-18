#pragma once

#include "qt/memory/memory_service.hpp"

#include <optional>
#include <string>

namespace qt::memory {

struct LifecyclePolicy {
  bool require_evidence_for_validation = true;
  bool allow_expiration = true;
};

struct LifecycleRequest {
  std::string id;
  MemoryStatus status = MemoryStatus::Hypothesis;
  std::string replacement_id;
  std::optional<double> confidence;
  bool has_evidence = false;
  std::string actor;
  std::string reason;
  std::string evidence_uri;
};

class GovernanceService {
 public:
  GovernanceService(MemoryService& memories, AuditRepository& audit,
                    GraphRepository& graph, LifecyclePolicy policy = {});

  MemoryEntry transition(const MemoryScope& scope,
                         const LifecycleRequest& request);
  MemoryEntry validate(const MemoryScope& scope, const std::string& id,
                       std::string actor, std::string reason,
                       std::string evidence_uri);
  MemoryEntry supersede(const MemoryScope& scope, const std::string& id,
                        const std::string& replacement_id, std::string actor,
                        std::string reason);
  static bool can_transition(MemoryStatus from, MemoryStatus to,
                             const LifecyclePolicy& policy, bool has_evidence,
                             bool has_replacement) noexcept;
  static bool trusted_for_recall(MemoryStatus status) noexcept;

 private:
  MemoryService& memories_;
  AuditRepository& audit_;
  GraphRepository& graph_;
  LifecyclePolicy policy_;
};

}  // namespace qt::memory
