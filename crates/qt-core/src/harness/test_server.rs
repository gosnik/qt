//! A scripted OpenAI-compatible HTTP server on 127.0.0.1 for unit tests.

use std::sync::{Arc, Mutex};

use serde_json::{Value, json};
use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
use tokio::net::TcpListener;

pub(crate) enum Reply {
    Sse(String),
    Status(u16, String),
}

pub(crate) fn sse(chunks: &[Value]) -> String {
    let mut out: String = chunks.iter().map(|c| format!("data: {c}\n\n")).collect();
    out.push_str("data: [DONE]\n\n");
    out
}

pub(crate) fn text(text: &str) -> Reply {
    Reply::Sse(sse(&[
        json!({"choices":[{"delta":{"content":text}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}}),
    ]))
}

pub(crate) fn tool_calls(calls: &[(&str, &str, Value)]) -> Reply {
    let deltas: Vec<Value> = calls
        .iter()
        .enumerate()
        .map(|(i, (id, name, args))| {
            json!({"index": i, "id": id, "function": {"name": name, "arguments": args.to_string()}})
        })
        .collect();
    Reply::Sse(sse(&[
        json!({"choices":[{"delta":{"tool_calls": deltas}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}}),
    ]))
}

/// Serve one reply per request in order; returns the port and the recorded
/// request bodies.
pub(crate) async fn serve(replies: Vec<Reply>) -> (u16, Arc<Mutex<Vec<Value>>>) {
    let listener = TcpListener::bind("127.0.0.1:0").await.expect("bind");
    let port = listener.local_addr().expect("addr").port();
    let requests = Arc::new(Mutex::new(Vec::new()));
    let recorded = requests.clone();
    tokio::spawn(async move {
        for reply in replies {
            let Ok((mut socket, _)) = listener.accept().await else {
                return;
            };
            let mut buf = Vec::new();
            let mut chunk = [0u8; 8192];
            loop {
                let n = socket.read(&mut chunk).await.unwrap_or(0);
                buf.extend_from_slice(&chunk[..n]);
                let text = String::from_utf8_lossy(&buf).to_string();
                if let Some(split) = text.find("\r\n\r\n") {
                    let len = text[..split]
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase()
                                .strip_prefix("content-length:")
                                .and_then(|v| v.trim().parse::<usize>().ok())
                        })
                        .unwrap_or(0);
                    if buf.len() >= split + 4 + len {
                        if let Ok(v) = serde_json::from_slice(&buf[split + 4..split + 4 + len]) {
                            recorded.lock().expect("requests").push(v);
                        }
                        break;
                    }
                }
                if n == 0 {
                    break;
                }
            }
            let response = match reply {
                Reply::Sse(body) => format!(
                    "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{body}"
                ),
                Reply::Status(code, body) => format!(
                    "HTTP/1.1 {code} Error\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
                    body.len()
                ),
            };
            let _ = socket.write_all(response.as_bytes()).await;
            let _ = socket.shutdown().await;
        }
    });
    (port, requests)
}
