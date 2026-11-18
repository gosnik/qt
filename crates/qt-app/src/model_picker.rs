//! Items for the model and thinking-level selectors, scoped models
//! (`enabledModels`) and thinking-level clamping.

use gpui_kit::component::searchable_list::SearchableListItem;
use gpui_kit::*;
use qt_core::Settings;
use qt_core::provider::{ModelInfo, ModelRef, ThinkingLevel};
use serde_json::Value;

/// One selectable model; the value is the `provider/id` spec.
#[derive(Clone, Debug, PartialEq)]
pub struct ModelItem {
    pub title: SharedString,
    pub value: SharedString,
    pub context_window: Option<u64>,
}

impl ModelItem {
    pub fn from_info(info: &ModelInfo, provider_label: &str) -> Self {
        let name = if info.name == info.id {
            info.id.clone()
        } else {
            format!("{} ({})", info.name, info.id)
        };
        Self {
            title: format!("{provider_label} · {name}").into(),
            value: info.model_ref().to_string().into(),
            context_window: info.context_window,
        }
    }

    /// Placeholder for a model that is selected but not (yet) listed.
    pub fn from_ref(model: &ModelRef) -> Self {
        Self {
            title: format!("{} · {}", model.provider, model.id).into(),
            value: model.to_string().into(),
            context_window: None,
        }
    }
}

impl SearchableListItem for ModelItem {
    type Value = SharedString;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &SharedString {
        &self.value
    }

    fn matches(&self, query: &str) -> bool {
        let query = query.to_lowercase();
        query.split_whitespace().all(|word| {
            self.title.to_lowercase().contains(word) || self.value.to_lowercase().contains(word)
        })
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ThinkingItem {
    pub title: SharedString,
    pub value: SharedString,
}

impl ThinkingItem {
    pub fn all() -> Vec<Self> {
        Self::for_levels(&ThinkingLevel::ALL)
    }

    pub fn for_levels(levels: &[ThinkingLevel]) -> Vec<Self> {
        levels
            .iter()
            .map(|level| Self {
                title: thinking_title(*level).into(),
                value: level.as_str().into(),
            })
            .collect()
    }
}

pub fn thinking_title(level: ThinkingLevel) -> &'static str {
    match level {
        ThinkingLevel::Off => "Thinking: off",
        ThinkingLevel::Minimal => "Thinking: minimal",
        ThinkingLevel::Low => "Thinking: low",
        ThinkingLevel::Medium => "Thinking: medium",
        ThinkingLevel::High => "Thinking: high",
        ThinkingLevel::XHigh => "Thinking: xhigh",
    }
}

impl SearchableListItem for ThinkingItem {
    type Value = SharedString;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &SharedString {
        &self.value
    }
}

/// Thinking levels offered for a model: its own list when known, else all.
pub fn levels_for(model: Option<&ModelInfo>) -> Vec<ThinkingLevel> {
    match model {
        Some(m) if !m.thinking_levels.is_empty() => m.thinking_levels.clone(),
        Some(m) if !m.reasoning => vec![ThinkingLevel::Off],
        _ => ThinkingLevel::ALL.to_vec(),
    }
}

/// Nearest available level, searching upwards first (Pi `clampThinkingLevel`).
pub fn clamp_level(level: ThinkingLevel, available: &[ThinkingLevel]) -> ThinkingLevel {
    if available.is_empty() || available.contains(&level) {
        return level;
    }
    let all = ThinkingLevel::ALL;
    let index = all.iter().position(|l| *l == level).unwrap_or(0);
    all[index..]
        .iter()
        .chain(all[..index].iter().rev())
        .find(|l| available.contains(l))
        .copied()
        .unwrap_or(ThinkingLevel::Off)
}

/// `enabledModels` from settings (scoped models; empty means all).
pub fn enabled_models(settings: &Settings) -> Vec<String> {
    settings
        .raw()
        .get("enabledModels")
        .and_then(Value::as_array)
        .map(|list| {
            list.iter()
                .filter_map(Value::as_str)
                .map(str::trim)
                .filter(|s| !s.is_empty())
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

/// Whether `spec` (`provider/id`) matches a scoped-model pattern. Patterns
/// are exact specs, bare model ids, or globs with `*` (Pi `enabledModels`).
pub fn pattern_matches(pattern: &str, spec: &str) -> bool {
    let pattern = pattern.trim().to_lowercase();
    let spec = spec.to_lowercase();
    let id = spec.split_once('/').map(|(_, id)| id).unwrap_or(&spec);
    if !pattern.contains('*') {
        return pattern == spec || pattern == id;
    }
    glob(&pattern, &spec) || (!pattern.contains('/') && glob(&pattern, id))
}

fn glob(pattern: &str, text: &str) -> bool {
    let parts: Vec<&str> = pattern.split('*').collect();
    let mut rest = text;
    for (i, part) in parts.iter().enumerate() {
        if i == 0 {
            let Some(stripped) = rest.strip_prefix(part) else {
                return false;
            };
            rest = stripped;
        } else if i == parts.len() - 1 {
            return rest.ends_with(part);
        } else if let Some(pos) = rest.find(part) {
            rest = &rest[pos + part.len()..];
        } else {
            return false;
        }
    }
    rest.is_empty()
}

/// Whether a model is in scope (`patterns` empty means every model).
pub fn in_scope(patterns: &[String], spec: &str) -> bool {
    patterns.is_empty() || patterns.iter().any(|p| pattern_matches(p, spec))
}

/// Toggle `spec` in the scoped list (archived `/scoped-models`): an empty
/// list first expands to every available model; the result is sorted.
pub fn toggle_scoped(
    current: &[String],
    available: &[String],
    spec: &str,
    on: bool,
) -> Vec<String> {
    let mut list: Vec<String> = if current.is_empty() {
        available.to_vec()
    } else {
        current.to_vec()
    };
    list.retain(|s| s != spec);
    if on {
        list.push(spec.to_string());
    }
    list.sort();
    list.dedup();
    list
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{
        clamp_level, enabled_models, in_scope, levels_for, pattern_matches, toggle_scoped,
    };
    use qt_core::Settings;
    use qt_core::provider::{ModelInfo, ThinkingLevel as L};

    #[test]
    fn clamps_upwards_first() {
        let available = [L::Off, L::Low, L::Medium, L::High];
        assert_eq!(clamp_level(L::Minimal, &available), L::Low);
        assert_eq!(clamp_level(L::XHigh, &available), L::High);
        assert_eq!(clamp_level(L::Medium, &available), L::Medium);
        assert_eq!(clamp_level(L::High, &[L::Off]), L::Off);
        assert_eq!(clamp_level(L::High, &[]), L::High);
    }

    #[test]
    fn levels_come_from_the_model() {
        assert_eq!(levels_for(None).len(), 6);
        let mut info = ModelInfo {
            provider: "p".into(),
            id: "m".into(),
            name: "m".into(),
            context_window: None,
            max_tokens: None,
            reasoning: false,
            vision: false,
            thinking_levels: Vec::new(),
        };
        assert_eq!(levels_for(Some(&info)), [L::Off]);
        info.reasoning = true;
        info.thinking_levels = vec![L::Off, L::Low, L::High, L::XHigh];
        assert_eq!(levels_for(Some(&info)), [L::Off, L::Low, L::High, L::XHigh]);
    }

    #[test]
    fn scoped_patterns() {
        assert!(pattern_matches(
            "anthropic/claude-sonnet-4",
            "anthropic/claude-sonnet-4"
        ));
        assert!(pattern_matches(
            "claude-sonnet-4",
            "anthropic/claude-sonnet-4"
        ));
        assert!(pattern_matches("anthropic/*", "anthropic/claude-x"));
        assert!(pattern_matches("*sonnet*", "anthropic/claude-sonnet-4"));
        assert!(pattern_matches("gpt-*", "openai/gpt-5"));
        assert!(!pattern_matches("openai/*", "anthropic/claude"));
        assert!(!pattern_matches("claude", "anthropic/claude-sonnet"));
        assert!(in_scope(&[], "x/y"));
        assert!(in_scope(&["x/*".into()], "x/y"));
        assert!(!in_scope(&["z/*".into()], "x/y"));
    }

    #[test]
    fn toggling_expands_an_empty_scope() {
        let all = vec!["a/1".to_string(), "b/2".into(), "c/3".into()];
        assert_eq!(toggle_scoped(&[], &all, "b/2", false), ["a/1", "c/3"]);
        assert_eq!(
            toggle_scoped(&["c/3".into()], &all, "a/1", true),
            ["a/1", "c/3"]
        );
        assert_eq!(toggle_scoped(&["a/1".into()], &all, "a/1", true), ["a/1"]);

        let mut settings = Settings::default();
        settings
            .raw_mut()
            .insert("enabledModels".into(), serde_json::json!(["a/1", " ", 3]));
        assert_eq!(enabled_models(&settings), ["a/1"]);
    }
}
