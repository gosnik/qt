//! Isolated child-agent runner (archived `isolated_agent_runner.cpp`).
//!
//! The memory pre-handler and semantic consolidation run the qt CLI as a
//! child in JSON mode with no session, tools, skills, extensions or memory.
//! The request travels on stdin; the final assistant text of the last
//! `agent_end` event on stdout is the child's answer. The composition root
//! normally uses [`ProcessAgentRunner`] with the current executable.

use std::io::{Read as _, Write as _};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde_json::Value;

use crate::error::{MemoryError, Result};
use crate::recall::CancelFn;

/// One child invocation.
#[derive(Clone, Default)]
pub struct IsolatedAgentRequest {
    pub arguments: Vec<String>,
    /// Written to stdin; never exposed in argv.
    pub stdin_payload: String,
    pub cwd: Option<PathBuf>,
    pub environment: Vec<(String, String)>,
    pub timeout_ms: Option<i64>,
    pub cancelled: Option<CancelFn>,
}

impl std::fmt::Debug for IsolatedAgentRequest {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("IsolatedAgentRequest")
            .field("arguments", &self.arguments)
            .field("cwd", &self.cwd)
            .field("environment", &self.environment)
            .field("timeout_ms", &self.timeout_ms)
            .finish_non_exhaustive()
    }
}

/// Outcome of a child run.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct IsolatedAgentResult {
    pub exit_code: i32,
    pub cancelled: bool,
    pub timed_out: bool,
    pub stdout_text: String,
    pub stderr_text: String,
    pub events: Vec<Value>,
    pub final_messages: Vec<Value>,
    pub final_output: String,
    pub diagnostics: Vec<String>,
}

impl IsolatedAgentResult {
    pub fn completed(&self) -> bool {
        self.exit_code == 0 && !self.cancelled && !self.timed_out
    }
}

/// Runs an isolated child agent.
pub trait IsolatedAgentRunner: Send + Sync {
    /// `Err` when the child could not be started.
    fn run(&self, request: &IsolatedAgentRequest) -> Result<IsolatedAgentResult>;
}

fn assistant_text(message: &Value) -> String {
    let Some(content) = message.get("content").and_then(Value::as_array) else {
        return String::new();
    };
    content
        .iter()
        .filter(|item| item.get("type").and_then(Value::as_str) == Some("text"))
        .filter_map(|item| item.get("text").and_then(Value::as_str))
        .collect::<Vec<_>>()
        .join("\n")
}

/// Parses qt JSON-mode NDJSON: every event, and from the last `agent_end`
/// event its messages and the text of its last message that has text.
pub fn parse_json_lines(stdout: &str, result: &mut IsolatedAgentResult) {
    for (index, line) in stdout.split('\n').enumerate() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if line.is_empty() {
            continue;
        }
        match serde_json::from_str::<Value>(line) {
            Ok(event) => {
                if event.get("type").and_then(Value::as_str) == Some("agent_end")
                    && let Some(messages) = event.get("messages").and_then(Value::as_array)
                {
                    result.final_messages = messages.clone();
                    for message in messages {
                        let text = assistant_text(message);
                        if !text.is_empty() {
                            result.final_output = text;
                        }
                    }
                }
                result.events.push(event);
            }
            Err(error) => result.diagnostics.push(format!(
                "ignored non-JSON child stdout line {}: {error}",
                index + 1
            )),
        }
    }
}

/// Spawns `executable [executable_arguments] [request.arguments]`.
#[derive(Debug, Clone)]
pub struct ProcessAgentRunner {
    pub executable: PathBuf,
    pub executable_arguments: Vec<String>,
}

impl ProcessAgentRunner {
    pub fn new(executable: impl Into<PathBuf>) -> Self {
        Self {
            executable: executable.into(),
            executable_arguments: Vec::new(),
        }
    }

    /// The running executable (the composition root's own binary).
    pub fn current_executable() -> Result<Self> {
        Ok(Self::new(std::env::current_exe()?))
    }
}

impl IsolatedAgentRunner for ProcessAgentRunner {
    fn run(&self, request: &IsolatedAgentRequest) -> Result<IsolatedAgentResult> {
        let mut command = Command::new(&self.executable);
        command
            .args(&self.executable_arguments)
            .args(&request.arguments)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        if let Some(cwd) = &request.cwd {
            command.current_dir(cwd);
        }
        for (name, value) in &request.environment {
            command.env(name, value);
        }
        let mut child = command.spawn().map_err(|error| {
            MemoryError::storage(format!("unable to start isolated agent: {error}"))
        })?;
        let mut stdin = child.stdin.take();
        let payload = request.stdin_payload.clone();
        let writer = std::thread::spawn(move || {
            if let Some(stdin) = stdin.as_mut() {
                let _ = stdin.write_all(payload.as_bytes());
            }
        });
        // Readers report through channels so a grandchild that keeps a pipe
        // open after the child was killed cannot block the caller.
        let reader = |stream: Option<Box<dyn std::io::Read + Send>>| {
            let (sender, receiver) = std::sync::mpsc::channel::<String>();
            std::thread::spawn(move || {
                let mut bytes = Vec::new();
                if let Some(mut stream) = stream {
                    let _ = stream.read_to_end(&mut bytes);
                }
                let _ = sender.send(String::from_utf8_lossy(&bytes).into_owned());
            });
            receiver
        };
        let stdout = reader(
            child
                .stdout
                .take()
                .map(|s| Box::new(s) as Box<dyn std::io::Read + Send>),
        );
        let stderr = reader(
            child
                .stderr
                .take()
                .map(|s| Box::new(s) as Box<dyn std::io::Read + Send>),
        );
        let deadline = request
            .timeout_ms
            .filter(|timeout| *timeout > 0)
            .map(|timeout| Instant::now() + Duration::from_millis(timeout as u64));
        let mut result = IsolatedAgentResult::default();
        let status = loop {
            if let Some(status) = child.try_wait()? {
                break Some(status);
            }
            if request
                .cancelled
                .as_ref()
                .is_some_and(|cancelled| cancelled())
            {
                result.cancelled = true;
                break None;
            }
            if deadline.is_some_and(|deadline| Instant::now() >= deadline) {
                result.timed_out = true;
                break None;
            }
            std::thread::sleep(Duration::from_millis(10));
        };
        let status = match status {
            Some(status) => status,
            None => {
                let _ = child.kill();
                child.wait()?
            }
        };
        drop(writer);
        result.exit_code = status.code().unwrap_or(-1);
        let grace = if result.completed()
            || status.code().is_some() && !result.timed_out && !result.cancelled
        {
            Duration::from_secs(30)
        } else {
            Duration::from_secs(1)
        };
        result.stdout_text = stdout.recv_timeout(grace).unwrap_or_default();
        result.stderr_text = stderr.recv_timeout(grace).unwrap_or_default();
        let stdout_text = result.stdout_text.clone();
        parse_json_lines(&stdout_text, &mut result);
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_final_assistant_text_from_agent_end() {
        let mut result = IsolatedAgentResult::default();
        parse_json_lines(
            "{\"type\":\"agent_start\"}\nnot json\n{\"type\":\"agent_end\",\"messages\":[{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"a\"},{\"type\":\"text\",\"text\":\"b\"}]},{\"role\":\"user\",\"content\":[]}]}\r\n",
            &mut result,
        );
        assert_eq!(result.final_output, "a\nb");
        assert_eq!(result.events.len(), 2);
        assert_eq!(result.final_messages.len(), 2);
        assert_eq!(result.diagnostics.len(), 1);
        assert!(result.diagnostics[0].starts_with("ignored non-JSON child stdout line 2"));
    }

    #[cfg(unix)]
    #[test]
    fn process_runner_feeds_stdin_and_times_out() {
        let runner = ProcessAgentRunner::new("/bin/sh");
        let request = IsolatedAgentRequest {
            arguments: vec![
                "-c".into(),
                "read line; printf '{\"type\":\"agent_end\",\"messages\":[{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}]}\\n' \"$line\"".into(),
            ],
            stdin_payload: "hello\n".into(),
            timeout_ms: Some(5000),
            ..Default::default()
        };
        let result = runner.run(&request).unwrap();
        assert!(result.completed());
        assert_eq!(result.final_output, "hello");
        let slow = IsolatedAgentRequest {
            arguments: vec!["-c".into(), "sleep 5".into()],
            timeout_ms: Some(100),
            ..Default::default()
        };
        let result = runner.run(&slow).unwrap();
        assert!(result.timed_out);
        assert!(!result.completed());
    }
}
