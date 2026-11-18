//! Window frame for compositors without server-side decorations.
//!
//! GPUI asks Linux compositors for server-side decorations, but some (GNOME
//! on Wayland) only support client-side ones; the window then has no title
//! bar, buttons or move handle unless the app draws them. `Framed` wraps a
//! window's root view and adds gpui-component's `TitleBar` (drag, double-click
//! to maximise, minimise/maximise/close) only in that case. Server-decorated
//! windows (X11, KDE, macOS, Windows) keep their native frame. Resize edges
//! and the shadow come from gpui-component's `Root`.

use gpui_kit::component::{TitleBar, v_flex};
use gpui_kit::{
    AnyView, App, AppContext as _, Context, Decorations, Entity, IntoElement, ParentElement,
    Render, SharedString, Styled, Window, div,
};

/// Root view that adds a client-drawn title bar when the compositor needs one.
pub struct Framed {
    title: SharedString,
    content: AnyView,
}

impl Framed {
    pub fn new(title: impl Into<SharedString>, content: impl Into<AnyView>) -> Self {
        Self {
            title: title.into(),
            content: content.into(),
        }
    }
}

/// Build `content` inside a [`Framed`] root (for `gpui_kit::open_window`).
pub fn framed<V: Render>(
    title: impl Into<SharedString>,
    window: &mut Window,
    cx: &mut App,
    content: impl FnOnce(&mut Window, &mut App) -> Entity<V>,
) -> Entity<Framed> {
    let view = content(window, cx);
    let title = title.into();
    cx.new(|_| Framed::new(title, view))
}

/// Whether the window needs the app to draw its title bar.
pub fn needs_client_title_bar(window: &Window) -> bool {
    cfg!(target_os = "linux") && matches!(window.window_decorations(), Decorations::Client { .. })
}

impl Render for Framed {
    fn render(&mut self, window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        let client = needs_client_title_bar(window);
        v_flex()
            .size_full()
            .when_some(client.then(|| self.title.clone()), |el, title| {
                el.child(TitleBar::new().child(div().text_sm().child(title)))
            })
            .child(div().flex_1().min_h_0().child(self.content.clone()))
    }
}

use gpui_kit::prelude::FluentBuilder as _;
