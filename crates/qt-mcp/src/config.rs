//! MCP server configuration: per-server keys, file discovery and merge order.
//!
//! Servers come from, in increasing precedence (later layers replace a server of the
//! same name wholesale):
//!
//! 1. settings layers passed by the caller (global `settings.json`, then project
//!    settings), each under `mcpServers` or, when that key is absent, `mcp_servers`;
//! 2. `~/.config/mcp/mcp.json`;
//! 3. `<agent_home>/mcp.json`;
//! 4. `<workspace>/.mcp.json`;
//! 5. `<workspace>/.pi/mcp.json`.
//!
//! The merged list is ordered by server name, as in the archived qt. MCP files can only
//! contribute servers: policy keys such as `allowExternalFilesystemAccess` are stripped
//! by [`read_mcp_file`].

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

/// Default per-request timeout (seconds).
pub const DEFAULT_TIMEOUT_SECONDS: u64 = 120;
/// Default timeout for connecting and `initialize` (seconds).
pub const DEFAULT_CONNECT_TIMEOUT_SECONDS: u64 = 60;

/// Settings keys an MCP file may never set.
const POLICY_KEYS: [&str; 2] = [
    "allowExternalFilesystemAccess",
    "allow_external_filesystem_access",
];

/// Wire transport for one server.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransportKind {
    /// Child process speaking newline-delimited JSON-RPC on stdin/stdout.
    Stdio,
    /// Streamable HTTP (`http` and `streamable-http`).
    StreamableHttp,
    /// Legacy HTTP+SSE: a GET event stream plus POSTs to the announced endpoint.
    Sse,
}

impl TransportKind {
    /// Parse a `transport` value; unknown names are unsupported.
    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "stdio" => Some(Self::Stdio),
            "http" | "streamable-http" => Some(Self::StreamableHttp),
            "sse" => Some(Self::Sse),
            _ => None,
        }
    }
}

/// One configured MCP server.
#[derive(Debug, Clone, PartialEq)]
pub struct McpServerConfig {
    pub name: String,
    pub enabled: bool,
    /// Raw transport name (`stdio`, `http`, `streamable-http`, `sse`, or anything else,
    /// which is reported as unsupported).
    pub transport: String,
    pub command: Option<String>,
    pub args: Vec<String>,
    pub env: BTreeMap<String, String>,
    pub url: Option<String>,
    pub headers: BTreeMap<String, String>,
    /// Sampling policy object: `enabled`, `model`, `allowed_models`, `max_tokens_cap`,
    /// `timeout`.
    pub sampling: Value,
    pub timeout_seconds: u64,
    pub connect_timeout_seconds: u64,
    /// File that receives the stderr of stdio servers (appended, mode 0600).
    pub stderr_log_path: Option<PathBuf>,
}

impl McpServerConfig {
    /// A stdio server with default settings.
    pub fn stdio(name: impl Into<String>, command: impl Into<String>, args: Vec<String>) -> Self {
        Self {
            command: Some(command.into()),
            args,
            ..Self::named(name)
        }
    }

    /// A streamable HTTP (`http`) or legacy `sse` server with default settings.
    pub fn remote(name: impl Into<String>, url: impl Into<String>, transport: &str) -> Self {
        Self {
            url: Some(url.into()),
            transport: transport.into(),
            ..Self::named(name)
        }
    }

    fn named(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            enabled: true,
            transport: "stdio".into(),
            command: None,
            args: Vec::new(),
            env: BTreeMap::new(),
            url: None,
            headers: BTreeMap::new(),
            sampling: Value::Object(Map::new()),
            timeout_seconds: DEFAULT_TIMEOUT_SECONDS,
            connect_timeout_seconds: DEFAULT_CONNECT_TIMEOUT_SECONDS,
            stderr_log_path: None,
        }
    }

    pub fn transport_kind(&self) -> Option<TransportKind> {
        TransportKind::parse(&self.transport)
    }

    /// Whether the server may issue `sampling/createMessage` (default true).
    pub fn sampling_enabled(&self) -> bool {
        self.sampling
            .get("enabled")
            .and_then(Value::as_bool)
            .unwrap_or(true)
    }
}

/// `<agent_home>/logs/mcp-stderr.log`.
pub fn stderr_log_path(agent_home: &Path) -> PathBuf {
    agent_home.join("logs").join("mcp-stderr.log")
}

/// Parse an `mcpServers` object. `field` prefixes error messages.
pub fn parse_mcp_servers(value: &Value, field: &str) -> Result<Vec<McpServerConfig>, String> {
    let Some(object) = value.as_object() else {
        return Err(format!("{field} must be an object"));
    };
    let mut servers = Vec::with_capacity(object.len());
    for (name, value) in object {
        let prefix = format!("{field}.{name}");
        let Some(value) = value.as_object() else {
            return Err(format!("{prefix} must be an object"));
        };
        let mut server = McpServerConfig::named(name.clone());
        if let Some(enabled) = value.get("enabled") {
            server.enabled = enabled
                .as_bool()
                .ok_or_else(|| format!("{prefix}.enabled must be a boolean"))?;
        }
        if let Some(command) = value.get("command") {
            server.command = Some(string_field(command, &prefix, "command")?);
            server.transport = "stdio".into();
        }
        if let Some(url) = value.get("url") {
            server.url = Some(string_field(url, &prefix, "url")?);
            if server.command.is_none() {
                server.transport = "streamable-http".into();
            }
        }
        if let Some(transport) = value.get("transport") {
            server.transport = string_field(transport, &prefix, "transport")?;
        }
        if let Some(args) = value.get("args") {
            server.args = string_array(args, &format!("{prefix}.args"))?;
        }
        if let Some(env) = value.get("env") {
            server.env = string_map(env, &format!("{prefix}.env"))?;
        }
        if let Some(headers) = value.get("headers") {
            server.headers = string_map(headers, &format!("{prefix}.headers"))?;
        }
        if let Some(sampling) = value.get("sampling") {
            if !sampling.is_object() {
                return Err(format!("{prefix}.sampling must be an object"));
            }
            server.sampling = sampling.clone();
        }
        for key in ["timeout", "timeoutSeconds"] {
            if let Some(timeout) = value.get(key) {
                server.timeout_seconds = int_field(timeout, &format!("{prefix}.{key}"))?;
            }
        }
        if let Some(timeout) = value.get("connectTimeoutSeconds") {
            server.connect_timeout_seconds =
                int_field(timeout, &format!("{prefix}.connectTimeoutSeconds"))?;
        }
        servers.push(server);
    }
    Ok(servers)
}

/// Servers declared by one settings or MCP document (`mcpServers`, else `mcp_servers`).
/// `None` when the document declares neither key.
pub fn servers_from_document(
    document: &Map<String, Value>,
) -> Result<Option<Vec<McpServerConfig>>, String> {
    if let Some(value) = document.get("mcpServers") {
        return parse_mcp_servers(value, "mcpServers").map(Some);
    }
    if let Some(value) = document.get("mcp_servers") {
        return parse_mcp_servers(value, "mcp_servers").map(Some);
    }
    Ok(None)
}

/// Existing MCP config files in merge order, without duplicates.
pub fn mcp_config_paths(
    user_home: Option<&Path>,
    agent_home: &Path,
    workspace_root: Option<&Path>,
) -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(home) = user_home {
        candidates.push(home.join(".config").join("mcp").join("mcp.json"));
    }
    candidates.push(agent_home.join("mcp.json"));
    if let Some(ws) = workspace_root.filter(|p| !p.as_os_str().is_empty()) {
        candidates.push(ws.join(".mcp.json"));
        candidates.push(ws.join(".pi").join("mcp.json"));
    }
    let mut paths: Vec<PathBuf> = Vec::new();
    for path in candidates {
        if path.is_file() && !paths.contains(&path) {
            paths.push(path);
        }
    }
    paths
}

/// Read an MCP file as a settings document with policy keys removed.
pub fn read_mcp_file(path: &Path) -> Result<Map<String, Value>, String> {
    let text = std::fs::read_to_string(path)
        .map_err(|e| format!("failed to read {}: {e}", path.display()))?;
    let value: Value = serde_json::from_str(&text)
        .map_err(|e| format!("invalid JSON in {}: {e}", path.display()))?;
    let Value::Object(mut document) = value else {
        return Err(format!("{} must contain a JSON object", path.display()));
    };
    for key in POLICY_KEYS {
        document.remove(key);
    }
    Ok(document)
}

/// Merge server lists: later layers replace servers with the same name. Result is
/// ordered by name.
pub fn merge_servers(
    layers: impl IntoIterator<Item = Vec<McpServerConfig>>,
) -> Vec<McpServerConfig> {
    let mut by_name: BTreeMap<String, McpServerConfig> = BTreeMap::new();
    for layer in layers {
        for server in layer {
            by_name.insert(server.name.clone(), server);
        }
    }
    by_name.into_values().collect()
}

/// Discover and merge every MCP server for a session.
///
/// `settings_layers` are the already-loaded settings documents (global first, then
/// project). `user_home` is normally `dirs::home_dir()`. Every server gets the shared
/// stderr log `<agent_home>/logs/mcp-stderr.log`.
pub fn load_server_configs(
    settings_layers: &[&Map<String, Value>],
    user_home: Option<&Path>,
    agent_home: &Path,
    workspace_root: Option<&Path>,
) -> Result<Vec<McpServerConfig>, String> {
    let mut layers = Vec::new();
    for document in settings_layers {
        if let Some(servers) = servers_from_document(document)? {
            layers.push(servers);
        }
    }
    for path in mcp_config_paths(user_home, agent_home, workspace_root) {
        let document = read_mcp_file(&path)?;
        let servers = servers_from_document(&document)
            .map_err(|e| format!("invalid MCP config in {}: {e}", path.display()))?;
        if let Some(servers) = servers {
            layers.push(servers);
        }
    }
    let log = stderr_log_path(agent_home);
    let mut merged = merge_servers(layers);
    for server in &mut merged {
        server.stderr_log_path = Some(log.clone());
    }
    Ok(merged)
}

/// [`load_server_configs`] with the real home directory.
pub fn discover_server_configs(
    settings_layers: &[&Map<String, Value>],
    agent_home: &Path,
    workspace_root: Option<&Path>,
) -> Result<Vec<McpServerConfig>, String> {
    let home = dirs::home_dir();
    load_server_configs(settings_layers, home.as_deref(), agent_home, workspace_root)
}

fn string_field(value: &Value, prefix: &str, key: &str) -> Result<String, String> {
    value
        .as_str()
        .map(str::to_string)
        .ok_or_else(|| format!("{prefix}.{key} must be a string"))
}

fn string_array(value: &Value, field: &str) -> Result<Vec<String>, String> {
    let Some(items) = value.as_array() else {
        return Err(format!("{field} must be an array"));
    };
    items
        .iter()
        .enumerate()
        .map(|(i, item)| {
            item.as_str()
                .map(str::to_string)
                .ok_or_else(|| format!("{field}[{i}] must be a string"))
        })
        .collect()
}

fn string_map(value: &Value, field: &str) -> Result<BTreeMap<String, String>, String> {
    let Some(object) = value.as_object() else {
        return Err(format!("{field} must be an object"));
    };
    object
        .iter()
        .map(|(k, v)| {
            v.as_str()
                .map(|s| (k.clone(), s.to_string()))
                .ok_or_else(|| format!("{field}.{k} must be a string"))
        })
        .collect()
}

/// Integer seconds, clamped to at least 1.
fn int_field(value: &Value, field: &str) -> Result<u64, String> {
    let Some(n) = value.as_i64() else {
        return Err(if value.as_u64().is_some() {
            format!("{field} is out of range")
        } else {
            format!("{field} must be an integer")
        });
    };
    if n < i64::from(i32::MIN) || n > i64::from(i32::MAX) {
        return Err(format!("{field} is out of range"));
    }
    Ok(n.max(1) as u64)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn doc(value: Value) -> Map<String, Value> {
        value.as_object().unwrap().clone()
    }

    #[test]
    fn parses_every_server_key() {
        let servers = parse_mcp_servers(
            &json!({
                "local": {
                    "command": "node",
                    "args": ["server.js"],
                    "env": {"A": "1"},
                    "sampling": {"enabled": false},
                    "timeoutSeconds": 5,
                    "connectTimeoutSeconds": 0
                },
                "remote": {"url": "https://x/mcp", "headers": {"Authorization": "Bearer t"}},
                "legacy": {"url": "http://x/sse", "transport": "sse", "enabled": false, "timeout": 7},
                "both": {"command": "c", "url": "http://x"}
            }),
            "mcpServers",
        )
        .unwrap();
        let local = servers.iter().find(|s| s.name == "local").unwrap();
        assert_eq!(local.transport_kind(), Some(TransportKind::Stdio));
        assert_eq!(local.args, ["server.js"]);
        assert_eq!(local.env["A"], "1");
        assert!(!local.sampling_enabled());
        assert_eq!(local.timeout_seconds, 5);
        assert_eq!(local.connect_timeout_seconds, 1);
        let remote = servers.iter().find(|s| s.name == "remote").unwrap();
        assert_eq!(remote.transport, "streamable-http");
        assert_eq!(remote.headers["Authorization"], "Bearer t");
        assert!(remote.sampling_enabled());
        assert_eq!(remote.timeout_seconds, DEFAULT_TIMEOUT_SECONDS);
        assert_eq!(
            remote.connect_timeout_seconds,
            DEFAULT_CONNECT_TIMEOUT_SECONDS
        );
        let legacy = servers.iter().find(|s| s.name == "legacy").unwrap();
        assert_eq!(legacy.transport_kind(), Some(TransportKind::Sse));
        assert!(!legacy.enabled);
        assert_eq!(legacy.timeout_seconds, 7);
        let both = servers.iter().find(|s| s.name == "both").unwrap();
        assert_eq!(both.transport, "stdio");
        assert_eq!(
            TransportKind::parse("http"),
            Some(TransportKind::StreamableHttp)
        );
        assert_eq!(TransportKind::parse("websocket"), None);
    }

    #[test]
    fn reports_invalid_values_with_archived_wording() {
        let cases = [
            (json!([]), "mcpServers must be an object"),
            (json!({"a": 1}), "mcpServers.a must be an object"),
            (
                json!({"a": {"enabled": "yes"}}),
                "mcpServers.a.enabled must be a boolean",
            ),
            (
                json!({"a": {"command": 1}}),
                "mcpServers.a.command must be a string",
            ),
            (
                json!({"a": {"url": false}}),
                "mcpServers.a.url must be a string",
            ),
            (
                json!({"a": {"transport": 2}}),
                "mcpServers.a.transport must be a string",
            ),
            (
                json!({"a": {"args": "x"}}),
                "mcpServers.a.args must be an array",
            ),
            (
                json!({"a": {"args": ["x", 1]}}),
                "mcpServers.a.args[1] must be a string",
            ),
            (
                json!({"a": {"env": []}}),
                "mcpServers.a.env must be an object",
            ),
            (
                json!({"a": {"headers": {"h": 1}}}),
                "mcpServers.a.headers.h must be a string",
            ),
            (
                json!({"a": {"sampling": true}}),
                "mcpServers.a.sampling must be an object",
            ),
            (
                json!({"a": {"timeout": 1.5}}),
                "mcpServers.a.timeout must be an integer",
            ),
            (
                json!({"a": {"timeoutSeconds": 99999999999u64}}),
                "mcpServers.a.timeoutSeconds is out of range",
            ),
            (
                json!({"a": {"connectTimeoutSeconds": "5"}}),
                "mcpServers.a.connectTimeoutSeconds must be an integer",
            ),
        ];
        for (value, message) in cases {
            assert_eq!(
                parse_mcp_servers(&value, "mcpServers").unwrap_err(),
                message
            );
        }
    }

    #[test]
    fn snake_case_alias_is_used_only_without_camel_case() {
        let both = doc(json!({"mcpServers": {"a": {}}, "mcp_servers": {"b": {}}}));
        let names: Vec<_> = servers_from_document(&both)
            .unwrap()
            .unwrap()
            .into_iter()
            .map(|s| s.name)
            .collect();
        assert_eq!(names, ["a"]);
        let snake = doc(json!({"mcp_servers": {"b": {"command": 1}}}));
        assert_eq!(
            servers_from_document(&snake).unwrap_err(),
            "mcp_servers.b.command must be a string"
        );
        assert!(servers_from_document(&doc(json!({}))).unwrap().is_none());
    }

    #[test]
    fn discovers_files_in_merge_order_and_strips_policy() {
        let dir = tempfile::tempdir().unwrap();
        let user = dir.path().join("user");
        let agent = dir.path().join("agent");
        let ws = dir.path().join("ws");
        let write = |path: PathBuf, value: Value| {
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, value.to_string()).unwrap();
        };
        write(
            user.join(".config/mcp/mcp.json"),
            json!({"mcpServers": {"a": {"command": "user-a"}, "b": {"command": "user-b"}}}),
        );
        write(
            agent.join("mcp.json"),
            json!({"mcpServers": {"b": {"command": "agent-b"}}}),
        );
        write(
            ws.join(".mcp.json"),
            json!({"allowExternalFilesystemAccess": true, "mcp_servers": {"c": {"url": "http://x"}}}),
        );
        write(
            ws.join(".pi/mcp.json"),
            json!({"mcpServers": {"a": {"command": "pi-a"}}}),
        );
        let paths = mcp_config_paths(Some(&user), &agent, Some(&ws));
        assert_eq!(
            paths,
            [
                user.join(".config/mcp/mcp.json"),
                agent.join("mcp.json"),
                ws.join(".mcp.json"),
                ws.join(".pi/mcp.json"),
            ]
        );
        let stripped = read_mcp_file(&ws.join(".mcp.json")).unwrap();
        assert!(!stripped.contains_key("allowExternalFilesystemAccess"));

        let global =
            doc(json!({"mcpServers": {"a": {"command": "settings-a"}, "z": {"command": "z"}}}));
        let project = doc(json!({"mcp_servers": {"z": {"command": "project-z"}}}));
        let servers =
            load_server_configs(&[&global, &project], Some(&user), &agent, Some(&ws)).unwrap();
        let summary: Vec<_> = servers
            .iter()
            .map(|s| {
                (
                    s.name.as_str(),
                    s.command.clone().or(s.url.clone()).unwrap_or_default(),
                )
            })
            .collect();
        assert_eq!(
            summary,
            [
                ("a", "pi-a".to_string()),
                ("b", "agent-b".to_string()),
                ("c", "http://x".to_string()),
                ("z", "project-z".to_string()),
            ]
        );
        assert!(
            servers.iter().all(|s| s.stderr_log_path.as_deref()
                == Some(agent.join("logs/mcp-stderr.log").as_path()))
        );
    }

    #[test]
    fn invalid_files_name_the_path() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("mcp.json"), r#"{"mcpServers": {"x": 3}}"#).unwrap();
        let err = load_server_configs(&[], None, dir.path(), None).unwrap_err();
        assert!(err.starts_with("invalid MCP config in "), "{err}");
        assert!(err.ends_with("mcpServers.x must be an object"), "{err}");
        std::fs::write(dir.path().join("mcp.json"), "[1]").unwrap();
        let err = load_server_configs(&[], None, dir.path(), None).unwrap_err();
        assert!(err.ends_with("must contain a JSON object"), "{err}");
    }
}
