//! Index operations behind the `index` router and the direct tools.
//!
//! Each operation takes a string query map (the archived `HttpRequest`
//! query) and returns a JSON payload. Arguments are strings because the
//! router converts validated tool arguments to text, exactly as the archived
//! tools did (`true`/`false` become `1`/`0`).

mod build;
mod code;
mod core;
mod fs;
mod git;

use std::collections::BTreeMap;
use std::path::{Component, Path, PathBuf};

use serde_json::Value;

use crate::service::BackgroundService;
use crate::util::lexically_normal;

pub use code::source_lines_payload;

/// Operation arguments as text.
pub type Query = BTreeMap<String, String>;

/// A failed operation. `Invalid` maps to an invalid-request tool result and
/// `Failed` to an execution error.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OpError {
    Invalid(String),
    Failed(String),
}

impl OpError {
    pub fn message(&self) -> &str {
        match self {
            Self::Invalid(m) | Self::Failed(m) => m,
        }
    }
}

impl std::fmt::Display for OpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.message())
    }
}

pub type OpResult = Result<Value, OpError>;

pub(crate) fn invalid(message: impl Into<String>) -> OpError {
    OpError::Invalid(message.into())
}

pub(crate) fn failed(message: impl Into<String>) -> OpError {
    OpError::Failed(message.into())
}

/// `strtoull` semantics: leading whitespace, optional sign, digits; anything
/// else yields 0. Negative values wrap (so `-1` means "unbounded").
pub(crate) fn parse_size(text: &str) -> usize {
    let text = text.trim_start();
    let (negative, digits) = match text.as_bytes().first() {
        Some(b'-') => (true, &text[1..]),
        Some(b'+') => (false, &text[1..]),
        _ => (false, text),
    };
    let digits: String = digits.chars().take_while(char::is_ascii_digit).collect();
    if digits.is_empty() {
        return 0;
    }
    let value = digits
        .parse::<u128>()
        .map(|v| v.min(usize::MAX as u128) as usize)
        .unwrap_or(usize::MAX);
    if negative {
        value.wrapping_neg()
    } else {
        value
    }
}

pub(crate) fn query_size(query: &Query, key: &str, fallback: usize) -> usize {
    match query.get(key) {
        Some(value) if !value.is_empty() => parse_size(value),
        _ => fallback,
    }
}

pub(crate) fn query_bool(query: &Query, key: &str, fallback: bool) -> bool {
    match query.get(key) {
        Some(value) if !value.is_empty() => value != "0" && value != "false",
        _ => fallback,
    }
}

pub(crate) fn query_value<'a>(query: &'a Query, key: &str, fallback: &'a str) -> &'a str {
    match query.get(key) {
        Some(value) if !value.is_empty() => value,
        _ => fallback,
    }
}

pub(crate) fn query_json(query: &Query) -> Value {
    Value::Object(
        query
            .iter()
            .map(|(k, v)| (k.clone(), Value::String(v.clone())))
            .collect(),
    )
}

/// Validate a project-relative path: not empty, not absolute, no `..`, no `.git`.
pub(crate) fn relative_project_path(value: &str) -> Result<PathBuf, OpError> {
    let mut relative = lexically_normal(Path::new(value));
    if value.is_empty() || relative.is_absolute() || value.starts_with('/') {
        return Err(invalid("file must be a relative path inside the project"));
    }
    if relative.as_os_str().is_empty() {
        relative = PathBuf::from(".");
    }
    for part in relative.components() {
        match part {
            Component::ParentDir => return Err(invalid("file must not contain '..'")),
            Component::Normal(name) if name == ".git" => {
                return Err(invalid("path must not reference .git"));
            }
            _ => {}
        }
    }
    Ok(relative)
}

pub(crate) fn requested_relative_file(query: &Query) -> Result<PathBuf, OpError> {
    match query.get("file") {
        Some(file) if !file.is_empty() => relative_project_path(file),
        _ => Err(invalid("file query parameter is required")),
    }
}

/// Run the operation implemented by `handler` (archived handler name such as
/// `index_symbol`).
pub fn call_operation(handler: &str, service: &BackgroundService, query: &Query) -> OpResult {
    match handler {
        "index_status" => Ok(core::status_payload(service, query)),
        "index_rescan" => Ok(core::rescan_payload(service, query)),
        "index_metadata" => Ok(core::metadata_payload(service, query)),
        "index_tree_sitter_failures" => Ok(core::tree_sitter_failures_payload(service, query)),
        "index_project_summary" => Ok(code::project_summary_payload(service, query)),
        "index_memory_graph" => code::memory_graph_operation(service, query),
        "index_graph_schema" => Ok(code::graph_schema_payload(service, query)),
        "index_search_graph" => code::search_graph_payload(service, query),
        "index_trace_call_path" => code::trace_call_path_payload(service, query),
        "index_dead_code" => Ok(code::dead_code_payload(service, query)),
        "index_code_snippet" => code::code_snippet_payload(service, query),
        "index_fs_stat" => fs::stat_payload(service, query),
        "index_fs_list" => fs::list_payload(service, query),
        "index_fs_tree" => fs::tree_payload(service, query),
        "index_fs_read" => fs::read_payload(service, query),
        "index_fs_search" => fs::search_payload(service, query),
        "index_git_status" => git::status_payload(service, query),
        "index_git_log" => git::log_payload(service, query),
        "index_git_diff" => git::diff_payload(service, query),
        "index_git_show" => git::show_payload(service, query),
        "index_git_blame" => git::blame_payload(service, query),
        "index_git_branches" => git::branches_payload(service, query),
        "index_build_target_graph" => Ok(build::target_graph_payload(service, query)),
        "index_build_compile_flags" => Ok(build::compile_flags_payload(service, query)),
        "index_build_include_graph" => Ok(build::include_graph_payload(service, query)),
        "index_build_compile_commands" => Ok(build::compile_commands_payload(service, query)),
        "index_files" => code::files_payload(service, query),
        "index_file_info" => code::file_info_payload(service, query),
        "index_file_analysis" => code::file_analysis_payload(service, query),
        "index_symbol" => code::symbol_payload(service, query),
        "index_search" => code::search_payload(service, query),
        "index_fts_search" => code::fts_search_payload(service, query),
        "index_ast" => code::ast_payload(service, query),
        "index_classes" => Ok(code::classes_payload(service, query)),
        "index_functions" => Ok(code::functions_payload(service, query)),
        "index_callgraph" => Ok(code::callgraph_payload(service, query)),
        "index_read_context" => code::read_context_payload(service, query),
        other => Err(invalid(format!("unknown index tool: {other}"))),
    }
}

/// Read a file's lines as `std::getline` would (lossy UTF-8).
pub(crate) fn read_lines(path: &Path) -> Option<Vec<String>> {
    let bytes = std::fs::read(path).ok()?;
    let text = String::from_utf8_lossy(&bytes);
    Some(crate::util::getlines(&text).map(str::to_string).collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn size_parsing_matches_strtoull() {
        assert_eq!(parse_size("42"), 42);
        assert_eq!(parse_size(" 7x"), 7);
        assert_eq!(parse_size("abc"), 0);
        assert_eq!(parse_size("-1"), usize::MAX);
    }

    #[test]
    fn relative_paths_are_validated() {
        assert_eq!(
            relative_project_path("a/./b").unwrap(),
            PathBuf::from("a/b")
        );
        assert_eq!(
            relative_project_path("../x").unwrap_err(),
            invalid("file must not contain '..'")
        );
        assert_eq!(
            relative_project_path("/etc/passwd").unwrap_err(),
            invalid("file must be a relative path inside the project")
        );
        assert_eq!(
            relative_project_path(".git/config").unwrap_err(),
            invalid("path must not reference .git")
        );
        assert_eq!(relative_project_path(".").unwrap(), PathBuf::from("."));
        assert!(relative_project_path("").is_err());
    }
}
