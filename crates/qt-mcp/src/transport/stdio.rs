//! stdio transport: a child process speaking newline-delimited JSON-RPC.
//!
//! The child gets a minimal environment (`PATH`, `HOME`, `USER`, `LANG`, `LC_ALL`,
//! `TERM`, `SHELL`, `TMPDIR`, `XDG_*`) plus the configured `env`, the resolved
//! command's directory is prepended to `PATH`, and stderr is appended to the shared
//! log file (mode 0600) or discarded when no log is configured.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::atomic::AtomicI64;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use serde_json::Value;
use tokio::io::{AsyncBufReadExt as _, AsyncWriteExt as _, BufReader};
use tokio::process::{Child, ChildStdin, ChildStdout, Command};
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use super::{Incoming, Pending, Reply, ServerRequests, await_reply, classify};
use crate::config::McpServerConfig;
use crate::error::McpError;

const DEFAULT_PATH: &str = "/usr/local/bin:/usr/bin:/bin";

pub(crate) struct StdioConnection {
    pub server: String,
    pub next_id: AtomicI64,
    writer: mpsc::UnboundedSender<String>,
    pending: Arc<Pending>,
    child: Mutex<Option<Child>>,
    tasks: Vec<JoinHandle<()>>,
}

impl StdioConnection {
    pub fn spawn(config: &McpServerConfig, requests: ServerRequests) -> Result<Self, McpError> {
        let command = config
            .command
            .as_deref()
            .filter(|c| !c.is_empty())
            .ok_or_else(|| McpError::Config("MCP stdio server has no command".into()))?;
        let mut env = safe_environment(std::env::vars(), &config.env);
        let program = resolve_command(command, env.get("PATH").map(String::as_str));
        if let Some(dir) = Path::new(&program)
            .parent()
            .filter(|d| !d.as_os_str().is_empty())
        {
            let path = env.get("PATH").cloned().unwrap_or_default();
            env.insert("PATH".into(), format!("{}:{path}", dir.display()));
        }

        let mut cmd = Command::new(&program);
        cmd.args(&config.args)
            .env_clear()
            .envs(&env)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(stderr_target(config.stderr_log_path.as_deref()))
            .kill_on_drop(true);
        let mut child = cmd.spawn().map_err(|e| {
            McpError::Transport(format!(
                "failed to start MCP stdio server {}: {e}",
                config.name
            ))
        })?;
        if let Ok(Some(_)) = child.try_wait() {
            return Err(McpError::Transport(format!(
                "MCP stdio server exited during startup: {}",
                config.name
            )));
        }
        let (Some(stdin), Some(stdout)) = (child.stdin.take(), child.stdout.take()) else {
            return Err(McpError::Transport(
                "failed to create MCP stdio pipes".into(),
            ));
        };

        let pending = Arc::new(Pending::default());
        let (writer, rx) = mpsc::unbounded_channel();
        let tasks = vec![
            tokio::spawn(write_loop(stdin, rx, pending.clone())),
            tokio::spawn(read_loop(stdout, pending.clone(), requests, writer.clone())),
        ];
        Ok(Self {
            server: config.name.clone(),
            next_id: AtomicI64::new(1),
            writer,
            pending,
            child: Mutex::new(Some(child)),
            tasks,
        })
    }

    /// Queue one message for the child's stdin.
    pub fn send(&self, message: &Value) -> Result<(), McpError> {
        if self.pending.is_closed() {
            return Err(McpError::Transport(
                "failed to write MCP stdio request".into(),
            ));
        }
        self.writer
            .send(message.to_string())
            .map_err(|_| McpError::Transport("failed to write MCP stdio request".into()))
    }

    pub async fn request(
        &self,
        id: i64,
        message: &Value,
        method: &str,
        timeout: Duration,
    ) -> Reply {
        let rx = self.pending.register(id)?;
        self.send(message)?;
        await_reply(
            &self.pending,
            id,
            rx,
            timeout,
            format!("MCP request timed out: {method} on {}", self.server),
        )
        .await
    }

    pub fn is_alive(&self) -> bool {
        !self.pending.is_closed()
    }

    /// Close stdin, send SIGTERM, and kill the process if it has not exited in 200 ms.
    pub async fn close(&self) {
        self.pending.close(McpError::Transport(format!(
            "MCP stdio server closed: {}",
            self.server
        )));
        for task in &self.tasks {
            task.abort();
        }
        let child = self.child.lock().unwrap_or_else(|e| e.into_inner()).take();
        let Some(mut child) = child else {
            return;
        };
        #[cfg(unix)]
        if let Some(pid) = child.id() {
            // SAFETY: plain signal delivery to the child we spawned.
            unsafe {
                libc::kill(pid as libc::pid_t, libc::SIGTERM);
            }
        }
        if tokio::time::timeout(Duration::from_millis(200), child.wait())
            .await
            .is_err()
        {
            let _ = child.kill().await;
        }
    }
}

impl Drop for StdioConnection {
    fn drop(&mut self) {
        for task in &self.tasks {
            task.abort();
        }
        // `kill_on_drop` terminates a child that was not closed explicitly.
    }
}

async fn write_loop(
    mut stdin: ChildStdin,
    mut rx: mpsc::UnboundedReceiver<String>,
    pending: Arc<Pending>,
) {
    while let Some(mut line) = rx.recv().await {
        line.push('\n');
        let written = async {
            stdin.write_all(line.as_bytes()).await?;
            stdin.flush().await
        }
        .await;
        if written.is_err() {
            pending.close(McpError::Transport(
                "failed to write MCP stdio request".into(),
            ));
            return;
        }
    }
}

async fn read_loop(
    stdout: ChildStdout,
    pending: Arc<Pending>,
    requests: ServerRequests,
    writer: mpsc::UnboundedSender<String>,
) {
    let mut reader = BufReader::new(stdout);
    let mut line = Vec::new();
    loop {
        line.clear();
        match reader.read_until(b'\n', &mut line).await {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let Ok(message) = serde_json::from_slice::<Value>(line.trim_ascii()) else {
            continue;
        };
        match classify(message) {
            Incoming::Response { id, reply } => pending.resolve(id, reply),
            Incoming::Request { id, method, params } => {
                let requests = requests.clone();
                let writer = writer.clone();
                tokio::spawn(async move {
                    let response = requests.handle(id, &method, params).await;
                    let _ = writer.send(response.to_string());
                });
            }
            Incoming::Notification | Incoming::Invalid => {}
        }
    }
    pending.close(McpError::Transport(format!(
        "MCP stdio server exited: {}",
        requests.server
    )));
}

fn stderr_target(log: Option<&Path>) -> Stdio {
    let Some(log) = log else {
        return Stdio::null();
    };
    if let Some(parent) = log.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let mut options = std::fs::OpenOptions::new();
    options.create(true).append(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt as _;
        options.mode(0o600);
    }
    match options.open(log) {
        Ok(file) => Stdio::from(file),
        Err(_) => Stdio::null(),
    }
}

fn safe_name(name: &str) -> bool {
    matches!(
        name,
        "PATH" | "HOME" | "USER" | "LANG" | "LC_ALL" | "TERM" | "SHELL" | "TMPDIR"
    ) || name.starts_with("XDG_")
}

/// The inherited variables a server may see, plus its configured `env`.
pub(crate) fn safe_environment(
    inherited: impl IntoIterator<Item = (String, String)>,
    configured: &BTreeMap<String, String>,
) -> BTreeMap<String, String> {
    let mut env: BTreeMap<String, String> = inherited
        .into_iter()
        .filter(|(name, _)| safe_name(name))
        .collect();
    env.extend(configured.iter().map(|(k, v)| (k.clone(), v.clone())));
    env.entry("PATH".into())
        .or_insert_with(|| DEFAULT_PATH.into());
    env
}

/// Search `PATH` for a bare command name; names containing `/` are used as given.
pub(crate) fn resolve_command(command: &str, path: Option<&str>) -> String {
    if command.contains('/') {
        return command.to_string();
    }
    for dir in path.unwrap_or(DEFAULT_PATH).split(':') {
        let candidate = PathBuf::from(if dir.is_empty() { "." } else { dir }).join(command);
        if is_executable(&candidate) {
            return candidate.to_string_lossy().into_owned();
        }
    }
    command.to_string()
}

fn is_executable(path: &Path) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt as _;
        path.metadata()
            .map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
            .unwrap_or(false)
    }
    #[cfg(not(unix))]
    {
        path.is_file()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn environment_is_filtered() {
        let inherited = vec![
            ("PATH".to_string(), "/p".to_string()),
            ("HOME".to_string(), "/h".to_string()),
            ("XDG_CONFIG_HOME".to_string(), "/x".to_string()),
            ("AWS_SECRET_ACCESS_KEY".to_string(), "s".to_string()),
            ("OPENAI_API_KEY".to_string(), "k".to_string()),
        ];
        let configured = BTreeMap::from([("TOKEN".to_string(), "t".to_string())]);
        let env = safe_environment(inherited, &configured);
        assert_eq!(
            env.keys().map(String::as_str).collect::<Vec<_>>(),
            ["HOME", "PATH", "TOKEN", "XDG_CONFIG_HOME"]
        );
        let env = safe_environment(Vec::new(), &BTreeMap::new());
        assert_eq!(env["PATH"], DEFAULT_PATH);
    }

    #[test]
    fn resolves_commands_on_path() {
        let dir = tempfile::tempdir().unwrap();
        let exe = dir.path().join("tool");
        std::fs::write(&exe, "#!/bin/sh\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            std::fs::set_permissions(&exe, std::fs::Permissions::from_mode(0o755)).unwrap();
        }
        let path = format!("/nonexistent:{}", dir.path().display());
        assert_eq!(resolve_command("tool", Some(&path)), exe.to_string_lossy());
        assert_eq!(resolve_command("missing-tool", Some(&path)), "missing-tool");
        assert_eq!(resolve_command("./rel/tool", Some(&path)), "./rel/tool");
    }
}
