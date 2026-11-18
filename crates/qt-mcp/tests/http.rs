//! Streamable HTTP and legacy SSE transports against local mock servers.

mod support;

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures::future::BoxFuture;
use qt_core::tools::{ToolContext, ToolRegistry};
use qt_mcp::{
    McpManager, McpOptions, McpServerConfig, SamplingHandler, SamplingRequest, ServerStatus,
};
use serde_json::{Value, json};
use support::{HttpRequest, HttpResponse, serve};
use tokio::sync::{mpsc, oneshot};

fn fast() -> McpOptions {
    McpOptions {
        attempts: 3,
        base_delay: Duration::from_millis(10),
    }
}

fn ctx() -> ToolContext {
    ToolContext::new(std::env::temp_dir())
}

fn text(value: &str) -> Value {
    json!({"content": [{"type": "text", "text": value}]})
}

fn response(id: &Value, result: Value) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "result": result})
}

struct Pong;

impl SamplingHandler for Pong {
    fn create_message(
        &self,
        request: SamplingRequest,
    ) -> BoxFuture<'static, Result<Value, String>> {
        Box::pin(async move {
            Ok(
                json!({"role": "assistant", "content": {"type": "text", "text": format!("pong from {}", request.server_name)}, "model": "m", "stopReason": "endTurn"}),
            )
        })
    }
}

// ---------------------------------------------------------------------------
// Streamable HTTP

#[derive(Default)]
struct Streamable {
    log: Mutex<Vec<HttpRequest>>,
    valid_session: Mutex<String>,
    issued: AtomicUsize,
    sampling_reply: Mutex<Option<oneshot::Sender<Value>>>,
}

impl Streamable {
    fn requests(&self) -> Vec<HttpRequest> {
        self.log.lock().unwrap().clone()
    }
}

async fn streamable(state: Arc<Streamable>, req: HttpRequest) -> HttpResponse {
    state.log.lock().unwrap().push(req.clone());
    if req.path == "/broken" {
        return HttpResponse::status(500, "boom token=s3cret");
    }
    if req.method == "DELETE" {
        return HttpResponse::status(200, "");
    }
    let body = req.json();
    let Some(method) = body["method"].as_str() else {
        // A client response to our sampling request.
        if body["id"] == 99
            && let Some(tx) = state.sampling_reply.lock().unwrap().take()
        {
            let _ = tx.send(body.clone());
        }
        return HttpResponse::status(202, "");
    };
    let id = body["id"].clone();
    if method == "initialize" {
        let n = state.issued.fetch_add(1, Ordering::SeqCst) + 1;
        let session = format!("sess-{n}");
        *state.valid_session.lock().unwrap() = session.clone();
        return HttpResponse::json(&response(
            &id,
            json!({
                "protocolVersion": "2025-03-26",
                "capabilities": {"tools": {}, "resources": {}},
                "serverInfo": {"name": "http-mock", "version": "2"}
            }),
        ))
        .with_header("Mcp-Session-Id", &session);
    }
    let valid = state.valid_session.lock().unwrap().clone();
    match req.header("mcp-session-id") {
        Some(sid) if sid == valid => {}
        Some(_) => return HttpResponse::status(404, "unknown session"),
        None => return HttpResponse::status(400, "missing session"),
    }
    if id.is_null() {
        return HttpResponse::status(202, "");
    }
    let params = &body["params"];
    match method {
        "tools/list" if params["cursor"] == "2" => HttpResponse::sse(&[response(
            &id,
            json!({"tools": [
                {"name": "sample", "inputSchema": {"type": "object"}},
                {"name": "expire", "inputSchema": {"type": "object"}}
            ]}),
        )]),
        "tools/list" => HttpResponse::json(&response(
            &id,
            json!({"tools": [{"name": "echo", "description": "Echo", "inputSchema": {"type": "object"}}], "nextCursor": "2"}),
        )),
        "tools/call" => match params["name"].as_str().unwrap_or_default() {
            "echo" => HttpResponse::sse(&[
                json!({"jsonrpc": "2.0", "method": "notifications/progress", "params": {"progress": 1}}),
                json!({"jsonrpc": "2.0", "id": 12345, "result": text("not mine")}),
                response(
                    &id,
                    text(&format!(
                        "echo: {}",
                        params["arguments"]["text"].as_str().unwrap_or_default()
                    )),
                ),
            ]),
            "expire" => {
                state.valid_session.lock().unwrap().clear();
                HttpResponse::json(&response(&id, text("expired")))
            }
            "sample" => {
                let (tx, chunks) = mpsc::unbounded_channel();
                let (reply_tx, reply_rx) = oneshot::channel();
                *state.sampling_reply.lock().unwrap() = Some(reply_tx);
                let request = json!({"jsonrpc": "2.0", "id": 99, "method": "sampling/createMessage", "params": {"messages": [], "maxTokens": 5}});
                tx.send(format!("event: message\ndata: {request}\n\n"))
                    .unwrap();
                tokio::spawn(async move {
                    let reply = reply_rx.await.unwrap_or(Value::Null);
                    let sampled = reply["result"]["content"]["text"]
                        .as_str()
                        .unwrap_or("none")
                        .to_string();
                    let done = response(&id, text(&format!("sampled: {sampled}")));
                    let _ = tx.send(format!("event: message\ndata: {done}\n\n"));
                });
                HttpResponse::Stream {
                    headers: vec![("content-type".into(), "text/event-stream".into())],
                    chunks,
                }
            }
            _ => HttpResponse::json(
                &json!({"jsonrpc": "2.0", "id": id, "error": {"code": -32602, "message": "unknown tool"}}),
            ),
        },
        "resources/list" if params["cursor"] == "r2" => HttpResponse::json(&response(
            &id,
            json!({"resources": [{"uri": "http://doc2", "name": "doc2"}]}),
        )),
        "resources/list" => HttpResponse::json(&response(
            &id,
            json!({"resources": [{"uri": "http://doc", "name": "doc"}], "nextCursor": "r2"}),
        )),
        "resources/read" => HttpResponse::json(&response(
            &id,
            json!({"contents": [{"uri": params["uri"], "text": "doc text"}]}),
        )),
        _ => HttpResponse::json(
            &json!({"jsonrpc": "2.0", "id": id, "error": {"code": -32601, "message": "nope"}}),
        ),
    }
}

async fn streamable_server() -> (Arc<Streamable>, String) {
    let state = Arc::new(Streamable::default());
    let handler_state = state.clone();
    let base = serve(move |req| streamable(handler_state.clone(), req)).await;
    (state, base)
}

#[tokio::test]
async fn streamable_http_session_lifecycle() {
    let (state, base) = streamable_server().await;
    let mut config = McpServerConfig::remote("remote", format!("{base}/mcp"), "http");
    config
        .headers
        .insert("Authorization".into(), "Bearer t0k".into());
    let manager = McpManager::with_options(vec![config], fast());
    manager.set_sampling_handler(Some(Arc::new(Pong)));
    assert!(manager.connect_all().await.is_empty());
    let names: Vec<_> = manager
        .tools()
        .into_iter()
        .map(|t| t.registered_name)
        .collect();
    assert_eq!(
        names,
        ["mcp_remote_echo", "mcp_remote_sample", "mcp_remote_expire"]
    );
    let snapshot = &manager.snapshots()[0];
    assert_eq!(snapshot.status, ServerStatus::Connected);
    assert_eq!(
        snapshot.info.as_ref().unwrap().server_info["name"],
        "http-mock"
    );
    assert_eq!(snapshot.target, format!("{base}/mcp"));

    let requests = state.requests();
    let init = &requests[0];
    assert_eq!(init.method, "POST");
    assert_eq!(init.path, "/mcp");
    assert_eq!(
        init.header("accept"),
        Some("application/json, text/event-stream")
    );
    assert_eq!(init.header("content-type"), Some("application/json"));
    assert_eq!(init.header("mcp-protocol-version"), Some("2025-03-26"));
    assert_eq!(init.header("authorization"), Some("Bearer t0k"));
    assert_eq!(init.header("mcp-session-id"), None);
    assert_eq!(
        init.json()["params"]["capabilities"],
        json!({"sampling": {}})
    );
    assert_eq!(init.json()["params"]["clientInfo"]["name"], "qt");
    let initialized = &requests[1];
    assert_eq!(initialized.json()["method"], "notifications/initialized");
    assert_eq!(initialized.header("mcp-session-id"), Some("sess-1"));
    assert_eq!(requests[3].json()["params"]["cursor"], "2");

    // SSE response carrying a notification and an unrelated id before the answer.
    let result = manager
        .call_tool("remote", "echo", json!({"text": "hi"}), None)
        .await
        .unwrap();
    assert_eq!(result["content"][0]["text"], "echo: hi");

    // Sampling request delivered on the response stream, answered with a POST.
    let result = manager
        .call_tool("remote", "sample", json!({}), None)
        .await
        .unwrap();
    assert_eq!(result["content"][0]["text"], "sampled: pong from remote");

    // An expired session (404) triggers a fresh initialize.
    manager
        .call_tool("remote", "expire", json!({}), None)
        .await
        .unwrap();
    let result = manager
        .call_tool("remote", "echo", json!({"text": "again"}), None)
        .await
        .unwrap();
    assert_eq!(result["content"][0]["text"], "echo: again");
    assert_eq!(state.issued.load(Ordering::SeqCst), 2);

    // Resource pagination through the tool.
    let registry = manager
        .resource_tools()
        .into_iter()
        .fold(ToolRegistry::empty(), |r, t| r.with(t));
    let out = registry
        .execute("list_mcp_resources", json!({"server": "remote"}), ctx())
        .await;
    assert!(out.text_content().contains("\"uri\": \"http://doc\""));
    assert!(
        out.text_content()
            .ends_with("More resources available. Call list_mcp_resources again with cursor: r2")
    );
    let out = registry
        .execute(
            "list_mcp_resources",
            json!({"server": "remote", "cursor": "r2"}),
            ctx(),
        )
        .await;
    assert!(out.text_content().contains("http://doc2"));
    let out = registry
        .execute("list_mcp_resources", json!({"cursor": "r2"}), ctx())
        .await;
    assert_eq!(
        out.text_content(),
        "list_mcp_resources cursor requires a server"
    );
    let out = registry
        .execute(
            "read_mcp_resource",
            json!({"server": "remote", "uri": "http://doc"}),
            ctx(),
        )
        .await;
    assert_eq!(out.text_content(), "doc text");

    manager.shutdown().await;
    let last = state.requests().last().cloned().unwrap();
    assert_eq!(last.method, "DELETE");
    assert_eq!(last.header("mcp-session-id"), Some("sess-2"));
}

#[tokio::test]
async fn streamable_http_errors_and_explicit_version() {
    let (state, base) = streamable_server().await;
    let broken = McpServerConfig::remote("broken", format!("{base}/broken"), "streamable-http");
    let bad_url = McpServerConfig::remote("ftp", "ftp://example", "http");
    let mut pinned = McpServerConfig::remote("pinned", format!("{base}/mcp"), "http");
    pinned
        .headers
        .insert("MCP-Protocol-Version".into(), "2024-11-05".into());
    let manager = McpManager::with_options(vec![broken, bad_url, pinned], fast());
    let diagnostics = manager.connect_all().await;
    assert_eq!(
        diagnostics,
        [
            "MCP tool discovery failed for broken: MCP HTTP server broken returned status 500: boom token=[REDACTED]",
            "MCP tool discovery failed for ftp: MCP URL must use http or https: ftp://example",
        ]
    );
    let broken_attempts = state
        .requests()
        .iter()
        .filter(|r| r.path == "/broken")
        .count();
    assert_eq!(broken_attempts, 3);
    let pinned_requests: Vec<_> = state
        .requests()
        .into_iter()
        .filter(|r| r.path == "/mcp")
        .collect();
    assert!(!pinned_requests.is_empty());
    assert!(
        pinned_requests
            .iter()
            .all(|r| r.header("mcp-protocol-version") == Some("2024-11-05"))
    );
    // No sampling handler: the capability is not advertised.
    assert_eq!(
        pinned_requests[0].json()["params"]["capabilities"],
        json!({})
    );
    manager.shutdown().await;
}

// ---------------------------------------------------------------------------
// Legacy SSE

#[derive(Default)]
struct Legacy {
    log: Mutex<Vec<HttpRequest>>,
    stream: Mutex<Option<mpsc::UnboundedSender<String>>>,
    sampling_call: Mutex<Option<Value>>,
    streams_opened: AtomicUsize,
}

impl Legacy {
    fn push(&self, message: &Value) {
        if let Some(tx) = self.stream.lock().unwrap().as_ref() {
            let _ = tx.send(format!("event: message\ndata: {message}\n\n"));
        }
    }
}

async fn legacy(state: Arc<Legacy>, req: HttpRequest) -> HttpResponse {
    state.log.lock().unwrap().push(req.clone());
    if req.method == "GET" {
        let (tx, chunks) = mpsc::unbounded_channel();
        if req.path == "/sse" {
            state.streams_opened.fetch_add(1, Ordering::SeqCst);
            tx.send(": keep-alive\n\nevent: endpoint\ndata: /messages?sessionId=abc\n\n".into())
                .unwrap();
            *state.stream.lock().unwrap() = Some(tx);
        } else {
            // Never announces an endpoint.
            std::mem::forget(tx);
        }
        return HttpResponse::Stream {
            headers: vec![("content-type".into(), "text/event-stream".into())],
            chunks,
        };
    }
    let body = req.json();
    let id = body["id"].clone();
    let Some(method) = body["method"].as_str() else {
        if id == "srv-1"
            && let Some(call_id) = state.sampling_call.lock().unwrap().take()
        {
            let sampled = body["result"]["content"]["text"].as_str().unwrap_or("none");
            state.push(&response(&call_id, text(&format!("sampled: {sampled}"))));
        }
        return HttpResponse::status(202, "Accepted");
    };
    if id.is_null() {
        return HttpResponse::status(202, "Accepted");
    }
    let params = &body["params"];
    let reply = match method {
        "initialize" => Some(response(
            &id,
            json!({"protocolVersion": "2024-11-05", "capabilities": {"tools": {}, "resources": {}}, "serverInfo": {"name": "legacy"}}),
        )),
        "tools/list" => Some(response(
            &id,
            json!({"tools": [{"name": "echo", "inputSchema": {"type": "object"}}, {"name": "sample"}]}),
        )),
        "tools/call" if params["name"] == "sample" => {
            *state.sampling_call.lock().unwrap() = Some(id.clone());
            state.push(&json!({"jsonrpc": "2.0", "id": "srv-1", "method": "sampling/createMessage", "params": {"messages": []}}));
            None
        }
        "tools/call" => Some(response(
            &id,
            text(&format!(
                "legacy echo: {}",
                params["arguments"]["text"].as_str().unwrap_or_default()
            )),
        )),
        "resources/list" => Some(response(&id, json!({"resources": [{"uri": "legacy://a"}]}))),
        "resources/read" => Some(response(
            &id,
            json!({"contents": [{"uri": "legacy://a", "text": "legacy text"}]}),
        )),
        _ => {
            Some(json!({"jsonrpc": "2.0", "id": id, "error": {"code": -32601, "message": "nope"}}))
        }
    };
    if let Some(reply) = reply {
        state.push(&reply);
    }
    HttpResponse::status(202, "Accepted")
}

#[tokio::test]
async fn legacy_sse_transport() {
    let state = Arc::new(Legacy::default());
    let handler_state = state.clone();
    let base = serve(move |req| legacy(handler_state.clone(), req)).await;
    let mut config = McpServerConfig::remote("old", format!("{base}/sse"), "sse");
    config.headers.insert("X-Api".into(), "v".into());
    let mut silent = McpServerConfig::remote("silent", format!("{base}/silent"), "sse");
    silent.connect_timeout_seconds = 1;
    let manager = McpManager::with_options(
        vec![config, silent],
        McpOptions {
            attempts: 1,
            base_delay: Duration::from_millis(10),
        },
    );
    manager.set_sampling_handler(Some(Arc::new(Pong)));
    let diagnostics = manager.connect_all().await;
    assert_eq!(
        diagnostics,
        [
            "MCP tool discovery failed for silent: MCP SSE server did not provide an endpoint event: silent"
        ]
    );
    let names: Vec<_> = manager
        .tools()
        .into_iter()
        .map(|t| t.registered_name)
        .collect();
    assert_eq!(names, ["mcp_old_echo", "mcp_old_sample"]);
    assert_eq!(
        manager.snapshots()[0]
            .info
            .as_ref()
            .unwrap()
            .protocol_version,
        "2024-11-05"
    );

    let requests = state.log.lock().unwrap().clone();
    let get = requests.iter().find(|r| r.path == "/sse").unwrap();
    assert_eq!(get.header("accept"), Some("text/event-stream"));
    assert_eq!(get.header("mcp-protocol-version"), Some("2025-03-26"));
    assert_eq!(get.header("x-api"), Some("v"));
    let post = requests.iter().find(|r| r.method == "POST").unwrap();
    assert_eq!(post.path, "/messages?sessionId=abc");
    assert_eq!(post.header("x-api"), Some("v"));

    let registry = manager.register_tools(ToolRegistry::empty(), None, &mut Vec::new());
    let out = registry
        .execute("mcp_old_echo", json!({"text": "yo"}), ctx())
        .await;
    assert_eq!(out.text_content(), "legacy echo: yo");
    let out = registry.execute("mcp_old_sample", json!({}), ctx()).await;
    assert_eq!(out.text_content(), "sampled: pong from old");
    let read = manager
        .read_resource("old", "legacy://a", None)
        .await
        .unwrap();
    assert_eq!(read["contents"][0]["text"], "legacy text");

    // The server drops the event stream: the next call reconnects.
    state.stream.lock().unwrap().take();
    tokio::time::sleep(Duration::from_millis(100)).await;
    let out = registry
        .execute("mcp_old_echo", json!({"text": "back"}), ctx())
        .await;
    assert_eq!(out.text_content(), "legacy echo: back");
    assert_eq!(state.streams_opened.load(Ordering::SeqCst), 2);
    manager.shutdown().await;
}
