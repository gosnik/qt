//! [`RuntimeOptions`]: every option the CLI (and the desktop app) passes to
//! the composition root, plus model-argument parsing (`provider/id`,
//! `:thinking` suffixes, `--provider` scoping).

use std::path::PathBuf;

use qt_core::ModelInfo;
use qt_core::model_scope::{fuzzy_matches, resolve_model_scope};
use qt_core::provider::{ModelRef, ProviderConfig, ThinkingLevel};
use qt_core::tools::{ToolSelection, WebOptions};

/// How the process is used. Background services (the index scanner, the
/// periodic model refresh, Dreaming) only run for [`RunMode::Interactive`],
/// as in the archived binary.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum RunMode {
    /// The desktop app (or an interactive terminal session).
    #[default]
    Interactive,
    /// `-p` / `--print` and piped stdin: print the final answer.
    Print,
    /// `--mode json`: Pi event lines.
    Json,
    /// `--mode rpc`: JSONL commands on stdin, responses and events on stdout.
    Rpc,
}

impl RunMode {
    pub fn is_interactive(self) -> bool {
        self == Self::Interactive
    }
}

/// Composition-root options. Paths are used as given (the CLI resolves them
/// against the working directory first).
#[derive(Debug, Clone, Default)]
pub struct RuntimeOptions {
    /// Working directory of the session.
    pub cwd: PathBuf,
    /// Agent home; `None` uses `QT_CODING_AGENT_DIR` / `~/.qt`.
    pub agent_home: Option<PathBuf>,
    pub mode: RunMode,
    /// `--provider`.
    pub provider: Option<String>,
    /// `--model` (`id`, `provider/id`, a pattern, optional `:thinking`).
    pub model: Option<String>,
    /// `--api-key` for the selected provider (never persisted).
    pub api_key: Option<String>,
    /// `--thinking`.
    pub thinking: Option<ThinkingLevel>,
    /// `--models`: model scope patterns (comma lists already split).
    pub models: Vec<String>,
    /// `--system-prompt`.
    pub system_prompt: Option<String>,
    /// `--append-system-prompt` (repeatable).
    pub append_system_prompt: Vec<String>,
    /// `--tools` allowlist (comma lists already split); `None` allows all.
    pub tools: Option<Vec<String>>,
    /// `--no-tools`.
    pub no_tools: bool,
    /// `--no-builtin-tools`.
    pub no_builtin_tools: bool,
    /// `--no-memory`.
    pub no_memory: bool,
    /// `-e` / `--extension` paths.
    pub extensions: Vec<String>,
    /// `--no-extensions` (explicit `-e` paths still load).
    pub no_extensions: bool,
    /// Unrecognised `--flag [value]` arguments, resolved against the flags
    /// registered by extensions.
    pub extension_flags: Vec<String>,
    /// `--skill` paths.
    pub skills: Vec<String>,
    pub no_skills: bool,
    /// `--prompt-template` paths.
    pub prompt_templates: Vec<String>,
    pub no_prompt_templates: bool,
    /// `--theme` paths.
    pub themes: Vec<String>,
    pub no_themes: bool,
    /// `--no-context-files`.
    pub no_context_files: bool,
    /// `--discover-local-models` directories.
    pub discover_local_models: Vec<PathBuf>,
    /// `--offline` (or `PI_OFFLINE`).
    pub offline: bool,
    /// `--session-dir`.
    pub session_dir: Option<PathBuf>,
    /// `--no-session`.
    pub no_session: bool,
    /// Run the archived startup migrations before loading settings.
    pub startup_migrations: bool,
    /// Executable for subagent, memory pre-handler and Dreaming children
    /// (default: `QT_SUBAGENT_EXECUTABLE`, else `qt-cli` beside the running
    /// binary, else `qt-cli` on `PATH`).
    pub child_executable: Option<PathBuf>,
    /// Web tool options (tests allow private networks).
    pub web: Option<WebOptions>,
}

impl RuntimeOptions {
    pub fn new(cwd: impl Into<PathBuf>) -> Self {
        Self {
            cwd: cwd.into(),
            startup_migrations: true,
            ..Default::default()
        }
    }

    /// Interactive options for the desktop app.
    pub fn interactive(cwd: impl Into<PathBuf>) -> Self {
        Self {
            mode: RunMode::Interactive,
            ..Self::new(cwd)
        }
    }

    /// The tool selection for `--tools`, `--no-tools`, `--no-builtin-tools`.
    pub fn tool_selection(&self) -> ToolSelection {
        ToolSelection {
            no_tools: self.no_tools,
            no_builtin_tools: self.no_builtin_tools,
            allow: self.tools.clone().unwrap_or_default(),
            offline: self.offline,
        }
    }
}

/// A model argument split into its parts.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelArgument {
    pub provider: Option<String>,
    pub model: String,
    pub thinking: Option<ThinkingLevel>,
}

/// Split `--model` (and `--provider`): a trailing `:<level>` naming a valid
/// thinking level is removed; a `provider/` prefix is split off when
/// `is_provider` accepts it (provider-scoped ids such as `qwen/qwen3` stay
/// whole when `--provider` is given).
pub fn parse_model_argument(
    provider: Option<&str>,
    model: &str,
    is_provider: impl Fn(&str) -> bool,
) -> ModelArgument {
    let mut model = model.trim().to_string();
    let mut thinking = None;
    if let Some((head, level)) = model.rsplit_once(':')
        && let Some(parsed) = ThinkingLevel::parse(level)
        && !head.is_empty()
    {
        thinking = Some(parsed);
        model = head.to_string();
    }
    let provider = provider.map(str::trim).filter(|p| !p.is_empty());
    match provider {
        Some(p) => {
            let scoped = model
                .split_once('/')
                .filter(|(head, _)| head.eq_ignore_ascii_case(p))
                .map(|(_, rest)| rest.to_string())
                .unwrap_or(model);
            ModelArgument {
                provider: Some(p.to_string()),
                model: scoped,
                thinking,
            }
        }
        None => match model.split_once('/') {
            Some((head, rest)) if is_provider(head) && !rest.is_empty() => ModelArgument {
                provider: Some(head.to_string()),
                model: rest.to_string(),
                thinking,
            },
            _ => ModelArgument {
                provider: None,
                model,
                thinking,
            },
        },
    }
}

fn list_alternatives(items: impl Iterator<Item = String>) -> String {
    items.take(8).collect::<Vec<_>>().join(", ")
}

/// Resolve `--provider` / `--model` against the configured providers and
/// their known models (archived `resolve_explicit_model` wording). With a
/// provider the model id is opaque; without one it must match a known
/// model exactly (case-insensitively) or by pattern.
pub fn resolve_model_argument(
    providers: &[ProviderConfig],
    provider: Option<&str>,
    model: &str,
) -> Result<(ModelRef, Option<ThinkingLevel>), String> {
    if model.trim().is_empty() {
        return Err("model selection must not be empty".into());
    }
    if provider.is_some_and(|p| p.trim().is_empty()) {
        return Err("provider selection must not be empty".into());
    }
    let find_provider = |id: &str| providers.iter().find(|p| p.id.eq_ignore_ascii_case(id));
    let arg = parse_model_argument(provider, model, |p| find_provider(p).is_some());
    if let Some(p) = &arg.provider {
        let Some(config) = find_provider(p) else {
            let alternatives = list_alternatives(providers.iter().map(|p| p.id.clone()));
            return Err(if alternatives.is_empty() {
                format!("unknown provider selection: {p}")
            } else {
                format!("unknown provider selection: {p}. Available providers: {alternatives}")
            });
        };
        let id = config
            .models
            .iter()
            .find(|m| m.id.eq_ignore_ascii_case(&arg.model))
            .map(|m| m.id.clone())
            .unwrap_or(arg.model);
        return Ok((ModelRef::new(config.id.clone(), id), arg.thinking));
    }
    let available: Vec<ModelInfo> = providers
        .iter()
        .flat_map(|p| p.models.iter().map(|m| ModelInfo::from_spec(&p.id, m)))
        .collect();
    if let Some(exact) = available
        .iter()
        .find(|m| m.id.eq_ignore_ascii_case(&arg.model))
    {
        return Ok((exact.model_ref(), arg.thinking));
    }
    if let Some(first) = resolve_model_scope(std::slice::from_ref(&arg.model), &available)
        .models
        .into_iter()
        .next()
    {
        return Ok((first.model, arg.thinking.or(first.thinking)));
    }
    let mut fuzzy: Vec<&ModelInfo> = available
        .iter()
        .filter(|m| fuzzy_matches(&arg.model, &format!("{} {}", m.provider, m.id)))
        .collect();
    fuzzy.sort_by(|a, b| (&a.provider, &a.id).cmp(&(&b.provider, &b.id)));
    if let Some(first) = fuzzy.first() {
        return Ok((first.model_ref(), arg.thinking));
    }
    let alternatives =
        list_alternatives(available.iter().map(|m| format!("{}/{}", m.provider, m.id)));
    Err(if alternatives.is_empty() {
        format!("unknown model selection: {}", arg.model)
    } else {
        format!(
            "unknown model selection: {}. Available models include: {alternatives}",
            arg.model
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use qt_core::catalog::ModelSpec;
    use std::sync::Arc;

    fn provider(id: &str, models: &[&str]) -> ProviderConfig {
        ProviderConfig {
            id: id.into(),
            local: true,
            models: Arc::new(
                models
                    .iter()
                    .map(|m| ModelSpec {
                        provider: id.into(),
                        id: (*m).into(),
                        ..Default::default()
                    })
                    .collect(),
            ),
            ..Default::default()
        }
    }

    #[test]
    fn splits_provider_prefix_and_thinking_suffix() {
        let known = |p: &str| p == "openai" || p == "openrouter";
        assert_eq!(
            parse_model_argument(None, "openai/gpt-4o:high", known),
            ModelArgument {
                provider: Some("openai".into()),
                model: "gpt-4o".into(),
                thinking: Some(ThinkingLevel::High),
            }
        );
        assert_eq!(
            parse_model_argument(Some("openrouter"), "qwen/qwen3:xhigh", known),
            ModelArgument {
                provider: Some("openrouter".into()),
                model: "qwen/qwen3".into(),
                thinking: Some(ThinkingLevel::XHigh),
            }
        );
        // An unknown level is part of the id (e.g. `llama3:8b`).
        assert_eq!(
            parse_model_argument(None, "llama3:8b", known).model,
            "llama3:8b"
        );
        assert_eq!(
            parse_model_argument(None, "sonnet:minimal", known).thinking,
            Some(ThinkingLevel::Minimal)
        );
    }

    #[test]
    fn resolves_against_known_models() {
        let providers = vec![
            provider("anthropic", &["claude-sonnet-4-5", "claude-haiku-4-5"]),
            provider("ollama:local", &["qwen3:8b"]),
        ];
        let (m, t) = resolve_model_argument(&providers, None, "claude-haiku-4-5:low").unwrap();
        assert_eq!(m, ModelRef::new("anthropic", "claude-haiku-4-5"));
        assert_eq!(t, Some(ThinkingLevel::Low));
        let (m, _) = resolve_model_argument(&providers, None, "ollama:local/qwen3:8b").unwrap();
        assert_eq!(m, ModelRef::new("ollama:local", "qwen3:8b"));
        let (m, _) = resolve_model_argument(&providers, None, "sonnet").unwrap();
        assert_eq!(m.id, "claude-sonnet-4-5");
        let (m, _) = resolve_model_argument(&providers, Some("anthropic"), "new-model").unwrap();
        assert_eq!(m, ModelRef::new("anthropic", "new-model"));
        assert!(
            resolve_model_argument(&providers, Some("nope"), "x")
                .unwrap_err()
                .starts_with("unknown provider selection: nope. Available providers: anthropic")
        );
        assert!(
            resolve_model_argument(&providers, None, "zzz")
                .unwrap_err()
                .starts_with("unknown model selection: zzz. Available models include:")
        );
        assert_eq!(
            resolve_model_argument(&providers, None, " ").unwrap_err(),
            "model selection must not be empty"
        );
    }
}
