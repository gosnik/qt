use std::path::PathBuf;
use std::process::Stdio;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use futures::future::BoxFuture;
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::io::{AsyncRead, AsyncReadExt as _};
use tokio::process::Command;

use super::{Tool, ToolContext, ToolOutput, de_opt_usize, parse_args};
use crate::util::{DEFAULT_MAX_BYTES, DEFAULT_MAX_LINES, truncate_tail};

pub struct BashTool;

#[derive(Deserialize)]
struct Args {
    command: String,
    #[serde(default, deserialize_with = "de_opt_usize")]
    timeout: Option<usize>,
}

/// Shell program and the flag that takes a command string.
fn shell() -> (PathBuf, Vec<&'static str>) {
    if let Some(bash) = find_in_path(if cfg!(windows) { "bash.exe" } else { "bash" }) {
        return (bash, vec!["-c"]);
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

fn find_in_path(program: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path)
        .map(|dir| dir.join(program))
        .find(|candidate| candidate.is_file())
}

enum Outcome {
    Exited(Option<i32>),
    TimedOut(u64),
    Aborted,
    SpawnFailed(String),
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
                "timeout": { "type": "number", "description": "Timeout in seconds (optional, no default timeout)" }
            },
            "required": ["command"]
        })
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("bash", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let output = Arc::new(Mutex::new(Vec::<u8>::new()));
            let outcome = run(
                &args.command,
                args.timeout.map(|t| t as u64),
                &ctx,
                output.clone(),
            )
            .await;
            let bytes = std::mem::take(&mut *output.lock().expect("output lock"));
            finish(outcome, &String::from_utf8_lossy(&bytes))
        })
    }
}

async fn run(
    command: &str,
    timeout: Option<u64>,
    ctx: &ToolContext,
    output: Arc<Mutex<Vec<u8>>>,
) -> Outcome {
    if !ctx.cwd.is_dir() {
        return Outcome::SpawnFailed(format!(
            "Working directory does not exist: {}",
            ctx.cwd.display()
        ));
    }
    let (program, flags) = shell();
    let mut cmd = Command::new(&program);
    cmd.args(&flags)
        .arg(command)
        .current_dir(&ctx.cwd)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .kill_on_drop(true);
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
        match timeout {
            Some(secs) if secs > 0 => tokio::time::sleep(Duration::from_secs(secs)).await,
            _ => std::future::pending::<()>().await,
        }
    };
    let outcome = tokio::select! {
        status = child.wait() => Outcome::Exited(status.ok().and_then(|s| s.code())),
        _ = deadline => Outcome::TimedOut(timeout.unwrap_or(0)),
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

async fn pump(mut stream: impl AsyncRead + Unpin, output: Arc<Mutex<Vec<u8>>>, ctx: ToolContext) {
    let mut buf = [0u8; 8192];
    loop {
        match stream.read(&mut buf).await {
            Ok(0) | Err(_) => break,
            Ok(n) => {
                let snapshot = {
                    let mut out = output.lock().expect("output lock");
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

fn finish(outcome: Outcome, raw: &str) -> ToolOutput {
    if let Outcome::SpawnFailed(msg) = outcome {
        return ToolOutput::error(msg);
    }
    let t = truncate_tail(raw, DEFAULT_MAX_LINES, DEFAULT_MAX_BYTES);
    let mut text = if t.text.trim().is_empty() {
        "(no output)".to_string()
    } else {
        t.text.clone()
    };
    let mut full_output_path = None;
    if t.truncated {
        let path = std::env::temp_dir().join(format!("qt-bash-{}.log", crate::util::short_id()));
        if std::fs::write(&path, raw).is_ok() {
            text.push_str(&format!(
                "\n\n[Showing last {} of {} lines. Full output: {}]",
                t.kept_lines,
                t.total_lines,
                path.display()
            ));
            full_output_path = Some(path);
        }
    }
    let (is_error, exit_code) = match outcome {
        Outcome::Exited(Some(0)) => (false, Some(0)),
        Outcome::Exited(code) => {
            match code {
                Some(c) => text.push_str(&format!("\n\nCommand exited with code {c}")),
                None => text.push_str("\n\nCommand terminated by signal"),
            }
            (true, code)
        }
        Outcome::TimedOut(secs) => {
            text.push_str(&format!("\n\nCommand timed out after {secs} seconds"));
            (true, None)
        }
        Outcome::Aborted => {
            text.push_str("\n\nCommand aborted");
            (true, None)
        }
        Outcome::SpawnFailed(_) => unreachable!("handled above"),
    };
    ToolOutput { is_error, ..ToolOutput::text(text) }
        .with_details(json!({ "exitCode": exit_code, "truncated": t.truncated, "fullOutputPath": full_output_path }))
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
        let out = BashTool
            .execute(json!({"command": "exit 3"}), ctx.clone())
            .await;
        assert!(out.is_error);
        assert!(out.text_content().ends_with("Command exited with code 3"));
        let out = BashTool.execute(json!({"command": "pwd"}), ctx).await;
        assert!(
            out.text_content()
                .contains(dir.path().file_name().unwrap().to_str().unwrap())
        );
    }

    #[tokio::test]
    async fn times_out_and_aborts() {
        let ctx = ToolContext::new(std::env::temp_dir());
        let started = std::time::Instant::now();
        let out = BashTool
            .execute(json!({"command": "sleep 30", "timeout": 1}), ctx.clone())
            .await;
        assert!(out.text_content().contains("timed out after 1 seconds"));
        assert!(started.elapsed() < Duration::from_secs(10));

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
    }

    #[tokio::test]
    async fn truncates_long_output_to_temp_file() {
        let ctx = ToolContext::new(std::env::temp_dir());
        let out = BashTool
            .execute(json!({"command": "seq 1 3000"}), ctx)
            .await;
        let text = out.text_content();
        assert!(text.starts_with("1001\n"));
        assert!(text.contains("[Showing last 2000 of 3000 lines. Full output: "));
        let path = out.details.unwrap()["fullOutputPath"]
            .as_str()
            .unwrap()
            .to_string();
        assert!(
            std::fs::read_to_string(&path)
                .unwrap()
                .starts_with("1\n2\n")
        );
        let _ = std::fs::remove_file(path);
    }
}
