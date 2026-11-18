# qt (Rust + GPUI) Plan

The C++17 TUI implementation was archived to `archived/` on 2026-11-18 and qt is being
rebuilt as a Rust workspace with a GPUI desktop UI. Items are checked only when the code
and edge cases are complete and covered by tests or a recorded manual check.

The parity checklist below comes from a full inventory of the archived code (2026-11-18).
Items marked *(unwired)* exist in the C++ code but were never enabled in the shipped
binary. Items marked *(gap)* were promised by help text or docs but not implemented.
TUI-only behaviour (terminal rendering, Kitty images, SIGWINCH, OSC progress, `/hotkeys`,
`onTerminalInput`) is out of scope. Decisions (2026-11-18): parity targets the shipped binary plus Pi behaviours it left unwired (auto-retry, auto-compaction, overflow recovery, OAuth refresh). Dead code (`web_core` contracts, the index
`run_service` MCP server, hidden `--inspect-*`/file-op developer flags) is not ported.

## Milestone 1: Foundation (done)

- [x] Workspace layout: `qt-core` (no GPUI), `qt-app` (GPUI via `gpui-kit` 0.10), `qt-cli`.
- [x] Agent home compatible with archived qt (`~/.qt`, `QT_CODING_AGENT_DIR`, settings/auth preserved, auth 0600).
- [x] Providers: Anthropic Messages; OpenAI-compatible chat completions (OpenAI, OpenRouter, Ollama).
- [x] Model discovery per provider.
- [x] Tools read, bash, edit, write, grep, find, ls with Pi schemas and limits.
- [x] Agent loop with cancellation; history sanitising.
- [x] System prompt with AGENTS.md/CLAUDE.md discovery.
- [x] Pi v3 sessions (linear replay, resume, listing).
- [x] Desktop UI: sidebar, model/thinking pickers, streamed markdown, tool cards, diffs, keys dialog, themes.
- [x] `qt-cli` print mode.

## Integration (wave 1 libraries → runtime)

Wave 1 (2026-11-18) delivered the libraries below as tested code; these wiring steps are what
remain before the checked items are visible in the app and CLI.

- [x] Engine reads workspace policy, bash and web options, mutation policy and image resize from settings and passes them to tools.
- [x] Engine builds the system prompt from `resources::load_resources` (skills, SYSTEM.md, APPEND_SYSTEM.md, shellPath).
- [x] Composition root (`qt-runtime`) registers index tools + service, MCP manager + tools, llama provider, memory and extensions for app and CLI.
- [x] Pluggable provider backends in qt-core so `llama` (qt-llama) is dispatched by `provider::stream`.
- [x] Settings migration from milestone-1 `ollama` provider id to `ollama:local`.

Wave 2 (2026-11-18) added these as tested libraries, not yet wired into app/CLI: subagents
(definitions, tool, persistent workers), memory recall/rendering/capture/prehandler/tools/CLI+RPC
handlers, Dreaming/research/verifier/post-processing, extension packages and `ExtensionUi`,
core slash registry, prompt history, user bash, `@file`, JSON event serialiser.

## Milestone 2: Harness and sessions

- [x] Session entry types: model_change, thinking_level_change, session_info (name), label, branch_summary, compaction, custom_message, custom, bashExecution/custom message roles; v1/v2 → v3 migration; strict validation; stats (tokens, cost, per-model).
- [x] Tree: branch, branch_with_summary (model-backed), labels, get_tree, build context from any leaf; GUI tree view (`/tree`). (Verified end to end 2026-11-18.)
- [x] Fork (from a user message), clone, new, switch, import, missing-cwd prompt; `parentSession` header. (Verified end to end 2026-11-18.)
- [x] Session catalog: id-prefix resolution, `--session`, `--fork`, `--continue`, `--resume` (GUI picker), `--no-session`, `--session-dir` / `PI_CODING_AGENT_SESSION_DIR` / `sessionDir`. (Verified end to end 2026-11-18.)
- [x] Compaction: Pi summary prompts, cut point, split-turn prefix summary, iterative update, read/modified file tracking, `compaction` entry, replay injection; manual `/compact [instructions]`.
- [x] Auto-compaction on threshold *(unwired)* and overflow recovery (compact + retry once, Pi).
- [x] Context-overflow detection (phrase list + usage checks).
- [x] Auto-retry with exponential backoff for transient errors *(unwired)*; provider retry settings, Retry-After.
- [x] Steering / follow-up / next-turn queues with `steeringMode` / `followUpMode`; queue events; Enter-while-running steers. (Verified end to end 2026-11-18.)
- [x] Parallel tool execution for non-destructive batches *(unwired)*.
- [x] Hooks pipeline: before_agent_start, context, model-context augmentor, before_provider_request/payload, after_provider_response, tool_call (block), tool_result (rewrite), before_compact, session lifecycle events.
- [x] Save points / deferred runtime changes during a turn; active tool set persisted (`tools-config` entry). (Verified end to end 2026-11-18.)
- [x] Goals (`/goal`): objective, token/time accounting, continuation prompt. (Verified end to end 2026-11-18.)

## Milestone 3: Prompt resources

- [x] Skills: discovery roots, SKILL.md + root `*.md`, frontmatter, ignore files, collisions; `<available_skills>` prompt block; `/skill:<name>` (`enableSkillCommands`).
- [x] Prompt templates: roots, frontmatter, argument expansion (`$1`, `$@`, `$ARGUMENTS`, `${@:N:L}`, `{{args}}`), exposed as slash commands.
- [x] SYSTEM.md / APPEND_SYSTEM.md (project and agent home), `--system-prompt`, `--append-system-prompt`.
- [x] Runtime metadata section (shell, timezone, network, permissions), workspace-policy text.
- [x] Themes: JSON theme files (51 tokens), discovery roots, live reload; mapped onto the GPUI theme.

## Milestone 4: Providers and models

- [x] OpenAI Responses API (reasoning replay, encrypted content, prompt cache key/retention, session headers).
- [x] openai-codex subscription (JWT account id, Codex endpoint and headers).
- [x] Azure OpenAI Responses.
- [x] Chat-completions compat matrix (store, developer role, max-tokens field, thinking formats, routing, provider hosts).
- [x] Anthropic extras: OAuth (Claude Pro/Max) headers and tool-name mapping, betas, adaptive thinking, 1h cache TTL.
- Out of scope (decision 2026-11-18): Google/Vertex, Bedrock, Mistral and the Copilot device flow were serializer-only or unwired in the shipped binary.
- [x] Built-in model catalog (providers, ~77 models, context/max tokens/pricing/thinking maps); `models.json` overrides incl. `!command` keys; cost calculation incl. service tiers; thinking levels minimal/xhigh with clamping.
- [x] OpenRouter extras: pricing/metadata, disk cache, credits; image-generation models.
- [x] Auth: OAuth PKCE login with localhost callback (anthropic, openai-codex), token refresh *(gap in C++)*, `/login` `/logout` GUI, env-var table (~28 providers), legacy auth.json shapes, `--api-key`. (Verified 2026-11-18 against mock OAuth and provider endpoints; the real Anthropic/OpenAI token endpoints were not called.)
- [x] Ollama management: instance CRUD, status, running, info, pull, load/unload, delete (GUI panel).
- [x] Image handling: dimension sniffing, EXIF orientation, resize to 2000×2000 / 4.5 MiB, `images.autoResize`. (Verified end to end 2026-11-18.)
- [x] In-process llama.cpp via a `qt-llama-sys` crate building the PrismML fork (Vulkan/CPU verified on Linux 2026-11-18: discovery, contextWindow, GPU offload, generation through qt-cli); settings.llama, offload planning, GGUF discovery, tool-call parsing, embeddings, `/llama` panel.
  - [x] `/llama` archived parity: `grammar inline|file|off` runtime override with safe path resolution (shown by `grammar show` and `status`), `run instruct` 8-round framework tool loop (128 tokens/step, repeat suppression, archived lines), `gpu` `useMmap`/`useMlock` and archived errors. Verified with qwen2.5-3B on CPU (`QT_LLAMA_COMMAND_TESTS=1`, `crates/qt-runtime/tests/llama_commands_native.rs`).

## Milestone 5: Tools

- [x] Workspace path policy: `workspaceRoot`, `allowExternalFilesystemAccess` (global only; project may narrow), applied to all path tools and bash cwd; prompt text.
- [x] Tool aliases and argument normalisation (`read_text`, `file_path`, `old_string`, …); `--tools` / `--no-tools` / `--no-builtin-tools`; `/tools` toggles; mutation approval queue.
- [x] Extras: read repeated-read guard and image resize; edit `replace_all` and NFKC fallback; grep `output_mode`, `head_limit`, `offset`, `multiline`, `type`, `hidden`, `no_ignore`; find `type`/`directories`; ls `recursive`/`depth`/`ignore`; bash `timeout_ms`, `workdir`, details fields, streaming.
- [x] New tools: append_text, apply_patch (Codex envelope), clipboard, complete_path, package_manager (plans only).
- [x] Web tools: web_fetch, web_search (DuckDuckGo), web_run (stateful batched browser); public-URL policy; `PI_OFFLINE` / `--offline`.
- [x] `!cmd` / `!!cmd` user bash with session entries.
- [x] `@file` arguments and mentions (inline `<file>`, images), path autocomplete.

## Milestone 6: Project index

- [x] Background scanner (gitignore-aware, fingerprints, 30 s rescans), SQLite + FTS5 store under `agent_home/index/<hash>`.
- [x] Build metadata (CMake, Make, Ninja, qmake, waf, PlatformIO), compile_commands generation, include graph.
- [x] C/C++ code index (classes, methods, functions, call graph) and tree-sitter metadata.
- [x] Model tools: symbol, analyze_file, references, build_context, search_index, `index` router.
- [x] Router operation groups: core, code, fs, git, build. (Decision 2026-11-18: serial, debug, rtos, fetch and util groups are not ported.)
- [x] `/index` panel.

## Milestone 7: Subagents and MCP

- [x] Agent definitions (frontmatter, built-in scout/planner/reviewer/worker, user/project scope, trust). (Verified end to end 2026-11-18.)
- [x] `subagent` tool: single, parallel, chain; child `qt-cli --mode json` processes; depth limits. (Verified end to end 2026-11-18.)
- [x] Persistent workers: spawn/send/follow_up/wait/interrupt/list, storage, mailbox, completion events, restart recovery, fork snapshots. (Verified end to end 2026-11-18.)
- [x] MCP client: config files and merge order, stdio / streamable-http / sse, sampling, proxy tools `mcp_<server>_<tool>`, resource tools, stderr log; `/mcp` panel.

## Milestone 8: Modes and CLI

- [x] JSON event mode (`--mode json`), Pi event shapes, exit codes.
- [x] RPC mode (all commands and events, extension UI requests).
- [x] Full CLI flag set (model, thinking, tools, skills/templates/themes toggles, `--no-context-files`, `--list-models` table, `--export`, `--offline`, logging flags/env).
- [x] Desktop app (`qt`) takes the archived interactive flag set through the shared parser (`qt_runtime::cli_args`): every runtime flag as in `qt-cli`, `--cwd`, session flags, headless-only flags refused with a pointer to `qt-cli`, messages / `@file` / piped stdin sent as the first prompt, archived startup warnings (package updates, Anthropic subscription auth) in `Runtime::diagnostics()`.
- [x] `qt` command-line messages run like typed composer lines (`Workspace::submit_startup_lines`): built-in, extension, template and skill slash commands, `!` / `!!` user bash and prompts, in order; plain messages behind a running turn queue as follow-ups, commands and `!` lines wait for the run / bash before them, an abort returns unsent lines to the composer. Verified on Xvfb 2026-11-18 (`/model ollama:local/qwen3.5:9b` + prompt; prompt, prompt, `/name`, `!echo`, prompt in order). Gap: a slash command whose work is asynchronous (`SlashOutcome::Task`, extension commands that start runs later) does not hold back the lines after it.
- [x] Subcommands: `install/remove/update/list` packages, `config`, `memory`.
- [x] Startup migrations (oauth.json, sessions dir, tools→bin, commands→prompts).
- [x] Logging (`--log-file`, `--log-stderr`, `--log-level`, `QT_LOG_*`).

## Milestone 9: GUI equivalents of interactive features

- [x] Command palette / slash commands with autocomplete (all built-ins listed in the inventory).
- [x] `/settings` panel, `/session` and `/status` info, `/context` inspector, `/export` (HTML + JSONL), `/share` (gist), `/copy`, `/name`, `/changelog`, `/scoped-models` / `enabledModels`, model refresh. (Verified end to end 2026-11-18.)
- [x] Image paste and drop into the composer; prompt history (`history.jsonl`) with Up/Down; external editor.
- [x] Status line (archived footer: cwd + git branch + session name, `(provider) model • thinking`, OpenRouter credits every 5 min), startup diagnostics as notifications, 5 s memory review notification, live compaction/branch markers after compactions outside the view's runs, `terminal.promptHistoryLimit` in Settings, per-session `promptHistory` header (every submitted line). Verified on Xvfb 2026-11-18. Extension commands can call `ctx.compact()` (their compaction hooks run on the waiting thread; 2026-11-18). Gap: `input` hooks run on the UI thread, so an input-hook compaction (astra-auto-compact) blocks the window while it summarizes and its marker only appears on the next transcript reload (the prompt starts before the reload listener runs); needs `workspace/subsystems.rs` to run input hooks off the UI thread.
- [x] HTML export renderer.

## Milestone 10: Extensions

- [x] Discovery (`-e`, `.pi/extensions`, agent home, packages with filters), manifest v1, `package.json` `pi.extensions`.
- [x] Rhai script runtime (decision 2026-11-18, replaces AngelScript; `.rhai` files) with the same host API (`pi.*`, `ctx.*`), lifecycle events, commands, tools, flags, providers, resources, step budget and source limits.
- [x] Subprocess JSON-RPC transport.
- [x] `ctx.ui.*` dialogs and status/widgets/header/footer/working indicator mapped to GPUI; message renderers, shortcuts, autocomplete providers, custom components.
- [x] `/extensions`, `/skills`, `/prompts`, `/themes` package toggles; `/reload`.

## Milestone 11: Governed memory

- [x] SQLite store with migrations v1–v7 and triggers; domain model, scopes, governance transitions, audit.
- [x] CRUD with secret redaction; capture/ingestion worker with spool and dedup; post-processing. (Verified end to end 2026-11-18.)
- [x] Recall: lexical (FTS5 + hashed similarity), graph, dense (embeddings, indexer jobs), code; RRF fusion; explanations (Code recall verified end to end 2026-11-18: persisted index snapshot, pinned FTS lexical provider, `code_bm25`/`code_graph` in CLI/RPC explain, dashboard and injected context.)
- [x] Context rendering envelope and placement; pre-handler child protocol. (Verified end to end 2026-11-18.)
- [x] Review workflow (kinds, actions, batches, backup/restore, stats).
- [ ] Dreaming, web research, auto_validate verifier. (Full cycle verified with real Dream and verifier children on a local llama model and a stubbed research child; gap: the real research child has not been run against public web search/fetch.)
- [x] Skill import, offline qtmem import, export/prune/scrub.
- [x] Memory tools, `/memory` panel incl. review inbox, settings, `qt memory` CLI, RPC actions. (Verified end to end 2026-11-18.)
- [x] Dashboard as a native GPUI window (replaces the ImGui/WASM browser dashboard).
- [x] One memory scope per working tree across front ends: app, `qt-cli` print/json/rpc and `qt memory` all use `workspace_memory_scope(cwd)` (session-free exact scope), so RPC/print/tool memories count in every session's `/memory status`, inbox, dashboard and the CLI; stores with per-session exact scopes of the same tree are folded in on open. Acceptance: `qt-cli/tests/memory_scope.rs` (RPC remember → second RPC session and `qt memory status/recall` from root and subdirectory), `qt-memory/tests/runtime.rs` adoption test. Gap: not re-checked in the GUI on Xvfb (the app uses the same `qt-runtime` path as RPC).

## Platform

- [ ] CI on Linux, macOS, Windows (`.github/workflows/ci.yml`, not run yet).
- [ ] Packaging (macOS `.app`, Windows MSI, Linux AppImage/Flatpak).
- [ ] Manual GUI verification on macOS and Windows.
