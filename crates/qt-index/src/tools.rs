//! Model-facing index tools: `symbol`, `analyze_file`, `references`,
//! `build_context`, `search_index` and the grouped `index` router.
//!
//! Schemas, descriptions, limits and error payloads follow the archived
//! `index_tool.cpp`. Arguments are validated (with coercion) against each
//! tool's schema; on failure the result carries a retryable
//! `invalid_arguments` (or, for router operations, `invalid_index_arguments`)
//! payload with the expected schema.

use std::path::PathBuf;
use std::sync::Arc;
use std::time::{Duration, Instant};

use futures::future::BoxFuture;
use qt_core::tools::{Tool, ToolContext, ToolOutput};
use serde_json::{Map, Value, json};

use crate::ops::{OpError, Query, call_operation};
use crate::schema::validate_tool_arguments;
use crate::service::{BackgroundService, IndexManager};

/// Archived `OutputGuardOptions` defaults.
const MAX_STRING_BYTES: usize = 64 * 1024;
const MAX_ARRAY_ITEMS: usize = 1000;
const READY_WAIT: Duration = Duration::from_secs(3);
const READY_POLL: Duration = Duration::from_millis(50);

/// Archived result status of a tool call.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Status {
    Success,
    InvalidRequest,
    ExecutionError,
}

impl Status {
    fn as_str(self) -> &'static str {
        match self {
            Self::Success => "success",
            Self::InvalidRequest => "invalid_request",
            Self::ExecutionError => "execution_error",
        }
    }
}

/// An archived `ToolResult`: status, optional error message and output JSON.
#[derive(Debug, Clone, PartialEq)]
struct IndexResult {
    status: Status,
    error: Option<String>,
    output: Value,
}

impl IndexResult {
    fn success(output: Value) -> Self {
        Self {
            status: Status::Success,
            error: None,
            output,
        }
    }

    fn invalid(message: impl Into<String>) -> Self {
        Self {
            status: Status::InvalidRequest,
            error: Some(message.into()),
            output: Value::Null,
        }
    }

    fn execution(message: impl Into<String>) -> Self {
        Self {
            status: Status::ExecutionError,
            error: Some(message.into()),
            output: Value::Null,
        }
    }

    fn with_output(mut self, output: Value) -> Self {
        self.output = output;
        self
    }

    fn from_op_error(error: OpError) -> Self {
        match error {
            OpError::Invalid(message) => Self::invalid(message),
            OpError::Failed(message) => Self::execution(message),
        }
    }

    /// Success: the output JSON. Errors: the message, followed by the
    /// structured output (when any) as JSON.
    fn into_tool_output(self) -> ToolOutput {
        match self.status {
            Status::Success => {
                ToolOutput::text(serde_json::to_string(&self.output).unwrap_or_default())
                    .with_details(json!({ "status": self.status.as_str() }))
            }
            status => {
                let message = self.error.unwrap_or_else(|| status.as_str().to_string());
                let text = if self.output.is_null() {
                    message
                } else {
                    format!(
                        "{message}\n{}",
                        serde_json::to_string(&self.output).unwrap_or_default()
                    )
                };
                let mut details = json!({ "status": status.as_str() });
                if let Some(code) = self.output.get("code") {
                    details["code"] = code.clone();
                }
                ToolOutput::error(text).with_details(details)
            }
        }
    }
}

fn object_schema(properties: Value, required: &[&str]) -> Value {
    json!({
        "type": "object",
        "properties": properties,
        "required": required,
        "additionalProperties": false,
    })
}

/// Convert validated arguments to the operation query (strings; booleans as
/// `1`/`0`; objects, arrays and nulls dropped).
fn query_from_arguments(arguments: &Value) -> Query {
    let mut query = Query::new();
    if let Value::Object(object) = arguments {
        for (key, value) in object {
            let text = match value {
                Value::String(s) => s.clone(),
                Value::Bool(b) => if *b { "1" } else { "0" }.to_string(),
                Value::Number(n) => match (n.as_u64(), n.as_i64(), n.as_f64()) {
                    (Some(u), _, _) => u.to_string(),
                    (_, Some(i), _) => i.to_string(),
                    (_, _, Some(f)) => format!("{f:.6}"),
                    _ => continue,
                },
                _ => continue,
            };
            query.insert(key.clone(), text);
        }
    }
    query
}

fn normalize_arguments(args: Value) -> Value {
    match args {
        Value::Null => Value::Object(Map::new()),
        other => other,
    }
}

/// Validate against a direct tool's schema, producing the archived
/// `invalid_arguments` payload on failure.
fn validate_direct(name: &str, schema: &Value, args: &Value) -> Result<Value, IndexResult> {
    validate_tool_arguments(name, schema, args).map_err(|message| {
        IndexResult::invalid(message).with_output(json!({
            "code": "invalid_arguments",
            "tool": name,
            "retryable": true,
            "received_arguments": args,
            "expected_schema": schema,
        }))
    })
}

fn truncate_utf8(text: &str, max: usize) -> &str {
    let mut end = max.min(text.len());
    while end > 0 && !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

fn guard_value(value: &mut Value, path: &str, fields: &mut Vec<Value>) {
    match value {
        Value::String(text) if text.len() > MAX_STRING_BYTES => {
            let original = text.len();
            *text = truncate_utf8(text, MAX_STRING_BYTES).to_string();
            fields.push(json!({
                "path": path,
                "type": "string",
                "original_bytes": original,
                "preview_bytes": MAX_STRING_BYTES,
            }));
        }
        Value::Array(items) => {
            let original = items.len();
            let preview = original.min(MAX_ARRAY_ITEMS);
            for (index, item) in items.iter_mut().take(preview).enumerate() {
                guard_value(item, &format!("{path}/{index}"), fields);
            }
            if original > MAX_ARRAY_ITEMS {
                items.truncate(preview);
                fields.push(json!({
                    "path": path,
                    "type": "array",
                    "original_items": original,
                    "preview_items": preview,
                }));
            }
        }
        Value::Object(object) => {
            for (key, item) in object.iter_mut() {
                guard_value(item, &format!("{path}/{key}"), fields);
            }
        }
        _ => {}
    }
}

/// Bound strings and arrays in the output and record what was cut.
fn apply_output_guards(output: &mut Value) {
    let mut fields = Vec::new();
    guard_value(output, "", &mut fields);
    if let Value::Object(object) = output {
        object.insert(
            "output_guard".into(),
            json!({
                "truncated": !fields.is_empty(),
                "max_string_bytes": MAX_STRING_BYTES,
                "max_array_items": MAX_ARRAY_ITEMS,
                "fields": fields,
            }),
        );
    }
}

fn workspace_root(ctx: &ToolContext) -> Result<PathBuf, IndexResult> {
    std::fs::canonicalize(&ctx.cwd)
        .map_err(|e| IndexResult::execution(format!("{}: {e}", ctx.cwd.display())))
}

/// Run blocking index work off the async runtime, honouring cancellation.
async fn run_blocking<F>(ctx: &ToolContext, work: F) -> IndexResult
where
    F: FnOnce() -> IndexResult + Send + 'static,
{
    let task = tokio::task::spawn_blocking(work);
    tokio::select! {
        joined = task => joined.unwrap_or_else(|e| IndexResult::execution(format!("index task failed: {e}"))),
        _ = ctx.cancel.cancelled() => IndexResult::execution("Operation aborted"),
    }
}

type DirectPayload = fn(&BackgroundService, &Query) -> Result<Value, OpError>;

/// Shared flow of the direct tools: make sure a generation exists, run the
/// payload, attach index freshness and apply output guards.
fn invoke_direct(
    service: &BackgroundService,
    tool: &str,
    query: &Query,
    payload: DirectPayload,
) -> IndexResult {
    let mut status = service.status();
    if status.generation == 0 && status.running {
        service.request_scan();
        let deadline = Instant::now() + READY_WAIT;
        while status.generation == 0 && Instant::now() < deadline {
            std::thread::sleep(READY_POLL);
            status = service.status();
        }
    } else if status.generation == 0 {
        service.run_once();
        status = service.status();
    }
    if status.generation == 0 {
        return IndexResult::execution(format!(
            "project code index is still building; retry {tool}"
        ))
        .with_output(json!({
            "code": "index_not_ready",
            "retryable": true,
            "scan_active": status.scan_active,
            "scan_phase": status.scan_phase,
            "scan_progress_percent": status.scan_progress_percent,
            "last_error": status.last_error,
        }));
    }
    match payload(service, query) {
        Ok(mut output) => {
            let status = service.status();
            if let Value::Object(object) = &mut output {
                object.insert(
                    "index".into(),
                    json!({
                        "generation": status.generation,
                        "stale": status.index_stale,
                        "snapshot_loaded": status.snapshot_loaded,
                    }),
                );
            }
            apply_output_guards(&mut output);
            IndexResult::success(output)
        }
        Err(error) => IndexResult::from_op_error(error),
    }
}

fn require_project_file(
    service: &BackgroundService,
    file: &str,
    tool: &str,
) -> Result<(), OpError> {
    let relative = std::path::Path::new(file);
    if file.is_empty() || relative.is_absolute() {
        return Err(OpError::Invalid(format!(
            "{tool} requires a project-relative file"
        )));
    }
    let normalized = crate::util::lexically_normal(relative);
    let first = normalized.components().next();
    let escapes = match first {
        None => true,
        Some(std::path::Component::ParentDir) => true,
        Some(std::path::Component::Normal(name)) => name == ".git",
        _ => false,
    };
    if escapes {
        return Err(OpError::Invalid(format!(
            "{tool} file must stay inside the project"
        )));
    }
    if !crate::util::is_regular_file(&service.project_root().join(&normalized)) {
        return Err(OpError::Invalid(format!(
            "{tool} file does not exist: {file}"
        )));
    }
    Ok(())
}

fn analyze_file_payload(service: &BackgroundService, query: &Query) -> Result<Value, OpError> {
    require_project_file(
        service,
        query.get("file").map(String::as_str).unwrap_or_default(),
        "analyze_file",
    )?;
    let mut bounded = query.clone();
    bounded
        .entry("limit".into())
        .or_insert_with(|| "100".into());
    let mut output = call_operation("index_file_analysis", service, &bounded)?;
    output["call_graph_approximate"] = json!(true);
    Ok(output)
}

fn references_payload(service: &BackgroundService, query: &Query) -> Result<Value, OpError> {
    let mut translated = query.clone();
    let symbol = translated.remove("symbol").unwrap_or_default();
    translated.insert("function_name".into(), symbol);
    translated
        .entry("direction".into())
        .or_insert_with(|| "both".into());
    translated
        .entry("depth".into())
        .or_insert_with(|| "3".into());
    translated
        .entry("limit".into())
        .or_insert_with(|| "100".into());
    let mut output = call_operation("index_trace_call_path", service, &translated)?;
    output["approximate"] = json!(true);
    output["verification"] = json!("Verify material call edges against current source.");
    Ok(output)
}

fn build_context_payload(service: &BackgroundService, query: &Query) -> Result<Value, OpError> {
    let file = query.get("file").cloned().unwrap_or_default();
    require_project_file(service, &file, "build_context")?;
    let limit = query
        .get("limit")
        .map(|l| crate::ops::parse_size(l))
        .unwrap_or(100);
    let mut unbounded = query.clone();
    unbounded.insert("limit".into(), "0".into());
    let mut compile_flags = call_operation("index_build_compile_flags", service, &unbounded)?;
    let mut include_graph = call_operation("index_build_include_graph", service, &unbounded)?;
    let filter_and_limit = |values: &mut Value, field: &str| -> bool {
        let Value::Array(items) = values else {
            return false;
        };
        items.retain(|v| v.get(field).and_then(Value::as_str) == Some(file.as_str()));
        let truncated = items.len() > limit;
        items.truncate(limit);
        truncated
    };
    let flags_truncated = compile_flags
        .get_mut("entries")
        .map(|v| filter_and_limit(v, "file"))
        .unwrap_or(false);
    let graph_truncated = include_graph
        .get_mut("edges")
        .map(|v| filter_and_limit(v, "from"))
        .unwrap_or(false);
    let query_json = crate::ops::query_json(query);
    compile_flags["truncated"] = json!(flags_truncated);
    include_graph["truncated"] = json!(graph_truncated);
    compile_flags["query"] = query_json.clone();
    include_graph["query"] = query_json;
    Ok(json!({ "file": file, "compile_flags": compile_flags, "include_graph": include_graph }))
}

/// Quote every whitespace-separated term for a literal FTS5 match.
pub fn plain_fts_query(query: &str) -> String {
    query
        .split_whitespace()
        .map(|token| format!("\"{}\"", token.replace('"', "\"\"")))
        .collect::<Vec<_>>()
        .join(" ")
}

fn search_index_payload(service: &BackgroundService, query: &Query) -> Result<Value, OpError> {
    let mut translated = query.clone();
    let syntax = translated
        .remove("syntax")
        .unwrap_or_else(|| "plain".into());
    let original = query.get("query").cloned().unwrap_or_default();
    if syntax == "plain" {
        translated.insert("query".into(), plain_fts_query(&original));
    }
    translated
        .entry("limit".into())
        .or_insert_with(|| "50".into());
    if !service.status().sqlite_fts_available {
        return Err(OpError::Failed(
            "persisted full-text index is unavailable; request an index rescan before retrying search_index".into(),
        ));
    }
    let mut output = match call_operation("index_fts_search", service, &translated) {
        Ok(output) => output,
        Err(OpError::Failed(message)) if syntax == "fts5" => {
            return Err(OpError::Invalid(format!("invalid FTS5 query: {message}")));
        }
        Err(error) => return Err(error),
    };
    output["query"] = json!(original);
    output["syntax"] = json!(syntax);
    if service.status().index_stale {
        output["snapshot_warning"] =
            json!("Results may lag behind files changed since the last scan.");
    }
    Ok(output)
}

fn symbol_tool_payload(service: &BackgroundService, query: &Query) -> Result<Value, OpError> {
    call_operation("index_symbol", service, query)
}

/// A direct index tool with its archived schema and payload.
pub struct DirectIndexTool {
    manager: Arc<IndexManager>,
    name: &'static str,
    description: &'static str,
    snippet: &'static str,
    guidelines: &'static [&'static str],
    schema: fn() -> Value,
    payload: DirectPayload,
}

const SYMBOL_GUIDELINES: &[&str] = &[
    "For C/C++ class, function, method, definition, or identifier lookup, call symbol first. Use grep for literal text, regex, or languages not covered by the C/C++ index.",
];
const ANALYZE_GUIDELINES: &[&str] = &[
    "Use analyze_file for a known file's indexed C/C++ structure and call edges; verify important results against current source.",
];
const REFERENCES_GUIDELINES: &[&str] = &[
    "Use references for approximate indexed C/C++ callers and callees; verify material edges against current source.",
];
const BUILD_CONTEXT_GUIDELINES: &[&str] = &[
    "Use build_context for a C/C++ file's compiler flags, definitions, include paths, and resolved include edges.",
];
const SEARCH_INDEX_GUIDELINES: &[&str] = &[
    "Use search_index for ranked persisted repository text, and grep for regexes, unsupported languages, or freshly changed text.",
];
const SCHEMA_RETRY_GUIDELINE: &str = "Match each tool's advertised JSON schema exactly. If a tool returns invalid_arguments with retryable=true, correct the listed fields and retry the call.";
const ROUTER_GUIDELINES_WITH_SYMBOL: &[&str] = &[SCHEMA_RETRY_GUIDELINE];
const ROUTER_GUIDELINES: &[&str] = &[
    "For C/C++ symbol navigation, prefer index with action=call and operation=code.symbol over grep; put operation at the top level and the symbol name inside arguments.",
    SCHEMA_RETRY_GUIDELINE,
];

fn symbol_schema() -> Value {
    object_schema(
        json!({
            "name": { "type": "string", "minLength": 1, "description": "C/C++ identifier or qualified symbol name." },
            "file": { "type": "string", "description": "Optional project-relative file filter." },
            "kind": { "type": "string", "description": "Optional symbol kind filter such as function or method." },
            "context": { "type": "boolean", "description": "Include source lines around each match." },
            "before": { "type": "integer", "minimum": 0, "description": "Context lines before each match." },
            "after": { "type": "integer", "minimum": 0, "description": "Context lines after each match." },
            "limit": { "type": "integer", "minimum": 1, "maximum": 100, "description": "Maximum matching symbols." }
        }),
        &["name"],
    )
}

fn analyze_file_schema() -> Value {
    object_schema(
        json!({
            "file": { "type": "string", "minLength": 1, "description": "Project-relative source file." },
            "limit": { "type": "integer", "minimum": 1, "maximum": 100, "description": "Maximum incoming and outgoing call edges." }
        }),
        &["file"],
    )
}

fn references_schema() -> Value {
    object_schema(
        json!({
            "symbol": { "type": "string", "minLength": 1, "description": "Function, method, or qualified symbol name." },
            "direction": { "type": "string", "enum": ["inbound", "outbound", "both"], "description": "Trace callers, callees, or both. Defaults to both." },
            "depth": { "type": "integer", "minimum": 1, "maximum": 5, "description": "Maximum call traversal depth." },
            "limit": { "type": "integer", "minimum": 1, "maximum": 100, "description": "Maximum call-path edges." }
        }),
        &["symbol"],
    )
}

fn build_context_schema() -> Value {
    object_schema(
        json!({
            "file": { "type": "string", "minLength": 1, "description": "Project-relative C/C++ source or header file." },
            "limit": { "type": "integer", "minimum": 1, "maximum": 100, "description": "Maximum entries or include edges." }
        }),
        &["file"],
    )
}

fn search_index_schema() -> Value {
    object_schema(
        json!({
            "query": { "type": "string", "minLength": 1, "description": "Search terms, or an FTS5 expression when syntax is fts5." },
            "syntax": { "type": "string", "enum": ["plain", "fts5"], "description": "plain safely searches all whitespace-separated terms and is the default; fts5 accepts raw MATCH syntax." },
            "limit": { "type": "integer", "minimum": 1, "maximum": 100, "description": "Maximum indexed matches." }
        }),
        &["query"],
    )
}

impl DirectIndexTool {
    pub fn symbol(manager: Arc<IndexManager>) -> Self {
        Self {
            manager,
            name: "symbol",
            description: "Find C/C++ class, function, or method definitions by identifier using the project code index. Prefer this over grep for symbol lookup. Example: {\"name\":\"ToolRegistry::prepare_request\",\"context\":true}.",
            snippet: "Find C/C++ definitions with the project code index",
            guidelines: SYMBOL_GUIDELINES,
            schema: symbol_schema,
            payload: symbol_tool_payload,
        }
    }

    pub fn analyze_file(manager: Arc<IndexManager>) -> Self {
        Self {
            manager,
            name: "analyze_file",
            description: "Analyze one project-relative source file with the code index. Returns its C/C++ classes, functions, line statistics, and approximate incoming and outgoing call edges.",
            snippet: "Analyze indexed symbols and calls in one source file",
            guidelines: ANALYZE_GUIDELINES,
            schema: analyze_file_schema,
            payload: analyze_file_payload,
        }
    }

    pub fn references(manager: Arc<IndexManager>) -> Self {
        Self {
            manager,
            name: "references",
            description: "Trace approximate indexed C/C++ callers and callees for a function or method. Prefer this over text search for call impact analysis, then verify important edges in source.",
            snippet: "Trace indexed C/C++ callers and callees",
            guidelines: REFERENCES_GUIDELINES,
            schema: references_schema,
            payload: references_payload,
        }
    }

    pub fn build_context(manager: Arc<IndexManager>) -> Self {
        Self {
            manager,
            name: "build_context",
            description: "Return indexed compiler and include context for one project-relative C/C++ file, including compile flags, defines, include paths, standards, and resolved include edges.",
            snippet: "Show per-file compiler flags and include edges",
            guidelines: BUILD_CONTEXT_GUIDELINES,
            schema: build_context_schema,
            payload: build_context_payload,
        }
    }

    pub fn search_index(manager: Arc<IndexManager>) -> Self {
        Self {
            manager,
            name: "search_index",
            description: "Search the persisted project full-text index. Use for broad repository concepts and known text; use symbol for C/C++ identifiers and grep for regexes or freshly changed text.",
            snippet: "Search the persisted project full-text index",
            guidelines: SEARCH_INDEX_GUIDELINES,
            schema: search_index_schema,
            payload: search_index_payload,
        }
    }

    fn prepare(
        &self,
        args: Value,
        ctx: &ToolContext,
    ) -> Result<(Arc<BackgroundService>, Query), IndexResult> {
        let args = normalize_arguments(args);
        let validated = validate_direct(self.name, &(self.schema)(), &args)?;
        let root = workspace_root(ctx)?;
        Ok((
            self.manager.service_for(&root),
            query_from_arguments(&validated),
        ))
    }
}

impl Tool for DirectIndexTool {
    fn name(&self) -> &'static str {
        self.name
    }
    fn description(&self) -> String {
        self.description.to_string()
    }
    fn snippet(&self) -> &'static str {
        self.snippet
    }
    fn guidelines(&self) -> &'static [&'static str] {
        self.guidelines
    }
    fn parameters(&self) -> Value {
        (self.schema)()
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let prepared = self.prepare(args, &ctx);
        let name = self.name;
        let payload = self.payload;
        Box::pin(async move {
            let result = match prepared {
                Err(result) => result,
                Ok((service, query)) => {
                    run_blocking(&ctx, move || invoke_direct(&service, name, &query, payload)).await
                }
            };
            result.into_tool_output()
        })
    }
}

/// One router operation (archived `IndexToolSpec`).
struct OperationSpec {
    category: &'static str,
    operation: &'static str,
    handler: &'static str,
    description: &'static str,
    parameters: &'static [&'static str],
    required: &'static [&'static str],
}

const fn op(
    category: &'static str,
    operation: &'static str,
    handler: &'static str,
    description: &'static str,
    parameters: &'static [&'static str],
    required: &'static [&'static str],
) -> OperationSpec {
    OperationSpec {
        category,
        operation,
        handler,
        description,
        parameters,
        required,
    }
}

/// Router operations for the ported groups (core, fs, git, build, code).
const OPERATIONS: &[OperationSpec] = &[
    op(
        "core",
        "status",
        "index_status",
        "Return index scan, build metadata, and Tree-sitter status.",
        &["include_build_metadata", "include_tree_sitter"],
        &[],
    ),
    op(
        "core",
        "rescan",
        "index_rescan",
        "Run an immediate index rescan.",
        &["reason"],
        &[],
    ),
    op(
        "core",
        "metadata",
        "index_metadata",
        "Return generated index metadata locations.",
        &[
            "include_compile_commands",
            "include_tree_sitter",
            "include_sqlite",
        ],
        &[],
    ),
    op(
        "core",
        "tree_sitter_failures",
        "index_tree_sitter_failures",
        "List Tree-sitter parse failures.",
        &["path", "path_prefix", "path_contains", "limit"],
        &[],
    ),
    op(
        "core",
        "project_summary",
        "index_project_summary",
        "Return a ranked index project summary.",
        &["limit"],
        &[],
    ),
    op(
        "core",
        "memory_graph",
        "index_memory_graph",
        "Return and optionally persist the index memory graph.",
        &["persist", "include_nodes", "include_edges"],
        &[],
    ),
    op(
        "core",
        "graph_schema",
        "index_graph_schema",
        "Return index memory graph schema counts.",
        &["include_counts"],
        &[],
    ),
    op(
        "core",
        "search_graph",
        "index_search_graph",
        "Search index memory graph nodes.",
        &["label", "name_pattern", "file", "limit", "offset"],
        &[],
    ),
    op(
        "core",
        "trace_call_path",
        "index_trace_call_path",
        "Trace inbound, outbound, or bidirectional call paths.",
        &["function_name", "direction", "depth", "limit"],
        &["function_name"],
    ),
    op(
        "core",
        "dead_code",
        "index_dead_code",
        "List indexed functions with no observed callers.",
        &["limit"],
        &[],
    ),
    op(
        "core",
        "code_snippet",
        "index_code_snippet",
        "Read source context for a function or method.",
        &["qualified_name", "before", "after"],
        &["qualified_name"],
    ),
    op(
        "fs",
        "stat",
        "index_fs_stat",
        "Stat a project-relative filesystem path.",
        &["path"],
        &[],
    ),
    op(
        "fs",
        "list",
        "index_fs_list",
        "List a project-relative directory.",
        &["path", "recursive", "depth", "limit"],
        &[],
    ),
    op(
        "fs",
        "tree",
        "index_fs_tree",
        "Return a bounded recursive project filesystem tree.",
        &["path", "depth", "limit"],
        &[],
    ),
    op(
        "fs",
        "read",
        "index_fs_read",
        "Read project-relative file lines.",
        &["file", "start_line", "end_line", "max_lines"],
        &["file"],
    ),
    op(
        "fs",
        "search",
        "index_fs_search",
        "Search project-relative file paths by substring.",
        &["pattern", "path", "limit"],
        &["pattern"],
    ),
    op(
        "git",
        "status",
        "index_git_status",
        "Return read-only git status.",
        &["short", "limit"],
        &[],
    ),
    op(
        "git",
        "log",
        "index_git_log",
        "Return recent git commits.",
        &["limit", "file"],
        &[],
    ),
    op(
        "git",
        "diff",
        "index_git_diff",
        "Return git diff output.",
        &["file", "staged", "stat", "limit"],
        &[],
    ),
    op(
        "git",
        "show",
        "index_git_show",
        "Show a read-only git revision summary.",
        &["rev", "limit"],
        &[],
    ),
    op(
        "git",
        "blame",
        "index_git_blame",
        "Return git blame lines for a file range.",
        &["file", "start_line", "end_line", "limit"],
        &["file"],
    ),
    op(
        "git",
        "branches",
        "index_git_branches",
        "List git branches.",
        &["all"],
        &[],
    ),
    op(
        "build",
        "target_graph",
        "index_build_target_graph",
        "Return a best-effort CMake/Ninja target graph.",
        &["limit"],
        &[],
    ),
    op(
        "build",
        "compile_flags",
        "index_build_compile_flags",
        "Return compile flags from compile_commands.json.",
        &["file", "limit"],
        &[],
    ),
    op(
        "build",
        "include_graph",
        "index_build_include_graph",
        "Return source-to-header include edges.",
        &["file", "limit"],
        &[],
    ),
    op(
        "build",
        "compile_commands",
        "index_build_compile_commands",
        "Return generated compile_commands entries.",
        &["file", "limit"],
        &[],
    ),
    op(
        "code",
        "files",
        "index_files",
        "List scanned project files.",
        &[
            "kind",
            "path",
            "path_prefix",
            "path_contains",
            "extension",
            "limit",
        ],
        &[],
    ),
    op(
        "code",
        "file_info",
        "index_file_info",
        "Return metadata for one project-relative file.",
        &["file"],
        &["file"],
    ),
    op(
        "code",
        "file_analysis",
        "index_file_analysis",
        "Analyze one file with symbols and call edges.",
        &["file", "limit"],
        &["file"],
    ),
    op(
        "code",
        "symbol",
        "index_symbol",
        "Find classes, methods, or functions by name.",
        &[
            "name",
            "file",
            "path",
            "path_prefix",
            "path_contains",
            "kind",
            "context",
            "before",
            "after",
            "limit",
        ],
        &["name"],
    ),
    op(
        "code",
        "search",
        "index_search",
        "Search project text files.",
        &[
            "pattern",
            "file",
            "path",
            "path_prefix",
            "path_contains",
            "kind",
            "before",
            "after",
            "limit",
        ],
        &["pattern"],
    ),
    op(
        "code",
        "fts_search",
        "index_fts_search",
        "Search the persisted SQLite FTS index.",
        &["query", "limit"],
        &["query"],
    ),
    op(
        "code",
        "ast",
        "index_ast",
        "Return generated Tree-sitter AST text for a file.",
        &["file", "max_lines"],
        &["file"],
    ),
    op(
        "code",
        "classes",
        "index_classes",
        "List indexed C++ classes.",
        &[
            "name",
            "qualified_name",
            "file",
            "path",
            "path_prefix",
            "path_contains",
            "limit",
        ],
        &[],
    ),
    op(
        "code",
        "functions",
        "index_functions",
        "List indexed C/C++ functions and methods.",
        &[
            "name",
            "qualified_name",
            "kind",
            "file",
            "path",
            "path_prefix",
            "path_contains",
            "limit",
        ],
        &[],
    ),
    op(
        "code",
        "callgraph",
        "index_callgraph",
        "List approximate C/C++ call graph edges.",
        &[
            "caller",
            "callee",
            "file",
            "path",
            "path_prefix",
            "path_contains",
            "limit",
        ],
        &[],
    ),
    op(
        "code",
        "read_context",
        "index_read_context",
        "Read source lines around a file and line number.",
        &["file", "line", "before", "after"],
        &["file", "line"],
    ),
];

/// Handlers that need a completed scan before they run.
const SCAN_HANDLERS: &[&str] = &[
    "index_status",
    "index_metadata",
    "index_tree_sitter_failures",
    "index_project_summary",
    "index_memory_graph",
    "index_graph_schema",
    "index_search_graph",
    "index_trace_call_path",
    "index_dead_code",
    "index_code_snippet",
    "index_build_target_graph",
    "index_build_compile_flags",
    "index_build_include_graph",
    "index_build_compile_commands",
    "index_files",
    "index_file_info",
    "index_file_analysis",
    "index_symbol",
    "index_search",
    "index_fts_search",
    "index_ast",
    "index_classes",
    "index_functions",
    "index_callgraph",
    "index_read_context",
];

fn public_operation_name(spec: &OperationSpec) -> String {
    if spec.category == "core" {
        spec.operation.to_string()
    } else {
        format!("{}.{}", spec.category, spec.operation)
    }
}

/// JSON schema for an operation's arguments.
fn parameter_schema(parameters: &[&str], required: &[&str]) -> Value {
    let mut properties = Map::new();
    for name in parameters {
        let boolean = matches!(
            *name,
            "context"
                | "raw"
                | "dry_run"
                | "recursive"
                | "include_hidden"
                | "include_nodes"
                | "include_edges"
                | "persist"
                | "include_tree_sitter"
                | "include_build_metadata"
                | "include_compile_commands"
                | "include_sqlite"
                | "header"
        );
        let number = matches!(
            *name,
            "limit"
                | "before"
                | "after"
                | "line"
                | "depth"
                | "max_depth"
                | "max_lines"
                | "max_length"
                | "start_index"
                | "count"
                | "wait_ms"
                | "frames"
                | "indent"
                | "timestamp"
                | "from_timestamp"
        );
        let description = match *name {
            "name" => "C/C++ class, function, method, or qualified symbol name.".to_string(),
            "pattern" => "Text or regular-expression search pattern.".to_string(),
            "file" => "Project-relative file path.".to_string(),
            "context" => "Include source context around each symbol match.".to_string(),
            "limit" => "Maximum number of results to return.".to_string(),
            other => format!("Value for {other}."),
        };
        let kind = if boolean {
            "boolean"
        } else if number {
            "integer"
        } else {
            "string"
        };
        properties.insert(
            name.to_string(),
            json!({ "type": kind, "description": description }),
        );
    }
    object_schema(Value::Object(properties), required)
}

fn spec_summary(spec: &OperationSpec, include_schema: bool) -> Value {
    let mut summary = json!({
        "name": spec.operation,
        "operation": public_operation_name(spec),
        "description": spec.description,
        "parameters": spec.parameters,
        "required": spec.required,
    });
    if include_schema {
        summary["input_schema"] = parameter_schema(spec.parameters, spec.required);
    }
    summary
}

fn find_spec(category: &str, operation: &str) -> Option<&'static OperationSpec> {
    OPERATIONS.iter().find(|spec| {
        (operation == public_operation_name(spec)
            || (!category.is_empty() && operation == spec.operation))
            && (category.is_empty() || category == spec.category)
    })
}

fn router_schema() -> Value {
    object_schema(
        json!({
            "action": {
                "type": "string",
                "enum": ["tree", "call"],
                "description": "Use tree to discover grouped index operations; use call to run one."
            },
            "category": {
                "type": "string",
                "description": "Optional operation group such as code, fs, git, build."
            },
            "operation": {
                "type": "string",
                "description": "Operation name, or category.operation when category is omitted. For code navigation use code.symbol, code.functions, code.classes, code.callgraph, or code.read_context."
            },
            "arguments": {
                "type": "object",
                "description": "Arguments forwarded to the selected operation."
            },
            "include_schemas": {
                "type": "boolean",
                "description": "Include per-operation JSON schemas in tree output."
            }
        }),
        &["action"],
    )
}

/// The archived registry rewrites for `index` calls: hoist `category`,
/// `operation` and `include_schemas` out of `arguments`, map symbol-lookup
/// aliases to `code.symbol` and rename `pattern` to `name` for it.
pub fn rewrite_router_arguments(mut args: Value) -> Value {
    let Value::Object(object) = &mut args else {
        return args;
    };
    if !object.get("arguments").is_some_and(Value::is_object) {
        return args;
    }
    let Some(Value::Object(mut forwarded)) = object.remove("arguments") else {
        return args;
    };
    for field in ["category", "operation", "include_schemas"] {
        if !object.contains_key(field)
            && let Some(value) = forwarded.remove(field)
        {
            object.insert(field.to_string(), value);
        }
    }
    if let Some(Value::String(operation)) = object.get("operation").cloned() {
        if matches!(
            operation.as_str(),
            "symbol_lookup" | "find_symbol" | "lookup_symbol"
        ) {
            object.insert("operation".into(), json!("code.symbol"));
            object.remove("category");
        }
        if object.get("operation") == Some(&json!("code.symbol"))
            && !forwarded.contains_key("name")
            && let Some(pattern) = forwarded.remove("pattern")
        {
            forwarded.insert("name".into(), pattern);
        }
    }
    object.insert("arguments".into(), Value::Object(forwarded));
    args
}

fn tree_payload(category: &str, include_schemas: bool) -> Value {
    let mut by_category: std::collections::BTreeMap<&str, Vec<Value>> =
        std::collections::BTreeMap::new();
    for spec in OPERATIONS {
        if !category.is_empty() && category != spec.category {
            continue;
        }
        by_category
            .entry(spec.category)
            .or_default()
            .push(spec_summary(spec, include_schemas));
    }
    let categories: Vec<Value> = by_category
        .into_iter()
        .map(|(name, operations)| json!({ "name": name, "operations": operations }))
        .collect();
    json!({
        "usage": {
            "discover_all": { "action": "tree", "include_schemas": false },
            "discover_category": { "action": "tree", "category": "code" },
            "call": { "action": "call", "operation": "fs.read", "arguments": { "file": "PLAN.md" } }
        },
        "categories": categories,
    })
}

fn optional_string(args: &Value, name: &str) -> Result<String, IndexResult> {
    match args.get(name) {
        None => Ok(String::new()),
        Some(Value::String(s)) => Ok(s.clone()),
        Some(_) => Err(IndexResult::invalid(format!(
            "index requires a string {name} argument"
        ))),
    }
}

fn required_string(args: &Value, name: &str) -> Result<String, IndexResult> {
    match args.get(name) {
        Some(Value::String(s)) => Ok(s.clone()),
        _ => Err(IndexResult::invalid(format!(
            "index requires a string {name} argument"
        ))),
    }
}

/// The grouped `index` router tool.
pub struct IndexRouterTool {
    manager: Arc<IndexManager>,
    symbol_tool_available: bool,
}

impl IndexRouterTool {
    /// `symbol_tool_available` selects the prompt guidance: without the
    /// direct `symbol` tool the router recommends `code.symbol`.
    pub fn new(manager: Arc<IndexManager>, symbol_tool_available: bool) -> Self {
        Self {
            manager,
            symbol_tool_available,
        }
    }

    /// Validate the call and prepare the operation; `Ok(None)` for `tree`.
    fn prepare(&self, args: Value, ctx: &ToolContext) -> Result<RouterCall, IndexResult> {
        let args = rewrite_router_arguments(normalize_arguments(args));
        let args = validate_direct("index", &router_schema(), &args)?;
        let action = required_string(&args, "action")?;
        if action == "tree" {
            let category = optional_string(&args, "category")?;
            let include = args
                .get("include_schemas")
                .and_then(Value::as_bool)
                .unwrap_or(false);
            return Ok(RouterCall::Tree(tree_payload(&category, include)));
        }
        if action != "call" {
            return Err(IndexResult::invalid("index action must be tree or call"));
        }
        let operation = required_string(&args, "operation")?;
        let category = optional_string(&args, "category")?;
        let Some(spec) = find_spec(&category, &operation) else {
            return Err(IndexResult::invalid(format!(
                "unknown index operation: {operation}"
            )));
        };
        let forwarded = match args.get("arguments") {
            None => Value::Object(Map::new()),
            Some(Value::Object(object)) => Value::Object(object.clone()),
            Some(_) => return Err(IndexResult::invalid("index arguments must be an object")),
        };
        let public_name = public_operation_name(spec);
        let schema = parameter_schema(spec.parameters, spec.required);
        let validated =
            validate_tool_arguments(&public_name, &schema, &forwarded).map_err(|message| {
                IndexResult::invalid(message).with_output(json!({
                    "code": "invalid_index_arguments",
                    "operation": public_name,
                    "retryable": true,
                    "expected_schema": schema,
                    "received_arguments": forwarded,
                }))
            })?;
        let root = workspace_root(ctx)?;
        Ok(RouterCall::Operation {
            service: self.manager.service_for(&root),
            spec,
            query: query_from_arguments(&validated),
        })
    }
}

enum RouterCall {
    Tree(Value),
    Operation {
        service: Arc<BackgroundService>,
        spec: &'static OperationSpec,
        query: Query,
    },
}

fn invoke_operation(
    service: &BackgroundService,
    spec: &OperationSpec,
    query: &Query,
) -> IndexResult {
    if SCAN_HANDLERS.contains(&spec.handler) {
        let status = service.status();
        if status.running {
            if status.generation == 0 && !status.scan_active {
                service.request_scan();
            }
        } else {
            service.run_once();
        }
    }
    match call_operation(spec.handler, service, query) {
        Ok(output) => IndexResult::success(json!({
            "category": spec.category,
            "operation": public_operation_name(spec),
            "output": output,
        })),
        Err(error) => IndexResult::from_op_error(error),
    }
}

impl Tool for IndexRouterTool {
    fn name(&self) -> &'static str {
        "index"
    }
    fn description(&self) -> String {
        "Discover and invoke grouped project index, code, filesystem, git, and build operations. Prefer the direct symbol tool for C/C++ definitions. For other calls use, for example, action=call, operation=code.callgraph, arguments={...}; use action=tree only when the operation is unknown.".into()
    }
    fn snippet(&self) -> &'static str {
        "Discover and invoke grouped in-process project index operations"
    }
    fn guidelines(&self) -> &'static [&'static str] {
        if self.symbol_tool_available {
            ROUTER_GUIDELINES_WITH_SYMBOL
        } else {
            ROUTER_GUIDELINES
        }
    }
    fn parameters(&self) -> Value {
        router_schema()
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        let prepared = self.prepare(args, &ctx);
        Box::pin(async move {
            let result = match prepared {
                Err(result) => result,
                Ok(RouterCall::Tree(payload)) => IndexResult::success(payload),
                Ok(RouterCall::Operation {
                    service,
                    spec,
                    query,
                }) => run_blocking(&ctx, move || invoke_operation(&service, spec, &query)).await,
            };
            result.into_tool_output()
        })
    }
}

/// Names of the index tools, in registration order.
pub const INDEX_TOOL_NAMES: &[&str] = &[
    "index",
    "symbol",
    "analyze_file",
    "references",
    "build_context",
    "search_index",
];

/// Build the enabled index tools sharing `manager`.
pub fn index_tools(
    manager: Arc<IndexManager>,
    enabled: impl Fn(&str) -> bool,
) -> Vec<Arc<dyn Tool>> {
    let mut tools: Vec<Arc<dyn Tool>> = Vec::new();
    let symbol = enabled("symbol");
    if enabled("index") {
        tools.push(Arc::new(IndexRouterTool::new(manager.clone(), symbol)));
    }
    if symbol {
        tools.push(Arc::new(DirectIndexTool::symbol(manager.clone())));
    }
    if enabled("analyze_file") {
        tools.push(Arc::new(DirectIndexTool::analyze_file(manager.clone())));
    }
    if enabled("references") {
        tools.push(Arc::new(DirectIndexTool::references(manager.clone())));
    }
    if enabled("build_context") {
        tools.push(Arc::new(DirectIndexTool::build_context(manager.clone())));
    }
    if enabled("search_index") {
        tools.push(Arc::new(DirectIndexTool::search_index(manager)));
    }
    tools
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn router_rewrites() {
        let rewritten = rewrite_router_arguments(json!({
            "action": "call",
            "arguments": { "operation": "find_symbol", "category": "code", "pattern": "Foo" }
        }));
        assert_eq!(
            rewritten,
            json!({ "action": "call", "operation": "code.symbol", "arguments": { "name": "Foo" } })
        );
        let kept = rewrite_router_arguments(json!({
            "action": "call", "operation": "code.symbol", "arguments": { "name": "A", "pattern": "B" }
        }));
        assert_eq!(kept["arguments"], json!({ "name": "A", "pattern": "B" }));
    }

    #[test]
    fn spec_lookup() {
        assert_eq!(find_spec("", "status").unwrap().handler, "index_status");
        assert_eq!(
            find_spec("git", "status").unwrap().handler,
            "index_git_status"
        );
        assert_eq!(
            find_spec("", "git.status").unwrap().handler,
            "index_git_status"
        );
        assert_eq!(
            find_spec("code", "code.symbol").unwrap().handler,
            "index_symbol"
        );
        assert!(find_spec("", "symbol").is_none());
        assert!(find_spec("", "serial.list").is_none());
        assert!(find_spec("fs", "git.status").is_none());
    }

    #[test]
    fn parameter_schema_types() {
        let schema = parameter_schema(&["name", "context", "limit", "rev"], &["name"]);
        assert_eq!(schema["properties"]["context"]["type"], "boolean");
        assert_eq!(schema["properties"]["limit"]["type"], "integer");
        assert_eq!(schema["properties"]["rev"]["description"], "Value for rev.");
        assert_eq!(schema["required"], json!(["name"]));
        assert_eq!(schema["additionalProperties"], false);
    }

    #[test]
    fn query_conversion_and_fts_quoting() {
        let query = query_from_arguments(&json!({"a": true, "b": 3, "c": "x", "d": [1], "e": ""}));
        assert_eq!(query.get("a").unwrap(), "1");
        assert_eq!(query.get("b").unwrap(), "3");
        assert_eq!(query.get("e").unwrap(), "");
        assert!(!query.contains_key("d"));
        assert_eq!(plain_fts_query("foo  \"bar"), "\"foo\" \"\"\"bar\"");
    }

    #[test]
    fn output_guards_bound_strings_and_arrays() {
        let mut output = json!({
            "items": (0..1005).collect::<Vec<_>>(),
            "text": "é".repeat(MAX_STRING_BYTES),
        });
        apply_output_guards(&mut output);
        assert_eq!(output["items"].as_array().unwrap().len(), MAX_ARRAY_ITEMS);
        assert!(output["text"].as_str().unwrap().len() <= MAX_STRING_BYTES);
        let guard = &output["output_guard"];
        assert_eq!(guard["truncated"], true);
        let paths: Vec<_> = guard["fields"]
            .as_array()
            .unwrap()
            .iter()
            .map(|f| f["path"].clone())
            .collect();
        assert!(paths.contains(&json!("/items")));
        assert!(paths.contains(&json!("/text")));
    }

    #[test]
    fn tree_lists_only_ported_groups() {
        let tree = tree_payload("", false);
        let names: Vec<_> = tree["categories"]
            .as_array()
            .unwrap()
            .iter()
            .map(|c| c["name"].as_str().unwrap().to_string())
            .collect();
        assert_eq!(names, vec!["build", "code", "core", "fs", "git"]);
        let code = tree_payload("code", true);
        assert_eq!(code["categories"].as_array().unwrap().len(), 1);
        assert!(code["categories"][0]["operations"][0]["input_schema"].is_object());
    }
}
