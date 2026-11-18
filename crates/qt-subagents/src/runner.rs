//! Child agent execution.
//!
//! Every subagent turn runs in a separate `qt-cli --mode json` process (the
//! archived `IsolatedAgentRunner`). The child prints Pi-shaped JSONL events on
//! stdout; the `agent_end` event's `messages` carry the final output.
//! [`ChildRunner`] abstracts the launch so tests and embedders can substitute
//! their own executor.

use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::Arc;
use std::time::Duration;

use futures::future::BoxFuture;
use serde_json::Value;
use tokio::io::{AsyncBufReadExt as _, AsyncReadExt as _, AsyncWriteExt as _, BufReader};
use tokio::process::Command;
use tokio_util::sync::CancellationToken;

/// Environment variable naming the child executable explicitly.
pub const EXECUTABLE_ENV: &str = "QT_SUBAGENT_EXECUTABLE";
/// Name of the headless binary launched for children.
pub const CHILD_BINARY: &str = "qt-cli";
/// stderr kept from a child (the tail beyond it is dropped).
const MAX_STDERR_BYTES: usize = 256 * 1024;

/// One child launch.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ChildRequest {
    /// Arguments after the executable (and its fixed prefix arguments).
    pub arguments: Vec<String>,
    pub cwd: Option<PathBuf>,
    /// Extra environment: `Some(value)` sets, `None` removes the variable.
    pub environment: Vec<(String, Option<String>)>,
    /// Written to the child's stdin, which is then closed.
    pub stdin: Option<String>,
    pub timeout_ms: Option<u64>,
}

impl ChildRequest {
    /// Set (or replace) an environment variable.
    pub fn set_env(&mut self, name: &str, value: impl Into<String>) {
        self.environment.retain(|(n, _)| n != name);
        self.environment
            .push((name.to_string(), Some(value.into())));
    }

    /// Value set for `name` by this request.
    pub fn env(&self, name: &str) -> Option<&str> {
        self.environment
            .iter()
            .rev()
            .find(|(n, _)| n == name)
            .and_then(|(_, v)| v.as_deref())
    }
}

/// Live progress of a running child: one per JSON event on stdout.
#[derive(Debug, Clone, PartialEq)]
pub struct ChildUpdate {
    pub event: Value,
    /// Latest assistant text seen so far (`message_end` or `agent_end`).
    pub streaming_output: String,
}

pub type ChildUpdateFn = Arc<dyn Fn(ChildUpdate) + Send + Sync>;

/// What a finished child produced.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ChildOutcome {
    pub exit_code: i32,
    pub cancelled: bool,
    pub timed_out: bool,
    pub stdout: String,
    pub stderr: String,
    /// Every JSON event printed on stdout.
    pub events: Vec<Value>,
    /// `messages` of the last `agent_end` event (an array, empty when absent).
    pub final_messages: Value,
    /// Text of the last assistant message with text in `final_messages`.
    pub final_output: String,
    /// Non-JSON stdout lines that were ignored.
    pub diagnostics: Vec<String>,
}

impl ChildOutcome {
    /// Exited with status 0 without being cancelled or timing out.
    pub fn completed(&self) -> bool {
        self.exit_code == 0 && !self.cancelled && !self.timed_out
    }

    /// An `agent_end` event with a `messages` array was printed.
    pub fn has_final_result(&self) -> bool {
        self.events.iter().any(|e| {
            e.get("type").and_then(Value::as_str) == Some("agent_end")
                && e.get("messages").is_some_and(Value::is_array)
        })
    }
}

/// Launches child agents.
pub trait ChildRunner: Send + Sync {
    /// Run one child to completion. `cancel` terminates it (the outcome is
    /// then marked `cancelled`). `Err` means the child could not be started.
    fn run(
        &self,
        request: ChildRequest,
        cancel: CancellationToken,
        on_update: Option<ChildUpdateFn>,
    ) -> BoxFuture<'static, Result<ChildOutcome, String>>;
}

/// Text of an assistant message's `text` blocks joined by newlines.
pub fn assistant_text(message: &Value) -> String {
    if message.get("role").and_then(Value::as_str) != Some("assistant") {
        return String::new();
    }
    let Some(content) = message.get("content").and_then(Value::as_array) else {
        return String::new();
    };
    let mut out = String::new();
    for block in content {
        if block.get("type").and_then(Value::as_str) != Some("text") {
            continue;
        }
        if let Some(text) = block.get("text").and_then(Value::as_str) {
            if !out.is_empty() {
                out.push('\n');
            }
            out.push_str(text);
        }
    }
    out
}

/// Final output of an `agent_end` messages array: the last non-empty
/// assistant text.
pub fn final_output_of(messages: &Value) -> String {
    let mut output = String::new();
    for message in messages.as_array().into_iter().flatten() {
        let text = assistant_text(message);
        if !text.is_empty() {
            output = text;
        }
    }
    output
}

/// Incremental parser of a child's JSONL stdout.
#[derive(Debug, Default)]
struct EventParser {
    line: usize,
    events: Vec<Value>,
    final_messages: Option<Value>,
    final_output: String,
    streaming_output: String,
    diagnostics: Vec<String>,
}

impl EventParser {
    /// Parse one stdout line; returns the event when it was JSON.
    fn feed(&mut self, raw: &str) -> Option<Value> {
        self.line += 1;
        let line = raw.strip_suffix('\r').unwrap_or(raw);
        if line.is_empty() {
            return None;
        }
        match serde_json::from_str::<Value>(line) {
            Ok(event) => {
                match event.get("type").and_then(Value::as_str) {
                    Some("agent_end") => {
                        if let Some(messages) = event.get("messages").filter(|m| m.is_array()) {
                            self.final_output = final_output_of(messages);
                            if !self.final_output.is_empty() {
                                self.streaming_output = self.final_output.clone();
                            }
                            self.final_messages = Some(messages.clone());
                        }
                    }
                    Some("message_end") => {
                        if let Some(message) = event.get("message") {
                            let text = assistant_text(message);
                            if !text.is_empty() {
                                self.streaming_output = text;
                            }
                        }
                    }
                    _ => {}
                }
                self.events.push(event.clone());
                Some(event)
            }
            Err(e) => {
                self.diagnostics.push(format!(
                    "ignored non-JSON child stdout line {}: {e}",
                    self.line
                ));
                None
            }
        }
    }
}

/// Parse a complete JSONL stdout into `(events, final_messages, final_output,
/// diagnostics)`.
pub fn parse_json_lines(stdout: &str) -> (Vec<Value>, Value, String, Vec<String>) {
    let mut parser = EventParser::default();
    for line in stdout.split('\n') {
        parser.feed(line);
    }
    (
        parser.events,
        parser
            .final_messages
            .unwrap_or_else(|| Value::Array(Vec::new())),
        parser.final_output,
        parser.diagnostics,
    )
}

/// Resolve the child executable: `QT_SUBAGENT_EXECUTABLE`, else `qt-cli`
/// next to the running executable, else `qt-cli` found on `PATH`, else the
/// bare name (the launch then reports that it cannot be found).
pub fn resolve_child_executable() -> PathBuf {
    resolve_child_executable_from(
        std::env::var_os(EXECUTABLE_ENV),
        std::env::current_exe().ok(),
        std::env::var_os("PATH"),
    )
}

/// [`resolve_child_executable`] with explicit inputs.
pub fn resolve_child_executable_from(
    configured: Option<OsString>,
    current_exe: Option<PathBuf>,
    path_var: Option<OsString>,
) -> PathBuf {
    if let Some(configured) = configured.filter(|v| !v.is_empty()) {
        return PathBuf::from(configured);
    }
    let file_name = format!("{CHILD_BINARY}{}", std::env::consts::EXE_SUFFIX);
    if let Some(sibling) = current_exe
        .as_deref()
        .and_then(Path::parent)
        .map(|dir| dir.join(&file_name))
        .filter(|p| p.is_file())
    {
        return sibling;
    }
    if let Some(path_var) = path_var {
        for dir in std::env::split_paths(&path_var) {
            let candidate = dir.join(&file_name);
            if candidate.is_file() {
                return candidate;
            }
        }
    }
    PathBuf::from(file_name)
}

/// Runs children as separate processes of the qt CLI.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProcessChildRunner {
    pub executable: PathBuf,
    /// Fixed arguments placed before every request's arguments.
    pub arguments: Vec<String>,
    /// Environment applied to every child before the request's own.
    pub environment: Vec<(String, String)>,
}

impl ProcessChildRunner {
    pub fn new(executable: impl Into<PathBuf>) -> Self {
        Self {
            executable: executable.into(),
            arguments: Vec::new(),
            environment: Vec::new(),
        }
    }

    /// Runner for [`resolve_child_executable`].
    pub fn discover() -> Self {
        Self::new(resolve_child_executable())
    }

    pub fn with_arguments(mut self, arguments: Vec<String>) -> Self {
        self.arguments = arguments;
        self
    }

    pub fn with_env(mut self, name: impl Into<String>, value: impl Into<String>) -> Self {
        self.environment.push((name.into(), value.into()));
        self
    }
}

#[cfg(unix)]
fn kill_process_tree(child: &mut tokio::process::Child) {
    if let Some(pid) = child.id() {
        // The child leads its own process group; take its descendants too.
        // SAFETY: plain syscall on a process group id we created.
        unsafe {
            libc::kill(-(pid as libc::pid_t), libc::SIGKILL);
        }
    }
    let _ = child.start_kill();
}

#[cfg(not(unix))]
fn kill_process_tree(child: &mut tokio::process::Child) {
    let _ = child.start_kill();
}

fn exit_code(status: std::process::ExitStatus) -> i32 {
    if let Some(code) = status.code() {
        return code;
    }
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt as _;
        if let Some(signal) = status.signal() {
            return 128 + signal;
        }
    }
    -1
}

impl ChildRunner for ProcessChildRunner {
    fn run(
        &self,
        request: ChildRequest,
        cancel: CancellationToken,
        on_update: Option<ChildUpdateFn>,
    ) -> BoxFuture<'static, Result<ChildOutcome, String>> {
        let runner = self.clone();
        Box::pin(async move { runner.run_process(request, cancel, on_update).await })
    }
}

impl ProcessChildRunner {
    async fn run_process(
        &self,
        request: ChildRequest,
        cancel: CancellationToken,
        on_update: Option<ChildUpdateFn>,
    ) -> Result<ChildOutcome, String> {
        let mut command = Command::new(&self.executable);
        command
            .args(&self.arguments)
            .args(&request.arguments)
            .stdin(if request.stdin.is_some() {
                Stdio::piped()
            } else {
                Stdio::null()
            })
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true);
        if let Some(cwd) = &request.cwd {
            command.current_dir(cwd);
        }
        for (name, value) in &self.environment {
            command.env(name, value);
        }
        for (name, value) in &request.environment {
            match value {
                Some(value) => command.env(name, value),
                None => command.env_remove(name),
            };
        }
        #[cfg(unix)]
        command.process_group(0);
        let mut child = command.spawn().map_err(|e| {
            format!(
                "cannot start subagent executable '{}': {e}",
                self.executable.display()
            )
        })?;

        if let (Some(payload), Some(mut stdin)) = (request.stdin.clone(), child.stdin.take()) {
            tokio::spawn(async move {
                let _ = stdin.write_all(payload.as_bytes()).await;
                let _ = stdin.shutdown().await;
            });
        }
        let stdout = child.stdout.take();
        let stderr = child.stderr.take();

        let read_stdout = async move {
            let mut parser = EventParser::default();
            let mut text = String::new();
            let Some(stdout) = stdout else {
                return (parser, text);
            };
            let mut reader = BufReader::new(stdout);
            let mut buf = Vec::new();
            loop {
                buf.clear();
                match reader.read_until(b'\n', &mut buf).await {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {
                        let line = String::from_utf8_lossy(&buf).into_owned();
                        text.push_str(&line);
                        let trimmed = line.strip_suffix('\n').unwrap_or(&line);
                        if let Some(event) = parser.feed(trimmed)
                            && let Some(update) = &on_update
                        {
                            update(ChildUpdate {
                                event,
                                streaming_output: parser.streaming_output.clone(),
                            });
                        }
                    }
                }
            }
            (parser, text)
        };
        let read_stderr = async move {
            let mut collected = Vec::new();
            if let Some(mut stderr) = stderr {
                let mut chunk = [0u8; 8192];
                loop {
                    match stderr.read(&mut chunk).await {
                        Ok(0) | Err(_) => break,
                        Ok(n) => {
                            collected.extend_from_slice(&chunk[..n]);
                            if collected.len() > MAX_STDERR_BYTES {
                                let excess = collected.len() - MAX_STDERR_BYTES;
                                collected.drain(..excess);
                            }
                        }
                    }
                }
            }
            String::from_utf8_lossy(&collected).into_owned()
        };
        let timeout = request.timeout_ms.map(Duration::from_millis);
        let wait = async {
            let sleep = async {
                match timeout {
                    Some(t) => tokio::time::sleep(t).await,
                    None => std::future::pending().await,
                }
            };
            tokio::select! {
                status = child.wait() => (status.ok(), false, false),
                _ = cancel.cancelled() => {
                    kill_process_tree(&mut child);
                    (child.wait().await.ok(), true, false)
                }
                _ = sleep => {
                    kill_process_tree(&mut child);
                    (child.wait().await.ok(), false, true)
                }
            }
        };
        let ((status, cancelled, timed_out), (parser, stdout_text), stderr_text) =
            tokio::join!(wait, read_stdout, read_stderr);
        Ok(ChildOutcome {
            exit_code: status.map(exit_code).unwrap_or(-1),
            cancelled,
            timed_out,
            stdout: stdout_text,
            stderr: stderr_text,
            events: parser.events,
            final_messages: parser
                .final_messages
                .unwrap_or_else(|| Value::Array(Vec::new())),
            final_output: parser.final_output,
            diagnostics: parser.diagnostics,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn parses_agent_end_and_diagnostics() {
        let stdout = [
            json!({"type": "agent_start"}).to_string(),
            "not json".to_string(),
            json!({"type": "message_end", "message": {"role": "assistant", "content": [{"type": "text", "text": "draft"}]}}).to_string(),
            json!({"type": "agent_end", "messages": [
                {"role": "user", "content": [{"type": "text", "text": "Task: x"}]},
                {"role": "assistant", "content": [{"type": "text", "text": "a"}, {"type": "toolCall", "id": "1", "name": "ls", "arguments": {}}, {"type": "text", "text": "b"}]},
                {"role": "toolResult", "toolCallId": "1", "content": [{"type": "text", "text": "listing"}]},
                {"role": "assistant", "content": []}
            ]})
            .to_string(),
        ]
        .join("\r\n");
        let (events, messages, output, diagnostics) = parse_json_lines(&stdout);
        assert_eq!(events.len(), 3);
        assert_eq!(messages.as_array().unwrap().len(), 4);
        assert_eq!(output, "a\nb");
        assert_eq!(diagnostics.len(), 1);
        assert!(diagnostics[0].starts_with("ignored non-JSON child stdout line 2: "));
    }

    #[test]
    fn missing_agent_end_has_no_output() {
        let (events, messages, output, _) =
            parse_json_lines("{\"type\":\"agent_start\"}\n{\"type\":\"agent_end\"}\n");
        assert_eq!(events.len(), 2);
        assert_eq!(messages, json!([]));
        assert!(output.is_empty());
        let outcome = ChildOutcome {
            events,
            ..Default::default()
        };
        assert!(!outcome.has_final_result());
        assert!(outcome.completed());
    }

    #[test]
    fn executable_resolution_order() {
        let dir = tempfile::tempdir().unwrap();
        let bin = dir.path().join("bin");
        let other = dir.path().join("other");
        std::fs::create_dir_all(&bin).unwrap();
        std::fs::create_dir_all(&other).unwrap();
        let name = format!("{CHILD_BINARY}{}", std::env::consts::EXE_SUFFIX);
        let exe = bin.join("qt");

        assert_eq!(
            resolve_child_executable_from(Some("/opt/qt".into()), Some(exe.clone()), None),
            PathBuf::from("/opt/qt")
        );
        assert_eq!(
            resolve_child_executable_from(Some("".into()), Some(exe.clone()), None),
            PathBuf::from(&name)
        );
        std::fs::write(other.join(&name), "").unwrap();
        let path_var = std::env::join_paths([other.clone()]).unwrap();
        assert_eq!(
            resolve_child_executable_from(None, Some(exe.clone()), Some(path_var.clone())),
            other.join(&name)
        );
        std::fs::write(bin.join(&name), "").unwrap();
        assert_eq!(
            resolve_child_executable_from(None, Some(exe), Some(path_var)),
            bin.join(&name)
        );
    }

    #[test]
    fn request_environment_helpers() {
        let mut request = ChildRequest::default();
        request.set_env("A", "1");
        request.set_env("A", "2");
        assert_eq!(request.environment.len(), 1);
        assert_eq!(request.env("A"), Some("2"));
        assert_eq!(request.env("B"), None);
    }
}
