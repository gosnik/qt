//! `/extensions`, `/skills`, `/prompts`, `/themes`: the package resource
//! selector (archived settings list with `on` / `off` / `loaded` values).
//! Toggling writes the package filter to the user or project settings and
//! reloads extensions and resources (`/reload`).

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::switch::Switch;
use gpui_kit::component::{ActiveTheme as _, Sizable as _, WindowExt as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_runtime::Runtime;
use qt_runtime::package_resources::{PackageResourceItem, empty_listing_line, filter_items};
use qt_runtime::qt_extensions::packages::ResourceKind;

use super::chrome::ExtensionChrome;

/// Dialog title of a selector.
pub fn kind_title(kind: ResourceKind) -> &'static str {
    match kind {
        ResourceKind::Extensions => "Extensions",
        ResourceKind::Skills => "Skills",
        ResourceKind::Prompts => "Prompt templates",
        ResourceKind::Themes => "Themes",
    }
}

pub struct PackagesPanel {
    runtime: Runtime,
    chrome: WeakEntity<ExtensionChrome>,
    kind: ResourceKind,
    items: Vec<PackageResourceItem>,
    error: Option<String>,
    message: Option<(String, bool)>,
    filter: Entity<InputState>,
    _subscription: Subscription,
}

impl PackagesPanel {
    fn new(
        runtime: Runtime,
        chrome: WeakEntity<ExtensionChrome>,
        kind: ResourceKind,
        filter: Option<String>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let input = cx.new(|cx| {
            InputState::new(window, cx)
                .placeholder("Filter by name, package or path")
                .default_value(filter.unwrap_or_default())
        });
        let subscription = cx.subscribe(&input, |_, _, event: &InputEvent, cx| {
            if matches!(event, InputEvent::Change) {
                cx.notify();
            }
        });
        let mut this = Self {
            runtime,
            chrome,
            kind,
            items: Vec::new(),
            error: None,
            message: None,
            filter: input,
            _subscription: subscription,
        };
        this.refresh();
        this
    }

    fn refresh(&mut self) {
        match self.runtime.package_resource_items(self.kind) {
            Ok(items) => {
                self.items = items;
                self.error = None;
            }
            Err(e) => {
                self.items.clear();
                self.error = Some(e);
            }
        }
    }

    fn toggle(&mut self, item: PackageResourceItem, enabled: bool, cx: &mut Context<Self>) {
        match self.runtime.toggle_package_resource(&item, enabled) {
            Ok(message) => {
                let lines = self
                    .chrome
                    .update(cx, |chrome, cx| chrome.reload(cx))
                    .unwrap_or_else(|_| self.runtime.reload());
                let mut card = vec![message.clone()];
                card.extend(lines.into_iter().skip(1));
                let _ = self.chrome.update(cx, |chrome, cx| {
                    chrome.card(format!("/{}", self.kind.as_str()), card, false, cx)
                });
                self.message = Some((message, false));
            }
            Err(e) => self.message = Some((format!("error: {e}"), true)),
        }
        self.refresh();
        cx.notify();
    }
}

impl Render for PackagesPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let filter = self.filter.read(cx).value().to_string();
        let shown = filter_items(&self.items, &filter);
        let mut list = v_flex()
            .id("package-resources")
            .gap_0p5()
            .flex_1()
            .min_h(px(120.))
            .overflow_y_scroll();
        if let Some(error) = &self.error {
            list = list.child(
                div()
                    .text_sm()
                    .text_color(theme.danger)
                    .child(format!("error: {error}")),
            );
        } else if shown.is_empty() {
            list = list.child(
                div()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child(empty_listing_line(self.kind)),
            );
        }
        for (i, item) in shown.into_iter().enumerate() {
            let toggle_item = item.clone();
            let control: AnyElement = if item.can_toggle {
                Switch::new(("package-toggle", i))
                    .checked(item.enabled)
                    .small()
                    .on_click(cx.listener(move |this, checked: &bool, _, cx| {
                        this.toggle(toggle_item.clone(), *checked, cx)
                    }))
                    .into_any_element()
            } else {
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(item.state_label())
                    .into_any_element()
            };
            list = list.child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .px_2()
                    .py_1()
                    .rounded_md()
                    .hover(|s| s.bg(theme.muted.opacity(0.5)))
                    .child(
                        v_flex()
                            .flex_1()
                            .min_w_0()
                            .child(div().text_sm().child(item.label.clone()))
                            .child(
                                div()
                                    .text_xs()
                                    .text_color(theme.muted_foreground)
                                    .overflow_hidden()
                                    .text_ellipsis()
                                    .whitespace_nowrap()
                                    .child(item.description.clone()),
                            ),
                    )
                    .child(control),
            );
        }
        v_flex()
            .size_full()
            .gap_2()
            .child(Input::new(&self.filter).small())
            .child(list)
            .when_some(self.message.clone(), |el, (message, error)| {
                el.child(
                    div()
                        .text_xs()
                        .text_color(if error { theme.danger } else { theme.foreground })
                        .child(message),
                )
            })
            .child(
                h_flex()
                    .gap_2()
                    .items_center()
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .text_xs()
                            .text_color(theme.muted_foreground)
                            .child("Package resources are toggled in the package's user or project settings; other resources are read-only here."),
                    )
                    .child(
                        Button::new("packages-reload")
                            .label("Reload")
                            .small()
                            .ghost()
                            .on_click(cx.listener(|this, _, _, cx| {
                                let lines = this
                                    .chrome
                                    .update(cx, |chrome, cx| chrome.reload(cx))
                                    .unwrap_or_default();
                                let _ = this.chrome.update(cx, |chrome, cx| {
                                    chrome.card("/reload".into(), lines, false, cx)
                                });
                                this.refresh();
                                cx.notify();
                            })),
                    ),
            )
    }
}

/// Open the selector for `kind` with an optional filter.
pub fn open_packages(
    runtime: Runtime,
    chrome: WeakEntity<ExtensionChrome>,
    kind: ResourceKind,
    filter: Option<String>,
    window: &mut Window,
    cx: &mut App,
) {
    let panel = cx.new(|cx| PackagesPanel::new(runtime, chrome, kind, filter, window, cx));
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(dialog.title(kind_title(kind)), window, 760., panel.clone())
            .on_ok(|_, _, _| false)
    });
}

#[cfg(test)]
mod tests {
    use super::kind_title;
    use qt_runtime::qt_extensions::packages::ResourceKind;

    #[test]
    fn titles() {
        assert_eq!(kind_title(ResourceKind::Prompts), "Prompt templates");
        assert_eq!(kind_title(ResourceKind::Extensions), "Extensions");
    }
}
