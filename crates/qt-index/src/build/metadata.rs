//! Build metadata: include paths, defines and observed compile commands.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use serde_json::Value;

use super::{ObservedCompileCommand, emulate_build_systems};
use crate::cancel::{Cancelled, Pacer};
use crate::scan::{ProjectFileKind, ProjectScanResult};
use crate::util::{
    file_name, getlines, normalize_path, path_text, read_text_file, split_shell_like, trim_ascii,
    weakly_canonical,
};

#[derive(Debug, Clone, Default)]
pub struct BuildMetadata {
    pub include_paths: Vec<PathBuf>,
    pub defines: Vec<String>,
    /// Keyed by absolute normalised source path.
    pub observed_compile_commands: BTreeMap<String, ObservedCompileCommand>,
    pub emulated_edges_by_backend: BTreeMap<String, usize>,
    pub build_files_scanned_by_family: BTreeMap<String, usize>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Family {
    CMake,
    Make,
    Ninja,
    Meson,
    Autotools,
    Bazel,
    Buck,
    QMake,
    SCons,
    Premake,
    Xmake,
    Waf,
    Gn,
    MsBuild,
    Xcode,
    Embedded,
    Package,
    Nix,
    Container,
}

impl Family {
    fn name(self) -> &'static str {
        match self {
            Self::CMake => "CMake",
            Self::Make => "Make",
            Self::Ninja => "Ninja",
            Self::Meson => "Meson",
            Self::Autotools => "Autotools",
            Self::Bazel => "Bazel",
            Self::Buck => "Buck",
            Self::QMake => "QMake",
            Self::SCons => "SCons",
            Self::Premake => "Premake",
            Self::Xmake => "Xmake",
            Self::Waf => "Waf",
            Self::Gn => "GN",
            Self::MsBuild => "MSBuild",
            Self::Xcode => "Xcode",
            Self::Embedded => "Embedded",
            Self::Package => "Package",
            Self::Nix => "Nix",
            Self::Container => "Container",
        }
    }

    fn scan_limit(self) -> usize {
        match self {
            Self::CMake => 400,
            Self::Make => 300,
            Self::Ninja => 200,
            Self::MsBuild | Self::Xcode => 100,
            _ => 120,
        }
    }
}

/// Family for metadata scanning. Unlike [`crate::scan::build_file_type`],
/// Cargo, npm and Python manifests have no family and are not scanned.
fn build_file_family(path: &Path) -> Option<Family> {
    Some(match crate::scan::build_file_type(path)? {
        "CMake" => Family::CMake,
        "Make" => Family::Make,
        "Ninja" => Family::Ninja,
        "Meson" => Family::Meson,
        "Autotools" => Family::Autotools,
        "Bazel" => Family::Bazel,
        "Buck" => Family::Buck,
        "QMake" => Family::QMake,
        "SCons" => Family::SCons,
        "Premake" => Family::Premake,
        "Xmake" => Family::Xmake,
        "Waf" => Family::Waf,
        "GN" => Family::Gn,
        "MSBuild" => Family::MsBuild,
        "Xcode" => Family::Xcode,
        "Embedded" => Family::Embedded,
        "Package" => Family::Package,
        "Nix" => Family::Nix,
        "Container" => Family::Container,
        _ => return None,
    })
}

fn is_source_or_build_directory_skip(path: &Path) -> bool {
    path.components().any(|part| {
        matches!(
            part.as_os_str().to_string_lossy().as_ref(),
            ".git"
                | ".hg"
                | ".svn"
                | ".pio"
                | ".idea"
                | ".vscode"
                | "node_modules"
                | "__pycache__"
                | "third_party"
                | "vendor"
                | "external"
        )
    })
}

fn strip_inline_comment(line: &str, marker: u8) -> &str {
    let bytes = line.as_bytes();
    let mut quote = 0u8;
    let mut escaped = false;
    for (index, ch) in bytes.iter().copied().enumerate() {
        if escaped {
            escaped = false;
            continue;
        }
        if ch == b'\\' {
            escaped = true;
            continue;
        }
        if quote != 0 {
            if ch == quote {
                quote = 0;
            }
            continue;
        }
        if ch == b'"' || ch == b'\'' {
            quote = ch;
            continue;
        }
        if ch == marker {
            return &line[..index];
        }
    }
    line
}

fn normalize_token_separators(text: &str) -> String {
    let mut quote: Option<char> = None;
    let mut escaped = false;
    text.chars()
        .map(|ch| {
            if escaped {
                escaped = false;
                return ch;
            }
            if ch == '\\' {
                escaped = true;
                return ch;
            }
            if let Some(q) = quote {
                if ch == q {
                    quote = None;
                }
                return ch;
            }
            if ch == '"' || ch == '\'' {
                quote = Some(ch);
                return ch;
            }
            if matches!(ch, '(' | ')' | '[' | ']' | '{' | '}' | ',' | ';') {
                ' '
            } else {
                ch
            }
        })
        .collect()
}

/// Words separated by blanks (space/tab). As with the archived lexy grammar,
/// any byte that is neither printable ASCII nor a blank fails the parse and
/// yields no words.
fn parse_words(text: &str) -> Vec<String> {
    let text = normalize_token_separators(text);
    if text
        .bytes()
        .any(|b| !(b.is_ascii_graphic() || b == b' ' || b == b'\t'))
    {
        return Vec::new();
    }
    text.split([' ', '\t'])
        .filter(|w| !w.is_empty())
        .map(str::to_string)
        .collect()
}

/// `key = value` or `key: value` where the key is an identifier allowing `-`
/// and `.`, and the value is non-empty printable ASCII.
fn parse_metadata_assignment(line: &str) -> Option<(String, String)> {
    let line = strip_inline_comment(line, b'#');
    let bytes = line.as_bytes();
    let blank = |b: u8| b == b' ' || b == b'\t';
    let mut pos = 0;
    while pos < bytes.len() && blank(bytes[pos]) {
        pos += 1;
    }
    let key_start = pos;
    if pos >= bytes.len() || !(bytes[pos].is_ascii_alphabetic() || bytes[pos] == b'_') {
        return None;
    }
    pos += 1;
    while pos < bytes.len()
        && (bytes[pos].is_ascii_alphanumeric() || matches!(bytes[pos], b'_' | b'-' | b'.'))
    {
        pos += 1;
    }
    let key = &line[key_start..pos];
    while pos < bytes.len() && blank(bytes[pos]) {
        pos += 1;
    }
    if pos >= bytes.len() || !(bytes[pos] == b'=' || bytes[pos] == b':') {
        return None;
    }
    pos += 1;
    while pos < bytes.len() && blank(bytes[pos]) {
        pos += 1;
    }
    let value = &line[pos..];
    if value.is_empty() || !value.bytes().all(|b| (0x20..0x7f).contains(&b)) {
        return None;
    }
    Some((key.to_string(), value.to_string()))
}

fn strip_quotes(value: &str) -> &str {
    let b = value.as_bytes();
    if b.len() >= 2
        && ((b[0] == b'"' && b[b.len() - 1] == b'"') || (b[0] == b'\'' && b[b.len() - 1] == b'\''))
    {
        &value[1..value.len() - 1]
    } else {
        value
    }
}

fn add_include_path(paths: &mut BTreeSet<String>, base: &Path, raw: &str) {
    let mut value = strip_quotes(trim_ascii(raw));
    if value.is_empty()
        || matches!(
            value,
            "SYSTEM" | "BEFORE" | "AFTER" | "PUBLIC" | "PRIVATE" | "INTERFACE"
        )
        || value.contains("$<")
        || value.contains("${")
    {
        return;
    }
    if let Some(rest) = value.strip_prefix("-I") {
        value = rest;
    }
    if value.is_empty() {
        return;
    }
    let path = normalize_path(base, value);
    if path.is_dir() {
        paths.insert(path_text(&path));
    }
}

fn add_define(defines: &mut BTreeSet<String>, raw: &str) {
    let mut value = strip_quotes(trim_ascii(raw));
    if value.is_empty() {
        return;
    }
    if let Some(rest) = value.strip_prefix("-D") {
        value = rest;
    }
    let lead_ok = value
        .as_bytes()
        .first()
        .is_some_and(|b| b.is_ascii_alphabetic() || *b == b'_');
    if !lead_ok
        || value.contains("${")
        || value.contains("$<")
        || value.contains(['/', '<', '>', '$', '\'', '"'])
        || value.starts_with("-U")
        || value.starts_with("-include")
        || value.starts_with("MODULE_NAME=")
    {
        return;
    }
    defines.insert(value.to_string());
}

fn resolve_cmake_variables(value: &str, root: &Path, cmake_dir: &Path) -> String {
    let cmake_dir = path_text(cmake_dir);
    let root = path_text(root);
    value
        .replace("${CMAKE_CURRENT_SOURCE_DIR}", &cmake_dir)
        .replace("${CMAKE_CURRENT_LIST_DIR}", &cmake_dir)
        .replace("${PROJECT_SOURCE_DIR}", &root)
        .replace("${CMAKE_SOURCE_DIR}", &root)
        .replace("${PX4_SOURCE_DIR}", &root)
        .replace("$(PROJECT_SOURCE_DIR)", &root)
        .replace("$(CMAKE_SOURCE_DIR)", &root)
        .replace("$(PX4_SOURCE_DIR)", &root)
}

fn record_compile_command(
    scan: &ProjectScanResult,
    object: &Value,
    include_paths: &mut BTreeSet<String>,
    metadata: &mut BuildMetadata,
) {
    let Some(object) = object.as_object() else {
        return;
    };
    let directory = object
        .get("directory")
        .and_then(Value::as_str)
        .map(str::to_string)
        .unwrap_or_else(|| path_text(&scan.root));
    let Some(file) = object.get("file").and_then(Value::as_str) else {
        return;
    };
    let mut tokens: Vec<String> = object
        .get("arguments")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default();
    if tokens.is_empty() {
        let Some(command) = object.get("command").and_then(Value::as_str) else {
            return;
        };
        tokens = split_shell_like(command);
    }
    if tokens.is_empty() {
        return;
    }
    let directory_path = PathBuf::from(&directory);
    let normalized_file = path_text(&normalize_path(&directory_path, file));
    let observed = metadata
        .observed_compile_commands
        .entry(normalized_file)
        .or_default();
    if observed.arguments.is_empty() {
        *observed = ObservedCompileCommand {
            directory: normalize_path(&scan.root, &directory),
            arguments: tokens.clone(),
            backend: "compile_commands".into(),
            origin: "observed".into(),
            confidence: 100,
        };
    }
    let mut index = 0;
    while index < tokens.len() {
        let token = tokens[index].as_str();
        if token == "-I" || token == "-isystem" {
            if index + 1 < tokens.len() {
                index += 1;
                add_include_path(include_paths, &directory_path, &tokens[index]);
            }
        } else if let Some(rest) = token.strip_prefix("-I") {
            add_include_path(include_paths, &directory_path, rest);
        }
        index += 1;
    }
}

fn scan_compile_command_flags(
    scan: &ProjectScanResult,
    include_paths: &mut BTreeSet<String>,
    metadata: &mut BuildMetadata,
) {
    for relative in &scan.existing_compilation_databases {
        if relative == Path::new("compile_commands.json") {
            continue;
        }
        let Ok(bytes) = std::fs::read(scan.root.join(relative)) else {
            continue;
        };
        let Ok(Value::Array(entries)) = serde_json::from_slice::<Value>(&bytes) else {
            continue;
        };
        for entry in &entries {
            record_compile_command(scan, entry, include_paths, metadata);
        }
    }
}

/// Arguments of every `function_name(...)` call; `search_pos` advances past
/// each call and becomes `None` when no further call exists.
fn cmake_call_arguments(
    text: &str,
    function_name: &str,
    search_pos: &mut Option<usize>,
) -> Vec<String> {
    let Some(start) = *search_pos else {
        return Vec::new();
    };
    let Some(found) = text[start..].find(function_name).map(|i| i + start) else {
        *search_pos = None;
        return Vec::new();
    };
    let Some(open) = text[found + function_name.len()..]
        .find('(')
        .map(|i| i + found + function_name.len())
    else {
        *search_pos = None;
        return Vec::new();
    };
    let bytes = text.as_bytes();
    let mut depth = 1;
    let mut quote = 0u8;
    let mut body = Vec::new();
    for (index, ch) in bytes.iter().copied().enumerate().skip(open + 1) {
        if quote != 0 {
            if ch == quote {
                quote = 0;
            }
            body.push(ch);
            continue;
        }
        if ch == b'"' || ch == b'\'' {
            quote = ch;
            body.push(ch);
            continue;
        }
        if ch == b'(' {
            depth += 1;
        } else if ch == b')' {
            depth -= 1;
            if depth == 0 {
                *search_pos = Some(index + 1);
                return parse_words(&String::from_utf8_lossy(&body));
            }
        }
        body.push(ch);
    }
    *search_pos = None;
    Vec::new()
}

fn include_key(key: &str) -> bool {
    let lower = key.to_ascii_lowercase();
    ["include", "incdir", "cpppath", "header_search_paths"]
        .iter()
        .any(|k| lower.contains(k))
}

fn define_key(key: &str) -> bool {
    let lower = key.to_ascii_lowercase();
    lower.contains("define")
        || lower.contains("cppdefines")
        || lower == "defs"
        || lower == "defines"
}

fn metadata_relevant_text(text: &str) -> bool {
    let lower = text.to_ascii_lowercase();
    [
        "include",
        "incdir",
        "cpppath",
        "define",
        "cppdefines",
        "cflags",
        "cxxflags",
        "-i",
        "-d",
        "/i",
        "/d",
        "header_search_paths",
        "preprocessor",
    ]
    .iter()
    .any(|k| lower.contains(k))
}

fn scan_tokens(
    tokens: &[String],
    base: &Path,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let mut index = 0;
    while index < tokens.len() {
        let token = tokens[index].as_str();
        if matches!(token, "-I" | "-isystem" | "/I") {
            if index + 1 < tokens.len() {
                index += 1;
                add_include_path(includes, base, &tokens[index]);
            }
        } else if let Some(rest) = token.strip_prefix("-I") {
            add_include_path(includes, base, rest);
        } else if let Some(rest) = token.strip_prefix("/I") {
            add_include_path(includes, base, rest);
        } else if matches!(token, "-D" | "/D") {
            if index + 1 < tokens.len() {
                index += 1;
                add_define(defines, &tokens[index]);
            }
        } else if let Some(rest) = token.strip_prefix("-D") {
            add_define(defines, rest);
        } else if let Some(rest) = token.strip_prefix("/D") {
            add_define(defines, rest);
        }
        index += 1;
    }
}

fn scan_assignment(
    line: &str,
    base: &Path,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let Some((key, value)) = parse_metadata_assignment(line) else {
        return;
    };
    let tokens = parse_words(&value);
    if include_key(&key) {
        for token in &tokens {
            add_include_path(includes, base, token);
        }
    }
    if define_key(&key) {
        for token in &tokens {
            add_define(defines, token);
        }
    }
    scan_tokens(&tokens, base, includes, defines);
}

fn scan_token_file(
    path: &Path,
    marker: u8,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let text = read_text_file(path);
    let base = path.parent().unwrap_or(Path::new(""));
    for line in getlines(&text) {
        let line = strip_inline_comment(line, marker);
        if !metadata_relevant_text(line) {
            continue;
        }
        scan_assignment(line, base, includes, defines);
        scan_tokens(&parse_words(line), base, includes, defines);
    }
}

fn scan_lua_style_file(
    path: &Path,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let text = read_text_file(path);
    let base = path.parent().unwrap_or(Path::new(""));
    for line in getlines(&text) {
        let line = match line.find("--") {
            Some(comment) => &line[..comment],
            None => line,
        };
        if !metadata_relevant_text(line) {
            continue;
        }
        scan_assignment(line, base, includes, defines);
        scan_tokens(&parse_words(line), base, includes, defines);
    }
}

fn scan_xml_like_file(
    path: &Path,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let text = read_text_file(path);
    if text.is_empty() {
        return;
    }
    let base = path.parent().unwrap_or(Path::new(""));
    if metadata_relevant_text(&text) {
        scan_tokens(&parse_words(&text), base, includes, defines);
    }
    for key in [
        "AdditionalIncludeDirectories",
        "IncludePath",
        "HEADER_SEARCH_PATHS",
        "GCC_PREPROCESSOR_DEFINITIONS",
        "PreprocessorDefinitions",
    ] {
        let mut pos = 0;
        while let Some(found) = text[pos..].find(key).map(|i| i + pos) {
            let close = text[found + key.len()..]
                .find('<')
                .map(|i| i + found + key.len());
            let open = text[..=found].rfind('>');
            if let (Some(open), Some(close)) = (open, close)
                && open < close
            {
                let tokens = parse_words(&text[open + 1..close]);
                if include_key(key) {
                    for token in &tokens {
                        add_include_path(includes, base, token);
                    }
                } else {
                    for token in &tokens {
                        add_define(defines, token);
                    }
                }
            }
            pos = found + key.len();
        }
    }
}

fn scan_cmake_file(
    scan: &ProjectScanResult,
    relative: &Path,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let path = scan.root.join(relative);
    let text = read_text_file(&path);
    if text.is_empty() {
        return;
    }
    let cmake_dir = path.parent().unwrap_or(Path::new("")).to_path_buf();
    for function in ["include_directories", "target_include_directories"] {
        let mut pos = Some(0);
        while pos.is_some() {
            let args = cmake_call_arguments(&text, function, &mut pos);
            if args.is_empty() {
                continue;
            }
            let first = usize::from(function == "target_include_directories");
            for arg in args.iter().skip(first) {
                let resolved = resolve_cmake_variables(arg, &scan.root, &cmake_dir);
                add_include_path(includes, &cmake_dir, &resolved);
            }
        }
    }
    for function in ["add_definitions", "add_compile_definitions"] {
        let mut pos = Some(0);
        while pos.is_some() {
            for arg in cmake_call_arguments(&text, function, &mut pos) {
                add_define(
                    defines,
                    &resolve_cmake_variables(&arg, &scan.root, &cmake_dir),
                );
            }
        }
    }
}

fn scan_build_file(
    scan: &ProjectScanResult,
    relative: &Path,
    family: Family,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
) {
    let path = scan.root.join(relative);
    match family {
        Family::CMake => {
            scan_cmake_file(scan, relative, includes, defines);
            scan_token_file(&path, b'#', includes, defines);
        }
        Family::Premake | Family::Xmake => scan_lua_style_file(&path, includes, defines),
        Family::MsBuild | Family::Xcode => scan_xml_like_file(&path, includes, defines),
        _ => scan_token_file(&path, b'#', includes, defines),
    }
}

fn scan_conventional_include_dirs(scan: &ProjectScanResult, paths: &mut BTreeSet<String>) {
    for seed in [
        "",
        "src",
        "src/include",
        "boards",
        "platforms/common/include",
        "platforms/nuttx/src/px4/common/include",
        "platforms/posix/include",
        "msg",
    ] {
        let path = if seed.is_empty() {
            scan.root.clone()
        } else {
            scan.root.join(seed)
        };
        if path.is_dir() {
            paths.insert(path_text(&weakly_canonical(&path)));
        }
    }
    let _ = crate::util::walk(&scan.root, |path, is_dir, _| {
        if !is_dir {
            return Ok(true);
        }
        if is_source_or_build_directory_skip(path) {
            return Ok(false);
        }
        let name = file_name(path);
        if name == "include" || name == "inc" {
            paths.insert(path_text(&weakly_canonical(path)));
        }
        Ok(true)
    });
}

/// Collect build metadata for a scan: existing compilation databases under
/// `build/`, emulated build systems, conventional include directories and
/// include/define hints from up to a per-family number of build files
/// (each at most 512 KiB).
pub fn scan_build_metadata(
    scan: &ProjectScanResult,
    pacer: &Pacer,
) -> Result<BuildMetadata, Cancelled> {
    let mut metadata = BuildMetadata::default();
    let mut include_paths = BTreeSet::new();
    let mut defines = BTreeSet::new();

    scan_compile_command_flags(scan, &mut include_paths, &mut metadata);
    pacer.checkpoint(0, 0, 10)?;
    let emulation = emulate_build_systems(scan, pacer)?;
    pacer.checkpoint(0, 0, 10)?;
    for (source, command) in emulation.compile_commands {
        let observed = metadata
            .observed_compile_commands
            .entry(source)
            .or_default();
        if observed.arguments.is_empty() {
            *observed = command;
        }
    }
    for path in &emulation.include_paths {
        include_paths.insert(path_text(path));
    }
    defines.extend(emulation.defines.iter().cloned());
    metadata.emulated_edges_by_backend = emulation.edges_by_backend;
    scan_conventional_include_dirs(scan, &mut include_paths);
    pacer.checkpoint(0, 0, 10)?;

    let mut scanned: BTreeMap<Family, usize> = BTreeMap::new();
    for (visited, file) in scan.files.iter().enumerate() {
        pacer.checkpoint(visited + 1, 16, 10)?;
        if file.kind != ProjectFileKind::BuildFile {
            continue;
        }
        let Some(family) = build_file_family(&file.path) else {
            continue;
        };
        let count = scanned.entry(family).or_default();
        if *count >= family.scan_limit() {
            continue;
        }
        let full = scan.root.join(&file.path);
        match std::fs::metadata(&full) {
            Ok(meta) if meta.len() <= 512 * 1024 => {}
            _ => continue,
        }
        *count += 1;
        *metadata
            .build_files_scanned_by_family
            .entry(family.name().to_string())
            .or_default() += 1;
        scan_build_file(scan, &file.path, family, &mut include_paths, &mut defines);
        pacer.checkpoint(0, 0, 10)?;
    }

    metadata.include_paths = include_paths.into_iter().map(PathBuf::from).collect();
    metadata.defines = defines.into_iter().collect();
    Ok(metadata)
}

/// `include_paths=N defines=N observed_commands=N emulated_edges=a:1 build_files=CMake:2`.
pub fn build_metadata_summary(metadata: &BuildMetadata) -> String {
    let edges: Vec<String> = metadata
        .emulated_edges_by_backend
        .iter()
        .map(|(b, c)| format!("{b}:{c}"))
        .collect();
    let files: Vec<String> = metadata
        .build_files_scanned_by_family
        .iter()
        .map(|(f, c)| format!("{f}:{c}"))
        .collect();
    format!(
        "include_paths={} defines={} observed_commands={} emulated_edges={} build_files={}",
        metadata.include_paths.len(),
        metadata.defines.len(),
        metadata.observed_compile_commands.len(),
        edges.join(","),
        files.join(",")
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::scan_project_tree;
    use std::fs;

    #[test]
    fn lexy_style_parsers() {
        assert_eq!(parse_words("a  b\tc"), vec!["a", "b", "c"]);
        assert_eq!(parse_words("f(a,b);"), vec!["f", "a", "b"]);
        assert!(parse_words("a\nb").is_empty());
        assert_eq!(
            parse_metadata_assignment("  CPPPATH = -Iinc # c"),
            Some(("CPPPATH".into(), "-Iinc ".into()))
        );
        assert_eq!(
            parse_metadata_assignment("x.y-z: 1"),
            Some(("x.y-z".into(), "1".into()))
        );
        assert_eq!(parse_metadata_assignment("1x = 2"), None);
        assert_eq!(parse_metadata_assignment("x ="), None);
    }

    #[test]
    fn cmake_calls_and_summary() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("lib/api")).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::write(root.join("src/main.cpp"), "int main(){}\n").unwrap();
        fs::write(
            root.join("CMakeLists.txt"),
            "project(x)\ntarget_include_directories(app PRIVATE lib/api)\nadd_compile_definitions(FEATURE_X=1 \"QUOTED\")\nadd_definitions(-DLEGACY)\n",
        )
        .unwrap();
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let metadata = scan_build_metadata(&scan, &Pacer::unpaced()).unwrap();
        assert!(metadata.include_paths.contains(&root.join("lib/api")));
        assert!(metadata.include_paths.contains(&root.join("src")));
        assert!(metadata.defines.contains(&"FEATURE_X=1".to_string()));
        assert!(metadata.defines.contains(&"QUOTED".to_string()));
        assert!(metadata.defines.contains(&"LEGACY".to_string()));
        let summary = build_metadata_summary(&metadata);
        assert!(summary.starts_with("include_paths="));
        assert!(summary.ends_with("build_files=CMake:1"), "{summary}");
    }

    #[test]
    fn observed_compile_commands_from_build_dirs() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("build/rel")).unwrap();
        fs::create_dir_all(root.join("inc")).unwrap();
        fs::write(root.join("a.cpp"), "int a;\n").unwrap();
        fs::write(
            root.join("build/rel/compile_commands.json"),
            serde_json::json!([{
                "directory": path_text(&root.join("build/rel")),
                "file": "../../a.cpp",
                "command": "g++ -I../../inc -c ../../a.cpp"
            }])
            .to_string(),
        )
        .unwrap();
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let metadata = scan_build_metadata(&scan, &Pacer::unpaced()).unwrap();
        let observed = &metadata.observed_compile_commands[&path_text(&root.join("a.cpp"))];
        assert_eq!(observed.backend, "compile_commands");
        assert_eq!(observed.origin, "observed");
        assert_eq!(observed.confidence, 100);
        assert!(metadata.include_paths.contains(&root.join("inc")));
    }
}
