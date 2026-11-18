//! Memory settings editor (archived `/memory settings`): every `memory.*`
//! setting with toggles, choices, validated numbers and model pickers (the
//! verifier picker lists local llama.cpp models only). Each change is
//! validated and written to global settings at once; changes apply to the
//! next runtime.

use std::collections::HashMap;

use gpui_kit::component::button::Button;
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::switch::Switch;
use gpui_kit::component::{ActiveTheme as _, Sizable as _, WindowExt as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_memory::settings::MemorySettings;
use qt_memory::views::settings_editor::{
    SAVED_NOTICE, SettingKind, SettingSpec, apply_setting, global_memory_settings, model_choices,
    model_picker_heading, setting_specs, setting_value, write_memory_settings,
};

use super::MemoryHost;
use super::widgets::{chip, hint};

pub struct MemorySettingsEditor {
    host: MemoryHost,
    specs: Vec<SettingSpec>,
    settings: MemorySettings,
    load_error: Option<String>,
    error: Option<String>,
    changed: bool,
    numbers: HashMap<&'static str, Entity<InputState>>,
    picker: Option<&'static str>,
    filter: Entity<InputState>,
    _subscriptions: Vec<Subscription>,
}

impl MemorySettingsEditor {
    fn new(host: MemoryHost, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let (settings, load_error) =
            global_memory_settings(host.runtime.engine().global_settings().raw());
        let specs = setting_specs();
        let mut numbers = HashMap::new();
        let mut subscriptions = Vec::new();
        for spec in &specs {
            if matches!(spec.kind, SettingKind::Number { .. }) {
                let id = spec.id;
                let input = cx.new(|cx| InputState::new(window, cx));
                let value = setting_value(&settings, id);
                input.update(cx, |s, cx| s.set_value(value, window, cx));
                subscriptions.push(cx.subscribe_in(
                    &input,
                    window,
                    move |this, input, event: &InputEvent, window, cx| {
                        if let InputEvent::PressEnter { .. } = event {
                            let value = input.read(cx).value().to_string();
                            this.change(id, &value, window, cx);
                        }
                    },
                ));
                numbers.insert(id, input);
            }
        }
        let filter = cx.new(|cx| InputState::new(window, cx).placeholder("Type to filter models"));
        subscriptions.push(cx.subscribe(&filter, |_, _, event: &InputEvent, cx| {
            if matches!(event, InputEvent::Change) {
                cx.notify();
            }
        }));
        // Archived: closing the editor after a change reports the notice.
        subscriptions.push(cx.on_release(|this: &mut Self, cx| this.closed(cx)));
        Self {
            host,
            specs,
            settings,
            load_error,
            error: None,
            changed: false,
            numbers,
            picker: None,
            filter,
            _subscriptions: subscriptions,
        }
    }

    /// Validates and persists one change (archived change callback).
    fn change(
        &mut self,
        id: &'static str,
        value: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let next = match apply_setting(&self.settings, id, value.trim()) {
            Ok(next) => next,
            Err(message) => {
                self.fail(id, message, window, cx);
                return;
            }
        };
        let saved = self
            .host
            .runtime
            .engine()
            .update_settings(|settings| write_memory_settings(settings.raw_mut(), &next));
        if let Err(error) = saved {
            self.fail(id, format!("error: {error:#}"), window, cx);
            return;
        }
        self.settings = next;
        self.changed = true;
        self.error = None;
        self.picker = None;
        if let Some(input) = self.numbers.get(id) {
            let shown = setting_value(&self.settings, id);
            input.update(cx, |s, cx| s.set_value(shown, window, cx));
        }
        cx.notify();
    }

    /// Restores the previous value and reports the error (archived: the
    /// transcript line).
    fn fail(
        &mut self,
        id: &'static str,
        message: String,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if let Some(input) = self.numbers.get(id) {
            let shown = setting_value(&self.settings, id);
            input.update(cx, |s, cx| s.set_value(shown, window, cx));
        }
        self.host
            .card("/memory settings", vec![message.clone()], true, cx);
        self.error = Some(message);
        cx.notify();
    }

    fn closed(&self, cx: &mut App) {
        if self.changed {
            self.host
                .card("/memory settings", vec![SAVED_NOTICE.into()], false, cx);
        }
    }

    fn render_control(&self, spec: &SettingSpec, cx: &mut Context<Self>) -> AnyElement {
        let id = spec.id;
        let current = setting_value(&self.settings, id);
        match &spec.kind {
            SettingKind::Toggle => {
                Switch::new(SharedString::from(format!("memory-setting-toggle-{id}")))
                    .checked(current == "on")
                    .on_click(cx.listener(move |this, checked: &bool, window, cx| {
                        this.change(id, if *checked { "on" } else { "off" }, window, cx)
                    }))
                    .into_any_element()
            }
            SettingKind::Choice(choices) => {
                let mut row = h_flex().gap_1().flex_wrap();
                for (i, choice) in choices.iter().enumerate() {
                    let choice: &'static str = choice;
                    row = row.child(
                        chip(
                            SharedString::from(format!("memory-setting-{id}-{i}")),
                            choice,
                            current == choice,
                        )
                        .on_click(cx.listener(
                            move |this, _, window, cx| this.change(id, choice, window, cx),
                        )),
                    );
                }
                row.into_any_element()
            }
            SettingKind::Number { .. } => {
                let input = self.numbers.get(id).cloned();
                h_flex()
                    .gap_1()
                    .when_some(input.clone(), |el, input| {
                        el.child(div().w(px(160.)).child(Input::new(&input).small()))
                    })
                    .child(
                        Button::new(SharedString::from(format!("memory-setting-set-{id}")))
                            .label("Set")
                            .xsmall()
                            .on_click(cx.listener(move |this, _, window, cx| {
                                if let Some(input) = this.numbers.get(id).cloned() {
                                    let value = input.read(cx).value().to_string();
                                    this.change(id, &value, window, cx);
                                }
                            })),
                    )
                    .into_any_element()
            }
            SettingKind::Model { .. } => {
                Button::new(SharedString::from(format!("memory-setting-model-{id}")))
                    .label(current)
                    .xsmall()
                    .outline()
                    .dropdown_caret(true)
                    .on_click(cx.listener(move |this, _, window, cx| {
                        this.picker = if this.picker == Some(id) {
                            None
                        } else {
                            Some(id)
                        };
                        this.filter.update(cx, |s, cx| s.set_value("", window, cx));
                        cx.notify();
                    }))
                    .into_any_element()
            }
        }
    }

    fn render_picker(&self, spec: &SettingSpec, cx: &mut Context<Self>) -> Div {
        let theme = cx.theme().clone();
        let id = spec.id;
        let current = setting_value(&self.settings, id);
        let choices = model_choices(spec, &current, &self.host.model_choices());
        let filter = self.filter.read(cx).value().to_ascii_lowercase();
        let mut list = v_flex()
            .id(SharedString::from(format!("memory-picker-{id}")))
            .gap_0p5()
            .max_h(px(320.))
            .overflow_y_scroll();
        for (i, choice) in choices
            .iter()
            .filter(|choice| {
                filter.is_empty()
                    || choice.value.to_ascii_lowercase().contains(&filter)
                    || choice.description.to_ascii_lowercase().contains(&filter)
            })
            .enumerate()
        {
            let value = choice.value.clone();
            list = list.child(
                super::widgets::list_row(
                    SharedString::from(format!("memory-picker-{id}-{i}")),
                    choice.title.clone(),
                    choice.description.clone(),
                    choice.value == current,
                    cx,
                )
                .on_click(
                    cx.listener(move |this, _, window, cx| this.change(id, &value, window, cx)),
                ),
            );
        }
        v_flex()
            .gap_1()
            .p_2()
            .rounded_md()
            .border_1()
            .border_color(theme.border)
            .child(
                div()
                    .text_xs()
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(model_picker_heading(spec, &choices)),
            )
            .child(Input::new(&self.filter).small())
            .child(list)
    }
}

impl Render for MemorySettingsEditor {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let mut rows = v_flex()
            .id("memory-settings-rows")
            .gap_2()
            .flex_1()
            .min_h(px(120.))
            .overflow_y_scroll()
            .pr_2();
        let specs = self.specs.clone();
        for spec in &specs {
            let control = self.render_control(spec, cx);
            rows = rows.child(
                v_flex()
                    .gap_1()
                    .pb_2()
                    .border_b_1()
                    .border_color(theme.border.opacity(0.5))
                    .child(
                        h_flex()
                            .gap_3()
                            .items_center()
                            .child(
                                v_flex()
                                    .flex_1()
                                    .min_w_0()
                                    .child(div().text_sm().child(spec.label))
                                    .child(hint(
                                        format!("{} · {}", spec.description, spec.key),
                                        cx,
                                    )),
                            )
                            .child(div().flex_none().child(control)),
                    )
                    .when(self.picker == Some(spec.id), |el| {
                        el.child(self.render_picker(spec, cx))
                    }),
            );
        }
        v_flex()
            .size_full()
            .gap_2()
            .child(hint(
                "Settings are written to global settings and apply on the next qt start; a project settings file may only narrow them.",
                cx,
            ))
            .when_some(self.load_error.clone(), |el, error| {
                el.child(
                    div()
                        .text_xs()
                        .text_color(theme.warning)
                        .child(format!("Global memory settings are invalid ({error}); showing defaults.")),
                )
            })
            .when_some(self.error.clone(), |el, error| {
                el.child(div().text_xs().text_color(theme.danger).child(error))
            })
            .when(self.changed, |el| {
                el.child(div().text_xs().text_color(theme.success).child(SAVED_NOTICE))
            })
            .child(rows)
    }
}

/// Opens the memory settings editor.
pub fn open(host: MemoryHost, window: &mut Window, cx: &mut App) {
    let editor = cx.new(|cx| MemorySettingsEditor::new(host, window, cx));
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(
            dialog.title("Memory settings"),
            window,
            760.,
            editor.clone(),
        )
        .on_ok(|_, _, _| false)
    });
}
