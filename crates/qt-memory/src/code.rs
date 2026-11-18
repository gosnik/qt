//! Read-only code-index snapshot consumed by code recall (archived
//! `index::RetrievalSnapshot`).
//!
//! The project index (qt-index) builds a [`CodeSnapshot`] for one published
//! generation and supplies it through a [`CodeSnapshotProvider`]. Memory never
//! reads index storage directly: every code owner is validated against the
//! snapshot and the current file fingerprint before it can reach context.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use crate::retrieval::{RetrievalOwnerKind, RetrievalReference};

/// Files larger than this are not fingerprinted (`"unindexed"`).
pub const MAX_CHUNK_SOURCE_BYTES: u64 = 1024 * 1024;

/// Kind of a code node.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CodeNodeKind {
    Project,
    File,
    Class,
    Method,
    Function,
}

impl CodeNodeKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Project => "project",
            Self::File => "file",
            Self::Class => "class",
            Self::Method => "method",
            Self::Function => "function",
        }
    }
}

/// One symbol or file of the snapshot.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodeNode {
    pub id: String,
    pub repository: String,
    pub worktree: String,
    /// Path relative to [`CodeSnapshot::root`] (`/`-separated).
    pub path: String,
    pub kind: CodeNodeKind,
    pub name: String,
    pub qualified_name: String,
    pub signature_discriminator: String,
    pub source_fingerprint: String,
    pub generation: u64,
    pub line: usize,
}

/// A bounded source window owned by one node.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodeChunk {
    pub id: String,
    pub owner_id: String,
    pub path: String,
    pub text: String,
    pub first_line: usize,
    pub last_line: usize,
    pub source_fingerprint: String,
    pub generation: u64,
    pub bounded_source_window: bool,
}

/// A (possibly heuristic or ambiguous) relation between code nodes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodeEdge {
    pub id: String,
    pub caller_id: String,
    pub callee_ids: Vec<String>,
    pub unresolved_name: String,
    pub path: String,
    pub line: usize,
    pub heuristic: bool,
    pub ambiguous: bool,
    pub relation: String,
}

/// A source window that still matches the file on disk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CodeSnippet {
    pub chunk: CodeChunk,
    pub current: bool,
}

/// An immutable view of one code-index generation.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct CodeSnapshot {
    pub root: PathBuf,
    pub generation: u64,
    pub stale: bool,
    pub nodes: Vec<CodeNode>,
    pub chunks: Vec<CodeChunk>,
    pub edges: Vec<CodeEdge>,
}

impl CodeSnapshot {
    /// Nodes whose name or qualified name equals `query`, optionally limited to
    /// one path. Callers must handle zero and several matches.
    pub fn find_symbols(&self, query: &str, path: Option<&str>) -> Vec<CodeNode> {
        self.nodes
            .iter()
            .filter(|node| path.is_none_or(|path| node.path == path))
            .filter(|node| node.name == query || node.qualified_name == query)
            .cloned()
            .collect()
    }

    /// The node's source window, only when the file still has the fingerprint
    /// the snapshot recorded.
    pub fn read_snippet(&self, node_id: &str) -> Option<CodeSnippet> {
        if self.stale {
            return None;
        }
        let node = self.nodes.iter().find(|node| node.id == node_id)?;
        let path = self.root.join(&node.path);
        if !path.is_file() || source_fingerprint(&path) != node.source_fingerprint {
            return None;
        }
        let chunk = self.chunks.iter().find(|chunk| chunk.owner_id == node_id)?;
        Some(CodeSnippet {
            chunk: chunk.clone(),
            current: true,
        })
    }

    /// A pinned reference to `node`.
    pub fn reference(node: &CodeNode) -> RetrievalReference {
        RetrievalReference {
            kind: RetrievalOwnerKind::Code,
            id: node.id.clone(),
            revision: 0,
            repository: node.repository.clone(),
            worktree: node.worktree.clone(),
            path: node.path.clone(),
            source_fingerprint: node.source_fingerprint.clone(),
            snapshot_generation: node.generation,
        }
    }
}

/// Supplies the current code snapshot and its lexical index.
pub trait CodeSnapshotProvider: Send + Sync {
    /// The latest published snapshot, if any.
    fn snapshot(&self) -> Option<Arc<CodeSnapshot>>;

    /// Why code retrieval is unavailable (no usable snapshot), reported as a
    /// retrieval warning by `qt memory explain`.
    fn unavailable_reason(&self) -> Option<String> {
        None
    }

    /// Lexical (BM25) matches for `query`, best first. The default matches
    /// symbol names exactly, ordered by node id.
    fn lexical(&self, query: &str, limit: usize) -> Vec<RetrievalReference> {
        let Some(snapshot) = self.snapshot() else {
            return Vec::new();
        };
        let mut nodes = snapshot.find_symbols(query, None);
        nodes.sort_by(|left, right| left.id.cmp(&right.id));
        nodes
            .iter()
            .filter(|node| {
                source_fingerprint(&snapshot.root.join(&node.path)) == node.source_fingerprint
            })
            .take(limit)
            .map(CodeSnapshot::reference)
            .collect()
    }
}

/// `fnv1a64:<16 hex>` of a text.
pub fn fingerprint_text(text: &[u8]) -> String {
    let mut hash: u64 = 14_695_981_039_346_656_037;
    for byte in text {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(1_099_511_628_211);
    }
    format!("fnv1a64:{hash:016x}")
}

/// Current fingerprint of a source file: `missing`, `unindexed` (over 1 MiB),
/// `unreadable`, `changed` (size changed while reading) or `fnv1a64:<hex>`.
pub fn source_fingerprint(path: &Path) -> String {
    let Ok(metadata) = std::fs::metadata(path) else {
        return "missing".into();
    };
    if !metadata.is_file() {
        return "missing".into();
    }
    if metadata.len() > MAX_CHUNK_SOURCE_BYTES {
        return "unindexed".into();
    }
    match std::fs::read(path) {
        Ok(bytes) if bytes.len() as u64 == metadata.len() => fingerprint_text(&bytes),
        Ok(_) => "changed".into(),
        Err(_) => "unreadable".into(),
    }
}

/// Stable node identity: `code:` plus the hex of the fingerprint of
/// worktree, path, kind, qualified name and signature discriminator.
pub fn node_id(
    worktree: &str,
    path: &str,
    kind: CodeNodeKind,
    qualified_name: &str,
    signature_discriminator: &str,
) -> String {
    let key = format!(
        "{worktree}\n{path}\n{}\n{qualified_name}\n{signature_discriminator}",
        kind.as_str()
    );
    format!("code:{}", &fingerprint_text(key.as_bytes())[8..])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fingerprints_follow_archived_format() {
        assert_eq!(fingerprint_text(b""), "fnv1a64:cbf29ce484222325");
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("a.rs");
        assert_eq!(source_fingerprint(&file), "missing");
        std::fs::write(&file, "a").unwrap();
        assert_eq!(source_fingerprint(&file), "fnv1a64:af63dc4c8601ec8c");
        assert_eq!(
            node_id("w", "a.rs", CodeNodeKind::File, "a.rs", ""),
            format!("code:{}", &fingerprint_text(b"w\na.rs\nfile\na.rs\n")[8..])
        );
    }

    #[test]
    fn snippets_require_current_fingerprint() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("a.rs"), "fn a() {}\n").unwrap();
        let fingerprint = source_fingerprint(&dir.path().join("a.rs"));
        let node = CodeNode {
            id: "code:a".into(),
            repository: "r".into(),
            worktree: "w".into(),
            path: "a.rs".into(),
            kind: CodeNodeKind::Function,
            name: "a".into(),
            qualified_name: "a".into(),
            signature_discriminator: String::new(),
            source_fingerprint: fingerprint.clone(),
            generation: 1,
            line: 1,
        };
        let snapshot = CodeSnapshot {
            root: dir.path().into(),
            generation: 1,
            stale: false,
            nodes: vec![node],
            chunks: vec![CodeChunk {
                id: "chunk".into(),
                owner_id: "code:a".into(),
                path: "a.rs".into(),
                text: "fn a() {}\n".into(),
                first_line: 1,
                last_line: 1,
                source_fingerprint: fingerprint,
                generation: 1,
                bounded_source_window: true,
            }],
            edges: Vec::new(),
        };
        assert!(snapshot.read_snippet("code:a").is_some());
        assert_eq!(snapshot.find_symbols("a", None).len(), 1);
        std::fs::write(dir.path().join("a.rs"), "fn b() {}\n").unwrap();
        assert!(snapshot.read_snippet("code:a").is_none());
    }
}
