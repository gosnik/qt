//! Connects every configured server and exposes their tools and resources.
//!
//! The manager is the integration point for the runtime (tool registration, sampling)
//! and for a `/mcp` panel ([`McpManager::snapshots`], [`McpManager::reconnect`],
//! [`McpManager::subscribe`]).

use std::sync::{Arc, Mutex, RwLock};
use std::time::Duration;

use futures::future::join_all;
use qt_core::tools::{Tool, ToolRegistry};
use serde_json::{Value, json};
use tokio::sync::watch;
use tokio_util::sync::CancellationToken;

use crate::client::{InitializeInfo, McpClient};
use crate::config::McpServerConfig;
use crate::error::McpError;
use crate::sampling::{SamplingHandler, SamplingSlot};
use crate::tools::{ListMcpResourcesTool, McpProxyTool, ReadMcpResourceTool};
use crate::util::{looks_like_prompt_injection, registered_tool_name, sanitize_mcp_error};

/// Retry policy for transport failures.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct McpOptions {
    /// Attempts per operation (at least 1).
    pub attempts: u32,
    /// Delay before the second attempt; doubles each time.
    pub base_delay: Duration,
}

impl Default for McpOptions {
    /// Five attempts with 100 ms exponential backoff, as in the archived qt.
    fn default() -> Self {
        Self {
            attempts: 5,
            base_delay: Duration::from_millis(100),
        }
    }
}

/// A tool discovered on a server.
#[derive(Debug, Clone, PartialEq)]
pub struct RemoteTool {
    pub server_name: String,
    pub remote_name: String,
    /// `mcp_<server>_<tool>`.
    pub registered_name: String,
    pub description: String,
    pub input_schema: Value,
}

/// Connection state of one server.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ServerStatus {
    /// `enabled: false` in the configuration.
    Disabled,
    /// The transport name is not supported.
    Unsupported(String),
    /// Not connected yet (or disconnected on request).
    Idle,
    Connecting,
    Connected,
    /// The last connection attempt failed with this (sanitized) message.
    Failed(String),
}

impl ServerStatus {
    pub fn label(&self) -> &'static str {
        match self {
            Self::Disabled => "disabled",
            Self::Unsupported(_) => "unsupported",
            Self::Idle => "idle",
            Self::Connecting => "connecting",
            Self::Connected => "connected",
            Self::Failed(_) => "failed",
        }
    }
}

/// Everything a `/mcp` panel shows for one server.
#[derive(Debug, Clone, PartialEq)]
pub struct ServerSnapshot {
    pub name: String,
    pub transport: String,
    pub enabled: bool,
    pub status: ServerStatus,
    pub tools: Vec<RemoteTool>,
    pub info: Option<InitializeInfo>,
    /// Command line (stdio) or URL (HTTP/SSE), without headers or env values.
    pub target: String,
}

struct ServerEntry {
    client: McpClient,
    state: Mutex<ServerState>,
}

struct ServerState {
    status: ServerStatus,
    tools: Vec<RemoteTool>,
}

impl ServerEntry {
    fn config(&self) -> &McpServerConfig {
        self.client.config()
    }

    fn state(&self) -> std::sync::MutexGuard<'_, ServerState> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

/// Owns one [`McpClient`] per configured server.
pub struct McpManager {
    entries: Vec<ServerEntry>,
    sampling: SamplingSlot,
    options: McpOptions,
    changes: watch::Sender<u64>,
}

impl McpManager {
    pub fn new(configs: Vec<McpServerConfig>) -> Arc<Self> {
        Self::with_options(configs, McpOptions::default())
    }

    pub fn with_options(configs: Vec<McpServerConfig>, options: McpOptions) -> Arc<Self> {
        let sampling: SamplingSlot = Arc::new(RwLock::new(None));
        let entries = configs
            .into_iter()
            .map(|config| {
                let status = if !config.enabled {
                    ServerStatus::Disabled
                } else if config.transport_kind().is_none() {
                    ServerStatus::Unsupported(config.transport.clone())
                } else {
                    ServerStatus::Idle
                };
                ServerEntry {
                    client: McpClient::new(config, sampling.clone()),
                    state: Mutex::new(ServerState {
                        status,
                        tools: Vec::new(),
                    }),
                }
            })
            .collect();
        let (changes, _) = watch::channel(0);
        Arc::new(Self {
            entries,
            sampling,
            options,
            changes,
        })
    }

    /// Install (or clear) the handler for `sampling/createMessage`. Install it before
    /// connecting so `initialize` advertises the sampling capability.
    pub fn set_sampling_handler(&self, handler: Option<Arc<dyn SamplingHandler>>) {
        *self.sampling.write().unwrap_or_else(|e| e.into_inner()) = handler;
    }

    /// Change counter bumped on every status change, for UI refresh.
    pub fn subscribe(&self) -> watch::Receiver<u64> {
        self.changes.subscribe()
    }

    fn notify_changed(&self) {
        self.changes.send_modify(|n| *n = n.wrapping_add(1));
    }

    pub fn server_names(&self) -> Vec<String> {
        self.entries
            .iter()
            .map(|e| e.config().name.clone())
            .collect()
    }

    fn entry(&self, name: &str) -> Result<&ServerEntry, McpError> {
        self.entries
            .iter()
            .find(|e| e.config().name == name)
            .ok_or_else(|| McpError::NotFound(name.to_string()))
    }

    fn usable_entry(&self, name: &str) -> Result<&ServerEntry, McpError> {
        let entry = self.entry(name)?;
        let config = entry.config();
        if !config.enabled {
            return Err(McpError::Config(format!("MCP server is disabled: {name}")));
        }
        if config.transport_kind().is_none() {
            return Err(McpError::Config(format!(
                "MCP transport is not implemented for server {name}: {}",
                config.transport
            )));
        }
        Ok(entry)
    }

    async fn retry<T, F, Fut>(&self, mut op: F) -> Result<T, McpError>
    where
        F: FnMut() -> Fut,
        Fut: Future<Output = Result<T, McpError>>,
    {
        let attempts = self.options.attempts.max(1);
        let mut attempt = 0;
        loop {
            match op().await {
                Err(err) if err.is_retryable() && attempt + 1 < attempts => {
                    tokio::time::sleep(self.options.base_delay * 2u32.saturating_pow(attempt))
                        .await;
                    attempt += 1;
                }
                result => return result,
            }
        }
    }

    /// Connect every enabled server and list its tools. Returns diagnostics in
    /// configuration order (unsupported transports, failures, suspicious descriptions).
    pub async fn connect_all(&self) -> Vec<String> {
        let results = join_all(self.entries.iter().map(|e| self.connect_entry(e))).await;
        results.into_iter().flatten().collect()
    }

    /// Disconnect and connect one server again. Returns its diagnostics.
    pub async fn reconnect(&self, name: &str) -> Result<Vec<String>, McpError> {
        let entry = self.entry(name)?;
        entry.client.disconnect().await;
        Ok(self.connect_entry(entry).await)
    }

    /// Close one server's connection without forgetting its tools' names.
    pub async fn disconnect(&self, name: &str) -> Result<(), McpError> {
        let entry = self.entry(name)?;
        entry.client.disconnect().await;
        {
            let mut state = entry.state();
            if matches!(
                state.status,
                ServerStatus::Connected | ServerStatus::Failed(_)
            ) {
                state.status = ServerStatus::Idle;
            }
        }
        self.notify_changed();
        Ok(())
    }

    /// Close every connection (terminating stdio servers).
    pub async fn shutdown(&self) {
        join_all(self.entries.iter().map(|e| e.client.disconnect())).await;
        for entry in &self.entries {
            let mut state = entry.state();
            if matches!(state.status, ServerStatus::Connected) {
                state.status = ServerStatus::Idle;
            }
        }
        self.notify_changed();
    }

    async fn connect_entry(&self, entry: &ServerEntry) -> Vec<String> {
        let config = entry.config();
        let mut diagnostics = Vec::new();
        if !config.enabled {
            return diagnostics;
        }
        if config.transport_kind().is_none() {
            diagnostics.push(format!(
                "MCP server {} uses unsupported transport {}",
                config.name, config.transport
            ));
            return diagnostics;
        }
        entry.state().status = ServerStatus::Connecting;
        self.notify_changed();
        match self.retry(|| entry.client.list_tools()).await {
            Ok(listed) => {
                let tools = remote_tools(&config.name, &listed);
                for tool in &tools {
                    if looks_like_prompt_injection(&tool.description) {
                        diagnostics.push(format!(
                            "MCP tool {} description contains prompt-like text",
                            tool.registered_name
                        ));
                    }
                }
                let mut state = entry.state();
                state.status = ServerStatus::Connected;
                state.tools = tools;
            }
            Err(err) => {
                let message = sanitize_mcp_error(&err.to_string());
                diagnostics.push(format!(
                    "MCP tool discovery failed for {}: {message}",
                    config.name
                ));
                let mut state = entry.state();
                state.status = ServerStatus::Failed(message);
                state.tools.clear();
            }
        }
        self.notify_changed();
        diagnostics
    }

    /// Status of every configured server, in configuration order.
    pub fn snapshots(&self) -> Vec<ServerSnapshot> {
        self.entries
            .iter()
            .map(|entry| {
                let config = entry.config();
                let state = entry.state();
                let target = match (&config.command, &config.url) {
                    (Some(command), _) if config.transport == "stdio" => {
                        std::iter::once(command.as_str())
                            .chain(config.args.iter().map(String::as_str))
                            .collect::<Vec<_>>()
                            .join(" ")
                    }
                    (_, Some(url)) => url.clone(),
                    (Some(command), None) => command.clone(),
                    (None, None) => String::new(),
                };
                ServerSnapshot {
                    name: config.name.clone(),
                    transport: config.transport.clone(),
                    enabled: config.enabled,
                    status: state.status.clone(),
                    tools: state.tools.clone(),
                    info: entry.client.info(),
                    target,
                }
            })
            .collect()
    }

    /// Tools of every connected server, in configuration order.
    pub fn tools(&self) -> Vec<RemoteTool> {
        self.entries
            .iter()
            .flat_map(|e| e.state().tools.clone())
            .collect()
    }

    /// `tools/call` with retries on transport failures; the raw `CallToolResult`.
    pub async fn call_tool(
        &self,
        server: &str,
        tool: &str,
        arguments: Value,
        cancel: Option<&CancellationToken>,
    ) -> Result<Value, McpError> {
        let entry = self.usable_entry(server)?;
        let result = self
            .retry(|| entry.client.call_tool(tool, arguments.clone(), cancel))
            .await;
        self.track(entry, &result);
        result
    }

    /// One page of resources. With `server`, that server's page at `cursor`; without,
    /// the first page of every connected server that declares resources. Each resource
    /// gains a `server` field. Per-server failures are returned in `errors`.
    pub async fn list_resources(
        &self,
        server: Option<&str>,
        cursor: Option<&str>,
    ) -> Result<Value, McpError> {
        if let Some(server) = server {
            let entry = self.usable_entry(server)?;
            let result = self.retry(|| entry.client.list_resources(cursor)).await;
            self.track(entry, &result);
            let result = result?;
            return Ok(json!({
                "server": server,
                "resources": tag_resources(server, &result),
                "nextCursor": result.get("nextCursor").cloned().unwrap_or(Value::Null),
                "errors": [],
            }));
        }
        let candidates: Vec<&ServerEntry> = self
            .entries
            .iter()
            .filter(|e| {
                matches!(e.state().status, ServerStatus::Connected)
                    && e.client.info().is_some_and(|i| i.supports_resources())
            })
            .collect();
        let results = join_all(candidates.iter().map(|entry| async move {
            let result = self.retry(|| entry.client.list_resources(None)).await;
            self.track(entry, &result);
            (entry.config().name.clone(), result)
        }))
        .await;
        let mut resources = Vec::new();
        let mut errors = Vec::new();
        for (name, result) in results {
            match result {
                Ok(result) => resources.extend(tag_resources(&name, &result)),
                Err(err) => errors.push(json!({"server": name, "error": err.to_string()})),
            }
        }
        Ok(json!({
            "server": Value::Null,
            "resources": resources,
            "nextCursor": Value::Null,
            "errors": errors,
        }))
    }

    /// `resources/read` on one server; the raw result (`contents`).
    pub async fn read_resource(
        &self,
        server: &str,
        uri: &str,
        cancel: Option<&CancellationToken>,
    ) -> Result<Value, McpError> {
        let entry = self.usable_entry(server)?;
        let result = self.retry(|| entry.client.read_resource(uri, cancel)).await;
        self.track(entry, &result);
        result
    }

    /// Keep the status in step with lazy reconnects done by calls.
    fn track<T>(&self, entry: &ServerEntry, result: &Result<T, McpError>) {
        let changed = {
            let mut state = entry.state();
            let next = match result {
                Ok(_) => ServerStatus::Connected,
                Err(McpError::Transport(message)) => ServerStatus::Failed(message.clone()),
                Err(_) => return,
            };
            let changed = state.status != next;
            state.status = next;
            changed
        };
        if changed {
            self.notify_changed();
        }
    }

    /// Proxy tools for every discovered remote tool.
    pub fn proxy_tools(self: &Arc<Self>) -> Vec<Arc<dyn Tool>> {
        self.tools()
            .into_iter()
            .map(|tool| Arc::new(McpProxyTool::new(tool, self.clone())) as Arc<dyn Tool>)
            .collect()
    }

    /// `list_mcp_resources` and `read_mcp_resource`.
    pub fn resource_tools(self: &Arc<Self>) -> Vec<Arc<dyn Tool>> {
        vec![
            Arc::new(ListMcpResourcesTool::new(self.clone())),
            Arc::new(ReadMcpResourceTool::new(self.clone())),
        ]
    }

    /// Add the proxy tools to `registry`, skipping names `enabled` rejects and names
    /// already registered. Diagnostics are appended for skipped duplicates.
    pub fn register_tools(
        self: &Arc<Self>,
        mut registry: ToolRegistry,
        enabled: Option<&dyn Fn(&str) -> bool>,
        diagnostics: &mut Vec<String>,
    ) -> ToolRegistry {
        for tool in self.proxy_tools() {
            if enabled.is_some_and(|f| !f(tool.name())) {
                continue;
            }
            if registry.get(tool.name()).is_some() {
                diagnostics.push(format!(
                    "MCP tool registration skipped duplicate name: {}",
                    tool.name()
                ));
                continue;
            }
            registry = registry.with(tool);
        }
        registry
    }
}

fn tag_resources(server: &str, result: &Value) -> Vec<Value> {
    result
        .get("resources")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .map(|resource| {
            let mut resource = match resource {
                Value::Object(_) => resource.clone(),
                other => json!({"uri": other.to_string()}),
            };
            resource["server"] = json!(server);
            resource
        })
        .collect()
}

/// Turn `tools/list` entries into [`RemoteTool`]s, skipping malformed ones.
pub(crate) fn remote_tools(server: &str, listed: &[Value]) -> Vec<RemoteTool> {
    listed
        .iter()
        .filter_map(|tool| {
            let name = tool.get("name")?.as_str()?;
            let description = tool
                .get("description")
                .and_then(Value::as_str)
                .map(str::to_string)
                .unwrap_or_else(|| format!("MCP tool {name} from {server}"));
            let input_schema = tool
                .get("inputSchema")
                .filter(|s| s.is_object())
                .cloned()
                .unwrap_or_else(|| json!({"type": "object", "additionalProperties": true}));
            Some(RemoteTool {
                server_name: server.to_string(),
                remote_name: name.to_string(),
                registered_name: registered_tool_name(server, name),
                description,
                input_schema,
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn remote_tools_get_defaults() {
        let tools = remote_tools(
            "Git Hub",
            &[
                json!({"name": "list", "description": "List things", "inputSchema": {"type": "object", "properties": {"q": {"type": "string"}}}}),
                json!({"name": "bare", "inputSchema": "nope"}),
                json!({"description": "no name"}),
                json!(3),
            ],
        );
        assert_eq!(tools.len(), 2);
        assert_eq!(tools[0].registered_name, "mcp_git_hub_list");
        assert_eq!(tools[0].input_schema["properties"]["q"]["type"], "string");
        assert_eq!(tools[1].description, "MCP tool bare from Git Hub");
        assert_eq!(
            tools[1].input_schema,
            json!({"type": "object", "additionalProperties": true})
        );
    }

    #[tokio::test]
    async fn statuses_reflect_configuration() {
        let mut disabled = McpServerConfig::stdio("off", "true", vec![]);
        disabled.enabled = false;
        let mut weird = McpServerConfig::stdio("ws", "true", vec![]);
        weird.transport = "websocket".into();
        let manager = McpManager::new(vec![disabled, weird]);
        let diagnostics = manager.connect_all().await;
        assert_eq!(
            diagnostics,
            ["MCP server ws uses unsupported transport websocket"]
        );
        let snaps = manager.snapshots();
        assert_eq!(snaps[0].status, ServerStatus::Disabled);
        assert_eq!(
            snaps[1].status,
            ServerStatus::Unsupported("websocket".into())
        );
        assert_eq!(snaps[1].target, "true");
        assert_eq!(
            manager
                .call_tool("ws", "x", json!({}), None)
                .await
                .unwrap_err()
                .to_string(),
            "MCP transport is not implemented for server ws: websocket"
        );
        assert_eq!(
            manager
                .call_tool("nope", "x", json!({}), None)
                .await
                .unwrap_err()
                .to_string(),
            "MCP server not found: nope"
        );
        assert_eq!(
            manager
                .read_resource("off", "u", None)
                .await
                .unwrap_err()
                .to_string(),
            "MCP server is disabled: off"
        );
    }

    #[tokio::test]
    async fn config_errors_are_not_retried() {
        let config = McpServerConfig::remote("r", "", "http");
        let manager = McpManager::new(vec![config]);
        let started = std::time::Instant::now();
        let diagnostics = manager.connect_all().await;
        assert!(started.elapsed() < Duration::from_millis(100));
        assert_eq!(
            diagnostics,
            ["MCP tool discovery failed for r: MCP HTTP server has no url: r"]
        );
        assert_eq!(
            manager.snapshots()[0].status,
            ServerStatus::Failed("MCP HTTP server has no url: r".into())
        );
    }
}
