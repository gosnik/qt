//! Compatibility with databases created by the archived C++ binary. The
//! migration SQL is extracted from `archived/src/memory/storage/
//! sqlite_memory_database.cpp`, replayed into a fresh file exactly as the C++
//! initialiser does, populated with rows the C++ binary would write, and
//! then opened through `MemoryStore`.

mod common;

use std::path::PathBuf;

use common::*;
use qt_memory::store::schema::MIGRATIONS;
use qt_memory::*;
use rusqlite::{Connection, params};

const MIGRATION_FUNCTIONS: [&str; 7] = [
    "migration_one",
    "migration_two",
    "migration_three",
    "migration_four",
    "migration_five",
    "migration_six",
    "migration_seven",
];

fn archived_source() -> String {
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../archived/src/memory/storage/sqlite_memory_database.cpp");
    std::fs::read_to_string(&path)
        .unwrap_or_else(|error| panic!("read {}: {error}", path.display()))
}

/// Concatenates the C++ string literals (plain and `R"SQL(...)SQL"`) in a
/// function body, skipping `//` comments.
fn literals(body: &str) -> String {
    let bytes = body.as_bytes();
    let mut out = String::new();
    let mut index = 0;
    while index < bytes.len() {
        if body[index..].starts_with("//") {
            index += body[index..].find('\n').unwrap_or(body.len() - index);
        } else if body[index..].starts_with("R\"SQL(") {
            let start = index + 6;
            let end = start + body[start..].find(")SQL\"").expect("raw literal end");
            out.push_str(&body[start..end]);
            index = end + 5;
        } else if bytes[index] == b'"' {
            index += 1;
            while bytes[index] != b'"' {
                if bytes[index] == b'\\' {
                    index += 1;
                    out.push(match bytes[index] {
                        b'n' => '\n',
                        b't' => '\t',
                        other => other as char,
                    });
                } else {
                    out.push(bytes[index] as char);
                }
                index += 1;
            }
            index += 1;
        } else {
            index += 1;
        }
    }
    out
}

fn archived_migrations() -> Vec<String> {
    let source = archived_source();
    MIGRATION_FUNCTIONS
        .iter()
        .map(|name| {
            let start = source
                .find(&format!("void {name}(sqlite3* db) {{"))
                .expect("migration function");
            let end = start + source[start..].find("\n}\n").expect("function end");
            literals(&source[start..end])
        })
        .collect()
}

/// Replays migrations `1..=upto` the way the archived initialiser does.
fn create_archived_database(agent_home: &std::path::Path, upto: usize) -> Connection {
    let directory = agent_home.join("memory");
    std::fs::create_dir_all(&directory).unwrap();
    let conn = Connection::open(directory.join("memory.sqlite3")).unwrap();
    conn.execute_batch("PRAGMA foreign_keys=ON;PRAGMA journal_mode=WAL;")
        .unwrap();
    conn.execute_batch(
        "CREATE TABLE IF NOT EXISTS memory_schema_migrations(version INTEGER PRIMARY KEY,applied_at INTEGER NOT NULL);",
    )
    .unwrap();
    for (index, sql) in archived_migrations().iter().take(upto).enumerate() {
        conn.execute_batch(&format!("BEGIN IMMEDIATE;{sql}"))
            .unwrap();
        conn.execute(
            "INSERT INTO memory_schema_migrations VALUES(?,?);",
            params![index as i64 + 1, 1_700_000_000_000i64],
        )
        .unwrap();
        conn.execute_batch("COMMIT;").unwrap();
    }
    conn
}

fn insert_cpp_memory(conn: &Connection, id: &str, scope: &MemoryScope, content: &str) {
    let key = scope_key(scope).unwrap();
    conn.execute(
        "INSERT INTO memories VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        params![
            id, key, "exact", scope.user, "", scope.project, scope.repository, scope.repository_display, scope.branch,
            "", "", scope.session, "", "decision", "hypothesis", "cpp", "", "file", "file:cpp", "h1", "", "agent", 0.6,
            content, 0.5, 0.0, 0, 0, "unknown", 0, "", "", 1_700_000_000_000i64, 1_700_000_000_000i64, 0
        ],
    )
    .unwrap();
    conn.execute(
        "INSERT INTO memory_tags VALUES(?,?);",
        params![id, "type:decision"],
    )
    .unwrap();
    conn.execute(
        "INSERT INTO memory_fts(memory_id,content,source,memory_type,source_uri) VALUES(?,?,?,?,?);",
        params![id, content, "cpp", "decision", "file:cpp"],
    )
    .unwrap();
}

fn schema_objects(conn: &Connection) -> Vec<(String, String, String)> {
    conn.prepare("SELECT type,name,COALESCE(sql,'') FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name;")
        .unwrap()
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap()
}

#[test]
fn migration_sql_is_byte_identical_to_archive() {
    let archived = archived_migrations();
    assert_eq!(archived.len(), MIGRATIONS.len());
    for (index, (ours, theirs)) in MIGRATIONS.iter().zip(&archived).enumerate() {
        assert_eq!(
            *ours,
            theirs,
            "migration {} differs from the archived SQL",
            index + 1
        );
    }
    assert_eq!(SCHEMA_VERSION, 7);
}

#[test]
fn fresh_store_schema_matches_archived_replay() {
    let archived_home = tempfile::tempdir().unwrap();
    let archived = create_archived_database(archived_home.path(), 7);
    let fixture = Fixture::new();
    assert_eq!(schema_objects(&fixture.raw()), schema_objects(&archived));
}

#[test]
fn opens_database_written_by_archived_binary() {
    let home = tempfile::tempdir().unwrap();
    let scope = scope();
    {
        let conn = create_archived_database(home.path(), 7);
        insert_cpp_memory(
            &conn,
            "mem_cpp1",
            &scope,
            "We decided to keep the C++ schema",
        );
        conn.execute(
            "INSERT INTO review_items(id,scope_key,kind,status,priority,reason,subject_id,source_id,source_type,source_uri,\
             source_hash,source_span,created_by,evidence_strength,candidate_json,decision,decided_by,decision_note,tags_json,\
             created_at,updated_at,decided_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
            params![
                "review_cpp1", scope_key(&scope).unwrap(), "candidate_memory", "pending", "normal", "check",
                "mem_cpp1", "", "file", "file:cpp", "", "", "agent", 0.6,
                r#"{"kind":"general","payload":{"content":"x","memory_ids":[],"reason":"check","tags":[]},"version":1}"#,
                "", "", "", "[]", 1_700_000_000_000i64, 1_700_000_000_000i64, 0
            ],
        )
        .unwrap();
    }
    let store = MemoryStore::open(home.path()).unwrap();
    assert_eq!(store.schema_version(), 7);
    let memory = store.get(&scope, "mem_cpp1").unwrap();
    assert_eq!(memory.memory_type, MemoryType::Decision);
    assert_eq!(memory.tags, ["type:decision"]);
    assert_eq!(memory.created_at, 1_700_000_000_000);
    let found = store
        .query(
            &scope,
            &MemoryQuery {
                text: "schema".into(),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(found.len(), 1);
    // The archived triggers created the derived-index owner on insert.
    assert_eq!(store.retrieval_revision(&scope, "mem_cpp1").unwrap(), 1);
    store
        .update(
            &scope,
            "mem_cpp1",
            &MemoryPatch {
                content: Some("revised".into()),
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(store.retrieval_revision(&scope, "mem_cpp1").unwrap(), 2);
    let review = store.review(&scope, "review_cpp1").unwrap();
    assert_eq!(review.revision, 0);
    assert_eq!(review.application_status, "not_started");
    assert_eq!(store.integrity_check().unwrap(), "ok");
}

#[test]
fn upgrades_partially_migrated_archived_database() {
    let home = tempfile::tempdir().unwrap();
    let scope = scope();
    {
        let conn = create_archived_database(home.path(), 3);
        insert_cpp_memory(&conn, "mem_old", &scope, "procedure: legacy steps");
    }
    let store = MemoryStore::open(home.path()).unwrap();
    assert_eq!(store.schema_version(), 7);
    // Migration 6 back-fills owners, jobs and assertion boundaries.
    assert_eq!(store.retrieval_revision(&scope, "mem_old").unwrap(), 1);
    let conn = Connection::open(store.path()).unwrap();
    let jobs: i64 = conn
        .query_row("SELECT COUNT(*) FROM retrieval_jobs WHERE owner_key='memory:mem_old' AND max_attempts=5;", [], |row| row.get(0))
        .unwrap();
    assert_eq!(jobs, 1);
    let versions: Vec<i64> = conn
        .prepare("SELECT version FROM memory_schema_migrations ORDER BY version;")
        .unwrap()
        .query_map([], |row| row.get(0))
        .unwrap()
        .collect::<rusqlite::Result<_>>()
        .unwrap();
    assert_eq!(versions, [1, 2, 3, 4, 5, 6, 7]);
}

#[test]
fn refuses_legacy_qtmem_and_future_schemas() {
    let home = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(home.path().join("memory")).unwrap();
    let conn = Connection::open(home.path().join("memory/memory.sqlite3")).unwrap();
    conn.execute_batch("CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT);INSERT INTO meta VALUES('schema_version','3');")
        .unwrap();
    drop(conn);
    assert_eq!(
        MemoryStore::open(home.path()).unwrap_err().to_string(),
        "standalone qtmem database requires explicit offline import"
    );

    let future = tempfile::tempdir().unwrap();
    drop(create_archived_database(future.path(), 7));
    let conn = Connection::open(future.path().join("memory/memory.sqlite3")).unwrap();
    conn.execute("INSERT INTO memory_schema_migrations VALUES(8,0);", [])
        .unwrap();
    drop(conn);
    assert_eq!(
        MemoryStore::open(future.path()).unwrap_err().to_string(),
        "unsupported memory schema version 8"
    );
}

#[test]
fn store_layout_pragmas_and_permissions() {
    let fixture = Fixture::new();
    let root = fixture.dir.path().join("memory");
    assert!(root.join("blocks").is_dir() && root.join("exports").is_dir());
    assert_eq!(fixture.store.path(), root.join("memory.sqlite3"));
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        let mode =
            |path: &std::path::Path| std::fs::metadata(path).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(&root), 0o700);
        assert_eq!(mode(&root.join("blocks")), 0o700);
        assert_eq!(mode(&root.join("exports")), 0o700);
        assert_eq!(mode(fixture.store.path()), 0o600);
    }
    let conn = fixture.raw();
    let journal: String = conn
        .query_row("PRAGMA journal_mode;", [], |row| row.get(0))
        .unwrap();
    assert_eq!(journal, "wal");
    // Re-opening is idempotent and applies no further migrations.
    let reopened = MemoryStore::open(fixture.dir.path()).unwrap();
    assert_eq!(reopened.schema_version(), 7);
    let count: i64 = conn
        .query_row(
            "SELECT COUNT(*) FROM memory_schema_migrations;",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(count, 7);
}
