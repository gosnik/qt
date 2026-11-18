//! Code-index operations: files, symbols, call graph, search, AST, context.

use std::collections::{BTreeMap, BTreeSet, HashMap, VecDeque};
use std::path::Path;

use serde_json::{Map, Value, json};

use super::{
    OpError, OpResult, Query, failed, invalid, query_bool, query_json, query_size, read_lines,
    requested_relative_file,
};
use crate::code_index::{call_json, class_json, function_json};
use crate::scan::{ProjectFile, ProjectFileKind};
use crate::service::BackgroundService;
use crate::util::{extension_text, file_name, generic, lexically_normal, path_text};

fn query_limit(query: &Query) -> usize {
    query_size(query, "limit", 0)
}

fn limited(values: Vec<Value>, limit: usize) -> Vec<Value> {
    if limit == 0 || values.len() <= limit {
        values
    } else {
        values.into_iter().take(limit).collect()
    }
}

fn str_field<'a>(object: &'a Value, key: &str) -> Option<&'a str> {
    object.get(key).and_then(Value::as_str)
}

/// When `query[key]` is set, `object[key]` must be a string containing it.
fn contains_filter(object: &Value, key: &str, query: &Query) -> bool {
    match query.get(key) {
        Some(wanted) if !wanted.is_empty() => {
            str_field(object, key).is_some_and(|v| v.contains(wanted.as_str()))
        }
        _ => true,
    }
}

fn path_prefix_matches(value: &str, prefix: &str) -> bool {
    if prefix.is_empty() || prefix == "." {
        return true;
    }
    let path_text = generic(&lexically_normal(Path::new(value)));
    let mut prefix_text = generic(&lexically_normal(Path::new(prefix)));
    if prefix_text.is_empty() || prefix_text == "." {
        return true;
    }
    if prefix_text.ends_with('/') {
        prefix_text.pop();
    }
    path_text == prefix_text || path_text.starts_with(&format!("{prefix_text}/"))
}

/// `path_prefix`, else `path` (both prefix matches), else `path_contains`.
fn path_filter_matches_text(value: &str, query: &Query) -> bool {
    if let Some(prefix) = query.get("path_prefix").filter(|p| !p.is_empty()) {
        return path_prefix_matches(value, prefix);
    }
    if let Some(path) = query.get("path").filter(|p| !p.is_empty()) {
        return path_prefix_matches(value, path);
    }
    match query.get("path_contains") {
        Some(needle) if !needle.is_empty() => value.contains(needle.as_str()),
        _ => true,
    }
}

fn path_filter_matches(object: &Value, key: &str, query: &Query) -> bool {
    str_field(object, key).is_some_and(|v| path_filter_matches_text(v, query))
}

pub(super) fn file_entry_payload(service: &BackgroundService, file: &ProjectFile) -> Value {
    let path = lexically_normal(&service.project_root().join(&file.path));
    let mut value = json!({
        "path": generic(&file.path),
        "file": generic(&file.path),
        "kind": file.kind.as_str(),
        "extension": extension_text(&file.path),
    });
    if let Ok(meta) = std::fs::metadata(&path) {
        value["size"] = json!(meta.len());
    }
    value
}

/// Lines `line - before ..= line + after` of a file.
pub fn source_lines_payload(
    path: &Path,
    relative: &Path,
    requested_line: usize,
    before: usize,
    after: usize,
) -> OpResult {
    if requested_line == 0 {
        return Err(invalid("line must be one-based"));
    }
    let Some(file_lines) = read_lines(path) else {
        return Err(failed(format!(
            "could not open file: {}",
            generic(relative)
        )));
    };
    let start_line = if requested_line > before {
        requested_line - before
    } else {
        1
    };
    let end_line = requested_line.saturating_add(after);
    let mut lines = Vec::new();
    let mut line_number = 0usize;
    for text in &file_lines {
        line_number += 1;
        if line_number < start_line {
            continue;
        }
        if line_number > end_line {
            break;
        }
        lines.push(
            json!({ "line": line_number, "text": text, "target": line_number == requested_line }),
        );
    }
    Ok(json!({
        "file": generic(relative),
        "line": requested_line,
        "before": before,
        "after": after,
        "start_line": start_line,
        "end_line": line_number.min(end_line),
        "lines": lines,
    }))
}

pub(super) fn read_context_payload(service: &BackgroundService, query: &Query) -> OpResult {
    if query.get("line").is_none_or(|l| l.is_empty()) {
        return Err(invalid(
            "read context requires file and line query parameters",
        ));
    }
    let relative = requested_relative_file(query)?;
    let mut payload = source_lines_payload(
        &lexically_normal(&service.project_root().join(&relative)),
        &relative,
        query_size(query, "line", 1),
        query_size(query, "before", 5),
        query_size(query, "after", 5),
    )?;
    payload["query"] = query_json(query);
    Ok(payload)
}

fn context_lines(
    service: &BackgroundService,
    symbol: &Value,
    before: usize,
    after: usize,
) -> OpResult {
    let mut context_query = Query::new();
    context_query.insert(
        "file".into(),
        str_field(symbol, "file").unwrap_or_default().to_string(),
    );
    context_query.insert(
        "line".into(),
        symbol
            .get("line")
            .and_then(Value::as_u64)
            .unwrap_or(1)
            .to_string(),
    );
    context_query.insert("before".into(), before.to_string());
    context_query.insert("after".into(), after.to_string());
    read_context_payload(service, &context_query)
}

pub(super) fn files_payload(service: &BackgroundService, query: &Query) -> OpResult {
    if query.is_empty() {
        return Err(invalid("files requires at least one query parameter"));
    }
    let scan = service.project_scan();
    let files: Vec<Value> = scan
        .files
        .iter()
        .map(|f| file_entry_payload(service, f))
        .filter(|entry| {
            contains_filter(entry, "kind", query)
                && path_filter_matches(entry, "path", query)
                && contains_filter(entry, "extension", query)
        })
        .collect();
    Ok(json!({
        "root": path_text(&scan.root),
        "files": limited(files, query_limit(query)),
        "total_files": scan.files.len(),
        "query": query_json(query),
    }))
}

pub(super) fn file_info_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let relative = requested_relative_file(query)?;
    let full = lexically_normal(&service.project_root().join(&relative));
    let mut value = json!({
        "file": generic(&relative),
        "exists": crate::util::is_regular_file(&full),
    });
    let scan = service.project_scan();
    if let Some(file) = scan.files.iter().find(|f| f.path == relative)
        && let (Some(target), Value::Object(entry)) =
            (value.as_object_mut(), file_entry_payload(service, file))
    {
        target.extend(entry);
    }
    if let Ok(meta) = std::fs::metadata(&full) {
        value["size"] = json!(meta.len());
    }
    Ok(value)
}

fn search_contains(line: &str, pattern: &str, case_sensitive: bool) -> bool {
    if case_sensitive {
        line.contains(pattern)
    } else {
        line.to_ascii_lowercase()
            .contains(&pattern.to_ascii_lowercase())
    }
}

pub(super) fn search_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let pattern = match query.get("pattern") {
        Some(p) if !p.is_empty() => p.clone(),
        _ => return Err(invalid("search requires pattern query parameter")),
    };
    let case_sensitive = query
        .get("case_sensitive")
        .is_some_and(|v| v != "0" && v != "false");
    let limit = query_size(query, "limit", 100);
    let before = query_size(query, "before", 0);
    let after = query_size(query, "after", 0);
    let scan = service.project_scan();
    let mut matches = Vec::new();
    for file in &scan.files {
        if !file.kind.is_searchable() {
            continue;
        }
        let entry = file_entry_payload(service, file);
        if !contains_filter(&entry, "file", query)
            || !path_filter_matches(&entry, "file", query)
            || !contains_filter(&entry, "kind", query)
            || !contains_filter(&entry, "extension", query)
        {
            continue;
        }
        let full = lexically_normal(&service.project_root().join(&file.path));
        let Some(lines) = read_lines(&full) else {
            continue;
        };
        for (index, line) in lines.iter().enumerate() {
            if !search_contains(line, &pattern, case_sensitive) {
                continue;
            }
            let line_number = index + 1;
            let mut found = json!({
                "file": generic(&file.path),
                "line": line_number,
                "text": line,
                "kind": file.kind.as_str(),
            });
            if before != 0 || after != 0 {
                let context = source_lines_payload(&full, &file.path, line_number, before, after)?;
                found["context"] = context["lines"].clone();
            }
            matches.push(found);
            if limit != 0 && matches.len() >= limit {
                return Ok(
                    json!({ "matches": matches, "query": query_json(query), "truncated": true }),
                );
            }
        }
    }
    Ok(json!({ "matches": matches, "query": query_json(query), "truncated": false }))
}

pub(super) fn fts_search_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let text = query.get("query").cloned().unwrap_or_default();
    let limit = match query.get("limit") {
        Some(limit) => super::parse_size(limit),
        None => 100,
    };
    crate::store::fts_search(
        &service.status().sqlite_database,
        service.project_root(),
        &text,
        limit,
    )
    .map_err(|error| match error {
        crate::store::FtsError::EmptyQuery => invalid(error.to_string()),
        crate::store::FtsError::Failed(message) => failed(message),
    })
}

pub(super) fn ast_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let relative = requested_relative_file(query)?;
    let status = service.status();
    let mut tree_name = relative.clone().into_os_string();
    tree_name.push(".tree");
    let tree_path = lexically_normal(
        &status
            .tree_sitter
            .output_directory
            .join("trees")
            .join(tree_name),
    );
    let Some(lines) = read_lines(&tree_path) else {
        return Err(failed(format!(
            "tree-sitter AST not available for file: {}",
            generic(&relative)
        )));
    };
    let max_lines = query_size(query, "max_lines", 200);
    let truncated = max_lines != 0 && lines.len() > max_lines;
    let shown: Vec<Value> = lines
        .iter()
        .take(if max_lines == 0 {
            usize::MAX
        } else {
            max_lines
        })
        .enumerate()
        .map(|(i, text)| json!({ "line": i + 1, "text": text }))
        .collect();
    Ok(json!({
        "file": generic(&relative),
        "tree": path_text(&tree_path),
        "lines": shown,
        "truncated": truncated,
        "query": query_json(query),
    }))
}

#[derive(Default)]
struct LineStats {
    total: usize,
    code: usize,
    blank: usize,
    comments: usize,
}

fn line_stats(path: &Path) -> LineStats {
    let mut stats = LineStats::default();
    let mut block_comment = false;
    for line in read_lines(path).unwrap_or_default() {
        stats.total += 1;
        let trimmed = line.trim();
        if trimmed.is_empty() {
            stats.blank += 1;
            continue;
        }
        if block_comment {
            stats.comments += 1;
            if trimmed.contains("*/") {
                block_comment = false;
            }
            continue;
        }
        if trimmed.starts_with("//") {
            stats.comments += 1;
            continue;
        }
        if trimmed.starts_with("/*") {
            stats.comments += 1;
            if !trimmed.contains("*/") {
                block_comment = true;
            }
            continue;
        }
        stats.code += 1;
    }
    stats
}

pub(super) fn project_summary_payload(service: &BackgroundService, query: &Query) -> Value {
    let scan = service.project_scan();
    let index = service.code_index();
    let mut kinds: BTreeMap<&str, usize> = BTreeMap::new();
    for file in &scan.files {
        *kinds.entry(file.kind.as_str()).or_default() += 1;
    }
    let mut callers_by_symbol: HashMap<&str, usize> = HashMap::new();
    let mut calls_by_file: HashMap<String, usize> = HashMap::new();
    for edge in &index.call_graph {
        *callers_by_symbol.entry(edge.callee.as_str()).or_default() += 1;
        *calls_by_file.entry(generic(&edge.file)).or_default() += 1;
    }
    let mut nodes: Vec<(usize, usize, Value)> = index
        .classes
        .iter()
        .map(|class| {
            let incoming = callers_by_symbol
                .get(class.qualified_name.as_str())
                .copied()
                .unwrap_or(0);
            (
                incoming,
                class.methods.len(),
                json!({
                    "name": class.qualified_name,
                    "kind": "class",
                    "file": generic(&class.file),
                    "line": class.line,
                    "methods": class.methods.len(),
                    "incoming_calls": incoming,
                }),
            )
        })
        .collect();
    nodes.sort_by(|a, b| b.0.cmp(&a.0).then(b.1.cmp(&a.1)));

    let mut symbols_by_file: HashMap<String, usize> = HashMap::new();
    for class in &index.classes {
        *symbols_by_file.entry(generic(&class.file)).or_default() += 1;
    }
    for function in &index.functions {
        *symbols_by_file.entry(generic(&function.file)).or_default() += 1;
    }
    let mut top_files: Vec<(usize, Value)> = Vec::new();
    for file in &scan.files {
        let text = generic(&file.path);
        let symbols = symbols_by_file.get(&text).copied().unwrap_or(0);
        let calls = calls_by_file.get(&text).copied().unwrap_or(0);
        if symbols == 0 && calls == 0 {
            continue;
        }
        let score = symbols * 10 + calls;
        top_files.push((
            score,
            json!({
                "file": text,
                "kind": file.kind.as_str(),
                "symbols": symbols,
                "call_edges": calls,
                "score": score,
            }),
        ));
    }
    top_files.sort_by_key(|a| std::cmp::Reverse(a.0));
    let limit = if query.contains_key("limit") {
        query_limit(query)
    } else {
        20
    };
    json!({
        "root": path_text(&scan.root),
        "files": scan.files.len(),
        "file_kinds": kinds,
        "symbols": {
            "classes": index.classes.len(),
            "functions": index.functions.len(),
            "call_graph_edges": index.call_graph.len(),
        },
        "build_files": scan.build_files_by_type,
        "architecture_nodes": limited(nodes.into_iter().map(|n| n.2).collect(), limit),
        "top_files": limited(top_files.into_iter().map(|f| f.1).collect(), limit),
        "query": query_json(query),
    })
}

pub(super) fn file_analysis_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let relative = requested_relative_file(query)?;
    let file_text = generic(&relative);
    let index = service.code_index();
    let classes: Vec<Value> = index
        .classes
        .iter()
        .filter(|c| generic(&c.file) == file_text)
        .map(class_json)
        .collect();
    let functions: Vec<_> = index
        .functions
        .iter()
        .filter(|f| generic(&f.file) == file_text)
        .collect();
    let mut outgoing = Vec::new();
    let mut incoming = Vec::new();
    for edge in &index.call_graph {
        if generic(&edge.file) == file_text {
            outgoing.push(call_json(edge));
        }
        if functions
            .iter()
            .any(|f| edge.callee == f.name || matches_symbol(&f.qualified_name, &edge.callee))
        {
            incoming.push(call_json(edge));
        }
    }
    let stats = line_stats(&lexically_normal(&service.project_root().join(&relative)));
    let limit = query_limit(query);
    Ok(json!({
        "file": file_text,
        "line_stats": {
            "total": stats.total,
            "code": stats.code,
            "blank": stats.blank,
            "comments": stats.comments,
        },
        "classes": classes,
        "functions": functions.iter().map(|f| function_json(f)).collect::<Vec<_>>(),
        "outgoing_calls": limited(outgoing, limit),
        "incoming_calls": limited(incoming, limit),
        "query": query_json(query),
    }))
}

pub(super) fn symbol_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let name = match query.get("name") {
        Some(name) if !name.is_empty() => name.clone(),
        _ => return Err(invalid("symbol requires name query parameter")),
    };
    let with_context = query
        .get("context")
        .is_some_and(|v| v != "0" && v != "false");
    let before = query_size(query, "before", 3);
    let after = query_size(query, "after", 8);
    let index = service.code_index();
    let mut symbols = Vec::new();
    let mut add = |mut symbol: Value, kind: &str| -> Result<(), OpError> {
        symbol["symbol_kind"] = json!(kind);
        if with_context {
            let context = context_lines(service, &symbol, before, after)?;
            symbol["context"] = context["lines"].clone();
        }
        symbols.push(symbol);
        Ok(())
    };
    for class in &index.classes {
        let value = class_json(class);
        if (class.name.contains(&name) || class.qualified_name.contains(&name))
            && contains_filter(&value, "file", query)
            && path_filter_matches(&value, "file", query)
        {
            add(
                value,
                if class.kind.is_empty() {
                    "class"
                } else {
                    &class.kind
                },
            )?;
        }
    }
    for function in &index.functions {
        let value = function_json(function);
        if (function.name.contains(&name) || function.qualified_name.contains(&name))
            && contains_filter(&value, "file", query)
            && path_filter_matches(&value, "file", query)
            && contains_filter(&value, "kind", query)
        {
            let kind = if function.kind.is_empty() {
                "function"
            } else {
                function.kind.as_str()
            };
            add(value, kind)?;
        }
    }
    Ok(json!({ "symbols": limited(symbols, query_limit(query)), "query": query_json(query) }))
}

/// Project / file / class / function nodes with CONTAINS_FILE, DEFINES and
/// CALLS edges.
pub fn memory_graph_payload(service: &BackgroundService) -> Value {
    let scan = service.project_scan();
    let index = service.code_index();
    let mut nodes = Vec::new();
    let mut edges = Vec::new();
    let project_id = format!("project:{}", path_text(&scan.root));
    nodes.push(json!({
        "id": project_id,
        "label": "Project",
        "name": file_name(&scan.root),
        "path": path_text(&scan.root),
    }));
    for file in &scan.files {
        let path = generic(&file.path);
        let file_id = format!("file:{path}");
        nodes.push(json!({
            "id": file_id,
            "label": "File",
            "name": file_name(&file.path),
            "path": path,
            "kind": file.kind.as_str(),
        }));
        edges.push(json!({ "from": project_id, "to": file_id, "type": "CONTAINS_FILE" }));
    }
    for class in &index.classes {
        let id = format!("class:{}", class.qualified_name);
        let file = generic(&class.file);
        nodes.push(json!({
            "id": id,
            "label": "Class",
            "name": class.name,
            "qualified_name": class.qualified_name,
            "file": file,
            "line": class.line,
        }));
        edges.push(json!({ "from": format!("file:{file}"), "to": id, "type": "DEFINES" }));
    }
    let mut ids_by_name: HashMap<&str, String> = HashMap::new();
    let mut ids_by_qualified: HashMap<&str, String> = HashMap::new();
    for function in &index.functions {
        let id = format!("function:{}", function.qualified_name);
        let file = generic(&function.file);
        nodes.push(json!({
            "id": id,
            "label": if function.kind == "method" { "Method" } else { "Function" },
            "name": function.name,
            "qualified_name": function.qualified_name,
            "file": file,
            "line": function.line,
        }));
        edges.push(json!({ "from": format!("file:{file}"), "to": id, "type": "DEFINES" }));
        ids_by_name.insert(function.name.as_str(), id.clone());
        ids_by_qualified.insert(function.qualified_name.as_str(), id);
    }
    for edge in &index.call_graph {
        let Some(from) = ids_by_qualified.get(edge.caller.as_str()) else {
            continue;
        };
        let to = ids_by_qualified
            .get(edge.callee.as_str())
            .or_else(|| ids_by_name.get(edge.callee.as_str()))
            .cloned()
            .unwrap_or_else(|| format!("external:{}", edge.callee));
        edges.push(json!({
            "from": from,
            "to": to,
            "type": "CALLS",
            "callee": edge.callee,
            "file": generic(&edge.file),
            "line": edge.line,
        }));
    }
    json!({
        "format": "index-codebase-memory-v1",
        "root": path_text(&scan.root),
        "nodes": nodes,
        "edges": edges,
    })
}

fn persist_memory_graph(service: &BackgroundService, mut graph: Value) -> OpResult {
    let directory = service.options().state_directory.join("codebase-memory");
    let path = directory.join("graph.json");
    let text = format!(
        "{}\n",
        serde_json::to_string_pretty(&graph).unwrap_or_default()
    );
    std::fs::create_dir_all(&directory)
        .and_then(|_| std::fs::write(&path, text))
        .map_err(|_| {
            failed(format!(
                "could not write codebase memory graph: {}",
                path_text(&path)
            ))
        })?;
    graph["artifact"] = json!(path_text(&path));
    Ok(graph)
}

pub(super) fn memory_graph_operation(service: &BackgroundService, query: &Query) -> OpResult {
    let mut graph = memory_graph_payload(service);
    let object = graph.as_object_mut().expect("graph is an object");
    if !query_bool(query, "include_nodes", true) {
        object.remove("nodes");
    }
    if !query_bool(query, "include_edges", true) {
        object.remove("edges");
    }
    if !query.is_empty() {
        object.insert("query".into(), query_json(query));
    }
    if query_bool(query, "persist", true) {
        persist_memory_graph(service, graph)
    } else {
        Ok(graph)
    }
}

pub(super) fn graph_schema_payload(service: &BackgroundService, query: &Query) -> Value {
    let graph = memory_graph_payload(service);
    let empty = Vec::new();
    let nodes = graph["nodes"].as_array().unwrap_or(&empty);
    let edges = graph["edges"].as_array().unwrap_or(&empty);
    let mut labels: BTreeMap<String, usize> = BTreeMap::new();
    let mut edge_types: BTreeMap<String, usize> = BTreeMap::new();
    for node in nodes {
        *labels
            .entry(str_field(node, "label").unwrap_or_default().to_string())
            .or_default() += 1;
    }
    for edge in edges {
        *edge_types
            .entry(str_field(edge, "type").unwrap_or_default().to_string())
            .or_default() += 1;
    }
    let mut payload = Map::new();
    payload.insert("node_labels".into(), json!(labels));
    payload.insert("edge_types".into(), json!(edge_types));
    if query_bool(query, "include_counts", true) {
        payload.insert("nodes".into(), json!(nodes.len()));
        payload.insert("edges".into(), json!(edges.len()));
    }
    if !query.is_empty() {
        payload.insert("query".into(), query_json(query));
    }
    Value::Object(payload)
}

pub(super) fn search_graph_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let graph = memory_graph_payload(service);
    let label = query.get("label").cloned().unwrap_or_default();
    let file = query.get("file").cloned().unwrap_or_default();
    let pattern = match query.get("name_pattern").filter(|p| !p.is_empty()) {
        Some(text) => Some(
            regex::Regex::new(text).map_err(|e| failed(format!("invalid name_pattern: {e}")))?,
        ),
        None => None,
    };
    let offset = query_size(query, "offset", 0);
    let limit = query_size(query, "limit", 100);
    let mut results = Vec::new();
    let mut skipped = 0usize;
    for node in graph["nodes"].as_array().into_iter().flatten() {
        if !label.is_empty() && str_field(node, "label") != Some(label.as_str()) {
            continue;
        }
        let node_file = str_field(node, "file")
            .or_else(|| str_field(node, "path"))
            .unwrap_or_default();
        if !file.is_empty() && !node_file.contains(&file) {
            continue;
        }
        let name = str_field(node, "qualified_name")
            .or_else(|| str_field(node, "name"))
            .unwrap_or_default();
        if pattern.as_ref().is_some_and(|p| !p.is_match(name)) {
            continue;
        }
        let skip = skipped < offset;
        skipped += 1;
        if skip {
            continue;
        }
        results.push(node.clone());
        if limit != 0 && results.len() >= limit {
            break;
        }
    }
    Ok(json!({ "results": results, "query": query_json(query) }))
}

/// `name` and each shorter qualified suffix of it, split at `::` (C/C++,
/// Rust) or `.` (TypeScript): `a::B::run` → `a::B::run`, `B::run`, `run`.
fn qualified_suffixes(name: &str) -> impl Iterator<Item = &str> {
    std::iter::once(name).chain(name.char_indices().filter_map(move |(i, c)| match c {
        ':' if name[i + 1..].starts_with(':') => Some(&name[i + 2..]),
        '.' => Some(&name[i + 1..]),
        _ => None,
    }))
}

/// Whether two symbol references name the same function: equal, or one is a
/// qualified suffix of the other (`ns::Foo::run` and `Foo::run` or `run`;
/// `Store.save` and `save`). Call edges record the callee as written, often
/// unqualified, so the match goes both ways.
fn matches_symbol(candidate: &str, current: &str) -> bool {
    let (long, short) = if candidate.len() >= current.len() {
        (candidate, current)
    } else {
        (current, candidate)
    };
    !short.is_empty() && qualified_suffixes(long).any(|suffix| suffix == short)
}

pub(super) fn trace_call_path_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let function = match query.get("function_name") {
        Some(f) if !f.is_empty() => f.clone(),
        _ => {
            return Err(invalid(
                "trace call path requires function_name query parameter",
            ));
        }
    };
    let direction = query
        .get("direction")
        .cloned()
        .unwrap_or_else(|| "both".into());
    let max_depth = query_size(query, "depth", 3).min(5);
    let index = service.code_index();
    let mut paths = Vec::new();
    let mut visited = BTreeSet::new();
    let mut pending = VecDeque::new();
    pending.push_back((function.clone(), 0usize));
    visited.insert(function.clone());
    let outbound_allowed = direction == "outbound" || direction == "both";
    let inbound_allowed = direction == "inbound" || direction == "both";
    while let Some((current, depth)) = pending.pop_front() {
        if depth >= max_depth {
            continue;
        }
        for edge in &index.call_graph {
            let outbound = outbound_allowed && matches_symbol(&edge.caller, &current);
            let inbound = inbound_allowed && matches_symbol(&edge.callee, &current);
            if !outbound && !inbound {
                continue;
            }
            paths.push(json!({
                "depth": depth + 1,
                "direction": if outbound { "outbound" } else { "inbound" },
                "caller": edge.caller,
                "callee": edge.callee,
                "file": generic(&edge.file),
                "line": edge.line,
            }));
            let next = if outbound { &edge.callee } else { &edge.caller };
            if !next.is_empty() && visited.insert(next.clone()) {
                pending.push_back((next.clone(), depth + 1));
            }
        }
    }
    Ok(json!({
        "function_name": function,
        "direction": direction,
        "depth": max_depth,
        "paths": limited(paths, query_limit(query)),
        "query": query_json(query),
    }))
}

pub(super) fn dead_code_payload(service: &BackgroundService, query: &Query) -> Value {
    let index = service.code_index();
    let called: BTreeSet<&str> = index.call_graph.iter().map(|e| e.callee.as_str()).collect();
    let functions: Vec<Value> = index
        .functions
        .iter()
        .filter(|f| {
            f.name != "main"
                && !qualified_suffixes(&f.qualified_name).any(|suffix| called.contains(suffix))
                && !called.contains(f.name.as_str())
        })
        .map(function_json)
        .collect();
    json!({ "functions": limited(functions, query_limit(query)), "query": query_json(query) })
}

pub(super) fn code_snippet_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let wanted = match query.get("qualified_name") {
        Some(n) if !n.is_empty() => n.clone(),
        _ => {
            return Err(invalid(
                "code snippet requires qualified_name query parameter",
            ));
        }
    };
    let index = service.code_index();
    let Some(function) = index
        .functions
        .iter()
        .find(|f| f.qualified_name.contains(&wanted))
    else {
        return Err(failed(format!("symbol not found: {wanted}")));
    };
    let mut payload = context_lines(
        service,
        &function_json(function),
        query_size(query, "before", 2),
        query_size(query, "after", 20),
    )?;
    payload["qualified_name"] = json!(function.qualified_name);
    Ok(payload)
}

pub(super) fn classes_payload(service: &BackgroundService, query: &Query) -> Value {
    let index = service.code_index();
    let classes: Vec<Value> = index
        .classes
        .iter()
        .map(class_json)
        .filter(|c| {
            contains_filter(c, "name", query)
                && contains_filter(c, "qualified_name", query)
                && contains_filter(c, "kind", query)
                && contains_filter(c, "file", query)
                && path_filter_matches(c, "file", query)
        })
        .collect();
    json!({ "classes": limited(classes, query_limit(query)), "query": query_json(query) })
}

pub(super) fn functions_payload(service: &BackgroundService, query: &Query) -> Value {
    let index = service.code_index();
    let functions: Vec<Value> = index
        .functions
        .iter()
        .map(function_json)
        .filter(|f| {
            contains_filter(f, "name", query)
                && contains_filter(f, "qualified_name", query)
                && contains_filter(f, "file", query)
                && path_filter_matches(f, "file", query)
                && contains_filter(f, "kind", query)
        })
        .collect();
    json!({ "functions": limited(functions, query_limit(query)), "query": query_json(query) })
}

pub(super) fn callgraph_payload(service: &BackgroundService, query: &Query) -> Value {
    let index = service.code_index();
    let edges: Vec<Value> = index
        .call_graph
        .iter()
        .map(call_json)
        .filter(|e| {
            contains_filter(e, "caller", query)
                && contains_filter(e, "callee", query)
                && contains_filter(e, "file", query)
                && path_filter_matches(e, "file", query)
        })
        .collect();
    json!({ "call_graph": limited(edges, query_limit(query)), "query": query_json(query) })
}

/// Kinds counted as C/C++ for the include graph.
pub(super) fn is_cpp_kind(kind: ProjectFileKind) -> bool {
    kind.is_cpp_candidate()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn path_filters() {
        assert!(path_prefix_matches("src/a/b.cpp", "src/a"));
        assert!(path_prefix_matches("src/a/b.cpp", "src/a/"));
        assert!(!path_prefix_matches("src/ab/b.cpp", "src/a"));
        assert!(path_prefix_matches("x", "."));
        let mut q = Query::new();
        q.insert("path_contains".into(), "lib".into());
        assert!(path_filter_matches_text("src/lib/x.c", &q));
        assert!(!path_filter_matches_text("src/x.c", &q));
    }

    #[test]
    fn qualified_suffix_matching() {
        assert!(matches_symbol("ns::Foo::run", "run"));
        assert!(matches_symbol("ns::Foo::run", "Foo::run"));
        assert!(!matches_symbol("rerun", "run"));
        assert!(matches_symbol("run", "run"));
        // Callees recorded unqualified still match a qualified query.
        assert!(matches_symbol("Foo::run", "ns::Foo::run"));
        assert!(!matches_symbol("Bar::run", "ns::Foo::run"));
        // TypeScript qualification.
        assert!(matches_symbol("Store.save", "save"));
        assert!(matches_symbol("Util.Store.save", "Store.save"));
        assert!(!matches_symbol("Store.save", ""));
        let suffixes: Vec<&str> = qualified_suffixes("a::B.run").collect();
        assert_eq!(suffixes, ["a::B.run", "B.run", "run"]);
    }
}
