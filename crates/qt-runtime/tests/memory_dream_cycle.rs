//! The full `auto_validate` Dreaming cycle through real child processes:
//! Dream child -> restricted research child -> verifier child, as archived
//! (`researchPolicy = required`, `promotionPolicy = auto_validate`).
//!
//! The research child's web tools only reach public hosts, so no test here
//! runs the real research child: a local stub process stands in for it and
//! reports the `web_search` / `web_fetch` events and final answer a research
//! child prints in JSON mode. Nothing contacts the network.
//!
//! * `stubbed_children_complete_the_auto_validate_cycle` runs every child as
//!   the stub (needs `python3`, Unix only): it checks process spawning, the
//!   child arguments and markers, JSON-mode parsing, citation validation and
//!   the atomic validation of only the researched successor.
//! * `real_dream_and_verifier_children_complete_the_cycle` (ignored) runs the
//!   Dream and verifier children as a real `qt-cli` with an in-process
//!   llama.cpp model and only the research child as the stub:
//!   `QT_E2E_QT_CLI=<llama-enabled qt-cli> QT_E2E_DREAM_GGUF=<model.gguf>
//!   cargo test -p qt-runtime --test memory_dream_cycle -- --ignored --nocapture`.

#![cfg(unix)]

use std::path::{Path, PathBuf};
use std::sync::Arc;

use qt_memory::dreaming::{
    ChildOutcome, ChildRequest, ChildTask, DreamTrigger, DreamingConfig, DreamingService,
    IsolatedChild, ProcessChild,
};
use qt_memory::model::*;
use qt_memory::service::MemoryService;
use qt_memory::settings::MemorySettings;
use qt_memory::store::MemoryStore;
use serde_json::{Value, json};

const STUB: &str = r#"#!/usr/bin/env python3
import json, os, sys

URL = "https://docs.example/retry"
SOURCE = ("Exponential backoff with jitter is the recommended retry strategy "
          "for transient network failures.")
raw = sys.stdin.read()
payload = json.loads(raw) if raw.strip() else {}

def flag(name):
    return os.environ.get(name, "").strip().lower() not in ("", "0", "false", "no", "off")

record = os.environ.get("QT_E2E_STUB_RECORD")
if record:
    with open(record, "a") as out:
        out.write(json.dumps({"argv": sys.argv[1:], "input": payload,
                              "research": flag("QT_MEMORY_DREAM_RESEARCH"),
                              "verify": flag("QT_MEMORY_DREAM_VERIFY"),
                              "dreaming": flag("QT_MEMORY_DREAMING"),
                              "offline": flag("PI_OFFLINE")}) + "\n")

def emit(event):
    print(json.dumps(event), flush=True)

def finish(answer):
    emit({"type": "agent_end", "messages": [
        {"role": "user", "content": [{"type": "text", "text": "input"}]},
        {"role": "assistant", "content": [{"type": "text", "text": json.dumps(answer)}]}]})

if flag("QT_MEMORY_DREAM_VERIFY"):
    finish({"version": 1, "verdict": "supported",
            "summary_hash": payload["summary_hash"], "evidence_hash": payload["evidence_hash"],
            "rationale": "The source states it directly.",
            "citations": [{"url": URL, "quote": "recommended retry strategy for transient network failures"}]})
elif flag("QT_MEMORY_DREAM_RESEARCH"):
    emit({"type": "tool_execution_start", "toolName": "web_search"})
    emit({"type": "tool_execution_end", "toolName": "web_search", "isError": False,
          "result": {"details": {"results": [{"title": "Retry guidance", "url": URL}]}}})
    emit({"type": "tool_execution_start", "toolName": "web_fetch"})
    emit({"type": "tool_execution_end", "toolName": "web_fetch", "isError": False,
          "result": {"details": {"url": URL, "status": 200, "content": SOURCE}}})
    finish({"version": 1,
            "summary": "Exponential backoff with jitter is the recommended retry strategy for transient network failures.",
            "source_urls": [URL], "confidence": 0.7})
elif flag("QT_MEMORY_DREAMING"):
    ids = [candidate["id"] for candidate in payload["candidates"][:2]]
    finish({"version": 1, "source_memory_ids": ids, "artifact_kind": "pattern_discovery",
            "content": "Both services retry transient failures with exponential backoff.",
            "confidence": 0.8, "research_required": True})
else:
    sys.exit(2)
"#;

fn python3_available() -> bool {
    std::process::Command::new("python3")
        .arg("--version")
        .output()
        .is_ok_and(|output| output.status.success())
}

/// Writes the stub child and returns its path.
fn write_stub(dir: &Path) -> PathBuf {
    use std::os::unix::fs::PermissionsExt;
    let path = dir.join("research-stub.py");
    std::fs::write(&path, STUB).unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)).unwrap();
    path
}

/// Real children for every task except research, which the stub answers.
struct RoutedChild {
    main: ProcessChild,
    research: ProcessChild,
}

impl IsolatedChild for RoutedChild {
    fn run(&self, request: &ChildRequest) -> Result<ChildOutcome, String> {
        eprintln!("child {:?}: {:?}", request.task, request.cli_arguments());
        let outcome = match request.task {
            ChildTask::DreamResearch => self.research.run(request),
            _ => self.main.run(request),
        };
        if let Ok(outcome) = &outcome {
            eprintln!(
                "child {:?} exit={:?} output={}",
                request.task, outcome.exit, outcome.final_output
            );
        }
        outcome
    }
}

fn scope() -> MemoryScope {
    MemoryScope {
        kind: ScopeKind::Repository,
        user: "local_user".into(),
        project: "demo".into(),
        repository: "repo_0123456789abcdef".into(),
        repository_display: "demo".into(),
        ..Default::default()
    }
}

fn seed(store: &Arc<MemoryStore>, scope: &MemoryScope) {
    for content in [
        "Service alpha retries failed HTTP calls with exponential backoff.",
        "Service beta retries transient network failures with exponential backoff and jitter.",
        "Service gamma caps retries at five attempts.",
    ] {
        MemoryService::new(store.clone())
            .remember(
                scope,
                MemoryWrite {
                    memory_type: MemoryType::Semantic,
                    status: MemoryStatus::Validated,
                    content: content.into(),
                    provenance: Provenance {
                        source_type: SourceType::Manual,
                        source_uri: "test://memory".into(),
                        ..Default::default()
                    },
                    confidence: 0.8,
                    ..Default::default()
                },
            )
            .unwrap();
    }
}

fn dreaming(
    home: &Path,
    cwd: &Path,
    model: &str,
    child: Arc<dyn IsolatedChild>,
) -> (Arc<MemoryStore>, MemoryScope, DreamingService) {
    let store = Arc::new(MemoryStore::open(home).unwrap());
    let scope = scope();
    seed(&store, &scope);
    let mut settings = MemorySettings::default().dreaming;
    settings.enabled = true;
    settings.trigger = "manual".into();
    settings.model = Some(model.into());
    settings.research_policy = "required".into();
    settings.promotion_policy = "auto_validate".into();
    settings.timeout_ms = 900_000;
    let mut config = DreamingConfig::new(settings);
    config.agent_home = Some(home.to_path_buf());
    config.cwd = cwd.to_string_lossy().into_owned();
    let service = DreamingService::new(store.clone(), config, scope.clone(), Some(child));
    (store, scope, service)
}

/// Runs one manual cycle and checks the archived `auto_validate` outcome:
/// the Dream stays a pending artifact and only the researched successor is
/// validated, by `memory_dream_verifier`.
fn run_and_check(
    store: &Arc<MemoryStore>,
    scope: &MemoryScope,
    service: &DreamingService,
) -> Value {
    assert!(service.enqueue(DreamTrigger::Manual, None));
    let report = service.run_ready().expect("a queued manual cycle");
    let receipt = report.receipt.clone().expect("a cycle receipt");
    eprintln!("receipt: {receipt}");
    assert!(report.success, "{report:?}");
    assert_eq!(receipt["research_attempted"], true, "{receipt}");
    assert_eq!(receipt["research_completed"], true, "{receipt}");
    assert_eq!(receipt["research_failure_reason"], "", "{receipt}");
    let dream = store.get(scope, &report.dream_id).unwrap();
    assert_eq!(dream.status, MemoryStatus::DreamArtifact);
    assert!(dream.tags.iter().any(|tag| tag == "research:pending"));
    let research_id = receipt["research_id"].as_str().unwrap().to_string();
    let research = store.get(scope, &research_id).unwrap();
    assert_eq!(research.provenance.source_uri, "https://docs.example/retry");
    assert!(research.tags.iter().any(|tag| tag == "research:web"));
    let validation = &receipt["auto_validation"];
    assert_eq!(validation["attempted"], true, "{receipt}");
    if validation["validated"] == true {
        assert_eq!(research.status, MemoryStatus::Validated);
    } else {
        assert_eq!(research.status, MemoryStatus::Candidate, "{receipt}");
    }
    receipt
}

#[test]
fn stubbed_children_complete_the_auto_validate_cycle() {
    if !python3_available() {
        eprintln!("python3 is not available; skipping");
        return;
    }
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("home");
    let cwd = dir.path().join("work");
    std::fs::create_dir_all(&home).unwrap();
    std::fs::create_dir_all(&cwd).unwrap();
    let stub = write_stub(dir.path());
    let record = dir.path().join("children.jsonl");
    // SAFETY: set before any child starts; only this test reads it.
    unsafe { std::env::set_var("QT_E2E_STUB_RECORD", &record) };
    let child: Arc<dyn IsolatedChild> = Arc::new(ProcessChild::new(&stub));
    let (store, scope, service) = dreaming(&home, &cwd, "llama/dreamer", child);
    let receipt = run_and_check(&store, &scope, &service);
    assert_eq!(receipt["auto_validation"]["validated"], true, "{receipt}");
    assert_eq!(receipt["auto_validation"]["model"], "llama/dreamer");

    let children: Vec<Value> = std::fs::read_to_string(&record)
        .unwrap()
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect();
    assert_eq!(children.len(), 3, "{children:?}");
    let argv = |index: usize| -> Vec<String> {
        children[index]["argv"]
            .as_array()
            .unwrap()
            .iter()
            .map(|value| value.as_str().unwrap().to_string())
            .collect()
    };
    // Dream: no tools, the configured model.
    assert_eq!(children[0]["dreaming"], true);
    assert!(argv(0).contains(&"--no-tools".to_string()));
    // Research: only the three web tools and the Dream as input.
    assert_eq!(children[1]["research"], true);
    let research_argv = argv(1);
    let tools = research_argv
        .iter()
        .position(|arg| arg == "--tools")
        .unwrap();
    assert_eq!(research_argv[tools + 1], "web_run,web_search,web_fetch");
    assert!(research_argv.contains(&"--no-memory".to_string()));
    assert_eq!(children[1]["input"]["dream_id"], receipt["dream_id"]);
    // Verifier: offline, no tools, the local llama provider.
    assert_eq!(children[2]["verify"], true);
    assert_eq!(children[2]["offline"], true);
    let verify_argv = argv(2);
    assert!(verify_argv.contains(&"--no-tools".to_string()));
    assert!(
        verify_argv
            .windows(2)
            .any(|w| w[0] == "--provider" && w[1] == "llama")
    );
    assert_eq!(
        children[2]["input"]["documents"][0]["url"],
        "https://docs.example/retry"
    );
    let research = store
        .get(&scope, receipt["research_id"].as_str().unwrap())
        .unwrap();
    assert_eq!(research.status, MemoryStatus::Validated);
}

#[test]
#[ignore = "needs QT_E2E_QT_CLI (llama-enabled) and QT_E2E_DREAM_GGUF"]
fn real_dream_and_verifier_children_complete_the_cycle() {
    let (Ok(cli), Ok(gguf)) = (
        std::env::var("QT_E2E_QT_CLI"),
        std::env::var("QT_E2E_DREAM_GGUF"),
    ) else {
        eprintln!("QT_E2E_QT_CLI / QT_E2E_DREAM_GGUF not set; skipping");
        return;
    };
    assert!(python3_available(), "the research stub needs python3");
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("home");
    let cwd = dir.path().join("work");
    std::fs::create_dir_all(&home).unwrap();
    std::fs::create_dir_all(&cwd).unwrap();
    std::fs::write(
        home.join("settings.json"),
        json!({"llama": {"models": [
            {"id": "dreamer", "name": "Dreamer", "path": gguf, "contextWindow": 8192}
        ]}})
        .to_string(),
    )
    .unwrap();
    let child: Arc<dyn IsolatedChild> = Arc::new(RoutedChild {
        main: ProcessChild::new(PathBuf::from(cli)),
        research: ProcessChild::new(write_stub(dir.path())),
    });
    let (store, scope, service) = dreaming(&home, &cwd, "llama/dreamer", child);
    let started = std::time::Instant::now();
    let receipt = run_and_check(&store, &scope, &service);
    eprintln!("cycle took {:?}", started.elapsed());
    let reason = receipt["auto_validation"]["reason"]
        .as_str()
        .unwrap_or_default();
    assert!(
        !matches!(
            reason,
            "verifier_start_failure" | "verifier_failure" | "verifier_timeout"
        ),
        "the local llama verifier must run to completion: {receipt}"
    );
}
