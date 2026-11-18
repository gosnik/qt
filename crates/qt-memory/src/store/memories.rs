//! Memory CRUD, graph edges, procedure feedback, sources, audit, session
//! events, context blocks and statistics.

use rusqlite::params;
use rusqlite::types::Value as Sql;
use serde_json::{Value, json};

use super::{
    Db, MemoryStore, Tx, int_at, key, make_id, memory_columns, placeholders, real_at, text_at,
};
use crate::error::{MemoryError, Result, invalid, storage};
use crate::model::*;
use crate::scope::{allowed_keys, validate_scope};
use crate::text::{
    MAXIMUM_DURABLE_METADATA, MAXIMUM_DURABLE_TEXT, MAXIMUM_DURABLE_URI, ascii_lower, dump,
    normalize_content, sanitize_json_default, sanitize_meta, sanitize_text,
};

/// Archived `memory_json`: the revision snapshot and audit representation.
pub(crate) fn memory_json(entry: &MemoryEntry) -> Value {
    json!({
        "id": entry.id, "scope_kind": entry.scope.kind.as_str(),
        "user": entry.scope.user, "organization": entry.scope.organization,
        "project": entry.scope.project, "repository": entry.scope.repository,
        "repository_display": entry.scope.repository_display,
        "branch": entry.scope.branch, "task": entry.scope.task,
        "agent": entry.scope.agent, "session": entry.scope.session,
        "conversation": entry.scope.conversation, "type": entry.memory_type.as_str(),
        "status": entry.status.as_str(), "source": entry.source,
        "source_id": entry.provenance.source_id,
        "source_type": entry.provenance.source_type.as_str(),
        "source_uri": entry.provenance.source_uri,
        "source_hash": entry.provenance.source_hash,
        "source_span": entry.provenance.source_span,
        "created_by": entry.provenance.created_by,
        "evidence_strength": entry.provenance.evidence_strength,
        "content": entry.content, "tags": entry.tags,
        "confidence": entry.confidence, "quality_score": entry.quality_score,
        "success_count": entry.success_count, "failure_count": entry.failure_count,
        "last_outcome": entry.last_outcome.as_str(),
        "access_count": entry.access_count, "superseded_by": entry.superseded_by,
        "replacement_id": entry.replacement_id,
        "created_at": entry.created_at,
        "updated_at": entry.updated_at,
        "last_accessed_at": entry.last_accessed_at,
    })
}

/// FTS5 query: alphanumeric/`_`/`-` words, each quoted, joined with `OR`.
pub(crate) fn fts_query(value: &str) -> String {
    value
        .split(|ch: char| !(ch.is_ascii_alphanumeric() || ch == '_' || ch == '-'))
        .filter(|word| !word.is_empty())
        .map(|word| format!("\"{word}\""))
        .collect::<Vec<_>>()
        .join(" OR ")
}

fn sql_text(value: impl Into<String>) -> Sql {
    Sql::Text(value.into())
}

fn sql_keys(keys: &[String]) -> impl Iterator<Item = Sql> + '_ {
    keys.iter().map(|key| Sql::Text(key.clone()))
}

fn sanitize_provenance(provenance: &mut Provenance) {
    provenance.source_id = sanitize_meta(&provenance.source_id);
    provenance.source_uri = sanitize_text(&provenance.source_uri, MAXIMUM_DURABLE_URI);
    provenance.source_hash = sanitize_meta(&provenance.source_hash);
    provenance.source_span = sanitize_meta(&provenance.source_span);
    provenance.created_by = sanitize_meta(&provenance.created_by);
}

/// Statement filters on scope keys share this bound-parameter shape.
pub(crate) fn scope_filter(keys: &[String]) -> String {
    format!("scope_key IN ({})", placeholders(keys.len()))
}

impl Db<'_> {
    pub(crate) fn insert_memory_row(
        &self,
        id: &str,
        scope: &MemoryScope,
        write: &MemoryWrite,
        counters: (i64, i64, &str, i64),
        times: (Millis, Millis, Millis),
    ) -> Result<()> {
        let sql = format!(
            "INSERT INTO memories({}) VALUES({});",
            memory_columns(""),
            placeholders(35)
        );
        let (success, failure, outcome, access) = counters;
        let values: Vec<Sql> = vec![
            sql_text(id),
            sql_text(key(scope)?),
            sql_text(scope.kind.as_str()),
            sql_text(scope.user.clone()),
            sql_text(scope.organization.clone()),
            sql_text(scope.project.clone()),
            sql_text(scope.repository.clone()),
            sql_text(sanitize_text(
                &scope.repository_display,
                MAXIMUM_DURABLE_URI,
            )),
            sql_text(scope.branch.clone()),
            sql_text(scope.task.clone()),
            sql_text(scope.agent.clone()),
            sql_text(scope.session.clone()),
            sql_text(scope.conversation.clone()),
            sql_text(write.memory_type.as_str()),
            sql_text(write.status.as_str()),
            sql_text(write.source.clone()),
            sql_text(write.provenance.source_id.clone()),
            sql_text(write.provenance.source_type.as_str()),
            sql_text(write.provenance.source_uri.clone()),
            sql_text(write.provenance.source_hash.clone()),
            sql_text(write.provenance.source_span.clone()),
            sql_text(write.provenance.created_by.clone()),
            Sql::Real(write.provenance.evidence_strength),
            sql_text(write.content.clone()),
            Sql::Real(write.confidence),
            Sql::Real(write.quality_score),
            Sql::Integer(success),
            Sql::Integer(failure),
            sql_text(outcome),
            Sql::Integer(access),
            sql_text(""),
            sql_text(""),
            Sql::Integer(times.0),
            Sql::Integer(times.1),
            Sql::Integer(times.2),
        ];
        self.conn
            .execute(&sql, rusqlite::params_from_iter(values.iter()))?;
        Ok(())
    }

    pub(crate) fn index_memory_fts(
        &self,
        id: &str,
        content: &str,
        source: &str,
        memory_type: MemoryType,
        source_uri: &str,
    ) -> Result<()> {
        self.conn
            .prepare_cached("INSERT INTO memory_fts(memory_id,content,source,memory_type,source_uri) VALUES(?,?,?,?,?);")?
            .execute(params![id, content, source, memory_type.as_str(), source_uri])
            .map_err(|_| MemoryError::storage("unable to index memory"))?;
        Ok(())
    }

    pub(crate) fn add(&self, scope: &MemoryScope, mut write: MemoryWrite) -> Result<String> {
        validate_scope(scope)?;
        write.source = sanitize_meta(&write.source);
        sanitize_provenance(&mut write.provenance);
        write.content = sanitize_text(&write.content, MAXIMUM_DURABLE_TEXT);
        for tag in &mut write.tags {
            *tag = sanitize_text(tag, 256);
        }
        let id = make_id("mem_");
        let now = now_ms();
        let tx = Tx::begin(self.conn, false)?;
        self.insert_memory_row(&id, scope, &write, (0, 0, "unknown", 0), (now, now, 0))
            .map_err(|error| MemoryError::storage(format!("unable to insert memory: {error}")))?;
        for tag in &write.tags {
            self.conn
                .prepare_cached("INSERT OR IGNORE INTO memory_tags(memory_id,tag) VALUES(?,?);")?
                .execute(params![id, tag])
                .map_err(|_| MemoryError::storage("unable to insert memory tag"))?;
        }
        self.index_memory_fts(
            &id,
            &write.content,
            &write.source,
            write.memory_type,
            &write.provenance.source_uri,
        )?;
        tx.commit()?;
        Ok(id)
    }

    pub(crate) fn query(
        &self,
        scope: &MemoryScope,
        query: &MemoryQuery,
    ) -> Result<Vec<MemoryEntry>> {
        let keys = allowed_keys(scope, query.policy, query.include_global_user)?;
        let search = fts_query(&query.text);
        let has_search = !search.is_empty();
        let mut sql = format!("SELECT {} FROM memories m ", memory_columns("m."));
        if has_search {
            sql += "JOIN memory_fts f ON f.memory_id=m.id ";
        }
        sql += &format!("WHERE m.scope_key IN ({}) ", placeholders(keys.len()));
        if has_search {
            sql += "AND memory_fts MATCH ? ";
        }
        if query.memory_type.is_some() {
            sql += "AND m.memory_type=? ";
        }
        if !query.statuses.is_empty() {
            sql += &format!("AND m.status IN ({}) ", placeholders(query.statuses.len()));
        } else if !query.include_superseded {
            sql += "AND m.status NOT IN ('rejected','expired','superseded','deprecated','dream_artifact') ";
        }
        for _ in &query.tags {
            sql += "AND EXISTS(SELECT 1 FROM memory_tags mt WHERE mt.memory_id=m.id AND mt.tag=?) ";
        }
        sql += "ORDER BY CASE m.scope_key ";
        for rank in 0..keys.len() {
            sql += &format!("WHEN ? THEN {rank} ");
        }
        sql += &format!("ELSE {} END,", keys.len());
        sql += if has_search {
            "bm25(memory_fts),m.updated_at DESC,m.id "
        } else {
            "m.updated_at DESC,m.id "
        };
        if query.limit != 0 {
            sql += "LIMIT ? OFFSET ?";
        }
        let mut binds: Vec<Sql> = sql_keys(&keys).collect();
        if has_search {
            binds.push(sql_text(search));
        }
        if let Some(memory_type) = query.memory_type {
            binds.push(sql_text(memory_type.as_str()));
        }
        binds.extend(
            query
                .statuses
                .iter()
                .map(|status| sql_text(status.as_str())),
        );
        binds.extend(query.tags.iter().map(|tag| sql_text(tag.clone())));
        binds.extend(sql_keys(&keys));
        if query.limit != 0 {
            binds.push(Sql::Integer(query.limit as i64));
            binds.push(Sql::Integer(query.offset as i64));
        }
        self.select_memories(&sql, &binds)
    }

    fn get_with_keys(&self, id: &str, keys: &[String]) -> Result<Option<MemoryEntry>> {
        let sql = format!(
            "SELECT {} FROM memories WHERE id=? AND scope_key IN ({}) LIMIT 1;",
            memory_columns(""),
            placeholders(keys.len())
        );
        let mut binds = vec![sql_text(id)];
        binds.extend(sql_keys(keys));
        Ok(self.select_memories(&sql, &binds)?.into_iter().next())
    }

    pub(crate) fn get(&self, scope: &MemoryScope, id: &str) -> Result<MemoryEntry> {
        let keys = allowed_keys(scope, RecallPolicy::Broad, false)?;
        self.get_with_keys(id, &keys)?
            .ok_or_else(|| MemoryError::invalid("memory not found in scope"))
    }

    pub(crate) fn find_for_recall(
        &self,
        scope: &MemoryScope,
        id: &str,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<Option<MemoryEntry>> {
        let keys = allowed_keys(scope, policy, include_global_user)?;
        self.get_with_keys(id, &keys)
    }

    fn insert_revision(
        &self,
        scope_key: &str,
        memory_id: &str,
        kind: &str,
        entry: &MemoryEntry,
        at: Millis,
    ) -> Result<()> {
        self.conn
            .prepare_cached("INSERT INTO memory_revisions(id,scope_key,memory_id,revision_kind,entry_json,created_at) VALUES(?,?,?,?,?,?);")?
            .execute(params![make_id("rev_"), scope_key, memory_id, kind, dump(&memory_json(entry)), at])
            .map_err(|_| MemoryError::storage("unable to record memory revision"))?;
        Ok(())
    }

    pub(crate) fn update(
        &self,
        scope: &MemoryScope,
        id: &str,
        patch: &MemoryPatch,
    ) -> Result<MemoryEntry> {
        let before = self.get(scope, id)?;
        let mut after = before.clone();
        if let Some(content) = &patch.content {
            after.content = sanitize_text(content, MAXIMUM_DURABLE_TEXT);
        }
        if let Some(memory_type) = patch.memory_type {
            after.memory_type = memory_type;
        }
        if let Some(status) = patch.status {
            after.status = status;
        }
        if let Some(replacement) = &patch.replacement_id {
            after.replacement_id = replacement.clone();
            after.superseded_by = replacement.clone();
        }
        if let Some(confidence) = patch.confidence {
            after.confidence = confidence;
        }
        if let Some(quality) = patch.quality_score {
            after.quality_score = quality;
        }
        if let Some(provenance) = &patch.provenance {
            after.provenance = provenance.clone();
            sanitize_provenance(&mut after.provenance);
            after.provenance.evidence_strength = after.provenance.evidence_strength.clamp(0.0, 1.0);
        }
        if let Some(tags) = &patch.tags {
            let mut tags: Vec<String> = tags
                .iter()
                .map(|tag| sanitize_text(tag, 256))
                .filter(|tag| !tag.is_empty())
                .collect();
            tags.sort();
            tags.dedup();
            after.tags = tags;
        }
        after.updated_at = now_ms();
        let before_key = key(&before.scope)?;
        let tx = Tx::begin(self.conn, false)?;
        self.insert_revision(&before_key, id, "update", &before, after.updated_at)?;
        let changed = self.conn.execute(
            "UPDATE memories SET content=?,memory_type=?,status=?,replacement_id=?,superseded_by=?,\
             confidence=?,quality_score=?,source_id=?,source_type=?,source_uri=?,source_hash=?,\
             source_span=?,created_by=?,evidence_strength=?,updated_at=? WHERE id=? AND scope_key=?;",
            params![
                after.content,
                after.memory_type.as_str(),
                after.status.as_str(),
                after.replacement_id,
                after.superseded_by,
                after.confidence,
                after.quality_score,
                after.provenance.source_id,
                after.provenance.source_type.as_str(),
                after.provenance.source_uri,
                after.provenance.source_hash,
                after.provenance.source_span,
                after.provenance.created_by,
                after.provenance.evidence_strength,
                after.updated_at,
                id,
                before_key
            ],
        )?;
        if changed != 1 {
            return storage("unable to update memory");
        }
        if patch.tags.is_some() {
            self.conn
                .execute("DELETE FROM memory_tags WHERE memory_id=?;", [id])?;
            for tag in &after.tags {
                self.conn
                    .execute(
                        "INSERT INTO memory_tags(memory_id,tag) VALUES(?,?);",
                        params![id, tag],
                    )
                    .map_err(|_| MemoryError::storage("unable to refresh memory tag"))?;
            }
        }
        self.conn
            .execute("DELETE FROM memory_fts WHERE memory_id=?;", [id])?;
        self.index_memory_fts(
            id,
            &after.content,
            &after.source,
            after.memory_type,
            &after.provenance.source_uri,
        )
        .map_err(|_| MemoryError::storage("unable to refresh memory index"))?;
        tx.commit()?;
        Ok(after)
    }

    pub(crate) fn forget(&self, scope: &MemoryScope, id: &str) -> Result<bool> {
        let before = match self.get(scope, id) {
            Ok(before) => before,
            Err(error) if error.is_invalid_argument() => return Ok(false),
            Err(error) => return Err(error),
        };
        let before_key = key(&before.scope)?;
        let tx = Tx::begin(self.conn, false)?;
        self.insert_revision(&before_key, id, "delete", &before, now_ms())
            .map_err(|_| MemoryError::storage("unable to record deletion revision"))?;
        self.conn.execute(
            "DELETE FROM graph_edges WHERE scope_key=? AND (from_id=? OR to_id=?);",
            params![before_key, id, id],
        )?;
        self.conn
            .execute("DELETE FROM memory_fts WHERE memory_id=?;", [id])?;
        let removed = self
            .conn
            .execute(
                "DELETE FROM memories WHERE id=? AND scope_key=?;",
                params![id, before_key],
            )
            .map_err(|_| MemoryError::storage("unable to delete memory"))?;
        tx.commit()?;
        Ok(removed == 1)
    }

    pub(crate) fn apply_governed_update(
        &self,
        scope: &MemoryScope,
        id: &str,
        expected_status: MemoryStatus,
        patch: &MemoryPatch,
        edge: Option<GraphEdge>,
        audit: AuditEventWrite,
    ) -> Result<MemoryEntry> {
        validate_scope(scope)?;
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let before = self.get(scope, id)?;
        if key(&before.scope)? != scope_key || before.status != expected_status {
            return invalid("governance subject changed or is outside the exact scope");
        }
        if audit.actor.is_empty() || audit.reason.is_empty() {
            return invalid("governance requires an actor and reason");
        }
        let require_exact = |target: &str| -> Result<()> {
            if key(&self.get(scope, target)?.scope)? != scope_key {
                return invalid("governance endpoint is outside the exact scope");
            }
            Ok(())
        };
        if let Some(replacement) = patch
            .replacement_id
            .as_deref()
            .filter(|value| !value.is_empty())
        {
            if replacement == id {
                return invalid("memory cannot replace itself");
            }
            require_exact(replacement)?;
        }
        if let Some(edge) = &edge {
            require_exact(&edge.from_id)?;
            require_exact(&edge.to_id)?;
        }
        let after = self.update(scope, id, patch)?;
        if let Some(edge) = edge {
            self.add_edge(scope, edge)?;
        }
        self.record_audit(scope, audit)?;
        tx.commit()?;
        Ok(after)
    }

    pub(crate) fn revisions(
        &self,
        scope: &MemoryScope,
        memory_id: &str,
    ) -> Result<Vec<MemoryRevision>> {
        let keys = allowed_keys(scope, RecallPolicy::Broad, false)?;
        let sql = format!(
            "SELECT id,memory_id,revision_kind,entry_json,created_at FROM memory_revisions \
             WHERE memory_id=? AND scope_key IN ({}) ORDER BY created_at DESC,id;",
            placeholders(keys.len())
        );
        let mut binds = vec![sql_text(memory_id)];
        binds.extend(sql_keys(&keys));
        let mut statement = self.conn.prepare(&sql)?;
        let rows = statement
            .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                Ok(MemoryRevision {
                    id: text_at(row, 0),
                    memory_id: text_at(row, 1),
                    kind: text_at(row, 2),
                    entry_json: text_at(row, 3),
                    created_at: int_at(row, 4),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub(crate) fn record_access(&self, scope: &MemoryScope, memory_ids: &[String]) -> Result<()> {
        if memory_ids.is_empty() {
            return Ok(());
        }
        let keys = allowed_keys(scope, RecallPolicy::Broad, true)?;
        let sql = format!(
            "UPDATE memories SET access_count=access_count+1,last_accessed_at=? WHERE id=? AND scope_key IN ({});",
            placeholders(keys.len())
        );
        let tx = Tx::begin(self.conn, false)?;
        for id in memory_ids {
            let mut binds = vec![Sql::Integer(now_ms()), sql_text(id.clone())];
            binds.extend(sql_keys(&keys));
            self.conn
                .execute(&sql, rusqlite::params_from_iter(binds.iter()))
                .map_err(|_| MemoryError::storage("unable to record memory access"))?;
        }
        tx.commit()
    }

    pub(crate) fn size(&self, scope: &MemoryScope) -> Result<usize> {
        let count: i64 = self.conn.query_row(
            "SELECT COUNT(*) FROM memories WHERE scope_key=?;",
            [key(scope)?],
            |row| row.get(0),
        )?;
        Ok(count as usize)
    }

    pub(crate) fn retrieval_revision(&self, scope: &MemoryScope, memory_id: &str) -> Result<i64> {
        let memory = self.get(scope, memory_id)?;
        let mut statement = self.conn.prepare_cached(
            "SELECT revision FROM retrieval_owners WHERE owner_key=? AND scope_key=? AND deleted=0;",
        )?;
        let mut rows =
            statement.query(params![format!("memory:{memory_id}"), key(&memory.scope)?])?;
        match rows.next()? {
            Some(row) => Ok(int_at(row, 0)),
            None => invalid("missing retrieval owner"),
        }
    }

    pub(crate) fn statistics(
        &self,
        scope: &MemoryScope,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<MemoryStatistics> {
        let keys = allowed_keys(scope, policy, include_global_user)?;
        let binds: Vec<Sql> = sql_keys(&keys).collect();
        let params = || rusqlite::params_from_iter(binds.iter());
        let filter = scope_filter(&keys);
        let mut stats = MemoryStatistics::default();
        let totals_sql = format!(
            "SELECT COUNT(*),\
             COALESCE(SUM(CASE WHEN status='hypothesis' THEN 1 ELSE 0 END),0),\
             COALESCE(SUM(CASE WHEN confidence<0.35 THEN 1 ELSE 0 END),0),\
             COALESCE(SUM(CASE WHEN source='' AND source_type='unknown' AND \
             source_uri='' AND source_hash='' THEN 1 ELSE 0 END),0),\
             COALESCE(SUM(CASE WHEN status='deprecated' THEN 1 ELSE 0 END),0),\
             COALESCE(SUM(CASE WHEN memory_type='procedural' AND failure_count>\
             success_count THEN 1 ELSE 0 END),0),\
             COALESCE(SUM(CASE WHEN status='hypothesis' AND access_count>=3 THEN 1 \
             ELSE 0 END),0),\
             COALESCE(SUM(CASE WHEN access_count>0 THEN 1 ELSE 0 END),0),\
             COALESCE(SUM(access_count),0),COALESCE(SUM(success_count),0),\
             COALESCE(SUM(failure_count),0) FROM memories WHERE {filter};"
        );
        let totals: Vec<u64> = self
            .conn
            .query_row(&totals_sql, params(), |row| {
                Ok((0..11)
                    .map(|index| int_at(row, index).max(0) as u64)
                    .collect())
            })
            .map_err(|_| MemoryError::storage("unable to aggregate memory statistics"))?;
        stats.memory_entries = totals[0];
        stats.hypotheses = totals[1];
        stats.low_confidence = totals[2];
        stats.unprovenanced = totals[3];
        stats.deprecated = totals[4];
        stats.failed_procedures = totals[5];
        stats.high_use_hypotheses = totals[6];
        stats.accessed_memories = totals[7];
        stats.total_accesses = totals[8];
        stats.successful_outcomes = totals[9];
        stats.failed_outcomes = totals[10];

        let dream_sql = format!(
            "SELECT event_type,payload_json FROM session_events WHERE {filter} \
             AND event_type IN ('memory_dream_claim','memory_dream_receipt');"
        );
        let events: Vec<(String, String)> = self
            .conn
            .prepare(&dream_sql)?
            .query_map(params(), |row| Ok((text_at(row, 0), text_at(row, 1))))?
            .collect::<rusqlite::Result<_>>()?;
        let dreaming = &mut stats.dreaming;
        for (event_type, payload) in events {
            let Ok(Value::Object(payload)) = serde_json::from_str::<Value>(&payload) else {
                continue;
            };
            let string = |field: &str| {
                payload
                    .get(field)
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string()
            };
            if event_type == "memory_dream_claim" {
                dreaming.attempted += 1;
                let trigger = string("trigger");
                let model = string("model");
                if !trigger.is_empty() {
                    *dreaming.by_trigger.entry(trigger).or_default() += 1;
                }
                if !model.is_empty() {
                    *dreaming.by_model.entry(model).or_default() += 1;
                }
                continue;
            }
            let error = string("error");
            if error.is_empty() {
                dreaming.completed += 1;
            } else {
                dreaming.failed += 1;
                let mut reason = string("failure_reason");
                if reason.is_empty() {
                    reason = if error.contains("rejected by the Dreaming protocol") {
                        "protocol_rejection"
                    } else if error.contains("timed out") {
                        "timeout"
                    } else if error.contains("cancelled") {
                        "cancelled"
                    } else if error.contains("failed to start") {
                        "start_failure"
                    } else {
                        "unspecified"
                    }
                    .into();
                }
                match reason.as_str() {
                    "protocol_rejection" => dreaming.protocol_rejections += 1,
                    "timeout" => dreaming.timed_out += 1,
                    "cancelled" => dreaming.cancelled += 1,
                    "start_failure" => dreaming.start_failures += 1,
                    _ => {}
                }
                *dreaming.failures_by_reason.entry(reason).or_default() += 1;
            }
            if string("backend_fallback") == "cpu" {
                dreaming.cpu_fallbacks += 1;
            }
            if payload
                .get("review_created")
                .and_then(Value::as_bool)
                .unwrap_or(false)
            {
                dreaming.reviews_created += 1;
            }
        }
        dreaming.pending = dreaming
            .attempted
            .saturating_sub(dreaming.completed + dreaming.failed);

        for (column, target) in [
            ("memory_type", &mut stats.memory_counts),
            ("status", &mut stats.trust_states),
        ] {
            let sql = format!(
                "SELECT {column},COUNT(*) FROM memories WHERE {filter} GROUP BY {column} ORDER BY {column};"
            );
            let mut statement = self.conn.prepare(&sql)?;
            let rows = statement.query_map(params(), |row| {
                Ok((text_at(row, 0), int_at(row, 1).max(0) as u64))
            })?;
            for row in rows {
                let (name, count) = row?;
                target.insert(name, count);
            }
        }

        let conflicts_sql = format!(
            "SELECT COUNT(*),COALESCE(SUM(CASE WHEN lhs.status='superseded' OR \
             rhs.status='superseded' OR lhs.replacement_id<>'' OR \
             rhs.replacement_id<>'' THEN 0 ELSE 1 END),0) FROM graph_edges edge \
             JOIN memories lhs ON lhs.id=edge.from_id JOIN memories rhs ON \
             rhs.id=edge.to_id WHERE edge.scope_key IN ({}) AND edge.type='conflicts_with';",
            placeholders(keys.len())
        );
        let (conflicts, unresolved) = self
            .conn
            .query_row(&conflicts_sql, params(), |row| {
                Ok((int_at(row, 0), int_at(row, 1)))
            })
            .map_err(|_| MemoryError::storage("unable to aggregate memory conflict statistics"))?;
        stats.conflict_count = conflicts.max(0) as u64;
        stats.unresolved_conflicts = unresolved.max(0) as u64;
        let reviews_sql =
            format!("SELECT COUNT(*) FROM review_items WHERE {filter} AND status='pending';");
        stats.pending_reviews = self
            .conn
            .query_row(&reviews_sql, params(), |row| {
                Ok(int_at(row, 0).max(0) as u64)
            })
            .map_err(|_| MemoryError::storage("unable to aggregate memory review statistics"))?;
        Ok(stats)
    }

    pub(crate) fn add_edge(&self, scope: &MemoryScope, mut edge: GraphEdge) -> Result<String> {
        if edge.from_id.is_empty() || edge.to_id.is_empty() || edge.edge_type.is_empty() {
            return invalid("graph edge requires endpoints and type");
        }
        self.get(scope, &edge.from_id)?;
        self.get(scope, &edge.to_id)?;
        if edge.id.is_empty() {
            edge.id = make_id("edge_");
        }
        edge.edge_type = sanitize_text(&edge.edge_type, 256);
        edge.source = sanitize_meta(&edge.source);
        edge.created_at = now_ms();
        self.conn
            .prepare_cached(
                "INSERT INTO graph_edges(id,scope_key,from_id,to_id,type,confidence,source,created_at) \
                 VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(scope_key,from_id,to_id,type) DO UPDATE SET \
                 confidence=excluded.confidence,source=excluded.source;",
            )?
            .execute(params![
                edge.id,
                key(scope)?,
                edge.from_id,
                edge.to_id,
                edge.edge_type,
                edge.confidence,
                edge.source,
                edge.created_at
            ])
            .map_err(|_| MemoryError::storage("unable to add graph edge"))?;
        // An existing (scope, from, to, type) edge is updated in place and
        // keeps its ID; report the stored ID rather than the unused new one.
        let stored = self.conn.query_row(
            "SELECT id FROM graph_edges WHERE scope_key=? AND from_id=? AND to_id=? AND type=?;",
            params![key(scope)?, edge.from_id, edge.to_id, edge.edge_type],
            |row| Ok(text_at(row, 0)),
        )?;
        Ok(stored)
    }

    pub(crate) fn query_edges(
        &self,
        scope: &MemoryScope,
        query: &GraphQuery,
    ) -> Result<Vec<GraphEdge>> {
        let keys = allowed_keys(scope, RecallPolicy::Broad, true)?;
        let mut sql = format!(
            "SELECT id,from_id,to_id,type,confidence,source,created_at FROM graph_edges WHERE {} ",
            scope_filter(&keys)
        );
        let mut binds: Vec<Sql> = sql_keys(&keys).collect();
        if !query.memory_id.is_empty() {
            sql += "AND (from_id=? OR to_id=?) ";
            binds.push(sql_text(query.memory_id.clone()));
            binds.push(sql_text(query.memory_id.clone()));
        }
        for (value, clause) in [
            (&query.from_id, "AND from_id=? "),
            (&query.to_id, "AND to_id=? "),
            (&query.edge_type, "AND type=? "),
        ] {
            if !value.is_empty() {
                sql += clause;
                binds.push(sql_text(value.clone()));
            }
        }
        sql += "ORDER BY created_at DESC,id LIMIT ?;";
        binds.push(Sql::Integer(query.limit as i64));
        let mut statement = self.conn.prepare(&sql)?;
        let rows = statement
            .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                Ok(GraphEdge {
                    id: text_at(row, 0),
                    from_id: text_at(row, 1),
                    to_id: text_at(row, 2),
                    edge_type: text_at(row, 3),
                    confidence: real_at(row, 4),
                    source: text_at(row, 5),
                    created_at: int_at(row, 6),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub(crate) fn add_feedback_once(
        &self,
        scope: &MemoryScope,
        feedback: ProcedureFeedback,
    ) -> Result<ProcedureFeedbackResult> {
        let mut memory = self.get(scope, &feedback.procedure_id)?;
        if memory.memory_type != MemoryType::Procedural {
            return invalid("procedure feedback target is not procedural memory");
        }
        let mut run = ProcedureRun {
            id: make_id("run_"),
            procedure_id: feedback.procedure_id.clone(),
            task_id: sanitize_meta(&feedback.task_id),
            outcome: feedback.outcome,
            notes: sanitize_text(&feedback.notes, MAXIMUM_DURABLE_METADATA),
            evidence_uri: sanitize_text(&feedback.evidence_uri, MAXIMUM_DURABLE_URI),
            created_at: now_ms(),
            deduplication_key: sanitize_text(&feedback.deduplication_key, 256),
        };
        let memory_key = key(&memory.scope)?;
        let tx = Tx::begin(self.conn, false)?;
        let inserted = self
            .conn
            .execute(
                "INSERT INTO procedure_runs(id,scope_key,procedure_id,task_id,outcome,notes,evidence_uri,created_at,deduplication_key) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT DO NOTHING;",
                params![
                    run.id,
                    memory_key,
                    run.procedure_id,
                    run.task_id,
                    run.outcome.as_str(),
                    run.notes,
                    run.evidence_uri,
                    run.created_at,
                    run.deduplication_key
                ],
            )
            .map_err(|_| MemoryError::storage("unable to add procedure feedback"))?;
        if inserted == 0 {
            if run.deduplication_key.is_empty() {
                return storage("unable to add procedure feedback");
            }
            let existing = self
                .conn
                .query_row(
                    "SELECT id,task_id,outcome,notes,evidence_uri,created_at FROM procedure_runs \
                     WHERE scope_key=? AND procedure_id=? AND deduplication_key=? LIMIT 1;",
                    params![memory_key, run.procedure_id, run.deduplication_key],
                    |row| {
                        Ok((
                            text_at(row, 0),
                            text_at(row, 1),
                            text_at(row, 2),
                            text_at(row, 3),
                            text_at(row, 4),
                            int_at(row, 5),
                        ))
                    },
                )
                .map_err(|_| {
                    MemoryError::storage("unable to resolve deduplicated procedure feedback")
                })?;
            run.id = existing.0;
            run.task_id = existing.1;
            run.outcome = ProcedureOutcome::parse(&existing.2);
            run.notes = existing.3;
            run.evidence_uri = existing.4;
            run.created_at = existing.5;
            tx.commit()?;
            return Ok(ProcedureFeedbackResult {
                run,
                memory,
                inserted: false,
            });
        }
        match run.outcome {
            ProcedureOutcome::Success => memory.success_count += 1,
            ProcedureOutcome::Failure => memory.failure_count += 1,
            _ => {}
        }
        memory.last_outcome = run.outcome;
        let attempts = memory.success_count + memory.failure_count;
        if attempts != 0 {
            let observed = memory.success_count as f64 / attempts as f64;
            memory.confidence = (0.7 * memory.confidence + 0.3 * observed).clamp(0.0, 1.0);
        }
        memory.updated_at = now_ms();
        self.conn
            .execute(
                "UPDATE memories SET success_count=?,failure_count=?,last_outcome=?,confidence=?,updated_at=? WHERE id=? AND scope_key=?;",
                params![
                    memory.success_count as i64,
                    memory.failure_count as i64,
                    memory.last_outcome.as_str(),
                    memory.confidence,
                    memory.updated_at,
                    memory.id,
                    memory_key
                ],
            )
            .map_err(|_| MemoryError::storage("unable to update procedure outcome"))?;
        tx.commit()?;
        Ok(ProcedureFeedbackResult {
            run,
            memory,
            inserted: true,
        })
    }

    pub(crate) fn runs(
        &self,
        scope: &MemoryScope,
        procedure_id: &str,
    ) -> Result<Vec<ProcedureRun>> {
        let keys = allowed_keys(scope, RecallPolicy::Broad, false)?;
        let sql = format!(
            "SELECT id,procedure_id,task_id,outcome,notes,evidence_uri,created_at,deduplication_key FROM procedure_runs \
             WHERE procedure_id=? AND {} ORDER BY created_at DESC,id;",
            scope_filter(&keys)
        );
        let mut binds = vec![sql_text(procedure_id)];
        binds.extend(sql_keys(&keys));
        let mut statement = self.conn.prepare(&sql)?;
        let rows = statement
            .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                Ok(ProcedureRun {
                    id: text_at(row, 0),
                    procedure_id: text_at(row, 1),
                    task_id: text_at(row, 2),
                    outcome: ProcedureOutcome::parse(&text_at(row, 3)),
                    notes: text_at(row, 4),
                    evidence_uri: text_at(row, 5),
                    created_at: int_at(row, 6),
                    deduplication_key: text_at(row, 7),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub(crate) fn record_event(
        &self,
        scope: &MemoryScope,
        mut source: SourceRecord,
        mut event: SourceEvent,
    ) -> Result<()> {
        validate_scope(scope)?;
        if source.id.is_empty() {
            source.id = make_id("src_");
        }
        if event.id.is_empty() {
            event.id = make_id("sevt_");
        }
        source.id = sanitize_meta(&source.id);
        source.uri = sanitize_text(&source.uri, MAXIMUM_DURABLE_URI);
        source.hash = sanitize_meta(&source.hash);
        source.display_name = sanitize_meta(&source.display_name);
        source.trust_level = sanitize_text(&source.trust_level, 256);
        source.hash_status = sanitize_text(&source.hash_status, 256);
        event.action = sanitize_text(&event.action, 256);
        event.status = sanitize_text(&event.status, 256);
        event.source_hash = sanitize_meta(&event.source_hash);
        event.note = sanitize_text(&event.note, MAXIMUM_DURABLE_METADATA);
        event.source_id = source.id.clone();
        let now = now_ms();
        if source.imported_at == 0 {
            source.imported_at = now;
        }
        source.last_ingested_at = now;
        source.last_checked_at = now;
        source.ingest_count += 1;
        event.created_at = now;
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, false)?;
        self.conn
            .execute(
                "INSERT INTO sources(id,scope_key,source_type,source_uri,source_hash,display_name,trust_level,hash_status,ingest_count,imported_at,last_ingested_at,last_checked_at) \
                 VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(scope_key,id) DO UPDATE SET source_type=excluded.source_type,source_uri=excluded.source_uri,\
                 source_hash=excluded.source_hash,display_name=excluded.display_name,trust_level=excluded.trust_level,hash_status=excluded.hash_status,\
                 ingest_count=sources.ingest_count+1,last_ingested_at=excluded.last_ingested_at,last_checked_at=excluded.last_checked_at;",
                params![
                    source.id,
                    scope_key,
                    source.source_type.as_str(),
                    source.uri,
                    source.hash,
                    source.display_name,
                    source.trust_level,
                    source.hash_status,
                    source.ingest_count as i64,
                    source.imported_at,
                    source.last_ingested_at,
                    source.last_checked_at
                ],
            )
            .map_err(|_| MemoryError::storage("unable to record source"))?;
        self.conn
            .execute(
                "INSERT INTO source_events(id,scope_key,source_id,action,status,source_hash,note,created_at) VALUES(?,?,?,?,?,?,?,?);",
                params![event.id, scope_key, event.source_id, event.action, event.status, event.source_hash, event.note, event.created_at],
            )
            .map_err(|_| MemoryError::storage("unable to record source event"))?;
        tx.commit()
    }

    pub(crate) fn sources(&self, scope: &MemoryScope) -> Result<Vec<SourceRecord>> {
        let mut statement = self.conn.prepare(
            "SELECT id,source_type,source_uri,source_hash,display_name,trust_level,hash_status,ingest_count,imported_at,last_ingested_at,last_checked_at FROM sources WHERE scope_key=? ORDER BY last_checked_at DESC,id;",
        )?;
        let rows = statement
            .query_map([key(scope)?], |row| {
                Ok(SourceRecord {
                    id: text_at(row, 0),
                    source_type: SourceType::parse(&text_at(row, 1)),
                    uri: text_at(row, 2),
                    hash: text_at(row, 3),
                    display_name: text_at(row, 4),
                    trust_level: text_at(row, 5),
                    hash_status: text_at(row, 6),
                    ingest_count: int_at(row, 7).max(0) as u64,
                    imported_at: int_at(row, 8),
                    last_ingested_at: int_at(row, 9),
                    last_checked_at: int_at(row, 10),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub(crate) fn source_events(
        &self,
        scope: &MemoryScope,
        source_id: &str,
    ) -> Result<Vec<SourceEvent>> {
        let mut statement = self.conn.prepare(
            "SELECT id,source_id,action,status,source_hash,note,created_at FROM source_events WHERE scope_key=? AND source_id=? ORDER BY created_at DESC,id;",
        )?;
        let rows = statement
            .query_map(params![key(scope)?, source_id], |row| {
                Ok(SourceEvent {
                    id: text_at(row, 0),
                    source_id: text_at(row, 1),
                    action: text_at(row, 2),
                    status: text_at(row, 3),
                    source_hash: text_at(row, 4),
                    note: text_at(row, 5),
                    created_at: int_at(row, 6),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub(crate) fn record_audit(
        &self,
        scope: &MemoryScope,
        write: AuditEventWrite,
    ) -> Result<String> {
        validate_scope(scope)?;
        if write.actor.is_empty() || write.action.is_empty() || write.reason.is_empty() {
            return invalid("audit event requires actor, action, and reason");
        }
        let id = make_id("audit_");
        self.conn
            .prepare_cached(
                "INSERT INTO audit_events(id,scope_key,actor,action,reason,before_id,after_id,evidence_uri,metadata_json,created_at) VALUES(?,?,?,?,?,?,?,?,?,?);",
            )?
            .execute(params![
                id,
                key(scope)?,
                sanitize_meta(&write.actor),
                sanitize_meta(&write.action),
                sanitize_text(&write.reason, MAXIMUM_DURABLE_METADATA),
                sanitize_meta(&write.before_id),
                sanitize_meta(&write.after_id),
                sanitize_text(&write.evidence_uri, MAXIMUM_DURABLE_URI),
                sanitize_json_default(&write.metadata_json),
                now_ms()
            ])
            .map_err(|_| MemoryError::storage("unable to record audit event"))?;
        Ok(id)
    }

    pub(crate) fn audit_events(
        &self,
        scope: &MemoryScope,
        limit: usize,
    ) -> Result<Vec<AuditEvent>> {
        let mut statement = self.conn.prepare(
            "SELECT id,actor,action,reason,before_id,after_id,evidence_uri,metadata_json,created_at FROM audit_events WHERE scope_key=? ORDER BY created_at DESC,id LIMIT ?;",
        )?;
        let rows = statement
            .query_map(params![key(scope)?, limit as i64], |row| {
                Ok(AuditEvent {
                    id: text_at(row, 0),
                    actor: text_at(row, 1),
                    action: text_at(row, 2),
                    reason: text_at(row, 3),
                    before_id: text_at(row, 4),
                    after_id: text_at(row, 5),
                    evidence_uri: text_at(row, 6),
                    metadata_json: text_at(row, 7),
                    created_at: int_at(row, 8),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub(crate) fn record_session_event_once(
        &self,
        scope: &MemoryScope,
        write: SessionEventWrite,
    ) -> Result<SessionEventRecordResult> {
        validate_scope(scope)?;
        if write.event_type.is_empty() {
            return invalid("session event type is required");
        }
        let event_type = sanitize_text(&write.event_type, 256);
        let dedup = sanitize_text(&write.deduplication_key, 256);
        let id = make_id("session_evt_");
        let scope_key = key(scope)?;
        let inserted = self
            .conn
            .execute(
                "INSERT INTO session_events(id,scope_key,event_type,source,payload_json,created_at,deduplication_key) VALUES(?,?,?,?,?,?,?) ON CONFLICT DO NOTHING;",
                params![id, scope_key, event_type, sanitize_meta(&write.source), sanitize_json_default(&write.payload_json), now_ms(), dedup],
            )
            .map_err(|_| MemoryError::storage("unable to record session event"))?;
        if inserted != 0 {
            return Ok(SessionEventRecordResult { id, inserted: true });
        }
        if dedup.is_empty() {
            return storage("unable to record session event");
        }
        let existing = self
            .conn
            .query_row(
                "SELECT id FROM session_events WHERE scope_key=? AND event_type=? AND deduplication_key=? LIMIT 1;",
                params![scope_key, event_type, dedup],
                |row| Ok(text_at(row, 0)),
            )
            .map_err(|_| MemoryError::storage("unable to resolve deduplicated session event"))?;
        Ok(SessionEventRecordResult {
            id: existing,
            inserted: false,
        })
    }

    pub(crate) fn session_events(
        &self,
        scope: &MemoryScope,
        event_type: &str,
        limit: usize,
        offset: usize,
    ) -> Result<Vec<SessionEvent>> {
        let mut sql = String::from(
            "SELECT id,event_type,source,payload_json,created_at,deduplication_key FROM session_events WHERE scope_key=? ",
        );
        let mut binds = vec![sql_text(key(scope)?)];
        if !event_type.is_empty() {
            sql += "AND event_type=? ";
            binds.push(sql_text(event_type));
        }
        sql += "ORDER BY created_at DESC,id LIMIT ? OFFSET ?;";
        binds.push(Sql::Integer(limit as i64));
        binds.push(Sql::Integer(offset as i64));
        let mut statement = self.conn.prepare(&sql)?;
        let rows = statement
            .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                Ok(SessionEvent {
                    id: text_at(row, 0),
                    event_type: text_at(row, 1),
                    source: text_at(row, 2),
                    payload_json: text_at(row, 3),
                    created_at: int_at(row, 4),
                    deduplication_key: text_at(row, 5),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    fn block_ordering(keys: &[String], tail: &str) -> String {
        let mut ordering = String::from("CASE scope_key ");
        for index in 0..keys.len() {
            ordering += &format!("WHEN ? THEN {index} ");
        }
        ordering += "ELSE 999 END";
        ordering += tail;
        ordering
    }

    pub(crate) fn get_block(&self, scope: &MemoryScope, id: &str) -> Result<ContextBlock> {
        let keys = allowed_keys(scope, RecallPolicy::Broad, false)?;
        let sql = format!(
            "SELECT id,content,updated_at FROM context_blocks WHERE id=? AND {} ORDER BY {} LIMIT 1;",
            scope_filter(&keys),
            Self::block_ordering(&keys, "")
        );
        let mut binds = vec![sql_text(id)];
        binds.extend(sql_keys(&keys));
        binds.extend(sql_keys(&keys));
        let mut statement = self.conn.prepare(&sql)?;
        let mut rows = statement.query(rusqlite::params_from_iter(binds.iter()))?;
        Ok(match rows.next()? {
            Some(row) => ContextBlock {
                id: text_at(row, 0),
                content: text_at(row, 1),
                updated_at: int_at(row, 2),
            },
            None => ContextBlock {
                id: id.to_string(),
                ..Default::default()
            },
        })
    }

    pub(crate) fn update_block(
        &self,
        scope: &MemoryScope,
        id: &str,
        content: &str,
    ) -> Result<ContextBlock> {
        validate_scope(scope)?;
        if id.is_empty() {
            return invalid("context block id is required");
        }
        let content = sanitize_text(content, MAXIMUM_DURABLE_TEXT);
        let now = now_ms();
        self.conn
            .execute(
                "INSERT INTO context_blocks(scope_key,id,content,updated_at) VALUES(?,?,?,?) ON CONFLICT(scope_key,id) DO UPDATE SET content=excluded.content,updated_at=excluded.updated_at;",
                params![key(scope)?, id, content, now],
            )
            .map_err(|_| MemoryError::storage("unable to update context block"))?;
        Ok(ContextBlock {
            id: id.to_string(),
            content,
            updated_at: now,
        })
    }

    pub(crate) fn blocks(&self, scope: &MemoryScope) -> Result<Vec<ContextBlock>> {
        let keys = allowed_keys(scope, RecallPolicy::Broad, false)?;
        let sql = format!(
            "SELECT id,content,updated_at FROM context_blocks WHERE {} ORDER BY {};",
            scope_filter(&keys),
            Self::block_ordering(&keys, ",id")
        );
        let binds: Vec<Sql> = sql_keys(&keys).chain(sql_keys(&keys)).collect();
        let mut statement = self.conn.prepare(&sql)?;
        let rows = statement
            .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                Ok(ContextBlock {
                    id: text_at(row, 0),
                    content: text_at(row, 1),
                    updated_at: int_at(row, 2),
                })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        let mut seen = std::collections::HashSet::new();
        Ok(rows
            .into_iter()
            .filter(|block| seen.insert(block.id.clone()))
            .collect())
    }

    pub(crate) fn add_capture_candidate_once(
        &self,
        scope: &MemoryScope,
        mut write: MemoryWrite,
        claim: SessionEventWrite,
    ) -> Result<(String, bool)> {
        validate_scope(scope)?;
        if claim.deduplication_key.is_empty() || claim.event_type != "candidate_ingestion_claim" {
            return invalid("capture candidate requires a deduplicated ingestion claim");
        }
        write.content = normalize_content(&sanitize_text(&write.content, MAXIMUM_DURABLE_TEXT));
        if write.content.is_empty() {
            return invalid("capture candidate must not be empty");
        }
        let canonicalize = |value: &str| ascii_lower(&normalize_content(value));
        let canonical = canonicalize(&write.content);
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let prior = {
            let mut statement = self.conn.prepare_cached(
                "SELECT memory_id,canonical_content FROM capture_candidate_receipts WHERE scope_key=? AND deduplication_key=?;",
            )?;
            let mut rows = statement.query(params![scope_key, claim.deduplication_key])?;
            rows.next()?.map(|row| (text_at(row, 0), text_at(row, 1)))
        };
        if let Some((id, prior_canonical)) = prior {
            if prior_canonical != canonical {
                return invalid("capture receipt key reused for different content");
            }
            tx.commit()?;
            return Ok((id, false));
        }
        let existing = {
            let mut statement = self
                .conn
                .prepare("SELECT id,content FROM memories WHERE scope_key=? ORDER BY id;")?;
            let rows = statement
                .query_map([&scope_key], |row| Ok((text_at(row, 0), text_at(row, 1))))?
                .collect::<rusqlite::Result<Vec<_>>>()
                .map_err(|_| MemoryError::storage("unable to inspect capture duplicates"))?;
            rows.into_iter()
                .find(|(_, content)| canonicalize(content) == canonical)
                .map(|(id, _)| id)
        };
        let inserted = existing.is_none();
        let id = match existing {
            Some(id) => id,
            None => {
                write.status = MemoryStatus::Candidate;
                let prepared = crate::service::prepare_remember(scope, write)?;
                self.add(scope, prepared)?
            }
        };
        self.conn
            .execute(
                "INSERT INTO capture_candidate_receipts VALUES(?,?,?,?);",
                params![scope_key, claim.deduplication_key, canonical, id],
            )
            .map_err(|_| MemoryError::storage("unable to publish capture receipt"))?;
        self.record_session_event_once(scope, claim)?;
        tx.commit()?;
        Ok((id, inserted))
    }
}

impl MemoryStore {
    /// Inserts a memory exactly as given (after durable sanitisation). Most
    /// callers want [`crate::MemoryService::remember`], which applies the
    /// write policy first.
    pub fn add(&self, scope: &MemoryScope, write: MemoryWrite) -> Result<String> {
        self.with(|db| db.add(scope, write))
    }

    /// Stores a captured candidate once per `(scope, claim.deduplication_key)`
    /// and reuses an existing memory with the same canonical content. Returns
    /// the memory ID and whether a new memory was created.
    pub fn add_capture_candidate_once(
        &self,
        scope: &MemoryScope,
        write: MemoryWrite,
        claim: SessionEventWrite,
    ) -> Result<(String, bool)> {
        self.with(|db| db.add_capture_candidate_once(scope, write, claim))
    }

    pub fn query(&self, scope: &MemoryScope, query: &MemoryQuery) -> Result<Vec<MemoryEntry>> {
        self.with(|db| db.query(scope, query))
    }

    /// A memory visible from `scope` under the broad policy.
    pub fn get(&self, scope: &MemoryScope, id: &str) -> Result<MemoryEntry> {
        self.with(|db| db.get(scope, id))
    }

    pub fn find_for_recall(
        &self,
        scope: &MemoryScope,
        id: &str,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<Option<MemoryEntry>> {
        self.with(|db| db.find_for_recall(scope, id, policy, include_global_user))
    }

    /// Applies a patch, recording the prior state as an `update` revision.
    pub fn update(
        &self,
        scope: &MemoryScope,
        id: &str,
        patch: &MemoryPatch,
    ) -> Result<MemoryEntry> {
        self.with(|db| db.update(scope, id, patch))
    }

    /// Commits a lifecycle patch, optional edge and audit record atomically,
    /// provided the subject is in `expected_status` in the exact scope.
    pub fn apply_governed_update(
        &self,
        scope: &MemoryScope,
        id: &str,
        expected_status: MemoryStatus,
        patch: &MemoryPatch,
        edge: Option<GraphEdge>,
        audit: AuditEventWrite,
    ) -> Result<MemoryEntry> {
        self.with(|db| db.apply_governed_update(scope, id, expected_status, patch, edge, audit))
    }

    /// Deletes a memory and its edges, keeping a `delete` revision.
    pub fn forget(&self, scope: &MemoryScope, id: &str) -> Result<bool> {
        self.with(|db| db.forget(scope, id))
    }

    pub fn revisions(&self, scope: &MemoryScope, memory_id: &str) -> Result<Vec<MemoryRevision>> {
        self.with(|db| db.revisions(scope, memory_id))
    }

    pub fn record_access(&self, scope: &MemoryScope, memory_ids: &[String]) -> Result<()> {
        self.with(|db| db.record_access(scope, memory_ids))
    }

    /// Number of memories stored in exactly `scope`.
    pub fn size(&self, scope: &MemoryScope) -> Result<usize> {
        self.with(|db| db.size(scope))
    }

    /// Current derived-index revision of a memory (bumped by triggers).
    pub fn retrieval_revision(&self, scope: &MemoryScope, memory_id: &str) -> Result<i64> {
        self.with(|db| db.retrieval_revision(scope, memory_id))
    }

    pub fn statistics(
        &self,
        scope: &MemoryScope,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<MemoryStatistics> {
        self.with(|db| db.statistics(scope, policy, include_global_user))
    }

    pub fn add_edge(&self, scope: &MemoryScope, edge: GraphEdge) -> Result<String> {
        self.with(|db| db.add_edge(scope, edge))
    }

    pub fn query_edges(&self, scope: &MemoryScope, query: &GraphQuery) -> Result<Vec<GraphEdge>> {
        self.with(|db| db.query_edges(scope, query))
    }

    pub fn add_feedback(
        &self,
        scope: &MemoryScope,
        feedback: ProcedureFeedback,
    ) -> Result<(ProcedureRun, MemoryEntry)> {
        let result = self.add_feedback_once(scope, feedback)?;
        Ok((result.run, result.memory))
    }

    pub fn add_feedback_once(
        &self,
        scope: &MemoryScope,
        feedback: ProcedureFeedback,
    ) -> Result<ProcedureFeedbackResult> {
        self.with(|db| db.add_feedback_once(scope, feedback))
    }

    pub fn runs(&self, scope: &MemoryScope, procedure_id: &str) -> Result<Vec<ProcedureRun>> {
        self.with(|db| db.runs(scope, procedure_id))
    }

    pub fn record_event(
        &self,
        scope: &MemoryScope,
        source: SourceRecord,
        event: SourceEvent,
    ) -> Result<()> {
        self.with(|db| db.record_event(scope, source, event))
    }

    pub fn sources(&self, scope: &MemoryScope) -> Result<Vec<SourceRecord>> {
        self.with(|db| db.sources(scope))
    }

    pub fn source_events(&self, scope: &MemoryScope, source_id: &str) -> Result<Vec<SourceEvent>> {
        self.with(|db| db.source_events(scope, source_id))
    }

    pub fn record_audit(&self, scope: &MemoryScope, write: AuditEventWrite) -> Result<String> {
        self.with(|db| db.record_audit(scope, write))
    }

    pub fn audit_events(&self, scope: &MemoryScope, limit: usize) -> Result<Vec<AuditEvent>> {
        self.with(|db| db.audit_events(scope, limit))
    }

    pub fn record_session_event(
        &self,
        scope: &MemoryScope,
        mut write: SessionEventWrite,
    ) -> Result<String> {
        write.deduplication_key.clear();
        Ok(self.record_session_event_once(scope, write)?.id)
    }

    pub fn record_session_event_once(
        &self,
        scope: &MemoryScope,
        write: SessionEventWrite,
    ) -> Result<SessionEventRecordResult> {
        self.with(|db| db.record_session_event_once(scope, write))
    }

    pub fn session_events(
        &self,
        scope: &MemoryScope,
        event_type: &str,
        limit: usize,
        offset: usize,
    ) -> Result<Vec<SessionEvent>> {
        self.with(|db| db.session_events(scope, event_type, limit, offset))
    }

    /// Nearest visible block with `id`; empty content when none exists.
    pub fn get_block(&self, scope: &MemoryScope, id: &str) -> Result<ContextBlock> {
        self.with(|db| db.get_block(scope, id))
    }

    pub fn update_block(
        &self,
        scope: &MemoryScope,
        id: &str,
        content: &str,
    ) -> Result<ContextBlock> {
        self.with(|db| db.update_block(scope, id, content))
    }

    /// Visible blocks, nearest scope winning per ID.
    pub fn blocks(&self, scope: &MemoryScope) -> Result<Vec<ContextBlock>> {
        self.with(|db| db.blocks(scope))
    }
}
