#![allow(dead_code)]

use std::sync::Arc;

use qt_memory::*;

pub fn exact_scope(project: &str, repository: &str, session: &str) -> MemoryScope {
    MemoryScope {
        kind: ScopeKind::Exact,
        user: "local_user".into(),
        project: project.into(),
        repository: repository.into(),
        repository_display: project.into(),
        branch: "main".into(),
        session: session.into(),
        ..Default::default()
    }
}

pub fn scope() -> MemoryScope {
    exact_scope("alpha", "repo_alpha", "session-1")
}

pub struct Fixture {
    pub dir: tempfile::TempDir,
    pub store: Arc<MemoryStore>,
}

impl Fixture {
    pub fn new() -> Self {
        let dir = tempfile::tempdir().unwrap();
        let store = Arc::new(MemoryStore::open(dir.path()).unwrap());
        Self { dir, store }
    }

    pub fn service(&self) -> MemoryService {
        MemoryService::new(Arc::clone(&self.store))
    }

    pub fn remember(&self, scope: &MemoryScope, content: &str, status: MemoryStatus) -> String {
        self.store
            .add(
                scope,
                MemoryWrite {
                    content: content.into(),
                    status,
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
            .unwrap()
    }

    pub fn raw(&self) -> rusqlite::Connection {
        rusqlite::Connection::open(self.store.path()).unwrap()
    }
}
