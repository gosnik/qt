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
  (`native-<micros>.jsonl`, `fork-<id>.jsonl`, `clone-<id>.jsonl`, `-2`… suffixes on
  collision), stored flat in the session directory with `cwd` and `parentSession` in the
  header; listing scans the directory recursively so Pi's per-cwd folders are found too.
- **Session entries:** all Pi v3 entry types (`message`, `model_change`,
  `thinking_level_change`, `compaction`, `branch_summary`, `custom`, `custom_message`,
  `label`, `session_info`) and message roles `custom` and `bashExecution`; v1 (no ids,
  `firstKeptEntryIndex`) and v2 (`hookMessage`) files are migrated to v3 on open and
  rewritten. Like Pi, a session file is first written when it holds an assistant message.
  Context replay follows Pi `buildSessionContext` (compaction summary, kept entries from
  `firstKeptEntryId`, branch summaries, custom messages), with Pi's summary prefixes.
- **Session directory:** `--session-dir` > `PI_CODING_AGENT_SESSION_DIR` > `sessionDir`
  setting > `<agent home>/sessions`. Id-prefix resolution and its error wording
  (`No session found matching '…'`, `Session prefix '…' is ambiguous; matches: …`) and
  most-recent selection (this cwd first, then modification time) follow the archived qt.
- **Tree and forks:** `navigateTree`, `branch_with_summary`, labels, `fork` (before a user
  message, or at an entry), clone, import and `forkFrom` follow Pi; summaries use Pi's
  branch-summary prompt and preamble with read/modified file lists.
- **Compaction:** Pi's prompts (initial, update, split-turn prefix), cut-point rules
  (never at tool results, split-turn detection), chars/4 estimates, file tracking in
  `details`, `Already compacted` / `Nothing to compact (session too small)` errors and
  `compaction.*` defaults (enabled, reserve 16384, keep 20000). Auto-compaction runs on
  the threshold and recovers from context overflow by compacting and retrying once.
- **Auto-retry:** Pi's retryable-error pattern and `retry.*` defaults (enabled, 3 retries,
  2 s base, exponential); the failed response stays in the session but leaves the context.
- **Queues:** steering / follow-up / next-turn delivery and `steeringMode` /
  `followUpMode` (`one-at-a-time` default, legacy `queueMode` honoured) follow Pi's loop.
- **Active tools:** persisted as the archived qt's `custom` entry `tools-config`
  (`{"enabledTools": [...]}`) and restored from the current branch.
- **Goals:** `/goal` wording, prompts, budget rules and elapsed/token formatting follow the
  archived qt.
- **Tools:** names, JSON schemas, descriptions, prompt snippets and guidelines, the
  2000-line / 50 KB limits and continuation notices follow Pi's coding-agent tools. `edit`
  accepts Pi's `edits[]` form and the legacy single `oldText`/`newText` form. Bash
  truncation notices use Pi's `[Showing lines S-E of T. Full output: <path>]` wording.
- **Archived tool extras:** the archived qt's implementation names (`read_text`,
  `write_text`, `edit_text`, `search_text`, `list_directory`, `find_files`) resolve to the
  Pi-named tools, and its argument aliases are accepted (`file_path`/`filePath`, `text` for
  `write` content, `old_string`/`oldString`/`new_string`/`newString`/`replace_all`, grep
  `query`/`include`/`file_glob`/`-i`/`-A`/`-B`/`-C`/`literal_text`, find `glob`/`include`,
  bash `cwd`/`working_dir`, apply_patch `patchText`). Advertised schemas stay Pi-shaped plus
  the archived options (`edit` `replace_all`; grep `output_mode`, `head_limit`, `offset`,
  `multiline`, `type`, `hidden`, `no_ignore`; find `type`, `directories`, `hidden`,
  `no_ignore`; ls `recursive`, `depth`, `ignore`; bash `timeout_ms`, `workdir`). The
  archived-only tools `append_text`, `apply_patch`, `clipboard`, `complete_path`,
  `package_manager` (detect and plan only), `web_fetch`, `web_search` and `web_run` are
  available through `ToolRegistry::builtin()`; `--tools` globs, `--no-tools`,
  `--no-builtin-tools` and offline mode are modelled by `ToolSelection`.
- **Workspace policy:** `workspaceRoot` (default: enclosing git repository, else cwd) and
  `allowExternalFilesystemAccess` (global settings only; `.pi/settings.json` may only narrow)
  confine every path argument and the bash working directory, with symlinks resolved. Refusals
  read `path escapes workspace_root: <path>` / `working directory escapes workspace_root`.
  `ToolContext::new(cwd)` confines to `cwd`. The prompt sentence matches the archived
  `<tool_permissions>` text.
- **Mutation queue:** with `MutationPolicy::Queue`, `write`, `edit`, `append_text`,
  `apply_patch` and `bash` return a `pending_approval` result with a `mutation-<n>` id;
  approved mutations re-run once via `ToolRegistry::execute_approved`.
- **Read guard:** the second and third identical reads of an unchanged file range carry the
  archived "This unchanged file range has already been read." warning and the fourth is
  refused with the archived `read_text refused a duplicate unchanged read…` message.
- **Web tools:** the public-URL policy (no credentials, localhost, private/reserved IPv4,
  IPv6 literals; every resolved address must be public; connections pinned to the checked
  addresses), five-redirect limit, 2 MiB download cap, simplified-text conversion,
  DuckDuckGo `uddg` decoding, `web_run` `turnNsearchM` / `turnNfetchM` ref ids and limits
  follow the archived qt. `PI_OFFLINE` or offline mode disables them with
  `web access is disabled in offline mode`.
- **System prompt:** Pi's structure, with "qt" in place of "pi" and without the Pi
  documentation section. Context files are searched in the same way (`AGENTS.md`, then
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`.

## Differences

- **Invalid session files:** a file without a valid header is an error; Pi silently starts
  a fresh session over it. Entries of unknown types, or known types with invalid fields,
  are kept verbatim (they survive rewrites) and reported as diagnostics, never replayed.
- **Threshold compaction** needs a known context window (from model discovery or
  `Conversation::set_context_window`); without one only overflow recovery runs. Summaries
  are requested with thinking off and `maxOutputTokens` as the model output limit.
- **Overflow detection** also treats qt's own body-less `HTTP 400/413 …:` transport errors
  as overflow, matching Pi's `4xx (no body)` pattern.
- **Goals:** the archived qt kept goals in memory and its prompts named an `update_goal`
  tool that did not exist. qt persists the goal as a `custom` entry `goal`, offers an
  `update_goal` tool (`complete` / `blocked`) while a goal is active, accounts tokens
  (input + output + cache) and wall time per run, injects the budget-limit prompt once as
  a steering message, and stops auto-continuation after a continuation turn that made no
  tool calls.
- **Parallel tools:** a batch runs concurrently only when every tool in it reports
  `parallel_safe()`; Pi runs batches in parallel unless a tool is marked sequential.
  Results are committed in source order either way.
- **Hooks:** `before_provider_request` rewrites qt's `Request` (model, system prompt,
  messages, tools, limits) rather than the provider JSON payload.
- **Manual `/compact`** refuses to run while the agent is running instead of aborting it.
- **Events:** one `AgentStart` / `AgentEnd` pair spans a whole prompt including retries,
  compaction and follow-ups (Pi emits `agent_end` per loop run).
- **`edit` fallback:** when an exact match fails, qt matches line by line on NFKC-folded
  text, ignoring trailing whitespace and typographic quote, dash and space variants, and
  replaces the original lines. Pi and the archived qt apply the replacement to
  fuzzy-normalised file content instead. `replace_all` replaces every non-overlapping match;
  without it Pi's uniqueness errors apply.
- **`grep` / `find`:** implemented in-process with the `ignore` crate rather than by calling
  `rg`/`fd`. Output format is `path:line: text` (matches) and `path-line- text` (context),
  with `--` between separated context groups. Hidden files are skipped unless `hidden` is
  set (`.git` is always skipped). `find` lists files only unless `directories` is set or
  `type` is `d`, and never descends into `node_modules` (archived behaviour). grep
  `multiline` really spans lines (the archived qt accepted the flag but ignored it).
- **`read`:** directories return their listing (archived) instead of an error; missing
  files suggest similarly named siblings. Images are detected by content. Without a resize
  hook (`ImageOptions::resize`, owned by the image subsystem) images over 5 MB are omitted
  with a note rather than resized.
- **Replaying history:** assistant turns containing only reasoning are omitted from
  OpenAI-compatible requests, because some servers (Ollama) reject assistant messages that
  have neither content nor tool calls. Thinking blocks are replayed to Anthropic only when
  they carry a signature from Anthropic.
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
- **`ls`:** recursive listings do not follow symlinked directories or list `.git` contents.
- **`bash`:** runs `/bin/bash -lc` when available (archived), else `bash -lc` from `PATH`,
  else `/bin/sh -c`. Relative `workdir` values resolve against the workspace root. The full
  output of truncated commands is written to `qt-bash-<id>.log` in the temp directory and
  stays readable by `read` even outside the workspace.
- **`apply_patch`:** each `@@` hunk is matched separately and in order (the archived qt
  concatenated all hunks of a file into one block); `*** Move to:` and `*** End of File` are
  supported; a bare empty line inside a hunk is an empty context line. The result text is
  Codex's `Success. Updated the following files:` list.
- **`web_fetch`:** non-HTML responses are returned as-is (the archived qt stripped tags from
  every response); bodies over the cap are cut with a note instead of failing; HTTP errors
  are reported as tool errors. URL fragments are dropped instead of rejected.
- **`clipboard`:** image copies pass `-t <mime>` to xclip and `--type <mime>` to wl-copy.
- **Not yet implemented** (see `PLAN.md`): GUI tree / fork / resume pickers and the
  missing-cwd dialog, provider-level retry (`retry.provider.*`, `Retry-After`), slash commands, skills, OAuth
  logins, in-process llama.cpp, memory, extensions, MCP, subagents, the project index and
  RPC mode.
