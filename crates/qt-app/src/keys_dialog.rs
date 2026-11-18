//! API key management dialog.

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::*;
use qt_core::Engine;
use qt_core::config::env_var_for;
use qt_core::provider::KEYED_PROVIDERS;

/// Masked inputs for each keyed provider; created once and reused.
pub struct KeyInputs {
    inputs: Vec<(&'static str, &'static str, Entity<InputState>)>,
}

impl KeyInputs {
    pub fn new(window: &mut Window, cx: &mut App) -> Self {
        let inputs = KEYED_PROVIDERS
            .iter()
            .map(|(id, label)| {
                let input = cx.new(|cx| InputState::new(window, cx).masked(true));
                (*id, *label, input)
            })
            .collect();
        Self { inputs }
    }

    /// Open the dialog; `on_saved` runs after keys were written.
    pub fn open(
        &self,
        engine: &Engine,
        on_saved: impl Fn(&mut Window, &mut App) + 'static,
        window: &mut Window,
        cx: &mut App,
    ) {
        for (id, _, input) in &self.inputs {
            let placeholder = if engine.has_stored_api_key(id) {
                "Saved — enter a new key to replace it".to_string()
            } else if let Some(var) =
                env_var_for(id).filter(|v| std::env::var(v).is_ok_and(|s| !s.is_empty()))
            {
                format!("Using ${var} — enter a key to store one")
            } else {
                "Not set".to_string()
            };
            input.update(cx, |state, cx| {
                state.set_value("", window, cx);
                state.set_placeholder(placeholder, window, cx);
            });
        }
        let inputs = self.inputs.clone();
        let engine = engine.clone();
        let on_saved = std::rc::Rc::new(on_saved);
        window.open_dialog(cx, move |dialog, _window, cx| {
            let theme = cx.theme().clone();
            let mut rows = v_flex().gap_3();
            for (id, label, input) in &inputs {
                let engine_for_clear = engine.clone();
                let provider = *id;
                let input_for_clear = input.clone();
                let on_saved_clear = on_saved.clone();
                rows = rows.child(
                    v_flex()
                        .gap_1()
                        .child(div().text_sm().font_weight(FontWeight::MEDIUM).child(*label))
                        .child(
                            h_flex()
                                .gap_2()
                                .child(div().flex_1().child(Input::new(input)))
                                .child(
                                    Button::new(SharedString::from(format!("clear-{provider}")))
                                        .label("Remove")
                                        .small()
                                        .ghost()
                                        .disabled(!engine.has_stored_api_key(provider))
                                        .on_click(move |_, window, cx| {
                                            if let Err(err) = engine_for_clear.set_api_key(provider, "") {
                                                window.push_notification(
                                                    component::notification::Notification::error(format!("{err:#}")),
                                                    cx,
                                                );
                                                return;
                                            }
                                            input_for_clear.update(cx, |s, cx| s.set_placeholder("Not set", window, cx));
                                            on_saved_clear(window, cx);
                                        }),
                                ),
                        ),
                );
            }
            let save_inputs = inputs.clone();
            let save_engine = engine.clone();
            let save_cb = on_saved.clone();
            dialog
                .title("API keys")
                .w(px(560.))
                .child(
                    v_flex()
                        .gap_4()
                        .child(
                            div()
                                .text_sm()
                                .text_color(theme.muted_foreground)
                                .child("Keys are stored in auth.json in the qt home directory (owner-only permissions). Ollama needs no key."),
                        )
                        .child(rows),
                )
                .footer(
                    h_flex()
                        .w_full()
                        .justify_end()
                        .gap_2()
                        .child(Button::new("keys-cancel").label("Cancel").on_click(|_, window, cx| window.close_dialog(cx)))
                        .child(Button::new("keys-save").label("Save").primary().on_click(move |_, window, cx| {
                            for (id, _, input) in &save_inputs {
                                let value = input.read(cx).value().to_string();
                                if value.trim().is_empty() {
                                    continue;
                                }
                                if let Err(err) = save_engine.set_api_key(id, &value) {
                                    window.push_notification(
                                        component::notification::Notification::error(format!("{err:#}")),
                                        cx,
                                    );
                                    return;
                                }
                            }
                            window.close_dialog(cx);
                            save_cb(window, cx);
                        })),
                )
        });
    }
}
