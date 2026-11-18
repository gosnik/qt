//! Window-relative sizing for panel dialogs.
//!
//! gpui-component dialogs take a fixed width (clamped to the window) and
//! size their height to the content. Panels such as `/memory`, `/mcp` or the
//! session pickers have lists and logs that should use the window instead:
//! [`fill_panel`] sizes the dialog from the current viewport on every frame
//! (the builder passed to `open_dialog` runs on each render, so resizing the
//! window resizes the panel) and hands the view the full body height, so a
//! panel root that is `size_full()` with a `flex_1().min_h_0()` list fills the
//! space and scrolls inside it.

use gpui_kit::component::dialog::Dialog;
use gpui_kit::component::window_paddings;
use gpui_kit::{AnyView, ParentElement, Pixels, Styled, Window, div, px};

/// Gap kept between a panel dialog and the window edges.
const EDGE: f32 = 24.;
/// Panels never get wider than this, so lines stay readable on large screens.
const MAX_WIDTH: f32 = 1600.;
/// Share of the window width a panel takes when that is more than its
/// preferred width.
const WIDTH_SHARE: f32 = 0.9;

/// Panel width for a viewport width: at least `preferred` (when the window
/// allows), growing to [`WIDTH_SHARE`] of the window, capped at
/// [`MAX_WIDTH`] and never wider than the window minus the edge gaps.
pub fn panel_width(viewport_width: f32, preferred: f32) -> f32 {
    let available = (viewport_width - 2. * EDGE).max(0.);
    (viewport_width * WIDTH_SHARE)
        .max(preferred)
        .min(MAX_WIDTH.max(preferred))
        .min(available)
}

/// Panel height for a viewport height: everything but the edge gaps.
pub fn panel_height(viewport_height: f32) -> f32 {
    (viewport_height - 2. * EDGE).max(0.)
}

fn viewport(window: &Window) -> (Pixels, Pixels) {
    let pad = window_paddings(window);
    let size = window.viewport_size();
    (
        size.width - pad.left - pad.right,
        size.height - pad.top - pad.bottom,
    )
}

/// Size `dialog` to the window and give `view` the whole body: width from
/// [`panel_width`], height from [`panel_height`]. `view`'s root should be
/// `size_full()` and put its scrolling list or log in a `flex_1().min_h_0()`
/// element.
pub fn fill_panel(
    dialog: Dialog,
    window: &Window,
    preferred_width: f32,
    view: impl Into<AnyView>,
) -> Dialog {
    let (w, h) = viewport(window);
    let view: AnyView = view.into();
    dialog
        .w(px(panel_width(f32::from(w), preferred_width)))
        .h(px(panel_height(f32::from(h))))
        .margin_top(px(EDGE))
        .content(move |content, _, _| {
            content
                .min_h_0()
                .child(div().flex_1().min_h_0().size_full().child(view.clone()))
        })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn panels_grow_with_the_window_and_stay_inside_it() {
        // Small window: never wider than the window minus the edge gaps.
        assert_eq!(panel_width(700., 900.), 652.);
        // Medium window: the preferred width wins over the share.
        assert_eq!(panel_width(1000., 900.), 900.);
        // Large window: grows to 90% of the window.
        assert_eq!(panel_width(1400., 900.), 1260.);
        // Very large window: capped.
        assert_eq!(panel_width(3000., 900.), MAX_WIDTH);
        assert_eq!(panel_height(900.), 852.);
        assert_eq!(panel_height(10.), 0.);
    }
}
