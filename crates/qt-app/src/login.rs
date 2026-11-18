//! Login / logout: OAuth subscriptions (Claude Pro/Max, ChatGPT Codex) and
//! provider API keys, with credential status (archived `/login`, `/logout`).

use std::rc::Rc;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_core::Engine;
use qt_core::auth::oauth::{AuthInfo, LoginOptions, OAUTH_PROVIDERS};
use qt_core::auth::{AuthStore, Credential, find_env_vars, login_and_store, logout};
use tokio_util::sync::CancellationToken;

/// Credential status shown next to a provider (archived
/// `auth_status_description`).
pub fn auth_status(
    credential: Option<&Credential>,
    env_vars: &[&str],
    has_config_key: bool,
    subscription_row: bool,
) -> String {
    match credential {
        Some(Credential::OAuth(_)) => return "subscription configured".into(),
        Some(Credential::ApiKey(_)) => return "API key configured".into(),
        None => {}
    }
    if subscription_row {
        return "unconfigured".into();
    }
    if let Some(var) = env_vars.first() {
        return format!("env: {var}");
    }
    if has_config_key {
        return "key in models.json".into();
    }
    "unconfigured".into()
}

/// Provider ids that only support OAuth (no API key entry).
fn oauth_only(id: &str) -> bool {
    id == "openai-codex"
}

enum OAuthUpdate {
    Auth(AuthInfo),
    Progress(String),
}

struct OAuthFlow {
    name: String,
    url: Option<String>,
    instructions: Option<String>,
    progress: Option<String>,
    code: Entity<InputState>,
    manual: async_channel::Sender<String>,
    cancel: CancellationToken,
    _tasks: Vec<Task<()>>,
}

impl Drop for OAuthFlow {
    fn drop(&mut self) {
        self.cancel.cancel();
    }
}

struct ProviderRow {
    id: String,
    name: String,
    status: String,
    stored_key: bool,
}

pub struct LoginPanel {
    engine: Engine,
    http: reqwest::Client,
    search: Entity<InputState>,
    key: Entity<InputState>,
    expanded: Option<String>,
    flow: Option<OAuthFlow>,
    message: Option<(bool, String)>,
    on_changed: Rc<dyn Fn(&mut Window, &mut App)>,
    _subscriptions: Vec<Subscription>,
}

impl LoginPanel {
    pub fn new(
        engine: Engine,
        http: reqwest::Client,
        on_changed: impl Fn(&mut Window, &mut App) + 'static,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let search = cx.new(|cx| InputState::new(window, cx).placeholder("Filter providers"));
        let key = cx.new(|cx| {
            InputState::new(window, cx)
                .masked(true)
                .placeholder("Enter API key")
        });
        let subscriptions = vec![
            cx.subscribe(&search, |_, _, event: &InputEvent, cx| {
                if matches!(event, InputEvent::Change) {
                    cx.notify();
                }
            }),
            cx.subscribe_in(&key, window, |this, _, event: &InputEvent, window, cx| {
                if let InputEvent::PressEnter { .. } = event
                    && let Some(id) = this.expanded.clone()
                {
                    this.save_key(&id, window, cx);
                }
            }),
        ];
        Self {
            engine,
            http,
            search,
            key,
            expanded: None,
            flow: None,
            message: None,
            on_changed: Rc::new(on_changed),
            _subscriptions: subscriptions,
        }
    }

    fn store(&self) -> AuthStore {
        AuthStore::load(self.engine.home()).unwrap_or_default()
    }

    fn auth_path(&self) -> String {
        self.engine.home().auth_path().display().to_string()
    }

    fn changed(&mut self, message: String, window: &mut Window, cx: &mut Context<Self>) {
        if let Err(err) = self.engine.reload_auth() {
            self.message = Some((true, format!("{err:#}")));
        } else {
            self.message = Some((false, message));
        }
        (self.on_changed)(window, cx);
        cx.notify();
    }

    fn fail(&mut self, message: String, cx: &mut Context<Self>) {
        self.message = Some((true, message));
        cx.notify();
    }

    fn save_key(&mut self, provider: &str, window: &mut Window, cx: &mut Context<Self>) {
        let secret = self.key.read(cx).value().trim().to_string();
        if secret.is_empty() {
            self.fail("API key cannot be empty".into(), cx);
            return;
        }
        if let Err(err) = self.engine.set_api_key(provider, &secret) {
            self.fail(format!("{err:#}"), cx);
            return;
        }
        let name = self.provider_name(provider);
        self.key.update(cx, |s, cx| s.set_value("", window, cx));
        self.expanded = None;
        let message = format!(
            "Saved API key for {name}. Credentials saved to {}",
            self.auth_path()
        );
        self.changed(message, window, cx);
    }

    fn remove_key(&mut self, provider: &str, window: &mut Window, cx: &mut Context<Self>) {
        if let Err(err) = self.engine.set_api_key(provider, "") {
            self.fail(format!("{err:#}"), cx);
            return;
        }
        let name = self.provider_name(provider);
        self.changed(
            format!(
                "Removed stored API key for {name}. Environment variables and models.json config are unchanged."
            ),
            window,
            cx,
        );
    }

    fn logout_subscription(&mut self, provider: &str, window: &mut Window, cx: &mut Context<Self>) {
        match logout(self.engine.home(), provider) {
            Ok(_) => {
                let name = subscription_name(provider);
                self.changed(format!("Logged out of {name}"), window, cx);
            }
            Err(err) => self.fail(format!("{err:#}"), cx),
        }
    }

    fn provider_name(&self, id: &str) -> String {
        self.engine
            .provider(id)
            .map(|p| p.label)
            .unwrap_or_else(|| id.to_string())
    }

    fn start_oauth(&mut self, provider: &str, window: &mut Window, cx: &mut Context<Self>) {
        let name = subscription_name(provider);
        let (update_tx, update_rx) = async_channel::unbounded::<OAuthUpdate>();
        let (manual_tx, manual_rx) = async_channel::unbounded::<String>();
        let cancel = CancellationToken::new();
        let auth_tx = update_tx.clone();
        let mut options = LoginOptions::new(move |info| {
            let _ = auth_tx.try_send(OAuthUpdate::Auth(info));
        });
        options.on_progress = Some(Box::new(move |m: &str| {
            let _ = update_tx.try_send(OAuthUpdate::Progress(m.to_string()));
        }));
        options.manual_input = Some(manual_rx);
        options.cancel = cancel.clone();
        let http = self.http.clone();
        let home = self.engine.home().clone();
        let id = provider.to_string();
        let handle = self
            .engine
            .spawn(async move { login_and_store(&http, &home, &id, options).await });

        let updates = cx.spawn_in(window, async move |this, cx| {
            while let Ok(update) = update_rx.recv().await {
                let ok = this.update_in(cx, |this, _window, cx| {
                    let Some(flow) = this.flow.as_mut() else {
                        return;
                    };
                    match update {
                        OAuthUpdate::Auth(info) => {
                            cx.open_url(&info.url);
                            flow.url = Some(info.url);
                            flow.instructions = Some(info.instructions);
                        }
                        OAuthUpdate::Progress(m) => flow.progress = Some(m),
                    }
                    cx.notify();
                });
                if ok.is_err() {
                    break;
                }
            }
        });
        let provider_id = provider.to_string();
        let done = cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update_in(cx, |this, window, cx| {
                let cancelled = this.flow.as_ref().is_none_or(|f| f.cancel.is_cancelled());
                this.flow = None;
                match result {
                    Ok(Ok(_)) => {
                        let name = subscription_name(&provider_id);
                        let message = format!(
                            "Saved subscription login for {name}. Credentials saved to {}",
                            this.auth_path()
                        );
                        this.changed(message, window, cx);
                    }
                    Ok(Err(_)) if cancelled => {
                        this.message = Some((false, "Login cancelled".into()));
                        cx.notify();
                    }
                    Ok(Err(err)) => this.fail(format!("Login failed: {err:#}"), cx),
                    Err(err) => this.fail(format!("Login failed: {err}"), cx),
                }
            });
        });
        let code = cx.new(|cx| {
            InputState::new(window, cx).placeholder("Paste the authorization code or redirect URL")
        });
        self.message = None;
        self.flow = Some(OAuthFlow {
            name,
            url: None,
            instructions: None,
            progress: None,
            code,
            manual: manual_tx,
            cancel,
            _tasks: vec![updates, done],
        });
        cx.notify();
    }

    fn submit_code(&mut self, cx: &mut Context<Self>) {
        let Some(flow) = self.flow.as_mut() else {
            return;
        };
        let code = flow.code.read(cx).value().trim().to_string();
        if code.is_empty() {
            return;
        }
        let _ = flow.manual.try_send(code);
        flow.progress = Some("Checking the pasted code…".into());
        cx.notify();
    }

    fn cancel_oauth(&mut self, cx: &mut Context<Self>) {
        if let Some(flow) = &self.flow {
            flow.cancel.cancel();
        }
        cx.notify();
    }

    /// Stop a running login (the dialog was closed).
    pub fn shutdown(&mut self) {
        if let Some(flow) = &self.flow {
            flow.cancel.cancel();
        }
    }

    fn rows(&self, store: &AuthStore, query: &str) -> Vec<ProviderRow> {
        let query = query.trim().to_lowercase();
        self.engine
            .providers()
            .into_iter()
            .filter(|p| p.needs_api_key() && !oauth_only(&p.id))
            .filter(|p| {
                query.is_empty()
                    || p.label.to_lowercase().contains(&query)
                    || p.id.to_lowercase().contains(&query)
            })
            .map(|p| {
                let credential = store.credential(&p.id);
                let env = find_env_vars(&p.id);
                let status = auth_status(credential.as_ref(), &env, p.api_key.is_some(), false);
                ProviderRow {
                    stored_key: matches!(credential, Some(Credential::ApiKey(_))),
                    id: p.id,
                    name: p.label,
                    status,
                }
            })
            .collect()
    }

    fn render_flow(&self, flow: &OAuthFlow, cx: &mut Context<Self>) -> AnyElement {
        let theme = cx.theme().clone();
        let url = flow.url.clone();
        v_flex()
            .gap_3()
            .child(
                div()
                    .text_sm()
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(format!("Login to {}", flow.name)),
            )
            .child(
                div().text_sm().text_color(theme.muted_foreground).child(
                    flow.instructions
                        .clone()
                        .unwrap_or_else(|| "Starting the login…".into()),
                ),
            )
            .when_some(url, |el, url| {
                let open_url = url.clone();
                let copy_url = url.clone();
                el.child(
                    div()
                        .p_2()
                        .rounded_md()
                        .bg(theme.muted)
                        .text_xs()
                        .font_family(theme.mono_font_family.clone())
                        .child(qt_core::util::shorten(&url, 300)),
                )
                .child(
                    h_flex()
                        .gap_2()
                        .child(
                            Button::new("oauth-open")
                                .icon(IconName::ExternalLink)
                                .label("Open browser again")
                                .small()
                                .on_click(move |_, _, cx| cx.open_url(&open_url)),
                        )
                        .child(
                            Button::new("oauth-copy")
                                .icon(IconName::Copy)
                                .label("Copy URL")
                                .small()
                                .ghost()
                                .on_click(move |_, _, cx| {
                                    cx.write_to_clipboard(ClipboardItem::new_string(
                                        copy_url.clone(),
                                    ))
                                }),
                        ),
                )
            })
            .child(
                h_flex()
                    .gap_2()
                    .child(div().flex_1().child(Input::new(&flow.code).small()))
                    .child(
                        Button::new("oauth-submit")
                            .label("Submit code")
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| this.submit_code(cx))),
                    ),
            )
            .when_some(flow.progress.clone(), |el, p| {
                el.child(div().text_xs().text_color(theme.muted_foreground).child(p))
            })
            .child(
                h_flex().justify_end().child(
                    Button::new("oauth-cancel")
                        .label("Cancel login")
                        .small()
                        .ghost()
                        .on_click(cx.listener(|this, _, _, cx| this.cancel_oauth(cx))),
                ),
            )
            .into_any_element()
    }
}

pub fn subscription_name(provider: &str) -> String {
    OAUTH_PROVIDERS
        .iter()
        .find(|p| p.id == provider)
        .map(|p| p.name.to_string())
        .unwrap_or_else(|| provider.to_string())
}

impl Render for LoginPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let store = self.store();
        if let Some(flow) = &self.flow {
            let flow_view = self.render_flow(flow, cx);
            return v_flex().gap_3().child(flow_view).into_any_element();
        }

        let mut subs = v_flex().gap_1();
        for info in OAUTH_PROVIDERS {
            let credential = store.credential(info.id);
            let status = auth_status(credential.as_ref(), &[], false, true);
            let logged_in = matches!(credential, Some(Credential::OAuth(_)));
            let id = info.id;
            subs = subs.child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .px_2()
                    .py_1p5()
                    .rounded_md()
                    .bg(theme.muted.opacity(0.4))
                    .child(
                        v_flex()
                            .flex_1()
                            .child(div().text_sm().child(info.name))
                            .child(
                                div()
                                    .text_xs()
                                    .text_color(if logged_in {
                                        theme.success
                                    } else {
                                        theme.muted_foreground
                                    })
                                    .child(status),
                            ),
                    )
                    .child(if logged_in {
                        Button::new(SharedString::from(format!("logout-{id}")))
                            .label("Log out")
                            .small()
                            .ghost()
                            .on_click(cx.listener(move |this, _, window, cx| {
                                this.logout_subscription(id, window, cx)
                            }))
                    } else {
                        Button::new(SharedString::from(format!("login-{id}")))
                            .label("Log in")
                            .small()
                            .primary()
                            .on_click(cx.listener(move |this, _, window, cx| {
                                this.start_oauth(id, window, cx)
                            }))
                    }),
            );
        }

        let query = self.search.read(cx).value().to_string();
        let rows = self.rows(&store, &query);
        let mut keys = v_flex()
            .id("login-keys")
            .gap_0p5()
            .max_h(px(300.))
            .overflow_y_scroll();
        if rows.is_empty() {
            keys = keys.child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("No API key providers available."),
            );
        }
        for row in rows {
            let expanded = self.expanded.as_deref() == Some(row.id.as_str());
            let id = row.id.clone();
            let configured = row.status != "unconfigured";
            let mut item = v_flex().gap_1().px_2().py_1().rounded_md().child(
                h_flex()
                    .id(SharedString::from(format!("key-row-{}", row.id)))
                    .gap_2()
                    .items_center()
                    .cursor_pointer()
                    .on_click(cx.listener(move |this, _, window, cx| {
                        this.expanded = if this.expanded.as_deref() == Some(id.as_str()) {
                            None
                        } else {
                            Some(id.clone())
                        };
                        this.key.update(cx, |s, cx| {
                            s.set_value("", window, cx);
                            s.focus(window, cx);
                        });
                        cx.notify();
                    }))
                    .child(div().flex_1().text_sm().child(row.name.clone()))
                    .child(
                        div()
                            .text_xs()
                            .text_color(if configured {
                                theme.success
                            } else {
                                theme.muted_foreground
                            })
                            .child(row.status.clone()),
                    ),
            );
            if expanded {
                let save_id = row.id.clone();
                let remove_id = row.id.clone();
                item = item.bg(theme.muted.opacity(0.4)).child(
                    h_flex()
                        .gap_2()
                        .child(div().flex_1().child(Input::new(&self.key).small()))
                        .child(
                            Button::new("key-save")
                                .label("Save")
                                .small()
                                .primary()
                                .on_click(cx.listener(move |this, _, window, cx| {
                                    this.save_key(&save_id, window, cx)
                                })),
                        )
                        .child(
                            Button::new("key-remove")
                                .label("Remove")
                                .small()
                                .ghost()
                                .disabled(!row.stored_key)
                                .on_click(cx.listener(move |this, _, window, cx| {
                                    this.remove_key(&remove_id, window, cx)
                                })),
                        ),
                );
            }
            keys = keys.child(item);
        }
        let _ = window;

        v_flex()
            .gap_3()
            .child(
                div()
                    .text_xs()
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(theme.muted_foreground)
                    .child("SUBSCRIPTIONS"),
            )
            .child(subs)
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(
                        div()
                            .flex_1()
                            .text_xs()
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme.muted_foreground)
                            .child("API KEYS"),
                    )
                    .child(div().w(px(200.)).child(Input::new(&self.search).small())),
            )
            .child(keys)
            .child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(format!(
                        "Credentials are stored in {} with owner-only permissions. Environment variables and models.json keys are used when nothing is stored.",
                        self.auth_path()
                    )),
            )
            .when_some(self.message.clone(), |el, (error, text)| {
                el.child(
                    div()
                        .text_sm()
                        .text_color(if error { theme.danger } else { theme.success })
                        .child(text),
                )
            })
            .into_any_element()
    }
}

/// Open the login dialog. `on_changed` runs after credentials changed.
pub fn open(
    engine: &Engine,
    http: &reqwest::Client,
    on_changed: impl Fn(&mut Window, &mut App) + 'static,
    window: &mut Window,
    cx: &mut App,
) {
    let panel = cx.new(|cx| LoginPanel::new(engine.clone(), http.clone(), on_changed, window, cx));
    window.open_dialog(cx, move |dialog, _window, _cx| {
        let close_panel = panel.clone();
        dialog
            .title("Login")
            .w(px(600.))
            .on_close(move |_, _, cx| close_panel.update(cx, |p, _| p.shutdown()))
            .child(panel.clone())
    });
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{auth_status, oauth_only, subscription_name};
    use qt_core::auth::{Credential, OAuthCredentials};

    #[test]
    fn status_matches_archived_wording() {
        let key = Credential::ApiKey("k".into());
        let oauth = Credential::OAuth(OAuthCredentials::default());
        assert_eq!(
            auth_status(Some(&key), &[], false, false),
            "API key configured"
        );
        assert_eq!(
            auth_status(Some(&oauth), &[], false, true),
            "subscription configured"
        );
        assert_eq!(
            auth_status(Some(&key), &[], false, true),
            "API key configured"
        );
        assert_eq!(
            auth_status(None, &["OPENAI_API_KEY"], true, false),
            "env: OPENAI_API_KEY"
        );
        assert_eq!(auth_status(None, &["X"], false, true), "unconfigured");
        assert_eq!(auth_status(None, &[], true, false), "key in models.json");
        assert_eq!(auth_status(None, &[], false, false), "unconfigured");
        assert!(oauth_only("openai-codex") && !oauth_only("anthropic"));
        assert_eq!(subscription_name("anthropic"), "Anthropic (Claude Pro/Max)");
        assert_eq!(subscription_name("x"), "x");
    }
}
