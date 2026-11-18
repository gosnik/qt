//! End-to-end runs of the real `qt-cli` binary against a mock
//! OpenAI-compatible server (configured as an Ollama instance on
//! 127.0.0.1): Pi JSON event lines, extension tools, MCP proxy tools, index
//! tools, subagent children (the same binary) and governed memory.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::sync::{Arc, Mutex};

use serde_json::{Value, json};

type Responder = Arc<dyn Fn(&Value) -> String + Send + Sync>;

/// A mock chat-completions server: every POST body is recorded and answered
/// with the SSE stream `respond` returns.
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
                    let reply = if line.starts_with("POST") {
                        let value: Value = serde_json::from_slice(&body).unwrap_or(Value::Null);
                        recorded.lock().unwrap().push(value.clone());
                        let sse = respond(&value);
                        format!(
                            "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\nconnection: close\r\n\r\n{sse}"
                        )
                    } else {
                        let body = r#"{"data":[],"models":[]}"#;
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

fn tool_reply(id: &str, name: &str, args: Value) -> String {
    sse(&[
        json!({"choices":[{"delta":{"tool_calls":[{"index":0,"id":id,"function":{"name":name,"arguments":args.to_string()}}]}}]}),
        json!({"choices":[{"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":12,"completion_tokens":3,"total_tokens":15}}),
    ])
}

fn system_prompt(request: &Value) -> String {
    request["messages"][0]["content"]
        .as_str()
        .unwrap_or_default()
        .to_string()
}

fn tool_result(request: &Value) -> Option<String> {
    request["messages"]
        .as_array()?
        .iter()
        .rev()
        .find(|m| m["role"] == "tool")
        .map(|m| m["content"].to_string())
}

fn tool_names(request: &Value) -> Vec<String> {
    request["tools"]
        .as_array()
        .map(|tools| {
            tools
                .iter()
                .filter_map(|t| t["function"]["name"].as_str().map(str::to_string))
                .collect()
        })
        .unwrap_or_default()
}

struct Env {
    _dir: tempfile::TempDir,
    home: PathBuf,
    work: PathBuf,
    user_home: PathBuf,
}

fn env(port: u16, extra: Value) -> Env {
    let dir = tempfile::tempdir().unwrap();
    let home = dir.path().join("agent");
    let work = dir.path().join("work");
    let user_home = dir.path().join("user");
    for d in [&home, &work, &user_home] {
        std::fs::create_dir_all(d).unwrap();
    }
    let mut settings = json!({
        "ollama": {"instances": [{"id": "mock", "baseUrl": format!("http://127.0.0.1:{port}")}]},
        "defaultProvider": "ollama:mock",
        "defaultModel": "m",
        "retry": {"enabled": false, "provider": {"maxRetries": 0}},
    });
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

fn qt_cli() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_qt-cli"))
}

fn run(env: &Env, args: &[&str], stdin: Option<&str>) -> Output {
    let mut command = Command::new(qt_cli());
    command
        .args(args)
        .current_dir(&env.work)
        .env("QT_CODING_AGENT_DIR", &env.home)
        .env("HOME", &env.user_home)
        .env("QT_SUBAGENT_EXECUTABLE", qt_cli())
        .env_remove("PI_CODING_AGENT_SESSION_DIR")
        .env_remove("QT_SUBAGENT_DEPTH")
        .env_remove("QT_SUBAGENT_CHILD")
        .env_remove("PI_OFFLINE")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = command.spawn().expect("spawn qt-cli");
    let mut pipe = child.stdin.take().unwrap();
    if let Some(input) = stdin {
        pipe.write_all(input.as_bytes()).unwrap();
    }
    drop(pipe);
    child.wait_with_output().unwrap()
}

fn json_lines(output: &Output) -> Vec<Value> {
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| serde_json::from_str(l).unwrap_or_else(|e| panic!("not JSON ({e}): {l}")))
        .collect()
}

fn types(lines: &[Value]) -> Vec<String> {
    lines
        .iter()
        .map(|l| l["type"].as_str().unwrap_or_default().to_string())
        .collect()
}

fn stderr(output: &Output) -> String {
    String::from_utf8_lossy(&output.stderr).into_owned()
}

#[test]
fn json_mode_prints_pi_event_lines_and_exit_codes() {
    let server = MockServer::start(Arc::new(|_| text_reply("Hello from mock")));
    let env = env(server.port, json!({}));
    let output = run(
        &env,
        &["--mode", "json", "--no-session", "Say hello"],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let lines = json_lines(&output);
    assert_eq!(lines[0]["type"], "session");
    assert_eq!(lines[0]["version"], 3);
    let kinds = types(&lines);
    for expected in [
        "agent_start",
        "turn_start",
        "message_start",
        "message_update",
        "message_end",
        "turn_end",
        "agent_end",
    ] {
        assert!(
            kinds.iter().any(|k| k == expected),
            "missing {expected}: {kinds:?}"
        );
    }
    assert_eq!(kinds.last().map(String::as_str), Some("agent_end"));
    let update = lines
        .iter()
        .find(|l| l["type"] == "message_update")
        .unwrap();
    assert!(
        update["assistantMessageEvent"]["type"].is_string(),
        "{update}"
    );
    let end = lines
        .iter()
        .rev()
        .find(|l| l["type"] == "message_end" && l["message"]["role"] == "assistant")
        .unwrap();
    assert_eq!(end["message"]["content"][0]["text"], "Hello from mock");
    assert_eq!(end["message"]["stopReason"], "stop");
    assert_eq!(end["message"]["usage"]["totalTokens"], 15);

    // Print mode prints only the final answer; stdin becomes the prompt.
    let printed = run(&env, &["-p", "--no-session"], Some("from stdin\n"));
    assert!(printed.status.success(), "{}", stderr(&printed));
    assert_eq!(
        String::from_utf8_lossy(&printed.stdout),
        "Hello from mock\n"
    );
    let last = server.requests().last().cloned().unwrap();
    let user = last["messages"].as_array().unwrap().last().unwrap().clone();
    assert_eq!(user["content"], "from stdin");

    // A provider error is exit code 1 with Pi's error stop reason.
    let failing = MockServer::start(Arc::new(|_| {
        sse(&[json!({"choices":[{"delta":{},"finish_reason":"content_filter"}]})])
    }));
    let env = self::env(failing.port, json!({}));
    let output = run(&env, &["--mode", "json", "--no-session", "x"], Some(""));
    assert_eq!(output.status.code(), Some(1));
    let lines = json_lines(&output);
    let end = lines
        .iter()
        .rev()
        .find(|l| l["type"] == "message_end" && l["message"]["role"] == "assistant")
        .unwrap();
    assert_eq!(end["message"]["stopReason"], "error");
    let printed = run(&env, &["-p", "--no-session", "x"], Some(""));
    assert_eq!(printed.status.code(), Some(1));
    assert!(stderr(&printed).contains("error: "), "{}", stderr(&printed));
}

#[test]
fn cli_flags_select_models_tools_and_sessions() {
    let server = MockServer::start(Arc::new(|_| text_reply("ok")));
    let env = env(server.port, json!({}));
    // Model with provider prefix and thinking suffix; tool allowlist.
    let output = run(
        &env,
        &[
            "--mode",
            "json",
            "--model",
            "ollama:mock/other-model:low",
            "--tools",
            "read,symbol,index",
            "first",
            "second",
        ],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let requests = server.requests();
    assert_eq!(requests.len(), 2, "two messages run one after the other");
    assert_eq!(requests[0]["model"], "other-model");
    let mut names = tool_names(&requests[0]);
    names.sort();
    assert_eq!(names, ["index", "read", "symbol"]);
    // The session was saved and can be continued.
    let continued = run(&env, &["--mode", "json", "-c", "third"], Some(""));
    assert!(continued.status.success(), "{}", stderr(&continued));
    let last = server.requests().last().cloned().unwrap();
    let users = last["messages"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|m| m["role"] == "user")
        .count();
    assert_eq!(users, 3, "{last}");

    // Index, subagent and the built-in tools are offered by default.
    let output = run(&env, &["--mode", "json", "--no-session", "x"], Some(""));
    assert!(output.status.success());
    let names = tool_names(server.requests().last().unwrap());
    for expected in [
        "read",
        "bash",
        "symbol",
        "analyze_file",
        "references",
        "build_context",
        "search_index",
        "index",
        "subagent",
    ] {
        assert!(
            names.iter().any(|n| n == expected),
            "missing {expected}: {names:?}"
        );
    }
    // --no-builtin-tools drops them; --no-tools sends none.
    run(&env, &["-p", "--no-session", "-nbt", "x"], Some(""));
    assert!(tool_names(server.requests().last().unwrap()).is_empty());

    // Unknown options and conflicting session flags are errors.
    let bad = run(&env, &["--no-session", "--frobnicate", "x"], Some(""));
    assert_eq!(bad.status.code(), Some(1));
    assert!(
        stderr(&bad).contains("Unknown option: --frobnicate"),
        "{}",
        stderr(&bad)
    );
    let bad = run(&env, &["--fork", "abc", "-c"], Some(""));
    assert_eq!(bad.status.code(), Some(1));
    assert!(stderr(&bad).contains("--fork cannot be combined with --continue"));
    let bad = run(&env, &["--session", "zzzz", "-p", "x"], Some(""));
    assert_eq!(bad.status.code(), Some(1));
    assert!(stderr(&bad).contains("No session found matching 'zzzz'"));
    let bad = run(&env, &["--api-key", "k", "-p", "x"], Some(""));
    assert!(stderr(&bad).contains("--api-key requires a model"));

    // --version, --help and --export.
    let version = run(&env, &["--version"], None);
    assert_eq!(
        String::from_utf8_lossy(&version.stdout).trim(),
        env!("CARGO_PKG_VERSION")
    );
    let help = run(&env, &["--help"], Some(""));
    assert!(stderr(&help).contains("--list-models [search]"));
    fn find_jsonl(dir: &Path) -> Option<PathBuf> {
        for entry in std::fs::read_dir(dir).ok()?.flatten() {
            let path = entry.path();
            if path.is_dir() {
                if let Some(found) = find_jsonl(&path) {
                    return Some(found);
                }
            } else if path.extension().is_some_and(|e| e == "jsonl") {
                return Some(path);
            }
        }
        None
    }
    let session = find_jsonl(&env.home.join("sessions")).expect("a saved session");
    let exported = run(
        &env,
        &["--export", session.to_str().unwrap(), "out.html"],
        None,
    );
    assert!(exported.status.success(), "{}", stderr(&exported));
    assert!(
        std::fs::read_to_string(env.work.join("out.html"))
            .unwrap()
            .contains("<html")
    );
}

const EXTENSION: &str = r#"
fn activate() {
    registerTool(#{
        name: "shout",
        description: "Upper-case a text",
        parameters: #{ type: "object", properties: #{ text: #{ type: "string" } }, required: ["text"] },
    });
    registerFlag("loud", "boolean", false, "Shout louder");
}

fn tool_shout(args) {
    let text = args.text;
    text.make_upper();
    if pi::getFlag(#{ name: "loud" }) { text += "!!!"; }
    #{ content: [#{ type: "text", text: text }] }
}
"#;

#[test]
fn extension_tool_and_flag_are_used_in_a_run() {
    let server = MockServer::start(Arc::new(|request: &Value| match tool_result(request) {
        None => tool_reply("call_1", "shout", json!({"text": "quiet words"})),
        Some(result) => text_reply(&format!("tool said {result}")),
    }));
    let env = env(server.port, json!({}));
    let script = env.work.join("shout.rhai");
    std::fs::write(&script, EXTENSION).unwrap();
    let output = run(
        &env,
        &[
            "--mode",
            "json",
            "--no-session",
            "-e",
            "shout.rhai",
            "--loud",
            "go",
        ],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let lines = json_lines(&output);
    let end = lines
        .iter()
        .find(|l| l["type"] == "tool_execution_end")
        .expect("tool ran");
    assert_eq!(end["toolName"], "shout");
    assert!(
        end["result"].to_string().contains("QUIET WORDS!!!"),
        "{end}"
    );
    assert!(tool_names(&server.requests()[0]).contains(&"shout".to_string()));
    // An unregistered flag is still an error.
    let bad = run(
        &env,
        &["--no-session", "-e", "shout.rhai", "--quiet", "go"],
        Some(""),
    );
    assert!(
        stderr(&bad).contains("Unknown option: --quiet"),
        "{}",
        stderr(&bad)
    );
}

const INPUT_EXTENSION: &str = r#"
fn activate() {
    on("input", "on_input");
    registerCommand("ping", "Ping", "Answer pong");
}

fn on_input(event) {
    if event.text == "skip me" {
        return #{ action: "handled" };
    }
    if event.text.starts_with("rewrite:") {
        return #{ action: "transform", text: "rewritten from " + event.source };
    }
    #{ action: "continue" }
}

fn command_ping(args) { #{ text: "pong " + args.args } }
"#;

/// The user texts the model saw, one per request.
fn last_user_texts(server: &MockServer) -> Vec<String> {
    server
        .requests()
        .iter()
        .filter_map(|r| {
            r["messages"]
                .as_array()?
                .iter()
                .rev()
                .find(|m| m["role"] == "user")
                .map(|m| match &m["content"] {
                    Value::String(s) => s.clone(),
                    other => other.to_string(),
                })
        })
        .collect()
}

#[test]
fn print_mode_runs_extension_commands_input_hooks_and_templates() {
    let server = MockServer::start(Arc::new(|_: &Value| text_reply("ok")));
    let env = env(server.port, json!({}));
    std::fs::write(env.work.join("hooks.rhai"), INPUT_EXTENSION).unwrap();
    let prompts = env.work.join(".pi/prompts");
    std::fs::create_dir_all(&prompts).unwrap();
    std::fs::write(
        prompts.join("hi.md"),
        "---\ndescription: hi\n---\nSay hello to $1",
    )
    .unwrap();
    let output = run(
        &env,
        &[
            "--no-session",
            "-e",
            "hooks.rhai",
            "-p",
            "/ping there",
            "skip me",
            "rewrite: this",
            "/hi Ada",
        ],
        None,
    );
    assert!(output.status.success(), "{}", stderr(&output));
    assert!(
        stderr(&output).contains("pong there"),
        "{}",
        stderr(&output)
    );
    // The command and the handled input never reach the model; the
    // transformed input and the expanded template do (Pi `prompt`).
    assert_eq!(
        last_user_texts(&server),
        ["rewritten from interactive", "Say hello to Ada"]
    );
}

#[test]
fn astra_auto_compact_example_compacts_before_the_next_input() {
    let usage = |tokens: u64| {
        Arc::new(move |request: &Value| {
            let text = if request["tools"].is_array() {
                "answer"
            } else {
                "summary of the work"
            };
            sse(&[
                json!({"choices":[{"delta":{"content":text}}]}),
                json!({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":tokens,"completion_tokens":3,"total_tokens":tokens + 3}}),
            ])
        }) as Responder
    };
    let example = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../examples/extensions/astra-auto-compact.rhai")
        .canonicalize()
        .unwrap();
    for (model, tokens, compacts) in [
        ("Astra-1", 250_000, true),
        ("Astra-1", 150_000, false),
        ("other", 250_000, false),
    ] {
        let server = MockServer::start(usage(tokens));
        let env = env(
            server.port,
            json!({
                "ollama": {"instances": [{"id": "mock", "baseUrl": format!("http://127.0.0.1:{}", server.port), "models": [{"name": model, "contextWindow": 1_000_000}]}]},
                "defaultModel": model,
            }),
        );
        let sessions = env.work.join("sessions");
        let output = run(
            &env,
            &[
                "--session-dir",
                sessions.to_str().unwrap(),
                "-e",
                example.to_str().unwrap(),
                "-p",
                "first",
                "second",
            ],
            None,
        );
        assert!(output.status.success(), "{}", stderr(&output));
        let session = std::fs::read_dir(&sessions)
            .unwrap()
            .map(|e| e.unwrap().path())
            .find(|p| p.extension().is_some_and(|x| x == "jsonl"))
            .expect("session file");
        let kinds: Vec<String> = std::fs::read_to_string(session)
            .unwrap()
            .lines()
            .map(|l| serde_json::from_str::<Value>(l).unwrap()["type"].to_string())
            .collect();
        assert_eq!(
            kinds.iter().any(|k| k == "\"compaction\""),
            compacts,
            "{model} at {tokens}: {kinds:?}"
        );
    }
}

fn mcp_mock_server() -> Option<PathBuf> {
    let exe = qt_cli();
    let dir = exe.parent()?;
    let candidate = dir.join(format!(
        "qt-mcp-mock-server{}",
        std::env::consts::EXE_SUFFIX
    ));
    candidate.is_file().then_some(candidate)
}

#[test]
fn mcp_proxy_tool_is_offered_and_called() {
    let Some(mock) = mcp_mock_server() else {
        panic!(
            "qt-mcp-mock-server is not built; run `cargo build -p qt-mcp --bin qt-mcp-mock-server` (cargo test --workspace builds it)"
        );
    };
    let server = MockServer::start(Arc::new(|request: &Value| match tool_result(request) {
        None => tool_reply("call_1", "mcp_mock_echo", json!({"text": "via mcp"})),
        Some(result) => text_reply(&format!("echoed {result}")),
    }));
    let env = env(
        server.port,
        json!({"mcpServers": {"mock": {"command": mock.to_string_lossy(), "args": []}}}),
    );
    let output = run(
        &env,
        &["--mode", "json", "--no-session", "use mcp"],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let names = tool_names(&server.requests()[0]);
    assert!(names.contains(&"mcp_mock_echo".to_string()), "{names:?}");
    assert!(
        names.contains(&"list_mcp_resources".to_string()),
        "{names:?}"
    );
    let lines = json_lines(&output);
    let end = lines
        .iter()
        .find(|l| l["type"] == "tool_execution_end")
        .unwrap();
    assert_eq!(end["isError"], false, "{end}");
    assert!(end["result"].to_string().contains("via mcp"), "{end}");
}

#[test]
fn subagent_runs_the_real_cli_as_a_child() {
    let server = MockServer::start(Arc::new(|request: &Value| {
        if system_prompt(request).contains("read-only scouting agent") {
            return text_reply("CHILD-FINDINGS: main.c defines main");
        }
        match tool_result(request) {
            None => tool_reply(
                "call_1",
                "subagent",
                json!({"agent": "scout", "task": "Find main"}),
            ),
            Some(result) => text_reply(&format!("PARENT-DONE {result}")),
        }
    }));
    let env = env(server.port, json!({}));
    let output = run(
        &env,
        &["--mode", "json", "--no-session", "delegate"],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let requests = server.requests();
    let child = requests
        .iter()
        .find(|r| system_prompt(r).contains("read-only scouting agent"))
        .expect("the child asked the model");
    let user = child["messages"]
        .as_array()
        .unwrap()
        .last()
        .unwrap()
        .clone();
    assert!(
        user["content"].to_string().contains("Task: Find main"),
        "{user}"
    );
    let lines = json_lines(&output);
    let end = lines
        .iter()
        .find(|l| l["type"] == "tool_execution_end" && l["toolName"] == "subagent")
        .unwrap();
    assert_eq!(end["isError"], false, "{end}");
    assert!(
        end["result"].to_string().contains("CHILD-FINDINGS"),
        "{end}"
    );
    let last = requests.last().unwrap();
    assert!(tool_result(last).unwrap().contains("CHILD-FINDINGS"));
}

#[test]
fn json_mode_waits_for_persistent_workers_and_delivers_completions() {
    let server = MockServer::start(Arc::new(|request: &Value| {
        if system_prompt(request).contains("read-only scouting agent") {
            return text_reply("WORKER-REPORT");
        }
        let last = request["messages"]
            .as_array()
            .and_then(|m| m.last().cloned())
            .unwrap_or_default();
        if last["role"] == "user" && last["content"].to_string().contains("subagent_complete") {
            return text_reply("FOLLOWUP-DONE");
        }
        match tool_result(request) {
            None => tool_reply(
                "call_1",
                "subagent",
                json!({"action": "spawn", "name": "w1", "agent": "scout", "task": "Find main"}),
            ),
            Some(_) => text_reply("spawned"),
        }
    }));
    let env = env(server.port, json!({}));
    let output = run(
        &env,
        &["--mode", "json", "--no-session", "start a worker"],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let lines = json_lines(&output);
    let frames: Vec<&Value> = lines
        .iter()
        .filter(|l| l["type"] == "subagent_event")
        .collect();
    let complete = frames
        .iter()
        .find(|f| f["subagent"]["type"] == "subagent_complete")
        .unwrap_or_else(|| panic!("no completion frame: {:?}", types(&lines)));
    assert_eq!(complete["subagent"]["child_id"], "/root/w1");
    assert!(
        complete["subagent"]["output"]
            .to_string()
            .contains("WORKER-REPORT"),
        "{complete}"
    );
    // The completion ran as a follow-up turn after the frames.
    let last_end = lines
        .iter()
        .rev()
        .find(|l| l["type"] == "message_end" && l["message"]["role"] == "assistant")
        .unwrap();
    assert_eq!(last_end["message"]["content"][0]["text"], "FOLLOWUP-DONE");
}

fn seed_memory(home: &Path, work: &Path, content: &str) -> String {
    use qt_memory::model::{MemoryStatus, MemoryType, MemoryWrite, Provenance, SourceType};
    let store = qt_memory::MemoryStore::open(home).unwrap();
    let scope = qt_memory::durable_memory_scope(qt_memory::cli::command_scope(work, false));
    store
        .add(
            &scope,
            MemoryWrite {
                content: content.into(),
                memory_type: MemoryType::Procedural,
                status: MemoryStatus::Active,
                provenance: Provenance {
                    source_type: SourceType::File,
                    source_uri: "file:notes.md".into(),
                    evidence_strength: 0.8,
                    ..Default::default()
                },
                ..Default::default()
            },
        )
        .unwrap()
}

#[test]
fn memory_recall_is_injected_and_the_run_is_captured() {
    let server = MockServer::start(Arc::new(|request: &Value| {
        if system_prompt(request).contains("memory pre-handler") {
            // Not a valid protocol answer: the parent falls back to the
            // deterministic context.
            return text_reply("{}");
        }
        text_reply("Use cargo nextest to run the tests.")
    }));
    let env = env(server.port, json!({"memory": {"enabled": true}}));
    let id = seed_memory(
        &env.home,
        &env.work,
        "Run the test suite with cargo nextest run",
    );
    let output = run(
        &env,
        &[
            "-p",
            "--no-session",
            "How do I run the test suite with cargo?",
        ],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let parent = server
        .requests()
        .into_iter()
        .rev()
        .find(|r| !system_prompt(r).contains("memory pre-handler"))
        .unwrap();
    let text = parent["messages"].to_string();
    assert!(text.contains("qt_memory_context"), "{text}");
    assert!(text.contains(&id), "{text}");
    assert!(text.contains("cargo nextest run"), "{text}");
    assert!(tool_names(&parent).contains(&"memory_recall".to_string()));

    let db = rusqlite::Connection::open(env.home.join("memory/memory.sqlite3")).unwrap();
    let captured: i64 = db
        .query_row(
            "SELECT count(*) FROM session_events WHERE event_type = 'completed_episode'",
            [],
            |row| row.get(0),
        )
        .unwrap();
    assert!(captured >= 1, "no completed_episode event recorded");

    // --no-memory disables recall for the invocation.
    let before = server.requests().len();
    let output = run(
        &env,
        &["-p", "--no-session", "--no-memory", "tests?"],
        Some(""),
    );
    assert!(output.status.success(), "{}", stderr(&output));
    let requests = server.requests();
    assert_eq!(
        requests.len(),
        before + 1,
        "no pre-handler child without memory"
    );
    assert!(
        !requests.last().unwrap()["messages"]
            .to_string()
            .contains("qt_memory_context")
    );
}

#[test]
fn memory_and_package_subcommands() {
    let env = env(1, json!({"memory": {"enabled": true}}));
    let output = run(&env, &["memory", "status"], None);
    assert!(output.status.success(), "{}", stderr(&output));
    assert!(!output.stdout.is_empty());
    let output = run(&env, &["list"], None);
    assert!(output.status.success(), "{}", stderr(&output));
    let output = run(&env, &["memory", "bogus"], None);
    assert_eq!(output.status.code(), Some(1));
}

#[test]
fn list_models_offline_table() {
    let env = env(
        1,
        json!({"ollama": {"instances": [{"id": "mock", "baseUrl": "http://127.0.0.1:1", "models": [{"name": "alpha"}, {"name": "beta"}]}]}}),
    );
    let output = run(&env, &["--list-models", "--offline"], None);
    assert!(output.status.success(), "{}", stderr(&output));
    let text = String::from_utf8_lossy(&output.stdout).into_owned();
    assert!(text.starts_with("provider"), "{text}");
    assert!(
        text.contains("ollama:mock") && text.contains("alpha"),
        "{text}"
    );
    let output = run(&env, &["--list-models", "zzzz", "--offline"], None);
    assert_eq!(
        String::from_utf8_lossy(&output.stdout),
        "No models matching \"zzzz\"\n"
    );
}
