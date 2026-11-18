//! CMake "Unix Makefiles" build trees: `flags.make` + `DependInfo.cmake`.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use super::{
    BuildEmulationResult, ObservedCompileCommand, append_compile_tokens, is_c_source_path,
    is_source_path, parse_assignment, parse_quoted_string_at, record_generated_command,
    scan_tokens_for_metadata,
};
use crate::util::{
    file_name, getlines, normalize_path, path_text, read_text_file, split_shell_like, trim_ascii,
};

#[derive(Default)]
struct CMakeMakeFlags {
    compiler: String,
    variables: BTreeMap<String, String>,
}

fn scan_flags_make(flags_make: &Path) -> CMakeMakeFlags {
    let mut flags = CMakeMakeFlags::default();
    let text = read_text_file(flags_make);
    for line in getlines(&text) {
        if line.starts_with("# compile ") {
            if let Some(with) = line.find(" with ") {
                flags.compiler = trim_ascii(&line[with + 6..]).to_string();
            }
            continue;
        }
        if let Some((key, value)) = parse_assignment(line) {
            flags.variables.insert(key, value);
        }
    }
    flags
}

fn cmake_make_build_root_for(flags_make: &Path) -> PathBuf {
    let parent = flags_make.parent().unwrap_or(Path::new("")).to_path_buf();
    let mut dir = Some(parent.as_path());
    while let Some(current) = dir {
        if current.as_os_str().is_empty() {
            break;
        }
        if current.file_name().is_some_and(|n| n == "CMakeFiles") {
            return current.parent().unwrap_or(Path::new("")).to_path_buf();
        }
        dir = current.parent();
    }
    parent
}

fn scan_depend_info(depend_info: &Path, build_root: &Path) -> Vec<(PathBuf, PathBuf)> {
    let mut sources = Vec::new();
    let text = read_text_file(depend_info);
    let Some(block) = text.find("CMAKE_DEPENDS_DEPENDENCY_FILES") else {
        return sources;
    };
    let bytes = text.as_bytes();
    let mut values = Vec::new();
    let mut pos = block;
    while pos < bytes.len() {
        let Some(offset) = bytes[pos..].iter().position(|b| *b == b'"') else {
            break;
        };
        pos += offset;
        match parse_quoted_string_at(bytes, &mut pos) {
            Some(value) => values.push(value),
            None => break,
        }
    }
    let mut index = 0;
    while index + 1 < values.len() {
        let source = normalize_path(build_root, &values[index]);
        if is_source_path(&source) {
            sources.push((source, normalize_path(build_root, &values[index + 1])));
        }
        index += 4;
    }
    sources
}

fn record_cmake_make_source(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    build_root: &Path,
    flags: &CMakeMakeFlags,
    source: &Path,
    output: &Path,
) {
    let c = is_c_source_path(source);
    let mut arguments = vec![if flags.compiler.is_empty() {
        if c {
            "cc".to_string()
        } else {
            "c++".to_string()
        }
    } else {
        flags.compiler.clone()
    }];
    let prefixes = [if c { "C" } else { "CXX" }, "C", "CXX"];
    let mut added = BTreeSet::new();
    for prefix in prefixes {
        for suffix in ["_DEFINES", "_INCLUDES", "_FLAGS"] {
            let key = format!("{prefix}{suffix}");
            if !added.insert(key.clone()) {
                continue;
            }
            let Some(value) = flags.variables.get(&key) else {
                continue;
            };
            let tokens = split_shell_like(value);
            scan_tokens_for_metadata(&tokens, build_root, includes, defines);
            append_compile_tokens(&mut arguments, &tokens, build_root);
        }
    }
    arguments.extend([
        "-c".to_string(),
        path_text(source),
        "-o".to_string(),
        path_text(output),
    ]);
    record_generated_command(
        result,
        source,
        ObservedCompileCommand {
            directory: build_root.to_path_buf(),
            arguments,
            backend: "cmake-make".into(),
            origin: "emulated".into(),
            confidence: 85,
        },
    );
}

pub(super) fn emulate_cmake_make(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    build_root: &Path,
) {
    let mut flag_files = Vec::new();
    let _ = crate::util::walk(build_root, |path, is_dir, _| {
        if is_dir {
            let name = file_name(path);
            return Ok(!(name == "_deps" || name == "external" || name == "Testing"));
        }
        if path.file_name().is_some_and(|n| n == "flags.make") && crate::util::is_regular_file(path)
        {
            flag_files.push(path.to_path_buf());
        }
        Ok(true)
    });
    for flags_path in flag_files {
        let flags = scan_flags_make(&flags_path);
        let effective_root = cmake_make_build_root_for(&flags_path);
        let depend_info = flags_path
            .parent()
            .unwrap_or(Path::new(""))
            .join("DependInfo.cmake");
        for (source, output) in scan_depend_info(&depend_info, &effective_root) {
            record_cmake_make_source(
                result,
                includes,
                defines,
                &effective_root,
                &flags,
                &source,
                &output,
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn flags_make_and_depend_info_produce_commands() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        let target = root.join("build/CMakeFiles/app.dir");
        fs::create_dir_all(&target).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("inc")).unwrap();
        fs::write(root.join("src/main.cpp"), "int main(){}\n").unwrap();
        fs::write(
            target.join("flags.make"),
            format!(
                "# compile CXX with /usr/bin/g++\nCXX_DEFINES = -DAPP=1\nCXX_INCLUDES = -I{}\nCXX_FLAGS = -O2\n",
                root.join("inc").display()
            ),
        )
        .unwrap();
        fs::write(
            target.join("DependInfo.cmake"),
            format!(
                "set(CMAKE_DEPENDS_DEPENDENCY_FILES\n  \"{}\" \"CMakeFiles/app.dir/src/main.cpp.o\" \"gcc\" \"CMakeFiles/app.dir/src/main.cpp.o.d\"\n  )\n",
                root.join("src/main.cpp").display()
            ),
        )
        .unwrap();
        let mut result = BuildEmulationResult::default();
        let mut includes = BTreeSet::new();
        let mut defines = BTreeSet::new();
        emulate_cmake_make(
            &mut result,
            &mut includes,
            &mut defines,
            &root.join("build"),
        );
        let key = path_text(&root.join("src/main.cpp"));
        let command = result.compile_commands.get(&key).expect("command");
        assert_eq!(command.backend, "cmake-make");
        assert_eq!(command.confidence, 85);
        assert_eq!(command.directory, root.join("build"));
        assert_eq!(command.arguments[0], "/usr/bin/g++");
        assert!(command.arguments.contains(&"-DAPP=1".to_string()));
        assert!(command.arguments.contains(&"-O2".to_string()));
        assert_eq!(
            command.arguments.last().unwrap(),
            &path_text(&root.join("build/CMakeFiles/app.dir/src/main.cpp.o"))
        );
        assert!(defines.contains("APP=1"));
        assert!(includes.contains(&path_text(&root.join("inc"))));
        assert_eq!(result.edges_by_backend.get("cmake-make"), Some(&1));
    }
}
