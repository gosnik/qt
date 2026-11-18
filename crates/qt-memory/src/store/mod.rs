//! SQLite memory store (archived `SQLiteMemoryDatabase`).
//!
//! The database lives at `<agent_home>/memory/memory.sqlite3` next to the
//! `blocks/` and `exports/` directories. Directories are created owner-only
//! (0700) and the database file is 0600. The connection runs with foreign
//! keys, WAL journaling, `synchronous=FULL` and a jittered busy-retry
//! handler. All access is serialised through one connection mutex; nested
//! operations use savepoints, so composite operations commit atomically.

mod maintenance;
mod memories;
mod reviews;
pub mod schema;
mod state;

use std::ffi::{c_int, c_void};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

use rusqlite::types::ValueRef;
use rusqlite::{Connection, OpenFlags, Row, params};

use crate::error::{MemoryError, Result, storage};
use crate::model::{MemoryEntry, MemoryScope, ProcedureOutcome, ScopeKind, SourceType, now_ms};
use crate::model::{MemoryStatus, MemoryType};
use crate::text::{MAXIMUM_DURABLE_TEXT, MAXIMUM_DURABLE_URI, sanitize_meta, sanitize_text};

pub use schema::SCHEMA_VERSION;

/// Busy-retry policy (archived `DatabaseOptions`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DatabaseOptions {
    /// Upper bound on one retry sleep.
    pub busy_timeout: Duration,
    /// Retries before `SQLITE_BUSY` is returned to the caller.
    pub busy_retries: usize,
    /// Linear backoff step; jitter is up to one step.
    pub retry_base_delay: Duration,
}

impl Default for DatabaseOptions {
    fn default() -> Self {
        Self {
            busy_timeout: Duration::from_millis(3000),
            busy_retries: 6,
            retry_base_delay: Duration::from_millis(4),
        }
    }
}

/// Governed memory database.
pub struct MemoryStore {
    // Field order matters: the connection must drop before the busy-handler
    // options it points to.
    conn: Mutex<Connection>,
    busy_options: Box<DatabaseOptions>,
    root: PathBuf,
    path: PathBuf,
    schema_version: usize,
}

impl std::fmt::Debug for MemoryStore {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MemoryStore")
            .field("path", &self.path)
            .field("schema_version", &self.schema_version)
            .finish_non_exhaustive()
    }
}

/// Borrowed connection used by the storage operations. Every public
/// [`MemoryStore`] method locks the mutex once and runs on a `Db`.
pub(crate) struct Db<'a> {
    pub(crate) conn: &'a Connection,
}

impl MemoryStore {
    /// Opens (creating and migrating if needed) `<agent_home>/memory/memory.sqlite3`.
    pub fn open(agent_home: impl AsRef<Path>) -> Result<Self> {
        Self::open_with_options(agent_home, DatabaseOptions::default())
    }

    pub fn open_with_options(
        agent_home: impl AsRef<Path>,
        options: DatabaseOptions,
    ) -> Result<Self> {
        let root = agent_home.as_ref().join("memory");
        let path = root.join("memory.sqlite3");
        secure_directory(&root)?;
        secure_directory(&root.join("blocks"))?;
        secure_directory(&root.join("exports"))?;
        let conn = Connection::open_with_flags(
            &path,
            OpenFlags::SQLITE_OPEN_READ_WRITE
                | OpenFlags::SQLITE_OPEN_CREATE
                | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .map_err(|error| {
            MemoryError::storage(format!("unable to open memory database: {error}"))
        })?;
        secure_file(&path)?;
        let busy_options = Box::new(options);
        conn.busy_timeout(options.busy_timeout)?;
        exec(&conn, "PRAGMA foreign_keys=ON;")?;
        let has_migrations = table_exists(&conn, "memory_schema_migrations")?;
        let has_legacy_meta = table_exists(&conn, "meta")?;
        let mut schema_version = current_version(&conn)?;
        if schema_version > SCHEMA_VERSION {
            return storage(format!(
                "unsupported memory schema version {schema_version}"
            ));
        }
        if !has_migrations && has_legacy_meta {
            return storage("standalone qtmem database requires explicit offline import");
        }
        install_busy_handler(&conn, &busy_options)?;
        let _: String = conn.query_row("PRAGMA journal_mode=WAL;", [], |row| row.get(0))?;
        exec(&conn, "PRAGMA synchronous=FULL;")?;
        if !table_exists(&conn, "memory_schema_migrations")? {
            exec(&conn, schema::MIGRATION_TABLE)?;
        }
        for version in schema_version + 1..=SCHEMA_VERSION {
            let tx = Tx::begin(&conn, true)?;
            let locked_version = current_version(&conn)?;
            if locked_version >= version {
                tx.commit()?;
                schema_version = locked_version;
                continue;
            }
            exec(&conn, schema::MIGRATIONS[version - 1])?;
            conn.execute(
                "INSERT INTO memory_schema_migrations(version,applied_at) VALUES(?,?);",
                params![version as i64, now_ms()],
            )
            .map_err(|_| MemoryError::storage("unable to record memory migration"))?;
            tx.commit()?;
            schema_version = version;
        }
        Ok(Self {
            conn: Mutex::new(conn),
            busy_options,
            root,
            path,
            schema_version,
        })
    }

    /// `<agent_home>/memory`.
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// `<agent_home>/memory/memory.sqlite3`.
    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn schema_version(&self) -> usize {
        self.schema_version
    }

    pub fn options(&self) -> DatabaseOptions {
        *self.busy_options
    }

    pub(crate) fn lock(&self) -> MutexGuard<'_, Connection> {
        self.conn
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    pub(crate) fn with<T>(&self, operation: impl FnOnce(&Db<'_>) -> Result<T>) -> Result<T> {
        let guard = self.lock();
        operation(&Db { conn: &guard })
    }

    /// `PRAGMA quick_check` result (`"ok"` when healthy).
    pub fn integrity_check(&self) -> Result<String> {
        self.with(|db| {
            db.conn
                .query_row("PRAGMA quick_check;", [], |row| row.get::<_, String>(0))
                .map_err(|_| {
                    MemoryError::storage("memory database integrity check returned no result")
                })
        })
    }
}

fn secure_directory(path: &Path) -> Result<()> {
    std::fs::create_dir_all(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700)).map_err(|_| {
            MemoryError::storage(format!(
                "unable to secure memory directory: {}",
                path.display()
            ))
        })?;
    }
    Ok(())
}

fn secure_file(path: &Path) -> Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600)).map_err(|_| {
            MemoryError::storage(format!(
                "unable to secure memory database: {}",
                path.display()
            ))
        })?;
    }
    #[cfg(not(unix))]
    let _ = path;
    Ok(())
}

unsafe extern "C" fn busy_retry(context: *mut c_void, prior_attempts: c_int) -> c_int {
    // SAFETY: `context` is the boxed `DatabaseOptions` owned by the store,
    // which outlives the connection that invokes this handler.
    let options = unsafe { &*(context as *const DatabaseOptions) };
    let attempts = usize::try_from(prior_attempts).unwrap_or(0);
    if attempts >= options.busy_retries {
        return 0;
    }
    let base_ms = options.retry_base_delay.as_millis() as u64;
    let cap = (options.busy_timeout.as_millis() as u64).max(1);
    let jitter = rand::random::<u64>() % base_ms.saturating_add(1).max(1);
    let delay = (base_ms * (attempts as u64 + 1) + jitter).min(cap);
    std::thread::sleep(Duration::from_millis(delay));
    1
}

fn install_busy_handler(conn: &Connection, options: &DatabaseOptions) -> Result<()> {
    // SAFETY: the handle is valid for the connection's lifetime and the
    // options pointer stays valid until after the connection is closed
    // (`MemoryStore` drops `conn` before `busy_options`).
    let rc = unsafe {
        rusqlite::ffi::sqlite3_busy_handler(
            conn.handle(),
            Some(busy_retry),
            options as *const DatabaseOptions as *mut c_void,
        )
    };
    if rc != rusqlite::ffi::SQLITE_OK {
        return storage("unable to install memory busy handler");
    }
    Ok(())
}

/// Executes a SQL batch, retrying `SQLITE_BUSY`/`SQLITE_LOCKED` briefly.
pub(crate) fn exec(conn: &Connection, sql: &str) -> Result<()> {
    let mut attempt = 0u64;
    loop {
        match conn.execute_batch(sql) {
            Ok(()) => return Ok(()),
            Err(rusqlite::Error::SqliteFailure(error, _))
                if matches!(
                    error.code,
                    rusqlite::ErrorCode::DatabaseBusy | rusqlite::ErrorCode::DatabaseLocked
                ) && attempt < 8 =>
            {
                attempt += 1;
                std::thread::sleep(Duration::from_millis(5 * attempt));
            }
            Err(error) => return storage(format!("memory sqlite execution failed: {error}")),
        }
    }
}

/// Transaction or, when one is already open, a uniquely named savepoint.
/// Dropping without `commit` rolls back.
pub(crate) struct Tx<'a> {
    conn: &'a Connection,
    savepoint: Option<String>,
    finished: bool,
}

impl<'a> Tx<'a> {
    pub(crate) fn begin(conn: &'a Connection, immediate: bool) -> Result<Self> {
        use std::sync::atomic::{AtomicU64, Ordering};
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let savepoint = if conn.is_autocommit() {
            exec(
                conn,
                if immediate {
                    "BEGIN IMMEDIATE;"
                } else {
                    "BEGIN;"
                },
            )?;
            None
        } else {
            let name = format!("qt_memory_{}", NEXT.fetch_add(1, Ordering::Relaxed) + 1);
            exec(conn, &format!("SAVEPOINT {name};"))?;
            Some(name)
        };
        Ok(Self {
            conn,
            savepoint,
            finished: false,
        })
    }

    pub(crate) fn commit(mut self) -> Result<()> {
        match &self.savepoint {
            None => exec(self.conn, "COMMIT;")?,
            Some(name) => exec(self.conn, &format!("RELEASE {name};"))?,
        }
        self.finished = true;
        Ok(())
    }
}

impl Drop for Tx<'_> {
    fn drop(&mut self) {
        if self.finished {
            return;
        }
        let _ = match &self.savepoint {
            None => self.conn.execute_batch("ROLLBACK;"),
            Some(name) => self
                .conn
                .execute_batch(&format!("ROLLBACK TO {name}; RELEASE {name};")),
        };
    }
}

fn table_exists(conn: &Connection, name: &str) -> Result<bool> {
    let mut statement =
        conn.prepare_cached("SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1;")?;
    Ok(statement.exists([name])?)
}

fn current_version(conn: &Connection) -> Result<usize> {
    if table_exists(conn, "memory_schema_migrations")? {
        let version: i64 = conn.query_row(
            "SELECT COALESCE(MAX(version),0) FROM memory_schema_migrations;",
            [],
            |row| row.get(0),
        )?;
        return Ok(usize::try_from(version).unwrap_or(0));
    }
    if table_exists(conn, "meta")? {
        let value: Option<String> = conn
            .query_row(
                "SELECT value FROM meta WHERE key='schema_version' LIMIT 1;",
                [],
                |row| Ok(text_at(row, 0)),
            )
            .map(Some)
            .or_else(|error| match error {
                rusqlite::Error::QueryReturnedNoRows => Ok(None),
                other => Err(other),
            })?;
        if let Some(value) = value {
            return value.trim().parse::<usize>().map_err(|_| {
                MemoryError::storage(format!("invalid legacy memory schema version: {value}"))
            });
        }
    }
    Ok(0)
}

pub(crate) fn make_id(prefix: &str) -> String {
    let micros = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_micros() as u64)
        .unwrap_or(0);
    format!("{prefix}{micros:x}{:x}", rand::random::<u64>())
}

pub(crate) fn placeholders(count: usize) -> String {
    vec!["?"; count].join(",")
}

/// Column text as the archived `text()` helper reads it: `NULL` is empty and
/// numbers are rendered.
pub(crate) fn text_at(row: &Row<'_>, index: usize) -> String {
    match row.get_ref(index) {
        Ok(ValueRef::Text(bytes)) | Ok(ValueRef::Blob(bytes)) => {
            String::from_utf8_lossy(bytes).into_owned()
        }
        Ok(ValueRef::Integer(value)) => value.to_string(),
        Ok(ValueRef::Real(value)) => value.to_string(),
        _ => String::new(),
    }
}

pub(crate) fn int_at(row: &Row<'_>, index: usize) -> i64 {
    match row.get_ref(index) {
        Ok(ValueRef::Integer(value)) => value,
        Ok(ValueRef::Real(value)) => value as i64,
        Ok(ValueRef::Text(bytes)) => std::str::from_utf8(bytes)
            .ok()
            .and_then(|text| text.trim().parse::<i64>().ok())
            .unwrap_or(0),
        _ => 0,
    }
}

pub(crate) fn real_at(row: &Row<'_>, index: usize) -> f64 {
    match row.get_ref(index) {
        Ok(ValueRef::Real(value)) => value,
        Ok(ValueRef::Integer(value)) => value as f64,
        Ok(ValueRef::Text(bytes)) => std::str::from_utf8(bytes)
            .ok()
            .and_then(|text| text.trim().parse::<f64>().ok())
            .unwrap_or(0.0),
        _ => 0.0,
    }
}

pub(crate) fn is_null_at(row: &Row<'_>, index: usize) -> bool {
    matches!(row.get_ref(index), Ok(ValueRef::Null))
}

pub(crate) const MEMORY_COLUMNS: [&str; 35] = [
    "id",
    "scope_key",
    "scope_kind",
    "user_id",
    "organization_id",
    "project_id",
    "repository_id",
    "repository_display",
    "branch_id",
    "task_id",
    "agent_id",
    "session_id",
    "conversation_id",
    "memory_type",
    "status",
    "source",
    "source_id",
    "source_type",
    "source_uri",
    "source_hash",
    "source_span",
    "created_by",
    "evidence_strength",
    "content",
    "confidence",
    "quality_score",
    "success_count",
    "failure_count",
    "last_outcome",
    "access_count",
    "superseded_by",
    "replacement_id",
    "created_at",
    "updated_at",
    "last_accessed_at",
];

pub(crate) fn memory_columns(prefix: &str) -> String {
    MEMORY_COLUMNS
        .iter()
        .map(|column| format!("{prefix}{column}"))
        .collect::<Vec<_>>()
        .join(",")
}

/// A memory row without its tags. Unknown persisted enum spellings are
/// reported as errors, as the archived reader throws on them.
pub(crate) struct RawMemory {
    pub(crate) entry: MemoryEntry,
    pub(crate) scope_kind: String,
    pub(crate) memory_type: String,
    pub(crate) status: String,
}

pub(crate) fn read_memory_row(row: &Row<'_>) -> RawMemory {
    let mut entry = MemoryEntry {
        id: text_at(row, 0),
        ..Default::default()
    };
    let scope = &mut entry.scope;
    scope.user = text_at(row, 3);
    scope.organization = text_at(row, 4);
    scope.project = text_at(row, 5);
    scope.repository = text_at(row, 6);
    scope.repository_display = text_at(row, 7);
    scope.branch = text_at(row, 8);
    scope.task = text_at(row, 9);
    scope.agent = text_at(row, 10);
    scope.session = text_at(row, 11);
    scope.conversation = text_at(row, 12);
    entry.source = text_at(row, 15);
    entry.provenance.source_id = text_at(row, 16);
    entry.provenance.source_type = SourceType::parse(&text_at(row, 17));
    entry.provenance.source_uri = text_at(row, 18);
    entry.provenance.source_hash = text_at(row, 19);
    entry.provenance.source_span = text_at(row, 20);
    entry.provenance.created_by = text_at(row, 21);
    entry.provenance.evidence_strength = real_at(row, 22);
    entry.content = text_at(row, 23);
    entry.confidence = real_at(row, 24);
    entry.quality_score = real_at(row, 25);
    entry.success_count = int_at(row, 26).max(0) as u64;
    entry.failure_count = int_at(row, 27).max(0) as u64;
    entry.last_outcome = ProcedureOutcome::parse(&text_at(row, 28));
    entry.access_count = int_at(row, 29).max(0) as u64;
    entry.superseded_by = text_at(row, 30);
    entry.replacement_id = text_at(row, 31);
    entry.created_at = int_at(row, 32);
    entry.updated_at = int_at(row, 33);
    entry.last_accessed_at = int_at(row, 34);
    RawMemory {
        entry,
        scope_kind: text_at(row, 2),
        memory_type: text_at(row, 13),
        status: text_at(row, 14),
    }
}

impl Db<'_> {
    pub(crate) fn load_tags(&self, id: &str) -> Result<Vec<String>> {
        let mut statement = self
            .conn
            .prepare_cached("SELECT tag FROM memory_tags WHERE memory_id=? ORDER BY tag;")?;
        let tags = statement
            .query_map([id], |row| Ok(sanitize_text(&text_at(row, 0), 256)))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(tags)
    }

    /// Completes a raw row: parses enums, loads tags and re-sanitises the
    /// fields older rows may hold unredacted.
    pub(crate) fn finish_memory(&self, raw: RawMemory) -> Result<MemoryEntry> {
        let mut entry = raw.entry;
        entry.scope.kind = ScopeKind::parse(&raw.scope_kind)?;
        entry.memory_type = MemoryType::parse(&raw.memory_type)?;
        entry.status = MemoryStatus::parse(&raw.status)?;
        entry.tags = self.load_tags(&entry.id)?;
        entry.scope.repository_display =
            sanitize_text(&entry.scope.repository_display, MAXIMUM_DURABLE_URI);
        entry.source = sanitize_meta(&entry.source);
        entry.provenance.source_id = sanitize_meta(&entry.provenance.source_id);
        entry.provenance.source_uri =
            sanitize_text(&entry.provenance.source_uri, MAXIMUM_DURABLE_URI);
        entry.provenance.source_hash = sanitize_meta(&entry.provenance.source_hash);
        entry.provenance.source_span = sanitize_meta(&entry.provenance.source_span);
        entry.provenance.created_by = sanitize_meta(&entry.provenance.created_by);
        entry.content = sanitize_text(&entry.content, MAXIMUM_DURABLE_TEXT);
        Ok(entry)
    }

    /// Runs a memory `SELECT` (columns from [`memory_columns`]) and returns
    /// completed entries.
    pub(crate) fn select_memories(
        &self,
        sql: &str,
        binds: &[rusqlite::types::Value],
    ) -> Result<Vec<MemoryEntry>> {
        let raws = {
            let mut statement = self.conn.prepare(sql)?;
            statement
                .query_map(rusqlite::params_from_iter(binds.iter()), |row| {
                    Ok(read_memory_row(row))
                })?
                .collect::<rusqlite::Result<Vec<_>>>()
                .map_err(|error| MemoryError::storage(format!("memory query failed: {error}")))?
        };
        raws.into_iter()
            .map(|raw| self.finish_memory(raw))
            .collect()
    }
}

/// Scope key of an already validated scope.
pub(crate) fn key(scope: &MemoryScope) -> Result<String> {
    crate::scope::scope_key(scope)
}
