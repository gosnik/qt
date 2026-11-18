//! Model scoping (`enabledModels`, Pi `resolveModelScope`), model pattern
//! matching (Pi `parseModelPattern`) and the archived `--list-models`
//! table.

use globset::GlobBuilder;

use crate::provider::{ModelInfo, ModelRef, ThinkingLevel};

/// A model admitted by the scope, with an optional `:level` suffix.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ScopedModel {
    pub model: ModelRef,
    pub thinking: Option<ThinkingLevel>,
}

/// Result of resolving `enabledModels` patterns.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ScopeResolution {
    pub models: Vec<ScopedModel>,
    /// Pi's warnings (`No models match pattern "…"`, invalid levels).
    pub warnings: Vec<String>,
}

impl ScopeResolution {
    pub fn contains(&self, model: &ModelRef) -> bool {
        self.models.iter().any(|m| &m.model == model)
    }
}

/// Whether `id` looks like an alias (no `-YYYYMMDD` suffix, or `-latest`).
fn is_alias(id: &str) -> bool {
    if id.ends_with("-latest") {
        return true;
    }
    let bytes = id.as_bytes();
    !(bytes.len() >= 9
        && bytes[bytes.len() - 9] == b'-'
        && bytes[bytes.len() - 8..].iter().all(u8::is_ascii_digit))
}

/// Pi `findExactModelReferenceMatch`: `provider/id` or a bare id that is
/// unique across providers (case-insensitive).
pub fn find_exact_model<'a>(reference: &str, available: &'a [ModelInfo]) -> Option<&'a ModelInfo> {
    let trimmed = reference.trim();
    if trimmed.is_empty() {
        return None;
    }
    let wanted = trimmed.to_lowercase();
    let canonical: Vec<&ModelInfo> = available
        .iter()
        .filter(|m| format!("{}/{}", m.provider, m.id).to_lowercase() == wanted)
        .collect();
    match canonical.len() {
        1 => return Some(canonical[0]),
        n if n > 1 => return None,
        _ => {}
    }
    if let Some((provider, id)) = trimmed.split_once('/') {
        let (provider, id) = (provider.trim(), id.trim());
        if !provider.is_empty() && !id.is_empty() {
            let matches: Vec<&ModelInfo> = available
                .iter()
                .filter(|m| {
                    m.provider.eq_ignore_ascii_case(provider) && m.id.eq_ignore_ascii_case(id)
                })
                .collect();
            match matches.len() {
                1 => return Some(matches[0]),
                n if n > 1 => return None,
                _ => {}
            }
        }
    }
    let ids: Vec<&ModelInfo> = available
        .iter()
        .filter(|m| m.id.to_lowercase() == wanted)
        .collect();
    (ids.len() == 1).then(|| ids[0])
}

/// Pi `tryMatchModel`: exact reference, else a partial id/name match
/// preferring aliases (highest sorting), else the latest dated version.
pub fn match_model<'a>(pattern: &str, available: &'a [ModelInfo]) -> Option<&'a ModelInfo> {
    if let Some(exact) = find_exact_model(pattern, available) {
        return Some(exact);
    }
    let needle = pattern.to_lowercase();
    let matches: Vec<&ModelInfo> = available
        .iter()
        .filter(|m| {
            m.id.to_lowercase().contains(&needle) || m.name.to_lowercase().contains(&needle)
        })
        .collect();
    if matches.is_empty() {
        return None;
    }
    let (mut aliases, mut dated): (Vec<&ModelInfo>, Vec<&ModelInfo>) =
        matches.into_iter().partition(|m| is_alias(&m.id));
    if !aliases.is_empty() {
        aliases.sort_by(|a, b| b.id.cmp(&a.id));
        return Some(aliases[0]);
    }
    dated.sort_by(|a, b| b.id.cmp(&a.id));
    dated.first().copied()
}

/// Pi `parseModelPattern` in scope mode: the full pattern first, then the
/// part before the last `:` with the suffix read as a thinking level.
pub fn parse_model_pattern<'a>(
    pattern: &str,
    available: &'a [ModelInfo],
) -> (Option<&'a ModelInfo>, Option<ThinkingLevel>, Option<String>) {
    if let Some(model) = match_model(pattern, available) {
        return (Some(model), None, None);
    }
    let Some((prefix, suffix)) = pattern.rsplit_once(':') else {
        return (None, None, None);
    };
    let (model, level, warning) = parse_model_pattern(prefix, available);
    if model.is_none() {
        return (None, level, warning);
    }
    match ThinkingLevel::parse(suffix) {
        Some(level) => (model, warning.is_none().then_some(level), warning),
        None => (
            model,
            None,
            Some(format!(
                "Invalid thinking level \"{suffix}\" in pattern \"{pattern}\". Using default instead."
            )),
        ),
    }
}

fn glob_matches(pattern: &str, value: &str) -> bool {
    GlobBuilder::new(pattern)
        .case_insensitive(true)
        .literal_separator(true)
        .build()
        .map(|g| g.compile_matcher().is_match(value))
        .unwrap_or(false)
}

/// Resolve `enabledModels` patterns against the available models.
pub fn resolve_model_scope(patterns: &[String], available: &[ModelInfo]) -> ScopeResolution {
    let mut out = ScopeResolution::default();
    let push = |out: &mut ScopeResolution, model: &ModelInfo, thinking: Option<ThinkingLevel>| {
        let model = model.model_ref();
        if !out.contains(&model) {
            out.models.push(ScopedModel { model, thinking });
        }
    };
    for pattern in patterns {
        if pattern.contains(['*', '?', '[']) {
            let (glob, thinking) = match pattern.rsplit_once(':') {
                Some((prefix, suffix)) if ThinkingLevel::parse(suffix).is_some() => {
                    (prefix, ThinkingLevel::parse(suffix))
                }
                _ => (pattern.as_str(), None),
            };
            let matching: Vec<&ModelInfo> = available
                .iter()
                .filter(|m| {
                    glob_matches(glob, &format!("{}/{}", m.provider, m.id))
                        || glob_matches(glob, &m.id)
                })
                .collect();
            if matching.is_empty() {
                out.warnings
                    .push(format!("No models match pattern \"{pattern}\""));
                continue;
            }
            for model in matching {
                push(&mut out, model, thinking);
            }
            continue;
        }
        let (model, thinking, warning) = parse_model_pattern(pattern, available);
        if let Some(w) = warning {
            out.warnings.push(w);
        }
        match model {
            Some(model) => push(&mut out, model, thinking),
            None => out
                .warnings
                .push(format!("No models match pattern \"{pattern}\"")),
        }
    }
    out
}

/// Archived `format_token_count`: `1M`, `1.5M`, `128K`, `200`, using C++
/// stream formatting (six significant digits).
pub fn format_token_count(count: u64) -> String {
    fn general(value: f64) -> String {
        let digits = if value >= 1.0 {
            value.log10().floor() as i32 + 1
        } else {
            1
        };
        let decimals = (6 - digits).max(0) as usize;
        let text = format!("{value:.decimals$}");
        if text.contains('.') {
            text.trim_end_matches('0').trim_end_matches('.').to_string()
        } else {
            text
        }
    }
    if count >= 1_000_000 {
        if count.is_multiple_of(1_000_000) {
            return format!("{}M", count / 1_000_000);
        }
        return format!("{}M", general(count as f64 / 1_000_000.0));
    }
    if count >= 1_000 {
        if count.is_multiple_of(1_000) {
            return format!("{}K", count / 1_000);
        }
        return format!("{}K", general(count as f64 / 1_000.0));
    }
    count.to_string()
}

/// Archived fuzzy match used by `--list-models [search]` and `/model`:
/// every query character appears in order (case-insensitive, spaces in
/// the query ignored).
pub fn fuzzy_matches(query: &str, text: &str) -> bool {
    let mut haystack = text.chars().flat_map(char::to_lowercase);
    query
        .chars()
        .filter(|c| !c.is_whitespace())
        .flat_map(char::to_lowercase)
        .all(|q| haystack.any(|h| h == q))
}

/// One row of the `--list-models` table.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelTableRow {
    pub provider: String,
    pub model: String,
    pub context: String,
    pub max_out: String,
    pub thinking: String,
    pub images: String,
    pub capability: String,
    pub usage: String,
}

impl ModelTableRow {
    pub fn from_info(info: &ModelInfo) -> Self {
        Self {
            provider: info.provider.clone(),
            model: info.id.clone(),
            context: format_token_count(info.context_window.unwrap_or(0)),
            max_out: format_token_count(info.max_tokens.unwrap_or(0)),
            thinking: if info.reasoning { "yes" } else { "no" }.into(),
            images: if info.vision { "yes" } else { "no" }.into(),
            capability: String::new(),
            usage: String::new(),
        }
    }
}

/// The archived `--list-models [search]` output: rows filtered by fuzzy
/// search over `provider model`, sorted by provider then model, in padded
/// columns `provider model context max-out thinking images capability usage`.
pub fn render_model_table(rows: &[ModelTableRow], search: &str) -> String {
    let mut rows: Vec<&ModelTableRow> = rows
        .iter()
        .filter(|r| {
            search.is_empty() || fuzzy_matches(search, &format!("{} {}", r.provider, r.model))
        })
        .collect();
    if rows.is_empty() {
        return if search.is_empty() {
            "No models available. Run /login or configure credentials.\n".into()
        } else {
            format!("No models matching \"{search}\"\n")
        };
    }
    rows.sort_by(|a, b| (&a.provider, &a.model).cmp(&(&b.provider, &b.model)));
    let header = [
        "provider",
        "model",
        "context",
        "max-out",
        "thinking",
        "images",
        "capability",
        "usage",
    ];
    let cells = |r: &ModelTableRow| -> [String; 8] {
        [
            r.provider.clone(),
            r.model.clone(),
            r.context.clone(),
            r.max_out.clone(),
            r.thinking.clone(),
            r.images.clone(),
            r.capability.clone(),
            r.usage.clone(),
        ]
    };
    let mut widths: [usize; 8] = header.map(str::len);
    for row in &rows {
        for (w, c) in widths.iter_mut().zip(cells(row).iter()) {
            *w = (*w).max(c.len());
        }
    }
    let render = |values: &[String; 8]| -> String {
        let mut line = String::new();
        for (i, (v, w)) in values.iter().zip(widths.iter()).enumerate() {
            if i > 0 {
                line.push_str("  ");
            }
            line.push_str(&format!("{v:<w$}"));
        }
        line.push('\n');
        line
    };
    let mut out = render(&header.map(str::to_string));
    for row in rows {
        out.push_str(&render(&cells(row)));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn info(provider: &str, id: &str) -> ModelInfo {
        ModelInfo {
            provider: provider.into(),
            id: id.into(),
            name: id.into(),
            ..Default::default()
        }
    }

    #[test]
    fn scope_patterns_globs_levels_and_warnings() {
        let models = vec![
            info("anthropic", "claude-sonnet-4-5"),
            info("anthropic", "claude-sonnet-4-5-20250929"),
            info("openai", "gpt-5"),
            info("openai", "gpt-5-mini"),
            info("openrouter", "openai/gpt-5:exacto"),
        ];
        let patterns: Vec<String> = [
            "anthropic/*:high",
            "*mini",
            "sonnet",
            "gpt-5:low",
            "openai/gpt-5:exacto",
            "gpt-5:bogus",
            "nothing*",
            "missing",
        ]
        .iter()
        .map(|s| s.to_string())
        .collect();
        let r = resolve_model_scope(&patterns, &models);
        let refs: Vec<String> = r
            .models
            .iter()
            .map(|m| format!("{}:{:?}", m.model, m.thinking))
            .collect();
        assert_eq!(
            refs,
            [
                "anthropic/claude-sonnet-4-5:Some(High)",
                "anthropic/claude-sonnet-4-5-20250929:Some(High)",
                "openai/gpt-5-mini:None",
                "openai/gpt-5:Some(Low)",
                "openrouter/openai/gpt-5:exacto:None",
            ]
        );
        assert_eq!(
            r.warnings,
            [
                "Invalid thinking level \"bogus\" in pattern \"gpt-5:bogus\". Using default instead.",
                "No models match pattern \"nothing*\"",
                "No models match pattern \"missing\"",
            ]
        );
        assert_eq!(
            match_model("claude-sonnet", &models).unwrap().id,
            "claude-sonnet-4-5"
        );
    }

    #[test]
    fn token_counts_use_stream_formatting() {
        assert_eq!(format_token_count(0), "0");
        assert_eq!(format_token_count(999), "999");
        assert_eq!(format_token_count(128_000), "128K");
        assert_eq!(format_token_count(262_144), "262.144K");
        assert_eq!(format_token_count(1_048_576), "1.04858M");
        assert_eq!(format_token_count(2_000_000), "2M");
        assert_eq!(format_token_count(1_500), "1.5K");
    }

    #[test]
    fn model_table_is_sorted_padded_and_searchable() {
        let mut a = info("openai", "gpt-5");
        a.context_window = Some(400_000);
        a.max_tokens = Some(128_000);
        a.reasoning = true;
        a.vision = true;
        let b = info("anthropic", "claude-x");
        let rows = vec![ModelTableRow::from_info(&a), ModelTableRow::from_info(&b)];
        let table = render_model_table(&rows, "");
        let lines: Vec<&str> = table.lines().collect();
        assert_eq!(
            lines[0],
            "provider   model     context  max-out  thinking  images  capability  usage"
        );
        assert_eq!(
            lines[1].trim_end(),
            "anthropic  claude-x  0        0        no        no"
        );
        assert_eq!(
            lines[2].trim_end(),
            "openai     gpt-5     400K     128K     yes       yes"
        );
        // Every row is padded to the full table width.
        assert!(lines.iter().all(|l| l.len() == lines[0].len()));
        assert_eq!(render_model_table(&rows, "oai g5").lines().count(), 2);
        assert_eq!(
            render_model_table(&rows, "zzz"),
            "No models matching \"zzz\"\n"
        );
        assert_eq!(
            render_model_table(&[], ""),
            "No models available. Run /login or configure credentials.\n"
        );
    }
}
