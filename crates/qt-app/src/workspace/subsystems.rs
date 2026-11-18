//! Workspace glue for the subsystem panels (`/index`, `/mcp`, `/llama`,
//! `/subagent`), extension commands, the package resource selectors,
//! `/reload` and the extension chrome (runs started outside the composer,
//! shortcuts, autocomplete providers).

use std::rc::Rc;

use async_channel::Receiver;
use gpui_kit::component::WindowExt as _;
use gpui_kit::component::notification::Notification;
use gpui_kit::*;
use qt_core::AgentEvent;
use qt_core::provider::ModelRef;
use qt_runtime::Runtime;
use qt_runtime::qt_extensions::packages::ResourceKind;
use qt_runtime::qt_extensions::ui::AutocompleteSuggestions;

use super::completion::{CompletionKind, CompletionRequest, Suggestion};
use super::{RunKind, Workspace};
use crate::RuntimeGlobal;
use crate::extensions::{ChromeEvent, open_packages, prefix_start};
use crate::index_panel::PanelHooks;

/// Suggestions of extension providers as popup rows replacing `prefix`.
pub fn extension_suggestions(
    text: &str,
    cursor: usize,
    suggestions: &AutocompleteSuggestions,
) -> Option<(CompletionRequest, Vec<Suggestion>)> {
    let start = prefix_start(text, cursor, &suggestions.prefix)?;
    let items: Vec<Suggestion> = suggestions
        .items
        .iter()
        .map(|item| Suggestion {
            value: item.value.clone(),
            label: item.label.clone(),
            detail: item.description.clone(),
            directory: false,
        })
        .collect();
    (!items.is_empty()).then(|| {
        (
            CompletionRequest {
                kind: CompletionKind::Extension,
                query: suggestions.prefix.clone(),
                start,
            },
            items,
        )
    })
}

/// The selector kind of `/extensions`, `/skills`, `/prompts`, `/themes`
/// (`/settings resources|packages` open the extensions list).
pub fn resource_kind(kind: &str) -> Option<ResourceKind> {
    match kind {
        "resources" | "packages" => Some(ResourceKind::Extensions),
        other => ResourceKind::parse(other),
    }
}

impl Workspace {
    fn runtime(&self, cx: &App) -> Runtime {
        cx.global::<RuntimeGlobal>().0.clone()
    }

    /// Callbacks panels use to reach the workspace.
    pub(super) fn panel_hooks(&self, cx: &mut Context<Self>) -> PanelHooks {
        let card = cx.entity().downgrade();
        let prefill = card.clone();
        let models = card.clone();
        let choose = card.clone();
        PanelHooks {
            card: Rc::new(move |title, lines, error, _, cx| {
                let _ = card.update(cx, |this, cx| this.card(title, lines, error, cx));
            }),
            prefill: Rc::new(move |text, window, cx| {
                let _ = prefill.update(cx, |this, cx| {
                    this.set_composer_end(&text, window, cx);
                    cx.notify();
                });
            }),
            models_changed: Rc::new(move |window, cx| {
                let _ = models.update(cx, |this, cx| this.refresh_models(window, cx));
            }),
            choose_model: Rc::new(move |model: ModelRef, window, cx| {
                let _ = choose.update(cx, |this, cx| this.choose_model(model, window, cx));
            }),
        }
    }

    /// `/index`, `/mcp`, `/llama`, `/subagent`: the panel without
    /// arguments, the archived text command (as a card) with them.
    /// Returns false for commands this module does not own.
    pub(super) fn open_subsystem(
        &mut self,
        line: &str,
        command: &str,
        arguments: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if !matches!(command, "index" | "mcp" | "llama" | "subagent") {
            return false;
        }
        let runtime = self.runtime(cx);
        if arguments.trim().is_empty() {
            let hooks = self.panel_hooks(cx);
            match command {
                "index" => crate::index_panel::open(runtime, hooks, window, cx),
                "mcp" => crate::mcp_panel::open(runtime, hooks, window, cx),
                "llama" => crate::llama_panel::open(
                    runtime,
                    hooks,
                    crate::llama_panel::Tab::Status,
                    window,
                    cx,
                ),
                _ => crate::subagent_panel::open(runtime, hooks, window, cx),
            }
            return true;
        }
        let title = line.to_string();
        let conversation = self.conversation.clone();
        let (command, arguments) = (command.to_string(), arguments.to_string());
        let refreshes_models = command == "llama"
            && matches!(
                arguments.split_whitespace().next(),
                Some("discover" | "select" | "reload")
            );
        let handle = self.engine.spawn(async move {
            runtime
                .run_subsystem_command(&conversation, &command, &arguments)
                .await
        });
        cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update_in(cx, |this, window, cx| {
                match result {
                    Ok(response) => this.card(title, response.lines, false, cx),
                    Err(e) => this.card(title, vec![e.to_string()], true, cx),
                }
                if refreshes_models {
                    this.refresh_models(window, cx);
                }
            });
        })
        .detach();
        true
    }

    /// Run extension command `/name` off the UI thread.
    pub(super) fn run_extension_command(
        &mut self,
        name: String,
        arguments: String,
        cx: &mut Context<Self>,
    ) {
        let before = self.conversation.agent_messages().len();
        self.ext
            .update(cx, |ext, cx| ext.run_command(name, arguments, before, cx));
    }

    /// `/extensions`, `/skills`, `/prompts`, `/themes` selectors.
    pub(super) fn open_package_resources(
        &mut self,
        kind: &str,
        filter: Option<String>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let Some(kind) = resource_kind(kind) else {
            return false;
        };
        let runtime = self.runtime(cx);
        open_packages(runtime, self.ext.downgrade(), kind, filter, window, cx);
        true
    }

    /// `/reload`: extensions and resources, then the views that show them.
    pub(super) fn reload_runtime(
        &mut self,
        line: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let lines = self.ext.update(cx, |ext, cx| ext.reload(cx));
        crate::theme::rediscover(&self.engine, cx);
        self.card(line.to_string(), lines, false, cx);
        self.rebuild_model_items(window, cx);
        self.sync_thinking(window, cx);
        cx.notify();
    }

    pub(super) fn on_chrome_event(
        &mut self,
        event: &ChromeEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match event {
            ChromeEvent::Card {
                title,
                lines,
                error,
                messages_before,
            } => {
                self.card(title.clone(), lines.clone(), *error, cx);
                if let Some(before) = messages_before {
                    self.show_appended_messages(*before, cx);
                }
            }
            ChromeEvent::Run(events) => self.external_run(events.clone(), window, cx),
        }
    }

    /// Show displayed messages an idle extension command appended to the
    /// session (`pi.sendMessage` without a turn).
    fn show_appended_messages(&mut self, before: usize, cx: &mut Context<Self>) {
        if self.running.is_some() {
            return;
        }
        let messages = self.conversation.agent_messages();
        for message in messages.iter().skip(before) {
            if matches!(message, qt_core::message::AgentMessage::Custom(c) if c.display) {
                let event = AgentEvent::CustomMessageEnd(message.clone());
                self.transcript
                    .update(cx, |t, cx| t.apply_event(&event, cx));
            }
        }
    }

    /// A run started by an extension or a subagent completion: pump it now,
    /// or after the current run.
    fn external_run(
        &mut self,
        events: Receiver<AgentEvent>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.running.is_some() {
            self.pending_runs.push_back(events);
            return;
        }
        self.attach_run(events, RunKind::Agent, window, cx);
    }

    /// Start the next queued external run, if any (after a run finished).
    pub(super) fn next_pending_run(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.running.is_none()
            && let Some(events) = self.pending_runs.pop_front()
        {
            self.attach_run(events, RunKind::Agent, window, cx);
        }
    }

    /// Registered extension shortcuts (capture phase, before the composer).
    pub(super) fn extension_shortcut(
        &mut self,
        event: &KeyDownEvent,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self
            .ext
            .update(cx, |ext, _| ext.handle_keystroke(&event.keystroke))
        {
            cx.stop_propagation();
        }
    }

    /// Run the `input` hooks before sending; `None` when an extension
    /// handled (or rejected) the input.
    pub(super) fn input_hooks(
        &mut self,
        text: &str,
        images: Vec<qt_core::ContentBlock>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Option<(String, Vec<qt_core::ContentBlock>)> {
        use qt_core::harness::hooks::{InputOutcome, InputSource};
        match self
            .engine
            .process_input(text, images, InputSource::Interactive)
        {
            Ok(InputOutcome::Continue { text, images }) => Some((text, images)),
            Ok(InputOutcome::Handled) => None,
            Err(err) => {
                window.push_notification(Notification::error(format!("{err:#}")), cx);
                None
            }
        }
    }

    /// Extension autocomplete for the composer state, merged into `base`
    /// when the prefixes agree.
    pub(super) fn extension_completion(
        &self,
        text: &str,
        cursor: usize,
        base: Option<(&CompletionRequest, &mut Vec<Suggestion>)>,
        cx: &App,
    ) -> Option<(CompletionRequest, Vec<Suggestion>)> {
        let suggestions = self.ext.read(cx).autocomplete(text, cursor)?;
        let (request, items) = extension_suggestions(text, cursor, &suggestions)?;
        match base {
            Some((base_request, base_items)) => {
                if request.start == base_request.start {
                    base_items.extend(items);
                }
                None
            }
            None => Some((request, items)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{extension_suggestions, resource_kind};
    use crate::workspace::completion::CompletionKind;
    use qt_runtime::qt_extensions::packages::ResourceKind;
    use qt_runtime::qt_extensions::ui::{AutocompleteItem, AutocompleteSuggestions};

    #[test]
    fn extension_rows_replace_the_prefix() {
        let s = AutocompleteSuggestions {
            prefix: ":sm".into(),
            items: vec![AutocompleteItem {
                value: ":smile:".into(),
                label: "smile".into(),
                description: Some("emoji".into()),
            }],
        };
        let (request, items) = extension_suggestions("hi :sm", 6, &s).unwrap();
        assert_eq!(request.kind, CompletionKind::Extension);
        assert_eq!(request.start, 3);
        assert_eq!(items[0].detail.as_deref(), Some("emoji"));
        assert!(extension_suggestions("hi :x", 5, &s).is_none());
        assert_eq!(resource_kind("prompts"), Some(ResourceKind::Prompts));
        assert_eq!(resource_kind("packages"), Some(ResourceKind::Extensions));
        assert_eq!(resource_kind("bogus"), None);
    }
}
