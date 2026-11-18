//! Pi JSON themes (`qt_core::resources::themes`) mapped onto the
//! gpui-component theme, plus the transcript palette and live reload.
//!
//! A theme is selected by the `theme` settings key (global
//! `settings.json`, overridden by the project's `.pi/settings.json`). When
//! no theme is set, the older `gui.theme` (`"light"`/`"dark"`) is honoured
//! and otherwise the window follows the system appearance with the
//! built-in `dark` / `light` themes. Custom themes are discovered from the
//! resource roots (`<agent home>/themes`, `.pi/themes`, `.agents/themes`,
//! `themeRoots`) and reloaded through [`ThemeWatcher`] when their file
//! changes.

use std::path::{Path, PathBuf};

use gpui_kit::component::{Colorize as _, Theme as UiTheme, ThemeColor as UiColors, ThemeMode};
use gpui_kit::*;
use qt_core::Engine;
use qt_core::resources::theme_watcher::ThemeWatcher;
use qt_core::resources::themes::{
    Rgb, Theme as PiTheme, ThemeColor as PiColor, ThemeResources, ThemeToken, builtin_theme,
    canonicalize_theme_name,
};
use qt_core::resources::{ResourceOptions, load_resources};

/// Colours the transcript draws with that have no gpui-component token.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct QtPalette {
    pub user_bg: Hsla,
    pub user_fg: Hsla,
    pub custom_bg: Hsla,
    pub custom_fg: Hsla,
    pub custom_label: Hsla,
    pub tool_pending_bg: Hsla,
    pub tool_success_bg: Hsla,
    pub tool_error_bg: Hsla,
    pub tool_title: Hsla,
    pub tool_output: Hsla,
    pub thinking_text: Hsla,
    pub diff_added: Hsla,
    pub diff_removed: Hsla,
    pub diff_context: Hsla,
    pub accent: Hsla,
    pub border_accent: Hsla,
    pub dim: Hsla,
    pub success: Hsla,
    pub error: Hsla,
    pub warning: Hsla,
    /// `thinkingOff` … `thinkingXhigh`, indexed like `ThinkingLevel::ALL`.
    pub thinking: [Hsla; 6],
}

impl Global for QtPalette {}

impl QtPalette {
    /// The palette for the current window theme.
    pub fn get(cx: &App) -> Self {
        cx.try_global::<QtPalette>()
            .copied()
            .unwrap_or_else(|| Self::fallback(&UiTheme::global(cx).colors))
    }

    /// Palette derived from gpui-component colours alone.
    pub fn fallback(base: &UiColors) -> Self {
        Self {
            user_bg: base.secondary,
            user_fg: base.secondary_foreground,
            custom_bg: base.muted,
            custom_fg: base.foreground,
            custom_label: base.info,
            tool_pending_bg: base.background,
            tool_success_bg: base.background,
            tool_error_bg: base.background,
            tool_title: base.foreground,
            tool_output: base.muted_foreground,
            thinking_text: base.muted_foreground,
            diff_added: base.success,
            diff_removed: base.danger,
            diff_context: base.muted_foreground,
            accent: base.primary,
            border_accent: base.ring,
            dim: base.muted_foreground,
            success: base.success,
            error: base.danger,
            warning: base.warning,
            thinking: [base.muted_foreground; 6],
        }
    }
}

pub fn hsla_of(rgb: Rgb) -> Hsla {
    rgb_u32(rgb).into()
}

fn rgb_u32(rgb: Rgb) -> Rgba {
    gpui_kit::rgb(((rgb.r as u32) << 16) | ((rgb.g as u32) << 8) | rgb.b as u32)
}

/// Relative luminance (sRGB, 0–1).
pub fn luminance(rgb: Rgb) -> f32 {
    let channel = |c: u8| {
        let c = c as f32 / 255.0;
        if c <= 0.039_28 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        }
    };
    0.2126 * channel(rgb.r) + 0.7152 * channel(rgb.g) + 0.0722 * channel(rgb.b)
}

/// Whether a Pi theme is meant for a dark background. Pi themes have no
/// background token (the terminal provides it), so the HTML export page
/// background decides, then the user-message background, then the text
/// colour, then the name.
pub fn is_dark_theme(theme: &PiTheme) -> bool {
    if let Some(bg) = theme.export.page_bg.and_then(PiColor::rgb) {
        return luminance(bg) < 0.4;
    }
    if let Some(bg) = theme.color(ThemeToken::UserMessageBg).rgb() {
        return luminance(bg) < 0.4;
    }
    if let Some(fg) = theme.color(ThemeToken::Text).rgb() {
        return luminance(fg) >= 0.4;
    }
    !theme.canonical_name.contains("light")
}

/// Black or white, whichever reads better on `bg`.
fn contrast_on(bg: Rgb) -> Hsla {
    if luminance(bg) > 0.45 {
        gpui_kit::black()
    } else {
        gpui_kit::white()
    }
}

/// Overlay the theme's colours onto gpui-component colours (which already
/// hold the defaults for the theme's light/dark mode).
pub fn apply_to_colors(theme: &PiTheme, colors: &mut UiColors) {
    let tok = |t: ThemeToken| theme.color(t).rgb();
    let set = |slot: &mut Hsla, value: Option<Rgb>| {
        if let Some(v) = value {
            *slot = hsla_of(v);
        }
    };
    let dark = is_dark_theme(theme);
    let shade = |c: Hsla, amount: f32| {
        if dark {
            c.lighten(amount)
        } else {
            c.darken(amount)
        }
    };

    let page = theme.export.page_bg.and_then(PiColor::rgb);
    let card = theme.export.card_bg.and_then(PiColor::rgb).or(page);
    set(&mut colors.background, page);
    for slot in [
        &mut colors.popover,
        &mut colors.sidebar,
        &mut colors.title_bar,
        &mut colors.tab_bar,
        &mut colors.status_bar,
        &mut colors.list,
        &mut colors.table,
    ] {
        set(slot, card);
    }

    let text = tok(ThemeToken::Text);
    for slot in [
        &mut colors.foreground,
        &mut colors.popover_foreground,
        &mut colors.sidebar_foreground,
        &mut colors.tab_active_foreground,
    ] {
        set(slot, text);
    }
    set(&mut colors.muted_foreground, tok(ThemeToken::Muted));
    set(&mut colors.tab_foreground, tok(ThemeToken::Muted));

    let border = tok(ThemeToken::BorderMuted);
    for slot in [
        &mut colors.border,
        &mut colors.input,
        &mut colors.sidebar_border,
        &mut colors.title_bar_border,
        &mut colors.status_bar_border,
        &mut colors.table_row_border,
    ] {
        set(slot, border);
    }

    if let Some(accent) = tok(ThemeToken::Accent) {
        let a = hsla_of(accent);
        colors.primary = a;
        colors.primary_hover = shade(a, 0.1);
        colors.primary_active = shade(a, 0.2);
        colors.primary_foreground = contrast_on(accent);
        colors.button_primary = a;
        colors.button_primary_hover = colors.primary_hover;
        colors.button_primary_active = colors.primary_active;
        colors.button_primary_foreground = colors.primary_foreground;
        colors.ring = a;
        colors.caret = a;
        colors.switch = a;
        colors.slider_bar = a;
        colors.progress_bar = a;
        colors.sidebar_primary = a;
        colors.sidebar_primary_foreground = colors.primary_foreground;
        colors.list_active_border = a;
        colors.drag_border = a;
    }
    set(&mut colors.info, tok(ThemeToken::BorderAccent));
    set(&mut colors.link, tok(ThemeToken::MdLink));
    if let Some(link) = tok(ThemeToken::MdLink) {
        colors.link_hover = shade(hsla_of(link), 0.15);
        colors.link_active = shade(hsla_of(link), 0.25);
    }

    for (slot, hover, active, token) in [
        (
            &mut colors.success,
            &mut colors.success_hover,
            &mut colors.success_active,
            ThemeToken::Success,
        ),
        (
            &mut colors.danger,
            &mut colors.danger_hover,
            &mut colors.danger_active,
            ThemeToken::Error,
        ),
        (
            &mut colors.warning,
            &mut colors.warning_hover,
            &mut colors.warning_active,
            ThemeToken::Warning,
        ),
    ] {
        if let Some(c) = tok(token) {
            *slot = hsla_of(c);
            *hover = shade(*slot, 0.1);
            *active = shade(*slot, 0.2);
        }
    }
    for (slot, token) in [
        (&mut colors.success_foreground, ThemeToken::Success),
        (&mut colors.danger_foreground, ThemeToken::Error),
        (&mut colors.warning_foreground, ThemeToken::Warning),
    ] {
        if let Some(c) = tok(token) {
            *slot = contrast_on(c);
        }
    }

    if let Some(sel) = tok(ThemeToken::SelectedBg) {
        let s = hsla_of(sel);
        colors.selection = s.opacity(0.85);
        colors.list_active = s;
        colors.list_hover = s.opacity(0.6);
        colors.sidebar_accent = s;
        colors.table_active = s;
        colors.table_hover = s.opacity(0.6);
        colors.accent = s;
        colors.secondary_hover = s;
    }
    if let Some(fg) = text {
        colors.sidebar_accent_foreground = hsla_of(fg);
        colors.accent_foreground = hsla_of(fg);
    }
    if let Some(user) = tok(ThemeToken::UserMessageBg) {
        colors.secondary = hsla_of(user);
        colors.secondary_active = shade(hsla_of(user), 0.1);
        colors.muted = hsla_of(user);
    }
    set(
        &mut colors.secondary_foreground,
        tok(ThemeToken::UserMessageText).or(text),
    );
}

/// The transcript palette for a theme; `Default` tokens fall back to `base`.
pub fn palette_for(theme: &PiTheme, base: &UiColors) -> QtPalette {
    let fallback = QtPalette::fallback(base);
    let pick = |t: ThemeToken, default: Hsla| theme.color(t).rgb().map_or(default, hsla_of);
    let levels = [
        ThemeToken::ThinkingOff,
        ThemeToken::ThinkingMinimal,
        ThemeToken::ThinkingLow,
        ThemeToken::ThinkingMedium,
        ThemeToken::ThinkingHigh,
        ThemeToken::ThinkingXhigh,
    ];
    QtPalette {
        user_bg: pick(ThemeToken::UserMessageBg, fallback.user_bg),
        user_fg: pick(ThemeToken::UserMessageText, base.foreground),
        custom_bg: pick(ThemeToken::CustomMessageBg, fallback.custom_bg),
        custom_fg: pick(ThemeToken::CustomMessageText, base.foreground),
        custom_label: pick(ThemeToken::CustomMessageLabel, fallback.custom_label),
        tool_pending_bg: pick(ThemeToken::ToolPendingBg, fallback.tool_pending_bg),
        tool_success_bg: pick(ThemeToken::ToolSuccessBg, fallback.tool_success_bg),
        tool_error_bg: pick(ThemeToken::ToolErrorBg, fallback.tool_error_bg),
        tool_title: pick(ThemeToken::ToolTitle, base.foreground),
        tool_output: pick(ThemeToken::ToolOutput, fallback.tool_output),
        thinking_text: pick(ThemeToken::ThinkingText, fallback.thinking_text),
        diff_added: pick(ThemeToken::ToolDiffAdded, fallback.diff_added),
        diff_removed: pick(ThemeToken::ToolDiffRemoved, fallback.diff_removed),
        diff_context: pick(ThemeToken::ToolDiffContext, fallback.diff_context),
        accent: pick(ThemeToken::Accent, fallback.accent),
        border_accent: pick(ThemeToken::BorderAccent, fallback.border_accent),
        dim: pick(ThemeToken::Dim, fallback.dim),
        success: pick(ThemeToken::Success, fallback.success),
        error: pick(ThemeToken::Error, fallback.error),
        warning: pick(ThemeToken::Warning, fallback.warning),
        thinking: levels.map(|t| pick(t, base.muted_foreground)),
    }
}

/// Which theme the settings ask for: `Some(name)` for `theme` (or the
/// legacy `gui.theme`), `None` to follow the system appearance.
pub fn configured_theme_name(
    project_theme: Option<&str>,
    legacy_gui_theme: Option<&str>,
) -> Option<String> {
    project_theme
        .filter(|t| !t.trim().is_empty())
        .or(legacy_gui_theme.filter(|t| matches!(*t, "dark" | "light")))
        .map(str::to_string)
}

/// The theme to apply for a selection. Unknown names fall back to the
/// built-in theme matching the system appearance.
pub fn resolve_selection(
    resources: &ThemeResources,
    selected: Option<&str>,
    system_dark: bool,
) -> (PiTheme, Option<PathBuf>) {
    let fallback = if system_dark { "dark" } else { "light" };
    if let Some(name) = selected
        && let Some(theme) = resources.resolve(name)
    {
        return (theme, resources.path_of(name).map(Path::to_path_buf));
    }
    (
        builtin_theme(fallback).expect("built-in themes parse"),
        None,
    )
}

/// Live theme state: discovered themes, the selection and its watcher.
pub struct ThemeState {
    pub resources: ThemeResources,
    /// `None` follows the system appearance.
    pub selected: Option<String>,
    pub applied: String,
    watcher: Option<ThemeWatcher>,
    _reload_task: Option<Task<()>>,
}

impl Global for ThemeState {}

impl ThemeState {
    pub fn global(cx: &App) -> Option<&ThemeState> {
        cx.try_global::<ThemeState>()
    }

    /// Names offered by the theme picker.
    pub fn available(cx: &App) -> Vec<String> {
        Self::global(cx)
            .map(|s| s.resources.available_names())
            .unwrap_or_else(|| vec!["dark".into(), "light".into()])
    }
}

fn discover(engine: &Engine) -> (ThemeResources, Option<String>) {
    let mut options = ResourceOptions::new(engine.cwd());
    options.no_skills = true;
    options.no_prompt_templates = true;
    let snapshot = load_resources(engine.home(), &options);
    (snapshot.themes, snapshot.settings.theme)
}

/// Discover themes and apply the configured one. Call once at startup.
pub fn init(engine: &Engine, cx: &mut App) {
    let (resources, setting) = discover(engine);
    let legacy = engine.settings().theme();
    let selected = configured_theme_name(setting.as_deref(), legacy.as_deref());
    cx.set_global(ThemeState {
        resources,
        selected: selected.clone(),
        applied: String::new(),
        watcher: None,
        _reload_task: None,
    });
    apply_selection(selected.as_deref(), cx);
}

/// Re-discover themes (after settings or files changed) and re-apply.
pub fn rediscover(engine: &Engine, cx: &mut App) {
    let (resources, _) = discover(engine);
    let selected = ThemeState::global(cx).and_then(|s| s.selected.clone());
    if cx.has_global::<ThemeState>() {
        cx.global_mut::<ThemeState>().resources = resources;
    }
    apply_selection(selected.as_deref(), cx);
}

/// Apply a theme now (`None` follows the system appearance).
pub fn apply_selection(selected: Option<&str>, cx: &mut App) {
    let system_dark = matches!(
        cx.window_appearance(),
        WindowAppearance::Dark | WindowAppearance::VibrantDark
    );
    let resources = ThemeState::global(cx)
        .map(|s| s.resources.clone())
        .unwrap_or_default();
    let (theme, path) = resolve_selection(&resources, selected, system_dark);
    apply_theme(&theme, cx);

    // Watch the custom theme file, if any.
    let (watcher, task) = match path {
        Some(path) => start_watcher(&theme.name, &path, cx),
        None => (None, None),
    };
    if cx.has_global::<ThemeState>() {
        let state = cx.global_mut::<ThemeState>();
        state.selected = selected.map(str::to_string);
        state.applied = theme.name.clone();
        state.watcher = watcher;
        state._reload_task = task;
    }
}

fn start_watcher(
    name: &str,
    path: &Path,
    cx: &mut App,
) -> (Option<ThemeWatcher>, Option<Task<()>>) {
    let (tx, rx) = async_channel::unbounded::<PiTheme>();
    let watcher = match ThemeWatcher::start(name, path, move |theme| {
        let _ = tx.try_send(theme);
    }) {
        Ok(Some(w)) => w,
        Ok(None) | Err(_) => return (None, None),
    };
    let expected = canonicalize_theme_name(name);
    let task = cx.spawn(async move |cx| {
        while let Ok(theme) = rx.recv().await {
            let still_selected = cx.update(|cx| {
                ThemeState::global(cx)
                    .is_some_and(|s| canonicalize_theme_name(&s.applied) == expected)
            });
            if !still_selected {
                break;
            }
            cx.update(|cx| apply_theme(&theme, cx));
        }
    });
    (Some(watcher), Some(task))
}

/// Switch gpui-component to the theme's mode and overlay its colours.
pub fn apply_theme(theme: &PiTheme, cx: &mut App) {
    let mode = if is_dark_theme(theme) {
        ThemeMode::Dark
    } else {
        ThemeMode::Light
    };
    UiTheme::change(mode, None, cx);
    UiTheme::update(cx, |t| apply_to_colors(theme, &mut t.colors));
    let palette = palette_for(theme, &UiTheme::global(cx).colors);
    cx.set_global(palette);
    cx.refresh_windows();
}

/// Follow a system appearance change when no theme is configured.
pub fn on_appearance_changed(cx: &mut App) {
    let selected = ThemeState::global(cx).and_then(|s| s.selected.clone());
    if selected.is_none() {
        apply_selection(None, cx);
    }
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{
        apply_to_colors, configured_theme_name, hsla_of, is_dark_theme, luminance, palette_for,
        resolve_selection,
    };
    use gpui_kit::component::ThemeColor as UiColors;
    use qt_core::resources::themes::{
        LoadedTheme, Rgb, ThemeResources, builtin_theme, parse_theme_document,
    };

    fn custom(text: &str, page: Option<&str>) -> qt_core::resources::themes::Theme {
        let dark = builtin_theme("dark").unwrap();
        let mut colors = serde_json::Map::new();
        for (token, color) in dark.palette.iter() {
            colors.insert(token.as_str().into(), serde_json::json!(color.to_hex("")));
        }
        colors.insert("text".into(), serde_json::json!(text));
        colors.insert("accent".into(), serde_json::json!("#ff0000"));
        colors.insert("userMessageBg".into(), serde_json::json!(""));
        let mut doc = serde_json::json!({ "name": "Custom", "colors": colors });
        if let Some(p) = page {
            doc["export"] = serde_json::json!({ "pageBg": p });
        }
        parse_theme_document("custom.json", &doc.to_string()).unwrap()
    }

    #[test]
    fn builtins_have_the_expected_mode() {
        assert!(is_dark_theme(&builtin_theme("dark").unwrap()));
        assert!(!is_dark_theme(&builtin_theme("light").unwrap()));
        assert!(
            luminance(Rgb {
                r: 255,
                g: 255,
                b: 255
            }) > 0.99
        );
        assert!(luminance(Rgb { r: 0, g: 0, b: 0 }) < 0.01);
    }

    #[test]
    fn custom_theme_mode_comes_from_page_or_text() {
        assert!(!is_dark_theme(&custom("#101010", Some("#fafafa"))));
        assert!(is_dark_theme(&custom("#101010", Some("#000000"))));
        // Without export colours or a user background the text colour decides.
        assert!(is_dark_theme(&custom("#eeeeee", None)));
        assert!(!is_dark_theme(&custom("#111111", None)));
    }

    #[test]
    fn colours_are_mapped_onto_the_component_theme() {
        let theme = builtin_theme("dark").unwrap();
        let mut colors = UiColors::default();
        apply_to_colors(&theme, &mut colors);
        let hex = |h: &str| hsla_of(Rgb::from_hex(h).unwrap());
        assert_eq!(colors.background, hex("#18181e"));
        assert_eq!(colors.popover, hex("#1e1e24"));
        assert_eq!(colors.primary, hex("#8abeb7"));
        assert_eq!(colors.border, hex("#505050"));
        assert_eq!(colors.danger, hex("#cc6666"));
        assert_eq!(colors.success, hex("#b5bd68"));
        assert_eq!(colors.secondary, hex("#343541"));
        assert_eq!(colors.list_active, hex("#3a3a4a"));
        // `text` is the terminal default: the component foreground stays.
        assert_eq!(colors.foreground, UiColors::default().foreground);

        let palette = palette_for(&theme, &colors);
        assert_eq!(palette.tool_success_bg, hex("#283228"));
        assert_eq!(palette.custom_label, hex("#9575cd"));
        assert_eq!(palette.thinking[5], hex("#d183e8"));
        assert_eq!(palette.user_fg, colors.foreground);
    }

    #[test]
    fn selection_prefers_settings_and_falls_back_to_system() {
        assert_eq!(
            configured_theme_name(Some("nord"), Some("dark")).as_deref(),
            Some("nord")
        );
        assert_eq!(
            configured_theme_name(None, Some("light")).as_deref(),
            Some("light")
        );
        assert_eq!(configured_theme_name(None, Some("system")), None);
        assert_eq!(configured_theme_name(Some("  "), None), None);

        let loaded = custom("#eeeeee", None);
        let resources = ThemeResources {
            themes: vec![LoadedTheme {
                root: "/t".into(),
                path: "/t/custom.json".into(),
                theme: loaded,
            }],
            ..Default::default()
        };
        let (theme, path) = resolve_selection(&resources, Some("CUSTOM"), false);
        assert_eq!(theme.name, "Custom");
        assert_eq!(
            path.as_deref(),
            Some(std::path::Path::new("/t/custom.json"))
        );
        let (theme, path) = resolve_selection(&resources, Some("missing"), true);
        assert_eq!(theme.name, "dark");
        assert!(path.is_none());
        let (theme, _) = resolve_selection(&resources, None, false);
        assert_eq!(theme.name, "light");
    }
}
