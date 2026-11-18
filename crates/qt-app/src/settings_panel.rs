//! Settings panel (archived `/settings` toggles plus harness, theme and
//! scoped-model settings). Every change is written to the global
//! `settings.json` right away.

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::notification::Notification;
use gpui_kit::component::switch::Switch;
use gpui_kit::component::{
    ActiveTheme as _, IconName, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::harness::settings::QueueMode;
use qt_core::{Conversation, Engine, Settings};
use serde_json::{Value, json};

use crate::dialogs::UiCallback;
use crate::model_picker::{enabled_models, in_scope, toggle_scoped};
use crate::theme::{self, ThemeState};

/// A boolean setting: key path (dot-separated), label, help text, default.
pub struct BoolSetting {
    pub key: &'static str,
    pub label: &'static str,
    pub help: &'static str,
    pub default: bool,
}

/// Archived `/settings` toggles that apply to the desktop app.
pub const BOOL_SETTINGS: [BoolSetting; 6] = [
    BoolSetting {
        key: "quietStartup",
        label: "Quiet startup",
        help: "Hide startup hints",
        default: false,
    },
    BoolSetting {
        key: "collapseChangelog",
        label: "Collapse changelog",
        help: "Use compact changelog on startup",
        default: false,
    },
    BoolSetting {
        key: "hideThinkingBlock",
        label: "Hide thinking",
        help: "Prefer compact thinking display",
        default: false,
    },
    BoolSetting {
        key: "enableSkillCommands",
        label: "Skill commands",
        help: "Enable /skill:* slash commands",
        default: true,
    },
    BoolSetting {
        key: "allowExternalFilesystemAccess",
        label: "External file access",
        help: "Allow model-facing native tools to use paths outside the workspace",
        default: false,
    },
    BoolSetting {
        key: "images.autoResize",
        label: "Image auto resize",
        help: "Resize oversized images to 2000×2000 / 4.5 MB before sending",
        default: true,
    },
];

/// Read a (possibly nested, dot-separated) boolean setting.
pub fn get_bool(settings: &Settings, key: &str, default: bool) -> bool {
    let mut parts = key.split('.');
    let Some(first) = parts.next() else {
        return default;
    };
    let mut value = settings.raw().get(first);
    for part in parts {
        value = value.and_then(|v| v.get(part));
    }
    value.and_then(Value::as_bool).unwrap_or(default)
}

/// Write a (possibly nested) boolean setting, replacing non-object parents.
pub fn set_bool(settings: &mut Settings, key: &str, value: bool) {
    let parts: Vec<&str> = key.split('.').collect();
    let (last, parents) = parts.split_last().expect("non-empty key");
    let mut map = settings.raw_mut();
    for part in parents {
        let entry = map.entry(part.to_string()).or_insert_with(|| json!({}));
        if !entry.is_object() {
            *entry = json!({});
        }
        map = entry.as_object_mut().expect("object");
    }
    map.insert(last.to_string(), json!(value));
}

/// Store the theme selection (`None` follows the system appearance).
pub fn set_theme_setting(settings: &mut Settings, theme: Option<&str>) {
    match theme {
        Some(name) => {
            settings.raw_mut().insert("theme".into(), json!(name));
        }
        None => {
            settings.raw_mut().remove("theme");
            if let Some(gui) = settings
                .raw_mut()
                .get_mut("gui")
                .and_then(Value::as_object_mut)
            {
                gui.remove("theme");
            }
        }
    }
}

/// Store `enabledModels` (empty removes the key: every model is in scope).
pub fn set_enabled_models(settings: &mut Settings, models: &[String]) {
    if models.is_empty() {
        settings.raw_mut().remove("enabledModels");
    } else {
        settings
            .raw_mut()
            .insert("enabledModels".into(), json!(models));
    }
}

/// Archived `/settings` choices for `terminal.promptHistoryLimit`.
pub const PROMPT_HISTORY_LIMITS: [usize; 7] = [0, 50, 100, 250, 500, 1000, 5000];

/// Store `terminal.promptHistoryLimit`, replacing a non-object `terminal`.
pub fn set_prompt_history_limit(settings: &mut Settings, limit: usize) {
    let terminal = settings
        .raw_mut()
        .entry("terminal".to_string())
        .or_insert_with(|| json!({}));
    if !terminal.is_object() {
        *terminal = json!({});
    }
    if let Some(map) = terminal.as_object_mut() {
        map.insert("promptHistoryLimit".into(), json!(limit));
    }
}

/// Callbacks into the workspace.
pub struct SettingsHooks {
    /// Settings changed; re-read whatever depends on them.
    pub changed: UiCallback,
    /// Re-discover models from every provider.
    pub refresh_models: UiCallback,
}

pub struct SettingsPanel {
    engine: Engine,
    conversation: Conversation,
    /// `(provider/id, title)` of every discovered model.
    models: Vec<(String, String)>,
    model_filter: Entity<InputState>,
    hooks: SettingsHooks,
    scroll: ScrollHandle,
    _subscriptions: Vec<Subscription>,
}

impl SettingsPanel {
    pub fn new(
        engine: Engine,
        conversation: Conversation,
        models: Vec<(String, String)>,
        hooks: SettingsHooks,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let model_filter = cx.new(|cx| InputState::new(window, cx).placeholder("Filter models"));
        let subscriptions = vec![cx.subscribe(&model_filter, |_, _, e: &InputEvent, cx| {
            if matches!(e, InputEvent::Change) {
                cx.notify();
            }
        })];
        Self {
            engine,
            conversation,
            models,
            model_filter,
            hooks,
            scroll: ScrollHandle::new(),
            _subscriptions: subscriptions,
        }
    }

    /// Show the scoped-models section (`/scoped-models`): scroll to it and
    /// focus its filter.
    pub fn focus_models(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.scroll.scroll_to_bottom();
        self.model_filter
            .update(cx, |state, cx| state.focus(window, cx));
        cx.notify();
    }

    pub fn set_models(&mut self, models: Vec<(String, String)>, cx: &mut Context<Self>) {
        self.models = models;
        cx.notify();
    }

    fn save(
        &mut self,
        change: impl FnOnce(&mut Settings),
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if let Err(err) = self.engine.update_settings(change) {
            window.push_notification(
                Notification::error(format!("Could not save settings: {err:#}")),
                cx,
            );
            return false;
        }
        (self.hooks.changed)(window, cx);
        cx.notify();
        true
    }

    fn set_queue_mode(
        &mut self,
        follow_up: bool,
        mode: QueueMode,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let result = if follow_up {
            self.conversation.set_follow_up_mode(mode)
        } else {
            self.conversation.set_steering_mode(mode)
        };
        if let Err(err) = result {
            window.push_notification(
                Notification::error(format!("Could not save settings: {err:#}")),
                cx,
            );
        }
        (self.hooks.changed)(window, cx);
        cx.notify();
    }

    fn select_theme(&mut self, name: Option<String>, window: &mut Window, cx: &mut Context<Self>) {
        if self.save(|s| set_theme_setting(s, name.as_deref()), window, cx) {
            theme::apply_selection(name.as_deref(), cx);
        }
    }

    fn row(
        &self,
        id: impl Into<ElementId>,
        label: &'static str,
        help: &'static str,
        checked: bool,
        on_change: impl Fn(&mut Self, bool, &mut Window, &mut Context<Self>) + 'static,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let theme = cx.theme().clone();
        h_flex()
            .gap_3()
            .items_center()
            .py_1()
            .child(
                v_flex()
                    .flex_1()
                    .min_w_0()
                    .child(div().text_sm().child(label))
                    .child(
                        div()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child(help),
                    ),
            )
            .child(Switch::new(id).checked(checked).on_click(cx.listener(
                move |this, value: &bool, window, cx| on_change(this, *value, window, cx),
            )))
            .into_any_element()
    }

    fn choice<T: Copy + PartialEq + 'static>(
        &self,
        id: &'static str,
        options: &[(T, &'static str)],
        current: T,
        on_pick: impl Fn(&mut Self, T, &mut Window, &mut Context<Self>) + Clone + 'static,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let mut group = h_flex().flex_wrap().justify_end().gap_1();
        for (i, (value, label)) in options.iter().enumerate() {
            let value = *value;
            let on_pick = on_pick.clone();
            group = group.child(
                Button::new((id, i))
                    .label(*label)
                    .xsmall()
                    .map(|b| {
                        if value == current {
                            b.primary()
                        } else {
                            b.ghost()
                        }
                    })
                    .on_click(
                        cx.listener(move |this, _, window, cx| on_pick(this, value, window, cx)),
                    ),
            );
        }
        group.into_any_element()
    }
}

fn heading(text: &'static str, cx: &App) -> Div {
    div()
        .pt_2()
        .text_xs()
        .font_weight(FontWeight::SEMIBOLD)
        .text_color(cx.theme().muted_foreground)
        .child(text)
}

impl Render for SettingsPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme_colors = cx.theme().clone();
        let settings = self.engine.settings();
        let hs = self.engine.harness_settings();

        // Appearance.
        let selected_theme = ThemeState::global(cx).and_then(|s| s.selected.clone());
        let mut themes = h_flex().gap_1().flex_wrap().child(
            Button::new("theme-system")
                .label("System")
                .xsmall()
                .map(|b| {
                    if selected_theme.is_none() {
                        b.primary()
                    } else {
                        b.ghost()
                    }
                })
                .on_click(cx.listener(|this, _, w, cx| this.select_theme(None, w, cx))),
        );
        for (i, name) in ThemeState::available(cx).into_iter().enumerate() {
            let active = selected_theme
                .as_deref()
                .is_some_and(|s| s.eq_ignore_ascii_case(&name));
            let pick = name.clone();
            themes = themes.child(
                Button::new(("theme", i))
                    .label(name)
                    .xsmall()
                    .map(|b| if active { b.primary() } else { b.ghost() })
                    .on_click(cx.listener(move |this, _, w, cx| {
                        this.select_theme(Some(pick.clone()), w, cx)
                    })),
            );
        }
        let theme_diagnostics: Vec<String> = ThemeState::global(cx)
            .map(|s| s.resources.diagnostics.clone())
            .unwrap_or_default();

        let mut body = v_flex()
            .id("settings-body")
            .size_full()
            .gap_1()
            .overflow_y_scroll()
            .track_scroll(&self.scroll)
            .pr_2()
            .child(heading("APPEARANCE", cx))
            .child(
                h_flex()
                    .gap_3()
                    .items_start()
                    .py_1()
                    .child(
                        v_flex()
                            .w(px(180.))
                            .child(div().text_sm().child("Theme"))
                            .child(
                                div()
                                    .text_xs()
                                    .text_color(theme_colors.muted_foreground)
                                    .child("JSON themes reload when their file changes"),
                            ),
                    )
                    .child(div().flex_1().min_w_0().child(themes))
                    .child(
                        Button::new("theme-reload")
                            .icon(IconName::RefreshCw)
                            .xsmall()
                            .ghost()
                            .tooltip("Re-discover theme files")
                            .on_click(cx.listener(|this, _, _, cx| {
                                theme::rediscover(&this.engine, cx);
                                cx.notify();
                            })),
                    ),
            )
            .children(theme_diagnostics.into_iter().map(|d| {
                div()
                    .text_xs()
                    .text_color(theme_colors.warning)
                    .child(SharedString::from(d))
            }))
            .child(heading("GENERAL", cx));

        for (i, setting) in BOOL_SETTINGS.iter().enumerate() {
            let key = setting.key;
            body = body.child(self.row(
                ("setting", i),
                setting.label,
                setting.help,
                get_bool(&settings, key, setting.default),
                move |this, value, window, cx| {
                    this.save(|s| set_bool(s, key, value), window, cx);
                },
                cx,
            ));
        }

        // Prompt history (archived `prompt_history_limit`). The workspace
        // reloads its history on `changed`, which compacts `history.jsonl`
        // to the new limit (0 deletes it).
        let limits: Vec<(usize, &'static str)> = PROMPT_HISTORY_LIMITS
            .iter()
            .zip(["0", "50", "100", "250", "500", "1000", "5000"])
            .map(|(n, label)| (*n, label))
            .collect();
        let history_limit = self.choice(
            "prompt-history-limit",
            &limits,
            settings.prompt_history_limit(),
            |this, limit, window, cx| {
                this.save(|s| set_prompt_history_limit(s, limit), window, cx);
            },
            cx,
        );
        body = body.child(
            h_flex()
                .gap_3()
                .items_center()
                .py_1()
                .child(
                    v_flex()
                        .flex_1()
                        .child(div().text_sm().child("Prompt history"))
                        .child(
                            div()
                                .text_xs()
                                .text_color(theme_colors.muted_foreground)
                                .child("Maximum prompts retained in the global qt history"),
                        ),
                )
                .child(history_limit),
        );

        body = body
            .child(heading("AGENT", cx))
            .child(self.row(
                "auto-compaction",
                "Auto-compaction",
                "Compact the context when it nears the model's window",
                hs.compaction.enabled,
                |this, value, window, cx| {
                    this.save(
                        |s| qt_core::harness::settings::set_compaction_enabled(s, value),
                        window,
                        cx,
                    );
                },
                cx,
            ))
            .child(self.row(
                "auto-retry",
                "Auto-retry",
                "Retry transient provider errors with exponential backoff",
                hs.retry.enabled,
                |this, value, window, cx| {
                    this.save(
                        |s| qt_core::harness::settings::set_retry_enabled(s, value),
                        window,
                        cx,
                    );
                },
                cx,
            ));
        let modes = [
            (QueueMode::OneAtATime, "One at a time"),
            (QueueMode::All, "All"),
        ];
        let steering = self.choice(
            "steering-mode",
            &modes,
            hs.steering_mode,
            |this, mode, w, cx| this.set_queue_mode(false, mode, w, cx),
            cx,
        );
        let follow = self.choice(
            "follow-mode",
            &modes,
            hs.follow_up_mode,
            |this, mode, w, cx| this.set_queue_mode(true, mode, w, cx),
            cx,
        );
        body = body
            .child(
                h_flex()
                    .gap_3()
                    .items_center()
                    .py_1()
                    .child(
                        v_flex()
                            .flex_1()
                            .child(div().text_sm().child("Steering mode"))
                            .child(
                                div()
                                    .text_xs()
                                    .text_color(theme_colors.muted_foreground)
                                    .child("Queued steering messages delivered per turn (Enter while running)"),
                            ),
                    )
                    .child(steering),
            )
            .child(
                h_flex()
                    .gap_3()
                    .items_center()
                    .py_1()
                    .child(
                        v_flex()
                            .flex_1()
                            .child(div().text_sm().child("Follow-up mode"))
                            .child(
                                div()
                                    .text_xs()
                                    .text_color(theme_colors.muted_foreground)
                                    .child("Queued follow-ups delivered when the agent stops (Alt+Enter)"),
                            ),
                    )
                    .child(follow),
            );

        // Scoped models.
        let enabled = enabled_models(&settings);
        let filter = self.model_filter.read(cx).value().to_lowercase();
        let available: Vec<String> = self.models.iter().map(|(spec, _)| spec.clone()).collect();
        let refresh = self.hooks.refresh_models.clone();
        body = body
            .child(heading("MODELS", cx))
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(
                        div()
                            .flex_1()
                            .text_xs()
                            .text_color(theme_colors.muted_foreground)
                            .child(if enabled.is_empty() {
                                "Scoped models: all models are shown in the model picker."
                                    .to_string()
                            } else {
                                format!("Scoped models: {} enabled (enabledModels).", enabled.len())
                            }),
                    )
                    .child(
                        Button::new("models-refresh")
                            .icon(IconName::RefreshCw)
                            .label("Refresh model list")
                            .xsmall()
                            .on_click(move |_, window, cx| refresh(window, cx)),
                    )
                    .when(!enabled.is_empty(), |el| {
                        el.child(
                            Button::new("models-all")
                                .label("Show all")
                                .xsmall()
                                .ghost()
                                .on_click(cx.listener(|this, _, window, cx| {
                                    this.save(|s| set_enabled_models(s, &[]), window, cx);
                                })),
                        )
                    }),
            )
            .child(Input::new(&self.model_filter).small());
        let mut list = v_flex().gap_0p5();
        let shown: Vec<&(String, String)> = self
            .models
            .iter()
            .filter(|(spec, title)| {
                filter
                    .split_whitespace()
                    .all(|w| spec.to_lowercase().contains(w) || title.to_lowercase().contains(w))
            })
            .collect();
        if shown.is_empty() {
            list = list.child(
                div()
                    .text_xs()
                    .text_color(theme_colors.muted_foreground)
                    .child("No models discovered yet."),
            );
        }
        for (i, (spec, title)) in shown.into_iter().enumerate() {
            let checked = in_scope(&enabled, spec);
            let spec = spec.clone();
            let enabled = enabled.clone();
            let available = available.clone();
            list = list.child(
                Checkbox::new(("scoped", i))
                    .checked(checked)
                    .label(title.clone())
                    .on_click(cx.listener(move |this, value: &bool, window, cx| {
                        let next = toggle_scoped(&enabled, &available, &spec, *value);
                        this.save(|s| set_enabled_models(s, &next), window, cx);
                    })),
            );
        }
        body.child(list)
    }
}

/// Open the settings panel.
pub fn open(
    engine: &Engine,
    conversation: &Conversation,
    models: Vec<(String, String)>,
    hooks: SettingsHooks,
    window: &mut Window,
    cx: &mut App,
) -> Entity<SettingsPanel> {
    let panel = cx.new(|cx| {
        SettingsPanel::new(
            engine.clone(),
            conversation.clone(),
            models,
            hooks,
            window,
            cx,
        )
    });
    let view = panel.clone();
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(dialog.title("Settings"), window, 640., view.clone())
            .on_ok(|_, _, _| false)
    });
    panel
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{
        BOOL_SETTINGS, PROMPT_HISTORY_LIMITS, get_bool, set_bool, set_enabled_models,
        set_prompt_history_limit, set_theme_setting,
    };
    use qt_core::Settings;
    use serde_json::json;

    #[test]
    fn nested_booleans_round_trip() {
        let mut s = Settings::default();
        assert!(get_bool(&s, "images.autoResize", true));
        set_bool(&mut s, "images.autoResize", false);
        assert_eq!(s.raw()["images"], json!({"autoResize": false}));
        assert!(!get_bool(&s, "images.autoResize", true));
        assert!(!s.image_auto_resize());
        s.raw_mut().insert("images".into(), json!(3));
        set_bool(&mut s, "images.autoResize", true);
        assert_eq!(s.raw()["images"], json!({"autoResize": true}));
        set_bool(&mut s, "hideThinkingBlock", true);
        assert!(get_bool(&s, "hideThinkingBlock", false));
        assert_eq!(BOOL_SETTINGS.len(), 6);
    }

    #[test]
    fn prompt_history_limit_setting() {
        let mut s = Settings::default();
        assert_eq!(s.prompt_history_limit(), 500);
        assert!(PROMPT_HISTORY_LIMITS.contains(&500));
        s.raw_mut().insert("terminal".into(), json!("bogus"));
        set_prompt_history_limit(&mut s, 50);
        assert_eq!(s.raw()["terminal"], json!({"promptHistoryLimit": 50}));
        s.raw_mut()
            .insert("terminal".into(), json!({"showProgress": true}));
        set_prompt_history_limit(&mut s, 0);
        assert_eq!(
            s.raw()["terminal"],
            json!({"showProgress": true, "promptHistoryLimit": 0})
        );
        assert_eq!(s.prompt_history_limit(), 0);
    }

    #[test]
    fn theme_and_scope_settings() {
        let mut s = Settings::default();
        s.set_theme("dark");
        set_theme_setting(&mut s, Some("nord"));
        assert_eq!(s.raw()["theme"], "nord");
        set_theme_setting(&mut s, None);
        assert!(s.raw().get("theme").is_none());
        assert!(s.theme().is_none());
        set_enabled_models(&mut s, &["a/b".into()]);
        assert_eq!(s.raw()["enabledModels"], json!(["a/b"]));
        set_enabled_models(&mut s, &[]);
        assert!(s.raw().get("enabledModels").is_none());
    }
}
