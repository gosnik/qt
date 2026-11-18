//! `qt-cli`: run one prompt without the GUI and stream the answer to stdout.

use std::io::{IsTerminal as _, Read as _, Write as _};
use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context as _, Result, bail};
use qt_core::provider::{ModelRef, StreamEvent, ThinkingLevel};
use qt_core::{AgentEvent, AgentHome, ContentBlock, Engine, Message, StopReason};

const USAGE: &str = "\
Usage: qt-cli [options] <prompt...>

Runs one prompt through the qt agent in the current directory and prints the
answer. Reads the prompt from stdin when none is given.

Options:
  -m, --model <provider/id>   Model to use (default: settings defaultProvider/defaultModel)
  -t, --thinking <level>      off | low | medium | high
  -c, --continue              Continue the most recent session for this directory
  -s, --session <path>        Continue a specific session file
      --cwd <dir>             Working directory (default: current directory)
      --list-models           List models from configured providers and exit
      --list-sessions         List sessions for the working directory and exit
  -h, --help                  Show this help
";

#[derive(Default)]
struct Options {
    model: Option<ModelRef>,
    thinking: ThinkingLevel,
    continue_last: bool,
    session: Option<PathBuf>,
    cwd: Option<PathBuf>,
    list_models: bool,
    list_sessions: bool,
    prompt: Vec<String>,
}

fn parse_args() -> Result<Option<Options>> {
    let mut opts = Options::default();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        let mut value = |name: &str| args.next().with_context(|| format!("{name} needs a value"));
        match arg.as_str() {
            "-h" | "--help" => return Ok(None),
            "-m" | "--model" => {
                let spec = value("--model")?;
                opts.model = Some(
                    ModelRef::parse(&spec)
                        .with_context(|| format!("expected provider/model, got {spec:?}"))?,
                );
            }
            "-t" | "--thinking" => {
                let level = value("--thinking")?;
                opts.thinking = ThinkingLevel::parse(&level)
                    .with_context(|| format!("unknown thinking level {level:?}"))?;
            }
            "-c" | "--continue" => opts.continue_last = true,
            "-s" | "--session" => opts.session = Some(PathBuf::from(value("--session")?)),
            "--cwd" => opts.cwd = Some(PathBuf::from(value("--cwd")?)),
            "--list-models" => opts.list_models = true,
            "--list-sessions" => opts.list_sessions = true,
            "--" => {
                opts.prompt.extend(args.by_ref());
                break;
            }
            other if other.starts_with('-') && other.len() > 1 => {
                bail!("unknown option {other}\n\n{USAGE}")
            }
            other => opts.prompt.push(other.to_string()),
        }
    }
    Ok(Some(opts))
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => code,
        Err(err) => {
            eprintln!("qt-cli: {err:#}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<ExitCode> {
    let Some(opts) = parse_args()? else {
        print!("{USAGE}");
        return Ok(ExitCode::SUCCESS);
    };
    let cwd = match opts.cwd {
        Some(dir) => dir,
        None => std::env::current_dir()?,
    };
    let engine = Engine::new(AgentHome::from_env(), cwd)?;

    if opts.list_models {
        for (provider, result) in engine.block_on(engine.list_models())? {
            match result {
                Ok(models) => models.iter().for_each(|m| println!("{provider}/{}", m.id)),
                Err(err) => eprintln!("{provider}: {err}"),
            }
        }
        return Ok(ExitCode::SUCCESS);
    }
    if opts.list_sessions {
        for s in engine.sessions() {
            println!("{}\t{}", s.path.display(), s.title);
        }
        return Ok(ExitCode::SUCCESS);
    }

    let mut prompt = opts.prompt.join(" ");
    if prompt.trim().is_empty() && !std::io::stdin().is_terminal() {
        std::io::stdin().read_to_string(&mut prompt)?;
    }
    if prompt.trim().is_empty() {
        bail!("no prompt given\n\n{USAGE}");
    }

    let session_path = match (opts.session, opts.continue_last) {
        (Some(path), _) => Some(path),
        (None, true) => Some(
            engine
                .sessions()
                .first()
                .context("no previous session in this directory")?
                .path
                .clone(),
        ),
        _ => None,
    };
    let conversation = match session_path {
        Some(path) => engine.open_conversation(&path)?,
        None => engine.new_conversation(engine.default_model()),
    };
    if let Some(model) = opts.model {
        conversation.set_model(model);
    }
    conversation.set_thinking(opts.thinking);

    let rx = conversation.prompt(&prompt, Vec::new())?;
    let failed = engine.block_on(async move {
        let mut stdout = std::io::stdout();
        let mut failed = false;
        let mut text_open = false;
        while let Ok(event) = rx.recv().await {
            match event {
                AgentEvent::Stream(StreamEvent::TextDelta { delta, .. }) => {
                    print!("{delta}");
                    text_open = !delta.ends_with('\n');
                    let _ = stdout.flush();
                }
                AgentEvent::ToolStart(exec) => {
                    if text_open {
                        println!();
                        text_open = false;
                    }
                    eprintln!("→ {} {}", exec.name, compact_args(&exec.args));
                }
                AgentEvent::MessageEnd(Message::ToolResult(r)) if r.is_error => {
                    let first = r.text().lines().last().unwrap_or_default().to_string();
                    eprintln!("  ✗ {first}");
                }
                AgentEvent::MessageEnd(Message::Assistant(a)) => {
                    if a.stop_reason == StopReason::Error {
                        eprintln!("error: {}", a.error_message.unwrap_or_default());
                        failed = true;
                    } else if a.stop_reason == StopReason::Aborted {
                        failed = true;
                    }
                    // Keep image-only or empty answers visible as a newline boundary.
                    if a.content
                        .iter()
                        .any(|b| matches!(b, ContentBlock::Text { .. }))
                        && text_open
                    {
                        println!();
                        text_open = false;
                    }
                }
                AgentEvent::Warning(w) => eprintln!("warning: {w}"),
                AgentEvent::AgentEnd { usage, .. } => {
                    eprintln!(
                        "[{} in / {} out tokens{}]",
                        usage.input_tokens + usage.cache_read_tokens + usage.cache_write_tokens,
                        usage.output_tokens,
                        if usage.total_cost_usd > 0.0 {
                            format!(", ${:.4}", usage.total_cost_usd)
                        } else {
                            String::new()
                        }
                    );
                }
                _ => {}
            }
        }
        failed
    });
    eprintln!("session: {}", conversation.session_path().display());
    Ok(if failed {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    })
}

fn compact_args(args: &serde_json::Value) -> String {
    let text = match args
        .get("command")
        .or_else(|| args.get("path"))
        .or_else(|| args.get("pattern"))
    {
        Some(serde_json::Value::String(s)) => s.clone(),
        _ => args.to_string(),
    };
    let first = text.lines().next().unwrap_or_default();
    if first.chars().count() > 120 {
        format!("{}…", first.chars().take(119).collect::<String>())
    } else {
        first.to_string()
    }
}
