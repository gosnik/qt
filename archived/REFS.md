# Secondary Reference Index

`reference/pi` remains the behavioral source of truth for this port. The files below are secondary references only: use them for implementation patterns, protocol shape ideas, TUI behavior, config layering, and extension architecture when `reference/pi` is silent or thin.

## Project strengths

- `reference/codex`: strongest non-`pi` source for native-ish CLI/TUI structure, typed app-server contracts, approval flows, config loading, and auth management.
- `reference/claude-code`: strongest source for product-level command/tool/skill/MCP behavior, structured headless IO, and bridge/web contract surface area.
- `reference/hermes-agent`: strongest source for modern TUI UX, skill preprocessing/loading, provider metadata, multimodal routing, and codex app-server adapter patterns.

## Memory V2 research references

These sources inform the user-approved [Memory V2 plan](mem_v2.md). They do not
replace the existing governed-memory contracts in `MEM.md`.

| Source | Why it is useful |
| --- | --- |
| [SQLite FTS5](https://www.sqlite.org/fts5.html) | Native lexical indexing and BM25 ranking. |
| [Nomic Embed Text v1.5 GGUF model card](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF) | Existing local encoder evaluation: compatible GGUF weights, query/document prefixes and context limits. |
| [Reciprocal rank fusion](https://cormack.uwaterloo.ca/cormacksigir09-rrf.pdf) | Combining lexical, dense and graph candidate rankings without mixing raw score scales. |
| [GraphRAG query overview](https://microsoft.github.io/graphrag/query/overview/) | Local entity-centered retrieval and the distinction from deferred global community synthesis. |
| [LarQL revision 3915016](https://github.com/chrishayuk/larql/tree/391501611e28d7be65c47ef148ae6646831841a9) | Source authority, scoped promotion and recovery concepts; residual inference is outside the initial implementation. |
| [SPLADE](https://arxiv.org/abs/2107.05720) | Deferred learned sparse representations and weighted postings. |
| [ColBERT](https://arxiv.org/abs/2004.12832) and [ColBERTv2](https://arxiv.org/abs/2112.01488) | Deferred token-level late interaction and compression. |

## External protocol references

| Source | Why it is useful |
| --- | --- |
| [OpenAI GPT-6 Astra model documentation](https://developers.openai.com/api/docs/models/gpt-6-astra) | Authoritative model ID, API capabilities, context/output limits, reasoning efforts, and token prices for Astra, which is absent from the checked-in Pi catalog. |
| [OpenAI Models API](https://developers.openai.com/api/reference/resources/models) | Authenticated model-ID discovery through GET /v1/models; this endpoint does not return context limits, capabilities, or pricing. |
| [`0xKobold/pi-ollama`](https://github.com/0xKobold/pi-ollama) | Pi extension behavior for Ollama discovery, `/api/show` capability/context metadata, zero-cost model records, and `/ollama` inspection commands. |
| [Ollama API documentation](https://github.com/ollama/ollama/blob/main/docs/api.md) | Authoritative endpoint and payload contract for tags, show, version, running models, pull, chat load/unload, and delete operations. |

## External llama.cpp references

| Source | Why it is useful |
| --- | --- |
| [PrismML llama.cpp fork (`prism` branch)](https://github.com/PrismML-Eng/llama.cpp/tree/prism) | Authoritative implementation of PrismML's group-128 `Q2_0` tensor layout, quantization paths, and CPU, Metal, CUDA, and Vulkan kernels used by the vendored built-in llama runtime. |
| [Ternary Bonsai 1.7B GGUF model card](https://huggingface.co/prism-ml/Ternary-Bonsai-1.7B-gguf/blob/main/README.md) | Authoritative model metadata and format contract for the recommended `Ternary-Bonsai-1.7B-Q2_0.gguf`, including Qwen3 architecture, context/vocabulary sizes, 128-value/34-byte blocks, and local inference examples. |
| [PrismML Bonsai demo](https://github.com/PrismML-Eng/Bonsai-demo) | Official end-to-end examples for selecting, downloading, serving, benchmarking, and exercising Ternary Bonsai models across CPU and accelerated llama.cpp backends. |

## `reference/codex`

| File | Why it is useful |
| --- | --- |
| `reference/codex/codex-rs/exec/src/cli.rs` | Concrete CLI parser layout for subcommands, global flags, prompt/stdin behavior, JSON mode flags, and compatibility traps. |
| `reference/codex/codex-rs/exec/src/cli_tests.rs` | Good regression patterns for flag ordering, resume semantics, deprecated switches, and parser edge cases. |
| `reference/codex/codex-rs/tui/src/tui.rs` | Native terminal bootstrap for raw mode, alternate screen handling, redraw scheduling, focus changes, and viewport cleanup. |
| `reference/codex/codex-rs/tui/src/tui/keyboard_modes.rs` | Useful terminal capability probes and keyboard-enhancement toggles, especially tmux/WSL/VS Code edge handling. |
| `reference/codex/codex-rs/tui/src/clipboard_paste.rs` | Good reference for bracketed paste, image paste fallback paths, and WSL clipboard interoperability. |
| `reference/codex/codex-rs/tui/src/bottom_pane/approval_overlay.rs` | High-value approval UI contract for exec/apply-patch/MCP requests and explicit decision routing. |
| `reference/codex/codex-rs/core/src/config/mod.rs` | Large but valuable config loader covering layered config resolution, permission/profile merging, MCP constraints, and persisted settings. |
| `reference/codex/codex-rs/core/src/tools/handlers/mcp_resource.rs` | Strong model for listing/reading MCP resources with typed payloads, per-server aggregation, and turn item emission. |
| `reference/codex/codex-rs/core/src/tools/handlers/apply_patch.rs` | Useful for freeform patch parsing, streaming diff progress, permission-aware file change handling, and filesystem routing. |
| `reference/codex/codex-rs/login/src/auth/manager.rs` | Best auth-management cross reference here: API-key vs ChatGPT auth, token refresh, persistence, and refresh-failure classification. |
| `reference/codex/codex-rs/app-server-protocol/src/protocol/v2/thread.rs` | Primary typed contract for thread startup, dynamic tools, permission profile selection, and sticky environment setup. |
| `reference/codex/codex-rs/app-server-protocol/src/protocol/v2/turn.rs` | Primary typed contract for turn start/steer, per-turn overrides, collaboration mode, and output schema handling. |
| `reference/codex/codex-rs/app-server-protocol/src/protocol/v2/item.rs` | Best place to study streamed item/event shapes for user messages, tool items, approvals, and thread history payloads. |
| `reference/codex/codex-rs/app-server-protocol/src/protocol/v2/command_exec.rs` | Useful standalone exec/RPC contract for PTY sizing, streamed stdout/stderr, stdin writes, timeout rules, and sandbox selection. |

## `reference/claude-code`

| File | Why it is useful |
| --- | --- |
| `reference/claude-code/docs/architecture.md` | Fast orientation for the end-to-end pipeline: CLI parse, query/tool loop, REPL/TUI, config, and state management. |
| `reference/claude-code/docs/subsystems.md` | Good map of bridge, MCP, permissions, plugins, skills, tasks, and multi-agent subsystems. |
| `reference/claude-code/docs/commands.md` | Handy product inventory for slash-command surface area and feature parity checks. |
| `reference/claude-code/docs/tools.md` | Concise tool catalog with permission and concurrency framing that is useful when shaping native tool contracts. |
| `reference/claude-code/src/cli/print.ts` | Main headless/interactive orchestration path; useful for session startup, permission bridging, MCP reconciliation, and JSON/non-JSON flows. |
| `reference/claude-code/src/cli/structuredIO.ts` | Strong structured stdio transport reference for control requests/responses, NDJSON framing, and duplicate-response hardening. |
| `reference/claude-code/src/skills/loadSkillsDir.ts` | Best reference for markdown-driven skill discovery, frontmatter parsing, deduplication, path scoping, and lightweight token estimation. |
| `reference/claude-code/src/outputStyles/loadOutputStylesDir.ts` | Useful pattern for user/project markdown-driven style loading and frontmatter-based prompt customization. |
| `reference/claude-code/src/services/mcp/config.ts` | Good MCP config source for multi-scope config merging, atomic writes, URL/command deduplication, and plugin-provided MCP servers. |
| `reference/claude-code/src/bridge/types.ts` | Strong bridge/remote-control contract reference for session lifecycle, permission responses, environment registration, and reconnect semantics. |
| `reference/claude-code/web/lib/api/types.ts` | Useful shared API typing reference for stream events, API errors, file/search results, and MCP JSON-RPC payloads. |

## `reference/hermes-agent`

| File | Why it is useful |
| --- | --- |
| `reference/hermes-agent/website/docs/user-guide/tui.md` | Best high-level UX reference for TUI behavior: instant-first-frame startup, queued input, overlays, status line, and session resume behavior. |
| `reference/hermes-agent/ui-tui/src/app.tsx` | Minimal but useful TUI composition entry showing how gateway state, transcript, composer, and layout get wired together. |
| `reference/hermes-agent/ui-tui/src/gatewayClient.ts` | Strong runtime transport reference for sidecar/gateway startup, attach URLs, log redaction, buffering, ready timeouts, and reconnect behavior. |
| `reference/hermes-agent/agent/prompt_builder.py` | Valuable system-prompt assembly reference for context-file scanning, skill index building, and instruction-source composition. |
| `reference/hermes-agent/agent/skill_preprocessing.py` | Useful for skill templating, inline shell expansion, timeout handling, and safe preprocessing of markdown skill content. |
| `reference/hermes-agent/agent/skill_utils.py` | Good lightweight reference for frontmatter parsing, platform filtering, disabled-skill rules, and external skill directory discovery. |
| `reference/hermes-agent/website/docs/guides/work-with-skills.md` | Good behavioral reference for progressive skill loading, install flow, config prompts, and skill packaging expectations. |
| `reference/hermes-agent/website/docs/guides/build-a-hermes-plugin.md` | Helpful secondary reference for plugin manifests, tool schema design, bundled skills, and extension packaging shape. |
| `reference/hermes-agent/agent/model_metadata.py` | Strong provider/model metadata reference for context-window lookup, provider-specific fallbacks, and CA-bundle aware metadata fetches. |
| `reference/hermes-agent/agent/image_routing.py` | Best multimodal routing reference here: native-vs-text image handling, capability checks, MIME sniffing, and retry-friendly attachment rules. |
| `reference/hermes-agent/agent/transports/codex_app_server.py` | Clean wire-level reference for spawning `codex app-server`, JSON-RPC framing, request/notification queues, and stderr diagnostics capture. |
| `reference/hermes-agent/agent/transports/codex_app_server_session.py` | Best higher-level adapter reference for driving turns over the app-server, approval bridging, interrupt handling, and projector-based event translation. |
