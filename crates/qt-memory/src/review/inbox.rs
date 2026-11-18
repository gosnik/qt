//! The Review Inbox surface over one runtime scope (archived
//! `MemoryRuntime` review methods): listing that merges the exact scope with
//! its durable scope, scope fallback for detail/preview/apply, constrained
//! batches with a preview token, and background batch jobs.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;

use serde_json::json;

use super::model::*;
use super::service::{ReviewService, rank_and_page};
use crate::error::{MemoryError, Result, invalid};
use crate::model::*;
use crate::scope::{durable_memory_scope, scope_key};
use crate::text::{dump, stable_hash_hex};

const NOT_FOUND: &str = "review item not found in scope";

struct PendingBatch {
    id: String,
    items: Vec<ReviewBatchItem>,
    preview_token: String,
}

struct Inner {
    service: ReviewService,
    scope: Mutex<MemoryScope>,
    jobs: Mutex<HashMap<String, ReviewBatchJob>>,
    next_job: AtomicU64,
}

struct Worker {
    sender: mpsc::Sender<PendingBatch>,
    handle: JoinHandle<()>,
}

/// Review workflow bound to the runtime's current exact scope.
pub struct ReviewInbox {
    inner: Arc<Inner>,
    worker: Mutex<Option<Worker>>,
}

impl std::fmt::Debug for ReviewInbox {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ReviewInbox")
            .field("scope", &self.inner.scope())
            .finish_non_exhaustive()
    }
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn error_code(message: &str) -> String {
    if message.contains("stale") {
        "stale_preview"
    } else {
        "application_failed"
    }
    .into()
}

fn validate_batch_size(items: &[ReviewBatchItem]) -> Result<()> {
    if items.is_empty() || items.len() > 100 {
        return invalid("review batch must contain from 1 to 100 items");
    }
    Ok(())
}

impl Inner {
    fn scope(&self) -> MemoryScope {
        lock(&self.scope).clone()
    }

    /// Runs `operation` on the exact scope, retrying on the durable scope
    /// when the review is not stored in the exact one.
    fn with_fallback<T>(&self, operation: impl Fn(&MemoryScope) -> Result<T>) -> Result<T> {
        let exact = self.scope();
        match operation(&exact) {
            Err(MemoryError::InvalidArgument(message)) if message == NOT_FOUND => {
                operation(&durable_memory_scope(exact))
            }
            other => other,
        }
    }

    fn apply_review_action(
        &self,
        id: &str,
        action: ReviewAction,
    ) -> Result<ReviewApplicationResult> {
        self.with_fallback(|scope| self.service.apply(scope, id, action.clone()))
    }

    fn apply_batch(
        &self,
        items: Vec<ReviewBatchItem>,
        preview_token: String,
        progress: &dyn Fn(usize),
    ) -> Result<ReviewBatchResult> {
        validate_batch_size(&items)?;
        let mut material = String::from("review-batch-v1");
        for item in &items {
            if item.review_id.is_empty()
                || item.action.idempotency_key.is_empty()
                || item.action.expected_revision == 0
            {
                return invalid(
                    "review batch apply requires each preview revision and idempotency key",
                );
            }
            material += &format!(":{}:{}", item.review_id, item.action.idempotency_key);
        }
        if preview_token.is_empty() || preview_token != stable_hash_hex(&material) {
            return invalid("review batch changed after preview: stale idempotency token");
        }
        let mut batch = ReviewBatchResult {
            idempotency_token: preview_token,
            ..Default::default()
        };
        let mut replays = 0usize;
        let mut compensations = 0usize;
        for (index, item) in items.into_iter().enumerate() {
            let mut result = ReviewBatchItemResult {
                review_id: item.review_id.clone(),
                ..Default::default()
            };
            let compensation = item.action.kind == ReviewActionKind::Compensate;
            let mut action = item.action;
            action.human_authority = true;
            match self.apply_review_action(&item.review_id, action) {
                Ok(applied) => {
                    if applied.idempotent_replay {
                        replays += 1;
                    }
                    if compensation && applied.decision_persisted {
                        compensations += 1;
                    }
                    if applied.decision_persisted {
                        batch.applied_count += 1;
                    } else {
                        batch.failed_count += 1;
                    }
                    result.result = Some(applied);
                }
                Err(error) => {
                    result.error = error.to_string();
                    result.error_code = error_code(&result.error);
                    batch.failed_count += 1;
                }
            }
            batch.items.push(result);
            progress(index + 1);
        }
        batch.partial_failure = batch.applied_count != 0 && batch.failed_count != 0;
        let reason = if batch.partial_failure {
            "review batch completed with partial failure"
        } else if batch.failed_count == 0 {
            "review batch completed"
        } else {
            "review batch failed"
        };
        self.service.store().record_audit(
            &durable_memory_scope(self.scope()),
            AuditEventWrite {
                actor: "user:review_batch".into(),
                action: "review_batch".into(),
                reason: reason.into(),
                metadata_json: dump(&json!({
                    "idempotency_token": batch.idempotency_token,
                    "batch_size": batch.items.len(),
                    "applied_count": batch.applied_count,
                    "failed_count": batch.failed_count,
                    "partial_failure": batch.partial_failure,
                    "retry_count": replays,
                    "idempotent_replay_count": replays,
                    "compensation_count": compensations,
                    "application_error_count": batch.failed_count,
                })),
                ..Default::default()
            },
        )?;
        Ok(batch)
    }

    fn run_job(&self, pending: PendingBatch) {
        if let Some(job) = lock(&self.jobs).get_mut(&pending.id) {
            job.status = ReviewBatchJobStatus::Running;
        }
        let id = pending.id.clone();
        let outcome = self.apply_batch(pending.items, pending.preview_token, &|processed| {
            if let Some(job) = lock(&self.jobs).get_mut(&id) {
                job.processed_count = processed;
            }
        });
        let mut jobs = lock(&self.jobs);
        let Some(job) = jobs.get_mut(&pending.id) else {
            return;
        };
        match outcome {
            Ok(result) => {
                job.processed_count = job.total_count;
                job.result = Some(result);
                job.status = ReviewBatchJobStatus::Completed;
            }
            Err(error) => {
                job.status = ReviewBatchJobStatus::Failed;
                job.error = error.to_string();
                job.error_code = error_code(&job.error);
            }
        }
    }
}

impl ReviewInbox {
    pub fn new(service: ReviewService, exact_scope: MemoryScope) -> Self {
        Self {
            inner: Arc::new(Inner {
                service,
                scope: Mutex::new(exact_scope),
                jobs: Mutex::new(HashMap::new()),
                next_job: AtomicU64::new(1),
            }),
            worker: Mutex::new(None),
        }
    }

    pub fn service(&self) -> &ReviewService {
        &self.inner.service
    }

    pub fn scope(&self) -> MemoryScope {
        self.inner.scope()
    }

    /// Follows a session/agent change of the runtime scope.
    pub fn set_scope(&self, scope: MemoryScope) {
        *lock(&self.inner.scope) = scope;
    }

    /// Ranked page over the exact queue and/or its durable queue.
    pub fn review_items(&self, query: &ReviewQuery) -> Result<Vec<ReviewItem>> {
        let exact = self.scope();
        let Some(end) = query.offset.checked_add(query.limit) else {
            return invalid("review query offset is too large");
        };
        let mut scoped = query.clone();
        scoped.offset = 0;
        scoped.limit = end.min(1000);
        let mut result = Vec::new();
        if query.scope != ReviewScopeSelector::Durable {
            result = self.inner.service.query(&exact, &scoped)?;
        }
        let durable = durable_memory_scope(exact.clone());
        if query.scope != ReviewScopeSelector::Exact && scope_key(&durable)? != scope_key(&exact)? {
            result.extend(self.inner.service.query(&durable, &scoped)?);
        }
        Ok(rank_and_page(result, query))
    }

    pub fn pending_reviews(&self, limit: usize) -> Result<Vec<ReviewItem>> {
        self.review_items(&ReviewQuery {
            statuses: vec![ReviewStatus::Pending],
            limit,
            ..Default::default()
        })
    }

    pub fn review_detail(&self, id: &str) -> Result<HydratedReview> {
        self.inner
            .with_fallback(|scope| self.inner.service.hydrate(scope, id))
    }

    pub fn preview_review(&self, id: &str, action: ReviewAction) -> Result<ReviewMutationPreview> {
        self.inner
            .with_fallback(|scope| self.inner.service.preview(scope, id, action.clone()))
    }

    pub fn apply_review_action(
        &self,
        id: &str,
        action: ReviewAction,
    ) -> Result<ReviewApplicationResult> {
        self.inner.apply_review_action(id, action)
    }

    /// Previews a constrained batch: reject, defer, or homogeneous low-risk
    /// approvals of source-backed, non-critical, non-complex items.
    pub fn preview_review_batch(&self, items: &[ReviewBatchItem]) -> Result<ReviewBatchPreview> {
        validate_batch_size(items)?;
        let mut result = ReviewBatchPreview::default();
        let kind = items[0].action.kind;
        if !matches!(
            kind,
            ReviewActionKind::Reject
                | ReviewActionKind::Defer
                | ReviewActionKind::Approve
                | ReviewActionKind::EditAndApprove
        ) {
            result
                .warnings
                .push("batch supports only reject, defer, or homogeneous low-risk approval".into());
            return Ok(result);
        }
        let mut material = String::from("review-batch-v1");
        for item in items {
            if item.review_id.is_empty() || item.action.kind != kind {
                result
                    .warnings
                    .push("batch items must have IDs and homogeneous action semantics".into());
                return Ok(result);
            }
            let preview = self.preview_review(&item.review_id, item.action.clone())?;
            if !preview.applicable {
                result
                    .warnings
                    .push(format!("review {} is not applicable", item.review_id));
            }
            if matches!(
                kind,
                ReviewActionKind::Approve | ReviewActionKind::EditAndApprove
            ) {
                let detail = self.review_detail(&item.review_id)?;
                let critical = detail
                    .subject
                    .as_ref()
                    .is_some_and(|subject| subject.memory_type.is_critical());
                let provenance = &detail.item.provenance;
                let source_backed = provenance.source_type != SourceType::Unknown
                    && provenance.source_type != SourceType::Dreaming
                    && (!provenance.source_uri.is_empty() || !provenance.source_hash.is_empty())
                    && provenance.evidence_strength > 0.0;
                if critical
                    || detail.risk.risk_class == "critical"
                    || detail.risk.risk_class == "high"
                    || !source_backed
                    || matches!(
                        detail.candidate.kind,
                        ReviewCandidateKind::Conflict
                            | ReviewCandidateKind::SemanticConsolidation
                            | ReviewCandidateKind::Compression
                    )
                {
                    result.warnings.push(format!(
                        "review {} requires individual review because it is critical, high-risk, not independently source-backed, or has complex semantics",
                        item.review_id
                    ));
                }
            }
            material += &format!(":{}:{}", item.review_id, preview.idempotency_token);
            result.items.push(preview);
        }
        result.idempotency_token = stable_hash_hex(&material);
        result.applicable =
            result.warnings.is_empty() && result.items.iter().all(|preview| preview.applicable);
        Ok(result)
    }

    /// Applies a previewed batch synchronously; per-item failures are
    /// reported, not raised, and the batch is audited on the durable scope.
    pub fn apply_review_batch(
        &self,
        items: Vec<ReviewBatchItem>,
        preview_token: String,
    ) -> Result<ReviewBatchResult> {
        self.inner.apply_batch(items, preview_token, &|_| {})
    }

    /// Queues a previewed batch on the background worker and returns its job.
    pub fn start_review_batch(
        &self,
        items: Vec<ReviewBatchItem>,
        preview_token: String,
    ) -> Result<ReviewBatchJob> {
        validate_batch_size(&items)?;
        if preview_token.is_empty() {
            return invalid("review batch apply requires the preview idempotency token");
        }
        if items.iter().any(|item| {
            item.review_id.is_empty()
                || item.action.idempotency_key.is_empty()
                || item.action.expected_revision == 0
        }) {
            return invalid(
                "review batch apply requires each preview revision and idempotency key",
            );
        }
        let sequence = self.inner.next_job.fetch_add(1, Ordering::Relaxed);
        let id = format!(
            "review-batch-job-{sequence}-{}",
            &stable_hash_hex(&preview_token)[..12]
        );
        let job = ReviewBatchJob {
            id: id.clone(),
            status: ReviewBatchJobStatus::Queued,
            total_count: items.len(),
            processed_count: 0,
            result: None,
            error_code: String::new(),
            error: String::new(),
        };
        lock(&self.inner.jobs).insert(id.clone(), job.clone());
        let mut worker = lock(&self.worker);
        if worker.is_none() {
            let (sender, receiver) = mpsc::channel::<PendingBatch>();
            let inner = Arc::clone(&self.inner);
            let handle = std::thread::Builder::new()
                .name("qt-memory-review-batch".into())
                .spawn(move || {
                    for pending in receiver {
                        inner.run_job(pending);
                    }
                })
                .map_err(|error| {
                    MemoryError::storage(format!("unable to start review batch worker: {error}"))
                })?;
            *worker = Some(Worker { sender, handle });
        }
        let pending = PendingBatch {
            id,
            items,
            preview_token,
        };
        if let Some(worker) = worker.as_ref()
            && worker.sender.send(pending).is_err()
        {
            return Err(MemoryError::storage("review batch worker stopped"));
        }
        Ok(job)
    }

    pub fn review_batch_job(&self, id: &str) -> Result<ReviewBatchJob> {
        if id.is_empty() {
            return invalid("review batch status requires a non-empty job_id");
        }
        lock(&self.inner.jobs)
            .get(id)
            .cloned()
            .ok_or_else(|| MemoryError::invalid("review batch job not found"))
    }

    /// Action history of the exact scope, topped up from the durable scope.
    pub fn review_history(
        &self,
        id: Option<&str>,
        limit: usize,
    ) -> Result<Vec<ReviewActionRecord>> {
        let exact = self.scope();
        let store = self.inner.service.store();
        let mut result = store.review_history(&exact, id, limit)?;
        let durable = durable_memory_scope(exact.clone());
        if scope_key(&durable)? != scope_key(&exact)? && result.len() < limit {
            result.extend(store.review_history(&durable, id, limit - result.len())?);
        }
        Ok(result)
    }

    pub fn load_review_view_state(&self) -> Result<Option<ReviewViewState>> {
        self.inner
            .service
            .store()
            .load_review_view_state(&durable_memory_scope(self.scope()))
    }

    pub fn save_review_view_state(&self, state: ReviewViewState) -> Result<()> {
        self.inner
            .service
            .store()
            .save_review_view_state(&durable_memory_scope(self.scope()), state)
    }

    pub fn clear_review_view_state(&self) -> Result<()> {
        self.inner
            .service
            .store()
            .clear_review_view_state(&durable_memory_scope(self.scope()))
    }

    pub fn review_statistics(&self) -> Result<ReviewStatistics> {
        self.inner
            .service
            .statistics_including_durable(&self.scope())
    }

    /// Waits for queued batch jobs to finish and stops the worker.
    pub fn shutdown(&self) {
        if let Some(worker) = lock(&self.worker).take() {
            drop(worker.sender);
            let _ = worker.handle.join();
        }
    }
}

impl Drop for ReviewInbox {
    fn drop(&mut self) {
        self.shutdown();
    }
}
