//! stdio transport against the scripted `qt-mcp-mock-server`.

mod support;

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use futures::future::BoxFuture;
use qt_core::message::ContentBlock;
use qt_core::provider::{ProviderConfig, ProviderKind};
use qt_core::tools::{ToolContext, ToolRegistry};
use qt_mcp::{
    McpError, McpManager, McpOptions, McpServerConfig, ProviderSamplingHandler, SamplingHandler,
    SamplingRequest, ServerStatus,
};
use serde_json::{Value, json};
use support::{HttpResponse, serve};
use tokio_util::sync::CancellationToken;

fn mock(name: &str) -> McpServerConfig {
    McpServerConfig::stdio(name, env!("CARGO_BIN_EXE_qt-mcp-mock-server"), vec![])
}

fn fast() -> McpOptions {
    McpOptions {
        attempts: 2,
        base_delay: Duration::from_millis(10),
    }
}

fn ctx() -> ToolContext {
    ToolContext::new(std::env::temp_dir())
}

#[tokio::test]
async fn discovers_paginated_tools_and_logs_stderr() {
    let dir = tempfile::tempdir().unwrap();
    let log = dir.path().join("logs/mcp-stderr.log");
    let mut config = mock("Mock");
    config.stderr_log_path = Some(log.clone());
    let manager = McpManager::new(vec![config]);
    let diagnostics = manager.connect_all().await;
    assert_eq!(
        diagnostics,
        ["MCP tool mcp_mock_image description contains prompt-like text"]
    );
    let names: Vec<_> = manager
        .tools()
        .into_iter()
        .map(|t| t.registered_name)
        .collect();
    assert_eq!(
        names,
        [
            "mcp_mock_echo",
            "mcp_mock_fail",
            "mcp_mock_image",
            "mcp_mock_sample",
            "mcp_mock_slow",
            "mcp_mock_crash",
            "mcp_mock_env",
        ]
    );
    let snapshot = &manager.snapshots()[0];
    assert_eq!(snapshot.status, ServerStatus::Connected);
    assert_eq!(snapshot.transport, "stdio");
    let info = snapshot.info.as_ref().unwrap();
    assert_eq!(info.server_info["name"], "mock");
    assert_eq!(info.protocol_version, "2025-03-26");
    assert!(info.supports_resources());
    // No sampling handler installed: the capability is not advertised.
    assert_eq!(
        info.instructions.as_deref(),
        Some("client capabilities: {}")
    );

    let text = std::fs::read_to_string(&log).unwrap();
    assert!(text.contains("mock-mcp-server started"), "{text}");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        let mode = std::fs::metadata(&log).unwrap().permissions().mode();
        assert_eq!(mode & 0o777, 0o600);
    }
    manager.shutdown().await;
    assert_eq!(manager.snapshots()[0].status, ServerStatus::Idle);
}

#[tokio::test]
async fn proxy_tools_call_the_server() {
    let mut config = mock("mock");
    config.env.insert("MOCK_VAR".into(), "configured".into());
    let manager = McpManager::with_options(vec![config], fast());
    manager.connect_all().await;
    let mut diagnostics = Vec::new();
    let registry = manager.register_tools(
        ToolRegistry::coding(),
        Some(&|name: &str| name != "mcp_mock_crash"),
        &mut diagnostics,
    );
    assert!(diagnostics.is_empty());
    assert!(registry.get("mcp_mock_crash").is_none());
    let echo = registry.get("mcp_mock_echo").unwrap();
    assert_eq!(echo.description(), "MCP mock/echo: Echo text back");
    assert_eq!(echo.snippet(), "MCP mock/echo: Echo text back");
    assert_eq!(echo.parameters()["required"], json!(["text"]));

    let out = registry
        .execute("mcp_mock_echo", json!({"text": "hello"}), ctx())
        .await;
    assert!(!out.is_error);
    assert_eq!(out.text_content(), "hello");
    assert_eq!(out.details.as_ref().unwrap()["tool"], "echo");

    let out = registry.execute("mcp_mock_fail", Value::Null, ctx()).await;
    assert!(out.is_error);
    assert_eq!(out.text_content(), "it failed");

    let out = registry.execute("mcp_mock_image", json!({}), ctx()).await;
    assert!(
        matches!(&out.content[..], [ContentBlock::Image { mime_type, .. }] if mime_type == "image/png")
    );

    let env = |name: &str| {
        let registry = registry.clone();
        let name = name.to_string();
        async move {
            registry
                .execute("mcp_mock_env", json!({"name": name}), ctx())
                .await
                .text_content()
        }
    };
    assert_eq!(env("MOCK_VAR").await, "configured");
    assert_eq!(env("CARGO_PKG_NAME").await, "<unset>");
    assert_eq!(
        env("PATH").await.split(':').next().unwrap(),
        env!("CARGO_BIN_EXE_qt-mcp-mock-server")
            .rsplit_once('/')
            .unwrap()
            .0
    );

    let out = registry
        .execute("mcp_mock_echo", Value::String("{oops".into()), ctx())
        .await;
    assert!(out.is_error);
    assert!(
        out.text_content()
            .starts_with("Invalid arguments for mcp_mock_echo: not valid JSON")
    );

    // Duplicate names are reported, not replaced.
    let again = manager.register_tools(registry, None, &mut diagnostics);
    assert!(again.get("mcp_mock_crash").is_some());
    assert!(
        diagnostics
            .contains(&"MCP tool registration skipped duplicate name: mcp_mock_echo".to_string())
    );
    manager.shutdown().await;
}

#[tokio::test]
async fn resource_tools_list_and_read() {
    let manager = McpManager::with_options(vec![mock("mock")], fast());
    manager.connect_all().await;
    let registry = manager
        .resource_tools()
        .into_iter()
        .fold(ToolRegistry::empty(), |r, t| r.with(t));

    let out = registry
        .execute("list_mcp_resources", json!({}), ctx())
        .await;
    assert!(!out.is_error, "{}", out.text_content());
    let listed: Value = serde_json::from_str(&out.text_content()).unwrap();
    assert_eq!(
        listed,
        json!([{"uri": "mock://readme", "name": "readme", "mimeType": "text/plain", "server": "mock"}])
    );

    let out = registry
        .execute("list_mcp_resources", json!({"mcp_name": "mock"}), ctx())
        .await;
    assert_eq!(out.details.as_ref().unwrap()["server"], "mock");

    let out = registry
        .execute("list_mcp_resources", json!({"server": "other"}), ctx())
        .await;
    assert!(out.is_error);
    assert_eq!(out.text_content(), "MCP server not found: other");

    let out = registry
        .execute(
            "read_mcp_resource",
            json!({"server": "mock", "uri": "mock://readme"}),
            ctx(),
        )
        .await;
    assert_eq!(out.text_content(), "Mock readme");

    let out = registry
        .execute(
            "read_mcp_resource",
            json!({"server": "mock", "uri": "mock://nope"}),
            ctx(),
        )
        .await;
    assert!(out.is_error);
    assert_eq!(
        out.text_content(),
        r#"{"code":-32002,"message":"Resource not found: mock://nope"}"#
    );

    let out = registry
        .execute("read_mcp_resource", json!({"uri": "mock://readme"}), ctx())
        .await;
    assert_eq!(
        out.text_content(),
        "read_mcp_resource requires non-empty server and uri"
    );
    manager.shutdown().await;
}

struct Recording(Arc<Mutex<Vec<SamplingRequest>>>);

impl SamplingHandler for Recording {
    fn create_message(
        &self,
        request: SamplingRequest,
    ) -> BoxFuture<'static, Result<Value, String>> {
        self.0.lock().unwrap().push(request.clone());
        Box::pin(async move {
            let prompt = request.params["messages"][0]["content"]["text"]
                .as_str()
                .unwrap_or_default()
                .to_string();
            if prompt == "refuse" {
                return Err("declined with token=abc".into());
            }
            Ok(
                json!({"role": "assistant", "content": {"type": "text", "text": format!("pong to {prompt}")}, "model": "m", "stopReason": "endTurn"}),
            )
        })
    }
}

#[tokio::test]
async fn sampling_requests_reach_the_handler() {
    let mut config = mock("mock");
    config.sampling = json!({"max_tokens_cap": 10});
    let manager = McpManager::with_options(vec![config], fast());
    let seen = Arc::new(Mutex::new(Vec::new()));
    manager.set_sampling_handler(Some(Arc::new(Recording(seen.clone()))));
    manager.connect_all().await;
    let info = manager.snapshots()[0].info.clone().unwrap();
    assert_eq!(
        info.instructions.as_deref(),
        Some(r#"client capabilities: {"sampling":{}}"#)
    );

    let result = manager
        .call_tool("mock", "sample", json!({"prompt": "ping"}), None)
        .await
        .unwrap();
    assert_eq!(result["content"][0]["text"], "sampled: pong to ping");
    let requests = seen.lock().unwrap().clone();
    assert_eq!(requests.len(), 1);
    assert_eq!(requests[0].server_name, "mock");
    assert_eq!(requests[0].config, json!({"max_tokens_cap": 10}));
    assert_eq!(requests[0].params["maxTokens"], 50);

    let result = manager
        .call_tool("mock", "sample", json!({"prompt": "refuse"}), None)
        .await
        .unwrap();
    assert_eq!(
        result["content"][0]["text"],
        r#"sampling error: {"code":-32000,"message":"declined with token=[REDACTED]"}"#
    );

    manager.set_sampling_handler(None);
    let result = manager
        .call_tool("mock", "sample", json!({"prompt": "x"}), None)
        .await
        .unwrap();
    assert_eq!(
        result["content"][0]["text"],
        r#"sampling error: {"code":-32601,"message":"MCP sampling/createMessage is not enabled for this server"}"#
    );
    manager.shutdown().await;
}

#[tokio::test]
async fn disabled_sampling_policy_rejects_requests() {
    let mut config = mock("mock");
    config.sampling = json!({"enabled": false});
    let manager = McpManager::with_options(vec![config], fast());
    manager.set_sampling_handler(Some(Arc::new(Recording(Arc::default()))));
    manager.connect_all().await;
    let info = manager.snapshots()[0].info.clone().unwrap();
    assert_eq!(
        info.instructions.as_deref(),
        Some("client capabilities: {}")
    );
    let result = manager
        .call_tool("mock", "sample", json!({"prompt": "x"}), None)
        .await
        .unwrap();
    assert!(
        result["content"][0]["text"]
            .as_str()
            .unwrap()
            .contains("-32601")
    );
    manager.shutdown().await;
}

#[tokio::test]
async fn provider_sampling_handler_runs_a_completion() {
    let requests = Arc::new(Mutex::new(Vec::new()));
    let seen = requests.clone();
    let base = serve(move |request| {
        let seen = seen.clone();
        async move {
            seen.lock()
                .unwrap()
                .push((request.path.clone(), request.json()));
            let chunks = [
                json!({"choices": [{"delta": {"content": "model says hi"}}]}),
                json!({"choices": [{"delta": {}, "finish_reason": "stop"}]}),
            ];
            let mut body: String = chunks.iter().map(|c| format!("data: {c}\n\n")).collect();
            body.push_str("data: [DONE]\n\n");
            HttpResponse::Full {
                status: 200,
                headers: vec![("content-type".into(), "text/event-stream".into())],
                body,
            }
        }
    })
    .await;
    let provider = ProviderConfig {
        id: "ollama:local".into(),
        label: "Ollama".into(),
        kind: ProviderKind::OpenAiCompletions,
        base_url: format!("{base}/v1"),
        local: true,
        ..Default::default()
    };
    let mut config = mock("mock");
    config.sampling = json!({"max_tokens_cap": 20});
    let manager = McpManager::with_options(vec![config], fast());
    manager.set_sampling_handler(Some(Arc::new(ProviderSamplingHandler::new(
        reqwest::Client::new(),
        provider,
        "llama3",
    ))));
    manager.connect_all().await;
    let result = manager
        .call_tool("mock", "sample", json!({"prompt": "hello"}), None)
        .await
        .unwrap();
    assert_eq!(result["content"][0]["text"], "sampled: model says hi");
    let requests = requests.lock().unwrap().clone();
    assert_eq!(requests.len(), 1);
    assert_eq!(requests[0].0, "/v1/chat/completions");
    let body = &requests[0].1;
    assert_eq!(body["model"], "llama3");
    let last = body["messages"].as_array().unwrap().last().unwrap().clone();
    assert_eq!(last["role"], "user");
    assert!(last["content"].to_string().contains("hello"));
    manager.shutdown().await;
}

#[tokio::test]
async fn crashed_servers_are_restarted() {
    let manager = McpManager::with_options(vec![mock("mock")], fast());
    manager.connect_all().await;
    let err = manager
        .call_tool("mock", "crash", json!({}), None)
        .await
        .unwrap_err();
    assert_eq!(
        err,
        McpError::Transport("MCP stdio server exited: mock".into())
    );
    assert_eq!(
        manager.snapshots()[0].status,
        ServerStatus::Failed("MCP stdio server exited: mock".into())
    );
    let result = manager
        .call_tool("mock", "echo", json!({"text": "back"}), None)
        .await
        .unwrap();
    assert_eq!(result["content"][0]["text"], "back");
    assert_eq!(manager.snapshots()[0].status, ServerStatus::Connected);

    let mut changes = manager.subscribe();
    let before = *changes.borrow_and_update();
    let diagnostics = manager.reconnect("mock").await.unwrap();
    assert_eq!(diagnostics.len(), 1);
    assert!(*changes.borrow() > before);
    manager.disconnect("mock").await.unwrap();
    assert_eq!(manager.snapshots()[0].status, ServerStatus::Idle);
    assert_eq!(
        manager.reconnect("ghost").await.unwrap_err(),
        McpError::NotFound("ghost".into())
    );
}

#[tokio::test]
async fn cancellation_and_timeouts() {
    let mut config = mock("mock");
    config.timeout_seconds = 1;
    let manager = McpManager::with_options(vec![config], fast());
    manager.connect_all().await;

    let started = Instant::now();
    let err = manager
        .call_tool("mock", "slow", json!({}), None)
        .await
        .unwrap_err();
    assert_eq!(
        err,
        McpError::Timeout("MCP request timed out: tools/call on mock".into())
    );
    assert!(started.elapsed() < Duration::from_millis(2500));

    let registry = manager.register_tools(ToolRegistry::empty(), None, &mut Vec::new());
    let cancel = CancellationToken::new();
    let trigger = cancel.clone();
    tokio::spawn(async move {
        tokio::time::sleep(Duration::from_millis(100)).await;
        trigger.cancel();
    });
    let started = Instant::now();
    let out = registry
        .execute("mcp_mock_slow", json!({}), ctx().with_cancel(cancel))
        .await;
    assert!(started.elapsed() < Duration::from_millis(900));
    assert!(out.is_error);
    assert_eq!(
        out.text_content(),
        "MCP request cancelled: tools/call on mock"
    );
    manager.shutdown().await;
}

#[tokio::test]
async fn startup_failures_become_diagnostics() {
    let missing = McpServerConfig::stdio("missing", "/nonexistent/qt-mcp-server", vec![]);
    let mut empty = McpServerConfig::stdio("empty", "", vec![]);
    empty.command = None;
    let manager = McpManager::with_options(vec![missing, empty], fast());
    let diagnostics = manager.connect_all().await;
    assert_eq!(diagnostics.len(), 2);
    assert!(
        diagnostics[0].starts_with(
            "MCP tool discovery failed for missing: failed to start MCP stdio server missing:"
        ),
        "{diagnostics:?}"
    );
    assert_eq!(
        diagnostics[1],
        "MCP tool discovery failed for empty: MCP stdio server has no command"
    );
    assert!(matches!(
        manager.snapshots()[0].status,
        ServerStatus::Failed(_)
    ));
    assert!(manager.tools().is_empty());
}
