//! Keyboard helpers for extensions: `registerShortcut` key specs matched
//! against GPUI keystrokes, and keystrokes encoded as the terminal input
//! data (`{type:"input", data}`) Pi components expect.

/// A parsed shortcut such as `ctrl+shift+p` or `alt+x`.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Shortcut {
    pub control: bool,
    pub alt: bool,
    pub shift: bool,
    /// Cmd on macOS, Super/Windows elsewhere.
    pub platform: bool,
    /// Lower-case GPUI key name (`p`, `enter`, `f5`, `up`).
    pub key: String,
}

fn key_alias(key: &str) -> String {
    match key {
        "return" => "enter".into(),
        "esc" => "escape".into(),
        "del" => "delete".into(),
        "pgup" | "pageup" => "pageup".into(),
        "pgdn" | "pagedown" => "pagedown".into(),
        "spacebar" => "space".into(),
        other => other.into(),
    }
}

/// Parse a Pi / archived key spec (`+` or `-` separated, case-insensitive).
pub fn parse_shortcut(spec: &str) -> Option<Shortcut> {
    let spec = spec.trim().to_ascii_lowercase();
    if spec.is_empty() {
        return None;
    }
    // A trailing separator is the key itself ("ctrl++", "ctrl+-").
    let bytes = spec.as_bytes();
    let separator = |b: u8| b == b'+' || b == b'-';
    let (mods, key) = if spec.len() >= 2
        && separator(bytes[spec.len() - 1])
        && separator(bytes[spec.len() - 2])
    {
        (&spec[..spec.len() - 2], &spec[spec.len() - 1..])
    } else if spec.len() == 1 {
        ("", spec.as_str())
    } else {
        match spec.rfind(['+', '-']) {
            Some(i) if i + 1 < spec.len() => (&spec[..i], &spec[i + 1..]),
            Some(_) => return None,
            None => ("", spec.as_str()),
        }
    };
    let mut shortcut = Shortcut {
        key: key_alias(key),
        ..Default::default()
    };
    for part in mods.split(['+', '-']).filter(|p| !p.is_empty()) {
        match part {
            "ctrl" | "control" => shortcut.control = true,
            "alt" | "option" | "opt" | "meta" => shortcut.alt = true,
            "shift" => shortcut.shift = true,
            "cmd" | "super" | "win" | "command" | "platform" => shortcut.platform = true,
            _ => return None,
        }
    }
    Some(shortcut)
}

/// The modifier state and key of a keystroke, as plain values (so this
/// module stays independent of GPUI types in tests).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Pressed {
    pub control: bool,
    pub alt: bool,
    pub shift: bool,
    pub platform: bool,
    pub key: String,
    /// The typed character, when any.
    pub key_char: Option<String>,
}

/// Whether `pressed` is `shortcut`.
pub fn shortcut_matches(shortcut: &Shortcut, pressed: &Pressed) -> bool {
    shortcut.control == pressed.control
        && shortcut.alt == pressed.alt
        && shortcut.shift == pressed.shift
        && shortcut.platform == pressed.platform
        && shortcut.key == key_alias(&pressed.key.to_ascii_lowercase())
}

/// Terminal input data for a keystroke (`\r` for Enter, escape sequences
/// for arrows, control bytes for Ctrl+letter, the typed text otherwise).
pub fn input_data(pressed: &Pressed) -> Option<String> {
    let key = pressed.key.as_str();
    let special = match key {
        "enter" => Some("\r"),
        "backspace" => Some("\x7f"),
        "tab" if pressed.shift => Some("\x1b[Z"),
        "tab" => Some("\t"),
        "escape" => Some("\x1b"),
        "up" => Some("\x1b[A"),
        "down" => Some("\x1b[B"),
        "right" => Some("\x1b[C"),
        "left" => Some("\x1b[D"),
        "home" => Some("\x1b[H"),
        "end" => Some("\x1b[F"),
        "delete" => Some("\x1b[3~"),
        "pageup" => Some("\x1b[5~"),
        "pagedown" => Some("\x1b[6~"),
        _ => None,
    };
    if let Some(s) = special {
        return Some(s.to_string());
    }
    if pressed.control && !pressed.alt && key.len() == 1 {
        let c = key.as_bytes()[0];
        if c.is_ascii_lowercase() {
            return Some(((c - b'a' + 1) as char).to_string());
        }
    }
    if pressed.platform || pressed.control {
        return None;
    }
    let text = pressed
        .key_char
        .clone()
        .or_else(|| (key == "space").then(|| " ".to_string()))
        .or_else(|| (key.chars().count() == 1).then(|| key.to_string()))?;
    Some(if pressed.alt {
        format!("\x1b{text}")
    } else {
        text
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pressed(key: &str) -> Pressed {
        Pressed {
            key: key.into(),
            ..Default::default()
        }
    }

    #[test]
    fn parses_and_matches_shortcuts() {
        let s = parse_shortcut("Ctrl+Shift+P").unwrap();
        assert!(s.control && s.shift && !s.alt);
        assert_eq!(s.key, "p");
        assert_eq!(parse_shortcut("alt-x").unwrap().key, "x");
        assert_eq!(parse_shortcut("ctrl++").unwrap().key, "+");
        assert_eq!(
            parse_shortcut("f5").unwrap(),
            Shortcut {
                key: "f5".into(),
                ..Default::default()
            }
        );
        assert_eq!(parse_shortcut("ctrl+return").unwrap().key, "enter");
        assert!(parse_shortcut("hyper+x").is_none());
        assert!(parse_shortcut("").is_none());

        let mut p = pressed("p");
        p.control = true;
        p.shift = true;
        assert!(shortcut_matches(&s, &p));
        p.alt = true;
        assert!(!shortcut_matches(&s, &p));
    }

    #[test]
    fn encodes_component_input() {
        assert_eq!(input_data(&pressed("enter")).unwrap(), "\r");
        assert_eq!(input_data(&pressed("up")).unwrap(), "\x1b[A");
        assert_eq!(input_data(&pressed("escape")).unwrap(), "\x1b");
        let mut c = pressed("c");
        c.control = true;
        assert_eq!(input_data(&c).unwrap(), "\x03");
        let mut a = pressed("a");
        a.key_char = Some("A".into());
        assert_eq!(input_data(&a).unwrap(), "A");
        assert_eq!(input_data(&pressed("space")).unwrap(), " ");
        let mut alt = pressed("x");
        alt.alt = true;
        assert_eq!(input_data(&alt).unwrap(), "\x1bx");
        let mut cmd = pressed("c");
        cmd.platform = true;
        assert_eq!(input_data(&cmd), None);
        assert_eq!(input_data(&pressed("f5")), None);
    }
}
