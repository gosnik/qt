//! `/mcp` panel: per-server status, tools and server info, reconnect,
//! resources (list and read) and setup guidance, over the runtime's
//! `McpManager`.

use std::time::Duration;

use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::*;
use qt_runtime::Runtime;
use qt_runtime::mcp_commands::{
    read_lines, reconnect_lines, resource_lines, resource_rows, setup_lines, status_text,
};
use qt_runtime::qt_mcp::{ServerSnapshot, ServerStatus};

use crate::index_panel::{OutputSize, PanelHooks, output_view, section};

/// A resource row: `(server, uri, name, mime type)`.
type ResourceRow = (String, String, String, String);

/// Badge text and whether it is a problem.
pub fn badge(status: &ServerStatus) -> (String, bool) {
    (
        status.label().to_string(),
        matches!(
            status,
            ServerStatus::Failed(_) | ServerStatus::Unsupported(_)
        ),
    )
}

/// Detail lines of one server.
pub fn server_lines(server: &ServerSnapshot) -> Vec<String> {
    let mut lines = vec![
        format!("transport: {}", server.transport),
        format!("target: {}", server.target),
        format!("status: {}", status_text(&server.status)),
    ];
    if let Some(info) = &server.info {
        let name = info
            .server_info
            .get("name")
            .and_then(|v| v.as_str())
            .unwrap_or("");
        let version = info
            .server_info
            .get("version")
            .and_then(|v| v.as_str())
            .unwrap_or("");
        if !name.is_empty() {
            lines.push(format!("server: {name} {version}").trim_end().to_string());
        }
        lines.push(format!("protocol: {}", info.protocol_version));
        lines.push(format!(
            "resources: {}",
            if info.supports_resources() {
                "supported"
            } else {
                "not offered"
            }
        ));
    }
    lines
}

pub struct McpPanel {
    runtime: Runtime,
    hooks: PanelHooks,
    servers: Vec<ServerSnapshot>,
    selected: usize,
    resources: Vec<ResourceRow>,
    title: String,
    output: Vec<String>,
    error: bool,
    busy: Option<String>,
    _task: Option<Task<()>>,
    _refresh: Task<()>,
}

impl McpPanel {
    fn new(runtime: Runtime, hooks: PanelHooks, cx: &mut Context<Self>) -> Self {
        let refresh = cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(Duration::from_secs(1)).await;
                let alive = this
                    .update(cx, |this, cx| {
                        let next = this.snapshots();
                        if next != this.servers {
                            this.servers = next;
                            cx.notify();
                        }
                    })
                    .is_ok();
                if !alive {
                    break;
                }
            }
        });
        let mut this = Self {
            servers: Vec::new(),
            runtime,
            hooks,
            selected: 0,
            resources: Vec::new(),
            title: String::new(),
            output: Vec::new(),
            error: false,
            busy: None,
            _task: None,
            _refresh: refresh,
        };
        this.servers = this.snapshots();
        if this.servers.is_empty() {
            this.title = "/mcp setup".into();
            this.output = setup_lines(&this.runtime.mcp_config_paths());
        }
        this
    }

    fn snapshots(&self) -> Vec<ServerSnapshot> {
        self.runtime
            .mcp()
            .map(|m| m.snapshots())
            .unwrap_or_default()
    }

    fn current(&self) -> Option<&ServerSnapshot> {
        self.servers.get(self.selected)
    }

    /// Run `/mcp <arguments>` and show its lines.
    fn command(&mut self, arguments: String, window: &mut Window, cx: &mut Context<Self>) {
        if self.busy.is_some() {
            return;
        }
        self.busy = Some(format!("/mcp {arguments}…"));
        let runtime = self.runtime.clone();
        let args = arguments.clone();
        let handle = self
            .runtime
            .engine()
            .spawn(async move { runtime.mcp_command(&args).await });
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update(cx, |this, cx| {
                this.busy = None;
                this.title = format!("/mcp {arguments}");
                match result {
                    Ok(lines) => {
                        this.error = lines.first().is_some_and(|l| l.contains("failed"));
                        this.output = lines;
                    }
                    Err(e) => {
                        this.error = true;
                        this.output = vec![e.to_string()];
                    }
                }
                this.servers = this.snapshots();
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn reconnect(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(manager) = self.runtime.mcp().cloned() else {
            return;
        };
        let Some(name) = self.current().map(|s| s.name.clone()) else {
            return;
        };
        if self.busy.is_some() {
            return;
        }
        self.busy = Some(format!("Reconnecting {name}…"));
        let target = name.clone();
        let handle = self
            .runtime
            .engine()
            .spawn(async move { reconnect_lines(&manager, Some(&target)).await });
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update(cx, |this, cx| {
                this.busy = None;
                this.title = format!("/mcp reconnect {name}");
                this.error = false;
                this.output = result.unwrap_or_else(|e| vec![e.to_string()]);
                this.servers = this.snapshots();
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn list_resources(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(manager) = self.runtime.mcp().cloned() else {
            return;
        };
        let Some(name) = self.current().map(|s| s.name.clone()) else {
            return;
        };
        if self.busy.is_some() {
            return;
        }
        self.busy = Some(format!("Listing resources of {name}…"));
        let server = name.clone();
        let handle = self
            .runtime
            .engine()
            .spawn(async move { manager.list_resources(Some(&server), None).await });
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update(cx, |this, cx| {
                this.busy = None;
                this.title = format!("/mcp resources {name}");
                match result {
                    Ok(Ok(listing)) => {
                        this.error = false;
                        this.resources = resource_rows(&listing);
                        this.output = resource_lines(&listing);
                    }
                    Ok(Err(e)) => {
                        this.error = true;
                        this.resources.clear();
                        this.output = vec![format!("MCP resources failed: {e}")];
                    }
                    Err(e) => {
                        this.error = true;
                        this.output = vec![e.to_string()];
                    }
                }
                this.servers = this.snapshots();
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn read_resource(
        &mut self,
        server: String,
        uri: String,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let Some(manager) = self.runtime.mcp().cloned() else {
            return;
        };
        if self.busy.is_some() {
            return;
        }
        self.busy = Some(format!("Reading {uri}…"));
        let (s, u) = (server.clone(), uri.clone());
        let handle = self
            .runtime
            .engine()
            .spawn(async move { manager.read_resource(&s, &u, None).await });
        self._task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = handle.await;
            let _ = this.update(cx, |this, cx| {
                this.busy = None;
                this.title = format!("MCP resource {uri}");
                match result {
                    Ok(Ok(value)) => {
                        this.error = false;
                        this.output = read_lines(&uri, &value);
                    }
                    Ok(Err(e)) => {
                        this.error = true;
                        this.output = vec![format!("read {uri} on {server} failed: {e}")];
                    }
                    Err(e) => {
                        this.error = true;
                        this.output = vec![e.to_string()];
                    }
                }
                cx.notify();
            });
        }));
        cx.notify();
    }
}

impl Render for McpPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme().clone();
        let busy = self.busy.is_some();

        let mut list = v_flex()
            .id("mcp-servers")
            .gap_0p5()
            .flex_1()
            .min_h(px(80.))
            .overflow_y_scroll();
        if self.servers.is_empty() {
            list = list.child(
                div()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child("No MCP servers configured."),
            );
        }
        for (i, server) in self.servers.iter().enumerate() {
            let selected = i == self.selected;
            let (label, problem) = badge(&server.status);
            list = list.child(
                v_flex()
                    .id(("mcp-server", i))
                    .px_2()
                    .py_1()
                    .rounded_md()
                    .cursor_pointer()
                    .when(selected, |el| el.bg(theme.list_active))
                    .when(!selected, |el| el.hover(|s| s.bg(theme.muted.opacity(0.5))))
                    .on_click(cx.listener(move |this, _, _, cx| {
                        this.selected = i;
                        this.resources.clear();
                        cx.notify();
                    }))
                    .child(div().text_sm().child(server.name.clone()))
                    .child(
                        div()
                            .text_xs()
                            .text_color(if problem {
                                theme.danger
                            } else {
                                theme.muted_foreground
                            })
                            .child(format!(
                                "{label} · {} · {} tools",
                                server.transport,
                                server.tools.len()
                            )),
                    ),
            );
        }
        let left = v_flex()
            .id("mcp-left")
            .w(px(220.))
            .flex_none()
            .h_full()
            .overflow_y_scroll()
            .gap_2()
            .child(section("SERVERS", cx))
            .child(list)
            .child(
                h_flex()
                    .gap_1()
                    .flex_wrap()
                    .child(
                        Button::new("mcp-status")
                            .label("Status")
                            .xsmall()
                            .ghost()
                            .disabled(busy)
                            .on_click(
                                cx.listener(|this, _, w, cx| this.command("status".into(), w, cx)),
                            ),
                    )
                    .child(
                        Button::new("mcp-tools")
                            .label("All tools")
                            .xsmall()
                            .ghost()
                            .disabled(busy)
                            .on_click(
                                cx.listener(|this, _, w, cx| this.command("tools".into(), w, cx)),
                            ),
                    )
                    .child(
                        Button::new("mcp-reconnect-all")
                            .label("Reconnect all")
                            .xsmall()
                            .ghost()
                            .disabled(busy || self.servers.is_empty())
                            .on_click(cx.listener(|this, _, w, cx| {
                                this.command("reconnect".into(), w, cx)
                            })),
                    )
                    .child(
                        Button::new("mcp-setup")
                            .label("Setup")
                            .xsmall()
                            .ghost()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.title = "/mcp setup".into();
                                this.error = false;
                                this.output = setup_lines(&this.runtime.mcp_config_paths());
                                cx.notify();
                            })),
                    ),
            );

        let mut right = v_flex()
            .id("mcp-right")
            .flex_1()
            .min_w_0()
            .h_full()
            .overflow_y_scroll()
            .gap_2();
        if let Some(server) = self.current().cloned() {
            right = right
                .child(
                    h_flex()
                        .gap_2()
                        .items_center()
                        .child(
                            div()
                                .flex_1()
                                .min_w_0()
                                .text_sm()
                                .font_weight(FontWeight::SEMIBOLD)
                                .child(server.name.clone()),
                        )
                        .child(
                            Button::new("mcp-reconnect")
                                .label("Reconnect")
                                .small()
                                .disabled(busy || !server.enabled)
                                .on_click(cx.listener(|this, _, w, cx| this.reconnect(w, cx))),
                        )
                        .child(
                            Button::new("mcp-resources")
                                .label("Resources")
                                .small()
                                .disabled(busy || server.status != ServerStatus::Connected)
                                .on_click(cx.listener(|this, _, w, cx| this.list_resources(w, cx))),
                        ),
                )
                .children(server_lines(&server).into_iter().map(|l| {
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child(SharedString::from(l))
                }))
                .child(section("TOOLS", cx));
            let mut tools = v_flex()
                .id("mcp-tools-list")
                .gap_0p5()
                .flex_1()
                .min_h(px(100.))
                .overflow_y_scroll();
            if server.tools.is_empty() {
                tools = tools.child(
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child("No tools discovered."),
                );
            }
            for tool in &server.tools {
                tools = tools.child(
                    v_flex()
                        .child(
                            div()
                                .text_xs()
                                .font_family(theme.mono_font_family.clone())
                                .child(tool.registered_name.clone()),
                        )
                        .child(
                            div()
                                .text_xs()
                                .text_color(theme.muted_foreground)
                                .child(qt_core::util::shorten(&tool.description, 160)),
                        ),
                );
            }
            right = right.child(tools);
            if !self.resources.is_empty() {
                let mut rows = v_flex()
                    .id("mcp-resource-rows")
                    .gap_0p5()
                    .flex_1()
                    .min_h(px(80.))
                    .overflow_y_scroll();
                for (i, (server, uri, name, mime)) in self.resources.iter().cloned().enumerate() {
                    let (s, u) = (server.clone(), uri.clone());
                    rows = rows.child(
                        h_flex()
                            .gap_2()
                            .items_center()
                            .child(
                                div()
                                    .flex_1()
                                    .min_w_0()
                                    .text_xs()
                                    .overflow_hidden()
                                    .text_ellipsis()
                                    .whitespace_nowrap()
                                    .child(if name.is_empty() || name == uri {
                                        format!("{uri} {mime}")
                                    } else {
                                        format!("{name} · {uri} {mime}")
                                    }),
                            )
                            .child(
                                Button::new(("mcp-read", i))
                                    .label("Read")
                                    .xsmall()
                                    .ghost()
                                    .disabled(busy)
                                    .on_click(cx.listener(move |this, _, w, cx| {
                                        this.read_resource(s.clone(), u.clone(), w, cx)
                                    })),
                            ),
                    );
                }
                right = right.child(section("RESOURCES", cx)).child(rows);
            }
        }

        v_flex()
            .size_full()
            .gap_3()
            .child(
                h_flex()
                    .flex_1()
                    .min_h(px(160.))
                    .gap_4()
                    .items_stretch()
                    .child(left)
                    .child(right),
            )
            .child(output_view(
                "mcp-output",
                &self.output,
                self.error,
                self.busy.as_deref(),
                OutputSize::Share(0.35),
                cx,
            ))
            .when(!self.output.is_empty(), |el| {
                el.child(
                    h_flex().justify_end().child(
                        Button::new("mcp-to-transcript")
                            .label("Copy to transcript")
                            .small()
                            .ghost()
                            .on_click(cx.listener(|this, _, w, cx| {
                                (this.hooks.card)(
                                    this.title.clone(),
                                    this.output.clone(),
                                    this.error,
                                    w,
                                    cx,
                                )
                            })),
                    ),
                )
            })
    }
}

/// Open the `/mcp` panel.
pub fn open(runtime: Runtime, hooks: PanelHooks, window: &mut Window, cx: &mut App) {
    let panel = cx.new(|cx| McpPanel::new(runtime, hooks, cx));
    window.open_dialog(cx, move |dialog, window, _| {
        crate::layout::fill_panel(dialog.title("MCP servers"), window, 860., panel.clone())
            .on_ok(|_, _, _| false)
    });
}

#[cfg(test)]
mod tests {
    use super::{badge, server_lines};
    use qt_runtime::qt_mcp::{ServerSnapshot, ServerStatus};

    #[test]
    fn badges_and_details() {
        assert_eq!(badge(&ServerStatus::Connected), ("connected".into(), false));
        assert_eq!(
            badge(&ServerStatus::Failed("x".into())),
            ("failed".into(), true)
        );
        let server = ServerSnapshot {
            name: "files".into(),
            transport: "stdio".into(),
            enabled: true,
            status: ServerStatus::Failed("spawn failed".into()),
            tools: Vec::new(),
            info: None,
            target: "npx srv".into(),
        };
        assert_eq!(
            server_lines(&server),
            [
                "transport: stdio",
                "target: npx srv",
                "status: failed: spawn failed"
            ]
        );
    }
}
