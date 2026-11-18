//! End-to-end tests of the extension runtime with a scripted host.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use serde_json::{Value, json};

use qt_core::AgentEvent;
use qt_core::engine::Delivery;
use qt_core::harness::hooks::{BeforeAgentStartEvent, ToolCallEvent, ToolResultEvent};
use qt_core::harness::{CompactionResult, ContextUsage, Hooks, SessionEvent, SessionStartReason};
use qt_core::message::{AgentMessage, ContentBlock, CustomMessage, Message};
use qt_core::provider::{ThinkingLevel, ToolSpec};
use qt_core::tools::{ToolContext, ToolOutput};

use qt_extensions::filters::display_path;
use qt_extensions::loader::{ExtensionOrigin, load_extension_roots};
use qt_extensions::ui::{ExtensionUi, NotifyKind};
use qt_extensions::{
    DiscoveryOptions, ExtensionHost, Extensions, InputOutcome, InputSource, SandboxPolicy,
};

#[derive(Default)]
struct MockState {
    calls: Vec<String>,
    name: Option<String>,
    idle: bool,
    model: Option<Value>,
    usage: Option<ContextUsage>,
    compact_error: Option<String>,
    active: Vec<String>,
    thinking: String,
}

#[derive(Default)]
struct MockHost {
    state: Mutex<MockState>,
}

impl MockHost {
    fn new() -> Arc<Self> {
        let h = Self::default();
        {
            let mut s = h.state.lock().unwrap();
            s.idle = true;
            s.active = vec!["read".into(), "bash".into()];
            s.thinking = "off".into();
        }
        Arc::new(h)
    }
    fn log(&self, entry: String) {
        self.state.lock().unwrap().calls.push(entry);
    }
    fn calls(&self) -> Vec<String> {
        self.state.lock().unwrap().calls.clone()
    }
}

impl ExtensionHost for MockHost {
    fn cwd(&self) -> PathBuf {
        std::env::temp_dir()
    }
    fn session_name(&self) -> Option<String> {
        self.state.lock().unwrap().name.clone()
    }
    fn set_session_name(&self, name: &str) -> Result<(), String> {
        self.state.lock().unwrap().name = Some(name.into());
        Ok(())
    }
    fn append_entry(&self, custom_type: &str, data: Value) -> Result<(), String> {
        self.log(format!("appendEntry {custom_type} {data}"));
        Ok(())
    }
    fn set_label(&self, entry_id: &str, label: Option<&str>) -> Result<(), String> {
        self.log(format!("setLabel {entry_id} {label:?}"));
        Ok(())
    }
    fn send_message(
        &self,
        message: CustomMessage,
        delivery: Delivery,
        trigger_turn: bool,
    ) -> Result<(), String> {
        self.log(format!(
            "sendMessage {} {} {delivery:?} {trigger_turn} {:?}",
            message.custom_type,
            message.content.plain_text(),
            message.details
        ));
        Ok(())
    }
    fn send_user_message(
        &self,
        content: Vec<ContentBlock>,
        delivery: Delivery,
    ) -> Result<(), String> {
        self.log(format!("sendUserMessage {} {delivery:?}", content.len()));
        Ok(())
    }
    fn active_tools(&self) -> Vec<String> {
        self.state.lock().unwrap().active.clone()
    }
    fn all_tools(&self) -> Vec<ToolSpec> {
        ["read", "bash", "edit"]
            .iter()
            .map(|n| ToolSpec {
                name: (*n).into(),
                description: format!("{n} tool"),
                parameters: json!({"type":"object"}),
            })
            .collect()
    }
    fn set_active_tools(&self, names: &[String]) -> Result<(), String> {
        self.state.lock().unwrap().active = names.to_vec();
        Ok(())
    }
    fn thinking_level(&self) -> String {
        self.state.lock().unwrap().thinking.clone()
    }
    fn set_thinking_level(&self, level: ThinkingLevel) -> Result<(), String> {
        self.state.lock().unwrap().thinking = level.as_str().into();
        Ok(())
    }
    fn set_model(&self, provider: &str, id: &str) -> Result<bool, String> {
        self.log(format!("setModel {provider}/{id}"));
        Ok(provider != "offline")
    }
    fn model(&self) -> Option<Value> {
        self.state.lock().unwrap().model.clone()
    }
    fn find_model(&self, provider: &str, id: &str) -> Option<Value> {
        (provider == "p").then(|| json!({"provider": provider, "id": id}))
    }
    fn model_catalog(&self) -> Value {
        json!({"providers": [], "models": []})
    }
    fn system_prompt(&self) -> String {
        "SYSTEM".into()
    }
    fn context_usage(&self) -> Option<ContextUsage> {
        self.state.lock().unwrap().usage
    }
    fn context_messages(&self) -> Vec<AgentMessage> {
        vec![AgentMessage::Llm(Message::user_text("hello"))]
    }
    fn compact(&self, custom_instructions: Option<&str>) -> Result<CompactionResult, String> {
        self.log(format!("compact {custom_instructions:?}"));
        if let Some(e) = self.state.lock().unwrap().compact_error.clone() {
            return Err(e);
        }
        Ok(CompactionResult {
            summary: "summary".into(),
            first_kept_entry_id: "e1".into(),
            tokens_before: 250_000,
            details: None,
        })
    }
    fn is_idle(&self) -> bool {
        self.state.lock().unwrap().idle
    }
    fn has_pending_messages(&self) -> bool {
        false
    }
    fn abort(&self) {
        self.log("abort".into());
    }
}

#[derive(Default)]
struct RecordingUi {
    notes: Mutex<Vec<String>>,
}

impl ExtensionUi for RecordingUi {
    fn notify(&self, message: &str, kind: NotifyKind) {
        self.notes
            .lock()
            .unwrap()
            .push(format!("{kind:?}: {message}"));
    }
    fn confirm(&self, _title: &str, _message: &str, _timeout: Option<std::time::Duration>) -> bool {
        true
    }
}

fn write(path: &Path, text: &str) {
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, text).unwrap();
}

fn load(paths: &[PathBuf]) -> Extensions {
    let roots: Vec<String> = paths.iter().map(|p| display_path(p)).collect();
    let result = load_extension_roots(&roots, &[], &ExtensionOrigin::Cli, SandboxPolicy::default());
    assert!(result.diagnostics.is_empty(), "{:?}", result.diagnostics);
    Extensions::from_result(DiscoveryOptions::default(), result)
}

fn example() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../examples/extensions/astra-auto-compact.rhai")
}

#[test]
fn astra_example_compacts_only_above_threshold() {
    let ext = load(&[example()]);
    let host = MockHost::new();
    let ui = Arc::new(RecordingUi::default());
    ext.set_host(Some(host.clone()));
    ext.set_ui(ui.clone());
    let set = |model: Value, tokens: Option<u64>, idle: bool| {
        let mut s = host.state.lock().unwrap();
        s.model = Some(model);
        s.usage = Some(ContextUsage {
            tokens,
            context_window: 1_000_000,
            percent: tokens.map(|t| t as f64 / 10_000.0),
        });
        s.idle = idle;
        s.calls.clear();
    };
    let input = || {
        ext.emit_input("next prompt", Value::Null, InputSource::Interactive)
            .unwrap()
    };
    let continued = InputOutcome::Continue {
        text: "next prompt".into(),
        images: Value::Null,
    };

    set(json!({"id": "Astra-1", "name": "x"}), Some(200_001), true);
    assert_eq!(input(), continued);
    assert_eq!(host.calls(), ["compact None"]);

    set(
        json!({"id": "m", "name": "Big ASTRA model"}),
        Some(300_000),
        true,
    );
    input();
    assert_eq!(host.calls(), ["compact None"]);

    for (model, tokens, idle) in [
        (json!({"id": "astra", "name": "a"}), Some(200_000), true),
        (json!({"id": "astra", "name": "a"}), None, true),
        (json!({"id": "other", "name": "gpt"}), Some(900_000), true),
        (json!({"id": "astra", "name": "a"}), Some(900_000), false),
    ] {
        set(model, tokens, idle);
        assert_eq!(input(), continued);
        assert!(host.calls().is_empty(), "{tokens:?} {idle}");
    }

    set(json!({"id": "astra", "name": "a"}), Some(250_000), true);
    host.state.lock().unwrap().compact_error = Some("provider down".into());
    assert_eq!(input(), continued);
    assert_eq!(
        ui.notes.lock().unwrap().clone(),
        ["Warning: Astra auto-compaction failed; continuing with the existing context."]
    );
}

const API_SCRIPT: &str = r#"
fn activate() {
    this.seen = 0;
    registerCommand("probe", "Probe", "Exercise host calls");
    registerTool(#{ name: "shout", description: "Upper-case text", parameters: #{ type: "object", properties: #{ text: #{ type: "string" } } } });
    registerFlag("greeting", "string", "Hello", "Greeting text");
    registerFlag("loud");
    registerResource("skill", "skills/demo/SKILL.md");
    registerShortcut("ctrl+g", "on_key", "Greet");
    registerMessageRenderer("note", "render_note");
    on("input", "on_input");
    on("before_agent_start", "on_start");
    on("tool_call", "on_tool_call");
    on("tool_result", "on_tool_result");
    on("context", "on_context");
    on("session_start", "on_session");
    on("turn_start", "on_turn");
    on("turn_end", "on_turn");
    on("agent_end", "on_turn");
    on("session_before_switch", "on_switch");
}

fn command_probe(args) {
    pi::setSessionName(#{ name: "named " + args.args });
    pi::appendEntry(#{ customType: "state", data: #{ n: 1 } });
    pi::setLabel(#{ entryId: "e1", label: () });
    pi::sendMessage(#{ customType: "note", content: "hi", details: #{ k: 2 }, deliverAs: "followUp" });
    pi::sendUserMessage(#{ content: [#{ type: "text", text: "go" }], deliverAs: "steer" });
    pi::setActiveTools(#{ toolNames: ["read"] });
    pi::setThinkingLevel(#{ level: "high" });
    let bad = "";
    try { pi::setActiveTools(#{ toolNames: ["nope"] }); } catch (e) { bad = e; }
    let bad_level = "";
    try { pi::setThinkingLevel(#{ level: "max" }); } catch (e) { bad_level = e; }
    let compact = ctx::compact(#{ customInstructions: "keep todos" });
    #{
        text: "probed",
        name: pi::getSessionName(),
        flag: pi::getFlag(#{ name: "greeting" }),
        loud: pi::getFlag(#{ name: "loud" }),
        commands: pi::getCommands(),
        active: pi::getActiveTools(),
        all: pi::getAllTools().len(),
        thinking: pi::getThinkingLevel(),
        model_set: pi::setModel(#{ provider: "offline", id: "x" }),
        found: ctx::getModel(#{ provider: "p", id: "m" }),
        prompt: ctx::getSystemPrompt(),
        context: ctx::getContext().messages.len(),
        idle: ctx::isIdle(),
        pending: ctx::hasPendingMessages(),
        confirm: ctx::ui::confirm(#{ title: "t", message: "m" }),
        bad: bad,
        bad_level: bad_level,
        summary: compact.summary,
    }
}

fn tool_shout(args) {
    #{ content: [#{ type: "text", text: args.text.to_upper() }], details: #{ length: args.text.len() } }
}

fn on_input(event) {
    this.seen += 1;
    if event.text == "/secret" { return #{ action: "handled" }; }
    if event.text.starts_with("!") { return #{ action: "transform", text: event.text.sub_string(1) }; }
    #{ action: "continue" }
}

fn on_start(event) {
    #{ systemPrompt: event.systemPrompt + " +ext", message: #{ customType: "ctx", content: "injected " + event.prompt } }
}

fn on_tool_call(event) {
    if event.toolName == "bash" && event.input.command.contains("rm -rf") {
        return #{ block: true, reason: "dangerous command" };
    }
}

fn on_tool_result(event) {
    if event.toolName == "read" { #{ content: [#{ type: "text", text: "[redacted]" }], isError: false } }
}

fn on_context(event) {
    let msgs = event.messages;
    msgs.push(#{ role: "user", content: "reminder" });
    #{ messages: msgs }
}

fn on_session(event) { host_call("ctx.ui.notify", #{ message: "session " + event.reason }); }

fn on_turn(event) { host_call("ctx.ui.notify", #{ message: event.type }); }

fn on_switch(event) { #{ cancel: event.reason == "new" } }

fn on_key(event) { "pressed " + event.key }

fn render_note(event) { #{ lines: ["note: " + event.message.content, "w=" + event.width] } }
"#;

#[test]
fn host_api_commands_tools_flags_and_events() {
    let dir = tempfile::tempdir().unwrap();
    let script = dir.path().join("api/index.rhai");
    write(&script, API_SCRIPT);
    let ext = load(&[dir.path().join("api")]);
    let host = MockHost::new();
    let ui = Arc::new(RecordingUi::default());
    ext.set_host(Some(host.clone()));
    ext.set_ui(ui.clone());

    // Flags from the CLI.
    let rest = ext
        .apply_cli_flags(&[
            "--greeting".into(),
            "Hi".into(),
            "--loud".into(),
            "--model".into(),
            "x".into(),
        ])
        .unwrap();
    assert_eq!(rest, ["--model", "x"]);
    assert_eq!(ext.flag_value("greeting"), json!("Hi"));
    assert_eq!(
        ext.apply_cli_flags(&["--greeting".into()]).unwrap_err(),
        "Extension flag --greeting requires a value"
    );
    assert_eq!(
        ext.set_flag_values(&serde_json::from_value(json!({"loud": "maybe"})).unwrap())
            .unwrap_err(),
        "Extension flag --loud expects a boolean value"
    );
    assert_eq!(
        ext.set_flag_values(&serde_json::from_value(json!({"other": true})).unwrap())
            .unwrap_err(),
        "Unknown option: --other"
    );

    // Command with host calls.
    let out = ext.run_command("probe", "x").unwrap();
    assert_eq!(out.lines, ["probed"]);
    let v = out.value;
    assert_eq!(v["name"], json!("named x"));
    assert_eq!(v["flag"], json!("Hi"));
    assert_eq!(v["loud"], json!(true));
    assert_eq!(
        v["commands"],
        json!([{"name":"probe","description":"Exercise host calls","source":"extension"}])
    );
    assert_eq!(v["active"], json!(["read"]));
    assert_eq!(v["all"], json!(3));
    assert_eq!(v["thinking"], json!("high"));
    assert_eq!(v["model_set"], json!(false));
    assert_eq!(v["found"], json!({"provider":"p","id":"m"}));
    assert_eq!(v["prompt"], json!("SYSTEM"));
    assert_eq!(v["context"], json!(1));
    assert_eq!(v["idle"], json!(true));
    assert_eq!(v["pending"], json!(false));
    assert_eq!(v["confirm"], json!(true));
    assert_eq!(v["bad"], json!("unknown active tool: nope"));
    assert_eq!(v["bad_level"], json!("invalid thinking level"));
    assert_eq!(v["summary"], json!("summary"));
    assert_eq!(
        host.calls(),
        [
            "appendEntry state {\"n\":1}",
            "setLabel e1 None",
            "sendMessage note hi FollowUp false Some(Object {\"k\": Number(2)})",
            "sendUserMessage 1 Steer",
            "compact Some(\"keep todos\")",
            "setModel offline/x",
        ]
    );
    assert_eq!(
        ext.run_command("nope", "").unwrap_err(),
        "extension command contribution not found: nope"
    );

    // Tool through the qt_core Tool trait.
    let tools = ext.tools();
    assert_eq!(tools.len(), 1);
    assert_eq!(tools[0].name(), "shout");
    assert_eq!(
        tools[0].parameters()["properties"]["text"]["type"],
        json!("string")
    );
    let rt = tokio::runtime::Runtime::new().unwrap();
    let output: ToolOutput =
        rt.block_on(tools[0].execute(json!({"text": "hey"}), ToolContext::new(dir.path())));
    assert_eq!(output.text_content(), "HEY");
    assert_eq!(output.details, Some(json!({"length": 3})));
    let failed = rt.block_on(tools[0].execute(json!({}), ToolContext::new(dir.path())));
    assert!(failed.is_error);
    assert_eq!(
        failed.details.as_ref().unwrap()["code"],
        json!("execution_error")
    );

    // Input routing.
    assert_eq!(
        ext.emit_input("/secret", Value::Null, InputSource::Interactive)
            .unwrap(),
        InputOutcome::Handled
    );
    assert_eq!(
        ext.emit_input("!ls", json!([]), InputSource::Rpc).unwrap(),
        InputOutcome::Continue {
            text: "ls".into(),
            images: json!([])
        }
    );
    assert_eq!(
        ext.emit_input("x", json!(5), InputSource::Rpc).unwrap_err(),
        "extension input event images must be null or a JSON array"
    );
    assert!(ext.emit_session_before_switch("new", None));
    assert!(!ext.emit_session_before_switch("resume", Some("/s.jsonl")));

    // Contributions.
    assert_eq!(
        ext.resources()[0].path,
        dir.path().join("api/skills/demo/SKILL.md")
    );
    assert_eq!(ext.shortcuts()[0].key, "ctrl+g");
    assert_eq!(ext.run_shortcut("ctrl+g").unwrap(), json!("pressed ctrl+g"));
    let note = CustomMessage::new(
        "note",
        qt_core::message::UserContent::Text("hello".into()),
        true,
    );
    assert_eq!(
        ext.render_message(&note, 40, false).unwrap().unwrap(),
        ["note: hello", "w=40"]
    );
    assert_eq!(ext.commands()[0].title.as_deref(), Some("Probe"));
    let summary = &ext.summaries()[0];
    assert_eq!(summary.name, "api");
    assert!(summary.events.contains(&"tool_call".to_string()));

    // Hooks through qt_core.
    let hooks = Hooks::new();
    ext.install_hooks(&hooks);
    let started = hooks.run_before_agent_start(&BeforeAgentStartEvent {
        prompt: Message::user_text("do it"),
        system_prompt: "base".into(),
        cwd: dir.path().to_path_buf(),
    });
    assert_eq!(started.system_prompt.as_deref(), Some("base +ext"));
    assert_eq!(started.messages[0].content.plain_text(), "injected do it");
    let call = |name: &str, cmd: &str| ToolCallEvent {
        turn_index: 0,
        tool_call_id: "c1".into(),
        tool_name: name.into(),
        args: json!({"command": cmd}),
    };
    let blocked = hooks.run_tool_call(&call("bash", "rm -rf /")).unwrap();
    assert_eq!(blocked.reason.as_deref(), Some("dangerous command"));
    assert!(hooks.run_tool_call(&call("bash", "ls")).is_none());
    let out = hooks.run_tool_result(ToolResultEvent {
        turn_index: 0,
        tool_call_id: "c2".into(),
        tool_name: "read".into(),
        args: json!({}),
        output: ToolOutput::text("secret"),
    });
    assert_eq!(out.text_content(), "[redacted]");
    let ctx = hooks.run_context(vec![AgentMessage::Llm(Message::user_text("a"))], 0);
    assert_eq!(ctx.len(), 2);

    ui.notes.lock().unwrap().clear();
    hooks.emit_session(&SessionEvent::Start {
        reason: SessionStartReason::Resume,
        session_id: "s".into(),
        session_file: None,
        previous_session_file: None,
    });
    for event in [
        AgentEvent::AgentStart,
        AgentEvent::AssistantStart,
        AgentEvent::AssistantStart,
        AgentEvent::AgentEnd {
            usage: Default::default(),
            aborted: false,
        },
    ] {
        hooks.emit_agent_event(&event);
    }
    assert_eq!(
        ui.notes.lock().unwrap().clone(),
        [
            "Info: session resume",
            "Info: turn_start",
            "Info: turn_end",
            "Info: turn_start",
            "Info: turn_end",
            "Info: agent_end",
        ]
    );

    ext.uninstall_hooks();
    assert!(hooks.run_tool_call(&call("bash", "rm -rf /")).is_none());
}

#[test]
fn failing_hooks_are_reported_and_do_not_change_results() {
    let dir = tempfile::tempdir().unwrap();
    write(
        &dir.path().join("bad.rhai"),
        r#"
        fn activate() { on("tool_call", "boom"); on("before_agent_start", "wrong"); on("input", "bad_input"); }
        fn boom(e) { throw "kaput"; }
        fn wrong(e) { #{ systemPrompt: 5 } }
        fn bad_input(e) { #{ action: "explode" } }
        "#,
    );
    let ext = load(&[dir.path().join("bad.rhai")]);
    let reports = Arc::new(Mutex::new(Vec::new()));
    let r = reports.clone();
    ext.set_reporter(Arc::new(move |m: &str| {
        r.lock().unwrap().push(m.to_string())
    }));
    let hooks = Hooks::new();
    ext.install_hooks(&hooks);
    assert!(
        hooks
            .run_tool_call(&ToolCallEvent {
                turn_index: 0,
                tool_call_id: "1".into(),
                tool_name: "bash".into(),
                args: json!({}),
            })
            .is_none()
    );
    let start = hooks.run_before_agent_start(&BeforeAgentStartEvent {
        prompt: Message::user_text("x"),
        system_prompt: "base".into(),
        cwd: dir.path().to_path_buf(),
    });
    assert_eq!(start.system_prompt, None);
    assert_eq!(
        reports.lock().unwrap().clone(),
        [
            "extension tool_call hook failed: bad: kaput",
            "extension before_agent_start hook failed: bad: extension before_agent_start result field 'systemPrompt' must be a string",
        ]
    );
    assert_eq!(
        ext.emit_input("x", Value::Null, InputSource::Interactive)
            .unwrap_err(),
        "extension input event result action must be continue, transform, or handled"
    );
    // Without a host, session APIs fail inside the script.
    write(
        &dir.path().join("nohost.rhai"),
        r#"fn activate() { registerCommand("n"); } fn command_n(a) { pi::getSessionName() }"#,
    );
    let ext = load(&[dir.path().join("nohost.rhai")]);
    assert_eq!(
        ext.run_command("n", "").unwrap_err(),
        "extension command /n failed: extension host is not available"
    );
}

#[cfg(unix)]
#[test]
fn subprocess_extension_commands_tools_and_events() {
    use std::os::unix::fs::PermissionsExt;
    let dir = tempfile::tempdir().unwrap();
    let ext_dir = dir.path().join("py");
    write(
        &ext_dir.join("extension.json"),
        r#"{"name":"sub","executable":"run.sh","commands":[{"name":"hello"}],
            "tools":[{"name":"echo","description":"Echo"}],
            "metadata":{"lifecycle_handlers":[{"event":"tool_call"},{"event":"input"}]}}"#,
    );
    // A tiny line-protocol server in sh: reply based on the method name.
    write(
        &ext_dir.join("run.sh"),
        r#"#!/bin/sh
read line
id=$(printf '%s' "$line" | sed 's/.*"id":\([0-9]*\).*/\1/')
case "$line" in
  *qt.command.invoke*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"text\":\"hello from sub\"}}" ;;
  *qt.tool.invoke*) echo "diag" >&2; echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":\"echoed\"}" ;;
  *qt.event.tool_call*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"result\":{\"block\":true,\"reason\":\"sub says no\"}}" ;;
  *qt.event.input*) echo "{\"jsonrpc\":\"2.0\",\"id\":$id,\"error\":{\"code\":-1,\"message\":\"input refused\"}}" ;;
esac
"#,
    );
    std::fs::set_permissions(
        ext_dir.join("run.sh"),
        std::fs::Permissions::from_mode(0o755),
    )
    .unwrap();
    let ext = load(&[dir.path().to_path_buf()]);
    let reports = Arc::new(Mutex::new(Vec::new()));
    let r = reports.clone();
    ext.set_reporter(Arc::new(move |m: &str| {
        r.lock().unwrap().push(m.to_string())
    }));

    assert_eq!(
        ext.run_command("hello", "a b").unwrap().lines,
        ["hello from sub"]
    );
    let rt = tokio::runtime::Runtime::new().unwrap();
    let tool = ext.tools().pop().unwrap();
    assert_eq!(tool.description(), "Echo");
    let out = rt.block_on(tool.execute(json!({"x":1}), ToolContext::new(dir.path())));
    assert_eq!(out.text_content(), "echoed");
    assert_eq!(reports.lock().unwrap().clone(), ["[sub] diag"]);

    let hooks = Hooks::new();
    ext.install_hooks(&hooks);
    let d = hooks
        .run_tool_call(&ToolCallEvent {
            turn_index: 0,
            tool_call_id: "1".into(),
            tool_name: "bash".into(),
            args: json!({"command":"ls"}),
        })
        .unwrap();
    assert_eq!(d.reason.as_deref(), Some("sub says no"));
    assert_eq!(
        ext.emit_input("x", Value::Null, InputSource::Interactive)
            .unwrap_err(),
        "extension 'sub' input event failed: input refused"
    );
}

#[test]
fn provider_registrations_extend_the_catalog() {
    let dir = tempfile::tempdir().unwrap();
    write(
        &dir.path().join("prov.rhai"),
        r#"fn activate() {
            registerProvider("corp", #{ baseUrl: "https://llm.corp.example/v1", api: "openai-completions", apiKey: "CORP_KEY",
                models: [#{ id: "corp-large", name: "Corp Large", contextWindow: 64000, maxTokens: 4096 }] });
            registerProvider("broken", #{ models: [#{ name: "no id" }] });
        }"#,
    );
    let ext = load(&[dir.path().join("prov.rhai")]);
    let mut catalog = qt_core::catalog::ModelCatalog::builtin().clone();
    let errors = ext.apply_providers(&mut catalog);
    let m = catalog
        .find_model("corp", "corp-large")
        .expect("registered model");
    assert_eq!(
        (m.context_window, m.base_url.as_str()),
        (64000, "https://llm.corp.example/v1")
    );
    assert_eq!(errors.len(), 1, "{errors:?}");
    assert!(errors[0].starts_with("extension prov provider broken: "));
}

#[test]
fn reference_modules_are_listed_but_not_executed() {
    let dir = tempfile::tempdir().unwrap();
    write(
        &dir.path().join("ref/index.ts"),
        r#"export default (pi) => { pi.registerCommand("plan", { description: "Plan" }); pi.on("input", () => {}); }"#,
    );
    let ext = load(&[dir.path().join("ref")]);
    assert_eq!(ext.commands()[0].description.as_deref(), Some("Plan"));
    assert_eq!(
        ext.run_command("plan", "").unwrap_err(),
        "extension command /plan failed: not_implemented: extension command 'plan': reference modules are not executed by the native extension host"
    );
    // Reference handlers are not invoked.
    assert!(!ext.has_handlers("input"));
}
