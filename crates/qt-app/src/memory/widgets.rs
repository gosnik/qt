//! Small layout helpers shared by the memory views.

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::{ActiveTheme as _, Sizable as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;

/// Upper-case section caption.
pub fn section(title: impl Into<SharedString>, cx: &App) -> Div {
    div()
        .text_xs()
        .font_weight(FontWeight::SEMIBOLD)
        .text_color(cx.theme().muted_foreground)
        .child(title.into())
}

/// Muted helper text.
pub fn hint(text: impl Into<SharedString>, cx: &App) -> Div {
    div()
        .text_xs()
        .text_color(cx.theme().muted_foreground)
        .child(text.into())
}

/// Monospace output lines; `error` colours them as an error.
pub fn lines(lines: &[String], error: bool, cx: &App) -> Div {
    let theme = cx.theme();
    v_flex().gap_0p5().children(lines.iter().map(|line| {
        div()
            .text_xs()
            .font_family(theme.mono_font_family.clone())
            .text_color(if error {
                theme.danger
            } else if line.starts_with("BLOCKED") || line.starts_with("Warning") {
                theme.warning
            } else {
                theme.foreground
            })
            .child(SharedString::from(line.clone()))
    }))
}

/// A bordered, scrollable output box.
pub fn output_box(id: impl Into<ElementId>, body: Div, max_height: f32, cx: &App) -> Stateful<Div> {
    div()
        .id(id)
        .p_2()
        .rounded_md()
        .border_1()
        .border_color(cx.theme().border)
        .max_h(px(max_height))
        .overflow_y_scroll()
        .child(body)
}

/// A selectable chip button.
pub fn chip(id: impl Into<ElementId>, label: impl Into<SharedString>, active: bool) -> Button {
    Button::new(id)
        .label(label)
        .xsmall()
        .when(active, |b| b.primary())
        .when(!active, |b| b.ghost())
}

/// `label: value` row.
pub fn row(label: &str, value: impl Into<SharedString>, cx: &App) -> Div {
    h_flex()
        .gap_2()
        .items_start()
        .child(
            div()
                .w(px(130.))
                .flex_none()
                .text_xs()
                .text_color(cx.theme().muted_foreground)
                .child(SharedString::from(label.to_string())),
        )
        .child(div().flex_1().min_w_0().text_xs().child(value.into()))
}

/// A metric card for the dashboard overview.
pub fn metric(label: &str, value: String, detail: &str, warn: bool, cx: &App) -> Div {
    let theme = cx.theme();
    v_flex()
        .flex_1()
        .min_w(px(120.))
        .p_3()
        .gap_1()
        .rounded_lg()
        .border_1()
        .border_color(theme.border)
        .bg(theme.secondary)
        .child(
            div()
                .text_xs()
                .text_color(theme.muted_foreground)
                .child(SharedString::from(label.to_string())),
        )
        .child(
            div()
                .text_2xl()
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(if warn {
                    theme.warning
                } else {
                    theme.foreground
                })
                .child(SharedString::from(value)),
        )
        .child(
            div()
                .text_xs()
                .text_color(theme.muted_foreground)
                .child(SharedString::from(detail.to_string())),
        )
}

/// A selectable list row with a title and a detail line.
pub fn list_row(
    id: impl Into<ElementId>,
    title: impl Into<SharedString>,
    detail: impl Into<SharedString>,
    selected: bool,
    cx: &App,
) -> Stateful<Div> {
    let theme = cx.theme();
    v_flex()
        .id(id)
        .px_2()
        .py_1()
        .rounded_md()
        .cursor_pointer()
        .when(selected, |el| el.bg(theme.list_active))
        .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.5))))
        .child(div().text_sm().child(title.into()))
        .child(
            div()
                .text_xs()
                .text_color(theme.muted_foreground)
                .child(detail.into()),
        )
}
