use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::io::{AsyncRead, AsyncReadExt as _};
use tokio::process::Command;

use super::mutation::{MutationKind, require_approval};
use super::{Tool, ToolContext, ToolOutput, parse_args};
use crate::message::BashExecutionMessage;
use crate::util::{DEFAULT_MAX_BYTES, DEFAULT_MAX_LINES, human_bytes, truncate_tail};

pub struct BashTool;

/// Settings-driven bash behaviour (`shellCommandPrefix`, environment, shell).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BashOptions {
    /// Prepended to every command on its own line (Pi `commandPrefix`).
    pub command_prefix: Option<String>,
    /// Extra environment: `Some(value)` sets, `None` removes the variable.
    pub env: Vec<(String, Option<String>)>,
    /// Explicit shell executable (run as `<shell> -c <command>`).
    pub shell_path: Option<PathBuf>,
}

#[derive(Deserialize)]
struct Args {
    command: String,
    #[serde(default, deserialize_with = "de_opt_f64")]
    timeout: Option<f64>,
    #[serde(default, deserialize_with = "de_opt_f64")]
    timeout_ms: Option<f64>,
    #[serde(default)]
    workdir: Option<String>,
    #[serde(default)]
    cwd: Option<String>,
    #[serde(default)]
    working_dir: Option<String>,
}

fn de_opt_f64<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<f64>, D::Error> {
    let v: Option<Value> = serde::Deserialize::deserialize(d)?;
    Ok(match v {
        Some(Value::Number(n)) => n.as_f64(),
        Some(Value::String(s)) => s.trim().parse::<f64>().ok(),
        _ => None,
    })
}

/// Shell program and the arguments preceding the command string.
fn shell(options: &BashOptions) -> (PathBuf, Vec<&'static str>) {
    if let Some(path) = &options.shell_path {
        return (path.clone(), vec!["-c"]);
    }
    if cfg!(unix) && Path::new("/bin/bash").is_file() {
        // Archived qt ran commands as a login shell: `/bin/bash -lc`.
        return (PathBuf::from("/bin/bash"), vec!["-lc"]);
    }
    if let Some(bash) = find_in_path(if cfg!(windows) { "bash.exe" } else { "bash" }) {
        return (bash, vec!["-lc"]);
    }
    if cfg!(windows) {
        (
            PathBuf::from("powershell.exe"),
            vec!["-NoProfile", "-NonInteractive", "-Command"],
        )
    } else {
        (PathBuf::from("/bin/sh"), vec!["-c"])
    }
}

pub(crate) fn find_in_path(program: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path)
        .map(|dir| dir.join(program))
        .find(|candidate| candidate.is_file())
}

enum Outcome {
    Exited(ExitInfo),
    TimedOut(u64),
    Aborted,
    SpawnFailed(String),
}

#[derive(Clone, Copy)]
struct ExitInfo {
    code: Option<i32>,
    signal: Option<i32>,
}

impl Tool for BashTool {
    fn name(&self) -> &'static str {
        "bash"
    }
    fn description(&self) -> String {
        format!(
            "Execute a bash command in the current working directory. Returns stdout and stderr. Output is truncated to last {DEFAULT_MAX_LINES} lines or {}KB (whichever is hit first). If truncated, full output is saved to a temp file. Optionally provide a timeout in seconds.",
            DEFAULT_MAX_BYTES / 1024
        )
    }
    fn snippet(&self) -> &'static str {
        "Execute bash commands (ls, grep, find, etc.)"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "command": { "type": "string", "description": "Bash command to execute" },
                "timeout": { "type": "number", "description": "Timeout in seconds (optional, no default timeout)" },
                "timeout_ms": { "type": "number", "description": "Timeout in milliseconds (alternative to timeout)" },
                "workdir": { "type": "string", "description": "Working directory for the command, relative to the workspace or absolute within it (default: current directory)" }
            },
            "required": ["command"]
        })
    }

    fn execute(&self, raw: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("bash", raw.clone()) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let timeout_ms = match timeout_ms(&args) {
                Ok(t) => t,
                Err(e) => return ToolOutput::error(e),
            };
            let workdir = match working_directory(&args, &ctx) {
                Ok(w) => w,
                Err(e) => return ToolOutput::error(e),
            };
            if let Some(queued) = require_approval(
                &ctx,
                MutationKind::Command,
                "bash",
                format!("run bash command: {}", args.command),
                &raw,
            ) {
                return queued;
            }
            let resolved_command = match ctx.bash.command_prefix.as_deref() {
                Some(prefix) if !prefix.is_empty() => format!("{prefix}\n{}", args.command),
                _ => args.command.clone(),
            };
            let output = Arc::new(Mutex::new(Vec::<u8>::new()));
            let outcome = run(
                &resolved_command,
                &workdir,
                timeout_ms,
                &ctx,
                output.clone(),
            )
            .await;
            let bytes = std::mem::take(&mut *output.lock().unwrap_or_else(|e| e.into_inner()));
            let mut out = finish(outcome, &String::from_utf8_lossy(&bytes));
            if let Some(Value::Object(details)) = out.details.as_mut() {
                details.insert("command".into(), json!(args.command));
                details.insert("cwd".into(), json!(ctx.display(&workdir)));
                if resolved_command != args.command {
                    details.insert("resolved_command".into(), json!(resolved_command));
                }
                if let Some(ms) = timeout_ms {
                    details.insert("timeout_ms".into(), json!(ms));
                }
            }
            out
        })
    }
}

/// Run a user `!command` (Pi `executeBash`): the session cwd, the
/// settings shell and prefix, no workspace confinement and no timeout.
/// Output is truncated to the last 2000 lines / 50 KB like the tool; the
/// full output is saved to a temp file when cut.
pub async fn execute_user_command(command: &str, ctx: &ToolContext) -> BashExecutionMessage {
    let resolved = match ctx.bash.command_prefix.as_deref() {
        Some(prefix) if !prefix.is_empty() => format!("{prefix}\n{command}"),
        _ => command.to_string(),
    };
    let output = Arc::new(Mutex::new(Vec::<u8>::new()));
    let outcome = run(&resolved, &ctx.cwd, None, ctx, output.clone()).await;
    let bytes = std::mem::take(&mut *output.lock().unwrap_or_else(|e| e.into_inner()));
    let raw = String::from_utf8_lossy(&bytes).into_owned();
    let mut message = BashExecutionMessage {
        command: command.to_string(),
        output: String::new(),
        exit_code: None,
        cancelled: false,
        truncated: false,
        full_output_path: None,
        timestamp: crate::message::now_millis(),
        exclude_from_context: false,
    };
    match outcome {
        Outcome::SpawnFailed(error) => {
            message.output = error;
            message.exit_code = Some(127);
            return message;
        }
        Outcome::Exited(info) => {
            message.exit_code = info
                .code
                .map(i64::from)
                .or(info.signal.map(|s| 128 + i64::from(s)));
        }
        Outcome::TimedOut(_) | Outcome::Aborted => message.cancelled = true,
    }
    let t = truncate_tail(&raw, DEFAULT_MAX_LINES, DEFAULT_MAX_BYTES);
    message.output = t.text.trim_end_matches('\n').to_string();
    if t.truncated {
        message.truncated = true;
        let path = std::env::temp_dir().join(format!("qt-bash-{}.log", crate::util::short_id()));
        if std::fs::write(&path, &raw).is_ok() {
            crate::policy::allow_read_artifact(&path);
            message.full_output_path = Some(path.display().to_string());
        }
    }
    message
}

fn timeout_ms(args: &Args) -> Result<Option<u64>, String> {
    let (value, unit_ms) = match (args.timeout, args.timeout_ms) {
        (Some(_), Some(_)) => return Err("bash accepts timeout or timeout_ms, not both".into()),
        (Some(secs), None) => (secs, 1000.0),
        (None, Some(ms)) => (ms, 1.0),
        (None, None) => return Ok(None),
    };
    if !value.is_finite() || value < 0.0 {
        return Err(if unit_ms > 1.0 {
            "bash timeout must be non-negative".into()
        } else {
            "bash timeout_ms must be non-negative".into()
        });
    }
    let ms = (value * unit_ms).ceil();
    if ms == 0.0 {
        return Ok(None);
    }
    if ms > u64::MAX as f64 {
        return Err("bash timeout is too large".into());
    }
    Ok(Some(ms as u64))
}

fn working_directory(args: &Args, ctx: &ToolContext) -> Result<PathBuf, String> {
    let requested: Vec<&String> = [&args.workdir, &args.cwd, &args.working_dir]
        .into_iter()
        .flatten()
        .collect();
    if requested.len() > 1 {
        return Err("bash accepts only one of workdir, cwd, or working_dir".into());
    }
    let path = match requested
        .first()
        .map(|s| s.trim())
        .filter(|s| !s.is_empty())
    {
        // Relative working directories are taken from the workspace root.
        Some(dir) => crate::policy::resolve_argument(ctx.policy.root(), dir)
            .map(|r| r.path)
            .ok_or_else(|| "bash working directory must not be empty".to_string())?,
        None => ctx.cwd.clone(),
    };
    if ctx.policy.check(&path).is_err() {
        return Err("working directory escapes workspace_root".into());
    }
    if !path.is_dir() {
        return Err(format!(
            "Working directory does not exist: {}",
            path.display()
        ));
    }
    Ok(path)
}

async fn run(
    command: &str,
    workdir: &Path,
    timeout_ms: Option<u64>,
    ctx: &ToolContext,
    output: Arc<Mutex<Vec<u8>>>,
) -> Outcome {
    let (program, flags) = shell(&ctx.bash);
    let mut cmd = Command::new(&program);
    cmd.args(&flags)
        .arg(command)
        .current_dir(workdir)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
    for (key, value) in &ctx.bash.env {
        match value {
            Some(v) => {
                cmd.env(key, v);
            }
            None => {
                cmd.env_remove(key);
            }
        }
    }
    #[cfg(unix)]
    cmd.process_group(0);
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            return Outcome::SpawnFailed(format!("Failed to start {}: {e}", program.display()));
        }
    };
    let pid = child.id();
    let readers = [
        child
            .stdout
            .take()
            .map(|s| tokio::spawn(pump(s, output.clone(), ctx.clone()))),
        child
            .stderr
            .take()
            .map(|s| tokio::spawn(pump(s, output.clone(), ctx.clone()))),
    ];
    let deadline = async {
        match timeout_ms {
            Some(ms) => tokio::time::sleep(Duration::from_millis(ms)).await,
            None => std::future::pending::<()>().await,
        }
    };
    let outcome = tokio::select! {
        status = child.wait() => Outcome::Exited(exit_info(status.ok())),
        _ = deadline => Outcome::TimedOut(timeout_ms.unwrap_or(0)),
        _ = ctx.cancel.cancelled() => Outcome::Aborted,
    };
    if !matches!(outcome, Outcome::Exited(_)) {
        kill_tree(pid);
        let _ = child.kill().await;
    }
    // Background processes may keep the pipes open; do not wait on them forever.
    for reader in readers.into_iter().flatten() {
        let abort = reader.abort_handle();
        if tokio::time::timeout(Duration::from_secs(2), reader)
            .await
            .is_err()
        {
            abort.abort();
        }
    }
    outcome
}

fn exit_info(status: Option<std::process::ExitStatus>) -> ExitInfo {
    let Some(status) = status else {
        return ExitInfo {
            code: None,
            signal: None,
        };
    };
    #[cfg(unix)]
    let signal = std::os::unix::process::ExitStatusExt::signal(&status);
    #[cfg(not(unix))]
    let signal = None;
    ExitInfo {
        code: status.code(),
        signal,
    }
}

async fn pump(mut stream: impl AsyncRead + Unpin, output: Arc<Mutex<Vec<u8>>>, ctx: ToolContext) {
    let mut buf = [0u8; 8192];
    loop {
        match stream.read(&mut buf).await {
            Ok(0) | Err(_) => break,
            Ok(n) => {
                let snapshot = {
                    let mut out = output.lock().unwrap_or_else(|e| e.into_inner());
                    out.extend_from_slice(&buf[..n]);
                    ctx.progress.as_ref().map(|_| {
                        let start = out.len().saturating_sub(16 * 1024);
                        String::from_utf8_lossy(&out[start..]).into_owned()
                    })
                };
                if let Some(text) = snapshot {
                    ctx.report(text);
                }
            }
        }
    }
}

#[cfg(unix)]
fn kill_tree(pid: Option<u32>) {
    if let Some(pid) = pid {
        // The child leads its own process group (see `process_group(0)`).
        unsafe {
            libc::killpg(pid as libc::pid_t, libc::SIGKILL);
        }
    }
}

#[cfg(not(unix))]
fn kill_tree(pid: Option<u32>) {
    if let Some(pid) = pid {
        let _ = std::process::Command::new("taskkill")
            .args(["/F", "/T", "/PID", &pid.to_string()])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}

fn format_seconds(ms: u64) -> String {
    if ms.is_multiple_of(1000) {
        (ms / 1000).to_string()
    } else {
        let text = format!("{:.3}", ms as f64 / 1000.0);
        text.trim_end_matches('0').to_string()
    }
}

fn finish(outcome: Outcome, raw: &str) -> ToolOutput {
    if let Outcome::SpawnFailed(msg) = &outcome {
        return ToolOutput::error(msg.clone());
    }
    let t = truncate_tail(raw, DEFAULT_MAX_LINES, DEFAULT_MAX_BYTES);
    let mut text = if t.text.trim().is_empty() {
        "(no output)".to_string()
    } else {
        t.text.clone()
    };
    let mut details = json!({
        "exit_code": Value::Null,
        "timed_out": matches!(outcome, Outcome::TimedOut(_)),
        "cancelled": matches!(outcome, Outcome::Aborted),
        "truncated": t.truncated,
        "fullOutputPath": Value::Null,
    });
    if t.truncated {
        let last_line = raw
            .strip_suffix('\n')
            .unwrap_or(raw)
            .rsplit('\n')
            .next()
            .unwrap_or("");
        let partial = t.kept_lines == 1 && t.text.len() < last_line.len();
        let by_lines = !partial && t.kept_lines >= DEFAULT_MAX_LINES;
        let start = t.total_lines - t.kept_lines + 1;
        let end = t.total_lines;
        let mut note = if partial {
            format!(
                "\n\n[Showing last {} of line {end} (line is {})",
                human_bytes(t.text.len()),
                human_bytes(last_line.len())
            )
        } else if by_lines {
            format!("\n\n[Showing lines {start}-{end} of {end}")
        } else {
            format!(
                "\n\n[Showing lines {start}-{end} of {end} ({} limit)",
                human_bytes(DEFAULT_MAX_BYTES)
            )
        };
        let path = std::env::temp_dir().join(format!("qt-bash-{}.log", crate::util::short_id()));
        if std::fs::write(&path, raw).is_ok() {
            crate::policy::allow_read_artifact(&path);
            note.push_str(&format!(". Full output: {}", path.display()));
            details["fullOutputPath"] = json!(path);
        }
        note.push(']');
        text.push_str(&note);
        details["truncation"] = json!({
            "truncated": true,
            "truncatedBy": if by_lines { "lines" } else { "bytes" },
            "totalLines": t.total_lines,
            "totalBytes": raw.len(),
            "outputLines": t.kept_lines,
            "outputBytes": t.text.len(),
            "lastLinePartial": partial,
            "maxLines": DEFAULT_MAX_LINES,
            "maxBytes": DEFAULT_MAX_BYTES,
        });
    }
    let is_error = match outcome {
        Outcome::Exited(info) => {
            details["exit_code"] = json!(info.code);
            match info.code {
                Some(0) => {
                    details["termination_reason"] = json!("success");
                    false
                }
                Some(code) => {
                    text.push_str(&format!("\n\nCommand exited with code {code}"));
                    if code > 128 {
                        details["termination_reason"] = json!("signal");
                        details["signal"] = json!(code - 128);
                    } else {
                        details["termination_reason"] = json!("exit_code");
                    }
                    true
                }
                None => {
                    text.push_str("\n\nCommand terminated by signal");
                    details["termination_reason"] = json!("signal");
                    details["signal"] = json!(info.signal);
                    true
                }
            }
        }
        Outcome::TimedOut(ms) => {
            text.push_str(&format!(
                "\n\nCommand timed out after {} seconds",
                format_seconds(ms)
            ));
            details["termination_reason"] = json!("timeout");
            true
        }
        Outcome::Aborted => {
            text.push_str("\n\nCommand aborted");
            details["termination_reason"] = json!("cancelled");
            true
        }
        Outcome::SpawnFailed(_) => true,
    };
    ToolOutput {
        is_error,
        ..ToolOutput::text(text)
    }
    .with_details(details)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;

    #[tokio::test]
    async fn captures_output_and_exit_codes() {
        let dir = tempfile::tempdir().unwrap();
        let ctx = ToolContext::new(dir.path());
        let out = BashTool
            .execute(json!({"command": "echo hi; echo err >&2"}), ctx.clone())
            .await;
        assert!(!out.is_error);
        let text = out.text_content();
        assert!(text.contains("hi") && text.contains("err"));
        let details = out.details.unwrap();
        assert_eq!(details["exit_code"], 0);
        assert_eq!(details["termination_reason"], "success");
        let out = BashTool
            .execute(json!({"command": "exit 3"}), ctx.clone())
            .await;
        assert!(out.is_error);
        assert!(out.text_content().ends_with("Command exited with code 3"));
        assert_eq!(out.details.unwrap()["termination_reason"], "exit_code");
        let out = BashTool.execute(json!({"command": "pwd"}), ctx).await;
        assert!(
            out.text_content()
                .contains(dir.path().file_name().unwrap().to_str().unwrap())
        );
    }

    #[tokio::test]
    async fn workdir_aliases_and_policy() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir(dir.path().join("sub")).unwrap();
        let ctx = ToolContext::new(dir.path());
        for key in ["workdir", "cwd", "working_dir"] {
            let out = BashTool
                .execute(json!({"command": "pwd", key: "sub"}), ctx.clone())
                .await;
            assert!(out.text_content().trim_end().ends_with("/sub"), "{key}");
            assert_eq!(out.details.unwrap()["cwd"], "sub");
        }
        let out = BashTool
            .execute(json!({"command": "pwd", "workdir": "/"}), ctx.clone())
            .await;
        assert_eq!(
            out.text_content(),
            "working directory escapes workspace_root"
        );
        let out = BashTool
            .execute(
                json!({"command": "pwd", "workdir": "a", "cwd": "b"}),
                ctx.clone(),
            )
            .await;
        assert!(out.text_content().contains("only one of"));
        let out = BashTool
            .execute(
                json!({"command": "pwd", "timeout": 1, "timeout_ms": 5}),
                ctx,
            )
            .await;
        assert_eq!(
            out.text_content(),
            "bash accepts timeout or timeout_ms, not both"
        );
    }

    #[tokio::test]
    async fn prefix_env_and_login_shell() {
        let dir = tempfile::tempdir().unwrap();
        let ctx = ToolContext::new(dir.path()).with_bash_options(BashOptions {
            command_prefix: Some("export GREETING=hello".into()),
            env: vec![
                ("QT_TEST_VALUE".into(), Some("42".into())),
                ("QT_UNSET_TEST".into(), None),
            ],
            shell_path: None,
        });
        let out = BashTool
            .execute(
                json!({"command": "echo $GREETING $QT_TEST_VALUE ${QT_UNSET_TEST:-unset}"}),
                ctx,
            )
            .await;
        assert!(out.text_content().trim_end().ends_with("hello 42 unset"));
        let details = out.details.unwrap();
        assert_eq!(
            details["resolved_command"],
            "export GREETING=hello\necho $GREETING $QT_TEST_VALUE ${QT_UNSET_TEST:-unset}"
        );
        if Path::new("/bin/bash").is_file() {
            let (program, flags) = shell(&BashOptions::default());
            assert_eq!(program, PathBuf::from("/bin/bash"));
            assert_eq!(flags, ["-lc"]);
        }
    }

    #[tokio::test]
    async fn times_out_and_aborts() {
        let ctx = ToolContext::new(std::env::temp_dir());
        let started = std::time::Instant::now();
        let out = BashTool
            .execute(json!({"command": "sleep 30", "timeout": 1}), ctx.clone())
            .await;
        assert!(out.text_content().contains("timed out after 1 seconds"));
        let details = out.details.unwrap();
        assert_eq!(details["timed_out"], true);
        assert_eq!(details["termination_reason"], "timeout");
        assert!(started.elapsed() < Duration::from_secs(10));
        let out = BashTool
            .execute(
                json!({"command": "sleep 30", "timeout_ms": 300}),
                ctx.clone(),
            )
            .await;
        assert!(
            out.text_content()
                .ends_with("Command timed out after 0.3 seconds")
        );
        assert_eq!(out.details.unwrap()["timeout_ms"], 300);

        let cancel_ctx = ctx.clone();
        let token = cancel_ctx.cancel.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(200)).await;
            token.cancel();
        });
        let out = BashTool
            .execute(json!({"command": "sleep 30"}), cancel_ctx)
            .await;
        assert!(out.text_content().ends_with("Command aborted"));
        let details = out.details.unwrap();
        assert_eq!(details["cancelled"], true);
        assert_eq!(details["termination_reason"], "cancelled");
    }

    #[tokio::test]
    async fn truncates_long_output_to_temp_file() {
        let ctx = ToolContext::new(std::env::temp_dir());
        let out = BashTool
            .execute(json!({"command": "seq 1 3000"}), ctx.clone())
            .await;
        let text = out.text_content();
        assert!(text.starts_with("1001\n"));
        assert!(text.contains("[Showing lines 1001-3000 of 3000. Full output: "));
        let details = out.details.unwrap();
        assert_eq!(details["truncation"]["truncatedBy"], "lines");
        let path = details["fullOutputPath"].as_str().unwrap().to_string();
        assert!(
            std::fs::read_to_string(&path)
                .unwrap()
                .starts_with("1\n2\n")
        );
        // The full output stays readable even outside the workspace.
        let other = tempfile::tempdir().unwrap();
        let read = super::super::ReadTool::new()
            .execute(
                json!({"path": path, "limit": 1}),
                ToolContext::new(other.path()),
            )
            .await;
        assert!(!read.is_error, "{}", read.text_content());
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn seconds_formatting() {
        assert_eq!(format_seconds(2000), "2");
        assert_eq!(format_seconds(1500), "1.5");
        assert_eq!(format_seconds(1), "0.001");
    }
}
