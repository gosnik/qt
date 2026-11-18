# Compatibility notes

How the Rust qt relates to Pi (`reference/pi`, the behavioural reference) and to the
archived C++ qt (`archived/`).

## Compatible

- **Agent home:** same `~/.qt` layout and `QT_CODING_AGENT_DIR` override as the archived qt.
  `settings.json` keys `defaultProvider`, `defaultModel` and `ollama.instances` are read with
  the same meaning. Unknown keys are preserved.
- **Credentials:** `auth.json` `{ "version": 1, "providers": { "<id>": { "apiKey" | "token", "metadata" } } }`
  as written by the archived qt; OAuth logins keep the access token in `token` and
  `{ type: "oauth", access, refresh, expires, accountId }` in `metadata`. Pi's legacy top-level
  shape (`{ "<id>": { "type": "api_key", "key" } }` / `{ "type": "oauth", ... }`) is migrated
  on load. Saving re-reads the file and writes back only the entries changed through that
  store, so a token refreshed by another session is not overwritten. Environment variables
  follow Pi's table (`ANTHROPIC_OAUTH_TOKEN` before `ANTHROPIC_API_KEY`, `ZAI_API_KEY`,
  `CLOUDFLARE_API_KEY`, ... for every catalog provider); ambient Vertex/Bedrock credentials
  resolve to `<authenticated>`. Missing-key messages use the archived wording.
- **OAuth:** PKCE logins with the Pi client ids, scopes, callback ports (53692 for Anthropic,
  1455 for OpenAI Codex), `PI_OAUTH_CALLBACK_HOST`, pasted-code fallback and token
  exchange/refresh bodies. Anthropic expiry keeps Pi's 5 minute margin. Tokens are refreshed
  before a request when expired and persisted (the archived qt never refreshed). The archived
  Anthropic client id was missing a digit; Pi's id is used.
- **Model catalog:** the archived built-in providers and models (context window, max tokens,
  prices, thinking-level maps, compat) plus Azure OpenAI mirrors of the OpenAI models as
  Pi generates them; `models.json` provider entries (`baseUrl`, `api`, `headers`, `apiKey`
  as literal / environment variable / `!command`, `authHeader`, `compat`, `models`,
  `modelOverrides`) with the archived validation messages; thinking levels
  `off|minimal|low|medium|high|xhigh` with Pi's supported-level and clamping rules; costs
  from catalog prices with OpenAI service-tier multipliers (flex 0.5, priority 2, 2.5 for
  gpt-5.5; Codex `default` responses keep the requested tier).
- **Providers:** request shapes follow Pi: Anthropic (API key, Claude Pro/Max OAuth with the
  Claude Code identity, betas and tool-name casing, adaptive thinking for Opus/Sonnet 4.6+,
  budget thinking otherwise, `cache_control` with 1 h TTL for long retention, fine-grained
  tool streaming / interleaved thinking betas, session affinity), OpenAI Responses (reasoning
  items replayed from the thinking signature with `encrypted_content`, `call_id|item_id` tool
  ids with foreign-id hashing, `prompt_cache_key`/`prompt_cache_retention`, `session_id`
  headers), the ChatGPT Codex endpoint (JWT account id, `originator: pi`, instructions,
  `text.verbosity`, usage-limit messages) and Azure OpenAI (`AZURE_OPENAI_*`, deployment map,
  `api-key`). Chat completions use Pi's compatibility detection by provider and host and
  the per-model `compat` overrides. OpenRouter attribution headers are sent unless
  `telemetryEnabled` or `enableInstallTelemetry` is false.
- **HTTP retries:** `retry.provider.{timeoutMs,maxRetries,maxRetryDelayMs}`; 408/409/425/429/5xx
  and connection errors are retried with exponential backoff (SDK defaults: 2 retries,
  0.5 s doubling to 8 s) honouring `Retry-After`/`retry-after-ms`; a requested delay above
  `maxRetryDelayMs` (default 60 s) fails immediately with the delay in the message.
- **OpenRouter:** pricing, limits and modalities from `/models`, cached in
  `openrouter_models.json` (archived format; 7-day freshness for network fallback, stale
  data accepted at startup, `PI_OFFLINE` reads only the cache); `/credits`; image generation
  with the archived image-model list.
- **Ollama:** `ollama.instances` entries (`id`, `name`, `baseUrl`, `apiKey`, `enabled`,
  `timeoutMs`, cached `models`) with the archived validation messages; provider ids
  `ollama:<id>`; version, tags + show (capabilities, context length), ps, pull, load/unload
  (`/api/chat` keep-alive) and delete.
- **Images:** Pi's resize strategy (2000×2000, 4.5 MiB base64, PNG vs JPEG at falling
  qualities, then 25 % shrink steps), EXIF orientation applied, Pi's dimension note,
  `images.autoResize` (default true).
- **Sessions:** Pi v3 JSONL. Messages use Pi's `role`-tagged shapes (`text`, `thinking` with
  `thinkingSignature`, `toolCall`, `image`; `toolResult` with `toolCallId`, `isError`,
  `details`). Usage is written with the archived qt's snake_case fields and read with Pi's
  camelCase names as aliases. Session ids and file names follow the archived qt
  (`native-<micros>.jsonl`, stored flat in `sessions/` with `cwd` in the header).
- **Tools:** names, JSON schemas, descriptions, prompt snippets and guidelines, the
  2000-line / 50 KB limits and continuation notices follow Pi's coding-agent tools. `edit`
  accepts Pi's `edits[]` form and the legacy single `oldText`/`newText` form.
- **System prompt:** Pi's structure, with "qt" in place of "pi" and without the Pi
  documentation section. Context files are searched in the same way (`AGENTS.md`, then
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`.

## Differences

- **Session tree:** only the chain of `message` entries from the last entry to the root is
  replayed. Branch, compaction, label and model-change entries are kept on disk but ignored.
- **`edit` fallback:** when an exact match fails, qt matches line by line, ignoring trailing
  whitespace and typographic quote, dash and space variants, and replaces the original
  lines. Pi applies its replacement to fuzzy-normalised file content instead.
- **`grep` / `find`:** implemented in-process with the `ignore` crate rather than by calling
  `rg`/`fd`. Output format is `path:line: text` (matches) and `path-line- text` (context).
  Hidden files are skipped, as with ripgrep's defaults.
- **`read`:** images over 5 MB are rejected rather than resized.
- **Replaying history:** Pi's `transformMessages` rules apply (foreign thinking becomes
  text, tool-call ids are normalised per API, missing tool results get a synthetic
  "No result provided" error). In addition, tool results without a matching call are dropped.
  Assistant turns containing only reasoning are still omitted from chat-completions requests.
- **Responses text ids:** message items are replayed with `msg_<index>` ids because text
  blocks have no `textSignature` field yet; the original item id and `phase` are not kept.
- **Ollama:** provider ids are always `ollama:<id>` (the first Rust milestone used a bare
  `ollama` for the first instance); with no instances configured, `ollama:local` at
  `http://127.0.0.1:11434` is offered. Ollama requests use `max_tokens`, the `system` role
  and no `store` field.
- **Chat completions:** Pi's OpenRouter `reasoning_details` replay on tool calls is not kept,
  because tool-call blocks have no `thoughtSignature` field.
- **OpenRouter listing:** only tool-capable models are listed (models whose
  `supported_parameters` omit `tools` are hidden), in addition to the archived text-output filter.
- **Codex transport:** SSE only; Pi's WebSocket transport is not implemented.
- **Out of scope:** Google/Vertex, Bedrock, Mistral and GitHub Copilot keep their catalog
  records but have no transport and are not offered as providers.
- **Not yet implemented** (see `PLAN.md`): compaction, slash commands, skills, `/login`
  GUI, in-process llama.cpp, memory, extensions, MCP, subagents, the project index and
  RPC mode.
