//! Executing slash commands against a conversation.

use std::path::PathBuf;

use anyhow::{Result, anyhow, bail};
use async_channel::Receiver;
use tokio::task::JoinHandle;

use super::{SlashCommandSource, info, parse_invocation};
use crate::agent::AgentEvent;
use crate::engine::{Conversation, ModelListing};
use crate::provider::{ModelInfo, ModelRef, ThinkingLevel};

/// What the front end has to show or do for a command it must handle
/// itself (pickers, panels, subsystem routing, quitting).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SlashAction {
    /// `/settings [setting]`.
    OpenSettings { target: Option<String> },
    /// `/extensions`, `/skills`, `/prompts`, `/themes`, or `/settings
    /// <resources|packages|…> [filter]`: the package resource selector.
    OpenPackageResources {
        kind: String,
        filter: Option<String>,
    },
    /// `/model [search]` without an exact match.
    SelectModel { search: Option<String> },
    /// `/scoped-models`: the current `enabledModels` patterns.
    SelectScopedModels { enabled: Vec<String> },
    /// `/thinking` without a level: pick from the model's levels.
    SelectThinking {
        current: ThinkingLevel,
        levels: Vec<ThinkingLevel>,
    },
    /// `/tree [hint]`.
    OpenTree { hint: Option<String> },
    /// `/resume`.
    ResumeSession,
    /// `/fork`: pick a user message (`(entry id, text)`, oldest first);
    /// then call [`Conversation::fork`] with `at = false`
    /// ("Forked to new session.").
    ForkFromMessage { messages: Vec<(String, String)> },
    /// `/login [provider]`.
    Login { provider: Option<String> },
    /// `/logout [provider]`.
    Logout { provider: Option<String> },
    /// `/tools [filter]`: `(name, description, active)`; toggle with
    /// [`Conversation::set_tool_enabled`].
    SelectTools {
        filter: Option<String>,
        tools: Vec<(String, String, bool)>,
    },
    /// `/import` whose stored cwd no longer exists: offer "Continue in
    /// current cwd" ([`crate::Engine::import_session`] with the fallback).
    ImportMissingCwd {
        path: PathBuf,
        session_cwd: String,
        fallback_cwd: PathBuf,
    },
    /// `/llama`, `/ollama`, `/subagent`, `/index`, `/memory`, `/mcp`: routed
    /// to the subsystem that owns the command.
    Subsystem { command: String, arguments: String },
    /// An extension command.
    Extension {
        name: String,
        target: String,
        arguments: String,
        source_path: Option<PathBuf>,
    },
    /// `/quit`.
    Quit,
}

/// Result of [`dispatch`].
pub enum SlashOutcome {
    /// Lines for the transcript.
    Output(Vec<String>),
    /// A prompt template or skill expanded into the user message to send.
    Prompt(String),
    /// A run started (`/compact`, a `/goal` continuation); `lines` come first.
    Run {
        lines: Vec<String>,
        events: Receiver<AgentEvent>,
    },
    /// A background task whose result lines (or error) arrive later
    /// (`/share`).
    Task(JoinHandle<Result<Vec<String>>>),
    /// `/model refresh` started.
    Refresh {
        lines: Vec<String>,
        listing: JoinHandle<ModelListing>,
    },
    /// The front end should switch to `conversation` (`/new`, `/clone`,
    /// `/import`) and show `message`.
    SwitchConversation {
        conversation: Conversation,
        message: String,
    },
    /// `/copy`: put `text` on the clipboard, then show `lines`.
    Copy {
        text: String,
        lines: Vec<String>,
    },
    Action(SlashAction),
}

impl std::fmt::Debug for SlashOutcome {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Output(lines) => f.debug_tuple("Output").field(lines).finish(),
            Self::Prompt(text) => f.debug_tuple("Prompt").field(text).finish(),
            Self::Run { lines, .. } => f.debug_struct("Run").field("lines", lines).finish(),
            Self::Task(_) => f.write_str("Task"),
            Self::Refresh { lines, .. } => f.debug_struct("Refresh").field("lines", lines).finish(),
            Self::SwitchConversation { message, .. } => f
                .debug_struct("SwitchConversation")
                .field("message", message)
                .finish(),
            Self::Copy { text, lines } => f
                .debug_struct("Copy")
                .field("text", text)
                .field("lines", lines)
                .finish(),
            Self::Action(action) => f.debug_tuple("Action").field(action).finish(),
        }
    }
}

/// Archived `kUnavailableDuringActiveTurn`: `/compact`, `/context`,
/// `/reload` and mutating `/llama` subcommands wait for the turn to end.
pub fn is_blocked_while_running(name: &str, arguments: &str) -> bool {
    match name {
        "compact" | "context" | "reload" => true,
        "llama" => {
            let sub = arguments.split_whitespace().next().unwrap_or("status");
            !matches!(sub.to_ascii_lowercase().as_str(), "status" | "models")
        }
        _ => false,
    }
}

fn non_empty(text: &str) -> Option<String> {
    let t = text.trim();
    (!t.is_empty()).then(|| t.to_string())
}

/// Models the `/model` command can resolve: discovered models plus the
/// catalog records of every provider.
pub(crate) fn selectable_models(engine: &crate::Engine) -> Vec<ModelInfo> {
    let mut models = engine.known_models();
    for provider in engine.providers() {
        for spec in provider.models.iter() {
            if !models
                .iter()
                .any(|m| m.provider == provider.id && m.id.eq_ignore_ascii_case(&spec.id))
            {
                models.push(ModelInfo::from_spec(&provider.id, spec));
            }
        }
    }
    models
}

fn resolve_existing(path: &str, conversation: &Conversation) -> PathBuf {
    crate::export::resolve_export_path(path, conversation.engine().cwd())
        .unwrap_or_else(|| PathBuf::from(path))
}

/// Run one slash command line. Errors carry the archived messages.
pub fn dispatch(conversation: &Conversation, input: &str) -> Result<SlashOutcome> {
    let engine = conversation.engine().clone();
    let invocation = parse_invocation(input).map_err(|e| anyhow!(e))?;
    let registry = engine.slash_commands();
    let Some(command) = registry.find(&invocation.name).cloned() else {
        bail!("unknown slash command '/{}'", invocation.name);
    };
    let args = invocation.arguments.as_str();
    if command.source != SlashCommandSource::Extension
        && conversation.is_running()
        && is_blocked_while_running(&command.name, args)
    {
        bail!("slash commands are unavailable while a turn is running");
    }
    match command.source {
        SlashCommandSource::Extension => {
            return Ok(SlashOutcome::Action(SlashAction::Extension {
                target: command
                    .target_name
                    .clone()
                    .unwrap_or_else(|| command.name.clone()),
                name: command.name,
                arguments: args.to_string(),
                source_path: command.source_path,
            }));
        }
        SlashCommandSource::Prompt | SlashCommandSource::Skill => {
            let expanded = registry
                .expand(&command, args)
                .ok_or_else(|| anyhow!("unknown slash command '/{}'", command.name))?;
            return Ok(SlashOutcome::Prompt(expanded));
        }
        SlashCommandSource::Builtin => {}
    }
    let output = |lines: Vec<String>| Ok(SlashOutcome::Output(lines));
    let action = |a: SlashAction| Ok(SlashOutcome::Action(a));
    match command.name.as_str() {
        "help" => output(
            info::render_help(&registry, args)
                .lines()
                .map(str::to_string)
                .collect(),
        ),
        "settings" => {
            let (target, filter) = match args.trim().split_once(char::is_whitespace) {
                Some((t, f)) => (t.to_string(), non_empty(f)),
                None => (args.trim().to_string(), None),
            };
            if matches!(
                target.as_str(),
                "extensions" | "skills" | "prompts" | "themes" | "resources" | "packages"
            ) {
                return action(SlashAction::OpenPackageResources {
                    kind: target,
                    filter,
                });
            }
            action(SlashAction::OpenSettings {
                target: non_empty(&target),
            })
        }
        "extensions" | "skills" | "prompts" | "themes" => {
            action(SlashAction::OpenPackageResources {
                kind: command.name.clone(),
                filter: non_empty(args),
            })
        }
        "model" => model_command(conversation, args),
        "scoped-models" => action(SlashAction::SelectScopedModels {
            enabled: engine.settings().enabled_models(),
        }),
        "thinking" => thinking_command(conversation, args),
        "llama" | "ollama" | "subagent" | "index" | "memory" | "mcp" => {
            action(SlashAction::Subsystem {
                command: command.name.clone(),
                arguments: args.trim().to_string(),
            })
        }
        "export" => export_command(conversation, args),
        "import" => {
            let Some(path) = crate::export::resolve_export_path(args, engine.cwd()) else {
                bail!("Usage: /import <path.jsonl>");
            };
            let shown = args.trim().trim_matches(['"', '\'']).to_string();
            if !path.exists() {
                bail!("Session file not found: {}", path.display());
            }
            if let Some(issue) = engine.session_cwd_issue(&path) {
                return action(SlashAction::ImportMissingCwd {
                    path,
                    session_cwd: issue.session_cwd.clone(),
                    fallback_cwd: issue.fallback_cwd.clone(),
                });
            }
            let imported = engine.import_session(&path, None)?;
            Ok(SlashOutcome::SwitchConversation {
                conversation: imported,
                message: format!("Imported session from {shown}."),
            })
        }
        "share" => {
            if !conversation.is_persisted() {
                bail!("Cannot share an in-memory session");
            }
            let html = conversation.export_html_string();
            let job = engine.spawn(async move {
                let result =
                    crate::export::share_html_gist(std::path::Path::new("gh"), &html).await?;
                Ok(result.status_text.lines().map(str::to_string).collect())
            });
            Ok(SlashOutcome::Task(job))
        }
        "copy" => match conversation.last_assistant_text() {
            None => output(vec!["No assistant text is available yet.".into()]),
            Some(text) => Ok(SlashOutcome::Copy {
                text,
                lines: vec!["Copied the last assistant message to the clipboard.".into()],
            }),
        },
        "name" => {
            let name = args.trim();
            if name.is_empty() {
                return output(vec![format!(
                    "Session name: {}",
                    conversation.name().unwrap_or_else(|| "(unnamed)".into())
                )]);
            }
            conversation.set_name(name)?;
            output(vec![format!("Session name set to {name}")])
        }
        "session" => output(info::session_lines(conversation)),
        "status" => output(info::status_lines(conversation)),
        "context" => output(info::context_lines(conversation, args)),
        "changelog" => output(info::changelog_lines(engine.changelog())),
        "new" => Ok(SlashOutcome::SwitchConversation {
            conversation: {
                let next = engine.new_conversation(conversation.model());
                next.set_thinking(conversation.thinking());
                next
            },
            message: "Started a new session.".into(),
        }),
        "clone" => {
            if conversation.entries().is_empty() {
                bail!("Nothing to clone: the session has no entries");
            }
            Ok(SlashOutcome::SwitchConversation {
                conversation: conversation.clone_session()?,
                message: "Cloned to new session.".into(),
            })
        }
        "fork" => {
            if !conversation.is_persisted() {
                bail!("Cannot fork session: no active session file");
            }
            let messages = conversation.user_messages_for_forking();
            if messages.is_empty() {
                return output(vec!["No messages to fork from.".into()]);
            }
            action(SlashAction::ForkFromMessage { messages })
        }
        "tree" => action(SlashAction::OpenTree {
            hint: non_empty(args),
        }),
        "resume" => action(SlashAction::ResumeSession),
        "login" => action(SlashAction::Login {
            provider: non_empty(args),
        }),
        "logout" => action(SlashAction::Logout {
            provider: non_empty(args),
        }),
        "tools" => {
            let filter = non_empty(args);
            let active = conversation.active_tools();
            let tools: Vec<(String, String, bool)> = conversation
                .tool_set()
                .iter()
                .filter(|t| filter.as_deref().is_none_or(|f| t.name().contains(f)))
                .map(|t| {
                    (
                        t.name().to_string(),
                        t.description(),
                        active.iter().any(|a| a == t.name()),
                    )
                })
                .collect();
            if tools.is_empty() {
                return output(vec!["No tools matched.".into()]);
            }
            action(SlashAction::SelectTools { filter, tools })
        }
        "reload" => {
            let diagnostics = engine.reload_resources();
            let mut lines = vec!["Runtime resources reloaded.".to_string()];
            lines.extend(diagnostics);
            output(lines)
        }
        "quit" => action(SlashAction::Quit),
        "goal" => {
            let (out, events) = conversation.goal_command(args)?;
            match events {
                Some(events) => Ok(SlashOutcome::Run {
                    lines: out.lines,
                    events,
                }),
                None => output(out.lines),
            }
        }
        "follow-up" => {
            let text = args.trim();
            if text.is_empty() {
                bail!("follow-up requires message text");
            }
            if !conversation.is_running() {
                bail!("follow-up requires a running turn");
            }
            conversation.follow_up(text, Vec::new())?;
            let count = conversation.queue().follow_up.len();
            output(vec![format!("[follow-up #{count}] {text}")])
        }
        "compact" => {
            let events = conversation.compact(non_empty(args).as_deref())?;
            Ok(SlashOutcome::Run {
                lines: Vec::new(),
                events,
            })
        }
        other => bail!("builtin slash command '/{other}' has no deterministic app-layer execution"),
    }
}

fn model_command(conversation: &Conversation, args: &str) -> Result<SlashOutcome> {
    let engine = conversation.engine();
    let search = args.trim();
    if search.eq_ignore_ascii_case("refresh") {
        return Ok(match engine.refresh_models() {
            Ok(listing) => SlashOutcome::Refresh {
                lines: vec!["Refreshing OpenAI and OpenRouter model catalogs…".into()],
                listing,
            },
            Err(e) => SlashOutcome::Output(vec![e.to_string()]),
        });
    }
    if !search.is_empty() {
        let models = selectable_models(engine);
        if let Some(model) = crate::model_scope::find_exact_model(search, &models) {
            let target = ModelRef::new(model.provider.clone(), model.id.clone());
            if !engine
                .provider(&target.provider)
                .is_some_and(|p| p.is_ready())
            {
                bail!(
                    "model is not available; run /login or configure credentials: {}/{}",
                    target.provider,
                    target.id
                );
            }
            conversation.set_model(target.clone());
            engine.set_default_model(&target)?;
            return Ok(SlashOutcome::Output(vec![format!(
                "Model set to {}/{}",
                target.provider, target.id
            )]));
        }
    }
    Ok(SlashOutcome::Action(SlashAction::SelectModel {
        search: non_empty(search),
    }))
}

fn thinking_command(conversation: &Conversation, args: &str) -> Result<SlashOutcome> {
    let engine = conversation.engine();
    let levels = conversation
        .model()
        .and_then(|m| {
            engine
                .model_info(&m)
                .map(|i| i.thinking_levels)
                .or_else(|| {
                    engine
                        .provider(&m.provider)
                        .map(|p| p.model_spec(&m.id).thinking_levels())
                })
        })
        .filter(|l| !l.is_empty())
        .unwrap_or_else(|| ThinkingLevel::ALL.to_vec());
    let requested = args.trim();
    if requested.is_empty() {
        return Ok(SlashOutcome::Action(SlashAction::SelectThinking {
            current: conversation.thinking(),
            levels,
        }));
    }
    let Some(level) = ThinkingLevel::parse(requested) else {
        bail!("unsupported thinking level: {requested}");
    };
    let level = conversation
        .model()
        .and_then(|m| engine.provider(&m.provider).map(|p| p.model_spec(&m.id)))
        .map(|spec| {
            if level == ThinkingLevel::Off {
                level
            } else {
                spec.clamp_thinking(level)
            }
        })
        .unwrap_or(level);
    conversation.set_thinking(level);
    Ok(SlashOutcome::Output(vec![format!(
        "Thinking level set to {}",
        conversation.thinking().as_str()
    )]))
}

fn export_command(conversation: &Conversation, args: &str) -> Result<SlashOutcome> {
    if !conversation.is_persisted() {
        bail!("Cannot export an in-memory session");
    }
    let path = args.trim();
    let lower = path.to_ascii_lowercase();
    if path.is_empty() || lower.ends_with(".html") {
        let target = if path.is_empty() {
            None
        } else {
            Some(resolve_existing(path, conversation))
        };
        let written = conversation.export_html(target.as_deref())?;
        return Ok(SlashOutcome::Output(vec![format!(
            "Exported HTML to {}",
            written.display()
        )]));
    }
    if lower.ends_with(".jsonl") {
        let target = resolve_existing(path, conversation);
        conversation
            .export_jsonl(&target)
            .map_err(|e| anyhow!("failed to export session JSONL: {e:#}"))?;
        return Ok(SlashOutcome::Output(vec![format!(
            "Exported JSONL to {path}"
        )]));
    }
    bail!("unsupported export path: {path}")
}
