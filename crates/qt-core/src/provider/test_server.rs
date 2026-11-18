//! Scripted local HTTP server for provider tests (127.0.0.1 only).

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
use tokio::net::TcpListener;

/// One canned response.
#[derive(Debug, Clone)]
pub struct MockResponse {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: String,
}

impl MockResponse {
    pub fn sse(body: impl Into<String>) -> Self {
        Self {
            status: 200,
            headers: vec![("content-type".into(), "text/event-stream".into())],
            body: body.into(),
        }
    }

    pub fn json(status: u16, body: impl Into<String>) -> Self {
        Self {
            status,
            headers: vec![("content-type".into(), "application/json".into())],
            body: body.into(),
        }
    }

    pub fn with_header(mut self, name: &str, value: &str) -> Self {
        self.headers.push((name.into(), value.into()));
        self
    }
}

/// A request as received by the server.
#[derive(Debug, Clone)]
pub struct Recorded {
    pub method: String,
    pub path: String,
    /// Lower-cased header names.
    pub headers: HashMap<String, String>,
    pub body: String,
}

impl Recorded {
    pub fn json(&self) -> serde_json::Value {
        serde_json::from_str(&self.body).unwrap_or(serde_json::Value::Null)
    }
}

pub type Requests = Arc<Mutex<Vec<Recorded>>>;

/// Serve `responses` in order, one per connection; returns the base URL.
pub async fn serve(responses: Vec<MockResponse>) -> (String, Requests) {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    let requests: Requests = Arc::default();
    let recorded = requests.clone();
    tokio::spawn(async move {
        for response in responses {
            let Ok((mut socket, _)) = listener.accept().await else {
                return;
            };
            let Some(request) = read_request(&mut socket).await else {
                continue;
            };
            recorded.lock().unwrap().push(request);
            let reason = reqwest::StatusCode::from_u16(response.status)
                .ok()
                .and_then(|s| s.canonical_reason())
                .unwrap_or("Status");
            let mut head = format!("HTTP/1.1 {} {reason}\r\n", response.status);
            for (k, v) in &response.headers {
                head.push_str(&format!("{k}: {v}\r\n"));
            }
            head.push_str(&format!(
                "content-length: {}\r\nconnection: close\r\n\r\n",
                response.body.len()
            ));
            let _ = socket.write_all(head.as_bytes()).await;
            let _ = socket.write_all(response.body.as_bytes()).await;
            let _ = socket.shutdown().await;
        }
    });
    (format!("http://127.0.0.1:{port}"), requests)
}

/// Read one HTTP/1.1 request (Content-Length bodies only).
pub async fn read_request(socket: &mut tokio::net::TcpStream) -> Option<Recorded> {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 8192];
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
        let mut lines = head.lines();
        let mut first = lines.next()?.split_whitespace();
        let method = first.next()?.to_string();
        let path = first.next()?.to_string();
        let headers: HashMap<String, String> = lines
            .filter_map(|l| l.split_once(':'))
            .map(|(k, v)| (k.trim().to_ascii_lowercase(), v.trim().to_string()))
            .collect();
        let len: usize = headers
            .get("content-length")
            .and_then(|v| v.parse().ok())
            .unwrap_or(0);
        while buf.len() < split + 4 + len {
            let n = socket.read(&mut chunk).await.ok()?;
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
        }
        let body_end = (split + 4 + len).min(buf.len());
        let body = String::from_utf8_lossy(&buf[split + 4..body_end]).to_string();
        return Some(Recorded {
            method,
            path,
            headers,
            body,
        });
    }
}

/// `data: <json>\n\n` lines for each value.
pub fn sse_data(values: &[serde_json::Value]) -> String {
    values.iter().map(|v| format!("data: {v}\n\n")).collect()
}
