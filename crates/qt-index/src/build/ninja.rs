//! Ninja build files: `build out: rule in ...` edges with indented variables.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::{Path, PathBuf};

use super::{
    BuildEmulationResult, ObservedCompileCommand, append_compile_tokens, is_c_source_path,
    is_source_path, parse_assignment, record_generated_command, scan_tokens_for_metadata,
};
use crate::util::{file_name, getlines, normalize_path, path_text, split_shell_like, trim_ascii};

fn ninja_unescape(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut chars = value.chars().peekable();
    while let Some(ch) = chars.next() {
        if ch == '$'
            && let Some(&next) = chars.peek()
            && matches!(next, ' ' | ':' | '$')
        {
            out.push(next);
            chars.next();
            continue;
        }
        out.push(ch);
    }
    out
}

fn compiler_from_rule_command(command: &str) -> Option<String> {
    for mut token in split_shell_like(command) {
        if token.to_ascii_lowercase().contains("ccache") {
            continue;
        }
        if let Some(end) = token.rfind('}')
            && end + 1 < token.len()
        {
            token = token[end + 1..].to_string();
        }
        let name = file_name(Path::new(&token)).to_ascii_lowercase();
        if name.contains("gcc")
            || name.contains("g++")
            || name.contains("clang")
            || name == "cc"
            || name == "c++"
        {
            return Some(token);
        }
    }
    None
}

fn starts_with_space(line: &str) -> bool {
    line.as_bytes()
        .first()
        .is_some_and(|b| b.is_ascii_whitespace())
}

fn scan_ninja_rule_compilers(build_dir: &Path) -> HashMap<String, String> {
    let mut compilers = HashMap::new();
    for path in [
        build_dir.join("CMakeFiles").join("rules.ninja"),
        build_dir.join("rules.ninja"),
        build_dir.join("build.ninja"),
    ] {
        let Ok(bytes) = std::fs::read(&path) else {
            continue;
        };
        let text = String::from_utf8_lossy(&bytes);
        let mut current_rule = String::new();
        for line in getlines(&text) {
            if let Some(rest) = line.strip_prefix("rule ") {
                current_rule = trim_ascii(rest).to_string();
                continue;
            }
            if current_rule.is_empty() || line.is_empty() || !starts_with_space(line) {
                if !line.is_empty() && !starts_with_space(line) {
                    current_rule.clear();
                }
                continue;
            }
            let Some((key, value)) = parse_assignment(line) else {
                continue;
            };
            if key != "command" {
                continue;
            }
            if let Some(compiler) = compiler_from_rule_command(&value) {
                compilers.entry(current_rule.clone()).or_insert(compiler);
            }
        }
    }
    compilers
}

struct NinjaBuildEdge {
    output: PathBuf,
    input: PathBuf,
    rule: String,
    variables: BTreeMap<String, String>,
}

fn parse_ninja_build_line(build_dir: &Path, line: &str) -> Option<NinjaBuildEdge> {
    if !line.starts_with("build ") {
        return None;
    }
    let colon = line[6..].find(':')? + 6;
    let outputs = split_shell_like(&ninja_unescape(&line[6..colon]));
    let mut rhs = &line[colon + 1..];
    if let Some(order_only) = rhs.find(" || ") {
        rhs = &rhs[..order_only];
    }
    if let Some(implicit) = rhs.find(" | ") {
        rhs = &rhs[..implicit];
    }
    let rhs_tokens = split_shell_like(&ninja_unescape(rhs));
    if outputs.is_empty() || rhs_tokens.len() < 2 {
        return None;
    }
    let input = rhs_tokens[1..]
        .iter()
        .map(|t| normalize_path(build_dir, t))
        .find(|p| is_source_path(p))?;
    Some(NinjaBuildEdge {
        output: normalize_path(build_dir, &outputs[0]),
        input,
        rule: rhs_tokens[0].clone(),
        variables: BTreeMap::new(),
    })
}

fn record_ninja_edge(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    build_dir: &Path,
    edge: &NinjaBuildEdge,
    compilers: &HashMap<String, String>,
) {
    let mut arguments = vec![match compilers.get(&edge.rule) {
        Some(compiler) => compiler.clone(),
        None if is_c_source_path(&edge.input) => "cc".into(),
        None => "c++".into(),
    }];
    for key in ["DEFINES", "INCLUDES", "FLAGS"] {
        let Some(value) = edge.variables.get(key) else {
            continue;
        };
        let tokens = split_shell_like(value);
        scan_tokens_for_metadata(&tokens, build_dir, includes, defines);
        append_compile_tokens(&mut arguments, &tokens, build_dir);
    }
    arguments.extend([
        "-c".to_string(),
        path_text(&edge.input),
        "-o".to_string(),
        path_text(&edge.output),
    ]);
    record_generated_command(
        result,
        &edge.input,
        ObservedCompileCommand {
            directory: build_dir.to_path_buf(),
            arguments,
            backend: "ninja".into(),
            origin: "emulated".into(),
            confidence: 90,
        },
    );
}

pub(super) fn emulate_ninja(
    result: &mut BuildEmulationResult,
    includes: &mut BTreeSet<String>,
    defines: &mut BTreeSet<String>,
    build_ninja: &Path,
) {
    let Ok(bytes) = std::fs::read(build_ninja) else {
        return;
    };
    let text = String::from_utf8_lossy(&bytes);
    let build_dir = build_ninja.parent().unwrap_or(Path::new("")).to_path_buf();
    let compilers = scan_ninja_rule_compilers(&build_dir);
    let mut current: Option<NinjaBuildEdge> = None;
    for line in getlines(&text) {
        if line.starts_with("build ") {
            if let Some(edge) = current.take() {
                record_ninja_edge(result, includes, defines, &build_dir, &edge, &compilers);
            }
            current = parse_ninja_build_line(&build_dir, line);
            continue;
        }
        let Some(edge) = current.as_mut() else {
            continue;
        };
        if line.is_empty() || !starts_with_space(line) {
            if let Some(edge) = current.take() {
                record_ninja_edge(result, includes, defines, &build_dir, &edge, &compilers);
            }
            continue;
        }
        if let Some((key, value)) = parse_assignment(line) {
            edge.variables.insert(key, value);
        }
    }
    if let Some(edge) = current {
        record_ninja_edge(result, includes, defines, &build_dir, &edge, &compilers);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn ninja_edges_become_commands() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        let build = root.join("build/ninja");
        fs::create_dir_all(&build).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::write(root.join("src/a.c"), "int a;\n").unwrap();
        fs::write(
            build.join("rules.ninja"),
            "rule C_COMPILER__app\n  command = /usr/bin/ccache /usr/bin/clang $DEFINES $INCLUDES $FLAGS -c $in\n",
        )
        .unwrap();
        fs::write(
            build.join("build.ninja"),
            "build CMakeFiles/app.dir/src/a.c.o: C_COMPILER__app ../../src/a.c || cmake_object_order\n  DEFINES = -DX=1\n  INCLUDES = -I../../src\n  FLAGS = -g\n\nbuild all: phony\n",
        )
        .unwrap();
        let mut result = BuildEmulationResult::default();
        let mut includes = BTreeSet::new();
        let mut defines = BTreeSet::new();
        emulate_ninja(
            &mut result,
            &mut includes,
            &mut defines,
            &build.join("build.ninja"),
        );
        let command = result
            .compile_commands
            .get(&path_text(&root.join("src/a.c")))
            .expect("ninja command");
        assert_eq!(command.arguments[0], "/usr/bin/clang");
        assert_eq!(command.confidence, 90);
        assert!(
            command
                .arguments
                .contains(&format!("-I{}", root.join("src").display()))
        );
        assert!(defines.contains("X=1"));
        assert_eq!(ninja_unescape("a$ b$:c$$"), "a b:c$");
    }
}
