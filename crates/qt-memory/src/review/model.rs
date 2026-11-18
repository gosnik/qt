//! Review queue domain types (archived `review.hpp` / `review.cpp`).

use std::collections::BTreeMap;

use serde_json::{Value, json};

use crate::error::{MemoryError, Result};
use crate::model::*;

string_enum!(
    /// Typed candidate payload carried by a review item.
    ReviewCandidateKind {
        DreamArtifact => "dream_artifact",
        DreamResearchSuccessor => "dream_research_successor",
        SemanticConsolidation => "semantic_consolidation",
        Compression => "compression",
        Conflict => "conflict",
        ProcedureRevision => "procedure_revision",
        General => "general",
        Staleness => "staleness",
        ProvenanceRepair => "provenance_repair",
    }
);

impl ReviewCandidateKind {
    /// Lenient mapping of a stored `kind` column (unknown kinds are general).
    pub fn for_kind(kind: &str) -> Self {
        match kind {
            "dream_artifact" => Self::DreamArtifact,
            "dream_research_successor" => Self::DreamResearchSuccessor,
            "semantic_consolidation_candidate" | "semantic_consolidation" => {
                Self::SemanticConsolidation
            }
            "compression_candidate" | "compression" => Self::Compression,
            "conflict" => Self::Conflict,
            "procedure_revision" | "failed_procedure" => Self::ProcedureRevision,
            "staleness" => Self::Staleness,
            "provenance_repair" => Self::ProvenanceRepair,
            _ => Self::General,
        }
    }

    /// Strict parse: aliases accepted, unknown kinds rejected.
    pub fn parse(value: &str) -> Result<Self> {
        let kind = Self::for_kind(value);
        if kind == Self::General && !matches!(value, "general" | "candidate_memory" | "candidate") {
            return Err(MemoryError::invalid(format!(
                "invalid review candidate kind: {value}"
            )));
        }
        Ok(kind)
    }
}

string_enum!(
    /// Reviewer decision.
    ReviewActionKind {
        Approve => "approve",
        EditAndApprove => "edit_and_approve",
        Reject => "reject",
        Defer => "defer",
        Reopen => "reopen",
        Relate => "relate",
        Merge => "merge",
        Supersede => "supersede",
        MarkContradiction => "mark_contradiction",
        KeepBoth => "keep_both",
        Retain => "retain",
        NarrowScope => "narrow_scope",
        Revalidate => "revalidate",
        Deprecate => "deprecate",
        AttachEvidence => "attach_evidence",
        Compensate => "compensate",
    }
);

impl ReviewActionKind {
    pub fn parse(value: &str) -> Result<Self> {
        Self::ALL
            .iter()
            .copied()
            .find(|item| item.as_str() == value)
            .ok_or_else(|| MemoryError::invalid(format!("invalid review action: {value}")))
    }
}

string_enum!(
    /// Outcome of applying a decision's domain mutations.
    ReviewApplicationStatus {
        NotStarted => "not_started",
        Applied => "applied",
        NoOp => "no_op",
        Failed => "failed",
        PartiallyApplied => "partially_applied",
        Compensated => "compensated",
    }
);

impl ReviewApplicationStatus {
    pub fn parse(value: &str) -> Result<Self> {
        Self::ALL
            .iter()
            .copied()
            .find(|item| item.as_str() == value)
            .ok_or_else(|| {
                MemoryError::invalid(format!("invalid review application status: {value}"))
            })
    }
}

string_enum!(
    /// Background batch job state.
    ReviewBatchJobStatus {
        Queued => "queued",
        Running => "running",
        Completed => "completed",
        Failed => "failed",
    }
);

/// Subject signals joined in by queue queries to rank items.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct QueueSignals {
    pub critical_type: bool,
    pub recent_context: bool,
    pub trust_risk: f64,
    pub access_count: u64,
    pub procedure_failures: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewItem {
    pub queue_signals: QueueSignals,
    pub id: String,
    pub kind: String,
    pub status: ReviewStatus,
    pub priority: String,
    pub reason: String,
    pub subject_id: String,
    pub provenance: Provenance,
    pub candidate_json: String,
    pub decision: String,
    pub decided_by: String,
    pub decision_note: String,
    pub candidate_version: u32,
    pub revision: u64,
    pub deferred_until: Millis,
    pub application_status: String,
    pub application_error: String,
    pub action_key: String,
    pub tags: Vec<String>,
    pub created_at: Millis,
    pub updated_at: Millis,
    pub decided_at: Millis,
}

impl Default for ReviewItem {
    fn default() -> Self {
        Self {
            queue_signals: QueueSignals::default(),
            id: String::new(),
            kind: "candidate_memory".into(),
            status: ReviewStatus::Pending,
            priority: "normal".into(),
            reason: String::new(),
            subject_id: String::new(),
            provenance: Provenance::default(),
            candidate_json: String::new(),
            decision: String::new(),
            decided_by: String::new(),
            decision_note: String::new(),
            candidate_version: 1,
            revision: 0,
            deferred_until: 0,
            application_status: "not_started".into(),
            application_error: String::new(),
            action_key: String::new(),
            tags: Vec::new(),
            created_at: 0,
            updated_at: 0,
            decided_at: 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewItemWrite {
    pub kind: String,
    pub priority: String,
    pub reason: String,
    pub subject_id: String,
    pub provenance: Provenance,
    pub candidate_json: String,
    pub candidate_version: u32,
    pub tags: Vec<String>,
}

impl Default for ReviewItemWrite {
    fn default() -> Self {
        Self {
            kind: "candidate_memory".into(),
            priority: "normal".into(),
            reason: String::new(),
            subject_id: String::new(),
            provenance: Provenance::default(),
            candidate_json: String::new(),
            candidate_version: 1,
            tags: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewDecision {
    pub status: ReviewStatus,
    pub decision: String,
    pub decided_by: String,
    pub note: String,
    pub deferred_until: Option<Millis>,
    pub expected_revision: u64,
    pub action_key: String,
    pub application_status: String,
    pub application_error: String,
    pub application_result_json: String,
}

impl Default for ReviewDecision {
    fn default() -> Self {
        Self {
            status: ReviewStatus::Deferred,
            decision: String::new(),
            decided_by: String::new(),
            note: String::new(),
            deferred_until: None,
            expected_revision: 0,
            action_key: String::new(),
            application_status: "applied".into(),
            application_error: String::new(),
            application_result_json: "{}".into(),
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct DreamArtifactCandidate {
    pub artifact_id: String,
    pub content: String,
    pub source_memory_ids: Vec<String>,
    pub cycle_id: String,
    pub model: String,
    pub research_required: bool,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct DreamResearchCandidate {
    pub dream_id: String,
    pub research_memory_id: String,
    pub evidence_uri: String,
    pub source_memory_ids: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ConsolidationCandidate {
    pub memory_ids: Vec<String>,
    pub canonical_content: String,
    pub proposed_relationship: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ConflictCandidate {
    pub memory_ids: Vec<String>,
    pub severity: String,
    pub explanation: String,
}

impl Default for ConflictCandidate {
    fn default() -> Self {
        Self {
            memory_ids: Vec::new(),
            severity: "normal".into(),
            explanation: String::new(),
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ProcedureRevisionCandidate {
    pub current_memory_id: String,
    pub proposed_content: String,
    pub evidence_ids: Vec<String>,
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct GeneralReviewCandidate {
    pub content: String,
    pub memory_type: Option<MemoryType>,
    pub target_status: Option<MemoryStatus>,
    pub tags: Vec<String>,
    pub related_memory_ids: Vec<String>,
    pub reason: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum ReviewCandidatePayload {
    DreamArtifact(DreamArtifactCandidate),
    DreamResearch(DreamResearchCandidate),
    Consolidation(ConsolidationCandidate),
    Conflict(ConflictCandidate),
    ProcedureRevision(ProcedureRevisionCandidate),
    General(GeneralReviewCandidate),
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewCandidate {
    pub version: u32,
    pub kind: ReviewCandidateKind,
    pub payload: ReviewCandidatePayload,
    pub legacy: bool,
    pub diagnostics: Vec<String>,
}

impl Default for ReviewCandidate {
    fn default() -> Self {
        Self {
            version: 1,
            kind: ReviewCandidateKind::General,
            payload: ReviewCandidatePayload::General(GeneralReviewCandidate::default()),
            legacy: false,
            diagnostics: Vec::new(),
        }
    }
}

impl ReviewCandidate {
    pub fn conflict(&self) -> Option<&ConflictCandidate> {
        match &self.payload {
            ReviewCandidatePayload::Conflict(conflict) => Some(conflict),
            _ => None,
        }
    }

    pub fn consolidation(&self) -> Option<&ConsolidationCandidate> {
        match &self.payload {
            ReviewCandidatePayload::Consolidation(value) => Some(value),
            _ => None,
        }
    }

    /// Memory IDs the candidate is about, in payload order.
    pub fn source_ids(&self) -> Vec<String> {
        match &self.payload {
            ReviewCandidatePayload::DreamArtifact(value) => value.source_memory_ids.clone(),
            ReviewCandidatePayload::DreamResearch(value) => {
                let mut ids = value.source_memory_ids.clone();
                if !value.dream_id.is_empty() {
                    ids.push(value.dream_id.clone());
                }
                ids
            }
            ReviewCandidatePayload::Consolidation(value) => value.memory_ids.clone(),
            ReviewCandidatePayload::Conflict(value) => value.memory_ids.clone(),
            ReviewCandidatePayload::ProcedureRevision(value) => {
                let mut ids = value.evidence_ids.clone();
                if !value.current_memory_id.is_empty() {
                    ids.push(value.current_memory_id.clone());
                }
                ids
            }
            ReviewCandidatePayload::General(value) => value.related_memory_ids.clone(),
        }
    }
}

/// Which stored queue a listing reads.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ReviewScopeSelector {
    #[default]
    All,
    Exact,
    Durable,
}

impl ReviewScopeSelector {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::All => "all",
            Self::Exact => "exact",
            Self::Durable => "durable",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewQuery {
    pub statuses: Vec<ReviewStatus>,
    pub subject_id: Option<String>,
    pub kinds: Vec<String>,
    pub priorities: Vec<String>,
    pub source_types: Vec<SourceType>,
    pub tags: Vec<String>,
    pub dream_cycles: Vec<String>,
    pub stale: Option<bool>,
    pub conflict: Option<bool>,
    pub created_after: Option<Millis>,
    pub created_before: Option<Millis>,
    pub decided_after: Option<Millis>,
    pub deferred_due_before: Option<Millis>,
    pub include_due_deferred: bool,
    pub oldest_first: bool,
    pub scope: ReviewScopeSelector,
    pub limit: usize,
    pub offset: usize,
}

impl Default for ReviewQuery {
    fn default() -> Self {
        Self {
            statuses: vec![ReviewStatus::Pending],
            subject_id: None,
            kinds: Vec::new(),
            priorities: Vec::new(),
            source_types: Vec::new(),
            tags: Vec::new(),
            dream_cycles: Vec::new(),
            stale: None,
            conflict: None,
            created_after: None,
            created_before: None,
            decided_after: None,
            deferred_due_before: None,
            include_due_deferred: true,
            oldest_first: false,
            scope: ReviewScopeSelector::All,
            limit: 50,
            offset: 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewRisk {
    pub score: f64,
    pub risk_class: String,
    pub components: BTreeMap<String, f64>,
    pub explanations: Vec<String>,
}

impl Default for ReviewRisk {
    fn default() -> Self {
        Self {
            score: 0.0,
            risk_class: "normal".into(),
            components: BTreeMap::new(),
            explanations: Vec::new(),
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ReviewRecallImpact {
    pub strict: bool,
    pub balanced: bool,
    pub broad: bool,
    pub warnings: Vec<String>,
}

/// A review item with its subject, sources, graph context and risk.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct HydratedReview {
    pub item: ReviewItem,
    pub candidate: ReviewCandidate,
    pub subject: Option<MemoryEntry>,
    pub sources: Vec<MemoryEntry>,
    pub related: Vec<MemoryEntry>,
    pub edges: Vec<GraphEdge>,
    pub procedure_runs: Vec<ProcedureRun>,
    pub risk: ReviewRisk,
    pub recall: ReviewRecallImpact,
    pub blocking_diagnostics: Vec<String>,
}

impl HydratedReview {
    /// Subject, source or related memory with `id`.
    pub fn find_memory(&self, id: &str) -> Option<&MemoryEntry> {
        self.subject
            .iter()
            .chain(self.sources.iter())
            .chain(self.related.iter())
            .find(|memory| memory.id == id)
    }
}

/// Reviewer-supplied replacement values.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ReviewEdit {
    pub content: Option<String>,
    pub memory_type: Option<MemoryType>,
    pub target_status: Option<MemoryStatus>,
    pub confidence: Option<f64>,
    pub scope: Option<MemoryScope>,
    pub tags: Option<Vec<String>>,
    pub replacement_id: Option<String>,
}

/// Typed action payloads. Each kind accepts exactly one payload variant.
#[derive(Debug, Clone, PartialEq)]
pub enum ReviewActionPayload {
    Approve {
        evidence_uri: String,
        edit: Option<ReviewEdit>,
    },
    Reject,
    Defer {
        until: Option<Millis>,
    },
    Reopen,
    Relate,
    Merge {
        edit: Option<ReviewEdit>,
    },
    Supersede {
        target_id: String,
    },
    Contradiction,
    KeepBoth,
    Retain,
    NarrowScope {
        edit: Option<ReviewEdit>,
    },
    Revalidate {
        evidence_uri: String,
    },
    Deprecate,
    AttachEvidence {
        evidence_uri: String,
    },
    Compensate {
        target_id: String,
        edit: Option<ReviewEdit>,
    },
}

/// A reviewer action. Callers may fill either the typed `payload` or the
/// flattened compatibility fields; [`normalize_review_action`] makes both
/// observe the same values.
#[derive(Debug, Clone, PartialEq)]
pub struct ReviewAction {
    pub kind: ReviewActionKind,
    pub payload: Option<ReviewActionPayload>,
    pub actor: String,
    pub note: String,
    pub target_id: String,
    pub evidence_uri: String,
    pub deferred_until: Option<Millis>,
    pub edit: Option<ReviewEdit>,
    pub expected_revision: u64,
    pub idempotency_key: String,
    pub human_authority: bool,
}

impl ReviewAction {
    pub fn new(kind: ReviewActionKind, actor: impl Into<String>) -> Self {
        Self {
            kind,
            payload: None,
            actor: actor.into(),
            note: String::new(),
            target_id: String::new(),
            evidence_uri: String::new(),
            deferred_until: None,
            edit: None,
            expected_revision: 0,
            idempotency_key: String::new(),
            human_authority: false,
        }
    }
}

/// Synchronises the typed payload and the flattened fields; rejects a payload
/// of the wrong variant for the action kind.
pub fn normalize_review_action(mut action: ReviewAction) -> Result<ReviewAction> {
    use ReviewActionKind as K;
    use ReviewActionPayload as P;
    let Some(payload) = action.payload.clone() else {
        action.payload = Some(match action.kind {
            K::Approve | K::EditAndApprove => P::Approve {
                evidence_uri: action.evidence_uri.clone(),
                edit: action.edit.clone(),
            },
            K::Reject => P::Reject,
            K::Defer => P::Defer {
                until: action.deferred_until,
            },
            K::Reopen => P::Reopen,
            K::Relate => P::Relate,
            K::Merge => P::Merge {
                edit: action.edit.clone(),
            },
            K::Supersede => P::Supersede {
                target_id: action.target_id.clone(),
            },
            K::MarkContradiction => P::Contradiction,
            K::KeepBoth => P::KeepBoth,
            K::Retain => P::Retain,
            K::NarrowScope => P::NarrowScope {
                edit: action.edit.clone(),
            },
            K::Revalidate => P::Revalidate {
                evidence_uri: action.evidence_uri.clone(),
            },
            K::Deprecate => P::Deprecate,
            K::AttachEvidence => P::AttachEvidence {
                evidence_uri: action.evidence_uri.clone(),
            },
            K::Compensate => P::Compensate {
                target_id: action.target_id.clone(),
                edit: action.edit.clone(),
            },
        });
        return Ok(action);
    };
    let mismatch = || {
        MemoryError::invalid(format!(
            "review action payload does not match action kind {}",
            action.kind.as_str()
        ))
    };
    match (action.kind, payload) {
        (K::Approve | K::EditAndApprove, P::Approve { evidence_uri, edit }) => {
            action.evidence_uri = evidence_uri;
            action.edit = edit;
        }
        (K::Reject, P::Reject)
        | (K::Reopen, P::Reopen)
        | (K::Relate, P::Relate)
        | (K::MarkContradiction, P::Contradiction)
        | (K::KeepBoth, P::KeepBoth)
        | (K::Retain, P::Retain)
        | (K::Deprecate, P::Deprecate) => {}
        (K::Defer, P::Defer { until }) => action.deferred_until = until,
        (K::Merge, P::Merge { edit }) | (K::NarrowScope, P::NarrowScope { edit }) => {
            action.edit = edit
        }
        (K::Supersede, P::Supersede { target_id }) => action.target_id = target_id,
        (K::Revalidate, P::Revalidate { evidence_uri })
        | (K::AttachEvidence, P::AttachEvidence { evidence_uri }) => {
            action.evidence_uri = evidence_uri
        }
        (K::Compensate, P::Compensate { target_id, edit }) => {
            action.target_id = target_id;
            action.edit = edit;
        }
        _ => return Err(mismatch()),
    }
    Ok(action)
}

/// One record a preview says will change.
#[derive(Debug, Clone, PartialEq)]
pub struct ReviewRecordChange {
    pub record_type: String,
    pub record_id: String,
    pub operation: String,
    pub before_json: String,
    pub after_json: String,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewMutationPreview {
    pub review_id: String,
    pub action: ReviewAction,
    pub review_revision: u64,
    pub idempotency_token: String,
    pub changes: Vec<ReviewRecordChange>,
    pub will_do: Vec<String>,
    pub will_not_do: Vec<String>,
    pub warnings: Vec<String>,
    pub requires_human_authority: bool,
    pub applicable: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewApplicationResult {
    pub review_id: String,
    pub status: ReviewApplicationStatus,
    pub decision_persisted: bool,
    pub domain_mutations_applied: bool,
    pub idempotent_replay: bool,
    pub revision: u64,
    pub action_id: String,
    pub changed_ids: Vec<String>,
    pub failed_ids: Vec<String>,
    pub error: String,
}

impl Default for ReviewApplicationResult {
    fn default() -> Self {
        Self {
            review_id: String::new(),
            status: ReviewApplicationStatus::NotStarted,
            decision_persisted: false,
            domain_mutations_applied: false,
            idempotent_replay: false,
            revision: 0,
            action_id: String::new(),
            changed_ids: Vec::new(),
            failed_ids: Vec::new(),
            error: String::new(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReviewMemoryOperation {
    Create,
    Patch,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewMemoryMutation {
    pub operation: ReviewMemoryOperation,
    pub memory_id: String,
    pub target_scope: Option<MemoryScope>,
    pub create: MemoryWrite,
    pub patch: MemoryPatch,
    /// Optional compare-and-swap snapshot for asynchronous verification.
    pub expected_memory: Option<MemoryEntry>,
}

impl ReviewMemoryMutation {
    pub fn patch(memory_id: impl Into<String>, patch: MemoryPatch) -> Self {
        Self {
            operation: ReviewMemoryOperation::Patch,
            memory_id: memory_id.into(),
            target_scope: None,
            create: MemoryWrite::default(),
            patch,
            expected_memory: None,
        }
    }

    pub fn create(write: MemoryWrite, target_scope: Option<MemoryScope>) -> Self {
        Self {
            operation: ReviewMemoryOperation::Create,
            memory_id: String::new(),
            target_scope,
            create: write,
            patch: MemoryPatch::default(),
            expected_memory: None,
        }
    }
}

/// Domain changes committed together with a review decision. Created records
/// are referenced by zero-based `$created:N` placeholders in patch IDs,
/// replacement IDs and edge endpoints.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ReviewAtomicMutation {
    pub memories: Vec<ReviewMemoryMutation>,
    pub edges: Vec<GraphEdge>,
    pub require_domain_mutation: bool,
    pub expected_review_revision: Option<u64>,
    pub expected_review_status: Option<ReviewStatus>,
    pub expected_memories: Vec<MemoryEntry>,
}

/// `$created:N` placeholder for the N-th created memory.
pub fn review_created_reference(index: usize) -> String {
    format!("$created:{index}")
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ReviewStatistics {
    pub pending: u64,
    pub urgent: u64,
    pub due_deferred: u64,
    pub reviewed: u64,
    pub approved: u64,
    pub edited_and_approved: u64,
    pub rejected: u64,
    pub deferred: u64,
    pub resurfaced: u64,
    pub reopened: u64,
    pub merged: u64,
    pub superseded: u64,
    pub validated: u64,
    pub failed: u64,
    pub application_errors: u64,
    pub idempotent_replays: u64,
    pub compensated: u64,
    pub dreaming_pending: u64,
    pub dreaming_approved: u64,
    pub dreaming_rejected: u64,
    pub oldest_pending_age_seconds: i64,
    pub median_pending_age_seconds: i64,
    pub time_to_first_decision_p50_seconds: i64,
    pub time_to_first_decision_p95_seconds: i64,
    pub review_duration_p50_seconds: i64,
    pub review_duration_p95_seconds: i64,
    pub unresolved_critical_conflicts: u64,
    pub stale_validated_subjects: u64,
    pub unprovenanced_active_subjects: u64,
    pub high_use_low_trust_subjects: u64,
    pub batch_runs: u64,
    pub batch_items: u64,
    pub batch_applied_items: u64,
    pub batch_failed_items: u64,
    pub batch_partial_failures: u64,
    pub batch_retries: u64,
    pub batch_idempotent_replays: u64,
    pub batch_compensations: u64,
    pub batch_application_errors: u64,
    pub pending_by_kind: BTreeMap<String, u64>,
    pub pending_by_priority: BTreeMap<String, u64>,
    pub pending_by_scope: BTreeMap<String, u64>,
    pub pending_by_risk_class: BTreeMap<String, u64>,
    pub by_kind: BTreeMap<String, u64>,
    pub by_priority: BTreeMap<String, u64>,
    pub by_status: BTreeMap<String, u64>,
    pub by_source_type: BTreeMap<String, u64>,
    pub by_application_status: BTreeMap<String, u64>,
    pub by_outcome: BTreeMap<String, u64>,
    pub oldest_pending_age_by_kind_seconds: BTreeMap<String, i64>,
    pub median_pending_age_by_kind_seconds: BTreeMap<String, i64>,
    pub oldest_pending_age_by_priority_seconds: BTreeMap<String, i64>,
    pub median_pending_age_by_priority_seconds: BTreeMap<String, i64>,
    pub oldest_pending_age_by_scope_seconds: BTreeMap<String, i64>,
    pub median_pending_age_by_scope_seconds: BTreeMap<String, i64>,
    pub oldest_pending_age_by_risk_class_seconds: BTreeMap<String, i64>,
    pub median_pending_age_by_risk_class_seconds: BTreeMap<String, i64>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewActionRecord {
    pub id: String,
    pub review_id: String,
    pub action_key: String,
    pub action: String,
    pub actor: String,
    pub note: String,
    pub before_status: ReviewStatus,
    pub after_status: ReviewStatus,
    pub before_revision: u64,
    pub after_revision: u64,
    pub application_status: ReviewApplicationStatus,
    pub application_error: String,
    pub result_json: String,
    pub replay_count: u64,
    pub created_at: Millis,
}

/// Persisted Review Inbox position and filters.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ReviewViewState {
    pub query: ReviewQuery,
    pub selected_review_id: String,
    pub queue_offset: usize,
    pub updated_at: Millis,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewBatchItem {
    pub review_id: String,
    pub action: ReviewAction,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ReviewBatchPreview {
    pub idempotency_token: String,
    pub items: Vec<ReviewMutationPreview>,
    pub warnings: Vec<String>,
    pub applicable: bool,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ReviewBatchItemResult {
    pub review_id: String,
    pub result: Option<ReviewApplicationResult>,
    pub error_code: String,
    pub error: String,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ReviewBatchResult {
    pub idempotency_token: String,
    pub items: Vec<ReviewBatchItemResult>,
    pub applied_count: usize,
    pub failed_count: usize,
    pub partial_failure: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub struct ReviewBatchJob {
    pub id: String,
    pub status: ReviewBatchJobStatus,
    pub total_count: usize,
    pub processed_count: usize,
    pub result: Option<ReviewBatchResult>,
    pub error_code: String,
    pub error: String,
}

fn string_array(value: &Value, key: &str) -> Vec<String> {
    let Some(Value::Array(items)) = value.get(key) else {
        return Vec::new();
    };
    let mut result = Vec::with_capacity(items.len());
    for item in items {
        match item {
            Value::String(text) => result.push(text.clone()),
            _ => return Vec::new(),
        }
    }
    result
}

fn memory_ids(value: &Value) -> Vec<String> {
    for key in ["source_memory_ids", "source_ids", "memory_ids"] {
        let ids = string_array(value, key);
        if !ids.is_empty() {
            return ids;
        }
    }
    Vec::new()
}

fn str_or(value: &Value, key: &str, fallback: &str) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or(fallback)
        .to_string()
}

/// Decodes the stored candidate envelope (`{"version","kind","payload"}`)
/// or a legacy flat payload, recording diagnostics instead of failing.
pub fn decode_review_candidate(item: &ReviewItem) -> ReviewCandidate {
    let mut result = ReviewCandidate {
        kind: ReviewCandidateKind::for_kind(&item.kind),
        version: if item.candidate_version == 0 {
            1
        } else {
            item.candidate_version
        },
        ..Default::default()
    };
    let value = match serde_json::from_str::<Value>(&item.candidate_json) {
        Ok(value @ Value::Object(_)) => value,
        _ => {
            result.legacy = true;
            result
                .diagnostics
                .push("candidate payload is not a JSON object".into());
            result.payload = ReviewCandidatePayload::General(GeneralReviewCandidate {
                content: item.candidate_json.clone(),
                memory_type: None,
                target_status: None,
                tags: item.tags.clone(),
                related_memory_ids: Vec::new(),
                reason: item.reason.clone(),
            });
            return result;
        }
    };
    if let Some(version) = value
        .get("version")
        .and_then(Value::as_i64)
        .filter(|version| *version > 0)
    {
        result.version = u32::try_from(version).unwrap_or(u32::MAX);
    }
    result.legacy = value.get("version").is_none() || value.get("kind").is_none();
    if result.version != 1 {
        result
            .diagnostics
            .push("candidate envelope version is unsupported".into());
    }
    if let Some(kind) = value.get("kind").and_then(Value::as_str)
        && ReviewCandidateKind::for_kind(kind) != result.kind
    {
        result
            .diagnostics
            .push("candidate envelope kind does not match the review kind".into());
    }
    let payload = match value.get("payload") {
        Some(payload @ Value::Object(_)) => payload,
        _ => &value,
    };
    for key in [
        "source_memory_ids",
        "source_ids",
        "memory_ids",
        "evidence_ids",
        "tags",
    ] {
        let Some(field) = payload.get(key) else {
            continue;
        };
        let valid = field
            .as_array()
            .is_some_and(|items| items.iter().all(Value::is_string));
        if !valid {
            result
                .diagnostics
                .push(format!("candidate field {key} must contain only strings"));
        }
    }
    result.payload = match result.kind {
        ReviewCandidateKind::DreamArtifact => {
            ReviewCandidatePayload::DreamArtifact(DreamArtifactCandidate {
                artifact_id: str_or(
                    payload,
                    "artifact_id",
                    &str_or(payload, "memory_id", &item.subject_id),
                ),
                content: str_or(payload, "content", ""),
                source_memory_ids: memory_ids(payload),
                cycle_id: str_or(payload, "cycle_id", ""),
                model: str_or(payload, "model", ""),
                research_required: payload
                    .get("research_required")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
            })
        }
        ReviewCandidateKind::DreamResearchSuccessor => {
            ReviewCandidatePayload::DreamResearch(DreamResearchCandidate {
                dream_id: str_or(payload, "dream_id", ""),
                research_memory_id: str_or(payload, "research_memory_id", &item.subject_id),
                evidence_uri: str_or(payload, "evidence_uri", &item.provenance.source_uri),
                source_memory_ids: memory_ids(payload),
            })
        }
        ReviewCandidateKind::SemanticConsolidation | ReviewCandidateKind::Compression => {
            let candidate = ConsolidationCandidate {
                memory_ids: memory_ids(payload),
                canonical_content: str_or(
                    payload,
                    "canonical_content",
                    &str_or(payload, "content", ""),
                ),
                proposed_relationship: str_or(
                    payload,
                    "proposal",
                    &str_or(payload, "relationship", ""),
                ),
            };
            if candidate.memory_ids.len() < 2 {
                result
                    .diagnostics
                    .push("consolidation candidate requires at least two source memories".into());
            }
            ReviewCandidatePayload::Consolidation(candidate)
        }
        ReviewCandidateKind::Conflict => {
            let candidate = ConflictCandidate {
                memory_ids: memory_ids(payload),
                severity: str_or(payload, "severity", "normal"),
                explanation: str_or(payload, "explanation", &item.reason),
            };
            if candidate.memory_ids.len() < 2 {
                result
                    .diagnostics
                    .push("conflict candidate requires at least two memories".into());
            }
            ReviewCandidatePayload::Conflict(candidate)
        }
        ReviewCandidateKind::ProcedureRevision => {
            ReviewCandidatePayload::ProcedureRevision(ProcedureRevisionCandidate {
                current_memory_id: str_or(payload, "current_memory_id", &item.subject_id),
                proposed_content: str_or(
                    payload,
                    "proposed_content",
                    &str_or(payload, "content", ""),
                ),
                evidence_ids: string_array(payload, "evidence_ids"),
            })
        }
        _ => {
            let mut general = GeneralReviewCandidate {
                content: str_or(payload, "content", ""),
                tags: match payload.get("tags") {
                    Some(Value::Array(items)) => items
                        .iter()
                        .filter_map(|item| item.as_str().map(str::to_string))
                        .collect(),
                    _ => item.tags.clone(),
                },
                related_memory_ids: string_array(payload, "memory_ids"),
                reason: str_or(payload, "reason", &item.reason),
                ..Default::default()
            };
            if let Some(memory_type) = payload.get("type").and_then(Value::as_str) {
                match MemoryType::parse(memory_type) {
                    Ok(memory_type) => general.memory_type = Some(memory_type),
                    Err(_) => result
                        .diagnostics
                        .push("candidate contains an invalid memory type".into()),
                }
            }
            if let Some(status) = payload.get("target_status").and_then(Value::as_str) {
                match MemoryStatus::parse(status) {
                    Ok(status) => general.target_status = Some(status),
                    Err(_) => result
                        .diagnostics
                        .push("candidate contains an invalid target status".into()),
                }
            }
            ReviewCandidatePayload::General(general)
        }
    };
    result
}

/// Canonical envelope JSON for a decoded candidate.
pub fn review_candidate_json(candidate: &ReviewCandidate) -> Value {
    let payload = match &candidate.payload {
        ReviewCandidatePayload::DreamArtifact(value) => json!({
            "artifact_id": value.artifact_id, "content": value.content,
            "source_memory_ids": value.source_memory_ids, "cycle_id": value.cycle_id,
            "model": value.model, "research_required": value.research_required,
        }),
        ReviewCandidatePayload::DreamResearch(value) => json!({
            "dream_id": value.dream_id, "research_memory_id": value.research_memory_id,
            "evidence_uri": value.evidence_uri, "source_memory_ids": value.source_memory_ids,
        }),
        ReviewCandidatePayload::Consolidation(value) => json!({
            "memory_ids": value.memory_ids, "canonical_content": value.canonical_content,
            "relationship": value.proposed_relationship,
        }),
        ReviewCandidatePayload::Conflict(value) => json!({
            "memory_ids": value.memory_ids, "severity": value.severity, "explanation": value.explanation,
        }),
        ReviewCandidatePayload::ProcedureRevision(value) => json!({
            "current_memory_id": value.current_memory_id, "proposed_content": value.proposed_content,
            "evidence_ids": value.evidence_ids,
        }),
        ReviewCandidatePayload::General(value) => {
            let mut payload = json!({
                "content": value.content, "tags": value.tags,
                "memory_ids": value.related_memory_ids, "reason": value.reason,
            });
            if let Some(memory_type) = value.memory_type {
                payload["type"] = json!(memory_type.as_str());
            }
            if let Some(status) = value.target_status {
                payload["target_status"] = json!(status.as_str());
            }
            payload
        }
    };
    json!({ "version": candidate.version, "kind": candidate.kind.as_str(), "payload": payload })
}

pub(crate) fn priority_score(priority: &str) -> f64 {
    match priority {
        "critical" | "urgent" => 45.0,
        "high" => 30.0,
        "low" => 0.0,
        _ => 10.0,
    }
}

pub(crate) fn kind_score(kind: ReviewCandidateKind) -> f64 {
    match kind {
        ReviewCandidateKind::Conflict => 40.0,
        ReviewCandidateKind::ProcedureRevision => 28.0,
        ReviewCandidateKind::ProvenanceRepair => 24.0,
        ReviewCandidateKind::Staleness => 20.0,
        ReviewCandidateKind::SemanticConsolidation | ReviewCandidateKind::Compression => 15.0,
        ReviewCandidateKind::DreamResearchSuccessor => 10.0,
        ReviewCandidateKind::General => 8.0,
        ReviewCandidateKind::DreamArtifact => 5.0,
    }
}

/// Whole hours waited, scaled to at most 15 points (two days per point).
pub(crate) fn waiting_score(created_at: Millis, now: Millis) -> f64 {
    let hours = (now - created_at).div_euclid(3_600_000);
    (hours as f64 / 48.0).clamp(0.0, 15.0)
}

pub(crate) fn risk_class(score: f64) -> &'static str {
    if score >= 70.0 {
        "critical"
    } else if score >= 45.0 {
        "high"
    } else if score >= 20.0 {
        "normal"
    } else {
        "low"
    }
}

/// Deterministic 0–100 queue priority (archived `review_queue_score`).
pub fn review_queue_score(item: &ReviewItem, now: Millis) -> f64 {
    let kind = ReviewCandidateKind::for_kind(&item.kind);
    let mut score = priority_score(&item.priority) + kind_score(kind);
    if item.provenance.source_type == SourceType::Unknown
        && item.provenance.source_uri.is_empty()
        && item.provenance.source_hash.is_empty()
    {
        score += 18.0;
    }
    score += (1.0 - item.provenance.evidence_strength).max(0.0) * 12.0;
    if item.queue_signals.critical_type {
        score += 25.0;
    }
    score += item.queue_signals.trust_risk.min(20.0);
    score += (item.queue_signals.access_count as f64 * 2.0).min(20.0);
    score += (item.queue_signals.procedure_failures as f64 * 4.0).min(20.0);
    if item.queue_signals.recent_context {
        score += 8.0;
    }
    let waiting = waiting_score(item.created_at, now);
    score += waiting;
    if kind == ReviewCandidateKind::Staleness {
        score += waiting;
    }
    if let Some(conflict) = decode_review_candidate(item).conflict() {
        if conflict.severity == "critical" {
            // A producer-provided low priority must never bury a critical
            // conflict; headroom keeps explicit urgency and age ordering.
            score = (score + 25.0).max(95.0);
        } else {
            score += if conflict.severity == "high" {
                15.0
            } else {
                5.0
            };
        }
    }
    score.min(100.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exact_action_kind_and_status_sets() {
        let actions: Vec<_> = ReviewActionKind::ALL
            .iter()
            .map(|kind| kind.as_str())
            .collect();
        assert_eq!(
            actions,
            [
                "approve",
                "edit_and_approve",
                "reject",
                "defer",
                "reopen",
                "relate",
                "merge",
                "supersede",
                "mark_contradiction",
                "keep_both",
                "retain",
                "narrow_scope",
                "revalidate",
                "deprecate",
                "attach_evidence",
                "compensate"
            ]
        );
        let kinds: Vec<_> = ReviewCandidateKind::ALL
            .iter()
            .map(|kind| kind.as_str())
            .collect();
        assert_eq!(
            kinds,
            [
                "dream_artifact",
                "dream_research_successor",
                "semantic_consolidation",
                "compression",
                "conflict",
                "procedure_revision",
                "general",
                "staleness",
                "provenance_repair"
            ]
        );
        let statuses: Vec<_> = ReviewApplicationStatus::ALL
            .iter()
            .map(|kind| kind.as_str())
            .collect();
        assert_eq!(
            statuses,
            [
                "not_started",
                "applied",
                "no_op",
                "failed",
                "partially_applied",
                "compensated"
            ]
        );
        let jobs: Vec<_> = ReviewBatchJobStatus::ALL
            .iter()
            .map(|kind| kind.as_str())
            .collect();
        assert_eq!(jobs, ["queued", "running", "completed", "failed"]);
        assert_eq!(
            ReviewActionKind::parse("nope").unwrap_err().to_string(),
            "invalid review action: nope"
        );
        assert_eq!(
            ReviewCandidateKind::parse("weird").unwrap_err().to_string(),
            "invalid review candidate kind: weird"
        );
        assert_eq!(
            ReviewCandidateKind::parse("candidate_memory").unwrap(),
            ReviewCandidateKind::General
        );
        assert_eq!(
            ReviewCandidateKind::parse("failed_procedure").unwrap(),
            ReviewCandidateKind::ProcedureRevision
        );
    }

    #[test]
    fn normalization_fills_and_validates_payloads() {
        let mut action = ReviewAction::new(ReviewActionKind::Supersede, "user");
        action.target_id = "mem_b".into();
        let normalized = normalize_review_action(action).unwrap();
        assert_eq!(
            normalized.payload,
            Some(ReviewActionPayload::Supersede {
                target_id: "mem_b".into()
            })
        );
        let mut typed = ReviewAction::new(ReviewActionKind::Defer, "user");
        typed.payload = Some(ReviewActionPayload::Defer { until: Some(42) });
        assert_eq!(
            normalize_review_action(typed).unwrap().deferred_until,
            Some(42)
        );
        let mut wrong = ReviewAction::new(ReviewActionKind::Reject, "user");
        wrong.payload = Some(ReviewActionPayload::Relate);
        assert_eq!(
            normalize_review_action(wrong).unwrap_err().to_string(),
            "review action payload does not match action kind reject"
        );
    }

    #[test]
    fn decodes_envelopes_and_legacy_payloads() {
        let item = ReviewItem {
            kind: "conflict".into(),
            candidate_json: r#"{"version":1,"kind":"conflict","payload":{"memory_ids":["a","b"],"severity":"critical"}}"#.into(),
            reason: "why".into(),
            ..Default::default()
        };
        let candidate = decode_review_candidate(&item);
        assert!(!candidate.legacy);
        assert!(candidate.diagnostics.is_empty());
        assert_eq!(candidate.conflict().unwrap().explanation, "why");
        assert_eq!(
            crate::text::dump(&review_candidate_json(&candidate)),
            r#"{"kind":"conflict","payload":{"explanation":"why","memory_ids":["a","b"],"severity":"critical"},"version":1}"#
        );
        let legacy = ReviewItem {
            candidate_json: "plain text".into(),
            ..Default::default()
        };
        let decoded = decode_review_candidate(&legacy);
        assert!(decoded.legacy);
        assert_eq!(
            decoded.diagnostics,
            ["candidate payload is not a JSON object"]
        );
        let bad = ReviewItem {
            kind: "semantic_consolidation".into(),
            candidate_json: r#"{"memory_ids":["a",1]}"#.into(),
            ..Default::default()
        };
        assert_eq!(
            decode_review_candidate(&bad).diagnostics,
            [
                "candidate field memory_ids must contain only strings",
                "consolidation candidate requires at least two source memories"
            ]
        );
    }

    #[test]
    fn critical_conflicts_rank_at_least_95() {
        let now = 1_000_000_000;
        let item = ReviewItem {
            kind: "conflict".into(),
            priority: "low".into(),
            candidate_json: r#"{"memory_ids":["a","b"],"severity":"critical"}"#.into(),
            provenance: Provenance {
                evidence_strength: 1.0,
                source_uri: "x".into(),
                ..Default::default()
            },
            created_at: now,
            ..Default::default()
        };
        assert_eq!(review_queue_score(&item, now), 95.0);
        let general = ReviewItem {
            created_at: now,
            ..Default::default()
        };
        // normal priority 10 + general 8 + unprovenanced 18 + weak evidence 12
        assert_eq!(review_queue_score(&general, now), 48.0);
    }
}
