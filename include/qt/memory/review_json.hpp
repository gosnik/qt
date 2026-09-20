#pragma once

#include "qt/memory/review.hpp"

#include <nlohmann/json.hpp>

namespace qt::memory {

nlohmann::json review_memory_json(const MemoryEntry& memory);
nlohmann::json review_item_json(const ReviewItem& item);
nlohmann::json hydrated_review_json(const HydratedReview& review);
nlohmann::json review_preview_json(const ReviewMutationPreview& preview);
nlohmann::json review_application_json(const ReviewApplicationResult& result);
nlohmann::json review_action_record_json(const ReviewActionRecord& record);
nlohmann::json review_batch_preview_json(const ReviewBatchPreview& preview);
nlohmann::json review_batch_result_json(const ReviewBatchResult& result);
nlohmann::json review_batch_job_json(const ReviewBatchJob& job);
nlohmann::json review_statistics_json(const ReviewStatistics& statistics);

ReviewQuery review_query_from_json(const nlohmann::json& input);
ReviewAction review_action_from_json(const nlohmann::json& input,
                                     std::string actor,
                                     bool human_authority);

}  // namespace qt::memory
