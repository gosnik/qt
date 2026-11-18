//! Built-in providers and models, ported from the archived qt
//! `default_model_catalog()` / `default_image_model_catalog()`.

use serde_json::{Value, json};

use super::{Cost, ImageModelSpec, ModelSpec, ProviderSpec};

const BEDROCK_US: &str = "https://bedrock-runtime.us-east-1.amazonaws.com";
const BEDROCK_EU: &str = "https://bedrock-runtime.eu-central-1.amazonaws.com";
const COPILOT: &str = "https://api.individual.githubcopilot.com";
const CODEX: &str = "https://chatgpt.com/backend-api";
const ZAI_CODING: &str = "https://api.z.ai/api/coding/paas/v4";
const CF_WORKERS: &str =
    "https://api.cloudflare.com/client/v4/accounts/{CLOUDFLARE_ACCOUNT_ID}/ai/v1";
const CF_GATEWAY: &str =
    "https://gateway.ai.cloudflare.com/v1/{CLOUDFLARE_ACCOUNT_ID}/{CLOUDFLARE_GATEWAY_ID}";
const OPENROUTER: &str = "https://openrouter.ai/api/v1";

const COPILOT_HEADERS: &[(&str, &str)] = &[
    ("User-Agent", "GitHubCopilotChat/0.35.0"),
    ("Editor-Version", "vscode/1.107.0"),
    ("Editor-Plugin-Version", "copilot-chat/0.35.0"),
    ("Copilot-Integration-Id", "vscode-chat"),
];

fn provider(id: &str, api: &str, name: &str, base_url: &str, accepted: &[&str]) -> ProviderSpec {
    ProviderSpec {
        id: id.into(),
        name: name.into(),
        api: api.into(),
        base_url: base_url.into(),
        accepted_apis: accepted.iter().map(|s| s.to_string()).collect(),
        ..ProviderSpec::default()
    }
}

fn unsupported(id: &str, name: &str, reason: &str) -> ProviderSpec {
    ProviderSpec {
        id: id.into(),
        name: name.into(),
        api: "unsupported".into(),
        unsupported: Some(reason.into()),
        ..ProviderSpec::default()
    }
}

/// Built-in provider records.
pub(super) fn providers() -> Vec<ProviderSpec> {
    let gateway_compat = format!("{CF_GATEWAY}/compat");
    vec![
        provider(
            "anthropic",
            "anthropic-messages",
            "Anthropic",
            "https://api.anthropic.com",
            &[],
        ),
        provider(
            "amazon-bedrock",
            "bedrock-converse-stream",
            "Amazon Bedrock",
            BEDROCK_US,
            &[],
        ),
        // The base URL comes from AZURE_OPENAI_BASE_URL / AZURE_OPENAI_RESOURCE_NAME
        // or models.json at request time.
        provider(
            "azure-openai-responses",
            "azure-openai-responses",
            "Azure OpenAI Responses",
            "",
            &[],
        ),
        unsupported(
            "cerebras",
            "Cerebras",
            "Cerebras provider transport is not implemented yet.",
        ),
        provider(
            "cloudflare-workers-ai",
            "openai-completions",
            "Cloudflare Workers AI",
            CF_WORKERS,
            &[],
        ),
        provider(
            "cloudflare-ai-gateway",
            "openai-completions",
            "Cloudflare AI Gateway",
            &gateway_compat,
            &[
                "anthropic-messages",
                "openai-completions",
                "openai-responses",
            ],
        ),
        unsupported(
            "deepseek",
            "DeepSeek",
            "DeepSeek provider transport is not implemented yet.",
        ),
        provider(
            "fireworks",
            "anthropic-messages",
            "Fireworks",
            "https://api.fireworks.ai/inference",
            &[],
        ),
        provider(
            "github-copilot",
            "openai-responses",
            "GitHub Copilot",
            COPILOT,
            &[
                "anthropic-messages",
                "openai-completions",
                "openai-responses",
            ],
        ),
        provider(
            "google",
            "google-generative-ai",
            "Google",
            "https://generativelanguage.googleapis.com",
            &[],
        ),
        provider(
            "google-vertex",
            "google-vertex",
            "Google Vertex",
            "https://{location}-aiplatform.googleapis.com",
            &[],
        ),
        provider(
            "groq",
            "openai-completions",
            "Groq",
            "https://api.groq.com/openai/v1",
            &[],
        ),
        unsupported(
            "huggingface",
            "Hugging Face",
            "Hugging Face provider transport is not implemented yet.",
        ),
        unsupported(
            "kimi-coding",
            "Kimi Coding",
            "Kimi Coding provider transport is not implemented yet.",
        ),
        unsupported(
            "minimax",
            "MiniMax",
            "MiniMax provider transport is not implemented yet.",
        ),
        unsupported(
            "minimax-cn",
            "MiniMax CN",
            "MiniMax CN provider transport is not implemented yet.",
        ),
        provider(
            "mistral",
            "mistral-conversations",
            "Mistral",
            "https://api.mistral.ai",
            &[],
        ),
        unsupported(
            "moonshotai",
            "Moonshot AI",
            "Moonshot AI provider transport is not implemented yet.",
        ),
        unsupported(
            "moonshotai-cn",
            "Moonshot AI CN",
            "Moonshot AI CN provider transport is not implemented yet.",
        ),
        provider(
            "openai",
            "openai-responses",
            "OpenAI",
            "https://api.openai.com/v1",
            &[],
        ),
        provider(
            "openai-codex",
            "openai-codex-responses",
            "OpenAI Codex",
            CODEX,
            &[],
        ),
        provider(
            "opencode",
            "openai-completions",
            "OpenCode Zen",
            "https://opencode.ai/zen/v1",
            &[
                "anthropic-messages",
                "google-generative-ai",
                "openai-completions",
                "openai-responses",
            ],
        ),
        provider(
            "opencode-go",
            "openai-completions",
            "OpenCode Go",
            "https://opencode.ai/zen/go/v1",
            &[],
        ),
        provider(
            "openrouter",
            "openai-completions",
            "OpenRouter",
            OPENROUTER,
            &[],
        ),
        provider(
            "together",
            "openai-completions",
            "Together",
            "https://api.together.ai/v1",
            &[],
        ),
        unsupported(
            "vercel-ai-gateway",
            "Vercel AI Gateway",
            "Vercel AI Gateway provider transport is not implemented yet.",
        ),
        unsupported(
            "xai",
            "xAI",
            "xAI provider transport is not implemented yet.",
        ),
        unsupported(
            "xiaomi",
            "Xiaomi",
            "Xiaomi provider transport is not implemented yet.",
        ),
        unsupported(
            "xiaomi-token-plan-ams",
            "Xiaomi Token Plan AMS",
            "Xiaomi token-plan provider transport is not implemented yet.",
        ),
        unsupported(
            "xiaomi-token-plan-cn",
            "Xiaomi Token Plan CN",
            "Xiaomi token-plan provider transport is not implemented yet.",
        ),
        unsupported(
            "xiaomi-token-plan-sgp",
            "Xiaomi Token Plan SGP",
            "Xiaomi token-plan provider transport is not implemented yet.",
        ),
        provider(
            "zai",
            "openai-completions",
            "Z.ai",
            "https://api.z.ai/api/paas/v4",
            &[],
        ),
    ]
}

/// Fluent constructor for one built-in model record.
struct M(ModelSpec);

#[allow(clippy::too_many_arguments)]
fn m(
    provider: &str,
    api: &str,
    id: &str,
    name: &str,
    reasoning: bool,
    image: bool,
    context_window: u64,
    max_tokens: u64,
    cost: [f64; 4],
) -> M {
    M(ModelSpec {
        provider: provider.into(),
        id: id.into(),
        name: name.into(),
        api: api.into(),
        reasoning,
        input: if image {
            vec!["text".into(), "image".into()]
        } else {
            vec!["text".into()]
        },
        cost: Cost {
            input: cost[0],
            output: cost[1],
            cache_read: cost[2],
            cache_write: cost[3],
        },
        context_window,
        max_tokens,
        ..ModelSpec::default()
    })
}

impl M {
    fn levels(mut self, map: &[(&str, Option<&str>)]) -> Self {
        for (level, value) in map {
            self.0
                .thinking_level_map
                .insert(level.to_string(), value.map(str::to_string));
        }
        self
    }
    fn compat(mut self, compat: Value) -> Self {
        if let Value::Object(map) = compat {
            self.0.compat = map;
        }
        self
    }
    fn base(mut self, url: &str) -> Self {
        self.0.base_url = url.into();
        self
    }
    fn headers(mut self, headers: &[(&str, &str)]) -> Self {
        for (k, v) in headers {
            self.0.headers.insert(k.to_string(), v.to_string());
        }
        self
    }
}

/// Built-in model records (base URLs left empty inherit the provider's).
pub(super) fn models() -> Vec<ModelSpec> {
    let bedrock = |id: &str, name: &str, reasoning: bool, image: bool, ctx: u64, max: u64, cost| {
        m(
            "amazon-bedrock",
            "bedrock-converse-stream",
            id,
            name,
            reasoning,
            image,
            ctx,
            max,
            cost,
        )
        .base(BEDROCK_US)
    };
    let copilot = |api: &str, id: &str, name: &str, ctx: u64, max: u64| {
        m(
            "github-copilot",
            api,
            id,
            name,
            true,
            true,
            ctx,
            max,
            [0.0; 4],
        )
        .base(COPILOT)
        .headers(COPILOT_HEADERS)
    };
    let mistral = |id: &str, name: &str, reasoning: bool, image: bool, ctx: u64, max: u64, cost| {
        m(
            "mistral",
            "mistral-conversations",
            id,
            name,
            reasoning,
            image,
            ctx,
            max,
            cost,
        )
        .base("https://api.mistral.ai")
    };
    let openai = |id: &str, name: &str, cost| {
        m(
            "openai",
            "openai-responses",
            id,
            name,
            true,
            true,
            400_000,
            128_000,
            cost,
        )
    };
    let codex = |id: &str, name: &str, cost| {
        m(
            "openai-codex",
            "openai-codex-responses",
            id,
            name,
            true,
            true,
            272_000,
            128_000,
            cost,
        )
        .levels(&[("minimal", Some("low")), ("xhigh", Some("xhigh"))])
        .base(CODEX)
    };
    let together_compat = |reasoning_effort: bool, thinking_format: Option<&str>| {
        let mut compat = json!({
            "supportsStore": false,
            "supportsDeveloperRole": false,
            "supportsReasoningEffort": reasoning_effort,
            "maxTokensField": "max_tokens",
            "supportsStrictMode": false,
            "supportsLongCacheRetention": false,
        });
        if let Some(format) = thinking_format {
            compat["thinkingFormat"] = json!(format);
        }
        compat
    };
    let zai = |id: &str, name: &str, ctx: u64, max: u64, cost, tool_stream: bool| {
        let mut compat = json!({ "supportsDeveloperRole": false, "thinkingFormat": "zai" });
        if tool_stream {
            compat["zaiToolStream"] = json!(true);
        }
        m(
            "zai",
            "openai-completions",
            id,
            name,
            true,
            false,
            ctx,
            max,
            cost,
        )
        .compat(compat)
        .base(ZAI_CODING)
    };
    let gemini3_pro_levels: &[(&str, Option<&str>)] = &[
        ("off", None),
        ("minimal", None),
        ("low", Some("LOW")),
        ("medium", None),
        ("high", Some("HIGH")),
    ];
    let fireworks_compat = json!({
        "sendSessionAffinityHeaders": true,
        "supportsEagerToolInputStreaming": false,
        "supportsCacheControlOnTools": false,
        "supportsLongCacheRetention": false,
    });

    let list: Vec<M> = vec![
        bedrock(
            "amazon.nova-2-lite-v1:0",
            "Nova 2 Lite",
            false,
            true,
            128_000,
            4_096,
            [0.33, 2.75, 0.0, 0.0],
        ),
        bedrock(
            "amazon.nova-lite-v1:0",
            "Nova Lite",
            false,
            true,
            300_000,
            8_192,
            [0.06, 0.24, 0.015, 0.0],
        ),
        bedrock(
            "amazon.nova-micro-v1:0",
            "Nova Micro",
            false,
            false,
            128_000,
            8_192,
            [0.035, 0.14, 0.00875, 0.0],
        ),
        bedrock(
            "amazon.nova-premier-v1:0",
            "Nova Premier",
            true,
            true,
            1_000_000,
            16_384,
            [2.5, 12.5, 0.0, 0.0],
        ),
        bedrock(
            "amazon.nova-pro-v1:0",
            "Nova Pro",
            false,
            true,
            300_000,
            8_192,
            [0.8, 3.2, 0.2, 0.0],
        ),
        bedrock(
            "anthropic.claude-haiku-4-5-20251001-v1:0",
            "Claude Haiku 4.5",
            true,
            true,
            200_000,
            64_000,
            [1.0, 5.0, 0.1, 1.25],
        ),
        bedrock(
            "anthropic.claude-opus-4-6-v1",
            "Claude Opus 4.6",
            true,
            true,
            1_000_000,
            128_000,
            [5.0, 25.0, 0.5, 6.25],
        )
        .levels(&[("xhigh", Some("max"))]),
        bedrock(
            "anthropic.claude-opus-4-7",
            "Claude Opus 4.7",
            true,
            true,
            1_000_000,
            128_000,
            [5.0, 25.0, 0.5, 6.25],
        )
        .levels(&[("xhigh", Some("xhigh"))]),
        bedrock(
            "anthropic.claude-sonnet-4-5-20250929-v1:0",
            "Claude Sonnet 4.5",
            true,
            true,
            200_000,
            64_000,
            [3.0, 15.0, 0.3, 3.75],
        ),
        bedrock(
            "anthropic.claude-sonnet-4-6",
            "Claude Sonnet 4.6",
            true,
            true,
            1_000_000,
            64_000,
            [3.0, 15.0, 0.3, 3.75],
        ),
        bedrock(
            "au.anthropic.claude-opus-4-6-v1",
            "AU Anthropic Claude Opus 4.6",
            true,
            true,
            1_000_000,
            128_000,
            [16.5, 82.5, 0.5, 6.25],
        )
        .levels(&[("xhigh", Some("max"))]),
        bedrock(
            "eu.anthropic.claude-opus-4-7",
            "Claude Opus 4.7 (EU)",
            true,
            true,
            1_000_000,
            128_000,
            [5.0, 25.0, 0.5, 6.25],
        )
        .levels(&[("xhigh", Some("xhigh"))])
        .base(BEDROCK_EU),
        bedrock(
            "global.anthropic.claude-sonnet-4-6",
            "Claude Sonnet 4.6 (Global)",
            true,
            true,
            1_000_000,
            64_000,
            [3.0, 15.0, 0.3, 3.75],
        ),
        bedrock(
            "us.anthropic.claude-opus-4-6-v1",
            "Claude Opus 4.6 (US)",
            true,
            true,
            1_000_000,
            128_000,
            [5.0, 25.0, 0.5, 6.25],
        )
        .levels(&[("xhigh", Some("max"))]),
        bedrock(
            "deepseek.v3.2",
            "DeepSeek-V3.2",
            true,
            false,
            163_840,
            81_920,
            [0.62, 1.85, 0.0, 0.0],
        ),
        bedrock(
            "minimax.minimax-m2.5",
            "MiniMax M2.5",
            true,
            false,
            196_608,
            98_304,
            [0.3, 1.2, 0.0, 0.0],
        ),
        bedrock(
            "qwen.qwen3-vl-235b-a22b",
            "Qwen/Qwen3-VL-235B-A22B-Instruct",
            false,
            true,
            262_000,
            262_000,
            [0.3, 1.5, 0.0, 0.0],
        ),
        bedrock(
            "zai.glm-5",
            "GLM-5",
            true,
            false,
            202_752,
            101_376,
            [1.0, 3.2, 0.0, 0.0],
        ),
        m(
            "anthropic",
            "anthropic-messages",
            "claude-sonnet-4-6",
            "Claude Sonnet 4.6",
            true,
            true,
            1_000_000,
            64_000,
            [3.0, 15.0, 0.3, 3.75],
        ),
        m(
            "anthropic",
            "anthropic-messages",
            "claude-opus-4-7",
            "Claude Opus 4.7",
            true,
            true,
            1_000_000,
            128_000,
            [5.0, 25.0, 0.5, 6.25],
        )
        .levels(&[("xhigh", Some("xhigh"))]),
        m(
            "anthropic",
            "anthropic-messages",
            "claude-haiku-4-5",
            "Claude Haiku 4.5 (latest)",
            true,
            true,
            200_000,
            64_000,
            [1.0, 5.0, 0.1, 1.25],
        ),
        m(
            "google",
            "google-generative-ai",
            "gemini-2.5-pro",
            "Gemini 2.5 Pro",
            true,
            true,
            1_048_576,
            65_536,
            [1.25, 10.0, 0.31, 0.0],
        ),
        m(
            "google",
            "google-generative-ai",
            "gemini-3-pro-preview",
            "Gemini 3 Pro Preview",
            true,
            true,
            1_000_000,
            64_000,
            [2.0, 12.0, 0.2, 0.0],
        )
        .levels(gemini3_pro_levels),
        m(
            "google-vertex",
            "google-vertex",
            "gemini-3-flash-preview",
            "Gemini 3 Flash Preview (Vertex)",
            true,
            true,
            1_048_576,
            65_536,
            [0.5, 3.0, 0.05, 0.0],
        )
        .levels(&[("off", None)])
        .base("https://{location}-aiplatform.googleapis.com"),
        m(
            "google-vertex",
            "google-vertex",
            "gemini-3-pro-preview",
            "Gemini 3 Pro Preview (Vertex)",
            true,
            true,
            1_000_000,
            64_000,
            [2.0, 12.0, 0.2, 0.0],
        )
        .levels(gemini3_pro_levels)
        .base("https://{location}-aiplatform.googleapis.com"),
        copilot(
            "anthropic-messages",
            "claude-opus-4.7",
            "Claude Opus 4.7",
            144_000,
            64_000,
        )
        .levels(&[("xhigh", Some("xhigh"))]),
        copilot(
            "openai-completions",
            "gemini-3-pro-preview",
            "Gemini 3 Pro Preview",
            128_000,
            64_000,
        )
        .compat(json!({
            "supportsStore": false,
            "supportsDeveloperRole": false,
            "supportsReasoningEffort": false,
        })),
        copilot(
            "openai-responses",
            "gpt-5.3-codex",
            "GPT-5.3-Codex",
            400_000,
            128_000,
        )
        .levels(&[("off", None), ("xhigh", Some("xhigh"))]),
        m(
            "groq",
            "openai-completions",
            "qwen/qwen3-32b",
            "Qwen3 32B",
            true,
            false,
            131_072,
            40_960,
            [0.29, 0.59, 0.0, 0.0],
        )
        .levels(&[
            ("minimal", None),
            ("low", None),
            ("medium", Some("default")),
            ("high", Some("default")),
        ])
        .compat(json!({ "supportsReasoningEffort": true }))
        .base("https://api.groq.com/openai/v1"),
        m(
            "groq",
            "openai-completions",
            "openai/gpt-oss-20b",
            "GPT OSS 20B",
            true,
            false,
            131_072,
            65_536,
            [0.075, 0.3, 0.0, 0.0],
        )
        .compat(json!({ "supportsReasoningEffort": true }))
        .base("https://api.groq.com/openai/v1"),
        mistral(
            "mistral-large-latest",
            "Mistral Large (latest)",
            false,
            true,
            262_144,
            262_144,
            [0.5, 1.5, 0.0, 0.0],
        ),
        mistral(
            "magistral-medium-latest",
            "Magistral Medium (latest)",
            true,
            false,
            128_000,
            16_384,
            [2.0, 5.0, 0.0, 0.0],
        ),
        mistral(
            "mistral-medium-3.5",
            "Mistral Medium 3.5",
            true,
            true,
            262_144,
            262_144,
            [1.5, 7.5, 0.0, 0.0],
        ),
        mistral(
            "mistral-small-2603",
            "Mistral Small 4",
            true,
            true,
            256_000,
            256_000,
            [0.15, 0.6, 0.0, 0.0],
        ),
        openai("gpt-5", "GPT-5", [1.25, 10.0, 0.125, 0.0]).levels(&[("off", None)]),
        openai("gpt-5-codex", "GPT-5-Codex", [1.25, 10.0, 0.125, 0.0]).levels(&[("off", None)]),
        openai("gpt-5-mini", "GPT-5 Mini", [0.25, 2.0, 0.025, 0.0]).levels(&[("off", None)]),
        openai("gpt-5-nano", "GPT-5 Nano", [0.05, 0.4, 0.005, 0.0]).levels(&[("off", None)]),
        openai("gpt-5.1", "GPT-5.1", [1.25, 10.0, 0.13, 0.0]).levels(&[("off", Some("none"))]),
        openai("gpt-5.1-codex", "GPT-5.1 Codex", [1.25, 10.0, 0.125, 0.0]).levels(&[("off", None)]),
        openai("gpt-5.2", "GPT-5.2", [1.75, 14.0, 0.175, 0.0])
            .levels(&[("off", Some("none")), ("xhigh", Some("xhigh"))]),
        openai("gpt-5.2-codex", "GPT-5.2 Codex", [1.75, 14.0, 0.175, 0.0])
            .levels(&[("off", None), ("xhigh", Some("xhigh"))]),
        openai("gpt-5.3-codex", "GPT-5.3 Codex", [1.75, 14.0, 0.175, 0.0])
            .levels(&[("off", Some("none")), ("xhigh", Some("xhigh"))]),
        m(
            "openai",
            "openai-responses",
            "gpt-6-astra",
            "GPT-6 Astra",
            true,
            true,
            1_050_000,
            128_000,
            [10.0, 50.0, 1.0, 12.5],
        )
        .levels(&[("off", None), ("minimal", None), ("xhigh", Some("xhigh"))])
        .base("https://api.openai.com/v1"),
        codex("gpt-5.3-codex", "GPT-5.3 Codex", [1.75, 14.0, 0.175, 0.0]),
        codex("gpt-5.4", "GPT-5.4", [2.5, 15.0, 0.25, 0.0]),
        codex("gpt-5.4-mini", "GPT-5.4 Mini", [0.75, 4.5, 0.075, 0.0]),
        codex("gpt-5.5", "GPT-5.5", [5.0, 30.0, 0.5, 0.0]),
        m(
            "opencode",
            "openai-completions",
            "big-pickle",
            "Big Pickle",
            true,
            false,
            200_000,
            128_000,
            [0.0; 4],
        )
        .base("https://opencode.ai/zen/v1"),
        m(
            "opencode",
            "anthropic-messages",
            "claude-sonnet-4-5",
            "Claude Sonnet 4.5",
            true,
            true,
            200_000,
            64_000,
            [3.0, 15.0, 0.3, 3.75],
        )
        .base("https://opencode.ai/zen"),
        m(
            "opencode",
            "google-generative-ai",
            "gemini-3-flash",
            "Gemini 3 Flash",
            true,
            true,
            1_048_576,
            65_536,
            [0.5, 3.0, 0.05, 0.0],
        )
        .levels(&[("off", None)])
        .base("https://opencode.ai/zen/v1"),
        m(
            "opencode",
            "openai-responses",
            "gpt-5.2-codex",
            "GPT-5.2 Codex",
            true,
            true,
            400_000,
            128_000,
            [1.75, 14.0, 0.175, 0.0],
        )
        .levels(&[("off", None), ("xhigh", Some("xhigh"))])
        .base("https://opencode.ai/zen/v1"),
        m(
            "opencode",
            "anthropic-messages",
            "minimax-m2.5-free",
            "MiniMax M2.5 Free",
            true,
            false,
            204_800,
            131_072,
            [0.0; 4],
        )
        .base("https://opencode.ai/zen"),
        m(
            "opencode-go",
            "openai-completions",
            "kimi-k2.5",
            "Kimi K2.5",
            true,
            true,
            262_144,
            65_536,
            [0.6, 3.0, 0.1, 0.0],
        )
        .base("https://opencode.ai/zen/go/v1"),
        m(
            "opencode-go",
            "openai-completions",
            "minimax-m2.5",
            "MiniMax M2.5",
            true,
            false,
            204_800,
            65_536,
            [0.3, 1.2, 0.03, 0.0],
        )
        .base("https://opencode.ai/zen/go/v1"),
        m(
            "openrouter",
            "openai-completions",
            "openai/gpt-5",
            "OpenAI GPT-5",
            true,
            true,
            400_000,
            128_000,
            [1.25, 10.0, 0.125, 0.0],
        ),
        m(
            "cloudflare-workers-ai",
            "openai-completions",
            "@cf/moonshotai/kimi-k2.6",
            "Kimi K2.6",
            true,
            true,
            256_000,
            256_000,
            [0.95, 4.0, 0.16, 0.0],
        )
        .compat(json!({ "sendSessionAffinityHeaders": true }))
        .base(CF_WORKERS),
        m(
            "cloudflare-ai-gateway",
            "anthropic-messages",
            "claude-sonnet-4-5",
            "Claude Sonnet 4.5 (latest)",
            true,
            true,
            200_000,
            64_000,
            [3.0, 15.0, 0.3, 3.75],
        )
        .base(&format!("{CF_GATEWAY}/anthropic")),
        m(
            "cloudflare-ai-gateway",
            "openai-responses",
            "gpt-5.1",
            "GPT-5.1",
            true,
            true,
            400_000,
            128_000,
            [1.25, 10.0, 0.13, 0.0],
        )
        .levels(&[("off", None)])
        .base(&format!("{CF_GATEWAY}/openai")),
        m(
            "cloudflare-ai-gateway",
            "openai-completions",
            "workers-ai/@cf/moonshotai/kimi-k2.6",
            "Kimi K2.6",
            true,
            true,
            256_000,
            256_000,
            [0.95, 4.0, 0.16, 0.0],
        )
        .compat(json!({ "sendSessionAffinityHeaders": true }))
        .base(&format!("{CF_GATEWAY}/compat")),
        m(
            "fireworks",
            "anthropic-messages",
            "accounts/fireworks/models/kimi-k2p6",
            "Kimi K2.6",
            true,
            true,
            262_000,
            262_000,
            [0.95, 4.0, 0.16, 0.0],
        )
        .compat(fireworks_compat.clone())
        .base("https://api.fireworks.ai/inference"),
        m(
            "fireworks",
            "anthropic-messages",
            "accounts/fireworks/routers/kimi-k2p5-turbo",
            "Kimi K2.5 Turbo",
            true,
            true,
            256_000,
            256_000,
            [0.0; 4],
        )
        .compat(fireworks_compat)
        .base("https://api.fireworks.ai/inference"),
        m(
            "together",
            "openai-completions",
            "moonshotai/Kimi-K2.6",
            "Kimi K2.6",
            true,
            true,
            262_144,
            131_000,
            [1.2, 4.5, 0.2, 0.0],
        )
        .levels(&[("minimal", None), ("low", None), ("medium", None)])
        .compat(together_compat(false, Some("together"))),
        m(
            "together",
            "openai-completions",
            "openai/gpt-oss-120b",
            "GPT OSS 120B",
            true,
            false,
            131_072,
            131_072,
            [0.15, 0.6, 0.0, 0.0],
        )
        .levels(&[("off", None), ("minimal", None)])
        .compat(together_compat(true, Some("openai"))),
        m(
            "together",
            "openai-completions",
            "deepseek-ai/DeepSeek-V4-Pro",
            "DeepSeek V4 Pro",
            true,
            false,
            512_000,
            384_000,
            [2.1, 4.4, 0.2, 0.0],
        )
        .levels(&[
            ("minimal", None),
            ("low", None),
            ("medium", None),
            ("high", Some("high")),
            ("xhigh", None),
        ])
        .compat(together_compat(true, Some("together"))),
        m(
            "together",
            "openai-completions",
            "MiniMaxAI/MiniMax-M2.7",
            "MiniMax-M2.7",
            true,
            false,
            202_752,
            131_072,
            [0.3, 1.2, 0.06, 0.0],
        )
        .levels(&[
            ("off", None),
            ("minimal", None),
            ("low", None),
            ("medium", None),
        ])
        .compat(together_compat(false, None)),
        zai(
            "glm-5",
            "GLM-5",
            128_000,
            32_768,
            [0.6, 2.2, 0.0, 0.0],
            true,
        ),
        zai("glm-4.7", "GLM-4.7", 204_800, 131_072, [0.0; 4], true),
        zai(
            "glm-5-turbo",
            "GLM-5-Turbo",
            200_000,
            131_072,
            [0.0; 4],
            true,
        ),
        zai("glm-5.1", "GLM-5.1", 200_000, 131_072, [0.0; 4], true),
        zai(
            "glm-4.5-air",
            "GLM-4.5-Air",
            131_072,
            98_304,
            [0.0; 4],
            false,
        ),
    ];
    let mut models: Vec<ModelSpec> = list.into_iter().map(|m| m.0).collect();
    // Azure OpenAI deployments mirror the OpenAI Responses models (Pi's generator does the same).
    let azure: Vec<ModelSpec> = models
        .iter()
        .filter(|m| m.provider == "openai")
        .map(|m| ModelSpec {
            provider: "azure-openai-responses".into(),
            api: "azure-openai-responses".into(),
            base_url: String::new(),
            ..m.clone()
        })
        .collect();
    models.extend(azure);
    models
}

fn image_model(
    id: &str,
    name: &str,
    input: &[&str],
    output: &[&str],
    cost: [f64; 4],
) -> ImageModelSpec {
    ImageModelSpec {
        provider: "openrouter".into(),
        api: "openrouter-images".into(),
        id: id.into(),
        name: name.into(),
        base_url: OPENROUTER.into(),
        input: input.iter().map(|s| s.to_string()).collect(),
        output: output.iter().map(|s| s.to_string()).collect(),
        cost: Cost {
            input: cost[0],
            output: cost[1],
            cache_read: cost[2],
            cache_write: cost[3],
        },
        headers: Default::default(),
    }
}

/// Built-in OpenRouter image-generation models.
pub(super) fn image_models() -> Vec<ImageModelSpec> {
    let ti: &[&str] = &["text", "image"];
    let it: &[&str] = &["image", "text"];
    let img: &[&str] = &["image"];
    vec![
        image_model(
            "black-forest-labs/flux.2-flex",
            "Black Forest Labs: FLUX.2 Flex",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "black-forest-labs/flux.2-klein-4b",
            "Black Forest Labs: FLUX.2 Klein 4B",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "black-forest-labs/flux.2-max",
            "Black Forest Labs: FLUX.2 Max",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "black-forest-labs/flux.2-pro",
            "Black Forest Labs: FLUX.2 Pro",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "bytedance-seed/seedream-4.5",
            "ByteDance Seed: Seedream 4.5",
            it,
            img,
            [0.0; 4],
        ),
        image_model(
            "google/gemini-2.5-flash-image",
            "Google: Nano Banana (Gemini 2.5 Flash Image)",
            it,
            it,
            [0.3, 2.5, 0.03, 0.083_333_333_333_333_34],
        ),
        image_model(
            "google/gemini-3-pro-image-preview",
            "Google: Nano Banana Pro (Gemini 3 Pro Image Preview)",
            it,
            it,
            [2.0, 12.0, 0.2, 0.375],
        ),
        image_model(
            "google/gemini-3.1-flash-image-preview",
            "Google: Nano Banana 2 (Gemini 3.1 Flash Image Preview)",
            it,
            it,
            [0.5, 3.0, 0.0, 0.0],
        ),
        image_model(
            "openai/gpt-5-image",
            "OpenAI: GPT-5 Image",
            it,
            it,
            [10.0, 10.0, 1.25, 0.0],
        ),
        image_model(
            "openai/gpt-5-image-mini",
            "OpenAI: GPT-5 Image Mini",
            it,
            it,
            [2.5, 2.0, 0.25, 0.0],
        ),
        image_model(
            "openai/gpt-5.4-image-2",
            "OpenAI: GPT-5.4 Image 2",
            it,
            it,
            [8.0, 15.0, 2.0, 0.0],
        ),
        image_model(
            "openrouter/auto",
            "Auto Router",
            ti,
            ti,
            [-1_000_000.0, -1_000_000.0, 0.0, 0.0],
        ),
        image_model(
            "sourceful/riverflow-v2-fast",
            "Sourceful: Riverflow V2 Fast",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "sourceful/riverflow-v2-fast-preview",
            "Sourceful: Riverflow V2 Fast Preview",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "sourceful/riverflow-v2-max-preview",
            "Sourceful: Riverflow V2 Max Preview",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "sourceful/riverflow-v2-pro",
            "Sourceful: Riverflow V2 Pro",
            ti,
            img,
            [0.0; 4],
        ),
        image_model(
            "sourceful/riverflow-v2-standard-preview",
            "Sourceful: Riverflow V2 Standard Preview",
            ti,
            img,
            [0.0; 4],
        ),
    ]
}
