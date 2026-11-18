//! Startup session selection (archived `resolve_startup_session`):
//! `--fork`, `--no-session`, `--session`, `--continue`, `--resume` and the
//! missing-cwd prompt.

use std::io::{BufRead as _, IsTerminal as _, Write as _};
use std::path::{Path, PathBuf};

use qt_core::engine::{Conversation, Engine};
use qt_core::provider::ModelRef;
use qt_core::session::{SessionCwdIssue, looks_like_path};

use crate::args::CliArgs;

/// Why no conversation was opened.
#[derive(Debug, PartialEq, Eq)]
pub enum StartupStop {
    /// Print this to stdout and exit with the code (`No session selected`,
    /// a cancelled missing-cwd prompt).
    Message(String, i32),
    /// Print this to stderr and exit 1.
    Error(String),
}

fn has_header(path: &Path) -> bool {
    std::fs::read_to_string(path)
        .ok()
        .and_then(|t| t.lines().next().map(str::to_string))
        .and_then(|l| serde_json::from_str::<serde_json::Value>(&l).ok())
        .is_some_and(|v| v.get("type").and_then(|t| t.as_str()) == Some("session"))
}

fn missing(arg: &str) -> StartupStop {
    StartupStop::Error(format!("No session found matching '{arg}'"))
}

/// A failed id-prefix lookup: no match prints the archived bare
/// `No session found matching '…'`; other failures (an ambiguous prefix)
/// are `Error: …` lines.
fn lookup_failed(arg: &str, error: &anyhow::Error) -> StartupStop {
    let message = format!("{error:#}");
    if message.starts_with("No session found matching") {
        missing(arg)
    } else {
        StartupStop::Error(format!("Error: {message}"))
    }
}

/// Ask `Continue/Cancel` on the terminal (archived prompt). `true` continues.
fn confirm_missing_cwd(issue: &SessionCwdIssue) -> bool {
    print!("{}\n\n[Continue/Cancel] ", issue.prompt_text());
    let _ = std::io::stdout().flush();
    let mut line = String::new();
    if std::io::stdin().lock().read_line(&mut line).unwrap_or(0) == 0 {
        println!();
        return false;
    }
    let choice = line.trim().to_ascii_lowercase();
    choice == "continue" || choice == "c" || choice == "y" || choice == "yes"
}

fn open_checked(
    engine: &Engine,
    path: &Path,
    interactive: bool,
) -> Result<Conversation, StartupStop> {
    if let Some(issue) = engine.session_cwd_issue(path) {
        let force = std::env::var("QT_FORCE_MISSING_SESSION_CWD_PROMPT").is_ok_and(|v| v == "1");
        if interactive || force {
            if !confirm_missing_cwd(&issue) {
                return Err(StartupStop::Message(String::new(), 0));
            }
            return engine
                .open_conversation_in(path, engine.cwd())
                .map_err(|e| StartupStop::Error(format!("{e:#}")));
        }
        return Err(StartupStop::Error(issue.error_text()));
    }
    engine
        .open_conversation(path)
        .map_err(|e| StartupStop::Error(format!("{e:#}")))
}

/// Numbered session picker on the terminal (`--resume`).
fn pick_session(engine: &Engine) -> Option<PathBuf> {
    let sessions = engine.sessions();
    if sessions.is_empty() {
        return None;
    }
    eprintln!("Sessions:");
    for (i, s) in sessions.iter().enumerate().take(30) {
        eprintln!("  {:>2}. {}  {}", i + 1, s.title, s.path.display());
    }
    eprint!("Resume which session? ");
    let _ = std::io::stderr().flush();
    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).ok()?;
    let index: usize = line.trim().parse().ok()?;
    sessions.get(index.checked_sub(1)?).map(|s| s.path.clone())
}

/// The conversation to run, per the session flags.
pub fn open(
    engine: &Engine,
    parsed: &CliArgs,
    model: Option<ModelRef>,
    interactive: bool,
) -> Result<Conversation, StartupStop> {
    if let Some(source) = &parsed.fork {
        let path = engine
            .resolve_session(source)
            .map_err(|e| lookup_failed(source, &e))?;
        if looks_like_path(source) && !has_header(&path) {
            return Err(StartupStop::Error(format!(
                "Error: Cannot fork: source session file is empty or invalid: {source}"
            )));
        }
        if !path.exists() {
            return Err(missing(source));
        }
        return engine
            .fork_session(&path)
            .map_err(|e| StartupStop::Error(format!("Error: {e:#}")));
    }
    if parsed.no_session {
        return Ok(engine.new_ephemeral_conversation(model));
    }
    if let Some(arg) = &parsed.session {
        let path = engine
            .resolve_session(arg)
            .map_err(|e| lookup_failed(arg, &e))?;
        if !path.exists() {
            return Err(missing(arg));
        }
        return open_checked(engine, &path, interactive);
    }
    if parsed.continue_session {
        return match engine.most_recent_session() {
            Some(path) => open_checked(engine, &path, interactive),
            None => Ok(engine.new_conversation(model)),
        };
    }
    if parsed.resume {
        // Without a terminal to pick on, the most recent session (this
        // working directory first) is resumed, as in the archived binary.
        let picked = if std::io::stdin().is_terminal() {
            pick_session(engine)
        } else {
            engine.most_recent_session()
        };
        return match picked {
            Some(path) => open_checked(engine, &path, interactive),
            None => Err(StartupStop::Message("No session selected".into(), 0)),
        };
    }
    Ok(engine.new_conversation(model))
}
