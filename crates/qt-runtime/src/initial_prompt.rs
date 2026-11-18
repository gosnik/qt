//! The first prompt of an interactive session, from the command line
//! (archived `prepare_initial_message`): piped stdin, then the `@file`
//! blocks, then the first message form the initial prompt; the other
//! messages follow one by one (Pi's interactive `initialMessages`). The
//! desktop app submits each line through its composer path (`!command`,
//! `/command`, prompt), like archived `submit_line(initial_message)`.

use std::path::Path;

use qt_core::input::{DiagnosticSeverity, FileDiagnostic};
use qt_core::message::ContentBlock;

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

    /// The lines to submit, in order: the initial prompt (with the `@file`
    /// images), then each follow-up message.
    pub fn into_lines(self) -> Vec<(String, Vec<ContentBlock>)> {
        let mut lines = Vec::with_capacity(self.follow_ups.len() + 1);
        if !self.text.is_empty() || !self.images.is_empty() {
            lines.push((self.text, self.images));
        }
        lines.extend(self.follow_ups.into_iter().map(|m| (m, Vec::new())));
        lines
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

    #[test]
    fn lines_keep_command_messages_verbatim_and_in_order() {
        let dir = tempfile::tempdir().unwrap();
        let (p, _) = prepare_initial_prompt(
            &s(&["/model ollama:qwen3.5:9b", "hello", "/goal ship"]),
            &[],
            None,
            dir.path(),
            false,
        )
        .unwrap();
        let lines: Vec<String> = p.into_lines().into_iter().map(|(t, _)| t).collect();
        assert_eq!(lines, ["/model ollama:qwen3.5:9b", "hello", "/goal ship"]);
        assert!(InitialPrompt::default().into_lines().is_empty());
    }
}
