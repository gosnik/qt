//! Relationship graph canvas for the dashboard (archived
//! `draw_graph_canvas`): typed edges as lines, memories as coloured nodes
//! laid out by `views::dashboard::graph_layout`, wheel zoom, drag pan, fit,
//! and node selection.

use std::collections::HashMap;
use std::rc::Rc;

use gpui_kit::component::ActiveTheme as _;
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_memory::views::dashboard::{GraphData, NodeLayout, fit_view};

/// Called with the ID of a clicked node.
pub type SelectNode = Rc<dyn Fn(String, &mut Window, &mut App)>;

/// Canvas size in logical pixels.
pub const WIDTH: f32 = 700.;
pub const HEIGHT: f32 = 520.;

/// Archived group palette.
pub fn palette(index: usize) -> Hsla {
    const COLORS: [(f32, f32, f32); 8] = [
        (0.18, 0.78, 0.88),
        (0.96, 0.63, 0.24),
        (0.35, 0.89, 0.67),
        (0.63, 0.42, 1.00),
        (0.94, 0.82, 0.29),
        (0.30, 0.58, 0.95),
        (0.95, 0.34, 0.38),
        (0.44, 0.93, 0.88),
    ];
    let (r, g, b) = COLORS[index % COLORS.len()];
    Rgba { r, g, b, a: 1.0 }.into()
}

/// Zoom and pan of the canvas.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GraphView {
    pub zoom: f32,
    pub pan_x: f32,
    pub pan_y: f32,
}

impl GraphView {
    /// Fits the layout into the canvas.
    pub fn fit(layout: &HashMap<String, NodeLayout>) -> Self {
        let (zoom, pan_x, pan_y) = fit_view(layout, WIDTH, HEIGHT);
        Self { zoom, pan_x, pan_y }
    }

    pub fn screen(self, x: f32, y: f32) -> (f32, f32) {
        (x * self.zoom + self.pan_x, y * self.zoom + self.pan_y)
    }

    /// Wheel zoom (archived: ±0.08 per notch, 0.25–2.5) around the centre.
    pub fn zoom_by(&mut self, delta: f32) {
        let next = (self.zoom + delta).clamp(0.25, 2.5);
        let (cx, cy) = (WIDTH * 0.5, HEIGHT * 0.5);
        let world_x = (cx - self.pan_x) / self.zoom;
        let world_y = (cy - self.pan_y) / self.zoom;
        self.zoom = next;
        self.pan_x = cx - world_x * next;
        self.pan_y = cy - world_y * next;
    }
}

/// The graph element. `on_select` receives a clicked node ID.
pub fn render(
    graph: &GraphData,
    layout: &HashMap<String, NodeLayout>,
    view: GraphView,
    selected: Option<&str>,
    on_select: SelectNode,
    cx: &App,
) -> Div {
    let theme = cx.theme();
    let edge_color = theme.muted_foreground.opacity(0.55);
    let selected_edge = theme.primary;
    let segments: Vec<(Point<Pixels>, Point<Pixels>, bool)> = graph
        .edges
        .iter()
        .filter_map(|edge| {
            let from = layout.get(&edge.from_id)?;
            let to = layout.get(&edge.to_id)?;
            let (ax, ay) = view.screen(from.x, from.y);
            let (bx, by) = view.screen(to.x, to.y);
            let touches = selected.is_some_and(|id| id == edge.from_id || id == edge.to_id);
            Some((point(px(ax), px(ay)), point(px(bx), px(by)), touches))
        })
        .collect();
    let edges = canvas(
        move |_, _, _| segments,
        move |bounds, segments, window, _| {
            for (from, to, touches) in segments {
                let mut builder = PathBuilder::stroke(px(if touches { 2.0 } else { 1.2 }));
                builder.move_to(bounds.origin + from);
                builder.line_to(bounds.origin + to);
                if let Ok(path) = builder.build() {
                    window.paint_path(path, if touches { selected_edge } else { edge_color });
                }
            }
        },
    )
    .absolute()
    .top_0()
    .left_0()
    .size_full();

    let mut container = div()
        .relative()
        .w(px(WIDTH))
        .h(px(HEIGHT))
        .overflow_hidden()
        .rounded_lg()
        .border_1()
        .border_color(theme.border)
        .bg(theme.secondary)
        .child(edges);
    if graph.nodes.is_empty() {
        return container.child(
            div()
                .absolute()
                .top(px(20.))
                .left(px(18.))
                .text_sm()
                .text_color(theme.muted_foreground)
                .child("Load a graph to inspect typed memory relationships."),
        );
    }
    for (index, node) in graph.nodes.iter().enumerate() {
        let Some(position) = layout.get(&node.id) else {
            continue;
        };
        let (x, y) = view.screen(position.x, position.y);
        let radius = (position.radius * view.zoom).max(6.0);
        let is_selected = selected == Some(node.id.as_str());
        let color = palette(node.group);
        let id = node.id.clone();
        let on_select = on_select.clone();
        let label: String = node.label.chars().take(28).collect();
        // The circle is centred on the node position; the label sits
        // below it in its own element so it never shifts the circle.
        let on_label = on_select.clone();
        let label_id = id.clone();
        container = container
            .child(
                div()
                    .id(("graph-node", index))
                    .absolute()
                    .left(px(x - radius))
                    .top(px(y - radius))
                    .size(px(radius * 2.0))
                    .rounded_full()
                    .cursor_pointer()
                    .bg(color.opacity(if is_selected { 1.0 } else { 0.75 }))
                    .border_2()
                    .border_color(if is_selected {
                        theme.foreground
                    } else {
                        color.opacity(0.3)
                    })
                    .on_click(move |_, window, cx| on_select(id.clone(), window, cx)),
            )
            .when(view.zoom >= 0.5 || is_selected, |el| {
                el.child(
                    div()
                        .id(("graph-label", index))
                        .absolute()
                        .left(px(x - 80.0))
                        .top(px(y + radius + 2.0))
                        .w(px(160.))
                        .flex()
                        .justify_center()
                        .cursor_pointer()
                        .text_xs()
                        .text_color(theme.foreground)
                        .on_click(move |_, window, cx| on_label(label_id.clone(), window, cx))
                        .child(label),
                )
            });
    }
    container
}

#[cfg(test)]
mod tests {
    use super::{GraphView, HEIGHT, WIDTH};

    #[test]
    fn zoom_keeps_the_centre_fixed() {
        let mut view = GraphView {
            zoom: 1.0,
            pan_x: 10.0,
            pan_y: -20.0,
        };
        let centre = (
            (WIDTH * 0.5 - view.pan_x) / view.zoom,
            (HEIGHT * 0.5 - view.pan_y) / view.zoom,
        );
        view.zoom_by(0.5);
        let (x, y) = view.screen(centre.0, centre.1);
        assert!((x - WIDTH * 0.5).abs() < 0.01 && (y - HEIGHT * 0.5).abs() < 0.01);
        view.zoom_by(10.0);
        assert_eq!(view.zoom, 2.5);
    }
}
