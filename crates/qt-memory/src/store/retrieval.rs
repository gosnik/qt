//! Retrieval storage: entities, structured assertions, durable indexing jobs
//! and leases, chunks, dense vectors, code-owner publication and cross-store
//! links (archived `sqlite_memory_database.cpp` retrieval section,
//! `sqlite_retrieval_links.cpp` and `sqlite_retrieval_maintenance.cpp`).

use std::collections::{HashMap, HashSet};

use rusqlite::types::Value as Sql;
use rusqlite::{Row, params, params_from_iter};
use serde_json::{Value, json};

use super::state::scope_json;
use super::{Db, MemoryStore, Tx, int_at, is_null_at, key, make_id, memory_columns, placeholders};
use super::{read_memory_row, text_at};
use crate::code::CodeSnapshot;
use crate::error::{MemoryError, Result, invalid, storage};
use crate::model::*;
use crate::retrieval::*;
use crate::scope::{allowed_keys, recall_scopes, scope_can_access, validate_scope};
use crate::text::{
    MAXIMUM_DURABLE_JSON, MAXIMUM_DURABLE_METADATA, MAXIMUM_DURABLE_TEXT, dump,
    sanitize_json_default, sanitize_text, stable_hash_hex,
};

const MAXIMUM_LINK_TEXT: usize = 1024;
const ASSERTION_COLUMNS: &str = "memory_id,revision,subject_id,predicate,object_id,literal_json,\
qualifiers_json,positive,valid_from,valid_to,evidence_json,recorded_at,historical_json";

fn keys_sql(keys: &[String]) -> impl Iterator<Item = Sql> + '_ {
    keys.iter().map(|key| Sql::Text(key.clone()))
}

fn text(value: impl Into<String>) -> Sql {
    Sql::Text(value.into())
}

fn s(value: &Value, field: &str, fallback: &str) -> String {
    value
        .get(field)
        .and_then(Value::as_str)
        .unwrap_or(fallback)
        .to_string()
}

pub(crate) fn parse_scope_json(encoded: &str) -> Result<MemoryScope> {
    let value: Value = serde_json::from_str(encoded)
        .map_err(|_| MemoryError::storage("stored scope JSON is malformed"))?;
    let kind = value
        .get("kind")
        .and_then(Value::as_str)
        .ok_or_else(|| MemoryError::storage("stored scope JSON is malformed"))?;
    Ok(MemoryScope {
        kind: ScopeKind::parse(kind)?,
        user: s(&value, "user", ""),
        organization: s(&value, "organization", ""),
        project: s(&value, "project", ""),
        repository: s(&value, "repository", ""),
        repository_display: s(&value, "repository_display", ""),
        branch: s(&value, "branch", ""),
        task: s(&value, "task", ""),
        agent: s(&value, "agent", ""),
        session: s(&value, "session", ""),
        conversation: s(&value, "conversation", ""),
    })
}

fn read_assertion(row: &Row<'_>) -> MemoryAssertion {
    let mut value = MemoryAssertion {
        memory_id: text_at(row, 0),
        revision: int_at(row, 1),
        subject_id: text_at(row, 2),
        predicate: text_at(row, 3),
        object_id: text_at(row, 4),
        literal_json: text_at(row, 5),
        qualifiers_json: text_at(row, 6),
        positive: int_at(row, 7) != 0,
        valid_from: (!is_null_at(row, 8)).then(|| int_at(row, 8)),
        valid_to: (!is_null_at(row, 9)).then(|| int_at(row, 9)),
        recorded_at: int_at(row, 11),
        ..Default::default()
    };
    if let Ok(Value::Array(items)) = serde_json::from_str::<Value>(&text_at(row, 10)) {
        value.evidence = items
            .iter()
            .map(|item| AssertionEvidence {
                source_id: s(item, "source_id", ""),
                source_revision: s(item, "source_revision", ""),
                source_uri: s(item, "source_uri", ""),
                source_hash: s(item, "source_hash", ""),
                source_span: s(item, "source_span", ""),
                created_by: s(item, "created_by", ""),
            })
            .collect();
    }
    if let Ok(historical) = serde_json::from_str::<Value>(&text_at(row, 12))
        && historical.is_object()
    {
        value.historical_content = s(&historical, "content", "");
        value.historical_source = s(&historical, "source", "");
        value.historical_type = MemoryType::parse(&s(&historical, "type", "observation"))
            .unwrap_or(MemoryType::Observation);
        value.historical_status = MemoryStatus::parse(&s(&historical, "status", "hypothesis"))
            .unwrap_or(MemoryStatus::Hypothesis);
        let provenance = &mut value.historical_provenance;
        provenance.source_id = s(&historical, "source_id", "");
        provenance.source_type = SourceType::parse(&s(&historical, "source_type", "unknown"));
        provenance.source_uri = s(&historical, "source_uri", "");
        provenance.source_hash = s(&historical, "source_hash", "");
        provenance.source_span = s(&historical, "source_span", "");
        provenance.created_by = s(&historical, "created_by", "agent");
        provenance.evidence_strength = historical
            .get("evidence_strength")
            .and_then(Value::as_f64)
            .unwrap_or(0.0);
        value.historical_created_at = historical
            .get("created_at")
            .and_then(Value::as_i64)
            .unwrap_or(0);
        value.historical_updated_at = historical
            .get("updated_at")
            .and_then(Value::as_i64)
            .unwrap_or(0);
    }
    value
}

fn read_chunk(row: &Row<'_>) -> RetrievalChunk {
    RetrievalChunk {
        id: text_at(row, 0),
        owner_key: text_at(row, 1),
        owner_revision: int_at(row, 2),
        ordinal: int_at(row, 3).max(0) as usize,
        text: text_at(row, 4),
        content_hash: text_at(row, 5),
        source_uri: text_at(row, 6),
        source_span: text_at(row, 7),
        render_version: text_at(row, 8),
    }
}

fn dense_config_json(config: &DenseIndexConfig) -> Value {
    json!({
        "model_id": config.model_id, "model_fingerprint": config.model_fingerprint,
        "tokenizer_fingerprint": config.tokenizer_fingerprint,
        "query_prefix": config.query_prefix, "document_prefix": config.document_prefix,
        "pooling": config.pooling, "dimensions": config.dimensions,
        "normalize": config.normalize, "encoding": "f32le-v1",
    })
}

/// Euclidean norm; rejects dimension mismatches and non-finite or zero vectors.
pub(crate) fn vector_norm(values: &[f32], dimensions: usize) -> Result<f64> {
    if values.len() != dimensions || dimensions == 0 || dimensions > 65536 {
        return invalid("embedding dimension mismatch");
    }
    let mut sum = 0.0f64;
    for value in values {
        if !value.is_finite() {
            return invalid("embedding contains a non-finite value");
        }
        sum += f64::from(*value) * f64::from(*value);
    }
    if !sum.is_finite() || sum <= 0.0 {
        return invalid("embedding has invalid norm");
    }
    Ok(sum.sqrt())
}

fn encode_vector(values: &[f32], normalize: bool) -> Result<Vec<u8>> {
    let norm = vector_norm(values, values.len())?;
    let mut bytes = Vec::with_capacity(values.len() * 4);
    for value in values {
        let stored = if normalize {
            (f64::from(*value) / norm) as f32
        } else {
            *value
        };
        bytes.extend_from_slice(&stored.to_le_bytes());
    }
    Ok(bytes)
}

fn decode_vector(bytes: &[u8], dimensions: usize) -> Result<Vec<f32>> {
    if dimensions == 0 || dimensions > 65536 || bytes.len() != dimensions * 4 {
        return storage("corrupt dense vector payload");
    }
    let values: Vec<f32> = bytes
        .as_chunks::<4>()
        .0
        .iter()
        .map(|chunk| f32::from_le_bytes(*chunk))
        .collect();
    vector_norm(&values, dimensions)
        .map_err(|_| MemoryError::storage("corrupt dense vector payload"))?;
    Ok(values)
}

fn ordered_hits(a: &DenseSearchHit, b: &DenseSearchHit) -> std::cmp::Ordering {
    b.similarity
        .partial_cmp(&a.similarity)
        .unwrap_or(std::cmp::Ordering::Equal)
        .then_with(|| a.chunk.owner_key.cmp(&b.chunk.owner_key))
        .then_with(|| a.chunk.ordinal.cmp(&b.chunk.ordinal))
        .then_with(|| a.chunk.id.cmp(&b.chunk.id))
}

impl Db<'_> {
    fn changes(&self) -> u64 {
        self.conn.changes()
    }

    pub(crate) fn add_entity(
        &self,
        scope: &MemoryScope,
        mut entity: MemoryEntity,
    ) -> Result<MemoryEntity> {
        validate_scope(scope)?;
        entity.name = sanitize_text(&entity.name, 1024);
        entity.kind = sanitize_text(&entity.kind, 128);
        if entity.name.is_empty() || entity.kind.is_empty() || entity.aliases.len() > 128 {
            return invalid("entity requires a name, kind and bounded aliases");
        }
        if !entity.id.is_empty() {
            return invalid("new entity identity is assigned by storage");
        }
        entity.id = make_id("entity_");
        entity.scope = scope.clone();
        let tx = Tx::begin(self.conn, true)?;
        self.conn
            .execute(
                "INSERT INTO memory_entities VALUES(?,?,?,?,?);",
                params![
                    entity.id,
                    key(scope)?,
                    dump(&scope_json(scope)),
                    entity.kind,
                    entity.name
                ],
            )
            .map_err(|_| MemoryError::storage("unable to create entity"))?;
        entity.aliases.push(entity.name.clone());
        let mut aliases: Vec<String> = entity
            .aliases
            .iter()
            .map(|alias| sanitize_text(alias, 1024))
            .filter(|alias| !alias.is_empty())
            .collect();
        aliases.sort();
        aliases.dedup();
        for alias in &aliases {
            self.conn
                .execute(
                    "INSERT OR IGNORE INTO memory_entity_aliases VALUES(?,?);",
                    params![entity.id, alias],
                )
                .map_err(|_| MemoryError::storage("unable to create entity alias"))?;
        }
        entity.aliases = aliases;
        tx.commit()?;
        Ok(entity)
    }

    pub(crate) fn find_entities(
        &self,
        scope: &MemoryScope,
        name: &str,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
    ) -> Result<Vec<MemoryEntity>> {
        let keys = allowed_keys(scope, policy, include_global_user)?;
        let sql = format!(
            "SELECT e.id,e.kind,e.name,e.scope_json FROM memory_entities e WHERE e.scope_key IN ({}) AND \
             (e.id=? OR EXISTS(SELECT 1 FROM memory_entity_aliases a WHERE a.entity_id=e.id AND a.alias=?)) \
             ORDER BY e.id LIMIT ?;",
            placeholders(keys.len())
        );
        let mut binds: Vec<Sql> = keys_sql(&keys).collect();
        binds.push(text(name));
        binds.push(text(name));
        binds.push(Sql::Integer(limit.min(1000) as i64));
        let rows: Vec<(String, String, String, String)> = {
            let mut statement = self.conn.prepare(&sql)?;
            statement
                .query_map(params_from_iter(binds), |row| {
                    Ok((
                        text_at(row, 0),
                        text_at(row, 1),
                        text_at(row, 2),
                        text_at(row, 3),
                    ))
                })?
                .collect::<rusqlite::Result<_>>()
                .map_err(|_| MemoryError::storage("unable to resolve entity"))?
        };
        let mut result = Vec::with_capacity(rows.len());
        for (id, kind, name, scope_text) in rows {
            let aliases = self
                .conn
                .prepare_cached(
                    "SELECT alias FROM memory_entity_aliases WHERE entity_id=? ORDER BY alias;",
                )?
                .query_map([&id], |row| Ok(text_at(row, 0)))?
                .collect::<rusqlite::Result<Vec<_>>>()
                .map_err(|_| MemoryError::storage("unable to read entity aliases"))?;
            result.push(MemoryEntity {
                id,
                kind,
                name,
                aliases,
                scope: parse_scope_json(&scope_text)?,
            });
        }
        Ok(result)
    }

    pub(crate) fn add_assertion(
        &self,
        scope: &MemoryScope,
        assertion: MemoryAssertion,
    ) -> Result<()> {
        let tx = Tx::begin(self.conn, true)?;
        let memory = self.get(scope, &assertion.memory_id)?;
        if key(&memory.scope)? != key(scope)? {
            return invalid("assertion writes require the memory's exact scope");
        }
        if assertion.revision != self.retrieval_revision(scope, &assertion.memory_id)? {
            return invalid("assertion revision is stale");
        }
        if assertion.predicate.is_empty()
            || assertion.predicate.len() > 128
            || assertion.evidence.is_empty()
            || assertion.evidence.len() > 128
            || matches!((assertion.valid_from, assertion.valid_to), (Some(from), Some(to)) if from >= to)
        {
            return invalid("assertion requires predicate, evidence and a valid time interval");
        }
        let require_entity = |id: &str| -> Result<()> {
            if id.is_empty()
                || self
                    .find_entities(scope, id, RecallPolicy::Broad, false, 1)?
                    .is_empty()
            {
                return invalid("assertion endpoint is not a visible entity");
            }
            Ok(())
        };
        require_entity(&assertion.subject_id)?;
        if !assertion.object_id.is_empty() {
            require_entity(&assertion.object_id)?;
        }
        let literal = serde_json::from_str::<Value>(&assertion.literal_json);
        let qualifiers = serde_json::from_str::<Value>(&assertion.qualifiers_json);
        let (Ok(literal), Ok(qualifiers)) = (literal, qualifiers) else {
            return invalid("assertion requires one object and bounded JSON qualifiers");
        };
        if !qualifiers.is_object()
            || (assertion.object_id.is_empty() && literal.is_null())
            || (!assertion.object_id.is_empty() && !literal.is_null())
            || assertion.literal_json.len() > MAXIMUM_DURABLE_METADATA
            || assertion.qualifiers_json.len() > MAXIMUM_DURABLE_METADATA
        {
            return invalid("assertion requires one object and bounded JSON qualifiers");
        }
        let mut evidence = Vec::with_capacity(assertion.evidence.len());
        for item in &assertion.evidence {
            if (item.source_id.is_empty() && item.source_uri.is_empty())
                || item.source_hash.is_empty()
            {
                return invalid("assertion evidence requires a locator and fingerprint");
            }
            evidence.push(json!({
                "source_id": item.source_id, "source_revision": item.source_revision,
                "source_uri": item.source_uri, "source_hash": item.source_hash,
                "source_span": item.source_span, "created_by": item.created_by,
            }));
        }
        let evidence = dump(&Value::Array(evidence));
        if evidence.len() > MAXIMUM_DURABLE_JSON {
            return invalid("assertion evidence exceeds storage limit");
        }
        let historical = json!({
            "content": memory.content, "source": memory.source,
            "type": memory.memory_type.as_str(), "status": memory.status.as_str(),
            "source_id": memory.provenance.source_id,
            "source_type": memory.provenance.source_type.as_str(),
            "source_uri": memory.provenance.source_uri,
            "source_hash": memory.provenance.source_hash,
            "source_span": memory.provenance.source_span,
            "created_by": memory.provenance.created_by,
            "evidence_strength": memory.provenance.evidence_strength,
            "created_at": memory.created_at, "updated_at": memory.updated_at,
        });
        self.conn
            .execute(
                "INSERT INTO memory_assertions VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);",
                params![
                    assertion.memory_id,
                    assertion.revision,
                    assertion.subject_id,
                    sanitize_text(&assertion.predicate, 128),
                    assertion.object_id,
                    sanitize_json_default(&dump(&literal)),
                    sanitize_json_default(&dump(&qualifiers)),
                    i64::from(assertion.positive),
                    assertion.valid_from,
                    assertion.valid_to,
                    sanitize_json_default(&evidence),
                    now_ms(),
                    sanitize_json_default(&dump(&historical)),
                ],
            )
            .map_err(|_| MemoryError::invalid("assertion revision already exists or is invalid"))?;
        tx.commit()
    }

    pub(crate) fn assertion(
        &self,
        scope: &MemoryScope,
        memory_id: &str,
        as_of: Option<Millis>,
    ) -> Result<Option<MemoryAssertion>> {
        let memory = self.get(scope, memory_id)?;
        if !matches!(
            memory.status,
            MemoryStatus::Active | MemoryStatus::Validated
        ) {
            return Ok(None);
        }
        let at = as_of.unwrap_or_else(now_ms);
        let sql = format!(
            "SELECT {ASSERTION_COLUMNS} FROM memory_assertions WHERE memory_id=? AND recorded_at<=? \
             AND (valid_from IS NULL OR valid_from<=?) AND (valid_to IS NULL OR valid_to>?) {}\
             ORDER BY revision DESC LIMIT 1;",
            if as_of.is_some() {
                "AND revision=(SELECT MAX(revision) FROM memory_assertion_revisions WHERE memory_id=? AND recorded_at<=?) "
            } else {
                "AND revision=? "
            }
        );
        let mut binds = vec![
            text(memory_id),
            Sql::Integer(at),
            Sql::Integer(at),
            Sql::Integer(at),
        ];
        if as_of.is_some() {
            binds.push(text(memory_id));
            binds.push(Sql::Integer(at));
        } else {
            binds.push(Sql::Integer(self.retrieval_revision(scope, memory_id)?));
        }
        let mut statement = self.conn.prepare(&sql)?;
        let mut rows = statement.query(params_from_iter(binds))?;
        match rows
            .next()
            .map_err(|_| MemoryError::storage("unable to read assertion"))?
        {
            Some(row) => Ok(Some(read_assertion(row))),
            None => Ok(None),
        }
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn query_assertions(
        &self,
        scope: &MemoryScope,
        subject_id: &str,
        predicate: &str,
        object_id: &str,
        as_of: Option<Millis>,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
    ) -> Result<Vec<MemoryAssertion>> {
        let keys = allowed_keys(scope, policy, include_global_user)?;
        if subject_id.is_empty() && predicate.is_empty() && object_id.is_empty() {
            return Ok(Vec::new());
        }
        let list = placeholders(keys.len());
        let mut sql = format!(
            "SELECT a.memory_id,a.revision,a.subject_id,a.predicate,a.object_id,a.literal_json,\
             a.qualifiers_json,a.positive,a.valid_from,a.valid_to,a.evidence_json,a.recorded_at,\
             a.historical_json FROM memory_assertions a JOIN memories m ON m.id=a.memory_id \
             JOIN retrieval_owners o ON o.owner_key='memory:'||m.id \
             JOIN memory_entities s ON s.id=a.subject_id LEFT JOIN memory_entities t ON t.id=a.object_id \
             WHERE m.scope_key IN ({list}) AND s.scope_key IN ({list}) \
             AND (a.object_id='' OR t.scope_key IN ({list})) AND o.deleted=0 \
             AND m.status IN ('active','validated') "
        );
        if as_of.is_none() {
            sql += "AND o.revision=a.revision ";
        }
        sql += "AND a.recorded_at<=? AND (a.valid_from IS NULL OR a.valid_from<=?) AND (a.valid_to IS NULL OR a.valid_to>?) ";
        if as_of.is_some() {
            sql += "AND a.revision=(SELECT MAX(r.revision) FROM memory_assertion_revisions r \
                    WHERE r.memory_id=a.memory_id AND r.recorded_at<=?) ";
        }
        if !subject_id.is_empty() {
            sql += "AND a.subject_id=? ";
        }
        if !predicate.is_empty() {
            sql += "AND a.predicate=? ";
        }
        if !object_id.is_empty() {
            sql += "AND a.object_id=? ";
        }
        sql += "ORDER BY a.memory_id,a.revision DESC LIMIT ?;";
        let when = as_of.unwrap_or_else(now_ms);
        let mut binds: Vec<Sql> = Vec::new();
        for _ in 0..3 {
            binds.extend(keys_sql(&keys));
        }
        binds.extend([Sql::Integer(when), Sql::Integer(when), Sql::Integer(when)]);
        if as_of.is_some() {
            binds.push(Sql::Integer(when));
        }
        for value in [subject_id, predicate, object_id] {
            if !value.is_empty() {
                binds.push(text(value));
            }
        }
        binds.push(Sql::Integer(limit.min(1000) as i64));
        let mut statement = self.conn.prepare(&sql)?;
        let result = statement
            .query_map(params_from_iter(binds), |row| Ok(read_assertion(row)))?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(|_| MemoryError::storage("unable to query structured memory"))?;
        Ok(result)
    }

    pub(crate) fn claim_index_job(
        &self,
        scope: &MemoryScope,
        lease_seconds: i64,
        dense_generation: Option<&str>,
    ) -> Result<Option<RetrievalIndexJob>> {
        validate_scope(scope)?;
        if !(1..=300).contains(&lease_seconds) {
            return invalid("index lease must be between one and 300 seconds");
        }
        let now = now_ms();
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let generation_filter = if dense_generation.is_some() {
            "AND (representation='chunks' OR (representation='dense' AND generation=?)) "
        } else {
            ""
        };
        // A crash cannot report failure; retire only jobs this claimant could
        // otherwise recover (same scope and current revision, eligible retry or
        // expired lease, requested generation set).
        let exhaust = format!(
            "UPDATE retrieval_jobs SET state='failed',lease_token='',lease_until=0,next_attempt=0,\
             error=CASE WHEN state='leased' THEN 'retry limit reached after lease expiry' \
             ELSE 'retry limit reached before retry' END WHERE attempts>=max_attempts \
             AND ((state='pending' AND next_attempt<=?) OR (state='leased' AND lease_until<=?)) \
             AND EXISTS(SELECT 1 FROM retrieval_owners o WHERE o.owner_key=retrieval_jobs.owner_key \
             AND o.scope_key=? AND o.deleted=0 AND o.revision=retrieval_jobs.owner_revision) {generation_filter}"
        );
        let mut binds = vec![
            Sql::Integer(now),
            Sql::Integer(now),
            text(scope_key.clone()),
        ];
        if let Some(generation) = dense_generation {
            binds.push(text(generation));
        }
        self.conn
            .execute(&exhaust, params_from_iter(binds))
            .map_err(|_| MemoryError::storage("unable to exhaust index retries"))?;
        let query = format!(
            "SELECT j.id,j.owner_key,j.owner_revision,j.representation,j.generation,j.attempts,\
             o.content,o.source_uri FROM retrieval_jobs j JOIN retrieval_owners o ON o.owner_key=j.owner_key \
             WHERE o.scope_key=? AND o.deleted=0 AND o.revision=j.owner_revision \
             AND ((j.state='pending' AND j.next_attempt<=?) OR (j.state='leased' AND j.lease_until<=?)) \
             AND j.attempts<j.max_attempts {generation_filter}ORDER BY j.id LIMIT 1;"
        );
        let mut binds = vec![
            text(scope_key.clone()),
            Sql::Integer(now),
            Sql::Integer(now),
        ];
        if let Some(generation) = dense_generation {
            binds.push(text(generation));
        }
        let found = {
            let mut statement = self.conn.prepare(&query)?;
            let mut rows = statement.query(params_from_iter(binds))?;
            rows.next()
                .map_err(|_| MemoryError::storage("unable to claim index job"))?
                .map(|row| RetrievalIndexJob {
                    id: int_at(row, 0),
                    owner_key: text_at(row, 1),
                    owner_revision: int_at(row, 2),
                    representation: text_at(row, 3),
                    generation: text_at(row, 4),
                    attempts: int_at(row, 5).max(0) as usize + 1,
                    content: text_at(row, 6),
                    source_uri: text_at(row, 7),
                    lease_token: make_id("lease_"),
                })
        };
        let Some(job) = found else {
            tx.commit()?;
            return Ok(None);
        };
        let changed = self
            .conn
            .execute(
                "UPDATE retrieval_jobs SET state='leased',lease_token=?,lease_until=?,attempts=attempts+1 \
                 WHERE id=? AND attempts<max_attempts AND EXISTS(SELECT 1 FROM retrieval_owners o \
                 WHERE o.owner_key=retrieval_jobs.owner_key AND o.scope_key=? AND o.deleted=0 \
                 AND o.revision=retrieval_jobs.owner_revision);",
                params![job.lease_token, now + lease_seconds * 1000, job.id, scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to lease index job"))?;
        if changed != 1 {
            return storage("unable to lease index job");
        }
        tx.commit()?;
        Ok(Some(job))
    }

    pub(crate) fn publish_chunks(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        chunks: Vec<RetrievalChunk>,
    ) -> Result<bool> {
        validate_scope(scope)?;
        if chunks.is_empty() || chunks.len() > 4096 || job.lease_token.is_empty() {
            return invalid("chunk publication requires a lease and bounded nonempty chunks");
        }
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let source_uri: Option<String> = {
            let mut statement = self.conn.prepare_cached(
                "SELECT o.source_uri FROM retrieval_jobs j JOIN retrieval_owners o ON o.owner_key=j.owner_key \
                 WHERE j.id=? AND j.owner_key=? AND j.owner_revision=? AND j.representation='chunks' AND j.generation=? \
                 AND j.state='leased' AND j.lease_token=? AND j.lease_until>? AND o.scope_key=? \
                 AND o.deleted=0 AND o.revision=j.owner_revision;",
            )?;
            let mut rows = statement.query(params![
                job.id,
                job.owner_key,
                job.owner_revision,
                job.generation,
                job.lease_token,
                now_ms(),
                scope_key
            ])?;
            rows.next()
                .map_err(|_| MemoryError::storage("unable to validate chunk publication"))?
                .map(|row| text_at(row, 0))
        };
        let Some(source_uri) = source_uri else {
            return Ok(false);
        };
        self.conn
            .execute(
                "DELETE FROM retrieval_chunks WHERE owner_key=?;",
                [&job.owner_key],
            )
            .map_err(|_| MemoryError::storage("unable to replace chunks"))?;
        let mut total_bytes = 0usize;
        for (ordinal, chunk) in chunks.into_iter().enumerate() {
            total_bytes += chunk.text.len();
            if chunk.text.is_empty()
                || chunk.text.len() > 32768
                || total_bytes > 4 * MAXIMUM_DURABLE_TEXT
            {
                return invalid("chunk content exceeds publication budget");
            }
            let content = sanitize_text(&chunk.text, 32768);
            let id = format!(
                "{}:{}:{}:{ordinal}",
                job.owner_key, job.owner_revision, job.generation
            );
            self.conn
                .execute(
                    "INSERT INTO retrieval_chunks VALUES(?,?,?,?,?,?,?,?,?);",
                    params![
                        id,
                        job.owner_key,
                        job.owner_revision,
                        ordinal as i64,
                        content,
                        stable_hash_hex(&content),
                        source_uri,
                        sanitize_text(&chunk.source_span, 1024),
                        job.generation
                    ],
                )
                .map_err(|_| MemoryError::storage("unable to publish chunk"))?;
        }
        self.conn
            .execute(
                "UPDATE retrieval_jobs SET state='complete',lease_token='',lease_until=0,error='' WHERE id=?;",
                [job.id],
            )
            .map_err(|_| MemoryError::storage("unable to complete index job"))?;
        self.conn
            .execute(
                "INSERT OR IGNORE INTO retrieval_jobs(owner_key,owner_revision,representation,generation) \
                 SELECT ?,?,'dense',generation FROM retrieval_dense_indexes WHERE scope_key=? AND state IN ('building','active');",
                params![job.owner_key, job.owner_revision, scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to schedule dense indexing"))?;
        tx.commit()?;
        Ok(true)
    }

    pub(crate) fn fail_index_job(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        error: &str,
        max_attempts: usize,
    ) -> Result<bool> {
        validate_scope(scope)?;
        if !(1..=100).contains(&max_attempts) || job.lease_token.is_empty() {
            return invalid("invalid index retry policy or lease");
        }
        let now = now_ms();
        let backoff = (job.attempts * 2).min(60) as i64 * 1000;
        self.conn
            .execute(
                "UPDATE retrieval_jobs SET max_attempts=?,state=CASE WHEN attempts>=? THEN 'failed' ELSE 'pending' END,\
                 error=?,next_attempt=?,lease_token='',lease_until=0 WHERE id=? AND owner_key=? AND owner_revision=? \
                 AND state='leased' AND lease_token=? AND lease_until>? AND EXISTS(SELECT 1 FROM retrieval_owners o \
                 WHERE o.owner_key=retrieval_jobs.owner_key AND o.scope_key=? AND o.deleted=0 AND o.revision=owner_revision);",
                params![
                    max_attempts as i64,
                    max_attempts as i64,
                    sanitize_text(error, 2048),
                    now + backoff,
                    job.id,
                    job.owner_key,
                    job.owner_revision,
                    job.lease_token,
                    now,
                    key(scope)?
                ],
            )
            .map_err(|_| MemoryError::storage("unable to record indexing failure"))?;
        Ok(self.changes() == 1)
    }

    pub(crate) fn renew_index_job(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        lease_seconds: i64,
    ) -> Result<bool> {
        validate_scope(scope)?;
        if !(1..=300).contains(&lease_seconds) || job.lease_token.is_empty() {
            return invalid("invalid indexing lease renewal");
        }
        let now = now_ms();
        self.conn
            .execute(
                "UPDATE retrieval_jobs SET lease_until=? WHERE id=? AND owner_key=? AND owner_revision=? \
                 AND state='leased' AND lease_token=? AND lease_until>? AND EXISTS(SELECT 1 FROM retrieval_owners o \
                 WHERE o.owner_key=retrieval_jobs.owner_key AND o.scope_key=? AND o.deleted=0 AND o.revision=owner_revision);",
                params![
                    now + lease_seconds * 1000,
                    job.id,
                    job.owner_key,
                    job.owner_revision,
                    job.lease_token,
                    now,
                    key(scope)?
                ],
            )
            .map_err(|_| MemoryError::storage("unable to renew indexing lease"))?;
        Ok(self.changes() == 1)
    }

    pub(crate) fn retrieval_index_status(
        &self,
        scope: &MemoryScope,
    ) -> Result<RetrievalIndexStatus> {
        validate_scope(scope)?;
        let scope_key = key(scope)?;
        let mut status = RetrievalIndexStatus::default();
        let rows = self
            .conn
            .prepare_cached(
                "SELECT j.state,COUNT(*) FROM retrieval_jobs j JOIN retrieval_owners o ON o.owner_key=j.owner_key \
                 WHERE o.scope_key=? AND o.deleted=0 AND o.revision=j.owner_revision GROUP BY j.state;",
            )?
            .query_map([&scope_key], |row| Ok((text_at(row, 0), int_at(row, 1).max(0) as usize)))?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(|_| MemoryError::storage("unable to read index status"))?;
        for (state, count) in rows {
            match state.as_str() {
                "pending" => status.pending = count,
                "leased" => status.leased = count,
                "failed" => status.failed = count,
                "complete" => status.complete = count,
                _ => {}
            }
        }
        status.chunks = self
            .conn
            .query_row(
                "SELECT COUNT(*) FROM retrieval_chunks c JOIN retrieval_owners o ON o.owner_key=c.owner_key \
                 WHERE o.scope_key=? AND o.deleted=0 AND o.revision=c.owner_revision;",
                [&scope_key],
                |row| Ok(int_at(row, 0).max(0) as usize),
            )
            .map_err(|_| MemoryError::storage("unable to count chunks"))?;
        Ok(status)
    }

    pub(crate) fn retrieval_chunks(
        &self,
        scope: &MemoryScope,
        owner_key: &str,
    ) -> Result<Vec<RetrievalChunk>> {
        let keys = allowed_keys(scope, RecallPolicy::Broad, false)?;
        let sql = format!(
            "SELECT c.id,c.owner_key,c.owner_revision,c.ordinal,c.content,c.content_hash,c.source_uri,\
             c.source_span,c.render_version FROM retrieval_chunks c JOIN retrieval_owners o ON o.owner_key=c.owner_key \
             WHERE c.owner_key=? AND o.deleted=0 AND o.revision=c.owner_revision AND o.scope_key IN ({}) ORDER BY c.ordinal;",
            placeholders(keys.len())
        );
        let mut binds = vec![text(owner_key)];
        binds.extend(keys_sql(&keys));
        let mut statement = self.conn.prepare(&sql)?;
        let chunks = statement
            .query_map(params_from_iter(binds), |row| Ok(read_chunk(row)))?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(|_| MemoryError::storage("unable to read chunks"))?;
        Ok(chunks)
    }

    pub(crate) fn begin_dense_index(
        &self,
        scope: &MemoryScope,
        config: &DenseIndexConfig,
    ) -> Result<()> {
        validate_scope(scope)?;
        if config.generation.is_empty()
            || config.generation.len() > 256
            || config.model_id.is_empty()
            || config.model_fingerprint.is_empty()
            || config.tokenizer_fingerprint.is_empty()
            || config.pooling.is_empty()
            || config.dimensions == 0
            || config.dimensions > 65536
        {
            return invalid("dense index requires a complete embedding-space contract");
        }
        let encoded = dump(&dense_config_json(config));
        if encoded.len() > MAXIMUM_DURABLE_METADATA {
            return invalid("dense index configuration too large");
        }
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let existing = self
            .conn
            .prepare_cached("SELECT config_json FROM retrieval_dense_indexes WHERE generation=?;")?
            .query_map([&config.generation], |row| Ok(text_at(row, 0)))?
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(|_| MemoryError::storage("unable to inspect dense generation"))?;
        if existing.iter().any(|stored| stored != &encoded) {
            return invalid("dense generation already identifies another embedding space");
        }
        self.conn
            .execute(
                "INSERT OR IGNORE INTO retrieval_dense_indexes VALUES(?,?,?,'building');",
                params![scope_key, config.generation, encoded],
            )
            .map_err(|_| MemoryError::storage("unable to create dense generation"))?;
        self.conn
            .execute(
                "INSERT OR IGNORE INTO retrieval_jobs(owner_key,owner_revision,representation,generation) \
                 SELECT o.owner_key,o.revision,'dense',? FROM retrieval_owners o WHERE o.scope_key=? AND o.deleted=0 \
                 AND EXISTS(SELECT 1 FROM retrieval_chunks c WHERE c.owner_key=o.owner_key AND c.owner_revision=o.revision);",
                params![config.generation, scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to schedule dense backfill"))?;
        tx.commit()
    }

    pub(crate) fn dense_index(
        &self,
        scope: &MemoryScope,
        generation: &str,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<Option<DenseIndexConfig>> {
        validate_scope(scope)?;
        let keys = allowed_keys(scope, policy, include_global_user)?;
        let mut precedence = String::from(" CASE scope_key");
        for index in 0..keys.len() {
            precedence += &format!(" WHEN ? THEN {index}");
        }
        precedence += " END";
        let sql = format!(
            "SELECT generation,config_json FROM retrieval_dense_indexes WHERE scope_key IN ({}) AND {} ORDER BY{precedence} LIMIT 1;",
            placeholders(keys.len()),
            if generation.is_empty() {
                "state='active'"
            } else {
                "generation=?"
            }
        );
        let mut binds: Vec<Sql> = keys_sql(&keys).collect();
        if !generation.is_empty() {
            binds.push(text(generation));
        }
        binds.extend(keys_sql(&keys));
        let found = {
            let mut statement = self.conn.prepare(&sql)?;
            let mut rows = statement.query(params_from_iter(binds))?;
            rows.next()
                .map_err(|_| MemoryError::storage("unable to read dense configuration"))?
                .map(|row| (text_at(row, 0), text_at(row, 1)))
        };
        let Some((generation, config)) = found else {
            return Ok(None);
        };
        let value: Value = serde_json::from_str(&config)
            .map_err(|_| MemoryError::storage("unable to read dense configuration"))?;
        let field = |name: &str| -> Result<&Value> {
            value
                .get(name)
                .ok_or_else(|| MemoryError::storage("unable to read dense configuration"))
        };
        let string = |name: &str| -> Result<String> {
            field(name)?
                .as_str()
                .map(str::to_string)
                .ok_or_else(|| MemoryError::storage("unable to read dense configuration"))
        };
        Ok(Some(DenseIndexConfig {
            generation,
            model_id: string("model_id")?,
            model_fingerprint: string("model_fingerprint")?,
            tokenizer_fingerprint: string("tokenizer_fingerprint")?,
            query_prefix: string("query_prefix")?,
            document_prefix: string("document_prefix")?,
            pooling: string("pooling")?,
            dimensions: field("dimensions")?
                .as_u64()
                .ok_or_else(|| MemoryError::storage("unable to read dense configuration"))?
                as usize,
            normalize: field("normalize")?
                .as_bool()
                .ok_or_else(|| MemoryError::storage("unable to read dense configuration"))?,
        }))
    }

    pub(crate) fn publish_dense_vectors(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        vector_count: usize,
        next_vector: &mut dyn FnMut() -> Result<DenseChunkVector>,
    ) -> Result<bool> {
        validate_scope(scope)?;
        if job.lease_token.is_empty() || vector_count == 0 || vector_count > 4096 {
            return invalid("dense publication requires a lease and bounded vectors");
        }
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let valid = self
            .conn
            .prepare_cached(
                "SELECT 1 FROM retrieval_jobs j JOIN retrieval_owners o ON o.owner_key=j.owner_key \
                 JOIN retrieval_dense_indexes d ON d.scope_key=o.scope_key AND d.generation=j.generation \
                 WHERE j.id=? AND j.owner_key=? AND j.owner_revision=? AND j.representation='dense' AND j.generation=? \
                 AND j.state='leased' AND j.lease_token=? AND j.lease_until>? AND o.scope_key=? \
                 AND o.deleted=0 AND o.revision=j.owner_revision AND d.state IN ('building','active');",
            )?
            .exists(params![
                job.id,
                job.owner_key,
                job.owner_revision,
                job.generation,
                job.lease_token,
                now_ms(),
                scope_key
            ])
            .map_err(|_| MemoryError::storage("unable to validate dense publication"))?;
        if !valid {
            return Ok(false);
        }
        let config = self
            .dense_index(scope, &job.generation, RecallPolicy::Strict, false)?
            .ok_or_else(|| MemoryError::storage("missing dense configuration"))?;
        let chunks = self.retrieval_chunks(scope, &job.owner_key)?;
        if chunks.len() != vector_count {
            return invalid("dense publication must cover every owner chunk");
        }
        let mut expected: HashMap<String, bool> = chunks
            .iter()
            .map(|chunk| (chunk.id.clone(), false))
            .collect();
        for _ in 0..vector_count {
            let vector = next_vector()?;
            match expected.get_mut(&vector.chunk_id) {
                Some(seen) if !*seen => *seen = true,
                _ => return invalid("dense publication contains an unexpected or duplicate chunk"),
            }
            vector_norm(&vector.values, config.dimensions)?;
            let bytes = encode_vector(&vector.values, config.normalize)?;
            self.conn
                .execute(
                    "INSERT OR REPLACE INTO retrieval_vectors VALUES(?,?,?,?);",
                    params![
                        vector.chunk_id,
                        job.generation,
                        config.dimensions as i64,
                        bytes
                    ],
                )
                .map_err(|_| MemoryError::storage("unable to write dense vector"))?;
        }
        self.conn
            .execute(
                "UPDATE retrieval_jobs SET state='complete',lease_token='',lease_until=0,error='' WHERE id=?;",
                [job.id],
            )
            .map_err(|_| MemoryError::storage("unable to complete dense indexing"))?;
        tx.commit()?;
        Ok(true)
    }

    pub(crate) fn activate_dense_index(
        &self,
        scope: &MemoryScope,
        generation: &str,
    ) -> Result<bool> {
        validate_scope(scope)?;
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        if self
            .dense_index(scope, generation, RecallPolicy::Strict, false)?
            .is_none()
        {
            return invalid("unknown dense generation");
        }
        // A new model is published only after rendering and vector backfill
        // have caught up; later edits invalidate individual vectors.
        let missing: i64 = self
            .conn
            .query_row(
                "SELECT COUNT(*) FROM retrieval_owners o WHERE o.scope_key=? AND o.deleted=0 AND \
                 (NOT EXISTS(SELECT 1 FROM retrieval_chunks c WHERE c.owner_key=o.owner_key AND c.owner_revision=o.revision) \
                 OR EXISTS(SELECT 1 FROM retrieval_chunks c WHERE c.owner_key=o.owner_key AND c.owner_revision=o.revision AND \
                 NOT EXISTS(SELECT 1 FROM retrieval_vectors v WHERE v.chunk_id=c.id AND v.generation=?)));",
                params![scope_key, generation],
                |row| Ok(int_at(row, 0)),
            )
            .map_err(|_| MemoryError::storage("unable to inspect dense completeness"))?;
        if missing != 0 {
            return Ok(false);
        }
        self.conn
            .execute(
                "UPDATE retrieval_dense_indexes SET state='retired' WHERE scope_key=? AND state='active';",
                [&scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to retire dense index"))?;
        self.conn
            .execute(
                "UPDATE retrieval_dense_indexes SET state='active' WHERE scope_key=? AND generation=?;",
                params![scope_key, generation],
            )
            .map_err(|_| MemoryError::storage("unable to activate dense index"))?;
        tx.commit()?;
        Ok(true)
    }

    pub(crate) fn resolve_dense_memory_candidates(
        &self,
        scope: &MemoryScope,
        hits: &[DenseSearchHit],
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<Vec<DenseMemoryCandidate>> {
        let keys = allowed_keys(scope, policy, include_global_user)?;
        let sql = format!(
            "SELECT {} FROM memories m JOIN retrieval_owners o ON o.owner_id=m.id AND o.kind='memory' \
             JOIN retrieval_chunks c ON c.owner_key=o.owner_key AND c.owner_revision=o.revision \
             JOIN retrieval_vectors v ON v.chunk_id=c.id AND v.generation=? \
             WHERE o.owner_key=? AND o.revision=? AND c.id=? AND c.content_hash=? AND o.deleted=0 \
             AND c.content=? AND c.ordinal=? AND c.source_uri=? AND c.source_span=? AND c.render_version=? \
             AND m.status IN ('active','validated') AND m.scope_key IN ({});",
            memory_columns("m."),
            placeholders(keys.len())
        );
        let mut result = Vec::new();
        let mut seen = HashSet::new();
        for hit in hits.iter().take(1000) {
            if !hit.similarity.is_finite()
                || !(-1.0..=1.0).contains(&hit.similarity)
                || hit.generation.is_empty()
                || seen.contains(&hit.chunk.owner_key)
            {
                continue;
            }
            let mut binds = vec![
                text(hit.generation.clone()),
                text(hit.chunk.owner_key.clone()),
                Sql::Integer(hit.chunk.owner_revision),
                text(hit.chunk.id.clone()),
                text(hit.chunk.content_hash.clone()),
                text(hit.chunk.text.clone()),
                Sql::Integer(hit.chunk.ordinal as i64),
                text(hit.chunk.source_uri.clone()),
                text(hit.chunk.source_span.clone()),
                text(hit.chunk.render_version.clone()),
            ];
            binds.extend(keys_sql(&keys));
            let raw = {
                let mut statement = self.conn.prepare_cached(&sql)?;
                let mut rows = statement.query(params_from_iter(binds))?;
                rows.next()
                    .map_err(|_| MemoryError::storage("unable to resolve dense memory"))?
                    .map(read_memory_row)
            };
            if let Some(raw) = raw {
                seen.insert(hit.chunk.owner_key.clone());
                result.push(DenseMemoryCandidate {
                    memory: self.finish_memory(raw)?,
                    hit: hit.clone(),
                });
            }
        }
        Ok(result)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn search_dense(
        &self,
        scope: &MemoryScope,
        generation: &str,
        query: &[f32],
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
        distinct_owners: bool,
    ) -> Result<Vec<DenseSearchHit>> {
        let keys = allowed_keys(scope, policy, include_global_user)?;
        let Some(config) = self.dense_index(scope, "", policy, include_global_user)? else {
            return Ok(Vec::new());
        };
        if config.generation != generation {
            return Ok(Vec::new());
        }
        let query_norm = vector_norm(query, config.dimensions)?;
        let limit = limit.min(1000);
        if limit == 0 {
            return Ok(Vec::new());
        }
        let sql = format!(
            "SELECT c.id,c.owner_key,c.owner_revision,c.ordinal,c.content,c.content_hash,c.source_uri,\
             c.source_span,c.render_version,v.dimensions,v.vector FROM retrieval_vectors v \
             JOIN retrieval_chunks c ON c.id=v.chunk_id JOIN retrieval_owners o ON o.owner_key=c.owner_key \
             JOIN retrieval_dense_indexes d ON d.scope_key=o.scope_key AND d.generation=v.generation \
             LEFT JOIN memories m ON o.kind='memory' AND m.id=o.owner_id \
             WHERE v.generation=? AND d.state='active' AND o.deleted=0 AND o.revision=c.owner_revision \
             AND o.scope_key IN ({}) AND (o.kind='code' OR m.status IN ('active','validated')) ORDER BY c.id;",
            placeholders(keys.len())
        );
        let mut binds = vec![text(generation)];
        binds.extend(keys_sql(&keys));
        let mut statement = self.conn.prepare(&sql)?;
        let mut rows = statement.query(params_from_iter(binds))?;
        let mut hits: Vec<DenseSearchHit> = Vec::new();
        while let Some(row) = rows
            .next()
            .map_err(|_| MemoryError::storage("unable to search dense index"))?
        {
            let dimensions = int_at(row, 9).max(0) as usize;
            if dimensions != config.dimensions {
                return storage("dense index contains mixed dimensions");
            }
            let bytes: Vec<u8> = row
                .get::<_, Vec<u8>>(10)
                .map_err(|_| MemoryError::storage("corrupt dense vector payload"))?;
            let vector = decode_vector(&bytes, dimensions)?;
            let norm = vector_norm(&vector, dimensions)?;
            let dot: f64 = query
                .iter()
                .zip(&vector)
                .map(|(left, right)| f64::from(*left) * f64::from(*right))
                .sum();
            let hit = DenseSearchHit {
                chunk: read_chunk(row),
                similarity: (dot / (query_norm * norm)).clamp(-1.0, 1.0),
                generation: generation.to_string(),
            };
            if distinct_owners
                && let Some(prior) = hits
                    .iter()
                    .position(|value| value.chunk.owner_key == hit.chunk.owner_key)
            {
                // Keep only each owner's best chunk so repeated text cannot
                // crowd out other owners.
                if ordered_hits(&hit, &hits[prior]) != std::cmp::Ordering::Less {
                    continue;
                }
                hits.remove(prior);
            }
            let position =
                hits.partition_point(|value| ordered_hits(value, &hit) == std::cmp::Ordering::Less);
            hits.insert(position, hit);
            if hits.len() > limit {
                hits.pop();
            }
        }
        Ok(hits)
    }

    pub(crate) fn sync_code_snapshot(
        &self,
        scope: &MemoryScope,
        snapshot: &CodeSnapshot,
        cancelled: Cancellation<'_>,
    ) -> Result<()> {
        validate_scope(scope)?;
        if snapshot.stale {
            return invalid("cannot publish stale code snapshot");
        }
        if snapshot.generation > i64::MAX as u64 {
            return invalid("code snapshot generation is out of range");
        }
        let tx = Tx::begin(self.conn, true)?;
        let token = make_id("code_sync_");
        let scope_key = key(scope)?;
        let nodes: HashMap<&str, &crate::code::CodeNode> = snapshot
            .nodes
            .iter()
            .map(|node| (node.id.as_str(), node))
            .collect();
        let mut seen = HashSet::new();
        for chunk in &snapshot.chunks {
            if cancelled() {
                return storage("code publication cancelled");
            }
            let Some(node) = nodes.get(chunk.owner_id.as_str()) else {
                continue;
            };
            if !seen.insert(chunk.owner_id.clone()) {
                continue;
            }
            if chunk.generation != snapshot.generation
                || chunk.source_fingerprint != node.source_fingerprint
            {
                return invalid("inconsistent code snapshot chunk");
            }
            let content = format!(
                "Path: {}\nSymbol: {}\n{}",
                node.path, node.qualified_name, chunk.text
            );
            if content.len() > MAXIMUM_DURABLE_TEXT {
                return invalid("code chunk exceeds indexing budget");
            }
            let owner = format!("code:{scope_key}:{}", node.id);
            let old: Option<(i64, String, i64)> = {
                let mut statement = self.conn.prepare_cached(
                    "SELECT revision,content,deleted FROM retrieval_owners WHERE owner_key=?;",
                )?;
                let mut rows = statement.query([&owner])?;
                rows.next()
                    .map_err(|_| MemoryError::storage("unable to inspect code owner"))?
                    .map(|row| (int_at(row, 0), text_at(row, 1), int_at(row, 2)))
            };
            let changed = old
                .as_ref()
                .is_none_or(|(_, prior, deleted)| prior != &content || *deleted != 0);
            let revision = match &old {
                Some((revision, _, _)) => revision + i64::from(changed),
                None => 1,
            };
            let source_uri = snapshot
                .root
                .join(&node.path)
                .to_string_lossy()
                .replace('\\', "/");
            self.conn
                .execute(
                    "INSERT INTO retrieval_owners(owner_key,scope_key,kind,owner_id,revision,content,source_uri,deleted) \
                     VALUES(?,?,'code',?,?,?,?,0) ON CONFLICT(owner_key) DO UPDATE SET revision=excluded.revision,\
                     content=excluded.content,source_uri=excluded.source_uri,deleted=0;",
                    params![owner, scope_key, node.id, revision, content, source_uri],
                )
                .map_err(|_| MemoryError::storage("unable to publish code owner"))?;
            self.conn
                .execute(
                    "INSERT INTO retrieval_code_sources VALUES(?,?,?,?,?,?,?) ON CONFLICT(owner_key) DO UPDATE SET \
                     repository=excluded.repository,worktree=excluded.worktree,path=excluded.path,\
                     fingerprint=excluded.fingerprint,snapshot_generation=excluded.snapshot_generation,sync_token=excluded.sync_token;",
                    params![
                        owner,
                        node.repository,
                        node.worktree,
                        node.path,
                        node.source_fingerprint,
                        snapshot.generation as i64,
                        token
                    ],
                )
                .map_err(|_| MemoryError::storage("unable to publish code source metadata"))?;
            if changed {
                self.conn
                    .execute("DELETE FROM retrieval_chunks WHERE owner_key=?;", [&owner])
                    .map_err(|_| MemoryError::storage("unable to invalidate code chunks"))?;
                self.conn
                    .execute(
                        "UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0 \
                         WHERE owner_key=? AND state IN ('pending','leased','failed');",
                        [&owner],
                    )
                    .map_err(|_| MemoryError::storage("unable to supersede code jobs"))?;
                self.conn
                    .execute(
                        "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) VALUES(?,?,'chunks','text-v1');",
                        params![owner, revision],
                    )
                    .map_err(|_| MemoryError::storage("unable to schedule code indexing"))?;
            }
        }
        self.conn
            .execute(
                "UPDATE retrieval_owners SET deleted=1,revision=revision+1,content='',source_uri='' \
                 WHERE kind='code' AND scope_key=? AND deleted=0 AND owner_key IN \
                 (SELECT owner_key FROM retrieval_code_sources WHERE sync_token<>?);",
                params![scope_key, token],
            )
            .map_err(|_| MemoryError::storage("unable to retire removed code"))?;
        self.conn
            .execute(
                "DELETE FROM retrieval_chunks WHERE owner_key IN \
                 (SELECT owner_key FROM retrieval_owners WHERE kind='code' AND scope_key=? AND deleted=1);",
                [&scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to invalidate removed code chunks"))?;
        self.conn
            .execute(
                "UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0 WHERE owner_key IN \
                 (SELECT owner_key FROM retrieval_owners WHERE kind='code' AND scope_key=? AND deleted=1);",
                [&scope_key],
            )
            .map_err(|_| MemoryError::storage("unable to retire removed code jobs"))?;
        if cancelled() {
            return storage("code publication cancelled");
        }
        tx.commit()
    }

    pub(crate) fn resolve_code_hits(
        &self,
        scope: &MemoryScope,
        hits: &[DenseSearchHit],
    ) -> Result<Vec<RetrievalCandidate>> {
        let scope_key = key(scope)?;
        let mut result: Vec<RetrievalCandidate> = Vec::new();
        let mut seen = HashSet::new();
        for hit in hits {
            if result.len() >= 1000 {
                break;
            }
            if seen.contains(&hit.chunk.owner_key) {
                continue;
            }
            let found = {
                let mut statement = self.conn.prepare_cached(
                    "SELECT o.owner_id,o.revision,s.repository,s.worktree,s.path,s.fingerprint,s.snapshot_generation \
                     FROM retrieval_owners o JOIN retrieval_code_sources s ON s.owner_key=o.owner_key \
                     JOIN retrieval_chunks c ON c.owner_key=o.owner_key AND c.owner_revision=o.revision \
                     WHERE o.owner_key=? AND o.revision=? AND o.scope_key=? AND o.kind='code' AND o.deleted=0 \
                     AND c.id=? AND c.content_hash=?;",
                )?;
                let mut rows = statement.query(params![
                    hit.chunk.owner_key,
                    hit.chunk.owner_revision,
                    scope_key,
                    hit.chunk.id,
                    hit.chunk.content_hash
                ])?;
                rows.next()
                    .map_err(|_| MemoryError::storage("unable to resolve code hit"))?
                    .map(|row| RetrievalReference {
                        kind: RetrievalOwnerKind::Code,
                        id: text_at(row, 0),
                        revision: int_at(row, 1),
                        repository: text_at(row, 2),
                        worktree: text_at(row, 3),
                        path: text_at(row, 4),
                        source_fingerprint: text_at(row, 5),
                        snapshot_generation: int_at(row, 6).max(0) as u64,
                    })
            };
            let Some(owner) = found else {
                continue;
            };
            seen.insert(hit.chunk.owner_key.clone());
            let rank = result.len() + 1;
            result.push(RetrievalCandidate {
                owner,
                matched_chunks: vec![hit.clone()],
                ranks: vec![RetrievalRank {
                    retriever: "dense".into(),
                    rank,
                    score: hit.similarity,
                }],
                ..Default::default()
            });
        }
        Ok(result)
    }

    fn validate_link_endpoint(
        &self,
        endpoint: &mut RetrievalReference,
        scope: &MemoryScope,
    ) -> Result<()> {
        if endpoint.id.is_empty() || endpoint.id.len() > MAXIMUM_LINK_TEXT {
            return invalid("retrieval link endpoint id is invalid");
        }
        if endpoint.revision < 0 || endpoint.snapshot_generation > i64::MAX as u64 {
            return invalid("retrieval link endpoint numeric pin is invalid");
        }
        if [
            &endpoint.repository,
            &endpoint.worktree,
            &endpoint.path,
            &endpoint.source_fingerprint,
        ]
        .iter()
        .any(|value| value.len() > MAXIMUM_LINK_TEXT)
        {
            return invalid("retrieval link endpoint metadata is too large");
        }
        match endpoint.kind {
            RetrievalOwnerKind::Memory => {
                let memory = self.get(scope, &endpoint.id)?;
                if key(&memory.scope)? != key(scope)? {
                    return invalid("memory link endpoint requires exact write scope");
                }
                let revision = self.retrieval_revision(scope, &endpoint.id)?;
                if endpoint.revision != 0 && endpoint.revision != revision {
                    return invalid("memory link endpoint revision is stale");
                }
                endpoint.revision = revision;
            }
            RetrievalOwnerKind::Code => {
                if endpoint.repository.is_empty()
                    || endpoint.worktree.is_empty()
                    || endpoint.source_fingerprint.is_empty()
                    || endpoint.snapshot_generation == 0
                {
                    return invalid(
                        "code link endpoint requires repository, worktree, fingerprint, and generation",
                    );
                }
            }
            RetrievalOwnerKind::Entity => {
                let visible = self
                    .conn
                    .prepare_cached("SELECT 1 FROM memory_entities WHERE id=? AND scope_key=?;")?
                    .exists(params![endpoint.id, key(scope)?])?;
                if !visible {
                    return invalid("entity link endpoint is not visible in exact scope");
                }
            }
        }
        Ok(())
    }

    pub(crate) fn add_cross_store_link(
        &self,
        scope: &MemoryScope,
        mut link: CrossStoreLink,
    ) -> Result<String> {
        validate_scope(scope)?;
        if link.id.len() > MAXIMUM_LINK_TEXT
            || link.relation.is_empty()
            || link.relation.len() > MAXIMUM_LINK_TEXT
            || link.evidence_id.is_empty()
            || link.evidence_id.len() > MAXIMUM_LINK_TEXT
            || !link.evidence_id.starts_with("uri:")
            || link.evidence_id.len() == 4
        {
            return invalid("cross-store link requires bounded relation and uri evidence");
        }
        let tx = Tx::begin(self.conn, true)?;
        self.validate_link_endpoint(&mut link.from, scope)?;
        self.validate_link_endpoint(&mut link.to, scope)?;
        if link.id.is_empty() {
            link.id = self
                .conn
                .query_row("SELECT lower(hex(randomblob(16)));", [], |row| {
                    Ok(text_at(row, 0))
                })
                .map_err(|_| MemoryError::storage("unable to generate retrieval link id"))?;
        }
        let mut binds = vec![
            text(link.id.clone()),
            text(key(scope)?),
            text(dump(&scope_json(scope))),
        ];
        for endpoint in [&link.from, &link.to] {
            binds.extend([
                text(endpoint.kind.as_str()),
                text(endpoint.id.clone()),
                Sql::Integer(endpoint.revision),
                text(endpoint.repository.clone()),
                text(endpoint.worktree.clone()),
                text(endpoint.path.clone()),
                text(endpoint.source_fingerprint.clone()),
                Sql::Integer(endpoint.snapshot_generation as i64),
            ]);
        }
        binds.extend([
            text(link.relation.clone()),
            text(link.evidence_id.clone()),
            Sql::Integer(now_ms() / 1000),
        ]);
        self.conn
            .execute(
                "INSERT INTO retrieval_cross_store_links(id,scope_key,scope_json,from_kind,from_id,from_revision,\
                 from_repository,from_worktree,from_path,from_fingerprint,from_generation,to_kind,to_id,to_revision,\
                 to_repository,to_worktree,to_path,to_fingerprint,to_generation,relation,evidence_id,created_at) \
                 VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                params_from_iter(binds),
            )
            .map_err(|_| MemoryError::storage("unable to persist cross-store link"))?;
        tx.commit()?;
        Ok(link.id)
    }

    pub(crate) fn links(
        &self,
        scope: &MemoryScope,
        endpoint: &RetrievalReference,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
    ) -> Result<Vec<CrossStoreLink>> {
        validate_scope(scope)?;
        if endpoint.id.is_empty() || limit == 0 {
            return Ok(Vec::new());
        }
        let mut keys: Vec<String> = Vec::new();
        for visible in recall_scopes(scope, policy, include_global_user)? {
            let encoded = key(&visible)?;
            if !keys.contains(&encoded) {
                keys.push(encoded);
            }
        }
        let sql = format!(
            "SELECT id,scope_json,from_kind,from_id,from_revision,from_repository,from_worktree,from_path,\
             from_fingerprint,from_generation,to_kind,to_id,to_revision,to_repository,to_worktree,to_path,\
             to_fingerprint,to_generation,relation,evidence_id FROM retrieval_cross_store_links WHERE scope_key IN ({}) \
             AND ((from_kind=? AND from_id=?) OR (to_kind=? AND to_id=?)) ORDER BY created_at,id LIMIT ?;",
            placeholders(keys.len())
        );
        let mut binds: Vec<Sql> = keys_sql(&keys).collect();
        for _ in 0..2 {
            binds.push(text(endpoint.kind.as_str()));
            binds.push(text(endpoint.id.clone()));
        }
        binds.push(Sql::Integer(limit.min(1024) as i64));
        let read_endpoint = |row: &Row<'_>, offset: usize| RetrievalReference {
            kind: RetrievalOwnerKind::parse(&text_at(row, offset)),
            id: text_at(row, offset + 1),
            revision: int_at(row, offset + 2),
            repository: text_at(row, offset + 3),
            worktree: text_at(row, offset + 4),
            path: text_at(row, offset + 5),
            source_fingerprint: text_at(row, offset + 6),
            snapshot_generation: int_at(row, offset + 7).max(0) as u64,
        };
        let rows: Vec<(
            String,
            String,
            RetrievalReference,
            RetrievalReference,
            String,
            String,
        )> = {
            let mut statement = self.conn.prepare(&sql)?;
            statement
                .query_map(params_from_iter(binds), |row| {
                    Ok((
                        text_at(row, 0),
                        text_at(row, 1),
                        read_endpoint(row, 2),
                        read_endpoint(row, 10),
                        text_at(row, 18),
                        text_at(row, 19),
                    ))
                })?
                .collect::<rusqlite::Result<_>>()
                .map_err(|_| MemoryError::storage("unable to read retrieval links"))?
        };
        let mut result = Vec::new();
        for (id, scope_text, from, to, relation, evidence_id) in rows {
            let stored_scope = parse_scope_json(&scope_text)?;
            if !scope_can_access(scope, &stored_scope, policy, include_global_user) {
                continue;
            }
            let matches = |reference: &RetrievalReference| {
                reference.kind == endpoint.kind && reference.id == endpoint.id
            };
            if !matches(&from) && !matches(&to) {
                continue;
            }
            let valid_memory = |reference: &RetrievalReference| -> bool {
                if reference.kind != RetrievalOwnerKind::Memory {
                    return true;
                }
                let Ok(Some(memory)) =
                    self.find_for_recall(scope, &reference.id, policy, include_global_user)
                else {
                    return false;
                };
                scope_can_access(scope, &memory.scope, policy, include_global_user)
                    && self
                        .retrieval_revision(&memory.scope, &reference.id)
                        .is_ok_and(|revision| revision == reference.revision)
            };
            if !valid_memory(&from) || !valid_memory(&to) {
                continue;
            }
            result.push(CrossStoreLink {
                from,
                to,
                relation,
                evidence_id,
                id,
            });
            if result.len() >= limit {
                break;
            }
        }
        Ok(result)
    }

    pub(crate) fn rebuild_retrieval(
        &self,
        scope: &MemoryScope,
        requested_owner: Option<&str>,
    ) -> Result<RetrievalMaintenanceReport> {
        validate_scope(scope)?;
        if requested_owner.is_some_and(str::is_empty) {
            return invalid("retrieval rebuild owner key cannot be empty");
        }
        let scope_key = key(scope)?;
        let tx = Tx::begin(self.conn, true)?;
        let owners: Vec<(String, i64)> = self
            .conn
            .prepare(
                "SELECT owner_key,revision FROM retrieval_owners WHERE scope_key=? AND deleted=0 \
                 AND (?2 IS NULL OR owner_key=?2) ORDER BY owner_key;",
            )?
            .query_map(params![scope_key, requested_owner], |row| {
                Ok((text_at(row, 0), int_at(row, 1)))
            })?
            .collect::<rusqlite::Result<_>>()
            .map_err(|_| MemoryError::storage("unable to enumerate retrieval rebuild owners"))?;
        if requested_owner.is_some() && owners.is_empty() {
            return invalid("retrieval rebuild owner is not current in the requested scope");
        }
        let generations: Vec<String> = self
            .conn
            .prepare(
                "SELECT generation FROM retrieval_dense_indexes WHERE scope_key=? AND state='active' ORDER BY generation;",
            )?
            .query_map([&scope_key], |row| Ok(text_at(row, 0)))?
            .collect::<rusqlite::Result<_>>()
            .map_err(|_| MemoryError::storage("unable to enumerate active dense generations"))?;
        let mut report = RetrievalMaintenanceReport {
            owners: owners.len(),
            ..Default::default()
        };
        for (owner, revision) in &owners {
            report.chunks_invalidated += self.conn.query_row(
                "SELECT COUNT(*) FROM retrieval_chunks WHERE owner_key=?;",
                [owner],
                |row| Ok(int_at(row, 0).max(0) as usize),
            )?;
            report.vectors_invalidated += self.conn.query_row(
                "SELECT COUNT(*) FROM retrieval_vectors v JOIN retrieval_chunks c ON c.id=v.chunk_id WHERE c.owner_key=?;",
                [owner],
                |row| Ok(int_at(row, 0).max(0) as usize),
            )?;
            self.conn.execute(
                "DELETE FROM retrieval_vectors WHERE chunk_id IN (SELECT id FROM retrieval_chunks WHERE owner_key=?);",
                [owner],
            )?;
            self.conn
                .execute("DELETE FROM retrieval_chunks WHERE owner_key=?;", [owner])?;
            report.jobs_superseded += self.conn.execute(
                "UPDATE retrieval_jobs SET state='superseded',lease_token='',lease_until=0 WHERE owner_key=? AND state<>'superseded';",
                [owner],
            )?;
            // Jobs are derived state keyed by revision; clear them so the same
            // revision can be enqueued again.
            self.conn
                .execute("DELETE FROM retrieval_jobs WHERE owner_key=?;", [owner])?;
            self.conn.execute(
                "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) VALUES(?,?,'chunks','text-v1');",
                params![owner, revision],
            )?;
            report.jobs_enqueued += 1;
            for generation in &generations {
                self.conn.execute(
                    "INSERT INTO retrieval_jobs(owner_key,owner_revision,representation,generation) VALUES(?,?,'dense',?);",
                    params![owner, revision, generation],
                )?;
                report.jobs_enqueued += 1;
            }
        }
        tx.commit()?;
        Ok(report)
    }
}

impl MemoryStore {
    /// Creates an entity in exactly `scope`; its name is always an alias.
    pub fn add_entity(&self, scope: &MemoryScope, entity: MemoryEntity) -> Result<MemoryEntity> {
        self.with(|db| db.add_entity(scope, entity))
    }

    /// Entities visible under `policy` whose id or alias equals `name`.
    pub fn find_entities(
        &self,
        scope: &MemoryScope,
        name: &str,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
    ) -> Result<Vec<MemoryEntity>> {
        self.with(|db| db.find_entities(scope, name, policy, include_global_user, limit))
    }

    /// Attaches a structured claim to the current revision of a memory in its
    /// exact scope.
    pub fn add_assertion(&self, scope: &MemoryScope, assertion: MemoryAssertion) -> Result<()> {
        self.with(|db| db.add_assertion(scope, assertion))
    }

    /// The assertion of a trusted memory: current revision, or the revision
    /// known at `as_of` (Unix ms).
    pub fn assertion(
        &self,
        scope: &MemoryScope,
        memory_id: &str,
        as_of: Option<Millis>,
    ) -> Result<Option<MemoryAssertion>> {
        self.with(|db| db.assertion(scope, memory_id, as_of))
    }

    #[allow(clippy::too_many_arguments)]
    pub fn query_assertions(
        &self,
        scope: &MemoryScope,
        subject_id: &str,
        predicate: &str,
        object_id: &str,
        as_of: Option<Millis>,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
    ) -> Result<Vec<MemoryAssertion>> {
        self.with(|db| {
            db.query_assertions(
                scope,
                subject_id,
                predicate,
                object_id,
                as_of,
                policy,
                include_global_user,
                limit,
            )
        })
    }

    /// Leases the next runnable indexing job of exactly `scope`.
    pub fn claim_index_job(
        &self,
        scope: &MemoryScope,
        lease_seconds: i64,
        dense_generation: Option<&str>,
    ) -> Result<Option<RetrievalIndexJob>> {
        self.with(|db| db.claim_index_job(scope, lease_seconds, dense_generation))
    }

    /// Replaces an owner's chunks under a valid lease; `false` when the lease
    /// or owner revision was superseded.
    pub fn publish_chunks(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        chunks: Vec<RetrievalChunk>,
    ) -> Result<bool> {
        self.with(|db| db.publish_chunks(scope, job, chunks))
    }

    pub fn fail_index_job(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        error: &str,
        max_attempts: usize,
    ) -> Result<bool> {
        self.with(|db| db.fail_index_job(scope, job, error, max_attempts))
    }

    pub fn renew_index_job(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        lease_seconds: i64,
    ) -> Result<bool> {
        self.with(|db| db.renew_index_job(scope, job, lease_seconds))
    }

    pub fn retrieval_index_status(&self, scope: &MemoryScope) -> Result<RetrievalIndexStatus> {
        self.with(|db| db.retrieval_index_status(scope))
    }

    /// Current chunks of one owner visible from `scope`.
    pub fn retrieval_chunks(
        &self,
        scope: &MemoryScope,
        owner_key: &str,
    ) -> Result<Vec<RetrievalChunk>> {
        self.with(|db| db.retrieval_chunks(scope, owner_key))
    }

    /// Registers a dense generation (`building`) and schedules its backfill.
    pub fn begin_dense_index(&self, scope: &MemoryScope, config: &DenseIndexConfig) -> Result<()> {
        self.with(|db| db.begin_dense_index(scope, config))
    }

    /// The nearest visible generation: the active one when `generation` is
    /// empty, otherwise that generation in any state.
    pub fn dense_index(
        &self,
        scope: &MemoryScope,
        generation: &str,
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<Option<DenseIndexConfig>> {
        self.with(|db| db.dense_index(scope, generation, policy, include_global_user))
    }

    /// Publishes precomputed vectors for every chunk of a leased dense job in
    /// one transaction. `next_vector` must not touch the store.
    pub fn publish_dense_vectors(
        &self,
        scope: &MemoryScope,
        job: &RetrievalIndexJob,
        vector_count: usize,
        next_vector: &mut dyn FnMut() -> Result<DenseChunkVector>,
    ) -> Result<bool> {
        self.with(|db| db.publish_dense_vectors(scope, job, vector_count, next_vector))
    }

    pub fn activate_dense_index(&self, scope: &MemoryScope, generation: &str) -> Result<bool> {
        self.with(|db| db.activate_dense_index(scope, generation))
    }

    /// Trusted memories whose captured chunk still matches each hit.
    pub fn resolve_dense_memory_candidates(
        &self,
        scope: &MemoryScope,
        hits: &[DenseSearchHit],
        policy: RecallPolicy,
        include_global_user: bool,
    ) -> Result<Vec<DenseMemoryCandidate>> {
        self.with(|db| db.resolve_dense_memory_candidates(scope, hits, policy, include_global_user))
    }

    #[allow(clippy::too_many_arguments)]
    pub fn search_dense(
        &self,
        scope: &MemoryScope,
        generation: &str,
        query: &[f32],
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
        distinct_owners: bool,
    ) -> Result<Vec<DenseSearchHit>> {
        self.with(|db| {
            db.search_dense(
                scope,
                generation,
                query,
                policy,
                include_global_user,
                limit,
                distinct_owners,
            )
        })
    }

    /// Publishes a code snapshot's owners into exactly `scope`, retiring owners
    /// that disappeared.
    pub fn sync_code_snapshot(
        &self,
        scope: &MemoryScope,
        snapshot: &CodeSnapshot,
        cancelled: Cancellation<'_>,
    ) -> Result<()> {
        self.with(|db| db.sync_code_snapshot(scope, snapshot, cancelled))
    }

    pub fn resolve_code_hits(
        &self,
        scope: &MemoryScope,
        hits: &[DenseSearchHit],
    ) -> Result<Vec<RetrievalCandidate>> {
        self.with(|db| db.resolve_code_hits(scope, hits))
    }

    /// Persists a relation between owners of different stores (`uri:` evidence).
    pub fn add_cross_store_link(
        &self,
        scope: &MemoryScope,
        link: CrossStoreLink,
    ) -> Result<String> {
        self.with(|db| db.add_cross_store_link(scope, link))
    }

    pub fn links(
        &self,
        scope: &MemoryScope,
        endpoint: &RetrievalReference,
        policy: RecallPolicy,
        include_global_user: bool,
        limit: usize,
    ) -> Result<Vec<CrossStoreLink>> {
        self.with(|db| db.links(scope, endpoint, policy, include_global_user, limit))
    }

    /// Invalidates derived chunks and vectors and re-enqueues indexing for the
    /// scope (or one owner).
    pub fn rebuild_retrieval(
        &self,
        scope: &MemoryScope,
        owner_key: Option<&str>,
    ) -> Result<RetrievalMaintenanceReport> {
        self.with(|db| db.rebuild_retrieval(scope, owner_key))
    }
}
