//! Conversation driver behaviour against a scripted local OpenAI-compatible
//! server: auto-retry, overflow recovery, compaction, queues, tree
//! navigation, fork/clone, goals and the session catalog.

use std::path::Path;
use std::sync::{Arc, Mutex};

use qt_core::harness::hooks::{SessionEvent, ToolCallEvent};
use qt_core::harness::{CompactionReason, GoalStatus};
use qt_core::message::{AgentMessage, Message, StopReason};
use qt_core::session::EntryKind;
use qt_core::{AgentEvent, AgentHome, Conversation, Engine, ModelRef, NavigateOptions};
use serde_json::{Value, json};
use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
use tokio::net::TcpListener;

enum Reply {
    Sse(String),
    Status(u16, String),
}

fn sse(chunks: &[Value]) -> String {
    let mut out: String = chunks.iter().map(|c| format!("data: {c}\n\n")).collect();
    out.push_str("data: [DONE]\n\n");
    out
}

fn text(text: &str) -> Reply {
    Reply::Sse(sse(&[
        json!({"choices":[{"delta":{"content":text}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}}),
    ]))
}

fn tool_call(id: &str, name: &str, args: Value) -> Reply {
    Reply::Sse(sse(&[
        json!({"choices":[{"delta":{"tool_calls":[{"index":0,"id":id,"function":{"name":name,"arguments":args.to_string()}}]}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}}),
    ]))
}

struct Mock {
    _runtime: tokio::runtime::Runtime,
    requests: Arc<Mutex<Vec<Value>>>,
    port: u16,
}

impl Mock {
    fn start(replies: Vec<Reply>) -> Self {
        let runtime = tokio::runtime::Runtime::new().unwrap();
        let requests = Arc::new(Mutex::new(Vec::new()));
        let recorded = requests.clone();
        let port = runtime.block_on(async move {
            let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
            let port = listener.local_addr().unwrap().port();
            tokio::spawn(async move {
                for reply in replies {
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
                    let response = match reply {
                        Reply::Sse(body) => format!(
                            "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{body}"
                        ),
                        Reply::Status(code, body) => format!(
                            "HTTP/1.1 {code} Error\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
                            body.len()
                        ),
                    };
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

    fn count(&self) -> usize {
        self.requests.lock().unwrap().len()
    }
}

struct Setup {
    dir: tempfile::TempDir,
    engine: Engine,
    conv: Conversation,
}

fn setup(mock: &Mock, extra: Value) -> Setup {
    let dir = tempfile::tempdir().unwrap();
    let workspace = dir.path().join("work");
    std::fs::create_dir_all(&workspace).unwrap();
    let home = AgentHome::new(dir.path().join("home"));
    std::fs::create_dir_all(home.root()).unwrap();
    let mut settings = json!({
        "ollama": {"instances": [{"id": "local", "baseUrl": format!("http://127.0.0.1:{}", mock.port), "enabled": true}]},
        "retry": {"baseDelayMs": 1},
    });
    if let (Some(s), Some(e)) = (settings.as_object_mut(), extra.as_object()) {
        for (k, v) in e {
            s.insert(k.clone(), v.clone());
        }
    }
    std::fs::write(home.settings_path(), settings.to_string()).unwrap();
    let engine = Engine::new(home, &workspace).unwrap();
    let conv = engine.new_conversation(Some(ModelRef::new("ollama", "mock")));
    Setup { dir, engine, conv }
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

fn texts(conv: &Conversation) -> Vec<String> {
    conv.messages()
        .iter()
        .map(|m| match m {
            Message::User { content, .. } => content.plain_text(),
            Message::Assistant(a) => a.text(),
            Message::ToolResult(r) => r.text(),
        })
        .collect()
}

fn sent_texts(request: &Value) -> Vec<String> {
    request["messages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|m| match &m["content"] {
            Value::String(s) => s.clone(),
            Value::Array(parts) => parts
                .iter()
                .filter_map(|p| p["text"].as_str())
                .collect::<Vec<_>>()
                .join(""),
            _ => String::new(),
        })
        .collect()
}

#[test]
fn transient_errors_are_retried_with_backoff() {
    let mock = Mock::start(vec![
        Reply::Status(
            503,
            r#"{"error":{"message":"Service unavailable, overloaded"}}"#.into(),
        ),
        text("recovered"),
    ]);
    let s = setup(&mock, json!({}));
    let events = drain(&s.engine, s.conv.prompt("hi", vec![]).unwrap());
    assert!(events.iter().any(|e| matches!(
        e,
        AgentEvent::RetryStart {
            attempt: 1,
            max_attempts: 3,
            delay_ms: 1,
            ..
        }
    )));
    assert!(events.iter().any(|e| matches!(
        e,
        AgentEvent::RetryEnd {
            success: true,
            attempt: 1,
            ..
        }
    )));
    assert!(matches!(
        events.last(),
        Some(AgentEvent::AgentEnd { aborted: false, .. })
    ));
    assert_eq!(texts(&s.conv), ["hi", "recovered"]);
    // The retried request no longer carries the failed response.
    assert_eq!(sent_texts(&mock.request(1)).len(), 2);
    // The error stays in the session file for history.
    let raw = std::fs::read_to_string(s.conv.session_path()).unwrap();
    assert!(raw.contains(r#""stopReason":"error""#));
}

#[test]
fn retries_give_up_after_max_attempts() {
    let busy = || Reply::Status(429, r#"{"error":{"message":"rate limit"}}"#.into());
    let mock = Mock::start(vec![busy(), busy()]);
    let s = setup(&mock, json!({"retry": {"baseDelayMs": 1, "maxRetries": 1}}));
    let events = drain(&s.engine, s.conv.prompt("hi", vec![]).unwrap());
    assert!(events.iter().any(|e| matches!(
        e,
        AgentEvent::RetryEnd {
            success: false,
            attempt: 1,
            final_error: Some(_)
        }
    )));
    assert_eq!(mock.count(), 2);
    let last = s.conv.messages().last().cloned().unwrap();
    assert!(matches!(last, Message::Assistant(a) if a.stop_reason == StopReason::Error));
}

#[test]
fn context_overflow_compacts_and_retries_once() {
    let overflow = r#"{"error":{"message":"prompt is too long: 300000 tokens > 200000 maximum"}}"#;
    let mock = Mock::start(vec![
        text("a1"),
        Reply::Status(400, overflow.into()),
        text("SUMMARY"),
        text("answer"),
    ]);
    let s = setup(&mock, json!({"compaction": {"keepRecentTokens": 1}}));
    drain(&s.engine, s.conv.prompt("first", vec![]).unwrap());
    let events = drain(&s.engine, s.conv.prompt("second", vec![]).unwrap());
    let end = events
        .iter()
        .find_map(|e| match e {
            AgentEvent::CompactionEnd {
                reason: CompactionReason::Overflow,
                result,
                will_retry,
                error,
                ..
            } => Some((result.clone(), *will_retry, error.clone())),
            _ => None,
        })
        .expect("compaction end");
    assert!(end.1, "{end:?}");
    assert!(end.0.unwrap().summary.starts_with("SUMMARY"));
    // The summarization request.
    let summary_req = sent_texts(&mock.request(2));
    assert!(summary_req[0].starts_with("You are a context summarization assistant."));
    assert!(summary_req[1].starts_with("<conversation>\n[User]: first\n\n[Assistant]: a1\n</conversation>\n\nThe messages above are a conversation to summarize."));
    // The retried request: summary + kept user message.
    let retry = sent_texts(&mock.request(3));
    assert_eq!(retry.len(), 3);
    assert!(retry[1].contains("<summary>\nSUMMARY\n</summary>"));
    assert_eq!(retry[2], "second");
    assert_eq!(texts(&s.conv).last().unwrap(), "answer");
    let roles: Vec<_> = s.conv.agent_messages().iter().map(|m| m.role()).collect();
    assert_eq!(roles, ["compactionSummary", "user", "assistant"]);
}

#[test]
fn manual_and_threshold_compaction_update_iteratively() {
    let mock = Mock::start(vec![
        text("a1"),
        text("a2"),
        text("S1"),
        text("a3"),
        text("S2"),
    ]);
    // "second" (2 tokens) + "a2" (1 token) fill the kept budget, so the cut
    // lands on a user message and only one summary request is made.
    let s = setup(&mock, json!({"compaction": {"keepRecentTokens": 3}}));
    drain(&s.engine, s.conv.prompt("first", vec![]).unwrap());
    drain(&s.engine, s.conv.prompt("second", vec![]).unwrap());
    let events = drain(&s.engine, s.conv.compact(Some("focus on files")).unwrap());
    assert!(matches!(
        events[0],
        AgentEvent::CompactionStart {
            reason: CompactionReason::Manual
        }
    ));
    assert!(matches!(
        &events[1],
        AgentEvent::CompactionEnd {
            result: Some(_),
            ..
        }
    ));
    let summary_req = sent_texts(&mock.request(2));
    assert!(
        summary_req[1]
            .starts_with("<conversation>\n[User]: first\n\n[Assistant]: a1\n</conversation>")
    );
    assert!(summary_req[1].ends_with("Additional focus: focus on files"));
    assert_eq!(texts(&s.conv), ["second", "a2"]);
    let again = drain(&s.engine, s.conv.compact(None).unwrap());
    assert!(matches!(
        &again[1],
        AgentEvent::CompactionEnd { error: Some(e), result: None, .. } if e == "Compaction failed: Already compacted"
    ));

    // A tiny window makes every response cross the threshold.
    s.conv.set_context_window(Some(100));
    let events = drain(&s.engine, s.conv.prompt("third", vec![]).unwrap());
    assert!(events.iter().any(|e| matches!(
        e,
        AgentEvent::CompactionEnd {
            reason: CompactionReason::Threshold,
            result: Some(_),
            will_retry: false,
            ..
        }
    )));
    let update = sent_texts(&mock.request(4));
    assert!(
        update[1].starts_with("<conversation>\n[User]: second\n\n[Assistant]: a2\n</conversation>")
    );
    assert!(update[1].contains("<previous-summary>\nS1\n</previous-summary>"));
    assert!(update[1].contains("The messages above are NEW conversation messages"));
    let compactions = s
        .conv
        .entries()
        .iter()
        .filter(|e| matches!(e.kind, EntryKind::Compaction(_)))
        .count();
    assert_eq!(compactions, 2);
    let stats = s.conv.stats();
    assert_eq!(stats.context_usage.unwrap().tokens, None);
}

#[test]
fn steering_and_follow_up_queues_deliver_in_order() {
    let mock = Mock::start(vec![
        tool_call("c1", "ls", json!({"path": "."})),
        text("done"),
        text("after"),
    ]);
    let s = setup(&mock, json!({}));
    let conv = s.conv.clone();
    let steered = Arc::new(Mutex::new(false));
    let flag = steered.clone();
    s.engine
        .hooks()
        .add_tool_call(Arc::new(move |e: &ToolCallEvent| {
            let mut f = flag.lock().unwrap();
            if e.tool_name == "ls" && !*f {
                *f = true;
                conv.steer("also check README", vec![]).unwrap();
            }
            None
        }));
    s.conv.follow_up("then summarize", vec![]).unwrap();
    assert_eq!(s.conv.queue().follow_up, ["then summarize"]);
    let events = drain(&s.engine, s.conv.prompt("go", vec![]).unwrap());
    assert_eq!(
        texts(&s.conv)[..],
        [
            "go",
            "",
            texts(&s.conv)[2].as_str(),
            "also check README",
            "done",
            "then summarize",
            "after"
        ][..]
    );
    assert!(events.iter().any(|e| matches!(
        e,
        AgentEvent::QueueUpdate { steering, .. } if steering == &["also check README".to_string()]
    )));
    assert!(s.conv.queue().is_empty());
    let second = sent_texts(&mock.request(1));
    assert_eq!(second.last().unwrap(), "also check README");
}

#[test]
fn tree_navigation_fork_clone_and_labels() {
    let mock = Mock::start(vec![text("r1"), text("r2"), text("BRANCH"), text("r2b")]);
    let s = setup(&mock, json!({}));
    let events = Arc::new(Mutex::new(Vec::new()));
    let sink = events.clone();
    s.engine
        .hooks()
        .add_session_listener(Arc::new(move |e: &SessionEvent| {
            sink.lock().unwrap().push(e.clone())
        }));
    drain(&s.engine, s.conv.prompt("one", vec![]).unwrap());
    drain(&s.engine, s.conv.prompt("two", vec![]).unwrap());
    let users = s.conv.user_messages_for_forking();
    assert_eq!(
        users.iter().map(|u| u.1.as_str()).collect::<Vec<_>>(),
        ["one", "two"]
    );
    let two_id = users[1].0.clone();

    let nav = s
        .engine
        .block_on(s.conv.navigate_tree(
            &two_id,
            NavigateOptions {
                summarize: true,
                label: Some("explored".into()),
                ..Default::default()
            },
        ))
        .unwrap()
        .unwrap();
    assert_eq!(nav.editor_text.as_deref(), Some("two"));
    let summary_id = nav.summary_entry_id.unwrap();
    assert_eq!(s.conv.label(&summary_id).as_deref(), Some("explored"));
    let roles: Vec<_> = s.conv.agent_messages().iter().map(|m| m.role()).collect();
    assert_eq!(roles, ["user", "assistant", "branchSummary"]);
    let summary_prompt = sent_texts(&mock.request(2));
    assert!(summary_prompt[1].starts_with("<conversation>\n[Assistant]: r2\n</conversation>\n\nCreate a structured summary of this conversation branch"));
    match &s.conv.agent_messages()[2] {
        AgentMessage::BranchSummary(b) => assert!(b.summary.starts_with(
            "The user explored a different conversation branch before returning here.\nSummary of that exploration:\n\nBRANCH"
        )),
        other => panic!("{other:?}"),
    }
    drain(&s.engine, s.conv.prompt("two again", vec![]).unwrap());
    let sent = sent_texts(&mock.request(3));
    assert!(sent[3].starts_with(
        "The following is a summary of a branch that this conversation came back from:"
    ));
    let tree = s.conv.tree();
    assert_eq!(tree.roots.len(), 1);
    let branch = s.conv.branch();
    let r1 = branch
        .iter()
        .find(|e| {
            e.message()
                .and_then(|m| m.as_assistant())
                .is_some_and(|a| a.text() == "r1")
        })
        .unwrap();
    // The label entry became the leaf the new prompt was attached to.
    assert!(
        branch
            .iter()
            .any(|e| matches!(e.kind, EntryKind::Label { .. }))
    );
    assert_eq!(tree.node(&r1.id).unwrap().children.len(), 2);

    // Navigate to a non-user entry without summary: leaf moves there.
    s.engine
        .block_on(s.conv.navigate_tree(&r1.id, NavigateOptions::default()))
        .unwrap()
        .unwrap();
    assert_eq!(texts(&s.conv), ["one", "r1"]);

    // Fork before the first user message: an empty session pointing back.
    let one_id = users[0].0.clone();
    let fork = s.conv.fork(&one_id, false).unwrap();
    assert_eq!(fork.selected_text.as_deref(), Some("one"));
    assert!(fork.conversation.messages().is_empty());
    assert!(fork.conversation.session_id().starts_with("fork-native-"));
    assert_eq!(
        fork.conversation.parent_session().as_deref(),
        Some(s.conv.session_path().to_str().unwrap())
    );
    assert!(s.conv.fork(&r1.id, false).is_err());
    let at = s.conv.fork(&r1.id, true).unwrap();
    assert_eq!(texts(&at.conversation), ["one", "r1"]);
    assert!(at.conversation.session_path().exists());

    let clone = s.conv.clone_session().unwrap();
    assert!(clone.session_path().exists());
    assert_eq!(clone.entries().len(), s.conv.entries().len());
    assert_eq!(texts(&clone), texts(&s.conv));

    let reasons: Vec<_> = events
        .lock()
        .unwrap()
        .iter()
        .filter_map(|e| match e {
            SessionEvent::Start { reason, .. } => Some(format!("{reason:?}")),
            SessionEvent::Tree { .. } => Some("Tree".into()),
            _ => None,
        })
        .collect();
    assert_eq!(reasons, ["Tree", "Tree", "Fork", "Fork", "Clone"]);
}

#[test]
fn goals_continue_until_the_model_completes_them() {
    let mock = Mock::start(vec![
        tool_call("g1", "update_goal", json!({"status": "complete"})),
        text("All done."),
    ]);
    let s = setup(&mock, json!({}));
    let rx = s
        .conv
        .set_goal("write the docs", Some(10_000))
        .unwrap()
        .unwrap();
    let events = drain(&s.engine, rx);
    assert!(matches!(
        events.last(),
        Some(AgentEvent::AgentEnd { aborted: false, .. })
    ));
    let first = mock.request(0);
    let prompt = sent_texts(&first);
    assert!(prompt[1].starts_with("Continue working toward the active thread goal."));
    let tools: Vec<_> = first["tools"]
        .as_array()
        .unwrap()
        .iter()
        .map(|t| t["function"]["name"].as_str().unwrap().to_string())
        .collect();
    assert!(tools.contains(&"update_goal".to_string()));
    let goal = s.conv.goal().unwrap();
    assert_eq!(goal.status, GoalStatus::Complete);
    // Only responses while the goal was active count (10 in + 2 out).
    assert_eq!(goal.tokens_used, 12);
    assert_eq!(mock.count(), 2);
    let reopened = s.engine.open_conversation(&s.conv.session_path()).unwrap();
    assert_eq!(reopened.goal().unwrap().status, GoalStatus::Complete);
    let (out, rx) = reopened.goal_command("").unwrap();
    assert!(rx.is_none());
    assert_eq!(out.lines[1], "Status: complete");
}

#[test]
fn session_catalog_resolution_import_and_fork() {
    let mock = Mock::start(vec![text("r1")]);
    let s = setup(&mock, json!({}));
    let custom = s.dir.path().join("custom-sessions");
    s.engine.set_session_dir(Some(custom.clone()));
    assert_eq!(s.engine.session_dir(), custom);
    let conv = s
        .engine
        .new_conversation(Some(ModelRef::new("ollama", "mock")));
    drain(&s.engine, conv.prompt("hello", vec![]).unwrap());
    assert!(conv.session_path().starts_with(&custom));
    let id = conv.session_id();
    assert_eq!(
        s.engine.resolve_session(&id[..10]).unwrap(),
        conv.session_path()
    );
    assert_eq!(s.engine.most_recent_session().unwrap(), conv.session_path());
    assert_eq!(s.engine.continue_recent(None).unwrap().session_id(), id);
    let err = s.engine.resolve_session("zzz").unwrap_err().to_string();
    assert_eq!(err, "No session found matching 'zzz'");

    let forked = s.engine.fork_session(&conv.session_path()).unwrap();
    assert_eq!(forked.session_id(), format!("fork-{id}"));
    assert_eq!(texts(&forked), ["hello", "r1"]);
    assert!(forked.session_path().exists());

    // Import a session whose cwd is gone: error unless overridden.
    let external = s.dir.path().join("elsewhere.jsonl");
    std::fs::write(
        &external,
        concat!(
            r#"{"type":"session","version":3,"id":"ext-1","timestamp":"2025-01-01T00:00:00.000Z","cwd":"/definitely/missing/dir"}"#, "\n",
            r#"{"type":"message","id":"a","parentId":null,"timestamp":"2025-01-01T00:00:01.000Z","message":{"role":"user","content":"imported"}}"#, "\n",
        ),
    )
    .unwrap();
    let err = s
        .engine
        .import_session(&external, None)
        .unwrap_err()
        .to_string();
    assert!(
        err.starts_with("Stored session working directory does not exist: /definitely/missing/dir")
    );
    let imported = s
        .engine
        .import_session(&external, Some(Path::new(s.engine.cwd())))
        .unwrap();
    assert_eq!(texts(&imported), ["imported"]);
    assert!(custom.join("elsewhere.jsonl").exists());
    let ephemeral = s.engine.new_ephemeral_conversation(None);
    assert!(!ephemeral.is_persisted());
}
