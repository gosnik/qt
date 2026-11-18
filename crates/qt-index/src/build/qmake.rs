//! qmake `.pro` / `.pri` / `.prf` projects: SOURCES, INCLUDEPATH, DEFINES.

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

use super::{
    BuildEmulationResult, ProjectIndex, add_existing_include, is_c_source_path, is_source_path,
    parse_assignment, qmake_project_files, record_build_system_command,
};
use crate::util::{
    file_name, getlines, is_regular_file, normalize_path, path_text, read_text_file,
    split_shell_like, trim_ascii,
};

#[derive(Default)]
struct QMakeProjectFacts {
    sources: Vec<PathBuf>,
    includes: Vec<PathBuf>,
    defines: Vec<String>,
}

fn parse_qmake_project_file(project_file: &Path) -> QMakeProjectFacts {
    let mut facts = QMakeProjectFacts::default();
    let mut seen_sources = BTreeSet::new();
    let mut seen_includes = BTreeSet::new();
    let mut seen_defines = BTreeSet::new();
    let text = read_text_file(project_file);
    let base = project_file.parent().unwrap_or(Path::new("")).to_path_buf();
    let mut logical = String::new();
    for raw in getlines(&text) {
        let line = match raw.find('#') {
            Some(comment) => &raw[..comment],
            None => raw,
        };
        let line = trim_ascii(line);
        if line.is_empty() {
            continue;
        }
        if let Some(continued) = line.strip_suffix('\\') {
            logical.push_str(continued);
            logical.push(' ');
            continue;
        }
        logical.push_str(line);
        let assignment = parse_assignment(&logical);
        logical.clear();
        let Some((key, value)) = assignment else {
            continue;
        };
        let key = key.to_ascii_lowercase();
        for token in split_shell_like(&value) {
            let token: String = token.chars().filter(|c| *c != '\\').collect();
            let token = trim_ascii(&token).to_string();
            if token.is_empty() || token.contains('$') {
                continue;
            }
            match key.as_str() {
                "sources" => {
                    let source = normalize_path(&base, &token);
                    if is_source_path(&source)
                        && is_regular_file(&source)
                        && seen_sources.insert(path_text(&source))
                    {
                        facts.sources.push(source);
                    }
                }
                "includepath" => {
                    let include = normalize_path(&base, &token);
                    if include.is_dir() && seen_includes.insert(path_text(&include)) {
                        facts.includes.push(include);
                    }
                }
                "defines" => {
                    let define = token.strip_prefix("-D").unwrap_or(&token).to_string();
                    if !define.is_empty() && seen_defines.insert(define.clone()) {
                        facts.defines.push(define);
                    }
                }
                _ => {}
            }
        }
    }
    facts
}

pub(super) fn emulate_qmake_projects(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    index: &ProjectIndex<'_>,
) {
    for project_file in qmake_project_files(index) {
        let facts = parse_qmake_project_file(&project_file);
        let project_dir = project_file.parent().unwrap_or(Path::new("")).to_path_buf();
        for source in &facts.sources {
            let c = is_c_source_path(source);
            let mut arguments = vec![
                if c { "cc" } else { "c++" }.to_string(),
                if c { "-std=gnu11" } else { "-std=gnu++17" }.to_string(),
            ];
            for define in &facts.defines {
                defines.insert(define.clone());
                arguments.push(format!("-D{define}"));
            }
            let mut source_includes = facts.includes.clone();
            let mut seen: BTreeSet<String> = source_includes.iter().map(|p| path_text(p)).collect();
            add_existing_include(&mut source_includes, &mut seen, &project_dir);
            add_existing_include(
                &mut source_includes,
                &mut seen,
                source.parent().unwrap_or(Path::new("")),
            );
            for include in &source_includes {
                includes.insert(path_text(include));
                arguments.push(format!("-I{}", path_text(include)));
            }
            arguments.push("-c".into());
            arguments.push(path_text(source));
            arguments.push("-o".into());
            arguments.push(path_text(
                &project_dir
                    .join("build/index-qmake")
                    .join(format!("{}.o", file_name(source))),
            ));
            record_build_system_command(result, source, &project_dir, arguments, "qmake");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::{ProjectFile, ProjectFileKind, ProjectScanResult};
    use std::fs;

    #[test]
    fn qmake_sources_defines_and_includes() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("inc")).unwrap();
        fs::write(root.join("main.cpp"), "int main(){}\n").unwrap();
        fs::write(root.join("util.c"), "int u;\n").unwrap();
        fs::write(
            root.join("app.pro"),
            "SOURCES += main.cpp \\\n    util.c\nINCLUDEPATH += inc\nDEFINES += APP_VERSION=2 # comment\n",
        )
        .unwrap();
        let scan = ProjectScanResult {
            root: root.clone(),
            files: vec![ProjectFile {
                path: "app.pro".into(),
                kind: ProjectFileKind::BuildFile,
            }],
            ..Default::default()
        };
        let index = ProjectIndex::new(&scan);
        let mut result = BuildEmulationResult::default();
        let mut includes = BTreeSet::new();
        let mut defines = BTreeSet::new();
        emulate_qmake_projects(&mut result, &mut includes, &mut defines, &index);
        let cpp = &result.compile_commands[&path_text(&root.join("main.cpp"))];
        assert_eq!(
            &cpp.arguments[..3],
            &["c++", "-std=gnu++17", "-DAPP_VERSION=2"]
        );
        assert!(
            cpp.arguments
                .contains(&format!("-I{}", root.join("inc").display()))
        );
        assert_eq!(cpp.backend, "qmake");
        assert_eq!(cpp.confidence, 75);
        let c = &result.compile_commands[&path_text(&root.join("util.c"))];
        assert_eq!(c.arguments[0], "cc");
        assert!(defines.contains("APP_VERSION=2"));
    }
}
