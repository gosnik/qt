use std::sync::Arc;

use qt_core::agent::AgentEvent;
use qt_core::harness::Hooks;
use qt_core::message::{AssistantMessage, Message, Usage};
use qt_core::tools::{MutationPolicy, MutationQueue, ToolContext, ToolRegistry};
use qt_memory::cli::{
    CommandOutput, MemoryCommandEnvironment, normalize_import_state, parse_review_time,
    run_memory_command,
};
use qt_memory::integration::attach_memory;
use qt_memory::rpc::{RpcMemoryCommand, RpcMemoryContext, handle_memory_rpc, review_rpc_error};
use qt_memory::settings::MemorySettings;
use qt_memory::slash::{
    SlashMemoryAction, SlashMemoryContext, memory_slash_command, review_duration,
};
use qt_memory::tools::{MEMORY_TOOL_NAMES, memory_tools, register_memory_tools};
use qt_memory::*;
use serde_json::{Value, json};

struct Env {
    home: tempfile::TempDir,
    cwd: tempfile::TempDir,
}

impl Env {
    fn new() -> Self {
        Self {
            home: tempfile::tempdir().unwrap(),
            cwd: tempfile::tempdir().unwrap(),
        }
    }

    fn runtime(&self) -> Arc<MemoryRuntime> {
        Arc::new(
            MemoryRuntime::new(MemoryRuntimeOptions {
                agent_home: self.home.path().into(),
                cwd: self.cwd.path().into(),
                coordinator: MemoryCoordinatorSettings {
                    enabled: true,
                    ..Default::default()
                },
                ..Default::default()
            })
            .unwrap(),
        )
    }

    fn cli(&self, args: &[&str]) -> CommandOutput {
        let settings = MemorySettings {
            enabled: true,
            ..Default::default()
        };
        run_memory_command(
            &args.iter().map(|arg| arg.to_string()).collect::<Vec<_>>(),
            &MemoryCommandEnvironment {
                cwd: self.cwd.path().into(),
                agent_home: self.home.path().into(),
                settings,
                ..Default::default()
            },
        )
    }
}

#[tokio::test]
async fn tools_remember_recall_and_gate_mutations() {
    let env = Env::new();
    let runtime = env.runtime();
    let tools = memory_tools(&runtime, |_| true);
    let names: Vec<&str> = tools.iter().map(|tool| tool.name()).collect();
    assert_eq!(names, MEMORY_TOOL_NAMES);
    let get = |name: &str| {
        tools
            .iter()
            .find(|tool| tool.name() == name)
            .unwrap()
            .clone()
    };
    let ctx = ToolContext::new(env.cwd.path());
    let created = get("memory_remember")
        .execute(
            json!({"content": "Prefer tabs in Makefiles", "status": "candidate"}),
            ctx.clone(),
        )
        .await;
    assert!(!created.is_error, "{}", created.text_content());
    let details = created.details.unwrap();
    assert_eq!(details["status"], "candidate");
    assert_eq!(details["created"], true);
    let invalid = get("memory_remember")
        .execute(json!({"content": "x", "status": "active"}), ctx.clone())
        .await;
    assert!(invalid.is_error);
    assert_eq!(
        invalid.text_content(),
        "memory_remember status must be candidate or hypothesis"
    );
    let recalled = get("memory_recall")
        .execute(json!({"query": "tabs Makefiles"}), ctx.clone())
        .await;
    let details = recalled.details.unwrap();
    assert_eq!(details["count"], 1);
    assert_eq!(details["memories"][0]["status"], "candidate");
    assert_eq!(
        details["scope_repository"],
        json!(runtime.scope().repository)
    );
    let unknown = get("memory_recall")
        .execute(json!({"query": "x", "extra": 1}), ctx.clone())
        .await;
    assert!(unknown.is_error);

    let queue = Arc::new(MutationQueue::new());
    let queued_ctx = ctx
        .clone()
        .with_mutation_policy(MutationPolicy::Queue, Arc::clone(&queue));
    let queued = get("memory_forget")
        .execute(json!({"id": "mem_x", "confirm": true}), queued_ctx)
        .await;
    assert!(queued.is_error);
    assert_eq!(queued.details.unwrap()["status"], "pending_approval");
    assert_eq!(
        queue.pending()[0].summary,
        "permanently forget memory mem_x"
    );
    let missing = get("memory_forget")
        .execute(json!({"id": "mem_x", "confirm": true}), ctx.clone())
        .await;
    assert_eq!(missing.text_content(), "memory not found in current scope");
    let list = get("memory_review_list").execute(json!({}), ctx).await;
    assert_eq!(list.details.unwrap()["count"], 0);

    let disabled = register_memory_tools(
        ToolRegistry::empty(),
        &MemorySettings::default(),
        Some(&runtime),
        |_| true,
    );
    assert!(disabled.is_empty());
    let enabled = register_memory_tools(
        ToolRegistry::empty(),
        &MemorySettings {
            enabled: true,
            ..Default::default()
        },
        Some(&runtime),
        |name| name == "memory_recall" || name == "memory_review",
    );
    assert_eq!(enabled.len(), 5);
}

#[test]
fn cli_remember_recall_status_and_review() {
    let env = Env::new();
    let help = env.cli(&["--help"]);
    assert!(
        help.stdout
            .starts_with("Usage: qt memory <command> [arguments] [options]")
    );
    let created = env.cli(&["remember", "Deploys", "use", "blue-green", "--json"]);
    assert_eq!(created.exit_code, 0, "{}", created.stderr);
    let value: Value = serde_json::from_str(created.stdout.trim()).unwrap();
    assert_eq!(value["status"], "hypothesis");
    let recalled = env.cli(&["recall", "blue-green"]);
    assert!(recalled.stdout.starts_with("Recalled memories:\n  "));
    assert!(
        recalled
            .stdout
            .contains(" [hypothesis] (observation) Deploys use blue-green\n")
    );
    let status = env.cli(&["status", "--json"]);
    let status: Value = serde_json::from_str(status.stdout.trim()).unwrap();
    assert_eq!(status["schema_version"], 7);
    assert_eq!(status["scope_memory_count"], 1);
    assert_eq!(status["statistics"]["catalog"]["total"], 1);
    let health = env.cli(&["health"]);
    assert!(health.stdout.starts_with("Memory health:\n"));
    assert!(health.stdout.contains("  healthy: true\n"));
    let review = env.cli(&["review"]);
    assert_eq!(
        review.stdout,
        "Memory Review Inbox (0):\n  No reviews match the active filters.\n"
    );
    let forget = env.cli(&["forget", "x"]);
    assert_eq!(forget.exit_code, 1);
    assert_eq!(
        forget.stderr,
        "qt memory: forget requires --yes authority\n"
    );
    let unknown = env.cli(&["status", "--bogus"]);
    assert_eq!(
        unknown.stderr,
        "qt memory: unknown memory option: --bogus\n"
    );
    let empty = env.cli(&["conflicts"]);
    assert_eq!(empty.stdout, "Memory conflicts:\n  (none)\n");

    let export = env.home.path().join("export.json");
    assert_eq!(env.cli(&["export", export.to_str().unwrap()]).exit_code, 0);
    let dry = env.cli(&["import", export.to_str().unwrap(), "--dry-run", "--json"]);
    let dry: Value = serde_json::from_str(dry.stdout.trim()).unwrap();
    assert_eq!(dry["duplicate_memories"], 1);
    assert_eq!(dry["would_import_memories"], 0);
    let prune = env.cli(&["prune", "--older-than-days", "30", "--dry-run", "--json"]);
    assert_eq!(
        serde_json::from_str::<Value>(prune.stdout.trim()).unwrap()["dry_run"],
        true
    );
}

#[test]
fn cli_review_operations_reach_durable_scope_reviews() {
    let env = Env::new();
    let runtime = env.runtime();
    let durable = durable_memory_scope(qt_memory::cli::command_scope(env.cwd.path(), false));
    let subject = runtime
        .store()
        .add(
            &durable,
            MemoryWrite {
                content: "Durable hypothesis about deploys".into(),
                status: MemoryStatus::Hypothesis,
                source: "test".into(),
                provenance: Provenance {
                    source_type: SourceType::File,
                    source_uri: "file:fixture".into(),
                    evidence_strength: 0.8,
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap();
    let review = runtime
        .store()
        .add_review(
            &durable,
            qt_memory::review::ReviewItemWrite {
                kind: "candidate_memory".into(),
                reason: "needs review".into(),
                subject_id: subject.clone(),
                provenance: Provenance {
                    source_type: SourceType::File,
                    source_uri: "file:evidence".into(),
                    evidence_strength: 0.9,
                    created_by: "agent".into(),
                    ..Default::default()
                },
                candidate_json: "{}".into(),
                ..Default::default()
            },
        )
        .unwrap();
    drop(runtime);
    let listed = env.cli(&["review", "list"]);
    assert!(listed.stdout.contains(&review), "{}", listed.stdout);
    let show = env.cli(&["review", "show", &review, "--json"]);
    assert_eq!(show.exit_code, 0, "{}", show.stderr);
    let detail: Value = serde_json::from_str(show.stdout.trim()).unwrap();
    assert_eq!(detail["review"]["id"], review.as_str());
    assert_eq!(detail["subject"]["id"], subject.as_str());
    let preview = env.cli(&["review", "preview", &review, "reject"]);
    assert_eq!(preview.exit_code, 0, "{}", preview.stderr);
    let decided = env.cli(&["review", "decide", &review, "reject", "--yes", "--json"]);
    assert_eq!(decided.exit_code, 0, "{}", decided.stderr);
    let decided: Value = serde_json::from_str(decided.stdout.trim()).unwrap();
    assert_eq!(decided["status"], "applied");
    let history = env.cli(&["review", "history", "--json"]);
    let history: Value = serde_json::from_str(history.stdout.trim()).unwrap();
    assert_eq!(history["items"][0]["review_id"], review.as_str());
    let by_id = env.cli(&["review", "history", &review, "--json"]);
    let by_id: Value = serde_json::from_str(by_id.stdout.trim()).unwrap();
    assert_eq!(by_id["items"].as_array().unwrap().len(), 1);
    let missing = env.cli(&["review", "show", "review_missing"]);
    assert_eq!(
        missing.stderr,
        "qt memory: review item not found in scope\n"
    );
}

#[test]
fn cli_helpers_parse_times_and_import_shapes() {
    assert_eq!(parse_review_time("2h", 1000).unwrap(), 1000 + 7_200_000);
    assert_eq!(
        parse_review_time("1700000000000", 0).unwrap(),
        1_700_000_000_000
    );
    assert!(parse_review_time("0h", 0).is_err());
    let state =
        normalize_import_state(json!({"entries": [{"content": "x", "memory_type": "bug"}]}))
            .unwrap();
    assert_eq!(state["memories"][0]["type"], "bug");
    assert_eq!(state["graph_edges"], json!([]));
    assert!(normalize_import_state(json!({"memories": {}})).is_err());
}

#[test]
fn rpc_status_recall_and_validation() {
    let env = Env::new();
    let runtime = env.runtime();
    let context = RpcMemoryContext {
        runtime: Some(Arc::clone(&runtime)),
        settings: MemorySettings {
            enabled: true,
            ..Default::default()
        },
        agent_home: env.home.path().into(),
        ..Default::default()
    };
    let command = |action: &str, params: Value, confirm: bool| RpcMemoryCommand {
        action: action.into(),
        params,
        confirm,
    };
    let status = handle_memory_rpc(&context, &command("status", json!({}), false)).unwrap();
    assert_eq!(status["enabled"], true);
    assert_eq!(
        status["prehandler"],
        "isolated_child_with_deterministic_fallback"
    );
    assert_eq!(status["retrieval"]["jobs"]["pending"], 0);
    let refused = handle_memory_rpc(
        &context,
        &command("remember", json!({"content": "x"}), false),
    )
    .unwrap_err();
    assert_eq!(refused.message, "memory remember requires confirm=true");
    let created = handle_memory_rpc(
        &context,
        &command(
            "remember",
            json!({"content": "Use cargo fmt before commits"}),
            true,
        ),
    )
    .unwrap();
    assert_eq!(created["status"], "hypothesis");
    let recalled = handle_memory_rpc(
        &context,
        &command("recall", json!({"query": "cargo fmt"}), false),
    )
    .unwrap();
    assert_eq!(recalled["count"], 1);
    assert_eq!(
        recalled["memories"][0]["revision"]["basis"],
        "current_canonical_at_explanation"
    );
    let explained = handle_memory_rpc(
        &context,
        &command("explain", json!({"query": "cargo fmt"}), false),
    )
    .unwrap();
    assert_eq!(explained["context"]["injected"], false);
    assert_eq!(explained["requested_modes"]["lexical"], true);
    assert_eq!(explained["observed_retrievers"], json!(["lexical"]));
    assert_eq!(explained["memories"][0]["selection"]["preview"], true);
    let bad_limit = handle_memory_rpc(
        &context,
        &command("recall", json!({"query": "x", "limit": 99}), false),
    )
    .unwrap_err();
    assert_eq!(
        bad_limit.message,
        "memory recall limit must be from 1 to 50"
    );
    let review = handle_memory_rpc(
        &context,
        &command("review", json!({"action": "show"}), false),
    )
    .unwrap_err();
    assert_eq!(
        review.message,
        "rpc_error:invalid_review_schema:memory review show requires string id"
    );
    assert_eq!(
        review_rpc_error("review item not found in scope"),
        "rpc_error:review_not_found:review item not found in scope"
    );
    let disabled = RpcMemoryContext::default();
    assert_eq!(
        handle_memory_rpc(&disabled, &command("recall", json!({}), false))
            .unwrap_err()
            .message,
        "built-in memory is disabled for this RPC runtime"
    );
    assert_eq!(
        handle_memory_rpc(&disabled, &command("status", json!({}), false)).unwrap()["statistics"],
        Value::Null
    );
    let rebuild =
        handle_memory_rpc(&context, &command("retrieval_rebuild", json!({}), true)).unwrap();
    assert_eq!(rebuild["owners"], 1);
}

#[tokio::test]
async fn slash_commands_render_lines_and_actions() {
    let env = Env::new();
    let runtime = env.runtime();
    let context = SlashMemoryContext {
        runtime: Some(Arc::clone(&runtime)),
        settings: MemorySettings {
            enabled: true,
            ..Default::default()
        },
        agent_home: env.home.path().into(),
        cwd: env.cwd.path().into(),
        workspace_root: env.cwd.path().into(),
        diagnostics: vec!["memory index rebuilt".into(), "other".into()],
        ..Default::default()
    };
    let status = memory_slash_command(&context, "status").await;
    assert_eq!(status.lines[0], "Built-in memory:");
    assert!(status.lines.contains(&"  enabled: true".to_string()));
    assert!(status.lines.contains(&"    stored memories: 0".to_string()));
    assert!(
        status
            .lines
            .contains(&"    - memory index rebuilt".to_string())
    );
    let remembered = memory_slash_command(&context, "remember Use rg for search").await;
    assert_eq!(remembered.lines[0], "memory remember");
    let recalled = memory_slash_command(&context, "recall rg search").await;
    assert_eq!(recalled.lines[0], "memory recall");
    assert!(
        recalled
            .lines
            .iter()
            .any(|line| line.contains("Use rg for search"))
    );
    let forget = memory_slash_command(&context, "forget mem_1").await;
    assert_eq!(forget.lines[0], "usage: /memory forget <memory-id> confirm");
    assert_eq!(
        memory_slash_command(&context, "review").await.action,
        Some(SlashMemoryAction::OpenReviewInbox)
    );
    let decide = memory_slash_command(&context, "review decide r1 maybe confirm").await;
    assert_eq!(
        decide.lines,
        vec!["decision must be accepted, rejected, or deferred"]
    );
    let missing = memory_slash_command(&context, "review decide r1 accepted confirm").await;
    assert!(missing.lines[0].starts_with("Memory review was not changed: "));
    let dream = memory_slash_command(&context, "dream run").await;
    assert_eq!(
        dream.lines,
        vec![
            "Memory Dreaming cycle was not queued; enable Dreaming and verify its queue/model settings."
        ]
    );
    let skills = env.cwd.path().join("skills");
    std::fs::create_dir_all(&skills).unwrap();
    let scan = memory_slash_command(&context, "scan-skills recursive skills").await;
    assert!(matches!(
        scan.action,
        Some(SlashMemoryAction::ScanSkills {
            recursive: true,
            ..
        })
    ));
    let outside = memory_slash_command(&context, "scan-skills /").await;
    assert_eq!(
        outside.lines,
        vec![
            "memory skill scan folder is outside the workspace; enable external filesystem access in /settings to allow it"
        ]
    );
    assert_eq!(review_duration(7200), "2h");
    let disabled = SlashMemoryContext::default();
    assert_eq!(
        memory_slash_command(&disabled, "recall x").await.lines[0],
        "Built-in memory is disabled for this runtime."
    );
}

#[test]
fn hooks_capture_runs_and_lifecycle() {
    let env = Env::new();
    let runtime = env.runtime();
    let hooks = Hooks::new();
    let (bridge, ids) = attach_memory(&hooks, Arc::clone(&runtime), "session-9");
    assert_eq!(ids.0.len(), 3);
    assert_eq!(runtime.scope().session, "session-9");
    hooks.emit_agent_event(&AgentEvent::AgentStart);
    assert!(runtime.foreground_active());
    hooks.emit_agent_event(&AgentEvent::MessageEnd(Message::user_text(
        "Never commit generated files to the repository.",
    )));
    hooks.emit_agent_event(&AgentEvent::MessageEnd(Message::Assistant(
        AssistantMessage::default(),
    )));
    hooks.emit_agent_event(&AgentEvent::AgentEnd {
        usage: Usage::default(),
        aborted: false,
    });
    assert!(!runtime.foreground_active());
    assert_eq!(bridge.current_run(), 1);
    assert!(runtime.wait_for_capture_idle(std::time::Duration::from_secs(10)));
    let events = runtime
        .store()
        .session_events(&runtime.scope(), "completed_episode", 10, 0)
        .unwrap();
    assert_eq!(events.len(), 1);
    let payload: Value = serde_json::from_str(&events[0].payload_json).unwrap();
    assert_eq!(
        payload["source_uri"],
        "qt-session://session-9/rpc-episode/1"
    );
    assert_eq!(payload["turn_index"], 1);
}

const CODE_UNAVAILABLE: &str =
    "code retrieval unavailable: persisted index fingerprints are incomplete; rescan needed";

/// A project index without a usable snapshot.
struct NoSnapshot;

impl qt_memory::code::CodeSnapshotProvider for NoSnapshot {
    fn snapshot(&self) -> Option<Arc<qt_memory::code::CodeSnapshot>> {
        None
    }

    fn unavailable_reason(&self) -> Option<String> {
        Some(CODE_UNAVAILABLE.into())
    }
}

#[test]
fn cli_explain_reports_unavailable_code_retrieval() {
    let env = Env::new();
    let explain = |code_enabled: bool| {
        let mut settings = MemorySettings {
            enabled: true,
            ..Default::default()
        };
        settings.retrieval.code_enabled = code_enabled;
        let output = run_memory_command(
            &["explain", "checksum", "--json"].map(String::from),
            &MemoryCommandEnvironment {
                cwd: env.cwd.path().into(),
                agent_home: env.home.path().into(),
                settings,
                code: Some(Arc::new(NoSnapshot)),
                ..Default::default()
            },
        );
        assert_eq!(output.exit_code, 0, "{}", output.stderr);
        serde_json::from_str::<Value>(&output.stdout).unwrap()
    };
    let enabled = explain(true);
    assert_eq!(enabled["retrieval_warnings"], json!([CODE_UNAVAILABLE]));
    assert_eq!(enabled["requested_modes"]["code"], true);
    assert_eq!(enabled["code_candidates"], json!([]));
    assert_eq!(explain(false)["retrieval_warnings"], json!([]));
}
