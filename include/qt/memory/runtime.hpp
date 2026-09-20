#pragma once

#include "qt/memory/coordinator.hpp"
#include "qt/memory/database.hpp"
#include "qt/memory/governance_service.hpp"
#include "qt/memory/ingestion_service.hpp"
#include "qt/memory/post_processing_service.hpp"
#include "qt/memory/review_service.hpp"
#include "qt/memory/retrieval_engine.hpp"

#include <memory>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qt::memory {

struct RetrievalPreview {
  std::vector<RecalledMemory> memories;
  std::vector<RetrievalCandidate> code;
  ContextPack context;
};

struct MemoryDreamingOptions {
  bool enabled = false;
  std::string trigger = "post_turn";
  std::int64_t interval_seconds = 3600;
  std::int64_t idle_seconds = 300;
  std::size_t candidate_threshold = 1U;
  std::string model;
  std::int64_t timeout_ms = 600000;
  std::size_t max_model_input_tokens = 6000U;
  std::size_t max_candidates = 8U;
  std::string research_policy = "disabled";
  std::string promotion_policy = "review_only";
  std::string verifier_model;
};

struct MemoryRuntimeOptions {
  std::string agent_home;
  std::string cwd;
  std::string session;
  std::string agent;
  MemoryCoordinatorSettings coordinator;
  app::runtime::IsolatedAgentRunnerOptions isolated_agent;
  DatabaseOptions database;
  bool automatic_capture = true;
  std::string capture_post_process = "turn_end";
  std::size_t max_evidence_characters = 32000U;
  std::string consolidation_model;
  std::int64_t consolidation_timeout_ms = 600000;
  std::size_t consolidation_max_model_input_tokens = 6000U;
  std::vector<std::string> consolidation_model_discovery_roots;
  MemoryDreamingOptions dreaming;
  RetrievalEngineOptions retrieval;
  std::optional<MemoryScope> initial_scope;
  bool background_processing = true;
};

MemoryScope derive_runtime_memory_scope(const std::string& cwd,
                                        const std::string& session = {},
                                        const std::string& agent = {});

struct CompletedEpisodeMetadata {
  bool ok = false;
  std::uint64_t turn_index = 0U;
  std::size_t tool_request_count = 0U;
  std::size_t tool_error_count = 0U;
  std::string error;
  std::string model_provider;
  std::string model_id;
  std::int64_t input_tokens = 0;
  std::int64_t output_tokens = 0;
  std::vector<std::string> recalled_memory_ids;
};

struct MemoryDreamingStatus {
  bool enabled = false;
  std::string trigger;
  std::int64_t idle_seconds = 0;
  std::int64_t idle_for_seconds = 0;
  bool waiting_for_idle = false;
  bool running = false;
  std::size_t pending_runs = 0U;
  std::size_t completed_runs = 0U;
  std::size_t failed_runs = 0U;
  TimePoint last_started_at{};
  TimePoint last_completed_at{};
  std::string last_error;
  std::string last_dream_id;
  std::string last_review_id;
};

class MemoryRuntime final {
 public:
  explicit MemoryRuntime(MemoryRuntimeOptions options);
  ~MemoryRuntime();
  MemoryRuntime(const MemoryRuntime&) = delete;
  MemoryRuntime& operator=(const MemoryRuntime&) = delete;

  MemoryService& memories() noexcept { return memories_; }
  GovernanceService& governance() noexcept { return governance_; }
  RecallService& recall() noexcept { return recall_; }
  ContextService& context() noexcept { return context_; }
  MemoryIngestionService& ingestion() noexcept { return ingestion_; }
  PostProcessingService& post_processing() noexcept {
    return post_processing_;
  }
  MemoryCoordinator& coordinator() noexcept { return coordinator_; }
  const MemoryScope& scope() const noexcept { return scope_; }
  std::size_t schema_version() const noexcept;
  std::size_t memory_count() const;
  MemoryStatistics statistics() const;
  std::vector<GraphEdge> dashboard_graph_edges(GraphQuery query) const;
  std::vector<SourceRecord> dashboard_sources() const;
  std::vector<SourceEvent> dashboard_source_events(
      const std::string& source_id) const;
  std::vector<ReviewItem> review_items(const ReviewQuery& query) const;
  HydratedReview review_detail(const std::string& id) const;
  ReviewMutationPreview preview_review(const std::string& id,
                                       ReviewAction action) const;
  ReviewApplicationResult apply_review_action(const std::string& id,
                                               ReviewAction action);
  ReviewBatchPreview preview_review_batch(
      const std::vector<ReviewBatchItem>& items) const;
  ReviewBatchResult apply_review_batch(std::vector<ReviewBatchItem> items,
                                       std::string preview_token);
  ReviewBatchJob start_review_batch(std::vector<ReviewBatchItem> items,
                                    std::string preview_token);
  ReviewBatchJob review_batch_job(const std::string& id) const;
  std::vector<ReviewActionRecord> review_history(
      const std::optional<std::string>& id, std::size_t limit) const;
  std::optional<ReviewViewState> load_review_view_state() const;
  void save_review_view_state(ReviewViewState state);
  void clear_review_view_state();
  ReviewStatistics review_statistics() const;
  std::vector<ReviewItem> pending_reviews(std::size_t limit) const;
  std::pair<ProcedureRun, MemoryEntry> record_feedback(
      ProcedureFeedback feedback);
  void note_activity();
  void set_foreground_active(bool active);
  void set_session_context(std::string session, std::string agent);
  void note_recall(std::uint64_t turn_index,
                   const std::vector<std::string>& memory_ids);
  void record_lifecycle_event(std::string event_type, std::string source,
                              std::string payload_json,
                              std::string deduplication_key);
  void enqueue_completed_episode(const std::vector<ai::Message>& messages,
                                 std::string source_uri,
                                 CompletedEpisodeMetadata metadata = {});
  bool enqueue_dream_cycle(
      std::string trigger = "manual",
      std::optional<DreamResearchEvidence> evidence = std::nullopt);
  DreamArtifactResult add_dream_research(
      std::string dream_id, DreamResearchEvidence evidence);
  MemoryDreamingStatus dreaming_status() const;
  RetrievalEngineStatus retrieval_status() const { return retrieval_.status(); }
  RetrievalPreview preview_retrieval(RecallRequest request, std::size_t token_budget);
  std::optional<std::int64_t> current_retrieval_revision(
      const std::string& memory_id, RecallPolicy policy,
      bool include_global_user = false) const;
  RetrievalMaintenanceReport rebuild_retrieval(
      std::optional<std::string> owner_key = std::nullopt);

 private:
  std::vector<RetrievalCandidate> code_candidates(
      const MemoryScope&, const RecallRequest&, const std::vector<RecalledMemory>&);
  MemoryRuntimeOptions options_;
  MemoryScope scope_;
  SQLiteMemoryDatabase database_;
  RetrievalEngine retrieval_;
  MemoryService memories_;
  GovernanceService governance_;
  RecallService recall_;
  ContextService context_;
  MemoryIngestionService ingestion_;
  PostProcessingService post_processing_;
  ReviewInspectionService review_inspection_;
  ReviewApplicationService review_application_;
  std::shared_ptr<app::runtime::IsolatedAgentRunner> isolated_runner_;
  MemoryCoordinator coordinator_;
  struct PendingEpisode {
    MemoryScope scope;
    std::string content;
    std::string source_uri;
    std::string deduplication_key;
    std::string repository_state_hash;
    CompletedEpisodeMetadata metadata;
    std::size_t attempts = 0U;
    std::filesystem::path spool_path;
  };
  struct PendingDream {
    MemoryScope scope;
    std::string cycle_id;
    std::string trigger;
    std::optional<DreamResearchEvidence> evidence;
  };
  void process_episodes();
  void recover_spooled_episodes();
  void recover_completed_episodes();
  std::string completed_episode_payload(const PendingEpisode& episode) const;
  void run_dream_cycle(PendingDream dream);
  nlohmann::json auto_validate_dream_research(
      const MemoryScope& scope, const std::string& memory_id,
      const std::string& review_id, const nlohmann::json& documents,
      const std::string& evidence_hash, const std::string& dream_model);
  void run_semantic_consolidation(const MemoryScope& scope,
                                  std::uint64_t turn_index);
  mutable std::mutex episode_mutex_;
  std::condition_variable episode_condition_;
  std::deque<PendingEpisode> pending_episodes_;
  std::deque<PendingDream> pending_dreams_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::string>>
      recalled_by_turn_;
  std::unordered_set<std::string> pending_episode_keys_;
  MemoryDreamingStatus dreaming_status_;
  std::size_t new_candidate_count_ = 0U;
  std::chrono::steady_clock::time_point next_dream_at_{};
  std::chrono::steady_clock::time_point last_activity_at_{};
  bool foreground_active_ = false;
  bool stopping_ = false;
  bool episode_worker_active_ = false;
  std::shared_ptr<foundation::ProcessCancellationToken> worker_cancellation_;
  std::thread episode_worker_;
  struct PendingReviewBatch {
    std::string id;
    std::vector<ReviewBatchItem> items;
    std::string preview_token;
  };
  void process_review_batches();
  ReviewBatchResult apply_review_batch_impl(
      std::vector<ReviewBatchItem> items, std::string preview_token,
      const std::function<void(std::size_t)>& progress);
  mutable std::mutex review_batch_mutex_;
  std::condition_variable review_batch_condition_;
  std::deque<PendingReviewBatch> pending_review_batches_;
  std::unordered_map<std::string, ReviewBatchJob> review_batch_jobs_;
  std::uint64_t next_review_batch_id_ = 1U;
  bool review_batch_stopping_ = false;
  std::thread review_batch_worker_;
};

}  // namespace qt::memory
