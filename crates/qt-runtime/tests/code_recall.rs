//! Code recall end to end: a project index persisted by one process feeds
//! memory recall in a fresh process (archived `load_persisted_snapshot` and
//! `BackgroundService::retrieval_search`), without a model or network.

use std::path::Path;
use std::sync::Arc;

use qt_memory::code::CodeSnapshotProvider;
use qt_memory::settings::MemorySettings;
use qt_memory::{MemoryRuntime, RecallRequest};
use qt_runtime::code::{CODE_RETRIEVAL_UNAVAILABLE, IndexCodeSnapshots};
use qt_runtime::memory::{MemoryInputs, memory_runtime_options};
use qt_runtime::qt_index::IndexManager;

const SOURCE: &str = "#include <stdint.h>\n\
/* Rolling checksum used by the firmware updater. */\n\
uint32_t compute_checksum(const uint8_t *data, int len) {\n\
  uint32_t sum = 0;\n\
  for (int i = 0; i < len; ++i) sum = (sum << 1) ^ data[i];\n\
  return sum;\n\
}\n\
int verify_image(const uint8_t *data, int len, uint32_t expected) {\n\
  return compute_checksum(data, len) == expected;\n\
}\n";

fn runtime(home: &Path, work: &Path, code: Arc<dyn CodeSnapshotProvider>) -> MemoryRuntime {
    let mut settings = MemorySettings {
        enabled: true,
        ..Default::default()
    };
    settings.retrieval.code_enabled = true;
    settings.retrieval.graph_enabled = true;
    let options = memory_runtime_options(
        &settings,
        &MemoryInputs {
            agent_home: home.to_path_buf(),
            cwd: work.to_path_buf(),
            code: Some(code),
            ..Default::default()
        },
    );
    MemoryRuntime::new(options).unwrap()
}

#[test]
fn persisted_index_feeds_code_recall_in_a_fresh_process() {
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("home");
    let work = dir.path().join("work");
    std::fs::create_dir_all(work.join("src")).unwrap();
    std::fs::create_dir_all(&home).unwrap();
    std::fs::write(work.join("src/checksum.c"), SOURCE).unwrap();

    // Without a persisted snapshot code recall is unavailable and says so.
    let empty = IndexCodeSnapshots::new(IndexManager::new(&home).service_for(&work));
    assert!(empty.snapshot().is_none());
    assert_eq!(
        empty.unavailable_reason().as_deref(),
        Some(CODE_RETRIEVAL_UNAVAILABLE)
    );
    assert!(empty.lexical("compute_checksum", 10).is_empty());

    // First process: scan and persist.
    {
        let manager = IndexManager::new(&home);
        let service = manager.service_for(&work);
        service.run_once();
        let status = service.status();
        assert!(status.generation > 0, "{}", status.last_error);
        assert!(!status.index_stale);
    }

    // Fresh process: the snapshot is loaded from disk (the index reports it
    // stale until it rescans) and is usable for recall right away.
    let manager = IndexManager::new(&home);
    let service = manager.service_for(&work);
    assert!(service.status().snapshot_loaded && service.status().index_stale);
    let code = Arc::new(IndexCodeSnapshots::new(service));
    let snapshot = code.snapshot().expect("snapshot from the persisted index");
    assert!(!snapshot.stale);
    assert!(code.unavailable_reason().is_none());

    // Exact symbol names come first, then file nodes of full-text matches.
    let exact = code.lexical("compute_checksum", 10);
    assert!(!exact.is_empty());
    let function = snapshot
        .nodes
        .iter()
        .find(|node| node.id == exact[0].id)
        .unwrap();
    assert_eq!(function.name, "compute_checksum");
    let free_text = code.lexical("how is the firmware checksum computed?", 10);
    assert_eq!(free_text.len(), 1, "{free_text:?}");
    assert_eq!(free_text[0].path, "src/checksum.c");
    assert!(code.lexical("nonexistent_identifier_xyz", 10).is_empty());
    assert!(code.lexical("compute_checksum", 0).is_empty());

    // Memory recall explains the code hits with their retrievers.
    let memory = runtime(&home, &work, code.clone());
    let preview = memory
        .preview_retrieval(
            RecallRequest {
                query: "how is the firmware checksum computed".into(),
                ..Default::default()
            },
            900,
        )
        .unwrap();
    assert!(!preview.code.is_empty());
    let retrievers: Vec<&str> = preview
        .code
        .iter()
        .flat_map(|candidate| candidate.ranks.iter().map(|rank| rank.retriever.as_str()))
        .collect();
    assert!(retrievers.contains(&"code_bm25"), "{retrievers:?}");
    assert!(!preview.context.code_ids.is_empty());
    assert!(
        preview
            .context
            .markdown
            .contains("uint32_t compute_checksum(")
    );
    assert!(memory.retrieval_status().code.available);

    // A changed file is never served from the old snapshot.
    std::fs::write(work.join("src/checksum.c"), "int changed;\n").unwrap();
    assert!(code.lexical("compute_checksum", 10).is_empty());
    assert!(code.lexical("firmware checksum", 10).is_empty());
}
