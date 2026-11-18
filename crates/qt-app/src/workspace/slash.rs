//! Slash commands in the GUI: dispatch through the core registry, render
//! text output as transcript cards and map [`SlashAction`]s onto panels and
//! pickers (archived `interactive_mode.cpp` command handlers).

use std::path::PathBuf;
use std::rc::Rc;

use gpui_kit::component::WindowExt as _;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::notification::Notification;
use gpui_kit::component::{ActiveTheme as _, h_flex, v_flex};
use gpui_kit::*;
use qt_core::provider::{ModelRef, ThinkingLevel};
use qt_core::slash::{
    SlashAction, SlashCommand, SlashCommandSource, SlashOutcome, dispatch,
    is_blocked_while_running, parse_invocation,
};

use super::panels::{self, ResultHandler};
use super::picker::{PickerItem, open_picker};
use super::{RunKind, Workspace};
use crate::dialogs;
use crate::model_picker::{ModelItem, thinking_title};
use crate::settings_panel;
use crate::theme::{self, ThemeState};

/// Subsystems that have no GUI panel in this build.
pub fn unavailable_text(what: &str) -> String {
    format!("{what} is not available in this build.")
}

/// Palette row for a command: title `/name`, detail `hint - description`.
pub fn palette_item(command: &SlashCommand) -> PickerItem {
    let detail = match (&command.argument_hint, command.description.is_empty()) {
        (Some(hint), false) => format!("{hint} - {}", command.description),
        (Some(hint), true) => hint.clone(),
        (None, _) => command.description.clone(),
    };
    let source = match command.source {
        SlashCommandSource::Builtin => String::new(),
        other => format!(" [{}]", other.as_str()),
    };
    PickerItem::new(command.name.clone(), format!("/{}", command.name))
        .detail(format!("{detail}{source}"))
}

/// Whether picking `command` in the palette should only prefill the
/// composer: its arguments are required (`<...>` hint).
pub fn palette_needs_arguments(command: &SlashCommand) -> bool {
    command
        .argument_hint
        .as_deref()
        .is_some_and(|hint| hint.trim_start().starts_with('<'))
}

/// Path typed into the export fallback dialog: `.html` unless `.jsonl` or
/// `.html` is given.
pub fn export_target(path: &str) -> String {
    let path = path.trim();
    let lower = path.to_ascii_lowercase();
    if lower.ends_with(".html") || lower.ends_with(".jsonl") {
        path.to_string()
    } else {
        format!("{path}.html")
    }
}

impl Workspace {
    /// Push slash output into the transcript.
    pub(super) fn card(
        &mut self,
        title: impl Into<SharedString>,
        lines: Vec<String>,
        error: bool,
        cx: &mut Context<Self>,
    ) {
        let title = title.into();
        self.transcript
            .update(cx, |t, cx| t.push_card(title, &lines, error, cx));
        cx.notify();
    }

    /// Run a `/command` line typed in the composer.
    pub(super) fn run_slash(&mut self, line: &str, window: &mut Window, cx: &mut Context<Self>) {
        let line = line.trim().to_string();
        let invocation = match parse_invocation(&line) {
            Ok(i) => i,
            Err(err) => return self.card(line, vec![err], true, cx),
        };
        let args = invocation.arguments.trim().to_string();
        let registry = self.engine.slash_commands();
        let builtin = registry
            .find(&invocation.name)
            .is_some_and(|c| c.source == SlashCommandSource::Builtin);
        if builtin || invocation.name == "exit" {
            match invocation.name.as_str() {
                "exit" => return cx.quit(),
                "context" => {
                    if self.conversation.is_running() && is_blocked_while_running("context", &args)
                    {
                        return self.card(
                            line,
                            vec!["slash commands are unavailable while a turn is running".into()],
                            true,
                            cx,
                        );
                    }
                    panels::open_context(&self.conversation, &args, window, cx);
                    return;
                }
                "export" if args.is_empty() => return self.prompt_export(window, cx),
                "name" if args.is_empty() => {
                    return self.rename_session(&super::RenameSession, window, cx);
                }
                _ => {}
            }
        }
        match dispatch(&self.conversation, &line) {
            Ok(outcome) => self.handle_outcome(&line, &invocation.name, outcome, window, cx),
            Err(err) => self.card(line, vec![format!("{err:#}")], true, cx),
        }
    }

    fn handle_outcome(
        &mut self,
        line: &str,
        name: &str,
        outcome: SlashOutcome,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match outcome {
            SlashOutcome::Output(lines) => {
                self.card(line.to_string(), lines, false, cx);
                // `/name`, `/model x`, `/thinking x`, `/reload` change state shown elsewhere.
                self.after_state_change(window, cx);
            }
            SlashOutcome::Prompt(text) => {
                self.send_text(&text, Vec::new(), false, window, cx);
            }
            SlashOutcome::Run { lines, events } => {
                if !lines.is_empty() {
                    self.card(line.to_string(), lines, false, cx);
                }
                let kind = if name == "compact" {
                    RunKind::Compaction
                } else {
                    RunKind::Agent
                };
                self.attach_run(events, kind, window, cx);
            }
            SlashOutcome::Task(handle) => {
                let title = line.to_string();
                self.card(title.clone(), vec!["Working…".into()], false, cx);
                cx.spawn_in(window, async move |this, cx| {
                    let result = handle.await;
                    let _ = this.update(cx, |this, cx| match result {
                        Ok(Ok(lines)) => this.card(title, lines, false, cx),
                        Ok(Err(err)) => this.card(title, vec![format!("{err:#}")], true, cx),
                        Err(err) => this.card(title, vec![format!("{err}")], true, cx),
                    });
                })
                .detach();
            }
            SlashOutcome::Refresh { lines, listing } => {
                self.card(line.to_string(), lines, false, cx);
                self.models_loading = true;
                cx.spawn_in(window, async move |this, cx| {
                    let results = listing.await.unwrap_or_default();
                    let _ = this.update_in(cx, |this, window, cx| {
                        this.set_models(results, window, cx);
                        window
                            .push_notification(Notification::info("Model catalogs refreshed"), cx);
                    });
                })
                .detach();
            }
            SlashOutcome::SwitchConversation {
                conversation,
                message,
            } => {
                self.switch_to(conversation, window, cx);
                window.push_notification(Notification::info(message), cx);
            }
            SlashOutcome::Copy { text, lines } => {
                cx.write_to_clipboard(ClipboardItem::new_string(text));
                self.card(line.to_string(), lines, false, cx);
            }
            SlashOutcome::Action(action) => self.handle_action(line, action, window, cx),
        }
    }

    /// Re-read state a command may have changed (name, model, thinking).
    fn after_state_change(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.sessions = self.engine.sessions();
        self.rebuild_model_items(window, cx);
        self.sync_thinking(window, cx);
        cx.notify();
    }

    fn handle_action(
        &mut self,
        line: &str,
        action: SlashAction,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match action {
            SlashAction::OpenSettings { target } => {
                if matches!(target.as_deref(), Some("theme" | "themes")) {
                    self.open_theme_picker(window, cx);
                } else {
                    self.open_settings(&super::OpenSettings, window, cx);
                }
            }
            SlashAction::OpenPackageResources { kind, .. } => {
                if kind == "themes" {
                    self.open_theme_picker(window, cx);
                } else {
                    let what = match kind.as_str() {
                        "extensions" => "The extensions panel",
                        "skills" => "Skill package configuration",
                        "prompts" => "Prompt package configuration",
                        _ => "Package resource configuration",
                    };
                    self.card(line.to_string(), vec![unavailable_text(what)], false, cx);
                }
            }
            SlashAction::SelectModel { search } => {
                self.open_model_picker(search.as_deref(), window, cx)
            }
            SlashAction::SelectScopedModels { .. } => {
                self.open_settings(&super::OpenSettings, window, cx)
            }
            SlashAction::SelectThinking { current, levels } => {
                self.open_thinking_picker(current, levels, window, cx)
            }
            SlashAction::OpenTree { .. } => self.open_tree(&super::OpenTree, window, cx),
            SlashAction::ResumeSession => self.open_resume(&super::OpenResume, window, cx),
            SlashAction::ForkFromMessage { messages } => {
                let this = cx.entity().downgrade();
                dialogs::open_fork_picker(
                    messages,
                    move |id, window, cx| {
                        let _ = this.update(cx, |this, cx| this.fork_from(id, window, cx));
                    },
                    window,
                    cx,
                );
            }
            SlashAction::Login { .. } | SlashAction::Logout { .. } => {
                self.open_login(&super::OpenLogin, window, cx)
            }
            SlashAction::SelectTools { filter, .. } => self.open_tools_panel(filter, window, cx),
            SlashAction::ImportMissingCwd { path, .. } => self.import(path, window, cx),
            SlashAction::Subsystem { command, .. } => match command.as_str() {
                "ollama" => self.open_ollama(&super::OpenOllama, window, cx),
                other => {
                    let what = match other {
                        "llama" => "The llama.cpp panel (/llama)",
                        "memory" => "The memory panel (/memory)",
                        "index" => "The project index panel (/index)",
                        "mcp" => "The MCP panel (/mcp)",
                        "subagent" => "The subagent panel (/subagent)",
                        _ => "This command",
                    };
                    self.card(line.to_string(), vec![unavailable_text(what)], false, cx);
                }
            },
            SlashAction::Extension { name, .. } => self.card(
                line.to_string(),
                vec![unavailable_text(&format!("Extension command /{name}"))],
                false,
                cx,
            ),
            SlashAction::Quit => cx.quit(),
        }
    }

    // ----- pickers -------------------------------------------------------------

    /// Apply a model choice (header select, `/model` picker).
    pub(super) fn choose_model(
        &mut self,
        model: ModelRef,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.conversation.set_model(model.clone());
        if let Err(err) = self.engine.set_default_model(&model) {
            window.push_notification(
                Notification::error(format!("Could not save default model: {err:#}")),
                cx,
            );
        }
        if let Some(size) = self.engine.context_window(&model) {
            self.conversation.set_context_window(Some(size));
        }
        self.rebuild_model_items(window, cx);
        self.sync_thinking(window, cx);
        cx.notify();
    }

    pub(super) fn open_model_picker(
        &mut self,
        search: Option<&str>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let current = self.conversation.model().map(|m| m.to_string());
        let mut items: Vec<PickerItem> = self
            .models
            .iter()
            .map(|m| {
                let item = ModelItem::from_info(m, &self.provider_label(&m.provider));
                let value = item.value.to_string();
                PickerItem::new(value.clone(), item.title.to_string())
                    .detail(value.clone())
                    .current(current.as_deref() == Some(value.as_str()))
            })
            .collect();
        if let Some(model) = &current
            && !items.iter().any(|i| &i.value == model)
        {
            items.insert(
                0,
                PickerItem::new(model.clone(), model.clone()).current(true),
            );
        }
        let this = cx.entity().downgrade();
        let placeholder = if self.models_loading {
            "Search models (still loading…)"
        } else {
            "Search models"
        };
        open_picker(
            "Select model",
            placeholder,
            items,
            search,
            move |item, window, cx| {
                if let Some(model) = ModelRef::parse(&item.value) {
                    let _ = this.update(cx, |this, cx| this.choose_model(model, window, cx));
                }
            },
            window,
            cx,
        );
    }

    fn open_thinking_picker(
        &mut self,
        current: ThinkingLevel,
        levels: Vec<ThinkingLevel>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let items = levels
            .iter()
            .map(|l| PickerItem::new(l.as_str(), thinking_title(*l)).current(*l == current))
            .collect();
        let this = cx.entity().downgrade();
        open_picker(
            "Thinking level",
            "Filter levels",
            items,
            None,
            move |item, window, cx| {
                if let Some(level) = ThinkingLevel::parse(&item.value) {
                    let _ = this.update(cx, |this, cx| {
                        this.conversation.set_thinking(level);
                        this.sync_thinking(window, cx);
                        cx.notify();
                    });
                }
            },
            window,
            cx,
        );
    }

    pub(super) fn open_theme_picker(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let applied = ThemeState::global(cx)
            .map(|s| s.applied.clone())
            .unwrap_or_default();
        let items = ThemeState::available(cx)
            .into_iter()
            .map(|name| {
                let current = name == applied;
                PickerItem::new(name.clone(), name).current(current)
            })
            .collect();
        let engine = self.engine.clone();
        open_picker(
            "Theme",
            "Search themes",
            items,
            None,
            move |item, window, cx| {
                let name = item.value;
                if let Err(err) =
                    engine.update_settings(|s| settings_panel::set_theme_setting(s, Some(&name)))
                {
                    window.push_notification(
                        Notification::error(format!("Could not save theme: {err:#}")),
                        cx,
                    );
                }
                theme::apply_selection(Some(&name), cx);
            },
            window,
            cx,
        );
    }

    /// Ctrl/Cmd+K: every registered command; commands with required
    /// arguments prefill the composer, the rest run immediately.
    pub(super) fn open_palette(
        &mut self,
        _: &super::CommandPalette,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let commands = self.engine.slash_commands().commands;
        let items = commands.iter().map(palette_item).collect();
        let this = cx.entity().downgrade();
        open_picker(
            "Command palette",
            "Type a command",
            items,
            None,
            move |item, window, cx| {
                let name = item.value;
                let needs_args = commands
                    .iter()
                    .find(|c| c.name == name)
                    .is_some_and(palette_needs_arguments);
                let _ = this.update(cx, |this, cx| {
                    if needs_args {
                        let text = format!("/{name} ");
                        this.set_composer_end(&text, window, cx);
                        this.refresh_completion(window, cx);
                    } else {
                        this.history_record(&format!("/{name}"), window, cx);
                        this.run_slash(&format!("/{name}"), window, cx);
                    }
                });
            },
            window,
            cx,
        );
    }

    pub(super) fn open_tools_panel(
        &mut self,
        filter: Option<String>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let this = cx.entity().downgrade();
        let on_result: ResultHandler = Rc::new(move |title, lines, error, _, cx| {
            let _ = this.update(cx, |this, cx| {
                this.card(title, lines, error, cx);
                this.pending_mutations = this.engine.mutation_queue().len();
            });
        });
        panels::open_tools(
            &self.engine,
            &self.conversation,
            filter,
            on_result,
            window,
            cx,
        );
    }

    pub(super) fn open_tools_action(
        &mut self,
        _: &super::OpenTools,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.open_tools_panel(None, window, cx);
    }

    pub(super) fn open_context_action(
        &mut self,
        _: &super::OpenContext,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.run_slash("/context", window, cx);
    }

    // ----- export ------------------------------------------------------------------

    pub(super) fn export_action(
        &mut self,
        _: &super::ExportSession,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.prompt_export(window, cx);
    }

    /// `/export` without a path: the platform save dialog (HTML by default,
    /// `.jsonl` for JSONL); an in-app path dialog when no portal is available.
    fn prompt_export(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.conversation.is_persisted() {
            return self.card(
                "/export",
                vec!["Cannot export an in-memory session".into()],
                true,
                cx,
            );
        }
        let default_name =
            qt_core::export::default_html_filename(&self.conversation.session_path());
        let directory = self.engine.cwd().to_path_buf();
        let rx = cx.prompt_for_new_path(&directory, Some(&default_name));
        cx.spawn_in(window, async move |this, cx| {
            let choice = rx.await;
            let _ = this.update_in(cx, |this, window, cx| match choice {
                Ok(Ok(Some(path))) => this.export_to(path, window, cx),
                Ok(Ok(None)) => {}
                Ok(Err(_)) | Err(_) => {
                    this.open_export_dialog(directory.join(default_name), window, cx)
                }
            });
        })
        .detach();
    }

    fn export_to(&mut self, path: PathBuf, window: &mut Window, cx: &mut Context<Self>) {
        let target = export_target(&path.to_string_lossy());
        // The whole argument text is the path, spaces included.
        let line = format!("/export {target}");
        self.run_slash(&line, window, cx);
    }

    fn open_export_dialog(
        &mut self,
        suggested: PathBuf,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let input =
            cx.new(|cx| InputState::new(window, cx).default_value(suggested.display().to_string()));
        input.update(cx, |s, cx| s.focus(window, cx));
        let this = cx.entity().downgrade();
        window.open_dialog(cx, move |dialog, _, cx| {
            let input_ok = input.clone();
            let this_ok = this.clone();
            let input_enter = input.clone();
            let this_enter = this.clone();
            dialog
                .title("Export session")
                .w(px(620.))
                .on_ok(move |_, window, cx| {
                    let path = input_enter.read(cx).value().to_string();
                    let _ = this_enter.update(cx, |this, cx| {
                        this.export_to(PathBuf::from(path), window, cx)
                    });
                    true
                })
                .child(
                    v_flex()
                        .gap_2()
                        .child(
                            div()
                                .text_sm()
                                .text_color(cx.theme().muted_foreground)
                                .child("File to write: .html for a web page, .jsonl for the raw session."),
                        )
                        .child(Input::new(&input)),
                )
                .footer(
                    h_flex()
                        .w_full()
                        .justify_end()
                        .gap_2()
                        .child(
                            Button::new("export-cancel")
                                .label("Cancel")
                                .on_click(|_, window, cx| window.close_dialog(cx)),
                        )
                        .child(Button::new("export-ok").label("Export").primary().on_click(
                            move |_, window, cx| {
                                let path = input_ok.read(cx).value().to_string();
                                window.close_dialog(cx);
                                let _ = this_ok.update(cx, |this, cx| {
                                    this.export_to(PathBuf::from(path), window, cx)
                                });
                            },
                        )),
                )
        });
    }
}

#[cfg(test)]
mod tests {
    use super::{export_target, palette_item, palette_needs_arguments, unavailable_text};
    use qt_core::slash::{SlashCommand, builtin_commands};

    #[test]
    fn palette_rows_and_argument_detection() {
        let commands = builtin_commands();
        let find = |n: &str| commands.iter().find(|c| c.name == n).unwrap().clone();
        let model = palette_item(&find("model"));
        assert_eq!(model.title, "/model");
        assert_eq!(
            model.detail.as_deref(),
            Some("[provider/model|search|refresh] - Select model")
        );
        assert!(!palette_needs_arguments(&find("model")));
        assert!(palette_needs_arguments(&find("follow-up")));
        assert!(palette_needs_arguments(&find("ollama")));
        assert!(!palette_needs_arguments(&find("help")));
        let ext = SlashCommand::extension("deploy", "Deploy it", None);
        assert_eq!(
            palette_item(&ext).detail.as_deref(),
            Some("Deploy it [extension]")
        );
    }

    #[test]
    fn export_paths_and_texts() {
        assert_eq!(export_target("/tmp/a.jsonl"), "/tmp/a.jsonl");
        assert_eq!(export_target("/tmp/a.HTML"), "/tmp/a.HTML");
        assert_eq!(export_target(" /tmp/a "), "/tmp/a.html");
        assert_eq!(
            unavailable_text("The MCP panel (/mcp)"),
            "The MCP panel (/mcp) is not available in this build."
        );
    }
}
