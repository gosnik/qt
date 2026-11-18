//! Synchronous `subagent` modes against the scripted mock child.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{Value, json};

use qt_core::tools::{MutationPolicy, MutationQueue, Tool, ToolContext, ToolOutput};
use qt_subagents::{ProcessChildRunner, SubagentTool, SubagentToolOptions};

const MOCK: &str = env!("CARGO_BIN_EXE_qt-subagents-mock-child");

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
        std::fs::create_dir_all(workspace.join("sub")).unwrap();
        Self {
            log: root.join("calls.jsonl"),
            _dir: dir,
            home,
            workspace,
        }
    }

    fn options(&self) -> SubagentToolOptions {
        let runner =
            ProcessChildRunner::new(MOCK).with_env("QT_MOCK_CHILD_LOG", self.log.to_string_lossy());
        SubagentToolOptions::new(&self.home)
            .with_runner(Arc::new(runner))
            .with_depth(0, 2)
    }

    fn tool(&self) -> SubagentTool {
        SubagentTool::new(self.options())
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

fn details(output: &ToolOutput) -> &Value {
    output.details.as_ref().expect("details")
}

fn write(path: &Path, text: &str) {
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, text).unwrap();
}

#[tokio::test]
async fn single_mode_runs_one_child() {
    let fx = Fixture::new();
    let out = fx
        .tool()
        .execute(json!({"agent": "scout", "task": "look around"}), fx.ctx())
        .await;
    assert!(!out.is_error, "{out:?}");
    assert_eq!(out.text_content(), "done: look around");
    let d = details(&out);
    assert_eq!(d["mode"], "single");
    assert_eq!(d["agent_scope"], "user");
    assert_eq!(d["completed"], 1);
    assert_eq!(d["total"], 1);
    let result = &d["results"][0];
    assert_eq!(result["status"], "completed");
    assert_eq!(result["exit_code"], 0);
    assert_eq!(result["agent_source"], "builtin");
    assert_eq!(result["cwd"], fx.workspace.to_string_lossy().as_ref());
    assert_eq!(result["messages"].as_array().unwrap().len(), 2);
    assert!(d["run_id"].as_str().unwrap().starts_with("subagent-"));

    let calls = fx.calls();
    assert_eq!(calls.len(), 1);
    let args: Vec<String> = serde_json::from_value(calls[0]["args"].clone()).unwrap();
    assert_eq!(
        &args[..7],
        [
            "--mode",
            "json",
            "--no-session",
            "--no-extensions",
            "--no-context-files",
            "--no-skills",
            "--tools"
        ]
    );
    assert_eq!(
        args[7],
        "read,symbol,analyze_file,references,build_context,search_index,index,grep,find,ls,bash"
    );
    assert_eq!(args[8], "--system-prompt");
    assert_eq!(args.last().unwrap(), "Task: look around");
    let env = &calls[0]["env"];
    assert_eq!(env["QT_SUBAGENT_CHILD"], "1");
    assert_eq!(env["QT_SUBAGENT_DEPTH"], "1");
    assert_eq!(env["QT_SUBAGENT_MAX_DEPTH"], "0");
    assert_eq!(env["QT_SUBAGENT_ALLOW_EXTERNAL"], "0");
    assert_eq!(env["QT_SUBAGENT_NAME"], "scout");
    assert_eq!(
        env["QT_CODING_AGENT_DIR"],
        fx.home.to_string_lossy().as_ref()
    );
    assert_eq!(calls[0]["cwd"], fx.workspace.to_string_lossy().as_ref());
}

#[tokio::test]
async fn user_definition_with_model_cwd_and_append_prompt() {
    let fx = Fixture::new();
    write(
        &fx.home.join("agents/helper.md"),
        "---\nname: helper\ndescription: Helps\ntools: read\nmodel: m1\nthinking: low\nsystemPromptMode: append\ninheritSkills: true\n---\nBe helpful.\n",
    );
    let out = fx
        .tool()
        .execute(
            json!({"agent": "helper", "task": "t", "cwd": "sub", "model": "m2"}),
            fx.ctx(),
        )
        .await;
    assert!(!out.is_error, "{out:?}");
    let result = &details(&out)["results"][0];
    assert_eq!(result["model"], "m2:low");
    assert_eq!(result["agent_source"], "user");
    let call = &fx.calls()[0];
    let args: Vec<String> = serde_json::from_value(call["args"].clone()).unwrap();
    assert!(!args.contains(&"--no-skills".to_string()));
    let joined = args.join(" ");
    assert!(joined.contains("--tools read --model m2:low --append-system-prompt Be helpful."));
    assert_eq!(
        call["cwd"],
        fx.workspace.join("sub").to_string_lossy().as_ref()
    );
    assert_eq!(call["env"]["QT_SUBAGENT_MAX_DEPTH"], "2");
}

#[tokio::test]
async fn parallel_keeps_source_order_and_reports_failures() {
    let fx = Fixture::new();
    let progress: Arc<Mutex<Vec<String>>> = Arc::default();
    let sink = progress.clone();
    let ctx = fx
        .ctx()
        .with_progress(Arc::new(move |text| sink.lock().unwrap().push(text)));
    let out = fx
        .tool()
        .execute(
            json!({
                "tasks": [
                    {"agent": "scout", "task": "slow sleep:300"},
                    {"agent": "planner", "task": "fail please"},
                    {"agent": "reviewer", "task": "quick"}
                ],
                "concurrency": 3
            }),
            ctx,
        )
        .await;
    assert!(!out.is_error, "parallel mode never fails as a whole");
    let text = out.text_content();
    assert!(
        text.starts_with("2/3 subagents completed\n\n=== Task 1: scout ===\ndone: slow sleep:300")
    );
    assert!(text.contains("=== Task 2: planner ===\nboom"));
    assert!(text.ends_with("=== Task 3: reviewer ===\ndone: quick"));
    let d = details(&out);
    assert_eq!(d["mode"], "parallel");
    assert_eq!(d["completed"], 2);
    let agents: Vec<&str> = d["results"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| r["agent"].as_str().unwrap())
        .collect();
    assert_eq!(agents, ["scout", "planner", "reviewer"]);
    assert_eq!(d["results"][1]["exit_code"], 3);
    assert_eq!(d["results"][1]["error"], "boom");
    let updates = progress.lock().unwrap();
    assert!(updates.iter().any(|u| u.contains("1. scout [running]")));
    assert!(updates.last().unwrap().contains("3. reviewer [completed]"));
}

#[tokio::test]
async fn parallel_concurrency_bounds_children() {
    let fx = Fixture::new();
    let started = Instant::now();
    let out = fx
        .tool()
        .execute(
            json!({
                "tasks": [
                    {"agent": "scout", "task": "a sleep:300"},
                    {"agent": "scout", "task": "b sleep:300"}
                ],
                "concurrency": 1
            }),
            fx.ctx(),
        )
        .await;
    assert_eq!(details(&out)["completed"], 2);
    assert!(started.elapsed() >= Duration::from_millis(600));
}

#[tokio::test]
async fn chain_substitutes_previous_and_task() {
    let fx = Fixture::new();
    let out = fx
        .tool()
        .execute(
            json!({
                "task": "X",
                "chain": [
                    {"agent": "scout", "task": "find {task}"},
                    {"agent": "planner", "task": "plan from {previous} for {task}"}
                ]
            }),
            fx.ctx(),
        )
        .await;
    assert!(!out.is_error, "{out:?}");
    assert_eq!(out.text_content(), "done: plan from done: find X for X");
    assert_eq!(details(&out)["mode"], "chain");
    assert_eq!(details(&out)["completed"], 2);

    let out = fx
        .tool()
        .execute(
            json!({
                "chain": [
                    {"agent": "scout", "task": "noend"},
                    {"agent": "planner", "task": "never"}
                ]
            }),
            fx.ctx(),
        )
        .await;
    assert!(out.is_error);
    assert_eq!(
        out.text_content(),
        "child exited without a final assistant message"
    );
    let d = details(&out);
    assert_eq!(d["results"].as_array().unwrap().len(), 1);
    assert_eq!(d["total"], 2);
}

#[tokio::test]
async fn validation_messages() {
    let fx = Fixture::new();
    let tool = fx.tool();
    let check = |args: Value, expected: &'static str| {
        let tool = tool.clone();
        let ctx = fx.ctx();
        async move {
            let out = tool.execute(args, ctx).await;
            assert!(out.is_error);
            assert_eq!(out.text_content(), expected);
        }
    };
    check(
        json!({"bogus": 1}),
        "subagent received unsupported arguments",
    )
    .await;
    check(
        json!({"agentScope": 1}),
        "subagent agentScope must be a string",
    )
    .await;
    check(
        json!({"agentScope": "all"}),
        "agentScope must be user, project, or both",
    )
    .await;
    check(
        json!({"task": "x"}),
        "provide exactly one subagent mode: agent+task, tasks, or chain",
    )
    .await;
    check(
        json!({"agent": "scout", "task": "x", "concurrency": 2}),
        "subagent arguments from different execution modes cannot be combined",
    )
    .await;
    check(
        json!({"tasks": [{"agent": "scout", "task": "x"}], "concurrency": 0}),
        "subagent concurrency must be at least 1",
    )
    .await;
    check(
        json!({"tasks": [{"agent": "scout"}]}),
        "each subagent task requires agent and task strings and optional cwd/model",
    )
    .await;
    check(
        json!({"agent": "scout", "task": " "}),
        "single subagent mode requires non-empty string agent and task",
    )
    .await;
    check(
        json!({"agent": "nobody", "task": "x"}),
        "unknown subagent 'nobody'. Available: planner, reviewer, scout, worker",
    )
    .await;
    let nine: Vec<Value> = (0..9)
        .map(|_| json!({"agent": "scout", "task": "x"}))
        .collect();
    check(
        json!({"tasks": nine}),
        "too many parallel subagent tasks (max 8)",
    )
    .await;
    check(
        json!({"action": "list", "agent": "x"}),
        "subagent action=list accepts only agentScope",
    )
    .await;
    check(json!({"action": "explode"}), "unsupported subagent action").await;
    check(
        json!({"action": "spawn", "agent": "scout", "task": "t"}),
        "persistent subagent coordinator is unavailable",
    )
    .await;
    assert!(fx.calls().is_empty());
}

#[tokio::test]
async fn cwd_outside_workspace_fails_the_child() {
    let fx = Fixture::new();
    let out = fx
        .tool()
        .execute(
            json!({"agent": "scout", "task": "x", "cwd": ".."}),
            fx.ctx(),
        )
        .await;
    assert!(out.is_error);
    assert_eq!(out.text_content(), "subagent cwd escapes workspace root");
    assert!(fx.calls().is_empty());
}

#[tokio::test]
async fn depth_limit_is_enforced() {
    let fx = Fixture::new();
    let tool = SubagentTool::new(fx.options().with_depth(2, 2));
    let out = tool
        .execute(json!({"agent": "scout", "task": "x"}), fx.ctx())
        .await;
    assert!(out.is_error);
    assert_eq!(
        out.text_content(),
        "subagent depth limit reached at depth 2 (max 2)"
    );
    assert_eq!(details(&out)["status"], "permission_denied");
    // Listing still works at the limit.
    let out = tool.execute(json!({"action": "list"}), fx.ctx()).await;
    assert!(!out.is_error);
}

#[tokio::test]
async fn project_agents_need_trust() {
    let fx = Fixture::new();
    let file = fx.workspace.join(".pi/agents/local.md");
    write(
        &file,
        "---\nname: local\ndescription: Repo role\n---\nLocal.\n",
    );
    let tool = fx.tool();
    let out = tool
        .execute(json!({"agent": "local", "task": "x"}), fx.ctx())
        .await;
    assert_eq!(
        out.text_content(),
        "unknown subagent 'local'. Available: planner, reviewer, scout, worker"
    );
    let out = tool
        .execute(
            json!({"agent": "local", "task": "x", "agentScope": "both"}),
            fx.ctx(),
        )
        .await;
    assert!(out.is_error);
    assert_eq!(
        out.text_content(),
        format!(
            "project subagent 'local' requires trust confirmation. Inspect {} and retry with confirmProjectAgents=false only for a trusted repository.",
            file.display()
        )
    );
    let out = tool
        .execute(
            json!({"agent": "local", "task": "x", "agentScope": "project", "confirmProjectAgents": false}),
            fx.ctx(),
        )
        .await;
    assert!(!out.is_error, "{out:?}");
    assert_eq!(details(&out)["results"][0]["agent_source"], "project");
    assert_eq!(details(&out)["agent_scope"], "project");
}

#[tokio::test]
async fn list_action_reports_agents() {
    let fx = Fixture::new();
    write(&fx.home.join("agents/bad.md"), "nope");
    let out = fx
        .tool()
        .execute(json!({"action": "list", "agentScope": "user"}), fx.ctx())
        .await;
    assert!(!out.is_error);
    let d = details(&out);
    assert_eq!(d["mode"], "management");
    assert_eq!(d["action"], "list");
    assert_eq!(d["output"], "Discovered 4 subagents.");
    assert_eq!(d["agents"][0]["name"], "planner");
    assert_eq!(d["agents"][0]["source"], "builtin");
    assert_eq!(d["agents"][0]["file"], "");
    assert_eq!(d["diagnostics"].as_array().unwrap().len(), 1);
    let text: Value = serde_json::from_str(&out.text_content()).unwrap();
    assert_eq!(&text, d);
}

#[tokio::test]
async fn cancellation_kills_the_child() {
    let fx = Fixture::new();
    let ctx = fx.ctx();
    let cancel = ctx.cancel.clone();
    tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(200)).await;
        cancel.cancel();
    });
    let started = Instant::now();
    let out = fx
        .tool()
        .execute(json!({"agent": "scout", "task": "sleep:10000"}), ctx)
        .await;
    assert!(started.elapsed() < Duration::from_secs(5));
    assert!(out.is_error);
    let result = &details(&out)["results"][0];
    assert_eq!(result["cancelled"], true);
    assert_eq!(result["status"], "failed");
}

#[tokio::test]
async fn delegation_respects_the_mutation_queue() {
    let fx = Fixture::new();
    let queue = Arc::new(MutationQueue::new());
    let ctx = fx
        .ctx()
        .with_mutation_policy(MutationPolicy::Queue, queue.clone());
    let out = fx
        .tool()
        .execute(json!({"agent": "scout", "task": "x"}), ctx.clone())
        .await;
    assert!(out.is_error);
    assert_eq!(details(&out)["status"], "pending_approval");
    let id = details(&out)["mutation_id"].as_str().unwrap().to_string();
    assert_eq!(
        queue.pending()[0].summary,
        "delegate 1 task to isolated subagents"
    );
    assert!(fx.calls().is_empty());
    queue.approve(&id).unwrap();
    let out = fx
        .tool()
        .execute(
            json!({"agent": "scout", "task": "x"}),
            ctx.with_approved_mutation(id),
        )
        .await;
    assert!(!out.is_error, "{out:?}");
    assert_eq!(fx.calls().len(), 1);
}

#[tokio::test]
async fn missing_executable_is_reported() {
    let fx = Fixture::new();
    let options = fx.options().with_runner(Arc::new(ProcessChildRunner::new(
        fx.workspace.join("no-such-qt"),
    )));
    let out = SubagentTool::new(options)
        .execute(json!({"agent": "scout", "task": "x"}), fx.ctx())
        .await;
    assert!(out.is_error);
    assert!(
        out.text_content()
            .starts_with("cannot start subagent executable '")
    );
}
