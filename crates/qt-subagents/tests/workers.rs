//! Persistent workers: coordinator, tool actions, storage, recovery and
//! delivery into a parent conversation, against the scripted mock child.

use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{Value, json};
use tokio::runtime::Handle;

use qt_core::message::{AgentMessage, AssistantMessage, ContentBlock, CustomMessage, Message};
use qt_core::policy::WorkspacePolicy;
use qt_core::provider::ModelRef;
use qt_core::tools::{Tool, ToolContext, ToolOutput};
use qt_core::{AgentHome, Engine};
use qt_subagents::coordinator::{CoordinatorOptions, FORK_INTERRUPTED_TEXT, coordinator_directory};
use qt_subagents::delivery::{DeliverFn, attach_delivery};
use qt_subagents::{
    ParentState, ProcessChildRunner, SubagentCoordinator, SubagentTool, SubagentToolOptions,
    attach_conversation,
};

const MOCK: &str = env!("CARGO_BIN_EXE_qt-subagents-mock-child");
const WAIT: Duration = Duration::from_secs(20);

struct Fixture {
    _dir: tempfile::TempDir,
    home: PathBuf,
    workspace: PathBuf,
    log: PathBuf,
}

impl Fixture {
    fn new() -> Self {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().canonicalize().unwrap();
        let home = root.join("home");
        let workspace = root.join("ws");
        std::fs::create_dir_all(&home).unwrap();
        std::fs::create_dir_all(&workspace).unwrap();
        Self {
            log: root.join("calls.jsonl"),
            _dir: dir,
            home,
            workspace,
        }
    }

    fn runner(&self) -> Arc<ProcessChildRunner> {
        Arc::new(
            ProcessChildRunner::new(MOCK).with_env("QT_MOCK_CHILD_LOG", self.log.to_string_lossy()),
        )
    }

    fn coordinator_options(&self, session: &str, runtime: Handle) -> CoordinatorOptions {
        CoordinatorOptions {
            directory: coordinator_directory(&self.home, session),
            parent_session_id: session.into(),
            parent_path: "/root".into(),
            concurrency: 4,
            runtime,
        }
    }

    fn coordinator(&self, session: &str) -> Arc<SubagentCoordinator> {
        Arc::new(SubagentCoordinator::open(
            self.coordinator_options(session, Handle::current()),
            self.runner(),
        ))
    }

    fn tool(&self, coordinator: &Arc<SubagentCoordinator>) -> SubagentTool {
        SubagentTool::new(
            SubagentToolOptions::new(&self.home)
                .with_runner(self.runner())
                .with_depth(0, 2)
                .with_coordinator(coordinator.clone()),
        )
    }

    fn ctx(&self) -> ToolContext {
        ToolContext::new(&self.workspace)
    }

    fn calls(&self) -> Vec<Value> {
        std::fs::read_to_string(&self.log)
            .unwrap_or_default()
            .lines()
            .map(|l| serde_json::from_str(l).unwrap())
            .collect()
    }
}

fn details(output: &ToolOutput) -> Value {
    output.details.clone().expect("details")
}

async fn wait_done(coordinator: &SubagentCoordinator, target: &str) -> Value {
    let result = coordinator.wait(target, WAIT).await.unwrap();
    assert_eq!(result["completed"], true, "{result}");
    result
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn spawn_runs_a_worker_and_records_completion() {
    let fx = Fixture::new();
    let coordinator = fx.coordinator("parent-a");
    let tool = fx.tool(&coordinator);
    let out = tool
        .execute(
            json!({"action": "spawn", "agent": "reviewer", "name": "rev", "task": "Review it"}),
            fx.ctx(),
        )
        .await;
    assert!(!out.is_error, "{out:?}");
    let spawned = details(&out);
    assert_eq!(spawned["child_id"], "/root/rev");
    assert_eq!(spawned["generation"], 0);
    let session = coordinator.directory().join("rev/session.jsonl");
    assert_eq!(spawned["path"], session.to_string_lossy().as_ref());
    assert!(
        coordinator
            .directory()
            .ends_with(qt_subagents::stable_hash_hex("parent-a"))
    );

    let waited = wait_done(&coordinator, "rev").await;
    assert_eq!(waited["status"], "completed");
    assert_eq!(waited["event_id"], 1);

    let events = coordinator.take_events(usize::MAX);
    assert_eq!(events.len(), 1);
    let event = &events[0];
    assert_eq!(event["type"], "subagent_complete");
    assert_eq!(event["child_id"], "/root/rev");
    assert_eq!(event["generation"], 1);
    assert_eq!(event["status"], "completed");
    assert_eq!(event["interrupted"], false);
    assert_eq!(event["exit_code"], 0);
    assert_eq!(event["output"], "done: Review it");

    let call = &fx.calls()[0];
    let args: Vec<String> = serde_json::from_value(call["args"].clone()).unwrap();
    assert_eq!(&args[..3], ["--mode", "json", "--no-extensions"]);
    assert!(!args.contains(&"--no-session".to_string()));
    let n = args.len();
    assert_eq!(args[n - 2], "--session");
    assert_eq!(args[n - 1], session.to_string_lossy());
    assert_eq!(call["stdin"], "Review it\n");
    assert_eq!(call["env"]["QT_SUBAGENT_PATH"], "/root/rev");
    assert_eq!(call["env"]["QT_SUBAGENT_PARENT_SESSION"], "parent-a");
    assert_eq!(call["env"]["QT_SUBAGENT_DEPTH"], "1");
    assert_eq!(call["env"]["QT_SUBAGENT_MAX_DEPTH"], "0");

    // Storage: private index with the worker record.
    let index: Value = serde_json::from_str(
        &std::fs::read_to_string(coordinator.directory().join("children.json")).unwrap(),
    )
    .unwrap();
    assert_eq!(index["version"], 1);
    assert_eq!(index["parent_session_id"], "parent-a");
    assert_eq!(index["children"][0]["id"], "/root/rev");
    assert_eq!(index["children"][0]["status"], "completed");
    assert_eq!(
        index["children"][0]["permissions"]["workspace"],
        fx.workspace.to_string_lossy().as_ref()
    );
    assert_eq!(index["pending_events"], json!([]));
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        let mode = std::fs::metadata(&session).unwrap().permissions().mode();
        assert_eq!(mode & 0o777, 0o600);
    }

    // Names are unique.
    let again = tool
        .execute(
            json!({"action": "spawn", "agent": "reviewer", "name": "rev", "task": "x"}),
            fx.ctx(),
        )
        .await;
    assert!(again.is_error);
    assert_eq!(
        again.text_content(),
        "child already exists; use send with wake=true to reuse it"
    );
    coordinator.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn mailbox_send_and_follow_up() {
    let fx = Fixture::new();
    let coordinator = fx.coordinator("parent-b");
    let tool = fx.tool(&coordinator);
    tool.execute(
        json!({"action": "spawn", "agent": "worker", "name": "w", "task": "first"}),
        fx.ctx(),
    )
    .await;
    wait_done(&coordinator, "/root/w").await;

    let sent = tool
        .execute(
            json!({"action": "send", "target": "w", "message": "queued note"}),
            fx.ctx(),
        )
        .await;
    assert!(!sent.is_error, "{sent:?}");
    assert_eq!(
        details(&sent),
        json!({"child_id": "/root/w", "queued": true, "wake": false, "mailbox_depth": 1})
    );
    // An idle worker is not woken by a plain send.
    let waited = coordinator
        .wait("w", Duration::from_millis(200))
        .await
        .unwrap();
    assert_eq!(waited["status"], "completed");
    assert_eq!(waited["event_id"], 1);

    let follow = tool
        .execute(
            json!({"action": "follow_up", "target": "/root/w", "task": "second"}),
            fx.ctx(),
        )
        .await;
    assert_eq!(details(&follow)["wake"], true);
    assert_eq!(details(&follow)["mailbox_depth"], 2);
    // Both mailbox entries run, in order.
    for _ in 0..50 {
        let listed = coordinator.list().unwrap();
        if listed["children"][0]["event_id"] == 3 && listed["children"][0]["status"] == "completed"
        {
            break;
        }
        coordinator
            .wait("w", Duration::from_millis(200))
            .await
            .unwrap();
    }
    let outputs: Vec<String> = coordinator
        .take_events(usize::MAX)
        .iter()
        .map(|e| e["output"].as_str().unwrap().to_string())
        .collect();
    assert_eq!(
        outputs,
        ["done: first", "done: queued note", "done: second"]
    );
    let listed = tool
        .execute(json!({"action": "list_workers"}), fx.ctx())
        .await;
    let worker = &details(&listed)["children"][0];
    assert_eq!(worker["generation"], 3);
    assert_eq!(worker["mailbox_depth"], 0);
    assert_eq!(worker["parent_session_id"], "parent-b");

    let unknown = tool
        .execute(
            json!({"action": "send", "target": "nope", "message": "m"}),
            fx.ctx(),
        )
        .await;
    assert_eq!(unknown.text_content(), "unknown child: nope");
    let bad = tool
        .execute(
            json!({"action": "send", "target": "w", "task": "m"}),
            fx.ctx(),
        )
        .await;
    assert_eq!(
        bad.text_content(),
        "subagent send requires target and message"
    );
    let bad = tool
        .execute(
            json!({"action": "wait", "target": "w", "timeout_ms": 70000}),
            fx.ctx(),
        )
        .await;
    assert_eq!(bad.text_content(), "timeout_ms must be between 0 and 60000");
    let bad = tool
        .execute(json!({"action": "list_workers", "target": "w"}), fx.ctx())
        .await;
    assert_eq!(
        bad.text_content(),
        "unsupported arguments for persistent subagent action"
    );
    coordinator.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn interrupt_running_and_scheduled_turns() {
    let fx = Fixture::new();
    let coordinator = fx.coordinator("parent-c");
    let tool = fx.tool(&coordinator);
    tool.execute(
        json!({"action": "spawn", "agent": "worker", "name": "slow", "task": "sleep:10000"}),
        fx.ctx(),
    )
    .await;
    for _ in 0..100 {
        if coordinator.list().unwrap()["children"][0]["status"] == "running" {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    let out = tool
        .execute(json!({"action": "interrupt", "target": "slow"}), fx.ctx())
        .await;
    assert_eq!(
        details(&out),
        json!({"child_id": "/root/slow", "interrupted": true, "status": "interrupting"})
    );
    let waited = tool
        .execute(
            json!({"action": "wait", "target": "slow", "timeout_ms": 20000}),
            fx.ctx(),
        )
        .await;
    assert_eq!(details(&waited)["status"], "interrupted");
    let events = coordinator.take_events(usize::MAX);
    assert_eq!(events.last().unwrap()["interrupted"], true);
    assert_eq!(events.last().unwrap()["status"], "interrupted");

    // Nothing scheduled: interrupting again is a no-op.
    let out = coordinator.interrupt("slow").unwrap();
    assert_eq!(out["interrupted"], false);
    assert_eq!(out["status"], "interrupted");
    coordinator.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn directory_lock_and_restart_recovery() {
    let fx = Fixture::new();
    let coordinator = fx.coordinator("parent-d");
    let second = fx.coordinator("parent-d");
    assert_eq!(
        second.persistence_error().as_deref(),
        Some("another subagent coordinator owns the directory")
    );
    assert_eq!(
        second.list().unwrap_err(),
        "another subagent coordinator owns the directory"
    );
    drop(second);

    let tool = fx.tool(&coordinator);
    tool.execute(
        json!({"action": "spawn", "agent": "worker", "name": "long", "task": "sleep:10000"}),
        fx.ctx(),
    )
    .await;
    let index_path = coordinator.directory().join("children.json");
    let mut crashed = String::new();
    for _ in 0..100 {
        let text = std::fs::read_to_string(&index_path).unwrap();
        if text.contains("\"status\": \"running\"") {
            crashed = text;
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    assert!(!crashed.is_empty(), "worker never recorded as running");
    coordinator.shutdown().await;
    // Simulate a crash: the index still says the worker is running.
    std::fs::write(&index_path, crashed).unwrap();

    let restored = fx.coordinator("parent-d");
    assert_eq!(restored.persistence_error(), None);
    let events = restored.pending_events();
    assert_eq!(events.len(), 1);
    assert_eq!(events[0]["recovered"], true);
    assert_eq!(events[0]["status"], "interrupted");
    assert_eq!(events[0]["child_id"], "/root/long");
    let listed = restored.list().unwrap();
    assert_eq!(listed["children"][0]["status"], "interrupted");
    assert!(!restored.has_active_workers());
    // The recovered worker keeps its identity and resumes on follow-up.
    restored.send("long", "resume".into(), true).unwrap();
    wait_done(&restored, "long").await;
    assert_eq!(
        restored.list().unwrap()["children"][0]["status"],
        "completed"
    );
    restored.acknowledge_event("/root/long", 2).unwrap();
    assert!(restored.pending_events().iter().all(|e| e["event_id"] != 2));
    restored.shutdown().await;

    // An index of another parent session is rejected.
    let other_dir = coordinator_directory(&fx.home, "parent-e");
    std::fs::create_dir_all(&other_dir).unwrap();
    std::fs::write(
        other_dir.join("children.json"),
        json!({"version": 1, "parent_session_id": "someone-else", "children": []}).to_string(),
    )
    .unwrap();
    let rejected = fx.coordinator("parent-e");
    assert_eq!(
        rejected.persistence_error().as_deref(),
        Some("invalid children index: child index belongs to another parent session")
    );
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn fork_snapshot_closes_in_flight_calls() {
    let fx = Fixture::new();
    let coordinator = fx.coordinator("parent-f");
    let tool = fx.tool(&coordinator);
    let no_snapshot = tool
        .execute(
            json!({"action": "spawn", "agent": "scout", "name": "f0", "task": "t", "fork": "all"}),
            fx.ctx(),
        )
        .await;
    assert_eq!(
        no_snapshot.text_content(),
        "fork requested without context snapshot"
    );
    let context = vec![
        AgentMessage::Llm(Message::user_text("old turn")),
        AgentMessage::Llm(Message::user_text("spawn a scout")),
        AgentMessage::Llm(Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::ToolCall {
                id: "call-9".into(),
                name: "subagent".into(),
                arguments: json!({"action": "spawn"}),
            }],
            ..Default::default()
        })),
    ];
    coordinator.set_context_snapshot(Arc::new(move || context.clone()));
    let out = tool
        .execute(
            json!({"action": "spawn", "agent": "scout", "name": "f1", "task": "t", "fork": 1}),
            fx.ctx(),
        )
        .await;
    assert!(!out.is_error, "{out:?}");
    let session =
        std::fs::read_to_string(coordinator.directory().join("f1/session.jsonl")).unwrap();
    let lines: Vec<Value> = session
        .lines()
        .map(|l| serde_json::from_str(l).unwrap())
        .collect();
    assert_eq!(lines[0]["subagent"]["fork"], 1);
    assert_eq!(lines[0]["subagent"]["childId"], "/root/f1");
    assert_eq!(lines[0]["parentSession"], "parent-f");
    assert_eq!(lines.len(), 4);
    assert_eq!(lines[1]["message"]["content"], "spawn a scout");
    assert_eq!(lines[3]["message"]["toolCallId"], "call-9");
    assert_eq!(
        lines[3]["message"]["content"][0]["text"],
        FORK_INTERRUPTED_TEXT
    );

    let bad = tool
        .execute(
            json!({"action": "spawn", "agent": "scout", "name": "f2", "task": "t", "fork": "some"}),
            fx.ctx(),
        )
        .await;
    assert_eq!(
        bad.text_content(),
        "fork must be none, all, or a positive user-turn count"
    );
    coordinator.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn worker_permissions_are_rechecked() {
    let fx = Fixture::new();
    let coordinator = fx.coordinator("parent-g");
    let parent: Arc<Mutex<ParentState>> = Arc::new(Mutex::new(ParentState {
        active_tools: Some(vec!["read_text".into(), "grep".into(), "bash".into()]),
        model: Some(ModelRef::new("openai", "gpt-5")),
    }));
    let source = parent.clone();
    let tool = SubagentTool::new(
        SubagentToolOptions::new(&fx.home)
            .with_runner(fx.runner())
            .with_depth(0, 2)
            .with_coordinator(coordinator.clone())
            .with_parent_state(Arc::new(move || source.lock().unwrap().clone())),
    );
    let out = tool
        .execute(
            json!({"action": "spawn", "agent": "reviewer", "name": "p", "task": "t"}),
            fx.ctx(),
        )
        .await;
    assert!(!out.is_error, "{out:?}");
    wait_done(&coordinator, "p").await;
    let call = &fx.calls()[0];
    let args: Vec<String> = serde_json::from_value(call["args"].clone()).unwrap();
    let joined = args.join(" ");
    assert!(
        joined.contains("--tools read,grep,bash --provider openai --model gpt-5"),
        "{joined}"
    );
    let worker = &coordinator.list().unwrap()["children"][0];
    assert_eq!(
        worker["permissions"]["toolnames"],
        json!(["read", "grep", "bash"])
    );
    assert_eq!(worker["permissions"]["model"], "gpt-5");
    assert_eq!(worker["permissions"]["trust"], false);

    // Narrower active tools: messaging the worker is refused.
    parent.lock().unwrap().active_tools = Some(vec!["read".into()]);
    let out = tool
        .execute(
            json!({"action": "follow_up", "target": "p", "task": "more"}),
            fx.ctx(),
        )
        .await;
    assert_eq!(
        out.text_content(),
        "persistent worker tool permissions exceed current tool context"
    );
    parent.lock().unwrap().active_tools = None;
    // A different workspace is refused too.
    let elsewhere = tempfile::tempdir().unwrap();
    let ctx = ToolContext::new(elsewhere.path())
        .with_policy(Arc::new(WorkspacePolicy::confined(elsewhere.path())));
    let out = tool
        .execute(
            json!({"action": "send", "target": "p", "message": "m"}),
            ctx,
        )
        .await;
    assert_eq!(
        out.text_content(),
        "persistent worker permissions exceed current tool context"
    );
    // Depth limits apply to spawn.
    let limited = SubagentTool::new(
        SubagentToolOptions::new(&fx.home)
            .with_runner(fx.runner())
            .with_depth(1, 1)
            .with_coordinator(coordinator.clone()),
    );
    let out = limited
        .execute(
            json!({"action": "spawn", "agent": "reviewer", "name": "deep", "task": "t"}),
            fx.ctx(),
        )
        .await;
    assert_eq!(
        out.text_content(),
        "subagent depth limit reached at depth 1 (max 1)"
    );
    coordinator.shutdown().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn completions_are_delivered_and_acknowledged() {
    let fx = Fixture::new();
    let coordinator = fx.coordinator("parent-h");
    let delivered: Arc<Mutex<Vec<CustomMessage>>> = Arc::default();
    let sink = delivered.clone();
    let deliver: DeliverFn = Arc::new(move |message| {
        sink.lock().unwrap().push(message);
        true
    });
    attach_delivery(&coordinator, deliver);
    let tool = fx.tool(&coordinator);
    tool.execute(
        json!({"action": "spawn", "agent": "scout", "name": "d", "task": "deliver me"}),
        fx.ctx(),
    )
    .await;
    wait_done(&coordinator, "d").await;
    for _ in 0..100 {
        if !delivered.lock().unwrap().is_empty() {
            break;
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    let messages = delivered.lock().unwrap().clone();
    assert_eq!(messages.len(), 1);
    assert_eq!(messages[0].custom_type, "subagent_event");
    let details = messages[0].details.clone().unwrap();
    assert_eq!(details["output"], "done: deliver me");
    assert!(coordinator.pending_events().is_empty());
    assert!(coordinator.take_events(usize::MAX).is_empty());
    coordinator.shutdown().await;
}

#[test]
fn conversation_receives_subagent_events() {
    let fx = Fixture::new();
    let engine = Engine::new(AgentHome::new(fx.home.clone()), &fx.workspace).unwrap();
    let conversation = engine.new_ephemeral_conversation(None);
    let runtime = engine.block_on(async { Handle::current() });
    let coordinator = Arc::new(SubagentCoordinator::open(
        fx.coordinator_options(&conversation.session_id(), runtime),
        fx.runner(),
    ));
    attach_conversation(&coordinator, &conversation, Arc::new(|_events| {}));
    conversation
        .send_custom_message(
            CustomMessage::new(
                "note",
                qt_core::message::UserContent::Text("hi".into()),
                false,
            ),
            qt_core::Delivery::FollowUp,
            false,
        )
        .unwrap();
    let tool = fx.tool(&coordinator);
    let out = engine.block_on(tool.execute(
        json!({"action": "spawn", "agent": "scout", "name": "c", "task": "report", "fork": "all"}),
        fx.ctx(),
    ));
    assert!(!out.is_error, "{out:?}");
    engine.block_on(async {
        coordinator.wait("c", WAIT).await.unwrap();
        for _ in 0..100 {
            if coordinator.pending_events().is_empty() {
                break;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    });
    let messages = conversation.agent_messages();
    let event = messages
        .iter()
        .find_map(|m| match m {
            AgentMessage::Custom(c) if c.custom_type == "subagent_event" => Some(c.clone()),
            _ => None,
        })
        .expect("subagent_event delivered");
    assert!(event.display);
    assert_eq!(event.details.unwrap()["output"], "done: report");
    assert!(coordinator.pending_events().is_empty());
    // The fork snapshot came from the conversation.
    let session = std::fs::read_to_string(coordinator.directory().join("c/session.jsonl")).unwrap();
    assert!(session.contains("\"customType\":\"note\""));
    engine.block_on(coordinator.shutdown());
}
