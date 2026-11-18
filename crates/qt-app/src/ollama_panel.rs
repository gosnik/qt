//! Ollama management (archived `/ollama`): instances add / configure /
//! remove, status, models, running, info, pull, load / unload and delete.

use std::rc::Rc;

use anyhow::Result;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::switch::Switch;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::Engine;
use qt_core::ollama::{
    OllamaClient, OllamaInstance, OllamaModel, configure_instance, normalize_base_url, provider_id,
};
use serde_json::Value;

use crate::composer::format_bytes;
use crate::dialogs::UiCallback;

/// JSON lines shown for `/api/show`.
const MAX_JSON_LINES: usize = 400;

/// Lines for `/api/ps`.
pub fn format_running(id: &str, value: &Value) -> Vec<String> {
    let mut out = vec![format!("Ollama {id} running models:")];
    let models = value
        .get("models")
        .and_then(Value::as_array)
        .cloned()
        .unwrap_or_default();
    if models.is_empty() {
        out.push("- none loaded".into());
    }
    for m in models {
        let name = m
            .get("name")
            .or_else(|| m.get("model"))
            .and_then(Value::as_str)
            .unwrap_or("?");
        let mut line = format!("- {name}");
        if let Some(size) = m.get("size").and_then(Value::as_u64) {
            line.push_str(&format!(" · {}", format_bytes(size as usize)));
        }
        if let Some(vram) = m.get("size_vram").and_then(Value::as_u64) {
            line.push_str(&format!(" · VRAM {}", format_bytes(vram as usize)));
        }
        if let Some(ctx) = m.get("context_length").and_then(Value::as_u64) {
            line.push_str(&format!(" · context {ctx}"));
        }
        if let Some(until) = m.get("expires_at").and_then(Value::as_str) {
            line.push_str(&format!(" · until {until}"));
        }
        out.push(line);
    }
    out
}

/// Lines for `/api/show`: key facts, then the pretty JSON.
pub fn format_info(model: &str, value: &Value) -> Vec<String> {
    let mut out = vec![format!("Model {model}")];
    let details = value.get("details").cloned().unwrap_or(Value::Null);
    for (key, label) in [
        ("family", "family"),
        ("parameter_size", "parameters"),
        ("quantization_level", "quantization"),
        ("format", "format"),
    ] {
        if let Some(v) = details
            .get(key)
            .and_then(Value::as_str)
            .filter(|v| !v.is_empty())
        {
            out.push(format!("{label}: {v}"));
        }
    }
    if let Some(caps) = value.get("capabilities").and_then(Value::as_array) {
        let caps: Vec<&str> = caps.iter().filter_map(Value::as_str).collect();
        if !caps.is_empty() {
            out.push(format!("capabilities: {}", caps.join(", ")));
        }
    }
    if let Some(info) = value.get("model_info").and_then(Value::as_object)
        && let Some(ctx) = info
            .iter()
            .find(|(k, _)| k.ends_with(".context_length"))
            .and_then(|(_, v)| v.as_u64())
    {
        out.push(format!("context length: {ctx}"));
    }
    let mut trimmed = value.clone();
    if let Some(obj) = trimmed.as_object_mut() {
        // Licenses and templates are long and rarely useful here.
        for key in ["license", "modelfile", "template", "tensors"] {
            obj.remove(key);
        }
    }
    out.push(String::new());
    let pretty = serde_json::to_string_pretty(&trimmed).unwrap_or_default();
    let lines: Vec<&str> = pretty.lines().collect();
    out.extend(lines.iter().take(MAX_JSON_LINES).map(|l| l.to_string()));
    if lines.len() > MAX_JSON_LINES {
        out.push(format!("… {} more lines", lines.len() - MAX_JSON_LINES));
    }
    out
}

/// Lines for a model refresh.
pub fn format_models(instance: &OllamaInstance) -> Vec<String> {
    let mut out = vec![format!(
        "Ollama {} models (select with model {}/<model>):",
        instance.id,
        provider_id(&instance.id)
    )];
    for m in &instance.models {
        out.push(format!(
            "- {} · context {} · max output {}",
            m.name, m.context_window, m.max_tokens
        ));
    }
    if instance.models.is_empty() {
        out.push("- no models installed".into());
    }
    out
}

fn persist(engine: &Engine, instance: &OllamaInstance) -> Result<()> {
    let mut result = Ok(());
    engine.update_settings(|s| {
        let exists = s
            .try_ollama_settings()
            .map(|list| list.iter().any(|i| i.id.eq_ignore_ascii_case(&instance.id)));
        result = match exists {
            Ok(true) => s.update_ollama_instance(instance.clone()),
            Ok(false) => s.add_ollama_instance(instance.clone()),
            Err(e) => Err(e),
        };
    })?;
    result
}

pub struct OllamaPanel {
    engine: Engine,
    http: reqwest::Client,
    instances: Vec<OllamaInstance>,
    /// The instances list came from the implicit local default.
    implicit: bool,
    selected: usize,
    output: Vec<String>,
    output_error: bool,
    busy: Option<String>,
    add_id: Entity<InputState>,
    add_url: Entity<InputState>,
    add_name: Entity<InputState>,
    cfg_name: Entity<InputState>,
    cfg_url: Entity<InputState>,
    cfg_timeout: Entity<InputState>,
    cfg_key: Entity<InputState>,
    cfg_enabled: bool,
    pull: Entity<InputState>,
    on_changed: UiCallback,
    _task: Option<Task<()>>,
}

impl OllamaPanel {
    pub fn new(
        engine: Engine,
        http: reqwest::Client,
        on_changed: impl Fn(&mut Window, &mut App) + 'static,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let input = |placeholder: &'static str, window: &mut Window, cx: &mut Context<Self>| {
            cx.new(|cx| InputState::new(window, cx).placeholder(placeholder))
        };
        let mut this = Self {
            add_id: input("id (e.g. gpu)", window, cx),
            add_url: input("http://host:11434", window, cx),
            add_name: input("Display name (optional)", window, cx),
            cfg_name: input("Display name", window, cx),
            cfg_url: input("Base URL", window, cx),
            cfg_timeout: input("Timeout (ms)", window, cx),
            cfg_key: cx.new(|cx| {
                InputState::new(window, cx)
                    .masked(true)
                    .placeholder("API key (optional)")
            }),
            pull: input("Model to pull, e.g. qwen3:8b", window, cx),
            cfg_enabled: true,
            engine,
            http,
            instances: Vec::new(),
            implicit: false,
            selected: 0,
            output: Vec::new(),
            output_error: false,
            busy: None,
            on_changed: Rc::new(on_changed),
            _task: None,
        };
        this.reload(window, cx);
        this
    }

    fn reload(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        match self.engine.settings().try_ollama_settings() {
            Ok(list) if list.is_empty() => {
                self.instances = vec![OllamaInstance::local_default()];
                self.implicit = true;
            }
            Ok(list) => {
                self.instances = list;
                self.implicit = false;
            }
            Err(err) => {
                self.instances = Vec::new();
                self.implicit = false;
                self.show(true, vec![format!("Invalid Ollama settings: {err:#}")]);
            }
        }
        self.selected = self.selected.min(self.instances.len().saturating_sub(1));
        self.load_form(window, cx);
    }

    fn current(&self) -> Option<&OllamaInstance> {
        self.instances.get(self.selected)
    }

    fn load_form(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(i) = self.current().cloned() else {
            return;
        };
        self.cfg_enabled = i.enabled;
        let set =
            |e: &Entity<InputState>, v: String, window: &mut Window, cx: &mut Context<Self>| {
                e.update(cx, |s, cx| s.set_value(v, window, cx))
            };
        set(&self.cfg_name.clone(), i.name.clone(), window, cx);
        set(&self.cfg_url.clone(), i.base_url.clone(), window, cx);
        set(
            &self.cfg_timeout.clone(),
            i.timeout_ms.to_string(),
            window,
            cx,
        );
        set(
            &self.cfg_key.clone(),
            i.api_key.clone().unwrap_or_default(),
            window,
            cx,
        );
        cx.notify();
    }

    fn show(&mut self, error: bool, lines: Vec<String>) {
        self.output = lines;
        self.output_error = error;
    }

    fn changed(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        (self.on_changed)(window, cx);
        self.reload(window, cx);
    }

    /// Run `op` against the selected instance on the engine runtime.
    fn run<T, F, Fut>(
        &mut self,
        label: String,
        op: F,
        done: impl FnOnce(&mut Self, Result<T>, OllamaInstance, &mut Window, &mut Context<Self>)
        + 'static,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) where
        T: Send + 'static,
        F: FnOnce(reqwest::Client, OllamaInstance) -> Fut,
        Fut: std::future::Future<Output = Result<T>> + Send + 'static,
    {
        let Some(instance) = self.current().cloned() else {
            return;
        };
        if self.busy.is_some() {
            return;
        }
        self.busy = Some(label);
        let handle = self.engine.spawn(op(self.http.clone(), instance.clone()));
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = match handle.await {
                Ok(r) => r,
                Err(e) => Err(anyhow::anyhow!("{e}")),
            };
            let _ = this.update_in(cx, |this, window, cx| {
                this.busy = None;
                done(this, result, instance, window, cx);
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn refresh_models(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.run(
            "Querying models…".into(),
            |http, instance| async move { OllamaClient::new(&http, &instance).discover_models().await },
            |this, result, mut instance, window, cx| match result {
                Ok(models) => {
                    instance.models = models;
                    if let Err(err) = persist(&this.engine, &instance) {
                        this.show(true, vec![format!("Failed to save Ollama settings: {err:#}")]);
                        return;
                    }
                    this.show(false, format_models(&instance));
                    this.changed(window, cx);
                }
                Err(err) => this.show(true, vec![format!("{err:#}")]),
            },
            window,
            cx,
        );
    }

    fn status(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.run(
            "Checking status…".into(),
            |http, instance| async move { OllamaClient::new(&http, &instance).version().await },
            |this, result, instance, _, _| match result {
                Ok(version) => this.show(
                    false,
                    vec![
                        format!("Ollama {}: online", instance.id),
                        format!("url: {}", instance.base_url),
                        format!("version: {version}"),
                        format!("cached models: {}", instance.models.len()),
                    ],
                ),
                Err(err) => this.show(
                    true,
                    vec![
                        format!("Ollama {}: unavailable", instance.id),
                        format!("{err:#}"),
                    ],
                ),
            },
            window,
            cx,
        );
    }

    fn running(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.run(
            "Querying running models…".into(),
            |http, instance| async move { OllamaClient::new(&http, &instance).running().await },
            |this, result, instance, _, _| match result {
                Ok(v) => this.show(false, format_running(&instance.id, &v)),
                Err(err) => this.show(true, vec![format!("{err:#}")]),
            },
            window,
            cx,
        );
    }

    fn info(&mut self, model: String, window: &mut Window, cx: &mut Context<Self>) {
        let name = model.clone();
        self.run(
            format!("Querying {model}…"),
            move |http, instance| async move { OllamaClient::new(&http, &instance).show(&name).await },
            move |this, result, _, _, _| match result {
                Ok(v) => this.show(false, format_info(&model, &v)),
                Err(err) => this.show(true, vec![format!("{err:#}")]),
            },
            window,
            cx,
        );
    }

    fn set_loaded(
        &mut self,
        model: String,
        load: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let name = model.clone();
        self.run(
            format!("{} {model}…", if load { "Loading" } else { "Unloading" }),
            move |http, instance| async move {
                OllamaClient::new(&http, &instance)
                    .set_loaded(&name, load)
                    .await
            },
            move |this, result, instance, _, _| match result {
                Ok(_) => this.show(
                    false,
                    vec![format!(
                        "{} {model} on {}",
                        if load { "Loaded" } else { "Unloaded" },
                        instance.id
                    )],
                ),
                Err(err) => this.show(true, vec![format!("{err:#}")]),
            },
            window,
            cx,
        );
    }

    fn pull_model(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let model = self.pull.read(cx).value().trim().to_string();
        if model.is_empty() {
            self.show(true, vec!["Enter a model name to pull.".into()]);
            cx.notify();
            return;
        }
        let name = model.clone();
        self.run(
            format!("Pulling {model}… (this can take a while)"),
            move |http, instance| async move {
                let client = OllamaClient::new(&http, &instance);
                client.pull(&name).await?;
                Ok(client.discover_models().await)
            },
            move |this, result, mut instance, window, cx| match result {
                Ok(refreshed) => {
                    let mut lines = vec![format!("Pulled {model} on {}", instance.id)];
                    match refreshed {
                        Ok(models) => {
                            instance.models = models;
                            match persist(&this.engine, &instance) {
                                Ok(()) => lines.push("Model catalog refreshed".into()),
                                Err(e) => {
                                    lines.push(format!("Warning: catalog refresh failed: {e:#}"))
                                }
                            }
                        }
                        Err(e) => lines.push(format!("Warning: catalog refresh failed: {e:#}")),
                    }
                    this.show(false, lines);
                    this.pull.update(cx, |s, cx| s.set_value("", window, cx));
                    this.changed(window, cx);
                }
                Err(err) => this.show(true, vec![format!("{err:#}")]),
            },
            window,
            cx,
        );
    }

    fn delete_model(&mut self, model: String, window: &mut Window, cx: &mut Context<Self>) {
        let name = model.clone();
        self.run(
            format!("Deleting {model}…"),
            move |http, instance| async move {
                let client = OllamaClient::new(&http, &instance);
                client.delete(&name).await?;
                Ok(client.discover_models().await)
            },
            move |this, result, mut instance, window, cx| match result {
                Ok(refreshed) => {
                    let mut lines = vec![format!("Deleted {model} from {}", instance.id)];
                    match refreshed {
                        Ok(models) => {
                            instance.models = models;
                            if let Err(e) = persist(&this.engine, &instance) {
                                lines.push(format!("Warning: {e:#}"));
                            }
                        }
                        Err(e) => lines.push(format!("Warning: {e:#}")),
                    }
                    this.show(false, lines);
                    this.changed(window, cx);
                }
                Err(err) => this.show(true, vec![format!("{err:#}")]),
            },
            window,
            cx,
        );
    }

    fn confirm_delete(&mut self, model: String, window: &mut Window, cx: &mut Context<Self>) {
        let this = cx.entity().downgrade();
        let id = self.current().map(|i| i.id.clone()).unwrap_or_default();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let this = this.clone();
            let model = model.clone();
            alert
                .confirm()
                .title(format!("Delete {model} from {id}?"))
                .description("Deleting a model removes it from the Ollama server.")
                .ok_text("Delete")
                .on_ok(move |_, window, cx| {
                    let model = model.clone();
                    let _ = this.update(cx, |this, cx| this.delete_model(model, window, cx));
                    true
                })
        });
    }

    fn add_instance(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let id = self.add_id.read(cx).value().trim().to_string();
        let url = self.add_url.read(cx).value().trim().to_string();
        let name = self.add_name.read(cx).value().trim().to_string();
        if id.is_empty() || url.is_empty() {
            self.show(true, vec!["usage: id and URL are required".into()]);
            cx.notify();
            return;
        }
        let mut instance = OllamaInstance::new(&id, &url);
        if !name.is_empty() {
            instance.name = name;
        }
        let mut result = Ok(());
        let saved = self
            .engine
            .update_settings(|s| result = s.add_ollama_instance(instance.clone()));
        if let Err(err) = result.and(saved) {
            self.show(true, vec![format!("{err:#}")]);
            cx.notify();
            return;
        }
        for input in [&self.add_id, &self.add_url, &self.add_name] {
            input.update(cx, |s, cx| s.set_value("", window, cx));
        }
        self.changed(window, cx);
        if let Some(index) = self.instances.iter().position(|i| i.id == instance.id) {
            self.selected = index;
            self.load_form(window, cx);
        }
        // Archived `/ollama add` queries models right away.
        let header = format!(
            "Configured Ollama instance {} at {}",
            instance.id, instance.base_url
        );
        self.run(
            "Querying models…".into(),
            |http, instance| async move { OllamaClient::new(&http, &instance).discover_models().await },
            move |this, result, mut instance, window, cx| {
                match result {
                    Ok(models) => {
                        instance.models = models;
                        let count = instance.models.len();
                        match persist(&this.engine, &instance) {
                            Ok(()) => this.show(false, vec![header, format!("Discovered {count} models")]),
                            Err(e) => this.show(true, vec![header, format!("Warning: {e:#}")]),
                        }
                    }
                    Err(e) => this.show(
                        true,
                        vec![header, format!("Warning: initial model query failed: {e:#}")],
                    ),
                }
                this.changed(window, cx);
            },
            window,
            cx,
        );
    }

    fn save_config(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(mut instance) = self.current().cloned() else {
            return;
        };
        let name = self.cfg_name.read(cx).value().trim().to_string();
        let url = self.cfg_url.read(cx).value().trim().to_string();
        let timeout = self.cfg_timeout.read(cx).value().trim().to_string();
        let key = self.cfg_key.read(cx).value().trim().to_string();
        let mut apply = |field: &str, value: &str| configure_instance(&mut instance, field, value);
        let result = (|| -> Result<()> {
            if normalize_base_url(&url) != instance_url(&self.instances, self.selected) {
                apply("url", &url)?;
            }
            apply(
                "name",
                if name.is_empty() {
                    &self.instances[self.selected].id
                } else {
                    &name
                },
            )?;
            apply("timeout", &timeout)?;
            apply("key", if key.is_empty() { "-" } else { &key })?;
            apply("enabled", if self.cfg_enabled { "on" } else { "off" })?;
            Ok(())
        })();
        if let Err(err) = result.and_then(|_| persist(&self.engine, &instance)) {
            self.show(true, vec![format!("{err:#}")]);
            cx.notify();
            return;
        }
        self.show(
            false,
            vec![format!("Updated Ollama instance {}", instance.id)],
        );
        self.changed(window, cx);
    }

    fn confirm_remove(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(id) = self.current().map(|i| i.id.clone()) else {
            return;
        };
        let this = cx.entity().downgrade();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let this = this.clone();
            let id = id.clone();
            alert
                .confirm()
                .title(format!("Remove Ollama instance {id}?"))
                .description(
                    "Removing an instance only removes qt configuration; server data is unchanged.",
                )
                .ok_text("Remove")
                .on_ok(move |_, window, cx| {
                    let id = id.clone();
                    let _ = this.update(cx, |this, cx| {
                        let mut result = Ok(());
                        let saved = this.engine.update_settings(|s| {
                            result = s.remove_ollama_instance(&id).map(|_| ())
                        });
                        match result.and(saved) {
                            Ok(()) => {
                                this.show(false, vec![format!("Removed Ollama instance {id}")]);
                                this.selected = 0;
                                this.changed(window, cx);
                            }
                            Err(e) => this
                                .show(true, vec![format!("Failed to save Ollama settings: {e:#}")]),
                        }
                        cx.notify();
                    });
                    true
                })
        });
    }

    fn render_model(
        &self,
        index: usize,
        model: &OllamaModel,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let theme = cx.theme().clone();
        let busy = self.busy.is_some();
        let name = model.name.clone();
        let (n1, n2, n3, n4) = (name.clone(), name.clone(), name.clone(), name.clone());
        let mut caps = model.capabilities.join(", ");
        if caps.is_empty() {
            caps = "completion".into();
        }
        h_flex()
            .gap_1()
            .items_center()
            .px_2()
            .py_1()
            .rounded_md()
            .hover(|s| s.bg(theme.muted.opacity(0.5)))
            .child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .child(div().text_sm().child(name))
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(format!(
                                "{} · context {} · {}",
                                format_bytes(model.size as usize),
                                model.context_window,
                                caps
                            )),
                    ),
            )
            .child(
                Button::new(("model-info", index))
                    .label("Info")
                    .xsmall()
                    .ghost()
                    .disabled(busy)
                    .on_click(cx.listener(move |this, _, w, cx| this.info(n1.clone(), w, cx))),
            )
            .child(
                Button::new(("model-load", index))
                    .label("Load")
                    .xsmall()
                    .ghost()
                    .disabled(busy)
                    .on_click(
                        cx.listener(move |this, _, w, cx| this.set_loaded(n2.clone(), true, w, cx)),
                    ),
            )
            .child(
                Button::new(("model-unload", index))
                    .label("Unload")
                    .xsmall()
                    .ghost()
                    .disabled(busy)
                    .on_click(
                        cx.listener(move |this, _, w, cx| {
                            this.set_loaded(n3.clone(), false, w, cx)
                        }),
                    ),
            )
            .child(
                Button::new(("model-delete", index))
                    .icon(IconName::Delete)
                    .xsmall()
                    .ghost()
                    .tooltip("Delete from server")
                    .disabled(busy)
                    .on_click(
                        cx.listener(move |this, _, w, cx| this.confirm_delete(n4.clone(), w, cx)),
                    ),
            )
    }
}

fn instance_url(instances: &[OllamaInstance], index: usize) -> String {
    instances
        .get(index)
        .map(|i| i.base_url.clone())
        .unwrap_or_default()
}

fn section(title: &'static str, cx: &App) -> Div {
    div()
        .text_xs()
        .font_weight(FontWeight::SEMIBOLD)
        .text_color(cx.theme().muted_foreground)
        .child(title)
}

impl Render for OllamaPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let busy = self.busy.is_some();

        let mut list = v_flex().gap_0p5();
        for (i, instance) in self.instances.iter().enumerate() {
            let selected = i == self.selected;
            list = list.child(
                v_flex()
                    .id(("ollama-instance", i))
                    .px_2()
                    .py_1()
                    .rounded_md()
                    .cursor_pointer()
                    .when(selected, |el| el.bg(theme.list_active))
                    .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.5))))
                    .on_click(cx.listener(move |this, _, window, cx| {
                        this.selected = i;
                        this.output.clear();
                        this.load_form(window, cx);
                    }))
                    .child(div().text_sm().child(format!(
                        "{}{}",
                        instance.display_name(),
                        if instance.enabled { "" } else { " (disabled)" }
                    )))
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(format!(
                                "{} · {} cached models{}",
                                instance.base_url,
                                instance.models.len(),
                                if self.implicit { " · implicit" } else { "" }
                            )),
                    ),
            );
        }

        let left = v_flex()
            .id("ollama-left")
            .w(px(250.))
            .flex_none()
            .h_full()
            .overflow_y_scroll()
            .gap_2()
            .child(section("INSTANCES", cx))
            .child(list)
            .child(section("ADD INSTANCE", cx))
            .child(Input::new(&self.add_id).small())
            .child(Input::new(&self.add_url).small())
            .child(Input::new(&self.add_name).small())
            .child(
                Button::new("ollama-add")
                    .icon(IconName::Plus)
                    .label("Add instance")
                    .small()
                    .disabled(busy)
                    .on_click(cx.listener(|this, _, w, cx| this.add_instance(w, cx))),
            );

        let mut right = v_flex()
            .id("ollama-right")
            .flex_1()
            .min_w_0()
            .h_full()
            .overflow_y_scroll()
            .gap_2();
        if let Some(instance) = self.current().cloned() {
            let field = |label: &'static str, input: &Entity<InputState>| {
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(div().w(px(70.)).flex_none().text_xs().child(label))
                    .child(div().flex_1().child(Input::new(input).small()))
            };
            right = right
                .child(
                    h_flex()
                        .gap_1()
                        .flex_wrap()
                        .child(
                            Button::new("ollama-status")
                                .label("Status")
                                .small()
                                .disabled(busy)
                                .on_click(cx.listener(|this, _, w, cx| this.status(w, cx))),
                        )
                        .child(
                            Button::new("ollama-models")
                                .icon(IconName::RefreshCw)
                                .label("Refresh models")
                                .small()
                                .disabled(busy)
                                .on_click(cx.listener(|this, _, w, cx| this.refresh_models(w, cx))),
                        )
                        .child(
                            Button::new("ollama-running")
                                .label("Running")
                                .small()
                                .disabled(busy)
                                .on_click(cx.listener(|this, _, w, cx| this.running(w, cx))),
                        ),
                )
                .child(section("MODELS", cx));
            let mut models = v_flex()
                .gap_0p5()
                .id("ollama-models-list")
                .flex_1()
                .min_h(px(100.))
                .overflow_y_scroll();
            if instance.models.is_empty() {
                models = models.child(
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child("No cached models — use Refresh models."),
                );
            }
            for (i, model) in instance.models.iter().enumerate() {
                models = models.child(self.render_model(i, model, cx));
            }
            right = right
                .child(models)
                .child(
                    h_flex()
                        .gap_2()
                        .child(div().flex_1().child(Input::new(&self.pull).small()))
                        .child(
                            Button::new("ollama-pull")
                                .icon(IconName::ArrowDown)
                                .label("Pull")
                                .small()
                                .disabled(busy)
                                .on_click(cx.listener(|this, _, w, cx| this.pull_model(w, cx))),
                        ),
                )
                .child(section("SETTINGS", cx))
                .child(field("Name", &self.cfg_name))
                .child(field("URL", &self.cfg_url))
                .child(field("Timeout", &self.cfg_timeout))
                .child(field("API key", &self.cfg_key))
                .child(
                    h_flex()
                        .gap_2()
                        .items_center()
                        .child(
                            Switch::new("ollama-enabled")
                                .checked(self.cfg_enabled)
                                .label("Enabled")
                                .on_click(cx.listener(|this, checked: &bool, _, cx| {
                                    this.cfg_enabled = *checked;
                                    cx.notify();
                                })),
                        )
                        .child(div().flex_1())
                        .child(
                            Button::new("ollama-remove")
                                .label("Remove instance")
                                .small()
                                .ghost()
                                .disabled(busy || self.implicit)
                                .on_click(cx.listener(|this, _, w, cx| this.confirm_remove(w, cx))),
                        )
                        .child(
                            Button::new("ollama-save")
                                .label("Save")
                                .small()
                                .primary()
                                .disabled(busy)
                                .on_click(cx.listener(|this, _, w, cx| this.save_config(w, cx))),
                        ),
                );
        }

        // Under the instance area: as tall as its lines, at most 35% of the
        // panel.
        let output = v_flex()
            .id("ollama-output")
            .gap_0p5()
            .flex_shrink_1()
            .min_h_0()
            .max_h(relative(0.35))
            .p_2()
            .rounded_md()
            .border_1()
            .border_color(theme.border)
            .overflow_y_scroll()
            .when_some(self.busy.clone(), |el, label| {
                el.child(
                    h_flex()
                        .gap_2()
                        .items_center()
                        .text_xs()
                        .child(Spinner::new().small())
                        .child(label),
                )
            })
            .children(self.output.iter().map(|line| {
                div()
                    .text_xs()
                    .font_family(theme.mono_font_family.clone())
                    .text_color(if self.output_error {
                        theme.danger
                    } else {
                        theme.foreground
                    })
                    .child(SharedString::from(line.clone()))
            }));

        v_flex()
            .size_full()
            .gap_3()
            .child(
                h_flex()
                    .flex_1()
                    .min_h(px(160.))
                    .gap_4()
                    .items_stretch()
                    .child(left)
                    .child(right),
            )
            .when(busy || !self.output.is_empty(), |el| el.child(output))
    }
}

/// Open the Ollama panel; `on_changed` runs after settings or models changed.
pub fn open(
    engine: &Engine,
    http: &reqwest::Client,
    on_changed: impl Fn(&mut Window, &mut App) + 'static,
    window: &mut Window,
    cx: &mut App,
) {
    let panel = cx.new(|cx| OllamaPanel::new(engine.clone(), http.clone(), on_changed, window, cx));
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(dialog.title("Ollama"), window, 820., panel.clone())
            .on_ok(|_, _, _| false)
    });
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{format_info, format_models, format_running};
    use qt_core::ollama::{OllamaInstance, OllamaModel};
    use serde_json::json;

    #[test]
    fn running_lines() {
        let lines = format_running(
            "gpu",
            &json!({"models":[{"name":"qwen3:8b","size":2048,"size_vram":1024,"context_length":8192,"expires_at":"soon"}]}),
        );
        assert_eq!(lines[0], "Ollama gpu running models:");
        assert_eq!(
            lines[1],
            "- qwen3:8b · 2 KB · VRAM 1 KB · context 8192 · until soon"
        );
        assert_eq!(format_running("x", &json!({}))[1], "- none loaded");
    }

    #[test]
    fn info_lines() {
        let lines = format_info(
            "m",
            &json!({"details":{"family":"qwen3","parameter_size":"8B"},"capabilities":["completion","tools"],
                    "model_info":{"qwen3.context_length":40960},"license":"long"}),
        );
        assert_eq!(
            &lines[..5],
            [
                "Model m",
                "family: qwen3",
                "parameters: 8B",
                "capabilities: completion, tools",
                "context length: 40960"
            ]
        );
        assert!(!lines.iter().any(|l| l.contains("license")));
        assert_eq!(lines[6], "{");
    }

    #[test]
    fn model_lines() {
        let mut instance = OllamaInstance::new("gpu", "http://box:11434");
        assert_eq!(format_models(&instance)[1], "- no models installed");
        instance.models.push(OllamaModel {
            name: "a".into(),
            context_window: 4096,
            max_tokens: 4096,
            ..Default::default()
        });
        let lines = format_models(&instance);
        assert_eq!(
            lines[0],
            "Ollama gpu models (select with model ollama:gpu/<model>):"
        );
        assert_eq!(lines[1], "- a · context 4096 · max output 4096");
    }
}
