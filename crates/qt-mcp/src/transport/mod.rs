//! JSON-RPC plumbing shared by the stdio, streamable HTTP and legacy SSE transports.

mod http;
mod sse;
mod stdio;

use std::collections::HashMap;
use std::sync::Mutex;
use std::sync::atomic::{AtomicI64, Ordering};
use std::time::Duration;

use serde_json::{Value, json};
use tokio::sync::oneshot;
use tokio_util::sync::CancellationToken;

use crate::config::{McpServerConfig, TransportKind};
use crate::error::McpError;
use crate::sampling::{SamplingRequest, SamplingSlot, current_handler};
use crate::util::sanitize_mcp_error;

pub(crate) use http::HttpConnection;
pub(crate) use sse::SseConnection;
pub(crate) use stdio::StdioConnection;

pub(crate) type Reply = Result<Value, McpError>;

/// Requests awaiting a response, keyed by JSON-RPC id.
#[derive(Default)]
pub(crate) struct Pending {
    inner: Mutex<PendingInner>,
}

#[derive(Default)]
struct PendingInner {
    waiters: HashMap<i64, oneshot::Sender<Reply>>,
    closed: Option<McpError>,
}

impl Pending {
    fn lock(&self) -> std::sync::MutexGuard<'_, PendingInner> {
        self.inner.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn register(&self, id: i64) -> Result<oneshot::Receiver<Reply>, McpError> {
        let mut inner = self.lock();
        if let Some(err) = &inner.closed {
            return Err(err.clone());
        }
        let (tx, rx) = oneshot::channel();
        inner.waiters.insert(id, tx);
        Ok(rx)
    }

    pub fn resolve(&self, id: i64, reply: Reply) {
        if let Some(tx) = self.lock().waiters.remove(&id) {
            let _ = tx.send(reply);
        }
    }

    pub fn remove(&self, id: i64) {
        self.lock().waiters.remove(&id);
    }

    /// Fail every waiter and refuse new requests.
    pub fn close(&self, err: McpError) {
        let mut inner = self.lock();
        if inner.closed.is_none() {
            inner.closed = Some(err.clone());
        }
        for (_, tx) in inner.waiters.drain() {
            let _ = tx.send(Err(err.clone()));
        }
    }

    pub fn is_closed(&self) -> bool {
        self.lock().closed.is_some()
    }
}

/// Removes a pending id when the waiting future is dropped (timeout or cancel).
pub(crate) struct PendingGuard<'a> {
    pub pending: &'a Pending,
    pub id: i64,
}

impl Drop for PendingGuard<'_> {
    fn drop(&mut self) {
        self.pending.remove(self.id);
    }
}

/// Wait for a registered response with a timeout.
pub(crate) async fn await_reply(
    pending: &Pending,
    id: i64,
    rx: oneshot::Receiver<Reply>,
    timeout: Duration,
    timeout_message: String,
) -> Reply {
    let _guard = PendingGuard { pending, id };
    match tokio::time::timeout(timeout, rx).await {
        Ok(Ok(reply)) => reply,
        Ok(Err(_)) => Err(McpError::Transport("MCP connection closed".into())),
        Err(_) => Err(McpError::Timeout(timeout_message)),
    }
}

/// A decoded message from the server.
#[derive(Debug)]
pub(crate) enum Incoming {
    Response {
        id: i64,
        reply: Reply,
    },
    Request {
        id: Value,
        method: String,
        params: Value,
    },
    Notification,
    Invalid,
}

fn numeric_id(id: &Value) -> Option<i64> {
    id.as_i64()
        .or_else(|| id.as_str().and_then(|s| s.parse().ok()))
}

pub(crate) fn classify(message: Value) -> Incoming {
    let Value::Object(mut object) = message else {
        return Incoming::Invalid;
    };
    let method = object
        .get("method")
        .and_then(Value::as_str)
        .map(str::to_string);
    let id = object.remove("id").filter(|id| !id.is_null());
    match (method, id) {
        (Some(method), Some(id)) => Incoming::Request {
            id,
            method,
            params: object.remove("params").unwrap_or_else(|| json!({})),
        },
        (Some(_), None) => Incoming::Notification,
        (None, Some(id)) => {
            let Some(id) = numeric_id(&id) else {
                return Incoming::Invalid;
            };
            let reply = if let Some(error) = object.get("error") {
                Err(McpError::Rpc(sanitize_mcp_error(&error.to_string())))
            } else {
                Ok(object.remove("result").unwrap_or_else(|| json!({})))
            };
            Incoming::Response { id, reply }
        }
        (None, None) => Incoming::Invalid,
    }
}

/// Answers requests the server sends to the client.
#[derive(Clone)]
pub(crate) struct ServerRequests {
    pub server: String,
    pub sampling_config: Value,
    pub sampling_enabled: bool,
    pub sampling: SamplingSlot,
}

impl ServerRequests {
    pub fn new(config: &McpServerConfig, sampling: SamplingSlot) -> Self {
        Self {
            server: config.name.clone(),
            sampling_config: config.sampling.clone(),
            sampling_enabled: config.sampling_enabled(),
            sampling,
        }
    }

    /// Whether `initialize` should advertise the sampling capability.
    pub fn advertises_sampling(&self) -> bool {
        self.sampling_enabled && current_handler(&self.sampling).is_some()
    }

    /// Produce the JSON-RPC response for one server request.
    pub async fn handle(&self, id: Value, method: &str, params: Value) -> Value {
        let outcome: Result<Value, (i64, String)> = match method {
            "ping" => Ok(json!({})),
            "sampling/createMessage" => match current_handler(&self.sampling) {
                Some(handler) if self.sampling_enabled => handler
                    .create_message(SamplingRequest {
                        server_name: self.server.clone(),
                        params,
                        config: self.sampling_config.clone(),
                    })
                    .await
                    .map_err(|e| (-32000, sanitize_mcp_error(&e))),
                _ => Err((
                    -32601,
                    "MCP sampling/createMessage is not enabled for this server".into(),
                )),
            },
            other => Err((-32601, format!("Method not found: {other}"))),
        };
        match outcome {
            Ok(result) => json!({"jsonrpc": "2.0", "id": id, "result": result}),
            Err((code, message)) => {
                json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}})
            }
        }
    }
}

/// An open connection to one server.
pub(crate) enum Connection {
    Stdio(Box<StdioConnection>),
    Http(HttpConnection),
    Sse(SseConnection),
}

impl Connection {
    /// Open the transport (spawn the process, or connect the SSE stream).
    pub async fn open(
        config: &McpServerConfig,
        requests: ServerRequests,
        http: &reqwest::Client,
    ) -> Result<Self, McpError> {
        match config.transport_kind() {
            Some(TransportKind::Stdio) => Ok(Self::Stdio(Box::new(StdioConnection::spawn(
                config, requests,
            )?))),
            Some(TransportKind::StreamableHttp) => Ok(Self::Http(HttpConnection::new(
                config,
                requests,
                http.clone(),
            )?)),
            Some(TransportKind::Sse) => Ok(Self::Sse(
                SseConnection::connect(config, requests, http.clone()).await?,
            )),
            None => Err(McpError::Config(format!(
                "MCP transport is not implemented for server {}: {}",
                config.name, config.transport
            ))),
        }
    }

    fn ids(&self) -> &AtomicI64 {
        match self {
            Self::Stdio(c) => &c.next_id,
            Self::Http(c) => &c.next_id,
            Self::Sse(c) => &c.next_id,
        }
    }

    /// Send a request and wait for its result.
    pub async fn request(
        &self,
        method: &str,
        params: Value,
        timeout: Duration,
        cancel: Option<&CancellationToken>,
    ) -> Reply {
        let id = self.ids().fetch_add(1, Ordering::SeqCst);
        let message = json!({"jsonrpc": "2.0", "id": id, "method": method, "params": params});
        let exchange = async {
            match self {
                Self::Stdio(c) => c.request(id, &message, method, timeout).await,
                Self::Http(c) => c.request(id, &message, method, timeout).await,
                Self::Sse(c) => c.request(id, &message, method, timeout).await,
            }
        };
        let Some(cancel) = cancel else {
            return exchange.await;
        };
        tokio::select! {
            reply = exchange => reply,
            _ = cancel.cancelled() => {
                let _ = self
                    .notify("notifications/cancelled", json!({"requestId": id, "reason": "cancelled"}))
                    .await;
                Err(McpError::Cancelled(format!(
                    "MCP request cancelled: {method} on {}",
                    self.server()
                )))
            }
        }
    }

    /// Send a notification (no response expected).
    pub async fn notify(&self, method: &str, params: Value) -> Result<(), McpError> {
        let message = json!({"jsonrpc": "2.0", "method": method, "params": params});
        match self {
            Self::Stdio(c) => c.send(&message),
            Self::Http(c) => c.notify(&message).await,
            Self::Sse(c) => c.post(&message).await,
        }
    }

    /// Record the protocol version negotiated by `initialize`.
    pub fn set_protocol_version(&self, version: &str) {
        if let Self::Http(c) = self {
            c.set_protocol_version(version);
        }
    }

    pub fn is_alive(&self) -> bool {
        match self {
            Self::Stdio(c) => c.is_alive(),
            Self::Http(c) => c.is_alive(),
            Self::Sse(c) => c.is_alive(),
        }
    }

    fn server(&self) -> &str {
        match self {
            Self::Stdio(c) => &c.server,
            Self::Http(c) => &c.server,
            Self::Sse(c) => &c.server,
        }
    }

    /// Tear the connection down (terminate the process, end the HTTP session).
    pub async fn close(&self) {
        match self {
            Self::Stdio(c) => c.close().await,
            Self::Http(c) => c.close().await,
            Self::Sse(c) => c.close(),
        }
    }
}

/// Header map from configured headers plus the MCP defaults.
pub(crate) fn header_map(config: &McpServerConfig) -> Result<reqwest::header::HeaderMap, McpError> {
    use reqwest::header::{HeaderMap, HeaderName, HeaderValue};
    let mut headers = HeaderMap::new();
    for (name, value) in &config.headers {
        let name = HeaderName::from_bytes(name.as_bytes()).map_err(|_| {
            McpError::Config(format!(
                "invalid MCP header name for server {}: {name}",
                config.name
            ))
        })?;
        let value = HeaderValue::from_str(value).map_err(|_| {
            McpError::Config(format!(
                "invalid MCP header value for server {}: {name}",
                config.name
            ))
        })?;
        headers.insert(name, value);
    }
    Ok(headers)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, RwLock};

    #[test]
    fn classifies_messages() {
        match classify(json!({"jsonrpc": "2.0", "id": 3, "result": {"ok": true}})) {
            Incoming::Response {
                id: 3,
                reply: Ok(v),
            } => assert_eq!(v, json!({"ok": true})),
            other => panic!("{other:?}"),
        }
        match classify(json!({"id": "4", "error": {"code": -1, "message": "bad token=abc"}})) {
            Incoming::Response {
                id: 4,
                reply: Err(McpError::Rpc(m)),
            } => {
                assert_eq!(m, r#"{"code":-1,"message":"bad token=[REDACTED]"}"#)
            }
            other => panic!("{other:?}"),
        }
        match classify(json!({"id": 1})) {
            Incoming::Response { reply: Ok(v), .. } => assert_eq!(v, json!({})),
            other => panic!("{other:?}"),
        }
        assert!(matches!(
            classify(json!({"id": "s1", "method": "ping"})),
            Incoming::Request { method, .. } if method == "ping"
        ));
        assert!(matches!(
            classify(json!({"method": "notifications/progress"})),
            Incoming::Notification
        ));
        assert!(matches!(classify(json!([1])), Incoming::Invalid));
        assert!(matches!(
            classify(json!({"id": "x", "result": 1})),
            Incoming::Invalid
        ));
    }

    #[tokio::test]
    async fn pending_close_fails_waiters_and_new_requests() {
        let pending = Pending::default();
        let rx = pending.register(1).unwrap();
        pending.close(McpError::Transport("gone".into()));
        assert_eq!(rx.await.unwrap(), Err(McpError::Transport("gone".into())));
        assert!(pending.register(2).is_err());
        assert!(pending.is_closed());
    }

    #[tokio::test]
    async fn server_requests_without_sampling_handler() {
        let mut config = McpServerConfig::stdio("s", "x", vec![]);
        let slot: SamplingSlot = Arc::new(RwLock::new(None));
        let requests = ServerRequests::new(&config, slot.clone());
        assert!(!requests.advertises_sampling());
        let reply = requests
            .handle(json!(7), "sampling/createMessage", json!({}))
            .await;
        assert_eq!(
            reply,
            json!({"jsonrpc": "2.0", "id": 7, "error": {"code": -32601, "message": "MCP sampling/createMessage is not enabled for this server"}})
        );
        assert_eq!(
            requests.handle(json!("p"), "ping", json!({})).await,
            json!({"jsonrpc": "2.0", "id": "p", "result": {}})
        );
        assert_eq!(
            requests.handle(json!(1), "roots/list", json!({})).await["error"]["code"],
            -32601
        );
        config.sampling = json!({"enabled": false});
        let disabled = ServerRequests::new(&config, slot);
        assert!(!disabled.advertises_sampling());
    }
}
