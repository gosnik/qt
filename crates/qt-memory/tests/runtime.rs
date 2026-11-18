use std::sync::{Arc, Mutex};
use std::time::Duration;

use qt_core::harness::hooks::ModelContextEvent;
use qt_core::message::{AssistantMessage, ContentBlock, Message, ToolResultMessage};
use qt_core::provider::{ModelRef, Request};
use qt_memory::coordinator::CONTEXT_MESSAGE_PREAMBLE;
use qt_memory::integration::{augment_request, insert_before_current_user, logical_input};
use qt_memory::isolated::{IsolatedAgentRequest, IsolatedAgentResult, IsolatedAgentRunner};
use qt_memory::runtime::{CaptureHooks, episode_text};
use qt_memory::*;
use serde_json::{Value, json};

/// Records requests and answers with a fixed final output.
struct FakeRunner {
    output: Mutex<String>,
    requests: Mutex<Vec<IsolatedAgentRequest>>,
}

impl FakeRunner {
    fn new(output: &str) -> Arc<Self> {
        Arc::new(Self {
            output: Mutex::new(output.into()),
            requests: Mutex::new(Vec::new()),
        })
    }
}

impl IsolatedAgentRunner for FakeRunner {
    fn run(&self, request: &IsolatedAgentRequest) -> Result<IsolatedAgentResult> {
        self.requests.lock().unwrap().push(request.clone());
        Ok(IsolatedAgentResult {
            final_output: self.output.lock().unwrap().clone(),
            ..Default::default()
        })
    }
}

fn runtime(
    home: &std::path::Path,
    cwd: &std::path::Path,
    runner: Option<Arc<dyn IsolatedAgentRunner>>,
) -> MemoryRuntime {
    MemoryRuntime::new(MemoryRuntimeOptions {
        agent_home: home.into(),
        cwd: cwd.into(),
        session: "s1".into(),
        agent: "s1".into(),
        coordinator: MemoryCoordinatorSettings {
            enabled: true,
            ..Default::default()
        },
        runner,
        ..Default::default()
    })
    .unwrap()
}

fn add_active(runtime: &MemoryRuntime, content: &str, memory_type: MemoryType) -> String {
    runtime
        .store()
        .add(
            &runtime.scope(),
            MemoryWrite {
                content: content.into(),
                memory_type,
                status: MemoryStatus::Active,
                provenance: Provenance {
                    source_type: SourceType::File,
                    source_uri: "file:notes.md".into(),
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap()
}

fn model_event(messages: Vec<Message>) -> ModelContextEvent {
    ModelContextEvent {
        model: ModelRef::new("ollama", "qwen"),
        request: Request {
            messages,
            ..Default::default()
        },
        turn_index: 0,
        cancel: Default::default(),
    }
}

#[test]
fn augmentor_inserts_context_before_current_user_message() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let selected = FakeRunner::new("");
    let runtime = runtime(home.path(), cwd.path(), Some(selected.clone()));
    let id = add_active(
        &runtime,
        "Run tests with cargo nextest",
        MemoryType::Procedural,
    );
    let answer = json!({
        "version": 1, "logical_input_sequence": 2, "context_markdown": "", "memory_ids": [id],
        "block_ids": [], "warnings": [], "estimated_tokens": 0, "omission_reason": "",
        "candidate_pack_hash": "", "output_hash": "",
    });
    *selected.output.lock().unwrap() = answer.to_string();
    let event = model_event(vec![
        Message::user_text("hello"),
        Message::user_text("how do I run tests with cargo?"),
    ]);
    assert_eq!(logical_input(&event.request.messages).0, 2);
    let request = augment_request(&runtime, &event, 1).expect("memory injected");
    assert_eq!(request.messages.len(), 3);
    let Message::User { content, .. } = &request.messages[1] else {
        panic!("memory message is user-role");
    };
    let text = content.plain_text();
    assert!(text.starts_with(CONTEXT_MESSAGE_PREAMBLE));
    assert!(text.contains("<qt_memory_context>"));
    assert!(text.contains(&format!(
        "[Procedure {id}; status=active; confidence=0.5]\nRun tests with cargo nextest\n"
    )));
    let child = selected.requests.lock().unwrap()[0].clone();
    assert_eq!(&child.arguments[..2], ["--mode", "json"]);
    assert!(child.arguments.contains(&"--no-memory".to_string()));
    assert!(
        child
            .environment
            .contains(&("QT_MEMORY_PREHANDLER".to_string(), "1".to_string()))
    );
    let payload: Value = serde_json::from_str(&child.stdin_payload).unwrap();
    assert_eq!(payload["logical_input_sequence"], 2);
    assert_eq!(payload["candidates"][0]["id"], json!(id));
    let receipts = runtime
        .store()
        .session_events(&runtime.scope(), "recall_receipt", 10, 0)
        .unwrap();
    let receipt: Value = serde_json::from_str(&receipts[0].payload_json).unwrap();
    assert_eq!(receipt["isolated_agent"], true);
    assert_eq!(receipt["deterministic_fallback"], false);
    assert_eq!(receipt["outcome"], "injected");
    assert_eq!(
        runtime
            .store()
            .get(&runtime.scope(), &id)
            .unwrap()
            .access_count,
        1
    );

    // The same logical input is served from the cache (no second child).
    augment_request(&runtime, &event, 1).unwrap();
    assert_eq!(selected.requests.lock().unwrap().len(), 1);
}

#[test]
fn rejected_prehandler_output_falls_back_deterministically() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let runner = FakeRunner::new("{\"surprise\": true}");
    let runtime = runtime(home.path(), cwd.path(), Some(runner));
    add_active(
        &runtime,
        "The deploy target is staging",
        MemoryType::Semantic,
    );
    let augmentation = runtime
        .coordinator()
        .augment(
            1,
            1,
            &Default::default(),
            &[Message::user_text("what is the deploy target")],
            None,
        )
        .unwrap();
    assert!(augmentation.used_isolated_agent);
    assert!(augmentation.used_deterministic_fallback);
    assert!(augmentation.message.is_some());
    assert!(
        augmentation
            .diagnostics
            .iter()
            .any(|line| line.starts_with("memory pre-handler output was rejected: memory pre-handler result contains unknown field 'surprise'"))
    );
}

#[test]
fn insertion_helper_targets_newest_user_message() {
    let mut messages = vec![
        Message::user_text("a"),
        Message::Assistant(AssistantMessage::default()),
        Message::user_text("b"),
    ];
    insert_before_current_user(&mut messages, Message::user_text("memory"));
    assert!(
        matches!(&messages[2], Message::User { content, .. } if content.plain_text() == "memory")
    );
}

#[derive(Default)]
struct RecordingHooks {
    processed: Mutex<Vec<(usize, usize)>>,
}

impl CaptureHooks for RecordingHooks {
    fn post_process(&self, _scope: &MemoryScope) -> Result<()> {
        Ok(())
    }

    fn candidates_captured(
        &self,
        _scope: &MemoryScope,
        _turn: u64,
        extracted: usize,
        inserted: usize,
    ) {
        self.processed.lock().unwrap().push((extracted, inserted));
    }
}

fn episode() -> Vec<Message> {
    vec![
        Message::user_text("We decided to use SQLite for the cache layer."),
        Message::Assistant(AssistantMessage {
            content: vec![
                ContentBlock::text("Understood."),
                ContentBlock::ToolCall {
                    id: "1".into(),
                    name: "bash".into(),
                    arguments: json!({}),
                },
            ],
            ..Default::default()
        }),
        Message::ToolResult(ToolResultMessage {
            tool_call_id: "1".into(),
            tool_name: "bash".into(),
            content: vec![ContentBlock::text("ok")],
            is_error: false,
            details: None,
            timestamp: None,
        }),
    ]
}

#[test]
fn capture_worker_ingests_attributes_and_cleans_spool() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let hooks = Arc::new(RecordingHooks::default());
    let runtime = MemoryRuntime::new(MemoryRuntimeOptions {
        agent_home: home.path().into(),
        cwd: cwd.path().into(),
        session: "s1".into(),
        capture_hooks: Some(hooks.clone()),
        ..Default::default()
    })
    .unwrap();
    let procedure = add_active(
        &runtime,
        "Procedure: steps to deploy",
        MemoryType::Procedural,
    );
    runtime.note_recall(4, std::slice::from_ref(&procedure));
    runtime.enqueue_completed_episode(
        &episode(),
        "qt-session://s1/rpc-episode/1",
        CompletedEpisodeMetadata {
            ok: true,
            turn_index: 4,
            tool_request_count: 1,
            ..Default::default()
        },
    );
    assert!(runtime.wait_for_capture_idle(Duration::from_secs(10)));
    let scope = runtime.scope();
    let durable = durable_memory_scope(scope.clone());
    let captured = runtime
        .store()
        .query(
            &durable,
            &MemoryQuery {
                statuses: vec![MemoryStatus::Candidate],
                policy: RecallPolicy::Strict,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(captured.len(), 1);
    assert_eq!(
        captured[0].content,
        "We decided to use SQLite for the cache layer."
    );
    assert_eq!(captured[0].memory_type, MemoryType::Decision);
    assert!(captured[0].tags.contains(&"processed:decision".to_string()));
    assert_eq!(*hooks.processed.lock().unwrap(), vec![(1, 1)]);
    let store = runtime.store();
    let receipt: Value = serde_json::from_str(
        &store
            .session_events(&scope, "completed_episode_receipt", 10, 0)
            .unwrap()[0]
            .payload_json,
    )
    .unwrap();
    assert_eq!(receipt["stored_candidate_count"], 1);
    let attribution: Value = serde_json::from_str(
        &store
            .session_events(&scope, "recall_attribution", 10, 0)
            .unwrap()[0]
            .payload_json,
    )
    .unwrap();
    assert_eq!(attribution["attribution"], "positive");
    assert_eq!(store.get(&scope, &procedure).unwrap().success_count, 1);
    let spool = home.path().join("memory").join("capture_pending");
    assert_eq!(std::fs::read_dir(&spool).unwrap().count(), 0);
    let episode_event: Value = serde_json::from_str(
        &store
            .session_events(&scope, "completed_episode", 10, 0)
            .unwrap()[0]
            .payload_json,
    )
    .unwrap();
    assert_eq!(episode_event["outcome_kind"], "completed");
    assert!(
        episode_event["content"]
            .as_str()
            .unwrap()
            .starts_with("## user\n\nWe decided")
    );

    // The same episode is deduplicated.
    runtime.enqueue_completed_episode(
        &episode(),
        "qt-session://s1/rpc-episode/1",
        Default::default(),
    );
    assert!(runtime.wait_for_capture_idle(Duration::from_secs(10)));
    assert_eq!(
        store
            .session_events(&scope, "completed_episode_receipt", 10, 0)
            .unwrap()
            .len(),
        1
    );
    assert_eq!(
        store
            .query(
                &durable,
                &MemoryQuery {
                    statuses: vec![MemoryStatus::Candidate],
                    policy: RecallPolicy::Strict,
                    ..Default::default()
                }
            )
            .unwrap()
            .len(),
        1
    );
}

#[test]
fn spooled_episodes_are_recovered_after_restart() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let scope = derive_runtime_memory_scope(cwd.path(), "s1", "");
    let spool = home.path().join("memory").join("capture_pending");
    std::fs::create_dir_all(&spool).unwrap();
    let payload = json!({
        "deduplication_key": "k1", "source_uri": "qt-session://s1/rpc-episode/9",
        "content": "## user\n\nRoot cause was a stale lock file.\n",
        "scope": {"kind": "exact", "user": scope.user, "project": scope.project,
                  "repository": scope.repository, "branch": scope.branch, "session": "s1"},
        "ok": true, "turn_index": 9,
    });
    std::fs::write(spool.join("k1.json"), payload.to_string()).unwrap();
    std::fs::write(spool.join("ignored.txt"), "x").unwrap();
    let runtime = MemoryRuntime::new(MemoryRuntimeOptions {
        agent_home: home.path().into(),
        cwd: cwd.path().into(),
        session: "s1".into(),
        ..Default::default()
    })
    .unwrap();
    assert!(runtime.wait_for_capture_idle(Duration::from_secs(10)));
    assert!(!spool.join("k1.json").exists());
    let captured = runtime
        .store()
        .query(
            &durable_memory_scope(scope),
            &MemoryQuery {
                text: "stale lock".into(),
                statuses: vec![MemoryStatus::Candidate],
                policy: RecallPolicy::Strict,
                ..Default::default()
            },
        )
        .unwrap();
    assert_eq!(captured.len(), 1);
    assert_eq!(captured[0].memory_type, MemoryType::Bug);
}

#[test]
fn episode_text_renders_roles_tools_and_bounds() {
    let text = episode_text(&episode(), 32000);
    assert_eq!(
        text,
        "## user\n\nWe decided to use SQLite for the cache layer.\n\n## assistant\n\nUnderstood.\n\nTool call: bash\n## tool\n\nTool result: bash (success)\n\"ok\"\n\n"
    );
    assert_eq!(episode_text(&episode(), 10).len(), 10);
}

#[test]
fn foreground_and_idle_tracking() {
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    let runtime = runtime(home.path(), cwd.path(), None);
    runtime.set_foreground_active(true);
    assert_eq!(runtime.idle_for(), Duration::ZERO);
    runtime.set_foreground_active(false);
    std::thread::sleep(Duration::from_millis(20));
    assert!(runtime.idle_for() >= Duration::from_millis(20));
    assert_eq!(runtime.dreaming_status().pending_runs, 0);
    assert!(
        !runtime.enqueue_dream_cycle("manual"),
        "no Dreaming controls installed"
    );
    runtime.set_session_context("s2", "s2").unwrap();
    assert_eq!(runtime.scope().session, "s2");
    runtime.record_lifecycle_event("session_start", "qt:harness", "{\"reason\":\"new\"}", "");
    assert_eq!(
        runtime
            .store()
            .session_events(&runtime.scope(), "session_start", 10, 0)
            .unwrap()
            .len(),
        1
    );
}
