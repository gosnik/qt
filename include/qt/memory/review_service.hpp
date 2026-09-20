#pragma once

#include "qt/memory/memory_service.hpp"
#include "qt/memory/review.hpp"

namespace qt::memory {

struct ReviewGovernancePolicy {
  bool allow_dream_hypothesis_promotion = false;
};

class ReviewInspectionService final {
 public:
  ReviewInspectionService(MemoryService& memories, ReviewRepository& reviews,
                          GraphRepository& graph,
                          ProcedureRepository& procedures,
                          ReviewGovernancePolicy policy = {})
      : memories_(memories), reviews_(reviews), graph_(graph),
        procedures_(procedures), policy_(policy) {}

  std::vector<ReviewItem> query(const MemoryScope& scope,
                                const ReviewQuery& query) const;
  HydratedReview hydrate(const MemoryScope& scope,
                         const std::string& review_id) const;
  MemoryEntry get_memory(const MemoryScope& scope,
                         const std::string& memory_id) const;
  ReviewMutationPreview preview(const MemoryScope& scope,
                                const std::string& review_id,
                                ReviewAction action) const;
  ReviewStatistics statistics(const MemoryScope& scope) const;
  // Statistics for the queue the Review Inbox lists: the exact scope merged
  // with its canonical durable scope. Use statistics() to read one scope only.
  ReviewStatistics statistics_including_durable(
      const MemoryScope& exact_scope) const;

 private:
  MemoryService& memories_;
  ReviewRepository& reviews_;
  GraphRepository& graph_;
  ProcedureRepository& procedures_;
  ReviewGovernancePolicy policy_;
};

class ReviewApplicationService final {
 public:
  ReviewApplicationService(ReviewInspectionService& inspection,
                           ReviewRepository& reviews)
      : inspection_(inspection), reviews_(reviews) {}

  ReviewApplicationResult apply(const MemoryScope& scope,
                                const std::string& review_id,
                                ReviewAction action);

 private:
  ReviewInspectionService& inspection_;
  ReviewRepository& reviews_;
};

}  // namespace qt::memory
