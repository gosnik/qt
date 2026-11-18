//! Build operations over the generated `compile_commands.json` and build files.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use regex::Regex;
use serde_json::{Value, json};

use super::{Query, query_json, query_size, query_value, read_lines};
use crate::service::BackgroundService;
use crate::util::{
    extension_text, file_name, generic, lexically_normal, path_has_prefix, path_text, trim_ascii,
};

static INCLUDE_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r#"^\s*#\s*include\s*([<"])([^>"]+)[>"]"#).expect("include regex"));
static TARGET_RE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i)\badd_(executable|library)\s*\(\s*([A-Za-z0-9_.:+-]+)").expect("target regex")
});
static LINK_RE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i)\btarget_link_libraries\s*\(\s*([A-Za-z0-9_.:+-]+)\s+([^)]*)")
        .expect("link regex")
});
static SOURCES_RE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i)\btarget_sources\s*\(\s*([A-Za-z0-9_.:+-]+)\s+([^)]*)").expect("sources regex")
});

fn relative_or_original(root: &Path, value: &str) -> String {
    let path = lexically_normal(Path::new(value));
    if path_has_prefix(&path, root)
        && let Ok(relative) = path.strip_prefix(root)
        && !relative.as_os_str().is_empty()
    {
        return generic(relative);
    }
    generic(&path)
}

fn entry_str<'a>(entry: &'a Value, key: &str) -> &'a str {
    entry.get(key).and_then(Value::as_str).unwrap_or_default()
}

fn entry_matches_file(entry: &Value, filter: &str) -> bool {
    filter.is_empty() || entry_str(entry, "file").contains(filter)
}

fn read_compile_commands(service: &BackgroundService) -> Vec<Value> {
    std::fs::read(&service.options().compile_commands_output)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .and_then(|v| match v {
            Value::Array(entries) => Some(entries),
            _ => None,
        })
        .unwrap_or_default()
}

fn entry_arguments(entry: &Value) -> Vec<String> {
    entry
        .get("arguments")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|v| v.as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

pub(super) fn compile_commands_payload(service: &BackgroundService, query: &Query) -> Value {
    let commands = read_compile_commands(service);
    let limit = query_size(query, "limit", 200);
    let filter = query_value(query, "file", "");
    let entries: Vec<&Value> = commands
        .iter()
        .filter(|e| entry_matches_file(e, filter))
        .take(if limit == 0 { usize::MAX } else { limit })
        .collect();
    json!({
        "path": path_text(&service.options().compile_commands_output),
        "count": commands.len(),
        "entries": entries,
        "query": query_json(query),
    })
}

fn compile_flags_for_entry(root: &Path, entry: &Value) -> Value {
    let args = entry_arguments(entry);
    let file = entry_str(entry, "file");
    let mut includes = Vec::new();
    let mut defines = Vec::new();
    let mut standards = Vec::new();
    let mut flags = Vec::new();
    let compiler = args.first().cloned().unwrap_or_default();
    let mut output = entry_str(entry, "output").to_string();
    let mut index = 1;
    while index < args.len() {
        let arg = args[index].as_str();
        if matches!(arg, "-I" | "-isystem" | "/I") && index + 1 < args.len() {
            index += 1;
            includes.push(relative_or_original(root, &args[index]));
        } else if arg.len() > 2 && (arg.starts_with("-I") || arg.starts_with("/I")) {
            includes.push(relative_or_original(root, &arg[2..]));
        } else if matches!(arg, "-D" | "/D") && index + 1 < args.len() {
            index += 1;
            defines.push(args[index].clone());
        } else if arg.len() > 2 && (arg.starts_with("-D") || arg.starts_with("/D")) {
            defines.push(arg[2..].to_string());
        } else if arg.starts_with("-std=") || arg.starts_with("/std:") {
            standards.push(arg.to_string());
        } else if arg == "-o" && index + 1 < args.len() {
            index += 1;
            output = args[index].clone();
        } else if arg != "-c" && arg != file {
            flags.push(arg.to_string());
        }
        index += 1;
    }
    json!({
        "file": relative_or_original(root, file),
        "directory": entry_str(entry, "directory"),
        "compiler": compiler,
        "standards": standards,
        "defines": defines,
        "includes": includes,
        "flags": flags,
        "output": output,
    })
}

pub(super) fn compile_flags_payload(service: &BackgroundService, query: &Query) -> Value {
    let commands = read_compile_commands(service);
    let limit = query_size(query, "limit", 100);
    let filter = query_value(query, "file", "");
    let entries: Vec<Value> = commands
        .iter()
        .filter(|e| entry_matches_file(e, filter))
        .take(if limit == 0 { usize::MAX } else { limit })
        .map(|e| compile_flags_for_entry(service.project_root(), e))
        .collect();
    json!({
        "entries": entries,
        "compile_commands": commands.len(),
        "query": query_json(query),
    })
}

fn include_search_paths(service: &BackgroundService, entry: &Value) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    let mut seen = HashSet::new();
    let mut add = |path: PathBuf| {
        let path = lexically_normal(&path);
        if seen.insert(path_text(&path)) {
            paths.push(path);
        }
    };
    let file = entry_str(entry, "file");
    if !file.is_empty() {
        add(Path::new(file)
            .parent()
            .unwrap_or(Path::new(""))
            .to_path_buf());
    }
    add(service.project_root().to_path_buf());
    for arg in entry_arguments(entry) {
        if arg.len() > 2 && arg.starts_with("-I") {
            add(PathBuf::from(&arg[2..]));
        }
    }
    paths
}

fn resolve_include(search_paths: &[PathBuf], include: &str) -> Option<PathBuf> {
    search_paths
        .iter()
        .map(|base| lexically_normal(&base.join(include)))
        .find(|candidate| crate::util::is_regular_file(candidate))
}

pub(super) fn include_graph_payload(service: &BackgroundService, query: &Query) -> Value {
    let commands = read_compile_commands(service);
    let limit = query_size(query, "limit", 300);
    let filter = query_value(query, "file", "");
    let root = service.project_root();
    let by_file: HashMap<String, &Value> = commands
        .iter()
        .map(|c| {
            (
                path_text(&lexically_normal(Path::new(entry_str(c, "file")))),
                c,
            )
        })
        .collect();
    let mut edges = Vec::new();
    for file in &service.project_scan().files {
        if !super::code::is_cpp_kind(file.kind) {
            continue;
        }
        let source = lexically_normal(&root.join(&file.path));
        if !filter.is_empty() && !generic(&file.path).contains(filter) {
            continue;
        }
        let Some(lines) = read_lines(&source) else {
            continue;
        };
        let search_paths = match by_file.get(&path_text(&source)) {
            Some(entry) => include_search_paths(service, entry),
            None => vec![
                source.parent().unwrap_or(Path::new("")).to_path_buf(),
                root.to_path_buf(),
            ],
        };
        for (index, line) in lines.iter().enumerate() {
            let Some(captures) = INCLUDE_RE.captures(line) else {
                continue;
            };
            if limit != 0 && edges.len() >= limit {
                return json!({ "edges": edges, "truncated": true, "query": query_json(query) });
            }
            let include = &captures[2];
            let resolved = resolve_include(&search_paths, include)
                .map(|p| relative_or_original(root, &generic(&p)))
                .unwrap_or_default();
            edges.push(json!({
                "from": generic(&file.path),
                "line": index + 1,
                "include": include,
                "system": &captures[1] == "<",
                "resolved": resolved,
            }));
        }
    }
    json!({ "edges": edges, "truncated": false, "query": query_json(query) })
}

fn parse_cmake_targets(
    service: &BackgroundService,
    nodes: &mut Vec<Value>,
    edges: &mut Vec<Value>,
) {
    let scan = service.project_scan();
    let mut seen = HashSet::new();
    let mut add_node = |nodes: &mut Vec<Value>, name: &str, kind: &str, file: &Path| {
        if seen.insert(format!("{kind}:{name}")) {
            nodes.push(
                json!({ "name": name, "type": kind, "source": generic(file), "backend": "cmake" }),
            );
        }
    };
    for file in &scan.files {
        if file_name(&file.path) != "CMakeLists.txt" && extension_text(&file.path) != ".cmake" {
            continue;
        }
        let Ok(bytes) = std::fs::read(service.project_root().join(&file.path)) else {
            continue;
        };
        let content = String::from_utf8_lossy(&bytes);
        for captures in TARGET_RE.captures_iter(&content) {
            add_node(
                nodes,
                &captures[2],
                &captures[1].to_ascii_lowercase(),
                &file.path,
            );
        }
        for captures in LINK_RE.captures_iter(&content) {
            let target = &captures[1];
            add_node(nodes, target, "target", &file.path);
            for dep in captures[2].split_whitespace() {
                if matches!(dep, "PRIVATE" | "PUBLIC" | "INTERFACE") {
                    continue;
                }
                edges.push(
                    json!({ "from": target, "to": dep, "type": "links", "backend": "cmake" }),
                );
            }
        }
        for captures in SOURCES_RE.captures_iter(&content) {
            let target = &captures[1];
            for source in captures[2].split_whitespace() {
                if matches!(source, "PRIVATE" | "PUBLIC" | "INTERFACE") {
                    continue;
                }
                edges.push(json!({ "from": target, "to": source, "type": "uses_source", "backend": "cmake" }));
            }
        }
    }
}

fn parse_ninja_targets(
    service: &BackgroundService,
    nodes: &mut Vec<Value>,
    edges: &mut Vec<Value>,
) {
    let scan = service.project_scan();
    let mut seen = HashSet::new();
    for file in &scan.files {
        if file_name(&file.path) != "build.ninja" && extension_text(&file.path) != ".ninja" {
            continue;
        }
        let Some(lines) = read_lines(&service.project_root().join(&file.path)) else {
            continue;
        };
        for line in lines {
            let line = trim_ascii(&line);
            if !line.starts_with("build ") {
                continue;
            }
            let Some(colon) = line.find(':') else {
                continue;
            };
            let outputs: Vec<&str> = if colon >= 6 {
                trim_ascii(&line[6..colon]).split_whitespace().collect()
            } else {
                Vec::new()
            };
            for output in &outputs {
                if seen.insert(output.to_string()) {
                    nodes.push(json!({
                        "name": output,
                        "type": "output",
                        "source": generic(&file.path),
                        "backend": "ninja",
                    }));
                }
            }
            let mut rest = line[colon + 1..].split_whitespace();
            let rule = rest.next().unwrap_or_default();
            for dep in rest {
                if dep == "|" || dep == "||" {
                    continue;
                }
                for output in &outputs {
                    edges.push(
                        json!({ "from": output, "to": dep, "type": rule, "backend": "ninja" }),
                    );
                }
            }
        }
    }
}

pub(super) fn target_graph_payload(service: &BackgroundService, query: &Query) -> Value {
    let mut nodes = Vec::new();
    let mut edges = Vec::new();
    parse_cmake_targets(service, &mut nodes, &mut edges);
    parse_ninja_targets(service, &mut nodes, &mut edges);
    let limit = query_size(query, "limit", 500);
    if limit != 0 {
        nodes.truncate(limit);
        edges.truncate(limit);
    }
    json!({ "nodes": nodes, "edges": edges, "query": query_json(query) })
}
