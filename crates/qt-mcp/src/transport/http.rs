//! Streamable HTTP transport (MCP 2025-03-26).
//!
//! Every message is POSTed to the server URL with
//! `accept: application/json, text/event-stream`, the `mcp-protocol-version` header
//! (unless configured explicitly) and, once the server assigned one, `mcp-session-id`.
//! A request's answer is either a JSON body or an SSE stream; the stream may carry
//! server requests (for example sampling) before the response, which are answered with
//! further POSTs.

use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures::StreamExt as _;
use qt_core::sse::SseParser;
use reqwest::header::{ACCEPT, CONTENT_TYPE, HeaderMap, HeaderValue};
use serde_json::Value;

use super::{Incoming, Reply, ServerRequests, classify, header_map};
use crate::config::McpServerConfig;
use crate::error::McpError;
use crate::util::{check_http_url, sanitize_mcp_error};

const SESSION_HEADER: &str = "mcp-session-id";
const VERSION_HEADER: &str = "mcp-protocol-version";

pub(crate) struct HttpConnection {
    pub server: String,
    pub next_id: AtomicI64,
    inner: Arc<HttpInner>,
}

struct HttpInner {
    server: String,
    url: String,
    headers: HeaderMap,
    explicit_version: bool,
    timeout: Duration,
    http: reqwest::Client,
    requests: ServerRequests,
    session_id: Mutex<Option<String>>,
    protocol_version: Mutex<String>,
    alive: AtomicBool,
}

impl HttpConnection {
    pub fn new(
        config: &McpServerConfig,
        requests: ServerRequests,
        http: reqwest::Client,
    ) -> Result<Self, McpError> {
        let url = config
            .url
            .clone()
            .filter(|u| !u.is_empty())
            .ok_or_else(|| {
                McpError::Config(format!("MCP HTTP server has no url: {}", config.name))
            })?;
        check_http_url(&url).map_err(|e| McpError::Config(sanitize_mcp_error(&e)))?;
        let headers = header_map(config)?;
        let explicit_version = headers.contains_key(VERSION_HEADER);
        Ok(Self {
            server: config.name.clone(),
            next_id: AtomicI64::new(1),
            inner: Arc::new(HttpInner {
                server: config.name.clone(),
                url,
                headers,
                explicit_version,
                timeout: Duration::from_secs(config.timeout_seconds.max(1)),
                http,
                requests,
                session_id: Mutex::new(None),
                protocol_version: Mutex::new(crate::PROTOCOL_VERSION.into()),
                alive: AtomicBool::new(true),
            }),
        })
    }

    pub fn set_protocol_version(&self, version: &str) {
        *self
            .inner
            .protocol_version
            .lock()
            .unwrap_or_else(|e| e.into_inner()) = version.to_string();
    }

    pub fn is_alive(&self) -> bool {
        self.inner.alive.load(Ordering::SeqCst)
    }

    pub async fn request(
        &self,
        id: i64,
        message: &Value,
        method: &str,
        timeout: Duration,
    ) -> Reply {
        let inner = self.inner.clone();
        match tokio::time::timeout(timeout, inner.post(message, Some(id))).await {
            Ok(Ok(Some(reply))) => reply,
            Ok(Ok(None)) => Err(McpError::Protocol(
                "MCP SSE response did not contain a JSON data event".into(),
            )),
            Ok(Err(err)) => Err(err),
            Err(_) => Err(McpError::Timeout(format!(
                "MCP request timed out: {method} on {}",
                self.server
            ))),
        }
    }

    pub async fn notify(&self, message: &Value) -> Result<(), McpError> {
        let inner = self.inner.clone();
        let timeout = inner.timeout;
        match tokio::time::timeout(timeout, inner.post(message, None)).await {
            Ok(result) => result.map(|_| ()),
            Err(_) => Err(McpError::Timeout(format!(
                "MCP request timed out: notification on {}",
                self.server
            ))),
        }
    }

    /// End the HTTP session with `DELETE` (best effort).
    pub async fn close(&self) {
        self.inner.alive.store(false, Ordering::SeqCst);
        let session = self
            .inner
            .session_id
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .take();
        if let Some(session) = session {
            let request = self
                .inner
                .http
                .delete(&self.inner.url)
                .headers(self.inner.base_headers())
                .header(SESSION_HEADER, session)
                .timeout(Duration::from_secs(5));
            let _ = request.send().await;
        }
    }
}

impl HttpInner {
    fn base_headers(&self) -> HeaderMap {
        let mut headers = self.headers.clone();
        if !self.explicit_version {
            let version = self
                .protocol_version
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .clone();
            if let Ok(value) = HeaderValue::from_str(&version) {
                headers.insert(VERSION_HEADER, value);
            }
        }
        headers
    }

    /// POST one message. With `expect` set, returns the matching response.
    async fn post(
        self: Arc<Self>,
        message: &Value,
        expect: Option<i64>,
    ) -> Result<Option<Reply>, McpError> {
        let mut headers = self.base_headers();
        headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
        headers.insert(
            ACCEPT,
            HeaderValue::from_static("application/json, text/event-stream"),
        );
        let session = self
            .session_id
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone();
        if let Some(session) = &session
            && let Ok(value) = HeaderValue::from_str(session)
        {
            headers.insert(SESSION_HEADER, value);
        }
        let response = self
            .http
            .post(&self.url)
            .headers(headers)
            .body(message.to_string())
            .send()
            .await
            .map_err(|e| {
                McpError::Transport(sanitize_mcp_error(&format!("MCP HTTP request failed: {e}")))
            })?;
        for name in [SESSION_HEADER, "mcp-sessionid"] {
            if let Some(value) = response.headers().get(name).and_then(|v| v.to_str().ok()) {
                *self.session_id.lock().unwrap_or_else(|e| e.into_inner()) =
                    Some(value.to_string());
            }
        }
        let status = response.status();
        if !status.is_success() {
            if status.as_u16() == 404 && session.is_some() {
                // The session expired: start over with a new `initialize`.
                *self.session_id.lock().unwrap_or_else(|e| e.into_inner()) = None;
                self.alive.store(false, Ordering::SeqCst);
            }
            let body = response.text().await.unwrap_or_default();
            return Err(McpError::Transport(sanitize_mcp_error(&format!(
                "MCP HTTP server {} returned status {}: {body}",
                self.server,
                status.as_u16()
            ))));
        }
        let Some(expect) = expect else {
            return Ok(None);
        };
        let is_sse = response
            .headers()
            .get(CONTENT_TYPE)
            .and_then(|v| v.to_str().ok())
            .is_some_and(|v| v.to_ascii_lowercase().contains("text/event-stream"));
        if is_sse {
            return self.read_event_stream(response, expect).await;
        }
        let body = response.bytes().await.map_err(|e| {
            McpError::Transport(sanitize_mcp_error(&format!("MCP HTTP request failed: {e}")))
        })?;
        let text = String::from_utf8_lossy(&body);
        if text.starts_with("data:") || text.contains("\ndata:") {
            let mut parser = SseParser::new();
            let mut events = parser.push(text.as_bytes());
            events.extend(parser.finish());
            for event in events {
                if let Some(reply) = self.clone().handle_payload(&event.data, expect) {
                    return Ok(Some(reply));
                }
            }
            return Ok(None);
        }
        let value: Value = serde_json::from_str(&text).map_err(|e| {
            McpError::Protocol(sanitize_mcp_error(&format!(
                "invalid MCP HTTP JSON response: {e}"
            )))
        })?;
        Ok(self.handle_value(value, expect))
    }

    async fn read_event_stream(
        self: Arc<Self>,
        response: reqwest::Response,
        expect: i64,
    ) -> Result<Option<Reply>, McpError> {
        let mut parser = SseParser::new();
        let mut bytes = response.bytes_stream();
        loop {
            let chunk = bytes.next().await;
            let events = match &chunk {
                Some(Ok(chunk)) => parser.push(chunk),
                Some(Err(e)) => {
                    return Err(McpError::Transport(sanitize_mcp_error(&format!(
                        "MCP HTTP request failed: {e}"
                    ))));
                }
                None => parser.finish(),
            };
            for event in events {
                if let Some(reply) = self.clone().handle_payload(&event.data, expect) {
                    return Ok(Some(reply));
                }
            }
            if chunk.is_none() {
                return Ok(None);
            }
        }
    }

    fn handle_payload(self: Arc<Self>, data: &str, expect: i64) -> Option<Reply> {
        let data = data.trim_end_matches(['\r', '\n']);
        if data.is_empty() {
            return None;
        }
        let value: Value = serde_json::from_str(data).ok()?;
        self.handle_value(value, expect)
    }

    /// Dispatch one decoded message (or batch); return the response to `expect`.
    fn handle_value(self: Arc<Self>, value: Value, expect: i64) -> Option<Reply> {
        let messages = match value {
            Value::Array(items) => items,
            other => vec![other],
        };
        let mut found = None;
        for message in messages {
            match classify(message) {
                Incoming::Response { id, reply } if id == expect => found = Some(reply),
                Incoming::Request { id, method, params } => {
                    let this = self.clone();
                    tokio::spawn(async move {
                        let response = this.requests.handle(id, &method, params).await;
                        let _ = this.clone().post(&response, None).await;
                    });
                }
                _ => {}
            }
        }
        found
    }
}
