//! Text output of `/help`, `/session`, `/status`, `/context` and
//! `/changelog` (archived interactive handlers).

use std::collections::BTreeSet;
use std::path::Path;

use serde_json::json;

use super::SlashCommandRegistry;
use crate::engine::Conversation;
use crate::message::{AgentMessage, ContentBlock, Message};

/// `/context` sections in the archived order.
pub const CONTEXT_SECTIONS: &[&str] = &[
    "environment",
    "prompt",
    "messages",
    "resources",
    "diagnostics",
    "memory",
    "usage",
];

/// Archived `render_slash_command_help`.
pub fn render_help(registry: &SlashCommandRegistry, arguments: &str) -> String {
    let prefix = arguments.trim();
    let commands: Vec<_> = if prefix.is_empty() {
        registry.commands.iter().collect()
    } else {
        registry.filter_by_prefix(prefix)
    };
    let mut output = String::from("Available slash commands");
    if !prefix.is_empty() {
        output.push_str(&format!(
            " matching '/{}'",
            prefix.strip_prefix('/').unwrap_or(prefix)
        ));
    }
    output.push(':');
    for command in &commands {
        output.push_str("\n/");
        output.push_str(&command.name);
        if let Some(hint) = &command.argument_hint {
            output.push(' ');
            output.push_str(hint);
        }
        output.push_str(&format!(" [{}]", command.source.as_str()));
        if !command.description.is_empty() {
            output.push_str(" - ");
            output.push_str(&command.description);
        }
    }
    if commands.is_empty() {
        output.push_str("\n(no matching slash commands)");
    }
    output
}

fn model_text(conversation: &Conversation) -> String {
    conversation
        .model()
        .map(|m| format!("{}/{}", m.provider, m.id))
        .unwrap_or_else(|| "/".into())
}

fn session_file_text(conversation: &Conversation) -> String {
    if conversation.is_persisted() {
        conversation.session_path().display().to_string()
    } else {
        "in-memory".into()
    }
}

fn session_id_text(conversation: &Conversation) -> String {
    if conversation.is_persisted() {
        conversation.session_id()
    } else {
        "in-memory".into()
    }
}

/// `/session`: name, file, id, cwd, model, thinking, counts, tokens, cost
/// and context usage.
pub fn session_lines(conversation: &Conversation) -> Vec<String> {
    let stats = conversation.stats();
    let bash = conversation
        .agent_messages()
        .iter()
        .filter(|m| matches!(m, AgentMessage::BashExecution(_)))
        .count();
    let unknown_usage = conversation.agent_messages().iter().any(|m| {
        m.as_assistant().is_some_and(|a| {
            let u = &a.usage;
            u.input_tokens + u.output_tokens + u.cache_read_tokens + u.cache_write_tokens == 0
                && u.total_tokens == 0
        })
    });
    let mut lines = Vec::new();
    if let Some(name) = conversation.name().filter(|n| !n.is_empty()) {
        lines.push(format!("name: {name}"));
    }
    lines.push(format!("file: {}", session_file_text(conversation)));
    lines.push(format!("id: {}", session_id_text(conversation)));
    lines.push(format!("cwd: {}", conversation.engine().cwd().display()));
    lines.push(format!("model: {}", model_text(conversation)));
    lines.push(format!("thinking: {}", conversation.thinking().as_str()));
    lines.push(format!(
        "messages: user={}, assistant={}, tool_calls={}, tool_results={}, bash={bash}",
        stats.user_messages, stats.assistant_messages, stats.tool_calls, stats.tool_results
    ));
    let t = &stats.tokens;
    lines.push(format!(
        "tokens: input={}, output={}, total={}",
        t.input_tokens,
        t.output_tokens,
        t.input_tokens + t.output_tokens + t.cache_read_tokens + t.cache_write_tokens
    ));
    if stats.cost > 0.0 {
        lines.push(format!("cost: ${:.4}", stats.cost));
    }
    if unknown_usage {
        lines.push("usage: partial (some assistant usage is unknown)".into());
    }
    if let Some(usage) = stats.context_usage
        && let Some(tokens) = usage.tokens
    {
        let mut line = format!("context: {tokens}/{}", usage.context_window);
        if let Some(percent) = usage.percent {
            line.push_str(&format!(" ({percent:.1}%)"));
        }
        lines.push(line);
    }
    lines
}

/// `/status`: cwd, session, model, thinking, credential source and counts.
pub fn status_lines(conversation: &Conversation) -> Vec<String> {
    let engine = conversation.engine();
    let resources = engine.resources();
    let auth = conversation
        .model()
        .map(|m| engine.auth_status_label(&m.provider))
        .unwrap_or_else(|| "unconfigured".into());
    vec![
        format!("cwd: {}", engine.cwd().display()),
        format!("session: {}", session_id_text(conversation)),
        format!("model: {}", model_text(conversation)),
        format!("thinking: {}", conversation.thinking().as_str()),
        format!("auth: {auth}"),
        format!(
            "tools: {}, prompts: {}, skills: {}, extensions: {}",
            engine.tools().len(),
            resources.prompt_templates.templates.len(),
            resources.skills.skills.len(),
            engine.extensions_info().len()
        ),
    ]
}

/// Archived `/changelog`: the first `## ` section of the changelog.
pub fn changelog_lines(markdown: Option<&str>) -> Vec<String> {
    let mut lines = Vec::new();
    if let Some(text) = markdown {
        let mut in_latest = false;
        for line in text.lines() {
            let line = line.strip_suffix('\r').unwrap_or(line);
            if line.starts_with("## ") {
                if in_latest {
                    break;
                }
                in_latest = true;
            }
            if in_latest {
                lines.push(line.to_string());
            }
        }
    }
    if lines.is_empty() {
        lines.push("No changelog entries are available.".into());
    }
    lines
}

fn append_content(lines: &mut Vec<String>, heading: String, content: &str) {
    lines.push(heading);
    if content.is_empty() {
        lines.push("(empty)".into());
        return;
    }
    lines.extend(content.split('\n').map(str::to_string));
}

fn append_diagnostics(lines: &mut Vec<String>, heading: &str, values: &[String]) {
    lines.push(heading.to_string());
    if values.is_empty() {
        lines.push("- none".into());
    } else {
        lines.extend(values.iter().map(|v| format!("- {v}")));
    }
}

fn message_text_bytes(message: &AgentMessage) -> usize {
    match message {
        AgentMessage::Llm(Message::User { content, .. }) => content
            .blocks()
            .iter()
            .map(|b| match b {
                ContentBlock::Text { text } => text.len(),
                _ => 0,
            })
            .sum(),
        AgentMessage::Llm(Message::Assistant(a)) => a
            .content
            .iter()
            .map(|b| match b {
                ContentBlock::Text { text } => text.len(),
                ContentBlock::ToolCall { arguments, .. } => arguments.to_string().len(),
                _ => 0,
            })
            .sum(),
        AgentMessage::Llm(Message::ToolResult(r)) => {
            json!({"content": r.content, "details": r.details})
                .to_string()
                .len()
        }
        other => other.user_text().map(|t| t.len()).unwrap_or(0),
    }
}

fn pretty(value: &impl serde::Serialize) -> String {
    serde_json::to_string_pretty(value).unwrap_or_default()
}

fn context_file_labels(path: &Path, conversation: &Conversation) -> (&'static str, &'static str) {
    let engine = conversation.engine();
    let scope = if path == engine.home().global_agents_file() {
        "global"
    } else if path.parent() == Some(engine.cwd()) {
        "current"
    } else {
        "ancestor"
    };
    let name = path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or_default()
        .to_ascii_uppercase();
    let kind = if name.starts_with("CLAUDE") {
        "CLAUDE.md"
    } else {
        "AGENTS.md"
    };
    (scope, kind)
}

/// `/context [sections]`: the archived inspection of the assembled system
/// prompt, session context, resources, diagnostics, memory and usage.
pub fn context_lines(conversation: &Conversation, arguments: &str) -> Vec<String> {
    let mut sections: BTreeSet<&str> = BTreeSet::new();
    let normalized = arguments.to_ascii_lowercase().replace(',', " ");
    for raw in normalized.split_whitespace() {
        let section = match raw {
            "all" => {
                sections.extend(CONTEXT_SECTIONS.iter().copied());
                break;
            }
            "context" | "system" => "prompt",
            "session" | "history" => "messages",
            "resource" => "resources",
            "diagnostic" => "diagnostics",
            other => match CONTEXT_SECTIONS.iter().find(|s| **s == other) {
                Some(s) => s,
                None => {
                    return vec![
                        format!("Unknown /context section: {other}"),
                        "Use /context [all|environment|prompt|messages|resources|diagnostics|memory|usage]."
                            .into(),
                    ];
                }
            },
        };
        sections.insert(section);
    }
    if sections.is_empty() {
        sections.extend(CONTEXT_SECTIONS.iter().copied());
    }
    let engine = conversation.engine();
    let resources = engine.resources();
    let policy = engine.workspace_policy();
    let settings = engine.settings();
    let tools = conversation.tool_set();
    let active = conversation.active_tools();
    let system_prompt = conversation.system_prompt();
    let files = engine.context_files();
    let context_bytes: usize = files.iter().map(|f| f.content.len()).sum();
    let session_messages = conversation.session_context_messages();
    let transcript_bytes: usize = session_messages.iter().map(message_text_bytes).sum();

    let mut lines = vec![
        "Context inspection (default is all available content).".to_string(),
        "The next provider request is assembled at dispatch; this shows the current assembled system prompt and resolved session context, not unavailable future-turn state.".to_string(),
    ];

    if sections.contains("environment") {
        let runtime = crate::prompt::RuntimeMetadata::detect(
            engine.cwd(),
            Some(policy.root()),
            settings.shell_path().as_deref(),
            engine.is_offline(),
            crate::prompt::ToolPolicy {
                queue_mutations: engine.mutation_policy() == crate::tools::MutationPolicy::Queue,
                allow_external_filesystem_access: policy.allows_external_access(),
            },
            tools.len(),
        );
        lines.push("environment:".into());
        lines.push(format!("- cwd: {}", engine.cwd().display()));
        lines.push(format!(
            "- repository_root: {}",
            resources
                .repository_root
                .as_ref()
                .map(|p| p.display().to_string())
                .unwrap_or_else(|| "(none)".into())
        ));
        lines.push(format!("- workspace_root: {}", policy.root().display()));
        let mut field = |name: &str, value: &Option<String>| {
            if let Some(v) = value {
                lines.push(format!("- {name}: {v}"));
            }
        };
        field("shell", &runtime.shell);
        field("current_date", &runtime.current_date);
        field("timezone", &runtime.timezone);
        field("network", &runtime.network_access);
        field("tool_permissions", &runtime.tool_permissions);
        if let Some(count) = runtime.active_tool_count {
            lines.push(format!("- active_tools: {count}"));
        }
    }

    if sections.contains("prompt") {
        append_content(
            &mut lines,
            format!("assembled_system_prompt ({} bytes):", system_prompt.len()),
            &system_prompt,
        );
        lines.push(format!(
            "prompt_sections: {}, bytes={context_bytes}",
            files.len()
        ));
        for file in &files {
            let (scope, kind) = context_file_labels(&file.path, conversation);
            append_content(
                &mut lines,
                format!(
                    "prompt_section [{scope}] {kind} {} ({} bytes):",
                    file.path.display(),
                    file.content.len()
                ),
                &file.content,
            );
        }
        if files.is_empty() {
            lines.push("- no project context sections loaded".into());
        }
        if let Some(c) = &resources.contributions.system_prompt {
            append_content(
                &mut lines,
                format!("system_prompt_override {}:", c.source),
                &c.content,
            );
        }
        if let Some(c) = &resources.contributions.append_system_prompt {
            append_content(
                &mut lines,
                format!("append_system_prompt {}:", c.source),
                &c.content,
            );
        }
    }

    if sections.contains("messages") {
        lines.push(format!(
            "resolved_session_messages: {}, text_bytes={transcript_bytes}",
            session_messages.len()
        ));
        let (session_model, session_thinking) = conversation.session_context_settings();
        if let Some(model) = session_model {
            lines.push(format!("session_model: {}/{}", model.provider, model.id));
        }
        lines.push(format!("session_thinking: {session_thinking}"));
        for (index, message) in session_messages.iter().enumerate() {
            append_content(&mut lines, format!("message[{index}]:"), &pretty(message));
        }
        let active_messages = conversation.agent_messages();
        lines.push(format!(
            "active_agent_transcript: {} messages (before per-request context hooks)",
            active_messages.len()
        ));
        for (index, message) in active_messages.iter().enumerate() {
            append_content(
                &mut lines,
                format!("active_message[{index}]:"),
                &pretty(message),
            );
        }
        let queued = conversation.queued_messages();
        lines.push(format!("active_pending_messages: {}", queued.len()));
        for (sequence, message) in queued.iter().enumerate() {
            append_content(
                &mut lines,
                format!("pending_message sequence={sequence}:"),
                &pretty(message),
            );
        }
        match engine.last_provider_request() {
            Some(request) => append_content(
                &mut lines,
                "last_provider_payload (captured completed request; not a prediction of the next request):".into(),
                &pretty(&request),
            ),
            None => lines.push(
                "last_provider_payload: unavailable (no completed provider request in this runtime)"
                    .into(),
            ),
        }
    }

    if sections.contains("resources") {
        let extensions = engine.extensions_info();
        lines.push(format!(
            "resources: skills={}, prompts={}, extensions={}, tools={}",
            resources.skills.skills.len(),
            resources.prompt_templates.templates.len(),
            extensions.len(),
            tools.len()
        ));
        for skill in &resources.skills.skills {
            append_content(
                &mut lines,
                format!(
                    "skill {} {}{}",
                    skill.name,
                    skill.path.display(),
                    if skill.disable_model_invocation {
                        " (hidden from model):"
                    } else {
                        ":"
                    }
                ),
                &skill.content,
            );
        }
        for template in &resources.prompt_templates.templates {
            append_content(
                &mut lines,
                format!("prompt /{} {}:", template.name, template.path.display()),
                &template.content,
            );
        }
        for extension in &extensions {
            append_content(
                &mut lines,
                format!(
                    "extension manifest {} {}:",
                    extension.name,
                    extension.path.display()
                ),
                &pretty(&extension.manifest),
            );
        }
        if !extensions.is_empty() {
            lines.push("extension implementation source is not retained in the resource bundle; manifest paths above identify the loaded sources.".into());
        }
        for tool in tools.iter() {
            let state = if active.iter().any(|a| a == tool.name()) {
                " (active):"
            } else {
                " (disabled):"
            };
            let spec = tool.spec();
            append_content(
                &mut lines,
                format!("tool {}{state}", tool.name()),
                &pretty(&json!({
                    "name": spec.name,
                    "description": spec.description,
                    "parameters": spec.parameters,
                })),
            );
        }
    }

    if sections.contains("diagnostics") {
        let mut runtime = engine.settings_diagnostics();
        runtime.extend(conversation.session_diagnostics());
        append_diagnostics(&mut lines, "runtime_diagnostics:", &runtime);
        append_diagnostics(
            &mut lines,
            "resource_diagnostics:",
            &resources.diagnostic_lines(),
        );
        append_diagnostics(&mut lines, "context_diagnostics:", &[]);
        append_diagnostics(&mut lines, "prompt_assembly_diagnostics:", &[]);
        append_diagnostics(
            &mut lines,
            "system_prompt_diagnostics:",
            &resources.contributions.diagnostics,
        );
        lines.push(format!("instruction_load_events: {}", files.len()));
        for file in &files {
            let (scope, kind) = context_file_labels(&file.path, conversation);
            lines.push(format!(
                "- [{scope}] {kind} {} reason=session_start bytes={}",
                file.path.display(),
                file.content.len()
            ));
        }
        lines.push("instruction_load_hooks_planned: 0".into());
    }

    if sections.contains("memory") {
        lines.push("memory: latest prepared transient augmentation only; it does not prove provider consumption and is not persisted session context.".into());
        match engine.memory_context_lines() {
            Some(memory) => lines.extend(memory),
            None => lines.push("- memory runtime is unavailable".into()),
        }
    }

    if sections.contains("usage") {
        let stats = conversation.stats();
        let t = &stats.tokens;
        lines.push(format!(
            "model_usage: input={}, output={}, total={}",
            t.input_tokens,
            t.output_tokens,
            t.input_tokens + t.output_tokens + t.cache_read_tokens + t.cache_write_tokens
        ));
        match stats.context_usage {
            Some(usage) => lines.push(format!(
                "model_context_usage: tokens={}, context_window={}{}",
                usage
                    .tokens
                    .map(|t| t.to_string())
                    .unwrap_or_else(|| "unknown".into()),
                usage.context_window,
                usage
                    .percent
                    .map(|p| format!(", percent={p:.6}"))
                    .unwrap_or_else(|| ", percent=unknown".into())
            )),
            None => lines
                .push("model_context_usage: unavailable (no model context-window estimate)".into()),
        }
    }
    lines
}
