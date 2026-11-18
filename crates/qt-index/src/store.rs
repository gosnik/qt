//! SQLite snapshot store (`index.sqlite3`) with an FTS5 full-text table.
//!
//! The schema matches the archived index: `meta`, `project_files`,
//! `compilation_databases`, `build_file_counts`, `classes`, `methods`,
//! `functions`, `call_graph`, `skipped_code_files`, `scan_fingerprints` and the
//! `index_fts` FTS5 table (`path UNINDEXED, kind UNINDEXED, content`).
//! A snapshot is replaced wholesale in one transaction per scan.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use rusqlite::{Connection, OpenFlags, OptionalExtension, params};
use serde_json::{Value, json};

use crate::code_index::{
    CallGraphEdge, ClassSymbol, CodeIndex, FunctionSymbol, MethodSymbol, SkippedCodeIndexFile,
};
use crate::scan::{ProjectFile, ProjectFileKind, ProjectScanResult};
use crate::tree_sitter_meta::TreeSitterResult;
use crate::util::{generic, lexically_normal, path_text, weakly_canonical};

const SCHEMA_VERSION: i64 = 1;
const MAX_FTS_FILE_BYTES: u64 = 1024 * 1024;
const BUSY_TIMEOUT: Duration = Duration::from_secs(5);

/// A persisted scan generation.
#[derive(Debug, Clone, Default)]
pub struct SqliteIndexSnapshot {
    pub scan: ProjectScanResult,
    pub code_index: CodeIndex,
    pub generation: usize,
    pub compile_commands: usize,
    pub build_metadata_summary: String,
    pub last_scan_finished: Option<SystemTime>,
    pub tree_sitter: TreeSitterResult,
    pub fts_available: bool,
    pub scan_fingerprints: HashMap<String, String>,
    pub scan_fingerprints_complete: bool,
}

/// Everything written by [`save_snapshot`].
pub struct SnapshotData<'a> {
    pub scan: &'a ProjectScanResult,
    pub code_index: &'a CodeIndex,
    pub generation: usize,
    pub compile_commands: usize,
    pub build_metadata_summary: &'a str,
    pub tree_sitter: &'a TreeSitterResult,
    pub scan_fingerprints: &'a HashMap<String, String>,
}

pub fn database_path(state_directory: &Path) -> PathBuf {
    state_directory.join("index.sqlite3")
}

fn canonical_root_text(project_root: &Path) -> String {
    path_text(&weakly_canonical(project_root))
}

fn open(path: &Path, read_only: bool) -> rusqlite::Result<Connection> {
    let flags = if read_only {
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX
    } else {
        OpenFlags::SQLITE_OPEN_READ_WRITE
            | OpenFlags::SQLITE_OPEN_CREATE
            | OpenFlags::SQLITE_OPEN_NO_MUTEX
    };
    let connection = Connection::open_with_flags(path, flags)?;
    connection.busy_timeout(BUSY_TIMEOUT)?;
    Ok(connection)
}

fn create_schema(db: &Connection) -> rusqlite::Result<()> {
    db.execute_batch(
        "PRAGMA foreign_keys=ON;
         CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
         CREATE TABLE IF NOT EXISTS project_files(path TEXT PRIMARY KEY, kind TEXT NOT NULL);
         CREATE TABLE IF NOT EXISTS compilation_databases(path TEXT PRIMARY KEY);
         CREATE TABLE IF NOT EXISTS build_file_counts(type TEXT PRIMARY KEY, count INTEGER NOT NULL);
         CREATE TABLE IF NOT EXISTS classes(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, qualified_name TEXT NOT NULL, file TEXT NOT NULL, line INTEGER NOT NULL);
         CREATE TABLE IF NOT EXISTS methods(class_id INTEGER NOT NULL REFERENCES classes(id) ON DELETE CASCADE, name TEXT NOT NULL, qualified_name TEXT NOT NULL, file TEXT NOT NULL, line INTEGER NOT NULL);
         CREATE TABLE IF NOT EXISTS functions(name TEXT NOT NULL, qualified_name TEXT NOT NULL, file TEXT NOT NULL, line INTEGER NOT NULL, kind TEXT NOT NULL);
         CREATE TABLE IF NOT EXISTS call_graph(caller TEXT NOT NULL, callee TEXT NOT NULL, file TEXT NOT NULL, line INTEGER NOT NULL);
         CREATE TABLE IF NOT EXISTS skipped_code_files(file TEXT NOT NULL, reason TEXT NOT NULL);
         CREATE TABLE IF NOT EXISTS scan_fingerprints(path TEXT PRIMARY KEY, fingerprint TEXT NOT NULL);
         CREATE VIRTUAL TABLE IF NOT EXISTS index_fts USING fts5(path UNINDEXED, kind UNINDEXED, content);",
    )
}

fn get_meta(db: &Connection, key: &str) -> rusqlite::Result<String> {
    Ok(db
        .query_row("SELECT value FROM meta WHERE key=?1;", [key], |row| {
            row.get::<_, String>(0)
        })
        .optional()?
        .unwrap_or_default())
}

fn put_meta(db: &Connection, key: &str, value: &str) -> rusqlite::Result<()> {
    db.execute(
        "INSERT OR REPLACE INTO meta(key, value) VALUES(?1, ?2);",
        params![key, value],
    )?;
    Ok(())
}

fn table_exists(db: &Connection, table: &str) -> rusqlite::Result<bool> {
    Ok(db
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;",
            [table],
            |_| Ok(()),
        )
        .optional()?
        .is_some())
}

fn table_has_rows(db: &Connection, table: &str) -> rusqlite::Result<bool> {
    Ok(db
        .query_row(&format!("SELECT 1 FROM {table} LIMIT 1;"), [], |_| Ok(()))
        .optional()?
        .is_some())
}

/// Whether every path referenced by the scan and code index has a fingerprint.
pub fn complete_scan_fingerprints(
    scan: &ProjectScanResult,
    code_index: &CodeIndex,
    fingerprints: &HashMap<String, String>,
) -> bool {
    if fingerprints.is_empty() {
        return false;
    }
    let covered = |path: &Path| {
        path.as_os_str().is_empty()
            || fingerprints
                .get(&generic(path))
                .is_some_and(|f| !f.is_empty())
    };
    scan.files.iter().all(|f| covered(&f.path))
        && code_index
            .classes
            .iter()
            .all(|c| covered(&c.file) && c.methods.iter().all(|m| covered(&m.file)))
        && code_index.functions.iter().all(|f| covered(&f.file))
        && code_index.call_graph.iter().all(|e| covered(&e.file))
}

fn read_file_for_fts(path: &Path) -> String {
    match std::fs::metadata(path) {
        Ok(meta) if meta.len() <= MAX_FTS_FILE_BYTES => {}
        _ => return String::new(),
    }
    std::fs::read(path)
        .map(|b| String::from_utf8_lossy(&b).into_owned())
        .unwrap_or_default()
}

fn epoch_text(time: SystemTime) -> String {
    time.duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs().to_string())
        .unwrap_or_default()
}

fn time_from_epoch_text(value: &str) -> Option<SystemTime> {
    let seconds: u64 = value.trim().parse().ok()?;
    Some(UNIX_EPOCH + Duration::from_secs(seconds))
}

/// Load the persisted snapshot. `Ok(None)` when there is no database or it
/// belongs to another schema version or project root.
pub fn load_snapshot(
    database: &Path,
    project_root: &Path,
) -> Result<Option<SqliteIndexSnapshot>, String> {
    if !database.is_file() {
        return Ok(None);
    }
    load_snapshot_inner(database, project_root).map_err(|e| e.to_string())
}

fn load_snapshot_inner(
    database: &Path,
    project_root: &Path,
) -> rusqlite::Result<Option<SqliteIndexSnapshot>> {
    let mut db = open(database, true)?;
    let tx = db.transaction()?;
    if get_meta(&tx, "schema_version")? != SCHEMA_VERSION.to_string() {
        return Ok(None);
    }
    let canonical_root = canonical_root_text(project_root);
    if get_meta(&tx, "project_root")? != canonical_root {
        return Ok(None);
    }
    let parse = |text: String| text.trim().parse::<usize>().unwrap_or(0);
    let mut snapshot = SqliteIndexSnapshot {
        generation: parse(get_meta(&tx, "generation")?),
        compile_commands: parse(get_meta(&tx, "compile_commands")?),
        build_metadata_summary: get_meta(&tx, "build_metadata_summary")?,
        last_scan_finished: time_from_epoch_text(&get_meta(&tx, "last_scan_finished")?),
        tree_sitter: TreeSitterResult::from_json(&get_meta(&tx, "tree_sitter")?),
        fts_available: table_exists(&tx, "index_fts")? && table_has_rows(&tx, "index_fts")?,
        scan_fingerprints_complete: get_meta(&tx, "scan_fingerprints_complete")? == "1",
        ..Default::default()
    };
    snapshot.scan.root = PathBuf::from(&canonical_root);
    snapshot.scan.skipped_directories = parse(get_meta(&tx, "skipped_directories")?);

    if table_exists(&tx, "scan_fingerprints")? {
        let mut stmt =
            tx.prepare("SELECT path, fingerprint FROM scan_fingerprints ORDER BY path;")?;
        let rows = stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))?;
        for row in rows {
            let (path, fingerprint) = row?;
            snapshot.scan_fingerprints.insert(path, fingerprint);
        }
    }
    {
        let mut stmt = tx.prepare("SELECT path, kind FROM project_files ORDER BY path;")?;
        let rows = stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))?;
        for row in rows {
            let (path, kind) = row?;
            snapshot.scan.files.push(ProjectFile {
                path: PathBuf::from(path),
                kind: ProjectFileKind::parse(&kind),
            });
        }
    }
    {
        let mut stmt = tx.prepare("SELECT path FROM compilation_databases ORDER BY path;")?;
        let rows = stmt.query_map([], |r| r.get::<_, String>(0))?;
        for row in rows {
            snapshot
                .scan
                .existing_compilation_databases
                .push(PathBuf::from(row?));
        }
    }
    {
        let mut stmt = tx.prepare("SELECT type, count FROM build_file_counts ORDER BY type;")?;
        let rows = stmt.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?)))?;
        for row in rows {
            let (kind, count) = row?;
            snapshot
                .scan
                .build_files_by_type
                .insert(kind, count.max(0) as usize);
        }
    }
    {
        let mut classes =
            tx.prepare("SELECT id, name, qualified_name, file, line FROM classes ORDER BY id;")?;
        let mut methods =
            tx.prepare("SELECT name, qualified_name, file, line FROM methods WHERE class_id=?1 ORDER BY rowid;")?;
        let rows = classes.query_map([], |r| {
            Ok((
                r.get::<_, i64>(0)?,
                r.get::<_, String>(1)?,
                r.get::<_, String>(2)?,
                r.get::<_, String>(3)?,
                r.get::<_, i64>(4)?,
            ))
        })?;
        let class_rows: Vec<_> = rows.collect::<rusqlite::Result<_>>()?;
        for (id, name, qualified_name, file, line) in class_rows {
            let method_rows = methods.query_map([id], |r| {
                Ok(MethodSymbol {
                    name: r.get(0)?,
                    qualified_name: r.get(1)?,
                    file: PathBuf::from(r.get::<_, String>(2)?),
                    line: r.get::<_, i64>(3)?.max(0) as usize,
                })
            })?;
            snapshot.code_index.classes.push(ClassSymbol {
                name,
                qualified_name,
                file: PathBuf::from(file),
                line: line.max(0) as usize,
                methods: method_rows.collect::<rusqlite::Result<_>>()?,
            });
        }
    }
    {
        let mut stmt = tx.prepare(
            "SELECT name, qualified_name, file, line, kind FROM functions ORDER BY rowid;",
        )?;
        let rows = stmt.query_map([], |r| {
            Ok(FunctionSymbol {
                name: r.get(0)?,
                qualified_name: r.get(1)?,
                file: PathBuf::from(r.get::<_, String>(2)?),
                line: r.get::<_, i64>(3)?.max(0) as usize,
                kind: r.get(4)?,
            })
        })?;
        snapshot.code_index.functions = rows.collect::<rusqlite::Result<_>>()?;
    }
    {
        let mut stmt =
            tx.prepare("SELECT caller, callee, file, line FROM call_graph ORDER BY rowid;")?;
        let rows = stmt.query_map([], |r| {
            Ok(CallGraphEdge {
                caller: r.get(0)?,
                callee: r.get(1)?,
                file: PathBuf::from(r.get::<_, String>(2)?),
                line: r.get::<_, i64>(3)?.max(0) as usize,
            })
        })?;
        snapshot.code_index.call_graph = rows.collect::<rusqlite::Result<_>>()?;
    }
    {
        let mut stmt = tx.prepare("SELECT file, reason FROM skipped_code_files ORDER BY rowid;")?;
        let rows = stmt.query_map([], |r| {
            Ok(SkippedCodeIndexFile {
                file: PathBuf::from(r.get::<_, String>(0)?),
                reason: r.get(1)?,
            })
        })?;
        snapshot.code_index.skipped_files = rows.collect::<rusqlite::Result<_>>()?;
    }
    snapshot.scan_fingerprints_complete = snapshot.scan_fingerprints_complete
        && complete_scan_fingerprints(
            &snapshot.scan,
            &snapshot.code_index,
            &snapshot.scan_fingerprints,
        );
    tx.commit()?;
    Ok(Some(snapshot))
}

/// Replace the persisted snapshot with `data`.
pub fn save_snapshot(
    database: &Path,
    project_root: &Path,
    data: &SnapshotData<'_>,
) -> Result<(), String> {
    if let Some(parent) = database.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    save_inner(database, project_root, data).map_err(|e| e.to_string())
}

fn insert_fts(db: &Connection, path: &str, kind: &str, content: &str) -> rusqlite::Result<()> {
    if content.is_empty() {
        return Ok(());
    }
    db.prepare_cached("INSERT INTO index_fts(path, kind, content) VALUES(?1, ?2, ?3);")?
        .execute(params![path, kind, content])?;
    Ok(())
}

fn save_inner(
    database: &Path,
    project_root: &Path,
    data: &SnapshotData<'_>,
) -> rusqlite::Result<()> {
    let mut db = open(database, false)?;
    create_schema(&db)?;
    let tx = db.transaction_with_behavior(rusqlite::TransactionBehavior::Immediate)?;
    tx.execute_batch(
        "DELETE FROM meta; DELETE FROM project_files; DELETE FROM compilation_databases;
         DELETE FROM build_file_counts; DELETE FROM methods; DELETE FROM classes; DELETE FROM functions;
         DELETE FROM call_graph; DELETE FROM skipped_code_files; DELETE FROM scan_fingerprints;
         DELETE FROM index_fts;",
    )?;
    let scan = data.scan;
    let code_index = data.code_index;
    put_meta(&tx, "schema_version", &SCHEMA_VERSION.to_string())?;
    put_meta(&tx, "project_root", &canonical_root_text(project_root))?;
    put_meta(&tx, "generation", &data.generation.to_string())?;
    put_meta(&tx, "compile_commands", &data.compile_commands.to_string())?;
    put_meta(&tx, "build_metadata_summary", data.build_metadata_summary)?;
    put_meta(
        &tx,
        "skipped_directories",
        &scan.skipped_directories.to_string(),
    )?;
    put_meta(&tx, "last_scan_finished", &epoch_text(SystemTime::now()))?;
    put_meta(&tx, "tree_sitter", &data.tree_sitter.to_json().to_string())?;
    put_meta(
        &tx,
        "scan_fingerprints_complete",
        if complete_scan_fingerprints(scan, code_index, data.scan_fingerprints) {
            "1"
        } else {
            "0"
        },
    )?;
    {
        let mut stmt =
            tx.prepare("INSERT INTO scan_fingerprints(path, fingerprint) VALUES(?1, ?2);")?;
        for (path, fingerprint) in data.scan_fingerprints {
            stmt.execute(params![path, fingerprint])?;
        }
    }
    {
        let mut stmt = tx.prepare("INSERT INTO project_files(path, kind) VALUES(?1, ?2);")?;
        for file in &scan.files {
            let path = generic(&file.path);
            stmt.execute(params![path, file.kind.as_str()])?;
            if file.kind.is_searchable() {
                let content = read_file_for_fts(&lexically_normal(&scan.root.join(&file.path)));
                insert_fts(&tx, &path, "file", &format!("{path}\n{content}"))?;
            }
        }
    }
    {
        let mut stmt = tx.prepare("INSERT INTO compilation_databases(path) VALUES(?1);")?;
        for path in &scan.existing_compilation_databases {
            stmt.execute([generic(path)])?;
        }
    }
    {
        let mut stmt = tx.prepare("INSERT INTO build_file_counts(type, count) VALUES(?1, ?2);")?;
        for (kind, count) in &scan.build_files_by_type {
            stmt.execute(params![kind, *count as i64])?;
        }
    }
    {
        let mut class_stmt = tx.prepare(
            "INSERT INTO classes(name, qualified_name, file, line) VALUES(?1, ?2, ?3, ?4);",
        )?;
        let mut method_stmt = tx.prepare(
            "INSERT INTO methods(class_id, name, qualified_name, file, line) VALUES(?1, ?2, ?3, ?4, ?5);",
        )?;
        for class in &code_index.classes {
            let file = generic(&class.file);
            class_stmt.execute(params![
                class.name,
                class.qualified_name,
                file,
                class.line as i64
            ])?;
            let class_id = tx.last_insert_rowid();
            insert_fts(
                &tx,
                &file,
                "class",
                &format!("{} {}", class.name, class.qualified_name),
            )?;
            for method in &class.methods {
                method_stmt.execute(params![
                    class_id,
                    method.name,
                    method.qualified_name,
                    generic(&method.file),
                    method.line as i64
                ])?;
            }
        }
    }
    {
        let mut stmt = tx.prepare(
            "INSERT INTO functions(name, qualified_name, file, line, kind) VALUES(?1, ?2, ?3, ?4, ?5);",
        )?;
        for function in &code_index.functions {
            let file = generic(&function.file);
            stmt.execute(params![
                function.name,
                function.qualified_name,
                file,
                function.line as i64,
                function.kind
            ])?;
            let kind = if function.kind.is_empty() {
                "function"
            } else {
                function.kind.as_str()
            };
            insert_fts(
                &tx,
                &file,
                kind,
                &format!("{} {}", function.name, function.qualified_name),
            )?;
        }
    }
    {
        let mut stmt = tx.prepare(
            "INSERT INTO call_graph(caller, callee, file, line) VALUES(?1, ?2, ?3, ?4);",
        )?;
        for edge in &code_index.call_graph {
            stmt.execute(params![
                edge.caller,
                edge.callee,
                generic(&edge.file),
                edge.line as i64
            ])?;
        }
    }
    {
        let mut stmt =
            tx.prepare("INSERT INTO skipped_code_files(file, reason) VALUES(?1, ?2);")?;
        for skipped in &code_index.skipped_files {
            stmt.execute(params![generic(&skipped.file), skipped.reason])?;
        }
    }
    tx.commit()
}

/// Error from a full-text search.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FtsError {
    /// The caller supplied no query.
    EmptyQuery,
    /// The database is missing, stale or the query failed.
    Failed(String),
}

impl std::fmt::Display for FtsError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::EmptyQuery => f.write_str("fts search requires query"),
            Self::Failed(message) => f.write_str(message),
        }
    }
}

/// Run an FTS5 `MATCH` query ordered by rank. `limit == 0` means 100.
pub fn fts_search(
    database: &Path,
    project_root: &Path,
    query: &str,
    limit: usize,
) -> Result<Value, FtsError> {
    if query.is_empty() {
        return Err(FtsError::EmptyQuery);
    }
    let limit = if limit == 0 { 100 } else { limit };
    let run = || -> Result<Vec<Value>, FtsError> {
        let fail = |e: rusqlite::Error| FtsError::Failed(e.to_string());
        let db = open(database, false).map_err(fail)?;
        create_schema(&db).map_err(fail)?;
        if get_meta(&db, "schema_version").map_err(fail)? != SCHEMA_VERSION.to_string()
            || get_meta(&db, "project_root").map_err(fail)? != canonical_root_text(project_root)
        {
            return Err(FtsError::Failed(
                "sqlite index snapshot is not available for this project".into(),
            ));
        }
        let mut stmt = db
            .prepare(
                "SELECT path, kind, snippet(index_fts, 2, '[', ']', ' ... ', 12), rank \
                 FROM index_fts WHERE index_fts MATCH ?1 ORDER BY rank LIMIT ?2;",
            )
            .map_err(fail)?;
        let rows = stmt
            .query_map(params![query, limit as i64], |r| {
                Ok(json!({
                    "path": r.get::<_, String>(0)?,
                    "kind": r.get::<_, String>(1)?,
                    "snippet": r.get::<_, Option<String>>(2)?.unwrap_or_default(),
                    "rank": r.get::<_, f64>(3)?,
                }))
            })
            .map_err(fail)?;
        rows.collect::<rusqlite::Result<Vec<_>>>().map_err(fail)
    };
    let matches = run()?;
    Ok(json!({
        "query": query,
        "database": path_text(database),
        "matches": matches,
        "limit": limit,
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cancel::Pacer;
    use crate::code_index::build_code_index;
    use crate::scan::scan_project_tree;
    use std::fs;

    #[test]
    fn save_load_and_search_round_trip() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::write(
            root.join("src/engine.cpp"),
            "class Engine {\n  void ignite() { spark(); }\n};\n",
        )
        .unwrap();
        fs::write(
            root.join("CMakeLists.txt"),
            "add_executable(app src/engine.cpp)\n",
        )
        .unwrap();
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let code_index = build_code_index(&scan, &Pacer::unpaced()).unwrap();
        let mut fingerprints = HashMap::new();
        for file in &scan.files {
            fingerprints.insert(generic(&file.path), "fnv1a64:0".to_string());
        }
        let tree_sitter = TreeSitterResult {
            attempted: true,
            mode: "internal".into(),
            ..Default::default()
        };
        let database = root.join("state/index.sqlite3");
        let data = SnapshotData {
            scan: &scan,
            code_index: &code_index,
            generation: 3,
            compile_commands: 1,
            build_metadata_summary: "include_paths=1",
            tree_sitter: &tree_sitter,
            scan_fingerprints: &fingerprints,
        };
        save_snapshot(&database, &root, &data).unwrap();
        // Saving twice replaces the snapshot rather than appending.
        save_snapshot(&database, &root, &data).unwrap();

        let loaded = load_snapshot(&database, &root).unwrap().expect("snapshot");
        assert_eq!(loaded.generation, 3);
        assert_eq!(loaded.compile_commands, 1);
        assert_eq!(loaded.scan.files, scan.files);
        assert_eq!(loaded.code_index, code_index);
        assert_eq!(loaded.tree_sitter, tree_sitter);
        assert!(loaded.fts_available);
        assert!(loaded.scan_fingerprints_complete);
        assert!(loaded.last_scan_finished.is_some());
        assert_eq!(loaded.scan.build_files_by_type.get("CMake"), Some(&1));

        let other = tempfile::tempdir().unwrap();
        assert!(load_snapshot(&database, other.path()).unwrap().is_none());

        let result = fts_search(&database, &root, "\"ignite\"", 10).unwrap();
        let matches = result["matches"].as_array().unwrap();
        assert!(
            matches
                .iter()
                .any(|m| m["kind"] == "method" && m["path"] == "src/engine.cpp")
        );
        assert!(matches.iter().any(|m| m["kind"] == "file"));
        assert_eq!(result["limit"], 10);
        assert_eq!(
            fts_search(&database, &root, "", 1),
            Err(FtsError::EmptyQuery)
        );
        assert!(matches!(
            fts_search(&database, &root, "\"unbalanced", 1),
            Err(FtsError::Failed(_))
        ));
    }
}
