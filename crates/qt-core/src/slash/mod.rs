//! Slash commands (archived `app/slash/slash_commands.cpp` and the
//! interactive handlers in `app/interactive/interactive_mode.cpp`).
//!
//! The registry lists extension commands first, then the built-ins, then
//! prompt templates and `/skill:<name>` commands; a later command whose name
//! is taken is skipped with the archived diagnostic. [`dispatch`] executes a
//! command line against a [`crate::Conversation`]: commands the core can
//! perform (compaction, names, sessions, goals, exports, settings changes)
//! run directly, and commands that need a picker or panel come back as a
//! [`SlashAction`] for the front end.

mod dispatch;
mod info;

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;

use crate::resources::ResourceSnapshot;

pub use dispatch::{SlashAction, SlashOutcome, dispatch, is_blocked_while_running};
pub use info::{
    CONTEXT_SECTIONS, changelog_lines, context_lines, render_help, session_lines, status_lines,
};

/// Where a slash command comes from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SlashCommandSource {
    Builtin,
    Extension,
    Prompt,
    Skill,
}

impl SlashCommandSource {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Builtin => "builtin",
            Self::Extension => "extension",
            Self::Prompt => "prompt",
            Self::Skill => "skill",
        }
    }
}

/// One registered slash command.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SlashCommand {
    /// Invocation name without `/` (`help`, `review`, `skill:pdf`, `deploy:2`).
    pub name: String,
    pub description: String,
    pub argument_hint: Option<String>,
    pub source: SlashCommandSource,
    /// File that contributed the command (template, skill, extension).
    pub source_path: Option<PathBuf>,
    /// For extension commands: the name the extension registered (the
    /// invocation name gets a `:<n>` suffix when several extensions use it).
    pub target_name: Option<String>,
}

impl SlashCommand {
    fn builtin(name: &str, description: &str, hint: Option<&str>) -> Self {
        Self {
            name: name.into(),
            description: description.into(),
            argument_hint: hint.map(str::to_string),
            source: SlashCommandSource::Builtin,
            source_path: None,
            target_name: None,
        }
    }

    /// An extension command as an extension host registers it.
    pub fn extension(
        name: impl Into<String>,
        description: impl Into<String>,
        source_path: Option<PathBuf>,
    ) -> Self {
        let name = name.into();
        Self {
            target_name: Some(name.clone()),
            name,
            description: description.into(),
            argument_hint: None,
            source: SlashCommandSource::Extension,
            source_path,
        }
    }
}

/// `/llama` subcommands (archived `llama_slash_subcommand_infos`).
pub const LLAMA_SUBCOMMANDS: &[(&str, &str, &str)] = &[
    (
        "status",
        "Show llama runtime, model, KV cache, GPU, grammar, and embedding status.",
        "",
    ),
    (
        "models",
        "List configured local llama models and mark the selected model.",
        "",
    ),
    (
        "select",
        "Select the local llama model used by llama commands.",
        "<model-id>",
    ),
    (
        "load",
        "Load the selected or requested llama model now.",
        "[model-id]",
    ),
    ("unload", "Release llama model/context resources.", ""),
    (
        "reload",
        "Reload llama settings, catalog entries, and runtime state.",
        "",
    ),
    (
        "policy",
        "Show or update llama generation policy overrides.",
        "[temperature=<n>] [topP=<n>]",
    ),
    (
        "grammar",
        "Show, disable, or set grammar-constrained generation.",
        "<show|off|inline|file> ...",
    ),
    (
        "kv",
        "Inspect or mutate the llama KV cache.",
        "<show|clear|keep|drop|shift> ...",
    ),
    (
        "gpu",
        "Show or update pending llama GPU/backend offload settings.",
        "[gpuLayers=<n>] [mainGpu=<n>] [splitMode=<mode>]",
    ),
    (
        "embeddings",
        "Generate a llama embedding for text when supported by the model.",
        "<text>",
    ),
    (
        "prompt",
        "Run text through the selected llama model.",
        "<text>",
    ),
    (
        "run",
        "Run text through a tagged llama instance.",
        "<embed|rank|instruct|chat> <text>",
    ),
];

/// The built-in commands in the archived order. `/hotkeys` and `/plan`
/// (terminal-only) are not offered; `/follow-up` was registered by the
/// archived interactive shell.
pub fn builtin_commands() -> Vec<SlashCommand> {
    let b = SlashCommand::builtin;
    vec![
        b("help", "Show slash command help", None),
        b("settings", "Open settings menu", None),
        b(
            "model",
            "Select model",
            Some("[provider/model|search|refresh]"),
        ),
        b(
            "llama",
            "Inspect and configure local llama.cpp provider",
            Some(
                "<status|models|select|load|unload|reload|policy|grammar|kv|gpu|embeddings|prompt|run>",
            ),
        ),
        b(
            "ollama",
            "Manage Ollama instances and models",
            Some(
                "<instances|add|configure|remove|status|models|running|info|pull|load|unload|delete>",
            ),
        ),
        b(
            "subagent",
            "Inspect and use isolated subagents",
            Some("<list|help>"),
        ),
        b(
            "index",
            "Inspect and use project index operations",
            Some("<tree|call|status|rescan|code|fs|git|build>"),
        ),
        b(
            "memory",
            "Inspect and use built-in governed memory",
            Some(
                "<dashboard|status|settings|scan-skills|dream|recall|remember|review|feedback|forget>",
            ),
        ),
        b(
            "mcp",
            "Inspect and manage MCP servers",
            Some("<status|tools|resources|reconnect|setup>"),
        ),
        b(
            "scoped-models",
            "Enable or disable models for cycling",
            None,
        ),
        b(
            "export",
            "Export session (HTML default, or specify path: .html/.jsonl)",
            None,
        ),
        b("import", "Import and resume a session", None),
        b("share", "Share session", None),
        b("copy", "Copy last agent message", None),
        b("name", "Set session display name", None),
        b("session", "Show session info and stats", None),
        b(
            "context",
            "Inspect full current prompt, session context, resources, diagnostics, memory, and usage",
            Some("[all|environment|prompt|messages|resources|diagnostics|memory|usage]"),
        ),
        b("changelog", "Show changelog entries", None),
        b(
            "fork",
            "Create a new fork from a previous user message",
            None,
        ),
        b("clone", "Duplicate the current session", None),
        b("tree", "Navigate session tree", None),
        b("login", "Configure provider authentication", None),
        b("logout", "Remove provider authentication", None),
        b("new", "Start a new session", None),
        b("compact", "Manually compact the session context", None),
        b("resume", "Resume a different session", None),
        b("status", "Show runtime status", None),
        b("tools", "Configure enabled tools", None),
        b("thinking", "Configure thinking level", None),
        b(
            "goal",
            "Set or manage session goal",
            Some("[<objective>|clear|edit|pause|resume|status]"),
        ),
        b(
            "follow-up",
            "Queue a follow-up message for the running turn",
            Some("<message>"),
        ),
        b(
            "extensions",
            "List loaded extensions and configure package resources",
            None,
        ),
        b("skills", "Configure skill package resources", None),
        b("prompts", "Configure prompt package resources", None),
        b("themes", "Configure theme package resources", None),
        b(
            "reload",
            "Reload keybindings, extensions, skills, prompts, and themes",
            None,
        ),
        b("quit", "Quit qt", None),
    ]
}

/// A parsed `/name args` line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SlashInvocation {
    pub name: String,
    /// Arguments with leading whitespace removed (trailing kept, as archived).
    pub arguments: String,
}

/// Parse `/name args` (archived `parse_slash_command_invocation` wording).
pub fn parse_invocation(input: &str) -> Result<SlashInvocation, String> {
    let text = input.trim_start();
    if text.is_empty() {
        return Err("slash command input is empty".into());
    }
    let Some(rest) = text.strip_prefix('/') else {
        return Err("slash command input must start with '/'".into());
    };
    let end = rest.find(char::is_whitespace).unwrap_or(rest.len());
    if end == 0 {
        return Err("slash command name is empty".into());
    }
    Ok(SlashInvocation {
        name: rest[..end].to_string(),
        arguments: rest[end..].trim_start().to_string(),
    })
}

/// Whether `text` looks like a slash command (`/name`, not a path such as
/// `/usr/bin` with further slashes in the first word).
pub fn looks_like_command(text: &str) -> bool {
    let Some(rest) = text.trim_start().strip_prefix('/') else {
        return false;
    };
    let word = rest.split(char::is_whitespace).next().unwrap_or("");
    !word.is_empty() && !word.contains('/')
}

/// A completion offered for a command name or its arguments.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompletionItem {
    /// Text inserted (after `/` for names, after `/name ` for arguments).
    pub value: String,
    pub label: String,
    pub description: Option<String>,
}

/// All slash commands with their conflict diagnostics.
#[derive(Debug, Clone, Default)]
pub struct SlashCommandRegistry {
    pub commands: Vec<SlashCommand>,
    pub diagnostics: Vec<String>,
    resources: Option<std::sync::Arc<ResourceSnapshot>>,
}

impl SlashCommandRegistry {
    /// Extensions, then built-ins, then prompt templates, then skills
    /// (when `enableSkillCommands`), with the archived conflict rules.
    pub fn build(
        extensions: &[SlashCommand],
        resources: &std::sync::Arc<ResourceSnapshot>,
    ) -> Self {
        let mut registry = Self {
            resources: Some(resources.clone()),
            ..Default::default()
        };
        let mut reserved: HashSet<String> = HashSet::new();
        let mut counts: HashMap<&str, usize> = HashMap::new();
        for command in extensions.iter().filter(|c| !c.name.is_empty()) {
            *counts.entry(command.name.as_str()).or_default() += 1;
        }
        let mut seen: HashMap<&str, usize> = HashMap::new();
        for command in extensions.iter().filter(|c| !c.name.is_empty()) {
            let mut invocation = command.name.clone();
            if counts[command.name.as_str()] > 1 {
                let n = seen.entry(command.name.as_str()).or_default();
                *n += 1;
                invocation = format!("{}:{n}", command.name);
            }
            if reserved.contains(&invocation) {
                registry.diagnostics.push(format!(
                    "extension command '/{invocation}' conflicts with an existing extension slash command and was skipped: {}",
                    command
                        .source_path
                        .as_ref()
                        .map(|p| p.display().to_string())
                        .unwrap_or_default()
                ));
                continue;
            }
            reserved.insert(invocation.clone());
            registry.commands.push(SlashCommand {
                name: invocation,
                target_name: Some(
                    command
                        .target_name
                        .clone()
                        .unwrap_or_else(|| command.name.clone()),
                ),
                source: SlashCommandSource::Extension,
                ..command.clone()
            });
        }
        for command in builtin_commands() {
            reserved.insert(command.name.clone());
            registry.commands.push(command);
        }
        let (resource_commands, diagnostics) = resources.commands(|name| reserved.contains(name));
        registry.diagnostics.extend(diagnostics);
        for command in resource_commands {
            registry.commands.push(SlashCommand {
                name: command.name,
                description: command.description,
                argument_hint: command.argument_hint,
                source: match command.source {
                    crate::resources::ResourceCommandSource::Prompt => SlashCommandSource::Prompt,
                    crate::resources::ResourceCommandSource::Skill => SlashCommandSource::Skill,
                },
                source_path: Some(command.source_path),
                target_name: None,
            });
        }
        registry
    }

    pub fn find(&self, name: &str) -> Option<&SlashCommand> {
        let name = name.strip_prefix('/').unwrap_or(name);
        self.commands.iter().find(|c| c.name == name)
    }

    /// Commands whose name starts with `prefix` (leading `/` ignored).
    pub fn filter_by_prefix(&self, prefix: &str) -> Vec<&SlashCommand> {
        let prefix = prefix.strip_prefix('/').unwrap_or(prefix);
        self.commands
            .iter()
            .filter(|c| c.name.starts_with(prefix))
            .collect()
    }

    /// Expand a prompt-template or skill command into the user message.
    pub fn expand(&self, command: &SlashCommand, arguments: &str) -> Option<String> {
        let resources = self.resources.as_ref()?;
        match command.source {
            SlashCommandSource::Prompt => resources.prompt_templates.find(&command.name).map(|t| {
                crate::resources::templates::expand_prompt_template(&t.content, arguments)
            }),
            SlashCommandSource::Skill => command
                .name
                .strip_prefix("skill:")
                .and_then(|name| resources.skills.find(name))
                .map(|s| crate::resources::skills::expand_skill_command(s, arguments)),
            _ => None,
        }
    }

    /// Name completions for a partial `/name` (without arguments).
    pub fn command_completions(&self, prefix: &str) -> Vec<CompletionItem> {
        self.filter_by_prefix(prefix)
            .into_iter()
            .map(|c| CompletionItem {
                value: c.name.clone(),
                label: match &c.argument_hint {
                    Some(hint) => format!("{} {hint}", c.name),
                    None => c.name.clone(),
                },
                description: (!c.description.is_empty()).then(|| c.description.clone()),
            })
            .collect()
    }
}

fn filtered(items: &[(&str, &str, &str)], prefix: &str) -> Vec<CompletionItem> {
    items
        .iter()
        .filter(|(value, _, _)| prefix.is_empty() || value.contains(prefix))
        .map(|(value, label, description)| CompletionItem {
            value: value.to_string(),
            label: label.to_string(),
            description: (!description.is_empty()).then(|| description.to_string()),
        })
        .collect()
}

fn split_first(text: &str) -> (&str, &str) {
    let text = text.trim_start();
    let end = text.find(char::is_whitespace).unwrap_or(text.len());
    (&text[..end], text[end..].trim())
}

/// Argument completions for `/command <prefix>` (archived autocomplete
/// providers for `/model`, `/thinking`, `/llama`, `/ollama`, `/memory`,
/// `/subagent`, `/mcp`; plus `/context` sections and `/goal` subcommands).
pub fn argument_completions(
    engine: &crate::Engine,
    command: &str,
    prefix: &str,
) -> Vec<CompletionItem> {
    match command {
        "model" => {
            let mut items = Vec::new();
            let ready: HashSet<String> = engine
                .providers()
                .into_iter()
                .filter(|p| p.is_ready())
                .map(|p| p.id)
                .collect();
            for model in dispatch::selectable_models(engine) {
                if !ready.contains(&model.provider) {
                    continue;
                }
                if !prefix.is_empty()
                    && !crate::model_scope::fuzzy_matches(
                        prefix,
                        &format!("{} {}", model.provider, model.id),
                    )
                {
                    continue;
                }
                items.push(CompletionItem {
                    value: format!("{}/{}", model.provider, model.id),
                    label: model.id.clone(),
                    description: Some(model.name.clone()).filter(|n| !n.is_empty()),
                });
            }
            items
        }
        "thinking" => crate::provider::ThinkingLevel::ALL
            .iter()
            .map(|l| l.as_str())
            .filter(|l| prefix.is_empty() || l.contains(prefix))
            .map(|l| CompletionItem {
                value: l.into(),
                label: l.into(),
                description: None,
            })
            .collect(),
        "llama" => {
            let (first, rest) = split_first(prefix);
            if first == "run" {
                return ["embed", "rank", "instruct", "chat"]
                    .iter()
                    .filter(|m| rest.is_empty() || m.contains(rest))
                    .map(|m| CompletionItem {
                        value: format!("run {m}"),
                        label: format!("run {m}"),
                        description: Some("llama run mode".into()),
                    })
                    .collect();
            }
            LLAMA_SUBCOMMANDS
                .iter()
                .filter(|(name, _, _)| prefix.is_empty() || name.contains(prefix))
                .map(|(name, description, hint)| CompletionItem {
                    value: name.to_string(),
                    label: if hint.is_empty() {
                        name.to_string()
                    } else {
                        format!("{name} {hint}")
                    },
                    description: Some(description.to_string()),
                })
                .collect()
        }
        "ollama" => {
            let (first, rest) = split_first(prefix);
            let has_rest = prefix.trim_start().len() > first.len();
            if !has_rest {
                [
                    "instances",
                    "add",
                    "configure",
                    "remove",
                    "status",
                    "models",
                    "running",
                    "info",
                    "pull",
                    "load",
                    "unload",
                    "delete",
                ]
                .iter()
                .filter(|v| first.is_empty() || v.contains(first))
                .map(|v| CompletionItem {
                    value: v.to_string(),
                    label: v.to_string(),
                    description: Some("Ollama operation".into()),
                })
                .collect()
            } else {
                engine
                    .settings()
                    .ollama_settings()
                    .into_iter()
                    .filter(|i| i.id.contains(rest))
                    .map(|i| CompletionItem {
                        value: format!("{first} {}", i.id),
                        label: format!("{first} {}", i.id),
                        description: Some(i.base_url),
                    })
                    .collect()
            }
        }
        "memory" => [
            "dashboard",
            "status",
            "settings",
            "scan-skills",
            "dream",
            "recall",
            "remember",
            "review",
            "feedback",
            "forget",
        ]
        .iter()
        .filter(|v| prefix.is_empty() || v.contains(prefix))
        .map(|v| CompletionItem {
            value: v.to_string(),
            label: v.to_string(),
            description: Some(if *v == "scan-skills" {
                "Scan a folder for skill memories".into()
            } else {
                "Memory operation".into()
            }),
        })
        .collect(),
        "subagent" => filtered(
            &[
                (
                    "list",
                    "list [user|project|both]",
                    "List discovered subagent definitions",
                ),
                ("help", "help", "Show role locations and execution guidance"),
            ],
            prefix,
        ),
        "mcp" => filtered(
            &[
                ("status", "status", "Show configured MCP server status"),
                ("tools", "tools", "List registered MCP tools"),
                ("resources", "resources", "List MCP resources"),
                (
                    "reconnect",
                    "reconnect [server]",
                    "Reconnect all servers or one server",
                ),
                ("setup", "setup", "Show MCP config paths and starter config"),
            ],
            prefix,
        ),
        "context" => {
            let mut sections = vec!["all"];
            sections.extend_from_slice(CONTEXT_SECTIONS);
            sections
                .into_iter()
                .filter(|v| prefix.is_empty() || v.contains(prefix))
                .map(|v| CompletionItem {
                    value: v.into(),
                    label: v.into(),
                    description: None,
                })
                .collect()
        }
        "goal" => ["clear", "edit", "pause", "resume", "status"]
            .iter()
            .filter(|v| prefix.is_empty() || v.starts_with(prefix))
            .map(|v| CompletionItem {
                value: v.to_string(),
                label: v.to_string(),
                description: None,
            })
            .collect(),
        _ => Vec::new(),
    }
}

#[cfg(test)]
mod tests;
