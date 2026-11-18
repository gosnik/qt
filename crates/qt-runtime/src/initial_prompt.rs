//! The first prompt of an interactive session, from the command line
//! (archived `prepare_initial_message`): piped stdin, then the `@file`
//! blocks, then the first message form the initial prompt; the other
//! messages follow one by one (Pi's interactive `initialMessages`).

use std::path::Path;

use anyhow::Result;
use async_channel::Receiver;

use qt_core::agent::AgentEvent;
use qt_core::engine::{Conversation, Engine};
use qt_core::harness::hooks::{InputOutcome, InputSource};
use qt_core::input::{DiagnosticSeverity, FileDiagnostic};
use qt_core::message::ContentBlock;
use qt_core::slash::{SlashCommandSource, parse_invocation};

/// The prompt a window opens with.
#[derive(Debug, Clone, Default)]
pub struct InitialPrompt {
    /// stdin + `@file` text + first message.
    pub text: String,
    /// Images from `@file` arguments.
    pub images: Vec<ContentBlock>,
    /// Messages sent after the initial prompt, in order.
    pub follow_ups: Vec<String>,
}

impl InitialPrompt {
    pub fn is_empty(&self) -> bool {
        self.text.is_empty() && self.images.is_empty() && self.follow_ups.is_empty()
    }
}

/// Build the initial prompt. `Err` carries the first error diagnostic (a
/// missing or unreadable `@file`); warnings are returned alongside.
pub fn prepare_initial_prompt(
    messages: &[String],
    file_args: &[String],
    stdin: Option<&str>,
    cwd: &Path,
    auto_resize_images: bool,
) -> std::result::Result<(InitialPrompt, Vec<FileDiagnostic>), String> {
    let files = qt_core::input::process_file_arguments(file_args, cwd, auto_resize_images);
    if let Some(error) = files
        .diagnostics
        .iter()
        .find(|d| d.severity == DiagnosticSeverity::Error)
    {
        return Err(error.message.clone());
    }
    let has_initial = stdin.is_some() || !files.text.is_empty() || !messages.is_empty();
    let prompt = if has_initial {
        InitialPrompt {
            text: qt_core::input::build_initial_message(
                stdin,
                &files.text,
                messages.first().map(String::as_str),
            ),
            images: files.images,
            follow_ups: messages.iter().skip(1).cloned().collect(),
        }
    } else {
        InitialPrompt {
            images: files.images,
            ..Default::default()
        }
    };
    Ok((prompt, files.diagnostics))
}

/// Expand `/template args` and `/skill:name args` (Pi `_expandSkillCommand`
/// + `expandPromptTemplate`); other text is kept.
pub fn expand_prompt(engine: &Engine, text: &str) -> String {
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

/// Send `prompt` in `conversation` as typed input: the extension `input`
/// hooks run first (source `interactive`), then template / skill expansion;
/// the follow-up messages are queued behind it. `Ok(None)` when an input
/// hook handled the prompt.
pub fn submit_initial_prompt(
    conversation: &Conversation,
    prompt: InitialPrompt,
) -> Result<Option<Receiver<AgentEvent>>> {
    let engine = conversation.engine();
    let mut rx = None;
    if (!prompt.text.is_empty() || !prompt.images.is_empty())
        && let InputOutcome::Continue { text, images } =
            engine.process_input(&prompt.text, prompt.images, InputSource::Interactive)?
    {
        let text = expand_prompt(engine, &text);
        rx = Some(conversation.prompt(&text, images)?);
    }
    for message in prompt.follow_ups {
        let text = match engine.process_input(&message, Vec::new(), InputSource::Interactive)? {
            InputOutcome::Continue { text, .. } => expand_prompt(engine, &text),
            InputOutcome::Handled => continue,
        };
        if rx.is_none() {
            rx = Some(conversation.prompt(&text, Vec::new())?);
        } else {
            conversation.follow_up(&text, Vec::new())?;
        }
    }
    Ok(rx)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn combines_stdin_files_and_first_message() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("a.txt"), "alpha").unwrap();
        let (p, diags) = prepare_initial_prompt(
            &s(&["first", "second"]),
            &s(&["a.txt"]),
            Some("piped\n"),
            dir.path(),
            false,
        )
        .unwrap();
        assert!(diags.is_empty());
        assert!(p.text.starts_with("piped\n<file name="));
        assert!(p.text.contains("alpha"));
        assert!(p.text.ends_with("first"));
        assert_eq!(p.follow_ups, ["second"]);

        let (p, _) = prepare_initial_prompt(&[], &[], None, dir.path(), false).unwrap();
        assert!(p.is_empty());

        let err =
            prepare_initial_prompt(&[], &s(&["missing.txt"]), None, dir.path(), false).unwrap_err();
        assert!(err.contains("missing.txt"), "{err}");
    }
}
