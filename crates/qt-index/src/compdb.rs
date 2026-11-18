//! `compile_commands.json` generation.
//!
//! Every scanned C/C++ translation unit gets an entry: observed or emulated
//! commands are enriched with the project's conventional include directories
//! and the first eight defines; other sources get an inferred
//! `cc -std=c17` / `c++ -std=c++17` command. Output objects are rewritten to
//! `.index/objects/<source>.o`, gitignored include directories are dropped
//! (except PlatformIO dependencies), and Nix store compilers are reduced to
//! their file name.

use std::collections::{BTreeSet, HashMap, HashSet};
use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

use crate::build::BuildMetadata;
use crate::cancel::{Cancelled, Pacer};
use crate::gitignore::GitIgnoreMatcher;
use crate::scan::{ProjectFile, ProjectFileKind, ProjectScanResult};
use crate::util::{
    entry_is_dir, file_name, generic, is_ignored_inferred_translation_unit_path, is_nix_store_path,
    is_regular_file, lexically_normal, normalize_path, path_has_prefix, path_text,
    sorted_dir_entries,
};

const MAX_INCLUDES_PER_COMMAND: usize = 28;
const MAX_DEFINES_PER_COMMAND: usize = 8;

fn is_native_source_path(source: &Path) -> bool {
    crate::build::is_source_path(source)
}

fn object_path_for(relative_source: &Path) -> String {
    format!(
        "{}.o",
        generic(&Path::new(".index/objects").join(relative_source))
    )
}

fn relative_source_for_output(root: &Path, source: &Path) -> PathBuf {
    let normalized = lexically_normal(source);
    if path_has_prefix(&normalized, root)
        && let Ok(relative) = normalized.strip_prefix(root)
        && !relative.as_os_str().is_empty()
    {
        return relative.to_path_buf();
    }
    PathBuf::from(file_name(&normalized))
}

/// Quote an argument for a POSIX shell when it contains special characters.
pub fn shell_escape_argument(value: &str) -> String {
    if value.is_empty() {
        return "''".into();
    }
    let plain = !value.bytes().any(|ch| {
        ch.is_ascii_whitespace()
            || matches!(
                ch,
                b'\''
                    | b'"'
                    | b'\\'
                    | b'$'
                    | b'`'
                    | b';'
                    | b'&'
                    | b'|'
                    | b'<'
                    | b'>'
                    | b'('
                    | b')'
                    | b'['
                    | b']'
                    | b'{'
                    | b'}'
                    | b'*'
            )
    });
    if plain {
        return value.to_string();
    }
    format!("'{}'", value.replace('\'', "'\\''"))
}

fn command_from_arguments(arguments: &[String]) -> String {
    arguments
        .iter()
        .map(|a| shell_escape_argument(a))
        .collect::<Vec<_>>()
        .join(" ")
}

fn arguments_with_output_path(mut arguments: Vec<String>, output: &str) -> Vec<String> {
    for index in 0..arguments.len() {
        if arguments[index] == "-o" && index + 1 < arguments.len() {
            arguments[index + 1] = output.to_string();
            return arguments;
        }
        if arguments[index].starts_with("-o") && arguments[index].len() > 2 {
            arguments[index] = format!("-o{output}");
            return arguments;
        }
    }
    arguments.push("-o".into());
    arguments.push(output.to_string());
    arguments
}

fn arguments_without_nix_store_executable(mut arguments: Vec<String>) -> Vec<String> {
    if let Some(first) = arguments.first_mut()
        && is_nix_store_path(Path::new(first.as_str()))
    {
        *first = file_name(Path::new(first.as_str()));
    }
    arguments
}

struct IncludeFilter<'a> {
    gitignore: &'a GitIgnoreMatcher,
    cache: HashMap<String, bool>,
}

impl IncludeFilter<'_> {
    fn allowed(&mut self, directory: &Path, raw: &str) -> bool {
        if raw.is_empty() || raw.contains('$') {
            return true;
        }
        let include = normalize_path(directory, raw);
        let key = path_text(&include);
        if let Some(cached) = self.cache.get(&key) {
            return *cached;
        }
        let dependency = include.components().any(|part| {
            let name = part.as_os_str();
            name == "libdeps" || name == "boost_1_86_0"
        });
        let allowed =
            !is_nix_store_path(&include) && (dependency || !self.gitignore.ignores(&include));
        self.cache.insert(key, allowed);
        allowed
    }

    fn filter(&mut self, arguments: &[String], directory: &Path) -> Vec<String> {
        let mut filtered = Vec::with_capacity(arguments.len());
        let mut index = 0;
        while index < arguments.len() {
            let argument = &arguments[index];
            if matches!(argument.as_str(), "-I" | "-isystem" | "/I") && index + 1 < arguments.len()
            {
                if self.allowed(directory, &arguments[index + 1]) {
                    filtered.push(argument.clone());
                    filtered.push(arguments[index + 1].clone());
                }
                index += 2;
                continue;
            }
            if argument.len() > 2 && (argument.starts_with("-I") || argument.starts_with("/I")) {
                if self.allowed(directory, &argument[2..]) {
                    filtered.push(argument.clone());
                }
                index += 1;
                continue;
            }
            filtered.push(argument.clone());
            index += 1;
        }
        filtered
    }
}

fn add_include(
    selected: &mut Vec<PathBuf>,
    seen: &mut BTreeSet<String>,
    path: &Path,
    gitignore: &GitIgnoreMatcher,
    allow_ignored: bool,
) {
    let effective = lexically_normal(path);
    if is_nix_store_path(&effective) || (!allow_ignored && gitignore.ignores(&effective)) {
        return;
    }
    if seen.insert(path_text(&effective)) {
        selected.push(effective);
    }
}

fn add_platformio_dependency_includes(
    selected: &mut Vec<PathBuf>,
    seen: &mut BTreeSet<String>,
    root: &Path,
    gitignore: &GitIgnoreMatcher,
) {
    let boost = root.join("lib").join("boost_1_86_0");
    if boost.is_dir() {
        add_include(selected, seen, &boost, gitignore, true);
    }
    let libdeps = root.join(".pio").join("libdeps").join("native");
    if !libdeps.is_dir() {
        return;
    }
    for entry in sorted_dir_entries(&libdeps) {
        if !entry_is_dir(&entry) {
            continue;
        }
        let dependency = entry.path();
        add_include(selected, seen, &dependency.join("include"), gitignore, true);
        add_include(selected, seen, &dependency.join("src"), gitignore, true);
        if file_name(&dependency) == "FFS" {
            add_include(selected, seen, &dependency, gitignore, true);
        }
    }
}

fn include_paths_for(
    scan: &ProjectScanResult,
    metadata: &BuildMetadata,
    file: &ProjectFile,
    gitignore: &GitIgnoreMatcher,
) -> Vec<PathBuf> {
    let mut selected = Vec::new();
    let mut seen = BTreeSet::new();
    let source = lexically_normal(&scan.root.join(&file.path));
    let source_dir = source.parent().unwrap_or(Path::new("")).to_path_buf();
    for seed in [
        "",
        "src",
        "src/include",
        "platforms/common/include",
        "platforms/nuttx/src/px4/common/include",
        "platforms/nuttx/src/px4/rpi/rpi_common/include",
        "platforms/nuttx/src/px4/rpi/rp23xx/include",
        "platforms/nuttx/NuttX/include",
        "platforms/nuttx/NuttX/nuttx/include",
        "platforms/nuttx/NuttX/nuttx/arch/arm/include",
        "platforms/nuttx/NuttX/nuttx/arch/arm/src/rp23xx",
        "platforms/nuttx/NuttX/nuttx/arch/arm/src/rp23xx/hardware",
        "boards",
        "msg",
    ] {
        let path = if seed.is_empty() {
            scan.root.clone()
        } else {
            scan.root.join(seed)
        };
        add_include(&mut selected, &mut seen, &path, gitignore, false);
    }
    add_platformio_dependency_includes(&mut selected, &mut seen, &scan.root, gitignore);

    let parts: Vec<_> = file
        .path
        .components()
        .map(|c| c.as_os_str().to_owned())
        .collect();
    if parts.len() >= 3 && parts[0] == "boards" {
        add_include(
            &mut selected,
            &mut seen,
            &scan
                .root
                .join("boards")
                .join(&parts[1])
                .join(&parts[2])
                .join("nuttx-config")
                .join("include"),
            gitignore,
            false,
        );
    }

    let mut dir = Some(source_dir.as_path());
    while let Some(current) = dir {
        if current.as_os_str().is_empty() || !path_has_prefix(current, &scan.root) {
            break;
        }
        add_include(&mut selected, &mut seen, current, gitignore, false);
        if current == scan.root {
            break;
        }
        dir = current.parent();
    }

    for include in &metadata.include_paths {
        if selected.len() >= MAX_INCLUDES_PER_COMMAND {
            break;
        }
        if path_has_prefix(&source_dir, include)
            || path_has_prefix(include, &source_dir)
            || include.file_name().is_some_and(|n| n == "include")
        {
            add_include(&mut selected, &mut seen, include, gitignore, false);
        }
    }
    selected
}

fn defines_for(metadata: &BuildMetadata) -> Vec<String> {
    metadata
        .defines
        .iter()
        .take(MAX_DEFINES_PER_COMMAND)
        .cloned()
        .collect()
}

fn inferred_arguments(
    scan: &ProjectScanResult,
    metadata: &BuildMetadata,
    file: &ProjectFile,
    gitignore: &GitIgnoreMatcher,
) -> Vec<String> {
    let c = file.kind == ProjectFileKind::CSource;
    let mut arguments = vec![
        if c { "cc" } else { "c++" }.to_string(),
        if c { "-std=c17" } else { "-std=c++17" }.to_string(),
    ];
    arguments.extend(defines_for(metadata).into_iter().map(|d| format!("-D{d}")));
    arguments.extend(
        include_paths_for(scan, metadata, file, gitignore)
            .iter()
            .map(|p| format!("-I{}", path_text(p))),
    );
    arguments.push("-c".into());
    arguments.push(path_text(&lexically_normal(&scan.root.join(&file.path))));
    arguments.push("-o".into());
    arguments.push(object_path_for(&file.path));
    arguments
}

fn enrich_observed_arguments(
    scan: &ProjectScanResult,
    metadata: &BuildMetadata,
    file: &ProjectFile,
    mut arguments: Vec<String>,
    gitignore: &GitIgnoreMatcher,
) -> Vec<String> {
    let include_paths = include_paths_for(scan, metadata, file, gitignore);
    let mut insert_pos = arguments
        .iter()
        .position(|a| a == "-c")
        .unwrap_or(arguments.len());
    let additions = defines_for(metadata)
        .into_iter()
        .map(|d| format!("-D{d}"))
        .chain(include_paths.iter().map(|p| format!("-I{}", path_text(p))));
    for argument in additions {
        if !arguments.contains(&argument) {
            arguments.insert(insert_pos, argument);
            insert_pos += 1;
        }
    }
    arguments
}

fn command_entry_for(
    scan: &ProjectScanResult,
    metadata: &BuildMetadata,
    file: &ProjectFile,
    gitignore: &GitIgnoreMatcher,
) -> Option<(PathBuf, Vec<String>)> {
    let source = path_text(&lexically_normal(&scan.root.join(&file.path)));
    if let Some(observed) = metadata.observed_compile_commands.get(&source)
        && !observed.arguments.is_empty()
    {
        return Some((
            observed.directory.clone(),
            enrich_observed_arguments(scan, metadata, file, observed.arguments.clone(), gitignore),
        ));
    }
    if !file.kind.is_native_source() {
        return None;
    }
    Some((
        scan.root.clone(),
        inferred_arguments(scan, metadata, file, gitignore),
    ))
}

/// Render the compilation database text (`[\n<entry>,\n<entry>\n]\n`) and
/// return it with the number of entries.
pub fn render_compilation_database(
    scan: &ProjectScanResult,
    metadata: &BuildMetadata,
    pacer: &Pacer,
) -> Result<(String, usize), Cancelled> {
    let gitignore = GitIgnoreMatcher::load(&scan.root);
    let mut filter = IncludeFilter {
        gitignore: &gitignore,
        cache: HashMap::new(),
    };
    let mut emitted: HashSet<String> = HashSet::new();
    let mut entries: Vec<String> = Vec::new();
    let mut emit = |source: &str, directory: &Path, arguments: &[String]| {
        let source_path = Path::new(source);
        if !is_regular_file(source_path)
            || is_nix_store_path(source_path)
            || !emitted.insert(source.to_string())
            || gitignore.ignores(source_path)
        {
            return;
        }
        let output = object_path_for(&relative_source_for_output(&scan.root, source_path));
        let output_arguments = arguments_without_nix_store_executable(filter.filter(
            &arguments_with_output_path(arguments.to_vec(), &output),
            directory,
        ));
        let mut entry = Map::new();
        entry.insert(
            "arguments".into(),
            Value::Array(
                output_arguments
                    .iter()
                    .cloned()
                    .map(Value::String)
                    .collect(),
            ),
        );
        entry.insert(
            "command".into(),
            Value::String(command_from_arguments(&output_arguments)),
        );
        entry.insert("directory".into(), Value::String(path_text(directory)));
        entry.insert("file".into(), Value::String(source.to_string()));
        entry.insert("output".into(), Value::String(output));
        entries.push(serde_json::to_string_pretty(&Value::Object(entry)).unwrap_or_default());
    };

    let mut operations = 0usize;
    for file in &scan.files {
        operations += 1;
        pacer.checkpoint(operations, 64, 10)?;
        if !file.kind.is_native_source() || is_ignored_inferred_translation_unit_path(&file.path) {
            continue;
        }
        let source = path_text(&lexically_normal(&scan.root.join(&file.path)));
        if let Some((directory, arguments)) = command_entry_for(scan, metadata, file, &gitignore)
            && !arguments.is_empty()
        {
            emit(&source, &directory, &arguments);
        }
    }
    for (source, observed) in &metadata.observed_compile_commands {
        operations += 1;
        pacer.checkpoint(operations, 64, 10)?;
        if !observed.arguments.is_empty() && is_native_source_path(Path::new(source)) {
            emit(source, &observed.directory, &observed.arguments);
        }
    }
    let count = entries.len();
    Ok((format!("[\n{}\n]\n", entries.join(",\n")), count))
}

/// Error from [`write_compilation_database`].
#[derive(Debug)]
pub enum CompdbError {
    Cancelled,
    Io(String),
}

impl std::fmt::Display for CompdbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Cancelled => f.write_str("index scan cancelled"),
            Self::Io(message) => f.write_str(message),
        }
    }
}

impl From<Cancelled> for CompdbError {
    fn from(_: Cancelled) -> Self {
        Self::Cancelled
    }
}

/// Write `compile_commands.json` to `output_path` and return the number of
/// translation units it covers.
pub fn write_compilation_database(
    scan: &ProjectScanResult,
    metadata: &BuildMetadata,
    output_path: &Path,
    pacer: &Pacer,
) -> Result<usize, CompdbError> {
    let (text, count) = render_compilation_database(scan, metadata, pacer)?;
    let mut file = std::fs::File::create(output_path).map_err(|e| {
        CompdbError::Io(format!(
            "could not open compilation database output: {}: {e}",
            output_path.display()
        ))
    })?;
    file.write_all(text.as_bytes())
        .map_err(|e| CompdbError::Io(format!("could not write {}: {e}", output_path.display())))?;
    Ok(count)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::build::scan_build_metadata;
    use crate::scan::scan_project_tree;
    use std::fs;

    #[test]
    fn shell_escaping() {
        assert_eq!(shell_escape_argument("plain-arg"), "plain-arg");
        assert_eq!(shell_escape_argument(""), "''");
        assert_eq!(shell_escape_argument("a b"), "'a b'");
        assert_eq!(shell_escape_argument("it's"), "'it'\\''s'");
    }

    #[test]
    fn output_path_rewrites() {
        assert_eq!(
            arguments_with_output_path(vec!["cc".into(), "-o".into(), "x.o".into()], "y.o"),
            vec!["cc", "-o", "y.o"]
        );
        assert_eq!(
            arguments_with_output_path(vec!["cc".into(), "-ox.o".into()], "y.o"),
            vec!["cc", "-oy.o"]
        );
        assert_eq!(
            arguments_with_output_path(vec!["cc".into()], "y.o"),
            vec!["cc", "-o", "y.o"]
        );
        assert_eq!(
            arguments_without_nix_store_executable(vec!["/nix/store/abc-gcc/bin/g++".into()]),
            vec!["g++"]
        );
    }

    #[test]
    fn inferred_database_for_plain_project() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("ignored_inc")).unwrap();
        fs::write(root.join(".gitignore"), "ignored_inc/\n").unwrap();
        fs::write(root.join("src/main.cpp"), "int main(){}\n").unwrap();
        fs::write(root.join("src/util.c"), "int u;\n").unwrap();
        fs::write(root.join("src/util.h"), "").unwrap();
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let metadata = scan_build_metadata(&scan, &Pacer::unpaced()).unwrap();
        let output = root.join("compile_commands.json");
        let count =
            write_compilation_database(&scan, &metadata, &output, &Pacer::unpaced()).unwrap();
        assert_eq!(count, 2);
        let text = fs::read_to_string(&output).unwrap();
        assert!(text.starts_with("[\n{\n  \"arguments\": [\n"));
        assert!(text.ends_with("}\n]\n"));
        let parsed: Value = serde_json::from_str(&text).unwrap();
        let entries = parsed.as_array().unwrap();
        assert_eq!(entries.len(), 2);
        let main = entries
            .iter()
            .find(|e| e["file"].as_str().unwrap().ends_with("main.cpp"))
            .unwrap();
        let args: Vec<&str> = main["arguments"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap())
            .collect();
        assert_eq!(&args[..2], &["c++", "-std=c++17"]);
        assert!(args.contains(&format!("-I{}", root.join("src").display()).as_str()));
        assert!(!args.iter().any(|a| a.contains("ignored_inc")));
        assert_eq!(main["output"], ".index/objects/src/main.cpp.o");
        assert_eq!(main["directory"], path_text(&root));
        let util = entries
            .iter()
            .find(|e| e["file"].as_str().unwrap().ends_with("util.c"))
            .unwrap();
        assert_eq!(util["arguments"][0], "cc");
        assert_eq!(util["arguments"][1], "-std=c17");
    }

    #[test]
    fn empty_database_text() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let metadata = BuildMetadata::default();
        let (text, count) =
            render_compilation_database(&scan, &metadata, &Pacer::unpaced()).unwrap();
        assert_eq!(count, 0);
        assert_eq!(text, "[\n\n]\n");
    }
}
