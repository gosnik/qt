//! Per-provider compatibility settings (Pi `OpenAICompletionsCompat`,
//! `OpenAIResponsesCompat`, `AnthropicMessagesCompat`).
//!
//! Defaults are detected from the provider id and base URL; explicit
//! `compat` values on the provider (models.json) and the model override them.

use serde_json::{Map, Value};

use super::ProviderConfig;
use crate::catalog::ModelSpec;

/// How reasoning is requested from an OpenAI-compatible endpoint.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ThinkingFormat {
    /// `reasoning_effort`
    OpenAi,
    /// `reasoning: { effort }`
    OpenRouter,
    /// `thinking: { type }` plus `reasoning_effort`
    DeepSeek,
    /// `reasoning: { enabled }` plus `reasoning_effort` when supported
    Together,
    /// top-level `enable_thinking`
    Zai,
    /// top-level `enable_thinking`
    Qwen,
    /// `chat_template_kwargs.enable_thinking`
    QwenChatTemplate,
}

impl ThinkingFormat {
    fn parse(s: &str) -> Option<Self> {
        Some(match s {
            "openai" => Self::OpenAi,
            "openrouter" => Self::OpenRouter,
            "deepseek" => Self::DeepSeek,
            "together" => Self::Together,
            "zai" => Self::Zai,
            "qwen" => Self::Qwen,
            "qwen-chat-template" => Self::QwenChatTemplate,
            _ => return None,
        })
    }
}

/// Resolved chat-completions compatibility.
#[derive(Debug, Clone, PartialEq)]
pub struct CompletionsCompat {
    pub supports_store: bool,
    pub supports_developer_role: bool,
    pub supports_reasoning_effort: bool,
    pub supports_usage_in_streaming: bool,
    /// `max_completion_tokens` or `max_tokens`.
    pub max_tokens_field: String,
    pub requires_tool_result_name: bool,
    pub requires_assistant_after_tool_result: bool,
    pub requires_thinking_as_text: bool,
    pub requires_reasoning_content_on_assistant_messages: bool,
    pub thinking_format: ThinkingFormat,
    /// OpenRouter `provider` routing object (sent only to openrouter.ai).
    pub open_router_routing: Option<Value>,
    /// Vercel AI Gateway routing (`only` / `order`).
    pub vercel_gateway_routing: Option<Value>,
    pub zai_tool_stream: bool,
    pub supports_strict_mode: bool,
    /// Anthropic-style `cache_control` markers.
    pub cache_control_anthropic: bool,
    pub send_session_affinity_headers: bool,
    pub supports_long_cache_retention: bool,
}

/// Merge provider then model compat objects.
fn merged(provider: &ProviderConfig, model: &ModelSpec) -> Map<String, Value> {
    let mut out = provider.compat.clone();
    for (k, v) in &model.compat {
        if !v.is_null() {
            out.insert(k.clone(), v.clone());
        }
    }
    out
}

fn flag(map: &Map<String, Value>, key: &str, default: bool) -> bool {
    map.get(key).and_then(Value::as_bool).unwrap_or(default)
}

/// Hosts that reject OpenAI-only fields such as `store` and `developer`.
pub const NONSTANDARD_HOSTS: &[&str] = &[
    "cerebras.ai",
    "api.x.ai",
    "api.together.ai",
    "api.together.xyz",
    "chutes.ai",
    "deepseek.com",
    "api.z.ai",
    "api.moonshot.",
    "opencode.ai",
    "api.cloudflare.com",
    "gateway.ai.cloudflare.com",
];

/// Detect and merge chat-completions compatibility (Pi `getCompat`).
pub fn completions(provider: &ProviderConfig, model: &ModelSpec) -> CompletionsCompat {
    let id = model.provider.as_str();
    let url = model.base_url.as_str();
    let has = |needle: &str| url.contains(needle);
    let is_zai = id == "zai" || has("api.z.ai");
    let is_together = id == "together" || has("api.together.ai") || has("api.together.xyz");
    let is_moonshot = id == "moonshotai" || id == "moonshotai-cn" || has("api.moonshot.");
    let is_cf_workers = id == "cloudflare-workers-ai" || has("api.cloudflare.com");
    let is_cf_gateway = id == "cloudflare-ai-gateway" || has("gateway.ai.cloudflare.com");
    let is_grok = id == "xai" || has("api.x.ai");
    let is_deepseek = id == "deepseek" || has("deepseek.com");
    let is_openrouter = id == "openrouter" || has("openrouter.ai");
    let nonstandard = id == "cerebras"
        || has("cerebras.ai")
        || is_grok
        || is_together
        || has("chutes.ai")
        || is_deepseek
        || is_zai
        || is_moonshot
        || id == "opencode"
        || has("opencode.ai")
        || is_cf_workers
        || is_cf_gateway;
    let use_max_tokens = has("chutes.ai") || is_moonshot || is_cf_gateway || is_together;

    let c = merged(provider, model);
    let thinking_format = c
        .get("thinkingFormat")
        .and_then(Value::as_str)
        .and_then(ThinkingFormat::parse)
        .unwrap_or(if is_deepseek {
            ThinkingFormat::DeepSeek
        } else if is_zai {
            ThinkingFormat::Zai
        } else if is_together {
            ThinkingFormat::Together
        } else if is_openrouter {
            ThinkingFormat::OpenRouter
        } else {
            ThinkingFormat::OpenAi
        });
    let max_tokens_field = match c.get("maxTokensField").and_then(Value::as_str) {
        Some(f @ ("max_tokens" | "max_completion_tokens")) => f.to_string(),
        _ if use_max_tokens => "max_tokens".into(),
        _ => "max_completion_tokens".into(),
    };
    let cache_control_anthropic = match c.get("cacheControlFormat").and_then(Value::as_str) {
        Some(format) => format == "anthropic",
        None => id == "openrouter" && model.id.starts_with("anthropic/"),
    };
    CompletionsCompat {
        supports_store: flag(&c, "supportsStore", !nonstandard),
        supports_developer_role: flag(&c, "supportsDeveloperRole", !nonstandard),
        supports_reasoning_effort: flag(
            &c,
            "supportsReasoningEffort",
            !is_grok && !is_zai && !is_moonshot && !is_together && !is_cf_gateway,
        ),
        supports_usage_in_streaming: flag(&c, "supportsUsageInStreaming", true),
        max_tokens_field,
        requires_tool_result_name: flag(&c, "requiresToolResultName", false),
        requires_assistant_after_tool_result: flag(&c, "requiresAssistantAfterToolResult", false),
        requires_thinking_as_text: flag(&c, "requiresThinkingAsText", false),
        requires_reasoning_content_on_assistant_messages: flag(
            &c,
            "requiresReasoningContentOnAssistantMessages",
            is_deepseek,
        ),
        thinking_format,
        open_router_routing: c
            .get("openRouterRouting")
            .filter(|v| v.as_object().is_some_and(|o| !o.is_empty()))
            .cloned(),
        vercel_gateway_routing: c
            .get("vercelGatewayRouting")
            .filter(|v| v.is_object())
            .cloned(),
        zai_tool_stream: flag(&c, "zaiToolStream", false),
        supports_strict_mode: flag(
            &c,
            "supportsStrictMode",
            !is_moonshot && !is_together && !is_cf_gateway,
        ),
        cache_control_anthropic,
        send_session_affinity_headers: flag(&c, "sendSessionAffinityHeaders", false),
        supports_long_cache_retention: flag(
            &c,
            "supportsLongCacheRetention",
            !(is_together || is_cf_workers || is_cf_gateway),
        ),
    }
}

/// Resolved Responses compatibility.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResponsesCompat {
    pub send_session_id_header: bool,
    pub supports_long_cache_retention: bool,
}

pub fn responses(provider: &ProviderConfig, model: &ModelSpec) -> ResponsesCompat {
    let c = merged(provider, model);
    ResponsesCompat {
        send_session_id_header: flag(&c, "sendSessionIdHeader", true),
        supports_long_cache_retention: flag(&c, "supportsLongCacheRetention", true),
    }
}

/// Resolved Anthropic Messages compatibility.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct AnthropicCompat {
    pub supports_eager_tool_input_streaming: bool,
    pub supports_long_cache_retention: bool,
    pub send_session_affinity_headers: bool,
    pub supports_cache_control_on_tools: bool,
}

pub fn anthropic(provider: &ProviderConfig, model: &ModelSpec) -> AnthropicCompat {
    let is_fireworks = model.provider == "fireworks";
    let is_cf_gateway_anthropic =
        model.provider == "cloudflare-ai-gateway" && model.base_url.contains("anthropic");
    let c = merged(provider, model);
    AnthropicCompat {
        supports_eager_tool_input_streaming: flag(
            &c,
            "supportsEagerToolInputStreaming",
            !is_fireworks,
        ),
        supports_long_cache_retention: flag(&c, "supportsLongCacheRetention", !is_fireworks),
        send_session_affinity_headers: flag(
            &c,
            "sendSessionAffinityHeaders",
            is_fireworks || is_cf_gateway_anthropic,
        ),
        supports_cache_control_on_tools: flag(&c, "supportsCacheControlOnTools", !is_fireworks),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::catalog::ModelCatalog;
    use serde_json::json;

    fn spec(provider: &str, id: &str, url: &str) -> ModelSpec {
        ModelSpec::fallback(provider, "openai-completions", url, id)
    }

    #[test]
    fn detects_hosts() {
        let p = ProviderConfig::default();
        let openai = completions(&p, &spec("openai", "gpt-4o", "https://api.openai.com/v1"));
        assert!(openai.supports_store && openai.supports_developer_role);
        assert_eq!(openai.max_tokens_field, "max_completion_tokens");
        assert_eq!(openai.thinking_format, ThinkingFormat::OpenAi);

        let together = completions(&p, &spec("x", "m", "https://api.together.xyz/v1"));
        assert!(!together.supports_store && !together.supports_reasoning_effort);
        assert_eq!(together.max_tokens_field, "max_tokens");
        assert_eq!(together.thinking_format, ThinkingFormat::Together);
        assert!(!together.supports_strict_mode && !together.supports_long_cache_retention);

        let deepseek = completions(
            &p,
            &spec("deepseek", "deepseek-chat", "https://api.deepseek.com"),
        );
        assert!(deepseek.requires_reasoning_content_on_assistant_messages);
        assert_eq!(deepseek.thinking_format, ThinkingFormat::DeepSeek);

        let or = completions(
            &p,
            &spec(
                "openrouter",
                "anthropic/claude",
                "https://openrouter.ai/api/v1",
            ),
        );
        assert_eq!(or.thinking_format, ThinkingFormat::OpenRouter);
        assert!(or.cache_control_anthropic);

        let zai = completions(&p, &spec("zai", "glm", "https://api.z.ai/api/paas/v4"));
        assert_eq!(zai.thinking_format, ThinkingFormat::Zai);
        assert!(!zai.supports_reasoning_effort);

        let grok = completions(&p, &spec("xai", "grok", "https://api.x.ai/v1"));
        assert!(!grok.supports_reasoning_effort && !grok.supports_store);
    }

    #[test]
    fn explicit_compat_overrides_detection() {
        let p = ProviderConfig {
            compat: json!({ "requiresToolResultName": true, "supportsStore": true })
                .as_object()
                .cloned()
                .unwrap(),
            ..Default::default()
        };
        let mut m = spec("together", "m", "https://api.together.ai/v1");
        m.compat = json!({ "thinkingFormat": "qwen-chat-template", "openRouterRouting": { "order": ["a"] } })
            .as_object()
            .cloned()
            .unwrap();
        let c = completions(&p, &m);
        assert!(c.requires_tool_result_name && c.supports_store);
        assert_eq!(c.thinking_format, ThinkingFormat::QwenChatTemplate);
        assert_eq!(c.open_router_routing.unwrap()["order"][0], "a");

        let catalog = ModelCatalog::builtin();
        let kimi = catalog
            .find_model("fireworks", "accounts/fireworks/models/kimi-k2p6")
            .unwrap();
        let a = anthropic(&ProviderConfig::default(), kimi);
        assert!(!a.supports_eager_tool_input_streaming && a.send_session_affinity_headers);
        let claude = catalog.find_model("anthropic", "claude-opus-4-7").unwrap();
        let a = anthropic(&ProviderConfig::default(), claude);
        assert!(a.supports_eager_tool_input_streaming && !a.send_session_affinity_headers);
    }
}
