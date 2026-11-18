//! The Rhai script runtime (decision 2026-11-18: replaces the archived
//! AngelScript host with the same contract).
//!
//! Each loaded extension owns one Rhai engine and compiled script. The
//! script's top level runs at load, then `fn activate()` runs exactly once;
//! registration functions only work during `activate()`. Handlers are plain
//! script functions taking zero or one argument (the event / arguments as a
//! Rhai map) and returning any value, converted to JSON. A shared object map
//! bound as `this` keeps state across invocations. Every invocation has an
//! operation budget (100 000 by default), sources are limited to 256 KB,
//! `import` and `eval` are disabled, and entering a runtime that another
//! thread is executing is rejected unless that thread is waiting in
//! [`pump_while`] (as `ctx.compact` does), in which case the call runs on the
//! owning thread.

use std::cell::Cell;
use std::collections::HashMap;
use std::sync::{Arc, Mutex, mpsc};
use std::thread::ThreadId;

use rhai::{
    AST, CallFnOptions, Dynamic, Engine, EvalAltResult, FnPtr, Map, Module, Position, Scope,
};
use serde_json::Value;

use crate::registration::{Registration, handler_name};

/// Synchronous host API used by script calls (`pi.*`, `ctx.*`, `ctx.ui.*`).
pub type HostCall = Arc<dyn Fn(&str, Value) -> Result<Value, String> + Send + Sync>;

/// Limits applied to one script.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SandboxPolicy {
    pub max_source_bytes: usize,
    /// Rhai operations allowed per invocation (load + `activate()` counts as one).
    pub max_operations: u64,
}

impl Default for SandboxPolicy {
    fn default() -> Self {
        Self {
            max_source_bytes: 256 * 1024,
            max_operations: 100_000,
        }
    }
}

/// Nested invocations across all runtimes on one thread.
pub const MAX_INVOKE_DEPTH: u32 = 8;

/// `pi.*` host methods exposed as `pi::<name>(args)`.
pub const PI_METHODS: &[&str] = &[
    "getFlag",
    "getCommands",
    "getSessionName",
    "setSessionName",
    "appendEntry",
    "setLabel",
    "sendMessage",
    "sendUserMessage",
    "getActiveTools",
    "getAllTools",
    "setActiveTools",
    "getThinkingLevel",
    "setThinkingLevel",
    "setModel",
    "exec",
];

/// `ctx.*` host methods exposed as `ctx::<name>(args)`.
pub const CTX_METHODS: &[&str] = &[
    "getModel",
    "getModelCatalog",
    "getSystemPrompt",
    "getContextUsage",
    "getContext",
    "compact",
    "isIdle",
    "hasPendingMessages",
    "abort",
];

/// `ctx.ui.*` methods exposed as `ctx::ui::<name>(args)`.
pub const UI_METHODS: &[&str] = &[
    "select",
    "confirm",
    "input",
    "editor",
    "notify",
    "setStatus",
    "setWorkingMessage",
    "setWorkingVisible",
    "setWorkingIndicator",
    "setHiddenThinkingLabel",
    "setWidget",
    "setHeader",
    "setFooter",
    "setTitle",
    "onTerminalInput",
    "setEditorComponent",
    "getEditorComponent",
    "addAutocompleteProvider",
    "custom",
    "setEditorText",
    "getEditorText",
    "pasteToEditor",
    "getAllThemes",
    "getTheme",
    "setTheme",
    "getToolsExpanded",
    "setToolsExpanded",
];

type RhaiResult<T> = Result<T, Box<EvalAltResult>>;

fn runtime_error(message: impl Into<String>) -> Box<EvalAltResult> {
    EvalAltResult::ErrorRuntime(Dynamic::from(message.into()), Position::NONE).into()
}

/// Convert JSON to a Rhai value (objects become maps, `null` becomes `()`).
pub fn json_to_dynamic(value: &Value) -> Dynamic {
    match value {
        Value::Null => Dynamic::UNIT,
        Value::Bool(b) => Dynamic::from_bool(*b),
        Value::Number(n) => match n.as_i64() {
            Some(i) => Dynamic::from_int(i),
            None => Dynamic::from_float(n.as_f64().unwrap_or(0.0)),
        },
        Value::String(s) => Dynamic::from(s.clone()),
        Value::Array(items) => Dynamic::from_array(items.iter().map(json_to_dynamic).collect()),
        Value::Object(o) => {
            let mut map = Map::new();
            for (k, v) in o {
                map.insert(k.as_str().into(), json_to_dynamic(v));
            }
            Dynamic::from_map(map)
        }
    }
}

/// Convert a Rhai value to JSON. Unit is `null`; values without a JSON form
/// (function pointers, timestamps, ...) are an error.
pub fn dynamic_to_json(value: &Dynamic) -> Result<Value, String> {
    let value = value.flatten_clone();
    if value.is_unit() {
        return Ok(Value::Null);
    }
    if let Ok(b) = value.as_bool() {
        return Ok(Value::Bool(b));
    }
    if let Ok(i) = value.as_int() {
        return Ok(Value::from(i));
    }
    if let Ok(f) = value.as_float() {
        return serde_json::Number::from_f64(f)
            .map(Value::Number)
            .ok_or_else(|| "non-finite numbers cannot be converted to JSON".to_string());
    }
    if value.is_string() || value.is_char() {
        return Ok(Value::String(value.to_string()));
    }
    if value.is_array() {
        let array = value.into_array().map_err(|t| t.to_string())?;
        return array
            .iter()
            .map(dynamic_to_json)
            .collect::<Result<Vec<_>, _>>()
            .map(Value::Array);
    }
    if value.is_map() {
        let map = value.cast::<Map>();
        let mut out = serde_json::Map::new();
        for (k, v) in &map {
            out.insert(k.to_string(), dynamic_to_json(v)?);
        }
        return Ok(Value::Object(out));
    }
    if value.is_blob() {
        let blob = value.into_blob().map_err(|t| t.to_string())?;
        return Ok(Value::Array(blob.into_iter().map(Value::from).collect()));
    }
    Err(format!(
        "value of type {} cannot be converted to JSON",
        value.type_name()
    ))
}

/// Root cause text of a Rhai error.
fn describe(err: &EvalAltResult) -> String {
    match err {
        EvalAltResult::ErrorInFunctionCall(_, _, inner, _) => describe(inner),
        EvalAltResult::ErrorRuntime(value, _) => {
            if value.is_string() {
                value.to_string()
            } else {
                dynamic_to_json(value)
                    .map(|v| v.to_string())
                    .unwrap_or_else(|_| value.to_string())
            }
        }
        EvalAltResult::ErrorTooManyOperations(_) => "sandbox step budget exceeded".into(),
        other => other.to_string(),
    }
}

struct Shared {
    activation: Mutex<Option<Registration>>,
    hosts: Mutex<Vec<HostCall>>,
}

impl Shared {
    fn register(&self, f: impl FnOnce(&mut Registration) -> Result<(), String>) -> RhaiResult<()> {
        let mut slot = self.activation.lock().expect("activation lock");
        let Some(reg) = slot.as_mut() else {
            return Err(runtime_error(
                "registration APIs are only available during activate()",
            ));
        };
        f(reg).map_err(runtime_error)
    }

    fn host_call(&self, method: &str, args: Value) -> RhaiResult<Value> {
        if !args.is_object() {
            return Err(runtime_error("extension host arguments must be an object"));
        }
        let host = self
            .hosts
            .lock()
            .expect("host stack")
            .last()
            .cloned()
            .ok_or_else(|| runtime_error("extension host is not available"))?;
        host(method, args).map_err(runtime_error)
    }
}

fn args_value(args: &Dynamic) -> RhaiResult<Value> {
    if args.is_unit() {
        return Ok(Value::Object(Default::default()));
    }
    dynamic_to_json(args).map_err(runtime_error)
}

fn opt_str(map: &Map, key: &str) -> Option<String> {
    map.get(key)
        .filter(|v| v.is_string())
        .map(|v| v.to_string())
}

fn fn_ptr_name(ptr: &FnPtr) -> RhaiResult<String> {
    if ptr.is_anonymous() || !ptr.curry().is_empty() {
        return Err(runtime_error(
            "extension handlers must be named script functions",
        ));
    }
    Ok(ptr.fn_name().to_string())
}

fn register_api(engine: &mut Engine, shared: &Arc<Shared>) {
    // Commands.
    for name in ["registerCommand", "qt_register_command"] {
        let s = shared.clone();
        engine.register_fn(name, move |n: &str| {
            s.register(|r| r.add_command(n, None, None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, t: &str| {
            s.register(|r| r.add_command(n, Some(t.into()), None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, t: &str, d: &str| {
            s.register(|r| r.add_command(n, Some(t.into()), Some(d.into())))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, options: Map| {
            let title = opt_str(&options, "title").or_else(|| opt_str(&options, "label"));
            s.register(|r| r.add_command(n, title, opt_str(&options, "description")))
        });
    }
    // Tools.
    for name in ["registerTool", "qt_register_tool"] {
        let s = shared.clone();
        engine.register_fn(name, move |n: &str| {
            s.register(|r| r.add_tool(n, None, None, None, None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, t: &str| {
            s.register(|r| r.add_tool(n, Some(t.into()), None, None, None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, t: &str, d: &str| {
            s.register(|r| r.add_tool(n, Some(t.into()), Some(d.into()), None, None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |spec: Map| {
            let Some(n) = opt_str(&spec, "name") else {
                return Err(runtime_error(
                    "qt_register_tool expects name, optional title, optional description",
                ));
            };
            let parameters = match spec.get("parameters") {
                Some(p) if !p.is_unit() => Some(dynamic_to_json(p).map_err(runtime_error)?),
                _ => None,
            };
            let handler = match spec.get("handler") {
                Some(h) if h.is_fnptr() => Some(fn_ptr_name(&h.clone().cast::<FnPtr>())?),
                Some(h) if h.is_string() => Some(h.to_string()),
                _ => None,
            };
            let title = opt_str(&spec, "title").or_else(|| opt_str(&spec, "label"));
            s.register(|r| {
                r.add_tool(
                    &n,
                    title,
                    opt_str(&spec, "description"),
                    parameters,
                    handler,
                )
            })
        });
    }
    // Resources.
    for name in ["registerResource", "qt_register_resource"] {
        let s = shared.clone();
        engine.register_fn(name, move |kind: &str, path: &str| {
            s.register(|r| r.add_resource(kind, path, None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |kind: &str, path: &str, n: &str| {
            s.register(|r| r.add_resource(kind, path, Some(n.into())))
        });
    }
    // Providers.
    for name in ["registerProvider", "qt_register_provider"] {
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, config: Map| {
            let config = dynamic_to_json(&Dynamic::from_map(config)).map_err(runtime_error)?;
            s.register(|r| r.add_provider(n, config))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, config: &str| {
            let config: Value = serde_json::from_str(config).map_err(|e| {
                runtime_error(format!("extension provider config JSON is invalid: {e}"))
            })?;
            s.register(|r| r.add_provider(n, config))
        });
    }
    // Lifecycle handlers.
    for name in ["on", "qt_on"] {
        let s = shared.clone();
        engine.register_fn(name, move |event: &str, handler: &str| {
            s.register(|r| r.add_lifecycle_handler(event, handler))
        });
        let s = shared.clone();
        engine.register_fn(name, move |event: &str, handler: FnPtr| {
            let handler = fn_ptr_name(&handler)?;
            s.register(|r| r.add_lifecycle_handler(event, &handler))
        });
        let s = shared.clone();
        engine.register_fn(name, move |_event: &str| {
            s.register(|_| Err("qt_on expects event, handler".into()))
        });
    }
    // Flags.
    for name in ["registerFlag", "qt_register_flag"] {
        let s = shared.clone();
        engine.register_fn(name, move |n: &str| {
            s.register(|r| r.add_flag(n, None, Value::Null, None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, t: &str, default: Dynamic, d: &str| {
            let default = dynamic_to_json(&default).map_err(runtime_error)?;
            s.register(|r| r.add_flag(n, Some(t), default, Some(d.into())))
        });
        let s = shared.clone();
        engine.register_fn(name, move |n: &str, options: Map| {
            let default = match options.get("default") {
                Some(d) => dynamic_to_json(d).map_err(runtime_error)?,
                None => Value::Null,
            };
            let flag_type = opt_str(&options, "type");
            s.register(|r| {
                r.add_flag(
                    n,
                    flag_type.as_deref(),
                    default,
                    opt_str(&options, "description"),
                )
            })
        });
    }
    // Shortcuts.
    for name in ["registerShortcut", "qt_register_shortcut"] {
        let s = shared.clone();
        engine.register_fn(name, move |key: &str| {
            s.register(|r| r.add_shortcut(key, key, None))
        });
        let s = shared.clone();
        engine.register_fn(name, move |key: &str, handler: &str, d: &str| {
            s.register(|r| r.add_shortcut(key, handler, Some(d.into())))
        });
        let s = shared.clone();
        engine.register_fn(name, move |key: &str, handler: FnPtr, d: &str| {
            let handler = fn_ptr_name(&handler)?;
            s.register(|r| r.add_shortcut(key, &handler, Some(d.into())))
        });
    }
    // Message renderers.
    for name in ["registerMessageRenderer", "qt_register_message_renderer"] {
        let s = shared.clone();
        engine.register_fn(name, move |t: &str, handler: &str| {
            s.register(|r| r.add_message_renderer(t, handler))
        });
        let s = shared.clone();
        engine.register_fn(name, move |t: &str, handler: FnPtr| {
            let handler = fn_ptr_name(&handler)?;
            s.register(|r| r.add_message_renderer(t, &handler))
        });
    }

    // Host calls.
    let s = shared.clone();
    engine.register_fn("host_call", move |method: &str| -> RhaiResult<Dynamic> {
        s.host_call(method, Value::Object(Default::default()))
            .map(|v| json_to_dynamic(&v))
    });
    let s = shared.clone();
    engine.register_fn(
        "host_call",
        move |method: &str, args: Dynamic| -> RhaiResult<Dynamic> {
            s.host_call(method, args_value(&args)?)
                .map(|v| json_to_dynamic(&v))
        },
    );
    let s = shared.clone();
    engine.register_fn(
        "qt_host_call",
        move |method: &str, args: &str| -> RhaiResult<String> {
            let args: Value = serde_json::from_str(args)
                .map_err(|_| runtime_error("extension host arguments must be valid JSON"))?;
            s.host_call(method, args).map(|v| v.to_string())
        },
    );

    // JSON helpers.
    engine.register_fn("json_parse", |text: &str| -> RhaiResult<Dynamic> {
        serde_json::from_str::<Value>(text)
            .map(|v| json_to_dynamic(&v))
            .map_err(|e| runtime_error(format!("invalid JSON: {e}")))
    });
    engine.register_fn("json_stringify", |value: Dynamic| -> RhaiResult<String> {
        dynamic_to_json(&value)
            .map(|v| v.to_string())
            .map_err(runtime_error)
    });
    engine.register_fn(
        "qt_json_get",
        |document: &str, key: &str| -> RhaiResult<String> {
            let value: Value =
                serde_json::from_str(document).map_err(|e| runtime_error(e.to_string()))?;
            let obj = value
                .as_object()
                .ok_or_else(|| runtime_error("JSON document must be an object"))?;
            Ok(obj
                .get(key)
                .map(Value::to_string)
                .unwrap_or_else(|| "null".into()))
        },
    );
    engine.register_fn("qt_json_string", |value: &str| -> RhaiResult<String> {
        match serde_json::from_str::<Value>(value) {
            Ok(Value::String(s)) => Ok(s),
            Ok(_) => Err(runtime_error("JSON value is not a string")),
            Err(e) => Err(runtime_error(e.to_string())),
        }
    });

    // Namespaced host APIs: pi::x(args), ctx::x(args), ctx::ui::x(args).
    let method_module = |prefix: &str, names: &[&str]| {
        let mut module = Module::new();
        for name in names {
            let method = format!("{prefix}.{name}");
            let s = shared.clone();
            let m = method.clone();
            module.set_native_fn(*name, move || -> RhaiResult<Dynamic> {
                s.host_call(&m, Value::Object(Default::default()))
                    .map(|v| json_to_dynamic(&v))
            });
            let s = shared.clone();
            module.set_native_fn(*name, move |args: Dynamic| -> RhaiResult<Dynamic> {
                s.host_call(&method, args_value(&args)?)
                    .map(|v| json_to_dynamic(&v))
            });
        }
        module
    };
    engine.register_static_module("pi", method_module("pi", PI_METHODS).into());
    let mut ctx = method_module("ctx", CTX_METHODS);
    ctx.set_sub_module("ui", method_module("ctx.ui", UI_METHODS));
    engine.register_static_module("ctx", ctx.into());
}

/// Whether a line imports modules (rejected before compiling).
fn has_import(source: &str) -> Option<usize> {
    source.lines().enumerate().find_map(|(i, line)| {
        let t = line.trim_start();
        let is_import = t
            .strip_prefix("import")
            .is_some_and(|rest| !rest.starts_with(|c: char| c.is_alphanumeric() || c == '_'));
        is_import.then_some(i + 1)
    })
}

/// Work handed to a thread that is waiting in [`pump_while`].
type Job = Box<dyn FnOnce() + Send>;

enum Pumped {
    Job(Job),
    Done,
}

/// A thread's mailbox for invocations of runtimes it owns, open while the
/// thread waits in [`pump_while`].
#[derive(Default)]
struct Pump {
    tx: Mutex<Option<mpsc::Sender<Pumped>>>,
}

impl Pump {
    /// Queue `job` on the owning thread; `false` when it is not pumping.
    /// The send happens under the lock so nothing is queued after
    /// [`pump_while`] closed the mailbox and drained it.
    fn send(&self, job: Job) -> bool {
        let slot = self.tx.lock().expect("pump lock");
        slot.as_ref()
            .is_some_and(|tx| tx.send(Pumped::Job(job)).is_ok())
    }
}

thread_local! {
    static DEPTH: Cell<u32> = const { Cell::new(0) };
    static PUMP: Arc<Pump> = Arc::default();
}

/// Run the blocking `wait` on a helper thread while this thread runs the
/// invocations other threads make of runtimes it owns (archived hosts ran
/// compaction hooks on the thread that called `ctx.compact`). Host APIs
/// that wait on the engine use this so the hooks they trigger in the
/// calling extension do not fail as reentrant or deadlock.
pub fn pump_while<T: Send>(wait: impl FnOnce() -> T + Send) -> T {
    struct Finish(mpsc::Sender<Pumped>);
    impl Drop for Finish {
        fn drop(&mut self) {
            let _ = self.0.send(Pumped::Done);
        }
    }
    let pump = PUMP.with(Arc::clone);
    let (tx, rx) = mpsc::channel();
    let previous = pump.tx.lock().expect("pump lock").replace(tx.clone());
    let outcome = std::thread::scope(|scope| {
        let finish = Finish(tx);
        let waiter = scope.spawn(move || {
            let _finish = finish;
            wait()
        });
        while let Ok(Pumped::Job(job)) = rx.recv() {
            job();
        }
        waiter.join()
    });
    // Close the mailbox, then run whatever was queued before it closed.
    *pump.tx.lock().expect("pump lock") = previous;
    while let Ok(message) = rx.try_recv() {
        if let Pumped::Job(job) = message {
            job();
        }
    }
    outcome.unwrap_or_else(|panic| std::panic::resume_unwind(panic))
}

struct Entry<'a> {
    runtime: &'a ScriptRuntime,
}

impl Drop for Entry<'_> {
    fn drop(&mut self) {
        self.runtime.shared.hosts.lock().expect("host stack").pop();
        let mut owner = self.runtime.owner.lock().expect("owner lock");
        if let Some(o) = owner.as_mut() {
            o.depth -= 1;
            if o.depth == 0 {
                *owner = None;
            }
        }
        DEPTH.with(|d| d.set(d.get().saturating_sub(1)));
    }
}

/// One loaded Rhai extension.
pub struct ScriptRuntime {
    name: String,
    engine: Engine,
    ast: AST,
    state: Dynamic,
    functions: HashMap<String, Vec<usize>>,
    registration: Registration,
    shared: Arc<Shared>,
    owner: Mutex<Option<Owner>>,
}

/// The thread executing a runtime, its nesting depth and its mailbox.
struct Owner {
    thread: ThreadId,
    depth: u32,
    pump: Arc<Pump>,
}

/// Why a runtime could not be entered.
enum Refused {
    /// Another thread owns it; it may be pumping.
    Busy(Arc<Pump>),
    Other(String),
}

const REENTRANT: &str = "extension runtime invocation is reentrant";

impl std::fmt::Debug for ScriptRuntime {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ScriptRuntime")
            .field("name", &self.name)
            .finish_non_exhaustive()
    }
}

impl ScriptRuntime {
    /// Compile `source`, run its top level and `activate()` once.
    pub fn load(name: &str, source: &str, policy: SandboxPolicy) -> Result<Self, String> {
        if source.len() > policy.max_source_bytes {
            return Err("Rhai source exceeds sandbox size limit".into());
        }
        if let Some(line) = has_import(source) {
            return Err(format!(
                "Rhai sandbox disallows module imports (line {line})"
            ));
        }
        let shared = Arc::new(Shared {
            activation: Mutex::new(None),
            hosts: Mutex::new(Vec::new()),
        });
        let mut engine = Engine::new();
        engine.set_max_operations(policy.max_operations);
        engine.set_max_call_levels(64);
        engine.set_max_expr_depths(256, 128);
        engine.set_max_string_size(16 * 1024 * 1024);
        engine.set_max_array_size(1_000_000);
        engine.set_max_map_size(1_000_000);
        engine.set_module_resolver(rhai::module_resolvers::DummyModuleResolver::new());
        engine.disable_symbol("import");
        engine.disable_symbol("eval");
        let label = name.to_string();
        engine.on_print(move |text| eprintln!("[{label}] {text}"));
        let label = name.to_string();
        engine.on_debug(move |text, _, pos| eprintln!("[{label}] {pos:?} {text}"));
        register_api(&mut engine, &shared);

        let ast = engine.compile(source).map_err(|e| e.to_string())?;
        let mut functions: HashMap<String, Vec<usize>> = HashMap::new();
        for f in ast.iter_functions() {
            functions
                .entry(f.name.to_string())
                .or_default()
                .push(f.params.len());
        }
        if !functions.get("activate").is_some_and(|a| a.contains(&0)) {
            return Err("Rhai entrypoint 'fn activate()' was not found".into());
        }
        let mut state = Dynamic::from_map(Map::new()).into_shared();
        *shared.activation.lock().expect("activation lock") = Some(Registration::default());
        let result = {
            let mut scope = Scope::new();
            let options = CallFnOptions::new()
                .eval_ast(true)
                .rewind_scope(true)
                .bind_this_ptr(&mut state);
            engine.call_fn_with_options::<Dynamic>(options, &mut scope, &ast, "activate", ())
        };
        let registration = shared
            .activation
            .lock()
            .expect("activation lock")
            .take()
            .unwrap_or_default();
        if let Err(err) = result {
            return Err(format!("Rhai activation failed: {}", describe(&err)));
        }
        Ok(Self {
            name: name.into(),
            engine,
            ast,
            state,
            functions,
            registration,
            shared,
            owner: Mutex::new(None),
        })
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn registration(&self) -> &Registration {
        &self.registration
    }

    /// Whether a script function `name` taking zero or one argument exists.
    pub fn has_handler(&self, name: &str) -> bool {
        self.functions
            .get(name)
            .is_some_and(|a| a.contains(&0) || a.contains(&1))
    }

    /// Handler of command `name` (`command_<name>`).
    pub fn command_handler(name: &str) -> String {
        handler_name("command_", name)
    }

    fn enter(&self, host: HostCall) -> Result<Entry<'_>, Refused> {
        if DEPTH.with(Cell::get) >= MAX_INVOKE_DEPTH {
            return Err(Refused::Other(
                "extension runtime invocation depth exceeded".into(),
            ));
        }
        let me = std::thread::current().id();
        {
            let mut owner = self.owner.lock().expect("owner lock");
            match owner.as_mut() {
                Some(o) if o.thread == me => o.depth += 1,
                Some(o) => return Err(Refused::Busy(o.pump.clone())),
                None => {
                    *owner = Some(Owner {
                        thread: me,
                        depth: 1,
                        pump: PUMP.with(Arc::clone),
                    })
                }
            }
        }
        DEPTH.with(|d| d.set(d.get() + 1));
        self.shared.hosts.lock().expect("host stack").push(host);
        Ok(Entry { runtime: self })
    }

    /// Run the invocation on the thread that owns the runtime when it waits
    /// in [`pump_while`]; otherwise it is rejected as reentrant.
    fn forward(
        self: &Arc<Self>,
        pump: &Pump,
        handler: &str,
        arguments: &Value,
        host: HostCall,
    ) -> Result<Value, String> {
        let (reply_tx, reply_rx) = mpsc::channel();
        let runtime = self.clone();
        let handler = handler.to_string();
        let arguments = arguments.clone();
        let job: Job = Box::new(move || {
            let _ = reply_tx.send(runtime.invoke(&handler, &arguments, host));
        });
        if !pump.send(job) {
            return Err(REENTRANT.into());
        }
        reply_rx.recv().unwrap_or_else(|_| Err(REENTRANT.into()))
    }

    /// Call `handler` with `arguments`; host APIs go to `host`.
    pub fn invoke(
        self: &Arc<Self>,
        handler: &str,
        arguments: &Value,
        host: HostCall,
    ) -> Result<Value, String> {
        let arities = self
            .functions
            .get(handler)
            .ok_or_else(|| format!("extension handler was not found: {handler}"))?;
        let with_arg = arities.contains(&1);
        if !with_arg && !arities.contains(&0) {
            return Err(format!("extension handler was not found: {handler}"));
        }
        let _entry = match self.enter(host.clone()) {
            Ok(entry) => entry,
            Err(Refused::Busy(pump)) => return self.forward(&pump, handler, arguments, host),
            Err(Refused::Other(e)) => return Err(e),
        };
        let mut this = self.state.clone();
        let mut scope = Scope::new();
        let options = CallFnOptions::new()
            .eval_ast(true)
            .rewind_scope(true)
            .bind_this_ptr(&mut this);
        let result = if with_arg {
            self.engine.call_fn_with_options::<Dynamic>(
                options,
                &mut scope,
                &self.ast,
                handler,
                (json_to_dynamic(arguments),),
            )
        } else {
            self.engine
                .call_fn_with_options::<Dynamic>(options, &mut scope, &self.ast, handler, ())
        };
        let value = result.map_err(|e| describe(&e))?;
        dynamic_to_json(&value).map_err(|e| format!("extension handler returned {e}"))
    }

    /// A snapshot of the persistent `this` state as JSON.
    pub fn state(&self) -> Value {
        dynamic_to_json(&self.state).unwrap_or(Value::Null)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn host() -> HostCall {
        Arc::new(|method: &str, args: Value| match method {
            "ctx.isIdle" => Ok(json!(true)),
            "pi.getFlag" => Ok(json!(format!(
                "flag:{}",
                args["name"].as_str().unwrap_or("")
            ))),
            "ctx.ui.notify" => Ok(Value::Null),
            "fail" => Err("host failure".into()),
            other => Err(format!("unsupported extension host API: {other}")),
        })
    }

    fn load(source: &str) -> Result<Arc<ScriptRuntime>, String> {
        ScriptRuntime::load("test", source, SandboxPolicy::default()).map(Arc::new)
    }

    #[test]
    fn registers_during_activate_and_keeps_state() {
        let rt = load(
            r#"
            const GREETING = "hello";
            fn activate() {
                this.calls = 0;
                registerCommand("hello", "Hello", "Say hello");
                qt_register_command("bye", #{ label: "Bye" });
                registerTool(#{ name: "lookup", description: "Find", parameters: #{ type: "object" } });
                registerFlag("greeting", "string", "Hello", "Greeting text");
                registerFlag("verbose");
                registerFlag("mode", #{ type: "string", "default": "fast" });
                registerResource("skill", "skills/review/SKILL.md", "review");
                registerProvider("corp", #{ baseUrl: "https://corp" });
                registerProvider("json", "{\"baseUrl\":\"x\"}");
                on("input", "on_input");
                qt_on("tool_call", Fn("on_tool"));
                registerShortcut("ctrl+x", "on_key", "Do it");
                registerMessageRenderer("note", "render_note");
            }
            fn command_hello(args) {
                this.calls += 1;
                #{ text: `${global::GREETING} ${args.who} #${this.calls}`, flag: pi::getFlag(#{ name: "greeting" }) }
            }
            fn on_input() { #{ action: "continue" } }
            fn on_tool(e) { this.bump(); () }
            fn bump() { this.calls += 10; }
            "#,
        )
        .unwrap();
        let r = rt.registration();
        assert_eq!(r.commands.len(), 2);
        assert_eq!(r.commands[1].title.as_deref(), Some("Bye"));
        assert_eq!(r.tools[0].parameters, Some(json!({"type":"object"})));
        assert_eq!(r.flags[0].default, json!("Hello"));
        assert_eq!(r.flags[1].flag_type, "boolean");
        assert_eq!(r.flags[2].default, json!("fast"));
        assert_eq!(r.providers.len(), 2);
        assert_eq!(r.lifecycle_handlers[1].handler, "on_tool");
        assert_eq!(r.shortcuts[0].handler, "on_key");
        assert_eq!(r.message_renderers[0].custom_type, "note");

        let out = rt
            .invoke("command_hello", &json!({"who":"qt"}), host())
            .unwrap();
        assert_eq!(out, json!({"text":"hello qt #1","flag":"flag:greeting"}));
        let out = rt
            .invoke("command_hello", &json!({"who":"qt"}), host())
            .unwrap();
        assert_eq!(out["text"], json!("hello qt #2"));
        assert_eq!(rt.state(), json!({"calls": 2}));
        assert_eq!(
            rt.invoke("on_input", &json!({}), host()).unwrap(),
            json!({"action":"continue"})
        );
        assert_eq!(
            rt.invoke("on_tool", &json!({}), host()).unwrap(),
            Value::Null
        );
        assert_eq!(
            rt.invoke("missing", &json!({}), host()).unwrap_err(),
            "extension handler was not found: missing"
        );
    }

    #[test]
    fn registration_outside_activate_and_validation_errors() {
        let rt = load(r#"fn activate() {} fn late() { registerCommand("x"); }"#).unwrap();
        assert_eq!(
            rt.invoke("late", &json!({}), host()).unwrap_err(),
            "registration APIs are only available during activate()"
        );
        assert_eq!(
            load(r#"fn activate() { on("bogus", "h"); }"#).unwrap_err(),
            "Rhai activation failed: invalid extension lifecycle event: bogus"
        );
        assert_eq!(
            load(r#"fn activate() { on("input"); }"#).unwrap_err(),
            "Rhai activation failed: qt_on expects event, handler"
        );
        assert_eq!(
            load(r#"fn activate() { on("input", |e| e); }"#).unwrap_err(),
            "Rhai activation failed: extension handlers must be named script functions"
        );
        assert_eq!(
            load("fn other() {}").unwrap_err(),
            "Rhai entrypoint 'fn activate()' was not found"
        );
    }

    #[test]
    fn sandbox_limits() {
        let big = format!("fn activate() {{}}\n//{}", "x".repeat(256 * 1024));
        assert_eq!(
            load(&big).unwrap_err(),
            "Rhai source exceeds sandbox size limit"
        );
        assert_eq!(
            load("import \"fs\" as fs;\nfn activate() {}").unwrap_err(),
            "Rhai sandbox disallows module imports (line 1)"
        );
        assert!(load("fn activate() { eval(\"1\"); }").is_err());
        assert_eq!(
            load("fn activate() { loop {} }").unwrap_err(),
            "Rhai activation failed: sandbox step budget exceeded"
        );
        let rt = load("fn activate() {} fn spin() { loop {} }").unwrap();
        assert_eq!(
            rt.invoke("spin", &json!({}), host()).unwrap_err(),
            "sandbox step budget exceeded"
        );
        // The budget is per invocation: a later call runs normally.
        let rt = load("fn activate() {} fn count(n) { let i = 0; while i < n.n { i += 1; } i }")
            .unwrap();
        assert!(
            rt.invoke("count", &json!({"n": 1_000_000}), host())
                .is_err()
        );
        assert_eq!(
            rt.invoke("count", &json!({"n": 10}), host()).unwrap(),
            json!(10)
        );
    }

    #[test]
    fn host_errors_are_catchable_and_json_helpers_work() {
        let rt = load(
            r#"
            fn activate() {}
            fn run() {
                let caught = "";
                try { host_call("fail", #{}); } catch (e) { caught = e; }
                let encoded = qt_host_call("pi.getFlag", "{\"name\":\"x\"}");
                [caught, encoded, qt_json_string(qt_json_get("{\"a\":\"b\"}", "a")),
                 qt_json_get("{}", "missing"), json_parse("[1,2]"), json_stringify(#{ k: [true] }),
                 ctx::isIdle()]
            }
            fn bad_args() { host_call("pi.getFlag", 5) }
            "#,
        )
        .unwrap();
        assert_eq!(
            rt.invoke("run", &Value::Null, host()).unwrap(),
            json!([
                "host failure",
                "\"flag:x\"",
                "b",
                "null",
                [1, 2],
                "{\"k\":[true]}",
                true
            ])
        );
        assert_eq!(
            rt.invoke("bad_args", &Value::Null, host()).unwrap_err(),
            "extension host arguments must be an object"
        );
    }

    #[test]
    fn nested_invocation_on_one_thread_and_rejection_across_threads() {
        let rt = load(
            r#"
                fn activate() { this.renders = 0; }
                fn outer() { ctx::ui::custom(#{ handler: "inner" }) }
                fn inner(e) { this.renders += 1; #{ lines: ["x"], depth: e.depth } }
                fn deep() { ctx::ui::custom(#{ handler: "deep" }) }
                "#,
        )
        .unwrap();
        let weak = Arc::downgrade(&rt);
        let nested: HostCall = Arc::new(move |method: &str, args: Value| {
            assert_eq!(method, "ctx.ui.custom");
            let rt = weak.upgrade().unwrap();
            let inner_host: HostCall = Arc::new(|_: &str, _: Value| Ok(Value::Null));
            rt.invoke(
                args["handler"].as_str().unwrap(),
                &json!({"depth": 1}),
                inner_host,
            )
        });
        assert_eq!(
            rt.invoke("outer", &Value::Null, nested).unwrap(),
            json!({"lines":["x"],"depth":1})
        );
        assert_eq!(rt.state()["renders"], json!(1));

        fn recursive(rt: Arc<ScriptRuntime>) -> HostCall {
            Arc::new(move |_: &str, _: Value| {
                rt.invoke("deep", &Value::Null, recursive(rt.clone()))
            })
        }
        assert_eq!(
            rt.invoke("deep", &Value::Null, recursive(rt.clone()))
                .unwrap_err(),
            "extension runtime invocation depth exceeded"
        );

        // Another thread entering while this one runs is rejected.
        let rt2 = load("fn activate() {} fn wait() { host_call(\"block\") }").unwrap();
        let (tx, rx) = std::sync::mpsc::channel::<()>();
        let (started_tx, started_rx) = std::sync::mpsc::channel::<()>();
        let rx = Mutex::new(rx);
        let blocking: HostCall = Arc::new(move |_: &str, _: Value| {
            started_tx.send(()).unwrap();
            rx.lock().unwrap().recv().unwrap();
            Ok(Value::Null)
        });
        let worker = {
            let rt2 = rt2.clone();
            std::thread::spawn(move || rt2.invoke("wait", &Value::Null, blocking))
        };
        started_rx.recv().unwrap();
        assert_eq!(
            rt2.invoke("wait", &Value::Null, host()).unwrap_err(),
            "extension runtime invocation is reentrant"
        );
        tx.send(()).unwrap();
        assert_eq!(worker.join().unwrap().unwrap(), Value::Null);
    }

    #[test]
    fn calls_from_other_threads_run_on_a_pumping_owner() {
        // A command waits in a host call (like ctx.compact) while the engine
        // fires a hook of the same extension from another thread.
        let rt = load(
            r#"
            fn activate() { this.log = []; }
            fn command_squash() {
                this.log.push("start");
                let hook = ctx::compact(#{});
                this.log.push("end");
                hook
            }
            fn on_before(e) { this.log.push("hook " + e.n); #{ seen: e.n } }
            "#,
        )
        .unwrap();
        let owner = std::thread::current().id();
        let weak = Arc::downgrade(&rt);
        let compacting: HostCall = Arc::new(move |method: &str, _: Value| {
            assert_eq!(method, "ctx.compact");
            let rt = weak.upgrade().unwrap();
            pump_while(move || {
                std::thread::spawn(move || {
                    let hook_thread: HostCall = Arc::new(move |_: &str, _: Value| {
                        assert_eq!(std::thread::current().id(), owner);
                        Ok(Value::Null)
                    });
                    rt.invoke("on_before", &json!({"n": 7}), hook_thread)
                })
                .join()
                .unwrap()
            })
        });
        assert_eq!(
            rt.invoke("command_squash", &Value::Null, compacting)
                .unwrap(),
            json!({"seen": 7})
        );
        assert_eq!(rt.state()["log"], json!(["start", "hook 7", "end"]));

        // Without a pumping owner the call is still rejected.
        let rt2 = rt.clone();
        let busy: HostCall = Arc::new(move |_: &str, _: Value| {
            let rt2 = rt2.clone();
            std::thread::spawn(move || rt2.invoke("on_before", &json!({"n": 1}), host()))
                .join()
                .unwrap()
        });
        assert_eq!(
            rt.invoke("command_squash", &Value::Null, busy).unwrap_err(),
            "extension runtime invocation is reentrant"
        );
    }
}
