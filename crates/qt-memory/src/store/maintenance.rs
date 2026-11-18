//! Scoped retention (`prune`) and historical redaction scrub.

use rusqlite::params;
use serde_json::json;

use super::{Db, MemoryStore, Tx, int_at, key, text_at};
use crate::error::{MemoryError, Result, invalid};
use crate::model::*;
use crate::scope::validate_scope;
use crate::text::{
    MAXIMUM_DURABLE_JSON, MAXIMUM_DURABLE_METADATA, MAXIMUM_DURABLE_TEXT, MAXIMUM_DURABLE_URI,
    dump, sanitize_json, sanitize_text,
};

/// A key/value table whose scrubbed value is part of its primary key, so a
/// change is a delete plus insert.
struct PairTable {
    name: &'static str,
    select: &'static str,
    limit: usize,
    delete: &'static str,
    insert: &'static str,
}

/// Column scrubbed in place: name, JSON payload?, byte limit.
type Column = (&'static str, bool, usize);

const META: usize = MAXIMUM_DURABLE_METADATA;
const URI: usize = MAXIMUM_DURABLE_URI;

impl Db<'_> {
    pub(crate) fn enforce_retention(
        &self,
        scope: &MemoryScope,
        policy: &RetentionPolicy,
    ) -> Result<RetentionReport> {
        validate_scope(scope)?;
        if policy.batch_limit == 0 || policy.batch_limit > 10_000 {
            return invalid("retention batch_limit must be from 1 to 10000");
        }
        if policy.older_than == 0 && policy.keep_latest == 0 {
            return invalid("retention requires an age cutoff or non-zero keep_latest");
        }
        let scope_key = key(scope)?;
        let select_ids = |table: &str, time_column: &str| -> Result<Vec<String>> {
            let mut sql = format!("SELECT id FROM {table} WHERE scope_key=? ");
            let mut binds: Vec<rusqlite::types::Value> = vec![scope_key.clone().into()];
            if policy.older_than != 0 {
                sql += &format!("AND {time_column}<? ");
                binds.push(policy.older_than.into());
            }
            if policy.keep_latest != 0 {
                sql += &format!(
                    "AND id NOT IN (SELECT id FROM {table} WHERE scope_key=? ORDER BY {time_column} DESC,id DESC LIMIT ?) "
                );
                binds.push(scope_key.clone().into());
                binds.push((policy.keep_latest as i64).into());
            }
            sql += &format!("ORDER BY {time_column} ASC,id ASC LIMIT ?;");
            binds.push((policy.batch_limit as i64).into());
            Ok(self
                .conn
                .prepare(&sql)?
                .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                    Ok(text_at(row, 0))
                })?
                .collect::<rusqlite::Result<_>>()?)
        };
        let pick = |enabled: bool, table: &str, column: &str| -> Result<Vec<String>> {
            if enabled {
                select_ids(table, column)
            } else {
                Ok(Vec::new())
            }
        };
        let session_ids = pick(policy.raw_evidence, "session_events", "created_at")?;
        let source_event_ids = pick(policy.raw_evidence, "source_events", "created_at")?;
        let memory_ids = pick(policy.canonical_memories, "memories", "updated_at")?;
        let audit_ids = pick(policy.audit_events, "audit_events", "created_at")?;
        let block_ids = pick(policy.context_blocks, "context_blocks", "updated_at")?;
        let mut report = RetentionReport {
            dry_run: policy.dry_run,
            session_events: session_ids.len(),
            source_events: source_event_ids.len(),
            canonical_memories: memory_ids.len(),
            audit_events: audit_ids.len(),
            context_blocks: block_ids.len(),
            orphan_sources: 0,
        };
        if policy.dry_run {
            return Ok(report);
        }
        let tx = Tx::begin(self.conn, false)?;
        let remove_ids = |table: &str, ids: &[String]| -> Result<()> {
            let mut statement = self
                .conn
                .prepare(&format!("DELETE FROM {table} WHERE scope_key=? AND id=?;"))?;
            for id in ids {
                statement.execute(params![scope_key, id]).map_err(|_| {
                    MemoryError::storage(format!("retention deletion failed for {table}"))
                })?;
            }
            Ok(())
        };
        remove_ids("session_events", &session_ids)?;
        remove_ids("source_events", &source_event_ids)?;
        remove_ids("audit_events", &audit_ids)?;
        remove_ids("context_blocks", &block_ids)?;
        for id in &memory_ids {
            if !self.forget(scope, id)? {
                return Err(MemoryError::storage(
                    "retention memory disappeared during deletion",
                ));
            }
            self.conn
                .execute(
                    "DELETE FROM retrieval_cross_store_links WHERE scope_key=? AND \
                     ((from_kind='memory' AND from_id=?) OR (to_kind='memory' AND to_id=?));",
                    params![scope_key, id, id],
                )
                .map_err(|_| MemoryError::storage("retention cross-store link deletion failed"))?;
            self.conn
                .execute(
                    "DELETE FROM memory_assertion_revisions WHERE memory_id=?;",
                    [id],
                )
                .map_err(|_| {
                    MemoryError::storage("retention assertion revision deletion failed")
                })?;
        }
        // Entities are scoped canonical structure: remove only those no
        // retained assertion or cross-store link still references.
        let orphan_entities: Vec<String> = self
            .conn
            .prepare(
                "SELECT e.id FROM memory_entities e WHERE e.scope_key=? AND \
                 NOT EXISTS(SELECT 1 FROM memory_assertions a WHERE a.subject_id=e.id OR a.object_id=e.id) AND \
                 NOT EXISTS(SELECT 1 FROM retrieval_cross_store_links l WHERE l.scope_key=e.scope_key AND \
                 ((l.from_kind='entity' AND l.from_id=e.id) OR (l.to_kind='entity' AND l.to_id=e.id))) \
                 ORDER BY e.id LIMIT ?;",
            )?
            .query_map(params![scope_key, policy.batch_limit as i64], |row| Ok(text_at(row, 0)))?
            .collect::<rusqlite::Result<_>>()?;
        for id in &orphan_entities {
            self.conn
                .execute(
                    "DELETE FROM memory_entities WHERE scope_key=? AND id=?;",
                    params![scope_key, id],
                )
                .map_err(|_| MemoryError::storage("retention entity deletion failed"))?;
        }
        if policy.raw_evidence {
            let mut sql = String::from(
                "SELECT id FROM sources s WHERE scope_key=? AND NOT EXISTS(SELECT 1 \
                 FROM source_events e WHERE e.scope_key=s.scope_key AND e.source_id=s.id) \
                 AND NOT EXISTS(SELECT 1 FROM memories m WHERE m.scope_key=s.scope_key \
                 AND m.source_id=s.id) ",
            );
            let mut binds: Vec<rusqlite::types::Value> = vec![scope_key.clone().into()];
            if policy.older_than != 0 {
                sql += "AND last_checked_at<? ";
                binds.push(policy.older_than.into());
            }
            sql += "ORDER BY last_checked_at ASC,id ASC LIMIT ?;";
            binds.push((policy.batch_limit as i64).into());
            let orphans: Vec<String> = self
                .conn
                .prepare(&sql)?
                .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                    Ok(text_at(row, 0))
                })?
                .collect::<rusqlite::Result<_>>()?;
            remove_ids("sources", &orphans)?;
            report.orphan_sources = orphans.len();
        }
        tx.commit()?;
        Ok(report)
    }

    fn scrub_collection(
        &self,
        scope_key: &str,
        policy: &RedactionScrubPolicy,
        report: &mut RedactionScrubReport,
        table: &str,
        columns: &[Column],
    ) -> Result<()> {
        let mut collection = RedactionScrubCollection {
            name: table.into(),
            ..Default::default()
        };
        let select = format!(
            "SELECT id,{} FROM {table} WHERE scope_key=? ORDER BY id LIMIT ?;",
            columns
                .iter()
                .map(|column| column.0)
                .collect::<Vec<_>>()
                .join(",")
        );
        let mut rows: Vec<(String, Vec<String>)> = self
            .conn
            .prepare(&select)?
            .query_map(params![scope_key, policy.batch_limit as i64 + 1], |row| {
                Ok((
                    text_at(row, 0),
                    (1..=columns.len())
                        .map(|index| text_at(row, index))
                        .collect(),
                ))
            })?
            .collect::<rusqlite::Result<_>>()?;
        if rows.len() > policy.batch_limit {
            report.truncated = true;
            rows.truncate(policy.batch_limit);
        }
        let update = format!(
            "UPDATE {table} SET {} WHERE scope_key=? AND id=?;",
            columns
                .iter()
                .map(|column| format!("{}=?", column.0))
                .collect::<Vec<_>>()
                .join(",")
        );
        for (id, originals) in rows {
            collection.examined += 1;
            let values: Vec<String> = originals
                .iter()
                .zip(columns)
                .map(|(original, (_, json, limit))| {
                    if *json {
                        sanitize_json(original, *limit)
                    } else {
                        sanitize_text(original, *limit)
                    }
                })
                .collect();
            if values == originals {
                continue;
            }
            collection.changed += 1;
            if policy.dry_run {
                continue;
            }
            let mut binds: Vec<rusqlite::types::Value> =
                values.into_iter().map(Into::into).collect();
            binds.push(scope_key.to_string().into());
            binds.push(id.clone().into());
            let changed = self
                .conn
                .execute(&update, rusqlite::params_from_iter(binds.iter()));
            if !matches!(changed, Ok(1)) {
                return Err(MemoryError::storage(format!(
                    "redaction scrub update failed for {table}"
                )));
            }
            if table == "memories" {
                self.conn
                    .execute("DELETE FROM memory_fts WHERE memory_id=?;", [&id])
                    .map_err(|_| MemoryError::storage("redaction scrub FTS deletion failed"))?;
                self.conn
                    .execute(
                        "INSERT INTO memory_fts(memory_id,content,source,memory_type,source_uri) \
                         SELECT id,content,source,memory_type,source_uri FROM memories WHERE scope_key=? AND id=?;",
                        params![scope_key, id],
                    )
                    .map_err(|_| MemoryError::storage("redaction scrub FTS restoration failed"))?;
            }
        }
        report.collections.push(collection);
        Ok(())
    }

    /// Rewrites a two-column key/value table whose value is part of the key.
    fn scrub_pairs(
        &self,
        scope_key: &str,
        policy: &RedactionScrubPolicy,
        report: &mut RedactionScrubReport,
        table: &PairTable,
    ) -> Result<()> {
        let PairTable {
            name,
            select,
            limit,
            delete,
            insert,
        } = *table;
        let mut collection = RedactionScrubCollection {
            name: name.into(),
            ..Default::default()
        };
        let mut rows: Vec<(String, String)> = self
            .conn
            .prepare(select)?
            .query_map(params![scope_key, policy.batch_limit as i64 + 1], |row| {
                Ok((text_at(row, 0), text_at(row, 1)))
            })?
            .collect::<rusqlite::Result<_>>()?;
        if rows.len() > policy.batch_limit {
            report.truncated = true;
            rows.truncate(policy.batch_limit);
        }
        for (owner, original) in rows {
            collection.examined += 1;
            let sanitized = sanitize_text(&original, limit);
            if sanitized == original {
                continue;
            }
            collection.changed += 1;
            if policy.dry_run {
                continue;
            }
            self.conn
                .execute(delete, params![owner, original])
                .map_err(|_| {
                    MemoryError::storage(format!("redaction {name} scrub deletion failed"))
                })?;
            self.conn
                .execute(insert, params![owner, sanitized])
                .map_err(|_| {
                    MemoryError::storage(format!("redaction {name} scrub restoration failed"))
                })?;
        }
        report.collections.push(collection);
        Ok(())
    }

    fn scrub_assertions(
        &self,
        scope_key: &str,
        policy: &RedactionScrubPolicy,
        report: &mut RedactionScrubReport,
    ) -> Result<()> {
        let mut collection = RedactionScrubCollection {
            name: "memory_assertions".into(),
            ..Default::default()
        };
        type Row = (String, i64, [String; 5]);
        let mut rows: Vec<Row> = self
            .conn
            .prepare(
                "SELECT a.memory_id,a.revision,a.predicate,a.literal_json,a.qualifiers_json,\
                 a.evidence_json,a.historical_json \
                 FROM memory_assertions a JOIN memories m ON m.id=a.memory_id \
                 WHERE m.scope_key=? ORDER BY a.memory_id,a.revision LIMIT ?;",
            )?
            .query_map(params![scope_key, policy.batch_limit as i64 + 1], |row| {
                Ok((
                    text_at(row, 0),
                    int_at(row, 1),
                    [
                        text_at(row, 2),
                        text_at(row, 3),
                        text_at(row, 4),
                        text_at(row, 5),
                        text_at(row, 6),
                    ],
                ))
            })?
            .collect::<rusqlite::Result<_>>()?;
        if rows.len() > policy.batch_limit {
            report.truncated = true;
            rows.truncate(policy.batch_limit);
        }
        for (memory_id, revision, original) in rows {
            collection.examined += 1;
            let sanitized = [
                sanitize_text(&original[0], 128),
                sanitize_json(&original[1], META),
                sanitize_json(&original[2], META),
                sanitize_json(&original[3], MAXIMUM_DURABLE_JSON),
                sanitize_json(&original[4], MAXIMUM_DURABLE_JSON),
            ];
            if sanitized == original {
                continue;
            }
            collection.changed += 1;
            if policy.dry_run {
                continue;
            }
            self.conn
                .execute(
                    "UPDATE memory_assertions SET predicate=?,literal_json=?,qualifiers_json=?,\
                     evidence_json=?,historical_json=? WHERE memory_id=? AND revision=?;",
                    params![
                        sanitized[0],
                        sanitized[1],
                        sanitized[2],
                        sanitized[3],
                        sanitized[4],
                        memory_id,
                        revision
                    ],
                )
                .map_err(|_| MemoryError::storage("redaction assertion scrub update failed"))?;
        }
        report.collections.push(collection);
        Ok(())
    }

    pub(crate) fn scrub_sensitive_data(
        &self,
        scope: &MemoryScope,
        policy: &RedactionScrubPolicy,
    ) -> Result<RedactionScrubReport> {
        validate_scope(scope)?;
        if policy.batch_limit == 0 || policy.batch_limit > 10_000 {
            return invalid("redaction scrub batch_limit must be from 1 to 10000");
        }
        let scope_key = key(scope)?;
        let mut report = RedactionScrubReport {
            dry_run: policy.dry_run,
            ..Default::default()
        };
        let tx = Tx::begin(self.conn, !policy.dry_run)?;
        let k = scope_key.as_str();
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "memories",
            &[
                ("repository_display", false, URI),
                ("source", false, META),
                ("source_id", false, META),
                ("source_uri", false, URI),
                ("source_hash", false, META),
                ("source_span", false, META),
                ("created_by", false, META),
                ("content", false, MAXIMUM_DURABLE_TEXT),
            ],
        )?;
        self.scrub_assertions(k, policy, &mut report)?;
        // Scope JSON is an identity serialisation; redacting it independently
        // of scope_key would orphan the row, so only the payload is scrubbed.
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "memory_entities",
            &[("kind", false, 256), ("name", false, META)],
        )?;
        self.scrub_pairs(
            k,
            policy,
            &mut report,
            &PairTable {
                name: "memory_entity_aliases",
                select: "SELECT a.entity_id,a.alias FROM memory_entity_aliases a \
                         JOIN memory_entities e ON e.id=a.entity_id WHERE e.scope_key=? \
                         ORDER BY a.entity_id,a.alias LIMIT ?;",
                limit: META,
                delete: "DELETE FROM memory_entity_aliases WHERE entity_id=? AND alias=?;",
                insert: "INSERT OR IGNORE INTO memory_entity_aliases(entity_id,alias) VALUES(?,?);",
            },
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "retrieval_cross_store_links",
            &[
                ("from_repository", false, META),
                ("from_worktree", false, URI),
                ("from_path", false, URI),
                ("from_fingerprint", false, META),
                ("to_repository", false, META),
                ("to_worktree", false, URI),
                ("to_path", false, URI),
                ("to_fingerprint", false, META),
                ("relation", false, 256),
                ("evidence_id", false, META),
            ],
        )?;
        self.scrub_pairs(
            k,
            policy,
            &mut report,
            &PairTable {
                name: "memory_tags",
                select: "SELECT t.memory_id,t.tag FROM memory_tags t JOIN memories m ON \
                         m.id=t.memory_id WHERE m.scope_key=? ORDER BY t.memory_id,t.tag LIMIT ?;",
                limit: 256,
                delete: "DELETE FROM memory_tags WHERE memory_id=? AND tag=?;",
                insert: "INSERT OR IGNORE INTO memory_tags(memory_id,tag) VALUES(?,?);",
            },
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "graph_edges",
            &[("type", false, 256), ("source", false, META)],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "procedure_runs",
            &[
                ("task_id", false, META),
                ("outcome", false, 256),
                ("notes", false, META),
                ("evidence_uri", false, URI),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "memory_revisions",
            &[
                ("revision_kind", false, 256),
                ("entry_json", true, MAXIMUM_DURABLE_JSON),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "sources",
            &[
                ("source_type", false, 256),
                ("source_uri", false, URI),
                ("source_hash", false, META),
                ("display_name", false, META),
                ("trust_level", false, 256),
                ("hash_status", false, 256),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "source_events",
            &[
                ("action", false, 256),
                ("status", false, 256),
                ("source_hash", false, META),
                ("note", false, META),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "review_items",
            &[
                ("kind", false, 256),
                ("priority", false, 256),
                ("reason", false, META),
                ("source_id", false, META),
                ("source_uri", false, URI),
                ("source_hash", false, META),
                ("source_span", false, META),
                ("created_by", false, META),
                ("candidate_json", true, MAXIMUM_DURABLE_JSON),
                ("decision", false, META),
                ("decided_by", false, META),
                ("decision_note", false, META),
                ("application_status", false, 64),
                ("application_error", false, META),
                ("action_key", false, META),
                ("tags_json", true, MAXIMUM_DURABLE_JSON),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "review_actions",
            &[
                ("action_key", false, META),
                ("action", false, 256),
                ("actor", false, META),
                ("note", false, META),
                ("application_status", false, 64),
                ("application_error", false, META),
                ("result_json", true, MAXIMUM_DURABLE_JSON),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "audit_events",
            &[
                ("actor", false, META),
                ("action", false, META),
                ("reason", false, META),
                ("evidence_uri", false, URI),
                ("metadata_json", true, MAXIMUM_DURABLE_JSON),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "session_events",
            &[
                ("event_type", false, 256),
                ("source", false, META),
                ("payload_json", true, MAXIMUM_DURABLE_JSON),
            ],
        )?;
        self.scrub_collection(
            k,
            policy,
            &mut report,
            "context_blocks",
            &[("content", false, MAXIMUM_DURABLE_TEXT)],
        )?;
        if !policy.dry_run && report.changed() != 0 {
            self.record_audit(
                scope,
                AuditEventWrite {
                    actor: "qt:memory_maintenance".into(),
                    action: "redaction_scrub".into(),
                    reason: "explicit historical redaction maintenance".into(),
                    metadata_json: dump(&json!({
                        "changed_rows": report.changed(),
                        "examined_rows": report.examined(),
                        "truncated": report.truncated,
                    })),
                    ..Default::default()
                },
            )?;
        }
        tx.commit()?;
        Ok(report)
    }
}

impl MemoryStore {
    /// Deletes (or with `dry_run` counts) old raw evidence and, when enabled,
    /// canonical memories, audit events and blocks of exactly `scope`.
    pub fn enforce_retention(
        &self,
        scope: &MemoryScope,
        policy: &RetentionPolicy,
    ) -> Result<RetentionReport> {
        self.with(|db| db.enforce_retention(scope, policy))
    }

    /// Re-applies current redaction to historical rows of `scope`, including
    /// FTS entries; `dry_run` only counts.
    pub fn scrub_sensitive_data(
        &self,
        scope: &MemoryScope,
        policy: &RedactionScrubPolicy,
    ) -> Result<RedactionScrubReport> {
        self.with(|db| db.scrub_sensitive_data(scope, policy))
    }
}
