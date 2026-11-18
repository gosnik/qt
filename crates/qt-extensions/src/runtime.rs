//! [`Extensions`]: the loaded extensions of one engine, their commands,
//! tools, flags, providers and resources, host-call dispatch, and lifecycle
//! event routing through `qt_core` hooks (archived
//! `extension_runtime_binding.cpp`, `extension_lifecycle_runner.cpp`,
//! `extension_contribution_runner.cpp`, `harness_lifecycle.cpp`).

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock, Weak};
use std::time::Duration;

use serde_json::{Map, Value, json};
use tokio_util::sync::CancellationToken;

use qt_core::agent::AgentEvent;
use qt_core::engine::Delivery;
use qt_core::harness::hooks::{
    BeforeAgentStartEvent, BeforeAgentStartResult, BeforeCompactEvent, BeforeCompactResult,
    BeforeForkResult, BeforeTreeResult, DiscoveredResourcePaths, InputEvent,
    InputOutcome as CoreInputOutcome, InputSource as CoreInputSource, ProviderRequestEvent,
    ProviderResponseEvent, ToolCallDecision, ToolCallEvent, ToolResultEvent, ToolResultPatch,
};
use qt_core::harness::{HookId, Hooks, SessionEvent, SessionStartReason};
use qt_core::message::{AgentMessage, ContentBlock, CustomMessage, Message};
use qt_core::provider::{Request, StreamEvent, ThinkingLevel};

use crate::events::{self, DiscoveredResources, InputAction, InputSource};
use crate::filters::display_path;
use crate::host::{ExtensionHost, exec, parse_content, usage_json, user_content};
use crate::jsonrpc::{COMMAND_INVOKE, EVENT_PREFIX, Message as RpcMessage, TOOL_INVOKE};
use crate::loader::{
    DiscoveryOptions, ExtensionOrigin, LoadResult, LoadedExtension, discover_extensions,
};
use crate::manifest::{FlagContribution, ProviderContribution, Transport};
use crate::registration::handler_name;
use crate::script::HostCall;
use crate::subprocess;
use crate::ui::{CallbackFactory, ExtensionUi, HandlerCallback, HeadlessUi, dispatch_ui};

/// Reports hook failures and extension diagnostics (stderr by default).
pub type Reporter = Arc<dyn Fn(&str) + Send + Sync>;

/// Lists every slash command for `pi.getCommands` as `(name, description, source)`.
pub type CommandLister = Arc<dyn Fn() -> Vec<(String, String, String)> + Send + Sync>;

/// One loaded extension, for `/extensions`.
#[derive(Debug, Clone, PartialEq)]
pub struct ExtensionSummary {
    pub name: String,
    pub display_name: Option<String>,
    pub path: PathBuf,
    pub transport: Transport,
    pub origin: ExtensionOrigin,
    pub commands: Vec<String>,
    pub tools: Vec<String>,
    pub events: Vec<String>,
}

/// A slash command contributed by an extension.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtensionCommand {
    pub name: String,
    pub title: Option<String>,
    pub description: Option<String>,
    pub extension: String,
}

/// A flag registration with its owning extension.
#[derive(Debug, Clone, PartialEq)]
pub struct ExtensionFlag {
    pub flag: FlagContribution,
    pub extension: String,
}

/// A provider registration with its owning extension.
#[derive(Debug, Clone, PartialEq)]
pub struct ExtensionProvider {
    pub provider: ProviderContribution,
    pub extension: String,
}

/// A skill, prompt or theme contributed by an extension (absolute path).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtensionResource {
    pub kind: String,
    pub path: PathBuf,
    pub name: Option<String>,
    pub extension: String,
}

/// A keyboard shortcut contributed by an extension.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtensionShortcut {
    pub key: String,
    pub description: Option<String>,
    pub extension: String,
}

/// What a command produced.
#[derive(Debug, Clone, PartialEq)]
pub struct CommandOutcome {
    /// Transcript lines (archived formatting of the result).
    pub lines: Vec<String>,
    pub value: Value,
}

/// Result of the `input` event chain.
#[derive(Debug, Clone, PartialEq)]
pub enum InputOutcome {
    /// Submit this (possibly transformed) input.
    Continue { text: String, images: Value },
    /// An extension consumed the input.
    Handled,
}

/// Result of `session_before_tree`.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct BeforeTreeOutcome {
    pub cancel: bool,
    pub summary: Option<Value>,
    pub custom_instructions: Option<String>,
    pub replace_instructions: Option<bool>,
    pub label: Option<String>,
}

/// Lines for a command result (archived `append_extension_result_text`).
pub fn result_lines(value: &Value) -> Vec<String> {
    let mut lines = Vec::new();
    append_result_text(&mut lines, value);
    lines
}

fn append_result_text(lines: &mut Vec<String>, value: &Value) {
    match value {
        Value::Null => {}
        Value::String(s) => lines.push(s.clone()),
        Value::Array(items) => items.iter().for_each(|i| append_result_text(lines, i)),
        Value::Object(o) => {
            for field in ["message", "text", "output"] {
                if let Some(Value::String(s)) = o.get(field) {
                    lines.push(s.clone());
                    return;
                }
            }
            if let Some(Value::Array(content)) = o.get("content") {
                let before = lines.len();
                for item in content {
                    match item {
                        Value::String(s) => lines.push(s.clone()),
                        Value::Object(b) => {
                            let is_text = b
                                .get("type")
                                .and_then(Value::as_str)
                                .is_none_or(|t| t == "text");
                            if let (true, Some(Value::String(t))) = (is_text, b.get("text")) {
                                lines.push(t.clone());
                            }
                        }
                        _ => {}
                    }
                }
                if lines.len() > before {
                    return;
                }
            }
            lines.push(value.to_string());
        }
        other => lines.push(other.to_string()),
    }
}

/// `--name`, `--name=value` and `--name value` arguments of registered flags.
fn split_flag(arg: &str) -> Option<(&str, Option<&str>)> {
    let body = arg.strip_prefix("--")?;
    if body.is_empty() {
        return None;
    }
    Some(match body.split_once('=') {
        Some((n, v)) => (n, Some(v)),
        None => (body, None),
    })
}

#[derive(Default)]
struct TurnState {
    index: u64,
    open: bool,
    message: Value,
    tool_results: Vec<Value>,
    tool_names: HashMap<String, String>,
}

struct Inner {
    options: DiscoveryOptions,
    loaded: RwLock<Arc<LoadResult>>,
    host: RwLock<Option<Arc<dyn ExtensionHost>>>,
    ui: RwLock<Arc<dyn ExtensionUi>>,
    reporter: RwLock<Reporter>,
    commands: RwLock<Option<CommandLister>>,
    flag_values: RwLock<Map<String, Value>>,
    request_id: AtomicU64,
    timeout: Duration,
    installed: Mutex<Option<(Hooks, Vec<HookId>)>>,
    turn: Mutex<TurnState>,
}

/// Where a handler runs.
#[derive(Debug, Clone)]
struct Target {
    index: usize,
    /// The Rhai function; `None` for subprocess extensions.
    handler: Option<String>,
}

/// The loaded extensions of an engine (cheaply cloneable).
#[derive(Clone)]
pub struct Extensions {
    inner: Arc<Inner>,
}

impl std::fmt::Debug for Extensions {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Extensions")
            .field("count", &self.snapshot().extensions.len())
            .finish_non_exhaustive()
    }
}

fn default_reporter() -> Reporter {
    Arc::new(|message: &str| eprintln!("{message}"))
}

fn invalid_args(method: &str) -> String {
    format!("invalid arguments for extension host API: {method}")
}

fn arg_str<'a>(args: &'a Value, field: &str, method: &str) -> Result<&'a str, String> {
    args.get(field)
        .and_then(Value::as_str)
        .ok_or_else(|| invalid_args(method))
}

fn stream_json(event: &StreamEvent) -> Value {
    match event {
        StreamEvent::BlockStart { index, block } => {
            json!({ "type": "block_start", "contentIndex": index, "block": block })
        }
        StreamEvent::TextDelta { index, delta } => {
            json!({ "type": "text_delta", "contentIndex": index, "delta": delta })
        }
        StreamEvent::ThinkingDelta { index, delta } => {
            json!({ "type": "thinking_delta", "contentIndex": index, "delta": delta })
        }
        StreamEvent::ToolArgsDelta { index, delta } => {
            json!({ "type": "toolcall_delta", "contentIndex": index, "delta": delta })
        }
    }
}

fn now_millis() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn request_json(request: &Request) -> Value {
    json!({
        "model": request.model,
        "systemPrompt": request.system,
        "messages": request.messages,
        "tools": request.tools.iter().map(|t| json!({"name": t.name, "description": t.description, "parameters": t.parameters})).collect::<Vec<_>>(),
        "maxTokens": request.max_tokens,
        "thinkingLevel": request.thinking.as_str(),
        "temperature": request.temperature,
    })
}

fn start_reason(reason: SessionStartReason) -> &'static str {
    match reason {
        SessionStartReason::Startup => "startup",
        SessionStartReason::New => "new",
        SessionStartReason::Resume => "resume",
        SessionStartReason::Fork => "fork",
        SessionStartReason::Clone => "clone",
        SessionStartReason::Import => "import",
    }
}

impl Extensions {
    /// Discover and load extensions.
    pub fn load(options: DiscoveryOptions) -> Self {
        let result = discover_extensions(&options);
        Self::from_result(options, result)
    }

    /// Wrap an already loaded set (tests, custom discovery).
    pub fn from_result(options: DiscoveryOptions, result: LoadResult) -> Self {
        Self {
            inner: Arc::new(Inner {
                options,
                loaded: RwLock::new(Arc::new(result)),
                host: RwLock::new(None),
                ui: RwLock::new(Arc::new(HeadlessUi)),
                reporter: RwLock::new(default_reporter()),
                commands: RwLock::new(None),
                flag_values: RwLock::new(Map::new()),
                request_id: AtomicU64::new(1),
                timeout: subprocess::DEFAULT_TIMEOUT,
                installed: Mutex::new(None),
                turn: Mutex::new(TurnState::default()),
            }),
        }
    }

    /// The current load result.
    pub fn snapshot(&self) -> Arc<LoadResult> {
        self.inner.loaded.read().expect("extensions lock").clone()
    }

    pub fn diagnostics(&self) -> Vec<String> {
        self.snapshot().diagnostics.clone()
    }

    /// Session operations for host calls (the active conversation).
    pub fn set_host(&self, host: Option<Arc<dyn ExtensionHost>>) {
        *self.inner.host.write().expect("host lock") = host;
    }

    /// The presenter for `ctx.ui.*` (headless by default).
    pub fn set_ui(&self, ui: Arc<dyn ExtensionUi>) {
        *self.inner.ui.write().expect("ui lock") = ui;
    }

    pub fn set_reporter(&self, reporter: Reporter) {
        *self.inner.reporter.write().expect("reporter lock") = reporter;
    }

    /// Full command list for `pi.getCommands` (default: extension commands).
    pub fn set_command_lister(&self, lister: CommandLister) {
        *self.inner.commands.write().expect("commands lock") = Some(lister);
    }

    fn report(&self, message: &str) {
        let r = self.inner.reporter.read().expect("reporter lock").clone();
        r(message);
    }

    /// Reload from disk: `session_shutdown` (reason `reload`) to the old set,
    /// then discovery again and `session_start` (reason `reload`). Flag
    /// values and hooks stay in place; UI callbacks registered by the old set
    /// should be cleared by the front end.
    pub fn reload(&self) -> Arc<LoadResult> {
        self.emit_session_shutdown("reload", None);
        let result = Arc::new(discover_extensions(&self.inner.options));
        *self.inner.loaded.write().expect("extensions lock") = result.clone();
        let session_file = None::<String>;
        self.observe(
            "session_start",
            json!({ "reason": "reload", "sessionFile": session_file }),
        );
        result
    }

    /// Loaded extensions for `/extensions`.
    pub fn summaries(&self) -> Vec<ExtensionSummary> {
        self.snapshot()
            .extensions
            .iter()
            .map(|e| ExtensionSummary {
                name: e.manifest.name.clone(),
                display_name: e.manifest.display_name.clone(),
                path: e.path.clone(),
                transport: e.manifest.transport,
                origin: e.origin.clone(),
                commands: e.manifest.commands.iter().map(|c| c.name.clone()).collect(),
                tools: e.manifest.tools.iter().map(|t| t.name.clone()).collect(),
                events: crate::registration::LIFECYCLE_EVENTS
                    .iter()
                    .filter(|ev| e.manifest.handles_event(ev))
                    .map(|ev| ev.to_string())
                    .collect(),
            })
            .collect()
    }

    // ----- contributions -----------------------------------------------------

    pub fn commands(&self) -> Vec<ExtensionCommand> {
        self.snapshot()
            .extensions
            .iter()
            .flat_map(|e| {
                e.manifest.commands.iter().map(|c| ExtensionCommand {
                    name: c.name.clone(),
                    title: c.title.clone(),
                    description: c.description.clone(),
                    extension: e.manifest.name.clone(),
                })
            })
            .collect()
    }

    pub fn flags(&self) -> Vec<ExtensionFlag> {
        self.snapshot()
            .extensions
            .iter()
            .flat_map(|e| {
                e.manifest.flags.iter().map(|f| ExtensionFlag {
                    flag: f.clone(),
                    extension: e.manifest.name.clone(),
                })
            })
            .collect()
    }

    pub fn providers(&self) -> Vec<ExtensionProvider> {
        self.snapshot()
            .extensions
            .iter()
            .flat_map(|e| {
                e.manifest.providers.iter().map(|p| ExtensionProvider {
                    provider: p.clone(),
                    extension: e.manifest.name.clone(),
                })
            })
            .collect()
    }

    /// Provider names removed by `pi.unregisterProvider` (reference modules).
    pub fn provider_unregistrations(&self) -> Vec<String> {
        self.snapshot()
            .extensions
            .iter()
            .filter_map(|e| {
                e.manifest
                    .metadata
                    .get("provider_unregistrations")?
                    .as_array()
                    .cloned()
            })
            .flatten()
            .filter_map(|v| v.get("name")?.as_str().map(str::to_string))
            .collect()
    }

    /// Apply provider registrations to a model catalog: each config is a
    /// `models.json` provider entry (Pi `registerProvider`); unregistered
    /// providers lose their models. Returns one message per rejected config.
    pub fn apply_providers(&self, catalog: &mut qt_core::catalog::ModelCatalog) -> Vec<String> {
        let mut errors = Vec::new();
        for name in self.provider_unregistrations() {
            catalog.remove_models_of(&name);
        }
        for p in self.providers() {
            if let Err(e) = catalog.apply_provider_config(&p.provider.name, &p.provider.config) {
                errors.push(format!(
                    "extension {} provider {}: {e}",
                    p.extension, p.provider.name
                ));
            }
        }
        errors
    }

    pub fn resources(&self) -> Vec<ExtensionResource> {
        self.snapshot()
            .extensions
            .iter()
            .flat_map(|e| {
                e.script
                    .iter()
                    .flat_map(|s| s.registration().resources.clone())
                    .map(|r| ExtensionResource {
                        kind: r.kind,
                        path: e.directory.join(&r.path),
                        name: r.name,
                        extension: e.manifest.name.clone(),
                    })
                    .collect::<Vec<_>>()
            })
            .collect()
    }

    pub fn shortcuts(&self) -> Vec<ExtensionShortcut> {
        self.snapshot()
            .extensions
            .iter()
            .flat_map(|e| {
                e.script
                    .iter()
                    .flat_map(|s| s.registration().shortcuts.clone())
                    .map(|s| ExtensionShortcut {
                        key: s.key,
                        description: s.description,
                        extension: e.manifest.name.clone(),
                    })
                    .collect::<Vec<_>>()
            })
            .collect()
    }

    /// Custom message types that have a renderer.
    pub fn renderer_types(&self) -> Vec<String> {
        self.snapshot()
            .extensions
            .iter()
            .flat_map(|e| {
                e.script
                    .iter()
                    .flat_map(|s| {
                        s.registration()
                            .message_renderers
                            .iter()
                            .map(|r| r.custom_type.clone())
                    })
                    .collect::<Vec<_>>()
            })
            .collect()
    }

    // ----- flags -------------------------------------------------------------

    fn find_flag(&self, name: &str) -> Option<FlagContribution> {
        self.flags()
            .into_iter()
            .find(|f| f.flag.name == name)
            .map(|f| f.flag)
    }

    /// Explicit value, else the registered default, else `null`.
    pub fn flag_value(&self, name: &str) -> Value {
        if let Some(v) = self.inner.flag_values.read().expect("flags lock").get(name) {
            return v.clone();
        }
        self.find_flag(name)
            .map(|f| f.default)
            .unwrap_or(Value::Null)
    }

    /// Set explicit flag values (`{name: value}`) with the archived checks:
    /// booleans accept `true`/`false`/`"true"`/`"false"`/`"1"`/`"0"`, strings need a string.
    pub fn set_flag_values(&self, values: &Map<String, Value>) -> Result<(), String> {
        let mut resolved = Map::new();
        for (name, value) in values {
            let flag = self
                .find_flag(name)
                .ok_or_else(|| format!("Unknown option: --{name}"))?;
            let v = if flag.flag_type == "boolean" {
                match value {
                    Value::Bool(b) => json!(*b),
                    Value::String(s) if s == "true" || s == "1" => json!(true),
                    Value::String(s) if s == "false" || s == "0" => json!(false),
                    _ => return Err(format!("Extension flag --{name} expects a boolean value")),
                }
            } else {
                match value {
                    Value::String(_) => value.clone(),
                    _ => return Err(format!("Extension flag --{name} requires a value")),
                }
            };
            resolved.insert(name.clone(), v);
        }
        self.inner
            .flag_values
            .write()
            .expect("flags lock")
            .extend(resolved);
        Ok(())
    }

    /// Consume registered extension flags from CLI arguments the main parser
    /// did not recognise. Boolean flags take no value (`--x=false` allowed);
    /// string flags take `--x=value` or the next argument. Other arguments
    /// are returned unchanged, in order.
    pub fn apply_cli_flags(&self, args: &[String]) -> Result<Vec<String>, String> {
        let mut values = Map::new();
        let mut rest = Vec::new();
        let mut i = 0;
        while i < args.len() {
            let arg = &args[i];
            let Some((name, inline)) = split_flag(arg) else {
                rest.push(arg.clone());
                i += 1;
                continue;
            };
            let Some(flag) = self.find_flag(name) else {
                rest.push(arg.clone());
                i += 1;
                continue;
            };
            let value = match (flag.flag_type.as_str(), inline) {
                ("boolean", None) => json!(true),
                (_, Some(v)) => json!(v),
                (_, None) => match args.get(i + 1) {
                    Some(next) => {
                        i += 1;
                        json!(next)
                    }
                    None => return Err(format!("Extension flag --{name} requires a value")),
                },
            };
            values.insert(name.to_string(), value);
            i += 1;
        }
        self.set_flag_values(&values)?;
        Ok(rest)
    }

    // ----- invocation ----------------------------------------------------------

    fn next_id(&self) -> Value {
        json!(self.inner.request_id.fetch_add(1, Ordering::Relaxed))
    }

    fn host_call_for(&self, snapshot: &Arc<LoadResult>, index: usize) -> HostCall {
        let weak: Weak<Inner> = Arc::downgrade(&self.inner);
        let snapshot = snapshot.clone();
        Arc::new(move |method: &str, args: Value| {
            let inner = weak
                .upgrade()
                .ok_or_else(|| "extension host runtime is no longer available".to_string())?;
            Extensions { inner }.dispatch(&snapshot, index, method, &args)
        })
    }

    fn callback_for(
        &self,
        snapshot: &Arc<LoadResult>,
        index: usize,
        handler: &str,
    ) -> HandlerCallback {
        let this = Arc::downgrade(&self.inner);
        let snapshot = snapshot.clone();
        let handler = handler.to_string();
        Arc::new(move |event: Value| {
            let inner = this
                .upgrade()
                .ok_or_else(|| "extension host runtime is no longer available".to_string())?;
            let ext = Extensions { inner };
            let script = snapshot.extensions[index]
                .script
                .clone()
                .ok_or_else(|| "extension handler is unavailable".to_string())?;
            script.invoke(&handler, &event, ext.host_call_for(&snapshot, index))
        })
    }

    fn invoke_rhai(
        &self,
        snapshot: &Arc<LoadResult>,
        index: usize,
        handler: &str,
        args: &Value,
    ) -> Result<Value, String> {
        let script = snapshot.extensions[index]
            .script
            .clone()
            .ok_or_else(|| "Rhai runtime was not loaded".to_string())?;
        script.invoke(handler, args, self.host_call_for(snapshot, index))
    }

    fn invoke_subprocess(
        &self,
        ext: &LoadedExtension,
        method: &str,
        params: Value,
        cancel: Option<&CancellationToken>,
    ) -> Result<Value, String> {
        let plan = subprocess::launch_plan(ext)?;
        let request = RpcMessage::request(self.next_id(), method, params)?;
        let r = subprocess::invoke(&plan, &request, self.inner.timeout, cancel)?;
        for d in &r.diagnostics {
            self.report(&format!("[{}] {d}", ext.manifest.name));
        }
        r.outcome.map_err(|e| e.message)
    }

    /// Host API dispatch for the extension at `index`.
    fn dispatch(
        &self,
        snapshot: &Arc<LoadResult>,
        index: usize,
        method: &str,
        args: &Value,
    ) -> Result<Value, String> {
        if !args.is_object() {
            return Err("extension host arguments must be an object".into());
        }
        if method.starts_with("ctx.ui.") {
            let ui = self.inner.ui.read().expect("ui lock").clone();
            let factory = |handler: &str| self.callback_for(snapshot, index, handler);
            let f: CallbackFactory<'_> = &factory;
            return dispatch_ui(ui.as_ref(), method, args, Some(f));
        }
        match method {
            "pi.getFlag" => return Ok(self.flag_value(arg_str(args, "name", method)?)),
            "pi.getCommands" => {
                let lister = self.inner.commands.read().expect("commands lock").clone();
                let list = match lister {
                    Some(l) => l(),
                    None => self
                        .commands()
                        .into_iter()
                        .map(|c| {
                            (
                                c.name,
                                c.description.unwrap_or_default(),
                                "extension".into(),
                            )
                        })
                        .collect(),
                };
                return Ok(list
                    .into_iter()
                    .map(|(name, description, source)| json!({"name": name, "description": description, "source": source}))
                    .collect());
            }
            _ => {}
        }
        let host = self
            .inner
            .host
            .read()
            .expect("host lock")
            .clone()
            .ok_or_else(|| "extension host is not available".to_string())?;
        match method {
            "pi.exec" => {
                let command = arg_str(args, "command", method)?;
                let argv: Vec<String> = match args.get("args") {
                    None | Some(Value::Null) => Vec::new(),
                    Some(v) => serde_json::from_value(v.clone()).map_err(|_| invalid_args(method))?,
                };
                let cwd = match args.get("cwd").and_then(Value::as_str) {
                    Some(c) => host.cwd().join(c),
                    None => host.cwd(),
                };
                let timeout = args.get("timeout").and_then(Value::as_i64).unwrap_or(30_000);
                if timeout <= 0 {
                    return Err("exec timeout must be positive".into());
                }
                let out = exec(command, &argv, &cwd, Duration::from_millis(timeout as u64))?;
                Ok(json!({"stdout": out.stdout, "stderr": out.stderr, "code": out.code, "killed": out.killed}))
            }
            "pi.getSessionName" => Ok(host.session_name().map(Value::String).unwrap_or(Value::Null)),
            "pi.setSessionName" => {
                host.set_session_name(arg_str(args, "name", method)?)?;
                Ok(Value::Null)
            }
            "pi.appendEntry" => {
                let custom_type = arg_str(args, "customType", method)?;
                host.append_entry(custom_type, args.get("data").cloned().unwrap_or(Value::Null))?;
                Ok(Value::Null)
            }
            "pi.setLabel" => {
                let entry = arg_str(args, "entryId", method)?;
                let label = match args.get("label") {
                    None | Some(Value::Null) => None,
                    Some(Value::String(s)) => Some(s.as_str()),
                    Some(_) => return Err(invalid_args(method)),
                };
                host.set_label(entry, label)?;
                Ok(Value::Null)
            }
            "pi.sendMessage" => {
                let custom_type = arg_str(args, "customType", method)?;
                if custom_type.is_empty() {
                    return Err("customType must not be empty".into());
                }
                let content = parse_content(args.get("content").ok_or_else(|| invalid_args(method))?)?;
                let display = args.get("display").and_then(Value::as_bool).unwrap_or(true);
                let mut message = CustomMessage::new(custom_type, user_content(content), display);
                message.details = args.get("details").filter(|d| !d.is_null()).cloned();
                let delivery = match args.get("deliverAs").and_then(Value::as_str).unwrap_or("steer") {
                    "steer" => Delivery::Steer,
                    "followUp" => Delivery::FollowUp,
                    "nextTurn" => Delivery::NextTurn,
                    _ => return Err("invalid extension message delivery mode".into()),
                };
                let trigger = args.get("triggerTurn").and_then(Value::as_bool).unwrap_or(false);
                host.send_message(message, delivery, trigger)?;
                Ok(Value::Null)
            }
            "pi.sendUserMessage" => {
                let content = parse_content(args.get("content").ok_or_else(|| invalid_args(method))?)?;
                let delivery = match args.get("deliverAs").and_then(Value::as_str).unwrap_or("steer") {
                    "steer" => Delivery::Steer,
                    "followUp" => Delivery::FollowUp,
                    _ => return Err("invalid extension message delivery mode".into()),
                };
                host.send_user_message(content, delivery)?;
                Ok(Value::Null)
            }
            "pi.getActiveTools" => Ok(json!(host.active_tools())),
            "pi.getAllTools" => Ok(host
                .all_tools()
                .into_iter()
                .map(|t| json!({"name": t.name, "description": t.description, "parameters": t.parameters}))
                .collect()),
            "pi.setActiveTools" => {
                let names: Vec<String> = args
                    .get("toolNames")
                    .and_then(|v| serde_json::from_value(v.clone()).ok())
                    .ok_or_else(|| invalid_args(method))?;
                let known: Vec<String> = host.all_tools().into_iter().map(|t| t.name).collect();
                if let Some(unknown) = names.iter().find(|n| !known.contains(n)) {
                    return Err(format!("unknown active tool: {unknown}"));
                }
                host.set_active_tools(&names)?;
                Ok(Value::Null)
            }
            "pi.getThinkingLevel" => Ok(json!(host.thinking_level())),
            "pi.setThinkingLevel" => {
                let level = arg_str(args, "level", method)?;
                let level = ThinkingLevel::ALL
                    .into_iter()
                    .find(|l| l.as_str() == level)
                    .ok_or_else(|| "invalid thinking level".to_string())?;
                host.set_thinking_level(level)?;
                Ok(Value::Null)
            }
            "pi.setModel" => {
                let provider = arg_str(args, "provider", method)?;
                let id = arg_str(args, "id", method)?;
                Ok(json!(host.set_model(provider, id)?))
            }
            "ctx.getModel" => {
                if args.get("provider").is_some() || args.get("id").is_some() {
                    let (Some(p), Some(i)) = (
                        args.get("provider").and_then(Value::as_str),
                        args.get("id").and_then(Value::as_str),
                    ) else {
                        return Err("ctx.getModel requires string provider and id fields".into());
                    };
                    return Ok(host.find_model(p, i).unwrap_or(Value::Null));
                }
                Ok(host.model().unwrap_or(Value::Null))
            }
            "ctx.getModelCatalog" => Ok(host.model_catalog()),
            "ctx.getSystemPrompt" => Ok(json!(host.system_prompt())),
            "ctx.getContextUsage" => Ok(usage_json(host.context_usage())),
            "ctx.getContext" => Ok(json!({
                "systemPrompt": host.system_prompt(),
                "messages": events::messages_json(&host.context_messages()),
                "thinkingLevel": host.thinking_level(),
                "model": host.model(),
                "usage": usage_json(host.context_usage()),
            })),
            "ctx.compact" => {
                let instructions = match args.get("customInstructions") {
                    None | Some(Value::Null) => None,
                    Some(Value::String(s)) => Some(s.as_str()),
                    Some(_) => return Err("ctx.compact customInstructions must be a string or null".into()),
                };
                if !host.is_idle() {
                    return Err("extension runtime mutation requires an idle harness".into());
                }
                let r = host.compact(instructions)?;
                Ok(events::compaction_result_json(&r))
            }
            "ctx.isIdle" => Ok(json!(host.is_idle())),
            "ctx.hasPendingMessages" => Ok(json!(host.has_pending_messages())),
            "ctx.abort" => {
                host.abort();
                Ok(Value::Null)
            }
            other => Err(format!("unsupported extension host API: {other}")),
        }
    }

    /// Run slash command `name` with its argument text. Commands may open
    /// dialogs, so GUI front ends call this off the UI thread.
    pub fn run_command(&self, name: &str, args: &str) -> Result<CommandOutcome, String> {
        let snapshot = self.snapshot();
        let (index, ext) = snapshot
            .extensions
            .iter()
            .enumerate()
            .find(|(_, e)| e.manifest.commands.iter().any(|c| c.name == name))
            .ok_or_else(|| format!("extension command contribution not found: {name}"))?;
        let arguments = json!({ "args": args });
        let value = match ext.manifest.transport {
            Transport::Rhai => self.invoke_rhai(&snapshot, index, &handler_name("command_", name), &arguments),
            Transport::SubprocessJsonRpc => self.invoke_subprocess(
                ext,
                COMMAND_INVOKE,
                json!({"name": name, "arguments": arguments}),
                None,
            ),
            Transport::ReferenceModule => Err(format!(
                "not_implemented: extension command '{name}': reference modules are not executed by the native extension host"
            )),
        }
        .map_err(|e| format!("extension command /{name} failed: {e}"))?;
        let mut lines = result_lines(&value);
        if lines.is_empty() {
            lines.push(format!("Extension command /{name} completed."));
        }
        Ok(CommandOutcome { lines, value })
    }

    /// Run tool `name` (used by [`crate::tool::ExtensionTool`]).
    pub fn run_tool(
        &self,
        name: &str,
        args: &Value,
        cancel: Option<&CancellationToken>,
    ) -> Result<Value, String> {
        let snapshot = self.snapshot();
        let (index, ext) = snapshot
            .extensions
            .iter()
            .enumerate()
            .find(|(_, e)| e.manifest.tools.iter().any(|t| t.name == name))
            .ok_or_else(|| format!("extension tool contribution not found: {name}"))?;
        let prefix = format!("extension tool '{name}'");
        match ext.manifest.transport {
            Transport::Rhai => {
                let handler = ext
                    .script
                    .as_ref()
                    .map(|s| s.registration().tool_handler(name))
                    .unwrap_or_else(|| handler_name("tool_", name));
                self.invoke_rhai(&snapshot, index, &handler, args)
                    .map_err(|e| format!("{prefix} execution failed: {e}"))
            }
            Transport::SubprocessJsonRpc => self
                .invoke_subprocess(
                    ext,
                    TOOL_INVOKE,
                    json!({"name": name, "arguments": args}),
                    cancel,
                )
                .map_err(|e| format!("{prefix} execution failed: {e}")),
            Transport::ReferenceModule => Err(format!(
                "not_implemented: {prefix}: reference modules are not executed by the native extension host"
            )),
        }
    }

    /// Path of the extension contributing tool `name`.
    pub fn tool_extension_path(&self, name: &str) -> Option<PathBuf> {
        self.snapshot()
            .extensions
            .iter()
            .find(|e| e.manifest.tools.iter().any(|t| t.name == name))
            .map(|e| e.path.clone())
    }

    /// Run a registered shortcut's handler with `{type:"shortcut", key}`.
    pub fn run_shortcut(&self, key: &str) -> Result<Value, String> {
        let snapshot = self.snapshot();
        for (index, ext) in snapshot.extensions.iter().enumerate() {
            let Some(script) = &ext.script else { continue };
            if let Some(s) = script
                .registration()
                .shortcuts
                .iter()
                .find(|s| s.key == key)
            {
                return self.invoke_rhai(
                    &snapshot,
                    index,
                    &s.handler,
                    &json!({"type": "shortcut", "key": key}),
                );
            }
        }
        Err(format!("extension shortcut not found: {key}"))
    }

    /// Lines from the renderer registered for `custom_type`, if any. The
    /// handler receives `{type:"render", message, width, expanded}` and
    /// returns `{lines:[...]}`.
    pub fn render_message(
        &self,
        message: &CustomMessage,
        width: u32,
        expanded: bool,
    ) -> Option<Result<Vec<String>, String>> {
        let snapshot = self.snapshot();
        for (index, ext) in snapshot.extensions.iter().enumerate() {
            let Some(script) = &ext.script else { continue };
            let Some(r) = script
                .registration()
                .message_renderers
                .iter()
                .find(|r| r.custom_type == message.custom_type)
            else {
                continue;
            };
            let payload = json!({
                "type": "render",
                "message": message,
                "content": message.content,
                "details": message.details,
                "width": width,
                "expanded": expanded,
            });
            return Some(
                self.invoke_rhai(&snapshot, index, &r.handler, &payload)
                    .map(|v| crate::ui::decode_component(&v).lines.unwrap_or_default()),
            );
        }
        None
    }

    // ----- events ----------------------------------------------------------

    fn targets(&self, snapshot: &LoadResult, event: &str) -> Vec<Target> {
        let mut out = Vec::new();
        for (index, ext) in snapshot.extensions.iter().enumerate() {
            match (&ext.script, ext.manifest.transport) {
                (Some(script), _) => {
                    out.extend(script.registration().handlers_for(event).map(|h| Target {
                        index,
                        handler: Some(h.to_string()),
                    }))
                }
                (None, Transport::SubprocessJsonRpc) if ext.manifest.handles_event(event) => out
                    .push(Target {
                        index,
                        handler: None,
                    }),
                _ => {}
            }
        }
        out
    }

    /// Whether any extension handles `event`.
    pub fn has_handlers(&self, event: &str) -> bool {
        !self.targets(&self.snapshot(), event).is_empty()
    }

    fn call_target(
        &self,
        snapshot: &Arc<LoadResult>,
        target: &Target,
        event: &str,
        payload: &Value,
    ) -> Result<Value, String> {
        let ext = &snapshot.extensions[target.index];
        match &target.handler {
            Some(h) => self.invoke_rhai(snapshot, target.index, h, payload),
            None => self.invoke_subprocess(
                ext,
                &format!("{EVENT_PREFIX}{event}"),
                payload.clone(),
                None,
            ),
        }
    }

    fn hook_error(&self, snapshot: &LoadResult, target: &Target, event: &str, error: &str) {
        self.report(&format!(
            "extension {event} hook failed: {}: {error}",
            snapshot.extensions[target.index].manifest.name
        ));
    }

    /// Run every handler of `event` in order; `step` decodes a result and
    /// returns whether to stop. Handler and decode failures are reported.
    fn route<S>(
        &self,
        event: &str,
        state: &mut S,
        payload: impl Fn(&S) -> Value,
        mut step: impl FnMut(&mut S, Value) -> Result<bool, String>,
    ) {
        let snapshot = self.snapshot();
        for target in self.targets(&snapshot, event) {
            let p = events::event(event, payload(state));
            match self
                .call_target(&snapshot, &target, event, &p)
                .and_then(|r| step(state, r))
            {
                Ok(true) => break,
                Ok(false) => {}
                Err(e) => self.hook_error(&snapshot, &target, event, &e),
            }
        }
    }

    /// Notify every handler of `event`; results are ignored.
    pub fn observe(&self, event: &str, fields: Value) {
        self.route(event, &mut (), |_| fields.clone(), |_, _| Ok(false));
    }

    /// `input`: handlers may continue, transform or handle the input. A
    /// failing handler stops the chain with its error (archived behaviour).
    pub fn emit_input(
        &self,
        text: &str,
        images: Value,
        source: InputSource,
    ) -> Result<InputOutcome, String> {
        if !events::valid_images(&images) {
            return Err("extension input event images must be null or a JSON array".into());
        }
        let snapshot = self.snapshot();
        let (mut text, mut images) = (text.to_string(), images);
        for target in self.targets(&snapshot, "input") {
            let mut fields = json!({ "text": text, "source": source.as_str() });
            if !images.is_null() {
                fields["images"] = images.clone();
            }
            let payload = events::event("input", fields);
            let result = self
                .call_target(&snapshot, &target, "input", &payload)
                .map_err(|e| {
                    format!(
                        "extension '{}' input event failed: {e}",
                        snapshot.extensions[target.index].manifest.name
                    )
                })?;
            match events::decode_input_result(&result)? {
                InputAction::Continue => {}
                InputAction::Handled => return Ok(InputOutcome::Handled),
                InputAction::Transform { text: t, images: i } => {
                    text = t;
                    if let Some(i) = i.filter(|i| !i.is_null()) {
                        images = i;
                    }
                }
            }
        }
        Ok(InputOutcome::Continue { text, images })
    }

    /// `session_before_switch`; `true` when an extension cancelled.
    pub fn emit_session_before_switch(
        &self,
        reason: &str,
        target_session_file: Option<&str>,
    ) -> bool {
        let mut cancelled = false;
        self.route(
            "session_before_switch",
            &mut cancelled,
            |_| json!({ "reason": reason, "targetSessionFile": target_session_file }),
            |cancelled, r| {
                *cancelled = events::decode_cancel(&r, "session_before_switch")?;
                Ok(*cancelled)
            },
        );
        cancelled
    }

    /// `session_before_fork`: `(cancel, skipConversationRestore)`.
    pub fn emit_session_before_fork(&self, entry_id: &str, position: &str) -> (bool, bool) {
        let mut state = (false, false);
        self.route(
            "session_before_fork",
            &mut state,
            |_| json!({ "entryId": entry_id, "position": position }),
            |state, r| {
                let (c, s) = events::decode_before_fork_result(&r)?;
                state.1 |= s;
                state.0 = c;
                Ok(c)
            },
        );
        state
    }

    /// `session_before_tree` with the navigation preparation; later handlers
    /// override earlier fields; a cancel stops the chain.
    pub fn emit_session_before_tree(&self, preparation: Value) -> BeforeTreeOutcome {
        let mut out = BeforeTreeOutcome::default();
        self.route(
            "session_before_tree",
            &mut out,
            |_| json!({ "preparation": preparation }),
            |out, r| {
                let d = events::decode_before_tree_result(&r)?;
                out.summary = d.summary;
                out.custom_instructions = d.custom_instructions;
                out.replace_instructions = d.replace_instructions;
                out.label = d.label;
                out.cancel = d.cancel;
                Ok(d.cancel)
            },
        );
        out
    }

    pub fn emit_model_select(&self, model: Value, previous: Value, source: &str) {
        self.observe(
            "model_select",
            json!({ "model": model, "previousModel": previous, "source": source }),
        );
    }

    pub fn emit_thinking_level_select(&self, level: &str, previous: &str) {
        self.observe(
            "thinking_level_select",
            json!({ "level": level, "previousLevel": previous }),
        );
    }

    pub fn emit_user_bash(&self, command: &str, exclude_from_context: bool, cwd: &Path) {
        self.observe(
            "user_bash",
            json!({ "command": command, "excludeFromContext": exclude_from_context, "cwd": display_path(cwd) }),
        );
    }

    pub fn emit_instructions_loaded(&self, files: Value) {
        self.observe("instructions_loaded", json!({ "files": files }));
    }

    pub fn emit_session_shutdown(&self, reason: &str, target_session_file: Option<&str>) {
        let mut fields = json!({ "reason": reason });
        if let Some(t) = target_session_file {
            fields["targetSessionFile"] = json!(t);
        }
        self.observe("session_shutdown", fields);
    }

    /// `resources_discover` (`reason`: `startup` or `reload`): extra skill,
    /// prompt and theme paths, resolved against each extension's directory.
    pub fn emit_resources_discover(&self, cwd: &Path, reason: &str) -> DiscoveredResources {
        let snapshot = self.snapshot();
        let mut out = DiscoveredResources::default();
        let payload = events::event(
            "resources_discover",
            json!({ "cwd": display_path(cwd), "reason": reason }),
        );
        for target in self.targets(&snapshot, "resources_discover") {
            let dir = snapshot.extensions[target.index].directory.clone();
            let resolve = |paths: Vec<String>| -> Vec<String> {
                paths
                    .into_iter()
                    .map(|p| {
                        if Path::new(&p).is_absolute() {
                            p
                        } else {
                            display_path(&dir.join(p))
                        }
                    })
                    .collect()
            };
            match self
                .call_target(&snapshot, &target, "resources_discover", &payload)
                .and_then(|r| events::decode_resources_discover_result(&r))
            {
                Ok(r) => {
                    out.skill_paths.extend(resolve(r.skill_paths));
                    out.prompt_paths.extend(resolve(r.prompt_paths));
                    out.theme_paths.extend(resolve(r.theme_paths));
                }
                Err(e) => self.hook_error(&snapshot, &target, "resources_discover", &e),
            }
        }
        out
    }

    fn on_agent_event(&self, event: &AgentEvent) {
        let has = |name: &str| self.has_handlers(name);
        let mut turn = self.inner.turn.lock().expect("turn lock");
        let close_turn = |turn: &mut TurnState, ext: &Extensions| -> Option<Value> {
            if !turn.open {
                return None;
            }
            turn.open = false;
            let payload = json!({
                "turnIndex": turn.index,
                "message": std::mem::take(&mut turn.message),
                "toolResults": std::mem::take(&mut turn.tool_results),
            });
            turn.index += 1;
            let _ = ext;
            Some(payload)
        };
        let mut pending: Vec<(&str, Value)> = Vec::new();
        match event {
            AgentEvent::AgentStart => {
                *turn = TurnState::default();
                pending.push(("agent_start", json!({})));
            }
            AgentEvent::AssistantStart => {
                if let Some(p) = close_turn(&mut turn, self) {
                    pending.push(("turn_end", p));
                }
                turn.open = true;
                pending.push((
                    "turn_start",
                    json!({ "turnIndex": turn.index, "timestamp": now_millis() }),
                ));
                pending.push((
                    "message_start",
                    json!({ "message": { "role": "assistant" } }),
                ));
            }
            AgentEvent::Stream(s) => {
                if has("message_update") {
                    pending.push((
                        "message_update",
                        json!({ "assistantMessageEvent": stream_json(s) }),
                    ));
                }
            }
            AgentEvent::MessageEnd(m) => {
                let mj = serde_json::to_value(m).unwrap_or(Value::Null);
                match m {
                    Message::Assistant(_) => turn.message = mj.clone(),
                    Message::ToolResult(r) => {
                        turn.tool_results.push(mj.clone());
                        pending.push((
                            "tool_execution_end",
                            json!({
                                "toolCallId": r.tool_call_id,
                                "toolName": r.tool_name,
                                "result": { "content": r.content, "details": r.details },
                                "isError": r.is_error,
                            }),
                        ));
                    }
                    Message::User { .. } => {}
                }
                // `message_end` handlers run as the qt-core replacement hook
                // before the message is recorded.
            }
            AgentEvent::ToolStart(t) => {
                turn.tool_names.insert(t.id.clone(), t.name.clone());
                pending.push((
                    "tool_execution_start",
                    json!({ "toolCallId": t.id, "toolName": t.name, "args": t.args }),
                ));
            }
            AgentEvent::ToolProgress { id, output } => {
                let name = turn.tool_names.get(id).cloned().unwrap_or_default();
                pending.push((
                    "tool_execution_update",
                    json!({ "toolCallId": id, "toolName": name, "partialResult": output }),
                ));
            }
            AgentEvent::AgentEnd { usage, aborted } => {
                if let Some(p) = close_turn(&mut turn, self) {
                    pending.push(("turn_end", p));
                }
                pending.push(("agent_end", json!({ "aborted": aborted, "usage": usage })));
            }
            _ => {}
        }
        drop(turn);
        for (name, fields) in pending {
            if has(name) {
                self.observe(name, fields);
            }
        }
    }

    fn on_session_event(&self, event: &SessionEvent) {
        match event {
            SessionEvent::Start {
                reason,
                session_id,
                session_file,
                previous_session_file,
            } => self.observe(
                "session_start",
                json!({
                    "reason": start_reason(*reason),
                    "sessionId": session_id,
                    "sessionFile": session_file.as_ref().map(|p| display_path(p)),
                    "previousSessionFile": previous_session_file.as_ref().map(|p| display_path(p)),
                }),
            ),
            SessionEvent::Shutdown {
                session_id,
                session_file,
                reason,
                target_session_file,
            } => {
                let mut fields = json!({
                    "reason": reason,
                    "sessionId": session_id,
                    "sessionFile": session_file.as_ref().map(|p| display_path(p)),
                });
                if let Some(target) = target_session_file {
                    fields["targetSessionFile"] = json!(display_path(target));
                }
                self.observe("session_shutdown", fields)
            }
            SessionEvent::Compact {
                entry_id,
                result,
                from_hook,
                ..
            } => self.observe(
                "session_compact",
                json!({
                    "compactionEntry": { "id": entry_id, "type": "compaction", "summary": result.summary, "firstKeptEntryId": result.first_kept_entry_id, "tokensBefore": result.tokens_before, "details": result.details },
                    "fromExtension": from_hook,
                }),
            ),
            SessionEvent::Tree {
                old_leaf_id,
                new_leaf_id,
                summary_entry_id,
                ..
            } => self.observe(
                "session_tree",
                json!({ "oldLeafId": old_leaf_id, "newLeafId": new_leaf_id, "summaryEntryId": summary_entry_id }),
            ),
            SessionEvent::NameChanged { .. } => {}
        }
    }

    fn on_before_agent_start(
        &self,
        event: &BeforeAgentStartEvent,
    ) -> Option<BeforeAgentStartResult> {
        if !self.has_handlers("before_agent_start") {
            return None;
        }
        let (text, images) = match &event.prompt {
            Message::User { content, .. } => {
                let blocks = content.blocks();
                let images: Vec<&ContentBlock> = blocks
                    .iter()
                    .filter(|b| matches!(b, ContentBlock::Image { .. }))
                    .collect();
                (
                    content.plain_text(),
                    if images.is_empty() {
                        Value::Null
                    } else {
                        json!(images)
                    },
                )
            }
            _ => (String::new(), Value::Null),
        };
        let mut state = (
            BeforeAgentStartResult::default(),
            event.system_prompt.clone(),
            false,
        );
        self.route(
            "before_agent_start",
            &mut state,
            |(_, system, _)| {
                let mut f = json!({
                    "prompt": text,
                    "systemPrompt": system,
                    "systemPromptOptions": { "cwd": display_path(&event.cwd) },
                });
                if !images.is_null() {
                    f["images"] = images.clone();
                }
                f
            },
            |(out, system, changed), r| {
                let d = events::decode_before_agent_start_result(&r)?;
                if let Some(m) = d.message {
                    out.messages.push(m);
                }
                if let Some(p) = d.system_prompt {
                    *system = p;
                    *changed = true;
                }
                Ok(false)
            },
        );
        let (mut out, system, changed) = state;
        if changed {
            out.system_prompt = Some(system);
        }
        (changed || !out.messages.is_empty()).then_some(out)
    }

    fn on_context(&self, messages: &[AgentMessage]) -> Option<Vec<AgentMessage>> {
        if !self.has_handlers("context") {
            return None;
        }
        let mut state = (messages.to_vec(), false);
        self.route(
            "context",
            &mut state,
            |(current, _)| json!({ "messages": events::messages_json(current) }),
            |(current, changed), r| {
                if let Some(m) = events::decode_context_result(&r)? {
                    *current = m;
                    *changed = true;
                }
                Ok(false)
            },
        );
        state.1.then_some(state.0)
    }

    fn on_provider_request(&self, event: &ProviderRequestEvent) -> Option<Request> {
        if !self.has_handlers("before_provider_request") {
            return None;
        }
        let mut state = (event.request.clone(), false);
        self.route(
            "before_provider_request",
            &mut state,
            |(request, _)| json!({ "model": { "provider": event.model.provider, "id": event.model.id }, "turnIndex": event.turn_index, "request": request_json(request) }),
            |(request, changed), r| {
                let p = events::decode_provider_request_result(&r)?;
                if let Some(s) = p.system_prompt {
                    request.system = s;
                    *changed = true;
                }
                if let Some(m) = p.messages {
                    request.messages = m;
                    *changed = true;
                }
                if let Some(t) = p.max_tokens {
                    request.max_tokens = t;
                    *changed = true;
                }
                if let Some(t) = p.temperature {
                    request.temperature = Some(t);
                    *changed = true;
                }
                Ok(false)
            },
        );
        state.1.then_some(state.0)
    }

    fn on_provider_response(&self, event: &ProviderResponseEvent) {
        if self.has_handlers("after_provider_response") {
            self.observe(
                "after_provider_response",
                json!({ "model": { "provider": event.model.provider, "id": event.model.id }, "turnIndex": event.turn_index, "message": event.response }),
            );
        }
    }

    fn on_tool_call(&self, event: &ToolCallEvent) -> Option<ToolCallDecision> {
        if !self.has_handlers("tool_call") {
            return None;
        }
        let input = if event.args.is_object() {
            event.args.clone()
        } else {
            json!({})
        };
        // (block decision, current input, input changed)
        let mut state: (Option<ToolCallDecision>, Value, bool) = (None, input, false);
        self.route(
            "tool_call",
            &mut state,
            |(_, input, _)| json!({ "toolCallId": event.tool_call_id, "toolName": event.tool_name, "input": input }),
            |(decision, input, changed), r| {
                let d = events::decode_tool_call_result(&r)?;
                if d.block {
                    *decision = Some(ToolCallDecision {
                        block: true,
                        reason: d.reason,
                        args: None,
                    });
                    return Ok(true);
                }
                if let Some(new_input) = d.input {
                    *input = new_input;
                    *changed = true;
                }
                Ok(false)
            },
        );
        let (decision, input, changed) = state;
        decision.or_else(|| {
            changed.then(|| ToolCallDecision {
                block: false,
                reason: None,
                args: Some(input),
            })
        })
    }

    /// `message_end`: handlers may return `{message}` to replace the
    /// finalized message (same role); later handlers see the replacement.
    fn on_message_end(&self, message: &AgentMessage) -> Option<AgentMessage> {
        if !self.has_handlers("message_end") {
            return None;
        }
        let original = serde_json::to_value(message).ok()?;
        let mut state = (original, false);
        self.route(
            "message_end",
            &mut state,
            |(current, _)| json!({ "message": current }),
            |(current, changed), r| {
                if let Some(replacement) = r.get("message").filter(|m| !m.is_null()) {
                    if replacement.get("role") != current.get("role") {
                        return Err(
                            "extension message_end result message must keep the original role"
                                .into(),
                        );
                    }
                    *current = replacement.clone();
                    *changed = true;
                }
                Ok(false)
            },
        );
        let (value, changed) = state;
        if !changed {
            return None;
        }
        match serde_json::from_value::<AgentMessage>(value) {
            Ok(m) => Some(m),
            Err(e) => {
                self.report(&format!("extension message_end result is invalid: {e}"));
                None
            }
        }
    }

    fn on_input(&self, event: &InputEvent) -> Result<Option<CoreInputOutcome>, String> {
        if !self.has_handlers("input") {
            return Ok(None);
        }
        let source = match event.source {
            CoreInputSource::Interactive => InputSource::Interactive,
            CoreInputSource::Rpc => InputSource::Rpc,
            CoreInputSource::Extension => InputSource::Extension,
        };
        let images = if event.images.is_empty() {
            Value::Null
        } else {
            serde_json::to_value(&event.images).unwrap_or(Value::Null)
        };
        Ok(Some(
            match self.emit_input(&event.text, images, source)? {
                InputOutcome::Handled => CoreInputOutcome::Handled,
                InputOutcome::Continue { text, images } => CoreInputOutcome::Continue {
                    text,
                    images: if images.is_null() {
                        Vec::new()
                    } else {
                        parse_content(&images)?
                    },
                },
            },
        ))
    }

    fn on_tool_result(&self, event: &ToolResultEvent) -> Option<ToolResultPatch> {
        if !self.has_handlers("tool_result") {
            return None;
        }
        let mut state = (event.output.clone(), false);
        let input = if event.args.is_object() {
            event.args.clone()
        } else {
            json!({})
        };
        self.route(
            "tool_result",
            &mut state,
            |(output, _)| {
                json!({
                    "toolCallId": event.tool_call_id,
                    "toolName": event.tool_name,
                    "input": input,
                    "content": output.content,
                    "details": output.details,
                    "isError": output.is_error,
                })
            },
            |(output, changed), r| {
                let d = events::decode_tool_result_result(&r)?;
                if let Some(c) = d.content {
                    output.content = c;
                    *changed = true;
                }
                if let Some(details) = d.details {
                    output.details = Some(details);
                    *changed = true;
                }
                if let Some(e) = d.is_error {
                    output.is_error = e;
                    *changed = true;
                }
                Ok(false)
            },
        );
        let (output, changed) = state;
        changed.then(|| ToolResultPatch {
            content: Some(output.content),
            details: output.details,
            is_error: Some(output.is_error),
        })
    }

    fn on_before_compact(&self, event: &BeforeCompactEvent) -> Option<BeforeCompactResult> {
        if !self.has_handlers("session_before_compact") {
            return None;
        }
        let mut result = BeforeCompactResult::default();
        self.route(
            "session_before_compact",
            &mut result,
            |_| {
                json!({
                    "reason": event.reason.as_str(),
                    "preparation": events::preparation_json(&event.preparation),
                    "branchEntries": event.branch_entries.iter().map(|e| e.to_value()).collect::<Vec<_>>(),
                    "customInstructions": event.custom_instructions,
                })
            },
            |result, r| {
                let d = events::decode_before_compact_result(&r)?;
                if d.compaction.is_some() {
                    result.compaction = d.compaction;
                }
                result.cancel = d.cancel;
                Ok(d.cancel)
            },
        );
        (result.cancel || result.compaction.is_some()).then_some(result)
    }

    /// Register the extension hooks on an engine's hook table (replacing a
    /// previous installation). Hooks resolve handlers at call time, so they
    /// follow [`Extensions::reload`].
    pub fn install_hooks(&self, hooks: &Hooks) {
        self.uninstall_hooks();
        let weak = Arc::downgrade(&self.inner);
        let get = move || weak.upgrade().map(|inner| Extensions { inner });
        let mut ids = Vec::new();
        let g = get.clone();
        ids.push(hooks.add_before_agent_start(Arc::new(move |e| g()?.on_before_agent_start(e))));
        let g = get.clone();
        ids.push(hooks.add_context(Arc::new(move |e| g()?.on_context(&e.messages))));
        let g = get.clone();
        ids.push(hooks.add_before_provider_request(Arc::new(move |e| g()?.on_provider_request(e))));
        let g = get.clone();
        ids.push(hooks.add_after_provider_response(Arc::new(move |e| {
            if let Some(x) = g() {
                x.on_provider_response(e);
            }
        })));
        let g = get.clone();
        ids.push(hooks.add_tool_call(Arc::new(move |e| g()?.on_tool_call(e))));
        let g = get.clone();
        ids.push(hooks.add_tool_result(Arc::new(move |e| g()?.on_tool_result(e))));
        let g = get.clone();
        ids.push(hooks.add_before_compact(Arc::new(move |e| g()?.on_before_compact(e))));
        let g = get.clone();
        ids.push(hooks.add_session_listener(Arc::new(move |e| {
            if let Some(x) = g() {
                x.on_session_event(e);
            }
        })));
        let g = get.clone();
        ids.push(hooks.add_agent_event_listener(Arc::new(move |e| {
            if let Some(x) = g() {
                x.on_agent_event(e);
            }
        })));
        let g = get.clone();
        ids.push(hooks.add_message_end(Arc::new(move |m| g()?.on_message_end(m))));
        let g = get.clone();
        ids.push(hooks.add_input(Arc::new(move |e| match g() {
            Some(x) => x.on_input(e),
            None => Ok(None),
        })));
        let g = get.clone();
        ids.push(hooks.add_session_before_switch(Arc::new(move |e| {
            g().is_some_and(|x| {
                x.emit_session_before_switch(
                    &e.reason,
                    e.target_session_file
                        .as_ref()
                        .map(|p| display_path(p))
                        .as_deref(),
                )
            })
        })));
        let g = get.clone();
        ids.push(hooks.add_session_before_fork(Arc::new(move |e| {
            let x = g()?;
            if !x.has_handlers("session_before_fork") {
                return None;
            }
            let (cancel, skip) = x.emit_session_before_fork(&e.entry_id, &e.position);
            Some(BeforeForkResult {
                cancel,
                skip_conversation_restore: skip,
            })
        })));
        let g = get.clone();
        ids.push(hooks.add_session_before_tree(Arc::new(move |e| {
            let x = g()?;
            if !x.has_handlers("session_before_tree") {
                return None;
            }
            let out = x.emit_session_before_tree(json!({
                "targetId": e.target_id,
                "oldLeafId": e.old_leaf_id,
                "userWantsSummary": e.user_wants_summary,
                "customInstructions": e.custom_instructions,
                "replaceInstructions": e.replace_instructions,
                "label": e.label,
                "entriesToSummarize": e.entries_to_summarize.iter().map(|x| x.to_value()).collect::<Vec<_>>(),
            }));
            Some(BeforeTreeResult {
                cancel: out.cancel,
                summary: out.summary,
                custom_instructions: out.custom_instructions,
                replace_instructions: out.replace_instructions,
                label: out.label,
            })
        })));
        let g = get.clone();
        ids.push(hooks.add_model_select(Arc::new(move |e| {
            if let Some(x) = g().filter(|x| x.has_handlers("model_select")) {
                let model =
                    |m: &qt_core::provider::ModelRef| json!({"provider": m.provider, "id": m.id});
                x.emit_model_select(
                    model(&e.model),
                    e.previous.as_ref().map(model).unwrap_or(Value::Null),
                    &e.source,
                );
            }
        })));
        let g = get.clone();
        ids.push(hooks.add_thinking_level_select(Arc::new(move |e| {
            if let Some(x) = g().filter(|x| x.has_handlers("thinking_level_select")) {
                x.emit_thinking_level_select(&e.level, &e.previous);
            }
        })));
        let g = get.clone();
        ids.push(hooks.add_user_bash(Arc::new(move |e| {
            if let Some(x) = g().filter(|x| x.has_handlers("user_bash")) {
                x.emit_user_bash(&e.command, e.exclude_from_context, &e.cwd);
            }
        })));
        let g = get.clone();
        ids.push(hooks.add_resources_discover(Arc::new(move |e| {
            let x = g()?;
            if !x.has_handlers("resources_discover") {
                return None;
            }
            let found = x.emit_resources_discover(&e.cwd, &e.reason);
            Some(DiscoveredResourcePaths {
                skill_paths: found.skill_paths,
                prompt_paths: found.prompt_paths,
                theme_paths: found.theme_paths,
            })
        })));
        let g = get;
        ids.push(hooks.add_instructions_loaded(Arc::new(move |e| {
            if let Some(x) = g().filter(|x| x.has_handlers("instructions_loaded")) {
                x.emit_instructions_loaded(json!(
                    e.files
                        .iter()
                        .map(|f| json!({"path": display_path(f), "reason": e.reason}))
                        .collect::<Vec<_>>()
                ));
            }
        })));
        *self.inner.installed.lock().expect("installed lock") = Some((hooks.clone(), ids));
    }

    /// Remove the hooks registered by [`Extensions::install_hooks`].
    pub fn uninstall_hooks(&self) {
        if let Some((hooks, ids)) = self.inner.installed.lock().expect("installed lock").take() {
            for id in ids {
                hooks.remove(id);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn result_line_formatting() {
        assert_eq!(result_lines(&json!({"text":"done"})), ["done"]);
        assert_eq!(
            result_lines(
                &json!({"content":[{"type":"text","text":"a"},"b",{"type":"image","text":"x"}]})
            ),
            ["a", "b"]
        );
        assert_eq!(result_lines(&json!({"other":1})), ["{\"other\":1}"]);
        assert_eq!(result_lines(&json!(["x", 3])), ["x", "3"]);
        assert!(result_lines(&Value::Null).is_empty());
    }

    #[test]
    fn flag_splitting() {
        assert_eq!(split_flag("--a=b"), Some(("a", Some("b"))));
        assert_eq!(split_flag("--a"), Some(("a", None)));
        assert_eq!(split_flag("-a"), None);
        assert_eq!(split_flag("--"), None);
    }
}
