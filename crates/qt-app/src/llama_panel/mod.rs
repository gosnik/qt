//! `/llama` panel: runtime status (devices, backends, offload), configured
//! models with selection, loading and "use in chat", local GGUF discovery,
//! a prompt / tagged-run playground, embeddings test, policy, grammar, GPU
//! settings and KV cache operations. Every action is an archived `/llama`
//! subcommand run on the llama worker off the UI thread.

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputState, Textarea, TextareaState};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::provider::ModelRef;
use qt_runtime::Runtime;
use qt_runtime::qt_llama::{LlamaSettings, ModelSettings, PROVIDER_ID};

use crate::index_panel::{PanelHooks, output_view, section};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tab {
    Status,
    Models,
    Playground,
    Runtime,
}

impl Tab {
    pub const ALL: [Tab; 4] = [Self::Status, Self::Models, Self::Playground, Self::Runtime];

    pub fn label(self) -> &'static str {
        match self {
            Self::Status => "Status",
            Self::Models => "Models",
            Self::Playground => "Playground",
            Self::Runtime => "KV · GPU · Grammar",
        }
    }
}

/// Playground actions: `(label, /llama command prefix)`.
pub const PLAYGROUND_ACTIONS: &[(&str, &str)] = &[
    ("Prompt", "prompt"),
    ("Run chat", "run chat"),
    ("Run instruct", "run instruct"),
    ("Run rank", "run rank"),
    ("Embed", "embeddings"),
];

/// The `/llama` line for a playground action, or the problem.
pub fn playground_command(action: &str, text: &str) -> Result<String, String> {
    let text = text.trim();
    if text.is_empty() {
        return Err("Enter text for the llama model first.".into());
    }
    Ok(format!("{action} {text}"))
}

/// One model row: id, display name, path, and whether it is selected.
pub fn model_rows(
    settings: &LlamaSettings,
    selected: Option<&str>,
) -> Vec<(String, String, String, bool)> {
    settings
        .models
        .iter()
        .map(|m: &ModelSettings| {
            (
                m.id.clone(),
                if m.name.is_empty() {
                    m.id.clone()
                } else {
                    m.name.clone()
                },
                m.path.clone(),
                selected == Some(m.id.as_str()),
            )
        })
        .collect()
}

pub struct LlamaPanel {
    runtime: Runtime,
    hooks: PanelHooks,
    tab: Tab,
    settings: LlamaSettings,
    selected: Option<String>,
    prompt: Entity<TextareaState>,
    discover: Entity<InputState>,
    kv_args: Entity<InputState>,
    gpu_args: Entity<InputState>,
    policy_args: Entity<InputState>,
    grammar: Entity<InputState>,
    title: String,
    output: Vec<String>,
    error: bool,
    busy: Option<String>,
    _task: Option<Task<()>>,
}

impl LlamaPanel {
    fn new(
        runtime: Runtime,
        hooks: PanelHooks,
        tab: Tab,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let input = |placeholder: &'static str, window: &mut Window, cx: &mut Context<Self>| {
            cx.new(|cx| InputState::new(window, cx).placeholder(placeholder))
        };
        let mut this = Self {
            prompt: cx.new(|cx| {
                TextareaState::new(window, cx)
                    .auto_grow(3, 10)
                    .placeholder("Text for the selected llama model")
            }),
            discover: input(
                "Directories to scan (empty: <project>/models, <agent home>/models)",
                window,
                cx,
            ),
            kv_args: input(
                "drop <seq> [from] [to] · shift <seq> <from> <to> <delta> · keep <seq>",
                window,
                cx,
            ),
            gpu_args: input(
                "gpuLayers=<n> mainGpu=<n> splitMode=<none|layer|row> offloadKqv=<bool>",
                window,
                cx,
            ),
            policy_args: input(
                "temperature=<n> topK=<n> topP=<n> minP=<n> seed=<n> maxOutputTokens=<n>",
                window,
                cx,
            ),
            grammar: input("Grammar name from llama.grammar.files", window, cx),
            settings: runtime.llama_settings(),
            selected: runtime.llama_selected_model(),
            runtime,
            hooks,
            tab,
            title: String::new(),
            output: Vec::new(),
            error: false,
            busy: None,
            _task: None,
        };
        if tab == Tab::Status {
            this.command("status".into(), window, cx);
        }
        this
    }

    fn reload_settings(&mut self) {
        self.settings = self.runtime.llama_settings();
        self.selected = self.runtime.llama_selected_model();
    }

    /// Run `/llama <line>` on the llama worker.
    fn command(&mut self, line: String, window: &mut Window, cx: &mut Context<Self>) {
        if self.busy.is_some() {
            return;
        }
        self.busy = Some(format!("/llama {line}…"));
        let runtime = self.runtime.clone();
        let args = line.clone();
        let handle = self
            .runtime
            .engine()
            .runtime_handle()
            .spawn_blocking(move || runtime.llama_command(&args));
        let changes_models = matches!(
            line.split_whitespace().next(),
            Some("discover" | "select" | "reload")
        );
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update_in(cx, |this, window, cx| {
                this.busy = None;
                this.title = format!("/llama {line}");
                match result {
                    Ok(lines) => {
                        this.error = lines.first().is_some_and(|l| {
                            l.starts_with("Usage")
                                || l.contains(" failed")
                                || l.starts_with("unknown llama")
                                || l.contains("not found")
                        });
                        this.output = lines;
                    }
                    Err(e) => {
                        this.error = true;
                        this.output = vec![format!("llama command failed: {e}")];
                    }
                }
                this.reload_settings();
                if changes_models {
                    (this.hooks.models_changed)(window, cx);
                }
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn input_command(
        &mut self,
        prefix: &str,
        input: &Entity<InputState>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let text = input.read(cx).value().trim().to_string();
        let line = if text.is_empty() {
            prefix.to_string()
        } else {
            format!("{prefix} {text}")
        };
        self.command(line, window, cx);
    }

    fn render_tabs(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let mut row = h_flex().gap_1();
        for tab in Tab::ALL {
            let selected = tab == self.tab;
            row = row.child(
                Button::new(SharedString::from(format!("llama-tab-{}", tab.label())))
                    .label(tab.label())
                    .small()
                    .when(selected, |b| b.primary())
                    .when(!selected, |b| b.ghost())
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.tab = tab;
                        cx.notify();
                    })),
            );
        }
        row
    }

    fn render_status(&self, cx: &mut Context<Self>) -> Div {
        let busy = self.busy.is_some();
        let theme = cx.theme().clone();
        let native = if qt_runtime::qt_llama::native_available() {
            let backends = qt_runtime::qt_llama::compiled_backends();
            if backends.is_empty() {
                "llama.cpp is built in (CPU)".to_string()
            } else {
                format!("llama.cpp is built in ({})", backends.join(", "))
            }
        } else {
            "llama.cpp is not built into this binary: models are listed but cannot run.".into()
        };
        v_flex()
            .gap_2()
            .child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(native),
            )
            .child(
                h_flex()
                    .gap_1()
                    .flex_wrap()
                    .child(self.button("llama-status", "Refresh status", "status", busy, cx))
                    .child(self.button("llama-gpu", "GPU and devices", "gpu", busy, cx))
                    .child(self.button("llama-grammar-list", "Grammars", "grammar list", busy, cx))
                    .child(self.button("llama-policy", "Policy", "policy", busy, cx))
                    .child(self.button("llama-reload", "Reload settings", "reload", busy, cx))
                    .child(self.button("llama-unload", "Unload", "unload", busy, cx)),
            )
    }

    fn button(
        &self,
        id: &'static str,
        label: &'static str,
        line: &'static str,
        busy: bool,
        cx: &mut Context<Self>,
    ) -> Button {
        Button::new(id)
            .label(label)
            .small()
            .ghost()
            .disabled(busy)
            .on_click(cx.listener(move |this, _, w, cx| this.command(line.into(), w, cx)))
    }

    fn render_models(&self, cx: &mut Context<Self>) -> Div {
        let theme = cx.theme().clone();
        let busy = self.busy.is_some();
        let mut list = v_flex()
            .id("llama-models")
            .gap_0p5()
            .max_h(px(240.))
            .overflow_y_scroll();
        let rows = model_rows(&self.settings, self.selected.as_deref());
        if rows.is_empty() {
            list = list.child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("No llama models are configured. Discover local GGUF files below or add llama.models to settings.json."),
            );
        }
        for (i, (id, name, path, selected)) in rows.into_iter().enumerate() {
            let (select_id, load_id, use_id) = (id.clone(), id.clone(), id.clone());
            list = list.child(
                h_flex()
                    .gap_1()
                    .items_center()
                    .px_2()
                    .py_1()
                    .rounded_md()
                    .when(selected, |el| el.bg(theme.list_active))
                    .child(
                        v_flex()
                            .flex_1()
                            .min_w_0()
                            .child(
                                div()
                                    .text_sm()
                                    .child(format!("{}{name}", if selected { "● " } else { "" })),
                            )
                            .child(
                                div()
                                    .text_xs()
                                    .text_color(theme.muted_foreground)
                                    .overflow_hidden()
                                    .text_ellipsis()
                                    .whitespace_nowrap()
                                    .child(format!("{PROVIDER_ID}/{id} · {path}")),
                            ),
                    )
                    .child(
                        Button::new(("llama-select", i))
                            .label("Select")
                            .xsmall()
                            .ghost()
                            .disabled(busy || selected)
                            .on_click(cx.listener(move |this, _, w, cx| {
                                this.command(format!("select {select_id}"), w, cx)
                            })),
                    )
                    .child(
                        Button::new(("llama-load", i))
                            .label("Load")
                            .xsmall()
                            .ghost()
                            .disabled(busy)
                            .on_click(cx.listener(move |this, _, w, cx| {
                                this.command(format!("load {load_id}"), w, cx)
                            })),
                    )
                    .child(
                        Button::new(("llama-use", i))
                            .label("Use in chat")
                            .xsmall()
                            .on_click(cx.listener(move |this, _, w, cx| {
                                (this.hooks.choose_model)(
                                    ModelRef::new(PROVIDER_ID, use_id.clone()),
                                    w,
                                    cx,
                                );
                                this.title = "llama".into();
                                this.error = false;
                                this.output =
                                    vec![format!("Chat model set to {PROVIDER_ID}/{use_id}.")];
                                cx.notify();
                            })),
                    ),
            );
        }
        v_flex()
            .gap_2()
            .child(list)
            .child(section("DISCOVER LOCAL MODELS", cx))
            .child(
                h_flex()
                    .gap_2()
                    .child(div().flex_1().child(Input::new(&self.discover).small()))
                    .child(
                        Button::new("llama-discover")
                            .label("Discover")
                            .small()
                            .disabled(busy)
                            .on_click(cx.listener(|this, _, w, cx| {
                                let input = this.discover.clone();
                                this.input_command("discover", &input, w, cx)
                            })),
                    ),
            )
            .child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("Discovered models are saved to llama.models in settings.json and appear in the model picker."),
            )
    }

    fn render_playground(&self, cx: &mut Context<Self>) -> Div {
        let busy = self.busy.is_some();
        let theme = cx.theme().clone();
        let mut actions = h_flex().gap_1().flex_wrap();
        for (i, (label, action)) in PLAYGROUND_ACTIONS.iter().enumerate() {
            let action = *action;
            actions = actions.child(
                Button::new(("llama-play", i))
                    .label(*label)
                    .small()
                    .when(i == 0, |b| b.primary())
                    .disabled(busy)
                    .on_click(cx.listener(move |this, _, w, cx| {
                        let text = this.prompt.read(cx).value().to_string();
                        match playground_command(action, &text) {
                            Ok(line) => this.command(line, w, cx),
                            Err(e) => {
                                this.error = true;
                                this.output = vec![e];
                                cx.notify();
                            }
                        }
                    })),
            );
        }
        v_flex()
            .gap_2()
            .child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(format!(
                        "Selected model: {}",
                        self.selected
                            .as_deref()
                            .map(|s| format!("{PROVIDER_ID}/{s}"))
                            .unwrap_or_else(|| "(none — select one under Models)".into())
                    )),
            )
            .child(Textarea::new(&self.prompt))
            .child(actions)
    }

    fn render_runtime(&self, cx: &mut Context<Self>) -> Div {
        let busy = self.busy.is_some();
        let row = |label: &'static str, input: &Entity<InputState>, button: Button| {
            h_flex()
                .gap_2()
                .items_center()
                .child(div().w(px(70.)).flex_none().text_xs().child(label))
                .child(div().flex_1().child(Input::new(input).small()))
                .child(button)
        };
        v_flex()
            .gap_2()
            .child(section("KV CACHE", cx))
            .child(
                h_flex()
                    .gap_1()
                    .child(self.button("llama-kv-status", "Inspect", "kv status", busy, cx))
                    .child(self.button("llama-kv-clear", "Clear", "kv clear", busy, cx)),
            )
            .child(row(
                "Operation",
                &self.kv_args,
                Button::new("llama-kv-run")
                    .label("Apply")
                    .small()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| {
                        let input = this.kv_args.clone();
                        this.input_command("kv", &input, w, cx)
                    })),
            ))
            .child(section("GPU OFFLOAD", cx))
            .child(row(
                "Settings",
                &self.gpu_args,
                Button::new("llama-gpu-save")
                    .label("Save")
                    .small()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| {
                        let input = this.gpu_args.clone();
                        this.input_command("gpu", &input, w, cx)
                    })),
            ))
            .child(section("GENERATION POLICY", cx))
            .child(row(
                "Policy",
                &self.policy_args,
                Button::new("llama-policy-save")
                    .label("Save")
                    .small()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| {
                        let input = this.policy_args.clone();
                        this.input_command("policy", &input, w, cx)
                    })),
            ))
            .child(section("GRAMMAR", cx))
            .child(
                h_flex()
                    .gap_1()
                    .child(self.button("llama-grammar-show", "Show", "grammar show", busy, cx))
                    .child(self.button("llama-grammar-files", "List", "grammar list", busy, cx))
                    .child(self.button("llama-grammar-off", "Disable", "grammar off", busy, cx)),
            )
            .child(row(
                "Default",
                &self.grammar,
                Button::new("llama-grammar-set")
                    .label("Use")
                    .small()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| {
                        let input = this.grammar.clone();
                        this.input_command("grammar default", &input, w, cx)
                    })),
            ))
    }
}

impl Render for LlamaPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let body = match self.tab {
            Tab::Status => self.render_status(cx),
            Tab::Models => self.render_models(cx),
            Tab::Playground => self.render_playground(cx),
            Tab::Runtime => self.render_runtime(cx),
        };
        v_flex()
            .gap_3()
            .child(self.render_tabs(cx))
            .child(body)
            .child(output_view(
                "llama-output",
                &self.output,
                self.error,
                self.busy.as_deref(),
                300.,
                cx,
            ))
            .when(!self.output.is_empty(), |el| {
                el.child(
                    h_flex().justify_end().child(
                        Button::new("llama-to-transcript")
                            .label("Copy to transcript")
                            .small()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| {
                                (this.hooks.card)(
                                    this.title.clone(),
                                    this.output.clone(),
                                    this.error,
                                    w,
                                    cx,
                                )
                            })),
                    ),
                )
            })
    }
}

/// Open the `/llama` panel on `tab`.
pub fn open(runtime: Runtime, hooks: PanelHooks, tab: Tab, window: &mut Window, cx: &mut App) {
    let panel = cx.new(|cx| LlamaPanel::new(runtime, hooks, tab, window, cx));
    window.open_dialog(cx, move |dialog, _, _| {
        dialog
            .title("llama.cpp")
            .w(px(820.))
            .on_ok(|_, _, _| false)
            .child(panel.clone())
    });
}

#[cfg(test)]
mod tests {
    use super::{Tab, model_rows, playground_command};
    use qt_runtime::qt_llama::{LlamaSettings, ModelSettings};

    #[test]
    fn tabs_commands_and_rows() {
        assert_eq!(Tab::ALL.len(), 4);
        assert_eq!(Tab::Runtime.label(), "KV · GPU · Grammar");
        assert_eq!(
            playground_command("run chat", " hi ").unwrap(),
            "run chat hi"
        );
        assert!(playground_command("prompt", "  ").is_err());
        let settings = LlamaSettings {
            models: vec![ModelSettings {
                id: "a".into(),
                path: "/a.gguf".into(),
                ..Default::default()
            }],
            ..Default::default()
        };
        assert_eq!(
            model_rows(&settings, Some("a")),
            [("a".into(), "a".into(), "/a.gguf".into(), true)]
        );
    }
}
