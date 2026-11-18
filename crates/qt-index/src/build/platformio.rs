//! PlatformIO projects: `platformio.ini` environments, `build_flags`,
//! `build_src_filter` and `.pio/libdeps/<env>` libraries.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use super::{
    BuildEmulationResult, ProjectIndex, add_existing_include, append_compile_tokens,
    is_c_source_path, is_source_path, parse_assignment, project_build_files,
    record_build_system_command, scan_tokens_for_metadata, strip_ini_comment,
};
use crate::scan::ProjectScanResult;
use crate::util::{
    entry_is_dir, entry_is_file, file_name, getlines, is_ignored_dependency_path, lexically_normal,
    path_has_prefix, path_text, read_text_file, sorted_dir_entries, split_shell_like, trim_ascii,
};

type Sections = BTreeMap<String, BTreeMap<String, String>>;

fn parse_platformio_ini(path: &Path) -> Sections {
    let mut sections = Sections::new();
    let text = read_text_file(path);
    let mut section = String::new();
    let mut current_key = String::new();
    for line in getlines(&text) {
        let continuation = !current_key.is_empty()
            && line
                .as_bytes()
                .first()
                .is_some_and(|b| b.is_ascii_whitespace());
        let stripped = strip_ini_comment(line);
        if stripped.is_empty() {
            continue;
        }
        if stripped.starts_with('[') && stripped.ends_with(']') && stripped.len() >= 2 {
            section = trim_ascii(&stripped[1..stripped.len() - 1]).to_string();
            current_key.clear();
            continue;
        }
        if section.is_empty() {
            continue;
        }
        if continuation {
            let value = sections
                .entry(section.clone())
                .or_default()
                .entry(current_key.clone())
                .or_default();
            if !value.is_empty() {
                value.push(' ');
            }
            value.push_str(&stripped);
            continue;
        }
        let Some((key, assigned)) = parse_assignment(&stripped) else {
            continue;
        };
        current_key = key.clone();
        let value = sections
            .entry(section.clone())
            .or_default()
            .entry(key)
            .or_default();
        if !value.is_empty() {
            value.push(' ');
        }
        value.push_str(&assigned);
    }
    sections
}

fn expand_value(sections: &Sections, current_section: &str, value: &str, depth: u32) -> String {
    let mut value = value.to_string();
    if depth > 12 {
        return value;
    }
    let mut pos = 0;
    while pos < value.len() {
        let Some(start) = value[pos..].find("${").map(|i| i + pos) else {
            break;
        };
        let Some(end) = value[start + 2..].find('}').map(|i| i + start + 2) else {
            break;
        };
        let key = value[start + 2..end].to_string();
        let (mut section_name, option_name) = match key.find('.') {
            None => (current_section.to_string(), key.clone()),
            Some(dot) => (key[..dot].to_string(), key[dot + 1..].to_string()),
        };
        if section_name == "this" {
            section_name = current_section.to_string();
        }
        if option_name == "__env__"
            && let Some(env_name) = current_section.strip_prefix("env:")
        {
            value.replace_range(start..=end, env_name);
            pos = start + env_name.len();
            continue;
        }
        let Some(option) = sections
            .get(&section_name)
            .and_then(|s| s.get(&option_name))
        else {
            pos = end + 1;
            continue;
        };
        let replacement = expand_value(sections, &section_name, option, depth + 1);
        value.replace_range(start..=end, &replacement);
        pos = start + replacement.len();
    }
    value
}

fn default_env_sections(sections: &Sections) -> Vec<String> {
    let mut result: Vec<String> = Vec::new();
    if let Some(default_envs) = sections
        .get("platformio")
        .and_then(|s| s.get("default_envs"))
    {
        for env in split_shell_like(default_envs) {
            let env: String = env.chars().filter(|c| *c != ',').collect();
            let env = trim_ascii(&env);
            if !env.is_empty() {
                result.push(format!("env:{env}"));
            }
        }
    }
    if result.is_empty() {
        result = sections
            .keys()
            .filter(|s| s.starts_with("env:"))
            .cloned()
            .collect();
    }
    result.sort();
    result.dedup();
    result
}

fn combined_value(sections: &Sections, env_section: &str, key: &str) -> String {
    let mut value = String::new();
    for section_name in ["env", env_section] {
        let Some(option) = sections.get(section_name).and_then(|s| s.get(key)) else {
            continue;
        };
        let expanded = expand_value(sections, section_name, option, 0);
        if !value.is_empty() {
            value.push(' ');
        }
        value.push_str(&expanded);
    }
    value
}

fn filter_to_glob(project_root: &Path, pattern: &str) -> PathBuf {
    let pattern = trim_ascii(pattern);
    if pattern.starts_with('/') {
        return lexically_normal(Path::new(pattern));
    }
    lexically_normal(&project_root.join("src").join(pattern))
}

fn pattern_matches(project_root: &Path, source: &Path, pattern: &str) -> bool {
    let glob = path_text(&filter_to_glob(project_root, pattern));
    let glob = glob.as_bytes();
    let text = path_text(source);
    let text = text.as_bytes();
    let (mut p, mut t) = (0usize, 0usize);
    let mut star: Option<usize> = None;
    let mut star_t = 0usize;
    while t < text.len() {
        if p < glob.len() && (glob[p] == b'?' || glob[p] == text[t]) {
            p += 1;
            t += 1;
        } else if p < glob.len() && glob[p] == b'*' {
            star = Some(p);
            p += 1;
            star_t = t;
        } else if let Some(s) = star {
            p = s + 1;
            star_t += 1;
            t = star_t;
        } else {
            return false;
        }
    }
    while p < glob.len() && glob[p] == b'*' {
        p += 1;
    }
    p == glob.len()
}

fn source_selected(project_root: &Path, source: &Path, filter_text: &str) -> bool {
    let mut includes = Vec::new();
    let mut excludes = Vec::new();
    for token in split_shell_like(filter_text) {
        let token = trim_ascii(&token);
        let bytes = token.as_bytes();
        if bytes.len() < 4 || bytes[1] != b'<' || !token.ends_with('>') {
            continue;
        }
        let pattern = token[2..token.len() - 1].to_string();
        match bytes[0] {
            b'+' => includes.push(pattern),
            b'-' => excludes.push(pattern),
            _ => {}
        }
    }
    let included = includes.is_empty()
        || includes
            .iter()
            .any(|p| pattern_matches(project_root, source, p));
    included
        && !excludes
            .iter()
            .any(|p| pattern_matches(project_root, source, p))
}

fn should_skip_libdep_dir(path: &Path) -> bool {
    matches!(
        file_name(path).to_ascii_lowercase().as_str(),
        ".git"
            | ".github"
            | ".vscode"
            | "example"
            | "examples"
            | "test"
            | "tests"
            | "docs"
            | "doc"
            | "scripts"
            | "extras"
    )
}

fn libdep_roots(project_root: &Path, env_name: &str) -> Vec<PathBuf> {
    let libdeps = project_root.join(".pio").join("libdeps").join(env_name);
    if !libdeps.is_dir() {
        return Vec::new();
    }
    let mut roots: Vec<PathBuf> = sorted_dir_entries(&libdeps)
        .into_iter()
        .filter(|e| entry_is_dir(e) && !should_skip_libdep_dir(&e.path()))
        .map(|e| lexically_normal(&e.path()))
        .collect();
    roots.sort();
    roots
}

fn libdep_include_roots(lib_roots: &[PathBuf]) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let mut seen = BTreeSet::new();
    for root in lib_roots {
        add_existing_include(&mut paths, &mut seen, &root.join("include"));
        add_existing_include(&mut paths, &mut seen, &root.join("src"));
        add_existing_include(&mut paths, &mut seen, root);
    }
    paths
}

fn libdep_sources(lib_roots: &[PathBuf]) -> Vec<PathBuf> {
    let mut sources = Vec::new();
    for root in lib_roots {
        let src = root.join("src");
        if src.is_dir() {
            let _ = crate::util::walk(&src, |path, is_dir, _| {
                if is_dir {
                    return Ok(!should_skip_libdep_dir(path));
                }
                if crate::util::is_regular_file(path) && is_source_path(path) {
                    sources.push(lexically_normal(path));
                }
                Ok(true)
            });
            continue;
        }
        for entry in sorted_dir_entries(root) {
            if entry_is_file(&entry) && is_source_path(&entry.path()) {
                sources.push(lexically_normal(&entry.path()));
            }
        }
    }
    sources.sort();
    sources.dedup();
    sources
}

#[allow(clippy::too_many_arguments)]
fn record_source(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    project_root: &Path,
    env_name: &str,
    source: &Path,
    flag_tokens: &[String],
    include_paths: &[PathBuf],
    backend: &str,
) {
    let c = is_c_source_path(source);
    let mut arguments = vec![
        if c { "cc" } else { "c++" }.to_string(),
        if c { "-std=gnu11" } else { "-std=gnu++20" }.to_string(),
    ];
    append_compile_tokens(&mut arguments, flag_tokens, project_root);
    for include in include_paths {
        includes.insert(path_text(include));
        arguments.push(format!("-I{}", path_text(include)));
    }
    arguments.push("-c".into());
    arguments.push(path_text(source));
    arguments.push("-o".into());
    let relative = source
        .strip_prefix(project_root)
        .map(Path::to_path_buf)
        .unwrap_or_else(|_| relative_fallback(source, project_root));
    arguments.push(format!(
        "{}.o",
        path_text(
            &project_root
                .join("build")
                .join("index-platformio")
                .join(env_name)
                .join(relative)
        )
    ));
    record_build_system_command(result, source, project_root, arguments, backend);
}

/// `std::filesystem::relative` for a path outside `base`: climb with `..`.
fn relative_fallback(path: &Path, base: &Path) -> PathBuf {
    let path_parts: Vec<_> = path.components().collect();
    let base_parts: Vec<_> = base.components().collect();
    let common = path_parts
        .iter()
        .zip(base_parts.iter())
        .take_while(|(a, b)| a == b)
        .count();
    let mut result = PathBuf::new();
    for _ in common..base_parts.len() {
        result.push("..");
    }
    for part in &path_parts[common..] {
        result.push(part.as_os_str());
    }
    result
}

fn emulate_project(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    scan: &ProjectScanResult,
    platformio_ini: &Path,
) {
    let sections = parse_platformio_ini(platformio_ini);
    let env_sections = default_env_sections(&sections);
    if env_sections.is_empty() {
        return;
    }
    let project_root = platformio_ini
        .parent()
        .unwrap_or(Path::new(""))
        .to_path_buf();
    for env_section in &env_sections {
        let env_name = env_section.strip_prefix("env:").unwrap_or(env_section);
        let flag_text = combined_value(&sections, env_section, "build_flags");
        let lib_deps_text = combined_value(&sections, env_section, "lib_deps");
        let filter_text = combined_value(&sections, env_section, "build_src_filter");
        let flag_tokens: Vec<String> = split_shell_like(&flag_text)
            .into_iter()
            .filter(|t| !(t.contains('$') || t.starts_with("-Wl,") || t.starts_with("-L")))
            .collect();
        scan_tokens_for_metadata(&flag_tokens, &project_root, includes, defines);

        let generated_libdeps = project_root.join(".pio").join("libdeps").join(env_name);
        let scan_libdeps = !trim_ascii(&lib_deps_text).is_empty() || generated_libdeps.is_dir();
        let lib_roots = if scan_libdeps {
            libdep_roots(&project_root, env_name)
        } else {
            Vec::new()
        };
        let lib_includes = libdep_include_roots(&lib_roots);

        for file in &scan.files {
            if !file.kind.is_native_source() || is_ignored_dependency_path(&file.path) {
                continue;
            }
            let source = lexically_normal(&scan.root.join(&file.path));
            if !path_has_prefix(&source, &project_root) {
                continue;
            }
            if !filter_text.is_empty() && !source_selected(&project_root, &source, &filter_text) {
                continue;
            }
            let mut source_includes = Vec::new();
            let mut seen = BTreeSet::new();
            add_existing_include(&mut source_includes, &mut seen, &project_root);
            add_existing_include(&mut source_includes, &mut seen, &project_root.join("src"));
            add_existing_include(
                &mut source_includes,
                &mut seen,
                &project_root.join("include"),
            );
            let source_dir = source.parent().unwrap_or(Path::new(""));
            add_existing_include(&mut source_includes, &mut seen, source_dir);
            let mut dir = Some(source_dir);
            while let Some(current) = dir {
                if current.as_os_str().is_empty() || !path_has_prefix(current, &project_root) {
                    break;
                }
                add_existing_include(&mut source_includes, &mut seen, current);
                if current == project_root {
                    break;
                }
                dir = current.parent();
            }
            source_includes.extend(lib_includes.iter().cloned());
            record_source(
                result,
                includes,
                &project_root,
                env_name,
                &source,
                &flag_tokens,
                &source_includes,
                "platformio",
            );
        }

        for source in libdep_sources(&lib_roots) {
            let mut source_includes = lib_includes.clone();
            let mut seen: BTreeSet<String> = source_includes.iter().map(|p| path_text(p)).collect();
            add_existing_include(
                &mut source_includes,
                &mut seen,
                source.parent().unwrap_or(Path::new("")),
            );
            record_source(
                result,
                includes,
                &project_root,
                env_name,
                &source,
                &flag_tokens,
                &source_includes,
                "platformio-libdeps",
            );
        }
    }
}

pub(super) fn emulate_platformio_projects(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    scan: &ProjectScanResult,
    index: &ProjectIndex<'_>,
) {
    for ini in project_build_files(index, "platformio.ini") {
        emulate_project(result, includes, defines, scan, &ini);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::{ProjectFile, ProjectFileKind};
    use std::fs;

    #[test]
    fn ini_parsing_and_expansion() {
        let dir = tempfile::tempdir().unwrap();
        let ini = dir.path().join("platformio.ini");
        fs::write(
            &ini,
            "[platformio]\ndefault_envs = native\n\n[common]\nflags = -DCOMMON=1 ; comment\n\n[env:native]\nbuild_flags =\n    ${common.flags}\n    -DENV=${this.__env__}\n",
        )
        .unwrap();
        let sections = parse_platformio_ini(&ini);
        assert_eq!(default_env_sections(&sections), vec!["env:native"]);
        assert_eq!(
            combined_value(&sections, "env:native", "build_flags"),
            "-DCOMMON=1 -DENV=native"
        );
    }

    #[test]
    fn src_filter_selects_sources() {
        let root = Path::new("/p");
        assert!(source_selected(
            root,
            Path::new("/p/src/a.cpp"),
            "+<*> -<test/>"
        ));
        assert!(!source_selected(
            root,
            Path::new("/p/src/x/b.cpp"),
            "+<a.cpp>"
        ));
        assert!(!source_selected(
            root,
            Path::new("/p/src/skip/b.cpp"),
            "+<*> -<skip/*>"
        ));
    }

    #[test]
    fn platformio_project_commands() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join(".pio/libdeps/native/Lib/src")).unwrap();
        fs::write(root.join("src/main.cpp"), "int main(){}\n").unwrap();
        fs::write(root.join(".pio/libdeps/native/Lib/src/lib.cpp"), "int l;\n").unwrap();
        fs::write(
            root.join("platformio.ini"),
            "[env:native]\nplatform = native\nbuild_flags = -std=c++17 -DPIO=1 -Wl,--gc -L/x\n",
        )
        .unwrap();
        let scan = ProjectScanResult {
            root: root.clone(),
            files: vec![
                ProjectFile {
                    path: "platformio.ini".into(),
                    kind: ProjectFileKind::BuildFile,
                },
                ProjectFile {
                    path: "src/main.cpp".into(),
                    kind: ProjectFileKind::CxxSource,
                },
            ],
            ..Default::default()
        };
        let index = ProjectIndex::new(&scan);
        let mut result = BuildEmulationResult::default();
        let mut includes = BTreeSet::new();
        let mut defines = BTreeSet::new();
        emulate_platformio_projects(&mut result, &mut includes, &mut defines, &scan, &index);
        let main = &result.compile_commands[&path_text(&root.join("src/main.cpp"))];
        assert_eq!(main.backend, "platformio");
        assert_eq!(
            &main.arguments[..4],
            &["c++", "-std=gnu++20", "-std=c++17", "-DPIO=1"]
        );
        assert!(
            !main
                .arguments
                .iter()
                .any(|a| a.starts_with("-Wl,") || a == "-L/x")
        );
        assert!(
            main.arguments
                .last()
                .unwrap()
                .ends_with("build/index-platformio/native/src/main.cpp.o")
        );
        assert!(main.arguments.contains(&format!(
            "-I{}",
            root.join(".pio/libdeps/native/Lib/src").display()
        )));
        let lib =
            &result.compile_commands[&path_text(&root.join(".pio/libdeps/native/Lib/src/lib.cpp"))];
        assert_eq!(lib.backend, "platformio-libdeps");
        assert!(defines.contains("PIO=1"));
    }
}
