//! Messages from the command line (`qt "…"`) go through the composer's
//! submission path, one by one and in order, like typed lines (archived
//! interactive mode: `active_session.submit_line(initial_message)`):
//! `!command` runs user bash, `/command` runs a slash command (built-in,
//! extension, template or skill), other text prompts. A plain message sent
//! while the agent runs is queued as a follow-up; a command or `!` line
//! waits until the run (or user bash) before it has finished, so the effects
//! land in the order given.

use gpui_kit::*;
use qt_core::ContentBlock;
use qt_core::slash::looks_like_command;

use super::completion::parse_bash;
use super::{RunKind, Workspace};

/// How a submitted line is handled.
#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) enum LineKind {
    /// `!command` (`!!command`: excluded from the context).
    Bash { command: String, exclude: bool },
    /// `/command args`.
    Slash,
    /// Anything else (or any line with attachments): a prompt.
    Prompt,
}

/// Classify a composer line; attachments always make it a prompt.
pub(super) fn classify_line(raw: &str, has_attachments: bool) -> LineKind {
    if has_attachments {
        return LineKind::Prompt;
    }
    if let Some((command, exclude)) = parse_bash(raw) {
        return LineKind::Bash { command, exclude };
    }
    if looks_like_command(raw) {
        return LineKind::Slash;
    }
    LineKind::Prompt
}

/// What to do with the next startup line given the view's state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum StartupStep {
    /// Submit it now (idle).
    Submit,
    /// Queue it behind the running agent turn.
    FollowUp,
    /// Keep it until the current run or user bash finishes.
    Wait,
}

pub(super) fn startup_step(
    kind: &LineKind,
    running: Option<RunKind>,
    bash_busy: bool,
) -> StartupStep {
    match (kind, running) {
        (LineKind::Prompt, Some(RunKind::Agent)) => StartupStep::FollowUp,
        _ if running.is_some() || bash_busy => StartupStep::Wait,
        _ => StartupStep::Submit,
    }
}

/// A startup line: text plus the images of `@file` arguments (first line only).
pub type StartupLine = (String, Vec<ContentBlock>);

impl Workspace {
    /// Submit the command line's messages as if typed in the composer.
    pub fn submit_startup_lines(
        &mut self,
        lines: Vec<StartupLine>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.startup_lines.extend(lines);
        self.drain_startup_lines(window, cx);
    }

    /// Submit startup lines until one has to wait for a run or user bash.
    pub(super) fn drain_startup_lines(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        while let Some((text, images)) = self.startup_lines.front() {
            let kind = classify_line(text, !images.is_empty());
            let step = startup_step(&kind, self.running, self.bash.is_some());
            if step == StartupStep::Wait {
                break;
            }
            let Some((text, images)) = self.startup_lines.pop_front() else {
                break;
            };
            if text.trim().is_empty() && images.is_empty() {
                continue;
            }
            self.history_record(&text, window, cx);
            match kind {
                LineKind::Bash { command, exclude } => {
                    self.run_user_bash(&command, exclude, window, cx);
                }
                LineKind::Slash => self.run_slash(&text, window, cx),
                LineKind::Prompt => {
                    // Failures are reported as notifications; later lines
                    // still go out (Pi shows the error and continues).
                    self.send_text(&text, images, step == StartupStep::FollowUp, window, cx);
                }
            }
        }
        cx.notify();
    }
}

#[cfg(test)]
mod tests {
    use super::{LineKind, RunKind, StartupStep, classify_line, startup_step};

    #[test]
    fn startup_lines_classify_like_typed_lines() {
        assert_eq!(
            classify_line("/model ollama:qwen3.5:9b", false),
            LineKind::Slash
        );
        assert_eq!(classify_line("  /goal ship it", false), LineKind::Slash);
        assert_eq!(
            classify_line("!ls -la", false),
            LineKind::Bash {
                command: "ls -la".into(),
                exclude: false
            }
        );
        assert_eq!(
            classify_line("!!git status", false),
            LineKind::Bash {
                command: "git status".into(),
                exclude: true
            }
        );
        assert_eq!(classify_line("hello there", false), LineKind::Prompt);
        assert_eq!(
            classify_line("/usr/bin/env is a path", false),
            LineKind::Prompt
        );
        // Images (from `@file`) make the line a prompt.
        assert_eq!(classify_line("/model x", true), LineKind::Prompt);
    }

    #[test]
    fn startup_lines_keep_order() {
        let slash = LineKind::Slash;
        let prompt = LineKind::Prompt;
        let bash = LineKind::Bash {
            command: "ls".into(),
            exclude: false,
        };
        assert_eq!(startup_step(&slash, None, false), StartupStep::Submit);
        assert_eq!(startup_step(&prompt, None, false), StartupStep::Submit);
        // Prompts behind a running turn become follow-ups.
        assert_eq!(
            startup_step(&prompt, Some(RunKind::Agent), false),
            StartupStep::FollowUp
        );
        // Commands and `!` lines wait for the run before them.
        assert_eq!(
            startup_step(&slash, Some(RunKind::Agent), false),
            StartupStep::Wait
        );
        assert_eq!(
            startup_step(&bash, Some(RunKind::Agent), false),
            StartupStep::Wait
        );
        assert_eq!(
            startup_step(&prompt, Some(RunKind::Compaction), false),
            StartupStep::Wait
        );
        assert_eq!(startup_step(&prompt, None, true), StartupStep::Wait);
        assert_eq!(startup_step(&slash, None, true), StartupStep::Wait);
    }
}
