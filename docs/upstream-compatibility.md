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
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`. `SYSTEM.md`
  replaces the body and `APPEND_SYSTEM.md` follows it (`<cwd>/.pi/` wins over the agent
  home), as in the archived qt; `--system-prompt` replaces the discovered file and
  `--append-system-prompt` values are appended to it, and (as in Pi) a value naming an
  existing file is replaced by its contents. The prompt ends with the archived qt's
  `# Environment Context` / `<environment_context>` block (cwd, workspace root, shell,
  UTC date, timezone, network, tool permissions, active tool count) instead of Pi's
  `Current date:` / `Current working directory:` lines.
- **Skills (`qt_core::resources::skills`):** archived discovery order (`<agent_home>/skills`,
  `~/.agents/skills` for a `~/.pi/agent` home, `<repo>/.pi/skills`, `.agents/skills` from the
  repository root down to cwd), `settings.skillRoots`/`skills` with `!`/`+`/`-` filters,
  `features.loadSkills`, `.gitignore`/`.ignore`/`.fdignore`, `package.json` `pi.skills`,
  `SKILL.md` vs root `*.md`, frontmatter checks and collision diagnostics with the archived
  wording. The `<available_skills>` block is the shared Pi/archived text and, as in the
  archived qt, is emitted whether or not the `read` tool is active. `/skill:<name> args`
  expands to the skill body plus `\n\nUser: <args>` (archived wording, not Pi's `<skill>`
  wrapper), gated by `enableSkillCommands`.
- **Prompt templates (`qt_core::resources::templates`):** archived roots, frontmatter
  (`description`, `argument-hint`), 60-character first-line fallback, shell-like argument
  splitting and `$N`, `$@`, `$ARGUMENTS`, `${@:N}`, `${@:N:L}`, `{{args}}`, `{{arguments}}`,
  `${N}`, `{{N}}` placeholders, expanded in one pass (substituted text is not re-expanded,
  unlike Pi's sequential replaces) with saturating index arithmetic.
- **Themes (`qt_core::resources::themes`):** Pi's JSON theme format with the 51 required
  tokens, `vars`, hex and ANSI-256 values and the optional `export` section; Pi's built-in
  `dark`/`light`; theme roots `<agent_home>/themes`, `<repo>/.pi/themes`,
  `<repo>/.agents/themes` plus `themeRoots` and filters; archived error wording. Theme names
  are matched case-insensitively with Unicode lowercasing (the archived qt used NFKC case
  folding). Custom theme files are watched with debounced reload (`ThemeWatcher`).
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`.
- **MCP client (`qt-mcp`):** same config sources and merge order as the archived qt
  (settings `mcpServers`/`mcp_servers`, then `~/.config/mcp/mcp.json`, `<agent_home>/mcp.json`,
  `<workspace>/.mcp.json`, `<workspace>/.pi/mcp.json`; later files replace servers of the same
  name; servers ordered by name), the same per-server keys and validation messages, and MCP
  files cannot set `allowExternalFilesystemAccess`. Protocol `2025-03-26`, client name `qt`;
  stdio servers get the archived qt's filtered environment and append stderr to
  `<agent_home>/logs/mcp-stderr.log` (0600). Proxy tools are named
  `mcp_<server>_<tool>` with description `MCP <server>/<tool>: <description>` and the remote
  `inputSchema`; tool text is the `text` items joined by newlines, else the result JSON.
  Error text is redacted with the archived qt's secret markers. Transport failures are retried
  five times with 100 ms exponential backoff. Sampling applies the per-server `sampling`
  policy (`enabled`, `model`, `allowed_models`, `max_tokens_cap` default 4096, `timeout`
  default 30 s) and returns `{role, content, model, stopReason}` as before.

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
- **Resource settings:** a `skillRoots`/`promptRoots`/`themeRoots` array holding only filter
  entries narrows the discovered roots instead of replacing them with none (the archived qt
  dropped every root in that case). Invalid resource settings become diagnostics rather than
  aborting startup. `.gitignore` patterns without a trailing slash also ignore directories
  (the archived matcher missed directories because it matched `name/`).
- **Not yet implemented** (see `PLAN.md`): GUI tree / fork / resume pickers and the
  missing-cwd dialog, provider-level retry (`retry.provider.*`, `Retry-After`), slash commands, skills, OAuth
  logins, in-process llama.cpp, memory, extensions, MCP, subagents and the project index.

## In-process llama.cpp (`qt-llama`)

The `qt-llama` crate ports the archived qt llama integration. `LlamaProviderBackend` serves
the `llama` provider through qt-core's provider backends (runs, compaction, summaries, the
model picker and `--list-models`); its models are `settings.llama.models` plus models found by
`--discover-local-models` (persisted to `settings.json` as archived and registered for the
process even when persisting fails). The runtime is created on first use and replaced when the
effective llama settings change. The GUI `/llama` panel is not built yet (`/llama status`,
`models` and `discover` are answered by `qt-runtime`).

- **Compatible:** `settings.llama` keys, defaults, error and validation messages; path
  resolution from the settings file directory with `~`; GGUF discovery roots, ids,
  quantisation names (including `Q2_0` = 41), classification, usage notes and memory-task
  model selection; persisting discovered models to `settings.json`; offload planning (1 GiB
  headroom per device, then 75/50/25 %/CPU retries); sampler chain order and defaults
  (temperature 0.3, top-p 0.9, 256 output tokens); grammar resolution; the prompt protocol
  (OpenAI-style request JSON with sorted keys, controller messages, `tool_choice` forcing on
  file/search/model words, first-JSON-object tool-call parsing); embedding queue timeout and
  cancellation; KV cache operations and runtime status fields.
- **Differences:** the `session` KV policy reuses the longest cached token prefix of the new
  prompt (the archived qt appended every full prompt after the previous one, duplicating
  context, and pruned at a 70 % watermark); generation is cancellable between tokens and
  during decode; streamed text is split only at UTF-8 boundaries; tool results are sent as
  the result text (with `is_error` when set) because Rust tool results have no structured
  payload; tool calls are only parsed when tools were offered; the request `maxTokens`
  caps the configured output limit; `tensorSplit` is padded to `llama_max_devices()`;
  embedding contexts size the batch to the context so encoder models accept full inputs.
  `kvCache.maxSessions` and `kvCache.clearOnModelChange` are parsed and validated but, as in
  the archived qt, have no runtime effect.

## Project index (`qt-index`)

Ported from the archived `index` subsystem and `tools/index_tool.cpp`.

- **Same as archived:** file classification and skip rules, `.gitignore` handling, build
  emulators (CMake Makefile/Ninja build trees, Make/Kbuild with `.config`, qmake, waf,
  PlatformIO incl. `.pio/libdeps`, Cargo/npm/Python), the `compile_commands.json` written to
  the project root (inferred `cc -std=c17` / `c++ -std=c++17` commands, eight defines,
  28 includes, `.index/objects/<file>.o` outputs), the line-based C/C++ code index, the
  SQLite schema (`meta`, `project_files`, `compilation_databases`, `build_file_counts`,
  `classes`, `methods`, `functions`, `call_graph`, `skipped_code_files`,
  `scan_fingerprints`, FTS5 `index_fts`), 30 s rescans with low-priority pacing,
  `index_stale` after loading a persisted snapshot, the six tools (`symbol`,
  `analyze_file`, `references`, `build_context`, `search_index`, `index`) with their
  schemas, limits, `invalid_arguments` / `invalid_index_arguments` / `index_not_ready`
  payloads, output guards (64 KiB strings, 1000 array items) and the registry rewrites for
  `index` calls (hoisting `operation`/`category`/`include_schemas`, `find_symbol` /
  `symbol_lookup` / `lookup_symbol` → `code.symbol`, `pattern` → `name`).
- **Router groups:** only `core`, `code`, `fs`, `git` and `build`. The `serial`, `debug`,
  `rtos`, `fetch` and `util` operations are not ported (decision 2026-11-18); the router
  description and `category` hint list only the ported groups, and calls to the others
  fail with `unknown index operation`.
- **State directory:** `<agent_home>/index/<hash>/` where `<hash>` is the decimal FNV-1a
  64-bit hash of the canonical project root. The archived build used `std::hash`, which is
  not stable across builds, so archived state directories are not reused (the index is
  simply rebuilt).
- **Tree-sitter:** the `tree-sitter` / `tree-sitter-cpp` crates replace the vendored C
  runtime; tree files hold the same S-expression text. Manifest `mtime` values are Unix
  nanoseconds rather than `file_time_type` ticks, so an archived manifest triggers a
  one-time reparse.
- **Fingerprints:** per-file `fnv1a64:` fingerprints are always recorded (the archived
  service did so only when memory code retrieval was enabled). `qt-runtime` projects the
  scan and code index into the memory `CodeSnapshot` exactly as the archived
  `RetrievalSnapshot::build` (node ids, 48-line file windows, 16/32-line symbol windows,
  signature discriminators, `contains_file`/`defines`/`calls` edges, ambiguity suffixes),
  rebuilt per index generation. Code-recall lexical matches use exact symbol names (the
  archived FTS-backed `retrieval_search` provider is not ported).
- **Fixes:** `.gitignore` rules are re-read on every scan (the archived matcher cached them
  for the life of the process); an FTS5 syntax error now surfaces as
  `invalid FTS5 query: ...` instead of silently returning no matches; scans triggered by
  tools and by the worker are serialised.
- **Tool results:** a success result's text is the compact JSON output; an error result's
  text is the error message, followed by the structured payload as JSON on the last line
  when there is one. `details.status` carries the archived status (`success`,
  `invalid_request`, `execution_error`).
- **MCP:** `tools/list` and `resources/list` follow `nextCursor` pagination; `initialize`
  advertises the `sampling` capability when a handler is installed and the server's policy
  allows it (the archived qt sent empty capabilities); streamable HTTP answers sampling
  requests that arrive on a response stream, re-initializes after a `404` for an expired
  session, sends the negotiated protocol version and ends the session with `DELETE`;
  server `ping` requests are answered; aborting a proxy tool sends
  `notifications/cancelled`. Only transport failures are retried (the archived qt also
  retried JSON-RPC errors and timeouts). `list_mcp_resources`/`read_mcp_resource` query the
  servers live (`resources/list`, `resources/read`) instead of a static metadata snapshot;
  read results render text contents, image blobs as images and other blobs as a summary
  line. MCP proxy results also carry image content blocks. MCP files contribute only their
  servers, not other settings keys.

## Governed memory (`qt-memory`)

Pi has no persistent-memory contract; the archived qt is the reference.

- **Store:** `<agent_home>/memory/memory.sqlite3` with `blocks/` and `exports/` beside it
  (directories 0700, database 0600), WAL, `synchronous=FULL`, foreign keys and a jittered
  busy-retry handler. Migrations v1–v7 are byte-identical to the archived SQL (a test
  extracts them from `archived/`), so databases written by the C++ binary open unchanged and
  partially migrated ones are upgraded. A standalone qtmem database (legacy `meta` table)
  is refused with "standalone qtmem database requires explicit offline import"; schema
  versions above 7 are refused.
- **Model and wording:** memory types, statuses, source types, scope kinds and dimensions,
  scope keys, recall policies, review kinds, actions, application statuses and batch job
  statuses use the archived spellings, and error messages match the archived text. Stored
  JSON uses the archived encoding (compact, sorted keys).
- **Same behaviour:** write policy and secret/path redaction, governance transitions and
  audit, review preview/apply with revision compare-and-set and idempotent action keys,
  atomic review mutations, scheduled resurfacing, constrained batches and background batch
  jobs, persisted view state, review statistics, native export/import with ID remapping,
  retention, historical scrub, read-only qtmem v3 import and skill import. `memory.*`
  settings use the archived defaults, validation and the project-may-not-broaden rule.
- **Differences:**
  - Text fields are truncated to their byte limits on a UTF-8 character boundary (the C++
    store could split a multi-byte character).
  - A graph-edge upsert reports the ID of the stored edge; the archived store returned a
    freshly generated ID that was never written when the edge already existed.
  - Import with replace clears the scope's assertion revision boundaries, so restoring a
    backup into the same scope (preserved IDs) no longer fails on the insert trigger.
  - Export reads review actions with the export's own limit; the archived export applied
    the interactive 10 000-row history cap and failed for backups (limit 100 000).
  - Lenient JSON readers: a field of the wrong type in an imported document falls back to
    its default instead of aborting the import.
- **Wiring:** recall, capture, the memory tools, Dreaming and the `qt-cli memory` command are
  wired by `qt-runtime` (see the composition-root section). The GUI `/memory` panel and the
  native dashboard are not built yet.

## Core integration (`qt-core` engine, settings, slash commands, export, events)

- **Engine composition:** `Engine::builder(home, cwd)` registers static tools, dynamic
  `ToolSource`s (queried on every run, e.g. MCP proxies after a reconnect), provider
  backends (`ProviderBackend`), a shared `Hooks` registry, resource options
  (`--system-prompt`, `--append-system-prompt`, `--skill`, `--prompt-template`, `--theme`,
  `--no-skills`, `--no-prompt-templates`, `--no-themes`), `--no-context-files`,
  `ToolSelection` (`--tools`, `--no-tools`, `--no-builtin-tools`), `--offline`,
  `--session-dir`, `--no-session`, the mutation policy, extra bash environment, web options
  and startup migrations. `Engine::new` is the builder with defaults. The default tool set
  is every built-in tool of the archived binary (`ToolRegistry::builtin`), not only Pi's
  seven coding tools.
- **Provider backends:** configs returned by a backend carry a `BackendHandle`;
  `provider::stream` and `provider::list_models` dispatch to it, so llama.cpp and
  extension providers work in conversation runs, compaction and branch summaries, model
  listing and the model picker. A backend id equal to a built-in provider replaces it.
- **Tool runtime:** each run builds a `ToolEnvironment` from settings: the workspace
  policy (`workspaceRoot`, `allowExternalFilesystemAccess`), the mutation policy and
  queue, `shellPath` / `shellCommandPrefix` plus builder environment for bash,
  `images.autoResize` with the `images` resize hook and the model's image support, web
  options, and the provider call id. The loop now normalises archived argument aliases and
  applies the output guard to every call (previously only `ToolRegistry::execute` did).
- **System prompt:** built from `resources::load_resources` (`SYSTEM.md`,
  `APPEND_SYSTEM.md`, CLI overrides, skills block), the context files, and the
  environment block with the real workspace root, `shellPath`, offline state, mutation
  policy and path confinement.
- **Settings layers:** global `settings.json` (or `settings.yaml` / `settings.yml` when no
  JSON file exists, archived lookup order) with `<workspace>/.pi/settings.{json,yaml,yml}`
  deep-merged on top. Project files may only narrow `allowExternalFilesystemAccess` and the
  `memory.*` keys (archived `merge_project_settings_patch` rules and wording); a violating
  project file is ignored and reported (`Engine::settings_diagnostics`). Edits always go to
  the global file. The YAML reader accepts the archived subset (maps, `- ` sequences, `#`
  comments, quoted scalars) plus flow sequences and block scalars, and types scalars
  generically except for string-valued keys; the archived reader dropped keys it did not
  know, qt keeps them. `llama` in YAML is rejected with the archived message.
- **Migrations:** `oauth.json` / `apiKeys` → `auth.json` (written owner-only), agent-home
  session files → `sessions/--<cwd>--/`, `tools/` → `bin/`, `commands/` → `prompts/`
  (archived), plus `defaultProvider: "ollama"` → `ollama:<first instance>` (`ollama:local`
  without instances); a bare `ollama` model in a session file is mapped the same way.
- **Slash commands:** registry order extension → built-in → prompt template →
  `/skill:<name>` with the archived conflict diagnostics and `:<n>` suffixes for duplicate
  extension commands. `/hotkeys` and `/plan` (terminal-only) are not offered; `/follow-up`
  (registered by the archived shell without a description) is listed as "Queue a follow-up
  message for the running turn". The `/index` hint lists only the ported groups. `/help`,
  `/session`, `/status`, `/context [sections]`, `/name`, `/copy`, `/thinking`, `/model`,
  `/export`, `/share`, `/follow-up` and `/changelog` use the archived text. `/context`
  reports instruction files as `reason=session_start` load events and has no prompt-assembly
  hooks; its memory section comes from `Engine::set_memory_context`. `/import` switches
  directly (the archived TUI asked for confirmation first; front ends may confirm before
  dispatching). `/clone` of an empty session is an error. `/compact`, `/context`, `/reload`
  and mutating `/llama` subcommands are refused while a turn runs (archived list).
  `/changelog` shows the latest section of the changelog passed to the builder.
- **User bash:** `Conversation::run_bash` runs `!cmd` in the session cwd with the settings
  shell and prefix (no workspace confinement, no timeout) and records a `bashExecution`
  message; while the agent runs the message is held until the run ends (Pi). `!!cmd` sets
  `excludeFromContext`. Output keeps the last 2000 lines / 50 KB; the full output goes to a
  temp file. A second command while one runs is refused with the archived message.
- **`@file` arguments:** Pi `processFileArguments` output (`<file name="…">` blocks,
  resized images with Pi's dimension note). An image that cannot be resized is replaced by
  Pi's omitted note and a warning (the archived binary could not resize at startup and
  worded its warnings around that). `@path` mentions in composer text that name existing
  files can be expanded with `input::expand_mentions` (Pi only autocompletes them).
- **Prompt history:** `history.jsonl` exactly as archived (trimmed, deduplicated, 8 MiB,
  `terminal.promptHistoryLimit` default 500, `flock` on `history.jsonl.lock`, fsynced atomic
  replace, 0600); writes are synchronous rather than through the archived background
  writer thread.
- **Export:** HTML layout, CSS and escaping follow the archived exporter; markdown is
  rendered by the `markdown` crate (GFM) instead of md4c, so unsafe link protocols become
  empty `href`s rather than removed anchors. Exports render the in-memory entries (the file
  may not be written yet). JSONL export writes the header and all entries. `/share` uses
  `gh` and `PI_SHARE_VIEWER_URL` with the archived messages; the temporary
  `session.html` lives in a private temp subdirectory.
- **Lifecycle hooks:** besides the agent-loop hooks, `Hooks` carries the front-end events
  extensions need: `input` (`Engine::process_input`), `session_before_switch`
  (`Engine::allow_session_switch`), `session_before_fork` (run by `Conversation::fork`;
  cancel or skip restoring the selected text), `session_before_tree` (run by
  `navigate_tree`; cancel or override instructions, label and the summary),
  `model_select` (`Conversation::select_model` with source `set`/`cycle`/`restore`),
  `thinking_level_select`, `user_bash`, `resources_discover` (extra skill, prompt and theme
  paths at startup and reload), `instructions_loaded`, `session_shutdown` with Pi's reason
  and target (`Conversation::shutdown_with`), `message_end` replacement and `tool_call`
  argument rewriting, plus a conversation-created listener for composition roots.
  Model-context augmentors run on a blocking thread, and fallible augmentors can abort a
  request. `Conversation::usage` is recomputed after tree navigation. `--api-key` uses
  `Engine::set_runtime_api_key` (not saved).
- **JSON events:** `events_json::PiEventSerializer` maps `AgentEvent`s to Pi
  `AgentSessionEvent` lines. `AgentEvent` gained `TurnStart` / `TurnEnd`. Block `*_end`
  events are synthesised when the next block starts or the message ends; Pi's `done` /
  `error` stream events are not emitted (Pi does not forward them either). Usage cost is
  reported only as `cost.total`.
- **Logging:** `tracing` with the archived options (`--log-file` repeatable,
  `--log-stderr`, `--log-level`, `QT_LOG_FILE` `:`-separated, `QT_LOG_STDERR`,
  `QT_LOG_LEVEL`, default `info`, off without sinks, files truncated). `critical` maps to
  `error`. Lines read `[time] LEVEL target: message fields` instead of spdlog's pattern.
- **Models:** `/model refresh` and `Engine::start_model_refresh` re-list OpenAI and
  OpenRouter; `enabledModels` scoping follows Pi `resolveModelScope` (globs with
  `:level`, exact references, alias preference, warnings). `model_scope::render_model_table`
  reproduces the archived `--list-models` columns and number formatting.

## Extensions (`qt-extensions`)

Ported from the archived `extensions/` subsystem, the runtime bindings in
`app/runtime/extension_*.cpp`, the package manager (`tools/package_manager_*.cpp`,
`app/cli/package_command.cpp`) and Pi's extension API shapes. The Rhai API is
documented in [extensions.md](extensions.md).

- **Same as archived:** discovery order (`-e`, `<workspace>/.pi/extensions`,
  `<agent_home>/extensions`, project then user settings packages with their
  `extensions` filters; `--no-extensions` keeps `-e`), canonical-path dedup,
  directory scan rules (`extension.json`, `package.json` `pi.extensions` with
  `!`/`+`/`-` patterns, index files, direct files, recursion, hidden and
  `node_modules` skipped), extension naming, manifest v1 fields, defaults and
  validation messages (`invalid_argument [field]: message`), loader diagnostics,
  the static scan of `.ts`/`.js` modules and the not-implemented API diagnostics,
  registration rules and messages (lifecycle event list, handler identifiers,
  resource types and path safety, flag types and defaults, shortcut and renderer
  checks), `command_<name>`/`tool_<name>` handler naming, the sandbox limits (256 KB
  source, 100 000 steps per invocation, no imports, nested depth 8, concurrent entry
  rejected as `... is reentrant`), host method names, argument shapes and error
  wording (`unknown active tool`, `invalid thinking level`,
  `exec timeout must be positive`, `ctx.getModel requires string provider and id fields`,
  `invalid extension message delivery mode`), the public model metadata allowlist,
  headless UI results (cancelled dialogs, `false` confirm, stderr notifications),
  UI argument validation messages, lifecycle result decoding and merge rules
  (input continue/transform/handled, before_agent_start message + system prompt
  chain, tool_call block, tool_result replacement, session_before_* cancel), the
  JSON-RPC 2.0 framing and validation messages, `qt.command.invoke` /
  `qt.tool.invoke` / `qt.event.<event>` with one process per invocation (request on
  stdin, stdin closed, 30 s limit, stderr as diagnostics), command result text
  formatting, package source parsing and identities (npm, git, local), install
  paths, `npmCommand`, update candidates and skip reasons, offline mode, settings
  `packages` entries and filters, `qt install/remove/update/list/config` argument
  parsing, messages and output.
- **Differences:**
  - Scripts are Rhai instead of AngelScript. Handlers take and return Rhai values
    (maps) instead of JSON strings; `qt_host_call`, `qt_json_get` and
    `qt_json_string` keep the JSON-text forms. Host methods are also available as
    `pi::name(args)`, `ctx::name(args)` and `ctx::ui::name(args)`. Persistent state
    lives in the `this` map (Rhai functions cannot read global variables).
    `registerFlag`'s default is a value, not JSON text. Handlers may be given as
    `Fn("name")`; tools may declare a `parameters` schema and an explicit handler.
  - `index.rhai` is preferred over `index.ts`/`index.js` (the archived loader preferred
    the TypeScript index, so a package shipping both never ran its native script).
  - An `extension.json` with `"transport": "rhai"` loads and activates its
    entrypoint (the archived loader kept AngelScript manifests as metadata only).
  - Session mutations other than `ctx.compact` are allowed during a run; qt-core
    defers them to the next save point instead of rejecting them.
  - Lifecycle events are delivered through qt-core hooks. `turn_start`/`turn_end` are
    derived from assistant-response boundaries; `message_end` and `tool_call` results
    can replace the message (same role; Pi `MessageEndEventResult`) and `tool_call`
    results can block or replace the input with `{input}` (Pi mutates `event.input`);
    later handlers see the replacement; `before_provider_request` patches `systemPrompt`, `messages`,
    `maxTokens` and `temperature` of qt's request rather than a provider payload.
  - Packages: install/remove/update run the npm/git commands (the archived CLI only
    printed them; `PackageRunOptions { execute: false }` restores that), and user-scope
    git sources get a path under `<agent_home>/git` (the archived CLI planned them
    without an agent home and skipped them). Removing a git checkout deletes it
    in-process instead of running `rm -rf`.
  - `ctx.ui.getEditorComponent` reports the presenter's component;
    `ctx.ui.onTerminalInput` (terminal-only) is unsupported except for clearing.
- **Wiring** (`qt-runtime`): extensions are discovered with `-e` / `--no-extensions`,
  their tools are a dynamic tool source, `install_hooks` covers every lifecycle event
  including the front-end ones qt-core now exposes (`input`, `session_before_switch`,
  `session_before_fork`, `session_before_tree`, `model_select`,
  `thinking_level_select`, `user_bash`, `resources_discover`, `instructions_loaded`,
  `session_shutdown` with its reason), commands go into the slash registry, registered flags
  resolve unknown CLI `--flags`, providers are served by a provider backend, the host is a
  `ConversationHost` over the active conversation and `ctx.ui.*` uses the pluggable
  `ExtensionUi` (headless by default). `qt-cli install|remove|uninstall|update|list|config`
  run the package commands.
- **Not yet wired:** a GPUI `ExtensionUi` (dialogs, status, widgets, header/footer,
  components, autocomplete, renderers, shortcuts), `/extensions` package toggles and the
  GUI's `/reload` (the runtime offers `Runtime::reload`).

## Governed memory: recall, context and capture (`qt-memory`, wave 2A)

The archived qt is the reference (`recall_service`, `retrieval_*`, `dense_retrieval`,
`context_service`, `prehandler_protocol`, `coordinator`, `ingestion_service`, `runtime`,
`memory_tool`, `memory_command`, RPC memory actions, `/memory`).

- **Same behaviour:**
  - Lexical recall: FTS5 candidates, 256-bucket signed hashed-token similarity (archived
    FNV constant), score weights 0.28/0.22/0.18/0.10/0.08/0.14, +0.08 validated, ×0.68
    candidate, ×0.78 hypothesis, freshness `exp(-days/180)` on whole hours, archived warning
    texts, source-drift and critical-conflict exclusion, tag filters, access counts.
  - Structured recall (entities, aliases, assertions, `as_of` historical snapshots), entity
    assertion paths and memory graph traversal with the archived budgets and directions;
    RRF fusion `1/(60+rank)` over lexical, graph and dense ranks (`owner-rrf-v2` ordering:
    RRF, scope, total, id).
  - Retrieval storage: durable index jobs with leases, renewal, retry backoff and attempt
    exhaustion, 2 KiB/128-byte UTF-8-safe chunking, dense generations with the archived
    `f32le-v1` config contract, activation only after complete backfill, owner-distinct
    cosine search, revision-pinned dense evidence, code-owner publication from a snapshot,
    cross-store links with `uri:` evidence and the rebuild report.
  - Dense query service with the bounded query-embedding LRU (entries and bytes) and its
    failure statistics; code recall with `code_bm25`, dense and `code_graph` ranks fused at
    k=60, interleaved memory/code seeds and validated contextual assertion seeds.
  - Context rendering: the `<qt_memory_context>` preamble, category headings and weights
    (pinned 20, procedures 20, memories 30, episodes 15, evidence 10, warnings 5, code 25),
    quota then fill, the 900-token default at 4 bytes per token, `[Memory id; status=…;
    confidence=…]` headers (C++ `%g` formatting), atomic graph, assertion and mixed
    memory/entity/code bundles, final revalidation and explanations.
  - Pre-handler protocol v1: request encoding and `candidate_pack_hash`, strict result
    validation (unknown/missing fields, types, unknown IDs, control characters, budget),
    parent-owned envelope, fixed system prompt, child arguments and environment,
    deterministic fallback, final revalidation and `recall_receipt` session events.
  - Capture: episode transcript, durable spool under `memory/capture_pending/` with restart
    recovery, `completed_episode` / `recall_outcome` / `recall_attribution` /
    `completed_episode_receipt` events, procedure feedback from attribution, keyword
    extraction (17 signals, 0.72 confidence), capture receipts, semantic consolidation
    claims and review proposals.
  - Memory tools, `qt memory` commands (help text, options, outputs, exit codes), RPC memory
    actions (including review error codes `rpc_error:<code>:…`) and `/memory` lines.
- **Differences:**
  - The provider-only memory message is a plain user message; qt-core messages carry no
    `name`/metadata, so the archived `qt_memory_context` name and metadata are not sent.
  - The logical input sequence is the number of user messages in the request and the
    projection epoch is a hash of the earlier user inputs (qt-core has no projection
    epoch); recall still runs once per user input.
  - `memory.prehandler.failOpen = false` makes recall fail-closed: a failed augmentation
    (store or recall error) aborts the request with `memory recall failed: <error>` as an
    assistant error, and a memory runtime that cannot open aborts startup with
    `unable to initialize built-in memory: <error>` (archived). A failed or invalid
    pre-handler child still falls back to the deterministic context, as archived.
  - Code recall reads a `CodeSnapshot` supplied by the project index and a lexical provider;
    without an index FTS provider only exact symbol names match.
  - `/memory scan-skills` and the views (menu, dashboard, settings, review inbox, folder
    picker) are returned as actions for the front end instead of TUI overlays.
  - Dreaming, post-processing and research are reached through `CaptureHooks` and
    `DreamingControl`; without them `/memory dream run` reports that no cycle was queued.

## Memory Dreaming, research, verifier and post-processing (`qt-memory`)

The archived qt is the reference (`runtime.cpp`, `post_processing_service.cpp`,
`web_tool.cpp` research mode).

- **Same behaviour:** Dreaming triggers `post_turn`, `interval`, `candidate_threshold` and
  `manual` with the idle-duration and foreground-activity gate, post-turn coalescing, an
  eight-cycle queue and manual cycles jumping it. Each cycle samples up to 64 governed
  records, prefers those least used by the last eight receipts, breaks ties and orders the
  prompt by hashes of a random UUID, and records `least-recent-cycle-hash-v1`, the seed,
  pool hash/count and exact input IDs. Claim and receipt events (`dreaming-v6` claim
  identity), the strict artifact schema and llama grammar, rejection codes, receipt fields,
  the pending `dream_artifact` review with `derived_from` edges, researched successors with
  their own `dream_research_successor` review, research citation validation (opened
  `web_run` refs, or 2xx `web_fetch` of a URL a prior `web_search` returned), the
  restricted research tools (shared three-call budget, 512 KiB / 20 s fetches, six results,
  256-byte queries, no `engine_url`, no direct `web_run` URLs, offline still authoritative),
  the `auto_validate` verifier (local `llama/` model only, four sources, 4096 bytes each,
  12288 total, literal quotes covering every source, atomic validation of only the
  successor by `memory_dream_verifier`), deterministic association/compression candidates,
  the semantic-consolidation child (up to eight records, review-only proposal,
  claim/receipt, CPU retry after an out-of-device-memory failure) and the
  `/memory dream [status|run|research <json>]` and RPC `dream` texts all follow the archive.
  Prompts are verbatim.
- **Differences:**
  - The child runner is a trait (`IsolatedChild`); `ProcessChild` launches a
    `qt --mode json` child, and the composition root may instead supply an in-process
    llama.cpp child using each task's grammar, temperature and output bounds.
  - Restricted `web_fetch` results carry the returned page text as `details.content`
    (the archived fetch payload included it); the search endpoint is the configured
    `WebOptions.search_engine_url`, which the model still cannot override.
  - Deterministic post-processing does not queue a second compression review for a pair
    that already has a pending or deferred one; the archived pass re-queued it on every
    turn.
  - Stopping the Dreaming worker discards all queued cycles (the archive still drained
    manual and threshold cycles with a cancelled child).

## Subagents (`qt-subagents`)

The archived qt is the reference (`archived/src/tools/subagent_tool.cpp`,
`archived/src/app/runtime/subagent_coordinator.cpp`, `archived/docs/persistent-subagents.md`);
Pi's subagent extension example supplies the child JSON contract.

- **Definitions:** `*.md` files (recursively, `*.chain.md` skipped) with the archived
  line-based frontmatter: `name`, `description` (both required), `package` (name becomes
  `<package>.<name>`), `tools` (comma or space list), `model`, `thinking`,
  `systemPromptMode` (`replace`/`append`), `inheritProjectContext`, `inheritSkills`,
  `maxSubagentDepth`/`maxDepth`, `disabled`; the body is the system prompt. An agent named
  `delegate` defaults to append mode with project context. Built-ins `scout`, `planner`,
  `reviewer`, `worker` keep the archived descriptions, tool lists, prompts and max depth 0.
  Discovery: built-ins, then `<agent_home>/agents` (scope `user`/`both`), then the nearest
  ancestor `.agents` and `.pi/agents` (scope `project`/`both`, `.pi/agents` wins); later
  scopes override by name and `disabled` hides an agent. Diagnostics use the archived wording.
- **Tool:** the `subagent` schema, description and validation messages are the archived
  ones: single `{agent, task}`, parallel `{tasks}` (default 8 tasks, concurrency 4, hard cap
  32, results in source order, the call succeeds even when children fail), chain `{chain}`
  with `{previous}`/`{task}` substitution stopping at the first failure, `action=list`,
  project trust via `confirmProjectAgents=false`, depth limits from `QT_SUBAGENT_DEPTH` /
  `QT_SUBAGENT_MAX_DEPTH` (default 2; an agent's own max depth narrows its children), child
  cwd resolved from the workspace root and checked against the workspace policy, and the
  mutation queue (kind `command`). Parallel output keeps the
  `N/M subagents completed` / `=== Task i: agent ===` layout.
- **Children:** `qt-cli --mode json --no-session --no-extensions [--no-context-files]
  [--no-skills] (--tools a,b | --no-tools) [--model m[:thinking]] [--system-prompt |
  --append-system-prompt] "Task: <task>"` with `QT_SUBAGENT_CHILD`, `QT_SUBAGENT_DEPTH`,
  `QT_SUBAGENT_MAX_DEPTH`, `QT_SUBAGENT_RUN_ID`, `QT_SUBAGENT_NAME`, `QT_CODING_AGENT_DIR`.
  The final output comes from the `agent_end` event's messages; non-JSON lines become
  diagnostics. Persistent workers drop `--no-session`, add `--session <worker
  session.jsonl>`, pass the turn on stdin, inherit the parent's model (`--provider`), filter
  tools by the parent's active tools and set `QT_SUBAGENT_PATH` and
  `QT_SUBAGENT_PARENT_SESSION`.
- **Persistent workers:** `spawn`/`send`/`follow_up`/`wait`/`interrupt`/`list_workers` with
  the archived arguments, results and errors; targets `/root/<name>` (or `QT_SUBAGENT_PATH`
  in a child); names unique; mailbox and generation counter; `fork` `none|all|N` snapshots
  with in-flight tool calls closed by "Parent tool call was still running when this
  context was forked."; storage `<agent_home>/subagents/<fnv1a64(parent session)>/`
  (`children.json` version 1 written atomically 0600, exclusive `children.lock`,
  `<name>/session.jsonl` with a Pi v3 header carrying `parentSession` and `subagent`
  lineage); at most four worker processes per host process; `subagent_start` /
  `subagent_complete` events (`child_id`, `generation`, `event_id`, `status`, `output`,
  `messages`, `exit_code`, `error`) pending until taken or acknowledged; workers that were
  running when the parent stopped come back `interrupted` with a `recovered` event.
  Permissions (workspace, external access, tools) are re-checked before messaging a worker.
- **Delivery:** completions reach the parent as `custom` messages with
  `customType: "subagent_event"`, the event JSON as text and as `details`, delivered as a
  follow-up (starting a run when idle), then acknowledged. JSON/RPC frames are
  `{"type":"subagent_event","subagent":<event>}`.
- **`/subagent`:** `list [scope]`, `help`, the role menu entries, `spawn`, `workers`,
  `send`, `follow_up`, `interrupt`, `wait` (1 s) and `call <JSON>` with the archived
  wording, as library functions for the front ends.
- **Differences:**
  - The child executable is `QT_SUBAGENT_EXECUTABLE`, else `qt-cli` beside the running
    binary, else `qt-cli` on `PATH` (the archived binary re-executed itself).
  - Final output takes the last assistant message with text (Pi `getFinalOutput`); the
    archived parser also accepted text from other roles.
  - Synchronous children also receive `QT_SUBAGENT_ALLOW_EXTERNAL`.
  - Definition files are read in sorted path order so overrides within a directory are
    deterministic; worker listings are sorted by id.
  - A child cwd must exist (it is canonicalised before the policy check).
  - Run modes return the output text to the model with the JSON result in `details`;
    `list` and the worker actions return the JSON as text. Failures carry
    `details.status` (`invalid_request`, `permission_denied`, `not_found`,
    `execution_error`, `pending_approval`).
  - Live progress is a short per-child status text (`1. scout [running]` plus the latest
    output lines) instead of JSON snapshots.
  - Cancellation kills the child's process group (the Linux descendant walk across
    process groups is not ported). A persistent `wait` ends when the parent aborts.

## Composition root and CLI (`qt-runtime`, `qt-cli`)

The archived `runtime_services.cpp`, `cli.cpp`, `cli_startup.cpp`, `print_json_mode.cpp` and
`main.cpp` are the reference.

- **Runtime:** `Runtime::build(RuntimeOptions)` loads settings, runs the startup migrations,
  loads extensions, builds the llama backend (with the archived memory-child overrides:
  8192-token context, up to 16 threads, task temperature/seed/output bound, KV cache off and
  the task grammar; `QT_MEMORY_FORCE_CPU`), the project index tools (the background scanner
  only starts for interactive use), the memory runtime (`memory.*` mapped as archived,
  `resolve_memory_task_models`, the pre-handler and consolidation children, Dreaming with
  post-processing at 500 records / 100 pairs and semantic consolidation, dense embeddings
  through llama.cpp, `/context memory` lines), MCP servers with sampling through the active
  model, the `subagent` tool (one persistent-worker coordinator per parent session) and the
  extension wiring, then `Engine::builder`. In a Dreaming research child the built-in tools
  are exactly the restricted research tools. `Runtime::attach` binds a conversation (an
  interactive runtime binds every new conversation itself); `start_background_services`
  starts Dreaming, the five-minute model refresh and MCP connections. `/changelog` reads
  `reference/pi/packages/coding-agent/CHANGELOG.md` under the working directory, as archived.
- **CLI:** the archived flag set and parsing rules: value options take the next argument
  even when it starts with `-`, repeatable options accumulate, `--tools`/`--models` take
  the last value, `@file` arguments, `-p` followed by a `---` prompt, invalid `--thinking`
  and `--log-level` values are warnings, unknown short options are errors and unknown
  `--flags` are resolved against extension flags (`Unknown option: --x`). Startup order,
  `--version`, `--help` (stdout when interactive, else stderr), `--export` (`Exported to:`),
  `--list-models [search]` (archived table and messages), `--fork` conflicts,
  `--api-key requires a model ...`, `No session found matching '<id>'`, the missing session
  cwd prompt (`QT_FORCE_MISSING_SESSION_CWD_PROMPT`) and error text, the initial message
  (stdin, then `@file` blocks, then the first message; further messages run one by one),
  print mode printing only the last answer, `error: <message>` and exit code 1 on failure,
  JSON mode's session header line, Pi event lines and the wait for persistent workers with
  `{"type":"subagent_event"}` frames and their follow-up runs, `qt-cli memory ...`, the
  package subcommands and the Dreaming-verifier guard follow the archive.
- **Differences:**
  - The binary is `qt-cli`; the help text names it and points at the desktop app. Without a
    prompt on a terminal, `qt-cli` exits with a hint instead of opening the TUI; with
    messages it runs them as in print mode. `--resume` shows a numbered picker on the
    terminal. `--version` prints the crate version (the archived binary printed `0.0.0`).
  - `--api-key` is held in memory for the run and never written to `auth.json`.
  - A value after a boolean extension flag (`--plan task`) is kept as a message (the
    archived parser assigned it to the flag and failed).
  - `--list-models` also lists models discovered live from ready providers (Ollama, OpenAI,
    OpenRouter) unless `--offline`.
  - `QT_SUBAGENT_ALLOW_EXTERNAL=0` in subagent children is not applied as a settings
    override (the child's workspace policy comes from its settings).

## RPC mode (`qt-cli --mode rpc`)

The archived `rpc_protocol.cpp`, `rpc_server.cpp` and `rpc_mode.cpp` and Pi's
`modes/rpc/{rpc-types,rpc-mode,jsonl}.ts` are the reference.

- **Framing:** one JSON object per line; records are split on `\n` only (U+2028/U+2029
  inside strings are kept), a trailing `\r` is removed and a last line without LF is
  read. Output is one JSON object per LF-terminated line on stdout; nothing else is
  written there.
- **Responses:** `{id?, type: "response", command, success: true, data?}` with `data`
  omitted for `prompt`, `steer`, `follow_up`, `abort`, `set_thinking_level`, the queue
  modes, `set_auto_compaction`, `set_auto_retry`, `abort_retry`, `abort_bash` and
  `set_session_name`. Failures are `{id?, type, command, success: false, error,
  error_code}`; codes follow the archive (`internal_error` by default, the memory
  handler's `invalid_argument` / `io_error`, `rpc_error:<code>:` prefixes such as
  `review_not_found`, `invalid_argument` when the `subagent` tool is disabled).
  Unparseable lines answer `command: "parse"` (`Failed to parse command: …`); frames
  that are not objects, lack `type`, carry an unknown type, are responses or events, or
  repeat a request id answer `command: "invalid"` with the archived messages
  (`unsupported rpc frame type`, `rpc server accepts command frames only, received
  <type>`, `duplicate rpc request id: <id>`). Command validation uses the archived
  messages (`prompt command message must not be empty`, `queue mode must equal 'all' or
  'one-at-a-time'`, `memory command action is unsupported`, ...).
- **Commands:** all Pi commands plus the archived `memory {action, params, confirm}`
  (qt-memory `handle_memory_rpc`) and `subagent {action, arguments}` (persistent-worker
  actions through the `subagent` tool). `prompt` runs extension commands (their output
  lines become `notify` requests, failures `extension_error` with `extensionPath:
  "command:<name>"`), then the `input` hooks (source `rpc`) and template / skill
  expansion; it is acknowledged before the run's events, and while a run is active it
  queues per `streamingBehavior` or fails with Pi's "Agent is already processing…".
  `steer` / `follow_up` refuse extension commands and emit `queue_update` (the run emits
  it while active). `abort` clears the queues, waits for the run to end and then
  answers; `abort`, `abort_bash`, `abort_retry` and `extension_ui_response` are handled
  as soon as they arrive, every other command in order. `get_state` reports the archived
  fields; `get_messages`, `get_session_stats` (Pi stats plus the per-model breakdown),
  `get_fork_messages`, `fork` (`{text, cancelled}`), `clone`, `new_session
  {parentSession}` (recorded in the new header), `switch_session`, `export_html
  {outputPath}`, `compact` (streams `compaction_start` / `_end`; a running agent is
  aborted first), `bash` / `abort_bash` (`{output, exitCode, cancelled, truncated,
  fullOutputPath?}`, recorded as `bashExecution`), the model, thinking and queue-mode
  commands and `get_commands` (extension, prompt and skill commands with `sourceInfo`
  and the registry diagnostics) follow Pi. `set_model`, `cycle_model` (scope from
  `--models` / `enabledModels`, else all available models) and
  `set_auto_compaction` / `set_auto_retry` / queue modes persist to settings as Pi does.
- **Events:** the Pi event lines of every run (prompts, extension-triggered runs and
  worker follow-ups) including `queue_update`, `compaction_start` / `_end`,
  `auto_retry_start` / `_end`, `extension_error` (hook failures with the extension
  path), `session_info_changed` and `{"type": "subagent_event", "subagent": …}` frames;
  worker completions are also delivered to the parent as follow-ups and acknowledged.
- **Extension UI:** `ctx.ui` dialogs emit `{type: "extension_ui_request", id (UUID v4),
  method, …}` (`select {title, options, timeout?}`, `confirm {title, message,
  timeout?}`, `input {title, placeholder?, timeout?}`, `editor {title, prefill?}`) and
  block the extension until an `extension_ui_response` with `value`, `confirmed` or
  `cancelled: true` arrives; a timeout or a cancelled answer gives the default
  (`undefined` / `false`). `notify {message, notifyType}`, `setStatus {statusKey,
  statusText?}`, `setWidget {widgetKey, widgetLines?}`, `setTitle` and
  `set_editor_text` (also for paste) are fire-and-forget; `setTheme` fails with "Theme
  switching not supported in RPC mode"; working indicators, header/footer and custom
  components are no-ops. Answers for unknown ids are ignored; a second answer for the
  same id fails with `duplicate extension ui response id: <id>`.
- **Lifecycle:** `@file` arguments are rejected; positional messages are ignored. At end
  of input open dialogs resolve to their defaults, queued commands finish, the process
  waits for the active run and exits 0 (1 when stdin cannot be read).
- **Differences:**
  - Error responses carry the code twice, as the archived `error_code` and as
    `errorCode`; Pi sends no code.
  - The archived RPC ran each prompt synchronously and queued `streamingBehavior`
    prompts even when idle; qt streams runs asynchronously as Pi does.
  - `compact`, `fork` and `clone` also work for `--no-session` sessions (the archive
    required a session file); `export_html` keeps the archived "Cannot export HTML: no
    active session file".
  - Models are reported in Pi's `Model` shape plus the archived `displayName` / `vision`;
    `get_available_models` lists the models of providers with credentials (catalog,
    discovered and a live discovery unless `--offline`), not the whole catalog.
  - `abort_retry` cancels the run while an auto-retry is pending (Pi aborts only the
    retry wait; the run then ends with the error either way). The `setWidget`
    placement and the dialog `signal` option have no equivalent in the Rhai host API.
  - SIGTERM/SIGHUP use the default handlers (no exit codes 143/129).

## Desktop GUI (`qt-app`)

The archived interactive (TUI) features are GPUI views; terminal rendering itself is out of scope.

- **Themes:** Pi JSON themes (built-in `dark`/`light` and files from `<agent home>/themes`,
  `.pi/themes`, `.agents/themes`, `themeRoots`/`themes`) are selected by the `theme` settings
  key (project `.pi/settings.json` overrides global). Without it the legacy `gui.theme`
  (`light`/`dark`) is honoured, otherwise the built-in theme matching the system appearance is
  used and follows appearance changes. Pi themes have no background token, so the HTML-export
  `pageBg`/`cardBg` give the window and panel backgrounds and decide light/dark mode (then
  `userMessageBg`, then `text`). Tokens map onto gpui-component colours (`accent` → primary,
  ring, caret; `borderMuted` → borders; `selectedBg` → list/selection; `userMessageBg` →
  secondary; `success`/`error`/`warning`; `mdLink` → links); the rest (tool backgrounds,
  diff colours, thinking text, custom-message colours) colour the transcript. The selected
  custom theme file is watched (`ThemeWatcher`, 100 ms debounce) and re-applied live. The
  theme picker is in Settings; the sidebar sun/moon button switches between `dark` and
  `light` and stores `theme`.
- **Login / logout:** one dialog replaces the archived `/login` and `/logout` selectors and
  the former API-keys dialog. Subscriptions (Anthropic Claude Pro/Max, ChatGPT Codex) run
  the core OAuth flow: the browser opens with the authorization URL, the localhost callback
  is awaited, and a pasted code or redirect URL can be submitted instead. Statuses and
  messages use the archived wording (`subscription configured`, `API key configured`,
  `env: VAR`, `key in models.json`, `unconfigured`, "Saved API key for X. Credentials saved
  to …", "Logged out of X", "Removed stored API key for X. Environment variables and
  models.json config are unchanged.", "API key cannot be empty"). Every catalog provider
  that takes a key is listed (not only Anthropic/OpenAI/OpenRouter).
- **Ollama:** one panel covers `/ollama` instances/add/configure/remove/status/models/
  running/info/pull/load/unload/delete with the archived result lines. Remove and delete ask
  for confirmation instead of `--yes`. The implicit `local` instance is written to settings
  the first time its models are refreshed. Pull is non-streaming (as in the archived qt); the
  panel shows a busy indicator until it finishes.
- **Sessions:** the tree view (`/tree`) offers Pi's filter modes (default, no tools, user
  only, labeled, all) and search, marks the active path and current position, edits labels,
  and navigates with Pi's "Summarize branch?" choice (No summary / Summarize / Summarize with
  custom prompt; `branchSummary.skipPrompt` skips it) plus an optional label; Esc cancels a
  running summary. Fork (`/fork`) is available from a picker, the tree and a hover button on
  user messages; the selected text seeds the composer. Clone copies the whole session
  (`clone-<id>`). The resume picker searches name, messages, id and folder across the
  current folder or all sessions and renames sessions. Import asks "Replace current session
  with <path>?" and resuming or importing a session whose stored cwd is gone shows Pi's
  "Session cwd not found" prompt (Continue/Cancel). The transcript reloads after compaction
  and tree navigation and renders compaction (`[compaction] Compacted from N tokens`), branch
  summary (`[branch]`) and displayed custom messages (`[customType]`).
- **Run controls:** Enter while running steers, Alt+Enter queues a follow-up, Alt+Up (or the
  queue's button) restores queued messages to the composer (queued first, then the draft,
  as Pi). Queued messages are listed above the composer. Compaction and retry show progress
  (retry with a countdown) and can be cancelled. Manual compaction takes optional
  instructions. The goal dialog sets, updates, pauses, resumes, clears and shows the goal;
  the status bar shows its state, time and token budget.
- **Settings:** the archived `/settings` toggles that apply to a desktop app (quietStartup,
  collapseChangelog, hideThinkingBlock, enableSkillCommands, allowExternalFilesystemAccess,
  images.autoResize) plus auto-compaction, auto-retry, steeringMode/followUpMode, the theme
  and scoped models. The archived terminal-only toggles (terminal progress, tool-entry
  compaction) are omitted and the prompt-history limit belongs to the prompt history work.
  `enabledModels` accepts exact `provider/id` specs, bare ids and `*` globs (Pi) and limits
  the model picker; toggling from "all" first expands to every discovered model (archived
  `/scoped-models`). The thinking picker offers the selected model's levels and clamps the
  current level (upwards first, Pi).
- **Composer:** clipboard images and dropped image files become attachments (chips with
  size and dimensions) prepared with `images.autoResize`; Pi's resize note is appended to the
  prompt text. Dropped non-image files insert their paths. Ctrl+G edits the draft in
  `$VISUAL`/`$EDITOR` (Pi's "No editor configured" message; one trailing newline removed);
  GUI editors need a blocking flag such as `code --wait`.
- **Slash commands:** typing `/` at the start of the composer opens the archived autocomplete
  popup over the core registry (extension, built-in, prompt-template and `/skill:` commands):
  case-insensitive substring matches (prefix matches first), else a fuzzy match; rows show
  `hint - description`. After the name, the core argument providers complete arguments
  (`/model`, `/thinking`, `/ollama`, `/llama`, `/memory`, `/mcp`, `/subagent`, `/context`,
  `/goal`). Up/Down select, Tab inserts, Enter inserts and runs a command name (Pi) or inserts
  an argument, Esc closes. Ctrl/Cmd+K opens a command palette with the same commands; commands
  whose hint starts with `<` (required arguments) prefill the composer instead of running.
  Lines are executed by `slash::dispatch`; text output appears as a card in the transcript
  (`/help`, `/session`, `/status`, `/changelog`, `/name x`, `/model x`, `/reload`, …). Actions
  open the existing views: settings (and a theme picker for `/settings theme` and `/themes`),
  a searchable model picker, a thinking picker, tree, resume, fork picker, login, Ollama,
  import (with the missing-cwd prompt) and quit (`/exit` too, archived alias). `/name` without
  arguments opens the rename dialog. `/llama`, `/memory`, `/index`, `/mcp`, `/subagent`,
  `/extensions`, `/skills`, `/prompts` and extension commands show "… is not available in this
  build." until their panels and the extension runtime are wired. An unknown `/name` is an
  error card (archived); a first word containing `/` (a path) is sent as a prompt.
- **`/tools`:** a panel with a checkbox per tool (filter, enable/disable all) persisted through
  `Conversation::set_tool_enabled` (`tools-config` entry), plus the mutation approval queue:
  each pending mutation (id, kind, summary, argument preview) can be approved (run with
  `Engine::execute_approved_mutation`, result shown as a card) or rejected. With the `queue`
  policy the status bar shows "N pending approvals" (opens the panel); with `execute` the
  panel says so.
- **`/context`:** an inspector dialog with section tabs (`all`, environment, prompt, messages,
  resources, diagnostics, memory, usage) showing `slash::context_lines`, plus refresh and
  copy. It is refused while a turn runs (archived).
- **`/export`:** without a path the platform save dialog (`prompt_for_new_path`, default
  `qt-session-<id>.html` in the project) chooses the file; a name without `.html`/`.jsonl`
  gets `.html`. When no file-chooser portal is available an in-app path dialog is shown
  instead. With a path the core command runs directly. `/share`, `/copy`, `/session`,
  `/status` and `/name` use the core output (cards; `/copy` writes the clipboard).
- **Prompt history:** the global `history.jsonl` is merged with the session's own prompts
  (session first, archived `restore_prompt_history`) within `terminal.promptHistoryLimit`
  and 8 MiB. Up in an empty composer, or on the first line while browsing, shows older
  entries; Down on the last line goes back to newer entries and finally the unsent draft;
  editing ends browsing. Every submitted line (prompts, slash commands, `!` commands) is
  recorded in the session history and appended to `history.jsonl`.
- **`@` mentions:** `@` at a token start opens path completion over the workspace with the
  archived scoring (exact name, name prefix, substring, path substring, fuzzy; directories
  first on ties), using a `.gitignore`-aware walk with hidden files and without `.git` (the
  archived binary used `fd` with the same rules); at most 50 rows. Paths with spaces are
  quoted (`@"my dir/file"`), files get a trailing space and directories keep completing.
  On send, mentions naming existing files are expanded with `input::expand_mentions` (file
  blocks and images appended, warnings shown as notifications).
- **User bash:** `!cmd` / `!!cmd` run through `Conversation::run_bash` with a live output card
  (output tail, spinner); Esc cancels the command before it stops the agent. A second command
  while one runs is refused with the archived message. Results recorded while the agent runs
  are shown immediately and not duplicated when the run ends.
