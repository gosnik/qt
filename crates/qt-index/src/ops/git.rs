//! Read-only git operations (`git -C <root> ...`).

use std::path::PathBuf;
use std::process::{Command, Stdio};

use serde_json::{Value, json};

use super::{
    OpResult, Query, failed, invalid, query_bool, query_json, query_size, query_value,
    relative_project_path, requested_relative_file,
};
use crate::service::BackgroundService;
use crate::util::{generic, path_text};

fn git_on_path() -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path)
        .filter(|dir| !dir.as_os_str().is_empty())
        .map(|dir| dir.join(if cfg!(windows) { "git.exe" } else { "git" }))
        .find(|candidate| candidate.exists())
}

fn optional_git_file(query: &Query) -> Result<Option<String>, super::OpError> {
    match query.get("file") {
        Some(file) if !file.is_empty() => Ok(Some(generic(&relative_project_path(file)?))),
        _ => Ok(None),
    }
}

fn checked_git_revision(query: &Query) -> Result<String, super::OpError> {
    let revision = query_value(query, "rev", "HEAD");
    if revision.is_empty() || revision.starts_with('-') {
        return Err(invalid("git revision must not be empty or start with '-'"));
    }
    Ok(revision.to_string())
}

fn lines(bytes: &[u8]) -> Vec<String> {
    let text = String::from_utf8_lossy(bytes);
    crate::util::getlines(&text).map(str::to_string).collect()
}

fn git_command_payload(
    service: &BackgroundService,
    args: Vec<String>,
    max_lines: usize,
) -> OpResult {
    let git = git_on_path().ok_or_else(|| failed("git executable not found on PATH"))?;
    let mut full_args = vec!["-C".to_string(), path_text(service.project_root())];
    full_args.extend(args);
    let output = Command::new(&git)
        .args(&full_args)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| failed(format!("could not run git: {e}")))?;
    let stdout_all = lines(&output.stdout);
    let truncated = max_lines != 0 && stdout_all.len() > max_lines;
    let stdout: Vec<String> = if max_lines == 0 {
        stdout_all
    } else {
        stdout_all.into_iter().take(max_lines).collect()
    };
    Ok(json!({
        "args": full_args,
        "exit_code": output.status.code().unwrap_or(-1),
        "stdout": stdout,
        "stderr": lines(&output.stderr),
        "truncated": truncated,
    }))
}

fn with_query(mut payload: Value, query: &Query) -> Value {
    payload["query"] = query_json(query);
    payload
}

pub(super) fn status_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let mut args = vec!["status".to_string()];
    if query_bool(query, "short", true) {
        args.push("--short".into());
        args.push("--branch".into());
    }
    Ok(with_query(
        git_command_payload(service, args, query_size(query, "limit", 200))?,
        query,
    ))
}

pub(super) fn log_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let limit = query_size(query, "limit", 20);
    let mut args = vec![
        "log".to_string(),
        "--oneline".into(),
        "--decorate".into(),
        format!("--max-count={limit}"),
    ];
    if let Some(file) = optional_git_file(query)? {
        args.push("--".into());
        args.push(file);
    }
    Ok(with_query(
        git_command_payload(service, args, limit)?,
        query,
    ))
}

pub(super) fn diff_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let mut args = vec!["diff".to_string()];
    if query_bool(query, "staged", false) {
        args.push("--cached".into());
    }
    if query_bool(query, "stat", false) {
        args.push("--stat".into());
    }
    if let Some(file) = optional_git_file(query)? {
        args.push("--".into());
        args.push(file);
    }
    Ok(with_query(
        git_command_payload(service, args, query_size(query, "limit", 300))?,
        query,
    ))
}

pub(super) fn show_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let args = vec![
        "show".to_string(),
        "--stat".into(),
        "--decorate".into(),
        checked_git_revision(query)?,
    ];
    Ok(with_query(
        git_command_payload(service, args, query_size(query, "limit", 200))?,
        query,
    ))
}

pub(super) fn blame_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let file = requested_relative_file(query)?;
    let start_line = query_size(query, "start_line", 1);
    let end_line = query_size(query, "end_line", start_line);
    let args = vec![
        "blame".to_string(),
        "-L".into(),
        format!("{start_line},{end_line}"),
        "--".into(),
        generic(&file),
    ];
    Ok(with_query(
        git_command_payload(service, args, query_size(query, "limit", 200))?,
        query,
    ))
}

pub(super) fn branches_payload(service: &BackgroundService, query: &Query) -> OpResult {
    let mut args = vec!["branch".to_string(), "--list".into()];
    if query_bool(query, "all", false) {
        args.insert(1, "--all".into());
    }
    Ok(with_query(
        git_command_payload(service, args, query_size(query, "limit", 200))?,
        query,
    ))
}
