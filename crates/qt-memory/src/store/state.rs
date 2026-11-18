//! Native JSON export/import of one scope (archived `export_state` /
//! `import_state`). The same format serves backup/restore: an import remaps
//! every memory, entity, graph edge, review and review-action ID it has to
//! re-issue, including IDs embedded in candidate, result, audit and view-state
//! JSON.

use std::collections::HashMap;

use rusqlite::params;
use serde_json::{Map, Value, json};

use super::memories::memory_json;
use super::{Db, MemoryStore, Tx, int_at, is_null_at, key, make_id, real_at, text_at};
use crate::error::{MemoryError, Result, invalid};
use crate::model::*;
use crate::review::{ReviewDecision, ReviewItemWrite};
use crate::scope::validate_scope;
use crate::store::schema::SCHEMA_VERSION;
use crate::text::{
    MAXIMUM_DURABLE_METADATA, MAXIMUM_DURABLE_TEXT, MAXIMUM_DURABLE_URI, dump, remap_json_ids,
    remapped_json, sanitize_json_default, sanitize_meta, sanitize_text,
};

pub(crate) fn scope_json(scope: &MemoryScope) -> Value {
    json!({
        "kind": scope.kind.as_str(), "user": scope.user,
        "organization": scope.organization, "project": scope.project,
        "repository": scope.repository, "repository_display": scope.repository_display,
        "branch": scope.branch, "task": scope.task, "agent": scope.agent,
        "session": scope.session, "conversation": scope.conversation,
    })
}

fn s(value: &Value, field: &str, fallback: &str) -> String {
    value
        .get(field)
        .and_then(Value::as_str)
        .unwrap_or(fallback)
        .to_string()
}

fn i(value: &Value, field: &str, fallback: i64) -> i64 {
    value.get(field).and_then(Value::as_i64).unwrap_or(fallback)
}

fn f(value: &Value, field: &str, fallback: f64) -> f64 {
    value.get(field).and_then(Value::as_f64).unwrap_or(fallback)
}

fn b(value: &Value, field: &str, fallback: bool) -> bool {
    value
        .get(field)
        .and_then(Value::as_bool)
        .unwrap_or(fallback)
}

fn strings(value: &Value, field: &str) -> Vec<String> {
    value
        .get(field)
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .filter_map(|item| item.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

fn array<'a>(state: &'a Value, field: &str) -> &'a [Value] {
    state
        .get(field)
        .and_then(Value::as_array)
        .map(Vec::as_slice)
        .unwrap_or(&[])
}

fn safe_identifier(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

fn scope_matches(encoded: &Value, declared: &MemoryScope, kind_field: &str) -> bool {
    s(encoded, kind_field, "") == declared.kind.as_str()
        && s(encoded, "user", "") == declared.user
        && s(encoded, "organization", "") == declared.organization
        && s(encoded, "project", "") == declared.project
        && s(encoded, "repository", "") == declared.repository
        && s(encoded, "branch", "") == declared.branch
        && s(encoded, "task", "") == declared.task
        && s(encoded, "agent", "") == declared.agent
        && s(encoded, "session", "") == declared.session
        && s(encoded, "conversation", "") == declared.conversation
}

impl Db<'_> {
    fn count(&self, sql: &str, binds: &[String]) -> Result<usize> {
        let count: i64 = self
            .conn
            .query_row(sql, rusqlite::params_from_iter(binds.iter()), |row| {
                row.get(0)
            })
            .map_err(|_| MemoryError::storage("unable to count bounded memory export rows"))?;
        Ok(count.max(0) as usize)
    }

    pub(crate) fn export_state(&self, scope: &MemoryScope, limits: &ExportLimits) -> Result<Value> {
        if limits
            .all()
            .iter()
            .any(|limit| *limit == 0 || *limit > 100_000)
        {
            return invalid("memory export collection limits must be from 1 to 100000");
        }
        let scope_key = key(scope)?;
        let mut state = Map::new();
        state.insert("schema_version".into(), json!(SCHEMA_VERSION));
        state.insert("scope_key".into(), json!(scope_key));
        state.insert("scope".into(), scope_json(scope));
        state.insert(
            "export_limits".into(),
            json!({
                "memories": limits.memories, "graph_edges": limits.graph_edges,
                "procedure_runs": limits.procedure_runs, "revisions": limits.revisions,
                "sources": limits.sources, "source_events": limits.source_events,
                "reviews": limits.reviews, "audit_events": limits.audit_events,
                "session_events": limits.session_events, "blocks": limits.blocks,
            }),
        );
        let keyed = std::slice::from_ref(&scope_key);
        let rows = |table: &str| {
            self.count(
                &format!("SELECT COUNT(*) FROM {table} WHERE scope_key IN (?);"),
                keyed,
            )
        };
        state.insert(
            "truncated".into(),
            json!({
                "memories": rows("memories")? > limits.memories,
                "graph_edges": rows("graph_edges")? > limits.graph_edges,
                "procedure_runs": rows("procedure_runs")? > limits.procedure_runs,
                "revisions": rows("memory_revisions")? > limits.revisions,
                "sources": rows("sources")? > limits.sources,
                "source_events": rows("source_events")? > limits.source_events,
                "reviews": rows("review_items")? > limits.reviews,
                "review_actions": rows("review_actions")? > limits.audit_events,
                "audit_events": rows("audit_events")? > limits.audit_events,
                "session_events": rows("session_events")? > limits.session_events,
                "blocks": rows("context_blocks")? > limits.blocks,
                "entities": self.count("SELECT COUNT(*) FROM memory_entities WHERE scope_key=?", keyed)? > limits.memories,
                "assertions": self.count(
                    "SELECT COUNT(*) FROM memory_assertions a JOIN memories m ON m.id=a.memory_id WHERE m.scope_key=?",
                    keyed,
                )? > limits.revisions,
                "assertion_revision_boundaries": self.count(
                    "SELECT COUNT(*) FROM memory_assertion_revisions r JOIN memories m ON m.id=r.memory_id WHERE m.scope_key=?",
                    keyed,
                )? > limits.revisions,
                "cross_store_links": self.count(
                    "SELECT COUNT(*) FROM retrieval_cross_store_links WHERE scope_key=?",
                    keyed,
                )? > limits.revisions,
            }),
        );
        let query = MemoryQuery {
            policy: RecallPolicy::Strict,
            include_superseded: true,
            limit: limits.memories,
            ..Default::default()
        };
        let exported_memories = self.query(scope, &query)?;
        let mut memories = Vec::with_capacity(exported_memories.len());
        for memory in &exported_memories {
            let mut encoded = memory_json(memory);
            let mut statement = self
                .conn
                .prepare_cached("SELECT revision FROM retrieval_owners WHERE owner_key=?;")?;
            let mut revision_rows = statement.query([format!("memory:{}", memory.id)])?;
            if let Some(row) = revision_rows.next()? {
                encoded["retrieval_revision"] = json!(int_at(row, 0));
            }
            memories.push(encoded);
        }
        state.insert("memories".into(), Value::Array(memories));

        let entity_rows: Vec<(String, String, String, String)> = self
            .conn
            .prepare("SELECT id,kind,name,scope_json FROM memory_entities WHERE scope_key=? ORDER BY id LIMIT ?;")?
            .query_map(params![scope_key, limits.memories as i64], |row| {
                Ok((text_at(row, 0), text_at(row, 1), text_at(row, 2), text_at(row, 3)))
            })?
            .collect::<rusqlite::Result<_>>()?;
        let mut entities = Vec::new();
        for (id, kind, name, scope_text) in entity_rows {
            let aliases: Vec<String> = self
                .conn
                .prepare_cached(
                    "SELECT alias FROM memory_entity_aliases WHERE entity_id=? ORDER BY alias;",
                )?
                .query_map([&id], |row| Ok(text_at(row, 0)))?
                .collect::<rusqlite::Result<_>>()?;
            entities.push(json!({
                "id": id, "kind": kind, "name": name,
                "scope": serde_json::from_str::<Value>(&scope_text).unwrap_or(Value::Null),
                "aliases": aliases,
            }));
        }
        state.insert("entities".into(), Value::Array(entities));

        let boundaries: Vec<Value> = self
            .conn
            .prepare(
                "SELECT r.memory_id,r.revision,r.recorded_at FROM memory_assertion_revisions r \
                 JOIN memories m ON m.id=r.memory_id WHERE m.scope_key=? ORDER BY r.memory_id,r.revision LIMIT ?;",
            )?
            .query_map(params![scope_key, limits.revisions as i64], |row| {
                Ok(json!({"memory_id": text_at(row, 0), "revision": int_at(row, 1), "recorded_at": int_at(row, 2)}))
            })?
            .collect::<rusqlite::Result<_>>()?;
        let assertions: Vec<Value> = self
            .conn
            .prepare(
                "SELECT a.memory_id,a.revision,a.subject_id,a.predicate,a.object_id,a.literal_json,\
                 a.qualifiers_json,a.positive,a.valid_from,a.valid_to,a.evidence_json,a.recorded_at,a.historical_json \
                 FROM memory_assertions a JOIN memories m ON m.id=a.memory_id \
                 WHERE m.scope_key=? ORDER BY a.memory_id,a.revision LIMIT ?;",
            )?
            .query_map(params![scope_key, limits.revisions as i64], |row| {
                let mut item = json!({
                    "memory_id": text_at(row, 0), "revision": int_at(row, 1),
                    "subject_id": text_at(row, 2), "predicate": text_at(row, 3),
                    "object_id": text_at(row, 4), "literal_json": text_at(row, 5),
                    "qualifiers_json": text_at(row, 6), "positive": int_at(row, 7) != 0,
                    "evidence": serde_json::from_str::<Value>(&text_at(row, 10)).unwrap_or(Value::Null),
                    "recorded_at": int_at(row, 11), "historical_json": text_at(row, 12),
                });
                if !is_null_at(row, 8) {
                    item["valid_from"] = json!(int_at(row, 8));
                }
                if !is_null_at(row, 9) {
                    item["valid_to"] = json!(int_at(row, 9));
                }
                Ok(item)
            })?
            .collect::<rusqlite::Result<_>>()?;
        state.insert("assertions".into(), Value::Array(assertions));
        state.insert(
            "assertion_revision_boundaries".into(),
            Value::Array(boundaries),
        );

        let links: Vec<Value> = self
            .conn
            .prepare(
                "SELECT id,scope_json,from_kind,from_id,from_revision,from_repository,from_worktree,\
                 from_path,from_fingerprint,from_generation,to_kind,to_id,to_revision,to_repository,\
                 to_worktree,to_path,to_fingerprint,to_generation,relation,evidence_id,created_at \
                 FROM retrieval_cross_store_links WHERE scope_key=? ORDER BY id LIMIT ?;",
            )?
            .query_map(params![scope_key, limits.revisions as i64], |row| {
                let endpoint = |offset: usize| {
                    json!({
                        "kind": text_at(row, offset), "id": text_at(row, offset + 1),
                        "revision": int_at(row, offset + 2), "repository": text_at(row, offset + 3),
                        "worktree": text_at(row, offset + 4), "path": text_at(row, offset + 5),
                        "source_fingerprint": text_at(row, offset + 6),
                        "snapshot_generation": int_at(row, offset + 7),
                    })
                };
                Ok(json!({
                    "id": text_at(row, 0),
                    "scope": serde_json::from_str::<Value>(&text_at(row, 1)).unwrap_or(Value::Null),
                    "from": endpoint(2), "to": endpoint(10),
                    "relation": text_at(row, 18), "evidence_id": text_at(row, 19),
                    "created_at": int_at(row, 20),
                }))
            })?
            .collect::<rusqlite::Result<_>>()?;
        state.insert("cross_store_links".into(), Value::Array(links));

        let edges: Vec<Value> = self
            .conn
            .prepare(
                "SELECT id,from_id,to_id,type,confidence,source,created_at FROM \
                 graph_edges WHERE scope_key=? ORDER BY created_at DESC,id LIMIT ?;",
            )?
            .query_map(params![scope_key, limits.graph_edges as i64], |row| {
                Ok(json!({
                    "id": text_at(row, 0), "from_id": text_at(row, 1), "to_id": text_at(row, 2),
                    "type": sanitize_text(&text_at(row, 3), 256), "confidence": real_at(row, 4),
                    "source": sanitize_meta(&text_at(row, 5)), "created_at": int_at(row, 6),
                }))
            })?
            .collect::<rusqlite::Result<_>>()?;
        state.insert("graph_edges".into(), Value::Array(edges));

        let blocks: Vec<Value> = self
            .conn
            .prepare("SELECT id,content,updated_at FROM context_blocks WHERE scope_key=? ORDER BY id LIMIT ?;")?
            .query_map(params![scope_key, limits.blocks as i64], |row| {
                Ok(json!({
                    "id": text_at(row, 0),
                    "content": sanitize_text(&text_at(row, 1), MAXIMUM_DURABLE_TEXT),
                    "updated_at": int_at(row, 2),
                }))
            })?
            .collect::<rusqlite::Result<_>>()?;
        state.insert("blocks".into(), Value::Array(blocks));

        let mut exported_sources = self.sources(scope)?;
        exported_sources.truncate(limits.sources);
        state.insert(
            "sources".into(),
            Value::Array(
                exported_sources
                    .iter()
                    .map(|source| {
                        json!({
                            "id": source.id, "type": source.source_type.as_str(), "uri": source.uri,
                            "hash": source.hash, "display_name": source.display_name,
                            "trust_level": source.trust_level, "hash_status": source.hash_status,
                            "ingest_count": source.ingest_count, "imported_at": source.imported_at,
                            "last_ingested_at": source.last_ingested_at, "last_checked_at": source.last_checked_at,
                        })
                    })
                    .collect(),
            ),
        );
        let mut source_events = Vec::new();
        'sources: for source in &exported_sources {
            for event in self.source_events(scope, &source.id)? {
                if source_events.len() >= limits.source_events {
                    break 'sources;
                }
                source_events.push(json!({
                    "id": event.id, "source_id": event.source_id, "action": event.action,
                    "status": event.status, "source_hash": event.source_hash, "note": event.note,
                    "created_at": event.created_at,
                }));
            }
        }
        state.insert("source_events".into(), Value::Array(source_events));

        let mut revisions = Vec::new();
        let mut runs = Vec::new();
        for memory in &exported_memories {
            for revision in self.revisions(scope, &memory.id)? {
                if revisions.len() >= limits.revisions {
                    break;
                }
                revisions.push(json!({
                    "id": revision.id, "memory_id": revision.memory_id, "kind": revision.kind,
                    "entry_json": revision.entry_json, "created_at": revision.created_at,
                }));
            }
            if memory.memory_type == MemoryType::Procedural {
                for run in self.runs(scope, &memory.id)? {
                    if runs.len() >= limits.procedure_runs {
                        break;
                    }
                    runs.push(json!({
                        "id": run.id, "procedure_id": run.procedure_id, "task_id": run.task_id,
                        "outcome": run.outcome.as_str(), "notes": run.notes, "evidence_uri": run.evidence_uri,
                        "deduplication_key": run.deduplication_key, "created_at": run.created_at,
                    }));
                }
            }
        }
        state.insert("memory_revisions".into(), Value::Array(revisions));
        state.insert("procedure_runs".into(), Value::Array(runs));

        let mut reviews = Vec::new();
        'statuses: for status in ReviewStatus::ALL {
            for review in self.reviews(scope, *status, limits.reviews)? {
                if reviews.len() >= limits.reviews {
                    break 'statuses;
                }
                reviews.push(json!({
                    "id": review.id, "kind": review.kind, "status": review.status.as_str(),
                    "priority": review.priority, "reason": review.reason, "subject_id": review.subject_id,
                    "source_id": review.provenance.source_id,
                    "source_type": review.provenance.source_type.as_str(),
                    "source_uri": review.provenance.source_uri, "source_hash": review.provenance.source_hash,
                    "source_span": review.provenance.source_span, "created_by": review.provenance.created_by,
                    "evidence_strength": review.provenance.evidence_strength,
                    "candidate_json": review.candidate_json, "candidate_version": review.candidate_version,
                    "revision": review.revision, "deferred_until": review.deferred_until,
                    "application_status": review.application_status,
                    "application_error": review.application_error, "action_key": review.action_key,
                    "decision": review.decision, "decided_by": review.decided_by,
                    "decision_note": review.decision_note, "tags": review.tags,
                    "created_at": review.created_at, "updated_at": review.updated_at,
                    "decided_at": review.decided_at,
                }));
            }
        }
        state.insert("review_items".into(), Value::Array(reviews));
        let actions = self
            .review_action_rows(scope, None, limits.audit_events)?
            .into_iter()
            .map(|action| {
                json!({
                    "id": action.id, "review_id": action.review_id, "action_key": action.action_key,
                    "action": action.action, "actor": action.actor, "note": action.note,
                    "before_status": action.before_status.as_str(), "after_status": action.after_status.as_str(),
                    "before_revision": action.before_revision, "after_revision": action.after_revision,
                    "application_status": action.application_status.as_str(),
                    "application_error": action.application_error, "result_json": action.result_json,
                    "replay_count": action.replay_count, "created_at": action.created_at,
                })
            })
            .collect();
        state.insert("review_actions".into(), Value::Array(actions));
        let view = {
            let mut statement = self.conn.prepare_cached(
                "SELECT state_json,updated_at FROM review_view_state WHERE scope_key=? LIMIT 1;",
            )?;
            let mut rows = statement.query([&scope_key])?;
            match rows.next()? {
                Some(row) => match serde_json::from_str::<Value>(&text_at(row, 0)) {
                    Ok(value @ Value::Object(_)) => {
                        json!({"state": value, "updated_at": int_at(row, 1)})
                    }
                    _ => Value::Null,
                },
                None => Value::Null,
            }
        };
        state.insert("review_view_state".into(), view);
        let audits = self
            .audit_events(scope, limits.audit_events)?
            .into_iter()
            .map(|audit| {
                json!({
                    "id": audit.id, "actor": audit.actor, "action": audit.action, "reason": audit.reason,
                    "before_id": audit.before_id, "after_id": audit.after_id,
                    "evidence_uri": audit.evidence_uri, "metadata_json": audit.metadata_json,
                    "created_at": audit.created_at,
                })
            })
            .collect();
        state.insert("audit_events".into(), Value::Array(audits));
        let events = self
            .session_events(scope, "", limits.session_events, 0)?
            .into_iter()
            .map(|event| {
                json!({
                    "id": event.id, "event_type": event.event_type, "source": event.source,
                    "payload_json": event.payload_json, "deduplication_key": event.deduplication_key,
                    "created_at": event.created_at,
                })
            })
            .collect();
        state.insert("session_events".into(), Value::Array(events));
        Ok(Value::Object(state))
    }

    fn clear_scope(&self, scope_key: &str) -> Result<()> {
        self.conn
            .execute(
                "DELETE FROM memory_assertions WHERE subject_id IN (SELECT id FROM memory_entities WHERE scope_key=?) OR object_id IN (SELECT id FROM memory_entities WHERE scope_key=?);",
                params![scope_key, scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to clear imported assertions"))?;
        self.conn
            .execute(
                "DELETE FROM memory_entity_aliases WHERE entity_id IN (SELECT id FROM memory_entities WHERE scope_key=?);",
                [scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to clear imported entity aliases"))?;
        // Revision boundaries are keyed by memory ID only; without this a
        // replace that restores preserved IDs collides with the insert trigger.
        self.conn
            .execute(
                "DELETE FROM memory_assertion_revisions WHERE memory_id IN (SELECT id FROM memories WHERE scope_key=?);",
                [scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to clear imported revision boundaries"))?;
        for table in [
            "retrieval_cross_store_links",
            "retrieval_owners",
            "memory_entities",
            "graph_edges",
            "procedure_runs",
            "memory_revisions",
            "source_events",
            "sources",
            "review_actions",
            "review_items",
            "audit_events",
            "session_events",
            "context_blocks",
            "review_view_state",
        ] {
            self.conn
                .execute(
                    &format!("DELETE FROM {table} WHERE scope_key=?;"),
                    [scope_key],
                )
                .map_err(|_| {
                    MemoryError::storage(format!("unable to clear imported memory state: {table}"))
                })?;
        }
        self.conn
            .execute("DELETE FROM memory_fts WHERE memory_id IN (SELECT id FROM memories WHERE scope_key=?);", [scope_key])
            .map_err(|_| MemoryError::storage("unable to clear memory index"))?;
        self.conn
            .execute("DELETE FROM memories WHERE scope_key=?;", [scope_key])
            .map_err(|_| MemoryError::storage("unable to clear memories"))?;
        Ok(())
    }

    fn import_memory(
        &self,
        scope: &MemoryScope,
        preserve_ids: bool,
        item: &Value,
        write: MemoryWrite,
    ) -> Result<String> {
        let original = s(item, "id", "");
        if !preserve_ids || !safe_identifier(&original) {
            return self.add(scope, write);
        }
        let exists = self
            .conn
            .prepare_cached("SELECT 1 FROM memories WHERE id=?;")?
            .exists([&original])?;
        if exists {
            return self.add(scope, write);
        }
        let now = now_ms();
        self.insert_memory_row(
            &original,
            scope,
            &write,
            (
                i(item, "success_count", 0),
                i(item, "failure_count", 0),
                &s(item, "last_outcome", "unknown"),
                i(item, "access_count", 0),
            ),
            (
                i(item, "created_at", now),
                i(item, "updated_at", now),
                i(item, "last_accessed_at", now),
            ),
        )
        .map_err(|_| MemoryError::invalid("unable to preserve imported memory identity"))?;
        for tag in &write.tags {
            self.conn
                .execute(
                    "INSERT OR IGNORE INTO memory_tags(memory_id,tag) VALUES(?,?);",
                    params![original, sanitize_text(tag, 256)],
                )
                .map_err(|_| MemoryError::storage("unable to restore imported memory tag"))?;
        }
        self.index_memory_fts(
            &original,
            &write.content,
            &write.source,
            write.memory_type,
            &write.provenance.source_uri,
        )
        .map_err(|_| MemoryError::storage("unable to index imported memory"))?;
        Ok(original)
    }

    pub(crate) fn import_state(
        &self,
        scope: &MemoryScope,
        state: &Value,
        replace_existing: bool,
    ) -> Result<()> {
        if state
            .get("schema_version")
            .and_then(Value::as_u64)
            .unwrap_or(0)
            > SCHEMA_VERSION as u64
        {
            return invalid("unsupported imported memory schema version");
        }
        validate_scope(scope)?;
        let scope_key = key(scope)?;
        let declares_scope = state.get("scope").is_some();
        let mut declared = scope.clone();
        if let Some(encoded @ Value::Object(_)) = state.get("scope") {
            declared.kind = ScopeKind::parse(&s(encoded, "kind", scope.kind.as_str()))?;
            declared.user = s(encoded, "user", "");
            declared.organization = s(encoded, "organization", "");
            declared.project = s(encoded, "project", "");
            declared.repository = s(encoded, "repository", "");
            declared.repository_display = s(encoded, "repository_display", "");
            declared.branch = s(encoded, "branch", "");
            declared.task = s(encoded, "task", "");
            declared.agent = s(encoded, "agent", "");
            declared.session = s(encoded, "session", "");
            declared.conversation = s(encoded, "conversation", "");
            validate_scope(&declared)?;
            if s(state, "scope_key", "") != key(&declared)? {
                return invalid("exported scope declaration is inconsistent");
            }
        }
        if let Some(Value::Object(truncated)) = state.get("truncated")
            && truncated
                .values()
                .any(|value| value.as_bool() == Some(true))
        {
            return invalid("cannot import a truncated canonical export");
        }
        let preserve_ids = scope_key == key(&declared)?;
        let tx = Tx::begin(self.conn, false)?;
        if replace_existing {
            self.clear_scope(&scope_key)?;
        }

        let mut memory_ids: HashMap<String, String> = HashMap::new();
        let memories = array(state, "memories");
        for item in memories {
            if declares_scope && !scope_matches(item, &declared, "scope_kind") {
                return invalid("imported memory scope does not match export declaration");
            }
            let write = MemoryWrite {
                memory_type: MemoryType::parse(&s(item, "type", "observation"))?,
                status: MemoryStatus::parse(&s(item, "status", "hypothesis"))?,
                source: sanitize_meta(&s(item, "source", "import")),
                provenance: Provenance {
                    source_id: sanitize_meta(&s(item, "source_id", "")),
                    source_type: SourceType::parse(&s(item, "source_type", "unknown")),
                    source_uri: sanitize_text(&s(item, "source_uri", ""), MAXIMUM_DURABLE_URI),
                    source_hash: sanitize_meta(&s(item, "source_hash", "")),
                    source_span: sanitize_meta(&s(item, "source_span", "")),
                    created_by: sanitize_meta(&s(item, "created_by", "import")),
                    evidence_strength: f(item, "evidence_strength", 0.0),
                },
                content: sanitize_text(&s(item, "content", ""), MAXIMUM_DURABLE_TEXT),
                tags: strings(item, "tags"),
                confidence: f(item, "confidence", 0.5),
                quality_score: f(item, "quality_score", 0.0),
            };
            let original = s(item, "id", "");
            if original.is_empty() || memory_ids.contains_key(&original) {
                return invalid("imported memory identity is empty or duplicated");
            }
            let imported = self.import_memory(scope, preserve_ids, item, write)?;
            memory_ids.insert(original, imported);
        }
        for item in memories {
            let Some(mapped) = memory_ids.get(&s(item, "id", "")) else {
                continue;
            };
            let replacement = memory_ids
                .get(&s(item, "replacement_id", ""))
                .cloned()
                .unwrap_or_default();
            let superseded = memory_ids
                .get(&s(item, "superseded_by", ""))
                .cloned()
                .unwrap_or_default();
            self.conn
                .execute(
                    "UPDATE memories SET success_count=?,failure_count=?,last_outcome=?,\
                     access_count=?,replacement_id=?,superseded_by=?,created_at=?,updated_at=?,\
                     last_accessed_at=? WHERE id=? AND scope_key=?;",
                    params![
                        i(item, "success_count", 0),
                        i(item, "failure_count", 0),
                        s(item, "last_outcome", "unknown"),
                        i(item, "access_count", 0),
                        replacement,
                        superseded,
                        i(item, "created_at", 0),
                        i(item, "updated_at", 0),
                        i(item, "last_accessed_at", 0),
                        mapped,
                        scope_key
                    ],
                )
                .map_err(|error| {
                    MemoryError::storage(format!(
                        "unable to restore imported memory metadata: {error}"
                    ))
                })?;
        }
        // Metadata restoration fires the invalidation triggers. Reconcile only
        // the newly imported owners afterwards, before restoring history.
        let has_boundaries = state.get("assertion_revision_boundaries").is_some();
        for item in memories {
            let id = &memory_ids[&s(item, "id", "")];
            let revision = i(item, "retrieval_revision", 1);
            if revision < 1 {
                return invalid("imported revision must be positive");
            }
            let owner_key = format!("memory:{id}");
            let changed = self.conn.execute(
                "UPDATE retrieval_owners SET revision=? WHERE owner_key=? AND scope_key=?;",
                params![revision, owner_key, scope_key],
            )?;
            if changed != 1 {
                return Err(MemoryError::storage(
                    "unable to restore imported retrieval revision",
                ));
            }
            self.conn.execute(
                "DELETE FROM retrieval_jobs WHERE owner_key=?;",
                [&owner_key],
            )?;
            self.conn
                .execute(
                    "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) VALUES(?,?,'chunks','text-v1');",
                    params![owner_key, revision],
                )
                .map_err(|_| MemoryError::storage("unable to schedule imported memory indexing"))?;
            self.conn.execute(
                "DELETE FROM memory_assertion_revisions WHERE memory_id=?;",
                [id],
            )?;
            if !has_boundaries {
                self.conn
                    .execute(
                        "INSERT INTO memory_assertion_revisions VALUES(?,?,?);",
                        params![id, revision, i(item, "updated_at", now_ms())],
                    )
                    .map_err(|_| {
                        MemoryError::storage("unable to initialize imported revision boundary")
                    })?;
            }
        }
        for item in array(state, "assertion_revision_boundaries") {
            let revision = i(item, "revision", 0);
            let Some(memory) = memory_ids.get(&s(item, "memory_id", "")) else {
                return invalid("imported assertion boundary is unresolved or invalid");
            };
            if revision < 1 || revision > self.retrieval_revision(scope, memory)? {
                return invalid("imported assertion boundary is unresolved or invalid");
            }
            self.conn
                .execute(
                    "INSERT INTO memory_assertion_revisions(memory_id,revision,recorded_at) VALUES(?,?,?);",
                    params![memory, revision, i(item, "recorded_at", 0)],
                )
                .map_err(|_| MemoryError::invalid("imported assertion boundary conflicts"))?;
        }
        if has_boundaries {
            for imported in memory_ids.values() {
                let current = self
                    .conn
                    .prepare_cached(
                        "SELECT 1 FROM memory_assertion_revisions b JOIN retrieval_owners o \
                         ON o.owner_id=b.memory_id AND o.kind='memory' AND o.scope_key=? \
                         WHERE b.memory_id=? AND b.revision=o.revision;",
                    )?
                    .exists(params![scope_key, imported])?;
                if !current {
                    return invalid("imported current revision boundary is missing");
                }
            }
        }

        let mut entity_ids: HashMap<String, String> = HashMap::new();
        for item in array(state, "entities") {
            let original = s(item, "id", "");
            if original.is_empty() || original.len() > 256 || entity_ids.contains_key(&original) {
                return invalid("imported entity id is invalid");
            }
            let encoded_scope = item.get("scope").cloned().unwrap_or_else(|| json!({}));
            if !encoded_scope.is_object() || !scope_matches(&encoded_scope, &declared, "kind") {
                return invalid("imported entity scope is invalid");
            }
            let kind = sanitize_text(&s(item, "kind", ""), 128);
            let name = sanitize_text(&s(item, "name", ""), 1024);
            if kind.is_empty() || name.is_empty() {
                return invalid("imported entity is incomplete");
            }
            let collision = self
                .conn
                .prepare_cached("SELECT 1 FROM memory_entities WHERE id=?;")?
                .exists([&original])?;
            let imported_id = if collision {
                make_id("entity_")
            } else {
                original.clone()
            };
            self.conn
                .execute(
                    "INSERT INTO memory_entities(id,scope_key,scope_json,kind,name) VALUES(?,?,?,?,?);",
                    params![imported_id, scope_key, dump(&scope_json(scope)), kind, name],
                )
                .map_err(|_| MemoryError::invalid("imported entity id conflicts"))?;
            entity_ids.insert(original, imported_id.clone());
            let aliases = match item.get("aliases") {
                None => Vec::new(),
                Some(Value::Array(aliases)) if aliases.len() <= 128 => aliases.clone(),
                Some(_) => return invalid("imported entity aliases are invalid"),
            };
            for alias in aliases {
                let Some(alias) = alias.as_str() else {
                    return invalid("imported entity aliases are invalid");
                };
                let value = sanitize_text(alias, 1024);
                if value.is_empty() {
                    continue;
                }
                self.conn
                    .execute(
                        "INSERT OR IGNORE INTO memory_entity_aliases VALUES(?,?);",
                        params![imported_id, value],
                    )
                    .map_err(|_| MemoryError::storage("unable to restore entity alias"))?;
            }
        }
        let source_revisions: HashMap<String, i64> = memories
            .iter()
            .map(|item| (s(item, "id", ""), i(item, "retrieval_revision", 1)))
            .collect();
        for item in array(state, "assertions") {
            let memory = memory_ids.get(&s(item, "memory_id", ""));
            let subject = entity_ids.get(&s(item, "subject_id", ""));
            let object_id = s(item, "object_id", "");
            let object = if object_id.is_empty() {
                None
            } else {
                entity_ids.get(&object_id)
            };
            let (Some(memory), Some(subject)) = (memory, subject) else {
                return invalid("imported assertion has an unresolved endpoint");
            };
            if !object_id.is_empty() && object.is_none() {
                return invalid("imported assertion has an unresolved endpoint");
            }
            let literal = serde_json::from_str::<Value>(&s(item, "literal_json", "null"));
            let qualifiers = serde_json::from_str::<Value>(&s(item, "qualifiers_json", "{}"));
            let evidence = item.get("evidence").cloned().unwrap_or_else(|| json!([]));
            let (Ok(literal), Ok(qualifiers @ Value::Object(_))) = (literal, qualifiers) else {
                return invalid("imported assertion evidence or JSON is invalid");
            };
            if !evidence.as_array().is_some_and(|items| !items.is_empty())
                || s(item, "predicate", "").is_empty()
            {
                return invalid("imported assertion evidence or JSON is invalid");
            }
            let revision = i(item, "revision", 0);
            let current = source_revisions
                .get(&s(item, "memory_id", ""))
                .copied()
                .unwrap_or(0);
            if revision < 1 || revision > current {
                return invalid("imported assertion revision is invalid");
            }
            let valid_from = item.get("valid_from").map(|_| i(item, "valid_from", 0));
            let valid_to = item.get("valid_to").map(|_| i(item, "valid_to", 0));
            self.conn
                .execute(
                    "INSERT INTO memory_assertions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);",
                    params![
                        memory,
                        revision,
                        subject,
                        sanitize_text(&s(item, "predicate", ""), 128),
                        object.cloned().unwrap_or_default(),
                        sanitize_json_default(&dump(&literal)),
                        sanitize_json_default(&dump(&qualifiers)),
                        i64::from(b(item, "positive", true)),
                        valid_from,
                        valid_to,
                        sanitize_json_default(&dump(&evidence)),
                        i(item, "recorded_at", 0),
                        sanitize_json_default(&s(item, "historical_json", "{}"))
                    ],
                )
                .map_err(|_| MemoryError::invalid("imported assertion conflicts or is invalid"))?;
        }
        for item in array(state, "cross_store_links") {
            self.import_cross_store_link(scope, &scope_key, item, &memory_ids, &entity_ids)?;
        }
        for item in array(state, "blocks") {
            self.update_block(scope, &s(item, "id", ""), &s(item, "content", ""))?;
        }
        let mut graph_ids: HashMap<String, String> = HashMap::new();
        for item in array(state, "graph_edges") {
            let (Some(from), Some(to)) = (
                memory_ids.get(&s(item, "from_id", "")),
                memory_ids.get(&s(item, "to_id", "")),
            ) else {
                continue;
            };
            let edge = GraphEdge {
                confidence: f(item, "confidence", 1.0),
                source: s(item, "source", "import"),
                ..GraphEdge::new(from.clone(), to.clone(), s(item, "type", "related"))
            };
            let edge_id = self.add_edge(scope, edge)?;
            graph_ids.insert(s(item, "id", &edge_id), edge_id);
        }
        for item in array(state, "memory_revisions") {
            let Some(memory) = memory_ids.get(&s(item, "memory_id", "")) else {
                continue;
            };
            self.conn
                .execute(
                    "INSERT INTO memory_revisions(id,scope_key,memory_id,revision_kind,entry_json,created_at) VALUES(?,?,?,?,?,?);",
                    params![
                        make_id("rev_"),
                        scope_key,
                        memory,
                        sanitize_text(&s(item, "kind", "import"), 256),
                        sanitize_json_default(&s(item, "entry_json", "{}")),
                        i(item, "created_at", 0)
                    ],
                )
                .map_err(|_| MemoryError::storage("unable to restore imported memory revision"))?;
        }
        for item in array(state, "procedure_runs") {
            let Some(procedure) = memory_ids.get(&s(item, "procedure_id", "")) else {
                continue;
            };
            self.conn
                .execute(
                    "INSERT INTO procedure_runs(id,scope_key,procedure_id,task_id,outcome,notes,evidence_uri,created_at,deduplication_key) \
                     VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT DO NOTHING;",
                    params![
                        make_id("run_"),
                        scope_key,
                        procedure,
                        sanitize_meta(&s(item, "task_id", "")),
                        sanitize_text(&s(item, "outcome", "unknown"), 256),
                        sanitize_text(&s(item, "notes", ""), MAXIMUM_DURABLE_METADATA),
                        sanitize_text(&s(item, "evidence_uri", ""), MAXIMUM_DURABLE_URI),
                        i(item, "created_at", 0),
                        sanitize_text(&s(item, "deduplication_key", ""), 256)
                    ],
                )
                .map_err(|_| MemoryError::storage("unable to restore imported procedure run"))?;
        }
        let source_events = array(state, "source_events");
        for item in array(state, "sources") {
            let source = SourceRecord {
                id: s(item, "id", ""),
                source_type: SourceType::parse(&s(item, "type", "unknown")),
                uri: s(item, "uri", ""),
                hash: s(item, "hash", ""),
                display_name: s(item, "display_name", ""),
                trust_level: s(item, "trust_level", "working"),
                hash_status: s(item, "hash_status", "unknown"),
                ..Default::default()
            };
            let mut found_event = false;
            for event in source_events
                .iter()
                .filter(|event| s(event, "source_id", "") == source.id)
            {
                self.record_event(
                    scope,
                    source.clone(),
                    SourceEvent {
                        action: s(event, "action", "import"),
                        status: s(event, "status", "ok"),
                        source_hash: s(event, "source_hash", ""),
                        note: s(event, "note", ""),
                        ..Default::default()
                    },
                )?;
                found_event = true;
            }
            if !found_event {
                let event = SourceEvent {
                    action: "import".into(),
                    status: "ok".into(),
                    source_hash: source.hash.clone(),
                    note: "source restored from native memory export".into(),
                    ..Default::default()
                };
                self.record_event(scope, source, event)?;
            }
        }
        let has_review_actions = state.get("review_actions").is_some_and(Value::is_array);
        let mut review_ids: HashMap<String, String> = HashMap::new();
        for item in array(state, "review_items") {
            let subject = s(item, "subject_id", "");
            let write = ReviewItemWrite {
                kind: s(item, "kind", "candidate_memory"),
                priority: s(item, "priority", "normal"),
                reason: s(item, "reason", "imported review"),
                subject_id: memory_ids.get(&subject).cloned().unwrap_or(subject),
                provenance: Provenance {
                    source_id: s(item, "source_id", ""),
                    source_type: SourceType::parse(&s(item, "source_type", "unknown")),
                    source_uri: s(item, "source_uri", ""),
                    source_hash: s(item, "source_hash", ""),
                    source_span: s(item, "source_span", ""),
                    created_by: s(item, "created_by", "import"),
                    evidence_strength: f(item, "evidence_strength", 0.0),
                },
                candidate_json: remapped_json(&s(item, "candidate_json", "{}"), &[&memory_ids]),
                candidate_version: u32::try_from(i(item, "candidate_version", 1)).unwrap_or(1),
                tags: strings(item, "tags"),
            };
            let review_id = self.add_review(scope, write)?;
            review_ids.insert(s(item, "id", &review_id), review_id.clone());
            let imported_status = ReviewStatus::parse(&s(item, "status", "pending"))?;
            if has_review_actions {
                let changed = self.conn.execute(
                    "UPDATE review_items SET status=?,decision=?,decided_by=?,decision_note=?,\
                     created_at=?,updated_at=?,decided_at=?,revision=?,deferred_until=?,\
                     application_status=?,application_error=?,action_key=? WHERE scope_key=? AND id=?;",
                    params![
                        imported_status.as_str(),
                        sanitize_meta(&s(item, "decision", "")),
                        sanitize_meta(&s(item, "decided_by", "")),
                        sanitize_text(&s(item, "decision_note", ""), MAXIMUM_DURABLE_METADATA),
                        i(item, "created_at", 0),
                        i(item, "updated_at", 0),
                        i(item, "decided_at", 0),
                        i(item, "revision", 0),
                        i(item, "deferred_until", 0),
                        sanitize_text(&s(item, "application_status", "not_started"), 64),
                        sanitize_text(&s(item, "application_error", ""), MAXIMUM_DURABLE_METADATA),
                        sanitize_meta(&s(item, "action_key", "")),
                        scope_key,
                        review_id
                    ],
                );
                if !matches!(changed, Ok(1)) {
                    return Err(MemoryError::storage(
                        "unable to restore imported review state",
                    ));
                }
            } else if imported_status != ReviewStatus::Pending {
                let deferred_until = i(item, "deferred_until", 0);
                let decided_by = s(item, "decided_by", "import");
                self.decide_review(
                    scope,
                    &review_id,
                    ReviewDecision {
                        status: imported_status,
                        decision: s(item, "decision", imported_status.as_str()),
                        decided_by: if decided_by.is_empty() {
                            "import".into()
                        } else {
                            decided_by
                        },
                        note: s(item, "decision_note", ""),
                        deferred_until: (deferred_until > 0).then_some(deferred_until),
                        action_key: s(item, "action_key", ""),
                        application_status: s(item, "application_status", "applied"),
                        application_error: s(item, "application_error", ""),
                        ..Default::default()
                    },
                )?;
            }
        }
        let mut action_ids: HashMap<String, String> = HashMap::new();
        for item in array(state, "review_actions") {
            let Some(review) = review_ids.get(&s(item, "review_id", "")) else {
                continue;
            };
            let action_id = make_id("review_action_");
            action_ids.insert(s(item, "id", &action_id), action_id.clone());
            self.conn
                .execute(
                    "INSERT INTO review_actions(id,scope_key,review_id,action_key,action,actor,note,\
                     before_status,after_status,before_revision,after_revision,application_status,\
                     application_error,result_json,replay_count,created_at) \
                     VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                    params![
                        action_id,
                        scope_key,
                        review,
                        sanitize_meta(&s(item, "action_key", "")),
                        sanitize_text(&s(item, "action", ""), 256),
                        sanitize_meta(&s(item, "actor", "import")),
                        sanitize_text(&s(item, "note", ""), MAXIMUM_DURABLE_METADATA),
                        sanitize_text(&s(item, "before_status", "pending"), 64),
                        sanitize_text(&s(item, "after_status", "pending"), 64),
                        i(item, "before_revision", 0),
                        i(item, "after_revision", 0),
                        sanitize_text(&s(item, "application_status", "not_started"), 64),
                        sanitize_text(&s(item, "application_error", ""), MAXIMUM_DURABLE_METADATA),
                        remapped_json(&s(item, "result_json", "{}"), &[&memory_ids, &graph_ids, &review_ids]),
                        i(item, "replay_count", 0),
                        i(item, "created_at", 0)
                    ],
                )
                .map_err(|_| MemoryError::storage("unable to restore imported review action"))?;
        }
        if let Some(view) = state
            .get("review_view_state")
            .filter(|view| view.get("state").is_some_and(Value::is_object))
        {
            let mut imported = view["state"].clone();
            remap_json_ids(&mut imported, &[&memory_ids, &graph_ids, &review_ids]);
            self.conn
                .execute(
                    "INSERT INTO review_view_state(scope_key,state_json,updated_at) VALUES(?,?,?) \
                     ON CONFLICT(scope_key) DO UPDATE SET state_json=excluded.state_json,updated_at=excluded.updated_at;",
                    params![scope_key, sanitize_json_default(&dump(&imported)), i(view, "updated_at", 0)],
                )
                .map_err(|_| MemoryError::storage("unable to restore review view state"))?;
        }
        let maps = [&memory_ids, &graph_ids, &review_ids, &action_ids];
        let remap_id = |original: String| {
            maps.iter()
                .find_map(|map| map.get(&original).cloned())
                .unwrap_or(original)
        };
        for item in array(state, "audit_events") {
            self.record_audit(
                scope,
                AuditEventWrite {
                    actor: s(item, "actor", "import"),
                    action: s(item, "action", "import"),
                    reason: s(item, "reason", "imported audit event"),
                    before_id: remap_id(s(item, "before_id", "")),
                    after_id: remap_id(s(item, "after_id", "")),
                    evidence_uri: s(item, "evidence_uri", ""),
                    metadata_json: remapped_json(&s(item, "metadata_json", "{}"), &maps),
                },
            )?;
        }
        for item in array(state, "session_events") {
            let write = SessionEventWrite {
                event_type: s(item, "event_type", "import"),
                source: s(item, "source", "import"),
                payload_json: s(item, "payload_json", "{}"),
                deduplication_key: s(item, "deduplication_key", ""),
            };
            self.record_session_event_once(scope, write)?;
        }
        tx.commit()
    }

    fn import_cross_store_link(
        &self,
        scope: &MemoryScope,
        scope_key: &str,
        item: &Value,
        memory_ids: &HashMap<String, String>,
        entity_ids: &HashMap<String, String>,
    ) -> Result<()> {
        struct Endpoint {
            kind: &'static str,
            id: String,
            revision: i64,
            repository: String,
            worktree: String,
            path: String,
            fingerprint: String,
            generation: i64,
        }
        let endpoint = |value: Option<&Value>| -> Result<Endpoint> {
            let value = value.cloned().unwrap_or_else(|| json!({}));
            let kind = match s(&value, "kind", "").as_str() {
                "memory" => "memory",
                "code" => "code",
                "entity" => "entity",
                _ => return invalid("imported link endpoint kind is invalid"),
            };
            let mut id = s(&value, "id", "");
            if kind == "memory" {
                id = memory_ids.get(&id).cloned().ok_or_else(|| {
                    MemoryError::invalid("imported link memory endpoint is unresolved")
                })?;
            } else if kind == "entity" {
                id = entity_ids.get(&id).cloned().ok_or_else(|| {
                    MemoryError::invalid("imported link entity endpoint is unresolved")
                })?;
            }
            Ok(Endpoint {
                kind,
                id,
                revision: i(&value, "revision", 0),
                repository: s(&value, "repository", ""),
                worktree: s(&value, "worktree", ""),
                path: s(&value, "path", ""),
                fingerprint: s(&value, "source_fingerprint", ""),
                generation: i(&value, "snapshot_generation", 0),
            })
        };
        let from = endpoint(item.get("from"))?;
        let to = endpoint(item.get("to"))?;
        let mut id = s(item, "id", "");
        let relation = s(item, "relation", "");
        let evidence = s(item, "evidence_id", "");
        if id.is_empty() || relation.is_empty() || !evidence.starts_with("uri:") {
            return invalid("imported cross-store link evidence is invalid");
        }
        for endpoint in [&from, &to] {
            match endpoint.kind {
                "memory" => {
                    let memory = self.get(scope, &endpoint.id)?;
                    if key(&memory.scope)? != scope_key {
                        return invalid("imported memory link endpoint is outside target scope");
                    }
                    if endpoint.revision < 0
                        || endpoint.revision > self.retrieval_revision(scope, &endpoint.id)?
                    {
                        return invalid("imported memory link revision is invalid");
                    }
                }
                "entity" => {
                    let exists = self
                        .conn
                        .prepare_cached(
                            "SELECT 1 FROM memory_entities WHERE id=? AND scope_key=?;",
                        )?
                        .exists(params![endpoint.id, scope_key])?;
                    if !exists {
                        return invalid("imported entity link endpoint is unresolved");
                    }
                }
                _ => {
                    if endpoint.repository.is_empty()
                        || endpoint.worktree.is_empty()
                        || endpoint.fingerprint.is_empty()
                        || endpoint.generation == 0
                    {
                        return invalid("imported code link endpoint is incomplete");
                    }
                }
            }
        }
        if self
            .conn
            .prepare_cached("SELECT 1 FROM retrieval_cross_store_links WHERE id=?;")?
            .exists([&id])?
        {
            id = make_id("link_");
        }
        self.conn
            .execute(
                "INSERT INTO retrieval_cross_store_links VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                params![
                    id,
                    scope_key,
                    dump(&scope_json(scope)),
                    from.kind,
                    from.id,
                    from.revision,
                    from.repository,
                    from.worktree,
                    from.path,
                    from.fingerprint,
                    from.generation,
                    to.kind,
                    to.id,
                    to.revision,
                    to.repository,
                    to.worktree,
                    to.path,
                    to.fingerprint,
                    to.generation,
                    relation,
                    evidence,
                    i(item, "created_at", 0)
                ],
            )
            .map_err(|_| MemoryError::invalid("imported cross-store link conflicts"))?;
        Ok(())
    }
}

impl MemoryStore {
    /// Bounded native export of exactly `scope` (default limits 10 000).
    pub fn export_state(&self, scope: &MemoryScope) -> Result<Value> {
        self.export_state_with_limits(scope, &ExportLimits::default())
    }

    pub fn export_state_with_limits(
        &self,
        scope: &MemoryScope,
        limits: &ExportLimits,
    ) -> Result<Value> {
        self.with(|db| db.export_state(scope, limits))
    }

    /// Imports a native or adapted state into `scope` in one transaction,
    /// optionally replacing everything stored there first. IDs are preserved
    /// when the export's declared scope is the target scope and the ID is
    /// unused; otherwise they are re-issued and references remapped.
    pub fn import_state(
        &self,
        scope: &MemoryScope,
        state: &Value,
        replace_existing: bool,
    ) -> Result<()> {
        self.with(|db| db.import_state(scope, state, replace_existing))
    }

    /// Writes a complete pre-import backup of `scope` to
    /// `<memory>/exports/pre-import-<ms>[-n].json` (owner-only) when the scope
    /// holds any state. Refuses when the scope exceeds the backup limits.
    pub fn write_backup(&self, scope: &MemoryScope) -> Result<Option<std::path::PathBuf>> {
        let backup = self.export_state_with_limits(scope, &ExportLimits::backup())?;
        let has_state = [
            "memories",
            "graph_edges",
            "procedure_runs",
            "memory_revisions",
            "sources",
            "source_events",
            "review_items",
            "audit_events",
            "session_events",
            "blocks",
        ]
        .iter()
        .any(|collection| {
            backup[*collection]
                .as_array()
                .is_some_and(|items| !items.is_empty())
        });
        if !has_state {
            return Ok(None);
        }
        if let Some(Value::Object(truncated)) = backup.get("truncated") {
            for (name, value) in truncated {
                if value.as_bool() == Some(true) {
                    return Err(MemoryError::storage(format!(
                        "scope is too large for a complete reversible import backup: {name}"
                    )));
                }
            }
        }
        let stamp = now_ms();
        let exports = self.root().join("exports");
        let mut path = exports.join(format!("pre-import-{stamp}.json"));
        let mut collision = 0;
        while path.exists() {
            collision += 1;
            path = exports.join(format!("pre-import-{stamp}-{collision}.json"));
        }
        write_private(&path, dump(&backup).as_bytes())?;
        Ok(Some(path))
    }
}

fn write_private(path: &std::path::Path, bytes: &[u8]) -> Result<()> {
    use std::io::Write as _;
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt as _;
        options.mode(0o600);
    }
    let mut file = options.open(path)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    Ok(())
}
