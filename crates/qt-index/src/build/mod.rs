//! Build-system emulation and build metadata.
//!
//! The emulators read build files and generated build trees (CMake Makefile
//! and Ninja generators, plain Make, qmake, waf, PlatformIO, Cargo, npm and
//! Python projects) and synthesise per-source compile commands without running
//! any build tool. [`metadata::scan_build_metadata`] merges them with existing
//! `compile_commands.json` files and include/define hints from build files.

mod cmake;
mod languages;
mod make;
pub mod metadata;
mod ninja;
mod platformio;
mod qmake;
mod waf;

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use crate::cancel::{Cancelled, Pacer};
use crate::scan::{ProjectFile, ProjectFileKind, ProjectScanResult};
use crate::util::{
    entry_is_dir, extension_lower, file_name, is_ignored_dependency_path, lexically_normal,
    normalize_path, path_text, sorted_dir_entries, trim_ascii, weakly_canonical,
};

pub use metadata::{BuildMetadata, build_metadata_summary, scan_build_metadata};

/// One compile command recovered from a build system or compilation database.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ObservedCompileCommand {
    pub directory: PathBuf,
    pub arguments: Vec<String>,
    pub backend: String,
    /// `observed` (existing compilation database) or `emulated`.
    pub origin: String,
    pub confidence: u32,
}

#[derive(Debug, Clone, Default)]
pub struct BuildEmulationResult {
    /// Keyed by absolute normalised source path.
    pub compile_commands: BTreeMap<String, ObservedCompileCommand>,
    pub include_paths: Vec<PathBuf>,
    pub defines: Vec<String>,
    pub edges_by_backend: BTreeMap<String, usize>,
}

/// Lookup tables over a scan (files by kind, build files by name/extension).
pub(crate) struct ProjectIndex<'a> {
    files_by_kind: BTreeMap<ProjectFileKind, Vec<&'a ProjectFile>>,
    build_files_by_name: BTreeMap<String, Vec<PathBuf>>,
    build_files_by_extension: BTreeMap<String, Vec<PathBuf>>,
}

impl<'a> ProjectIndex<'a> {
    pub(crate) fn new(scan: &'a ProjectScanResult) -> Self {
        let mut index = Self {
            files_by_kind: BTreeMap::new(),
            build_files_by_name: BTreeMap::new(),
            build_files_by_extension: BTreeMap::new(),
        };
        for file in &scan.files {
            index.files_by_kind.entry(file.kind).or_default().push(file);
            if file.kind != ProjectFileKind::BuildFile {
                continue;
            }
            let absolute = lexically_normal(&scan.root.join(&file.path));
            index
                .build_files_by_name
                .entry(file_name(&file.path))
                .or_default()
                .push(absolute.clone());
            index
                .build_files_by_extension
                .entry(extension_lower(&file.path))
                .or_default()
                .push(absolute);
        }
        index
    }

    pub(crate) fn files(&self, kind: ProjectFileKind) -> &[&'a ProjectFile] {
        self.files_by_kind
            .get(&kind)
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    fn build_files_named(&self, name: &str) -> &[PathBuf] {
        self.build_files_by_name
            .get(name)
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }

    fn build_files_with_extension(&self, ext: &str) -> &[PathBuf] {
        self.build_files_by_extension
            .get(&ext.to_ascii_lowercase())
            .map(Vec::as_slice)
            .unwrap_or(&[])
    }
}

pub(crate) fn is_source_path(path: &Path) -> bool {
    matches!(
        extension_lower(path).as_str(),
        ".c" | ".cc" | ".cpp" | ".cxx" | ".c++" | ".ino"
    )
}

pub(crate) fn is_c_source_path(path: &Path) -> bool {
    extension_lower(path) == ".c"
}

/// Parse a JSON-style double-quoted string starting at `pos`; on success
/// `pos` points just past the closing quote.
pub(crate) fn parse_quoted_string_at(text: &[u8], pos: &mut usize) -> Option<String> {
    if *pos >= text.len() || text[*pos] != b'"' {
        return None;
    }
    let mut value: Vec<u8> = Vec::new();
    let mut escaped = false;
    *pos += 1;
    while *pos < text.len() {
        let ch = text[*pos];
        if escaped {
            match ch {
                b'b' => value.push(8),
                b'f' => value.push(12),
                b'n' => value.push(b'\n'),
                b'r' => value.push(b'\r'),
                b't' => value.push(b'\t'),
                other => value.push(other),
            }
            escaped = false;
        } else if ch == b'\\' {
            escaped = true;
        } else if ch == b'"' {
            *pos += 1;
            return Some(String::from_utf8_lossy(&value).into_owned());
        } else {
            value.push(ch);
        }
        *pos += 1;
    }
    None
}

/// `KEY = value` / `KEY += value` / `KEY := value` / `KEY ?= value` /
/// `KEY != value` with `#` comments stripped.
pub(crate) fn parse_assignment(line: &str) -> Option<(String, String)> {
    let mut line = trim_ascii(line);
    if let Some(comment) = line.find('#') {
        line = trim_ascii(&line[..comment]);
    }
    let equals = line.find('=')?;
    let mut key = trim_ascii(&line[..equals]).to_string();
    if let Some(last) = key.chars().last()
        && matches!(last, '+' | ':' | '?' | '!')
    {
        key.pop();
        key = trim_ascii(&key).to_string();
    }
    Some((key, trim_ascii(&line[equals + 1..]).to_string()))
}

/// Strip `;`/`#` comments that start a line or follow whitespace.
pub(crate) fn strip_ini_comment(line: &str) -> String {
    let bytes = line.as_bytes();
    let mut end = bytes.len();
    for (index, ch) in bytes.iter().enumerate() {
        if *ch != b';' && *ch != b'#' {
            continue;
        }
        if index == 0 || bytes[index - 1].is_ascii_whitespace() {
            end = index;
            break;
        }
    }
    trim_ascii(&line[..end]).to_string()
}

pub(crate) fn add_define(defines: &mut BTreeSet<String>, value: &str) {
    let value = value.strip_prefix("-D").unwrap_or(value);
    let value = trim_ascii(value);
    if value.is_empty() || value.contains('$') || value.contains('/') {
        return;
    }
    let lead = value.as_bytes()[0];
    if !lead.is_ascii_alphabetic() && lead != b'_' {
        return;
    }
    defines.insert(value.to_string());
}

pub(crate) fn add_include(includes: &mut BTreeSet<String>, base: &Path, value: &str) {
    let value = value.strip_prefix("-I").unwrap_or(value);
    let value = trim_ascii(value);
    if value.is_empty() || value == "SYSTEM" || value.contains('$') {
        return;
    }
    let path = normalize_path(base, value);
    if path.is_dir() {
        includes.insert(path_text(&path));
    }
}

pub(crate) fn scan_tokens_for_metadata(
    tokens: &[String],
    base: &Path,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let mut index = 0;
    while index < tokens.len() {
        let token = tokens[index].as_str();
        if matches!(token, "-I" | "-isystem" | "/I") && index + 1 < tokens.len() {
            index += 1;
            add_include(includes, base, &tokens[index]);
        } else if let Some(rest) = token.strip_prefix("-I") {
            add_include(includes, base, rest);
        } else if let Some(rest) = token.strip_prefix("/I") {
            add_include(includes, base, rest);
        } else if matches!(token, "-D" | "/D") && index + 1 < tokens.len() {
            index += 1;
            add_define(defines, &tokens[index]);
        } else if let Some(rest) = token.strip_prefix("-D") {
            add_define(defines, rest);
        } else if let Some(rest) = token.strip_prefix("/D") {
            add_define(defines, rest);
        }
        index += 1;
    }
}

/// Copy compile tokens, resolving include directories against `base`.
pub(crate) fn append_compile_tokens(arguments: &mut Vec<String>, tokens: &[String], base: &Path) {
    let mut index = 0;
    while index < tokens.len() {
        let token = tokens[index].as_str();
        if matches!(token, "-I" | "-isystem" | "/I") && index + 1 < tokens.len() {
            arguments.push(token.to_string());
            index += 1;
            arguments.push(path_text(&normalize_path(base, &tokens[index])));
        } else if token.len() > 2 && token.starts_with("-I") {
            arguments.push(format!(
                "-I{}",
                path_text(&normalize_path(base, &token[2..]))
            ));
        } else if token.len() > 2 && token.starts_with("/I") {
            arguments.push(format!(
                "/I{}",
                path_text(&normalize_path(base, &token[2..]))
            ));
        } else {
            arguments.push(token.to_string());
        }
        index += 1;
    }
}

/// Add an existing directory (canonicalised) once.
pub(crate) fn add_existing_include(
    includes: &mut Vec<PathBuf>,
    seen: &mut BTreeSet<String>,
    path: &Path,
) {
    let normalized = weakly_canonical(path);
    if normalized.is_dir() && seen.insert(path_text(&normalized)) {
        includes.push(normalized);
    }
}

fn project_build_files(index: &ProjectIndex<'_>, filename: &str) -> Vec<PathBuf> {
    let mut result: Vec<PathBuf> = index
        .build_files_named(filename)
        .iter()
        .filter(|p| !is_ignored_dependency_path(p))
        .cloned()
        .collect();
    result.sort();
    result
}

fn qmake_project_files(index: &ProjectIndex<'_>) -> Vec<PathBuf> {
    let mut result: Vec<PathBuf> = [".pro", ".pri", ".prf"]
        .iter()
        .flat_map(|ext| index.build_files_with_extension(ext).iter())
        .filter(|p| !is_ignored_dependency_path(p))
        .cloned()
        .collect();
    result.sort();
    result.dedup();
    result
}

pub(crate) fn record_build_system_command(
    result: &mut BuildEmulationResult,
    source: &Path,
    directory: &Path,
    arguments: Vec<String>,
    backend: &str,
) {
    if !crate::util::is_regular_file(source) {
        return;
    }
    let observed = result
        .compile_commands
        .entry(path_text(&lexically_normal(source)))
        .or_default();
    if observed.arguments.is_empty() {
        *observed = ObservedCompileCommand {
            directory: directory.to_path_buf(),
            arguments,
            backend: backend.to_string(),
            origin: "emulated".into(),
            confidence: if backend == "make" { 55 } else { 75 },
        };
        *result
            .edges_by_backend
            .entry(backend.to_string())
            .or_default() += 1;
    }
}

/// Record a command produced by a generated build tree with a fixed confidence.
pub(crate) fn record_generated_command(
    result: &mut BuildEmulationResult,
    source: &Path,
    command: ObservedCompileCommand,
) {
    let observed = result
        .compile_commands
        .entry(path_text(source))
        .or_default();
    if observed.arguments.is_empty() {
        *result
            .edges_by_backend
            .entry(command.backend.clone())
            .or_default() += 1;
        *observed = command;
    }
}

/// Build directories: parents of `build/*/compile_commands.json` databases and
/// every directory directly under `build/`.
fn build_roots(scan: &ProjectScanResult) -> Vec<PathBuf> {
    let mut roots = BTreeSet::new();
    for relative in &scan.existing_compilation_databases {
        if relative == Path::new("compile_commands.json") {
            continue;
        }
        if let Some(parent) = scan.root.join(relative).parent() {
            roots.insert(path_text(&lexically_normal(parent)));
        }
    }
    let build_dir = scan.root.join("build");
    if build_dir.is_dir() {
        for entry in sorted_dir_entries(&build_dir) {
            if entry_is_dir(&entry) {
                roots.insert(path_text(&lexically_normal(&entry.path())));
            }
        }
    }
    roots.into_iter().map(PathBuf::from).collect()
}

/// Run every emulator over the scan.
pub fn emulate_build_systems(
    scan: &ProjectScanResult,
    pacer: &Pacer,
) -> Result<BuildEmulationResult, Cancelled> {
    let mut result = BuildEmulationResult::default();
    let index = ProjectIndex::new(scan);
    let mut includes = BTreeSet::new();
    let mut defines = BTreeSet::new();

    for root in build_roots(scan) {
        let build_ninja = root.join("build.ninja");
        if crate::util::is_regular_file(&build_ninja) {
            ninja::emulate_ninja(&mut result, &mut includes, &mut defines, &build_ninja);
            pacer.checkpoint(0, 0, 10)?;
        }
        cmake::emulate_cmake_make(&mut result, &mut includes, &mut defines, &root);
        pacer.checkpoint(0, 0, 10)?;
    }
    languages::emulate_cargo_projects(&mut result, scan, &index);
    pacer.checkpoint(0, 0, 10)?;
    languages::emulate_npm_projects(&mut result, scan, &index);
    pacer.checkpoint(0, 0, 10)?;
    platformio::emulate_platformio_projects(&mut result, &mut includes, &mut defines, scan, &index);
    pacer.checkpoint(0, 0, 10)?;
    qmake::emulate_qmake_projects(&mut result, &mut includes, &mut defines, &index);
    pacer.checkpoint(0, 0, 10)?;
    languages::emulate_python_projects(&mut result, scan, &index);
    pacer.checkpoint(0, 0, 10)?;
    waf::emulate_waf_projects(&mut result, &mut includes, scan);
    pacer.checkpoint(0, 0, 10)?;
    make::emulate_make_projects(&mut result, &mut includes, &mut defines, scan);
    pacer.checkpoint(0, 0, 10)?;

    result.include_paths = includes.into_iter().map(PathBuf::from).collect();
    result.defines = defines.into_iter().collect();
    Ok(result)
}

/// One-line summary of an emulation result.
pub fn build_emulation_summary(emulation: &BuildEmulationResult) -> String {
    let backends: Vec<String> = emulation
        .edges_by_backend
        .iter()
        .map(|(b, c)| format!("{b}:{c}"))
        .collect();
    format!(
        "commands={} include_paths={} defines={} backends={}",
        emulation.compile_commands.len(),
        emulation.include_paths.len(),
        emulation.defines.len(),
        backends.join(",")
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn assignment_parsing() {
        assert_eq!(
            parse_assignment("CFLAGS += -O2 # opt"),
            Some(("CFLAGS".into(), "-O2".into()))
        );
        assert_eq!(parse_assignment("X := 1"), Some(("X".into(), "1".into())));
        assert_eq!(parse_assignment("no assignment"), None);
        assert_eq!(strip_ini_comment("a = b ; c"), "a = b");
        assert_eq!(strip_ini_comment("a=b;c"), "a=b;c");
    }

    #[test]
    fn quoted_strings() {
        let text = br#"x "a\"b\n" y"#;
        let mut pos = 2;
        assert_eq!(
            parse_quoted_string_at(text, &mut pos),
            Some("a\"b\n".into())
        );
        assert_eq!(text[pos], b' ');
    }

    #[test]
    fn defines_are_filtered() {
        let mut defines = BTreeSet::new();
        add_define(&mut defines, "-DFOO=1");
        add_define(&mut defines, "1BAD");
        add_define(&mut defines, "$(X)");
        add_define(&mut defines, "a/b");
        assert_eq!(defines.into_iter().collect::<Vec<_>>(), vec!["FOO=1"]);
    }
}
