//! Command line of the desktop app. It is parsed with the parser `qt-cli`
//! uses ([`qt_runtime::cli_args`], archived `parse_cli_args`), so the model,
//! tool, resource, extension and log flags reach the runtime exactly as in
//! `qt-cli`. On top of that come `--cwd`, the session the window opens with
//! (`--continue`, `--resume`, `--session`, `--fork`, `--no-session`,
//! `--session-dir`) and the initial prompt (messages, `@file` arguments and
//! piped stdin). Headless-only flags (`-p`, `--mode`, `--export`,
//! `--list-models`) are refused with a pointer to `qt-cli`.

use std::path::{Path, PathBuf};

use anyhow::Result;
use qt_core::Engine;
use qt_runtime::cli_args::CliArgs;

pub const USAGE: &str = "\
qt - desktop coding agent with read, bash, edit, write tools

Usage:
  qt [options] [@files...] [messages...]
  qt [options] <dir>

The window opens on the project directory: --cwd, or a single argument that
is an existing directory written as a path (`.`, `..`, absolute or containing
`/`), else the current directory. Messages and @files are sent as the first
prompt once the window is open (piped stdin is prepended to it); further
messages follow one by one. Use qt-cli for headless runs (print, JSON and RPC
modes, --export, --list-models).

Options:
  --cwd <dir>                    Project directory (default: current directory)
  --provider <name>              Provider name
  --model <pattern>              Model pattern or ID (supports \"provider/id\" and optional \":<thinking>\")
  --api-key <key>                API key (defaults to env vars)
  --system-prompt <text>         System prompt (default: coding assistant prompt)
  --append-system-prompt <text>  Append text or file contents to the system prompt (can be used multiple times)
  --continue, -c                 Continue the most recent session
  --resume, -r                   Choose a session to resume
  --session <path|id>            Open a session file or session id prefix
  --fork <path|id>               Fork a session into a new session
  --session-dir <dir>            Directory for session storage and lookup
  --no-session                   Don't save session (ephemeral)
  --models <patterns>            Comma-separated model patterns for model cycling
  --no-tools, -nt                Disable all tools by default (built-in and extension)
  --no-builtin-tools, -nbt       Disable built-in tools by default but keep extension/custom tools enabled
  --no-memory                    Disable built-in memory for this invocation
  --tools, -t <tools>            Comma-separated allowlist of tool names to enable
  --thinking <level>             Set thinking level: off, minimal, low, medium, high, xhigh
  --extension, -e <path>         Load an extension file (can be used multiple times)
  --no-extensions, -ne           Disable extension discovery (explicit -e paths still work)
  --skill <path>                 Load a skill file or directory (can be used multiple times)
  --no-skills, -ns               Disable skills discovery and loading
  --prompt-template <path>       Load a prompt template file or directory (can be used multiple times)
  --no-prompt-templates, -np     Disable prompt template discovery and loading
  --theme <path>                 Load a theme file or directory (can be used multiple times)
  --no-themes                    Disable theme discovery and loading
  --no-context-files, -nc        Disable AGENTS.md and CLAUDE.md discovery and loading
  --discover-local-models <dir>  Scan local llama.cpp/GGUF model directory (repeatable; comma-separated dirs accepted)
  --verbose                      Show startup warnings even with the quietStartup setting
  --offline                      Disable startup network operations (same as PI_OFFLINE=1)
  --log-file <path>              Write structured runtime logs to a file (repeatable)
  --log-stderr                   Also write runtime logs to stderr
  --log-level <level>            Log level: trace, debug, info, warn, error, critical, off
  --help, -h                     Show this help
  --version, -v                  Show version number

Extensions can register additional flags (e.g., --plan from plan-mode extension).
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
#[derive(Debug, Clone, Default)]
pub struct StartupArgs {
    /// The shared flags, messages and `@file` arguments (without `--cwd` and
    /// a directory argument).
    pub cli: CliArgs,
    pub cwd: Option<PathBuf>,
    pub session: StartupSession,
}

impl StartupArgs {
    /// Messages or `@file` arguments were given.
    pub fn has_initial_input(&self) -> bool {
        !self.cli.messages.is_empty() || !self.cli.file_args.is_empty()
    }
}

/// A single positional argument opens a project directory when it names an
/// existing directory and is written as a path (`.`, `..`, absolute, `~/…`
/// or containing a separator); a bare word is always a message.
fn directory_argument(arg: &str) -> Option<PathBuf> {
    let looks_like_path = arg == "."
        || arg == ".."
        || arg.contains('/')
        || arg.contains(std::path::MAIN_SEPARATOR)
        || Path::new(arg).is_absolute();
    if !looks_like_path {
        return None;
    }
    let path = match arg.strip_prefix("~/") {
        Some(rest) => std::env::home_dir()?.join(rest),
        None => PathBuf::from(arg),
    };
    path.is_dir().then_some(path)
}

/// Take `--cwd <dir>` / `--cwd=<dir>` out of the unknown flags the shared
/// parser collected.
fn take_cwd(unknown: &mut Vec<String>) -> Result<Option<PathBuf>> {
    let mut cwd = None;
    let mut rest = Vec::new();
    let mut flags = std::mem::take(unknown).into_iter().peekable();
    while let Some(flag) = flags.next() {
        if let Some(value) = flag.strip_prefix("--cwd=") {
            anyhow::ensure!(!value.is_empty(), "--cwd needs a value");
            cwd = Some(PathBuf::from(value));
        } else if flag == "--cwd" {
            match flags.next_if(|next| !next.starts_with('-')) {
                Some(value) => cwd = Some(PathBuf::from(value)),
                None => anyhow::bail!("--cwd needs a value"),
            }
        } else {
            rest.push(flag);
        }
    }
    *unknown = rest;
    Ok(cwd)
}

pub fn parse_args(args: impl IntoIterator<Item = String>) -> Result<StartupArgs> {
    let raw: Vec<String> = args.into_iter().collect();
    let mut cli = qt_runtime::cli_args::parse(&raw);
    let mut cwd = take_cwd(&mut cli.unknown_flags)?;
    if cli.help || cli.version {
        return Ok(StartupArgs {
            cli,
            cwd,
            ..Default::default()
        });
    }
    let headless = cli.print_only_flags();
    if !headless.is_empty() {
        anyhow::bail!(
            "{} {} only available in qt-cli, the headless front end (for example: qt-cli -p \"<prompt>\"); the qt desktop app opens a window",
            headless.join(", "),
            if headless.len() == 1 { "is" } else { "are" }
        );
    }
    if cwd.is_none()
        && cli.messages.len() == 1
        && cli.file_args.is_empty()
        && let Some(dir) = directory_argument(&cli.messages[0])
    {
        cwd = Some(dir);
        cli.messages.clear();
    }
    if let Some(error) = cli.fork_error() {
        anyhow::bail!(error);
    }
    if cli.no_session {
        let others: Vec<&str> = [
            (cli.continue_session, "--continue"),
            (cli.resume, "--resume"),
            (cli.session.is_some(), "--session"),
        ]
        .into_iter()
        .filter_map(|(set, name)| set.then_some(name))
        .collect();
        if !others.is_empty() {
            anyhow::bail!("--no-session cannot be combined with {}", others.join(", "));
        }
    }
    if let Some(error) = cli.api_key_error() {
        anyhow::bail!(error);
    }
    // Archived precedence: --fork, --session, --resume, --continue.
    let session = if let Some(fork) = &cli.fork {
        StartupSession::Fork(fork.clone())
    } else if let Some(session) = &cli.session {
        StartupSession::Open(session.clone())
    } else if cli.resume {
        StartupSession::Resume
    } else if cli.continue_session {
        StartupSession::Continue
    } else {
        StartupSession::New
    };
    let parsed = StartupArgs { cli, cwd, session };
    if parsed.session == StartupSession::Resume && parsed.has_initial_input() {
        anyhow::bail!(
            "--resume opens the session picker and cannot be combined with an initial message; use --continue or --session <path|id>"
        );
    }
    Ok(parsed)
}

/// Piped stdin (archived: stdin that is not a terminal is read whole and
/// prepended to the initial message). Only pipes and regular files are read,
/// so a launcher's `/dev/null`, socket or other device never blocks the
/// window; empty input is ignored.
pub fn read_piped_stdin() -> Option<String> {
    use std::io::{IsTerminal as _, Read as _};
    let stdin = std::io::stdin();
    if stdin.is_terminal() {
        return None;
    }
    #[cfg(unix)]
    {
        use std::os::fd::AsFd as _;
        use std::os::unix::fs::FileTypeExt as _;
        let file = std::fs::File::from(stdin.as_fd().try_clone_to_owned().ok()?);
        let kind = file.metadata().ok()?.file_type();
        if !(kind.is_fifo() || kind.is_file()) {
            return None;
        }
    }
    let mut content = String::new();
    stdin.lock().read_to_string(&mut content).ok()?;
    let trimmed = content.trim();
    (!trimmed.is_empty()).then(|| trimmed.to_string())
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
        let a = parse(&["-c", "--cwd", "proj"]).unwrap();
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
        assert!(a.cli.no_session);
        assert_eq!(a.cli.session_dir.as_deref(), Some("s"));
        assert_eq!(
            parse(&["--fork", "x", "-c"]).unwrap_err().to_string(),
            "--fork cannot be combined with --continue"
        );
        assert_eq!(
            parse(&["--no-session", "-r"]).unwrap_err().to_string(),
            "--no-session cannot be combined with --resume"
        );
        assert_eq!(
            parse(&["-c", "-r"]).unwrap().session,
            StartupSession::Resume
        );
        assert!(parse(&["-h"]).unwrap().cli.help);
        assert!(parse(&["-v"]).unwrap().cli.version);
    }

    #[test]
    fn shared_flags_messages_and_cwd() {
        let a = parse(&[
            "--model",
            "ollama/qwen",
            "--thinking",
            "high",
            "-e",
            "x.rhai",
            "--no-tools",
            "--cwd=/tmp",
            "--plan",
            "@notes.md",
            "hello",
            "again",
        ])
        .unwrap();
        assert_eq!(a.cli.model.as_deref(), Some("ollama/qwen"));
        assert_eq!(a.cli.extensions, ["x.rhai"]);
        assert!(a.cli.no_tools);
        assert_eq!(a.cwd, Some(PathBuf::from("/tmp")));
        assert_eq!(a.cli.unknown_flags, ["--plan"]);
        assert_eq!(a.cli.file_args, ["notes.md"]);
        assert_eq!(a.cli.messages, ["hello", "again"]);
        assert!(a.has_initial_input());
        // `--cwd` before another flag has no value.
        assert_eq!(
            parse(&["--cwd", "--plan"]).unwrap_err().to_string(),
            "--cwd needs a value"
        );
        assert!(parse(&["--cwd"]).is_err());
        // Unknown short options are parse errors, reported by main.
        assert!(!parse(&["-z"]).unwrap().cli.diagnostics.is_empty());
    }

    #[test]
    fn headless_flags_point_at_qt_cli() {
        let err = parse(&["-p", "hi"]).unwrap_err().to_string();
        assert!(
            err.starts_with("--print is only available in qt-cli"),
            "{err}"
        );
        let err = parse(&["--mode", "json", "--export", "s.jsonl"])
            .unwrap_err()
            .to_string();
        assert!(
            err.starts_with("--mode, --export are only available in qt-cli"),
            "{err}"
        );
        assert!(parse(&["--list-models"]).is_err());
        assert!(
            parse(&["-r", "hello"])
                .unwrap_err()
                .to_string()
                .starts_with("--resume opens the session picker")
        );
        assert!(parse(&["--api-key", "k"]).is_err());
    }

    #[test]
    fn directory_argument_compatibility() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().to_string_lossy().into_owned();
        // An existing directory written as a path opens it.
        let a = parse(&[path.as_str()]).unwrap();
        assert_eq!(a.cwd, Some(dir.path().to_path_buf()));
        assert!(a.cli.messages.is_empty());
        let a = parse(&["."]).unwrap();
        assert_eq!(a.cwd, Some(PathBuf::from(".")));
        // A bare word is a message even when such a directory exists.
        let a = parse(&["src"]).unwrap();
        assert_eq!(a.cwd, None);
        assert_eq!(a.cli.messages, ["src"]);
        // Several arguments, a missing directory or an explicit --cwd keep
        // every positional a message.
        let a = parse(&[path.as_str(), "explain"]).unwrap();
        assert_eq!(a.cli.messages.len(), 2);
        let a = parse(&["./definitely-missing-dir/"]).unwrap();
        assert_eq!(a.cli.messages, ["./definitely-missing-dir/"]);
        let a = parse(&["--cwd", "/tmp", "."]).unwrap();
        assert_eq!(a.cwd, Some(PathBuf::from("/tmp")));
        assert_eq!(a.cli.messages, ["."]);
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
