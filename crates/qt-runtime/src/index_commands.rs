//! `/index` (archived `interactive_mode.cpp` index handler): text
//! subcommands answered through the `index` router and the direct index
//! tools, shared by the desktop `/index` panel and the slash command.

use serde_json::{Map, Value, json};

use qt_core::tools::ToolOutput;

use crate::runtime::Runtime;

/// The router operation groups ported from the archived index.
pub const INDEX_CATEGORIES: &[(&str, &str)] = &[
    ("core", "Core index, graph, metadata, and scan operations"),
    ("code", "Code search, symbols, files, AST, and call graph"),
    ("fs", "Filesystem stat, list, tree, read, and search"),
    ("git", "Git status, log, diff, show, blame, and branches"),
    ("build", "Build metadata, compile flags, and include graph"),
];

/// Entries of the bare `/index` menu (archived order, ported groups only).
pub fn index_menu() -> Vec<(&'static str, &'static str)> {
    let mut items = vec![
        ("tree", "List grouped index operation categories"),
        ("schemas", "List operation categories with JSON schemas"),
        ("status", "Show index metadata and scan status"),
        ("summary", "Show project summary from the index"),
        ("rescan", "Request an immediate background rescan"),
    ];
    items.extend(INDEX_CATEGORIES.iter().copied());
    items.push(("call", "Run any operation: /index call fs.read {json}"));
    items
}

/// One tool invocation an `/index` command maps to.
#[derive(Debug, Clone, PartialEq)]
pub struct IndexInvocation {
    /// `index`, `symbol`, `analyze_file`, `references`, `build_context` or
    /// `search_index`.
    pub tool: String,
    pub arguments: Value,
    /// Transcript label (`index status`, `index call: fs.read`, ...).
    pub label: String,
}

/// What an `/index` line asks for.
#[derive(Debug, Clone, PartialEq)]
pub enum IndexCommand {
    /// Bare `/index`: the menu.
    Menu,
    /// Lines to show (usage and argument errors).
    Lines(Vec<String>),
    Invoke(IndexInvocation),
}

fn split_first(text: &str) -> (&str, &str) {
    let text = text.trim();
    match text.find(char::is_whitespace) {
        Some(i) => (&text[..i], text[i..].trim()),
        None => (text, ""),
    }
}

/// `{}` for empty text, else a JSON object (archived
/// `parse_index_json_arguments`).
pub fn parse_json_object(raw: &str) -> Option<Value> {
    let text = raw.trim();
    if text.is_empty() {
        return Some(Value::Object(Map::new()));
    }
    serde_json::from_str::<Value>(text)
        .ok()
        .filter(Value::is_object)
}

/// The router `tree` request.
pub fn tree_request(category: Option<&str>, include_schemas: bool) -> Value {
    let mut args = json!({"action": "tree", "include_schemas": include_schemas});
    if let Some(c) = category.filter(|c| !c.is_empty()) {
        args["category"] = json!(c);
    }
    args
}

/// The router `call` request.
pub fn call_request(operation: &str, arguments: Value) -> Value {
    json!({"action": "call", "operation": operation, "arguments": arguments})
}

fn invoke(tool: &str, arguments: Value, label: impl Into<String>) -> IndexCommand {
    IndexCommand::Invoke(IndexInvocation {
        tool: tool.into(),
        arguments,
        label: label.into(),
    })
}

fn json_usage() -> IndexCommand {
    IndexCommand::Lines(vec![
        "index arguments must be a JSON object.".into(),
        "usage: /index call <operation> {\"name\":\"value\"}".into(),
        "usage: /index <category.operation> {\"name\":\"value\"}".into(),
    ])
}

/// Parse `/index <arguments>`.
pub fn parse_index_command(arguments: &str) -> IndexCommand {
    let (first, rest) = split_first(arguments);
    if first.is_empty() {
        return IndexCommand::Menu;
    }
    let sub = first.to_ascii_lowercase();
    let category = rest.to_ascii_lowercase();
    match sub.as_str() {
        "tree" | "schemas" => {
            let schemas = sub == "schemas";
            let label = if category.is_empty() {
                format!("index {sub}")
            } else {
                format!("index {sub}: {category}")
            };
            invoke("index", tree_request(Some(&category), schemas), label)
        }
        c if INDEX_CATEGORIES.iter().any(|(name, _)| *name == c) => invoke(
            "index",
            tree_request(Some(c), false),
            format!("index category: {c}"),
        ),
        "status" => invoke(
            "index",
            call_request(
                "status",
                json!({"include_build_metadata": true, "include_tree_sitter": true}),
            ),
            "index status",
        ),
        "metadata" => invoke(
            "index",
            call_request(
                "metadata",
                json!({"include_compile_commands": true, "include_tree_sitter": true}),
            ),
            "index metadata",
        ),
        "summary" => {
            let mut args = Map::new();
            if !rest.is_empty() {
                match rest.parse::<i64>() {
                    Ok(limit) => {
                        args.insert("limit".into(), json!(limit));
                    }
                    Err(_) => {
                        return IndexCommand::Lines(vec!["usage: /index summary [limit]".into()]);
                    }
                }
            }
            invoke(
                "index",
                call_request("project_summary", Value::Object(args)),
                "index summary",
            )
        }
        "rescan" => {
            let mut args = Map::new();
            if !rest.is_empty() {
                args.insert("reason".into(), json!(rest));
            }
            invoke(
                "index",
                call_request("rescan", Value::Object(args)),
                "index rescan",
            )
        }
        "call" => {
            let (operation, payload) = split_first(rest);
            if operation.is_empty() {
                return IndexCommand::Lines(vec![
                    "usage: /index call <operation> [json-object]".into(),
                    "example: /index call fs.read {\"file\":\"PLAN.md\"}".into(),
                ]);
            }
            match parse_json_object(payload) {
                Some(args) => invoke(
                    "index",
                    call_request(operation, args),
                    format!("index call: {operation}"),
                ),
                None => json_usage(),
            }
        }
        op if op.contains('.') => match parse_json_object(rest) {
            Some(args) => invoke("index", call_request(op, args), format!("index call: {op}")),
            None => json_usage(),
        },
        _ => IndexCommand::Lines(vec![
            format!("unknown index command: {first}"),
            "usage: /index".into(),
            "usage: /index tree [category]".into(),
            "usage: /index schemas [category]".into(),
            "usage: /index call <operation> [json-object]".into(),
        ]),
    }
}

/// Lines of the bare `/index` menu for text front ends.
pub fn menu_lines() -> Vec<String> {
    let mut lines = vec!["index:".to_string()];
    lines.extend(
        index_menu()
            .into_iter()
            .map(|(name, description)| format!("  {name} - {description}")),
    );
    lines
}

/// Archived `index_tool_result_lines`: the label and the pretty output,
/// or `<label> failed: <status> - <message>`.
pub fn index_result_lines(label: &str, output: &ToolOutput) -> Vec<String> {
    let text = output.text_content();
    if output.is_error {
        let status = output
            .details
            .as_ref()
            .and_then(|d| d.get("status"))
            .and_then(Value::as_str)
            .unwrap_or("execution_error");
        let message = text.lines().next().unwrap_or_default();
        let mut line = format!("{label} failed: {status}");
        if !message.is_empty() {
            line.push_str(&format!(" - {message}"));
        }
        return vec![line];
    }
    let mut lines = vec![label.to_string()];
    let pretty = serde_json::from_str::<Value>(&text)
        .ok()
        .and_then(|v| serde_json::to_string_pretty(&v).ok())
        .unwrap_or(text);
    lines.extend(pretty.lines().map(str::to_string));
    lines
}

/// Arguments of the direct `symbol` tool from panel fields.
pub fn symbol_arguments(name: &str, file: &str, kind: &str, context: bool) -> Option<Value> {
    let name = name.trim();
    if name.is_empty() {
        return None;
    }
    let mut args = json!({"name": name});
    if !file.trim().is_empty() {
        args["file"] = json!(file.trim());
    }
    if !kind.trim().is_empty() {
        args["kind"] = json!(kind.trim());
    }
    if context {
        args["context"] = json!(true);
    }
    Some(args)
}

/// Arguments of `analyze_file` / `build_context`.
pub fn file_arguments(file: &str) -> Option<Value> {
    let file = file.trim();
    (!file.is_empty()).then(|| json!({"file": file}))
}

/// Arguments of `references` (`direction` is `inbound`, `outbound` or
/// `both`; `depth` 1–5).
pub fn references_arguments(symbol: &str, direction: &str, depth: Option<u32>) -> Option<Value> {
    let symbol = symbol.trim();
    if symbol.is_empty() {
        return None;
    }
    let mut args = json!({"symbol": symbol});
    if matches!(direction, "inbound" | "outbound" | "both") {
        args["direction"] = json!(direction);
    }
    if let Some(depth) = depth {
        args["depth"] = json!(depth.clamp(1, 5));
    }
    Some(args)
}

/// Arguments of `search_index`.
pub fn search_arguments(query: &str, fts5: bool) -> Option<Value> {
    let query = query.trim();
    if query.is_empty() {
        return None;
    }
    Some(json!({"query": query, "syntax": if fts5 { "fts5" } else { "plain" }}))
}

impl Runtime {
    /// Run one index tool with `arguments` in the workspace.
    pub async fn run_index_tool(&self, tool: &str, arguments: Value) -> ToolOutput {
        let engine = self.engine();
        let Some(tool_impl) = engine.tools().get(tool) else {
            return ToolOutput::error(format!("index tool is not available: {tool}"));
        };
        let ctx = engine
            .tool_environment(None)
            .context(engine.cwd().to_path_buf());
        tool_impl.execute(arguments, ctx).await
    }

    /// `/index <arguments>` as transcript lines.
    pub async fn index_command(&self, arguments: &str) -> Vec<String> {
        match parse_index_command(arguments) {
            IndexCommand::Menu => menu_lines(),
            IndexCommand::Lines(lines) => lines,
            IndexCommand::Invoke(invocation) => {
                let output = self
                    .run_index_tool(&invocation.tool, invocation.arguments)
                    .await;
                index_result_lines(&invocation.label, &output)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn invocation(text: &str) -> IndexInvocation {
        match parse_index_command(text) {
            IndexCommand::Invoke(i) => i,
            other => panic!("expected invocation for {text}: {other:?}"),
        }
    }

    #[test]
    fn parses_archived_subcommands() {
        assert_eq!(parse_index_command("  "), IndexCommand::Menu);
        let status = invocation("status");
        assert_eq!(status.label, "index status");
        assert_eq!(status.arguments["operation"], "status");
        assert_eq!(
            status.arguments["arguments"]["include_tree_sitter"],
            json!(true)
        );
        let tree = invocation("schemas Code");
        assert_eq!(tree.label, "index schemas: code");
        assert_eq!(
            tree.arguments,
            json!({"action":"tree","include_schemas":true,"category":"code"})
        );
        assert_eq!(invocation("git").label, "index category: git");
        assert_eq!(
            invocation("summary 5").arguments["arguments"],
            json!({"limit": 5})
        );
        assert_eq!(
            parse_index_command("summary x"),
            IndexCommand::Lines(vec!["usage: /index summary [limit]".into()])
        );
        assert_eq!(
            invocation("rescan because").arguments["arguments"],
            json!({"reason": "because"})
        );
        let call = invocation("call fs.read {\"file\":\"PLAN.md\"}");
        assert_eq!(call.label, "index call: fs.read");
        assert_eq!(call.arguments["arguments"]["file"], "PLAN.md");
        assert_eq!(
            invocation("code.symbol {}").arguments["operation"],
            "code.symbol"
        );
        let IndexCommand::Lines(lines) = parse_index_command("call fs.read [1]") else {
            panic!()
        };
        assert_eq!(lines[0], "index arguments must be a JSON object.");
        let IndexCommand::Lines(lines) = parse_index_command("serial") else {
            panic!()
        };
        assert_eq!(lines[0], "unknown index command: serial");
        assert!(
            menu_lines()
                .iter()
                .any(|l| l == "  rescan - Request an immediate background rescan")
        );
    }

    #[test]
    fn result_lines_and_panel_arguments() {
        let ok = ToolOutput::text("{\"a\":1}");
        assert_eq!(
            index_result_lines("index status", &ok),
            ["index status", "{", "  \"a\": 1", "}"]
        );
        let mut err = ToolOutput::error("index_not_ready\n{\"code\":1}");
        err.details = Some(json!({"status": "execution_error"}));
        assert_eq!(
            index_result_lines("index status", &err),
            ["index status failed: execution_error - index_not_ready"]
        );
        assert_eq!(symbol_arguments(" ", "", "", false), None);
        assert_eq!(
            symbol_arguments("Foo::bar", "src/a.cpp", "", true).unwrap(),
            json!({"name":"Foo::bar","file":"src/a.cpp","context":true})
        );
        assert_eq!(file_arguments("x.c").unwrap(), json!({"file":"x.c"}));
        assert_eq!(
            references_arguments("f", "inbound", Some(9)).unwrap(),
            json!({"symbol":"f","direction":"inbound","depth":5})
        );
        assert_eq!(
            search_arguments("a b", false).unwrap(),
            json!({"query":"a b","syntax":"plain"})
        );
    }
}
