//! Explicit, read-only import of a standalone qtmem schema-v3 SQLite
//! database into the native export format (archived `qtmem_v3_import.cpp`).
//! The source is opened read-only inside one read transaction and is never
//! modified; the resulting state goes through [`MemoryStore::import_state`].
//!
//! [`MemoryStore::import_state`]: crate::MemoryStore::import_state

use std::collections::BTreeSet;
use std::io::Read as _;
use std::path::Path;

use rusqlite::{Connection, OpenFlags};
use serde_json::{Value, json};

use crate::error::{MemoryError, Result, invalid};
use crate::store::{int_at, real_at, text_at};
use crate::text::dump;

/// An adapted import: native-format `state` plus provenance notes.
#[derive(Debug, Clone, PartialEq)]
pub struct OfflineImportPackage {
    pub format: String,
    pub schema_version: usize,
    pub state: Value,
    pub source_scopes: Vec<String>,
    pub warnings: Vec<String>,
}

const BLOCK_IDS: [&str; 6] = [
    "user_profile",
    "project_profile",
    "repo_rules",
    "current_task",
    "agent_scratchpad",
    "active_constraints",
];
const MAXIMUM_BLOCK_BYTES: u64 = 256 * 1024;

/// True when the file starts with the SQLite header.
pub fn is_sqlite_database(path: &Path) -> bool {
    let mut header = [0u8; 16];
    std::fs::File::open(path)
        .and_then(|mut file| file.read_exact(&mut header))
        .is_ok()
        && &header == b"SQLite format 3\0"
}

fn table_exists(conn: &Connection, name: &str) -> Result<bool> {
    Ok(conn
        .prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;")?
        .exists([name])?)
}

fn rows(
    conn: &Connection,
    sql: &str,
    map: impl Fn(&rusqlite::Row<'_>) -> Value,
) -> Result<Vec<Value>> {
    let mut statement = conn
        .prepare(sql)
        .map_err(|error| MemoryError::storage(format!("qtmem import query failed: {error}")))?;
    let values = statement
        .query_map([], |row| Ok(map(row)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(values)
}

fn tags(conn: &Connection, sql: &str, id: &str) -> Result<Vec<String>> {
    let mut statement = conn.prepare_cached(sql)?;
    let values = statement
        .query_map([id], |row| Ok(text_at(row, 0)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    Ok(values)
}

fn scope_description(row: &rusqlite::Row<'_>, first: usize) -> String {
    dump(&json!({
        "user": text_at(row, first), "organization": text_at(row, first + 1),
        "project": text_at(row, first + 2), "repository": text_at(row, first + 3),
        "branch": text_at(row, first + 4), "task": text_at(row, first + 5),
        "session": text_at(row, first + 6), "agent": text_at(row, first + 7),
        "conversation": text_at(row, first + 8),
    }))
}

fn safe_path_component(value: &str) -> String {
    if value.is_empty() {
        return "default".into();
    }
    value
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
                ch
            } else {
                '_'
            }
        })
        .collect()
}

fn read_block(path: &Path, warnings: &mut Vec<String>) -> Result<Option<String>> {
    let name = path
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    let Ok(metadata) = std::fs::symlink_metadata(path) else {
        return Ok(None);
    };
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        warnings.push(format!("skipped non-regular qtmem block: {name}"));
        return Ok(None);
    }
    if metadata.len() > MAXIMUM_BLOCK_BYTES {
        warnings.push(format!("skipped oversized qtmem block: {name}"));
        return Ok(None);
    }
    let bytes = std::fs::read(path)
        .map_err(|_| MemoryError::storage(format!("unable to read qtmem block: {name}")))?;
    Ok(Some(String::from_utf8_lossy(&bytes).into_owned()))
}

/// Reads a qtmem v3 database without modifying it.
pub fn read_qtmem_v3(path: &Path) -> Result<OfflineImportPackage> {
    if !is_sqlite_database(path) {
        return invalid("qtmem import source is not a SQLite database");
    }
    let conn = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
    )
    .map_err(|error| {
        MemoryError::storage(format!("unable to open qtmem database read-only: {error}"))
    })?;
    if !conn.is_readonly(rusqlite::MAIN_DB)? {
        return Err(MemoryError::storage(
            "qtmem import source was not opened read-only",
        ));
    }
    if table_exists(&conn, "memory_schema_migrations")? {
        return invalid("import source is a native qt memory database, not standalone qtmem v3");
    }
    for table in [
        "meta",
        "memories",
        "memory_tags",
        "graph_edges",
        "procedure_runs",
        "memory_revisions",
        "sources",
        "source_events",
        "review_items",
        "review_item_tags",
        "audit_events",
        "session_events",
    ] {
        if !table_exists(&conn, table)? {
            return invalid(format!("qtmem v3 database is missing table: {table}"));
        }
    }
    let version: Option<String> = conn
        .prepare("SELECT value FROM meta WHERE key='schema_version' LIMIT 1;")?
        .query_map([], |row| Ok(text_at(row, 0)))?
        .next()
        .transpose()?;
    let Some(version) = version else {
        return invalid("qtmem database has no schema_version");
    };
    if version.parse::<usize>().ok() != Some(3) {
        return invalid("standalone qtmem import requires schema version 3");
    }
    conn.execute_batch("BEGIN;").map_err(|error| {
        MemoryError::storage(format!("qtmem import transaction failed: {error}"))
    })?;
    let result = read_tables(&conn, path);
    let _ = conn.execute_batch(if result.is_ok() {
        "COMMIT;"
    } else {
        "ROLLBACK;"
    });
    result
}

fn read_tables(conn: &Connection, path: &Path) -> Result<OfflineImportPackage> {
    let mut scopes = BTreeSet::new();
    let mut projects = BTreeSet::new();
    let mut memories = Vec::new();
    {
        let mut statement = conn.prepare(
            "SELECT id,user,organization,project,repository,branch,task,session,\
             agent_id,conversation_id,scope_json,memory_type,status,provenance_json,\
             source,source_type,source_uri,source_hash,source_span,created_by,content,\
             confidence,evidence_strength,quality_score,success_count,failure_count,\
             last_outcome,access_count,superseded_by,replacement_id,created_at,\
             updated_at,last_accessed_at FROM memories ORDER BY id;",
        )?;
        let mut cursor = statement.query([])?;
        while let Some(row) = cursor.next()? {
            let id = text_at(row, 0);
            scopes.insert(scope_description(row, 1));
            let project = text_at(row, 3);
            if !project.is_empty() {
                projects.insert(project);
            }
            memories.push(json!({
                "id": id, "user": text_at(row, 1), "organization": text_at(row, 2),
                "project": text_at(row, 3), "repository": text_at(row, 4), "branch": text_at(row, 5),
                "task": text_at(row, 6), "session": text_at(row, 7), "agent": text_at(row, 8),
                "conversation": text_at(row, 9), "scope_json": text_at(row, 10),
                "type": text_at(row, 11), "status": text_at(row, 12), "provenance_json": text_at(row, 13),
                "source": text_at(row, 14), "source_type": text_at(row, 15), "source_uri": text_at(row, 16),
                "source_hash": text_at(row, 17), "source_span": text_at(row, 18), "created_by": text_at(row, 19),
                "content": text_at(row, 20), "confidence": real_at(row, 21),
                "evidence_strength": real_at(row, 22), "quality_score": real_at(row, 23),
                "success_count": int_at(row, 24), "failure_count": int_at(row, 25),
                "last_outcome": text_at(row, 26), "access_count": int_at(row, 27),
                "superseded_by": text_at(row, 28), "replacement_id": text_at(row, 29),
                "created_at": int_at(row, 30), "updated_at": int_at(row, 31), "last_accessed_at": int_at(row, 32),
                "tags": tags(conn, "SELECT tag FROM memory_tags WHERE memory_id=CAST(? AS INTEGER) ORDER BY tag;", &id)?,
            }));
        }
    }
    let graph_edges = rows(
        conn,
        "SELECT id,from_id,to_id,type,confidence,source,created_at FROM graph_edges ORDER BY id;",
        |row| {
            json!({
                "id": text_at(row, 0), "from_id": text_at(row, 1), "to_id": text_at(row, 2), "type": text_at(row, 3),
                "confidence": real_at(row, 4), "source": text_at(row, 5), "created_at": int_at(row, 6),
            })
        },
    )?;
    let procedure_runs = rows(
        conn,
        "SELECT id,procedure_id,task_id,outcome,notes,evidence_uri,created_at FROM procedure_runs ORDER BY id;",
        |row| {
            json!({
                "id": text_at(row, 0), "procedure_id": text_at(row, 1), "task_id": text_at(row, 2),
                "outcome": text_at(row, 3), "notes": text_at(row, 4), "evidence_uri": text_at(row, 5),
                "created_at": int_at(row, 6),
            })
        },
    )?;
    let memory_revisions = rows(
        conn,
        "SELECT id,memory_id,revision_kind,entry_json,created_at FROM memory_revisions ORDER BY id;",
        |row| {
            json!({
                "id": text_at(row, 0), "memory_id": text_at(row, 1), "kind": text_at(row, 2),
                "entry_json": text_at(row, 3), "created_at": int_at(row, 4),
            })
        },
    )?;
    let mut sources = Vec::new();
    {
        let mut statement = conn.prepare(
            "SELECT id,project,source_type,source_uri,source_hash,display_name,\
             trust_level,hash_status,ingest_count,imported_at,last_ingested_at,\
             last_checked_at FROM sources ORDER BY id;",
        )?;
        let mut cursor = statement.query([])?;
        while let Some(row) = cursor.next()? {
            let project = text_at(row, 1);
            if !project.is_empty() {
                projects.insert(project);
            }
            sources.push(json!({
                "id": text_at(row, 0), "project": text_at(row, 1), "type": text_at(row, 2),
                "uri": text_at(row, 3), "hash": text_at(row, 4), "display_name": text_at(row, 5),
                "trust_level": text_at(row, 6), "hash_status": text_at(row, 7),
                "ingest_count": int_at(row, 8), "imported_at": int_at(row, 9),
                "last_ingested_at": int_at(row, 10), "last_checked_at": int_at(row, 11),
            }));
        }
    }
    let source_events = rows(
        conn,
        "SELECT id,source_id,project,action,status,source_hash,note,created_at FROM source_events ORDER BY id;",
        |row| {
            json!({
                "id": text_at(row, 0), "source_id": text_at(row, 1), "project": text_at(row, 2),
                "action": text_at(row, 3), "status": text_at(row, 4), "source_hash": text_at(row, 5),
                "note": text_at(row, 6), "created_at": int_at(row, 7),
            })
        },
    )?;
    let mut review_items = Vec::new();
    {
        let mut statement = conn.prepare(
            "SELECT id,user,organization,project,repository,branch,task,session,\
             agent_id,conversation_id,scope_json,kind,status,priority,reason,\
             subject_id,source,source_type,source_uri,source_hash,provenance_json,\
             candidate_json,decision,decided_by,decision_note,created_at,updated_at,\
             decided_at FROM review_items ORDER BY id;",
        )?;
        let mut cursor = statement.query([])?;
        while let Some(row) = cursor.next()? {
            let id = text_at(row, 0);
            scopes.insert(scope_description(row, 1));
            let project = text_at(row, 3);
            if !project.is_empty() {
                projects.insert(project);
            }
            review_items.push(json!({
                "id": id, "kind": text_at(row, 11), "status": text_at(row, 12), "priority": text_at(row, 13),
                "reason": text_at(row, 14), "subject_id": text_at(row, 15), "source": text_at(row, 16),
                "source_type": text_at(row, 17), "source_uri": text_at(row, 18), "source_hash": text_at(row, 19),
                "provenance_json": text_at(row, 20), "candidate_json": text_at(row, 21),
                "decision": text_at(row, 22), "decided_by": text_at(row, 23), "decision_note": text_at(row, 24),
                "created_at": int_at(row, 25), "updated_at": int_at(row, 26), "decided_at": int_at(row, 27),
                "tags": tags(conn, "SELECT tag FROM review_item_tags WHERE review_item_id=CAST(? AS INTEGER) ORDER BY tag;", &id)?,
            }));
        }
    }
    let mut audit_events = Vec::new();
    {
        let mut statement = conn.prepare(
            "SELECT id,actor,action,user,organization,project,repository,branch,\
             task,session,agent_id,conversation_id,scope_json,reason,before_id,\
             after_id,evidence_uri,metadata_json,created_at FROM audit_events ORDER BY id;",
        )?;
        let mut cursor = statement.query([])?;
        while let Some(row) = cursor.next()? {
            scopes.insert(scope_description(row, 3));
            audit_events.push(json!({
                "id": text_at(row, 0), "actor": text_at(row, 1), "action": text_at(row, 2),
                "reason": text_at(row, 13), "before_id": text_at(row, 14), "after_id": text_at(row, 15),
                "evidence_uri": text_at(row, 16), "metadata_json": text_at(row, 17), "created_at": int_at(row, 18),
            }));
        }
    }
    let mut session_events = Vec::new();
    {
        let mut statement = conn.prepare(
            "SELECT id,user,organization,project,repository,branch,task,session,\
             agent_id,conversation_id,scope_json,event_type,source,payload_json,\
             created_at FROM session_events ORDER BY id;",
        )?;
        let mut cursor = statement.query([])?;
        while let Some(row) = cursor.next()? {
            scopes.insert(scope_description(row, 1));
            session_events.push(json!({
                "id": text_at(row, 0), "event_type": text_at(row, 11), "source": text_at(row, 12),
                "payload_json": text_at(row, 13), "created_at": int_at(row, 14),
            }));
        }
    }
    let mut warnings = Vec::new();
    let source_scopes: Vec<String> = scopes.into_iter().collect();
    if source_scopes.len() > 1 {
        warnings.push("multiple qtmem scopes will be mapped into the selected qt scope".into());
    }
    let mut block_directories: BTreeSet<String> = projects
        .iter()
        .map(|project| safe_path_component(project))
        .collect();
    let blocks_root = path
        .parent()
        .unwrap_or_else(|| Path::new("."))
        .join("blocks");
    if block_directories.is_empty()
        && let Ok(entries) = std::fs::read_dir(&blocks_root)
    {
        for entry in entries.flatten() {
            if entry
                .file_type()
                .is_ok_and(|kind| kind.is_dir() && !kind.is_symlink())
            {
                block_directories.insert(entry.file_name().to_string_lossy().into_owned());
            }
        }
    }
    let mut blocks = Vec::new();
    if block_directories.len() == 1 {
        let directory = blocks_root.join(
            block_directories
                .iter()
                .next()
                .map(String::as_str)
                .unwrap_or_default(),
        );
        for block_id in BLOCK_IDS {
            if let Some(content) =
                read_block(&directory.join(format!("{block_id}.md")), &mut warnings)?
            {
                blocks.push(json!({"id": block_id, "content": content}));
            }
        }
    } else if block_directories.len() > 1 {
        warnings.push(
            "qtmem file blocks were not imported because multiple source projects map into one selected qt scope".into(),
        );
    }
    Ok(OfflineImportPackage {
        format: "qtmem-sqlite-v3".into(),
        schema_version: 3,
        state: json!({
            "schema_version": 3, "memories": memories, "graph_edges": graph_edges,
            "procedure_runs": procedure_runs, "memory_revisions": memory_revisions,
            "sources": sources, "source_events": source_events, "review_items": review_items,
            "audit_events": audit_events, "session_events": session_events, "blocks": blocks,
        }),
        source_scopes,
        warnings,
    })
}
