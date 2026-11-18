//! JSON theme files (Pi's theme format) parsed into a typed palette.
//!
//! A theme document has a `name`, optional `vars` (named colours), a
//! `colors` object that must define all 51 tokens, and an optional partial
//! `export` object (HTML export colours). A colour is a `#rrggbb` hex
//! string, an ANSI-256 index (0–255), a variable name, or `""` for the
//! terminal default. Parsing and error wording follow the archived qt
//! (`app/config/themes.cpp`); the built-in `dark` and `light` themes are
//! Pi's.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

use super::fsutil::{
    expand_tilde, is_json, list_files, path_string, read_pi_manifest_entries,
    resolve_manifest_entry, trim,
};

macro_rules! theme_tokens {
    ($($variant:ident => $name:literal),* $(,)?) => {
        /// The 51 colour tokens every theme must define.
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
        pub enum ThemeToken { $($variant),* }

        impl ThemeToken {
            /// All tokens, in the archived qt's order.
            pub const ALL: [ThemeToken; 51] = [$(ThemeToken::$variant),*];

            /// The JSON key of the token.
            pub fn as_str(self) -> &'static str {
                match self { $(ThemeToken::$variant => $name),* }
            }

            pub fn from_name(name: &str) -> Option<Self> {
                match name { $($name => Some(ThemeToken::$variant),)* _ => None }
            }
        }
    };
}

theme_tokens! {
    Accent => "accent", Border => "border", BorderAccent => "borderAccent",
    BorderMuted => "borderMuted", Success => "success", Error => "error",
    Warning => "warning", Muted => "muted", Dim => "dim", Text => "text",
    ThinkingText => "thinkingText", SelectedBg => "selectedBg",
    UserMessageBg => "userMessageBg", UserMessageText => "userMessageText",
    CustomMessageBg => "customMessageBg", CustomMessageText => "customMessageText",
    CustomMessageLabel => "customMessageLabel", ToolPendingBg => "toolPendingBg",
    ToolSuccessBg => "toolSuccessBg", ToolErrorBg => "toolErrorBg",
    ToolTitle => "toolTitle", ToolOutput => "toolOutput", MdHeading => "mdHeading",
    MdLink => "mdLink", MdLinkUrl => "mdLinkUrl", MdCode => "mdCode",
    MdCodeBlock => "mdCodeBlock", MdCodeBlockBorder => "mdCodeBlockBorder",
    MdQuote => "mdQuote", MdQuoteBorder => "mdQuoteBorder", MdHr => "mdHr",
    MdListBullet => "mdListBullet", ToolDiffAdded => "toolDiffAdded",
    ToolDiffRemoved => "toolDiffRemoved", ToolDiffContext => "toolDiffContext",
    SyntaxComment => "syntaxComment", SyntaxKeyword => "syntaxKeyword",
    SyntaxFunction => "syntaxFunction", SyntaxVariable => "syntaxVariable",
    SyntaxString => "syntaxString", SyntaxNumber => "syntaxNumber",
    SyntaxType => "syntaxType", SyntaxOperator => "syntaxOperator",
    SyntaxPunctuation => "syntaxPunctuation", ThinkingOff => "thinkingOff",
    ThinkingMinimal => "thinkingMinimal", ThinkingLow => "thinkingLow",
    ThinkingMedium => "thinkingMedium", ThinkingHigh => "thinkingHigh",
    ThinkingXhigh => "thinkingXhigh", BashMode => "bashMode",
}

/// An 8-bit RGB colour.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct Rgb {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

impl Rgb {
    pub fn to_hex(self) -> String {
        format!("#{:02x}{:02x}{:02x}", self.r, self.g, self.b)
    }

    /// Parse `#rrggbb`.
    pub fn from_hex(hex: &str) -> Option<Self> {
        if !is_valid_hex_color(hex) {
            return None;
        }
        let c = |i: usize| u8::from_str_radix(&hex[i..i + 2], 16).ok();
        Some(Self {
            r: c(1)?,
            g: c(3)?,
            b: c(5)?,
        })
    }
}

/// A resolved theme colour.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ThemeColor {
    /// `""`: the default foreground/background.
    Default,
    /// A `#rrggbb` colour.
    Rgb(Rgb),
    /// An ANSI-256 palette index.
    Ansi(u8),
}

impl ThemeColor {
    /// RGB value; `None` for [`ThemeColor::Default`].
    pub fn rgb(self) -> Option<Rgb> {
        match self {
            ThemeColor::Default => None,
            ThemeColor::Rgb(rgb) => Some(rgb),
            ThemeColor::Ansi(index) => Some(ansi256_to_rgb(index)),
        }
    }

    /// Lowercase hex, or `default_hex` for [`ThemeColor::Default`].
    pub fn to_hex(self, default_hex: &str) -> String {
        self.rgb()
            .map_or_else(|| default_hex.to_string(), Rgb::to_hex)
    }
}

/// Every token resolved to a colour.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ThemePalette {
    colors: [ThemeColor; 51],
}

impl ThemePalette {
    pub fn get(&self, token: ThemeToken) -> ThemeColor {
        self.colors[token as usize]
    }

    pub fn iter(&self) -> impl Iterator<Item = (ThemeToken, ThemeColor)> + '_ {
        ThemeToken::ALL.iter().map(|t| (*t, self.get(*t)))
    }
}

/// Optional HTML-export colours.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ThemeExportColors {
    pub page_bg: Option<ThemeColor>,
    pub card_bg: Option<ThemeColor>,
    pub info_bg: Option<ThemeColor>,
    /// Every key of the `export` object, resolved.
    pub all: BTreeMap<String, ThemeColor>,
}

/// A parsed theme document.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Theme {
    pub name: String,
    /// Lowercased name used for lookups.
    pub canonical_name: String,
    pub palette: ThemePalette,
    /// All `colors` keys, including any beyond the 51 tokens.
    pub colors: BTreeMap<String, ThemeColor>,
    pub export: ThemeExportColors,
}

impl Theme {
    pub fn color(&self, token: ThemeToken) -> ThemeColor {
        self.palette.get(token)
    }
}

const BUILTIN_DARK: &str = include_str!("builtin/dark.json");
const BUILTIN_LIGHT: &str = include_str!("builtin/light.json");

/// Names of the built-in themes.
pub const BUILTIN_THEME_NAMES: [&str; 2] = ["dark", "light"];

/// A built-in theme (`dark` or `light`).
pub fn builtin_theme(name: &str) -> Option<Theme> {
    let source = match canonicalize_theme_name(name).as_str() {
        "dark" => BUILTIN_DARK,
        "light" => BUILTIN_LIGHT,
        _ => return None,
    };
    parse_theme_document(name, source).ok()
}

/// Case-folded theme name used for lookups.
pub fn canonicalize_theme_name(name: &str) -> String {
    name.to_lowercase()
}

pub fn is_valid_hex_color(value: &str) -> bool {
    value.len() == 7 && value.starts_with('#') && value[1..].bytes().all(|b| b.is_ascii_hexdigit())
}

fn color_value(
    value: &Value,
    path: &str,
    vars: Option<&Map<String, Value>>,
    resolving: &mut Vec<String>,
) -> Result<ThemeColor, String> {
    if value.is_i64() || value.is_u64() {
        return match value.as_i64() {
            Some(i) if (0..=255).contains(&i) => Ok(ThemeColor::Ansi(i as u8)),
            _ => Err(format!("{path} must be an integer from 0 to 255")),
        };
    }
    let Some(text) = value.as_str() else {
        return Err(format!(
            "{path} must be a string or an integer from 0 to 255"
        ));
    };
    let text = trim(text);
    if text.is_empty() {
        return Ok(ThemeColor::Default);
    }
    if text.starts_with('#') {
        return Rgb::from_hex(&text.to_ascii_lowercase())
            .map(ThemeColor::Rgb)
            .ok_or_else(|| format!("{path} must be a 6-digit hex color"));
    }
    let Some(target) = vars.and_then(|v| v.get(text)) else {
        return Err(format!("{path} references unknown variable {text}"));
    };
    if resolving.iter().any(|r| r == text) {
        return Err(format!(
            "{path} has a circular variable reference at {text}"
        ));
    }
    resolving.push(text.to_string());
    let resolved = color_value(target, &format!("vars.{text}"), vars, resolving);
    resolving.pop();
    resolved
}

fn color_map(
    colors: &Value,
    vars: Option<&Map<String, Value>>,
    prefix: &str,
    required: bool,
) -> Result<BTreeMap<String, ThemeColor>, String> {
    let Some(object) = colors.as_object() else {
        return Err(format!("{prefix} must be an object"));
    };
    if required {
        let missing: Vec<&str> = ThemeToken::ALL
            .iter()
            .map(|t| t.as_str())
            .filter(|k| !object.contains_key(*k))
            .collect();
        if !missing.is_empty() {
            return Err(format!(
                "{prefix} is missing required tokens: {}",
                missing.join(" ")
            ));
        }
    }
    let mut out = BTreeMap::new();
    for (key, value) in object {
        let mut resolving = Vec::new();
        let color = color_value(value, &format!("{prefix}.{key}"), vars, &mut resolving)?;
        out.insert(key.clone(), color);
    }
    Ok(out)
}

fn require_string(doc: &Map<String, Value>, field: &str) -> Result<String, String> {
    match doc.get(field) {
        None => Err(format!("{field} is required")),
        Some(Value::String(s)) => {
            let s = trim(s);
            if s.is_empty() {
                Err(format!("{field} must not be empty"))
            } else {
                Ok(s.to_string())
            }
        }
        Some(_) => Err(format!("{field} must be a string")),
    }
}

/// Parse a theme document; `label` names it in JSON syntax errors.
pub fn parse_theme_document(label: &str, content: &str) -> Result<Theme, String> {
    let document: Value =
        serde_json::from_str(content).map_err(|e| format!("failed to parse theme {label}: {e}"))?;
    let Some(doc) = document.as_object() else {
        return Err("theme document must be an object".to_string());
    };
    let name = require_string(doc, "name")?;
    let vars = match doc.get("vars") {
        None => None,
        Some(Value::Object(map)) => Some(map),
        Some(_) => return Err("vars must be an object".to_string()),
    };
    let colors_value = match doc.get("colors") {
        None => return Err("colors is required".to_string()),
        Some(v) if v.is_object() => v,
        Some(_) => return Err("colors must be an object".to_string()),
    };
    let colors = color_map(colors_value, vars, "colors", true)?;
    let mut palette = [ThemeColor::Default; 51];
    for token in ThemeToken::ALL {
        palette[token as usize] = colors[token.as_str()];
    }
    let mut export = ThemeExportColors::default();
    if let Some(value) = doc.get("export") {
        export.all = color_map(value, vars, "export", false)?;
        export.page_bg = export.all.get("pageBg").copied();
        export.card_bg = export.all.get("cardBg").copied();
        export.info_bg = export.all.get("infoBg").copied();
    }
    Ok(Theme {
        canonical_name: canonicalize_theme_name(&name),
        name,
        palette: ThemePalette { colors: palette },
        colors,
        export,
    })
}

const ANSI_BASIC: [Rgb; 16] = [
    Rgb {
        r: 0x00,
        g: 0x00,
        b: 0x00,
    },
    Rgb {
        r: 0x80,
        g: 0x00,
        b: 0x00,
    },
    Rgb {
        r: 0x00,
        g: 0x80,
        b: 0x00,
    },
    Rgb {
        r: 0x80,
        g: 0x80,
        b: 0x00,
    },
    Rgb {
        r: 0x00,
        g: 0x00,
        b: 0x80,
    },
    Rgb {
        r: 0x80,
        g: 0x00,
        b: 0x80,
    },
    Rgb {
        r: 0x00,
        g: 0x80,
        b: 0x80,
    },
    Rgb {
        r: 0xc0,
        g: 0xc0,
        b: 0xc0,
    },
    Rgb {
        r: 0x80,
        g: 0x80,
        b: 0x80,
    },
    Rgb {
        r: 0xff,
        g: 0x00,
        b: 0x00,
    },
    Rgb {
        r: 0x00,
        g: 0xff,
        b: 0x00,
    },
    Rgb {
        r: 0xff,
        g: 0xff,
        b: 0x00,
    },
    Rgb {
        r: 0x00,
        g: 0x00,
        b: 0xff,
    },
    Rgb {
        r: 0xff,
        g: 0x00,
        b: 0xff,
    },
    Rgb {
        r: 0x00,
        g: 0xff,
        b: 0xff,
    },
    Rgb {
        r: 0xff,
        g: 0xff,
        b: 0xff,
    },
];

/// The xterm 256-colour palette entry for `index`.
pub fn ansi256_to_rgb(index: u8) -> Rgb {
    match index {
        0..=15 => ANSI_BASIC[index as usize],
        16..=231 => {
            let i = index - 16;
            let level = |c: u8| if c == 0 { 0 } else { 55 + c * 40 };
            Rgb {
                r: level(i / 36),
                g: level((i % 36) / 6),
                b: level(i % 6),
            }
        }
        232..=255 => {
            let gray = 8 + (index - 232) * 10;
            Rgb {
                r: gray,
                g: gray,
                b: gray,
            }
        }
    }
}

/// Nearest ANSI-256 index (colour cube, or the grey ramp for near-greys).
pub fn rgb_to_ansi256(rgb: Rgb) -> u8 {
    const CUBE: [i32; 6] = [0, 95, 135, 175, 215, 255];
    fn closest(value: i32, candidates: impl Iterator<Item = i32>) -> usize {
        let mut best = (0usize, i32::MAX);
        for (i, c) in candidates.enumerate() {
            let d = (value - c).abs();
            if d < best.1 {
                best = (i, d);
            }
        }
        best.0
    }
    fn distance(a: (i32, i32, i32), b: (i32, i32, i32)) -> f64 {
        let (dr, dg, db) = ((a.0 - b.0) as f64, (a.1 - b.1) as f64, (a.2 - b.2) as f64);
        dr * dr * 0.299 + dg * dg * 0.587 + db * db * 0.114
    }
    let c = (rgb.r as i32, rgb.g as i32, rgb.b as i32);
    let (ri, gi, bi) = (
        closest(c.0, CUBE.iter().copied()),
        closest(c.1, CUBE.iter().copied()),
        closest(c.2, CUBE.iter().copied()),
    );
    let cube_index = 16 + 36 * ri + 6 * gi + bi;
    let cube_distance = distance(c, (CUBE[ri], CUBE[gi], CUBE[bi]));
    let gray = (0.299 * c.0 as f64 + 0.587 * c.1 as f64 + 0.114 * c.2 as f64).round() as i32;
    let gi2 = closest(gray, (0..24).map(|i| 8 + i * 10));
    let gv = 8 + gi2 as i32 * 10;
    let gray_distance = distance(c, (gv, gv, gv));
    let spread = c.0.max(c.1).max(c.2) - c.0.min(c.1).min(c.2);
    if spread < 10 && gray_distance < cube_distance {
        (232 + gi2) as u8
    } else {
        cube_index as u8
    }
}

// ---- discovery --------------------------------------------------------------

/// A theme file loaded from a theme root.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LoadedTheme {
    pub root: PathBuf,
    pub path: PathBuf,
    pub theme: Theme,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ThemeResources {
    pub roots: Vec<PathBuf>,
    pub themes: Vec<LoadedTheme>,
    pub diagnostics: Vec<String>,
}

impl ThemeResources {
    /// Resolve a theme by name: built-ins first (as in Pi), then the first
    /// loaded theme with the same canonical name.
    pub fn resolve(&self, name: &str) -> Option<Theme> {
        if let Some(theme) = builtin_theme(name) {
            return Some(theme);
        }
        let canonical = canonicalize_theme_name(name);
        self.themes
            .iter()
            .find(|t| t.theme.canonical_name == canonical)
            .map(|t| t.theme.clone())
    }

    /// File backing a custom theme, if any (built-ins have none).
    pub fn path_of(&self, name: &str) -> Option<&Path> {
        if builtin_theme(name).is_some() {
            return None;
        }
        let canonical = canonicalize_theme_name(name);
        self.themes
            .iter()
            .find(|t| t.theme.canonical_name == canonical)
            .map(|t| t.path.as_path())
    }

    /// Built-in names followed by loaded theme names, sorted and unique.
    pub fn available_names(&self) -> Vec<String> {
        let mut names: Vec<String> = BUILTIN_THEME_NAMES.iter().map(|s| s.to_string()).collect();
        for t in &self.themes {
            if !names
                .iter()
                .any(|n| canonicalize_theme_name(n) == t.theme.canonical_name)
            {
                names.push(t.theme.name.clone());
            }
        }
        names.sort();
        names
    }
}

/// Read and parse a theme file. `Err` carries the archived diagnostic.
pub fn load_theme_file(path: &Path) -> Result<Theme, String> {
    let theme_path = path_string(path);
    if !path.is_file() {
        return Err(format!("theme entry is not a readable file: {theme_path}"));
    }
    let content =
        fs::read_to_string(path).map_err(|_| format!("failed to read theme file: {theme_path}"))?;
    parse_theme_document(&theme_path, &content).map_err(|e| format!("{e}: {theme_path}"))
}

fn load_into(root: &Path, path: &Path, out: &mut ThemeResources) {
    match load_theme_file(path) {
        Ok(theme) => out.themes.push(LoadedTheme {
            root: root.to_path_buf(),
            path: PathBuf::from(path_string(path)),
            theme,
        }),
        Err(message) => out.diagnostics.push(message),
    }
}

fn load_package_themes(root: &Path, directory: &Path, out: &mut ThemeResources) -> bool {
    let package_json = directory.join("package.json");
    if !package_json.is_file() {
        return false;
    }
    let declared = match read_pi_manifest_entries(&package_json, "themes") {
        Ok(entries) => entries,
        Err(message) => {
            out.diagnostics.push(message);
            Vec::new()
        }
    };
    if declared.is_empty() {
        return false;
    }
    for entry in declared {
        let resolved = resolve_manifest_entry(directory, &entry);
        if resolved.is_dir() {
            for file in list_files(&resolved, is_json) {
                load_into(root, &file, out);
            }
        } else if resolved.is_file() && is_json(&resolved) {
            load_into(root, &resolved, out);
        } else {
            out.diagnostics.push(format!(
                "package theme entry does not exist: {}",
                path_string(&resolved)
            ));
        }
    }
    true
}

/// Load `*.json` themes from `roots` (directories, single files or
/// packages with `pi.themes`).
pub fn load_themes(roots: &[PathBuf]) -> ThemeResources {
    let mut out = ThemeResources {
        roots: roots.to_vec(),
        ..Default::default()
    };
    for root in roots {
        if root.as_os_str().is_empty() {
            out.diagnostics.push("theme root was empty".to_string());
            continue;
        }
        let root_path = expand_tilde(&root.to_string_lossy());
        let resolved = path_string(&root_path);
        if root_path.is_file() && is_json(&root_path) {
            let parent = root_path.parent().map(path_string).unwrap_or_default();
            load_into(Path::new(&parent), &root_path, &mut out);
            continue;
        }
        if !root_path.exists() {
            out.diagnostics
                .push(format!("theme root does not exist: {resolved}"));
            continue;
        }
        if !root_path.is_dir() {
            out.diagnostics
                .push(format!("theme root is not a directory: {resolved}"));
            continue;
        }
        let resolved_root = PathBuf::from(&resolved);
        if load_package_themes(&resolved_root, &root_path, &mut out) {
            continue;
        }
        for file in list_files(&root_path, is_json) {
            load_into(&resolved_root, &file, &mut out);
        }
    }
    out
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A complete theme document with every token set to `value`.
    pub(crate) fn theme_json(name: &str, value: &str) -> String {
        let colors: Map<String, Value> = ThemeToken::ALL
            .iter()
            .map(|t| (t.as_str().to_string(), Value::String(value.to_string())))
            .collect();
        serde_json::json!({ "name": name, "vars": { "v": "#ABCDEF", "w": "v", "n": 33 }, "colors": colors })
            .to_string()
    }

    #[test]
    fn builtins_parse_with_all_tokens() {
        assert_eq!(ThemeToken::ALL.len(), 51);
        for name in BUILTIN_THEME_NAMES {
            let theme = builtin_theme(name).unwrap();
            assert_eq!(theme.name, name);
            assert_eq!(theme.colors.len(), 51);
        }
        let dark = builtin_theme("Dark").unwrap();
        assert_eq!(
            dark.color(ThemeToken::Accent),
            ThemeColor::Rgb(Rgb::from_hex("#8abeb7").unwrap())
        );
        assert_eq!(dark.color(ThemeToken::Text), ThemeColor::Default);
        assert_eq!(dark.color(ThemeToken::SyntaxKeyword).to_hex(""), "#569cd6");
        assert_eq!(dark.export.page_bg.unwrap().to_hex(""), "#18181e");
        assert_eq!(
            ThemeToken::from_name("bashMode"),
            Some(ThemeToken::BashMode)
        );
        assert_eq!(ThemeToken::BashMode.as_str(), "bashMode");
    }

    #[test]
    fn vars_ansi_and_errors() {
        let theme = parse_theme_document("t", &theme_json("Mine", "w")).unwrap();
        assert_eq!(theme.canonical_name, "mine");
        assert_eq!(theme.color(ThemeToken::Error).to_hex(""), "#abcdef");
        let theme = parse_theme_document("t", &theme_json("Mine", "n")).unwrap();
        assert_eq!(theme.color(ThemeToken::Error), ThemeColor::Ansi(33));

        let err = |json: &str| parse_theme_document("lbl", json).unwrap_err();
        assert!(err("{").starts_with("failed to parse theme lbl: "));
        assert_eq!(err("[]"), "theme document must be an object");
        assert_eq!(err("{}"), "name is required");
        assert_eq!(err(r#"{"name":1}"#), "name must be a string");
        assert_eq!(err(r#"{"name":"  "}"#), "name must not be empty");
        assert_eq!(err(r#"{"name":"x"}"#), "colors is required");
        assert_eq!(
            err(r#"{"name":"x","colors":[]}"#),
            "colors must be an object"
        );
        assert_eq!(
            err(r#"{"name":"x","vars":1,"colors":{}}"#),
            "vars must be an object"
        );
        let missing = err(r#"{"name":"x","colors":{"accent":""}}"#);
        assert!(missing.starts_with("colors is missing required tokens: border borderAccent"));
        assert!(missing.ends_with("thinkingXhigh bashMode"));

        let with = |accent: &str| {
            let mut doc: Value = serde_json::from_str(&theme_json("x", "")).unwrap();
            doc["colors"]["accent"] = serde_json::from_str(accent).unwrap();
            doc["vars"]["loop1"] = Value::String("loop2".into());
            doc["vars"]["loop2"] = Value::String("loop1".into());
            parse_theme_document("x", &doc.to_string())
        };
        assert_eq!(
            with("256").unwrap_err(),
            "colors.accent must be an integer from 0 to 255"
        );
        assert_eq!(
            with("-1").unwrap_err(),
            "colors.accent must be an integer from 0 to 255"
        );
        assert_eq!(
            with("1.5").unwrap_err(),
            "colors.accent must be a string or an integer from 0 to 255"
        );
        assert_eq!(
            with("\"#12345\"").unwrap_err(),
            "colors.accent must be a 6-digit hex color"
        );
        assert_eq!(
            with("\"nope\"").unwrap_err(),
            "colors.accent references unknown variable nope"
        );
        assert_eq!(
            with("\"loop1\"").unwrap_err(),
            "vars.loop2 has a circular variable reference at loop1"
        );
        assert_eq!(
            with("\"#FFAA00\"")
                .unwrap()
                .color(ThemeToken::Accent)
                .to_hex(""),
            "#ffaa00"
        );
    }

    #[test]
    fn ansi_conversions() {
        assert_eq!(ansi256_to_rgb(9).to_hex(), "#ff0000");
        assert_eq!(ansi256_to_rgb(16).to_hex(), "#000000");
        assert_eq!(ansi256_to_rgb(21).to_hex(), "#0000ff");
        assert_eq!(ansi256_to_rgb(196).to_hex(), "#ff0000");
        assert_eq!(ansi256_to_rgb(232).to_hex(), "#080808");
        assert_eq!(ansi256_to_rgb(255).to_hex(), "#eeeeee");
        assert_eq!(rgb_to_ansi256(Rgb::from_hex("#ff0000").unwrap()), 196);
        assert_eq!(rgb_to_ansi256(Rgb::from_hex("#808080").unwrap()), 244);
        assert_eq!(rgb_to_ansi256(Rgb::from_hex("#5f87ff").unwrap()), 69);
    }

    #[test]
    fn loads_theme_roots_and_resolves() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("themes");
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("ocean.json"), theme_json("Ocean", "#112233")).unwrap();
        fs::write(root.join("broken.json"), "{").unwrap();
        fs::write(root.join("dark.json"), theme_json("dark", "#000000")).unwrap();
        fs::write(root.join("readme.txt"), "x").unwrap();
        let res = load_themes(&[root.clone(), PathBuf::new()]);
        assert_eq!(res.themes.len(), 2);
        assert!(
            res.diagnostics
                .iter()
                .any(|d| d.starts_with("failed to parse theme ")
                    && d.ends_with(&format!(": {}", root.join("broken.json").display())))
        );
        assert!(
            res.diagnostics
                .contains(&"theme root was empty".to_string())
        );
        let ocean = res.resolve("OCEAN").unwrap();
        assert_eq!(ocean.color(ThemeToken::Accent).to_hex(""), "#112233");
        assert_eq!(
            res.path_of("ocean"),
            Some(root.join("ocean.json").as_path())
        );
        // Built-ins win over a custom file with the same name.
        assert_eq!(
            res.resolve("dark")
                .unwrap()
                .color(ThemeToken::Accent)
                .to_hex(""),
            "#8abeb7"
        );
        assert!(res.path_of("dark").is_none());
        assert_eq!(res.available_names(), vec!["Ocean", "dark", "light"]);
    }
}
