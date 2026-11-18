//! `qt-cli --mode rpc` driven over stdin/stdout against a mock
//! OpenAI-compatible server (an Ollama instance on 127.0.0.1): every
//! command, the streamed events and an extension dialog round trip.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{Receiver, RecvTimeoutError, channel};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde_json::{Value, json};

const TIMEOUT: Duration = Duration::from_secs(60);

type Responder = Arc<dyn Fn(&Value) -> String + Send + Sync>;

struct MockServer {
    port: u16,
    requests: Arc<Mutex<Vec<Value>>>,
}

fn read_request(stream: &mut TcpStream) -> Option<(String, Vec<u8>)> {
    let mut reader = BufReader::new(stream.try_clone().ok()?);
    let mut request_line = String::new();
    reader.read_line(&mut request_line).ok()?;
    let mut length = 0usize;
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line).ok()? == 0 {
            break;
        }
        let trimmed = line.trim_end();
        if trimmed.is_empty() {
            break;
        }
        if let Some(v) = trimmed.to_ascii_lowercase().strip_prefix("content-length:") {
            length = v.trim().parse().unwrap_or(0);
        }
    }
    let mut body = vec![0u8; length];
    reader.read_exact(&mut body).ok()?;
    Some((request_line, body))
}

impl MockServer {
    fn start(respond: Responder) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let requests = Arc::new(Mutex::new(Vec::new()));
        let recorded = requests.clone();
        std::thread::spawn(move || {
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { continue };
                let respond = respond.clone();
                let recorded = recorded.clone();
                std::thread::spawn(move || {
                    let Some((line, body)) = read_request(&mut stream) else {
                        return;
                    };
                    let reply = if line.starts_with("POST") && line.contains("chat/completions") {
                        let value: Value = serde_json::from_slice(&body).unwrap_or(Value::Null);
                        recorded.lock().unwrap().push(value.clone());
                        let sse = respond(&value);
                        if sse.starts_with("HTTP/") {
                            let _ = stream.write_all(sse.as_bytes());
                            return;
                        }
                        format!(
                            "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{sse}"
                        )
                    } else {
                        let body = if line.contains("/api/tags") {
                            r#"{"models":[{"name":"m"},{"name":"m2"}]}"#
                        } else if line.contains("/api/show") {
                            r#"{"capabilities":["completion","tools","thinking"],"model_info":{"x.context_length":32768}}"#
                        } else {
                            r#"{"data":[],"models":[]}"#
                        };
                        format!(
                            "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
                            body.len()
                        )
                    };
                    let _ = stream.write_all(reply.as_bytes());
                    let _ = stream.flush();
                });
            }
        });
        Self { port, requests }
    }

    fn requests(&self) -> Vec<Value> {
        self.requests.lock().unwrap().clone()
    }
}

fn sse(chunks: &[Value]) -> String {
    let mut out: String = chunks.iter().map(|c| format!("data: {c}\n\n")).collect();
    out.push_str("data: [DONE]\n\n");
    out
}

fn text_reply(text: &str) -> String {
    sse(&[
        json!({"choices":[{"delta":{"content":text}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15}}),
    ])
}

/// The text of the last user message of a chat request.
fn last_user_text(request: &Value) -> String {
    request["messages"]
        .as_array()
        .and_then(|m| m.iter().rev().find(|m| m["role"] == "user"))
        .map(|m| match &m["content"] {
            Value::String(s) => s.clone(),
            other => other.to_string(),
        })
        .unwrap_or_default()
}

/// Echo the prompt; prompts containing `slow` answer after two seconds.
fn echo_server() -> MockServer {
    MockServer::start(Arc::new(|request: &Value| {
        let text = last_user_text(request);
        if text.contains("slow") {
            std::thread::sleep(Duration::from_secs(2));
        }
        if text.contains("summar") || text.contains("<conversation>") {
            return text_reply("SUMMARY of the session");
        }
        text_reply(&format!("echo: {text}"))
    }))
}

struct Env {
    _dir: tempfile::TempDir,
    home: PathBuf,
    work: PathBuf,
    user_home: PathBuf,
}

fn env(port: u16) -> Env {
    env_with(port, json!({}))
}

fn env_with(port: u16, extra: Value) -> Env {
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("agent");
    let work = dir.path().join("work");
    let user_home = dir.path().join("user");
    for d in [&home, &work, &user_home] {
        std::fs::create_dir_all(d).unwrap();
    }
    let settings = json!({
        "ollama": {"instances": [{"id": "mock", "baseUrl": format!("http://127.0.0.1:{port}")}]},
        "defaultProvider": "ollama:mock",
        "defaultModel": "m",
        "retry": {"enabled": false, "provider": {"maxRetries": 0}},
        "compaction": {"enabled": false, "keepRecentTokens": 1},
        "memory": {"enabled": false},
    });
    let mut settings = settings;
    if let (Some(base), Some(extra)) = (settings.as_object_mut(), extra.as_object()) {
        for (k, v) in extra {
            base.insert(k.clone(), v.clone());
        }
    }
    std::fs::write(home.join("settings.json"), settings.to_string()).unwrap();
    Env {
        _dir: dir,
        home,
        work,
        user_home,
    }
}

/// A running `qt-cli --mode rpc` with its output lines.
struct Rpc {
    child: Child,
    stdin: Option<ChildStdin>,
    lines: Receiver<Value>,
    seen: Vec<Value>,
    next_id: u64,
}

impl Rpc {
    fn start(env: &Env, args: &[&str]) -> Self {
        let mut command = Command::new(env!("CARGO_BIN_EXE_qt-cli"));
        command
            .args(["--mode", "rpc"])
            .args(args)
            .current_dir(&env.work)
            .env("QT_CODING_AGENT_DIR", &env.home)
            .env("HOME", &env.user_home)
            .env("QT_SUBAGENT_EXECUTABLE", env!("CARGO_BIN_EXE_qt-cli"))
            .env_remove("PI_CODING_AGENT_SESSION_DIR")
            .env_remove("QT_SUBAGENT_DEPTH")
            .env_remove("QT_SUBAGENT_CHILD")
            .env_remove("PI_OFFLINE")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        let mut child = command.spawn().expect("spawn qt-cli");
        let stdout = child.stdout.take().unwrap();
        let (tx, lines) = channel();
        std::thread::spawn(move || {
            for line in BufReader::new(stdout).lines() {
                let Ok(line) = line else { break };
                let value: Value = serde_json::from_str(&line)
                    .unwrap_or_else(|e| panic!("stdout line is not JSON ({e}): {line}"));
                if tx.send(value).is_err() {
                    break;
                }
            }
        });
        Self {
            stdin: child.stdin.take(),
            child,
            lines,
            seen: Vec::new(),
            next_id: 0,
        }
    }

    fn write_raw(&mut self, text: &str) {
        let stdin = self.stdin.as_mut().expect("stdin open");
        stdin.write_all(text.as_bytes()).unwrap();
        stdin.flush().unwrap();
    }

    /// Send a command with a fresh id; returns the id.
    fn send(&mut self, mut command: Value) -> String {
        self.next_id += 1;
        let id = format!("req-{}", self.next_id);
        command["id"] = json!(id);
        self.write_raw(&format!("{command}\n"));
        id
    }

    /// The next line matching `matches` (lines before it are kept in `seen`).
    fn wait_for(&mut self, what: &str, matches: impl Fn(&Value) -> bool) -> Value {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            match self.lines.recv_timeout(left) {
                Ok(line) => {
                    self.seen.push(line.clone());
                    if matches(&line) {
                        return line;
                    }
                }
                Err(RecvTimeoutError::Timeout) => {
                    panic!("timed out waiting for {what}; seen: {:#?}", self.seen)
                }
                Err(RecvTimeoutError::Disconnected) => {
                    panic!("output closed waiting for {what}; seen: {:#?}", self.seen)
                }
            }
        }
    }

    fn response(&mut self, id: &str) -> Value {
        let id = id.to_string();
        self.wait_for(&format!("response {id}"), |l| {
            l["type"] == "response" && l["id"] == id.as_str()
        })
    }

    /// Send and wait for a successful response; returns its `data`.
    fn call(&mut self, command: Value) -> Value {
        let kind = command["type"].as_str().unwrap().to_string();
        let id = self.send(command);
        let response = self.response(&id);
        assert_eq!(response["command"], kind.as_str(), "{response}");
        assert_eq!(response["success"], true, "{response}");
        response.get("data").cloned().unwrap_or(Value::Null)
    }

    /// Send and expect a failure; returns the response.
    fn fail(&mut self, command: Value) -> Value {
        let id = self.send(command);
        let response = self.response(&id);
        assert_eq!(response["success"], false, "{response}");
        response
    }

    fn event(&mut self, kind: &str) -> Value {
        let kind = kind.to_string();
        self.wait_for(&kind.clone(), |l| l["type"] == kind.as_str())
    }

    /// Prompt and wait until the run's `agent_end`.
    fn prompt(&mut self, message: &str) -> Value {
        self.call(json!({"type": "prompt", "message": message}));
        self.event("agent_end")
    }

    fn close(mut self) -> std::process::ExitStatus {
        self.stdin.take();
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                return status;
            }
            assert!(Instant::now() < deadline, "qt-cli did not exit");
            std::thread::sleep(Duration::from_millis(20));
        }
    }
}

impl Drop for Rpc {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[test]
fn prompt_streams_events_and_reports_state() {
    let server = echo_server();
    let env = env(server.port);
    let mut rpc = Rpc::start(&env, &[]);

    let state = rpc.call(json!({"type": "get_state"}));
    assert_eq!(state["model"]["provider"], "ollama:mock");
    assert_eq!(state["model"]["id"], "m");
    assert_eq!(state["isStreaming"], false);
    assert_eq!(state["isCompacting"], false);
    assert_eq!(state["steeringMode"], "one-at-a-time");
    assert_eq!(state["followUpMode"], "one-at-a-time");
    assert_eq!(state["messageCount"], 0);
    assert_eq!(state["pendingMessageCount"], 0);
    assert_eq!(state["autoCompactionEnabled"], false);
    assert!(state["sessionId"].as_str().is_some_and(|s| !s.is_empty()));
    assert!(state["sessionFile"].as_str().is_some());

    // The response comes before the run's events.
    let id = rpc.send(json!({"type": "prompt", "message": "hello there"}));
    let response = rpc.response(&id);
    assert_eq!(response["success"], true);
    assert!(response.get("data").is_none());
    let end = rpc.event("agent_end");
    let kinds: Vec<&str> = rpc
        .seen
        .iter()
        .skip_while(|l| l["id"] != id.as_str())
        .map(|l| l["type"].as_str().unwrap())
        .collect();
    assert_eq!(kinds[1], "agent_start", "{kinds:?}");
    for expected in [
        "turn_start",
        "message_start",
        "message_update",
        "message_end",
        "turn_end",
    ] {
        assert!(kinds.contains(&expected), "missing {expected}: {kinds:?}");
    }
    let assistant = end["messages"]
        .as_array()
        .unwrap()
        .iter()
        .find(|m| m["role"] == "assistant")
        .unwrap();
    assert_eq!(assistant["content"][0]["text"], "echo: hello there");

    let messages = rpc.call(json!({"type": "get_messages"}));
    let roles: Vec<&str> = messages["messages"]
        .as_array()
        .unwrap()
        .iter()
        .map(|m| m["role"].as_str().unwrap())
        .collect();
    assert_eq!(roles, ["user", "assistant"]);
    assert_eq!(
        rpc.call(json!({"type": "get_last_assistant_text"})),
        json!({"text": "echo: hello there"})
    );
    let stats = rpc.call(json!({"type": "get_session_stats"}));
    assert_eq!(stats["userMessages"], 1);
    assert_eq!(stats["assistantMessages"], 1);
    assert_eq!(stats["tokens"]["input"], 12);
    assert!(stats["models"]["ollama:mock/m"].is_object(), "{stats}");

    // Idle queueing: steer and follow_up report the queue.
    rpc.call(json!({"type": "steer", "message": "first steer"}));
    let update = rpc.event("queue_update");
    assert_eq!(update["steering"], json!(["first steer"]));
    rpc.call(json!({"type": "follow_up", "message": "later"}));
    let update = rpc.event("queue_update");
    assert_eq!(update["followUp"], json!(["later"]));
    assert_eq!(
        rpc.call(json!({"type": "get_state"}))["pendingMessageCount"],
        2
    );
    // abort clears the queue.
    rpc.call(json!({"type": "abort"}));
    let update = rpc.event("queue_update");
    assert_eq!(
        update,
        json!({"type": "queue_update", "steering": [], "followUp": []})
    );

    rpc.call(json!({"type": "set_steering_mode", "mode": "all"}));
    rpc.call(json!({"type": "set_follow_up_mode", "mode": "all"}));
    rpc.call(json!({"type": "set_auto_compaction", "enabled": true}));
    rpc.call(json!({"type": "set_auto_retry", "enabled": true}));
    rpc.call(json!({"type": "abort_retry"}));
    let state = rpc.call(json!({"type": "get_state"}));
    assert_eq!(state["steeringMode"], "all");
    assert_eq!(state["followUpMode"], "all");
    assert_eq!(state["autoCompactionEnabled"], true);
    assert_eq!(state["messageCount"], 2);
    let settings: Value =
        serde_json::from_str(&std::fs::read_to_string(env.home.join("settings.json")).unwrap())
            .unwrap();
    assert_eq!(settings["retry"]["enabled"], true);
    assert_eq!(settings["compaction"]["enabled"], true);

    // While a run is active: prompts need a streamingBehavior.
    rpc.call(json!({"type": "prompt", "message": "slow one"}));
    rpc.event("agent_start");
    let busy = rpc.fail(json!({"type": "prompt", "message": "again"}));
    assert_eq!(
        busy["error"],
        "Agent is already processing. Specify streamingBehavior ('steer' or 'followUp') to queue the message."
    );
    assert_eq!(busy["error_code"], "internal_error");
    assert_eq!(busy["errorCode"], "internal_error");
    assert_eq!(rpc.call(json!({"type": "get_state"}))["isStreaming"], true);
    rpc.call(json!({"type": "prompt", "message": "also this", "streamingBehavior": "followUp"}));
    let update = rpc.event("queue_update");
    assert_eq!(update["followUp"], json!(["also this"]));
    // The follow-up runs after the slow answer, in the same run.
    let end = rpc.event("agent_end");
    let texts: Vec<String> = end["messages"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|m| m["role"] == "assistant")
        .map(|m| {
            m["content"][0]["text"]
                .as_str()
                .unwrap_or_default()
                .to_string()
        })
        .collect();
    assert_eq!(texts, ["echo: slow one", "echo: also this"]);
    assert!(rpc.close().success());
}

#[test]
fn abort_stops_a_running_prompt() {
    let server = echo_server();
    let env = env(server.port);
    let mut rpc = Rpc::start(&env, &["--no-session"]);
    rpc.call(json!({"type": "prompt", "message": "slow please"}));
    rpc.event("agent_start");
    let id = rpc.send(json!({"type": "abort"}));
    let response = rpc.response(&id);
    assert_eq!(response["success"], true);
    // The aborted run ended before the abort was acknowledged.
    assert!(rpc.seen.iter().any(|l| l["type"] == "agent_end"));
    let state = rpc.call(json!({"type": "get_state"}));
    assert_eq!(state["isStreaming"], false);
    assert!(state.get("sessionFile").is_none());
    assert!(rpc.close().success());
}

#[test]
fn models_and_thinking_levels() {
    let server = echo_server();
    let env = env(server.port);
    let mut rpc = Rpc::start(&env, &["--no-session"]);
    let models = rpc.call(json!({"type": "get_available_models"}));
    let ids: Vec<&str> = models["models"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|m| m["provider"] == "ollama:mock")
        .map(|m| m["id"].as_str().unwrap())
        .collect();
    assert_eq!(ids, ["m", "m2"]);
    let model = rpc.call(json!({"type": "set_model", "provider": "ollama:mock", "modelId": "m2"}));
    assert_eq!(model["id"], "m2");
    assert_eq!(model["provider"], "ollama:mock");
    assert_eq!(model["reasoning"], true);
    assert_eq!(model["contextWindow"], 32768);
    let missing =
        rpc.fail(json!({"type": "set_model", "provider": "ollama:mock", "modelId": "nope"}));
    assert_eq!(missing["error"], "Model not found: ollama:mock/nope");

    rpc.call(json!({"type": "set_thinking_level", "level": "high"}));
    assert_eq!(
        rpc.call(json!({"type": "get_state"}))["thinkingLevel"],
        "high"
    );
    let cycled = rpc.call(json!({"type": "cycle_thinking_level"}));
    assert!(cycled["level"].is_string(), "{cycled}");
    assert_ne!(cycled["level"], "high");

    let cycled = rpc.call(json!({"type": "cycle_model"}));
    assert_eq!(cycled["isScoped"], false);
    assert!(cycled["model"]["id"].is_string());
    assert!(cycled["thinkingLevel"].is_string());
    let state = rpc.call(json!({"type": "get_state"}));
    assert_eq!(state["model"]["id"], cycled["model"]["id"]);

    // Invalid arguments are rejected with the archived messages.
    let bad = rpc.fail(json!({"type": "set_thinking_level", "level": "max"}));
    assert_eq!(
        bad["error"],
        "thinking level must be off, minimal, low, medium, high, or xhigh"
    );
    assert_eq!(bad["command"], "set_thinking_level");
    assert!(rpc.close().success());
}

#[test]
fn sessions_fork_clone_switch_export_compact_and_bash() {
    let server = echo_server();
    let env = env(server.port);
    let mut rpc = Rpc::start(&env, &[]);
    rpc.prompt("one");
    rpc.prompt("two");
    let first = rpc.call(json!({"type": "get_state"}));
    let first_file = first["sessionFile"].as_str().unwrap().to_string();

    rpc.call(json!({"type": "set_session_name", "name": "  Named  "}));
    let info = rpc.event("session_info_changed");
    assert_eq!(info["name"], "Named");
    assert_eq!(
        rpc.call(json!({"type": "get_state"}))["sessionName"],
        "Named"
    );
    let empty = rpc.fail(json!({"type": "set_session_name", "name": "   "}));
    assert_eq!(empty["error"], "Session name cannot be empty");

    let fork_messages = rpc.call(json!({"type": "get_fork_messages"}));
    let messages = fork_messages["messages"].as_array().unwrap();
    assert_eq!(messages.len(), 2);
    assert_eq!(messages[1]["text"], "two");
    let entry = messages[1]["entryId"].as_str().unwrap().to_string();
    let forked = rpc.call(json!({"type": "fork", "entryId": entry}));
    assert_eq!(forked, json!({"text": "two", "cancelled": false}));
    let state = rpc.call(json!({"type": "get_state"}));
    assert_ne!(state["sessionId"], first["sessionId"]);
    assert_eq!(state["messageCount"], 2);
    let bad = rpc.fail(json!({"type": "fork", "entryId": "nope"}));
    assert_eq!(bad["error"], "Invalid entry ID for forking");

    assert_eq!(
        rpc.call(json!({"type": "clone"})),
        json!({"cancelled": false})
    );
    let cloned = rpc.call(json!({"type": "get_state"}));
    assert!(cloned["sessionId"].as_str().unwrap().starts_with("clone-"));

    let created = rpc.call(json!({"type": "new_session", "parentSession": first_file}));
    assert_eq!(created, json!({"cancelled": false}));
    let fresh = rpc.call(json!({"type": "get_state"}));
    assert_eq!(fresh["messageCount"], 0);
    rpc.prompt("in the new session");
    let new_file = rpc.call(json!({"type": "get_state"}))["sessionFile"]
        .as_str()
        .unwrap()
        .to_string();
    let header: Value = serde_json::from_str(
        std::fs::read_to_string(&new_file)
            .unwrap()
            .lines()
            .next()
            .unwrap(),
    )
    .unwrap();
    assert_eq!(header["parentSession"], first_file.as_str());

    let switched = rpc.call(json!({"type": "switch_session", "sessionPath": first_file}));
    assert_eq!(switched, json!({"cancelled": false}));
    let back = rpc.call(json!({"type": "get_state"}));
    assert_eq!(back["sessionId"], first["sessionId"]);
    assert_eq!(back["messageCount"], 4);
    let missing = rpc.fail(json!({"type": "switch_session", "sessionPath": "nowhere.jsonl"}));
    assert!(
        missing["error"]
            .as_str()
            .unwrap()
            .starts_with("Session file not found: "),
        "{missing}"
    );

    let exported = rpc.call(json!({"type": "export_html", "outputPath": "out/session.html"}));
    let path = PathBuf::from(exported["path"].as_str().unwrap());
    assert_eq!(path, env.work.join("out/session.html"));
    assert!(std::fs::read_to_string(&path).unwrap().contains("<html"));

    let compacted = rpc.call(json!({"type": "compact", "customInstructions": "keep it short"}));
    assert!(
        compacted["summary"]
            .as_str()
            .unwrap()
            .starts_with("SUMMARY of the session"),
        "{compacted}"
    );
    assert!(compacted["firstKeptEntryId"].is_string());
    assert!(compacted["tokensBefore"].as_u64().unwrap() > 0);
    assert!(
        rpc.seen
            .iter()
            .any(|l| l["type"] == "compaction_start" && l["reason"] == "manual")
    );
    assert!(rpc.seen.iter().any(|l| l["type"] == "compaction_end"));

    let bash = rpc.call(json!({"type": "bash", "command": "echo from-bash; exit 3"}));
    assert_eq!(bash["output"].as_str().unwrap().trim(), "from-bash");
    assert_eq!(bash["exitCode"], 3);
    assert_eq!(bash["cancelled"], false);
    assert_eq!(bash["truncated"], false);
    let messages = rpc.call(json!({"type": "get_messages"}));
    assert!(
        messages["messages"]
            .as_array()
            .unwrap()
            .iter()
            .any(|m| m["role"] == "bashExecution"),
        "{messages}"
    );
    // abort_bash while a command runs cancels it.
    let id = rpc.send(json!({"type": "bash", "command": "sleep 30"}));
    std::thread::sleep(Duration::from_millis(300));
    rpc.call(json!({"type": "abort_bash"}));
    let response = rpc.response(&id);
    assert_eq!(response["data"]["cancelled"], true, "{response}");
    assert!(rpc.close().success());
}

#[test]
fn protocol_errors_commands_memory_and_subagents() {
    let server = echo_server();
    let env = env(server.port);
    std::fs::create_dir_all(env.work.join(".pi/prompts")).unwrap();
    std::fs::write(
        env.work.join(".pi/prompts/greet.md"),
        "---\ndescription: Greet someone\n---\nSay hello to $1",
    )
    .unwrap();
    std::fs::write(env.work.join("ask.rhai"), EXTENSION).unwrap();
    let mut rpc = Rpc::start(&env, &["--no-session", "-e", "ask.rhai"]);

    rpc.write_raw("this is not json\n");
    let parse = rpc.wait_for("parse error", |l| l["command"] == "parse");
    assert!(
        parse["error"]
            .as_str()
            .unwrap()
            .starts_with("Failed to parse command: ")
    );
    assert_eq!(parse["success"], false);
    rpc.write_raw("{\"type\":\"dance\"}\n");
    let invalid = rpc.wait_for("invalid", |l| l["command"] == "invalid");
    assert_eq!(invalid["error"], "unsupported rpc frame type");
    rpc.write_raw("{\"type\":\"agent_end\",\"messages\":[]}\n");
    let invalid = rpc.wait_for("event frame", |l| {
        l["command"] == "invalid" && l["error"] != "unsupported rpc frame type"
    });
    assert_eq!(
        invalid["error"],
        "rpc server accepts command frames only, received agent_end"
    );
    rpc.write_raw(
        "{\"id\":\"dup\",\"type\":\"get_state\"}\n{\"id\":\"dup\",\"type\":\"get_state\"}\n",
    );
    // The duplicate is rejected by the input thread, possibly before the
    // first request was answered.
    rpc.response("dup");
    let is_duplicate = |l: &Value| {
        l["error"]
            .as_str()
            .is_some_and(|e| e.starts_with("duplicate rpc request id"))
    };
    let duplicate = match rpc.seen.iter().find(|l| is_duplicate(l)).cloned() {
        Some(line) => line,
        None => rpc.wait_for("duplicate", is_duplicate),
    };
    assert_eq!(duplicate["error"], "duplicate rpc request id: dup");
    // CRLF framing is accepted.
    rpc.write_raw("{\"id\":\"crlf\",\"type\":\"get_state\"}\r\n");
    assert_eq!(rpc.response("crlf")["success"], true);

    let commands = rpc.call(json!({"type": "get_commands"}));
    let list = commands["commands"].as_array().unwrap();
    let ask = list
        .iter()
        .find(|c| c["name"] == "ask")
        .expect("extension command");
    assert_eq!(ask["source"], "extension");
    assert_eq!(ask["description"], "Ask the client");
    assert_eq!(ask["sourceInfo"]["scope"], "project");
    let greet = list
        .iter()
        .find(|c| c["name"] == "greet")
        .expect("template");
    assert_eq!(greet["source"], "prompt");
    assert_eq!(greet["description"], "Greet someone");
    assert!(list.iter().all(|c| c["source"] != "builtin"));

    // Templates expand before the prompt is sent.
    rpc.prompt("/greet Bob");
    let sent = server.requests();
    assert_eq!(last_user_text(sent.last().unwrap()), "Say hello to Bob");

    let status = rpc.call(json!({"type": "memory", "action": "status"}));
    assert_eq!(status["enabled"], false);
    assert_eq!(status["configured"], false);
    let disabled =
        rpc.fail(json!({"type": "memory", "action": "recall", "params": {"query": "x"}}));
    assert_eq!(
        disabled["error"],
        "built-in memory is disabled for this RPC runtime"
    );
    assert_eq!(disabled["error_code"], "invalid_argument");
    let unsupported = rpc.fail(json!({"type": "memory", "action": "nap"}));
    assert_eq!(unsupported["error"], "memory command action is unsupported");

    let workers = rpc.call(json!({"type": "subagent", "action": "list_workers"}));
    assert!(workers.is_object() || workers.is_array(), "{workers}");
    let bad = rpc.fail(json!({"type": "subagent", "action": "send", "arguments": {"target": "/root/nobody", "message": "hi"}}));
    assert_eq!(bad["command"], "subagent");
    assert!(rpc.close().success());

    // With tools disabled the subagent command is refused.
    let mut rpc = Rpc::start(&env, &["--no-session", "--no-tools"]);
    let refused = rpc.fail(json!({"type": "subagent", "action": "list_workers"}));
    assert_eq!(
        refused["error"],
        "subagent tool is disabled for this runtime"
    );
    assert_eq!(refused["error_code"], "invalid_argument");
    assert!(rpc.close().success());
}

const EXTENSION: &str = r#"
fn activate() {
    registerCommand("ask", "Ask", "Ask the client");
    registerCommand("hurry", "Hurry", "A dialog with a timeout");
    registerCommand("boom", "Boom", "Always fails");
}

fn command_ask(args) {
    let ok = ctx::ui::confirm(#{ title: "Proceed?", message: `Run ${args.args}?` });
    let pick = ctx::ui::select(#{ title: "Pick", options: ["red", "blue"] });
    ctx::ui::setStatus(#{ statusKey: "ask", statusText: "done" });
    #{ text: `confirmed=${ok} pick=${pick}` }
}

fn command_hurry(args) {
    let value = ctx::ui::input(#{ title: "Quick", placeholder: "type", timeout: 200 });
    #{ text: `input=${value}` }
}

fn command_boom(args) {
    throw "kaboom";
}
"#;

#[test]
fn extension_dialogs_round_trip_over_rpc() {
    let server = echo_server();
    let env = env(server.port);
    std::fs::write(env.work.join("ask.rhai"), EXTENSION).unwrap();
    let mut rpc = Rpc::start(&env, &["--no-session", "-e", "ask.rhai"]);

    let prompt_id = rpc.send(json!({"type": "prompt", "message": "/ask the thing"}));
    let confirm = rpc.event("extension_ui_request");
    assert_eq!(confirm["method"], "confirm");
    assert_eq!(confirm["title"], "Proceed?");
    assert_eq!(confirm["message"], "Run the thing?");
    // Other commands are answered while the dialog is open.
    assert_eq!(rpc.call(json!({"type": "get_state"}))["isStreaming"], false);
    let confirm_id = confirm["id"].as_str().unwrap().to_string();
    rpc.write_raw(&format!(
        "{}\n",
        json!({"type": "extension_ui_response", "id": confirm_id, "confirmed": true})
    ));
    let select = rpc.event("extension_ui_request");
    assert_eq!(select["method"], "select");
    assert_eq!(select["options"], json!(["red", "blue"]));
    rpc.write_raw(&format!(
        "{}\n",
        json!({"type": "extension_ui_response", "id": select["id"], "value": "blue"})
    ));
    let status = rpc.event("extension_ui_request");
    assert_eq!(status["method"], "setStatus");
    assert_eq!(status["statusKey"], "ask");
    let notify = rpc.event("extension_ui_request");
    assert_eq!(notify["method"], "notify");
    assert_eq!(notify["message"], "confirmed=true pick=blue");
    let response = rpc.response(&prompt_id);
    assert_eq!(response["success"], true);
    // The command did not reach the model.
    assert!(server.requests().is_empty());
    // A second answer for the same dialog is an error.
    rpc.write_raw(&format!(
        "{}\n",
        json!({"type": "extension_ui_response", "id": confirm_id, "confirmed": false})
    ));
    let duplicate = rpc.wait_for("duplicate ui response", |l| l["command"] == "invalid");
    assert_eq!(
        duplicate["error"],
        format!("duplicate extension ui response id: {confirm_id}")
    );

    // A cancelled dialog resolves to its default.
    let prompt_id = rpc.send(json!({"type": "prompt", "message": "/ask again"}));
    let confirm = rpc.event("extension_ui_request");
    rpc.write_raw(&format!(
        "{}\n",
        json!({"type": "extension_ui_response", "id": confirm["id"], "cancelled": true})
    ));
    let select = rpc.event("extension_ui_request");
    rpc.write_raw(&format!(
        "{}\n",
        json!({"type": "extension_ui_response", "id": select["id"], "cancelled": true})
    ));
    rpc.event("extension_ui_request"); // setStatus
    let notify = rpc.event("extension_ui_request");
    assert_eq!(notify["message"], "confirmed=false pick=");
    rpc.response(&prompt_id);

    // A dialog with a timeout expires without an answer.
    let prompt_id = rpc.send(json!({"type": "prompt", "message": "/hurry"}));
    let input = rpc.event("extension_ui_request");
    assert_eq!(input["method"], "input");
    assert_eq!(input["placeholder"], "type");
    assert_eq!(input["timeout"], 200);
    let notify = rpc.event("extension_ui_request");
    assert_eq!(notify["message"], "input=");
    rpc.response(&prompt_id);

    // A failing command is reported as an extension error.
    let prompt_id = rpc.send(json!({"type": "prompt", "message": "/boom"}));
    let error = rpc.event("extension_error");
    assert_eq!(error["extensionPath"], "command:boom");
    assert_eq!(error["event"], "command");
    assert!(
        error["error"].as_str().unwrap().contains("kaboom"),
        "{error}"
    );
    assert_eq!(rpc.response(&prompt_id)["success"], true);

    // Extension commands cannot be queued.
    let queued = rpc.fail(json!({"type": "steer", "message": "/ask"}));
    assert_eq!(
        queued["error"],
        "Extension command \"/ask\" cannot be queued. Use prompt() or execute the command when not streaming."
    );

    // Closing stdin cancels an open dialog and the process exits.
    let prompt_id = rpc.send(json!({"type": "prompt", "message": "/ask at exit"}));
    rpc.event("extension_ui_request");
    rpc.stdin.take();
    let response = rpc.response(&prompt_id);
    assert_eq!(response["success"], true);
    assert!(rpc.close().success());
}

fn tool_reply(id: &str, name: &str, args: Value) -> String {
    sse(&[
        json!({"choices":[{"delta":{"tool_calls":[{"index":0,"id":id,"function":{"name":name,"arguments":args.to_string()}}]}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15}}),
    ])
}

const HOOK_EXTENSION: &str = r#"
fn activate() {
    on("tool_call", "check");
}

fn check(event) {
    #{ block: "sometimes" }
}
"#;

#[test]
fn auto_retry_and_extension_hook_errors_are_streamed() {
    let calls = Arc::new(Mutex::new(0usize));
    let counter = calls.clone();
    let server = MockServer::start(Arc::new(move |request: &Value| {
        let mut n = counter.lock().unwrap();
        *n += 1;
        if *n == 1 {
            let body = r#"{"error":{"message":"server overloaded, try again"}}"#;
            return format!(
                "HTTP/1.1 503 Service Unavailable\r\ncontent-type: application/json\r\ncontent-length: {}\r\nconnection: close\r\n\r\n{body}",
                body.len()
            );
        }
        let has_tool_result = request["messages"]
            .as_array()
            .is_some_and(|m| m.iter().any(|m| m["role"] == "tool"));
        if has_tool_result {
            text_reply("finished")
        } else {
            tool_reply("call_1", "ls", json!({"path": "."}))
        }
    }));
    let env = env_with(
        server.port,
        json!({"retry": {"enabled": true, "maxRetries": 2, "baseDelayMs": 10, "provider": {"maxRetries": 0}}}),
    );
    std::fs::write(env.work.join("hook.rhai"), HOOK_EXTENSION).unwrap();
    let mut rpc = Rpc::start(&env, &["--no-session", "-e", "hook.rhai"]);
    rpc.call(json!({"type": "prompt", "message": "list files"}));
    let start = rpc.event("auto_retry_start");
    assert_eq!(start["attempt"], 1);
    assert_eq!(start["maxAttempts"], 2);
    assert!(
        start["errorMessage"]
            .as_str()
            .is_some_and(|e| !e.is_empty())
    );
    let error = rpc.event("extension_error");
    assert_eq!(error["event"], "tool_call");
    assert!(
        error["extensionPath"]
            .as_str()
            .unwrap()
            .ends_with("hook.rhai"),
        "{error}"
    );
    assert!(
        error["error"].as_str().unwrap().contains("block"),
        "{error}"
    );
    rpc.event("agent_end");
    let kinds: Vec<&str> = rpc.seen.iter().filter_map(|l| l["type"].as_str()).collect();
    assert!(kinds.contains(&"auto_retry_end"), "{kinds:?}");
    assert!(kinds.contains(&"tool_execution_end"), "{kinds:?}");
    assert_eq!(
        rpc.call(json!({"type": "get_last_assistant_text"}))["text"],
        "finished"
    );
    assert!(rpc.close().success());
}

#[test]
fn file_arguments_are_rejected() {
    let env = env(1);
    std::fs::write(env.work.join("notes.txt"), "x").unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_qt-cli"))
        .args(["--mode", "rpc", "@notes.txt"])
        .current_dir(&env.work)
        .env("QT_CODING_AGENT_DIR", &env.home)
        .env("HOME", &env.user_home)
        .stdin(Stdio::null())
        .output()
        .unwrap();
    assert_eq!(output.status.code(), Some(1));
    assert!(
        String::from_utf8_lossy(&output.stderr)
            .contains("@file arguments are not supported in RPC mode")
    );
}
