//! Subprocess JSON-RPC extensions (archived `runner.cpp`,
//! `subprocess_channel.cpp`, `extension_contribution_runner.cpp`).
//!
//! Every invocation starts the extension's executable, writes one request
//! line to its stdin and closes it, then reads stdout until the matching
//! response. Other stdout lines must be valid JSON-RPC messages (requests from
//! the extension cannot be answered because stdin is closed); stderr lines
//! become diagnostics.

use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde_json::Value;
use tokio_util::sync::CancellationToken;

use crate::filters::display_path;
use crate::jsonrpc::{LineBuffer, Message, RpcError};
use crate::loader::LoadedExtension;
use crate::manifest::Transport;

/// Default time limit of one invocation (archived 30 s).
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(30);

/// How to start a subprocess extension.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LaunchPlan {
    pub executable: PathBuf,
    pub args: Vec<String>,
    pub working_directory: PathBuf,
}

fn resolve(base: &Path, candidate: &str) -> PathBuf {
    let p = Path::new(candidate);
    if p.is_absolute() {
        p.to_path_buf()
    } else {
        base.join(p)
    }
}

/// Validate the manifest and resolve paths against the extension directory.
pub fn launch_plan(extension: &LoadedExtension) -> Result<LaunchPlan, String> {
    let m = &extension.manifest;
    if extension.directory.as_os_str().is_empty() {
        return Err("extension directory is required".into());
    }
    if m.name.is_empty() {
        return Err("extension manifest name is required".into());
    }
    if m.transport != Transport::SubprocessJsonRpc {
        return Err(format!(
            "unsupported extension runner transport: {}",
            m.transport.as_str()
        ));
    }
    if m.executable.is_empty() {
        return Err("extension executable is required".into());
    }
    let executable = resolve(&extension.directory, &m.executable);
    if !executable.is_file() {
        return Err(format!(
            "extension executable is not a readable file: {}",
            display_path(&executable)
        ));
    }
    let wd = m
        .working_directory
        .clone()
        .unwrap_or_else(|| display_path(&extension.directory));
    if wd.is_empty() {
        return Err("extension working_directory resolved to an empty path".into());
    }
    let working_directory = resolve(&extension.directory, &wd);
    if !working_directory.is_dir() {
        return Err(format!(
            "extension working_directory is not a directory: {}",
            display_path(&working_directory)
        ));
    }
    if let Some(i) = m.args.iter().position(String::is_empty) {
        return Err(format!("extension argument at index {i} must not be empty"));
    }
    Ok(LaunchPlan {
        executable,
        args: m.args.clone(),
        working_directory,
    })
}

/// A completed invocation.
#[derive(Debug, Clone, PartialEq)]
pub struct Invocation {
    /// The response `result`, or the JSON-RPC error.
    pub outcome: Result<Value, RpcError>,
    /// stderr lines.
    pub diagnostics: Vec<String>,
}

fn read_all(mut reader: impl Read + Send + 'static) -> std::thread::JoinHandle<Vec<u8>> {
    std::thread::spawn(move || {
        let mut buf = Vec::new();
        let _ = reader.read_to_end(&mut buf);
        buf
    })
}

/// Run one request. `Err` covers launch failures, timeouts, cancellation,
/// malformed output and a missing response.
pub fn invoke(
    plan: &LaunchPlan,
    request: &Message,
    timeout: Duration,
    cancel: Option<&CancellationToken>,
) -> Result<Invocation, String> {
    let Message::Request { id, .. } = request else {
        return Err("extension invocation requires a JSON-RPC request".into());
    };
    let mut child = Command::new(&plan.executable)
        .args(&plan.args)
        .current_dir(&plan.working_directory)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("{}: {e}", display_path(&plan.executable)))?;
    if let Some(mut stdin) = child.stdin.take() {
        // A process that exits without reading its input is reported below.
        let _ = stdin.write_all(request.encode_line().as_bytes());
    }
    let stdout = read_all(child.stdout.take().expect("piped stdout"));
    let stderr = read_all(child.stderr.take().expect("piped stderr"));
    let started = Instant::now();
    let status = loop {
        if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
            break status;
        }
        if cancel.is_some_and(CancellationToken::is_cancelled) {
            let _ = child.kill();
            let _ = child.wait();
            return Err("cancelled".into());
        }
        if started.elapsed() >= timeout {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("timed out after {} ms", timeout.as_millis()));
        }
        std::thread::sleep(Duration::from_millis(5));
    };
    let out = stdout.join().unwrap_or_default();
    let err = stderr.join().unwrap_or_default();
    let mut err_buffer = LineBuffer::default();
    let mut diagnostics = err_buffer.push(&err);
    diagnostics.extend(err_buffer.finish());

    let mut buffer = LineBuffer::default();
    let mut lines = buffer.push(&out);
    lines.extend(buffer.finish());
    for line in lines {
        match Message::decode_line(&line)? {
            Message::Response { id: rid, result } if rid == *id => {
                return Ok(Invocation {
                    outcome: Ok(result),
                    diagnostics,
                });
            }
            Message::Error { id: rid, error } if rid == *id => {
                return Ok(Invocation {
                    outcome: Err(error),
                    diagnostics,
                });
            }
            _ => {}
        }
    }
    let mut message = String::from("did not return a JSON-RPC response");
    if !status.success() {
        match status.code() {
            Some(code) => message.push_str(&format!("; exit code {code}")),
            None => message.push_str("; terminated by signal"),
        }
    }
    Err(message)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::loader::ExtensionOrigin;
    use crate::manifest::ExtensionManifest;
    use serde_json::json;
    use std::os::unix::fs::PermissionsExt;

    fn extension(dir: &Path, script: &str) -> LoadedExtension {
        let exe = dir.join("ext.sh");
        std::fs::write(&exe, format!("#!/bin/sh\n{script}\n")).unwrap();
        std::fs::set_permissions(&exe, std::fs::Permissions::from_mode(0o755)).unwrap();
        LoadedExtension {
            root: display_path(dir),
            directory: dir.to_path_buf(),
            path: dir.join("extension.json"),
            manifest: ExtensionManifest {
                name: "sub".into(),
                executable: "ext.sh".into(),
                ..Default::default()
            },
            script: None,
            origin: ExtensionOrigin::Cli,
        }
    }

    fn request() -> Message {
        Message::request(
            json!(7),
            "qt.command.invoke",
            json!({"name":"x","arguments":{}}),
        )
        .unwrap()
    }

    #[test]
    fn round_trip_and_errors() {
        let dir = tempfile::tempdir().unwrap();
        let ext = extension(
            dir.path(),
            r#"read line; echo "log line" >&2
echo '{"jsonrpc":"2.0","method":"note","params":{}}'
echo '{"jsonrpc":"2.0","id":7,"result":{"text":"ok"}}'"#,
        );
        let plan = launch_plan(&ext).unwrap();
        let r = invoke(&plan, &request(), DEFAULT_TIMEOUT, None).unwrap();
        assert_eq!(r.outcome, Ok(json!({"text":"ok"})));
        assert_eq!(r.diagnostics, ["log line"]);

        let ext = extension(
            dir.path(),
            r#"echo '{"jsonrpc":"2.0","id":7,"error":{"code":-32000,"message":"boom"}}'"#,
        );
        let r = invoke(
            &launch_plan(&ext).unwrap(),
            &request(),
            DEFAULT_TIMEOUT,
            None,
        )
        .unwrap();
        assert_eq!(r.outcome.unwrap_err().message, "boom");

        let ext = extension(dir.path(), "exit 3");
        assert_eq!(
            invoke(
                &launch_plan(&ext).unwrap(),
                &request(),
                DEFAULT_TIMEOUT,
                None
            )
            .unwrap_err(),
            "did not return a JSON-RPC response; exit code 3"
        );
        let ext = extension(dir.path(), "echo not-json");
        assert!(
            invoke(
                &launch_plan(&ext).unwrap(),
                &request(),
                DEFAULT_TIMEOUT,
                None
            )
            .is_err()
        );
        let ext = extension(dir.path(), "sleep 5");
        assert_eq!(
            invoke(
                &launch_plan(&ext).unwrap(),
                &request(),
                Duration::from_millis(100),
                None
            )
            .unwrap_err(),
            "timed out after 100 ms"
        );
        let token = CancellationToken::new();
        token.cancel();
        assert_eq!(
            invoke(
                &launch_plan(&ext).unwrap(),
                &request(),
                DEFAULT_TIMEOUT,
                Some(&token)
            )
            .unwrap_err(),
            "cancelled"
        );
    }

    #[test]
    fn launch_validation() {
        let dir = tempfile::tempdir().unwrap();
        let mut ext = extension(dir.path(), "true");
        ext.manifest.executable = "missing".into();
        assert!(
            launch_plan(&ext)
                .unwrap_err()
                .starts_with("extension executable is not a readable file: ")
        );
        ext.manifest.executable = "ext.sh".into();
        ext.manifest.working_directory = Some("nope".into());
        assert!(
            launch_plan(&ext)
                .unwrap_err()
                .starts_with("extension working_directory is not a directory: ")
        );
        ext.manifest.working_directory = None;
        ext.manifest.args = vec!["a".into(), String::new()];
        assert_eq!(
            launch_plan(&ext).unwrap_err(),
            "extension argument at index 1 must not be empty"
        );
        ext.manifest.transport = Transport::Rhai;
        assert_eq!(
            launch_plan(&ext).unwrap_err(),
            "unsupported extension runner transport: rhai"
        );
    }
}
