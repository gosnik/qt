//! Read-only filesystem operations confined to the project root.

use std::path::{Path, PathBuf};

use serde_json::{Value, json};

use super::{
    OpResult, Query, failed, invalid, query_bool, query_json, query_size, read_lines,
    relative_project_path, requested_relative_file,
};
use crate::service::BackgroundService;
use crate::util::{
    generic, lexically_normal, should_skip_search_directory, sorted_dir_entries, weakly_canonical,
};

fn requested_relative_path(
    query: &Query,
    key: &str,
    fallback: &str,
) -> Result<PathBuf, super::OpError> {
    match query.get(key) {
        Some(value) if !value.is_empty() => relative_project_path(value),
        _ => relative_project_path(fallback),
    }
}

/// Resolve symlinks and reject paths that leave the project root.
fn checked_project_path(
    service: &BackgroundService,
    relative: &Path,
) -> Result<PathBuf, super::OpError> {
    let root = weakly_canonical(service.project_root());
    let full = weakly_canonical(&lexically_normal(&root.join(relative)));
    let root_text = generic(&root);
    let full_text = generic(&full);
    if full_text != root_text && !full_text.starts_with(&format!("{root_text}/")) {
        return Err(invalid("path escapes project root"));
    }
    Ok(full)
}

fn entry_payload(service: &BackgroundService, relative: &Path) -> OpResult {
    let full = checked_project_path(service, relative)?;
    let meta = std::fs::metadata(&full).ok();
    let is_dir = meta.as_ref().is_some_and(|m| m.is_dir());
    let is_file = meta.as_ref().is_some_and(|m| m.is_file());
    let name = relative
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| generic(relative));
    let mut entry = json!({
        "path": generic(relative),
        "name": name,
        "exists": meta.is_some(),
        "type": if is_dir { "directory" } else if is_file { "file" } else { "other" },
    });
    if is_file && let Some(meta) = meta {
        entry["size"] = json!(meta.len());
    }
    Ok(entry)
}

fn relative_child(service: &BackgroundService, path: &Path) -> Option<PathBuf> {
    let root = weakly_canonical(service.project_root());
    path.strip_prefix(&root).ok().map(Path::to_path_buf)
}

pub(super) fn stat_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let relative = requested_relative_path(query, "path", ".")?;
    let mut entry = entry_payload(service, &relative)?;
    entry["query"] = query_json(query);
    Ok(entry)
}

pub(super) fn list_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let relative = requested_relative_path(query, "path", ".")?;
    let full = checked_project_path(service, &relative)?;
    if !full.is_dir() {
        return Err(invalid("path must be a directory"));
    }
    let recursive = query_bool(query, "recursive", false);
    let max_depth = query_size(query, "depth", if recursive { 4 } else { 1 });
    let limit = query_size(query, "limit", 200);
    let mut entries: Vec<Value> = Vec::new();
    let full_limit = |entries: &Vec<Value>| limit != 0 && entries.len() >= limit;
    if recursive {
        let mut error = None;
        let _ = crate::util::walk(&full, |path, is_dir, depth| {
            if full_limit(&entries) || error.is_some() {
                return Err(crate::cancel::Cancelled);
            }
            if is_dir && should_skip_search_directory(path) {
                return Ok(false);
            }
            if depth + 1 > max_depth {
                return Ok(false);
            }
            if let Some(child) = relative_child(service, path) {
                match entry_payload(service, &child) {
                    Ok(entry) => entries.push(entry),
                    Err(e) => error = Some(e),
                }
            }
            Ok(true)
        });
        if let Some(error) = error {
            return Err(error);
        }
    } else {
        for entry in sorted_dir_entries(&full) {
            let path = entry.path();
            if crate::util::entry_is_dir(&entry) && should_skip_search_directory(&path) {
                continue;
            }
            if let Some(child) = relative_child(service, &path) {
                entries.push(entry_payload(service, &child)?);
            }
            if full_limit(&entries) {
                break;
            }
        }
    }
    Ok(json!({
        "path": generic(&relative),
        "recursive": recursive,
        "depth": max_depth,
        "entries": entries,
        "query": query_json(query),
    }))
}

pub(super) fn tree_payload(service: &BackgroundService, query: &Query) -> OpResult {
    if query.is_empty() {
        return Err(invalid(
            "filesystem tree requires at least one query parameter",
        ));
    }
    let mut tree_query = query.clone();
    tree_query.insert("recursive".into(), "1".into());
    tree_query
        .entry("depth".into())
        .or_insert_with(|| "3".into());
    let mut payload = list_payload(service, &tree_query)?;
    payload["query"] = query_json(&tree_query);
    Ok(payload)
}

pub(super) fn read_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let relative = requested_relative_file(query)?;
    let full = checked_project_path(service, &relative)?;
    if !crate::util::is_regular_file(&full) {
        return Err(invalid("file must be a regular file"));
    }
    let start_line = query_size(query, "start_line", 1);
    let end_line = query_size(query, "end_line", 0);
    let max_lines = query_size(query, "max_lines", 500);
    if start_line == 0 {
        return Err(invalid("start_line must be one-based"));
    }
    let Some(file_lines) = read_lines(&full) else {
        return Err(failed(format!(
            "could not open file: {}",
            generic(&relative)
        )));
    };
    let mut lines = Vec::new();
    let mut line_number = 0usize;
    let mut truncated = false;
    for text in &file_lines {
        line_number += 1;
        if line_number < start_line {
            continue;
        }
        if end_line != 0 && line_number > end_line {
            break;
        }
        if max_lines != 0 && lines.len() >= max_lines {
            truncated = true;
            break;
        }
        lines.push(json!({ "line": line_number, "text": text }));
    }
    Ok(json!({
        "file": generic(&relative),
        "start_line": start_line,
        "end_line": if end_line == 0 { line_number } else { end_line },
        "lines": lines,
        "truncated": truncated,
        "query": query_json(query),
    }))
}

pub(super) fn search_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let pattern = match query.get("pattern") {
        Some(p) if !p.is_empty() => p.clone(),
        _ => {
            return Err(invalid(
                "filesystem search requires pattern query parameter",
            ));
        }
    };
    let base = requested_relative_path(query, "path", ".")?;
    let base_full = checked_project_path(service, &base)?;
    let limit = query_size(query, "limit", 100);
    let mut matches: Vec<Value> = Vec::new();
    let mut error = None;
    let _ = crate::util::walk(&base_full, |path, is_dir, _| {
        if is_dir && should_skip_search_directory(path) {
            return Ok(false);
        }
        let Some(relative) = relative_child(service, path) else {
            return Ok(true);
        };
        if !generic(&relative).contains(&pattern) {
            return Ok(true);
        }
        match entry_payload(service, &relative) {
            Ok(entry) => matches.push(entry),
            Err(e) => {
                error = Some(e);
                return Err(crate::cancel::Cancelled);
            }
        }
        if limit != 0 && matches.len() >= limit {
            return Err(crate::cancel::Cancelled);
        }
        Ok(true)
    });
    if let Some(error) = error {
        return Err(error);
    }
    Ok(json!({ "path": generic(&base), "matches": matches, "query": query_json(query) }))
}
