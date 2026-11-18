//! JSON representation shared by CLI, RPC, tools and UI (archived
//! `review_json.cpp`). Every surface output is secret-redacted.

use serde_json::{Value, json};

use super::model::*;
use crate::error::{MemoryError, Result};
use crate::model::*;
use crate::text::redacted;

fn safe_text(value: &str) -> String {
    redacted(value)
}

fn safe_json(value: Value) -> Value {
    let encoded = redacted(&value.to_string());
    serde_json::from_str(&encoded).unwrap_or_else(|_| Value::String("[REDACTED]".into()))
}

fn scope_json(scope: &MemoryScope) -> Value {
    json!({
        "user": scope.user, "project": scope.project, "repository": scope.repository,
        "branch": scope.branch, "session": scope.session, "task": scope.task, "agent": scope.agent,
    })
}

fn provenance_json(provenance: &Provenance) -> Value {
    json!({
        "source_type": provenance.source_type.as_str(),
        "source_uri": safe_text(&provenance.source_uri),
        "source_hash": safe_text(&provenance.source_hash),
        "source_span": safe_text(&provenance.source_span),
        "created_by": safe_text(&provenance.created_by),
        "evidence_strength": provenance.evidence_strength,
    })
}

/// Memory as shown in review surfaces.
pub fn review_memory_json(memory: &MemoryEntry) -> Value {
    json!({
        "id": memory.id, "scope": scope_json(&memory.scope),
        "type": memory.memory_type.as_str(), "status": memory.status.as_str(),
        "content": safe_text(&memory.content), "source": safe_text(&memory.source),
        "tags": memory.tags, "confidence": memory.confidence, "quality_score": memory.quality_score,
        "provenance": provenance_json(&memory.provenance),
        "replacement_id": memory.replacement_id, "superseded_by": memory.superseded_by,
        "access_count": memory.access_count, "created_at": memory.created_at, "updated_at": memory.updated_at,
    })
}

fn edit_json(edit: &ReviewEdit) -> Value {
    let mut value = json!({});
    if let Some(content) = &edit.content {
        value["content"] = json!(content);
    }
    if let Some(memory_type) = edit.memory_type {
        value["type"] = json!(memory_type.as_str());
    }
    if let Some(status) = edit.target_status {
        value["target_status"] = json!(status.as_str());
    }
    if let Some(confidence) = edit.confidence {
        value["confidence"] = json!(confidence);
    }
    if let Some(scope) = &edit.scope {
        value["scope"] = scope_json(scope);
    }
    if let Some(tags) = &edit.tags {
        value["tags"] = json!(tags);
    }
    if let Some(replacement) = &edit.replacement_id {
        value["replacement_id"] = json!(replacement);
    }
    value
}

fn action_json(action: &ReviewAction) -> Value {
    let normalized = normalize_review_action(action.clone()).unwrap_or_else(|_| action.clone());
    let mut result = json!({
        "kind": normalized.kind.as_str(), "actor": normalized.actor, "note": normalized.note,
        "target_id": normalized.target_id, "evidence_uri": normalized.evidence_uri,
        "expected_revision": normalized.expected_revision, "idempotency_key": normalized.idempotency_key,
        "human_authority": normalized.human_authority,
    });
    let mut payload = json!({});
    if let Some(until) = normalized.deferred_until {
        result["deferred_until"] = json!(until);
        payload["until"] = json!(until);
    }
    if !normalized.target_id.is_empty() {
        payload["target_id"] = json!(normalized.target_id);
    }
    if !normalized.evidence_uri.is_empty() {
        payload["evidence_uri"] = json!(normalized.evidence_uri);
    }
    if let Some(edit) = &normalized.edit {
        result["edit"] = edit_json(edit);
        payload["edit"] = edit_json(edit);
    }
    result["payload"] = payload;
    result
}

pub fn review_item_json(item: &ReviewItem) -> Value {
    json!({
        "id": item.id, "kind": item.kind, "candidate_version": item.candidate_version,
        "revision": item.revision, "status": item.status.as_str(), "priority": item.priority,
        "reason": safe_text(&item.reason), "subject_id": item.subject_id,
        "provenance": provenance_json(&item.provenance),
        "candidate": safe_json(review_candidate_json(&decode_review_candidate(item))),
        "decision": safe_text(&item.decision), "decided_by": safe_text(&item.decided_by),
        "decision_note": safe_text(&item.decision_note), "deferred_until": item.deferred_until,
        "application_status": item.application_status,
        "application_error": safe_text(&item.application_error), "action_key": item.action_key,
        "queue_score": review_queue_score(item, now_ms()),
        "queue_signals": {
            "critical_type": item.queue_signals.critical_type,
            "trust_risk": item.queue_signals.trust_risk,
            "access_count": item.queue_signals.access_count,
            "recent_context": item.queue_signals.recent_context,
            "procedure_failures": item.queue_signals.procedure_failures,
        },
        "tags": safe_json(json!(item.tags)),
        "created_at": item.created_at, "updated_at": item.updated_at, "decided_at": item.decided_at,
    })
}

pub fn hydrated_review_json(review: &HydratedReview) -> Value {
    json!({
        "review": review_item_json(&review.item),
        "candidate": safe_json(review_candidate_json(&review.candidate)),
        "sources": review.sources.iter().map(review_memory_json).collect::<Vec<_>>(),
        "related": review.related.iter().map(review_memory_json).collect::<Vec<_>>(),
        "edges": review.edges.iter().map(|edge| json!({
            "id": edge.id, "from_id": edge.from_id, "to_id": edge.to_id, "type": edge.edge_type,
            "confidence": edge.confidence, "source": edge.source, "created_at": edge.created_at,
        })).collect::<Vec<_>>(),
        "procedure_runs": review.procedure_runs.iter().map(|run| json!({
            "id": run.id, "procedure_id": run.procedure_id, "task_id": run.task_id,
            "outcome": run.outcome.as_str(), "notes": safe_text(&run.notes),
            "evidence_uri": safe_text(&run.evidence_uri), "created_at": run.created_at,
        })).collect::<Vec<_>>(),
        "risk": {
            "score": review.risk.score, "class": review.risk.risk_class,
            "components": review.risk.components,
            "explanations": safe_json(json!(review.risk.explanations)),
        },
        "recall": {
            "strict": review.recall.strict, "balanced": review.recall.balanced,
            "broad": review.recall.broad, "warnings": safe_json(json!(review.recall.warnings)),
        },
        "blocking_diagnostics": safe_json(json!(review.blocking_diagnostics)),
        "subject": review.subject.as_ref().map(review_memory_json),
    })
}

pub fn review_preview_json(preview: &ReviewMutationPreview) -> Value {
    let encode = |text: &str| match serde_json::from_str::<Value>(text) {
        Ok(value) => safe_json(value),
        Err(_) => Value::String(safe_text(text)),
    };
    json!({
        "review_id": preview.review_id, "review_revision": preview.review_revision,
        "idempotency_token": preview.idempotency_token,
        "action": safe_json(action_json(&preview.action)),
        "changes": preview.changes.iter().map(|change| json!({
            "record_type": change.record_type, "record_id": change.record_id,
            "operation": change.operation, "before_json": encode(&change.before_json),
            "after_json": encode(&change.after_json),
        })).collect::<Vec<_>>(),
        "will_do": preview.will_do, "will_not_do": preview.will_not_do,
        "warnings": safe_json(json!(preview.warnings)),
        "requires_human_authority": preview.requires_human_authority,
        "applicable": preview.applicable,
    })
}

pub fn review_application_json(result: &ReviewApplicationResult) -> Value {
    json!({
        "review_id": result.review_id, "status": result.status.as_str(),
        "decision_persisted": result.decision_persisted,
        "domain_mutations_applied": result.domain_mutations_applied,
        "idempotent_replay": result.idempotent_replay, "revision": result.revision,
        "action_id": result.action_id, "changed_ids": result.changed_ids,
        "failed_ids": result.failed_ids, "error": safe_text(&result.error),
    })
}

pub fn review_action_record_json(record: &ReviewActionRecord) -> Value {
    json!({
        "id": record.id, "review_id": record.review_id, "action_key": record.action_key,
        "action": record.action, "actor": record.actor, "note": safe_text(&record.note),
        "before_status": record.before_status.as_str(), "after_status": record.after_status.as_str(),
        "before_revision": record.before_revision, "after_revision": record.after_revision,
        "application_status": record.application_status.as_str(),
        "application_error": safe_text(&record.application_error),
        "result": match serde_json::from_str::<Value>(&record.result_json) {
            Ok(value) => safe_json(value),
            Err(_) => Value::String(safe_text(&record.result_json)),
        },
        "replay_count": record.replay_count, "created_at": record.created_at,
    })
}

pub fn review_batch_preview_json(preview: &ReviewBatchPreview) -> Value {
    json!({
        "idempotency_token": preview.idempotency_token,
        "items": preview.items.iter().map(review_preview_json).collect::<Vec<_>>(),
        "warnings": preview.warnings, "applicable": preview.applicable,
    })
}

pub fn review_batch_result_json(result: &ReviewBatchResult) -> Value {
    json!({
        "idempotency_token": result.idempotency_token,
        "results": result.items.iter().map(|item| json!({
            "review_id": item.review_id, "error_code": item.error_code, "error": safe_text(&item.error),
            "result": item.result.as_ref().map(review_application_json),
        })).collect::<Vec<_>>(),
        "applied_count": result.applied_count, "failed_count": result.failed_count,
        "partial_failure": result.partial_failure,
    })
}

pub fn review_batch_job_json(job: &ReviewBatchJob) -> Value {
    json!({
        "job_id": job.id, "status": job.status.as_str(), "total_count": job.total_count,
        "processed_count": job.processed_count, "error_code": job.error_code,
        "error": safe_text(&job.error), "result": job.result.as_ref().map(review_batch_result_json),
    })
}

pub fn review_statistics_json(value: &ReviewStatistics) -> Value {
    json!({
        "pending": value.pending, "urgent": value.urgent, "due_deferred": value.due_deferred,
        "reviewed": value.reviewed, "approved": value.approved,
        "edited_and_approved": value.edited_and_approved, "rejected": value.rejected,
        "deferred": value.deferred, "resurfaced": value.resurfaced, "reopened": value.reopened,
        "merged": value.merged, "superseded": value.superseded, "validated": value.validated,
        "failed": value.failed, "application_errors": value.application_errors,
        "idempotent_replays": value.idempotent_replays, "compensated": value.compensated,
        "dreaming_pending": value.dreaming_pending, "dreaming_approved": value.dreaming_approved,
        "dreaming_rejected": value.dreaming_rejected,
        "oldest_pending_age_seconds": value.oldest_pending_age_seconds,
        "median_pending_age_seconds": value.median_pending_age_seconds,
        "time_to_first_decision_p50_seconds": value.time_to_first_decision_p50_seconds,
        "time_to_first_decision_p95_seconds": value.time_to_first_decision_p95_seconds,
        "review_duration_p50_seconds": value.review_duration_p50_seconds,
        "review_duration_p95_seconds": value.review_duration_p95_seconds,
        "unresolved_critical_conflicts": value.unresolved_critical_conflicts,
        "stale_validated_subjects": value.stale_validated_subjects,
        "unprovenanced_active_subjects": value.unprovenanced_active_subjects,
        "high_use_low_trust_subjects": value.high_use_low_trust_subjects,
        "batch_runs": value.batch_runs, "batch_items": value.batch_items,
        "batch_applied_items": value.batch_applied_items, "batch_failed_items": value.batch_failed_items,
        "batch_partial_failures": value.batch_partial_failures, "batch_retries": value.batch_retries,
        "batch_idempotent_replays": value.batch_idempotent_replays,
        "batch_compensations": value.batch_compensations,
        "batch_application_errors": value.batch_application_errors,
        "pending_by_kind": value.pending_by_kind, "pending_by_priority": value.pending_by_priority,
        "pending_by_scope": value.pending_by_scope, "pending_by_risk_class": value.pending_by_risk_class,
        "by_kind": value.by_kind, "by_priority": value.by_priority, "by_status": value.by_status,
        "by_source_type": value.by_source_type, "by_application_status": value.by_application_status,
        "by_outcome": value.by_outcome,
        "oldest_pending_age_by_kind_seconds": value.oldest_pending_age_by_kind_seconds,
        "median_pending_age_by_kind_seconds": value.median_pending_age_by_kind_seconds,
        "oldest_pending_age_by_priority_seconds": value.oldest_pending_age_by_priority_seconds,
        "median_pending_age_by_priority_seconds": value.median_pending_age_by_priority_seconds,
        "oldest_pending_age_by_scope_seconds": value.oldest_pending_age_by_scope_seconds,
        "median_pending_age_by_scope_seconds": value.median_pending_age_by_scope_seconds,
        "oldest_pending_age_by_risk_class_seconds": value.oldest_pending_age_by_risk_class_seconds,
        "median_pending_age_by_risk_class_seconds": value.median_pending_age_by_risk_class_seconds,
    })
}

fn string_array(input: &Value, field: &str) -> Result<Vec<String>> {
    let Some(value) = input.get(field) else {
        return Ok(Vec::new());
    };
    let Some(items) = value.as_array() else {
        return Err(MemoryError::invalid(format!("{field} must be an array")));
    };
    items
        .iter()
        .map(|item| {
            item.as_str()
                .map(str::to_string)
                .ok_or_else(|| MemoryError::invalid(format!("{field} must contain only strings")))
        })
        .collect()
}

fn string_value(input: &Value, field: &str, fallback: &str) -> Result<String> {
    match input.get(field) {
        None => Ok(fallback.to_string()),
        Some(Value::String(text)) => Ok(text.clone()),
        Some(_) => Err(MemoryError::invalid(format!("{field} must be a string"))),
    }
}

fn bool_value(input: &Value, field: &str, fallback: bool) -> Result<bool> {
    match input.get(field) {
        None => Ok(fallback),
        Some(Value::Bool(value)) => Ok(*value),
        Some(_) => Err(MemoryError::invalid(format!("{field} must be a boolean"))),
    }
}

fn size_value(input: &Value, field: &str, fallback: usize) -> Result<usize> {
    match input.get(field) {
        None => Ok(fallback),
        Some(value) if value.is_u64() => usize::try_from(value.as_u64().unwrap_or(0))
            .map_err(|_| MemoryError::invalid(format!("{field} is too large"))),
        Some(value) if value.is_i64() => Err(MemoryError::invalid(format!(
            "{field} must be a non-negative integer"
        ))),
        Some(_) => Err(MemoryError::invalid(format!(
            "{field} must be a non-negative integer"
        ))),
    }
}

fn timepoint(input: &Value, field: &str) -> Result<Millis> {
    input.get(field).and_then(Value::as_i64).ok_or_else(|| {
        MemoryError::invalid(format!("{field} must be a Unix timestamp in milliseconds"))
    })
}

fn number_value(input: &Value, field: &str) -> Result<f64> {
    input
        .get(field)
        .and_then(Value::as_f64)
        .ok_or_else(|| MemoryError::invalid(format!("{field} must be a number")))
}

/// Parses an RPC/CLI review query object.
pub fn review_query_from_json(input: &Value) -> Result<ReviewQuery> {
    if !input.is_object() {
        return Err(MemoryError::invalid("review query must be an object"));
    }
    let mut query = ReviewQuery::default();
    if input.get("statuses").is_some() {
        query.statuses = string_array(input, "statuses")?
            .iter()
            .map(|status| ReviewStatus::parse(status))
            .collect::<Result<_>>()?;
    }
    query.kinds = string_array(input, "kinds")?;
    query.priorities = string_array(input, "priorities")?;
    for source in string_array(input, "source_types")? {
        let parsed = SourceType::parse(&source);
        if parsed == SourceType::Unknown && source != "unknown" {
            return Err(MemoryError::invalid(format!(
                "invalid review source type: {source}"
            )));
        }
        query.source_types.push(parsed);
    }
    query.tags = string_array(input, "tags")?;
    query.dream_cycles = string_array(input, "dream_cycles")?;
    if input.get("stale").is_some() {
        query.stale = Some(bool_value(input, "stale", false)?);
    }
    if input.get("conflict").is_some() {
        query.conflict = Some(bool_value(input, "conflict", false)?);
    }
    for (field, target) in [
        ("created_after", &mut query.created_after),
        ("created_before", &mut query.created_before),
        ("decided_after", &mut query.decided_after),
        ("deferred_due_before", &mut query.deferred_due_before),
    ] {
        if input.get(field).is_some() {
            *target = Some(timepoint(input, field)?);
        }
    }
    query.include_due_deferred = bool_value(input, "include_due_deferred", true)?;
    query.oldest_first = bool_value(input, "oldest_first", false)?;
    query.scope = match string_value(input, "scope", "all")?.as_str() {
        "all" => ReviewScopeSelector::All,
        "exact" => ReviewScopeSelector::Exact,
        "durable" => ReviewScopeSelector::Durable,
        _ => {
            return Err(MemoryError::invalid(
                "review query scope must be all, exact, or durable",
            ));
        }
    };
    query.limit = size_value(input, "limit", 50)?;
    query.offset = size_value(input, "offset", 0)?;
    if query.limit == 0 || query.limit > 1000 {
        return Err(MemoryError::invalid(
            "review query limit must be from 1 to 1000",
        ));
    }
    Ok(query)
}

/// Parses an RPC/CLI review action; actor and authority come from the caller.
pub fn review_action_from_json(
    input: &Value,
    actor: &str,
    human_authority: bool,
) -> Result<ReviewAction> {
    let Some(kind) = input
        .get("action")
        .and_then(Value::as_str)
        .filter(|_| input.is_object())
    else {
        return Err(MemoryError::invalid("review action requires string action"));
    };
    let mut action = ReviewAction::new(ReviewActionKind::parse(kind)?, actor);
    action.human_authority = human_authority;
    action.note = string_value(input, "note", "")?;
    action.target_id = string_value(input, "target_id", "")?;
    action.evidence_uri = string_value(input, "evidence_uri", "")?;
    action.expected_revision = size_value(input, "expected_revision", 0)? as u64;
    action.idempotency_key = string_value(input, "idempotency_key", "")?;
    if input.get("deferred_until").is_some() {
        action.deferred_until = Some(timepoint(input, "deferred_until")?);
    }
    let payload = match input.get("payload") {
        None => None,
        Some(payload @ Value::Object(_)) => Some(payload),
        Some(_) => {
            return Err(MemoryError::invalid(
                "review action payload must be an object",
            ));
        }
    };
    if let Some(payload) = payload {
        if payload.get("target_id").is_some() {
            action.target_id = string_value(payload, "target_id", "")?;
        }
        if payload.get("evidence_uri").is_some() {
            action.evidence_uri = string_value(payload, "evidence_uri", "")?;
        }
        if payload.get("until").is_some() {
            action.deferred_until = Some(timepoint(payload, "until")?);
        }
    }
    let edit_owner = payload
        .filter(|payload| payload.get("edit").is_some())
        .unwrap_or(input);
    if let Some(value) = edit_owner.get("edit") {
        if !value.is_object() {
            return Err(MemoryError::invalid("edit must be an object"));
        }
        let mut edit = ReviewEdit::default();
        if value.get("content").is_some() {
            edit.content = Some(string_value(value, "content", "")?);
        }
        if value.get("type").is_some() {
            edit.memory_type = Some(MemoryType::parse(&string_value(value, "type", "")?)?);
        }
        if value.get("target_status").is_some() {
            edit.target_status = Some(MemoryStatus::parse(&string_value(
                value,
                "target_status",
                "",
            )?)?);
        }
        if value.get("confidence").is_some() {
            edit.confidence = Some(number_value(value, "confidence")?);
        }
        if let Some(scope_value) = value.get("scope") {
            if !scope_value.is_object() {
                return Err(MemoryError::invalid("edit.scope must be an object"));
            }
            edit.scope = Some(MemoryScope {
                kind: ScopeKind::parse(&string_value(scope_value, "kind", "exact")?)?,
                user: string_value(scope_value, "user", "")?,
                organization: string_value(scope_value, "organization", "")?,
                project: string_value(scope_value, "project", "")?,
                repository: string_value(scope_value, "repository", "")?,
                repository_display: string_value(scope_value, "repository_display", "")?,
                branch: string_value(scope_value, "branch", "")?,
                task: string_value(scope_value, "task", "")?,
                agent: string_value(scope_value, "agent", "")?,
                session: string_value(scope_value, "session", "")?,
                conversation: string_value(scope_value, "conversation", "")?,
            });
        }
        if value.get("tags").is_some() {
            edit.tags = Some(string_array(value, "tags")?);
        }
        if value.get("replacement_id").is_some() {
            edit.replacement_id = Some(string_value(value, "replacement_id", "")?);
        }
        action.edit = Some(edit);
    }
    normalize_review_action(action)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_query_with_validation() {
        let query = review_query_from_json(
            &json!({"statuses": ["pending", "deferred"], "scope": "durable", "limit": 5}),
        )
        .unwrap();
        assert_eq!(
            query.statuses,
            [ReviewStatus::Pending, ReviewStatus::Deferred]
        );
        assert_eq!(query.scope, ReviewScopeSelector::Durable);
        assert_eq!(query.limit, 5);
        assert_eq!(
            review_query_from_json(&json!({"limit": 0}))
                .unwrap_err()
                .to_string(),
            "review query limit must be from 1 to 1000"
        );
        assert_eq!(
            review_query_from_json(&json!({"source_types": ["bogus"]}))
                .unwrap_err()
                .to_string(),
            "invalid review source type: bogus"
        );
        assert_eq!(
            review_query_from_json(&json!({"scope": "galaxy"}))
                .unwrap_err()
                .to_string(),
            "review query scope must be all, exact, or durable"
        );
        assert_eq!(
            review_query_from_json(&json!({"offset": -1}))
                .unwrap_err()
                .to_string(),
            "offset must be a non-negative integer"
        );
    }

    #[test]
    fn parses_actions_with_payloads() {
        let action = review_action_from_json(
            &json!({"action": "defer", "payload": {"until": 99}, "note": "later"}),
            "user:cli",
            true,
        )
        .unwrap();
        assert_eq!(action.kind, ReviewActionKind::Defer);
        assert_eq!(action.deferred_until, Some(99));
        assert!(action.human_authority);
        let edit = review_action_from_json(
            &json!({"action": "edit_and_approve", "edit": {"content": "x", "type": "decision", "tags": ["a"]}}),
            "u",
            false,
        )
        .unwrap();
        let edit = edit.edit.unwrap();
        assert_eq!(edit.memory_type, Some(MemoryType::Decision));
        assert_eq!(edit.tags, Some(vec!["a".into()]));
        assert_eq!(
            review_action_from_json(&json!({}), "u", false)
                .unwrap_err()
                .to_string(),
            "review action requires string action"
        );
    }

    #[test]
    fn preview_json_redacts_secrets() {
        let preview = ReviewMutationPreview {
            review_id: "r".into(),
            action: ReviewAction {
                note: "password=hunter2".into(),
                ..ReviewAction::new(ReviewActionKind::Reject, "u")
            },
            review_revision: 0,
            idempotency_token: "t".into(),
            changes: Vec::new(),
            will_do: Vec::new(),
            will_not_do: Vec::new(),
            warnings: vec!["see /home/alice/x".into()],
            requires_human_authority: true,
            applicable: true,
        };
        let value = review_preview_json(&preview);
        // As in the archived encoder, a `password=` match runs to the next
        // whitespace, so the compact action JSON no longer parses and the
        // whole action is withheld rather than partially shown.
        assert_eq!(value["action"], "[REDACTED]");
        assert_eq!(value["warnings"][0], "see [REDACTED]");
    }
}
