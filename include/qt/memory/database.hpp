#pragma once

#include "qt/memory/repositories.hpp"
#include "qt/memory/retrieval_models.hpp"
#include "qt/memory/retrieval_graph.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

struct sqlite3;

namespace qt::memory {

struct DatabaseOptions {
  std::chrono::milliseconds busy_timeout{3000};
  std::size_t busy_retries = 6;
  std::chrono::milliseconds retry_base_delay{4};
};

struct RetrievalMaintenanceReport {
  std::size_t owners = 0;
  std::size_t chunks_invalidated = 0;
  std::size_t vectors_invalidated = 0;
  std::size_t jobs_superseded = 0;
  std::size_t jobs_enqueued = 0;
};

class SQLiteMemoryDatabase final : public MemoryRepository,
                                   public StructuredMemoryRepository,
                                   public GraphRepository,
                                   public ProcedureRepository,
                                   public SourceRepository,
                                   public ReviewRepository,
                                   public AuditRepository,
                                   public SessionEventRepository,
                                   public BlockRepository,
                                   public StateRepository,
                                   public MaintenanceRepository,
                                   public StatisticsRepository,
                                   public CrossStoreLinkRepository {
 public:
  static constexpr std::size_t kSchemaVersion = 7;

  explicit SQLiteMemoryDatabase(std::filesystem::path agent_home,
                                DatabaseOptions options = {});
  ~SQLiteMemoryDatabase() override;
  SQLiteMemoryDatabase(const SQLiteMemoryDatabase&) = delete;
  SQLiteMemoryDatabase& operator=(const SQLiteMemoryDatabase&) = delete;

  const std::filesystem::path& root() const noexcept { return root_; }
  const std::filesystem::path& path() const noexcept { return path_; }

  std::string add(const MemoryScope&, MemoryWrite) override;
  std::pair<std::string, bool> add_capture_candidate_once(
      const MemoryScope&, MemoryWrite, SessionEventWrite claim);
  std::vector<MemoryEntry> query(const MemoryScope&, const MemoryQuery&) const override;
  MemoryEntry get(const MemoryScope&, const std::string&) const override;
  std::optional<MemoryEntry> find_for_recall(const MemoryScope&, const std::string&,
      RecallPolicy, bool include_global_user) const override;
  MemoryEntry update(const MemoryScope&, const std::string&, const MemoryPatch&) override;
  MemoryEntry apply_governed_update(const MemoryScope&, const std::string&,
      MemoryStatus, const MemoryPatch&, std::optional<GraphEdge>, AuditEventWrite) override;
  bool forget(const MemoryScope&, const std::string&) override;
  std::vector<MemoryRevision> revisions(const MemoryScope&, const std::string&) const override;
  void record_access(const MemoryScope&, const std::vector<std::string>&) override;
  std::size_t size(const MemoryScope&) const override;

  std::string add_edge(const MemoryScope&, GraphEdge) override;
  std::vector<GraphEdge> query_edges(const MemoryScope&, const GraphQuery&) const override;
  std::pair<ProcedureRun, MemoryEntry> add_feedback(const MemoryScope&, ProcedureFeedback) override;
  ProcedureFeedbackResult add_feedback_once(const MemoryScope&, ProcedureFeedback) override;
  std::vector<ProcedureRun> runs(const MemoryScope&, const std::string&) const override;
  void record_event(const MemoryScope&, SourceRecord, SourceEvent) override;
  std::vector<SourceRecord> sources(const MemoryScope&) const override;
  std::vector<SourceEvent> source_events(const MemoryScope&, const std::string&) const override;
  std::string add_review(const MemoryScope&, ReviewItemWrite) override;
  std::vector<ReviewItem> reviews(const MemoryScope&, ReviewStatus, std::size_t) const override;
  ReviewItem review(const MemoryScope&, const std::string&) const override;
  std::vector<ReviewItem> query_reviews(const MemoryScope&,
                                        const ReviewQuery&) const override;
  std::vector<std::string> critical_conflict_memory_ids(
      const MemoryScope&) const override;
  ReviewItem decide_review(const MemoryScope&, const std::string&, ReviewDecision) override;
  ReviewApplicationResult apply_review_mutation(
      const MemoryScope&, const std::string&, ReviewDecision,
      ReviewAtomicMutation) override;
  std::vector<ReviewActionRecord> review_history(
      const MemoryScope&, const std::optional<std::string>&,
      std::size_t) const override;
  std::size_t resurface_due_reviews(const MemoryScope&, TimePoint) override;
  std::optional<ReviewViewState> load_review_view_state(
      const MemoryScope&) const override;
  void save_review_view_state(const MemoryScope&, ReviewViewState) override;
  void clear_review_view_state(const MemoryScope&) override;
  ReviewStatistics review_statistics(const MemoryScope&) const override;
  std::string record_audit(const MemoryScope&, AuditEventWrite) override;
  std::vector<AuditEvent> audit_events(const MemoryScope&, std::size_t) const override;
  std::string record_session_event(const MemoryScope&, SessionEventWrite) override;
  SessionEventRecordResult record_session_event_once(const MemoryScope&,
                                                      SessionEventWrite) override;
  std::vector<SessionEvent> session_events(const MemoryScope&, const std::string&,
                                           std::size_t, std::size_t) const override;
  ContextBlock get_block(const MemoryScope&, const std::string&) const override;
  ContextBlock update_block(const MemoryScope&, const std::string&, std::string) override;
  std::vector<ContextBlock> blocks(const MemoryScope&) const override;
  nlohmann::json export_state(const MemoryScope&) const override;
  nlohmann::json export_state(const MemoryScope&, const ExportLimits&) const;
  void import_state(const MemoryScope&, const nlohmann::json&, bool) override;
  RetentionReport enforce_retention(const MemoryScope&,
                                    const RetentionPolicy&) override;
  RedactionScrubReport scrub_sensitive_data(
      const MemoryScope&, const RedactionScrubPolicy&) override;
  std::size_t schema_version() const noexcept override { return schema_version_; }
  std::string integrity_check() const;
  MemoryStatistics statistics(const MemoryScope&, RecallPolicy,
                              bool include_global_user) const override;

  MemoryEntity add_entity(const MemoryScope&, MemoryEntity);
  std::optional<MemoryAssertion> assertion(
      const MemoryScope&, const std::string&,
      std::optional<TimePoint> as_of = std::nullopt) const override;
  std::vector<MemoryEntity> find_entities(const MemoryScope&, const std::string&,
      RecallPolicy = RecallPolicy::Balanced, bool include_global_user = false,
      std::size_t limit = 50) const override;
  std::vector<MemoryAssertion> query_assertions(const MemoryScope&,
      const std::string& subject_id, const std::string& predicate, const std::string& object_id,
      std::optional<TimePoint> as_of, RecallPolicy, bool include_global_user,
      std::size_t limit) const override;
  std::int64_t retrieval_revision(const MemoryScope&, const std::string&) const override;
  RetrievalMaintenanceReport rebuild_retrieval(
      const MemoryScope&, std::optional<std::string> owner_key = std::nullopt);
  void add_assertion(const MemoryScope&, MemoryAssertion);
  std::optional<RetrievalIndexJob> claim_index_job(const MemoryScope&,
      std::chrono::seconds lease = std::chrono::seconds{30},
      std::optional<std::string> dense_generation = std::nullopt);
  bool publish_chunks(const MemoryScope&, const RetrievalIndexJob&,
                      std::vector<RetrievalChunk>);
  bool fail_index_job(const MemoryScope&, const RetrievalIndexJob&,
                      const std::string& error, std::size_t max_attempts = 5);
  bool renew_index_job(const MemoryScope&, const RetrievalIndexJob&,
                       std::chrono::seconds lease);
  RetrievalIndexStatus retrieval_index_status(const MemoryScope&) const;
  void sync_code_snapshot(const MemoryScope&, const index::RetrievalSnapshot&,
                          const std::function<bool()>& cancelled = {});
  std::vector<RetrievalCandidate> resolve_code_hits(const MemoryScope&,
      const std::vector<DenseSearchHit>&) const;
  std::vector<RetrievalChunk> retrieval_chunks(const MemoryScope&,
      const std::string& owner_key) const;
  void begin_dense_index(const MemoryScope&, const DenseIndexConfig&);
  bool publish_dense_vectors(const MemoryScope&, const RetrievalIndexJob&,
                            const std::vector<DenseChunkVector>&);
  // Consumes already-computed vectors inside one publication transaction.
  // The reader must not invoke a model or mutate the database. Reader failures
  // roll back all writes; stale leases are rejected before reading any vectors.
  bool publish_dense_vectors(const MemoryScope&, const RetrievalIndexJob&,
      std::size_t vector_count, const std::function<DenseChunkVector()>& next_vector);
  bool activate_dense_index(const MemoryScope&, const std::string& generation);
  std::optional<DenseIndexConfig> dense_index(const MemoryScope&,
      const std::string& generation = {}, RecallPolicy policy = RecallPolicy::Strict,
      bool include_global_user = false) const;
  std::vector<DenseSearchHit> search_dense(const MemoryScope&, const std::string& generation,
      const std::vector<float>& query, RecallPolicy, bool include_global_user,
      std::size_t limit = 50, bool distinct_owners = false) const;
  // Row-only convenience view of the validated typed candidates below.
  std::vector<MemoryEntry> resolve_dense_memories(const MemoryScope&,
      const std::vector<DenseSearchHit>&, RecallPolicy, bool include_global_user) const;
  std::vector<DenseMemoryCandidate> resolve_dense_memory_candidates(const MemoryScope&,
      const std::vector<DenseSearchHit>&, RecallPolicy, bool include_global_user) const;

  std::string add_cross_store_link(const MemoryScope&, CrossStoreLink);
  std::vector<CrossStoreLink> links(const MemoryScope&, const RetrievalReference&,
                                    RecallPolicy, bool, std::size_t limit) const override;

 private:
  void initialize();

  std::filesystem::path root_;
  std::filesystem::path path_;
  DatabaseOptions options_;
  sqlite3* database_ = nullptr;
  std::size_t schema_version_ = 0;
  mutable std::recursive_mutex mutex_;
};

}  // namespace qt::memory
