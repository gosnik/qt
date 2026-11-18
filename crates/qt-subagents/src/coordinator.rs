//! Persistent subagent workers (the archived `SubagentCoordinator`).
//!
//! A worker is a named logical child (`/root/<name>`) whose turns each run in
//! a fresh child process resuming the same `session.jsonl`. Messages queue in
//! a per-worker mailbox; every finished turn bumps the worker's event id and
//! records a `subagent_complete` event that stays pending until the parent
//! takes or acknowledges it. State lives in
//! `<agent_home>/subagents/<hash(parent session)>/children.json`, guarded by
//! an exclusive lock on `children.lock`, and survives restarts: workers that
//! were running when the parent died come back `interrupted`.

use std::collections::{BTreeMap, VecDeque};
use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::{Arc, LazyLock, Mutex, MutexGuard};
use std::time::Duration;

use serde_json::{Map, Value, json};
use tokio::runtime::Handle;
use tokio::sync::{Notify, Semaphore};
use tokio::task::JoinHandle;
use tokio_util::sync::CancellationToken;

use qt_core::message::{AgentMessage, ContentBlock, Message, ToolResultMessage, now_millis};

use crate::runner::{ChildOutcome, ChildRequest, ChildRunner};

/// Worker processes running at once in this host process, across coordinators.
pub const MAX_HOST_PROCESSES: usize = 4;
/// Default per-coordinator concurrency.
pub const DEFAULT_CONCURRENCY: usize = 4;
/// Canonical path of the top-level parent.
pub const ROOT_PATH: &str = "/root";
/// Text of the synthetic result closing a tool call that was still running
/// when the parent context was forked.
pub const FORK_INTERRUPTED_TEXT: &str =
    "Parent tool call was still running when this context was forked.";

static HOST_PROCESSES: LazyLock<Semaphore> = LazyLock::new(|| Semaphore::new(MAX_HOST_PROCESSES));

/// Archived `stable_hash_hex`: 64-bit FNV-1a as 16 lowercase hex digits.
pub fn stable_hash_hex(value: &str) -> String {
    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for byte in value.bytes() {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    format!("{hash:016x}")
}

/// `<agent_home>/subagents/<stable_hash_hex(parent_session_id)>`.
pub fn coordinator_directory(agent_home: &Path, parent_session_id: &str) -> PathBuf {
    agent_home
        .join("subagents")
        .join(stable_hash_hex(parent_session_id))
}

/// A worker name: `[A-Za-z0-9][A-Za-z0-9._-]{0,127}`, not `.` or `..`.
pub fn valid_worker_name(name: &str) -> bool {
    let bytes = name.as_bytes();
    !bytes.is_empty()
        && bytes.len() <= 128
        && bytes[0].is_ascii_alphanumeric()
        && bytes
            .iter()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
        && name != "."
        && name != ".."
}

/// How much parent context a new worker starts with.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ForkMode {
    #[default]
    None,
    All,
    /// The last N parent user turns.
    Turns(u64),
}

impl ForkMode {
    pub fn to_value(self) -> Value {
        match self {
            Self::None => json!("none"),
            Self::All => json!("all"),
            Self::Turns(n) => json!(n),
        }
    }

    /// `"none"`, `"all"`, a positive integer or a positive integer string.
    pub fn from_value(value: &Value) -> Option<Self> {
        match value {
            Value::String(s) => match s.as_str() {
                "none" => Some(Self::None),
                "all" => Some(Self::All),
                other => crate::definitions::parse_nonnegative(other)
                    .filter(|n| *n > 0)
                    .map(|n| Self::Turns(n as u64)),
            },
            Value::Number(n) => n.as_i64().filter(|n| *n > 0).map(|n| Self::Turns(n as u64)),
            _ => None,
        }
    }
}

/// Everything needed to start a persistent worker.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct PersistentRequest {
    pub name: String,
    pub agent: String,
    pub task: String,
    /// Child launch template; `--session <path>` is appended per turn and the
    /// turn's task is written to stdin.
    pub process: ChildRequest,
    /// Permissions recorded at spawn (`workspace`, `allow_external`,
    /// `toolnames`, `depth`, `trust`, `model`), re-checked on reuse.
    pub permissions: Value,
    pub fork: ForkMode,
}

/// Coordinator configuration.
#[derive(Debug, Clone)]
pub struct CoordinatorOptions {
    /// Usually [`coordinator_directory`].
    pub directory: PathBuf,
    pub parent_session_id: String,
    /// Canonical path of the parent (`/root`, or `QT_SUBAGENT_PATH` in a child).
    pub parent_path: String,
    pub concurrency: usize,
    /// Runtime the worker tasks run on.
    pub runtime: Handle,
}

impl CoordinatorOptions {
    /// Options for `parent_session_id` under `agent_home`, on the current
    /// Tokio runtime. The parent path honours `QT_SUBAGENT_PATH`.
    ///
    /// Panics when called outside a Tokio runtime.
    pub fn new(agent_home: &Path, parent_session_id: impl Into<String>) -> Self {
        let parent_session_id = parent_session_id.into();
        Self {
            directory: coordinator_directory(agent_home, &parent_session_id),
            parent_session_id,
            parent_path: std::env::var("QT_SUBAGENT_PATH")
                .ok()
                .filter(|p| !p.is_empty())
                .unwrap_or_else(|| ROOT_PATH.to_string()),
            concurrency: DEFAULT_CONCURRENCY,
            runtime: Handle::current(),
        }
    }
}

/// Receives `subagent_start` and `subagent_complete` events.
pub type EventSink = Arc<dyn Fn(&Value) + Send + Sync>;
/// Supplies the parent's current context for `fork`.
pub type ContextSnapshot = Arc<dyn Fn() -> Vec<AgentMessage> + Send + Sync>;

struct Worker {
    id: String,
    name: String,
    agent: String,
    path: PathBuf,
    status: String,
    last_error: String,
    generation: u64,
    event_id: u64,
    fork_context: Vec<Value>,
    running: bool,
    interrupted: bool,
    wake: bool,
    stop: bool,
    request: PersistentRequest,
    mailbox: VecDeque<String>,
    cancel: Option<CancellationToken>,
    task: Option<JoinHandle<()>>,
    notify: Arc<Notify>,
}

impl Worker {
    fn new(id: String, name: String, path: PathBuf) -> Self {
        Self {
            id,
            name,
            agent: String::new(),
            path,
            status: "idle".into(),
            last_error: String::new(),
            generation: 0,
            event_id: 0,
            fork_context: Vec::new(),
            running: false,
            interrupted: false,
            wake: false,
            stop: false,
            request: PersistentRequest::default(),
            mailbox: VecDeque::new(),
            cancel: None,
            task: None,
            notify: Arc::new(Notify::new()),
        }
    }
}

struct State {
    workers: BTreeMap<String, Worker>,
    events: Vec<Value>,
    sink: Option<EventSink>,
    snapshot: Option<ContextSnapshot>,
    active: usize,
    completion_count: u64,
    stopping: bool,
    persistence_error: Option<String>,
    lock: Option<File>,
}

struct Inner {
    options: CoordinatorOptions,
    runner: Arc<dyn ChildRunner>,
    state: Mutex<State>,
    state_changed: Notify,
    shutdown: CancellationToken,
    permits: Arc<Semaphore>,
}

/// Owns the persistent workers of one parent session.
pub struct SubagentCoordinator {
    inner: Arc<Inner>,
}

impl std::fmt::Debug for SubagentCoordinator {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SubagentCoordinator")
            .field("directory", &self.inner.options.directory)
            .field("parent_session_id", &self.inner.options.parent_session_id)
            .finish_non_exhaustive()
    }
}

fn process_to_value(process: &ChildRequest) -> Value {
    json!({
        "arguments": process.arguments,
        "stdin_payload": process.stdin.clone().unwrap_or_default(),
        "cwd": process.cwd.as_ref().map(|c| c.to_string_lossy().into_owned()),
        "timeout_ms": process.timeout_ms,
        "environment": process.environment.iter().map(|(name, value)| json!({
            "name": name,
            "value": value,
        })).collect::<Vec<_>>(),
    })
}

fn process_from_value(value: &Value) -> Result<ChildRequest, String> {
    let mut process = ChildRequest::default();
    if let Some(arguments) = value.get("arguments") {
        process.arguments = serde_json::from_value(arguments.clone())
            .map_err(|_| "invalid child arguments".to_string())?;
    }
    process.stdin = value
        .get("stdin_payload")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
        .map(str::to_string);
    process.cwd = value.get("cwd").and_then(Value::as_str).map(PathBuf::from);
    process.timeout_ms = value.get("timeout_ms").and_then(Value::as_u64);
    if let Some(environment) = value.get("environment").and_then(Value::as_array) {
        for item in environment {
            let name = item
                .get("name")
                .and_then(Value::as_str)
                .ok_or("invalid child environment")?;
            let value = item
                .get("value")
                .and_then(Value::as_str)
                .map(str::to_string);
            process.environment.push((name.to_string(), value));
        }
    }
    Ok(process)
}

fn now_iso() -> String {
    chrono::Utc::now().to_rfc3339_opts(chrono::SecondsFormat::Millis, true)
}

fn uuid_v4() -> String {
    let mut bytes: [u8; 16] = rand::random();
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    let hex: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

/// Write `bytes` to `path` readable by the owner only.
fn write_private(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    use std::io::Write as _;
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt as _;
        options.mode(0o600);
    }
    let mut file = options.open(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        file.set_permissions(std::fs::Permissions::from_mode(0o600))?;
    }
    file.write_all(bytes)?;
    file.sync_all()
}

fn create_private_dir(path: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(path)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

/// Parent messages from the fork point on (`fork` = all or last N user turns).
pub fn fork_messages(context: &[AgentMessage], fork: ForkMode) -> Vec<AgentMessage> {
    let begin = match fork {
        ForkMode::None => return Vec::new(),
        ForkMode::All => 0,
        ForkMode::Turns(n) => {
            let mut remaining = n;
            let mut begin = 0;
            for (index, message) in context.iter().enumerate().rev() {
                if message.is_user() {
                    remaining -= 1;
                    if remaining == 0 {
                        begin = index;
                        break;
                    }
                }
            }
            begin
        }
    };
    context[begin..].to_vec()
}

/// The child session file for a new worker: a Pi v3 header carrying the
/// lineage, then the forked messages, with every tool call that has no result
/// yet closed by an explicit interrupted result.
pub fn child_session_contents(
    session_cwd: &str,
    parent_session_id: &str,
    subagent: Value,
    fork_context: &[Value],
) -> String {
    let header = json!({
        "type": "session",
        "version": 3,
        "id": uuid_v4(),
        "timestamp": now_iso(),
        "cwd": session_cwd,
        "parentSession": parent_session_id,
        "subagent": subagent,
    });
    let mut out = format!("{header}\n");
    let mut previous: Option<String> = None;
    let mut append = |out: &mut String, message: Value| {
        let id = qt_core::util::short_id();
        let entry = json!({
            "type": "message",
            "id": id,
            "parentId": previous,
            "timestamp": now_iso(),
            "message": message,
        });
        out.push_str(&entry.to_string());
        out.push('\n');
        previous = Some(id);
    };
    let mut pending: Vec<(String, String)> = Vec::new();
    for message in fork_context {
        match serde_json::from_value::<AgentMessage>(message.clone()) {
            Ok(AgentMessage::Llm(Message::Assistant(assistant))) => {
                for (id, name, _) in assistant.tool_calls() {
                    pending.push((id.to_string(), name.to_string()));
                }
            }
            Ok(AgentMessage::Llm(Message::ToolResult(result))) => {
                pending.retain(|(id, _)| *id != result.tool_call_id);
            }
            _ => {}
        }
        append(&mut out, message.clone());
    }
    for (id, name) in pending {
        let result = ToolResultMessage {
            tool_call_id: id,
            tool_name: name,
            content: vec![ContentBlock::text(FORK_INTERRUPTED_TEXT)],
            is_error: true,
            details: None,
            timestamp: Some(now_millis()),
        };
        append(
            &mut out,
            serde_json::to_value(Message::ToolResult(result)).unwrap_or(Value::Null),
        );
    }
    out
}

fn completion_event(worker: &Worker, interrupted: bool) -> Map<String, Value> {
    let mut event = Map::new();
    event.insert("type".into(), json!("subagent_complete"));
    event.insert("child_id".into(), json!(worker.id));
    event.insert("generation".into(), json!(worker.generation));
    event.insert("event_id".into(), json!(worker.event_id));
    event.insert("status".into(), json!(worker.status));
    event.insert("interrupted".into(), json!(interrupted));
    event
}

impl Inner {
    fn lock(&self) -> MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn index_path(&self) -> PathBuf {
        self.options.directory.join("children.json")
    }

    fn worker_json(&self, worker: &Worker) -> Value {
        let request = &worker.request;
        let mut value = json!({
            "id": worker.id,
            "name": worker.name,
            "agent": worker.agent,
            "path": worker.path.to_string_lossy(),
            "status": worker.status,
            "generation": worker.generation,
            "event_id": worker.event_id,
            "wake": worker.wake,
            "mailbox_depth": worker.mailbox.len(),
            "mailbox": worker.mailbox,
            "parent_session_id": self.options.parent_session_id,
            "permissions": request.permissions,
            "fork": request.fork.to_value(),
            "request": {
                "agent": request.agent,
                "task": request.task,
                "permissions": request.permissions,
                "fork": request.fork.to_value(),
                "process": process_to_value(&request.process),
            },
            "fork_context": worker.fork_context,
        });
        if !worker.last_error.is_empty() {
            value["last_error"] = json!(worker.last_error);
        }
        value
    }

    fn persist(&self, state: &mut State) -> bool {
        let children: Vec<Value> = state
            .workers
            .values()
            .map(|w| self.worker_json(w))
            .collect();
        let root = json!({
            "version": 1,
            "parent_session_id": self.options.parent_session_id,
            "pending_events": state.events,
            "children": children,
        });
        match qt_core::config::write_json_atomic(&self.index_path(), &root, true) {
            Ok(()) => {
                state.persistence_error = None;
                true
            }
            Err(_) => {
                state.persistence_error = Some("cannot write children index".into());
                false
            }
        }
    }

    fn check_usable(state: &State) -> Result<(), String> {
        match &state.persistence_error {
            Some(error) => Err(error.clone()),
            None => Ok(()),
        }
    }

    fn find_id(&self, state: &State, target: &str) -> Option<String> {
        if state.workers.contains_key(target) {
            return Some(target.to_string());
        }
        let id = format!("{}/{target}", self.options.parent_path);
        state.workers.contains_key(&id).then_some(id)
    }

    fn load_index(&self, state: &mut State) -> Result<(), String> {
        let text = match std::fs::read_to_string(self.index_path()) {
            Ok(text) => text,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(()),
            Err(e) => return Err(e.to_string()),
        };
        let root: Value = serde_json::from_str(&text).map_err(|e| e.to_string())?;
        let children = root
            .get("children")
            .and_then(Value::as_array)
            .filter(|_| root.get("version").and_then(Value::as_u64) == Some(1))
            .ok_or("invalid children index")?;
        if root.get("parent_session_id").and_then(Value::as_str)
            != Some(self.options.parent_session_id.as_str())
        {
            return Err("child index belongs to another parent session".into());
        }
        if let Some(events) = root.get("pending_events").and_then(Value::as_array) {
            for event in events {
                if !event.is_object() {
                    return Err("invalid pending event".into());
                }
                state.events.push(event.clone());
            }
        }
        for item in children {
            let (Some(id), Some(stored)) = (
                item.get("id").and_then(Value::as_str),
                item.get("request").filter(|r| r.is_object()),
            ) else {
                return Err("invalid child record".into());
            };
            let name = item.get("name").and_then(Value::as_str).unwrap_or(id);
            let path = item.get("path").and_then(Value::as_str).unwrap_or("");
            if !valid_worker_name(name)
                || id != format!("{}/{name}", self.options.parent_path)
                || Path::new(path) != self.options.directory.join(name).join("session.jsonl")
            {
                return Err("invalid child identity or session path".into());
            }
            let mut worker = Worker::new(id.to_string(), name.to_string(), PathBuf::from(path));
            worker.agent = item
                .get("agent")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            worker.status = item
                .get("status")
                .and_then(Value::as_str)
                .unwrap_or("interrupted")
                .to_string();
            let was_running = worker.status == "running";
            worker.wake = item.get("wake").and_then(Value::as_bool).unwrap_or(false);
            worker.last_error = item
                .get("last_error")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            worker.generation = item.get("generation").and_then(Value::as_u64).unwrap_or(0);
            worker.event_id = item.get("event_id").and_then(Value::as_u64).unwrap_or(0);
            worker.fork_context = item
                .get("fork_context")
                .and_then(Value::as_array)
                .cloned()
                .unwrap_or_default();
            worker.request = PersistentRequest {
                name: name.to_string(),
                agent: stored
                    .get("agent")
                    .and_then(Value::as_str)
                    .unwrap_or(&worker.agent)
                    .to_string(),
                task: stored
                    .get("task")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string(),
                permissions: stored.get("permissions").cloned().unwrap_or(json!({})),
                fork: stored
                    .get("fork")
                    .and_then(ForkMode::from_value)
                    .unwrap_or_default(),
                process: process_from_value(stored.get("process").unwrap_or(&Value::Null))?,
            };
            if let Some(mailbox) = item.get("mailbox").and_then(Value::as_array) {
                for message in mailbox {
                    let message = message.as_str().ok_or("invalid child mailbox")?;
                    worker.mailbox.push_back(message.to_string());
                }
            }
            worker.interrupted = worker.status == "interrupted" || was_running;
            if was_running || worker.wake {
                worker.status = "interrupted".into();
                worker.interrupted = true;
                worker.wake = false;
                worker.event_id += 1;
                let mut event = completion_event(&worker, true);
                event.insert("recovered".into(), json!(true));
                state.events.push(Value::Object(event));
            }
            state.workers.insert(worker.id.clone(), worker);
        }
        Ok(())
    }

    fn emit(&self, event: &Value) {
        let sink = {
            let mut state = self.lock();
            self.persist(&mut state);
            state.sink.clone()
        };
        if let Some(sink) = sink {
            sink(event);
        }
    }

    fn ensure_task(self: &Arc<Self>, worker: &mut Worker) {
        if worker.task.as_ref().is_some_and(|t| !t.is_finished()) {
            return;
        }
        let inner = self.clone();
        let id = worker.id.clone();
        worker.task = Some(
            self.options
                .runtime
                .spawn(async move { inner.run_worker(id).await }),
        );
    }

    async fn run_worker(self: Arc<Self>, id: String) {
        loop {
            // Wait until woken.
            loop {
                let notify = {
                    let state = self.lock();
                    let Some(worker) = state.workers.get(&id) else {
                        return;
                    };
                    if state.stopping || worker.stop {
                        return;
                    }
                    if worker.wake {
                        break;
                    }
                    worker.notify.clone()
                };
                tokio::select! {
                    _ = notify.notified() => {}
                    _ = self.shutdown.cancelled() => return,
                }
            }
            let local = tokio::select! {
                permit = self.permits.clone().acquire_owned() => match permit {
                    Ok(permit) => permit,
                    Err(_) => return,
                },
                _ = self.shutdown.cancelled() => return,
            };
            let global = tokio::select! {
                permit = HOST_PROCESSES.acquire() => match permit {
                    Ok(permit) => permit,
                    Err(_) => return,
                },
                _ = self.shutdown.cancelled() => return,
            };

            let (process, cancel, launch_error, generation) = {
                let mut guard = self.lock();
                let state = &mut *guard;
                if state.stopping {
                    return;
                }
                let Some(worker) = state.workers.get_mut(&id) else {
                    return;
                };
                if worker.stop {
                    return;
                }
                if !worker.wake {
                    continue;
                }
                worker.wake = false;
                worker.running = true;
                worker.generation += 1;
                worker.interrupted = false;
                worker.status = "running".into();
                let task = worker
                    .mailbox
                    .pop_front()
                    .unwrap_or_else(|| worker.request.task.clone());
                let cancel = self.shutdown.child_token();
                worker.cancel = Some(cancel.clone());
                let mut process = worker.request.process.clone();
                process.set_env("QT_SUBAGENT_PATH", worker.id.clone());
                process.set_env(
                    "QT_SUBAGENT_PARENT_SESSION",
                    self.options.parent_session_id.clone(),
                );
                process.arguments.push("--session".into());
                process
                    .arguments
                    .push(worker.path.to_string_lossy().into_owned());
                process.stdin = Some(format!("{task}\n"));
                let generation = worker.generation;
                state.active += 1;
                let launch_error = if self.persist(state) {
                    None
                } else {
                    state.persistence_error.clone()
                };
                (process, cancel, launch_error, generation)
            };

            self.emit(&json!({
                "type": "subagent_start",
                "child_id": id,
                "generation": generation,
            }));

            let result: Result<ChildOutcome, String> = match launch_error {
                Some(error) => Err(error),
                None => self.runner.run(process, cancel.clone(), None).await,
            };
            drop(global);
            drop(local);

            let (event, sink) = {
                let mut guard = self.lock();
                let state = &mut *guard;
                state.active = state.active.saturating_sub(1);
                state.completion_count += 1;
                let stopping = state.stopping;
                let Some(worker) = state.workers.get_mut(&id) else {
                    return;
                };
                let interrupted = worker.interrupted
                    || result.as_ref().is_ok_and(|outcome| outcome.cancelled)
                    || cancel.is_cancelled();
                worker.running = false;
                worker.cancel = None;
                let failed = match &result {
                    Err(error) => {
                        worker.status = if interrupted { "interrupted" } else { "failed" }.into();
                        worker.last_error = error.clone();
                        true
                    }
                    Ok(outcome) => {
                        let has_final = outcome.has_final_result();
                        let failed = !outcome.completed() || !has_final;
                        worker.status = if interrupted {
                            "interrupted"
                        } else if failed {
                            "failed"
                        } else {
                            "completed"
                        }
                        .into();
                        worker.last_error.clear();
                        if failed {
                            worker.last_error = if !outcome.stderr.is_empty() {
                                outcome.stderr.clone()
                            } else if !has_final {
                                "child did not produce a final JSON result".into()
                            } else {
                                "child process failed".into()
                            };
                        }
                        failed
                    }
                };
                worker.event_id += 1;
                let run_again =
                    !stopping && (!interrupted || worker.wake) && !worker.mailbox.is_empty();
                worker.wake = run_again;
                let mut event = completion_event(worker, interrupted);
                match &result {
                    Ok(outcome) => {
                        event.insert("exit_code".into(), json!(outcome.exit_code));
                        event.insert("output".into(), json!(outcome.final_output));
                        event.insert("messages".into(), outcome.final_messages.clone());
                        if failed {
                            event.insert("error".into(), json!(worker.last_error));
                        }
                    }
                    Err(error) => {
                        event.insert("error".into(), json!(error));
                    }
                }
                let event = Value::Object(event);
                state.events.push(event.clone());
                self.persist(state);
                (event, state.sink.clone())
            };
            self.state_changed.notify_waiters();
            if let Some(sink) = sink {
                sink(&event);
            }
        }
    }
}

impl SubagentCoordinator {
    /// Open (and lock) the worker directory, restoring persisted workers.
    ///
    /// Like the archived coordinator, failures to create, lock or read the
    /// directory do not fail construction: they are reported by every
    /// operation afterwards (see [`Self::persistence_error`]).
    pub fn open(options: CoordinatorOptions, runner: Arc<dyn ChildRunner>) -> Self {
        let permits = Arc::new(Semaphore::new(options.concurrency.max(1)));
        let inner = Arc::new(Inner {
            options,
            runner,
            state: Mutex::new(State {
                workers: BTreeMap::new(),
                events: Vec::new(),
                sink: None,
                snapshot: None,
                active: 0,
                completion_count: 0,
                stopping: false,
                persistence_error: None,
                lock: None,
            }),
            state_changed: Notify::new(),
            shutdown: CancellationToken::new(),
            permits,
        });
        {
            let mut state = inner.lock();
            let directory = &inner.options.directory;
            match create_private_dir(directory) {
                Err(e) => state.persistence_error = Some(e.to_string()),
                Ok(()) => {
                    let lock = std::fs::OpenOptions::new()
                        .read(true)
                        .write(true)
                        .create(true)
                        .truncate(false)
                        .open(directory.join("children.lock"));
                    match lock {
                        Ok(file) if file.try_lock().is_ok() => state.lock = Some(file),
                        _ => {
                            state.persistence_error =
                                Some("another subagent coordinator owns the directory".into())
                        }
                    }
                }
            }
            if state.persistence_error.is_none() {
                if let Err(error) = inner.load_index(&mut state) {
                    state.workers.clear();
                    state.events.clear();
                    state.persistence_error = Some(format!("invalid children index: {error}"));
                } else if !state.events.is_empty() {
                    inner.persist(&mut state);
                }
            }
        }
        Self { inner }
    }

    pub fn directory(&self) -> &Path {
        &self.inner.options.directory
    }

    pub fn parent_session_id(&self) -> &str {
        &self.inner.options.parent_session_id
    }

    pub fn parent_path(&self) -> &str {
        &self.inner.options.parent_path
    }

    /// The error every operation reports, if the directory is unusable.
    pub fn persistence_error(&self) -> Option<String> {
        self.inner.lock().persistence_error.clone()
    }

    /// Receive `subagent_start` / `subagent_complete` events as they happen.
    pub fn set_event_sink(&self, sink: EventSink) {
        self.inner.lock().sink = Some(sink);
    }

    /// Provide the parent context used by `fork`.
    pub fn set_context_snapshot(&self, snapshot: ContextSnapshot) {
        self.inner.lock().snapshot = Some(snapshot);
    }

    /// Start a new named worker; returns `{child_id, generation, status, path}`
    /// without waiting for the child.
    pub fn spawn(&self, request: PersistentRequest) -> Result<Value, String> {
        if !valid_worker_name(&request.name) {
            return Err("invalid child name".into());
        }
        if request.agent.is_empty() || request.task.is_empty() {
            return Err("agent and task are required".into());
        }
        let snapshot = {
            let state = self.inner.lock();
            Inner::check_usable(&state)?;
            state.snapshot.clone()
        };
        let context = if request.fork == ForkMode::None {
            Vec::new()
        } else {
            let Some(snapshot) = snapshot else {
                return Err("fork requested without context snapshot".into());
            };
            snapshot()
        };
        let mut guard = self.inner.lock();
        let state = &mut *guard;
        if state.stopping {
            return Err("coordinator is shut down".into());
        }
        if self.inner.find_id(state, &request.name).is_some() {
            return Err("child already exists; use send with wake=true to reuse it".into());
        }
        let id = format!("{}/{}", self.inner.options.parent_path, request.name);
        let dir = self.inner.options.directory.join(&request.name);
        create_private_dir(&dir).map_err(|e| e.to_string())?;
        let mut worker = Worker::new(id.clone(), request.name.clone(), dir.join("session.jsonl"));
        worker.agent = request.agent.clone();
        worker.fork_context = fork_messages(&context, request.fork)
            .iter()
            .filter_map(|m| serde_json::to_value(m).ok())
            .collect();
        worker.request = request;
        worker.status = "idle".into();
        worker.wake = true;
        let session_cwd = worker
            .request
            .process
            .cwd
            .as_ref()
            .map(|c| c.to_string_lossy().into_owned())
            .unwrap_or_default();
        let contents = child_session_contents(
            &session_cwd,
            &self.inner.options.parent_session_id,
            json!({
                "childId": worker.id,
                "agent": worker.request.agent,
                "fork": worker.request.fork.to_value(),
                "permissions": worker.request.permissions,
            }),
            &worker.fork_context,
        );
        if write_private(&worker.path, contents.as_bytes()).is_err() {
            state.persistence_error = Some("cannot create child session".into());
            return Err("cannot create child session".into());
        }
        let result = json!({
            "child_id": worker.id,
            "generation": worker.generation,
            "status": worker.status,
            "path": worker.path.to_string_lossy(),
        });
        state.workers.insert(id.clone(), worker);
        if !self.inner.persist(state) {
            state.workers.remove(&id);
            return Err(state
                .persistence_error
                .clone()
                .unwrap_or_else(|| "cannot write children index".into()));
        }
        if let Some(worker) = state.workers.get_mut(&id) {
            self.inner.ensure_task(worker);
            worker.notify.notify_one();
        }
        drop(guard);
        self.inner.state_changed.notify_waiters();
        Ok(result)
    }

    /// Queue `message` for a worker; with `wake` an idle worker runs it.
    pub fn send(&self, target: &str, message: String, wake: bool) -> Result<Value, String> {
        if message.is_empty() {
            return Err("message is empty".into());
        }
        let mut guard = self.inner.lock();
        let state = &mut *guard;
        Inner::check_usable(state)?;
        if state.stopping {
            return Err("coordinator is shut down".into());
        }
        let id = self
            .inner
            .find_id(state, target)
            .ok_or_else(|| format!("unknown child: {target}"))?;
        let depth = {
            let worker = state.workers.get_mut(&id).expect("worker exists");
            worker.mailbox.push_back(message);
            if wake {
                worker.wake = true;
            }
            worker.mailbox.len()
        };
        if !self.inner.persist(state) {
            return Err(state
                .persistence_error
                .clone()
                .unwrap_or_else(|| "cannot write children index".into()));
        }
        let worker = state.workers.get_mut(&id).expect("worker exists");
        if wake {
            self.inner.ensure_task(worker);
        }
        worker.notify.notify_one();
        drop(guard);
        self.inner.state_changed.notify_waiters();
        Ok(json!({
            "child_id": id,
            "queued": true,
            "wake": wake,
            "mailbox_depth": depth,
        }))
    }

    /// Interrupt the current or scheduled turn, keeping the worker.
    pub fn interrupt(&self, target: &str) -> Result<Value, String> {
        let mut guard = self.inner.lock();
        let state = &mut *guard;
        Inner::check_usable(state)?;
        let id = self
            .inner
            .find_id(state, target)
            .ok_or_else(|| format!("unknown child: {target}"))?;
        let worker = state.workers.get_mut(&id).expect("worker exists");
        if worker.running {
            worker.interrupted = true;
            if let Some(cancel) = &worker.cancel {
                cancel.cancel();
            }
            worker.wake = false;
            if !self.inner.persist(state) {
                return Err(state.persistence_error.clone().unwrap_or_default());
            }
            return Ok(json!({"child_id": id, "interrupted": true, "status": "interrupting"}));
        }
        if !worker.wake && worker.status != "idle" {
            return Ok(json!({"child_id": id, "interrupted": false, "status": worker.status}));
        }
        worker.wake = false;
        worker.interrupted = true;
        worker.status = "interrupted".into();
        worker.event_id += 1;
        let mut event = completion_event(worker, true);
        event.insert("queued".into(), json!(true));
        let event = Value::Object(event);
        state.completion_count += 1;
        state.events.push(event.clone());
        if !self.inner.persist(state) {
            return Err(state.persistence_error.clone().unwrap_or_default());
        }
        let sink = state.sink.clone();
        drop(guard);
        self.inner.state_changed.notify_waiters();
        if let Some(sink) = sink {
            sink(&event);
        }
        Ok(json!({"child_id": id, "interrupted": true, "status": "interrupted"}))
    }

    /// `{children: [...]}` with identity, status, generation, permissions and
    /// mailbox of every worker.
    pub fn list(&self) -> Result<Value, String> {
        let state = self.inner.lock();
        Inner::check_usable(&state)?;
        let children: Vec<Value> = state
            .workers
            .values()
            .map(|w| self.inner.worker_json(w))
            .collect();
        Ok(json!({ "children": children }))
    }

    /// Wait up to `timeout` for `target`'s scheduled turns to finish. With an
    /// empty target, wait for any completion or pending event.
    pub async fn wait(&self, target: &str, timeout: Duration) -> Result<Value, String> {
        let deadline = tokio::time::Instant::now() + timeout;
        let (id, before) = {
            let state = self.inner.lock();
            Inner::check_usable(&state)?;
            if target.is_empty() {
                (None, state.completion_count)
            } else {
                let id = self
                    .inner
                    .find_id(&state, target)
                    .ok_or_else(|| format!("unknown child: {target}"))?;
                (Some(id), state.completion_count)
            }
        };
        loop {
            let notified = self.inner.state_changed.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            let check = |state: &State| -> (bool, Value) {
                match &id {
                    None => {
                        let done = state.stopping
                            || !state.events.is_empty()
                            || state.completion_count > before;
                        (
                            done,
                            json!({"completed": done, "status": if done { "completed" } else { "timeout" }}),
                        )
                    }
                    Some(id) => match state.workers.get(id) {
                        Some(worker) => {
                            let done = !worker.running && !worker.wake;
                            (
                                done,
                                json!({
                                    "child_id": id,
                                    "completed": done,
                                    "status": worker.status,
                                    "event_id": worker.event_id,
                                }),
                            )
                        }
                        None => (true, json!({"child_id": id, "completed": true})),
                    },
                }
            };
            {
                let state = self.inner.lock();
                let (done, value) = check(&state);
                if done {
                    return Ok(value);
                }
            }
            if tokio::time::timeout_at(deadline, notified).await.is_err() {
                let state = self.inner.lock();
                return Ok(check(&state).1);
            }
        }
    }

    /// Remove and return up to `maximum` pending events.
    pub fn take_events(&self, maximum: usize) -> Vec<Value> {
        let mut guard = self.inner.lock();
        let state = &mut *guard;
        if state.persistence_error.is_some() || maximum == 0 {
            return Vec::new();
        }
        let count = maximum.min(state.events.len());
        let taken: Vec<Value> = state.events.drain(..count).collect();
        if taken.is_empty() {
            return taken;
        }
        if !state.stopping && !self.inner.persist(state) {
            let rest = std::mem::take(&mut state.events);
            state.events = taken;
            state.events.extend(rest);
            return Vec::new();
        }
        taken
    }

    /// Pending events without removing them.
    pub fn pending_events(&self) -> Vec<Value> {
        self.inner.lock().events.clone()
    }

    /// Drop a delivered event from the persisted pending list.
    pub fn acknowledge_event(&self, child_id: &str, event_id: u64) -> Result<(), String> {
        let mut guard = self.inner.lock();
        let state = &mut *guard;
        if let Some(error) = &state.persistence_error {
            return Err(error.clone());
        }
        if state.lock.is_none() {
            return Err("coordinator no longer owns its index".into());
        }
        let previous = state.events.clone();
        state.events.retain(|event| {
            !(event.get("child_id").and_then(Value::as_str) == Some(child_id)
                && event.get("event_id").and_then(Value::as_u64) == Some(event_id))
        });
        if !self.inner.persist(state) {
            state.events = previous;
            return Err(state.persistence_error.clone().unwrap_or_default());
        }
        Ok(())
    }

    /// Whether a worker is running or scheduled.
    pub fn has_active_workers(&self) -> bool {
        let state = self.inner.lock();
        if state.stopping {
            return false;
        }
        state.active != 0 || state.workers.values().any(|w| w.running || w.wake)
    }

    fn begin_shutdown(&self) -> Vec<JoinHandle<()>> {
        let mut state = self.inner.lock();
        let mut tasks = Vec::new();
        if !state.stopping {
            state.stopping = true;
            for worker in state.workers.values_mut() {
                worker.stop = true;
                worker.wake = false;
                if let Some(cancel) = &worker.cancel {
                    cancel.cancel();
                }
                worker.notify.notify_one();
                if let Some(task) = worker.task.take() {
                    tasks.push(task);
                }
            }
        }
        drop(state);
        self.inner.shutdown.cancel();
        self.inner.state_changed.notify_waiters();
        tasks
    }

    /// Cancel running children, join the worker tasks and release the lock.
    pub async fn shutdown(&self) {
        for task in self.begin_shutdown() {
            let _ = task.await;
        }
        self.inner.lock().lock = None;
    }
}

impl Drop for SubagentCoordinator {
    fn drop(&mut self) {
        // Worker tasks exit once cancelled; the lock is released with the
        // last reference to the shared state.
        let _ = self.begin_shutdown();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::message::{AssistantMessage, CustomMessage, UserContent};

    #[test]
    fn hash_matches_archived_fnv1a() {
        assert_eq!(stable_hash_hex(""), "cbf29ce484222325");
        assert_eq!(stable_hash_hex("a"), "af63dc4c8601ec8c");
        assert_eq!(
            coordinator_directory(Path::new("/h"), "a"),
            PathBuf::from("/h/subagents/af63dc4c8601ec8c")
        );
    }

    #[test]
    fn worker_names() {
        assert!(valid_worker_name("review-1.a_b"));
        assert!(!valid_worker_name(""));
        assert!(!valid_worker_name(".hidden"));
        assert!(!valid_worker_name("a/b"));
        assert!(!valid_worker_name(&"a".repeat(129)));
        assert!(valid_worker_name(&"a".repeat(128)));
    }

    #[test]
    fn fork_modes() {
        assert_eq!(ForkMode::from_value(&json!("all")), Some(ForkMode::All));
        assert_eq!(ForkMode::from_value(&json!("3")), Some(ForkMode::Turns(3)));
        assert_eq!(ForkMode::from_value(&json!(2)), Some(ForkMode::Turns(2)));
        assert_eq!(ForkMode::from_value(&json!(0)), None);
        assert_eq!(ForkMode::from_value(&json!("0")), None);
        assert_eq!(ForkMode::from_value(&json!("some")), None);
        assert_eq!(ForkMode::Turns(4).to_value(), json!(4));
    }

    fn user(text: &str) -> AgentMessage {
        AgentMessage::Llm(Message::user_text(text))
    }

    fn assistant_call(id: &str) -> AgentMessage {
        AgentMessage::Llm(Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: id.into(),
                name: "subagent".into(),
                arguments: json!({}),
            }],
            ..Default::default()
        }))
    }

    #[test]
    fn fork_selects_last_user_turns() {
        let context = vec![
            user("one"),
            assistant_call("a"),
            user("two"),
            AgentMessage::Custom(CustomMessage::new(
                "note",
                UserContent::Text("x".into()),
                true,
            )),
            user("three"),
        ];
        assert_eq!(fork_messages(&context, ForkMode::None).len(), 0);
        assert_eq!(fork_messages(&context, ForkMode::All).len(), 5);
        assert_eq!(fork_messages(&context, ForkMode::Turns(1)).len(), 1);
        assert_eq!(fork_messages(&context, ForkMode::Turns(2)).len(), 3);
        assert_eq!(fork_messages(&context, ForkMode::Turns(9)).len(), 5);
    }

    #[test]
    fn child_session_closes_in_flight_calls() {
        let context: Vec<Value> = [user("go"), assistant_call("call-1")]
            .iter()
            .map(|m| serde_json::to_value(m).unwrap())
            .collect();
        let text =
            child_session_contents("/w", "parent-1", json!({"childId": "/root/x"}), &context);
        let lines: Vec<Value> = text
            .lines()
            .map(|l| serde_json::from_str(l).unwrap())
            .collect();
        assert_eq!(lines.len(), 4);
        assert_eq!(lines[0]["type"], "session");
        assert_eq!(lines[0]["version"], 3);
        assert_eq!(lines[0]["parentSession"], "parent-1");
        assert_eq!(lines[0]["subagent"]["childId"], "/root/x");
        assert_eq!(lines[1]["parentId"], Value::Null);
        assert_eq!(lines[2]["parentId"], lines[1]["id"]);
        let closing = &lines[3]["message"];
        assert_eq!(closing["role"], "toolResult");
        assert_eq!(closing["toolCallId"], "call-1");
        assert_eq!(closing["isError"], true);
        assert_eq!(closing["content"][0]["text"], FORK_INTERRUPTED_TEXT);
        // The file is a valid qt-core session.
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("session.jsonl");
        std::fs::write(&path, &text).unwrap();
        let session = qt_core::session::Session::open(&path).unwrap();
        assert_eq!(session.build_context().messages.len(), 3);
    }

    #[test]
    fn process_round_trip() {
        let mut request = ChildRequest {
            arguments: vec!["--mode".into(), "json".into()],
            cwd: Some(PathBuf::from("/w")),
            stdin: Some("hi".into()),
            timeout_ms: Some(5),
            ..Default::default()
        };
        request.set_env("A", "1");
        request.environment.push(("B".into(), None));
        let back = process_from_value(&process_to_value(&request)).unwrap();
        assert_eq!(back, request);
    }
}
