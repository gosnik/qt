#pragma once

#include "qt/memory/models.hpp"
#include "qt/memory/retrieval_models.hpp"
#include "qt/memory/scope.hpp"
#include "qt/memory/review.hpp"

#include <cstddef>
#include <set>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qt::memory {

class MemoryRepository {
 public:
  virtual ~MemoryRepository() = default;
  virtual std::string add(const MemoryScope& scope, MemoryWrite write) = 0;
  virtual std::vector<MemoryEntry> query(const MemoryScope& scope,
                                         const MemoryQuery& query) const = 0;
  virtual MemoryEntry get(const MemoryScope& scope, const std::string& id) const = 0;
  // Zero means no revision authority is available. Dense evidence must fail
  // closed when its captured representation revision cannot be verified.
  virtual std::int64_t retrieval_revision(const MemoryScope&, const std::string&) const {
    return 0;
  }
  virtual std::optional<MemoryEntry> find_for_recall(const MemoryScope& scope,
      const std::string& id, RecallPolicy policy, bool include_global_user) const {
    try {
      auto memory = get(scope, id);
      if (scope_can_access(scope, memory.scope, policy, include_global_user)) return memory;
      return std::nullopt;
    }
    catch (const std::invalid_argument&) { return std::nullopt; }
  }
  virtual MemoryEntry update(const MemoryScope& scope, const std::string& id,
                             const MemoryPatch& patch) = 0;
  // Implementations with transactional graph/audit support may override this
  // to commit the lifecycle patch and its related records as one unit.
  virtual MemoryEntry apply_governed_update(
      const MemoryScope&, const std::string&, MemoryStatus,
      const MemoryPatch&, std::optional<GraphEdge>, AuditEventWrite) {
    throw std::logic_error("atomic governed update is not supported");
  }
  virtual bool forget(const MemoryScope& scope, const std::string& id) = 0;
  virtual std::vector<MemoryRevision> revisions(const MemoryScope& scope,
                                                const std::string& memory_id) const = 0;
  virtual void record_access(const MemoryScope& scope,
                             const std::vector<std::string>& memory_ids) = 0;
  virtual std::size_t size(const MemoryScope& scope) const = 0;
};

class StructuredMemoryRepository {
 public:
  virtual ~StructuredMemoryRepository() = default;
  virtual std::optional<MemoryAssertion> assertion(
      const MemoryScope&, const std::string&,
      std::optional<TimePoint> = std::nullopt) const {
    return std::nullopt;
  }
  virtual std::vector<MemoryEntity> find_entities(const MemoryScope&, const std::string&,
      RecallPolicy, bool include_global_user, std::size_t limit) const = 0;
  virtual std::vector<MemoryAssertion> query_assertions(const MemoryScope&,
      const std::string& subject_id, const std::string& predicate, const std::string& object_id,
      std::optional<TimePoint> as_of, RecallPolicy, bool include_global_user,
      std::size_t limit) const = 0;
};

class GraphRepository {
 public:
  virtual ~GraphRepository() = default;
  virtual std::string add_edge(const MemoryScope& scope, GraphEdge edge) = 0;
  virtual std::vector<GraphEdge> query_edges(const MemoryScope& scope,
                                             const GraphQuery& query) const = 0;
};

class ProcedureRepository {
 public:
  virtual ~ProcedureRepository() = default;
  virtual std::pair<ProcedureRun, MemoryEntry> add_feedback(
      const MemoryScope& scope, ProcedureFeedback feedback) = 0;
  virtual ProcedureFeedbackResult add_feedback_once(
      const MemoryScope& scope, ProcedureFeedback feedback) = 0;
  virtual std::vector<ProcedureRun> runs(const MemoryScope& scope,
                                         const std::string& procedure_id) const = 0;
};

class SourceRepository {
 public:
  virtual ~SourceRepository() = default;
  virtual void record_event(const MemoryScope& scope, SourceRecord source,
                            SourceEvent event) = 0;
  virtual std::vector<SourceRecord> sources(const MemoryScope& scope) const = 0;
  virtual std::vector<SourceEvent> source_events(
      const MemoryScope& scope, const std::string& source_id) const = 0;
};

class ReviewRepository {
 public:
  virtual ~ReviewRepository() = default;
  virtual std::string add_review(const MemoryScope& scope,
                                 ReviewItemWrite write) = 0;
  virtual std::vector<ReviewItem> reviews(const MemoryScope& scope,
                                          ReviewStatus status,
                                          std::size_t limit) const = 0;
  virtual ReviewItem review(const MemoryScope& scope,
                            const std::string& id) const = 0;
  virtual std::vector<ReviewItem> query_reviews(
      const MemoryScope& scope, const ReviewQuery& query) const = 0;
  // Authority checks must cover the entire unresolved queue, not a UI page.
  // Persistent implementations should read one consistent snapshot.
  virtual std::vector<std::string> critical_conflict_memory_ids(
      const MemoryScope& scope) const {
    ReviewQuery query;
    query.statuses = {ReviewStatus::Pending, ReviewStatus::Deferred};
    query.kinds = {"conflict"};
    query.limit = 256;
    std::set<std::string> ids;
    for (;;) {
      const auto page = query_reviews(scope, query);
      for (const auto& item : page) {
        const auto candidate = decode_review_candidate(item);
        const auto* conflict = std::get_if<ConflictCandidate>(&candidate.payload);
        if (conflict == nullptr || conflict->severity != "critical") continue;
        if (!item.subject_id.empty()) ids.insert(item.subject_id);
        ids.insert(conflict->memory_ids.begin(), conflict->memory_ids.end());
      }
      if (page.size() < query.limit) break;
      query.offset += page.size();
    }
    return {ids.begin(), ids.end()};
  }
  virtual ReviewItem decide_review(const MemoryScope& scope,
                                   const std::string& id,
                                   ReviewDecision decision) = 0;
  virtual ReviewApplicationResult apply_review_mutation(
      const MemoryScope& scope, const std::string& id, ReviewDecision decision,
      ReviewAtomicMutation mutation) = 0;
  virtual std::vector<ReviewActionRecord> review_history(
      const MemoryScope& scope, const std::optional<std::string>& review_id,
      std::size_t limit) const = 0;
  virtual std::size_t resurface_due_reviews(const MemoryScope& scope,
                                            TimePoint now) = 0;
  virtual std::optional<ReviewViewState> load_review_view_state(
      const MemoryScope& scope) const = 0;
  virtual void save_review_view_state(const MemoryScope& scope,
                                      ReviewViewState state) = 0;
  virtual void clear_review_view_state(const MemoryScope& scope) = 0;
  virtual ReviewStatistics review_statistics(const MemoryScope& scope) const = 0;
};

class AuditRepository {
 public:
  virtual ~AuditRepository() = default;
  virtual std::string record_audit(const MemoryScope& scope,
                                   AuditEventWrite write) = 0;
  virtual std::vector<AuditEvent> audit_events(const MemoryScope& scope,
                                               std::size_t limit) const = 0;
};

class SessionEventRepository {
 public:
  virtual ~SessionEventRepository() = default;
  virtual std::string record_session_event(const MemoryScope& scope,
                                           SessionEventWrite write) = 0;
  virtual SessionEventRecordResult record_session_event_once(
      const MemoryScope& scope, SessionEventWrite write) = 0;
  virtual std::vector<SessionEvent> session_events(
      const MemoryScope& scope, const std::string& event_type,
      std::size_t limit, std::size_t offset = 0) const = 0;
};

class BlockRepository {
 public:
  virtual ~BlockRepository() = default;
  virtual ContextBlock get_block(const MemoryScope& scope,
                                 const std::string& id) const = 0;
  virtual ContextBlock update_block(const MemoryScope& scope,
                                    const std::string& id,
                                    std::string content) = 0;
  virtual std::vector<ContextBlock> blocks(const MemoryScope& scope) const = 0;
};

class StateRepository {
 public:
  virtual ~StateRepository() = default;
  virtual nlohmann::json export_state(const MemoryScope& scope) const = 0;
  virtual void import_state(const MemoryScope& scope,
                            const nlohmann::json& state,
                            bool replace_existing) = 0;
  virtual std::size_t schema_version() const noexcept = 0;
};

class MaintenanceRepository {
 public:
  virtual ~MaintenanceRepository() = default;
  virtual RetentionReport enforce_retention(const MemoryScope& scope,
                                            const RetentionPolicy& policy) = 0;
  virtual RedactionScrubReport scrub_sensitive_data(
      const MemoryScope& scope, const RedactionScrubPolicy& policy) = 0;
};

class StatisticsRepository {
 public:
  virtual ~StatisticsRepository() = default;
  virtual MemoryStatistics statistics(const MemoryScope& scope,
                                      RecallPolicy policy,
                                      bool include_global_user) const = 0;
};

}  // namespace qt::memory
