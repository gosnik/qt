//! waf projects: sources under a `wscript` root with `includes=` hints.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use super::{BuildEmulationResult, add_existing_include, record_build_system_command};
use crate::scan::{ProjectFileKind, ProjectScanResult};
use crate::util::{
    entry_is_dir, is_ignored_dependency_path, lexically_normal, path_has_prefix, path_text,
    read_text_file, sorted_dir_entries, split_shell_like, trim_ascii,
};

fn parse_waf_include_tokens(root: &Path, text: &str) -> Vec<PathBuf> {
    let mut includes = Vec::new();
    let mut seen = BTreeSet::new();
    for key in ["includes", "export_includes"] {
        let mut pos = 0;
        while let Some(found) = text[pos..].find(key) {
            let start = pos + found;
            let Some(eq_offset) = text[start + key.len()..].find('=') else {
                pos = start + key.len();
                continue;
            };
            let equals = start + key.len() + eq_offset;
            let end = text[equals + 1..].find(['\n', ')']).map(|e| equals + 1 + e);
            let fragment = &text[equals + 1..end.unwrap_or(text.len())];
            for token in split_shell_like(fragment) {
                let token: String = token
                    .chars()
                    .filter(|c| !matches!(c, ',' | '[' | ']'))
                    .collect();
                let token = trim_ascii(&token);
                if token.is_empty()
                    || token.contains('$')
                    || token.contains('%')
                    || token == "+"
                    || token == ":"
                {
                    continue;
                }
                add_existing_include(&mut includes, &mut seen, &root.join(token));
            }
            pos = end.unwrap_or(text.len());
        }
    }
    includes
}

fn waf_include_roots(root: &Path, source: &Path, script_includes: &[PathBuf]) -> Vec<PathBuf> {
    let mut includes = Vec::new();
    let mut seen = BTreeSet::new();
    let source_dir = source.parent().unwrap_or(Path::new(""));
    add_existing_include(&mut includes, &mut seen, root);
    add_existing_include(&mut includes, &mut seen, source_dir);
    for path in script_includes {
        add_existing_include(&mut includes, &mut seen, path);
    }
    let mut dir = Some(source_dir);
    while let Some(current) = dir {
        if current.as_os_str().is_empty() || !path_has_prefix(current, root) {
            break;
        }
        add_existing_include(&mut includes, &mut seen, current);
        if current == root {
            break;
        }
        dir = current.parent();
    }
    for conventional in ["include", "inc", "src"] {
        add_existing_include(&mut includes, &mut seen, &root.join(conventional));
    }
    for top in ["libraries", "modules"] {
        let base = root.join(top);
        add_existing_include(&mut includes, &mut seen, &base);
        if base.is_dir() {
            for entry in sorted_dir_entries(&base) {
                if entry_is_dir(&entry) {
                    let path = entry.path();
                    add_existing_include(&mut includes, &mut seen, &path);
                    add_existing_include(&mut includes, &mut seen, &path.join("include"));
                    add_existing_include(&mut includes, &mut seen, &path.join("utility"));
                }
            }
        }
    }
    includes
}

fn inherited_waf_includes(
    roots: &[PathBuf],
    includes_by_root: &BTreeMap<String, Vec<PathBuf>>,
    owner: &Path,
) -> Vec<PathBuf> {
    let mut includes = Vec::new();
    let mut seen = BTreeSet::new();
    for root in roots {
        if !path_has_prefix(owner, root) {
            continue;
        }
        if let Some(found) = includes_by_root.get(&path_text(root)) {
            for include in found {
                add_existing_include(&mut includes, &mut seen, include);
            }
        }
    }
    includes
}

fn waf_roots(scan: &ProjectScanResult) -> Vec<PathBuf> {
    let mut roots = BTreeSet::new();
    for file in &scan.files {
        if file.kind == ProjectFileKind::BuildFile
            && file.path.file_name().is_some_and(|n| n == "wscript")
            && let Some(parent) = scan.root.join(&file.path).parent()
        {
            roots.insert(path_text(&lexically_normal(parent)));
        }
    }
    roots.into_iter().map(PathBuf::from).collect()
}

fn owning_waf_root(roots: &[PathBuf], source: &Path) -> Option<PathBuf> {
    let mut best: Option<&PathBuf> = None;
    for root in roots {
        if !path_has_prefix(source, root) {
            continue;
        }
        if best.is_none_or(|b| path_text(root).len() > path_text(b).len()) {
            best = Some(root);
        }
    }
    best.cloned()
}

fn should_skip_waf_source(relative: &Path) -> bool {
    is_ignored_dependency_path(relative)
        || relative.components().any(|part| {
            let name = part.as_os_str();
            name == "build" || name == "docs"
        })
}

pub(super) fn emulate_waf_projects(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    scan: &ProjectScanResult,
) {
    let roots = waf_roots(scan);
    let mut includes_by_root = BTreeMap::new();
    for root in &roots {
        let text = read_text_file(&root.join("wscript"));
        includes_by_root.insert(path_text(root), parse_waf_include_tokens(root, &text));
    }
    for file in &scan.files {
        if !file.kind.is_native_source() || should_skip_waf_source(&file.path) {
            continue;
        }
        let source = lexically_normal(&scan.root.join(&file.path));
        let Some(root) = owning_waf_root(&roots, &source) else {
            continue;
        };
        let c = file.kind == ProjectFileKind::CSource;
        let mut arguments = vec![
            if c { "cc" } else { "c++" }.to_string(),
            if c { "-std=gnu11" } else { "-std=gnu++17" }.to_string(),
        ];
        let script_includes = inherited_waf_includes(&roots, &includes_by_root, &root);
        for include in waf_include_roots(&root, &source, &script_includes) {
            includes.insert(path_text(&include));
            arguments.push(format!("-I{}", path_text(&include)));
        }
        arguments.push("-c".into());
        arguments.push(path_text(&source));
        arguments.push("-o".into());
        let relative = source.strip_prefix(&scan.root).unwrap_or(&source);
        arguments.push(format!(
            "{}.o",
            path_text(&scan.root.join("build/index-waf").join(relative))
        ));
        record_build_system_command(result, &source, &root, arguments, "waf");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::ProjectFile;
    use std::fs;

    #[test]
    fn waf_sources_get_script_includes() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("libraries/AP_Math")).unwrap();
        fs::create_dir_all(root.join("extra")).unwrap();
        fs::write(root.join("wscript"), "bld(includes=['extra'])\n").unwrap();
        fs::write(root.join("libraries/AP_Math/m.cpp"), "int m;\n").unwrap();
        let scan = ProjectScanResult {
            root: root.clone(),
            files: vec![
                ProjectFile {
                    path: "libraries/AP_Math/m.cpp".into(),
                    kind: ProjectFileKind::CxxSource,
                },
                ProjectFile {
                    path: "wscript".into(),
                    kind: ProjectFileKind::BuildFile,
                },
            ],
            ..Default::default()
        };
        let mut result = BuildEmulationResult::default();
        let mut includes = BTreeSet::new();
        emulate_waf_projects(&mut result, &mut includes, &scan);
        let command = &result.compile_commands[&path_text(&root.join("libraries/AP_Math/m.cpp"))];
        assert_eq!(command.backend, "waf");
        assert!(
            command
                .arguments
                .contains(&format!("-I{}", root.join("extra").display()))
        );
        assert!(
            command
                .arguments
                .contains(&format!("-I{}", root.join("libraries/AP_Math").display()))
        );
        assert_eq!(
            command.arguments.last().unwrap(),
            &format!(
                "{}.o",
                root.join("build/index-waf/libraries/AP_Math/m.cpp")
                    .display()
            )
        );
    }
}
