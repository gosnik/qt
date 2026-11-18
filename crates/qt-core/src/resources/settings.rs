//! Resource-related settings keys, read from the global
//! `<agent_home>/settings.json` and the project `<workspace>/.pi/settings.json`
//! (project values replace global ones).
//!
//! Keys (archived qt names): `promptRoots`/`prompts`, `skillRoots`/`skills`,
//! `themeRoots`/`themes` (paths resolved against the settings file's
//! directory, mixed with `!`/`+`/`-` filter entries), `features.loadPromptTemplates`,
//! `features.loadSkills`, `features.loadThemes`, `enableSkillCommands`,
//! `theme` and `shellPath`.

use std::fs;
use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

use super::filters::is_filter_entry;
use super::fsutil::{path_string, resolve_user_path};

/// Roots and filters from one `*Roots` array.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RootsSetting {
    /// `Some` when the array named at least one path (or was empty), which
    /// replaces the discovered default roots.
    pub roots: Option<Vec<PathBuf>>,
    /// `!glob`, `+exact` and `-exact` filter entries.
    pub filters: Option<Vec<String>>,
}

impl RootsSetting {
    fn overlay(&mut self, other: &RootsSetting) {
        if other.roots.is_some() {
            self.roots = other.roots.clone();
        }
        if other.filters.is_some() {
            self.filters = other.filters.clone();
        }
    }
}

/// Resource settings merged from the settings files.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ResourceSettings {
    pub prompts: RootsSetting,
    pub skills: RootsSetting,
    pub themes: RootsSetting,
    pub load_prompt_templates: Option<bool>,
    pub load_skills: Option<bool>,
    pub load_themes: Option<bool>,
    pub enable_skill_commands: Option<bool>,
    pub theme: Option<String>,
    pub shell_path: Option<String>,
}

impl ResourceSettings {
    /// Parse the resource keys of a settings object whose relative paths
    /// resolve against `base_directory`. Invalid values are reported in
    /// `diagnostics` and ignored.
    pub fn from_json(
        raw: &Map<String, Value>,
        base_directory: &Path,
        label: &str,
        diagnostics: &mut Vec<String>,
    ) -> Self {
        let mut out = Self::default();
        let mut diag =
            |message: String| diagnostics.push(format!("invalid settings in {label}: {message}"));
        out.prompts = parse_roots(raw, "promptRoots", "prompts", base_directory, &mut diag);
        out.skills = parse_roots(raw, "skillRoots", "skills", base_directory, &mut diag);
        out.themes = parse_roots(raw, "themeRoots", "themes", base_directory, &mut diag);
        match raw.get("features") {
            None => {}
            Some(Value::Object(features)) => {
                let mut flag = |key: &str| match features.get(key) {
                    None => None,
                    Some(Value::Bool(b)) => Some(*b),
                    Some(_) => {
                        diag(format!("features.{key} must be a boolean"));
                        None
                    }
                };
                out.load_prompt_templates = flag("loadPromptTemplates");
                out.load_skills = flag("loadSkills");
                out.load_themes = flag("loadThemes");
            }
            Some(_) => diag("features must be an object".to_string()),
        }
        match raw.get("enableSkillCommands") {
            None => {}
            Some(Value::Bool(b)) => out.enable_skill_commands = Some(*b),
            Some(_) => diag("enableSkillCommands must be a boolean".to_string()),
        }
        for (key, slot) in [
            ("theme", &mut out.theme),
            ("shellPath", &mut out.shell_path),
        ] {
            match raw.get(key) {
                None => {}
                Some(Value::String(s)) if !s.is_empty() => *slot = Some(s.clone()),
                Some(Value::String(_)) => {}
                Some(_) => diag(format!("{key} must be a string")),
            }
        }
        out
    }

    /// Read a settings file; a missing file yields defaults.
    pub fn load_file(path: &Path, base_directory: &Path, diagnostics: &mut Vec<String>) -> Self {
        let label = path_string(path);
        let text = match fs::read_to_string(path) {
            Ok(text) => text,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Self::default(),
            Err(err) => {
                diagnostics.push(format!("failed to read settings file {label}: {err}"));
                return Self::default();
            }
        };
        if text.trim().is_empty() {
            return Self::default();
        }
        match serde_json::from_str::<Value>(&text) {
            Ok(Value::Object(map)) => Self::from_json(&map, base_directory, &label, diagnostics),
            Ok(_) => {
                diagnostics.push(format!(
                    "invalid settings in {label}: settings document must be a JSON object"
                ));
                Self::default()
            }
            Err(err) => {
                diagnostics.push(format!("invalid settings JSON in {label}: {err}"));
                Self::default()
            }
        }
    }

    /// Values set in `other` replace values here.
    pub fn overlay(&mut self, other: &ResourceSettings) {
        self.prompts.overlay(&other.prompts);
        self.skills.overlay(&other.skills);
        self.themes.overlay(&other.themes);
        for (dst, src) in [
            (&mut self.load_prompt_templates, other.load_prompt_templates),
            (&mut self.load_skills, other.load_skills),
            (&mut self.load_themes, other.load_themes),
            (&mut self.enable_skill_commands, other.enable_skill_commands),
        ] {
            if src.is_some() {
                *dst = src;
            }
        }
        if other.theme.is_some() {
            self.theme = other.theme.clone();
        }
        if other.shell_path.is_some() {
            self.shell_path = other.shell_path.clone();
        }
    }
}

fn parse_roots(
    raw: &Map<String, Value>,
    preferred: &str,
    alias: &str,
    base: &Path,
    diag: &mut impl FnMut(String),
) -> RootsSetting {
    let (field, value) = match (raw.get(preferred), raw.get(alias)) {
        (Some(v), _) => (preferred, v),
        (None, Some(v)) => (alias, v),
        (None, None) => return RootsSetting::default(),
    };
    let Some(items) = value.as_array() else {
        diag(format!("{field} must be an array"));
        return RootsSetting::default();
    };
    let mut roots: Vec<PathBuf> = Vec::new();
    let mut filters = Vec::new();
    for (index, item) in items.iter().enumerate() {
        let Some(text) = item.as_str() else {
            diag(format!("{field}[{index}] must be a string"));
            continue;
        };
        if is_filter_entry(text) {
            filters.push(text.to_string());
            continue;
        }
        if text.is_empty() {
            diag(format!("{field} must not be empty"));
            continue;
        }
        let path = resolve_user_path(text, base);
        match fs::canonicalize(&path) {
            Ok(canonical) => {
                if !roots.contains(&canonical) {
                    roots.push(canonical);
                }
            }
            Err(_) => diag(format!(
                "{field} could not be canonicalized: {}",
                path_string(&path)
            )),
        }
    }
    // A filter-only array narrows the discovered roots instead of
    // replacing them with nothing.
    let roots = if roots.is_empty() && !filters.is_empty() {
        None
    } else {
        Some(roots)
    };
    RootsSetting {
        roots,
        filters: Some(filters),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_roots_filters_features_and_overlays() {
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir_all(dir.path().join("my-skills")).unwrap();
        let raw: Map<String, Value> = serde_json::from_str(
            r#"{"skills":["my-skills","!draft-*","missing",3],
                "themeRoots":["!x"],
                "prompts":"nope",
                "features":{"loadSkills":false,"loadThemes":"no"},
                "enableSkillCommands":false,"theme":"ocean","shellPath":"/bin/zsh"}"#,
        )
        .unwrap();
        let mut diagnostics = Vec::new();
        let s = ResourceSettings::from_json(&raw, dir.path(), "g.json", &mut diagnostics);
        let canonical = fs::canonicalize(dir.path().join("my-skills")).unwrap();
        assert_eq!(s.skills.roots, Some(vec![canonical]));
        assert_eq!(s.skills.filters, Some(vec!["!draft-*".to_string()]));
        assert_eq!(s.themes.roots, None);
        assert_eq!(s.themes.filters, Some(vec!["!x".to_string()]));
        assert_eq!(s.load_skills, Some(false));
        assert_eq!(s.load_themes, None);
        assert_eq!(s.enable_skill_commands, Some(false));
        assert_eq!(s.theme.as_deref(), Some("ocean"));
        assert_eq!(s.shell_path.as_deref(), Some("/bin/zsh"));
        let joined = diagnostics.join("\n");
        assert!(joined.contains("invalid settings in g.json: skills could not be canonicalized: "));
        assert!(joined.contains("skills[3] must be a string"));
        assert!(joined.contains("prompts must be an array"));
        assert!(joined.contains("features.loadThemes must be a boolean"));

        let mut base = ResourceSettings::default();
        base.overlay(&s);
        let project = ResourceSettings {
            load_skills: Some(true),
            ..Default::default()
        };
        base.overlay(&project);
        assert_eq!(base.load_skills, Some(true));
        assert_eq!(base.enable_skill_commands, Some(false));
    }
}
