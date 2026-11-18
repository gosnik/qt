//! Isolated model children used by Dreaming, research, the verifier and
//! semantic consolidation (archived `IsolatedAgentRunner` requests built in
//! `runtime.cpp`).
//!
//! The memory crate never talks to a model directly. It describes one bounded
//! task as a [`ChildRequest`] and hands it to an [`IsolatedChild`] that the
//! composition root supplies: a `qt --mode json` child process
//! ([`ProcessChild`]) or an in-process local llama.cpp model. Whatever runs the
//! task reports a [`ChildOutcome`]; the trusted parent validates the output.

use std::io::{Read, Write};
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use serde_json::Value;

/// Cooperative cancellation shared between a worker and its children.
#[derive(Debug, Clone, Default)]
pub struct CancelFlag(Arc<AtomicBool>);

impl CancelFlag {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn cancel(&self) {
        self.0.store(true, Ordering::SeqCst);
    }

    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}

/// What an isolated child is asked to do. Each task has fixed tool access,
/// environment marker and generation bounds.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ChildTask {
    /// Tool-free Dreaming analyst (strict artifact JSON).
    Dreaming,
    /// Restricted web research follow-up (`web_run`, `web_search`, `web_fetch`).
    DreamResearch,
    /// Tool-free, offline local evidence verifier.
    DreamVerification,
    /// Tool-free semantic-consolidation reviewer (pre-handler protocol).
    SemanticConsolidation,
}

/// Tools the research child may use, in the archived allowlist order.
pub const RESEARCH_CHILD_TOOLS: [&str; 3] = ["web_run", "web_search", "web_fetch"];

/// GBNF grammar constraining the Dreaming child to the artifact schema
/// (archived `runtime_services.cpp`).
pub const DREAMING_GRAMMAR: &str = r#"root ::= "{" ws "\"version\"" ws ":" ws "1" "," ws "\"source_memory_ids\"" ws ":" ws source-ids "," ws "\"artifact_kind\"" ws ":" ws artifact-kind "," ws "\"content\"" ws ":" ws nonempty-string "," ws "\"confidence\"" ws ":" ws confidence "," ws "\"research_required\"" ws ":" ws boolean "}" ws
source-ids ::= "[" ws id-string "," ws id-string ("," ws id-string)* "]" ws
artifact-kind ::= "\"association\"" ws | "\"canonicalization_candidate\"" ws | "\"contradiction\"" ws | "\"hypothesis\"" ws | "\"pattern_discovery\"" ws | "\"procedure_revision\"" ws
id-string ::= "\"" json-char{1,256} "\"" ws
nonempty-string ::= "\"" json-char{1,512} "\"" ws
json-char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4})
confidence ::= ("0" ("." [0-9]+)? | "1" ("." "0"+)?) ws
boolean ::= ("true" | "false") ws
ws ::= [ \t\n\r]{0,8}"#;

/// GBNF grammar for the selection-only pre-handler contribution used by
/// semantic consolidation (archived `runtime_services.cpp`).
pub const CONSOLIDATION_GRAMMAR: &str = r#"root ::= "{" ws "\"version\"" ws ":" ws uint "," ws "\"logical_input_sequence\"" ws ":" ws uint "," ws "\"context_markdown\"" ws ":" ws "\"\"" "," ws "\"memory_ids\"" ws ":" ws strings "," ws "\"block_ids\"" ws ":" ws strings "," ws "\"warnings\"" ws ":" ws "[]" "," ws "\"estimated_tokens\"" ws ":" ws "0" "," ws "\"omission_reason\"" ws ":" ws string "," ws "\"candidate_pack_hash\"" ws ":" ws string "," ws "\"output_hash\"" ws ":" ws "\"\"" "}" ws
strings ::= "[" ws (string ("," ws string)*)? "]" ws
string ::= "\"" chars "\"" ws
chars ::= ([^"\\] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))*
uint ::= ("0" | [1-9] [0-9]*) ws
ws ::= [ \t\n\r]{0,8}"#;

impl ChildTask {
    /// Environment marker the child runtime keys its restricted mode on.
    pub fn environment_markers(self) -> &'static [(&'static str, &'static str)] {
        match self {
            Self::Dreaming => &[("QT_MEMORY_DREAMING", "1")],
            Self::DreamResearch => &[("QT_MEMORY_DREAM_RESEARCH", "1")],
            Self::DreamVerification => &[("QT_MEMORY_DREAM_VERIFY", "1"), ("PI_OFFLINE", "1")],
            Self::SemanticConsolidation => &[
                ("QT_MEMORY_PREHANDLER", "1"),
                ("QT_MEMORY_CONSOLIDATION", "1"),
            ],
        }
    }

    /// Tools the child may call; empty means `--no-tools`.
    pub fn allowed_tools(self) -> &'static [&'static str] {
        match self {
            Self::DreamResearch => &RESEARCH_CHILD_TOOLS,
            _ => &[],
        }
    }

    /// Output grammar for local llama.cpp children. Research and verification
    /// run without a grammar: tool envelopes are template specific and the
    /// parent validates the final protocol.
    pub fn grammar(self) -> Option<&'static str> {
        match self {
            Self::Dreaming => Some(DREAMING_GRAMMAR),
            Self::SemanticConsolidation => Some(CONSOLIDATION_GRAMMAR),
            Self::DreamResearch | Self::DreamVerification => None,
        }
    }

    /// Sampling temperature for local children.
    pub fn temperature(self) -> f64 {
        match self {
            Self::Dreaming | Self::DreamResearch => 0.2,
            Self::DreamVerification | Self::SemanticConsolidation => 0.0,
        }
    }

    /// Output-token bound for local children.
    pub fn max_output_tokens(self) -> u32 {
        match self {
            Self::DreamVerification => 1536,
            Self::Dreaming | Self::DreamResearch => 1024,
            Self::SemanticConsolidation => 256,
        }
    }

    /// Context size for local children (smaller than chat to cut load time).
    pub fn context_size(self) -> u32 {
        8192
    }
}

/// One bounded isolated task.
#[derive(Debug, Clone)]
pub struct ChildRequest {
    pub task: ChildTask,
    /// Model reference, e.g. `llama/qwen2-5-3b-instruct-q4-k-m`.
    pub model: String,
    pub system_prompt: String,
    /// JSON payload written to the child's stdin (never placed in argv).
    pub input: String,
    pub cwd: Option<PathBuf>,
    pub agent_home: Option<PathBuf>,
    /// Extra `--discover-local-models` roots.
    pub discovery_roots: Vec<PathBuf>,
    pub timeout: Duration,
    /// Retry marker after an out-of-device-memory GPU load failure.
    pub force_cpu: bool,
    pub cancel: CancelFlag,
}

impl ChildRequest {
    pub fn new(
        task: ChildTask,
        model: impl Into<String>,
        system_prompt: impl Into<String>,
    ) -> Self {
        Self {
            task,
            model: model.into(),
            system_prompt: system_prompt.into(),
            input: String::new(),
            cwd: None,
            agent_home: None,
            discovery_roots: Vec::new(),
            timeout: Duration::from_millis(600_000),
            force_cpu: false,
            cancel: CancelFlag::new(),
        }
    }

    /// `qt --mode json` arguments isolating the child: no session,
    /// extensions, context files, skills or recursive memory, and only the
    /// task's tools.
    pub fn cli_arguments(&self) -> Vec<String> {
        let mut args: Vec<String> = [
            "--mode",
            "json",
            "--no-session",
            "--no-extensions",
            "--no-context-files",
            "--no-skills",
            "--no-memory",
        ]
        .iter()
        .map(|item| item.to_string())
        .collect();
        let tools = self.task.allowed_tools();
        if tools.is_empty() {
            args.push("--no-tools".into());
        } else {
            args.push("--tools".into());
            args.push(tools.join(","));
        }
        args.push("--system-prompt".into());
        args.push(self.system_prompt.clone());
        args.push("--thinking".into());
        args.push("off".into());
        for root in &self.discovery_roots {
            args.push("--discover-local-models".into());
            args.push(root.to_string_lossy().into_owned());
        }
        match (self.task, self.model.strip_prefix("llama/")) {
            (ChildTask::DreamVerification, Some(id)) => {
                args.extend([
                    "--provider".into(),
                    "llama".into(),
                    "--model".into(),
                    id.into(),
                ]);
            }
            _ => args.extend(["--model".into(), self.model.clone()]),
        }
        args
    }

    /// Environment for the child: the task marker, the agent home and the
    /// CPU-retry marker.
    pub fn environment(&self) -> Vec<(String, String)> {
        let mut env: Vec<(String, String)> = self
            .task
            .environment_markers()
            .iter()
            .map(|(key, value)| (key.to_string(), value.to_string()))
            .collect();
        if let Some(home) = &self.agent_home {
            env.push((
                "QT_CODING_AGENT_DIR".into(),
                home.to_string_lossy().into_owned(),
            ));
        }
        if self.force_cpu {
            env.push(("QT_MEMORY_FORCE_CPU".into(), "1".into()));
        }
        env
    }
}

/// A tool event observed in the child (from Pi JSON-mode
/// `tool_execution_start` / `tool_execution_end` lines, or reported directly
/// by an in-process child).
#[derive(Debug, Clone, PartialEq)]
pub enum ChildEvent {
    ToolStart {
        tool_name: String,
    },
    ToolEnd {
        tool_name: String,
        is_error: bool,
        /// `result.details` when it is an object.
        details: Option<Value>,
    },
}

impl ChildEvent {
    /// Decodes one Pi JSON-mode event; other event types yield `None`.
    pub fn from_pi_json(event: &Value) -> Option<Self> {
        let object = event.as_object()?;
        let tool_name = object
            .get("toolName")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_string();
        match object.get("type").and_then(Value::as_str)? {
            "tool_execution_start" => Some(Self::ToolStart { tool_name }),
            "tool_execution_end" => {
                let is_error = object
                    .get("isError")
                    .and_then(Value::as_bool)
                    .unwrap_or(true);
                let details = object
                    .get("result")
                    .and_then(Value::as_object)
                    .and_then(|result| result.get("details"))
                    .filter(|details| details.is_object())
                    .cloned();
                Some(Self::ToolEnd {
                    tool_name,
                    is_error,
                    details,
                })
            }
            _ => None,
        }
    }
}

/// How the child ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChildExit {
    Completed,
    Failed,
    TimedOut,
    Cancelled,
}

/// Result of one child run (start failures are an `Err` from
/// [`IsolatedChild::run`]).
#[derive(Debug, Clone, PartialEq)]
pub struct ChildOutcome {
    pub exit: ChildExit,
    pub exit_code: Option<i32>,
    /// Text of the last assistant message.
    pub final_output: String,
    pub stderr: String,
    pub events: Vec<ChildEvent>,
}

impl ChildOutcome {
    /// A completed run with `final_output` and no tool events.
    pub fn completed(final_output: impl Into<String>) -> Self {
        Self {
            exit: ChildExit::Completed,
            exit_code: Some(0),
            final_output: final_output.into(),
            stderr: String::new(),
            events: Vec::new(),
        }
    }

    pub fn is_completed(&self) -> bool {
        self.exit == ChildExit::Completed
    }

    /// A GPU out-of-device-memory load failure worth one CPU retry.
    pub fn out_of_device_memory(&self) -> bool {
        self.exit == ChildExit::Failed
            && (self.stderr.contains("OutOfDeviceMemory")
                || self.stderr.contains("ErrorOutOfDeviceMemory"))
    }
}

/// Runs isolated memory tasks. `Err` means the child could not be started.
pub trait IsolatedChild: Send + Sync {
    fn run(&self, request: &ChildRequest) -> Result<ChildOutcome, String>;
}

/// Runs `request`, retrying once with the CPU marker after a GPU
/// out-of-device-memory failure. Returns the outcome and whether the CPU
/// retry happened.
pub fn run_with_cpu_fallback(
    child: &dyn IsolatedChild,
    request: &ChildRequest,
) -> (Result<ChildOutcome, String>, bool) {
    let first = child.run(request);
    match &first {
        Ok(outcome) if outcome.out_of_device_memory() => {
            let mut retry = request.clone();
            retry.force_cpu = true;
            (child.run(&retry), true)
        }
        _ => (first, false),
    }
}

/// Text parts of an assistant message joined with newlines.
fn assistant_text(message: &Value) -> String {
    let Some(content) = message.get("content").and_then(Value::as_array) else {
        return String::new();
    };
    content
        .iter()
        .filter(|part| part.get("type").and_then(Value::as_str) == Some("text"))
        .filter_map(|part| part.get("text").and_then(Value::as_str))
        .collect::<Vec<_>>()
        .join("\n")
}

/// Parsed JSON-mode stdout.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct JsonModeOutput {
    pub events: Vec<ChildEvent>,
    /// Last non-empty message text of the final `agent_end` event.
    pub final_output: String,
    /// Non-JSON lines that were ignored.
    pub diagnostics: Vec<String>,
}

/// Parses `qt --mode json` NDJSON (archived `parse_json_lines`).
pub fn parse_json_mode_output(stdout: &str) -> JsonModeOutput {
    let mut output = JsonModeOutput::default();
    for (index, line) in stdout.split('\n').enumerate() {
        let line = line.strip_suffix('\r').unwrap_or(line);
        if line.is_empty() {
            continue;
        }
        let Ok(event) = serde_json::from_str::<Value>(line) else {
            output
                .diagnostics
                .push(format!("ignored non-JSON child stdout line {}", index + 1));
            continue;
        };
        if let Some(decoded) = ChildEvent::from_pi_json(&event) {
            output.events.push(decoded);
        }
        if event.get("type").and_then(Value::as_str) == Some("agent_end")
            && let Some(messages) = event.get("messages").and_then(Value::as_array)
        {
            for message in messages {
                let text = assistant_text(message);
                if !text.is_empty() {
                    output.final_output = text;
                }
            }
        }
    }
    output
}

/// An [`IsolatedChild`] that launches `executable` (normally the `qt`
/// binary) in JSON mode with [`ChildRequest::cli_arguments`].
#[derive(Debug, Clone)]
pub struct ProcessChild {
    pub executable: PathBuf,
    /// Arguments placed before the request's own.
    pub leading_arguments: Vec<String>,
}

impl ProcessChild {
    pub fn new(executable: impl Into<PathBuf>) -> Self {
        Self {
            executable: executable.into(),
            leading_arguments: Vec::new(),
        }
    }
}

/// Reads a pipe to the end on a helper thread. The result is collected with
/// a grace period so a killed child whose grandchildren still hold the pipe
/// open cannot block the parent.
fn read_to_end_in_thread<R: Read + Send + 'static>(
    mut reader: R,
) -> std::sync::mpsc::Receiver<String> {
    let (sender, receiver) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let mut bytes = Vec::new();
        let _ = reader.read_to_end(&mut bytes);
        let _ = sender.send(String::from_utf8_lossy(&bytes).into_owned());
    });
    receiver
}

fn collect(receiver: Option<std::sync::mpsc::Receiver<String>>, grace: Duration) -> String {
    receiver
        .and_then(|receiver| receiver.recv_timeout(grace).ok())
        .unwrap_or_default()
}

impl IsolatedChild for ProcessChild {
    fn run(&self, request: &ChildRequest) -> Result<ChildOutcome, String> {
        let mut command = Command::new(&self.executable);
        command
            .args(&self.leading_arguments)
            .args(request.cli_arguments())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        if let Some(cwd) = &request.cwd {
            command.current_dir(cwd);
        }
        for (key, value) in request.environment() {
            command.env(key, value);
        }
        let mut process = command
            .spawn()
            .map_err(|error| format!("unable to start isolated child: {error}"))?;
        let stdout = process.stdout.take().map(read_to_end_in_thread);
        let stderr = process.stderr.take().map(read_to_end_in_thread);
        if let Some(mut stdin) = process.stdin.take() {
            let input = request.input.clone();
            std::thread::spawn(move || {
                let _ = stdin.write_all(input.as_bytes());
                let _ = stdin.write_all(b"\n");
            });
        }
        let deadline = Instant::now() + request.timeout;
        let mut exit = ChildExit::Completed;
        let status = loop {
            match process.try_wait() {
                Ok(Some(status)) => break Some(status),
                Ok(None) => {}
                Err(error) => {
                    let _ = process.kill();
                    let _ = process.wait();
                    return Err(format!("unable to wait for isolated child: {error}"));
                }
            }
            if request.cancel.is_cancelled() {
                exit = ChildExit::Cancelled;
            } else if Instant::now() >= deadline {
                exit = ChildExit::TimedOut;
            }
            if exit != ChildExit::Completed {
                let _ = process.kill();
                let _ = process.wait();
                break None;
            }
            std::thread::sleep(Duration::from_millis(10));
        };
        // A normally exiting child closes its pipes; after a kill, inherited
        // descriptors may stay open, so only wait briefly.
        let grace = if exit == ChildExit::Completed {
            Duration::from_secs(30)
        } else {
            Duration::from_millis(500)
        };
        let stdout = collect(stdout, grace);
        let stderr = collect(stderr, grace);
        let parsed = parse_json_mode_output(&stdout);
        let exit_code = status.and_then(|status| status.code());
        if status.is_some() && exit_code != Some(0) {
            exit = ChildExit::Failed;
        }
        Ok(ChildOutcome {
            exit,
            exit_code,
            final_output: parsed.final_output,
            stderr,
            events: parsed.events,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::sync::Mutex;

    #[test]
    fn cli_arguments_isolate_each_task() {
        let mut request = ChildRequest::new(ChildTask::Dreaming, "llama/qwen", "PROMPT");
        request.discovery_roots = vec![PathBuf::from("/models")];
        assert_eq!(
            request.cli_arguments(),
            [
                "--mode",
                "json",
                "--no-session",
                "--no-extensions",
                "--no-context-files",
                "--no-skills",
                "--no-memory",
                "--no-tools",
                "--system-prompt",
                "PROMPT",
                "--thinking",
                "off",
                "--discover-local-models",
                "/models",
                "--model",
                "llama/qwen"
            ]
        );
        let research = ChildRequest::new(ChildTask::DreamResearch, "m", "P");
        let args = research.cli_arguments();
        let position = args.iter().position(|arg| arg == "--tools").unwrap();
        assert_eq!(args[position + 1], "web_run,web_search,web_fetch");
        assert!(!args.contains(&"--no-tools".to_string()));
        let verify = ChildRequest::new(ChildTask::DreamVerification, "llama/judge", "P");
        let args = verify.cli_arguments();
        assert!(args.ends_with(&[
            "--provider".to_string(),
            "llama".into(),
            "--model".into(),
            "judge".into()
        ]));
    }

    #[test]
    fn environment_carries_markers_home_and_cpu_retry() {
        let mut request = ChildRequest::new(ChildTask::DreamVerification, "llama/x", "");
        request.agent_home = Some(PathBuf::from("/home/agent"));
        request.force_cpu = true;
        assert_eq!(
            request.environment(),
            [
                ("QT_MEMORY_DREAM_VERIFY".to_string(), "1".to_string()),
                ("PI_OFFLINE".into(), "1".into()),
                ("QT_CODING_AGENT_DIR".into(), "/home/agent".into()),
                ("QT_MEMORY_FORCE_CPU".into(), "1".into()),
            ]
        );
        assert_eq!(ChildTask::Dreaming.grammar(), Some(DREAMING_GRAMMAR));
        assert_eq!(ChildTask::DreamResearch.grammar(), None);
        assert_eq!(ChildTask::DreamVerification.max_output_tokens(), 1536);
    }

    #[test]
    fn parses_json_mode_events_and_final_output() {
        let stdout = [
            json!({"type": "agent_start"}).to_string(),
            "not json".to_string(),
            json!({"type": "tool_execution_start", "toolName": "web_search"}).to_string(),
            json!({"type": "tool_execution_end", "toolName": "web_search", "isError": false,
                   "result": {"content": [], "details": {"results": []}}})
            .to_string(),
            json!({"type": "tool_execution_end", "toolName": "web_fetch"}).to_string(),
            json!({"type": "agent_end", "messages": [
                {"role": "assistant", "content": [{"type": "text", "text": "first"}]},
                {"role": "assistant", "content": [{"type": "text", "text": "a"}, {"type": "thinking", "thinking": "x"}, {"type": "text", "text": "b"}]}
            ]})
            .to_string(),
        ]
        .join("\r\n");
        let parsed = parse_json_mode_output(&stdout);
        assert_eq!(parsed.final_output, "a\nb");
        assert_eq!(parsed.diagnostics.len(), 1);
        assert_eq!(
            parsed.events,
            [
                ChildEvent::ToolStart {
                    tool_name: "web_search".into()
                },
                ChildEvent::ToolEnd {
                    tool_name: "web_search".into(),
                    is_error: false,
                    details: Some(json!({"results": []}))
                },
                ChildEvent::ToolEnd {
                    tool_name: "web_fetch".into(),
                    is_error: true,
                    details: None
                },
            ]
        );
    }

    struct Scripted(Mutex<Vec<ChildOutcome>>, Mutex<Vec<bool>>);

    impl IsolatedChild for Scripted {
        fn run(&self, request: &ChildRequest) -> Result<ChildOutcome, String> {
            self.1.lock().unwrap().push(request.force_cpu);
            Ok(self.0.lock().unwrap().remove(0))
        }
    }

    #[test]
    fn retries_once_on_cpu_after_out_of_device_memory() {
        let mut oom = ChildOutcome::completed("");
        oom.exit = ChildExit::Failed;
        oom.exit_code = Some(1);
        oom.stderr = "vk::Device::allocateMemory: ErrorOutOfDeviceMemory".into();
        let child = Scripted(
            Mutex::new(vec![oom.clone(), ChildOutcome::completed("{}")]),
            Mutex::new(Vec::new()),
        );
        let request = ChildRequest::new(ChildTask::Dreaming, "m", "p");
        let (result, cpu) = run_with_cpu_fallback(&child, &request);
        assert!(cpu);
        assert!(result.unwrap().is_completed());
        assert_eq!(*child.1.lock().unwrap(), [false, true]);

        let mut timeout = oom;
        timeout.exit = ChildExit::TimedOut;
        let child = Scripted(Mutex::new(vec![timeout]), Mutex::new(Vec::new()));
        let (result, cpu) = run_with_cpu_fallback(&child, &request);
        assert!(!cpu);
        assert_eq!(result.unwrap().exit, ChildExit::TimedOut);
    }

    #[cfg(unix)]
    #[test]
    fn process_child_runs_reads_stdin_and_enforces_timeout() {
        let script = r#"read line; printf '{"type":"agent_end","messages":[{"role":"assistant","content":[{"type":"text","text":"%s|%s"}]}]}\n' "$line" "$QT_MEMORY_DREAMING"; echo oops >&2"#;
        let child = ProcessChild {
            executable: "sh".into(),
            leading_arguments: vec!["-c".into(), script.into(), "child".into()],
        };
        let mut request = ChildRequest::new(ChildTask::Dreaming, "m", "p");
        request.input = "hello".into();
        request.timeout = Duration::from_secs(20);
        let outcome = child.run(&request).unwrap();
        assert!(outcome.is_completed(), "{outcome:?}");
        assert_eq!(outcome.final_output, "hello|1");
        assert_eq!(outcome.stderr.trim(), "oops");

        let failing = ProcessChild {
            executable: "sh".into(),
            leading_arguments: vec!["-c".into(), "exit 3".into(), "child".into()],
        };
        let outcome = failing.run(&request).unwrap();
        assert_eq!(outcome.exit, ChildExit::Failed);
        assert_eq!(outcome.exit_code, Some(3));

        let slow = ProcessChild {
            executable: "sh".into(),
            leading_arguments: vec!["-c".into(), "sleep 5".into(), "child".into()],
        };
        request.timeout = Duration::from_millis(100);
        let started = Instant::now();
        let outcome = slow.run(&request).unwrap();
        assert_eq!(outcome.exit, ChildExit::TimedOut);
        assert!(started.elapsed() < Duration::from_secs(4));

        request.timeout = Duration::from_secs(20);
        request.cancel.cancel();
        assert_eq!(slow.run(&request).unwrap().exit, ChildExit::Cancelled);

        let missing = ProcessChild::new("/nonexistent/qt-binary");
        assert!(missing.run(&request).is_err());
    }
}
