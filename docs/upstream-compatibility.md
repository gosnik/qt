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
  logins, in-process llama.cpp, memory, extensions, MCP, subagents, the project index and
  RPC mode.

## In-process llama.cpp (`qt-llama`)

The `qt-llama` crate ports the archived qt llama integration as a library; it is not yet
wired into the engine, model picker or a `/llama` panel.

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
  service did so only when memory code retrieval was enabled). The memory retrieval
  snapshot itself belongs to the governed-memory milestone.
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
- **Not yet ported:** recall (lexical, graph, dense, code), context rendering, the
  pre-handler, capture/ingestion, post-processing, Dreaming, research, the verifier, memory
  tools, the `qt memory` CLI and the GUI.

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
    cannot replace the message or the tool input (qt-core hooks do not support it),
    only block; `before_provider_request` patches `systemPrompt`, `messages`,
    `maxTokens` and `temperature` of qt's request rather than a provider payload.
  - Packages: install/remove/update run the npm/git commands (the archived CLI only
    printed them; `PackageRunOptions { execute: false }` restores that), and user-scope
    git sources get a path under `<agent_home>/git` (the archived CLI planned them
    without an agent home and skipped them). Removing a git checkout deletes it
    in-process instead of running `rm -rf`.
  - `ctx.ui.getEditorComponent` reports the presenter's component;
    `ctx.ui.onTerminalInput` (terminal-only) is unsupported except for clearing.
- **Not yet wired** (owned by the composition root / front ends): registering
  `Extensions::tools()` with the engine, `install_hooks`, the `-e` /
  `--no-extensions` / extension-flag CLI parsing, `run_package_command` as
  `qt install|remove|update|list|config`, the slash-command registry, a GPUI
  `ExtensionUi` (dialogs, status, widgets, header/footer, components,
  autocomplete, renderers, shortcuts), `/extensions` package toggles and `/reload`.

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
  - `memory.prehandler.failOpen = false` cannot abort a request: qt-core model-context
    augmentors cannot fail, so a failed augmentation always proceeds without memory.
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
