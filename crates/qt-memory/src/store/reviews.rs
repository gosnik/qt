//! Review queue storage: items, decisions with revision compare-and-set,
//! idempotent action keys, atomic review mutations, history, scheduled
//! resurfacing, persisted view state and statistics.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use rusqlite::params;
use rusqlite::types::Value as Sql;
use serde_json::{Value, json};

use super::memories::memory_json;
use super::{Db, MemoryStore, Tx, int_at, key, make_id, placeholders, real_at, text_at};
use crate::error::{MemoryError, Result, invalid};
use crate::model::*;
use crate::review::model::risk_class;
use crate::review::{
    QueueSignals, ReviewActionRecord, ReviewApplicationResult, ReviewApplicationStatus,
    ReviewAtomicMutation, ReviewDecision, ReviewItem, ReviewItemWrite, ReviewMemoryOperation,
    ReviewQuery, ReviewScopeSelector, ReviewStatistics, ReviewViewState, decode_review_candidate,
    review_candidate_json, review_created_reference, review_queue_score,
};
use crate::scope::{scope_can_access, validate_scope};
use crate::text::{
    MAXIMUM_DURABLE_METADATA, MAXIMUM_DURABLE_URI, dump, sanitize_json_default, sanitize_meta,
    sanitize_text,
};

pub(crate) const REVIEW_COLUMNS: &str = "id,kind,status,priority,reason,subject_id,source_id,source_type,source_uri,\
source_hash,source_span,created_by,evidence_strength,candidate_json,\
decision,decided_by,decision_note,tags_json,created_at,updated_at,decided_at,\
candidate_version,revision,deferred_until,application_status,\
application_error,action_key";

struct RawReview {
    item: ReviewItem,
    status: String,
}

fn read_review_row(row: &rusqlite::Row<'_>) -> RawReview {
    let tags = serde_json::from_str::<Value>(&text_at(row, 17))
        .ok()
        .and_then(|value| {
            value.as_array().map(|items| {
                items
                    .iter()
                    .filter_map(|tag| tag.as_str().map(str::to_string))
                    .collect()
            })
        })
        .unwrap_or_default();
    let item = ReviewItem {
        id: text_at(row, 0),
        kind: text_at(row, 1),
        priority: text_at(row, 3),
        reason: text_at(row, 4),
        subject_id: text_at(row, 5),
        provenance: Provenance {
            source_id: text_at(row, 6),
            source_type: SourceType::parse(&text_at(row, 7)),
            source_uri: text_at(row, 8),
            source_hash: text_at(row, 9),
            source_span: text_at(row, 10),
            created_by: text_at(row, 11),
            evidence_strength: real_at(row, 12),
        },
        candidate_json: text_at(row, 13),
        decision: text_at(row, 14),
        decided_by: text_at(row, 15),
        decision_note: text_at(row, 16),
        tags,
        created_at: int_at(row, 18),
        updated_at: int_at(row, 19),
        decided_at: int_at(row, 20),
        candidate_version: u32::try_from(int_at(row, 21)).unwrap_or(1),
        revision: int_at(row, 22).max(0) as u64,
        deferred_until: int_at(row, 23),
        application_status: text_at(row, 24),
        application_error: text_at(row, 25),
        action_key: text_at(row, 26),
        ..Default::default()
    };
    RawReview {
        item,
        status: text_at(row, 2),
    }
}

fn finish_review(raw: RawReview) -> Result<ReviewItem> {
    let mut item = raw.item;
    item.status = ReviewStatus::parse(&raw.status)?;
    Ok(item)
}

fn string_list(value: &Value, key: &str) -> Vec<String> {
    value
        .get(key)
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

/// Archived view-state JSON.
pub(crate) fn view_state_json(state: &ReviewViewState) -> Value {
    let optional = |value: Option<Millis>| value.unwrap_or(0);
    let query = &state.query;
    json!({
        "statuses": query.statuses.iter().map(|status| status.as_str()).collect::<Vec<_>>(),
        "kinds": query.kinds,
        "priorities": query.priorities,
        "source_types": query.source_types.iter().map(|source| source.as_str()).collect::<Vec<_>>(),
        "tags": query.tags,
        "dream_cycles": query.dream_cycles,
        "stale": query.stale,
        "conflict": query.conflict,
        "scope": query.scope.as_str(),
        "created_after": optional(query.created_after),
        "created_before": optional(query.created_before),
        "decided_after": optional(query.decided_after),
        "deferred_due_before": optional(query.deferred_due_before),
        "include_due_deferred": query.include_due_deferred,
        "oldest_first": query.oldest_first,
        "limit": query.limit.clamp(1, 10_000),
        "offset": query.offset,
        "selected_review_id": state.selected_review_id,
        "queue_offset": state.queue_offset,
    })
}

pub(crate) fn view_state_from_json(value: &Value, updated_at: Millis) -> ReviewViewState {
    let mut state = ReviewViewState::default();
    let query = &mut state.query;
    if value.get("statuses").is_some() {
        query.statuses = string_list(value, "statuses")
            .iter()
            .filter_map(|status| ReviewStatus::parse(status).ok())
            .collect();
    }
    query.kinds = string_list(value, "kinds");
    query.priorities = string_list(value, "priorities");
    query.source_types = string_list(value, "source_types")
        .iter()
        .map(|source| SourceType::parse(source))
        .collect();
    query.tags = string_list(value, "tags");
    query.dream_cycles = string_list(value, "dream_cycles");
    query.stale = value.get("stale").and_then(Value::as_bool);
    query.conflict = value.get("conflict").and_then(Value::as_bool);
    query.scope = match value.get("scope").and_then(Value::as_str) {
        Some("exact") => ReviewScopeSelector::Exact,
        Some("durable") => ReviewScopeSelector::Durable,
        _ => ReviewScopeSelector::All,
    };
    let time = |key: &str| {
        value
            .get(key)
            .and_then(Value::as_i64)
            .filter(|stored| *stored > 0)
    };
    query.created_after = time("created_after");
    query.created_before = time("created_before");
    query.decided_after = time("decided_after");
    query.deferred_due_before = time("deferred_due_before");
    query.include_due_deferred = value
        .get("include_due_deferred")
        .and_then(Value::as_bool)
        .unwrap_or(true);
    query.oldest_first = value
        .get("oldest_first")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    query.limit = value
        .get("limit")
        .and_then(Value::as_u64)
        .unwrap_or(50)
        .clamp(1, 10_000) as usize;
    query.offset = value.get("offset").and_then(Value::as_u64).unwrap_or(0) as usize;
    state.selected_review_id = value
        .get("selected_review_id")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string();
    state.queue_offset = value
        .get("queue_offset")
        .and_then(Value::as_u64)
        .unwrap_or(0) as usize;
    state.updated_at = updated_at;
    state
}

fn percentile(values: &[i64], fraction: f64) -> i64 {
    let index = ((fraction * values.len() as f64).ceil() as usize).saturating_sub(1);
    values[index.min(values.len() - 1)]
}

fn age_distribution(
    values: BTreeMap<String, Vec<i64>>,
    oldest: &mut BTreeMap<String, i64>,
    median: &mut BTreeMap<String, i64>,
) {
    for (name, mut ages) in values {
        ages.sort_unstable();
        oldest.insert(name.clone(), *ages.last().unwrap_or(&0));
        median.insert(name, ages[(ages.len() - 1) / 2]);
    }
}

fn resolve_created(reference: &str, created_ids: &[String]) -> Result<String> {
    let Some(suffix) = reference.strip_prefix("$created:") else {
        return Ok(reference.to_string());
    };
    if suffix.is_empty() || !suffix.bytes().all(|byte| byte.is_ascii_digit()) {
        return invalid("invalid review created-memory reference");
    }
    let index: usize = suffix
        .parse()
        .map_err(|_| MemoryError::invalid("review created-memory reference is out of range"))?;
    created_ids
        .get(index)
        .cloned()
        .ok_or_else(|| MemoryError::invalid("review created-memory reference is out of range"))
}

impl Db<'_> {
    pub(crate) fn add_review(
        &self,
        scope: &MemoryScope,
        mut write: ReviewItemWrite,
    ) -> Result<String> {
        let tx = Tx::begin(self.conn, true)?;
        validate_scope(scope)?;
        write.kind = sanitize_text(&write.kind, 256);
        write.priority = sanitize_text(&write.priority, 256);
        write.reason = sanitize_text(&write.reason, MAXIMUM_DURABLE_METADATA);
        write.subject_id = sanitize_meta(&write.subject_id);
        write.provenance.source_id = sanitize_meta(&write.provenance.source_id);
        write.provenance.source_uri =
            sanitize_text(&write.provenance.source_uri, MAXIMUM_DURABLE_URI);
        write.provenance.source_hash = sanitize_meta(&write.provenance.source_hash);
        write.provenance.source_span = sanitize_meta(&write.provenance.source_span);
        write.provenance.created_by = sanitize_meta(&write.provenance.created_by);
        write.candidate_json = sanitize_json_default(&write.candidate_json);
        for tag in &mut write.tags {
            *tag = sanitize_text(tag, 256);
        }
        let probe = ReviewItem {
            kind: write.kind.clone(),
            subject_id: write.subject_id.clone(),
            provenance: write.provenance.clone(),
            candidate_json: write.candidate_json.clone(),
            candidate_version: write.candidate_version,
            reason: write.reason.clone(),
            tags: write.tags.clone(),
            ..Default::default()
        };
        let candidate = decode_review_candidate(&probe);
        if candidate.diagnostics.is_empty() {
            write.candidate_version = candidate.version;
            write.candidate_json = dump(&review_candidate_json(&candidate));
        }
        let id = make_id("review_");
        let now = now_ms();
        self.conn
            .execute(
                &format!(
                    "INSERT INTO review_items({REVIEW_COLUMNS},scope_key) VALUES({});",
                    placeholders(28)
                ),
                params![
                    id,
                    write.kind,
                    "pending",
                    write.priority,
                    write.reason,
                    write.subject_id,
                    write.provenance.source_id,
                    write.provenance.source_type.as_str(),
                    write.provenance.source_uri,
                    write.provenance.source_hash,
                    write.provenance.source_span,
                    write.provenance.created_by,
                    write.provenance.evidence_strength,
                    write.candidate_json,
                    "",
                    "",
                    "",
                    dump(&json!(write.tags)),
                    now,
                    now,
                    0i64,
                    i64::from(write.candidate_version),
                    0i64,
                    0i64,
                    "not_started",
                    "",
                    "",
                    key(scope)?
                ],
            )
            .map_err(|_| MemoryError::storage("unable to add review item"))?;
        self.record_audit(
            scope,
            AuditEventWrite {
                actor: if write.provenance.created_by.is_empty() { "memory".into() } else { write.provenance.created_by.clone() },
                action: "review_create".into(),
                reason: if write.reason.is_empty() { "review item created".into() } else { write.reason.clone() },
                after_id: id.clone(),
                evidence_uri: write.provenance.source_uri.clone(),
                metadata_json: dump(&json!({"review_id": id, "kind": write.kind, "candidate_version": write.candidate_version})),
                ..Default::default()
            },
        )?;
        tx.commit()?;
        Ok(id)
    }

    fn select_reviews(&self, sql: &str, binds: &[Sql]) -> Result<Vec<ReviewItem>> {
        let raws = {
            let mut statement = self.conn.prepare(sql)?;
            statement
                .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                    Ok(read_review_row(row))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()?
        };
        raws.into_iter().map(finish_review).collect()
    }

    pub(crate) fn reviews(
        &self,
        scope: &MemoryScope,
        status: ReviewStatus,
        limit: usize,
    ) -> Result<Vec<ReviewItem>> {
        self.select_reviews(
            &format!("SELECT {REVIEW_COLUMNS} FROM review_items WHERE scope_key=? AND status=? ORDER BY created_at DESC,id LIMIT ?;"),
            &[Sql::Text(key(scope)?), Sql::Text(status.as_str().into()), Sql::Integer(limit as i64)],
        )
    }

    pub(crate) fn review(&self, scope: &MemoryScope, id: &str) -> Result<ReviewItem> {
        self.select_reviews(
            &format!(
                "SELECT {REVIEW_COLUMNS} FROM review_items WHERE scope_key=? AND id=? LIMIT 1;"
            ),
            &[Sql::Text(key(scope)?), Sql::Text(id.into())],
        )?
        .into_iter()
        .next()
        .ok_or_else(|| MemoryError::invalid("review item not found in scope"))
    }

    pub(crate) fn critical_conflict_memory_ids(&self, scope: &MemoryScope) -> Result<Vec<String>> {
        validate_scope(scope)?;
        let items = self.select_reviews(
            &format!(
                "SELECT {REVIEW_COLUMNS} FROM review_items WHERE scope_key=? AND kind='conflict' AND status IN ('pending','deferred');"
            ),
            &[Sql::Text(key(scope)?)],
        )?;
        let mut ids = BTreeSet::new();
        for item in items {
            let candidate = decode_review_candidate(&item);
            let Some(conflict) = candidate.conflict() else {
                continue;
            };
            if conflict.severity != "critical" {
                continue;
            }
            if !item.subject_id.is_empty() {
                ids.insert(item.subject_id.clone());
            }
            ids.extend(conflict.memory_ids.iter().cloned());
        }
        Ok(ids.into_iter().collect())
    }

    pub(crate) fn query_reviews(
        &self,
        scope: &MemoryScope,
        query: &ReviewQuery,
    ) -> Result<Vec<ReviewItem>> {
        validate_scope(scope)?;
        let scope_key = key(scope)?;
        let mut sql = format!("SELECT {REVIEW_COLUMNS} FROM review_items WHERE scope_key=?");
        let mut binds = vec![Sql::Text(scope_key.clone())];
        if let Some(subject) = &query.subject_id {
            sql += " AND subject_id=?";
            binds.push(Sql::Text(subject.clone()));
        }
        if !query.statuses.is_empty() {
            sql += &format!(" AND (status IN ({})", placeholders(query.statuses.len()));
            binds.extend(
                query
                    .statuses
                    .iter()
                    .map(|status| Sql::Text(status.as_str().into())),
            );
            if query.include_due_deferred
                && let Some(due) = query.deferred_due_before
            {
                sql += " OR (status='deferred' AND deferred_until>0 AND deferred_until<=?)";
                binds.push(Sql::Integer(due));
            }
            sql += ")";
        }
        for (values, column) in [(&query.kinds, "kind"), (&query.priorities, "priority")] {
            if !values.is_empty() {
                sql += &format!(" AND {column} IN ({})", placeholders(values.len()));
                binds.extend(values.iter().map(|value| Sql::Text(value.clone())));
            }
        }
        if !query.source_types.is_empty() {
            sql += &format!(
                " AND source_type IN ({})",
                placeholders(query.source_types.len())
            );
            binds.extend(
                query
                    .source_types
                    .iter()
                    .map(|source| Sql::Text(source.as_str().into())),
            );
        }
        if !query.dream_cycles.is_empty() {
            sql += &format!(
                " AND CASE WHEN json_valid(candidate_json) THEN \
                 COALESCE(json_extract(candidate_json,'$.payload.cycle_id'),\
                 json_extract(candidate_json,'$.cycle_id'),'') ELSE '' END IN ({})",
                placeholders(query.dream_cycles.len())
            );
            binds.extend(
                query
                    .dream_cycles
                    .iter()
                    .map(|cycle| Sql::Text(cycle.clone())),
            );
        }
        if let Some(stale) = query.stale {
            sql += if stale {
                " AND kind='staleness'"
            } else {
                " AND kind<>'staleness'"
            };
        }
        if let Some(conflict) = query.conflict {
            sql += if conflict {
                " AND kind='conflict'"
            } else {
                " AND kind<>'conflict'"
            };
        }
        for (value, clause) in [
            (query.created_after, " AND created_at>=?"),
            (query.created_before, " AND created_at<=?"),
            (query.decided_after, " AND decided_at>=?"),
        ] {
            if let Some(value) = value {
                sql += clause;
                binds.push(Sql::Integer(value));
            }
        }
        if let Some(due) = query.deferred_due_before
            && query.statuses.is_empty()
        {
            sql += " AND deferred_until>0 AND deferred_until<=?";
            binds.push(Sql::Integer(due));
        }
        for tag in &query.tags {
            sql += " AND EXISTS (SELECT 1 FROM json_each(review_items.tags_json) WHERE value=?)";
            binds.push(Sql::Text(tag.clone()));
        }
        sql += " ORDER BY CASE \
                WHEN kind='conflict' AND CASE WHEN json_valid(candidate_json) THEN \
                COALESCE(json_extract(candidate_json,'$.severity'),\
                json_extract(candidate_json,'$.payload.severity')) ELSE '' END=\
                'critical' THEN 0 \
                WHEN kind='conflict' THEN 1 \
                WHEN EXISTS (SELECT 1 FROM json_each(review_items.tags_json) \
                WHERE value IN ('memory:critical','review:critical')) THEN 2 ELSE 3 END,\
                 CASE priority WHEN 'urgent' THEN 0 WHEN 'high' THEN 1 \
                WHEN 'normal' THEN 2 WHEN 'low' THEN 3 ELSE 4 END,";
        sql += if query.oldest_first {
            " created_at ASC,id ASC"
        } else {
            " created_at DESC,id ASC"
        };
        sql += " LIMIT ? OFFSET ?;";
        binds.push(Sql::Integer(query.limit as i64));
        binds.push(Sql::Integer(query.offset as i64));
        let mut result = self.select_reviews(&sql, &binds)?;
        let mut subject_ids: Vec<String> = result
            .iter()
            .filter(|item| !item.subject_id.is_empty())
            .map(|item| item.subject_id.clone())
            .collect();
        subject_ids.sort();
        subject_ids.dedup();
        if subject_ids.is_empty() {
            return Ok(result);
        }
        let signals_sql = format!(
            "SELECT m.id,m.memory_type,m.status,m.confidence,m.quality_score,\
             m.access_count,m.failure_count,m.last_accessed_at,\
             COALESCE((SELECT s.trust_level FROM sources s WHERE \
             s.scope_key=m.scope_key AND s.id=m.source_id LIMIT 1),'') \
             FROM memories m WHERE m.scope_key=? AND m.id IN ({});",
            placeholders(subject_ids.len())
        );
        let mut signal_binds = vec![Sql::Text(scope_key)];
        signal_binds.extend(subject_ids.into_iter().map(Sql::Text));
        let recent_cutoff = now_ms() - 7 * 24 * 3_600_000;
        let mut by_subject = HashMap::new();
        {
            let mut statement = self.conn.prepare(&signals_sql)?;
            let mut rows = statement.query(rusqlite::params_from_iter(signal_binds.iter()))?;
            while let Some(row) = rows.next()? {
                let memory_type = text_at(row, 1);
                let status = text_at(row, 2);
                let confidence = real_at(row, 3);
                let quality = real_at(row, 4);
                let trust = text_at(row, 8);
                let trust_risk = if matches!(
                    status.as_str(),
                    "candidate" | "hypothesis" | "dream_artifact"
                ) {
                    10.0
                } else {
                    0.0
                } + (0.6 - confidence).max(0.0) * 15.0
                    + (0.5 - quality).max(0.0) * 10.0
                    + match trust.as_str() {
                        "untrusted" | "low" => 10.0,
                        "working" => 4.0,
                        _ => 0.0,
                    };
                by_subject.entry(text_at(row, 0)).or_insert(QueueSignals {
                    critical_type: matches!(
                        memory_type.as_str(),
                        "constraint" | "decision" | "procedural" | "preference"
                    ),
                    recent_context: int_at(row, 7) >= recent_cutoff,
                    trust_risk,
                    access_count: int_at(row, 5).max(0) as u64,
                    procedure_failures: int_at(row, 6).max(0) as u64,
                });
            }
        }
        for item in &mut result {
            if let Some(signals) = by_subject.get(&item.subject_id) {
                item.queue_signals = signals.clone();
            }
        }
        Ok(result)
    }

    pub(crate) fn decide_review(
        &self,
        scope: &MemoryScope,
        id: &str,
        mut decision: ReviewDecision,
    ) -> Result<ReviewItem> {
        if decision.decided_by.is_empty() {
            return invalid("review decision requires actor");
        }
        decision.decision = sanitize_meta(&decision.decision);
        decision.decided_by = sanitize_meta(&decision.decided_by);
        decision.note = sanitize_text(&decision.note, MAXIMUM_DURABLE_METADATA);
        decision.action_key = sanitize_meta(&decision.action_key);
        decision.application_status = sanitize_text(&decision.application_status, 64);
        decision.application_error =
            sanitize_text(&decision.application_error, MAXIMUM_DURABLE_METADATA);
        decision.application_result_json = sanitize_json_default(&decision.application_result_json);
        let now = now_ms();
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        if !decision.action_key.is_empty() {
            let prior: Option<String> = {
                let mut statement = self
                    .conn
                    .prepare_cached("SELECT review_id FROM review_actions WHERE scope_key=? AND action_key=? LIMIT 1;")?;
                let mut rows = statement.query(params![scope_key, decision.action_key])?;
                rows.next()?.map(|row| text_at(row, 0))
            };
            if let Some(review_id) = prior {
                if review_id != id {
                    return invalid("review action key belongs to another review");
                }
                self.conn
                    .execute(
                        "UPDATE review_actions SET replay_count=replay_count+1 WHERE scope_key=? AND action_key=?;",
                        params![scope_key, decision.action_key],
                    )
                    .map_err(|_| MemoryError::storage("unable to record idempotent review replay"))?;
                tx.commit()?;
                return self.review(scope, id);
            }
        }
        let before = self.review(scope, id)?;
        let resurfacing = decision.decision == "resurface";
        let reopening = decision.decision == "reopen" || resurfacing;
        let compensating = decision.decision == "compensate";
        if (reopening || compensating) && decision.note.is_empty() {
            return invalid("review reopen or compensation requires an explicit reason");
        }
        if (!reopening && !compensating && before.status != ReviewStatus::Pending)
            || ((reopening || compensating) && before.status == ReviewStatus::Pending)
        {
            return invalid("review decision rejected: item is not in the required state");
        }
        if reopening && decision.status != ReviewStatus::Pending {
            return invalid("reopen must transition the review to pending");
        }
        if compensating && decision.status != before.status {
            return invalid(
                "compensation records a new action without rewriting the terminal decision",
            );
        }
        if before.revision != decision.expected_revision {
            return invalid("review decision rejected: stale revision");
        }
        let deferred_until = if decision.status == ReviewStatus::Deferred {
            decision.deferred_until.unwrap_or(0)
        } else {
            0
        };
        let changed = if compensating {
            self.conn.execute(
                "UPDATE review_items SET updated_at=?,revision=revision+1,application_status=?,application_error=?,action_key=? WHERE id=? AND scope_key=? AND revision=? AND status=?;",
                params![
                    now,
                    decision.application_status,
                    decision.application_error,
                    decision.action_key,
                    id,
                    scope_key,
                    decision.expected_revision as i64,
                    before.status.as_str()
                ],
            )
        } else {
            self.conn.execute(
                "UPDATE review_items SET status=?,decision=?,decided_by=?,decision_note=?,updated_at=?,decided_at=?,deferred_until=?,revision=revision+1,application_status=?,application_error=?,action_key=? WHERE id=? AND scope_key=? AND revision=? AND status=?;",
                params![
                    decision.status.as_str(),
                    decision.decision,
                    decision.decided_by,
                    decision.note,
                    now,
                    if decision.status == ReviewStatus::Pending { 0 } else { now },
                    deferred_until,
                    decision.application_status,
                    decision.application_error,
                    decision.action_key,
                    id,
                    scope_key,
                    decision.expected_revision as i64,
                    before.status.as_str()
                ],
            )
        };
        if !matches!(changed, Ok(1)) {
            return invalid("review decision rejected by concurrent update");
        }
        let action_id = make_id("review_action_");
        self.conn
            .execute(
                "INSERT INTO review_actions(id,scope_key,review_id,action_key,action,actor,note,before_status,after_status,before_revision,after_revision,application_status,application_error,result_json,replay_count,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                params![
                    action_id,
                    scope_key,
                    id,
                    decision.action_key,
                    decision.decision,
                    decision.decided_by,
                    decision.note,
                    before.status.as_str(),
                    decision.status.as_str(),
                    before.revision as i64,
                    before.revision as i64 + 1,
                    decision.application_status,
                    decision.application_error,
                    decision.application_result_json,
                    0i64,
                    now
                ],
            )
            .map_err(|_| MemoryError::storage("unable to record review action"))?;
        let after = self.review(scope, id)?;
        let application_result =
            match serde_json::from_str::<Value>(&decision.application_result_json) {
                Ok(value @ Value::Object(_)) => value,
                _ => json!({}),
            };
        let evidence_uris = string_list(&application_result, "evidence_uris");
        let reason = if decision.note.is_empty() {
            decision.decision.clone()
        } else {
            decision.note.clone()
        };
        let metadata = json!({
            "originating_review": id, "review_id": id, "review_action_id": action_id,
            "reviewer": decision.decided_by, "action": decision.decision,
            "decision": decision.decision, "reason": reason,
            "before_status": before.status.as_str(), "after_status": decision.status.as_str(),
            "before_revision": before.revision, "after_revision": before.revision + 1,
            "before": {"status": before.status.as_str(), "revision": before.revision,
                       "decision": before.decision, "application_status": before.application_status},
            "after": {"status": after.status.as_str(), "revision": after.revision,
                      "decision": after.decision, "application_status": after.application_status},
            "evidence_uris": evidence_uris,
            "replacement_ids": string_list(&application_result, "replacement_ids"),
            "created_ids": string_list(&application_result, "created_ids"),
            "changed_ids": string_list(&application_result, "changed_ids"),
            "application_status": decision.application_status,
            "application_error": decision.application_error,
            "action_key": decision.action_key,
        });
        self.record_audit(
            scope,
            AuditEventWrite {
                actor: decision.decided_by.clone(),
                action: if resurfacing {
                    "review_resurface"
                } else if reopening {
                    "review_reopen"
                } else if compensating {
                    "review_compensation"
                } else {
                    "review_action"
                }
                .into(),
                reason,
                before_id: id.into(),
                after_id: id.into(),
                evidence_uri: evidence_uris.first().cloned().unwrap_or_default(),
                metadata_json: dump(&metadata),
            },
        )?;
        tx.commit()?;
        Ok(after)
    }

    pub(crate) fn review_history(
        &self,
        scope: &MemoryScope,
        review_id: Option<&str>,
        limit: usize,
    ) -> Result<Vec<ReviewActionRecord>> {
        validate_scope(scope)?;
        if limit == 0 || limit > 10_000 {
            return invalid("review history limit must be from 1 to 10000");
        }
        self.review_action_rows(scope, review_id, limit)
    }

    /// Action rows newest first, bounded only by `limit`. Export uses its own
    /// collection limit, which may exceed the interactive history cap.
    pub(crate) fn review_action_rows(
        &self,
        scope: &MemoryScope,
        review_id: Option<&str>,
        limit: usize,
    ) -> Result<Vec<ReviewActionRecord>> {
        let mut sql = String::from(
            "SELECT id,review_id,action_key,action,actor,note,before_status,after_status,\
             before_revision,after_revision,application_status,application_error,\
             result_json,replay_count,created_at FROM review_actions WHERE scope_key=?",
        );
        let mut binds = vec![Sql::Text(key(scope)?)];
        if let Some(review_id) = review_id {
            sql += " AND review_id=?";
            binds.push(Sql::Text(review_id.into()));
        }
        sql += " ORDER BY created_at DESC,id LIMIT ?;";
        binds.push(Sql::Integer(limit as i64));
        let mut statement = self.conn.prepare(&sql)?;
        let rows = statement
            .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                Ok((
                    (
                        text_at(row, 0),
                        text_at(row, 1),
                        text_at(row, 2),
                        text_at(row, 3),
                        text_at(row, 4),
                        text_at(row, 5),
                    ),
                    (
                        text_at(row, 6),
                        text_at(row, 7),
                        int_at(row, 8),
                        int_at(row, 9),
                        text_at(row, 10),
                    ),
                    (
                        text_at(row, 11),
                        text_at(row, 12),
                        int_at(row, 13),
                        int_at(row, 14),
                    ),
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        rows.into_iter()
            .map(|(ids, states, rest)| {
                Ok(ReviewActionRecord {
                    id: ids.0,
                    review_id: ids.1,
                    action_key: ids.2,
                    action: ids.3,
                    actor: ids.4,
                    note: ids.5,
                    before_status: ReviewStatus::parse(&states.0)?,
                    after_status: ReviewStatus::parse(&states.1)?,
                    before_revision: states.2.max(0) as u64,
                    after_revision: states.3.max(0) as u64,
                    application_status: ReviewApplicationStatus::parse(&states.4)?,
                    application_error: rest.0,
                    result_json: rest.1,
                    replay_count: rest.2.max(0) as u64,
                    created_at: rest.3,
                })
            })
            .collect()
    }

    pub(crate) fn resurface_due_reviews(&self, scope: &MemoryScope, now: Millis) -> Result<usize> {
        validate_scope(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let rows: Vec<(String, i64)> = self
            .conn
            .prepare(
                "SELECT id,revision FROM review_items WHERE scope_key=? AND \
                 status='deferred' AND deferred_until>0 AND deferred_until<=? ORDER BY deferred_until,id;",
            )?
            .query_map(params![key(scope)?, now], |row| Ok((text_at(row, 0), int_at(row, 1))))?
            .collect::<rusqlite::Result<_>>()?;
        for (id, revision) in &rows {
            self.decide_review(
                scope,
                id,
                ReviewDecision {
                    status: ReviewStatus::Pending,
                    decision: "resurface".into(),
                    decided_by: "qt:memory_review_scheduler".into(),
                    note: "scheduled review deferral elapsed".into(),
                    expected_revision: *revision as u64,
                    action_key: format!("review:resurface:{id}:{revision}"),
                    application_status: "no_op".into(),
                    ..Default::default()
                },
            )?;
        }
        tx.commit()?;
        Ok(rows.len())
    }

    pub(crate) fn load_review_view_state(
        &self,
        scope: &MemoryScope,
    ) -> Result<Option<ReviewViewState>> {
        let row: Option<(String, i64)> = {
            let mut statement = self.conn.prepare_cached(
                "SELECT state_json,updated_at FROM review_view_state WHERE scope_key=? LIMIT 1;",
            )?;
            let mut rows = statement.query([key(scope)?])?;
            rows.next()?.map(|row| (text_at(row, 0), int_at(row, 1)))
        };
        Ok(row.and_then(
            |(state, updated_at)| match serde_json::from_str::<Value>(&state) {
                Ok(value @ Value::Object(_)) => Some(view_state_from_json(&value, updated_at)),
                _ => None,
            },
        ))
    }

    pub(crate) fn save_review_view_state(
        &self,
        scope: &MemoryScope,
        mut state: ReviewViewState,
    ) -> Result<()> {
        validate_scope(scope)?;
        state.selected_review_id = sanitize_meta(&state.selected_review_id);
        self.conn
            .execute(
                "INSERT INTO review_view_state(scope_key,state_json,updated_at) VALUES(?,?,?) \
                 ON CONFLICT(scope_key) DO UPDATE SET state_json=excluded.state_json,updated_at=excluded.updated_at;",
                params![key(scope)?, sanitize_json_default(&dump(&view_state_json(&state))), now_ms()],
            )
            .map_err(|_| MemoryError::storage("unable to save review view state"))?;
        Ok(())
    }

    pub(crate) fn clear_review_view_state(&self, scope: &MemoryScope) -> Result<()> {
        self.conn
            .execute(
                "DELETE FROM review_view_state WHERE scope_key=?;",
                [key(scope)?],
            )
            .map_err(|_| MemoryError::storage("unable to clear review view state"))?;
        Ok(())
    }

    pub(crate) fn review_statistics(&self, scope: &MemoryScope) -> Result<ReviewStatistics> {
        let scope_key = key(scope)?;
        let now = now_ms();
        let mut result = ReviewStatistics::default();
        type ItemRow = (
            String,
            String,
            String,
            i64,
            i64,
            String,
            String,
            String,
            String,
            f64,
            String,
            String,
        );
        let items: Vec<ItemRow> = self
            .conn
            .prepare(
                "SELECT kind,priority,status,created_at,deferred_until,source_type,candidate_json,\
                 tags_json,subject_id,evidence_strength,source_uri,source_hash FROM review_items \
                 WHERE scope_key=? ORDER BY created_at;",
            )?
            .query_map([&scope_key], |row| {
                Ok((
                    text_at(row, 0),
                    text_at(row, 1),
                    text_at(row, 2),
                    int_at(row, 3),
                    int_at(row, 4),
                    text_at(row, 5),
                    text_at(row, 6),
                    text_at(row, 7),
                    text_at(row, 8),
                    real_at(row, 9),
                    text_at(row, 10),
                    text_at(row, 11),
                ))
            })?
            .collect::<rusqlite::Result<_>>()?;
        let mut pending_ages = Vec::new();
        let mut ages_by_kind: BTreeMap<String, Vec<i64>> = BTreeMap::new();
        let mut ages_by_priority: BTreeMap<String, Vec<i64>> = BTreeMap::new();
        let mut ages_by_scope: BTreeMap<String, Vec<i64>> = BTreeMap::new();
        let mut ages_by_risk: BTreeMap<String, Vec<i64>> = BTreeMap::new();
        for (
            kind,
            priority,
            status,
            created_at,
            deferred_until,
            source_type,
            candidate_json,
            tags_json,
            subject_id,
            evidence,
            uri,
            hash,
        ) in items
        {
            *result.by_kind.entry(kind.clone()).or_default() += 1;
            *result.by_priority.entry(priority.clone()).or_default() += 1;
            *result.by_status.entry(status.clone()).or_default() += 1;
            *result
                .by_source_type
                .entry(source_type.clone())
                .or_default() += 1;
            if status == "pending" {
                result.pending += 1;
                *result.pending_by_kind.entry(kind.clone()).or_default() += 1;
                *result
                    .pending_by_priority
                    .entry(priority.clone())
                    .or_default() += 1;
                *result
                    .pending_by_scope
                    .entry(scope_key.clone())
                    .or_default() += 1;
                if priority == "urgent" || priority == "high" {
                    result.urgent += 1;
                }
                let age = ((now - created_at) / 1000).max(0);
                pending_ages.push(age);
                ages_by_kind.entry(kind.clone()).or_default().push(age);
                ages_by_priority
                    .entry(priority.clone())
                    .or_default()
                    .push(age);
                ages_by_scope
                    .entry(scope_key.clone())
                    .or_default()
                    .push(age);
                let risk_item = ReviewItem {
                    kind: kind.clone(),
                    priority: priority.clone(),
                    created_at,
                    candidate_json: candidate_json.clone(),
                    provenance: Provenance {
                        source_type: SourceType::parse(&source_type),
                        evidence_strength: evidence,
                        source_uri: uri,
                        source_hash: hash,
                        ..Default::default()
                    },
                    ..Default::default()
                };
                let class = risk_class(review_queue_score(&risk_item, now)).to_string();
                *result
                    .pending_by_risk_class
                    .entry(class.clone())
                    .or_default() += 1;
                ages_by_risk.entry(class).or_default().push(age);
                if kind == "dream_artifact" || kind == "dream_research_successor" {
                    result.dreaming_pending += 1;
                }
                let candidate = serde_json::from_str::<Value>(&candidate_json).ok();
                let severity_critical = |value: Option<&Value>| {
                    value
                        .and_then(|value| value.get("severity"))
                        .and_then(Value::as_str)
                        == Some("critical")
                };
                let critical_candidate = candidate.as_ref().is_some_and(|candidate| {
                    candidate.is_object()
                        && (severity_critical(Some(candidate))
                            || severity_critical(
                                candidate
                                    .get("payload")
                                    .filter(|payload| payload.is_object()),
                            ))
                });
                let critical_tag =
                    serde_json::from_str::<Value>(&tags_json)
                        .ok()
                        .is_some_and(|tags| {
                            tags.as_array().is_some_and(|tags| {
                                tags.iter().any(|tag| {
                                    matches!(
                                        tag.as_str(),
                                        Some("memory:critical" | "review:critical")
                                    )
                                })
                            })
                        });
                if kind == "conflict" && (critical_candidate || critical_tag) {
                    result.unresolved_critical_conflicts += 1;
                }
                if !subject_id.is_empty() && kind == "staleness" {
                    let subject_status: Option<String> = {
                        let mut statement = self.conn.prepare_cached(
                            "SELECT status FROM memories WHERE scope_key=? AND id=? LIMIT 1;",
                        )?;
                        let mut rows = statement.query(params![scope_key, subject_id])?;
                        rows.next()?.map(|row| text_at(row, 0))
                    };
                    if subject_status.as_deref() == Some("validated") {
                        result.stale_validated_subjects += 1;
                    }
                }
            } else if status == "deferred" && deferred_until > 0 && deferred_until <= now {
                result.due_deferred += 1;
            }
        }
        if !pending_ages.is_empty() {
            pending_ages.sort_unstable();
            result.oldest_pending_age_seconds = *pending_ages.last().unwrap_or(&0);
            result.median_pending_age_seconds = pending_ages[(pending_ages.len() - 1) / 2];
        }
        age_distribution(
            ages_by_kind,
            &mut result.oldest_pending_age_by_kind_seconds,
            &mut result.median_pending_age_by_kind_seconds,
        );
        age_distribution(
            ages_by_priority,
            &mut result.oldest_pending_age_by_priority_seconds,
            &mut result.median_pending_age_by_priority_seconds,
        );
        age_distribution(
            ages_by_scope,
            &mut result.oldest_pending_age_by_scope_seconds,
            &mut result.median_pending_age_by_scope_seconds,
        );
        age_distribution(
            ages_by_risk,
            &mut result.oldest_pending_age_by_risk_class_seconds,
            &mut result.median_pending_age_by_risk_class_seconds,
        );

        let actions: Vec<(String, String, String, u64)> = self
            .conn
            .prepare(
                "SELECT action,after_status,application_status,COUNT(*) FROM review_actions \
                 WHERE scope_key=? GROUP BY action,after_status,application_status;",
            )?
            .query_map([&scope_key], |row| {
                Ok((
                    text_at(row, 0),
                    text_at(row, 1),
                    text_at(row, 2),
                    int_at(row, 3).max(0) as u64,
                ))
            })?
            .collect::<rusqlite::Result<_>>()?;
        for (action, status, application, count) in actions {
            let compensate = action == "compensate";
            if action != "reopen" && action != "resurface" && !compensate {
                result.reviewed += count;
            }
            *result
                .by_application_status
                .entry(application.clone())
                .or_default() += count;
            *result.by_outcome.entry(action.clone()).or_default() += count;
            if !compensate {
                match status.as_str() {
                    "accepted" => result.approved += count,
                    "rejected" => result.rejected += count,
                    "deferred" => result.deferred += count,
                    _ => {}
                }
            }
            if action == "edited_and_approved" || action.starts_with("edited_and_approved_as_") {
                result.edited_and_approved += count;
            }
            match action.as_str() {
                "reopen" => result.reopened += count,
                "resurface" => result.resurfaced += count,
                "compensate" => result.compensated += count,
                "merged" | "merge" => result.merged += count,
                "superseded" | "supersede" => result.superseded += count,
                _ => {}
            }
            if matches!(
                action.as_str(),
                "revalidated"
                    | "validated"
                    | "approved_as_validated"
                    | "edited_and_approved_as_validated"
            ) {
                result.validated += count;
            }
            if application == "failed" || application == "partially_applied" {
                result.failed += count;
                result.application_errors += count;
            }
        }
        result.idempotent_replays = self.conn.query_row(
            "SELECT COALESCE(SUM(replay_count),0) FROM review_actions WHERE scope_key=?;",
            [&scope_key],
            |row| Ok(int_at(row, 0).max(0) as u64),
        )?;
        let dream: Vec<(String, u64)> = self
            .conn
            .prepare(
                "SELECT r.status,COUNT(*) FROM review_items r WHERE r.scope_key=? AND \
                 r.kind IN ('dream_artifact','dream_research_successor') AND r.status IN \
                 ('accepted','rejected') GROUP BY r.status;",
            )?
            .query_map([&scope_key], |row| {
                Ok((text_at(row, 0), int_at(row, 1).max(0) as u64))
            })?
            .collect::<rusqlite::Result<_>>()?;
        for (status, count) in dream {
            if status == "accepted" {
                result.dreaming_approved = count;
            } else {
                result.dreaming_rejected = count;
            }
        }
        let durations = |extra: &str| -> Result<Vec<i64>> {
            let sql = format!(
                "SELECT (MIN(a.created_at)-r.created_at)/1000 FROM review_items r \
                 JOIN review_actions a ON a.review_id=r.id AND a.scope_key=r.scope_key \
                 WHERE r.scope_key=?{extra} GROUP BY r.id HAVING MIN(a.created_at)>=r.created_at ORDER BY 1;"
            );
            Ok(self
                .conn
                .prepare(&sql)?
                .query_map([&scope_key], |row| Ok(int_at(row, 0)))?
                .collect::<rusqlite::Result<_>>()?)
        };
        let decision_times = durations("")?;
        if !decision_times.is_empty() {
            result.time_to_first_decision_p50_seconds = percentile(&decision_times, 0.50);
            result.time_to_first_decision_p95_seconds = percentile(&decision_times, 0.95);
        }
        let review_durations = durations(" AND a.after_status IN ('accepted','rejected')")?;
        if !review_durations.is_empty() {
            result.review_duration_p50_seconds = percentile(&review_durations, 0.50);
            result.review_duration_p95_seconds = percentile(&review_durations, 0.95);
        }
        let trust: Vec<(String, bool, i64)> = self
            .conn
            .prepare(
                "SELECT status,source_id,source_uri,evidence_strength,access_count FROM memories \
                 WHERE scope_key=? AND status IN ('active','hypothesis');",
            )?
            .query_map([&scope_key], |row| {
                let low_trust = text_at(row, 1).is_empty()
                    && text_at(row, 2).is_empty()
                    && real_at(row, 3) <= 0.0;
                Ok((text_at(row, 0), low_trust, int_at(row, 4)))
            })?
            .collect::<rusqlite::Result<_>>()?;
        for (status, low_trust, accesses) in trust {
            if status == "active" && low_trust {
                result.unprovenanced_active_subjects += 1;
            }
            if accesses >= 5 && low_trust {
                result.high_use_low_trust_subjects += 1;
            }
        }
        let batches: Vec<String> = self
            .conn
            .prepare("SELECT metadata_json FROM audit_events WHERE scope_key=? AND action='review_batch';")?
            .query_map([&scope_key], |row| Ok(text_at(row, 0)))?
            .collect::<rusqlite::Result<_>>()?;
        for payload in batches {
            let Ok(payload @ Value::Object(_)) = serde_json::from_str::<Value>(&payload) else {
                continue;
            };
            let count = |field: &str| payload.get(field).and_then(Value::as_u64);
            result.batch_runs += 1;
            result.batch_items += count("batch_size")
                .or_else(|| count("size"))
                .or_else(|| count("item_count"))
                .unwrap_or(0);
            result.batch_applied_items += count("applied_count").unwrap_or(0);
            result.batch_failed_items += count("failed_count").unwrap_or(0);
            if payload
                .get("partial_failure")
                .and_then(Value::as_bool)
                .unwrap_or(false)
            {
                result.batch_partial_failures += 1;
            }
            result.batch_retries += count("retry_count").unwrap_or(0);
            result.batch_idempotent_replays += count("idempotent_replay_count").unwrap_or(0);
            result.batch_compensations += count("compensation_count").unwrap_or(0);
            result.batch_application_errors += count("application_error_count")
                .or_else(|| count("failed_count"))
                .unwrap_or(0);
        }
        Ok(result)
    }

    pub(crate) fn apply_review_mutation(
        &self,
        scope: &MemoryScope,
        id: &str,
        mut decision: ReviewDecision,
        mut mutation: ReviewAtomicMutation,
    ) -> Result<ReviewApplicationResult> {
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let mut result = ReviewApplicationResult {
            review_id: id.into(),
            ..Default::default()
        };
        if !decision.action_key.is_empty() {
            let prior = {
                let mut statement = self.conn.prepare_cached(
                    "SELECT id,review_id,result_json,after_revision,application_status FROM review_actions \
                     WHERE scope_key=? AND action_key=? LIMIT 1;",
                )?;
                let mut rows = statement.query(params![scope_key, decision.action_key])?;
                rows.next()?.map(|row| {
                    (
                        text_at(row, 0),
                        text_at(row, 1),
                        text_at(row, 2),
                        int_at(row, 3),
                        text_at(row, 4),
                    )
                })
            };
            if let Some((action_id, review_id, result_json, revision, status)) = prior {
                if review_id != id {
                    return invalid("review action key belongs to another review");
                }
                result.action_id = action_id;
                result.revision = revision.max(0) as u64;
                result.status = ReviewApplicationStatus::parse(&status)?;
                result.decision_persisted = true;
                result.idempotent_replay = true;
                if let Ok(prior @ Value::Object(_)) = serde_json::from_str::<Value>(&result_json) {
                    result.domain_mutations_applied = prior
                        .get("domain_mutations_applied")
                        .and_then(Value::as_bool)
                        .unwrap_or(false);
                    result.changed_ids = string_list(&prior, "changed_ids");
                    result.failed_ids = string_list(&prior, "failed_ids");
                    result.error = prior
                        .get("error")
                        .and_then(Value::as_str)
                        .unwrap_or_default()
                        .to_string();
                }
                self.conn
                    .execute(
                        "UPDATE review_actions SET replay_count=replay_count+1 WHERE id=?;",
                        [&result.action_id],
                    )
                    .map_err(|_| {
                        MemoryError::storage("unable to record idempotent review replay")
                    })?;
                tx.commit()?;
                return Ok(result);
            }
        }
        if mutation.require_domain_mutation
            && mutation.memories.is_empty()
            && mutation.edges.is_empty()
        {
            return invalid("review action requires a domain mutation");
        }
        if mutation.expected_review_revision.is_some() || mutation.expected_review_status.is_some()
        {
            let current = self.review(scope, id)?;
            if mutation
                .expected_review_revision
                .is_some_and(|revision| revision != current.revision)
                || mutation
                    .expected_review_status
                    .is_some_and(|status| status != current.status)
            {
                return invalid("review changed during verification");
            }
        }
        for expected in &mutation.expected_memories {
            let current = self.get(scope, &expected.id)?;
            if key(&current.scope)? != scope_key || memory_json(&current) != memory_json(expected) {
                return invalid("review evidence subject changed during verification");
            }
        }
        let reason = if decision.note.is_empty() {
            decision.decision.clone()
        } else {
            decision.note.clone()
        };
        let mut created_ids: Vec<String> = Vec::new();
        let mut replacement_ids: Vec<String> = Vec::new();
        let mut evidence_uris: Vec<String> = Vec::new();
        for memory in &mut mutation.memories {
            if memory.operation != ReviewMemoryOperation::Create {
                continue;
            }
            let expected_reference = review_created_reference(created_ids.len());
            if !memory.memory_id.is_empty() && memory.memory_id != expected_reference {
                return invalid(
                    "review create memory_id must be empty or its $created:N reference",
                );
            }
            let create_scope = memory.target_scope.clone().unwrap_or_else(|| scope.clone());
            validate_scope(&create_scope)?;
            if !scope_can_access(scope, &create_scope, RecallPolicy::Broad, true) {
                return invalid(
                    "review create target scope is unrelated to the review authority scope",
                );
            }
            if create_scope.kind.breadth() > scope.kind.breadth() {
                return invalid(
                    "review create target scope cannot be broader than the review authority scope",
                );
            }
            let created = self.add(&create_scope, std::mem::take(&mut memory.create))?;
            created_ids.push(created.clone());
            result.changed_ids.push(created.clone());
            let created_entry = self.get(&create_scope, &created)?;
            if !created_entry.provenance.source_uri.is_empty() {
                evidence_uris.push(created_entry.provenance.source_uri.clone());
            }
            self.record_audit(
                scope,
                AuditEventWrite {
                    actor: decision.decided_by.clone(),
                    action: "review_memory_create".into(),
                    reason: reason.clone(),
                    after_id: created.clone(),
                    evidence_uri: created_entry.provenance.source_uri.clone(),
                    metadata_json: dump(&json!({
                        "originating_review": id, "review_id": id, "action_key": decision.action_key,
                        "before": null, "after": memory_json(&created_entry),
                        "created_ids": [created], "changed_ids": [created],
                    })),
                    ..Default::default()
                },
            )?;
        }
        for memory in &mut mutation.memories {
            if memory.operation != ReviewMemoryOperation::Patch {
                continue;
            }
            memory.memory_id = resolve_created(&memory.memory_id, &created_ids)?;
            if let Some(replacement) = &memory.patch.replacement_id {
                memory.patch.replacement_id = Some(resolve_created(replacement, &created_ids)?);
            }
            if memory.memory_id.is_empty() {
                return invalid("review memory patch requires an id");
            }
            let before = self.get(scope, &memory.memory_id)?;
            if let Some(expected) = &memory.expected_memory
                && memory_json(&before) != memory_json(expected)
            {
                return invalid("review subject changed during verification");
            }
            if key(&before.scope)? != scope_key {
                return invalid("review mutation subject is outside the exact scope");
            }
            if let Some(replacement) = memory
                .patch
                .replacement_id
                .as_ref()
                .filter(|value| !value.is_empty())
            {
                replacement_ids.push(replacement.clone());
            }
            if let Some(provenance) = memory
                .patch
                .provenance
                .as_ref()
                .filter(|value| !value.source_uri.is_empty())
            {
                evidence_uris.push(provenance.source_uri.clone());
            }
            self.update(scope, &memory.memory_id, &memory.patch)?;
            let after = self.get(scope, &memory.memory_id)?;
            result.changed_ids.push(memory.memory_id.clone());
            self.record_audit(
                scope,
                AuditEventWrite {
                    actor: decision.decided_by.clone(),
                    action: "review_memory_patch".into(),
                    reason: reason.clone(),
                    before_id: memory.memory_id.clone(),
                    after_id: memory.memory_id.clone(),
                    evidence_uri: after.provenance.source_uri.clone(),
                    metadata_json: dump(&json!({
                        "originating_review": id, "review_id": id, "action_key": decision.action_key,
                        "before": memory_json(&before), "after": memory_json(&after),
                        "replacement_ids": replacement_ids, "changed_ids": [memory.memory_id],
                    })),
                },
            )?;
        }
        for edge in &mut mutation.edges {
            edge.from_id = resolve_created(&edge.from_id, &created_ids)?;
            edge.to_id = resolve_created(&edge.to_id, &created_ids)?;
            let written = edge.clone();
            let edge_id = self.add_edge(scope, edge.clone())?;
            result.changed_ids.push(edge_id.clone());
            self.record_audit(
                scope,
                AuditEventWrite {
                    actor: decision.decided_by.clone(),
                    action: "review_graph_change".into(),
                    reason: reason.clone(),
                    after_id: edge_id.clone(),
                    metadata_json: dump(&json!({
                        "originating_review": id, "review_id": id, "action_key": decision.action_key,
                        "before": null,
                        "after": {"id": edge_id, "from_id": written.from_id, "to_id": written.to_id,
                                  "type": written.edge_type, "confidence": written.confidence, "source": written.source},
                        "changed_ids": [edge_id],
                    })),
                    ..Default::default()
                },
            )?;
        }
        let compensation =
            decision.decision == "compensate" || decision.application_status == "compensated";
        decision.application_status = if compensation {
            "compensated"
        } else if mutation.memories.is_empty() && mutation.edges.is_empty() {
            "no_op"
        } else {
            "applied"
        }
        .into();
        decision.application_result_json = dump(&json!({
            "domain_mutations_applied": !result.changed_ids.is_empty(),
            "created_ids": created_ids, "changed_ids": result.changed_ids,
            "replacement_ids": replacement_ids, "evidence_uris": evidence_uris,
            "failed_ids": [], "error": "",
        }));
        let decided = self.decide_review(scope, id, decision)?;
        result.revision = decided.revision;
        if !decided.action_key.is_empty() {
            let mut statement = self.conn.prepare_cached(
                "SELECT id FROM review_actions WHERE scope_key=? AND action_key=? LIMIT 1;",
            )?;
            let mut rows = statement.query(params![scope_key, decided.action_key])?;
            if let Some(row) = rows.next()? {
                result.action_id = text_at(row, 0);
            }
        }
        result.decision_persisted = true;
        result.domain_mutations_applied = !result.changed_ids.is_empty();
        result.status = if compensation {
            ReviewApplicationStatus::Compensated
        } else if result.domain_mutations_applied {
            ReviewApplicationStatus::Applied
        } else {
            ReviewApplicationStatus::NoOp
        };
        tx.commit()?;
        Ok(result)
    }
}

impl MemoryStore {
    /// Queues a review item (status `pending`) and audits its creation.
    pub fn add_review(&self, scope: &MemoryScope, write: ReviewItemWrite) -> Result<String> {
        self.with(|db| db.add_review(scope, write))
    }

    pub fn reviews(
        &self,
        scope: &MemoryScope,
        status: ReviewStatus,
        limit: usize,
    ) -> Result<Vec<ReviewItem>> {
        self.with(|db| db.reviews(scope, status, limit))
    }

    /// One review in exactly `scope`; "review item not found in scope" otherwise.
    pub fn review(&self, scope: &MemoryScope, id: &str) -> Result<ReviewItem> {
        self.with(|db| db.review(scope, id))
    }

    /// Memory IDs named by unresolved critical conflicts, read in one snapshot.
    pub fn critical_conflict_memory_ids(&self, scope: &MemoryScope) -> Result<Vec<String>> {
        self.with(|db| db.critical_conflict_memory_ids(scope))
    }

    /// Filtered queue page in storage order, with subject queue signals.
    pub fn query_reviews(
        &self,
        scope: &MemoryScope,
        query: &ReviewQuery,
    ) -> Result<Vec<ReviewItem>> {
        self.with(|db| db.query_reviews(scope, query))
    }

    /// Records a decision with revision compare-and-set. A repeated action
    /// key replays (counting the replay) instead of deciding twice.
    pub fn decide_review(
        &self,
        scope: &MemoryScope,
        id: &str,
        decision: ReviewDecision,
    ) -> Result<ReviewItem> {
        self.with(|db| db.decide_review(scope, id, decision))
    }

    /// Commits the decision, its memory/graph mutations and their audits in
    /// one transaction.
    pub fn apply_review_mutation(
        &self,
        scope: &MemoryScope,
        id: &str,
        decision: ReviewDecision,
        mutation: ReviewAtomicMutation,
    ) -> Result<ReviewApplicationResult> {
        self.with(|db| db.apply_review_mutation(scope, id, decision, mutation))
    }

    pub fn review_history(
        &self,
        scope: &MemoryScope,
        review_id: Option<&str>,
        limit: usize,
    ) -> Result<Vec<ReviewActionRecord>> {
        self.with(|db| db.review_history(scope, review_id, limit))
    }

    /// Returns deferred reviews whose time has come to pending.
    pub fn resurface_due_reviews(&self, scope: &MemoryScope, now: Millis) -> Result<usize> {
        self.with(|db| db.resurface_due_reviews(scope, now))
    }

    pub fn load_review_view_state(&self, scope: &MemoryScope) -> Result<Option<ReviewViewState>> {
        self.with(|db| db.load_review_view_state(scope))
    }

    pub fn save_review_view_state(
        &self,
        scope: &MemoryScope,
        state: ReviewViewState,
    ) -> Result<()> {
        self.with(|db| db.save_review_view_state(scope, state))
    }

    pub fn clear_review_view_state(&self, scope: &MemoryScope) -> Result<()> {
        self.with(|db| db.clear_review_view_state(scope))
    }

    pub fn review_statistics(&self, scope: &MemoryScope) -> Result<ReviewStatistics> {
        self.with(|db| db.review_statistics(scope))
    }
}
