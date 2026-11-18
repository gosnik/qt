# Reference Verification Map

Native extension runtime checks:
- `python3 scripts/extension_selector_probe.py` checks `/extensions` with the
  actual Astra plugin loaded by relative `-e`, with/without `--no-extensions`,
  project/user discovery, case-insensitive filters, read-only Enter/Space,
  canonical CLI/package overlap, and installed package toggle persistence.
- `python3 scripts/astra_auto_compact_probe.py` loads the actual opt-in Astra
  plugin against a local provider to check the strict 200000-token boundary,
  prompt continuation, and persisted compaction. The engine-backed
  `extension_runtime_probe.py` additionally covers case-insensitive model ID/name
  matching, nonmatching models, busy/unknown usage, and compaction failure retry.
- `python3 scripts/extension_runtime_probe.py` links the real AngelScript engine
  and checks persistent state, callback failures, nested invocation, and budgets.
- `python3 scripts/extension_host_probe.py` drives the real interactive app with
  an explicit native extension, checking flags, dialogs, scripted components,
  repeat invocation, custom message rendering, reload, and durable lifecycle entries.
- `python3 scripts/extension_lifecycle_probe.py` runs a local streamed model
  turn and verifies prompt/message injection, tool blocking and result replacement,
  plus persisted startup/shutdown state.
- `python3 scripts/angelscript_context_probe.py` exercises resolved session context
  and usage/window inspection, current and catalog model metadata, synchronous
  idle-session compaction, custom summary instructions, persistence, and unknown
  usage immediately after compaction.

The previous native test map has been retired with the unit-test framework
removal. Native parity tracking now records reference areas and implementation
owners without pointing to checked-in test cases.

Verification for completed parity work is expected to use production builds,
targeted runtime probes, CLI/RPC inspection commands, and AngelScript binding
coverage where extension APIs are involved.

## Current Verification Surfaces

- Production build: `pio run -e native`
- Release build: `pio run -e native_full`
- Native AngelScript tests: `pio test -e native`, with test scripts under
  `test/**/*.as` and spdlog output written to
  `.pio/test/angelscript-tests.log`. The TUI PTY/tmux probe is registered as an
  AngelScript test and can be skipped on dependency-limited hosts with
  `QT_TEST_SKIP_TUI_PTY_PROBE=1`.
- Standalone TUI probe: `scripts/tui_pty_probe.py`
- Terminal resize probe: `python3 scripts/tui_resize_probe.py` starts the
  interactive app in a PTY, resizes it (delivering `SIGWINCH`), and verifies
  the live region repaints at the new width without exiting, for both the
  forced-ANSI and notcurses-requested configurations.
- Index persistence smoke coverage: instantiate `qt::index::BackgroundService`
  against a small project, run a foreground scan, confirm
  `index.sqlite3` contains FTS rows, then construct a fresh service and verify
  `snapshot_loaded=true` with `index_stale=true`.
- External MCP coverage: `pio test -e native` runs the AngelScript
  `mcp_client_probe` test, which compiles a focused native harness against the
  real `McpClientManager`. The harness starts local MCP stdio, Streamable HTTP,
  and legacy SSE servers, then verifies tool discovery, tool calls, stdio
  environment filtering, credential redaction, reconnect/retry behavior, and
  `sampling/createMessage` handling.
- Global prompt-history coverage: `pio test -e native` runs the AngelScript
  `prompt_history_probe`, which compiles a focused native harness against the
  real global store and verifies count bounds, newest-first recall order,
  multiline values, adjacent duplicate suppression, Unix file privacy,
  atomic concurrent writers, corruption recovery, default/config validation,
  cross-process and cross-workspace recall, and zero-limit clearing.
- Subagent core coverage: `pio test -e native` runs the AngelScript
  `subagent_probe`, which compiles a focused native harness and verifies
  recursive/precedence-aware role discovery, exact-one-mode validation,
  mutation approval, isolated single execution, source-ordered bounded
  parallelism, chain handoff substitution, partial child failure, project-role
  trust gating, workspace cwd confinement, depth limiting, streaming updates,
  and process-tree cancellation.
- `python3 scripts/subagent_probe.py --binary .pio/build/native/qt` additionally
  drives `/subagent` through a raw PTY. It verifies the root and nested menus,
  custom user-role metadata, task capture and deterministic direct child launch,
  project-role trust warnings, and messaging when the tool is disabled.
- `python3 scripts/agent_harness_cache_probe.py --turns 25 --mode cached`
  also checks runtime-factory diagnostic retention across repeated turns,
  complete hook delivery, save-point sequencing, session continuity, and
  retention after harness moves.
  additionally verifies that a direct user tool turn does not invoke the
  provider, records harness call/result hooks, and commits a paired
  user/assistant-call/tool-result transcript sequence. It also verifies atomic
  provider/model metadata switching and Pi-compatible 32,000-token default
  output capping.
- Optimized web-access coverage: `pio test -e native` runs the AngelScript
  `web_run_probe`, which compiles a focused native harness and verifies the
  honest command schema, batched search, recency/domain forwarding, stable
  refs, deferred open/click/find navigation, external-context marking,
  private-address rejection, response/query bounds, cancellation, and offline
  denial without making live network requests.
- CLI inspection commands: `--inspect-context`, `--inspect-system-prompt`,
  `--inspect-skills`, `--inspect-prompt-templates`, `--inspect-settings`, and
  `--inspect-slash-commands`
- AngelScript extension surface: `qt::extensions::angelscript_binding_catalog()`
  and the engine registrations installed by `AngelScriptHost`

## Built-in memory

- `python3 scripts/memory_probe.py` compiles and runs the direct native memory
  stack without MCP. It covers resolved-global-root persistence and file modes,
  schema creation/restart/future-version rejection, repository scope isolation,
  FTS/governed recall, context budgeting, lifecycle transitions, strict
  pre-handler result validation, parent-owned envelope normalization,
  isolated-child recursion/resource guards, empty-pack child bypass,
  retry/tool-loop caching, steer/follow-up and
  projection resets, timeout and cancellation fallback, RPC framing, and
  mutation permission denial. It also
  injects legacy sensitive rows directly, verifies scoped dry-run/apply
  `redaction_scrub` behavior and FTS/export cleanup, and proves child projection
  reduces images to metadata while bounding and redacting tool/model content.
  The direct probe also verifies scoped memory statistics from authoritative
  storage: zero/populated snapshots, type/status and quality counts, access and
  procedure outcomes, conflicts, pending reviews, isolation, and restart
  persistence. Native surface checks require the same nested statistics object
  from CLI and RPC status. The probe also drives the isolated semantic
  consolidation child contract, requiring Ternary Bonsai model routing,
  recursion/resource guards, parent-validated IDs, a pending review-only result,
  and a durable consolidation receipt.
- `python3 scripts/memory_model_routing_probe.py` compiles the production GGUF
  selector and verifies every task mapping, including Qwen 3B synchronous
  recall, Ternary Bonsai 27B context compilation, Qwen 3B fallback when the 27B
  file is unavailable, and deterministic tie selection.
- `python3 scripts/dreaming_probe.py` compiles the focused native Dreaming
  lifecycle against the production memory stack. It verifies the isolated-child
  flags and recursion guard, research-policy-scoped access to `web_run`,
  `web_search`, and `web_fetch` while
  ordinary Dreaming remains tool-free, bounded governed source selection with
  multi-cycle diversity and reproducible selection metadata,
  strict parent validation, cross-repository exclusion, all supported trigger
  labels, repeated manual and interval cycles over an unchanged eligible pool,
  per-cycle idempotent durable claims and receipts, pending reviews,
  `derived_from` edges, trusted-recall exclusion, supplied-evidence research
  successors, conservative review promotion, interval and post-turn
  idle/foreground gating, durable success/failure statistics, protocol-version
  evidence, and typed malformed-output failure isolation. Its runner links the
  retrieval-engine, indexer, graph, and code-snapshot objects the production
  memory runtime now depends on. The AngelScript
  memory host runs it after model-routing coverage.
- `python3 scripts/memory_review_probe.py --require-target-contract` compiles
  the native review domain/storage stack and seeds every supported review kind,
  mixed priorities and lifecycle states, malformed legacy data, missing and
  cross-scope subjects, and stable list/detail/preview/result/history/batch/error
  JSON fixtures. It proves schema-v5 migration fields, indexed filters,
  including source type, Dreaming cycle, stale/conflict, and durable ranking
  signals; empty, single, mixed, filtered, paginated, deferred/due, and
  restart-persistent queues; stable score ordering and explanations; scheduled
  deferral, audited reopen/history, pending-only concurrent compare-and-set,
  idempotent replay, atomic rollback, scope isolation, export/import,
  sensitive-data scrubbing, retention preservation, integrity across restart,
  hard-guidance exclusion for memories in unresolved critical conflicts,
  and durable review-statistics reconciliation across age/risk/scope distributions,
  decision and completion duration, typed outcomes, failures, and batch totals.
  It also proves that reported review statistics merge the exact turn scope with
  its canonical durable scope, so a status count reconciles with the queue the
  Review Inbox lists instead of reading the turn scope alone.
- `python3 scripts/memory_review_ui_probe.py --binary .pio/build/native/qt`
  drives the ordinary `/memory review` keyboard path through the Inbox, queue,
  detail, kind-constrained edit fields, defer and reject cancellation, Source
  Inspector, and filter overlays. It verifies that external evidence is blocked
  when it falls outside the workspace under the default filesystem policy, and
  that users can inspect and triage a review without copying an opaque
  identifier or reading raw candidate JSON. The interactive implementation
  is also exercised across nested prompt cancellation and queue-to-landing-to-
  editor Escape navigation on both fallback and notcurses input backends, proving
  review transitions replace rather than accumulate stale overlay screens. It
  also provides constrained multi-selection with kind/scope/risk summaries,
  exact aggregate preview, transcript progress/results, and failed-selection
  preservation for retry or inspection.
- `python3 scripts/memory_review_rpc_contract_probe.py --binary
  .pio/build/native/qt` verifies structured review-RPC failures use stable error
  codes and that malformed batch schemas are distinguished from missing jobs
  and authority failures. The same RPC surface exposes non-blocking
  `batch_start` and pollable `batch_status` operations so long review batches do
  not block unrelated commands.
- `python3 scripts/memory_probe.py --binary .pio/build/native/qt` additionally
  runs native `qt memory` management and RPC-mode smoke checks, including
  structured-stdout cleanliness, confirmation denial, and `--no-memory`; it
  also starts credential-free offline print/JSON invocations and proves the
  override does not initialize durable memory. A local deterministic provider
  then runs enabled/disabled successful model turns, proves isolated-child and
  main-request injection, and writes the measured baseline documented in
  `docs/memory-rollout-report.md`.
- `python3 scripts/memory_settings_probe.py --binary .pio/build/native/qt`
  drives the `/memory` Settings submenu through a raw PTY, verifies global JSON
  persistence and unknown-key preservation, reloads all nested memory settings
  through `--inspect-settings`, and proves enable/disable changes take effect
  in fresh runtimes without requiring libvterm or tmux.
  It also requires `/memory status` to render its visible statistics section.
- `python3 scripts/memory_dashboard_probe.py --binary .pio/build/native/qt`
  starts the browser dashboard through both the `/memory` menu and explicit
  `/memory dashboard` command, proves loopback-only authenticated shell/API
  access, validates the scoped live snapshot contract, checks idempotent
  startup, verifies UTF-8-safe graph excerpts and malformed JSON recovery, and
  verifies that interactive shutdown closes the listener.
- `pio test -e native` runs `test/test_angelscript/scripts/memory_probe.as`,
  exercising the direct C++ probe and the strict governed-review contract probe
  through separate AngelScript host functions; its prompt-history host check
  also runs the interactive memory-settings and dashboard probes. Set
  `QT_AS_TEST_FILTER=review` to run only the governed-review host test.
- `python3 scripts/filesystem_access_probe.py --binary .pio/build/native/qt`
  compiles a focused production-tool probe for confined/permitted absolute and
  symlink-resolved writes, then verifies the secure default, global/project/MCP
  policy layering, interactive persistence, unknown-key preservation, and
  restart loading. `scripts/subagent_probe.py` covers both confined and
  explicitly permitted child working directories.
- `python3 scripts/tool_call_pipeline_probe.py --binary
  .pio/build/native/qt` runs the real application against a credential-free
  fake OpenAI-compatible provider. It verifies canonical outbound read/symbol
  schemas and active-tool prompt guidance, forces an invalid missing-parameter
  call, requires retryable field-level feedback, then completes a corrected
  indexed symbol lookup in a temporary C++ workspace. The AngelScript
  prompt-history host check runs this probe after the native build.
- `python3 scripts/model_resolution_probe.py` compiles the production model
  resolver and verifies that persisted and explicit OpenRouter selections keep
  slash-delimited provider-scoped ids, including the optional matching provider
  prefix, without falling through to another authenticated provider. The
  AngelScript memory host check runs this probe before the memory pipeline
  probes.
- `python3 scripts/openrouter_catalog_probe.py` compiles the production
  OpenRouter discovery/cache normalizer without live HTTP. It verifies the
  reference 4096-token fallbacks, conservative top-provider context handling,
  positive completion/context invariants, malformed-cache record isolation, and
  `QT_CODING_AGENT_DIR` cache routing. It also verifies that catalog refresh
  removes the prior discovered slice, installs newly discovered models, and
  retains configured OpenRouter entries. Explicit refresh failures remain
  distinguishable from successful cache fallback.
- `python3 scripts/openrouter_limits_probe.py --binary .pio/build/native/qt`
  verifies OpenRouter request shaping with both a compiled transport probe and
  a fake OpenAI-compatible HTTP server. It covers the Pi-compatible 32,000-token
  default, smaller model ceilings, compatibility `max_tokens`, and prompt-aware
  clamping against the active context window.
- Pre-handler wire fixtures live under `test/fixtures/memory/`; source and
  adaptation provenance is recorded in `docs/memory-source-provenance.md`.
- `pio run -e native` plus
  `qt --discover-local-models models --list-models` covers native GGUF metadata
  discovery and verifies that BGE/Nomic are embedding models while SmolLM2/Qwen
  remain generative. The real stored `cli-anything-gimp` recall smoke records a
  Qwen 3B `recall_prehandler` receipt with isolated execution, no deterministic
  fallback, bounded candidates, and the exact active skill memory ID.
- `test/test_angelscript/scripts/prism_ternary.as` compiles against the pinned
  built-in llama/ggml ABI and asserts Prism's file type 41, tensor type 42,
  group-128/34-byte block layout, and separation from `TQ2_0` without a model
  download. `scripts/prism_ternary_probe.py` is the opt-in end-to-end smoke: it
  accepts an already cached official 1.7B GGUF through `QT_PRISM_MODEL_PATH` or
  `--model`, verifies the exact size and SHA-256, and runs bounded deterministic
  text generation through the native `llama` provider on CPU or a requested
  GPU layer count. With `--memory-consolidation` it drives the strict governed-ID
  selection grammar used by the background memory worker; the local 27B file is
  accepted only with the explicit `--allow-unpinned-model` test override. Network
  transfer occurs only with explicit `--download`.
- `python3 scripts/skill_memory_scan_probe.py` compiles the production skill
  importer and SQLite/governance stack. It verifies strict non-recursive scans,
  deterministic recursive skill-root discovery, nested/hidden package boundaries,
  bounded related text with binary/hidden-file omission, containment and symlink
  rejection, redacted provenance, active skill typing, identical and concurrent
  rescan deduplication, and changed-package supersession.
- `python3 scripts/memory_skill_scan_ui_probe.py --binary .pio/build/native/qt`
  drives the interactive `/memory` menu and folder submenu through a PTY, starts
  a recursive background import, verifies per-skill/final transcript reporting,
  and checks that direct external-folder scans respect the workspace boundary.

## Ollama providers

- `python3 scripts/ollama_probe.py` compiles the focused production Ollama
  discovery/catalog implementation with a deterministic HTTP transport. It
  verifies `/api/tags` plus `/api/show` routing, bearer isolation, context and
  capability metadata, `/v1` endpoint normalization, structured HTTP errors,
  and preservation of identical raw model tags across two provider instances.

## Persistent subagents

- `python3 scripts/persistent_subagent_probe.py` verifies coordinator spawn,
  mailbox/reuse/interruption, wait timeouts, context forks, private persistence,
  restoration, and lock/corrupt-index preservation using real isolated child
  processes.
- `python3 scripts/persistent_subagent_cli_probe.py --binary .pio/build/native/qt`
  checks native JSON-mode delegation and automatic completion delivery with a
  local mock provider.
- `python3 scripts/persistent_subagent_rpc_probe.py --binary .pio/build/native/qt`
  checks enabled RPC spawn, messaging, wait, restart reuse, new-session events,
  completion acknowledgment, and disabled-tool permission gating.
- `python3 scripts/process_tree_probe.py` verifies cancellation of descendants
  that create independent process groups.

## Deep TUI scan regressions

- `python3 scripts/input_escape_probe.py` covers fragmented UTF-8 and Kitty
  codepoint deduplication.
- `python3 scripts/editor_cursor_probe.py` covers word undo boundaries and
  shared large-paste snapshots, including a Linux peak-RSS guard.
- `python3 scripts/interactive_terminal_input_probe.py` checks owned extension
  rewrites, consumed input, and error propagation through the real session.
- `python3 scripts/tui_render_regression_probe.py` covers malformed escape
  progress, native control stripping, overlay state, and combining-mark width.
- `python3 scripts/image_render_cache_probe.py` covers cache reuse and key
  invalidation while preserving Kitty image identity.
- `python3 scripts/interactive_final_probe.py` covers streamed scrollback,
  assistant/tool ordering, CRLF paste payloads, and cancellation/restoration
  during delayed provider requests and asynchronous bash commands.
- `python3 scripts/agent_harness_cache_probe.py` also verifies cross-thread
  shutdown cancellation of a retained tool token.
