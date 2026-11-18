//! Extensions driving a real `qt_core` engine against a local mock
//! OpenAI-compatible server (an Ollama instance on 127.0.0.1).

use std::io::{Read, Write};
use std::net::TcpListener;
use std::path::Path;
use std::sync::{Arc, Mutex};

use serde_json::{Value, json};

use qt_core::config::AgentHome;
use qt_core::engine::Engine;
use qt_core::provider::ModelRef;
use qt_core::session::EntryKind;

use qt_extensions::filters::display_path;
use qt_extensions::loader::{ExtensionOrigin, load_extension_roots};
use qt_extensions::{ConversationHost, DiscoveryOptions, Extensions, SandboxPolicy};

fn sse(chunks: &[Value]) -> String {
    let mut out: String = chunks.iter().map(|c| format!("data: {c}\n\n")).collect();
    out.push_str("data: [DONE]\n\n");
    out
}

fn text_reply(text: &str) -> String {
    sse(&[
        json!({"choices":[{"delta":{"content":text}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}}),
    ])
}

fn tool_reply(id: &str, name: &str, args: Value) -> String {
    sse(&[
        json!({"choices":[{"delta":{"tool_calls":[{"index":0,"id":id,"function":{"name":name,"arguments":args.to_string()}}]}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}}),
    ])
}

/// Serve `replies` in order on a background thread; returns the port and
/// the recorded request bodies.
fn serve(replies: Vec<String>) -> (u16, Arc<Mutex<Vec<Value>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    let recorded = Arc::new(Mutex::new(Vec::new()));
    let r = recorded.clone();
    std::thread::spawn(move || {
        for reply in replies {
            let Ok((mut socket, _)) = listener.accept() else {
                return;
            };
            let mut buf = Vec::new();
            let mut chunk = [0u8; 8192];
            loop {
                let n = socket.read(&mut chunk).unwrap_or(0);
                buf.extend_from_slice(&chunk[..n]);
                let text = String::from_utf8_lossy(&buf).to_string();
                if let Some(split) = text.find("\r\n\r\n") {
                    let len = text[..split]
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase()
                                .strip_prefix("content-length:")
                                .and_then(|v| v.trim().parse::<usize>().ok())
                        })
                        .unwrap_or(0);
                    if buf.len() >= split + 4 + len {
                        if let Ok(v) = serde_json::from_slice(&buf[split + 4..split + 4 + len]) {
                            r.lock().unwrap().push(v);
                        }
                        break;
                    }
                }
                if n == 0 {
                    break;
                }
            }
            let response = format!(
                "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{reply}"
            );
            let _ = socket.write_all(response.as_bytes());
            let _ = socket.flush();
        }
    });
    (port, recorded)
}

const SCRIPT: &str = r#"
fn activate() {
    registerCommand("setup", "Setup", "Name the session and store state");
    registerCommand("squash", "Squash", "Compact now");
    on("before_agent_start", "on_start");
    on("tool_call", "on_tool_call");
    on("session_before_compact", "on_before_compact");
    on("session_compact", "on_compact");
}

fn on_before_compact(event) { this.before = event.reason; () }

fn on_compact(event) { this.after = event.compactionEntry.summary.contains("SUMMARY OF WORK"); }

fn on_start(event) { #{ systemPrompt: event.systemPrompt + "\nEXTENSION RULE" } }

fn on_tool_call(event) {
    if event.toolName == "bash" { #{ block: true, reason: "bash is blocked by policy" } }
}

fn command_setup(args) {
    pi::setSessionName(#{ name: args.args });
    pi::appendEntry(#{ customType: "ext-state", data: #{ count: 3 } });
    pi::setActiveTools(#{ toolNames: ["read", "bash"] });
    #{ text: "name=" + pi::getSessionName() + " tools=" + pi::getActiveTools().len()
        + " idle=" + ctx::isIdle() + " model=" + ctx::getModel().id
        + " prompt=" + ctx::getSystemPrompt().contains("qt") }
}

fn command_squash(args) {
    let r = "";
    this.before = ""; this.after = "";
    try { ctx::compact(#{}); r = "compacted"; } catch (e) { r = "compact failed: " + e; }
    r + " before=" + this.before + " after=" + this.after
}
"#;

#[test]
fn hooks_and_host_calls_against_a_real_conversation() {
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("home");
    std::fs::create_dir_all(&home).unwrap();
    let (port, requests) = serve(vec![
        tool_reply("call_1", "bash", json!({"command": "ls"})),
        text_reply("done"),
        text_reply("SUMMARY OF WORK"),
    ]);
    std::fs::write(
        home.join("settings.json"),
        json!({
            "ollama":{"instances":[{"id":"mock","baseUrl":format!("http://127.0.0.1:{port}")}]},
            "compaction":{"keepRecentTokens":1},
            "retry":{"enabled":false}
        })
        .to_string(),
    )
    .unwrap();
    let work = dir.path().join("work");
    std::fs::create_dir_all(&work).unwrap();
    let engine = Engine::new(AgentHome::new(&home), &work).unwrap();
    let conv = engine.new_conversation(Some(ModelRef::new("ollama:mock", "m")));

    let script = dir.path().join("ext/policy.rhai");
    std::fs::create_dir_all(script.parent().unwrap()).unwrap();
    std::fs::write(&script, SCRIPT).unwrap();
    let loaded = load_extension_roots(
        &[display_path(&script)],
        &[],
        &ExtensionOrigin::Cli,
        SandboxPolicy::default(),
    );
    assert!(loaded.diagnostics.is_empty(), "{:?}", loaded.diagnostics);
    let ext = Extensions::from_result(DiscoveryOptions::default(), loaded);
    ext.install_hooks(engine.hooks());
    ext.set_host(Some(Arc::new(ConversationHost::new(conv.clone()))));

    let out = ext.run_command("setup", "My session").unwrap();
    assert_eq!(
        out.lines,
        ["name=My session tools=2 idle=true model=m prompt=true"]
    );
    assert_eq!(conv.name().as_deref(), Some("My session"));
    assert_eq!(conv.active_tools(), ["read", "bash"]);

    let rx = conv.prompt("list files", vec![]).unwrap();
    while rx.recv_blocking().is_ok() {}

    let recorded = requests.lock().unwrap().clone();
    assert_eq!(recorded.len(), 2);
    let system = recorded[0]["messages"][0]["content"]
        .as_str()
        .unwrap_or_default()
        .to_string();
    assert!(system.ends_with("EXTENSION RULE"), "{system}");
    let tool_result = recorded[1]["messages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["role"] == "tool")
        .unwrap()
        .clone();
    assert!(
        tool_result["content"]
            .to_string()
            .contains("bash is blocked by policy"),
        "{tool_result}"
    );

    // The extension state entry was persisted on the branch.
    assert!(conv.entries().iter().any(
        |e| matches!(&e.kind, EntryKind::Custom { custom_type, .. } if custom_type == "ext-state")
    ));

    // ctx.compact runs synchronously from a command thread; the command's
    // own compaction hooks run (on the command thread) while it waits.
    let squash = ext.run_command("squash", "").unwrap();
    assert_eq!(squash.lines, ["compacted before=manual after=true"]);
    assert_eq!(requests.lock().unwrap().len(), 3);
    assert!(conv.entries().iter().any(
        |e| matches!(&e.kind, EntryKind::Compaction(c) if c.summary.contains("SUMMARY OF WORK"))
    ));
    let again = ext.run_command("squash", "").unwrap();
    assert!(
        again.lines[0].starts_with("compact failed: "),
        "{:?}",
        again.lines
    );
    assert!(Path::new(&home).exists());
    ext.uninstall_hooks();
}

const REWRITE: &str = r#"
fn activate() {
    on("tool_call", "on_tool_call");
    on("message_end", "on_message_end");
    on("input", "on_input");
}

fn on_tool_call(event) {
    if event.toolName == "read" { #{ input: #{ path: "rewritten.txt" } } }
}

fn on_message_end(event) {
    let m = event.message;
    if m.role == "assistant" { m.content = [#{ type: "text", text: "redacted" }]; #{ message: m } }
}

fn on_input(event) {
    if event.text == "/secret" { return #{ action: "handled" }; }
    #{ action: "transform", text: event.text + "!" }
}
"#;

#[test]
fn hooks_rewrite_tool_input_messages_and_input() {
    use qt_core::harness::Hooks;
    use qt_core::harness::hooks::{InputOutcome, InputSource, ToolCallEvent, ToolCallOutcome};
    use qt_core::message::{AgentMessage, AssistantMessage, ContentBlock, Message};

    let dir = tempfile::tempdir().unwrap();
    let script = dir.path().join("rewrite.rhai");
    std::fs::write(&script, REWRITE).unwrap();
    let loaded = load_extension_roots(
        &[display_path(&script)],
        &[],
        &ExtensionOrigin::Cli,
        SandboxPolicy::default(),
    );
    assert!(loaded.diagnostics.is_empty(), "{:?}", loaded.diagnostics);
    let ext = Extensions::from_result(DiscoveryOptions::default(), loaded);
    let hooks = Hooks::new();
    ext.install_hooks(&hooks);

    let call = |name: &str| ToolCallEvent {
        turn_index: 0,
        tool_call_id: "1".into(),
        tool_name: name.into(),
        args: json!({"path": "a.txt"}),
    };
    assert_eq!(
        hooks.run_tool_call_chain(&call("read")),
        ToolCallOutcome::Run(json!({"path": "rewritten.txt"}))
    );
    assert_eq!(
        hooks.run_tool_call_chain(&call("ls")),
        ToolCallOutcome::Run(json!({"path": "a.txt"}))
    );

    let replaced = hooks.run_message_end(AgentMessage::Llm(Message::Assistant(AssistantMessage {
        content: vec![ContentBlock::text("secret")],
        ..Default::default()
    })));
    assert_eq!(
        replaced.as_assistant().unwrap().content,
        vec![ContentBlock::text("redacted")]
    );
    let user = AgentMessage::Llm(Message::user_text("hi"));
    assert_eq!(hooks.run_message_end(user.clone()), user);

    assert_eq!(
        hooks.run_input("hello", vec![], InputSource::Interactive),
        Ok(InputOutcome::Continue {
            text: "hello!".into(),
            images: vec![]
        })
    );
    assert_eq!(
        hooks.run_input("/secret", vec![], InputSource::Rpc),
        Ok(InputOutcome::Handled)
    );
    ext.uninstall_hooks();
    assert_eq!(
        hooks.run_tool_call_chain(&call("read")),
        ToolCallOutcome::Run(json!({"path": "a.txt"}))
    );
}

/// examples/extensions/compact-command.rhai: `/squash` compacts from a
/// command while the extension's own compaction hooks run.
#[test]
fn compact_command_example_runs_its_hooks() {
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("home");
    std::fs::create_dir_all(&home).unwrap();
    let (port, requests) = serve(vec![text_reply("done"), text_reply("SQUASHED")]);
    std::fs::write(
        home.join("settings.json"),
        json!({
            "ollama":{"instances":[{"id":"mock","baseUrl":format!("http://127.0.0.1:{port}")}]},
            "compaction":{"keepRecentTokens":1},
            "retry":{"enabled":false}
        })
        .to_string(),
    )
    .unwrap();
    let work = dir.path().join("work");
    std::fs::create_dir_all(&work).unwrap();
    let engine = Engine::new(AgentHome::new(&home), &work).unwrap();
    let conv = engine.new_conversation(Some(ModelRef::new("ollama:mock", "m")));
    let example = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../examples/extensions/compact-command.rhai");
    let loaded = load_extension_roots(
        &[display_path(&example)],
        &[],
        &ExtensionOrigin::Cli,
        SandboxPolicy::default(),
    );
    assert!(loaded.diagnostics.is_empty(), "{:?}", loaded.diagnostics);
    let ext = Extensions::from_result(DiscoveryOptions::default(), loaded);
    ext.install_hooks(engine.hooks());
    ext.set_host(Some(Arc::new(ConversationHost::new(conv.clone()))));

    let rx = conv.prompt("hello", vec![]).unwrap();
    while rx.recv_blocking().is_ok() {}
    let out = ext.run_command("squash", "  keep todos ").unwrap();
    let first_kept = conv
        .entries()
        .iter()
        .find_map(|e| match &e.kind {
            EntryKind::Compaction(c) => Some(c.first_kept_entry_id.clone()),
            _ => None,
        })
        .expect("compaction entry");
    assert_eq!(out.lines.len(), 1, "{:?}", out.lines);
    assert!(
        out.lines[0].starts_with("Compacted (manual): ")
            && out.lines[0].ends_with(&format!("kept from {first_kept}.")),
        "{:?}",
        out.lines
    );
    assert_eq!(requests.lock().unwrap().len(), 2);
    let state = ext.snapshot().extensions[0]
        .script
        .as_ref()
        .unwrap()
        .state();
    assert_eq!(state["pending"], json!(false));
    assert_eq!(state["reason"], json!("manual"));
    assert_eq!(state["instructions"], json!("keep todos"));
    ext.uninstall_hooks();
}
