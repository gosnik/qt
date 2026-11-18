//! Background index service.
//!
//! A [`BackgroundService`] owns one project's index. Once started, a worker
//! thread loads the persisted snapshot (when deferred), scans immediately and
//! then rescans every `scan_interval` (30 s) or when [`request_scan`] is
//! called. Each scan runs the project scan, build metadata, compilation
//! database, code index and tree-sitter phases, records per-file fingerprints
//! and replaces the SQLite snapshot. A snapshot loaded from disk is marked
//! `index_stale` until the first scan of this process completes.
//!
//! [`request_scan`]: BackgroundService::request_scan

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde_json::{Value, json};

use crate::build::{build_metadata_summary, scan_build_metadata};
use crate::cancel::{Cancelled, Pacer};
use crate::code_index::{CodeIndex, build_code_index_cached};
use crate::compdb::{CompdbError, write_compilation_database};
use crate::scan::{ProjectScanResult, ScanError, scan_project_tree};
use crate::store::{self, SnapshotData};
use crate::syntax_index::SyntaxCache;
use crate::tree_sitter_meta::{TreeSitterOptions, TreeSitterResult, generate_tree_sitter_metadata};
use crate::util::{fnv1a64, generic, path_text, weakly_canonical};

const MAX_FINGERPRINT_BYTES: u64 = 1024 * 1024;

#[derive(Debug, Clone)]
pub struct ServiceOptions {
    pub project_root: PathBuf,
    /// Holds `index.sqlite3`, `tree-sitter/` and `codebase-memory/`.
    pub state_directory: PathBuf,
    /// Where the generated `compile_commands.json` is written.
    pub compile_commands_output: PathBuf,
    pub scan_interval: Duration,
    /// Pace worker-thread scans (short sleeps at checkpoints).
    pub low_priority_background_scans: bool,
    /// Load the persisted snapshot on the worker thread instead of in `new`.
    pub defer_snapshot_load: bool,
    /// Record a content fingerprint for every scanned file.
    pub fingerprints: bool,
    pub tree_sitter: TreeSitterOptions,
}

impl ServiceOptions {
    /// Archived defaults for a project: state in `<root>/.index`,
    /// `compile_commands.json` at the root, 30 s rescans.
    pub fn new(project_root: impl Into<PathBuf>) -> Self {
        let project_root = project_root.into();
        Self {
            state_directory: project_root.join(".index"),
            compile_commands_output: project_root.join("compile_commands.json"),
            project_root,
            scan_interval: Duration::from_secs(30),
            low_priority_background_scans: true,
            defer_snapshot_load: false,
            fingerprints: true,
            tree_sitter: TreeSitterOptions::default(),
        }
    }

    /// Runtime layout: state under `<agent_home>/index/<hash of root>`, with
    /// the snapshot loaded on the worker thread.
    pub fn in_agent_home(project_root: impl Into<PathBuf>, agent_home: &Path) -> Self {
        let mut options = Self::new(project_root);
        options.state_directory = state_directory_for(agent_home, &options.project_root);
        options.defer_snapshot_load = true;
        options
    }
}

/// `<agent_home>/index/<fnv1a64 of the absolute project root>`.
pub fn state_directory_for(agent_home: &Path, project_root: &Path) -> PathBuf {
    let absolute = weakly_canonical(project_root);
    agent_home
        .join("index")
        .join(fnv1a64(generic(&absolute).as_bytes()).to_string())
}

/// `fnv1a64:<hex>` of a file's bytes, or `missing` / `unindexed` (over 1 MiB)
/// / `unreadable`.
pub fn source_fingerprint(path: &Path) -> String {
    let meta = match std::fs::metadata(path) {
        Ok(meta) if meta.is_file() => meta,
        _ => return "missing".into(),
    };
    if meta.len() > MAX_FINGERPRINT_BYTES {
        return "unindexed".into();
    }
    match std::fs::read(path) {
        Ok(bytes) if bytes.len() as u64 == meta.len() => {
            format!("fnv1a64:{:016x}", fnv1a64(&bytes))
        }
        Ok(_) => "changed".into(),
        Err(_) => "unreadable".into(),
    }
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct ServiceStatus {
    pub running: bool,
    pub scan_active: bool,
    pub scan_progress_percent: u32,
    pub scan_phase: String,
    pub generation: usize,
    pub files: usize,
    pub compile_commands: usize,
    pub build_metadata_summary: String,
    pub last_error: String,
    pub snapshot_loaded: bool,
    pub index_stale: bool,
    pub sqlite_fts_available: bool,
    pub sqlite_database: PathBuf,
    pub last_scan_started: Option<SystemTime>,
    pub last_scan_finished: Option<SystemTime>,
    pub tree_sitter: TreeSitterResult,
}

fn time_text(time: Option<SystemTime>) -> String {
    time.and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_secs().to_string())
        .unwrap_or_default()
}

impl ServiceStatus {
    /// The archived `service_status_json` payload.
    pub fn to_json(&self) -> Value {
        json!({
            "running": self.running,
            "scan_active": self.scan_active,
            "scan_progress_percent": self.scan_progress_percent,
            "scan_phase": self.scan_phase,
            "scan_progress": { "percent": self.scan_progress_percent, "phase": self.scan_phase },
            "generation": self.generation,
            "files": self.files,
            "compile_commands": self.compile_commands,
            "build_metadata": self.build_metadata_summary,
            "last_error": self.last_error,
            "snapshot_loaded": self.snapshot_loaded,
            "index_stale": self.index_stale,
            "sqlite_fts_available": self.sqlite_fts_available,
            "sqlite_database": path_text(&self.sqlite_database),
            "last_scan_started": time_text(self.last_scan_started),
            "last_scan_finished": time_text(self.last_scan_finished),
            "tree_sitter": self.tree_sitter.to_json(),
        })
    }
}

struct State {
    status: ServiceStatus,
    scan_requested: bool,
    scan: Arc<ProjectScanResult>,
    code_index: Arc<CodeIndex>,
    fingerprints: Arc<HashMap<String, String>>,
}

struct Inner {
    options: ServiceOptions,
    state: Mutex<State>,
    wake: Condvar,
    stop: Arc<AtomicBool>,
    /// Held for the whole scan, so it also guards the syntax cache.
    scan_lock: Mutex<SyntaxCache>,
}

impl Inner {
    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn update_status(&self, update: impl FnOnce(&mut ServiceStatus)) {
        let mut state = self.lock();
        let running = state.status.running;
        update(&mut state.status);
        state.status.running = running && !self.stop.load(Ordering::Relaxed);
    }

    fn load_persisted_snapshot(&self) {
        let database = store::database_path(&self.options.state_directory);
        match store::load_snapshot(&database, &self.options.project_root) {
            Ok(Some(snapshot)) => {
                let mut state = self.lock();
                if state.status.generation > snapshot.generation {
                    return;
                }
                let status = &mut state.status;
                status.snapshot_loaded = true;
                status.index_stale = true;
                status.sqlite_fts_available = snapshot.fts_available;
                status.generation = snapshot.generation;
                status.files = snapshot.scan.files.len();
                status.compile_commands = snapshot.compile_commands;
                status.build_metadata_summary = snapshot.build_metadata_summary;
                status.last_scan_finished = snapshot.last_scan_finished;
                status.tree_sitter = snapshot.tree_sitter;
                state.scan = Arc::new(snapshot.scan);
                state.code_index = Arc::new(snapshot.code_index);
                state.fingerprints = Arc::new(snapshot.scan_fingerprints);
            }
            Ok(None) => {}
            Err(error) => {
                self.lock().status.last_error = format!("sqlite index load failed: {error}");
            }
        }
    }

    fn run_once(&self, force_low_priority: bool) {
        let mut syntax_cache = self.scan_lock.lock().unwrap_or_else(|e| e.into_inner());
        let generation = self.lock().status.generation;
        self.update_status(|s| {
            s.scan_active = true;
            s.scan_progress_percent = 0;
            s.scan_phase = "starting".into();
            s.last_scan_started = Some(SystemTime::now());
            s.last_error.clear();
        });
        let low_priority = force_low_priority || generation > 0;
        let pacer = Pacer::new(self.stop.clone(), low_priority);
        match self.scan(&pacer, generation, &mut syntax_cache) {
            Ok(()) => {}
            Err(failure) => {
                self.update_status(|s| {
                    s.scan_active = false;
                    s.scan_phase = if failure.cancelled {
                        "cancelled"
                    } else {
                        "error"
                    }
                    .into();
                    s.last_scan_finished = Some(SystemTime::now());
                    s.last_error = failure.message;
                });
            }
        }
    }

    fn progress(&self, percent: u32, phase: &str) {
        self.update_status(|s| {
            s.scan_active = true;
            s.scan_progress_percent = percent.min(100);
            s.scan_phase = phase.into();
        });
    }

    fn scan(
        &self,
        pacer: &Pacer,
        generation: usize,
        syntax_cache: &mut SyntaxCache,
    ) -> Result<(), ScanFailure> {
        let options = &self.options;
        std::fs::create_dir_all(&options.state_directory).map_err(|e| {
            ScanFailure::error(format!(
                "could not create index state directory {}: {e}",
                options.state_directory.display()
            ))
        })?;
        pacer.checkpoint(0, 0, 25)?;
        self.progress(5, "scanning project files");
        let scan = scan_project_tree(&options.project_root, pacer)?;
        let mut fingerprints = HashMap::new();
        if options.fingerprints {
            for (index, file) in scan.files.iter().enumerate() {
                pacer.checkpoint(index + 1, 64, 10)?;
                fingerprints.insert(
                    generic(&file.path),
                    source_fingerprint(&scan.root.join(&file.path)),
                );
            }
        }
        pacer.checkpoint(0, 0, 25)?;
        self.progress(25, "scanning build metadata");
        let metadata = scan_build_metadata(&scan, pacer)?;
        pacer.checkpoint(0, 0, 25)?;
        self.progress(45, "writing compilation database");
        let entries =
            write_compilation_database(&scan, &metadata, &options.compile_commands_output, pacer)?;
        pacer.checkpoint(0, 0, 25)?;
        self.progress(60, "building code index");
        let code_index = build_code_index_cached(&scan, syntax_cache, pacer)?;
        pacer.checkpoint(0, 0, 25)?;
        self.progress(80, "generating tree-sitter metadata");
        let tree_sitter = generate_tree_sitter_metadata(&scan, &options.tree_sitter, pacer)?;

        let summary = format!(
            "{} code_index=classes:{},functions:{},call_graph_edges:{},skipped_files:{}",
            build_metadata_summary(&metadata),
            code_index.classes.len(),
            code_index.functions.len(),
            code_index.call_graph.len(),
            code_index.skipped_files.len()
        );
        let next_generation = generation + 1;
        let database = store::database_path(&options.state_directory);
        store::save_snapshot(
            &database,
            &options.project_root,
            &SnapshotData {
                scan: &scan,
                code_index: &code_index,
                generation: next_generation,
                compile_commands: entries,
                build_metadata_summary: &summary,
                tree_sitter: &tree_sitter,
                scan_fingerprints: &fingerprints,
            },
        )
        .map_err(|e| ScanFailure::error(format!("sqlite index save failed: {e}")))?;
        let fts_available = matches!(
            store::load_snapshot(&database, &options.project_root),
            Ok(Some(snapshot)) if snapshot.fts_available
        );

        let mut state = self.lock();
        let running = state.status.running && !self.stop.load(Ordering::Relaxed);
        state.status = ServiceStatus {
            running,
            scan_active: false,
            scan_progress_percent: 100,
            scan_phase: "complete".into(),
            generation: next_generation,
            files: scan.files.len(),
            compile_commands: entries,
            build_metadata_summary: summary,
            last_error: String::new(),
            snapshot_loaded: true,
            index_stale: false,
            sqlite_fts_available: fts_available,
            sqlite_database: database,
            last_scan_started: state.status.last_scan_started,
            last_scan_finished: Some(SystemTime::now()),
            tree_sitter,
        };
        state.scan = Arc::new(scan);
        state.code_index = Arc::new(code_index);
        state.fingerprints = Arc::new(fingerprints);
        Ok(())
    }

    fn worker_loop(self: Arc<Self>) {
        if self.options.defer_snapshot_load {
            self.load_persisted_snapshot();
        }
        loop {
            {
                let mut state = self.lock();
                let deadline = Instant::now() + self.options.scan_interval;
                while !state.scan_requested && !self.stop.load(Ordering::Relaxed) {
                    let now = Instant::now();
                    if now >= deadline {
                        break;
                    }
                    state = self
                        .wake
                        .wait_timeout(state, deadline - now)
                        .unwrap_or_else(|e| e.into_inner())
                        .0;
                }
                if self.stop.load(Ordering::Relaxed) {
                    break;
                }
                state.scan_requested = false;
            }
            self.run_once(self.options.low_priority_background_scans);
        }
    }
}

struct ScanFailure {
    message: String,
    cancelled: bool,
}

impl ScanFailure {
    fn error(message: String) -> Self {
        Self {
            message,
            cancelled: false,
        }
    }
}

impl From<Cancelled> for ScanFailure {
    fn from(c: Cancelled) -> Self {
        Self {
            message: c.to_string(),
            cancelled: true,
        }
    }
}

impl From<ScanError> for ScanFailure {
    fn from(e: ScanError) -> Self {
        Self {
            cancelled: e == ScanError::Cancelled,
            message: e.to_string(),
        }
    }
}

impl From<CompdbError> for ScanFailure {
    fn from(e: CompdbError) -> Self {
        Self {
            cancelled: matches!(e, CompdbError::Cancelled),
            message: e.to_string(),
        }
    }
}

/// Index service for one project.
pub struct BackgroundService {
    inner: Arc<Inner>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl BackgroundService {
    pub fn new(mut options: ServiceOptions) -> Self {
        options.project_root = weakly_canonical(&options.project_root);
        if options.state_directory.as_os_str().is_empty() {
            options.state_directory = options.project_root.join(".index");
        }
        if options.compile_commands_output.as_os_str().is_empty() {
            options.compile_commands_output = options.project_root.join("compile_commands.json");
        }
        if options.tree_sitter.output_directory.as_os_str().is_empty() {
            options.tree_sitter.output_directory = options.state_directory.join("tree-sitter");
        }
        let status = ServiceStatus {
            sqlite_database: store::database_path(&options.state_directory),
            ..Default::default()
        };
        let defer = options.defer_snapshot_load;
        let inner = Arc::new(Inner {
            state: Mutex::new(State {
                status,
                scan_requested: false,
                scan: Arc::new(ProjectScanResult {
                    root: options.project_root.clone(),
                    ..Default::default()
                }),
                code_index: Arc::new(CodeIndex::default()),
                fingerprints: Arc::new(HashMap::new()),
            }),
            options,
            wake: Condvar::new(),
            stop: Arc::new(AtomicBool::new(false)),
            scan_lock: Mutex::new(SyntaxCache::default()),
        });
        if !defer {
            inner.load_persisted_snapshot();
        }
        Self {
            inner,
            worker: Mutex::new(None),
        }
    }

    /// Start the worker thread (no-op when already running). The first scan
    /// starts immediately.
    pub fn start(&self) -> std::io::Result<()> {
        let mut worker = self.worker.lock().unwrap_or_else(|e| e.into_inner());
        {
            let mut state = self.inner.lock();
            if state.status.running {
                return Ok(());
            }
            self.inner.stop.store(false, Ordering::Relaxed);
            state.status.running = true;
            state.scan_requested = true;
        }
        let inner = self.inner.clone();
        match std::thread::Builder::new()
            .name("qt-index".into())
            .spawn(move || inner.worker_loop())
        {
            Ok(handle) => {
                *worker = Some(handle);
                self.inner.wake.notify_all();
                Ok(())
            }
            Err(error) => {
                self.inner.lock().status.running = false;
                Err(error)
            }
        }
    }

    /// Stop the worker, cancelling an in-flight scan, and wait for it.
    pub fn stop(&self) {
        let handle = {
            let mut worker = self.worker.lock().unwrap_or_else(|e| e.into_inner());
            {
                let mut state = self.inner.lock();
                if !state.status.running && worker.is_none() {
                    return;
                }
                self.inner.stop.store(true, Ordering::Relaxed);
                state.status.running = false;
            }
            worker.take()
        };
        self.inner.wake.notify_all();
        if let Some(handle) = handle {
            let _ = handle.join();
        }
    }

    /// Ask the worker to rescan as soon as possible.
    pub fn request_scan(&self) {
        self.inner.lock().scan_requested = true;
        self.inner.wake.notify_all();
    }

    /// Run one scan on the calling thread. Failures are reported through
    /// [`ServiceStatus::last_error`] and `scan_phase`.
    pub fn run_once(&self) {
        self.inner.run_once(false);
    }

    pub fn status(&self) -> ServiceStatus {
        self.inner.lock().status.clone()
    }

    pub fn code_index(&self) -> Arc<CodeIndex> {
        self.inner.lock().code_index.clone()
    }

    pub fn project_scan(&self) -> Arc<ProjectScanResult> {
        self.inner.lock().scan.clone()
    }

    /// Per-file fingerprints from the last scan (or loaded snapshot).
    pub fn fingerprints(&self) -> Arc<HashMap<String, String>> {
        self.inner.lock().fingerprints.clone()
    }

    pub fn project_root(&self) -> &Path {
        &self.inner.options.project_root
    }

    pub fn options(&self) -> &ServiceOptions {
        &self.inner.options
    }
}

impl Drop for BackgroundService {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Lazily created services keyed by canonical project root, with state in
/// the agent home. Tools resolve the service for their working directory here.
pub struct IndexManager {
    agent_home: PathBuf,
    services: Mutex<HashMap<PathBuf, Arc<BackgroundService>>>,
}

impl IndexManager {
    pub fn new(agent_home: impl Into<PathBuf>) -> Self {
        Self {
            agent_home: agent_home.into(),
            services: Mutex::new(HashMap::new()),
        }
    }

    pub fn agent_home(&self) -> &Path {
        &self.agent_home
    }

    /// The service for `project_root`, created (not started) on first use.
    /// A service created here loads its persisted snapshot immediately.
    pub fn service_for(&self, project_root: &Path) -> Arc<BackgroundService> {
        let root = weakly_canonical(project_root);
        let mut services = self.services.lock().unwrap_or_else(|e| e.into_inner());
        services
            .entry(root.clone())
            .or_insert_with(|| {
                let mut options = ServiceOptions::in_agent_home(root, &self.agent_home);
                options.defer_snapshot_load = false;
                Arc::new(BackgroundService::new(options))
            })
            .clone()
    }

    /// Register and start a background service for the workspace root.
    pub fn start(&self, project_root: &Path) -> std::io::Result<Arc<BackgroundService>> {
        let root = weakly_canonical(project_root);
        let service = {
            let mut services = self.services.lock().unwrap_or_else(|e| e.into_inner());
            services
                .entry(root.clone())
                .or_insert_with(|| {
                    Arc::new(BackgroundService::new(ServiceOptions::in_agent_home(
                        root,
                        &self.agent_home,
                    )))
                })
                .clone()
        };
        service.start()?;
        Ok(service)
    }

    /// Use an existing service (for example one with custom options).
    pub fn insert(&self, service: Arc<BackgroundService>) {
        let root = service.project_root().to_path_buf();
        self.services
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(root, service);
    }

    /// Stop every running service.
    pub fn stop_all(&self) {
        let services: Vec<_> = self
            .services
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .values()
            .cloned()
            .collect();
        for service in services {
            service.stop();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn project() -> (tempfile::TempDir, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap().join("proj");
        fs::create_dir_all(root.join("src")).unwrap();
        fs::write(
            root.join("src/main.cpp"),
            "#include \"util.h\"\nint helper() { return 1; }\nint main() { return helper(); }\n",
        )
        .unwrap();
        fs::write(root.join("src/util.h"), "int helper();\n").unwrap();
        (dir, root)
    }

    fn options(root: &Path, home: &Path) -> ServiceOptions {
        let mut options = ServiceOptions::in_agent_home(root, home);
        options.scan_interval = Duration::from_millis(200);
        options.low_priority_background_scans = false;
        options
    }

    #[test]
    fn run_once_builds_and_persists() {
        let (dir, root) = project();
        let home = dir.path().join("home");
        let service = BackgroundService::new(options(&root, &home));
        assert_eq!(service.status().generation, 0);
        service.run_once();
        let status = service.status();
        assert_eq!(status.generation, 1, "{:?}", status.last_error);
        assert_eq!(status.scan_phase, "complete");
        assert_eq!(status.scan_progress_percent, 100);
        assert_eq!(status.files, 2);
        assert_eq!(status.compile_commands, 1);
        assert!(status.sqlite_fts_available);
        assert!(!status.index_stale);
        assert!(
            status
                .build_metadata_summary
                .contains("code_index=classes:0,functions:2")
        );
        assert!(status.sqlite_database.starts_with(home.join("index")));
        assert!(root.join("compile_commands.json").is_file());
        assert_eq!(service.code_index().functions.len(), 2);
        let fingerprints = service.fingerprints();
        assert!(fingerprints["src/main.cpp"].starts_with("fnv1a64:"));
        let json = status.to_json();
        assert_eq!(json["scan_progress"]["phase"], "complete");
        assert_eq!(json["tree_sitter"]["files_parsed"], 2);

        // A fresh service reloads the snapshot and marks it stale.
        drop(service);
        let reloaded = BackgroundService::new(options(&root, &home).tap_defer(false));
        let status = reloaded.status();
        assert_eq!(status.generation, 1);
        assert!(status.snapshot_loaded && status.index_stale);
        assert_eq!(reloaded.code_index().functions.len(), 2);
        assert_eq!(reloaded.fingerprints().len(), 2);
    }

    #[test]
    fn worker_scans_and_rescans_on_request() {
        let (dir, root) = project();
        let home = dir.path().join("home");
        let service = BackgroundService::new(options(&root, &home));
        service.start().unwrap();
        assert!(service.status().running);
        let wait_until = |done: &dyn Fn(&ServiceStatus) -> bool| {
            let deadline = Instant::now() + Duration::from_secs(20);
            while !done(&service.status()) {
                assert!(
                    Instant::now() < deadline,
                    "timed out: {:?}",
                    service.status()
                );
                std::thread::sleep(Duration::from_millis(20));
            }
        };
        wait_until(&|s| s.generation >= 1);
        let has_extra = || {
            service
                .project_scan()
                .files
                .iter()
                .any(|f| f.path == Path::new("src/extra.cpp"))
        };
        assert!(!has_extra());
        fs::write(root.join("src/extra.cpp"), "void extra() {}\n").unwrap();
        service.request_scan();
        let deadline = Instant::now() + Duration::from_secs(20);
        while !has_extra() {
            assert!(
                Instant::now() < deadline,
                "timed out: {:?}",
                service.status()
            );
            std::thread::sleep(Duration::from_millis(20));
        }
        // The interval keeps triggering rescans without requests.
        let generation = service.status().generation;
        wait_until(&|s| s.generation >= generation + 2);
        service.stop();
        assert!(!service.status().running);
    }

    #[test]
    fn missing_root_reports_error() {
        let dir = tempfile::tempdir().unwrap();
        let service = BackgroundService::new(ServiceOptions::in_agent_home(
            dir.path().join("missing"),
            &dir.path().join("home"),
        ));
        service.run_once();
        let status = service.status();
        assert_eq!(status.scan_phase, "error");
        assert!(
            status.last_error.contains("project root does not exist"),
            "{}",
            status.last_error
        );
        assert_eq!(status.generation, 0);
    }

    #[test]
    fn state_directory_is_stable_hash() {
        let a = state_directory_for(Path::new("/home"), Path::new("/tmp/project-a"));
        let b = state_directory_for(Path::new("/home"), Path::new("/tmp/project-a"));
        let c = state_directory_for(Path::new("/home"), Path::new("/tmp/project-b"));
        assert_eq!(a, b);
        assert_ne!(a, c);
        assert!(a.starts_with("/home/index"));
    }

    #[test]
    fn manager_reuses_services() {
        let (dir, root) = project();
        let manager = IndexManager::new(dir.path().join("home"));
        let a = manager.service_for(&root);
        let b = manager.service_for(&root.join("."));
        assert!(Arc::ptr_eq(&a, &b));
        assert!(!a.status().running);
    }

    trait TapDefer {
        fn tap_defer(self, defer: bool) -> Self;
    }

    impl TapDefer for ServiceOptions {
        fn tap_defer(mut self, defer: bool) -> Self {
            self.defer_snapshot_load = defer;
            self
        }
    }
}
