//! Core operations: status, rescan, metadata, tree-sitter failures.

use serde_json::{Value, json};

use super::{Query, query_json, query_size};
use crate::service::BackgroundService;
use crate::util::path_text;

pub(super) fn status_payload(service: &BackgroundService, query: &Query) -> Value {
    let mut status = service.status().to_json();
    let object = status.as_object_mut().expect("status is an object");
    if !query.is_empty() {
        object.insert("query".into(), query_json(query));
    }
    if matches!(
        query.get("include_tree_sitter").map(String::as_str),
        Some("false" | "0")
    ) {
        object.remove("tree_sitter");
    }
    if !query.contains_key("include_build_metadata") {
        object.remove("build_metadata");
    }
    status
}

pub(super) fn rescan_payload(service: &BackgroundService, query: &Query) -> Value {
    if service.status().running {
        service.request_scan();
    } else {
        service.run_once();
    }
    json!({ "requested": true, "query": query_json(query) })
}

pub(super) fn metadata_payload(service: &BackgroundService, query: &Query) -> Value {
    let mut payload = serde_json::Map::new();
    let options = service.options();
    if query.is_empty() || query.contains_key("include_compile_commands") {
        payload.insert(
            "compile_commands".into(),
            Value::String(path_text(&options.compile_commands_output)),
        );
    }
    if query.is_empty() || query.contains_key("include_tree_sitter") {
        payload.insert(
            "tree_sitter".into(),
            Value::String(path_text(&options.tree_sitter.output_directory)),
        );
    }
    if query.is_empty() || query.contains_key("include_sqlite") {
        payload.insert(
            "sqlite".into(),
            Value::String(path_text(&service.status().sqlite_database)),
        );
    }
    if !query.is_empty() {
        payload.insert("query".into(), query_json(query));
    }
    Value::Object(payload)
}

pub(super) fn tree_sitter_failures_payload(service: &BackgroundService, query: &Query) -> Value {
    let status = service.status();
    let limit = query_size(query, "limit", 200);
    let prefix_ok = |key: &str, file: &str| match query.get(key) {
        Some(prefix) if !prefix.is_empty() => file.starts_with(prefix.as_str()),
        _ => true,
    };
    let contains_ok = |file: &str| match query.get("path_contains") {
        Some(needle) if !needle.is_empty() => file.contains(needle.as_str()),
        _ => true,
    };
    let mut failures = Vec::new();
    let mut matched = 0usize;
    for file in &status.tree_sitter.failed_files {
        if !(prefix_ok("path", file) && prefix_ok("path_prefix", file) && contains_ok(file)) {
            continue;
        }
        matched += 1;
        if failures.len() < limit {
            failures.push(json!({ "file": file }));
        }
    }
    let ts = &status.tree_sitter;
    json!({
        "attempted": ts.attempted,
        "available": ts.available,
        "generated": ts.generated,
        "mode": ts.mode,
        "files_failed": ts.files_failed,
        "files_skipped": ts.files_skipped,
        "matched": matched,
        "failures": failures,
        "skipped_files": ts.skipped_files,
        "message": ts.message,
        "query": query_json(query),
    })
}
