//! Incremental tree-sitter metadata.
//!
//! Every C/C++ source and header is parsed with the tree-sitter C++ grammar,
//! and every Rust, TypeScript and JavaScript source with its own grammar
//! ([`Grammar`]); the S-expression tree is written to `<output>/trees/<file>.tree`.
//! `<output>/metadata.json` records size, mtime and an FNV-1a checksum per
//! file so unchanged files are reused on the next scan; trees for files that
//! disappeared are deleted.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use serde_json::{Value, json};

use crate::cancel::{Cancelled, Pacer};
use crate::code_index::dependency_or_generated_reason;
use crate::grammar::Grammar;
use crate::scan::ProjectScanResult;
use crate::syntax_index::syntax_skip_reason;
use crate::util::{fnv1a64, generic, lexically_normal, path_text};

#[derive(Debug, Clone)]
pub struct TreeSitterOptions {
    pub enabled: bool,
    pub output_directory: PathBuf,
}

impl Default for TreeSitterOptions {
    fn default() -> Self {
        Self {
            enabled: true,
            output_directory: PathBuf::new(),
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct TreeSitterResult {
    pub attempted: bool,
    pub available: bool,
    pub generated: bool,
    pub files_considered: usize,
    pub files_parsed: usize,
    pub files_updated: usize,
    pub files_unchanged: usize,
    pub files_added: usize,
    pub files_changed: usize,
    pub files_deleted: usize,
    pub files_failed: usize,
    pub files_skipped: usize,
    pub output_directory: PathBuf,
    pub manifest_path: PathBuf,
    pub failed_files: Vec<String>,
    pub skipped_files: Vec<String>,
    pub mode: String,
    pub message: String,
}

impl TreeSitterResult {
    pub fn to_json(&self) -> Value {
        json!({
            "attempted": self.attempted,
            "available": self.available,
            "generated": self.generated,
            "files_considered": self.files_considered,
            "files_parsed": self.files_parsed,
            "files_updated": self.files_updated,
            "files_unchanged": self.files_unchanged,
            "files_added": self.files_added,
            "files_changed": self.files_changed,
            "files_deleted": self.files_deleted,
            "files_failed": self.files_failed,
            "files_skipped": self.files_skipped,
            "output_directory": path_text(&self.output_directory),
            "manifest_path": path_text(&self.manifest_path),
            "failed_files": self.failed_files,
            "skipped_files": self.skipped_files,
            "mode": self.mode,
            "message": self.message,
        })
    }

    pub fn from_json(text: &str) -> Self {
        let Ok(Value::Object(v)) = serde_json::from_str::<Value>(text) else {
            return Self::default();
        };
        let b = |k: &str| v.get(k).and_then(Value::as_bool).unwrap_or(false);
        let n = |k: &str| v.get(k).and_then(Value::as_u64).unwrap_or(0) as usize;
        let s = |k: &str| {
            v.get(k)
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string()
        };
        let list = |k: &str| -> Vec<String> {
            v.get(k)
                .and_then(Value::as_array)
                .map(|a| {
                    a.iter()
                        .filter_map(|x| x.as_str().map(str::to_string))
                        .collect()
                })
                .unwrap_or_default()
        };
        Self {
            attempted: b("attempted"),
            available: b("available"),
            generated: b("generated"),
            files_considered: n("files_considered"),
            files_parsed: n("files_parsed"),
            files_updated: n("files_updated"),
            files_unchanged: n("files_unchanged"),
            files_added: n("files_added"),
            files_changed: n("files_changed"),
            files_deleted: n("files_deleted"),
            files_failed: n("files_failed"),
            files_skipped: n("files_skipped"),
            output_directory: PathBuf::from(s("output_directory")),
            manifest_path: PathBuf::from(s("manifest_path")),
            failed_files: list("failed_files"),
            skipped_files: list("skipped_files"),
            mode: s("mode"),
            message: s("message"),
        }
    }
}

fn checksum_text(bytes: &[u8]) -> String {
    format!("{:016x}", fnv1a64(bytes))
}

fn file_time_ticks(path: &Path) -> i64 {
    std::fs::metadata(path)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_nanos() as i64)
        .unwrap_or(0)
}

fn file_size_or_zero(path: &Path) -> u64 {
    std::fs::metadata(path).map(|m| m.len()).unwrap_or(0)
}

struct ManifestEntry {
    tree: String,
    parsed: bool,
    size: u64,
    mtime: i64,
    checksum: String,
}

fn read_manifest_entries(manifest_path: &Path) -> BTreeMap<String, ManifestEntry> {
    let mut entries = BTreeMap::new();
    let Ok(bytes) = std::fs::read(manifest_path) else {
        return entries;
    };
    let Ok(manifest) = serde_json::from_slice::<Value>(&bytes) else {
        return entries;
    };
    let Some(files) = manifest.get("files").and_then(Value::as_array) else {
        return entries;
    };
    for item in files {
        let Some(object) = item.as_object() else {
            continue;
        };
        let source = object
            .get("source")
            .and_then(Value::as_str)
            .unwrap_or_default();
        if source.is_empty() {
            continue;
        }
        entries.insert(
            source.to_string(),
            ManifestEntry {
                tree: object
                    .get("tree")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
                parsed: object
                    .get("parsed")
                    .and_then(Value::as_bool)
                    .unwrap_or(false),
                size: object.get("size").and_then(Value::as_u64).unwrap_or(0),
                mtime: object.get("mtime").and_then(Value::as_i64).unwrap_or(0),
                checksum: object
                    .get("checksum")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
                    .to_string(),
            },
        );
    }
    entries
}

fn tree_artifact_exists(output_directory: &Path, entry: &ManifestEntry) -> bool {
    !entry.tree.is_empty() && output_directory.join(&entry.tree).is_file()
}

/// Parse `source_text`, write the S-expression to `output`, and report
/// whether the tree is error-free.
fn parse_to_tree(parser: &mut tree_sitter::Parser, source_text: &[u8], output: &Path) -> bool {
    if source_text.len() > u32::MAX as usize {
        return false;
    }
    let Some(tree) = parser.parse(source_text, None) else {
        return false;
    };
    let root = tree.root_node();
    let sexp = root.to_sexp();
    if let Some(parent) = output.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let _ = std::fs::write(output, format!("{sexp}\n"));
    !root.has_error()
}

/// ABI version of the bundled tree-sitter C++ grammar.
pub fn grammar_abi_version() -> usize {
    let language: tree_sitter::Language = tree_sitter_cpp::LANGUAGE.into();
    language.abi_version()
}

/// Generate or refresh tree-sitter metadata for the scan's C/C++, Rust,
/// TypeScript and JavaScript files.
pub fn generate_tree_sitter_metadata(
    scan: &ProjectScanResult,
    options: &TreeSitterOptions,
    pacer: &Pacer,
) -> Result<TreeSitterResult, Cancelled> {
    let output_directory = if options.output_directory.as_os_str().is_empty() {
        scan.root.join(".index").join("tree-sitter")
    } else {
        options.output_directory.clone()
    };
    let mut result = TreeSitterResult {
        attempted: options.enabled,
        manifest_path: output_directory.join("metadata.json"),
        output_directory: output_directory.clone(),
        mode: if pacer.low_priority() {
            "internal-low-priority"
        } else {
            "internal"
        }
        .into(),
        ..Default::default()
    };
    if !options.enabled {
        result.message = "tree-sitter disabled".into();
        return Ok(result);
    }

    let abi = grammar_abi_version();
    let mut parsers: BTreeMap<Grammar, Option<tree_sitter::Parser>> = BTreeMap::new();
    if Grammar::Cpp.parser().is_none() {
        result.message = format!("internal tree-sitter-cpp grammar ABI is incompatible: {abi}");
        return Ok(result);
    }
    result.available = true;
    if let Err(error) = std::fs::create_dir_all(output_directory.join("trees")) {
        result.message = format!("could not create tree-sitter output directory: {error}");
        return Ok(result);
    }

    let existing = read_manifest_entries(&result.manifest_path);
    let mut current: BTreeMap<String, bool> = BTreeMap::new();
    let mut files_json: Vec<Value> = Vec::new();
    let mut languages = std::collections::BTreeSet::new();

    for file in &scan.files {
        let Some(grammar) = Grammar::for_file(file.kind, &file.path) else {
            continue;
        };
        result.files_considered += 1;
        let relative_source = generic(&file.path);
        current.insert(relative_source.clone(), true);
        let source = lexically_normal(&scan.root.join(&file.path));
        let mut tree_name = file.path.clone().into_os_string();
        tree_name.push(".tree");
        let output = output_directory.join("trees").join(&tree_name);
        let relative_tree = generic(&Path::new("trees").join(&tree_name));
        let size = file_size_or_zero(&source);
        let mtime = file_time_ticks(&source);

        let skip_reason = match grammar {
            Grammar::Cpp => dependency_or_generated_reason(&file.path),
            _ => syntax_skip_reason(&file.path),
        };
        if let Some(reason) = skip_reason {
            result.files_skipped += 1;
            result.skipped_files.push(relative_source.clone());
            let _ = std::fs::remove_file(&output);
            files_json.push(json!({
                "source": relative_source,
                "tree": relative_tree,
                "parsed": false,
                "size": size,
                "mtime": mtime,
                "checksum": "",
                "skipped": true,
                "reason": reason,
                "language": grammar.name(),
            }));
            pacer.checkpoint(result.files_considered, 32, 5)?;
            continue;
        }

        let previous = existing.get(&relative_source);
        let mut parsed = false;
        let mut checksum = String::new();
        let mut reused = false;
        match previous {
            Some(prev)
                if prev.size == size
                    && prev.mtime == mtime
                    && tree_artifact_exists(&output_directory, prev) =>
            {
                parsed = prev.parsed;
                checksum = prev.checksum.clone();
                reused = true;
            }
            _ => match std::fs::read(&source) {
                Ok(text) => {
                    checksum = checksum_text(&text);
                    match previous {
                        Some(prev)
                            if !prev.checksum.is_empty()
                                && prev.checksum == checksum
                                && tree_artifact_exists(&output_directory, prev) =>
                        {
                            parsed = prev.parsed;
                            reused = true;
                        }
                        _ => {
                            pacer.checkpoint(0, 0, 25)?;
                            let parser = parsers
                                .entry(grammar)
                                .or_insert_with(|| grammar.parser())
                                .as_mut();
                            parsed =
                                parser.is_some_and(|parser| parse_to_tree(parser, &text, &output));
                            pacer.checkpoint(0, 0, 25)?;
                            result.files_updated += 1;
                            if previous.is_none() {
                                result.files_added += 1;
                            } else {
                                result.files_changed += 1;
                            }
                        }
                    }
                }
                Err(_) => {
                    result.files_updated += 1;
                    if previous.is_none() {
                        result.files_added += 1;
                    } else {
                        result.files_changed += 1;
                    }
                }
            },
        }

        if parsed {
            result.files_parsed += 1;
        } else {
            result.files_failed += 1;
            result.failed_files.push(relative_source.clone());
        }
        if reused {
            result.files_unchanged += 1;
        }
        files_json.push(json!({
            "source": relative_source,
            "tree": relative_tree,
            "parsed": parsed,
            "size": size,
            "mtime": mtime,
            "checksum": checksum,
            "language": grammar.name(),
        }));
        languages.insert(grammar.name());
        pacer.checkpoint(result.files_considered, 32, 5)?;
    }

    for (source, entry) in &existing {
        if current.contains_key(source) {
            continue;
        }
        result.files_deleted += 1;
        if !entry.tree.is_empty() {
            let _ = std::fs::remove_file(output_directory.join(&entry.tree));
        }
        pacer.checkpoint(result.files_deleted, 32, 5)?;
    }

    let manifest = json!({
        "root": path_text(&scan.root),
        "mode": result.mode,
        "tree_sitter_language": "cpp",
        "tree_sitter_languages": languages,
        "tree_sitter_abi": abi,
        "files": files_json,
    });
    let text = format!(
        "{}\n",
        serde_json::to_string_pretty(&manifest).unwrap_or_default()
    );
    if std::fs::write(&result.manifest_path, text).is_err() {
        result.message = "could not write tree-sitter metadata manifest".into();
        return Ok(result);
    }
    result.generated = result.files_parsed > 0;
    result.message = "tree-sitter metadata generated".into();
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::scan_project_tree;
    use std::fs;

    #[test]
    fn generates_and_reuses_trees() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("third_party")).unwrap();
        fs::write(
            root.join("src/ok.cpp"),
            "int add(int a, int b) { return a + b; }\n",
        )
        .unwrap();
        fs::write(root.join("src/bad.cpp"), "int broken( {\n").unwrap();
        let out = root.join(".state/tree-sitter");
        let options = TreeSitterOptions {
            enabled: true,
            output_directory: out.clone(),
        };
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let first = generate_tree_sitter_metadata(&scan, &options, &Pacer::unpaced()).unwrap();
        assert!(first.available && first.generated);
        assert_eq!(first.files_considered, 2);
        assert_eq!(first.files_parsed, 1);
        assert_eq!(first.files_failed, 1);
        assert_eq!(first.failed_files, vec!["src/bad.cpp"]);
        assert_eq!(first.files_added, 2);
        assert_eq!(first.mode, "internal");
        assert_eq!(first.message, "tree-sitter metadata generated");
        let tree = fs::read_to_string(out.join("trees/src/ok.cpp.tree")).unwrap();
        assert!(tree.starts_with("(translation_unit (function_definition"));

        let second = generate_tree_sitter_metadata(&scan, &options, &Pacer::unpaced()).unwrap();
        assert_eq!(second.files_unchanged, 2);
        assert_eq!(second.files_updated, 0);

        fs::remove_file(root.join("src/bad.cpp")).unwrap();
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let third = generate_tree_sitter_metadata(&scan, &options, &Pacer::unpaced()).unwrap();
        assert_eq!(third.files_deleted, 1);
        assert!(!out.join("trees/src/bad.cpp.tree").exists());

        let manifest: Value =
            serde_json::from_str(&fs::read_to_string(out.join("metadata.json")).unwrap()).unwrap();
        assert_eq!(manifest["tree_sitter_language"], "cpp");
        assert_eq!(manifest["files"].as_array().unwrap().len(), 1);
        assert!(grammar_abi_version() >= 13);
    }

    #[test]
    fn json_round_trip() {
        let result = TreeSitterResult {
            attempted: true,
            files_failed: 2,
            failed_files: vec!["a".into()],
            mode: "internal".into(),
            ..Default::default()
        };
        assert_eq!(
            TreeSitterResult::from_json(&result.to_json().to_string()),
            result
        );
    }
}
