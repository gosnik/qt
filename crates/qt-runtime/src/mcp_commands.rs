//! `/mcp` (archived `interactive_mode.cpp` MCP handler): status, tools,
//! resources, reconnect and setup guidance over the live [`McpManager`],
//! shared by the desktop `/mcp` panel and the slash command.

use std::path::{Path, PathBuf};

use qt_mcp::{McpManager, ServerSnapshot, ServerStatus};
use serde_json::Value;

use crate::runtime::Runtime;

/// Entries of the bare `/mcp` menu.
pub const MCP_MENU: &[(&str, &str)] = &[
    ("status", "Show configured MCP server status"),
    ("tools", "List registered MCP tools"),
    ("resources", "List MCP resources through the resource tool"),
    (
        "reconnect",
        "Reconnect all MCP servers and refresh registered tools",
    ),
    ("setup", "Show config precedence and a starter .mcp.json"),
];

/// Where MCP servers are read from, in merge order (later wins).
pub fn config_paths(
    user_home: Option<&Path>,
    agent_home: &Path,
    workspace_root: &Path,
) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    if let Some(home) = user_home {
        paths.push(home.join(".config").join("mcp").join("mcp.json"));
    }
    paths.push(agent_home.join("mcp.json"));
    paths.push(workspace_root.join(".mcp.json"));
    paths.push(workspace_root.join(".pi").join("mcp.json"));
    paths
}

/// `/mcp setup`: config precedence and a starter file (archived wording,
/// with the resolved paths).
pub fn setup_lines(paths: &[PathBuf]) -> Vec<String> {
    let mut lines = vec!["MCP config paths:".to_string()];
    for (i, path) in paths.iter().enumerate() {
        let exists = if path.is_file() { " (found)" } else { "" };
        lines.push(format!("{}. {}{exists}", i + 1, path.display()));
    }
    lines.push("Settings files may also define mcpServers (global, then project).".into());
    lines.extend(
        [
            "",
            "Starter project .mcp.json:",
            "{",
            "  \"mcpServers\": {}",
            "}",
            "",
            "Example server entries:",
            "  \"files\": { \"command\": \"npx\", \"args\": [\"-y\", \"@modelcontextprotocol/server-filesystem\", \".\"] }",
            "  \"remote\": { \"type\": \"http\", \"url\": \"https://example.com/mcp\" }",
            "Restart qt after editing MCP configuration.",
        ]
        .map(str::to_string),
    );
    lines
}

/// The status text of one server.
pub fn status_text(status: &ServerStatus) -> String {
    match status {
        ServerStatus::Failed(e) => format!("failed: {e}"),
        ServerStatus::Unsupported(t) => format!("unsupported transport: {t}"),
        other => other.label().to_string(),
    }
}

/// `/mcp status` lines (archived layout, live connection state).
pub fn status_lines(servers: &[ServerSnapshot]) -> Vec<String> {
    let mut lines = vec!["MCP Server Status:".to_string(), String::new()];
    if servers.is_empty() {
        lines.push("No MCP servers configured".into());
        lines.push("Run /mcp setup to see config paths and a starter .mcp.json.".into());
        return lines;
    }
    for server in servers {
        let mut line = format!(
            "{} {}: {} ({}, {} tools)",
            if server.enabled { "[on]" } else { "[off]" },
            server.name,
            status_text(&server.status),
            server.transport,
            server.tools.len()
        );
        if !server.target.is_empty() {
            line.push(' ');
            line.push_str(&server.target);
        }
        lines.push(line);
    }
    lines
}

/// `/mcp tools` lines.
pub fn tool_lines(servers: &[ServerSnapshot]) -> Vec<String> {
    let mut lines = vec!["MCP Tools:".to_string(), String::new()];
    let mut count = 0;
    for server in servers {
        for tool in &server.tools {
            count += 1;
            lines.push(format!(
                "  {} - MCP {}/{}: {}",
                tool.registered_name, server.name, tool.remote_name, tool.description
            ));
        }
    }
    if count == 0 {
        lines.push("No MCP tools are currently registered.".into());
    } else {
        lines.push(String::new());
        lines.push(format!("Total: {count} tools"));
    }
    lines
}

/// One resource row: `(server, uri, name, mime type)`.
pub fn resource_rows(listing: &Value) -> Vec<(String, String, String, String)> {
    let text = |v: &Value, key: &str| v.get(key).and_then(Value::as_str).unwrap_or("").to_string();
    listing
        .get("resources")
        .and_then(Value::as_array)
        .map(|items| {
            items
                .iter()
                .map(|r| {
                    (
                        text(r, "server"),
                        text(r, "uri"),
                        text(r, "name"),
                        text(r, "mimeType"),
                    )
                })
                .collect()
        })
        .unwrap_or_default()
}

/// `/mcp resources` lines from a `list_resources` result.
pub fn resource_lines(listing: &Value) -> Vec<String> {
    let mut lines = vec!["MCP resources".to_string()];
    let rows = resource_rows(listing);
    if rows.is_empty() {
        lines.push("No MCP resources are available.".into());
    }
    for (server, uri, name, mime) in rows {
        let mut line = format!("  {server}: {uri}");
        if !name.is_empty() && name != uri {
            line.push_str(&format!(" ({name})"));
        }
        if !mime.is_empty() {
            line.push_str(&format!(" [{mime}]"));
        }
        lines.push(line);
    }
    if let Some(errors) = listing.get("errors").and_then(Value::as_array) {
        for e in errors {
            lines.push(format!(
                "  error: {}: {}",
                e.get("server").and_then(Value::as_str).unwrap_or(""),
                e.get("error").and_then(Value::as_str).unwrap_or("")
            ));
        }
    }
    if let Some(cursor) = listing.get("nextCursor").and_then(Value::as_str) {
        lines.push(format!("  more results: cursor {cursor}"));
    }
    lines
}

/// Text of a `resources/read` result: text contents, then a summary line
/// for each blob.
pub fn read_lines(uri: &str, result: &Value) -> Vec<String> {
    let mut lines = vec![format!("MCP resource {uri}")];
    let contents = result
        .get("contents")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    if contents.is_empty() {
        lines.push("(no contents)".into());
    }
    for item in contents {
        if let Some(text) = item.get("text").and_then(Value::as_str) {
            lines.extend(text.lines().map(str::to_string));
        } else if let Some(blob) = item.get("blob").and_then(Value::as_str) {
            lines.push(format!(
                "[binary {} · {} base64 bytes]",
                item.get("mimeType")
                    .and_then(Value::as_str)
                    .unwrap_or("application/octet-stream"),
                blob.len()
            ));
        }
    }
    lines
}

fn usage_lines(first: &str) -> Vec<String> {
    vec![
        format!("unknown mcp command: {first}"),
        "usage: /mcp".into(),
        "usage: /mcp status".into(),
        "usage: /mcp tools".into(),
        "usage: /mcp resources [server]".into(),
        "usage: /mcp reconnect [server]".into(),
        "usage: /mcp setup".into(),
    ]
}

/// Reconnect one server (or all) and report the registered tool count.
pub async fn reconnect_lines(manager: &McpManager, target: Option<&str>) -> Vec<String> {
    let names = manager.server_names();
    if let Some(name) = target
        && !names.iter().any(|n| n == name)
    {
        return vec![format!("MCP server not found: {name}")];
    }
    let mut diagnostics = Vec::new();
    let targets: Vec<String> = match target {
        Some(name) => vec![name.to_string()],
        None => names,
    };
    for name in &targets {
        match manager.reconnect(name).await {
            Ok(notes) => diagnostics.extend(notes),
            Err(e) => diagnostics.push(format!("{name}: {e}")),
        }
    }
    let mut lines = vec![
        match target {
            Some(name) => format!("MCP reconnect: {name}"),
            None => "MCP reconnect: all servers".into(),
        },
        format!("registered_mcp_tools: {}", manager.tools().len()),
    ];
    if !diagnostics.is_empty() {
        lines.push("diagnostics:".into());
        lines.extend(diagnostics.into_iter().map(|d| format!("- {d}")));
    }
    lines
}

impl Runtime {
    /// The MCP config files in merge order.
    pub fn mcp_config_paths(&self) -> Vec<PathBuf> {
        config_paths(
            dirs_home().as_deref(),
            self.agent_home().root(),
            self.engine().workspace_policy().root(),
        )
    }

    /// `/mcp <arguments>` as transcript lines.
    pub async fn mcp_command(&self, arguments: &str) -> Vec<String> {
        let text = arguments.trim();
        let (first, rest) = match text.find(char::is_whitespace) {
            Some(i) => (&text[..i], text[i..].trim()),
            None => (text, ""),
        };
        let sub = first.to_ascii_lowercase();
        if sub == "setup" {
            return setup_lines(&self.mcp_config_paths());
        }
        let servers = self.mcp().map(|m| m.snapshots()).unwrap_or_default();
        match sub.as_str() {
            "" => {
                let mut lines = vec!["mcp:".to_string()];
                lines.extend(MCP_MENU.iter().map(|(n, d)| format!("  {n} - {d}")));
                lines
            }
            "status" => status_lines(&servers),
            "tools" => tool_lines(&servers),
            "resources" => {
                let Some(manager) = self.mcp() else {
                    return vec![
                        "MCP is not initialized.".into(),
                        "Run /mcp setup to configure MCP servers.".into(),
                    ];
                };
                let server = (!rest.is_empty()).then_some(rest);
                match manager.list_resources(server, None).await {
                    Ok(listing) => resource_lines(&listing),
                    Err(e) => vec![format!("MCP resources failed: {e}")],
                }
            }
            "reconnect" => {
                let Some(manager) = self.mcp() else {
                    return vec![
                        "MCP is not initialized.".into(),
                        "Run /mcp setup to configure MCP servers.".into(),
                    ];
                };
                reconnect_lines(manager, (!rest.is_empty()).then_some(rest)).await
            }
            _ => usage_lines(first),
        }
    }
}

fn dirs_home() -> Option<PathBuf> {
    std::env::var_os("HOME")
        .filter(|h| !h.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("USERPROFILE").map(PathBuf::from))
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_mcp::RemoteTool;
    use serde_json::json;

    fn server(name: &str, status: ServerStatus, tools: usize) -> ServerSnapshot {
        ServerSnapshot {
            name: name.into(),
            transport: "stdio".into(),
            enabled: !matches!(status, ServerStatus::Disabled),
            status,
            tools: (0..tools)
                .map(|i| RemoteTool {
                    server_name: name.into(),
                    remote_name: format!("t{i}"),
                    registered_name: format!("mcp_{name}_t{i}"),
                    description: "does".into(),
                    input_schema: json!({}),
                })
                .collect(),
            info: None,
            target: "npx srv".into(),
        }
    }

    #[test]
    fn status_and_tools() {
        assert_eq!(
            status_lines(&[]),
            [
                "MCP Server Status:",
                "",
                "No MCP servers configured",
                "Run /mcp setup to see config paths and a starter .mcp.json."
            ]
        );
        let servers = [
            server("files", ServerStatus::Connected, 2),
            server("old", ServerStatus::Disabled, 0),
            server("bad", ServerStatus::Failed("boom".into()), 0),
        ];
        let lines = status_lines(&servers);
        assert_eq!(lines[2], "[on] files: connected (stdio, 2 tools) npx srv");
        assert_eq!(lines[3], "[off] old: disabled (stdio, 0 tools) npx srv");
        assert_eq!(lines[4], "[on] bad: failed: boom (stdio, 0 tools) npx srv");
        let tools = tool_lines(&servers);
        assert_eq!(tools[2], "  mcp_files_t0 - MCP files/t0: does");
        assert_eq!(tools.last().unwrap(), "Total: 2 tools");
        assert_eq!(tool_lines(&[])[2], "No MCP tools are currently registered.");
    }

    #[test]
    fn resources_reads_and_setup() {
        let listing = json!({"resources": [
            {"server": "files", "uri": "file:///a", "name": "a", "mimeType": "text/plain"},
            {"server": "files", "uri": "file:///b", "name": "file:///b"}
        ], "errors": [{"server": "x", "error": "down"}], "nextCursor": "c2"});
        assert_eq!(
            resource_lines(&listing),
            [
                "MCP resources",
                "  files: file:///a (a) [text/plain]",
                "  files: file:///b",
                "  error: x: down",
                "  more results: cursor c2"
            ]
        );
        assert_eq!(resource_rows(&listing).len(), 2);
        let read =
            json!({"contents": [{"text": "l1\nl2"}, {"blob": "AAAA", "mimeType": "image/png"}]});
        assert_eq!(
            read_lines("u", &read),
            [
                "MCP resource u",
                "l1",
                "l2",
                "[binary image/png · 4 base64 bytes]"
            ]
        );
        let paths = config_paths(Some(Path::new("/h")), Path::new("/a"), Path::new("/w"));
        assert_eq!(
            paths,
            [
                PathBuf::from("/h/.config/mcp/mcp.json"),
                PathBuf::from("/a/mcp.json"),
                PathBuf::from("/w/.mcp.json"),
                PathBuf::from("/w/.pi/mcp.json")
            ]
        );
        let setup = setup_lines(&paths);
        assert_eq!(setup[0], "MCP config paths:");
        assert_eq!(setup[1], "1. /h/.config/mcp/mcp.json");
        assert!(setup.iter().any(|l| l == "  \"mcpServers\": {}"));
        assert_eq!(usage_lines("x")[0], "unknown mcp command: x");
    }
}
