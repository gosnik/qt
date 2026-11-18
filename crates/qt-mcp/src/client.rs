//! One MCP session per server: connect, `initialize`, and the client requests qt uses.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::{Value, json};
use tokio_util::sync::CancellationToken;

use crate::config::McpServerConfig;
use crate::error::McpError;
use crate::sampling::SamplingSlot;
use crate::transport::{Connection, ServerRequests};

/// Upper bound on `tools/list` pages, guarding against servers that never stop paging.
const MAX_PAGES: usize = 100;

/// What the server reported in its `initialize` result.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct InitializeInfo {
    pub protocol_version: String,
    /// `serverInfo` (`name`, `version`).
    pub server_info: Value,
    pub capabilities: Value,
    pub instructions: Option<String>,
}

impl InitializeInfo {
    /// Whether the server declared the `resources` capability.
    pub fn supports_resources(&self) -> bool {
        self.capabilities.get("resources").is_some()
    }
}

/// A lazily connected client for one configured server. Reconnects transparently when
/// the previous connection died (process exit, closed stream, expired HTTP session).
pub struct McpClient {
    config: McpServerConfig,
    requests: ServerRequests,
    http: reqwest::Client,
    conn: tokio::sync::Mutex<Option<Arc<Connection>>>,
    info: Mutex<Option<InitializeInfo>>,
}

impl McpClient {
    pub(crate) fn new(config: McpServerConfig, sampling: SamplingSlot) -> Self {
        let http = reqwest::Client::builder()
            .connect_timeout(Duration::from_secs(config.connect_timeout_seconds.max(1)))
            .user_agent(concat!("qt/", env!("CARGO_PKG_VERSION")))
            .build()
            .unwrap_or_default();
        Self {
            requests: ServerRequests::new(&config, sampling),
            config,
            http,
            conn: tokio::sync::Mutex::new(None),
            info: Mutex::new(None),
        }
    }

    pub fn config(&self) -> &McpServerConfig {
        &self.config
    }

    /// The last `initialize` result, if connected at least once.
    pub fn info(&self) -> Option<InitializeInfo> {
        self.info.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }

    fn timeout(&self) -> Duration {
        Duration::from_secs(self.config.timeout_seconds.max(1))
    }

    fn connect_timeout(&self) -> Duration {
        Duration::from_secs(self.config.connect_timeout_seconds.max(1))
    }

    /// The live connection, connecting and initializing when needed.
    pub(crate) async fn connection(&self) -> Result<Arc<Connection>, McpError> {
        let mut guard = self.conn.lock().await;
        if let Some(conn) = guard.as_ref() {
            if conn.is_alive() {
                return Ok(conn.clone());
            }
            conn.close().await;
            *guard = None;
        }
        let conn =
            Arc::new(Connection::open(&self.config, self.requests.clone(), &self.http).await?);
        match self.initialize(&conn).await {
            Ok(info) => {
                *self.info.lock().unwrap_or_else(|e| e.into_inner()) = Some(info);
                *guard = Some(conn.clone());
                Ok(conn)
            }
            Err(err) => {
                conn.close().await;
                Err(err)
            }
        }
    }

    async fn initialize(&self, conn: &Connection) -> Result<InitializeInfo, McpError> {
        let capabilities = if self.requests.advertises_sampling() {
            json!({"sampling": {}})
        } else {
            json!({})
        };
        let params = json!({
            "protocolVersion": crate::PROTOCOL_VERSION,
            "capabilities": capabilities,
            "clientInfo": {"name": "qt", "version": env!("CARGO_PKG_VERSION")},
        });
        let result = conn
            .request("initialize", params, self.connect_timeout(), None)
            .await?;
        let info = InitializeInfo {
            protocol_version: result
                .get("protocolVersion")
                .and_then(Value::as_str)
                .unwrap_or(crate::PROTOCOL_VERSION)
                .to_string(),
            server_info: result.get("serverInfo").cloned().unwrap_or(Value::Null),
            capabilities: result
                .get("capabilities")
                .cloned()
                .unwrap_or_else(|| json!({})),
            instructions: result
                .get("instructions")
                .and_then(Value::as_str)
                .map(str::to_string),
        };
        conn.set_protocol_version(&info.protocol_version);
        conn.notify("notifications/initialized", json!({})).await?;
        Ok(info)
    }

    /// Run one request on the live connection; drop the connection when it failed.
    async fn call(
        &self,
        method: &str,
        params: Value,
        cancel: Option<&CancellationToken>,
    ) -> Result<Value, McpError> {
        let conn = self.connection().await?;
        let result = conn.request(method, params, self.timeout(), cancel).await;
        if matches!(result, Err(McpError::Transport(_))) || !conn.is_alive() {
            self.drop_connection(&conn).await;
        }
        result
    }

    async fn drop_connection(&self, conn: &Arc<Connection>) {
        let mut guard = self.conn.lock().await;
        if guard.as_ref().is_some_and(|c| Arc::ptr_eq(c, conn)) {
            *guard = None;
            drop(guard);
            conn.close().await;
        }
    }

    /// All tools, following `nextCursor` pagination.
    pub async fn list_tools(&self) -> Result<Vec<Value>, McpError> {
        let mut tools = Vec::new();
        let mut cursor: Option<String> = None;
        for _ in 0..MAX_PAGES {
            let params = match &cursor {
                Some(cursor) => json!({"cursor": cursor}),
                None => json!({}),
            };
            let result = self.call("tools/list", params, None).await?;
            if let Some(page) = result.get("tools").and_then(Value::as_array) {
                tools.extend(page.iter().cloned());
            }
            match result
                .get("nextCursor")
                .and_then(Value::as_str)
                .filter(|c| !c.is_empty())
            {
                Some(next) if cursor.as_deref() != Some(next) => cursor = Some(next.to_string()),
                _ => break,
            }
        }
        Ok(tools)
    }

    /// `tools/call`; the raw `CallToolResult`.
    pub async fn call_tool(
        &self,
        name: &str,
        arguments: Value,
        cancel: Option<&CancellationToken>,
    ) -> Result<Value, McpError> {
        let arguments = if arguments.is_null() {
            json!({})
        } else {
            arguments
        };
        self.call(
            "tools/call",
            json!({"name": name, "arguments": arguments}),
            cancel,
        )
        .await
    }

    /// One page of `resources/list`; the raw result (`resources`, `nextCursor`).
    pub async fn list_resources(&self, cursor: Option<&str>) -> Result<Value, McpError> {
        let params = match cursor {
            Some(cursor) => json!({"cursor": cursor}),
            None => json!({}),
        };
        self.call("resources/list", params, None).await
    }

    /// `resources/read`; the raw result (`contents`).
    pub async fn read_resource(
        &self,
        uri: &str,
        cancel: Option<&CancellationToken>,
    ) -> Result<Value, McpError> {
        self.call("resources/read", json!({"uri": uri}), cancel)
            .await
    }

    /// Close the connection (terminating a stdio process).
    pub async fn disconnect(&self) {
        let conn = self.conn.lock().await.take();
        if let Some(conn) = conn {
            conn.close().await;
        }
    }

    /// Whether a live connection is currently held.
    pub async fn is_connected(&self) -> bool {
        self.conn
            .lock()
            .await
            .as_ref()
            .is_some_and(|c| c.is_alive())
    }
}
