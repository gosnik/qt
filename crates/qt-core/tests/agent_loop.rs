//! End-to-end agent loop against a local OpenAI-compatible mock server.

use std::sync::{Arc, Mutex};

use qt_core::message::{ContentBlock, Message, StopReason};
use qt_core::provider::ModelRef;
use qt_core::{AgentEvent, AgentHome, Engine};
use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
use tokio::net::TcpListener;

/// Serve one canned SSE body per request, recording each request body.
async fn mock_server(responses: Vec<String>, requests: Arc<Mutex<Vec<serde_json::Value>>>) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    let port = listener.local_addr().unwrap().port();
    tokio::spawn(async move {
        for body in responses {
            let (mut socket, _) = listener.accept().await.unwrap();
            let mut buf = Vec::new();
            let mut chunk = [0u8; 4096];
            loop {
                let n = socket.read(&mut chunk).await.unwrap();
                buf.extend_from_slice(&chunk[..n]);
                let text = String::from_utf8_lossy(&buf).to_string();
                if let Some(split) = text.find("\r\n\r\n") {
                    let len = text[..split]
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase()
                                .strip_prefix("content-length:")
                                .map(|v| v.trim().parse::<usize>().unwrap())
                        })
                        .unwrap_or(0);
                    if buf.len() >= split + 4 + len {
                        let json = &buf[split + 4..split + 4 + len];
                        requests
                            .lock()
                            .unwrap()
                            .push(serde_json::from_slice(json).unwrap());
                        break;
                    }
                }
                if n == 0 {
                    break;
                }
            }
            let response = format!(
                "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{body}"
            );
            socket.write_all(response.as_bytes()).await.unwrap();
            socket.shutdown().await.unwrap();
        }
    });
    port
}

fn sse(chunks: &[serde_json::Value]) -> String {
    let mut out: String = chunks.iter().map(|c| format!("data: {c}\n\n")).collect();
    out.push_str("data: [DONE]\n\n");
    out
}

#[test]
fn runs_tool_calls_and_persists_session() {
    let dir = tempfile::tempdir().unwrap();
    let workspace = dir.path().join("work");
    std::fs::create_dir_all(&workspace).unwrap();
    let home = AgentHome::new(dir.path().join("home"));

    let requests = Arc::new(Mutex::new(Vec::new()));
    let first = sse(&[
        serde_json::json!({"choices":[{"delta":{"content":"Writing it."}}]}),
        serde_json::json!({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"write","arguments":"{\"path\":\"out.txt\",\"content\":\"made by qt\"}"}}]}}]}),
        serde_json::json!({"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":100,"completion_tokens":20,"total_tokens":120}}),
    ]);
    let second = sse(&[
        serde_json::json!({"choices":[{"delta":{"content":"Done."}}]}),
        serde_json::json!({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":150,"completion_tokens":5,"total_tokens":155}}),
    ]);

    // The engine owns its runtime; the mock server runs on a separate one.
    let server_rt = tokio::runtime::Runtime::new().unwrap();
    let port = server_rt.block_on(mock_server(vec![first, second], requests.clone()));
    std::fs::create_dir_all(home.root()).unwrap();
    std::fs::write(
        home.settings_path(),
        serde_json::json!({"ollama": {"instances": [{"id": "local", "baseUrl": format!("http://127.0.0.1:{port}"), "enabled": true}]}})
            .to_string(),
    )
    .unwrap();

    let engine = Engine::new(home.clone(), &workspace).unwrap();
    let conv = engine.new_conversation(Some(ModelRef::new("ollama", "mock-model")));
    let rx = conv.prompt("please write a file", vec![]).unwrap();
    let events: Vec<AgentEvent> = engine.block_on(async move {
        let mut events = Vec::new();
        while let Ok(e) = rx.recv().await {
            events.push(e);
        }
        events
    });

    assert_eq!(
        std::fs::read_to_string(workspace.join("out.txt")).unwrap(),
        "made by qt"
    );
    assert!(matches!(events.first(), Some(AgentEvent::AgentStart)));
    assert!(
        matches!(events.last(), Some(AgentEvent::AgentEnd { aborted: false, usage }) if usage.output_tokens == 25)
    );
    assert!(
        events
            .iter()
            .any(|e| matches!(e, AgentEvent::ToolStart(t) if t.name == "write"))
    );

    let messages = conv.messages();
    assert_eq!(messages.len(), 4, "{messages:#?}");
    let Message::Assistant(a) = &messages[1] else {
        panic!()
    };
    assert_eq!(a.stop_reason, StopReason::ToolCall);
    let Message::ToolResult(r) = &messages[2] else {
        panic!()
    };
    assert!(!r.is_error);
    let Message::Assistant(done) = &messages[3] else {
        panic!()
    };
    assert_eq!(done.content, vec![ContentBlock::text("Done.")]);
    assert_eq!(conv.context_tokens(), 155);

    // The second request carried the tool result back to the model.
    let reqs = requests.lock().unwrap();
    assert_eq!(reqs.len(), 2);
    let sent = reqs[1]["messages"].as_array().unwrap();
    assert_eq!(sent.last().unwrap()["role"], "tool");
    assert_eq!(sent.last().unwrap()["tool_call_id"], "call_1");
    assert_eq!(reqs[0]["model"], "mock-model");
    assert!(
        reqs[0]["messages"][0]["content"]
            .as_str()
            .unwrap()
            .contains("Available tools")
    );

    // The session file resumes to the same conversation.
    let resumed = engine.open_conversation(&conv.session_path()).unwrap();
    assert_eq!(resumed.messages(), messages);
    assert_eq!(engine.sessions().len(), 1);
    assert_eq!(engine.sessions()[0].title, "please write a file");
}
