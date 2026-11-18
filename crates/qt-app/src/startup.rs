//! Command line of the desktop app: the project directory and the session
//! the window opens with (`--continue`, `--resume`, `--session`, `--fork`,
//! `--no-session`, `--session-dir`), as the archived interactive binary
//! accepted them.

use std::path::PathBuf;

use anyhow::{Context as _, Result};
use qt_core::Engine;

pub const USAGE: &str = "Usage: qt [options] [--cwd <dir> | <dir>]

Opens the qt desktop app for a project directory (default: current directory).
Use qt-cli for headless runs.

Options:
  -c, --continue          Continue the most recent session
  -r, --resume            Choose a session to resume
  --session <path|id>     Open a session file or session id prefix
  --fork <path|id>        Fork a session into a new session
  --no-session            Do not save sessions
  --session-dir <dir>     Session directory
  -h, --help              Show this help
";

/// The session the window opens with, as given on the command line.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum StartupSession {
    /// A new session.
    #[default]
    New,
    /// `--continue`: the most recent session (this folder first).
    Continue,
    /// `--resume`: the resume picker.
    Resume,
    /// `--session <path|id>`.
    Open(String),
    /// `--fork <path|id>`.
    Fork(String),
}

/// What the window does once it is open.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum StartupAction {
    /// Keep the new session.
    #[default]
    None,
    /// Resume this session file (asking first when its cwd is gone).
    Open(PathBuf),
    /// Fork this session file into a new session.
    Fork(PathBuf),
    /// Show the resume picker.
    Resume,
}

/// Parsed command line.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct StartupArgs {
    pub cwd: Option<PathBuf>,
    pub session: StartupSession,
    pub session_dir: Option<PathBuf>,
    pub no_session: bool,
    pub help: bool,
}

pub fn parse_args(args: impl IntoIterator<Item = String>) -> Result<StartupArgs> {
    let mut parsed = StartupArgs::default();
    let mut selections: Vec<&'static str> = Vec::new();
    let mut args = args.into_iter();
    while let Some(arg) = args.next() {
        let mut value = |name: &str| args.next().with_context(|| format!("{name} needs a value"));
        match arg.as_str() {
            "-h" | "--help" => parsed.help = true,
            "--cwd" => parsed.cwd = Some(PathBuf::from(value("--cwd")?)),
            "-c" | "--continue" => {
                parsed.session = StartupSession::Continue;
                selections.push("--continue");
            }
            "-r" | "--resume" => {
                parsed.session = StartupSession::Resume;
                selections.push("--resume");
            }
            "--session" => {
                parsed.session = StartupSession::Open(value("--session")?);
                selections.push("--session");
            }
            "--fork" => {
                parsed.session = StartupSession::Fork(value("--fork")?);
                selections.push("--fork");
            }
            "--no-session" => {
                parsed.no_session = true;
                selections.push("--no-session");
            }
            "--session-dir" => {
                parsed.session_dir = Some(PathBuf::from(value("--session-dir")?));
            }
            other if other.starts_with('-') => anyhow::bail!("unknown option {other}\n\n{USAGE}"),
            other => parsed.cwd = Some(PathBuf::from(other)),
        }
    }
    if selections.contains(&"--fork") && selections.len() > 1 {
        let others: Vec<&str> = selections
            .iter()
            .copied()
            .filter(|s| *s != "--fork")
            .collect();
        anyhow::bail!("--fork cannot be combined with {}", others.join(", "));
    }
    if parsed.no_session && parsed.session != StartupSession::New {
        let others: Vec<&str> = selections
            .iter()
            .copied()
            .filter(|s| *s != "--no-session")
            .collect();
        anyhow::bail!("--no-session cannot be combined with {}", others.join(", "));
    }
    Ok(parsed)
}

/// Resolve the startup session against the session catalog. Unknown ids use
/// the archived `No session found matching '…'`; an ambiguous prefix keeps
/// the catalog's message.
pub fn resolve(engine: &Engine, session: &StartupSession) -> Result<StartupAction> {
    let lookup = |arg: &str| -> Result<PathBuf> {
        let path = engine.resolve_session(arg)?;
        anyhow::ensure!(path.exists(), "No session found matching '{arg}'");
        Ok(path)
    };
    Ok(match session {
        StartupSession::New => StartupAction::None,
        StartupSession::Continue => engine
            .most_recent_session()
            .map_or(StartupAction::None, StartupAction::Open),
        StartupSession::Resume => StartupAction::Resume,
        StartupSession::Open(arg) => StartupAction::Open(lookup(arg)?),
        StartupSession::Fork(arg) => StartupAction::Fork(lookup(arg)?),
    })
}

#[cfg(test)]
mod tests {
    use super::{StartupAction, StartupSession, parse_args, resolve};
    use std::path::PathBuf;

    fn parse(args: &[&str]) -> anyhow::Result<super::StartupArgs> {
        parse_args(args.iter().map(|s| s.to_string()))
    }

    #[test]
    fn session_flags() {
        let a = parse(&["-c", "proj"]).unwrap();
        assert_eq!(a.session, StartupSession::Continue);
        assert_eq!(a.cwd, Some(PathBuf::from("proj")));
        assert_eq!(
            parse(&["--resume"]).unwrap().session,
            StartupSession::Resume
        );
        assert_eq!(
            parse(&["--session", "native-1"]).unwrap().session,
            StartupSession::Open("native-1".into())
        );
        let a = parse(&["--no-session", "--session-dir", "s"]).unwrap();
        assert!(a.no_session);
        assert_eq!(a.session_dir, Some(PathBuf::from("s")));
        assert_eq!(
            parse(&["--fork", "x", "-c"]).unwrap_err().to_string(),
            "--fork cannot be combined with --continue"
        );
        assert_eq!(
            parse(&["--no-session", "-r"]).unwrap_err().to_string(),
            "--no-session cannot be combined with --resume"
        );
        assert!(parse(&["--session"]).is_err());
        assert!(parse(&["--bogus"]).is_err());
        assert!(parse(&["-h"]).unwrap().help);
    }

    #[test]
    fn resolves_against_the_catalog() {
        let dir = tempfile::tempdir().unwrap();
        let home = dir.path().join("home");
        let work = dir.path().join("work");
        std::fs::create_dir_all(home.join("sessions")).unwrap();
        std::fs::create_dir_all(&work).unwrap();
        let header = format!(
            "{{\"type\":\"session\",\"version\":3,\"id\":\"native-123\",\"timestamp\":\"2026-01-01T00:00:00Z\",\"cwd\":\"{}\"}}\n",
            work.display()
        );
        let file = home.join("sessions/native-123.jsonl");
        std::fs::write(&file, header).unwrap();
        let engine = qt_core::Engine::builder(qt_core::config::AgentHome::new(&home), &work)
            .startup_migrations(false)
            .build()
            .unwrap();
        assert_eq!(
            resolve(&engine, &StartupSession::Open("native-1".into())).unwrap(),
            StartupAction::Open(file.clone())
        );
        assert_eq!(
            resolve(&engine, &StartupSession::Continue).unwrap(),
            StartupAction::Open(file.clone())
        );
        assert_eq!(
            resolve(&engine, &StartupSession::Fork("native-1".into())).unwrap(),
            StartupAction::Fork(file)
        );
        assert_eq!(
            resolve(&engine, &StartupSession::Open("zzz".into()))
                .unwrap_err()
                .to_string(),
            "No session found matching 'zzz'"
        );
    }
}
