//! Review inspection (query, hydrate, preview, statistics) and application
//! (archived `review_service.cpp`).

use std::collections::{BTreeSet, HashMap};
use std::sync::Arc;

use serde_json::{Value, json};

use super::json::{review_item_json, review_memory_json};
use super::model::*;
use crate::error::{MemoryError, Result, invalid};
use crate::model::*;
use crate::scope::{durable_memory_scope, scope_key};
use crate::service::GovernanceService;
use crate::store::MemoryStore;
use crate::text::{normalize_content, redact_secrets, stable_hash_hex};

/// Dreaming promotion gate (`memory.dreaming.promotionPolicy != "disabled"`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ReviewGovernancePolicy {
    pub allow_dream_hypothesis_promotion: bool,
}

/// Typed review services over one store.
#[derive(Debug, Clone)]
pub struct ReviewService {
    store: Arc<MemoryStore>,
    policy: ReviewGovernancePolicy,
}

fn approval_status_for_kind(review: &HydratedReview) -> Option<MemoryStatus> {
    match review.candidate.kind {
        ReviewCandidateKind::DreamArtifact
        | ReviewCandidateKind::DreamResearchSuccessor
        | ReviewCandidateKind::ProcedureRevision => Some(MemoryStatus::Hypothesis),
        ReviewCandidateKind::General => match &review.candidate.payload {
            ReviewCandidatePayload::General(general) => {
                Some(general.target_status.unwrap_or(MemoryStatus::Active))
            }
            _ => Some(MemoryStatus::Active),
        },
        ReviewCandidateKind::SemanticConsolidation
        | ReviewCandidateKind::Compression
        | ReviewCandidateKind::Conflict
        | ReviewCandidateKind::Staleness
        | ReviewCandidateKind::ProvenanceRepair => None,
    }
}

fn approval_status(review: &HydratedReview, action: &ReviewAction) -> Option<MemoryStatus> {
    action
        .edit
        .as_ref()
        .and_then(|edit| edit.target_status)
        .or_else(|| approval_status_for_kind(review))
}

fn append_approval_effect(
    review: &HydratedReview,
    policy: &ReviewGovernancePolicy,
    preview: &mut ReviewMutationPreview,
) {
    let push = |list: &mut Vec<String>, text: &str| list.push(text.to_string());
    match review.candidate.kind {
        ReviewCandidateKind::DreamArtifact => {
            push(
                &mut preview.will_do,
                "promote the isolated dream artifact to a reviewable hypothesis",
            );
            push(
                &mut preview.will_do,
                "preserve Dreaming provenance and source links",
            );
            push(
                &mut preview.will_not_do,
                "mark the hypothesis as validated truth",
            );
            push(
                &mut preview.will_not_do,
                "treat model output as external evidence",
            );
            preview.applicable = review
                .subject
                .as_ref()
                .is_some_and(|subject| subject.status == MemoryStatus::DreamArtifact)
                && policy.allow_dream_hypothesis_promotion;
            if !policy.allow_dream_hypothesis_promotion {
                push(
                    &mut preview.warnings,
                    "Dreaming promotion is disabled by the configured promotion policy",
                );
            }
        }
        ReviewCandidateKind::DreamResearchSuccessor => {
            push(
                &mut preview.will_do,
                "approve the separately sourced research memory as a hypothesis",
            );
            push(
                &mut preview.will_not_do,
                "retroactively validate the dream artifact",
            );
            preview.applicable = review.subject.is_some();
        }
        ReviewCandidateKind::SemanticConsolidation | ReviewCandidateKind::Compression => {
            push(
                &mut preview.warnings,
                "choose merge, relate, supersede, or contradiction before applying",
            );
        }
        ReviewCandidateKind::Conflict => {
            push(
                &mut preview.warnings,
                "choose an explicit conflict resolution before applying",
            );
        }
        ReviewCandidateKind::ProcedureRevision => {
            push(
                &mut preview.will_do,
                "store the previewed procedure revision as an unvalidated hypothesis",
            );
            push(
                &mut preview.will_not_do,
                "mark revised procedure steps as validated without outcome evidence",
            );
            preview.applicable = review.subject.is_some();
        }
        ReviewCandidateKind::General => {
            push(
                &mut preview.will_do,
                "advance the candidate to its previewed trust state",
            );
            push(
                &mut preview.will_not_do,
                "validate a critical fact without evidence",
            );
            preview.applicable = review.subject.is_some();
        }
        ReviewCandidateKind::Staleness => {
            push(
                &mut preview.warnings,
                "choose revalidate, deprecate, supersede, or retain before applying",
            );
        }
        ReviewCandidateKind::ProvenanceRepair => {
            push(
                &mut preview.warnings,
                "attach evidence or reduce trust before applying provenance repair",
            );
        }
    }
}

fn review_risk(review: &HydratedReview, now: Millis) -> ReviewRisk {
    let subject = review.subject.as_ref();
    let provenance = &review.item.provenance;
    let waiting_age = waiting_score(review.item.created_at, now);
    let conflict_severity =
        review
            .candidate
            .conflict()
            .map_or(0.0, |conflict| match conflict.severity.as_str() {
                "critical" => 25.0,
                "high" => 15.0,
                _ => 5.0,
            });
    let components = [
        ("priority", priority_score(&review.item.priority)),
        ("kind", kind_score(review.candidate.kind)),
        ("conflict_severity", conflict_severity),
        (
            "missing_provenance",
            if provenance.source_type == SourceType::Unknown
                && provenance.source_uri.is_empty()
                && provenance.source_hash.is_empty()
            {
                18.0
            } else {
                0.0
            },
        ),
        (
            "weak_evidence",
            (1.0 - provenance.evidence_strength).max(0.0) * 12.0,
        ),
        (
            "critical_type",
            if subject.is_some_and(|subject| subject.memory_type.is_critical()) {
                25.0
            } else {
                0.0
            },
        ),
        (
            "usage",
            subject.map_or(0.0, |subject| (subject.access_count as f64 * 2.0).min(20.0)),
        ),
        ("waiting_age", waiting_age),
        (
            "stale_age",
            if review.candidate.kind == ReviewCandidateKind::Staleness {
                waiting_age
            } else {
                0.0
            },
        ),
        (
            "procedure_failures",
            subject
                .filter(|subject| subject.memory_type == MemoryType::Procedural)
                .map_or(0.0, |subject| {
                    (subject.failure_count as f64 * 4.0).min(20.0)
                }),
        ),
        (
            "recent_context",
            if subject.is_some_and(|subject| {
                subject.last_accessed_at != 0 && now - subject.last_accessed_at < 7 * 24 * 3_600_000
            }) {
                8.0
            } else {
                0.0
            },
        ),
    ];
    let mut risk = ReviewRisk::default();
    for (name, value) in components {
        risk.components.insert(name.to_string(), value);
    }
    // Components iterate in key order, as the archived std::map does.
    for (name, value) in &risk.components {
        risk.score += value;
        if *value > 0.0 {
            risk.explanations
                .push(format!("{name} contributes {}", *value as i64));
        }
    }
    risk.score = risk.score.min(100.0);
    risk.risk_class = risk_class(risk.score).to_string();
    risk
}

fn recall_impact(subject: Option<&MemoryEntry>) -> ReviewRecallImpact {
    let mut result = ReviewRecallImpact::default();
    let Some(subject) = subject else {
        result.warnings.push("review subject is unavailable".into());
        return result;
    };
    result.strict = subject.status == MemoryStatus::Validated;
    result.balanced = GovernanceService::trusted_for_recall(subject.status);
    result.broad = result.balanced
        || matches!(
            subject.status,
            MemoryStatus::Candidate | MemoryStatus::Hypothesis
        );
    match subject.status {
        MemoryStatus::DreamArtifact => result
            .warnings
            .push("dream artifact is isolated from ordinary recall".into()),
        MemoryStatus::Rejected => result
            .warnings
            .push("rejected memory is excluded from recall".into()),
        MemoryStatus::Superseded | MemoryStatus::Deprecated | MemoryStatus::Expired => result
            .warnings
            .push("inactive lifecycle state is excluded by default".into()),
        _ => {}
    }
    result
}

fn edited_patch(action: &ReviewAction) -> MemoryPatch {
    let Some(edit) = &action.edit else {
        return MemoryPatch::default();
    };
    MemoryPatch {
        content: edit.content.clone(),
        memory_type: edit.memory_type,
        status: edit.target_status,
        confidence: edit.confidence,
        replacement_id: edit.replacement_id.clone(),
        tags: edit.tags.clone(),
        ..Default::default()
    }
}

fn record_change(
    record_type: &str,
    id: String,
    operation: &str,
    before: &Value,
    after: &Value,
) -> ReviewRecordChange {
    ReviewRecordChange {
        record_type: record_type.into(),
        record_id: id,
        operation: operation.into(),
        before_json: before.to_string(),
        after_json: after.to_string(),
    }
}

fn clean_content(content: &str) -> String {
    let mut content = normalize_content(content);
    redact_secrets(&mut content);
    content
}

fn relationship_edge_type(kind: ReviewActionKind) -> &'static str {
    match kind {
        ReviewActionKind::Relate => "related",
        ReviewActionKind::MarkContradiction => "contradicts",
        _ => "conflict_warning",
    }
}

fn append_preview_records(review: &HydratedReview, preview: &mut ReviewMutationPreview) {
    let action = preview.action.clone();
    let mut after_review = review.item.clone();
    match action.kind {
        ReviewActionKind::Reject => after_review.status = ReviewStatus::Rejected,
        ReviewActionKind::Defer => after_review.status = ReviewStatus::Deferred,
        ReviewActionKind::Reopen => after_review.status = ReviewStatus::Pending,
        ReviewActionKind::Compensate => {}
        _ => after_review.status = ReviewStatus::Accepted,
    }
    after_review.revision = review.item.revision + 1;
    after_review.decision = action.kind.as_str().into();
    after_review.decided_by = action.actor.clone();
    after_review.decision_note = action.note.clone();
    preview.changes.push(record_change(
        "review",
        review.item.id.clone(),
        "compare_and_set",
        &review_item_json(&review.item),
        &review_item_json(&after_review),
    ));
    let memory_change = |before: &MemoryEntry, after: &MemoryEntry| {
        record_change(
            "memory",
            before.id.clone(),
            "patch",
            &review_memory_json(before),
            &review_memory_json(after),
        )
    };
    if let Some(subject) = &review.subject {
        let mut after = subject.clone();
        let mut changed = false;
        let approving = matches!(
            action.kind,
            ReviewActionKind::Approve | ReviewActionKind::EditAndApprove
        );
        if let Some(status) = approval_status(review, &action).filter(|_| approving) {
            after.status = status;
            if status == MemoryStatus::Validated && !action.evidence_uri.is_empty() {
                after.provenance.source_type = SourceType::Document;
                after.provenance.source_uri = action.evidence_uri.clone();
                after.provenance.created_by = action.actor.clone();
                after.provenance.evidence_strength = 1.0;
            }
            if review.candidate.kind == ReviewCandidateKind::ProcedureRevision
                && !action
                    .edit
                    .as_ref()
                    .is_some_and(|edit| edit.content.is_some())
                && let ReviewCandidatePayload::ProcedureRevision(procedure) =
                    &review.candidate.payload
            {
                after.content = clean_content(&procedure.proposed_content);
            }
            changed = true;
        } else if action.kind == ReviewActionKind::Reject
            && matches!(
                after.status,
                MemoryStatus::Candidate | MemoryStatus::Hypothesis | MemoryStatus::DreamArtifact
            )
        {
            after.status = MemoryStatus::Rejected;
            changed = true;
        } else if action.kind == ReviewActionKind::Deprecate {
            after.status = MemoryStatus::Deprecated;
            changed = true;
        } else if action.kind == ReviewActionKind::Revalidate {
            after.status = MemoryStatus::Validated;
            changed = true;
        } else if action.kind == ReviewActionKind::Retain {
            if !after
                .tags
                .iter()
                .any(|tag| tag == "review:retained_current")
            {
                after.tags.push("review:retained_current".into());
            }
            changed = true;
        }
        if let Some(edit) = &action.edit {
            if let Some(content) = &edit.content {
                after.content = content.clone();
            }
            if let Some(memory_type) = edit.memory_type {
                after.memory_type = memory_type;
            }
            if let Some(status) = edit.target_status {
                after.status = status;
            }
            if let Some(confidence) = edit.confidence {
                after.confidence = confidence;
            }
            if let Some(scope) = &edit.scope {
                after.scope = scope.clone();
            }
            if let Some(tags) = &edit.tags {
                after.tags = tags.clone();
            }
            if let Some(replacement) = &edit.replacement_id {
                after.replacement_id = replacement.clone();
                after.superseded_by = replacement.clone();
            }
            changed = true;
        }
        if action.kind == ReviewActionKind::AttachEvidence {
            after.provenance.source_uri = action.evidence_uri.clone();
            after.provenance.source_type = SourceType::Document;
            after.provenance.created_by = action.actor.clone();
            after.provenance.evidence_strength = 1.0;
            changed = true;
        }
        if changed {
            preview.changes.push(memory_change(subject, &after));
        }
    }
    let ids = review.candidate.source_ids();
    match action.kind {
        ReviewActionKind::Relate
        | ReviewActionKind::MarkContradiction
        | ReviewActionKind::KeepBoth => {
            let edge_type = relationship_edge_type(action.kind);
            for id in ids.iter().skip(1) {
                preview.changes.push(record_change(
                    "graph_edge",
                    format!("{}:{id}:{edge_type}", ids[0]),
                    "create",
                    &Value::Null,
                    &json!({"from_id": ids[0], "to_id": id, "type": edge_type}),
                ));
            }
        }
        ReviewActionKind::Merge => {
            let candidate = review.candidate.consolidation();
            if candidate.is_some() || review.candidate.kind == ReviewCandidateKind::Conflict {
                let content = action
                    .edit
                    .as_ref()
                    .and_then(|edit| edit.content.clone())
                    .or_else(|| candidate.map(|candidate| candidate.canonical_content.clone()))
                    .unwrap_or_default();
                let created = review_created_reference(0);
                preview.changes.push(record_change(
                    "memory",
                    created.clone(),
                    "create",
                    &Value::Null,
                    &json!({"content": clean_content(&content), "status": "hypothesis",
                            "source": "review:canonical_merge", "source_memory_ids": ids}),
                ));
                for id in &ids {
                    if let Some(source) = review.find_memory(id) {
                        let mut after = source.clone();
                        after.status = MemoryStatus::Superseded;
                        after.replacement_id = created.clone();
                        after.superseded_by = created.clone();
                        preview.changes.push(memory_change(source, &after));
                    }
                    preview.changes.push(record_change(
                        "graph_edge",
                        format!("{id}:canonical"),
                        "create",
                        &Value::Null,
                        &json!({"from_id": id, "to_id": created, "type": "superseded_by"}),
                    ));
                }
            }
        }
        ReviewActionKind::Supersede => {
            for id in ids.iter().filter(|id| **id != action.target_id) {
                if let Some(source) = review.find_memory(id) {
                    let mut after = source.clone();
                    after.status = MemoryStatus::Superseded;
                    after.replacement_id = action.target_id.clone();
                    after.superseded_by = action.target_id.clone();
                    preview.changes.push(memory_change(source, &after));
                }
                preview.changes.push(record_change(
                    "graph_edge",
                    format!("{id}:{}", action.target_id),
                    "create",
                    &Value::Null,
                    &json!({"from_id": id, "to_id": action.target_id, "type": "superseded_by"}),
                ));
            }
        }
        ReviewActionKind::NarrowScope => {
            if let (Some(subject), Some(edit)) = (&review.subject, &action.edit)
                && let Some(scope) = &edit.scope
            {
                let mut narrowed = subject.clone();
                narrowed.id = review_created_reference(0);
                narrowed.scope = scope.clone();
                if let Some(content) = &edit.content {
                    narrowed.content = content.clone();
                }
                if let Some(memory_type) = edit.memory_type {
                    narrowed.memory_type = memory_type;
                }
                if let Some(status) = edit.target_status {
                    narrowed.status = status;
                } else if narrowed.status == MemoryStatus::Validated {
                    narrowed.status = MemoryStatus::Active;
                }
                if let Some(confidence) = edit.confidence {
                    narrowed.confidence = confidence;
                }
                if let Some(tags) = &edit.tags {
                    narrowed.tags = tags.clone();
                }
                preview.changes.push(record_change(
                    "memory",
                    narrowed.id.clone(),
                    "create_narrower_copy",
                    &Value::Null,
                    &review_memory_json(&narrowed),
                ));
                let mut superseded = subject.clone();
                superseded.status = MemoryStatus::Superseded;
                superseded.replacement_id = narrowed.id.clone();
                superseded.superseded_by = narrowed.id.clone();
                preview.changes.push(memory_change(subject, &superseded));
            }
        }
        _ => {}
    }
}

fn exact_scope(lhs: &MemoryScope, rhs: &MemoryScope) -> Result<bool> {
    Ok(scope_key(lhs)? == scope_key(rhs)?)
}

fn review_edge(from: &str, to: &str, edge_type: &str) -> GraphEdge {
    GraphEdge {
        source: "memory_review".into(),
        ..GraphEdge::new(from, to, edge_type)
    }
}

impl ReviewService {
    pub fn new(store: Arc<MemoryStore>, policy: ReviewGovernancePolicy) -> Self {
        Self { store, policy }
    }

    pub fn store(&self) -> &Arc<MemoryStore> {
        &self.store
    }

    pub fn policy(&self) -> ReviewGovernancePolicy {
        self.policy
    }

    /// Resurfaces due deferrals, then ranks the bounded queue (first 1000
    /// matches) by queue score, creation time and ID before paging.
    pub fn query(&self, scope: &MemoryScope, query: &ReviewQuery) -> Result<Vec<ReviewItem>> {
        self.store.resurface_due_reviews(scope, now_ms())?;
        if query.offset.checked_add(query.limit).is_none() {
            return invalid("review query offset is too large");
        }
        let mut expanded = query.clone();
        expanded.offset = 0;
        expanded.limit = 1000;
        let items = self.store.query_reviews(scope, &expanded)?;
        Ok(rank_and_page(items, query))
    }

    pub fn hydrate(&self, scope: &MemoryScope, review_id: &str) -> Result<HydratedReview> {
        let item = self.store.review(scope, review_id)?;
        let mut result = HydratedReview {
            candidate: decode_review_candidate(&item),
            item,
            ..Default::default()
        };
        result.blocking_diagnostics = result.candidate.diagnostics.clone();
        if !result.item.subject_id.is_empty() {
            match self.store.get(scope, &result.item.subject_id) {
                Ok(subject) => result.subject = Some(subject),
                Err(_) => result
                    .blocking_diagnostics
                    .push("review subject is missing or outside the displayed scope".into()),
            }
        }
        if let (Some(subject), ReviewCandidatePayload::DreamArtifact(dream)) =
            (&result.subject, &mut result.candidate.payload)
            && dream.content.is_empty()
        {
            dream.content = subject.content.clone();
        }
        let scope_key_value = scope_key(scope)?;
        let mut unique = BTreeSet::new();
        for id in result.candidate.source_ids() {
            if id.is_empty() || id == result.item.subject_id || !unique.insert(id.clone()) {
                continue;
            }
            match self.store.get(scope, &id) {
                Ok(source) if scope_key(&source.scope)? != scope_key_value => {
                    result
                        .blocking_diagnostics
                        .push(format!("source memory {id} is outside the review scope"));
                }
                Ok(source) => result.sources.push(source),
                Err(_) => result
                    .blocking_diagnostics
                    .push(format!("source memory {id} is missing or outside scope")),
            }
        }
        if let Some(subject) = result.subject.clone() {
            if !result.item.provenance.source_hash.is_empty()
                && !subject.provenance.source_hash.is_empty()
                && result.item.provenance.source_hash != subject.provenance.source_hash
            {
                result
                    .blocking_diagnostics
                    .push("review source hash is stale relative to the current subject".into());
            }
            result.edges = self.store.query_edges(
                scope,
                &GraphQuery {
                    memory_id: subject.id.clone(),
                    limit: 100,
                    ..Default::default()
                },
            )?;
            for edge in &result.edges {
                let related_id = if edge.from_id == subject.id {
                    &edge.to_id
                } else {
                    &edge.from_id
                };
                if related_id.is_empty()
                    || *related_id == subject.id
                    || !unique.insert(related_id.clone())
                {
                    continue;
                }
                match self.store.get(scope, related_id) {
                    Ok(related) => {
                        if scope_key(&related.scope)? == scope_key_value {
                            result.related.push(related);
                        }
                    }
                    Err(_) => result
                        .blocking_diagnostics
                        .push(format!("related memory {related_id} is unavailable")),
                }
            }
            if subject.memory_type == MemoryType::Procedural
                || result.candidate.kind == ReviewCandidateKind::ProcedureRevision
            {
                result.procedure_runs = self.store.runs(scope, &subject.id)?;
            }
        }
        result.recall = recall_impact(result.subject.as_ref());
        result.risk = review_risk(&result, now_ms());
        Ok(result)
    }

    pub fn get_memory(&self, scope: &MemoryScope, memory_id: &str) -> Result<MemoryEntry> {
        self.store.get(scope, memory_id)
    }

    /// Describes exactly what applying `action` would change, and the
    /// idempotency token that apply must present.
    pub fn preview(
        &self,
        scope: &MemoryScope,
        review_id: &str,
        action: ReviewAction,
    ) -> Result<ReviewMutationPreview> {
        let mut action = normalize_review_action(action)?;
        redact_secrets(&mut action.note);
        redact_secrets(&mut action.evidence_uri);
        if let Some(edit) = &mut action.edit {
            if let Some(content) = &mut edit.content {
                *content = clean_content(content);
            }
            if let Some(tags) = &mut edit.tags {
                for tag in tags.iter_mut() {
                    redact_secrets(tag);
                }
            }
        }
        // Rebuild the typed payload from the normalised and redacted fields
        // so previewed bytes are exactly the bytes later applied.
        action.payload = None;
        let mut action = normalize_review_action(action)?;
        let review = self.hydrate(scope, review_id)?;
        action.expected_revision = review.item.revision;
        let mut result = ReviewMutationPreview {
            review_id: review.item.id.clone(),
            review_revision: review.item.revision,
            action,
            idempotency_token: String::new(),
            changes: Vec::new(),
            will_do: Vec::new(),
            will_not_do: Vec::new(),
            warnings: Vec::new(),
            requires_human_authority: true,
            applicable: false,
        };
        let kind = result.action.kind;
        let say = |list: &mut Vec<String>, text: &str| list.push(text.to_string());
        let ids = review.candidate.source_ids();
        if review.item.status != ReviewStatus::Pending
            && !matches!(
                kind,
                ReviewActionKind::Reopen | ReviewActionKind::Compensate
            )
        {
            say(&mut result.warnings, "review is no longer pending");
        } else {
            match kind {
                ReviewActionKind::Reopen => {
                    say(
                        &mut result.will_do,
                        "reopen the terminal review as pending without changing memory",
                    );
                    say(
                        &mut result.will_not_do,
                        "erase the original decision or action history",
                    );
                    result.applicable = review.item.status != ReviewStatus::Pending
                        && !result.action.note.is_empty();
                    if result.action.note.is_empty() {
                        say(&mut result.warnings, "reopen requires an explicit reason");
                    }
                }
                ReviewActionKind::Compensate => {
                    say(
                        &mut result.will_do,
                        "record a new governed compensation while preserving the original decision",
                    );
                    say(
                        &mut result.will_not_do,
                        "rewrite or erase prior audit history",
                    );
                    result.applicable = review.item.status != ReviewStatus::Pending
                        && !result.action.note.is_empty()
                        && result.action.edit.is_some()
                        && !result.action.target_id.is_empty();
                    if !result.applicable {
                        say(
                            &mut result.warnings,
                            "compensation requires a reason, target memory, and explicit replacement values",
                        );
                    }
                }
                ReviewActionKind::Reject => {
                    say(
                        &mut result.will_do,
                        "reject the proposal and exclude its subject from current recall",
                    );
                    say(
                        &mut result.will_not_do,
                        "delete the subject or its audit history",
                    );
                    result.applicable = true;
                }
                ReviewActionKind::Defer => {
                    say(
                        &mut result.will_do,
                        "defer the review without mutating its subject",
                    );
                    say(
                        &mut result.will_not_do,
                        "approve, reject, or change memory trust",
                    );
                    result.applicable = result.action.deferred_until.is_some();
                    if !result.applicable {
                        say(&mut result.warnings, "defer requires a resurface time");
                    }
                }
                ReviewActionKind::Approve | ReviewActionKind::EditAndApprove => {
                    append_approval_effect(&review, &self.policy, &mut result);
                    if kind == ReviewActionKind::EditAndApprove && result.action.edit.is_none() {
                        result.applicable = false;
                        say(
                            &mut result.warnings,
                            "edit-and-approve requires an edited candidate",
                        );
                    }
                }
                ReviewActionKind::Relate
                | ReviewActionKind::MarkContradiction
                | ReviewActionKind::KeepBoth => {
                    let supported = matches!(
                        review.candidate.kind,
                        ReviewCandidateKind::SemanticConsolidation
                            | ReviewCandidateKind::Compression
                            | ReviewCandidateKind::Conflict
                    );
                    say(
                        &mut result.will_do,
                        match kind {
                            ReviewActionKind::Relate => {
                                "link the reviewed memories without changing their trust states"
                            }
                            ReviewActionKind::MarkContradiction => {
                                "record explicit contradiction edges and preserve both records"
                            }
                            _ => "retain both memories with an unresolved-conflict warning edge",
                        },
                    );
                    say(
                        &mut result.will_not_do,
                        "validate, merge, or delete either memory",
                    );
                    result.applicable = supported && ids.len() >= 2;
                }
                ReviewActionKind::Merge => {
                    let candidate = review.candidate.consolidation();
                    let content = result
                        .action
                        .edit
                        .as_ref()
                        .and_then(|edit| edit.content.clone())
                        .or_else(|| candidate.map(|candidate| candidate.canonical_content.clone()))
                        .unwrap_or_default();
                    say(
                        &mut result.will_do,
                        "create one hypothesis canonical memory, link every source, and supersede originals",
                    );
                    say(
                        &mut result.will_not_do,
                        "mark the canonical memory as validated truth",
                    );
                    result.applicable = (candidate.is_some()
                        || review.candidate.kind == ReviewCandidateKind::Conflict)
                        && ids.len() >= 2
                        && !content.is_empty();
                }
                ReviewActionKind::Supersede => {
                    say(
                        &mut result.will_do,
                        "mark every other reviewed memory superseded by the selected current memory",
                    );
                    say(&mut result.will_not_do, "delete superseded history");
                    result.applicable = !result.action.target_id.is_empty()
                        && ids.len() >= 2
                        && ids.contains(&result.action.target_id);
                }
                ReviewActionKind::NarrowScope => {
                    say(
                        &mut result.will_do,
                        "create a narrower-scope copy and supersede the broader record with lineage",
                    );
                    say(
                        &mut result.will_not_do,
                        "broaden access or mutate unrelated scopes",
                    );
                    result.applicable = review.subject.is_some()
                        && result
                            .action
                            .edit
                            .as_ref()
                            .is_some_and(|edit| edit.scope.is_some());
                }
                ReviewActionKind::Revalidate => {
                    say(
                        &mut result.will_do,
                        "attach current independent evidence and transition the subject to validated",
                    );
                    say(
                        &mut result.will_not_do,
                        "treat model output alone as evidence",
                    );
                    result.applicable =
                        review.subject.is_some() && !result.action.evidence_uri.is_empty();
                }
                ReviewActionKind::Deprecate => {
                    say(
                        &mut result.will_do,
                        "deprecate the subject while preserving history and provenance",
                    );
                    say(&mut result.will_not_do, "delete the memory");
                    result.applicable = review.subject.is_some();
                }
                ReviewActionKind::Retain => {
                    say(
                        &mut result.will_do,
                        "retain the current lifecycle state and record the review rationale on the subject",
                    );
                    say(
                        &mut result.will_not_do,
                        "validate, supersede, or delete the memory",
                    );
                    result.applicable = review.subject.is_some()
                        && review.candidate.kind == ReviewCandidateKind::Staleness
                        && !result.action.note.is_empty();
                }
                ReviewActionKind::AttachEvidence => {
                    say(
                        &mut result.will_do,
                        "attach the supplied evidence provenance without silently validating the subject",
                    );
                    say(
                        &mut result.will_not_do,
                        "change lifecycle state or treat model output as evidence",
                    );
                    result.applicable =
                        review.subject.is_some() && !result.action.evidence_uri.is_empty();
                }
            }
        }
        if !review.blocking_diagnostics.is_empty() {
            result
                .warnings
                .extend(review.blocking_diagnostics.iter().cloned());
            if !matches!(
                kind,
                ReviewActionKind::Reject | ReviewActionKind::Defer | ReviewActionKind::Reopen
            ) {
                result.applicable = false;
            }
        }
        if result.applicable {
            append_preview_records(&review, &mut result);
        }
        result.idempotency_token = stable_hash_hex(&idempotency_material(&result, &review)?);
        Ok(result)
    }

    pub fn statistics(&self, scope: &MemoryScope) -> Result<ReviewStatistics> {
        self.store.review_statistics(scope)
    }

    /// Statistics for the queue the Review Inbox lists: the exact scope
    /// merged with its canonical durable scope.
    pub fn statistics_including_durable(
        &self,
        exact_scope: &MemoryScope,
    ) -> Result<ReviewStatistics> {
        let mut result = self.statistics(exact_scope)?;
        let durable_scope = durable_memory_scope(exact_scope.clone());
        if scope_key(&durable_scope)? == scope_key(exact_scope)? {
            return Ok(result);
        }
        let durable = self.statistics(&durable_scope)?;
        merge_statistics(&mut result, durable);
        Ok(result)
    }

    /// Applies a previewed action. The review revision and the preview's
    /// idempotency token must still match; replays of a recorded action key
    /// return the recorded result.
    pub fn apply(
        &self,
        scope: &MemoryScope,
        review_id: &str,
        action: ReviewAction,
    ) -> Result<ReviewApplicationResult> {
        if action.actor.is_empty() {
            return invalid("review action requires an actor");
        }
        let hydrated = self.hydrate(scope, review_id)?;
        if !action.idempotency_key.is_empty()
            && hydrated.item.status != ReviewStatus::Pending
            && !matches!(
                action.kind,
                ReviewActionKind::Reopen | ReviewActionKind::Compensate
            )
        {
            let replay = ReviewDecision {
                status: hydrated.item.status,
                decision: action.kind.as_str().into(),
                decided_by: action.actor.clone(),
                note: action.note.clone(),
                expected_revision: hydrated.item.revision,
                action_key: action.idempotency_key.clone(),
                ..Default::default()
            };
            return self.store.apply_review_mutation(
                scope,
                review_id,
                replay,
                ReviewAtomicMutation::default(),
            );
        }
        if action.expected_revision != 0 && action.expected_revision != hydrated.item.revision {
            return invalid("review or subject changed after preview: stale revision");
        }
        let human_authority = action.human_authority;
        let supplied_key = action.idempotency_key.clone();
        let preview = self.preview(scope, review_id, action)?;
        if !preview.applicable {
            return Err(MemoryError::invalid(
                preview
                    .warnings
                    .first()
                    .cloned()
                    .unwrap_or_else(|| "review action is not applicable".into()),
            ));
        }
        if preview.requires_human_authority && !human_authority {
            return invalid("review action requires explicit human authority");
        }
        let mut action = preview.action.clone();
        if !matches!(
            action.kind,
            ReviewActionKind::Defer | ReviewActionKind::Reopen
        ) && let Some(subject) = &hydrated.subject
            && !exact_scope(&subject.scope, scope)?
        {
            return invalid("review subject mutation requires the exact subject scope");
        }
        if supplied_key.is_empty() {
            action.idempotency_key = preview.idempotency_token.clone();
        } else if supplied_key != preview.idempotency_token {
            return invalid("review or subject changed after preview: stale idempotency token");
        } else {
            action.idempotency_key = supplied_key;
        }
        let mut decision = ReviewDecision {
            decided_by: action.actor.clone(),
            note: action.note.clone(),
            expected_revision: preview.review_revision,
            action_key: action.idempotency_key.clone(),
            application_status: "applied".into(),
            ..Default::default()
        };
        let mut mutation = ReviewAtomicMutation::default();
        let ids = hydrated.candidate.source_ids();
        match action.kind {
            ReviewActionKind::Reject => {
                decision.status = ReviewStatus::Rejected;
                decision.decision = "rejected".into();
                if let Some(subject) = &hydrated.subject
                    && matches!(
                        subject.status,
                        MemoryStatus::Candidate
                            | MemoryStatus::Hypothesis
                            | MemoryStatus::DreamArtifact
                    )
                    && matches!(
                        hydrated.candidate.kind,
                        ReviewCandidateKind::DreamArtifact
                            | ReviewCandidateKind::DreamResearchSuccessor
                            | ReviewCandidateKind::General
                    )
                {
                    mutation.memories.push(ReviewMemoryMutation::patch(
                        subject.id.clone(),
                        MemoryPatch {
                            status: Some(MemoryStatus::Rejected),
                            ..Default::default()
                        },
                    ));
                }
            }
            ReviewActionKind::Defer => {
                let until = action.deferred_until.filter(|until| *until > now_ms());
                if until.is_none() {
                    return invalid("deferred review requires a future resurface time");
                }
                decision.status = ReviewStatus::Deferred;
                decision.decision = "deferred".into();
                decision.deferred_until = until;
            }
            ReviewActionKind::Approve | ReviewActionKind::EditAndApprove => {
                let Some(subject) = &hydrated.subject else {
                    return invalid("review approval requires a subject memory");
                };
                let Some(target) = approval_status(&hydrated, &action) else {
                    return invalid("review kind requires an explicit typed resolution");
                };
                if target == MemoryStatus::Validated && action.evidence_uri.is_empty() {
                    return invalid("validation requires independently supplied evidence");
                }
                let mut patch = edited_patch(&action);
                patch.status = Some(target);
                if target == MemoryStatus::Validated {
                    patch.provenance = Some(Provenance {
                        source_type: SourceType::Document,
                        source_uri: action.evidence_uri.clone(),
                        created_by: action.actor.clone(),
                        evidence_strength: 1.0,
                        ..subject.provenance.clone()
                    });
                }
                if hydrated.candidate.kind == ReviewCandidateKind::ProcedureRevision
                    && patch.content.is_none()
                {
                    let proposed = match &hydrated.candidate.payload {
                        ReviewCandidatePayload::ProcedureRevision(procedure) => {
                            procedure.proposed_content.clone()
                        }
                        _ => String::new(),
                    };
                    if proposed.is_empty() {
                        return invalid("procedure revision has no proposed content");
                    }
                    patch.content = Some(clean_content(&proposed));
                }
                mutation
                    .memories
                    .push(ReviewMemoryMutation::patch(subject.id.clone(), patch));
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                let target_name = match target {
                    MemoryStatus::Validated => "validated",
                    MemoryStatus::Active => "active",
                    _ => "hypothesis",
                };
                let prefix = if action.kind == ReviewActionKind::EditAndApprove {
                    "edited_and_approved_as_"
                } else {
                    "approved_as_"
                };
                decision.decision = format!("{prefix}{target_name}");
            }
            ReviewActionKind::Relate
            | ReviewActionKind::MarkContradiction
            | ReviewActionKind::KeepBoth => {
                if ids.len() < 2 {
                    return invalid("relationship review requires at least two source memories");
                }
                let edge_type = relationship_edge_type(action.kind);
                for id in ids.iter().skip(1) {
                    mutation.edges.push(review_edge(&ids[0], id, edge_type));
                }
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = action.kind.as_str().into();
            }
            ReviewActionKind::Supersede => {
                if action.target_id.is_empty() || !ids.contains(&action.target_id) {
                    return invalid("supersede requires a target from the reviewed source set");
                }
                for id in ids.iter().filter(|id| **id != action.target_id) {
                    mutation.memories.push(ReviewMemoryMutation::patch(
                        id.clone(),
                        MemoryPatch {
                            status: Some(MemoryStatus::Superseded),
                            replacement_id: Some(action.target_id.clone()),
                            ..Default::default()
                        },
                    ));
                    mutation
                        .edges
                        .push(review_edge(id, &action.target_id, "superseded_by"));
                }
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = "superseded".into();
            }
            ReviewActionKind::Merge => {
                let candidate = hydrated.candidate.consolidation();
                if (candidate.is_none() && hydrated.candidate.kind != ReviewCandidateKind::Conflict)
                    || ids.len() < 2
                {
                    return invalid("merge requires at least two reviewed source memories");
                }
                let edit = action.edit.as_ref();
                let content = clean_content(
                    &edit
                        .and_then(|edit| edit.content.clone())
                        .or_else(|| candidate.map(|candidate| candidate.canonical_content.clone()))
                        .unwrap_or_default(),
                );
                if content.is_empty() {
                    return invalid("merge requires canonical content");
                }
                let edited_type = edit.and_then(|edit| edit.memory_type);
                let memory_type = edited_type
                    .or_else(|| hydrated.find_memory(&ids[0]).map(|first| first.memory_type))
                    .unwrap_or(MemoryType::Observation);
                let canonical = MemoryWrite {
                    content,
                    memory_type,
                    status: MemoryStatus::Hypothesis,
                    source: "review:canonical_merge".into(),
                    provenance: Provenance {
                        source_type: SourceType::ProcessedText,
                        created_by: action.actor.clone(),
                        evidence_strength: 0.0,
                        ..Default::default()
                    },
                    confidence: edit.and_then(|edit| edit.confidence).unwrap_or(0.5),
                    tags: edit
                        .and_then(|edit| edit.tags.clone())
                        .unwrap_or_else(|| vec!["reviewed_canonical".into()]),
                    quality_score: 0.0,
                };
                mutation
                    .memories
                    .push(ReviewMemoryMutation::create(canonical, None));
                let canonical_id = review_created_reference(0);
                for id in &ids {
                    if hydrated.find_memory(id).is_none() {
                        return Err(MemoryError::invalid(format!(
                            "merge source is missing or outside scope: {id}"
                        )));
                    }
                    mutation.memories.push(ReviewMemoryMutation::patch(
                        id.clone(),
                        MemoryPatch {
                            status: Some(MemoryStatus::Superseded),
                            replacement_id: Some(canonical_id.clone()),
                            ..Default::default()
                        },
                    ));
                    mutation
                        .edges
                        .push(review_edge(id, &canonical_id, "superseded_by"));
                }
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = "merged".into();
            }
            ReviewActionKind::NarrowScope => {
                let (Some(subject), Some(edit)) = (&hydrated.subject, &action.edit) else {
                    return invalid("narrow scope requires a subject and explicit target scope");
                };
                let Some(target_scope) = edit.scope.clone() else {
                    return invalid("narrow scope requires a subject and explicit target scope");
                };
                let narrowed = MemoryWrite {
                    content: edit
                        .content
                        .clone()
                        .unwrap_or_else(|| subject.content.clone()),
                    memory_type: edit.memory_type.unwrap_or(subject.memory_type),
                    status: edit.target_status.unwrap_or(
                        if subject.status == MemoryStatus::Validated {
                            MemoryStatus::Active
                        } else {
                            subject.status
                        },
                    ),
                    source: "review:narrow_scope".into(),
                    provenance: Provenance {
                        created_by: action.actor.clone(),
                        ..subject.provenance.clone()
                    },
                    tags: edit.tags.clone().unwrap_or_else(|| subject.tags.clone()),
                    confidence: edit.confidence.unwrap_or(subject.confidence),
                    quality_score: subject.quality_score,
                };
                mutation
                    .memories
                    .push(ReviewMemoryMutation::create(narrowed, Some(target_scope)));
                let replacement = review_created_reference(0);
                let mut patch = edited_patch(&action);
                patch.status = Some(MemoryStatus::Superseded);
                patch.replacement_id = Some(replacement.clone());
                mutation
                    .memories
                    .push(ReviewMemoryMutation::patch(subject.id.clone(), patch));
                mutation
                    .edges
                    .push(review_edge(&subject.id, &replacement, "superseded_by"));
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = "narrowed_scope".into();
            }
            ReviewActionKind::Deprecate => {
                let Some(subject) = &hydrated.subject else {
                    return invalid("deprecate requires a subject memory");
                };
                mutation.memories.push(ReviewMemoryMutation::patch(
                    subject.id.clone(),
                    MemoryPatch {
                        status: Some(MemoryStatus::Deprecated),
                        ..Default::default()
                    },
                ));
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = "deprecated".into();
            }
            ReviewActionKind::Retain => {
                let subject = hydrated.subject.as_ref().filter(|_| {
                    hydrated.candidate.kind == ReviewCandidateKind::Staleness
                        && !action.note.is_empty()
                });
                let Some(subject) = subject else {
                    return invalid("retain requires a staleness subject and explicit rationale");
                };
                let mut tags = subject.tags.clone();
                if !tags.iter().any(|tag| tag == "review:retained_current") {
                    tags.push("review:retained_current".into());
                }
                mutation.memories.push(ReviewMemoryMutation::patch(
                    subject.id.clone(),
                    MemoryPatch {
                        tags: Some(tags),
                        ..Default::default()
                    },
                ));
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = "retained_current".into();
            }
            ReviewActionKind::Revalidate => {
                let Some(subject) = hydrated
                    .subject
                    .as_ref()
                    .filter(|_| !action.evidence_uri.is_empty())
                else {
                    return invalid("revalidation requires a subject and current evidence URI");
                };
                mutation.memories.push(ReviewMemoryMutation::patch(
                    subject.id.clone(),
                    MemoryPatch {
                        status: Some(MemoryStatus::Validated),
                        provenance: Some(Provenance {
                            source_type: SourceType::Document,
                            source_uri: action.evidence_uri.clone(),
                            created_by: action.actor.clone(),
                            evidence_strength: 1.0,
                            ..subject.provenance.clone()
                        }),
                        ..Default::default()
                    },
                ));
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = "revalidated".into();
            }
            ReviewActionKind::AttachEvidence => {
                let Some(subject) = hydrated
                    .subject
                    .as_ref()
                    .filter(|_| !action.evidence_uri.is_empty())
                else {
                    return invalid("attach evidence requires a subject and evidence URI");
                };
                mutation.memories.push(ReviewMemoryMutation::patch(
                    subject.id.clone(),
                    MemoryPatch {
                        provenance: Some(Provenance {
                            source_type: SourceType::Document,
                            source_uri: action.evidence_uri.clone(),
                            created_by: action.actor.clone(),
                            evidence_strength: subject.provenance.evidence_strength.max(0.7),
                            ..subject.provenance.clone()
                        }),
                        ..Default::default()
                    },
                ));
                mutation.require_domain_mutation = true;
                decision.status = ReviewStatus::Accepted;
                decision.decision = "evidence_attached".into();
            }
            ReviewActionKind::Reopen => {
                decision.status = ReviewStatus::Pending;
                decision.decision = "reopen".into();
            }
            ReviewActionKind::Compensate => {
                let Some(edit) = action
                    .edit
                    .as_ref()
                    .filter(|_| !action.target_id.is_empty())
                else {
                    return invalid(
                        "compensation requires target memory and explicit replacement values",
                    );
                };
                let target = self.get_memory(scope, &action.target_id).map_err(|_| {
                    MemoryError::invalid(
                        "compensation target is missing or outside the authority scope",
                    )
                })?;
                if !exact_scope(&target.scope, scope)? {
                    return invalid("compensation target requires the exact target scope");
                }
                if edit.target_status.is_some() || edit.replacement_id.is_some() {
                    return invalid(
                        "compensation cannot bypass lifecycle or replacement governance",
                    );
                }
                mutation.memories.push(ReviewMemoryMutation::patch(
                    action.target_id.clone(),
                    edited_patch(&action),
                ));
                mutation.require_domain_mutation = true;
                decision.status = hydrated.item.status;
                decision.decision = "compensate".into();
                decision.application_status = "compensated".into();
            }
        }
        self.store
            .apply_review_mutation(scope, review_id, decision, mutation)
    }
}

fn idempotency_material(result: &ReviewMutationPreview, review: &HydratedReview) -> Result<String> {
    let action = &result.action;
    let mut material = format!(
        "{}:{}:{}:{}:{}:{}",
        result.review_id,
        result.review_revision,
        action.kind.as_str(),
        action.target_id,
        action.note,
        action.evidence_uri
    );
    if let Some(until) = action.deferred_until {
        material += &format!(":defer={until}");
    }
    if let Some(edit) = &action.edit {
        if let Some(content) = &edit.content {
            material += &format!(":content={content}");
        }
        if let Some(memory_type) = edit.memory_type {
            material += &format!(":type={}", memory_type.as_str());
        }
        if let Some(status) = edit.target_status {
            material += &format!(":status={}", status.as_str());
        }
        if let Some(confidence) = edit.confidence {
            // `std::to_string(double)` renders six decimals.
            material += &format!(":confidence={confidence:.6}");
        }
        if let Some(replacement) = &edit.replacement_id {
            material += &format!(":replacement={replacement}");
        }
        if let Some(scope) = &edit.scope {
            material += &format!(":scope={}", scope_key(scope)?);
        }
        if let Some(tags) = &edit.tags {
            for tag in tags {
                material += &format!(":tag={tag}");
            }
        }
    }
    for memory in review.subject.iter().chain(review.sources.iter()) {
        material += &format!(
            ":memory={}:{}:{}",
            memory.id,
            stable_hash_hex(&memory.content),
            memory.updated_at
        );
    }
    Ok(material)
}

/// Sorts by queue score (desc), then creation time, then ID, de-duplicates
/// by ID and applies the query's page.
pub(crate) fn rank_and_page(mut items: Vec<ReviewItem>, query: &ReviewQuery) -> Vec<ReviewItem> {
    let now = now_ms();
    let scores: HashMap<String, f64> = items
        .iter()
        .map(|item| (item.id.clone(), review_queue_score(item, now)))
        .collect();
    items.sort_by(|left, right| {
        let (a, b) = (scores[&left.id], scores[&right.id]);
        b.partial_cmp(&a)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| {
                if query.oldest_first {
                    left.created_at.cmp(&right.created_at)
                } else {
                    right.created_at.cmp(&left.created_at)
                }
            })
            .then_with(|| left.id.cmp(&right.id))
    });
    items.dedup_by(|right, left| left.id == right.id);
    items
        .into_iter()
        .skip(query.offset)
        .take(query.limit)
        .collect()
}

fn merge_statistics(result: &mut ReviewStatistics, durable: ReviewStatistics) {
    let exact_pending = result.pending;
    macro_rules! add {
        ($($field:ident),*) => { $( result.$field += durable.$field; )* };
    }
    add!(
        pending,
        urgent,
        due_deferred,
        reviewed,
        approved,
        edited_and_approved,
        rejected,
        deferred,
        resurfaced,
        reopened,
        merged,
        superseded,
        validated,
        failed,
        application_errors,
        idempotent_replays,
        compensated,
        dreaming_pending,
        dreaming_approved,
        dreaming_rejected,
        unresolved_critical_conflicts,
        stale_validated_subjects,
        unprovenanced_active_subjects,
        high_use_low_trust_subjects,
        batch_runs,
        batch_items,
        batch_applied_items,
        batch_failed_items,
        batch_partial_failures,
        batch_retries,
        batch_idempotent_replays,
        batch_compensations,
        batch_application_errors
    );
    result.oldest_pending_age_seconds = result
        .oldest_pending_age_seconds
        .max(durable.oldest_pending_age_seconds);
    if result.pending != 0 {
        result.median_pending_age_seconds = ((result.median_pending_age_seconds as f64
            * exact_pending as f64
            + durable.median_pending_age_seconds as f64 * durable.pending as f64)
            / result.pending as f64) as i64;
    }
    macro_rules! max {
        ($($field:ident),*) => { $( result.$field = result.$field.max(durable.$field); )* };
    }
    max!(
        time_to_first_decision_p50_seconds,
        time_to_first_decision_p95_seconds,
        review_duration_p50_seconds,
        review_duration_p95_seconds
    );
    macro_rules! counts {
        ($($field:ident),*) => { $( for (key, value) in durable.$field { *result.$field.entry(key).or_default() += value; } )* };
    }
    counts!(
        pending_by_kind,
        pending_by_priority,
        pending_by_scope,
        pending_by_risk_class,
        by_kind,
        by_priority,
        by_status,
        by_source_type,
        by_application_status,
        by_outcome
    );
    macro_rules! ages {
        ($($field:ident),*) => { $( for (key, value) in durable.$field {
            let entry = result.$field.entry(key).or_default();
            *entry = (*entry).max(value);
        } )* };
    }
    ages!(
        oldest_pending_age_by_kind_seconds,
        median_pending_age_by_kind_seconds,
        oldest_pending_age_by_priority_seconds,
        median_pending_age_by_priority_seconds,
        oldest_pending_age_by_scope_seconds,
        median_pending_age_by_scope_seconds,
        oldest_pending_age_by_risk_class_seconds,
        median_pending_age_by_risk_class_seconds
    );
}
