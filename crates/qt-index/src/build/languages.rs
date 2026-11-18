//! Cargo, npm (Vue/TypeScript) and Python project emulation.

use std::collections::BTreeSet;
use std::path::PathBuf;

use super::{BuildEmulationResult, ProjectIndex, project_build_files, record_build_system_command};
use crate::scan::{ProjectFileKind, ProjectScanResult};
use crate::util::{
    file_name, is_ignored_dependency_path, is_regular_file, lexically_normal, path_has_prefix,
    path_text, read_text_file,
};

pub(super) fn emulate_cargo_projects(
    result: &mut BuildEmulationResult,
    scan: &ProjectScanResult,
    index: &ProjectIndex<'_>,
) {
    for manifest in project_build_files(index, "Cargo.toml") {
        let Some(project_dir) = manifest.parent() else {
            continue;
        };
        for file in index.files(ProjectFileKind::RustSource) {
            if is_ignored_dependency_path(&file.path) {
                continue;
            }
            let source = lexically_normal(&scan.root.join(&file.path));
            if !path_has_prefix(&source, project_dir) {
                continue;
            }
            record_build_system_command(
                result,
                &source,
                project_dir,
                vec![
                    "cargo".into(),
                    "check".into(),
                    "--manifest-path".into(),
                    path_text(&manifest),
                    "--message-format=json".into(),
                ],
                "cargo",
            );
        }
    }
}

fn package_uses_vue_or_typescript(text: &str) -> bool {
    text.contains("vue-tsc")
        || text.contains("\"vite\"")
        || text.contains("\"typescript\"")
        || text.contains("\"vue\"")
}

pub(super) fn emulate_npm_projects(
    result: &mut BuildEmulationResult,
    scan: &ProjectScanResult,
    index: &ProjectIndex<'_>,
) {
    for package_json in project_build_files(index, "package.json") {
        let Some(project_dir) = package_json.parent() else {
            continue;
        };
        if !package_uses_vue_or_typescript(&read_text_file(&package_json)) {
            continue;
        }
        let arguments: Vec<String> = vec!["npm".into(), "run".into(), "build".into()];
        let sources = index
            .files(ProjectFileKind::TypeScriptSource)
            .iter()
            .chain(index.files(ProjectFileKind::JavaScriptSource).iter())
            .chain(index.files(ProjectFileKind::VueSource).iter());
        for file in sources {
            if is_ignored_dependency_path(&file.path) {
                continue;
            }
            let source = lexically_normal(&scan.root.join(&file.path));
            if !path_has_prefix(&source, project_dir) {
                continue;
            }
            record_build_system_command(result, &source, project_dir, arguments.clone(), "npm");
        }
    }
}

fn python_roots(scan: &ProjectScanResult, index: &ProjectIndex<'_>) -> Vec<PathBuf> {
    let mut roots = BTreeSet::new();
    for file in index.files(ProjectFileKind::BuildFile) {
        let name = file_name(&file.path);
        if (name == "setup.py" || name == "pyproject.toml" || name == "setup.cfg")
            && let Some(parent) = scan.root.join(&file.path).parent()
        {
            roots.insert(path_text(&lexically_normal(parent)));
        }
    }
    roots.into_iter().map(PathBuf::from).collect()
}

pub(super) fn emulate_python_projects(
    result: &mut BuildEmulationResult,
    scan: &ProjectScanResult,
    index: &ProjectIndex<'_>,
) {
    for root in python_roots(scan, index) {
        let root_text = path_text(&root);
        let arguments: Vec<String> = if is_regular_file(&root.join("pyproject.toml")) {
            vec!["python3".into(), "-m".into(), "build".into(), root_text]
        } else if is_regular_file(&root.join("setup.py")) {
            vec![
                "python3".into(),
                path_text(&root.join("setup.py")),
                "build".into(),
            ]
        } else {
            vec![
                "python3".into(),
                "-m".into(),
                "compileall".into(),
                root_text,
            ]
        };
        for file in index.files(ProjectFileKind::PythonSource) {
            if is_ignored_dependency_path(&file.path) {
                continue;
            }
            let source = lexically_normal(&scan.root.join(&file.path));
            if !path_has_prefix(&source, &root) {
                continue;
            }
            record_build_system_command(result, &source, &root, arguments.clone(), "python");
        }
    }
}
