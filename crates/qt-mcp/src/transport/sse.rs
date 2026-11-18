//! Legacy HTTP+SSE transport (MCP 2024-11-05 style).
//!
//! The client opens a GET event stream; the server announces the POST endpoint with an
//! `endpoint` event and then delivers every response and server request as `message`
//! events on that stream.

use std::sync::Arc;
use std::sync::atomic::AtomicI64;
use std::time::Duration;

use futures::StreamExt as _;
use qt_core::sse::SseParser;
use reqwest::header::{ACCEPT, CONTENT_TYPE, HeaderMap, HeaderValue};
use serde_json::Value;
use tokio::sync::oneshot;
use tokio::task::JoinHandle;

use super::{Incoming, Pending, Reply, ServerRequests, await_reply, classify, header_map};
use crate::config::McpServerConfig;
use crate::error::McpError;
use crate::util::{check_http_url, resolve_relative_url, sanitize_mcp_error};

pub(crate) struct SseConnection {
    pub server: String,
    pub next_id: AtomicI64,
    poster: Arc<Poster>,
    pending: Arc<Pending>,
    reader: JoinHandle<()>,
}

/// POSTs messages to the announced endpoint.
struct Poster {
    endpoint: String,
    headers: HeaderMap,
    timeout: Duration,
    http: reqwest::Client,
}

impl Poster {
    async fn post(&self, message: &Value) -> Result<(), McpError> {
        let mut headers = self.headers.clone();
        headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
        headers.insert(
            ACCEPT,
            HeaderValue::from_static("application/json, text/event-stream"),
        );
        let response = self
            .http
            .post(&self.endpoint)
            .headers(headers)
            .timeout(self.timeout)
            .body(message.to_string())
            .send()
            .await
            .map_err(|e| {
                McpError::Transport(sanitize_mcp_error(&format!("MCP HTTP request failed: {e}")))
            })?;
        let status = response.status();
        if !status.is_success() {
            let body = response.text().await.unwrap_or_default();
            return Err(McpError::Transport(sanitize_mcp_error(&format!(
                "MCP SSE POST returned status {}: {body}",
                status.as_u16()
            ))));
        }
        Ok(())
    }
}

impl SseConnection {
    pub async fn connect(
        config: &McpServerConfig,
        requests: ServerRequests,
        http: reqwest::Client,
    ) -> Result<Self, McpError> {
        let url = config
            .url
            .clone()
            .filter(|u| !u.is_empty())
            .ok_or_else(|| {
                McpError::Config(format!("MCP SSE server has no url: {}", config.name))
            })?;
        check_http_url(&url).map_err(|e| McpError::Config(sanitize_mcp_error(&e)))?;
        let mut headers = header_map(config)?;
        if !headers.contains_key("mcp-protocol-version") {
            headers.insert(
                "mcp-protocol-version",
                HeaderValue::from_static(crate::PROTOCOL_VERSION),
            );
        }
        let connect_timeout = Duration::from_secs(config.connect_timeout_seconds.max(1));
        let no_endpoint = || {
            McpError::Transport(format!(
                "MCP SSE server did not provide an endpoint event: {}",
                config.name
            ))
        };

        let mut stream_headers = headers.clone();
        stream_headers.insert(ACCEPT, HeaderValue::from_static("text/event-stream"));
        let open = http.get(&url).headers(stream_headers).send();
        let response = match tokio::time::timeout(connect_timeout, open).await {
            Ok(Ok(response)) => response,
            Ok(Err(e)) => {
                return Err(McpError::Transport(sanitize_mcp_error(&format!(
                    "MCP HTTP request failed: {e}"
                ))));
            }
            Err(_) => return Err(no_endpoint()),
        };
        if !response.status().is_success() {
            let status = response.status().as_u16();
            let body = response.text().await.unwrap_or_default();
            return Err(McpError::Transport(sanitize_mcp_error(&format!(
                "MCP HTTP server {} returned status {status}: {body}",
                config.name
            ))));
        }

        let pending = Arc::new(Pending::default());
        let (endpoint_tx, endpoint_rx) = oneshot::channel();
        let (poster_tx, poster_rx) = oneshot::channel::<Arc<Poster>>();
        let reader = tokio::spawn(read_stream(
            response,
            url.clone(),
            endpoint_tx,
            poster_rx,
            pending.clone(),
            requests,
        ));
        let endpoint = match tokio::time::timeout(connect_timeout, endpoint_rx).await {
            Ok(Ok(endpoint)) => endpoint,
            _ => {
                reader.abort();
                return Err(no_endpoint());
            }
        };
        let poster = Arc::new(Poster {
            endpoint,
            headers,
            timeout: Duration::from_secs(config.timeout_seconds.max(1)),
            http,
        });
        let _ = poster_tx.send(poster.clone());
        Ok(Self {
            server: config.name.clone(),
            next_id: AtomicI64::new(1),
            poster,
            pending,
            reader,
        })
    }

    pub async fn post(&self, message: &Value) -> Result<(), McpError> {
        self.poster.post(message).await
    }

    pub async fn request(
        &self,
        id: i64,
        message: &Value,
        method: &str,
        timeout: Duration,
    ) -> Reply {
        let rx = self.pending.register(id)?;
        if let Err(err) = self.poster.post(message).await {
            self.pending.remove(id);
            return Err(err);
        }
        await_reply(
            &self.pending,
            id,
            rx,
            timeout,
            format!("MCP SSE request timed out: {method} on {}", self.server),
        )
        .await
    }

    pub fn is_alive(&self) -> bool {
        !self.pending.is_closed() && !self.reader.is_finished()
    }

    pub fn close(&self) {
        self.reader.abort();
        self.pending.close(McpError::Transport(format!(
            "MCP SSE stream closed: {}",
            self.server
        )));
    }
}

impl Drop for SseConnection {
    fn drop(&mut self) {
        self.reader.abort();
    }
}

async fn read_stream(
    response: reqwest::Response,
    base_url: String,
    endpoint_tx: oneshot::Sender<String>,
    poster_rx: oneshot::Receiver<Arc<Poster>>,
    pending: Arc<Pending>,
    requests: ServerRequests,
) {
    let mut endpoint_tx = Some(endpoint_tx);
    let mut poster_rx = Some(poster_rx);
    let mut poster: Option<Arc<Poster>> = None;
    let mut parser = SseParser::new();
    let mut bytes = response.bytes_stream();
    loop {
        let chunk = bytes.next().await;
        let events = match &chunk {
            Some(Ok(chunk)) => parser.push(chunk),
            Some(Err(_)) => break,
            None => parser.finish(),
        };
        for event in events {
            let data = event.data.trim_end_matches(['\r', '\n']);
            if data.is_empty() {
                continue;
            }
            if event.event.as_deref() == Some("endpoint") {
                if let Some(tx) = endpoint_tx.take() {
                    let _ = tx.send(resolve_relative_url(&base_url, data));
                }
                continue;
            }
            let Ok(value) = serde_json::from_str::<Value>(data) else {
                continue;
            };
            match classify(value) {
                Incoming::Response { id, reply } => pending.resolve(id, reply),
                Incoming::Request { id, method, params } => {
                    if poster.is_none()
                        && let Some(rx) = poster_rx.take()
                    {
                        poster = rx.await.ok();
                    }
                    if let Some(poster) = poster.clone() {
                        let requests = requests.clone();
                        tokio::spawn(async move {
                            let response = requests.handle(id, &method, params).await;
                            let _ = poster.post(&response).await;
                        });
                    }
                }
                Incoming::Notification | Incoming::Invalid => {}
            }
        }
        if chunk.is_none() {
            break;
        }
    }
    pending.close(McpError::Transport(format!(
        "MCP SSE stream closed: {}",
        requests.server
    )));
}
