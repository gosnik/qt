//! Manual end-to-end check of the Dreaming `auto_validate` verifier with a
//! real local llama.cpp model run as an isolated `qt-cli --mode json` child.
//!
//! Ignored by default; run with
//! `QT_E2E_QT_CLI=target/debug/qt-cli QT_E2E_VERIFIER_GGUF=/path/model.gguf \
//!  cargo test -p qt-runtime --test memory_verifier_e2e -- --ignored --nocapture`.

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use qt_memory::dreaming::{CancelFlag, IsolatedChild, ProcessChild};
use qt_memory::model::*;
use qt_memory::post_processing::{
    DreamArtifactRequest, DreamResearchEvidence, PostProcessingService,
};
use qt_memory::review::{ReviewGovernancePolicy, ReviewService};
use qt_memory::service::MemoryService;
use qt_memory::store::MemoryStore;
use qt_memory::verifier::{AutoValidator, VerificationSubject, VerifierConfig};
use serde_json::json;

const SOURCE: &str = "Exponential backoff with jitter is the recommended retry strategy for transient network failures.";

fn remember(store: &Arc<MemoryStore>, scope: &MemoryScope, content: &str) -> String {
    MemoryService::new(store.clone())
        .remember(
            scope,
            MemoryWrite {
                memory_type: MemoryType::Semantic,
                status: MemoryStatus::Candidate,
                content: content.into(),
                provenance: Provenance {
                    source_type: SourceType::Manual,
                    source_uri: "test://memory".into(),
                    ..Default::default()
                },
                confidence: 0.6,
                ..Default::default()
            },
        )
        .unwrap()
}

#[test]
#[ignore = "needs QT_E2E_QT_CLI and QT_E2E_VERIFIER_GGUF"]
fn verifier_runs_a_real_local_llama_child() {
    let (Ok(cli), Ok(gguf)) = (
        std::env::var("QT_E2E_QT_CLI"),
        std::env::var("QT_E2E_VERIFIER_GGUF"),
    ) else {
        eprintln!("QT_E2E_QT_CLI / QT_E2E_VERIFIER_GGUF not set; skipping");
        return;
    };
    let home = tempfile::tempdir().unwrap();
    let cwd = tempfile::tempdir().unwrap();
    std::fs::write(
        home.path().join("settings.json"),
        json!({"llama": {"models": [
            {"id": "verifier", "name": "Verifier", "path": gguf, "contextWindow": 8192}
        ]}})
        .to_string(),
    )
    .unwrap();
    let store = Arc::new(MemoryStore::open(home.path()).unwrap());
    let scope = MemoryScope {
        kind: ScopeKind::Repository,
        user: "local_user".into(),
        project: "demo".into(),
        repository: "repo_0123456789abcdef".into(),
        repository_display: "demo".into(),
        ..Default::default()
    };
    let a = remember(
        &store,
        &scope,
        "service alpha retries with exponential backoff",
    );
    let b = remember(&store, &scope, "service beta adds jitter to retries");
    let post = PostProcessingService::new(store.clone());
    let dream = post
        .remember_dream_artifact(
            &scope,
            DreamArtifactRequest {
                content: "Retries should use exponential backoff with jitter".into(),
                source_memory_ids: vec![a.clone(), b.clone()],
                ..Default::default()
            },
        )
        .unwrap();
    let researched = post
        .remember_researched_successor(
            &scope,
            &dream.id,
            DreamResearchEvidence {
                summary: "Exponential backoff with jitter is the recommended retry strategy for transient network failures.".into(),
                source_memory_ids: vec![a, b],
                provenance: Provenance {
                    source_type: SourceType::ToolResult,
                    source_uri: "https://docs.example/retry".into(),
                    source_hash: "evidencehash".into(),
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap();
    let research_id = researched.researched_memory_id.unwrap();
    let review_id = researched.research_review_id.unwrap();
    let documents = json!([{"url": "https://docs.example/retry", "content": SOURCE}]);
    let child: Arc<dyn IsolatedChild> = Arc::new(ProcessChild::new(PathBuf::from(cli)));
    let validator = AutoValidator::new(
        store.clone(),
        ReviewService::new(
            store.clone(),
            ReviewGovernancePolicy {
                allow_dream_hypothesis_promotion: true,
            },
        ),
        VerifierConfig {
            enabled: true,
            verifier_model: Some("llama/verifier".into()),
            max_model_input_tokens: 6000,
            timeout: Duration::from_secs(600),
            discovery_roots: Vec::new(),
            cwd: Some(cwd.path().to_path_buf()),
            agent_home: Some(home.path().to_path_buf()),
        },
        child,
        CancelFlag::new(),
    );
    let receipt = validator.validate(&VerificationSubject {
        scope: &scope,
        memory_id: &research_id,
        review_id: &review_id,
        documents: &documents,
        evidence_hash: "evidencehash",
        dream_model: "ollama:local/any",
    });
    eprintln!("verifier receipt: {receipt}");
    assert_eq!(receipt["attempted"], true, "{receipt}");
    assert_eq!(receipt["model"], "llama/verifier");
    let reason = receipt["reason"].as_str().unwrap_or_default();
    assert!(
        !matches!(
            reason,
            "verifier_start_failure" | "verifier_failure" | "verifier_timeout"
        ),
        "the local llama child must run to completion: {receipt}"
    );
    let research = store.get(&scope, &research_id).unwrap();
    if receipt["validated"] == true {
        assert_eq!(research.status, MemoryStatus::Validated);
    } else {
        assert_eq!(research.status, MemoryStatus::Candidate);
    }
}
