//! Slash handling shared by the print/JSON and RPC modes (Pi
//! `AgentSession.prompt`): extension commands run instead of prompting, and
//! `/template args` / `/skill:name args` expand before the prompt is sent.

use qt_core::engine::Engine;
use qt_core::slash::{SlashCommandSource, parse_invocation};

/// An extension command invocation.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ExtensionInvocation {
    /// The name as invoked.
    pub name: String,
    /// The extension's registered command name.
    pub target: String,
    pub arguments: String,
}

/// The extension command `text` invokes, if any.
pub fn extension_command(engine: &Engine, text: &str) -> Option<ExtensionInvocation> {
    if !text.starts_with('/') {
        return None;
    }
    let invocation = parse_invocation(text).ok()?;
    let registry = engine.slash_commands();
    let command = registry.find(&invocation.name)?;
    (command.source == SlashCommandSource::Extension).then(|| ExtensionInvocation {
        name: command.name.clone(),
        target: command
            .target_name
            .clone()
            .unwrap_or_else(|| command.name.clone()),
        arguments: invocation.arguments,
    })
}

/// Expand `/template args` and `/skill:name args` (Pi `_expandSkillCommand`
/// + `expandPromptTemplate`); other text is kept.
pub fn expand(engine: &Engine, text: &str) -> String {
    if !text.starts_with('/') {
        return text.to_string();
    }
    let Ok(invocation) = parse_invocation(text) else {
        return text.to_string();
    };
    let registry = engine.slash_commands();
    match registry.find(&invocation.name) {
        Some(command)
            if matches!(
                command.source,
                SlashCommandSource::Prompt | SlashCommandSource::Skill
            ) =>
        {
            registry
                .expand(command, &invocation.arguments)
                .unwrap_or_else(|| text.to_string())
        }
        _ => text.to_string(),
    }
}

/// The text an extension command produced, without the generic completion
/// placeholder.
pub fn command_output(target: &str, lines: Vec<String>) -> Vec<String> {
    let placeholder = format!("Extension command /{target} completed.");
    lines.into_iter().filter(|l| *l != placeholder).collect()
}
