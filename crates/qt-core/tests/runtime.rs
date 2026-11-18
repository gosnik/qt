//! Engine composition and runtime wiring: provider backends, registered
//! and dynamic tools, tool selection, settings-driven tool options, the
//! system prompt from resources, user bash, `@file` prompts, startup
//! migrations, project settings and Pi JSON events.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use futures::future::BoxFuture;
use qt_core::events_json::PiEventSerializer;
use qt_core::message::{AgentMessage, AssistantMessage, ContentBlock, Message, StopReason};
use qt_core::provider::{
    ModelInfo, ProviderBackend, ProviderConfig, Request, StreamEvent, ThinkingLevel,
};
use qt_core::tools::{Tool, ToolContext, ToolOutput, ToolSelection};
use qt_core::{AgentEvent, AgentHome, Conversation, Engine, ModelRef, Settings};
use serde_json::{Value, json};
use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
use tokio::net::TcpListener;
use tokio_util::sync::CancellationToken;

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

struct Mock {
    _runtime: tokio::runtime::Runtime,
    requests: Arc<Mutex<Vec<Value>>>,
    port: u16,
}

impl Mock {
    fn start(replies: Vec<String>) -> Self {
        let runtime = tokio::runtime::Runtime::new().unwrap();
        let requests = Arc::new(Mutex::new(Vec::new()));
        let recorded = requests.clone();
        let port = runtime.block_on(async move {
            let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
            let port = listener.local_addr().unwrap().port();
            tokio::spawn(async move {
                for body in replies {
                    let (mut socket, _) = listener.accept().await.unwrap();
                    let mut buf = Vec::new();
                    let mut chunk = [0u8; 8192];
                    loop {
                        let n = socket.read(&mut chunk).await.unwrap_or(0);
                        buf.extend_from_slice(&chunk[..n]);
                        let t = String::from_utf8_lossy(&buf).to_string();
                        if let Some(split) = t.find("\r\n\r\n") {
                            let len = t[..split]
                                .lines()
                                .find_map(|l| {
                                    l.to_ascii_lowercase()
                                        .strip_prefix("content-length:")
                                        .map(|v| v.trim().parse::<usize>().unwrap())
                                })
                                .unwrap_or(0);
                            if buf.len() >= split + 4 + len {
                                recorded.lock().unwrap().push(
                                    serde_json::from_slice(&buf[split + 4..split + 4 + len])
                                        .unwrap(),
                                );
                                break;
                            }
                        }
                        if n == 0 {
                            break;
                        }
                    }
                    let response = format!(
                        "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{body}"
                    );
                    socket.write_all(response.as_bytes()).await.unwrap();
                    socket.shutdown().await.unwrap();
                }
            });
            port
        });
        Self {
            _runtime: runtime,
            requests,
            port,
        }
    }

    fn request(&self, i: usize) -> Value {
        self.requests.lock().unwrap()[i].clone()
    }
}

fn drain(engine: &Engine, rx: async_channel::Receiver<AgentEvent>) -> Vec<AgentEvent> {
    engine.block_on(async move {
        let mut events = Vec::new();
        while let Ok(e) = rx.recv().await {
            events.push(e);
        }
        events
    })
}

fn tool_texts(conv: &Conversation) -> Vec<String> {
    conv.messages()
        .iter()
        .filter_map(|m| match m {
            Message::ToolResult(r) => Some(r.text()),
            _ => None,
        })
        .collect()
}

fn write(path: &Path, text: &str) {
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, text).unwrap();
}

struct Dirs {
    _dir: tempfile::TempDir,
    root: PathBuf,
    work: PathBuf,
    home: AgentHome,
}

fn dirs(settings: Value) -> Dirs {
    let dir = tempfile::tempdir().unwrap();
    let root = dir.path().canonicalize().unwrap();
    let work = root.join("work");
    std::fs::create_dir_all(work.join(".git")).unwrap();
    let home = AgentHome::new(root.join("home"));
    write(&home.settings_path(), &settings.to_string());
    Dirs {
        _dir: dir,
        root,
        work,
        home,
    }
}

fn ollama_settings(port: u16) -> Value {
    json!({
        "ollama": {"instances": [{"id": "local", "baseUrl": format!("http://127.0.0.1:{port}"), "enabled": true}]},
        "retry": {"enabled": false, "provider": {"maxRetries": 0}},
    })
}

/// A provider served in-process: answers with the last user text.
struct EchoBackend {
    requests: Mutex<Vec<String>>,
}

impl ProviderBackend for EchoBackend {
    fn name(&self) -> &str {
        "echo"
    }

    fn providers(&self, _settings: &Settings) -> Vec<ProviderConfig> {
        vec![ProviderConfig {
            id: "echo".into(),
            label: "Echo".into(),
            local: true,
            ..Default::default()
        }]
    }

    fn list_models<'a>(
        &'a self,
        provider: &'a ProviderConfig,
        _settings: &'a Settings,
    ) -> BoxFuture<'a, anyhow::Result<Vec<ModelInfo>>> {
        Box::pin(async move {
            Ok(vec![ModelInfo {
                provider: provider.id.clone(),
                id: "echo-1".into(),
                name: "Echo 1".into(),
                context_window: Some(4096),
                ..Default::default()
            }])
        })
    }

    fn stream<'a>(
        &'a self,
        provider: &'a ProviderConfig,
        request: &'a Request,
        _cancel: &'a CancellationToken,
        on_event: &'a mut (dyn FnMut(StreamEvent) + Send),
    ) -> BoxFuture<'a, AssistantMessage> {
        Box::pin(async move {
            let last = request
                .messages
                .iter()
                .rev()
                .find_map(|m| match m {
                    Message::User { content, .. } => Some(content.plain_text()),
                    _ => None,
                })
                .unwrap_or_default();
            self.requests.lock().unwrap().push(request.system.clone());
            let text = format!("echo: {last}");
            on_event(StreamEvent::BlockStart {
                index: 0,
                block: ContentBlock::text(""),
            });
            on_event(StreamEvent::TextDelta {
                index: 0,
                delta: text.clone(),
            });
            AssistantMessage {
                content: vec![ContentBlock::text(text)],
                api: "echo".into(),
                provider: provider.id.clone(),
                model: request.model.clone(),
                ..Default::default()
            }
        })
    }
}

struct NamedTool(&'static str);

impl Tool for NamedTool {
    fn name(&self) -> &'static str {
        self.0
    }
    fn description(&self) -> String {
        format!("{} tool", self.0)
    }
    fn snippet(&self) -> &'static str {
        "test tool"
    }
    fn parameters(&self) -> Value {
        json!({"type": "object"})
    }
    fn execute(&self, _args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let id = ctx.call_id.clone().unwrap_or_default();
        let name = self.0;
        Box::pin(async move { ToolOutput::text(format!("{name} ran as {id}")) })
    }
}

#[test]
fn provider_backends_serve_runs_listing_and_turn_events() {
    let d = dirs(json!({}));
    let backend = Arc::new(EchoBackend {
        requests: Mutex::new(Vec::new()),
    });
    let engine = Engine::builder(d.home.clone(), &d.work)
        .provider_backend(backend.clone())
        .build()
        .unwrap();
    let provider = engine.provider("echo").unwrap();
    assert!(provider.backend.is_some());
    let listing = engine.block_on(engine.list_models()).unwrap();
    let echo = listing.iter().find(|(id, _)| id == "echo").unwrap();
    assert_eq!(echo.1.as_ref().unwrap()[0].id, "echo-1");
    let model = ModelRef::new("echo", "echo-1");
    assert_eq!(engine.context_window(&model), Some(4096));
    let conv = engine.new_conversation(Some(model));
    let events = drain(&engine, conv.prompt("ping", vec![]).unwrap());
    let last = conv.messages().last().cloned().unwrap();
    match last {
        Message::Assistant(a) => assert_eq!(a.text(), "echo: ping"),
        other => panic!("{other:?}"),
    }
    let starts = events
        .iter()
        .filter(|e| matches!(e, AgentEvent::TurnStart))
        .count();
    let ends = events
        .iter()
        .filter(|e| matches!(e, AgentEvent::TurnEnd { .. }))
        .count();
    assert_eq!((starts, ends), (1, 1));
    assert!(backend.requests.lock().unwrap()[0].contains("<environment_context>"));
    // Pi JSON lines for the run.
    let mut serializer = PiEventSerializer::new();
    let types: Vec<String> = events
        .iter()
        .flat_map(|e| serializer.serialize(e))
        .map(|v| v["type"].as_str().unwrap().to_string())
        .collect();
    assert_eq!(types.first().map(String::as_str), Some("agent_start"));
    assert_eq!(types.last().map(String::as_str), Some("agent_end"));
    assert!(types.iter().any(|t| t == "turn_end"));
}

#[test]
fn registered_dynamic_and_selected_tools() {
    let d = dirs(json!({}));
    let dynamic: Arc<Mutex<Vec<Arc<dyn Tool>>>> = Arc::new(Mutex::new(vec![]));
    let source = dynamic.clone();
    let engine = Engine::builder(d.home.clone(), &d.work)
        .tool(Arc::new(NamedTool("probe")))
        .tool_source(Arc::new(move || source.lock().unwrap().clone()))
        .tool_selection(ToolSelection {
            allow: vec![
                "read".into(),
                "probe".into(),
                "mcp_*".into(),
                "web_*".into(),
            ],
            ..Default::default()
        })
        .offline(true)
        .build()
        .unwrap();
    assert_eq!(engine.tools().names(), ["read", "probe"]);
    dynamic
        .lock()
        .unwrap()
        .push(Arc::new(NamedTool("mcp_fs_list")));
    assert_eq!(engine.tools().names(), ["read", "probe", "mcp_fs_list"]);
    assert_eq!(
        engine.refresh_models().unwrap_err().to_string(),
        "Model refresh is unavailable in offline mode"
    );
    let none = Engine::builder(d.home.clone(), &d.work)
        .tool(Arc::new(NamedTool("probe")))
        .tool_selection(ToolSelection {
            no_builtin_tools: true,
            ..Default::default()
        })
        .build()
        .unwrap();
    assert_eq!(none.tools().names(), ["probe"]);
    let full = Engine::new(d.home.clone(), &d.work).unwrap();
    assert!(full.tools().names().contains(&"web_fetch"));
    assert!(full.tools().names().contains(&"apply_patch"));
}

#[test]
fn settings_drive_policy_bash_and_system_prompt() {
    let mock = Mock::start(vec![
        tool_reply("c1", "read", json!({"path": "../outside.txt"})),
        tool_reply("c2", "bash", json!({"command": "echo $PREFIXED-$FROM_ENV"})),
        tool_reply("c3", "probe", json!({})),
        text_reply("done"),
    ]);
    let mut settings = ollama_settings(mock.port);
    settings["shellCommandPrefix"] = json!("PREFIXED=yes");
    settings["shellPath"] = json!("/bin/sh");
    let d = dirs(settings);
    write(&d.root.join("outside.txt"), "secret");
    write(&d.work.join(".pi/SYSTEM.md"), "You are the project agent.");
    write(&d.work.join("AGENTS.md"), "Project rules.");
    write(
        &d.home.root().join("skills/pdf/SKILL.md"),
        "---\nname: pdf\ndescription: PDF tools\n---\nUse pdftotext.",
    );
    let engine = Engine::builder(d.home.clone(), &d.work)
        .tool(Arc::new(NamedTool("probe")))
        .bash_env(vec![("FROM_ENV".into(), Some("env".into()))])
        .resource_options(qt_core::resources::ResourceOptions {
            append_system_prompt: vec!["Appended guidance.".into()],
            ..Default::default()
        })
        .build()
        .unwrap();
    let conv = engine.new_conversation(Some(ModelRef::new("ollama:local", "mock")));
    let events = drain(&engine, conv.prompt("go", vec![]).unwrap());
    assert_eq!(
        events
            .iter()
            .filter(|e| matches!(e, AgentEvent::TurnStart))
            .count(),
        4
    );
    let results = tool_texts(&conv);
    assert!(
        results[0].starts_with("path escapes workspace_root: "),
        "{results:?}"
    );
    assert_eq!(results[1], "yes-env");
    assert_eq!(results[2], "probe ran as c3");
    let system = mock.request(0)["messages"][0]["content"]
        .as_str()
        .unwrap()
        .to_string();
    assert!(
        system.starts_with("You are the project agent.\n\nAppended guidance."),
        "{system}"
    );
    assert!(system.contains("Project rules."));
    assert!(system.contains("<name>pdf</name>"));
    assert!(system.contains("  <shell>/bin/sh</shell>"));
    assert!(system.contains(&format!(
        "  <workspace_root>{}</workspace_root>",
        d.work.display()
    )));
    assert!(system.contains("filesystem path arguments are confined to the workspace"));
    let context = engine.context_files();
    assert_eq!(context.len(), 1);
    let quiet = Engine::builder(d.home.clone(), &d.work)
        .no_context_files(true)
        .build()
        .unwrap();
    assert!(quiet.context_files().is_empty());
    assert!(
        !quiet
            .system_prompt(&quiet.tools())
            .contains("Project rules.")
    );
    assert!(engine.last_provider_request().is_some());
}

#[test]
fn user_bash_is_recorded_excluded_and_cancellable() {
    let d = dirs(json!({}));
    let engine = Engine::new(d.home.clone(), &d.work).unwrap();
    let conv = engine.new_conversation(None);
    assert!(conv.run_bash("  ", false).is_err());
    let run = conv.run_bash("echo hi; exit 3", false).unwrap();
    let message = engine.block_on(run.result).unwrap();
    assert_eq!(message.output, "hi");
    assert_eq!(message.exit_code, Some(3));
    let hidden = conv.run_bash("echo secret", true).unwrap();
    let hidden = engine.block_on(hidden.result).unwrap();
    assert!(hidden.exclude_from_context);
    let bash: Vec<&AgentMessage> = Vec::new();
    let recorded = conv.agent_messages();
    let recorded: Vec<_> = recorded
        .iter()
        .filter(|m| matches!(m, AgentMessage::BashExecution(_)))
        .chain(bash)
        .collect();
    assert_eq!(recorded.len(), 2);
    let llm = qt_core::message::convert_to_llm(&conv.agent_messages());
    assert_eq!(llm.len(), 1);
    match &llm[0] {
        Message::User { content, .. } => {
            assert_eq!(
                content.plain_text(),
                "Ran `echo hi; exit 3`\n```\nhi\n```\n\nCommand exited with code 3"
            )
        }
        other => panic!("{other:?}"),
    }
    let slow = conv.run_bash("sleep 20", false).unwrap();
    assert_eq!(
        conv.run_bash("echo again", false)
            .err()
            .unwrap()
            .to_string(),
        "A bash command is already running. Press Esc to cancel it first."
    );
    assert!(conv.is_bash_running());
    conv.abort_bash();
    let cancelled = engine.block_on(slow.result).unwrap();
    assert!(cancelled.cancelled);
    assert!(!conv.is_bash_running());
}

#[test]
fn prompts_with_files_and_history() {
    let mock = Mock::start(vec![text_reply("read it")]);
    let d = dirs(ollama_settings(mock.port));
    write(&d.work.join("notes.txt"), "remember this");
    let engine = Engine::new(d.home.clone(), &d.work).unwrap();
    let conv = engine.new_conversation(Some(ModelRef::new("ollama:local", "mock")));
    assert!(
        conv.prompt_with_files("hi", &["missing.txt".into()], None)
            .err()
            .unwrap()
            .to_string()
            .starts_with("File not found: ")
    );
    let (rx, diagnostics) = conv
        .prompt_with_files("Summarise", &["notes.txt".into()], Some("stdin "))
        .unwrap();
    drain(&engine, rx);
    assert!(diagnostics.is_empty());
    let sent = mock.request(0)["messages"][1]["content"].clone();
    let sent = sent
        .as_str()
        .map(str::to_string)
        .unwrap_or_else(|| sent.to_string());
    assert!(
        sent.starts_with(&format!(
            "stdin <file name=\"{}\">\nremember this\n</file>\nSummarise",
            d.work.join("notes.txt").display()
        )),
        "{sent}"
    );
    assert_eq!(conv.prompt_history().len(), 1);
    let store = engine.prompt_history();
    store.append("first").unwrap();
    store.append("second").unwrap();
    assert_eq!(store.load().unwrap().prompts, ["second", "first"]);
}

#[test]
fn migrations_and_project_settings_layers() {
    let d =
        dirs(json!({"defaultProvider": "ollama", "defaultModel": "llama3", "workspaceRoot": "."}));
    write(
        &d.work.join(".pi/settings.json"),
        r#"{"allowExternalFilesystemAccess": true}"#,
    );
    let engine = Engine::builder(d.home.clone(), &d.work)
        .startup_migrations(true)
        .build()
        .unwrap();
    assert_eq!(
        engine.migration_result().migrated_default_provider,
        Some(("ollama".into(), "ollama:local".into()))
    );
    assert_eq!(
        engine.default_model(),
        Some(ModelRef::new("ollama:local", "llama3"))
    );
    let diagnostics = engine.settings_diagnostics();
    assert_eq!(diagnostics.len(), 1, "{diagnostics:?}");
    assert!(diagnostics[0].ends_with(
        "project settings may not broaden global policy: allowExternalFilesystemAccess"
    ));
    assert!(!engine.workspace_policy().allows_external_access());

    write(
        &d.work.join(".pi/settings.json"),
        r#"{"defaultModel": "qwen3", "images": {"autoResize": false}}"#,
    );
    engine.reload_settings();
    assert!(engine.settings_diagnostics().is_empty());
    assert_eq!(engine.settings().default_model().as_deref(), Some("qwen3"));
    assert_eq!(
        engine.global_settings().default_model().as_deref(),
        Some("llama3")
    );
    assert!(!engine.tool_environment(None).images.auto_resize);
    engine
        .update_settings(|s| s.set_enabled_models(&["ollama:local/*".into()]))
        .unwrap();
    let saved = Settings::load(&d.home).unwrap();
    assert!(saved.default_model().as_deref() == Some("llama3"));
    let scope = engine.scoped_models(&[ModelInfo {
        provider: "ollama:local".into(),
        id: "qwen3".into(),
        name: "qwen3".into(),
        ..Default::default()
    }]);
    assert_eq!(scope.models.len(), 1);
    let conv = engine.new_conversation(None);
    conv.set_thinking(ThinkingLevel::Minimal);
    assert_eq!(conv.thinking(), ThinkingLevel::Minimal);
    let _ = StopReason::Stop;
}
