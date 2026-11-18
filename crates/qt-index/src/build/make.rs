//! Plain Make / Kbuild projects: variables, Kconfig `.config` defines and
//! sources owned by the nearest directory with a makefile.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use super::{
    BuildEmulationResult, add_define, add_existing_include, append_compile_tokens,
    parse_assignment, record_build_system_command, scan_tokens_for_metadata,
};
use crate::scan::{ProjectFileKind, ProjectScanResult};
use crate::util::{
    extension_text, file_name, getlines, is_ignored_dependency_path, lexically_normal,
    normalize_path, path_has_prefix, path_text, read_text_file, split_shell_like, trim_ascii,
};

fn is_make_file_name(path: &Path) -> bool {
    let name = file_name(path);
    name == "Makefile"
        || name.eq_ignore_ascii_case("makefile")
        || name == "GNUmakefile"
        || name == "Make.defs"
        || name == "Kbuild"
        || extension_text(path) == ".mk"
}

fn make_roots(scan: &ProjectScanResult) -> BTreeSet<String> {
    let mut roots = BTreeSet::new();
    for file in &scan.files {
        if file.kind == ProjectFileKind::BuildFile
            && is_make_file_name(&file.path)
            && let Some(parent) = scan.root.join(&file.path).parent()
        {
            roots.insert(path_text(&lexically_normal(parent)));
        }
    }
    roots
}

fn owning_make_root(
    roots: &BTreeSet<String>,
    source: &Path,
    project_root: &Path,
) -> Option<PathBuf> {
    let mut dir = source.parent();
    while let Some(current) = dir {
        if current.as_os_str().is_empty() || !path_has_prefix(current, project_root) {
            break;
        }
        if roots.contains(&path_text(current)) {
            return Some(current.to_path_buf());
        }
        if current == project_root {
            break;
        }
        dir = current.parent();
    }
    None
}

/// Expand `$(NAME)` / `${NAME}` references from `variables`; unknown
/// references are left in place.
fn expand_make_variables(value: &str, variables: &BTreeMap<String, String>) -> String {
    let mut value = value.to_string();
    let mut pos = 0;
    while pos < value.len() {
        let paren = value[pos..].find("$(").map(|i| i + pos);
        let brace = value[pos..].find("${").map(|i| i + pos);
        let (start, close) = match (paren, brace) {
            (Some(p), Some(b)) if b < p => (b, '}'),
            (Some(p), _) => (p, ')'),
            (None, Some(b)) => (b, '}'),
            (None, None) => break,
        };
        let Some(end) = value[start + 2..].find(close).map(|i| i + start + 2) else {
            break;
        };
        let key = value[start + 2..end].to_string();
        match variables.get(&key) {
            Some(replacement) => {
                value.replace_range(start..=end, replacement);
                pos = start + replacement.len();
            }
            None => pos = end + 1,
        }
    }
    value
}

fn scan_make_file_variables(path: &Path, variables: &mut BTreeMap<String, String>) {
    let text = read_text_file(path);
    let mut logical = String::new();
    for line in getlines(&text) {
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
        if key.is_empty() {
            continue;
        }
        let expanded = expand_make_variables(&value, variables);
        variables.insert(key, expanded);
    }
}

fn scan_kconfig_defines(path: &Path, defines: &mut BTreeSet<String>) {
    let text = read_text_file(path);
    for line in getlines(&text) {
        let line = trim_ascii(line);
        if !line.starts_with("CONFIG_") {
            continue;
        }
        let Some(equals) = line.find('=') else {
            continue;
        };
        let key = &line[..equals];
        let value = &line[equals + 1..];
        match value {
            "y" => add_define(defines, &format!("{key}=1")),
            "n" => {}
            other => add_define(defines, &format!("{key}={other}")),
        }
    }
}

fn collect_make_variables(scan: &ProjectScanResult) -> BTreeMap<String, String> {
    let mut variables = BTreeMap::new();
    variables.insert("TOPDIR".to_string(), path_text(&scan.root));
    variables.insert("CURDIR".to_string(), path_text(&scan.root));
    for file in &scan.files {
        if file.kind == ProjectFileKind::BuildFile && is_make_file_name(&file.path) {
            scan_make_file_variables(&scan.root.join(&file.path), &mut variables);
        }
    }
    variables
}

fn make_include_roots(
    project_root: &Path,
    owner: &Path,
    source: &Path,
    variable_includes: &[PathBuf],
) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let mut seen = BTreeSet::new();
    add_existing_include(&mut paths, &mut seen, project_root);
    add_existing_include(&mut paths, &mut seen, owner);
    add_existing_include(
        &mut paths,
        &mut seen,
        source.parent().unwrap_or(Path::new("")),
    );
    for conventional in ["include", "inc", "src"] {
        add_existing_include(&mut paths, &mut seen, &project_root.join(conventional));
        add_existing_include(&mut paths, &mut seen, &owner.join(conventional));
    }
    for include in variable_includes {
        add_existing_include(&mut paths, &mut seen, include);
    }
    paths
}

fn has_shell_metachar(token: &str) -> bool {
    token.contains(['$', '(', ')', '`', '<', '>', '|', '&', ';'])
}

fn is_safe_make_path_token(token: &str) -> bool {
    !(token.is_empty() || token == "/" || token == "." || token == "/dev/null")
        && !has_shell_metachar(token)
}

fn make_variable_include_roots(owner: &Path, variables: &BTreeMap<String, String>) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let mut seen = BTreeSet::new();
    for (key, value) in variables {
        let lower = key.to_ascii_lowercase();
        if !["include", "incdir", "cflags", "cppflags", "cxxflags"]
            .iter()
            .any(|k| lower.contains(k))
        {
            continue;
        }
        let tokens = split_shell_like(&expand_make_variables(value, variables));
        let mut index = 0;
        while index < tokens.len() {
            let token = tokens[index].as_str();
            if matches!(token, "-I" | "-isystem") && index + 1 < tokens.len() {
                index += 1;
                let include = &tokens[index];
                if is_safe_make_path_token(include) {
                    add_existing_include(&mut paths, &mut seen, &normalize_path(owner, include));
                }
            } else if token.len() > 2 && token.starts_with("-I") {
                let include = &token[2..];
                if is_safe_make_path_token(include) {
                    add_existing_include(&mut paths, &mut seen, &normalize_path(owner, include));
                }
            } else if (token.contains('/') || token == "include") && is_safe_make_path_token(token)
            {
                add_existing_include(&mut paths, &mut seen, &normalize_path(owner, token));
            }
            index += 1;
        }
    }
    paths
}

fn make_flag_takes_value(token: &str) -> bool {
    matches!(
        token,
        "-I" | "-isystem"
            | "-include"
            | "-imacros"
            | "-idirafter"
            | "-iquote"
            | "-D"
            | "-U"
            | "/I"
            | "/D"
    )
}

fn is_safe_make_flag_token(token: &str) -> bool {
    if token.is_empty()
        || matches!(
            token,
            "&&" | "||" | "|" | ";" | "<" | ">" | "2>" | "1>" | "/dev/null"
        )
        || has_shell_metachar(token)
    {
        return false;
    }
    token.starts_with('-') || token.starts_with("/I") || token.starts_with("/D")
}

fn make_flag_tokens(variables: &BTreeMap<String, String>, kind: ProjectFileKind) -> Vec<String> {
    let keys: [&str; 2] = if kind == ProjectFileKind::CSource {
        ["CPPFLAGS", "CFLAGS"]
    } else {
        ["CPPFLAGS", "CXXFLAGS"]
    };
    let mut tokens = Vec::new();
    for key in keys {
        let Some(value) = variables.get(key) else {
            continue;
        };
        let split = split_shell_like(&expand_make_variables(value, variables));
        let mut index = 0;
        while index < split.len() {
            let token = &split[index];
            if is_safe_make_flag_token(token) {
                let takes_value = make_flag_takes_value(token);
                tokens.push(token.clone());
                if takes_value && index + 1 < split.len() {
                    index += 1;
                    if !has_shell_metachar(&split[index]) {
                        tokens.push(split[index].clone());
                    }
                }
            }
            index += 1;
        }
    }
    tokens
}

pub(super) fn emulate_make_projects(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    scan: &ProjectScanResult,
) {
    let roots = make_roots(scan);
    if roots.is_empty() {
        return;
    }
    let variables = collect_make_variables(scan);
    scan_kconfig_defines(&scan.root.join(".config"), defines);
    let mut include_roots_by_owner: BTreeMap<String, Vec<PathBuf>> = BTreeMap::new();

    for file in &scan.files {
        if !file.kind.is_native_source() || is_ignored_dependency_path(&file.path) {
            continue;
        }
        let source = lexically_normal(&scan.root.join(&file.path));
        let Some(owner) = owning_make_root(&roots, &source, &scan.root) else {
            continue;
        };
        let c = file.kind == ProjectFileKind::CSource;
        let mut arguments = vec![
            if c { "cc" } else { "c++" }.to_string(),
            if c { "-std=gnu11" } else { "-std=gnu++17" }.to_string(),
        ];
        let flag_tokens = make_flag_tokens(&variables, file.kind);
        scan_tokens_for_metadata(&flag_tokens, &owner, includes, defines);
        append_compile_tokens(&mut arguments, &flag_tokens, &owner);

        let owner_includes = include_roots_by_owner
            .entry(path_text(&owner))
            .or_insert_with(|| make_variable_include_roots(&owner, &variables))
            .clone();
        for include in make_include_roots(&scan.root, &owner, &source, &owner_includes) {
            includes.insert(path_text(&include));
            arguments.push(format!("-I{}", path_text(&include)));
        }
        for define in defines.iter() {
            if define.starts_with("CONFIG_") {
                arguments.push(format!("-D{define}"));
            }
        }
        arguments.push("-c".into());
        arguments.push(path_text(&source));
        arguments.push("-o".into());
        let relative = source.strip_prefix(&scan.root).unwrap_or(&source);
        arguments.push(format!(
            "{}.o",
            path_text(&scan.root.join("build/index-make").join(relative))
        ));
        record_build_system_command(result, &source, &owner, arguments, "make");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::ProjectFile;
    use std::fs;

    #[test]
    fn expands_variables() {
        let mut vars = BTreeMap::new();
        vars.insert("A".to_string(), "x".to_string());
        assert_eq!(expand_make_variables("$(A)/${A}/$(B)", &vars), "x/x/$(B)");
    }

    #[test]
    fn make_project_commands() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("include")).unwrap();
        fs::write(
            root.join("Makefile"),
            "INC = include\nCFLAGS = -O2 -I$(INC) -DMODE=3 $(shell x)\n",
        )
        .unwrap();
        fs::write(
            root.join(".config"),
            "CONFIG_FOO=y\nCONFIG_BAR=n\nCONFIG_N=4\n",
        )
        .unwrap();
        fs::write(root.join("src/a.c"), "int a;\n").unwrap();
        let scan = ProjectScanResult {
            root: root.clone(),
            files: vec![
                ProjectFile {
                    path: "Makefile".into(),
                    kind: ProjectFileKind::BuildFile,
                },
                ProjectFile {
                    path: "src/a.c".into(),
                    kind: ProjectFileKind::CSource,
                },
            ],
            ..Default::default()
        };
        let mut result = BuildEmulationResult::default();
        let mut includes = BTreeSet::new();
        let mut defines = BTreeSet::new();
        emulate_make_projects(&mut result, &mut includes, &mut defines, &scan);
        let command = &result.compile_commands[&path_text(&root.join("src/a.c"))];
        assert_eq!(command.backend, "make");
        assert_eq!(command.confidence, 55);
        assert_eq!(&command.arguments[..3], &["cc", "-std=gnu11", "-O2"]);
        assert!(
            command
                .arguments
                .contains(&format!("-I{}", root.join("include").display()))
        );
        assert!(command.arguments.contains(&"-DMODE=3".to_string()));
        assert!(command.arguments.contains(&"-DCONFIG_FOO=1".to_string()));
        assert!(command.arguments.contains(&"-DCONFIG_N=4".to_string()));
        assert!(!command.arguments.iter().any(|a| a.contains("shell")));
        assert!(defines.contains("CONFIG_FOO=1"));
    }
}
