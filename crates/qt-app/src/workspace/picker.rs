//! A searchable list dialog used by the command palette and the slash
//! command pickers (`/model`, `/thinking`, themes): type to filter, Up/Down
//! to move, Enter or a click to pick.

use std::rc::Rc;

use gpui_kit::component::input::{Input, InputEvent, InputState, MoveDown, MoveUp};
use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;

/// One row of a picker.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PickerItem {
    pub value: String,
    pub title: String,
    pub detail: Option<String>,
    /// Marked as the current choice.
    pub current: bool,
}

impl PickerItem {
    pub fn new(value: impl Into<String>, title: impl Into<String>) -> Self {
        Self {
            value: value.into(),
            title: title.into(),
            detail: None,
            current: false,
        }
    }

    pub fn detail(mut self, detail: impl Into<String>) -> Self {
        let detail = detail.into();
        self.detail = (!detail.is_empty()).then_some(detail);
        self
    }

    pub fn current(mut self, current: bool) -> Self {
        self.current = current;
        self
    }
}

/// Indices of `items` matching `query`: case-insensitive substring matches
/// on the title (then the detail) first, else a fuzzy subsequence match.
pub fn filter_items(items: &[PickerItem], query: &str) -> Vec<usize> {
    let query = query.trim();
    if query.is_empty() {
        return (0..items.len()).collect();
    }
    let lower = query.to_lowercase();
    let title_hits: Vec<usize> = (0..items.len())
        .filter(|&i| items[i].title.to_lowercase().contains(&lower))
        .collect();
    let detail_hits = (0..items.len()).filter(|&i| {
        !title_hits.contains(&i)
            && (items[i].value.to_lowercase().contains(&lower)
                || items[i]
                    .detail
                    .as_deref()
                    .is_some_and(|d| d.to_lowercase().contains(&lower)))
    });
    let mut out: Vec<usize> = title_hits.clone();
    out.extend(detail_hits);
    if out.is_empty() {
        out = (0..items.len())
            .filter(|&i| {
                qt_core::model_scope::fuzzy_matches(
                    query,
                    &format!("{} {}", items[i].title, items[i].value),
                )
            })
            .collect();
    }
    out
}

type PickHandler = Rc<dyn Fn(PickerItem, &mut Window, &mut App)>;

pub struct Picker {
    items: Vec<PickerItem>,
    filtered: Vec<usize>,
    selected: usize,
    query: Entity<InputState>,
    scroll: ScrollHandle,
    on_pick: PickHandler,
    empty_text: SharedString,
    _subscription: Subscription,
}

impl Picker {
    fn new(
        items: Vec<PickerItem>,
        placeholder: &str,
        initial_query: Option<&str>,
        on_pick: PickHandler,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let query = cx.new(|cx| {
            InputState::new(window, cx)
                .placeholder(placeholder.to_string())
                .default_value(initial_query.unwrap_or_default().to_string())
        });
        let subscription =
            cx.subscribe_in(&query, window, |this, _, event, window, cx| match event {
                InputEvent::Change => this.refilter(cx),
                InputEvent::PressEnter { .. } => this.pick_selected(window, cx),
                _ => {}
            });
        let mut this = Self {
            items,
            filtered: Vec::new(),
            selected: 0,
            query,
            scroll: ScrollHandle::new(),
            on_pick,
            empty_text: "No matches".into(),
            _subscription: subscription,
        };
        this.refilter(cx);
        if let Some(pos) = this.filtered.iter().position(|&i| this.items[i].current) {
            this.selected = pos;
            this.scroll.scroll_to_item(pos);
        }
        this
    }

    fn refilter(&mut self, cx: &mut Context<Self>) {
        let query = self.query.read(cx).value().to_string();
        self.filtered = filter_items(&self.items, &query);
        self.selected = 0;
        self.scroll.scroll_to_item(0);
        cx.notify();
    }

    fn move_selection(&mut self, delta: isize, cx: &mut Context<Self>) {
        if self.filtered.is_empty() {
            return;
        }
        let len = self.filtered.len() as isize;
        self.selected = (self.selected as isize + delta).rem_euclid(len) as usize;
        self.scroll.scroll_to_item(self.selected);
        cx.notify();
    }

    fn pick(&mut self, position: usize, window: &mut Window, cx: &mut Context<Self>) {
        let Some(item) = self
            .filtered
            .get(position)
            .and_then(|&i| self.items.get(i))
            .cloned()
        else {
            return;
        };
        window.close_dialog(cx);
        (self.on_pick)(item, window, cx);
    }

    fn pick_selected(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.pick(self.selected, window, cx);
    }
}

impl Render for Picker {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let mut list = v_flex()
            .id("picker-list")
            .track_scroll(&self.scroll)
            .flex_1()
            .min_h(px(100.))
            .overflow_y_scroll()
            .gap_0p5();
        if self.filtered.is_empty() {
            list = list.child(
                div()
                    .px_2()
                    .py_1()
                    .text_sm()
                    .text_color(theme.muted_foreground)
                    .child(self.empty_text.clone()),
            );
        }
        for (position, &index) in self.filtered.iter().enumerate() {
            let item = &self.items[index];
            let selected = position == self.selected;
            list = list.child(
                h_flex()
                    .id(("picker-item", position))
                    .gap_2()
                    .px_2()
                    .py_1()
                    .rounded_md()
                    .cursor_pointer()
                    .when(selected, |el| el.bg(theme.list_active))
                    .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.6))))
                    .on_click(
                        cx.listener(move |this, _, window, cx| this.pick(position, window, cx)),
                    )
                    .child(div().w(px(16.)).flex_none().when(item.current, |el| {
                        el.child(Icon::new(IconName::Check).xsmall())
                    }))
                    .child(
                        div()
                            .flex_none()
                            .max_w(relative(0.5))
                            .overflow_hidden()
                            .whitespace_nowrap()
                            .text_ellipsis()
                            .text_sm()
                            .child(item.title.clone()),
                    )
                    .when_some(item.detail.clone(), |el, detail| {
                        el.child(
                            div()
                                .flex_1()
                                .min_w_0()
                                .overflow_hidden()
                                .whitespace_nowrap()
                                .text_ellipsis()
                                .text_xs()
                                .text_color(theme.muted_foreground)
                                .child(detail),
                        )
                    }),
            );
        }
        v_flex()
            .size_full()
            .gap_2()
            .capture_action(cx.listener(|this, _: &MoveUp, _, cx| {
                this.move_selection(-1, cx);
                cx.stop_propagation();
            }))
            .capture_action(cx.listener(|this, _: &MoveDown, _, cx| {
                this.move_selection(1, cx);
                cx.stop_propagation();
            }))
            .child(Input::new(&self.query))
            .child(list)
            .child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("↑/↓ to move · Enter to choose · Esc to close"),
            )
    }
}

/// Open a picker dialog; `on_pick` runs after the dialog closes.
pub fn open_picker(
    title: impl Into<SharedString>,
    placeholder: &str,
    items: Vec<PickerItem>,
    initial_query: Option<&str>,
    on_pick: impl Fn(PickerItem, &mut Window, &mut App) + 'static,
    window: &mut Window,
    cx: &mut App,
) -> Entity<Picker> {
    let on_pick: PickHandler = Rc::new(on_pick);
    let picker = cx.new(|cx| Picker::new(items, placeholder, initial_query, on_pick, window, cx));
    let query = picker.read(cx).query.clone();
    let title: SharedString = title.into();
    let view = picker.clone();
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(dialog.title(title.clone()), window, 680., view.clone())
            .on_ok(|_, _, _| false)
    });
    // The dialog takes focus when it opens; hand it to the query field.
    window.defer(cx, move |window, cx| {
        query.update(cx, |s, cx| s.focus(window, cx));
    });
    picker
}

#[cfg(test)]
mod tests {
    use super::{PickerItem, filter_items};

    #[test]
    fn filters_titles_then_details_then_fuzzy() {
        let items = vec![
            PickerItem::new("anthropic/claude-sonnet", "Claude Sonnet").detail("Anthropic"),
            PickerItem::new("openai/gpt-5", "GPT-5").detail("OpenAI"),
            PickerItem::new("ollama:local/qwen", "qwen").detail("Ollama (local)"),
        ];
        assert_eq!(filter_items(&items, ""), [0, 1, 2]);
        assert_eq!(filter_items(&items, "gpt"), [1]);
        assert_eq!(filter_items(&items, "OPENAI"), [1]);
        assert_eq!(filter_items(&items, "local"), [2]);
        assert_eq!(filter_items(&items, "clsn"), [0]);
        assert!(filter_items(&items, "zzz").is_empty());
        assert!(PickerItem::new("a", "b").detail("").detail.is_none());
    }
}
