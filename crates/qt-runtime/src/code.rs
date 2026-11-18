//! Code recall over the project index: a [`CodeSnapshotProvider`] that
//! projects the index service's scan and code index (C/C++, Rust,
//! TypeScript/JavaScript) into a memory
//! [`CodeSnapshot`] (archived `index::RetrievalSnapshot::build`).
//!
//! Node ids, the 48-line file windows, the 16-before / 32-after symbol
//! windows, signature discriminators, `contains_file` / `defines` / `calls`
//! edges and ambiguity marking follow the archived builder. Snapshots are
//! rebuilt only when the index generation changes.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use qt_index::BackgroundService;
use qt_index::code_index::CodeIndex;
use qt_index::scan::{ProjectFileKind, ProjectScanResult};
use qt_memory::code::{
    CodeChunk, CodeEdge, CodeNode, CodeNodeKind, CodeSnapshot, CodeSnapshotProvider,
    MAX_CHUNK_SOURCE_BYTES, fingerprint_text, node_id, source_fingerprint,
};
use qt_memory::retrieval::RetrievalReference;

/// Window of at most this many bytes per chunk (archived `line_window`).
const MAX_WINDOW_BYTES: usize = 32768;

fn line_window(source: &str, first: usize, last: usize) -> String {
    let first = first.max(1);
    let mut out = String::new();
    for (index, line) in source.lines().enumerate() {
        let number = index + 1;
        if number > last {
            break;
        }
        if number >= first {
            if out.len() + line.len() + 1 > MAX_WINDOW_BYTES {
                break;
            }
            out.push_str(line);
            out.push('\n');
        }
    }
    out
}

fn generic(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

fn read_bounded(path: &Path) -> String {
    match std::fs::metadata(path) {
        Ok(meta) if meta.len() <= MAX_CHUNK_SOURCE_BYTES => {
            std::fs::read(path).map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
        }
        _ => return String::new(),
    }
    .unwrap_or_default()
}

/// Build the snapshot for one index generation.
pub fn build_snapshot(
    scan: &ProjectScanResult,
    index: &CodeIndex,
    generation: u64,
    fingerprints: &HashMap<String, String>,
    stale: bool,
) -> CodeSnapshot {
    let root = scan.root.clone();
    let worktree = generic(&root);
    let mut snapshot = CodeSnapshot {
        root: root.clone(),
        generation,
        stale,
        ..Default::default()
    };
    let project_id = node_id(&worktree, "", CodeNodeKind::Project, &worktree, "project");
    snapshot.nodes.push(CodeNode {
        id: project_id.clone(),
        repository: worktree.clone(),
        worktree: worktree.clone(),
        path: String::new(),
        kind: CodeNodeKind::Project,
        name: root
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default(),
        qualified_name: worktree.clone(),
        signature_discriminator: String::new(),
        source_fingerprint: String::new(),
        generation,
        line: 0,
    });
    let mut source: HashMap<String, String> = HashMap::new();
    let mut file_ids: HashMap<String, String> = HashMap::new();
    for file in &scan.files {
        let key = generic(&file.path);
        let full = root.join(&file.path);
        let extension = file
            .path
            .extension()
            .map(|e| e.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        let source_kind =
            file.kind != ProjectFileKind::Other || extension == "md" || extension == "txt";
        let text = if source_kind {
            read_bounded(&full)
        } else {
            String::new()
        };
        let current = fingerprint_text(text.as_bytes());
        let recorded = fingerprints.get(&key).cloned();
        let fingerprint = recorded.clone().unwrap_or_else(|| current.clone());
        let id = node_id(&worktree, &key, CodeNodeKind::File, &key, "file");
        snapshot.nodes.push(CodeNode {
            id: id.clone(),
            repository: worktree.clone(),
            worktree: worktree.clone(),
            path: key.clone(),
            kind: CodeNodeKind::File,
            name: file
                .path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default(),
            qualified_name: key.clone(),
            signature_discriminator: "file".into(),
            source_fingerprint: fingerprint.clone(),
            generation,
            line: 0,
        });
        snapshot.edges.push(CodeEdge {
            id: format!("contains:{project_id}:{id}"),
            caller_id: project_id.clone(),
            callee_ids: vec![id.clone()],
            unresolved_name: String::new(),
            path: key.clone(),
            line: 0,
            heuristic: false,
            ambiguous: false,
            relation: "contains_file".into(),
        });
        let unchanged = recorded.is_none_or(|r| r == current);
        if source_kind && !text.is_empty() && unchanged {
            snapshot.chunks.push(CodeChunk {
                id: format!("{id}:chunk:0"),
                owner_id: id.clone(),
                path: key.clone(),
                text: line_window(&text, 1, 48),
                first_line: 1,
                last_line: 48,
                source_fingerprint: fingerprint,
                generation,
                bounded_source_window: true,
            });
        }
        file_ids.insert(key.clone(), id);
        source.insert(key, text);
    }

    let mut symbol_ids: HashMap<String, Vec<String>> = HashMap::new();
    let add_symbol = |snapshot: &mut CodeSnapshot,
                      path: &Path,
                      kind: CodeNodeKind,
                      name: &str,
                      qualified: &str,
                      line: usize|
     -> Option<String> {
        let key = generic(path);
        let text = source.get(&key).cloned().unwrap_or_default();
        let current = fingerprint_text(text.as_bytes());
        let fingerprint = fingerprints.get(&key).cloned().unwrap_or(current.clone());
        if fingerprint != current || text.is_empty() {
            return None;
        }
        let mut lines = text.lines().skip(line.saturating_sub(1));
        let mut signature = String::new();
        let mut current_line = lines.next().unwrap_or_default().to_string();
        for _ in 0..16 {
            if current_line.is_empty() {
                break;
            }
            let end = current_line.find(['{', ';']);
            signature.push_str(&current_line[..end.unwrap_or(current_line.len())]);
            signature.push(' ');
            if end.is_some() || signature.len() > 4096 {
                break;
            }
            match lines.next() {
                Some(next) => current_line = next.to_string(),
                None => break,
            }
        }
        if signature.is_empty() {
            return None;
        }
        let mut id = node_id(&worktree, &key, kind, qualified, &signature);
        let duplicates = snapshot
            .nodes
            .iter()
            .filter(|n| {
                n.path == key
                    && n.kind == kind
                    && n.qualified_name == qualified
                    && n.signature_discriminator == signature
            })
            .count();
        if duplicates != 0 {
            id = format!("{id}:ambiguous:{duplicates}");
        }
        snapshot.nodes.push(CodeNode {
            id: id.clone(),
            repository: worktree.clone(),
            worktree: worktree.clone(),
            path: key.clone(),
            kind,
            name: name.into(),
            qualified_name: qualified.into(),
            signature_discriminator: signature,
            source_fingerprint: fingerprint.clone(),
            generation,
            line,
        });
        let first = if line > 16 { line - 16 } else { 1 };
        let last = line + 32;
        snapshot.chunks.push(CodeChunk {
            id: format!("{id}:chunk:0"),
            owner_id: id.clone(),
            path: key,
            text: line_window(&text, first, last),
            first_line: first,
            last_line: last,
            source_fingerprint: fingerprint,
            generation,
            bounded_source_window: true,
        });
        Some(id)
    };
    for class in &index.classes {
        if let Some(id) = add_symbol(
            &mut snapshot,
            &class.file,
            CodeNodeKind::Class,
            &class.name,
            &class.qualified_name,
            class.line,
        ) {
            symbol_ids
                .entry(class.qualified_name.clone())
                .or_default()
                .push(id);
        }
    }
    for function in &index.functions {
        let kind = if function.kind == "method" {
            CodeNodeKind::Method
        } else {
            CodeNodeKind::Function
        };
        if let Some(id) = add_symbol(
            &mut snapshot,
            &function.file,
            kind,
            &function.name,
            &function.qualified_name,
            function.line,
        ) {
            symbol_ids
                .entry(function.qualified_name.clone())
                .or_default()
                .push(id);
        }
    }
    for (i, edge) in index.call_graph.iter().enumerate() {
        let path = generic(&edge.file);
        let callers = symbol_ids.get(&edge.caller).cloned().unwrap_or_default();
        let callees = symbol_ids.get(&edge.callee).cloned().unwrap_or_default();
        snapshot.edges.push(CodeEdge {
            id: format!("edge:{i}:{path}:{}", edge.line),
            caller_id: if callers.len() == 1 {
                callers[0].clone()
            } else {
                String::new()
            },
            ambiguous: callees.len() > 1 || callers.len() != 1,
            callee_ids: callees,
            unresolved_name: edge.callee.clone(),
            path,
            line: edge.line,
            heuristic: true,
            relation: "calls".into(),
        });
    }
    let defines: Vec<CodeEdge> = snapshot
        .nodes
        .iter()
        .filter(|n| !matches!(n.kind, CodeNodeKind::File | CodeNodeKind::Project))
        .filter_map(|n| {
            let file = file_ids.get(&n.path)?;
            Some(CodeEdge {
                id: format!("defines:{file}:{}", n.id),
                caller_id: file.clone(),
                callee_ids: vec![n.id.clone()],
                unresolved_name: String::new(),
                path: n.path.clone(),
                line: 0,
                heuristic: false,
                ambiguous: false,
                relation: "defines".into(),
            })
        })
        .collect();
    snapshot.edges.extend(defines);
    snapshot
}

struct Cached {
    generation: usize,
    snapshot: Arc<CodeSnapshot>,
}

/// [`CodeSnapshotProvider`] over the index service of the workspace root.
pub struct IndexCodeSnapshots {
    service: Arc<BackgroundService>,
    cached: Mutex<Option<Cached>>,
}

impl IndexCodeSnapshots {
    pub fn new(service: Arc<BackgroundService>) -> Self {
        Self {
            service,
            cached: Mutex::new(None),
        }
    }

    pub fn project_root(&self) -> PathBuf {
        self.service.project_root().to_path_buf()
    }
}

/// Archived warning when the index has no complete fingerprinted snapshot.
pub const CODE_RETRIEVAL_UNAVAILABLE: &str =
    "code retrieval unavailable: persisted index fingerprints are incomplete; rescan needed";

impl IndexCodeSnapshots {
    /// Snapshot of the current generation together with that generation.
    fn current(&self) -> Option<(Arc<CodeSnapshot>, usize)> {
        let status = self.service.status();
        if status.generation == 0 {
            return None;
        }
        let mut cached = self.cached.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(c) = cached.as_ref()
            && c.generation == status.generation
        {
            return Some((c.snapshot.clone(), c.generation));
        }
        let scan = self.service.project_scan();
        let code_index = self.service.code_index();
        let fingerprints = self.service.fingerprints();
        // Archived `load_persisted_snapshot` / scan publication: a retrieval
        // snapshot exists only when every indexed path has a recorded
        // fingerprint, and is never stale (each node is still checked
        // against its file before use), so a snapshot loaded from disk is
        // usable before this process has rescanned.
        if !qt_index::store::complete_scan_fingerprints(&scan, &code_index, &fingerprints) {
            return None;
        }
        let snapshot = Arc::new(build_snapshot(
            &scan,
            &code_index,
            status.generation as u64,
            &fingerprints,
            false,
        ));
        *cached = Some(Cached {
            generation: status.generation,
            snapshot: snapshot.clone(),
        });
        Some((snapshot, status.generation))
    }
}

impl CodeSnapshotProvider for IndexCodeSnapshots {
    fn snapshot(&self) -> Option<Arc<CodeSnapshot>> {
        self.current().map(|(snapshot, _)| snapshot)
    }

    fn unavailable_reason(&self) -> Option<String> {
        self.current()
            .is_none()
            .then(|| CODE_RETRIEVAL_UNAVAILABLE.to_string())
    }

    /// Archived `BackgroundService::retrieval_search`: exact name, qualified
    /// name or path matches (by node id), then file nodes of the pinned
    /// full-text matches in rank order; only nodes whose file still has the
    /// recorded fingerprint. Nothing when the generation moves meanwhile.
    fn lexical(&self, query: &str, limit: usize) -> Vec<RetrievalReference> {
        let limit = limit.min(1000);
        if query.is_empty() || limit == 0 {
            return Vec::new();
        }
        let Some((snapshot, generation)) = self.current() else {
            return Vec::new();
        };
        let mut references: Vec<RetrievalReference> = Vec::new();
        let mut add = |node: &CodeNode| {
            if references.len() >= limit || references.iter().any(|r| r.id == node.id) {
                return;
            }
            if source_fingerprint(&snapshot.root.join(&node.path)) != node.source_fingerprint {
                return;
            }
            references.push(CodeSnapshot::reference(node));
        };
        let mut exact: Vec<&CodeNode> = snapshot
            .nodes
            .iter()
            .filter(|n| n.name == query || n.qualified_name == query || n.path == query)
            .collect();
        exact.sort_by(|a, b| a.id.cmp(&b.id));
        for node in exact {
            add(node);
        }
        let terms = qt_index::store::retrieval_fts_query(query);
        if !terms.is_empty() {
            let database = self.service.status().sqlite_database;
            if let Ok(matches) = qt_index::store::fts_search_pinned(
                &database,
                self.service.project_root(),
                &terms,
                limit,
                generation,
            ) {
                let files: HashMap<&str, &CodeNode> = snapshot
                    .nodes
                    .iter()
                    .filter(|n| n.kind == CodeNodeKind::File)
                    .map(|n| (n.path.as_str(), n))
                    .collect();
                for m in &matches {
                    if let Some(node) = files.get(m.path.as_str()) {
                        add(node);
                    }
                }
            }
        }
        if self.service.status().generation != generation {
            return Vec::new();
        }
        references
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_index::code_index::{CallGraphEdge, FunctionSymbol};
    use qt_index::scan::ProjectFile;

    #[test]
    fn snapshot_follows_the_archived_projection() {
        let dir = tempfile::tempdir().unwrap();
        let source = "int helper(int x) {\n  return x;\n}\nint main() {\n  return helper(1);\n}\n";
        std::fs::write(dir.path().join("main.c"), source).unwrap();
        let scan = ProjectScanResult {
            root: dir.path().to_path_buf(),
            files: vec![ProjectFile {
                path: PathBuf::from("main.c"),
                kind: ProjectFileKind::CSource,
            }],
            ..Default::default()
        };
        let function = |name: &str, line: usize| FunctionSymbol {
            name: name.into(),
            qualified_name: name.into(),
            file: PathBuf::from("main.c"),
            line,
            kind: "function".into(),
        };
        let index = CodeIndex {
            functions: vec![function("helper", 1), function("main", 4)],
            call_graph: vec![CallGraphEdge {
                caller: "main".into(),
                callee: "helper".into(),
                file: PathBuf::from("main.c"),
                line: 5,
            }],
            ..Default::default()
        };
        let snapshot = build_snapshot(&scan, &index, 3, &HashMap::new(), false);
        assert_eq!(snapshot.nodes.len(), 4);
        let helper = snapshot.find_symbols("helper", None);
        assert_eq!(helper.len(), 1);
        assert_eq!(helper[0].signature_discriminator, "int helper(int x)  ");
        assert_eq!(helper[0].kind, CodeNodeKind::Function);
        let call = snapshot
            .edges
            .iter()
            .find(|e| e.relation == "calls")
            .unwrap();
        assert_eq!(call.callee_ids, vec![helper[0].id.clone()]);
        assert!(!call.ambiguous);
        assert_eq!(
            snapshot
                .edges
                .iter()
                .filter(|e| e.relation == "defines")
                .count(),
            2
        );
        let snippet = snapshot.read_snippet(&helper[0].id).unwrap();
        assert!(snippet.chunk.text.starts_with("int helper"));
        // A changed file invalidates the window.
        std::fs::write(dir.path().join("main.c"), "changed\n").unwrap();
        assert!(snapshot.read_snippet(&helper[0].id).is_none());
    }
}
