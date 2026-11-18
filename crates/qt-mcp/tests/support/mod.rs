//! Minimal HTTP/1.1 server for the transport tests (one request per connection).

#![allow(dead_code)]

use std::future::Future;
use std::sync::Arc;

use serde_json::Value;
use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;

#[derive(Debug, Clone)]
pub struct HttpRequest {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl HttpRequest {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(name))
            .map(|(_, v)| v.as_str())
    }

    pub fn json(&self) -> Value {
        serde_json::from_slice(&self.body).unwrap_or(Value::Null)
    }
}

pub enum HttpResponse {
    Full {
        status: u16,
        headers: Vec<(String, String)>,
        body: String,
    },
    /// Body chunks are written as they arrive; the connection closes with the channel.
    Stream {
        headers: Vec<(String, String)>,
        chunks: mpsc::UnboundedReceiver<String>,
    },
}

impl HttpResponse {
    pub fn json(value: &Value) -> Self {
        Self::Full {
            status: 200,
            headers: vec![("content-type".into(), "application/json".into())],
            body: value.to_string(),
        }
    }

    pub fn status(status: u16, body: &str) -> Self {
        Self::Full {
            status,
            headers: Vec::new(),
            body: body.into(),
        }
    }

    pub fn with_header(mut self, name: &str, value: &str) -> Self {
        match &mut self {
            Self::Full { headers, .. } | Self::Stream { headers, .. } => {
                headers.push((name.into(), value.into()))
            }
        }
        self
    }

    /// A complete SSE body with one `message` event per value.
    pub fn sse(values: &[Value]) -> Self {
        let body = values
            .iter()
            .map(|v| format!("event: message\ndata: {v}\n\n"))
            .collect();
        Self::Full {
            status: 200,
            headers: vec![("content-type".into(), "text/event-stream".into())],
            body,
        }
    }
}

/// Serve `handler` on 127.0.0.1; returns `http://127.0.0.1:<port>`.
pub async fn serve<F, Fut>(handler: F) -> String
where
    F: Fn(HttpRequest) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = HttpResponse> + Send + 'static,
{
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    let handler = Arc::new(handler);
    tokio::spawn(async move {
        loop {
            let Ok((socket, _)) = listener.accept().await else {
                return;
            };
            let handler = handler.clone();
            tokio::spawn(async move {
                let mut socket = socket;
                if let Some(request) = read_request(&mut socket).await {
                    let response = handler(request).await;
                    write_response(&mut socket, response).await;
                }
            });
        }
    });
    format!("http://127.0.0.1:{port}")
}

async fn read_request(socket: &mut TcpStream) -> Option<HttpRequest> {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 4096];
    loop {
        let n = socket.read(&mut chunk).await.ok()?;
        if n == 0 {
            return None;
        }
        buf.extend_from_slice(&chunk[..n]);
        let Some(split) = buf.windows(4).position(|w| w == b"\r\n\r\n") else {
            continue;
        };
        let head = String::from_utf8_lossy(&buf[..split]).to_string();
        let mut lines = head.split("\r\n");
        let mut first = lines.next()?.split(' ');
        let method = first.next()?.to_string();
        let path = first.next()?.to_string();
        let headers: Vec<(String, String)> = lines
            .filter_map(|l| l.split_once(':'))
            .map(|(k, v)| (k.trim().to_ascii_lowercase(), v.trim().to_string()))
            .collect();
        let len = headers
            .iter()
            .find(|(k, _)| k == "content-length")
            .and_then(|(_, v)| v.parse::<usize>().ok())
            .unwrap_or(0);
        while buf.len() < split + 4 + len {
            let n = socket.read(&mut chunk).await.ok()?;
            if n == 0 {
                return None;
            }
            buf.extend_from_slice(&chunk[..n]);
        }
        return Some(HttpRequest {
            method,
            path,
            headers,
            body: buf[split + 4..split + 4 + len].to_vec(),
        });
    }
}

async fn write_response(socket: &mut TcpStream, response: HttpResponse) {
    match response {
        HttpResponse::Full {
            status,
            headers,
            body,
        } => {
            let mut head = format!(
                "HTTP/1.1 {status} X\r\nconnection: close\r\ncontent-length: {}\r\n",
                body.len()
            );
            for (k, v) in headers {
                head.push_str(&format!("{k}: {v}\r\n"));
            }
            head.push_str("\r\n");
            let _ = socket.write_all(head.as_bytes()).await;
            let _ = socket.write_all(body.as_bytes()).await;
        }
        HttpResponse::Stream {
            headers,
            mut chunks,
        } => {
            let mut head = "HTTP/1.1 200 OK\r\nconnection: close\r\n".to_string();
            for (k, v) in headers {
                head.push_str(&format!("{k}: {v}\r\n"));
            }
            head.push_str("\r\n");
            if socket.write_all(head.as_bytes()).await.is_err() {
                return;
            }
            while let Some(chunk) = chunks.recv().await {
                if socket.write_all(chunk.as_bytes()).await.is_err() {
                    return;
                }
                let _ = socket.flush().await;
            }
        }
    }
    let _ = socket.shutdown().await;
}
