# Upstream Compatibility Notes

`reference/pi` remains the behavioral source of truth. This file records native
changes that are cleanup or refactor oriented so future parity work can tell
whether behavior was intended to move.

## Runtime diagnostic retention

- App runtime hosts retain the latest entry in each harness diagnostic hook
  history and the latest save-point snapshot. Previously every provider call
  and completed turn retained additional copies of full conversation/request
  payloads for the runtime's lifetime, causing cumulative allocation growth.
- Live subscribers and context, provider, tool, and lifecycle hooks still
  receive every event. Session persistence and the active conversation are
  unchanged; `/context` can still inspect the latest completed provider request.
  Save-point sequence numbers continue increasing when old snapshots are
  discarded. Pi's agent delivers lifecycle events to subscribers; these native
  diagnostic history vectors are not part of its conversation contract.
- Standalone `AgentHarness` instances preserve cumulative diagnostics by
  default for probes; `set_diagnostic_history_limit(0)` selects full history,
  and a positive limit bounds retained entries per diagnostic history.
  This bounds duplicate snapshots by count, not total process memory: active
  conversations, session entries, and TUI transcript storage still grow with
  session content, and one request/tool result can itself be large.

## TUI scan corrections

- Input retains fragmented UTF-8 until complete and deduplicates Kitty printable
  events by Unicode codepoint. Bracketed CRLF paste normalizes once, preserving
  its line count. Extension rewrites remain owned through input decoding.
- Typed words share an undo boundary, with whitespace and non-typing actions
  opening new boundaries as in Pi. Undo snapshots share immutable large-paste
  payloads while retaining marker and cursor state.
- ANSI parsing always advances on unsupported or incomplete ESC sequences.
  Native cell rendering consumes terminal controls instead of displaying them;
  overlays restore inherited SGR and OSC8 state at the uncovered tail.
  Combining-mark widths use the pinned Unicode property implementation.
- Each provider response owns its assistant/thinking transcript components,
  preserving text before and after tool calls. Content updates are coalesced
  within each drained event batch. Assistant widgets own message snapshots.
- Images retain one render variant keyed by constraints, terminal capabilities,
  and cell dimensions, reusing the component's Kitty image identity.
- Interactive exit and fatal-error cleanup request provider/tool and asynchronous
  bash cancellation before waiting for completion. Shutdown cancellation uses
  atomic provider signals and locked tool tokens, independent of harness phase
  bookkeeping. Cancellation remains cooperative; editor render errors retain
  their existing recoverable presentation.

## Interactive terminal resize

- The native interactive TUI now installs its own `SIGWINCH` handler and
  re-queries the terminal size from the main loop, matching Pi's
  `process.stdout.on("resize", ...)` behavior. Previously the notcurses
  backend disabled notcurses' own `SIGWINCH` handler and the ANSI fallback had
  no handler at all, so a resize was never observed until unrelated input
  arrived.
- On a size change the session updates its render constraints, re-seeds the
  native scrollback region, and repaints the live region at the new width and
  height. The diff renderers are intentionally not reset so the width/height
  change is detected and a full redraw is emitted, matching Pi's
  `fullRender(true)` on width/height change.
- Resuming from suspend re-raises `SIGWINCH` so dimensions changed while the
  process was stopped are picked up, matching Pi's refresh-on-start behavior.

## Model-facing external filesystem access

- Native qt keeps model-facing built-in path arguments confined to the resolved
  workspace by default. The global `allowExternalFilesystemAccess` setting and
  `/settings` toggle can explicitly allow absolute, parent, and symlink-resolved
  paths outside that workspace; project settings may narrow but cannot enable
  the policy, and MCP configuration files cannot alter it.
- The policy is applied to native read, write, append, edit, patch, list, find,
  search, path-completion, clipboard-image, package-manager, bash working
  directory, and sub-agent working-directory arguments. It is not an operating
  system sandbox: shell command contents and independently implemented MCP or
  extension tools retain their own filesystem authority, while the project
  index remains rooted to its configured project.
- `reference/pi` accepts unrestricted absolute paths and has no equivalent
  setting. The secure default and explicit global opt-in are intentional native
  behavior.

## Native AngelScript runtime and UI

- `examples/extensions/astra-auto-compact.as` is an opt-in interactive plugin:
  case-insensitive `astra` substring in model ID/name, current context usage
  strictly above 200000 tokens, and no action for busy or unknown usage.
  Unlike Pi's callback-based `trigger-compact.ts` on `turn_end`, it checks the
  next idle `input` because native compaction is synchronous. Failed compaction
  warns and preserves the prompt. Idle input-hook session mutations now persist
  before prompt dispatch (also on later hook errors); active-turn input retains
  the worker-owned persistence path. Successful `ctx.compact` now installs the
  resolved compacted transcript for the next request, preserving queued input.
  No built-in compaction defaults change.

- Native extensions retain a per-load engine and invoke command, tool, and
  lifecycle handlers instead of discarding the engine after registration.
  Host calls use injected runtime callbacks with weak runtime ownership;
  copied extension descriptors retain the same callback identity.
- Native host calls expose the resolved persisted-session messages, system prompt,
  thinking level, context usage/window, detailed active model, and configured
  model catalog. Idle AngelScript commands can run model-backed compaction with
  custom instructions; the synchronous bridge returns the reference summary
  fields after persisting the compaction. Assistant usage is retained in session
  entries, trailing messages are estimated, and usage stays unknown between a
  compaction and the next model response.
- Interactive commands reach native dialogs, editor operations, status and
  working state, static UI regions, and named scripted components. Session
  persistence includes every entry produced by a command, even before a later
  handler error. Explicit `-e` extensions remain enabled with `--no-extensions`.
- Shortcuts use the interactive presenter too. Input interception, scripted
  editors, autocomplete callbacks, working visibility, tool expansion, and
  loaded-theme accents have native runtime effects.
- Interactive startup/reload hooks run with a live presenter. Reload emits the
  reload reason, clears extension UI callbacks, and creates a fresh script engine;
  startup and shutdown state writes are persisted across replacement.
- Native before-agent and context hooks can change model input; tool-call hooks
  block execution and tool-result hooks change the next model request. Print/JSON
  persistence includes startup and shutdown state and intermediate tool entries.
- The native contract and verification commands are documented in
  [native extensions](native-extensions.md). Reference JavaScript/TypeScript
  module execution is outside this native-only change and remains unsupported.
- This runtime bridge covers interactive and print/JSON modes. RPC's separate
  runtime and bidirectional UI integration remain open under EXT-01.

## OpenAI API model discovery

- Interactive startup and five-minute background refreshes discover available
  OpenAI chat model IDs with authenticated `GET /v1/models`. `/model refresh`
  refreshes OpenAI and OpenRouter, including an already-open model picker.
  Online CLI model listing uses the same OpenAI discovery and honors
  `models.json` endpoint and API-key overrides. Offline mode avoids discovery.
- Discovery supplements the built-in catalog, preserves configured metadata,
  excludes specialized non-chat models, and retains the current catalog on
  network/authentication errors. Discovered IDs are kept for the running session;
  the built-in catalog remains the offline fallback after restart.
- The Models API supplies IDs rather than capability/price metadata. Unknown
  chat models use text-only defaults of 128,000 context and 16,384 output tokens
  as conservative budgets, with unknown pricing; `models.json` can override them.
- `gpt-6-astra` has a built-in OpenAI Responses entry from the
  [official model documentation](https://developers.openai.com/api/docs/models/gpt-6-astra):
  text/image input, 1,050,000 context, 128,000 output tokens, and base token rates.
  Its visible reasoning settings use qt's supported low/medium/high/xhigh levels;
  the API's additional `max` setting is outside the current native level set.
  This does not change the separate Codex subscription catalog.
- Discovery normalization, overrides, filtering, and failures are checked by
  `scripts/openai_discovery_probe.py`; `scripts/openai_models_probe.py` exercises
  authenticated discovery, live picker updates, and CLI offline fallback.

## Search glob filtering

- Search filters preserve root-level matches for `**/*.ts`, immediate-child
  matches for `src/**/*.ts`, and nested basename matches for `*.ts`, consistent
  with the reference's ripgrep globs. Native fallback filtering uses the same
  rules for these patterns.
- `scripts/search_glob_probe.py` checks both ripgrep and native fallback paths,
  including ordinary `*` directory boundaries and nonmatching extensions.

## Prompt argument boundaries

- Prompt argument positions and slice lengths no longer wrap around on oversized
  decimal values. Out-of-range positions expand to empty text; long slices keep
  the remaining arguments, matching the reference behavior.
- `scripts/prompt_template_probe.py` covers normal substitutions, zero bounds,
  machine-integer boundaries, and decimal values hundreds of digits long.

## Storage I/O failures

- Shared text-file writes, the append tool, and session append/replacement now check stream closure
  before returning success. Buffered disk-full errors propagate as I/O failures,
  matching the reference's awaited file writes and synchronous session writes.
- `scripts/storage_write_probe.py` checks real `/dev/full` failures and successful
  file/session round trips, including replacement with an empty session.
- Shared file reads now propagate read errors instead of treating them as empty
  content, matching the reference file-read failure behavior. The probe covers
  directory-read errors as well as empty files and content spanning read buffers.

## Built-in governed memory

- qt adds opt-in built-in governed memory sourced from qtmem commit `3324ace`.
  `reference/pi` has no equivalent persistent-memory subsystem, so the feature
  defaults off and `--no-memory` is an unconditional invocation override.
- Durable memory is stored in `<agent_home>/memory/memory.sqlite3`; it never
  requires qtmem, MCP, HTTP, a daemon, a listening port, or project-local
  storage. SQLite FTS5 and deterministic hashed-token similarity provide the
  initial retrieval implementation.
- Memory V2 adds opt-in `memory.retrieval.graphEnabled` and `denseEnabled`.
  Mixed-path JSON explanations include relation provenance and the code index's
  heuristic flag. Context rendering uses the same provenance, including heuristic
  symbol definitions, and final validation rejects altered heuristic metadata.
  Interactive `/context` defaults to full available prompt, session, resource,
  diagnostic, usage and memory details; named subsections filter this output.
  Memory inspection copies the latest prepared contribution for the current
  harness projection without initiating recall or changing access counts.
  Normal automatic recall applies bounded memory-graph traversal and reciprocal
  rank fusion of independent lexical, graph and dense candidates. Paths carry
  direction and complete connecting evidence; excluded or stale endpoints cannot
  supply graph context. Defaults preserve the previous recall mode.
- Retrieval ranking revision `owner-rrf-v2` aggregates dense chunks before the
  owner limit and retains each owner's best chunk, with deterministic numeric
  ordinal ties. Repeated hits from one retriever supply one fusion rank and do
  not consume additional owner slots. Invalid stale hits cannot suppress a
  subsequent valid hit for the same owner.
- Dense memory recall preserves its matched chunk, source locator, captured
  owner revision, cosine score and embedding-index generation. Explanations add
  `dense_match` (null when absent), `score_breakdown.dense_similarity`, and
  `revision.basis="dense_capture"` for captured dense evidence; they do not relabel
  an old capture as the current revision. Recall and final context packing reject
  changed or unverifiable revisions, even if prose is restored to its prior
  value. Per-item authority lookup failures omit that item while preserving
  independent context. Evidence remains transient, outside canonical memory
  storage. The internal C++ `DenseCandidateProvider` now returns typed
  `DenseMemoryCandidate` values; custom providers must supply verified evidence
  and repository revision authority. Existing row-only engine/storage query
  helpers remain available, but cannot be used as typed recall providers.
- Dense code candidates retain their matched representation chunks as transient
  supporting evidence. `RetrievalCandidate` gains a `matched_chunks` field
  holding the exact dense hits (chunk, cosine similarity and index generation);
  the canonical source window remains the only chunk rendered into context, so
  context bytes and budgets are unchanged. Retrieval explanations add a
  `matched_chunks` array (empty when absent) alongside the existing `chunks`
  array. `resolve_code_hits` now populates `matched_chunks` instead of `chunks`;
  callers that need the rendered window must use the engine's `search_code`.
  Matches are deduplicated by chunk id, generation and owner revision when
  lexical, dense and graph candidates merge onto one owner.
- Memory schema 7 persists the indexing retry ceiling (five attempts by default).
  Recovery exhausts expired leases at that ceiling, including workers that crash
  before reporting failure, while permitting independent jobs to proceed.
  Explicit failure policies remain configurable from one to 100 attempts and
  survive reopening the database. Recovery preserves exact-scope, revision and
  generation filters and does not retire an active lease.
- Dense retrieval requires an explicit registered local `embeddingModel` ID.
  Query and document embeddings share the same GGUF content fingerprint,
  tokenizer, pooling, normalization, prefixes and dimensions. SQLite stores
  revision-pinned chunks, vectors, leased indexing jobs and separately published
  model generations; a cancelled or failed encoder leaves lexical/graph recall
  available. Background encoding pauses between jobs and embedding chunks during
  foreground activity. A paused job retains its computed vectors in the local
  spool and renews its durable lease every second; resume does not reclaim the
  job or spend another retry. Shutdown wakes the wait. A model call already past
  its checkpoint may finish before the pause, and model loading/device calls
  still have no hard deadline.
  Memory status RPC reports configured retrieval settings, indexing progress and
  embedding errors. It also reports effective code freshness under
  `retrieval.code` (`enabled`, `available`, `stale`, `generation`, `repository`,
  `worktree`) and query-cache accounting under `queries.cache_entries` and
  `queries.cache_bytes`; the dashboard snapshot and summary expose the same
  fields. These are additive; existing keys are unchanged. Rebuild counts remain
  available only from the rebuild command response, not from status.
  Opt-in `codeEnabled` adds independently retrieved code
  excerpts from the native index under the shared context budget; excerpts carry
  snapshot generations and source fingerprints and are revalidated before use.
  Retrieval snapshots and their additional file hashing are disabled unless
  memory and code retrieval are both enabled in the runtime. With both graph and
  code retrieval enabled, code and memory seeds produce bounded mixed paths and atomic context bundles
  containing source excerpts, governed memory, directed relations and evidence.
  Source drift, critical conflicts, source revisions and persisted relations are
  rechecked before injection. Graph recall also checks critical reviews in each
  memory's canonical scope before admitting a globally visible bridge, using a
  request-local cache. Structured claims retain polarity, qualifiers,
  validity and revision evidence alongside their prose in one budgeted context
  item, including historical snapshots. Opt-in graph recall additionally walks
  entity assertions using bounded relation policies and keeps their governed
  owners/revisions as an atomic evidence path. Negative assertions terminate
  traversal; assertion evidence source drift is checked before recall and final
  runtime context. Explicit entity seeds can traverse scoped code links, and
  multi-claim assertion seeds retain every supporting claim when continuing to
  current code. Literal assertions retain their owning memory and negative
  assertions remain terminal. Mixed bundles fit or are omitted atomically,
  including at the exact context-budget boundary. Failed graph-path validation
  also suppresses standalone fallback for its code endpoint. Full management integration
  and real-encoder rollout gates remain tracked in `mem_v2.md`;
  a configured code flag is not proof of availability.
- Dense query embeddings use both an entry limit (at most 1,024) and a 32 MiB
  retained key/vector/entry budget. Least-recently-used entries are evicted when
  either limit is exceeded; cancelled embeddings are not cached. Hash allocator
  overhead is bounded separately by the entry limit.
- Dense indexing computes one vector at a time, spools unpublished vectors to
  an anonymous temporary file, and streams them into an atomic owner publication.
  It no longer retains every owner vector in RAM or holds a publication write
  lock during model calls. Spool/reader failures preserve retryable durable jobs;
  incomplete vectors are never published, and stale leases fail before reading.
- RPC `memory explain` and dashboard `/context/build` preview the governed
  retrieval/context pipeline without recording access or injection. Shared
  explanations retain per-retriever ranks, directed paths, assertion evidence,
  current-versus-historical revision basis, selection and approximate rendered
  context cost. Preview selection is separate from `injected: false` and unknown
  subsequent usage. RPC distinguishes requested modes from retrievers that
  contributed candidates. Code traversal uses the request's access policy.
  The dashboard exposes query/budget controls and selection details; standalone
  `qt memory explain <query> --json` uses the same runtime preview and serializers,
  with the configured llama encoder and persisted code snapshot. It searches
  published indexes without starting indexing, capture, review or Dreaming workers.
  Missing models leave lexical fallback usable. Persisted code snapshots require
  complete scan-time fingerprints; legacy snapshots require a rescan, and changed
  sources are excluded. Snapshot loading is read-only and transactionally consistent.
  CLI structured filters, `--as-of` and `--token-budget` feed the same preview path.
  Ordinary CLI recall preserves its fields and adds an `explanation` object.
  Context explanations include complete bundle member IDs, selected/eligible
  state, budget-versus-validation omission reasons and rendered item cost.
  Unrenderable evidence has null cost; shared headings/envelope cost is reported
  separately. Candidate explanations reference the corresponding context item
  indices, and the dashboard exposes these details without treating a preview
  as injection or usage.
- Completed-turn capture is durably spooled before background processing and
  recovered using its original scope/session. Candidate effects and idempotency
  receipts commit together. Exact-scope governed lifecycle transitions and their
  graph/audit effects are atomic; derived data is invalidated on canonical edits.
- Canonical exports include scoped entities, aliases, immutable assertion
  snapshots, revision boundaries and typed links. Import can clone into another
  scope in the same database, remapping colliding identities and replacement
  links while preserving historical revisions. Imported vectors are rebuilt
  through newly queued indexing jobs; truncated canonical exports are rejected.
- Automatic recall runs a separate `qt --mode json` child with no session,
  extensions, context files, skills, tools, or memory. Its strict result is
  validated and inserted only into the provider-visible request as an
  preceding untrusted user-role reference message, keeping recalled content out of the
  session transcript and compaction input. If no explicit memory pre-handler
  model is configured, qt discovers the local GGUF model directory and selects
  Qwen 2.5 3B for bounded relevance/ID selection through built-in llama.cpp;
  the trusted parent, not the model, restores warnings/hashes and renders the
  selected context.
- Local memory model routing distinguishes Nomic document embeddings, BGE short
  embeddings, SmolLM2 worthiness gates, Qwen 0.5B extraction/rewrites, and Qwen
  3B latency-sensitive relevance work. Ternary Bonsai 27B Q2_0 is selected for
  asynchronous context compilation and semantic-consolidation review, where its
  additional capacity is useful without adding a multi-gigabyte cold load to
  every prompt. Classification uses actual GGUF architecture and model-family
  evidence, including Qwen3.5 context metadata, and generative loads do not
  enable embedding mode.
  Automatic recall shares runtime llama ownership to avoid duplicate concurrent
  loads; explicit `memory.prehandler.model` configuration still wins.
- After deterministic completed-turn ingestion stores a new candidate, the
  memory worker may launch the selected 27B model in an isolated child over at
  most eight governed records with a bounded timeout. Its strict output can identify
  records worth reviewing together, but the trusted parent accepts only known
  IDs and creates a pending review item; it cannot directly merge memories,
  assert a relationship, or change canonical/lifecycle state. Durable claim and
  receipt events make retries idempotent and failures remain fail-open. The
  isolated worker uses up to 16 CPU threads and retries a Vulkan
  out-of-device-memory load failure once on CPU, recording that backend fallback
  in its receipt.
- Native Dreaming is a separate, explicitly enabled governed pipeline rather
  than another name for semantic consolidation. An isolated qt child receives a
  bounded replay of governed memories with tools, extensions, skills, context
  files, sessions, and recursive memory disabled. With research policy
  `required`, a distinct isolated follow-up child receives only restricted
  `web_run`, `web_search`, and `web_fetch` tools. Their shared call budget,
  queries, results, response sizes, and timeouts are bounded; custom search
  engines and direct `web_run` URLs are disabled; configured MCP tools cannot
  leak through the CLI allowlist; and `PI_OFFLINE` remains authoritative. The
  trusted parent accepts citations only when their URLs appear in successful
  opened `web_run` results or successful fetched `web_fetch` results that were
  first discovered by a search, then validates
  the structured artifact and every source ID before persisting a per-cycle
  idempotent claim/receipt pair, a pending-review `dream_artifact`, bounded
  provenance, and explicit `derived_from` edges. Dream artifacts remain outside
  trusted recall; accepting their review advances them only to a hypothesis
  under the default `review_only` promotion policy. The explicit `disabled`
  policy blocks review-driven promotion and leaves the artifact isolated. The
  prompt and llama grammar enumerate
  the same artifact kinds and minimum source cardinality enforced by the trusted
  parent. Protocol versions and unique queued-cycle IDs participate in claim
  identities. This keeps writes for one cycle idempotent without collapsing
  later manual or interval cycles that intentionally revisit an unchanged
  governed source pack.
- `/memory settings` uses searchable catalog pickers for Pre-handler and
  Dreaming models, retaining default/automatic selection. The Dreaming verifier
  picker offers only local `llama/` models and an unset fallback; it does not
  download models or change the foreground model. Escape cancels selection.
  Numeric editors show their field name and save/cancel instructions.
- Dreaming supports post-turn, candidate-threshold, interval, and explicit
  triggers through typed global memory settings and `/memory` status/control
  surfaces. Interval Dreaming has two independent gates: cycles are separated
  by at least `intervalSeconds`, and foreground input, model execution, or
  compaction must remain inactive for `idleSeconds` before a cycle may start.
  Each elapsed interval launches a distinct cycle. A new cycle samples from a
  larger governed pool, prioritizes memories used least by recent Dreaming
  receipts, and uses its random UUID to break ties and order the bounded prompt.
  Receipts retain the algorithm, seed, pool identity, and exact input IDs for
  audit/replay; explicit manual requests likewise remain distinct across calls
  and runtime restarts. This randomized exploration is a native extension: the
  qtmem implementation itself selects deterministically newest-first.
  Post-turn Dreaming honors the same idle-duration and foreground-activity gate
  before launching a Dreaming cycle for the completed turn; its trigger does not
  bypass background idleness merely because capture has finished.
  Manual follow-up research consumes evidence supplied by the caller. Required
  research uses the isolated bounded web phase described above. Both create a
  separately reviewable successor linked to the dream; neither exposes MCP,
  shell, filesystem, Ollama, or ambient parent tools. Child failures and
  malformed output are recorded
  without mutating canonical memory state. Receipts retain a safe typed failure
  reason plus output hash and length, never raw rejected output. Persistent
  memory statistics aggregate attempted, completed, failed, pending, rejected,
  timed-out, cancelled, start-failed, CPU-fallback, and review-producing runs,
  with per-trigger, per-model, and per-failure-reason counts.
- Completed-turn evidence is queued after the agent-end lifecycle event and
  processed in the background into reviewable candidates or hypotheses. Recall
  receipts persist identifiers, warning and policy hashes, token estimates,
  fallback state, and injection outcome without persisting the injected prompt.
- Completed episodes are claimed through a durable SQLite uniqueness key before
  candidate ingestion, so replay after a process restart cannot duplicate
  ingestion or recall feedback. Attribution is linked to the memory IDs
  injected for that exact turn; positive, negative, and unused outcomes are
  persisted per ID, with idempotent feedback for procedural memories.
- Raw episode evidence includes a bounded repository-state fingerprint and is
  committed before canonical candidate work. Candidate mutation stages are
  serialized per repository/branch and use their own durable canonical-content
  claims; generated memories remain reviewable candidates rather than being
  validated automatically.
- Abort, provider/tool failure, session start/shutdown and tree transitions,
  and compaction lifecycle events are persisted as non-ingested evidence, so
  resume, fork/clone, and branch flows cannot masquerade as completed turns.
- Native `qt memory`, `/memory`, built-in memory tools, and qt RPC memory
  operations expose the same injected services. Writes remain hypotheses by
  default; review, deletion, replacement, and governance mutations require
  explicit authority.
- `/memory review` is a native scope-aware Review Inbox because `reference/pi`
  has no persistent-memory review contract. It ranks and filters durable review
  records, resumes queue position from SQLite, hydrates governed subjects,
  sources, graph context, risk, and recall impact, and requires one explicit
  preview confirmation before applying a typed action. CLI, RPC, and focused
  `memory_review_list`/`show`/`preview`/`apply` tools use the same typed review
  services and JSON representation. Interactive edit-and-approve exposes only
  fields permitted by the candidate kind: general candidates may change typed
  category, target trust state, tags, confidence, and replacement linkage,
  while procedure revisions receive their narrower supported field set. The
  read-only Source Inspector shows governed provenance and related context;
  local evidence opens only after canonical path validation against the shared
  external-filesystem policy and is bounded and secret-redacted before display.
  SQLite revision compare-and-set and action keys make decisions
  concurrency-safe and idempotent; review decisions, memory/graph effects, and
  linked audit records commit atomically. Scheduled
  deferrals resurface durably, reopen and compensating actions append history
  rather than rewriting it, and constrained RPC batches reject complex,
  critical, high-risk, or unprovenanced approval combinations. Dream-artifact
  approval is exposed under the default `review_only` and `manual` promotion
  policies and performs its conservative transition to hypothesis; the explicit
  `disabled` policy withholds the approval action. Review audits include the reviewer,
  decision reason, before/after review state, evidence and replacement
  references, and every created or changed ID.
  Review backup/restore remaps embedded memory, graph, review, and action IDs;
  preview, receipt, history, audit, and exported JSON are secret-redacted.
  Review RPC failures carry stable machine-readable codes, and long batches can
  be started in the background and polled without blocking unrelated RPC
  commands. Interactive review screens replace the current review overlay on
  navigation rather than accumulating an overlay stack, so Escape moves back
  exactly one logical level and exits the Inbox from its landing screen. Review
  text prompts also consume the terminal session's already-decoded standalone
  Escape without waiting on an unpolled nested decoder. Review explanations remain
  deterministic and component-based; this workstream intentionally exposes no
  model-generated advisory summary or inference-dependent decision path.
- `/memory` now includes a searchable Settings submenu for all typed memory
  controls, including enablement, recall/capture policy, budgets, isolated
  pre-handler configuration, and evidence limits. Changes are written to the
  resolved global qt settings while preserving unrelated and unknown nested
  keys; the UI states that they apply to the next runtime because memory
  services snapshot policy during startup.
- Memory status now exposes a scoped read-only statistics snapshot across CLI,
  interactive, and RPC surfaces. It includes qtmem-compatible totals by memory
  type and lifecycle state, quality/governance indicators, durable access and
  outcome totals, conflicts, and pending reviews. Statistics are aggregated
  from authoritative tables on demand; qt does not add dashboard aggregate
  tables, startup rebuild writes, a schema migration, or another dependency.
  Review-specific statistics additionally reconcile pending-age distributions
  by kind, priority, scope, and risk; first-decision and completion-duration
  percentiles; typed outcomes and application failures; constrained-batch
  outcomes; critical/stale/provenance queue risks; and Dreaming review outcomes
  directly from durable review and audit rows.
- `/memory dashboard` is an explicit native browser control panel sourced from
  qtmem because Pi has no persistent-memory dashboard contract. It lazily binds
  an authenticated `127.0.0.1` ephemeral listener, opens or prints its tokenized
  URL, and renders Overview, searchable Memory Explorer, and Review Queue views
  from the current injected runtime's live scoped snapshots. The listener is
  reused within an interactive process and stops on exit; automatic recall and
  capture never start it. The client is qtmem's embedded ImGui/WebAssembly
  dashboard adapted through a focused compatibility layer over qt's current
  runtime. Manual writes are conservatively capped at candidate/hypothesis;
  valid promote/deprecate/reject actions use native governance and audit, while
  graph excerpts preserve UTF-8 code-point boundaries and response serialization
  replaces malformed legacy bytes instead of failing an entire dashboard view;
  malformed JSON requests receive structured client errors. The dashboard's
  validation and deletion remain in `/memory review` or the confirmed native
  command path. Unsupported qtmem-daemon administration remains unavailable.
- The memory dashboard sizes its canvas to the browser and switches between
  a desktop workspace and a compact tabbed layout. Workspace navigation stays
  accessible from the menu; layout version 5 replaces the old densely split
  default. Desktop and compact docking arrangements are saved separately and
  restored when crossing the width breakpoint. Overview cards size to their
  contents, filter grids stack at narrow
  widths, memory results prioritize wrapped content, and wide history/activity
  tables scroll within their panels. The scoped APIs and governance policies
  are unchanged; native builds consume the rebuilt, checked-in WASM bundle.
- Dashboard Dream approval/rejection uses the native typed review preview and
  explicit confirmation flow, including revision/idempotency checks. Durable
  branch/repository Dream reviews resolve through the runtime review authority
  path rather than the current session's direct lifecycle scope. Approval only
  promotes a Dream artifact to hypothesis; generic lifecycle endpoints cannot
  bypass Dream review, and validation still requires independent evidence.
- Dreaming additionally supports opt-in `memory.dreaming.promotionPolicy:
  "auto_validate"`. With `researchPolicy: "required"`, successfully fetched web
  evidence can be checked in a separate, tool-free local llama.cpp child. Set
  `verifierModel` to `llama/<model-id>`; when unset, only an already-local Dreaming
  model may be reused. There is no remote verifier or fallback to model agreement
  without source text. The default remains `review_only`; project settings cannot
  enable automatic trust above the global policy or substitute another verifier.
  Only the research successor becomes validated, with an automated actor, source
  quotes, evidence hashes, verifier model, and rationale in provenance/audit. The
  original Dream retains its review state. Missing source text, unsupported or
  inconclusive verdicts, fabricated quotes, failures, and concurrent changes leave
  the successor pending. The verifier checks up to four cited sources (4096 bytes
  each, 12288 total); manual evidence without fetched content is not auto-validated.
  This is an automated evidence assessment, not a guarantee of factual truth.
- Automatic recall now returns its deterministic empty contribution without
  launching the isolated model-backed pre-handler when there are no governed
  candidates, pinned blocks, warnings, or required context. This prevents an
  empty memory scope from cold-loading or timing out a selected local model
  before the user's actual prompt.
- Automatic recall sends only the newest bounded message projection, at most
  four query-centred candidate excerpts, and a strict selection-only JSON
  result. An explicitly named active skill is parent-required, preventing a
  compact local relevance model from dropping the skill the user named.
- Explicit offline import accepts qtmem schema-v3 SQLite databases read-only or
  qtmem/native JSON with dry-run, scope mapping, duplicate reporting, verified
  backups, and rollback. Bounded export and `qt memory prune` provide scoped
  retention without discovering or mutating project-local `.qtmem` data.
- Local repository paths are reduced to a stable repository hash and project
  basename before persistence or child projection. Evidence and child payloads
  also bound tool output, reduce images to metadata/digests, and redact common
  credentials and absolute filesystem paths. `qt memory scrub` dry-runs or,
  with explicit `--yes` authority, rewrites historical scoped data and its FTS
  entries so records created before stronger redaction can be remediated.

## 2026-05-18 qt app home

- Native `qt` now defaults its user app home to `~/.qt` instead of the
  upstream `~/.pi/agent` location. This affects user-global `settings.json`,
  `auth.json`, sessions, extensions, prompts, skills, themes, and package
  metadata derived from `agent_home`. Project-local `.pi` directories and
  explicit `QT_CODING_AGENT_DIR` / `--agent-home` overrides are unchanged.
- OpenAI-compatible chat-completions SSE parsing now treats nullable provider
  string fields as absent. This preserves OpenRouter tool-call turns where
  chunks can contain `delta.content: null` before tool call arguments.
- Interactive TUI turns now subscribe to the harness event stream while a model
  turn is running, so assistant text deltas, thinking blocks, tool calls, and
  tool results are visible in the conversation transcript before the final
  answer completes. The transcript supports embedded mutable components for the
  live path, matching the reference approach where assistant messages and tool
  execution cards update arguments, partial output, and pending/success/error
  state in place. Turn completion now commits these components to terminal
  scrollback once; it appends fallback final text only when no assistant text
  was rendered from provider events, avoiding a second copy of streamed answers.
  `scripts/interactive_final_probe.py` verifies one committed answer per turn
  across successive streamed responses and a terminal resize.
- Interactive editor submissions are persisted globally in
  `<agent_home>/history.jsonl` (normally `~/.qt/history.jsonl`) so Up/Down
  prompt recall survives new sessions, session switches, and workspace changes.
  `terminal.promptHistoryLimit` in user `settings.json` controls the retained
  entry count, defaults to 500, and disables and clears persisted history when
  set to `0`. The JSONL store keeps oldest-to-newest records, uses a stable
  advisory lock and same-directory atomic replacement on Unix for concurrent
  clients, and is mode `0600` there. Session metadata continues to carry
  `prompt_history` / upstream `promptHistory` as a migration fallback and
  compatibility field, but the global file is the live editor-history source.
- Native runtime services now expose a process-isolated `subagent` tool based on
  the stable core shared by the upstream example, `pi-subagents`, and
  `pi-agent-flow`. It discovers recursive Markdown roles from
  `<agent_home>/agents`, `.agents/`, and `.pi/agents/`; supports mutually
  exclusive single, source-ordered bounded parallel, and sequential chain
  modes; forwards role-specific model/tool/prompt/cwd settings to ephemeral JSON
  child processes; streams aggregate progress; and propagates cancellation and
  recursion limits. Project roles require explicit trusted-repository opt-in,
  child cwd cannot escape the parent workspace, and the whole delegation passes
  through the parent's mutation approval boundary. A runtime-owned persistent
  coordinator additionally supports asynchronous spawn, mailboxes, follow-up,
  wait, interruption, context forks, and reloadable child JSONL sessions.
  Completion events wake the parent in print/JSON and interactive modes and are
  exposed through typed RPC commands/events. Workers reuse logical identity and
  session state with an isolated process per turn. See
  [persistent subagents](persistent-subagents.md) for lifecycle and platform
  semantics. Fresh installs provide
  leaf-only `scout`, `planner`, `reviewer`, and `worker` roles; recursive
  user/project definitions can override them by name.
- `/subagent` is a native interactive convenience over that tool; upstream Pi
  intentionally leaves subagent UX to extensions. The menu lists user/builtin
  roles, provides details and parallel/chain prompt starters, and allows
  project-role inspection with repository-control warnings. Choosing `Use this
  agent` opens a task prompt and submits a user-initiated `subagent` tool turn
  without calling the provider. The harness records the user/call/result
  sequence, tool hooks, live updates, cancellation, and savepoint through the
  same tracked lifecycle used by model-initiated calls; interactive persistent
  sessions append all three messages to the repository.
- Native runtime services now expose `web_run`, a flat-name equivalent of
  Codex's optimized `web.run` surface. It batches up to four search queries,
  accepts recency and domain filters, assigns stable source refs, and defers
  page content to bounded `open`, `click`, and `find` commands. Results are
  marked as external/untrusted context and include direct citation URLs.
  Unsupported verticals such as finance, sports, image search, and PDF
  screenshots are intentionally absent from the schema. The existing
  `web_search` and `web_fetch` tools remain available for compatibility.
- Direct web tools now honor `PI_OFFLINE`, reject private/local/reserved network
  targets, re-check resolved addresses after every redirect, and cap HTTP wire
  bodies independently of model-output truncation. This closes the unbounded
  chunked-response and basic SSRF gaps in the previous fetch path.
- Notcurses interactive TUI startup now leaves `use_alternate_screen` disabled,
  causing the backend to set `NCOPTION_NO_ALTERNATE_SCREEN`,
  `NCOPTION_NO_CLEAR_BITMAPS`, `NCOPTION_PRESERVE_CURSOR`, and
  `NCOPTION_SCROLLING` in TUI mode so output uses the normal terminal scroll
  buffer. Interactive sessions set a DECSTBM scrolling region above the fixed
  bottom prompt/footer area, write newly rendered transcript lines into that
  region with raw terminal cursor/clear sequences, reset DECSTBM, and then
  repaint the fixed bottom region on a notcurses live plane.
- Interactive terminal startup now starts the explicit terminal lifecycle before
  notcurses initialization. This ensures the lifecycle restores the user's
  cooked shell termios state on exit, even when notcurses temporarily switches
  the terminal into raw/no-echo mode during startup.
- Interactive startup now shows a fixed ASCII `qt` banner in the transcript via
  a literal transcript path that preserves leading spaces, and suppresses the
  old update notice/changelog text when a newer CLI release is detected. Other
  startup diagnostics are still preserved as warnings.
- Interactive resource slash commands (`/extensions`, `/skills`, `/prompts`,
  and `/themes`) now pass through the shell pre-dispatch layer to the runtime
  overlay executor, preventing stale deterministic command validation from
  reporting them as unknown commands.
- `/extensions` lists the live loaded inventory (explicit `-e` paths and
  project/user-discovered extensions) alongside installed package resources,
  deduplicating canonical source paths. Non-package entries are read-only and
  show `loaded`; package entries keep their enable/disable controls unless a
  CLI/discovery path independently loads the same extension. Such overlaps show
  one read-only row rather than an ineffective disable toggle. Read-only rows
  also show a read-only keyboard hint. Explicit
  extensions remain visible with `--no-extensions`, and argument filtering
  covers extension names, descriptions, and source paths.
- The checked-in native unit-test tree and its framework have been removed.
  Native extension verification now pivots to AngelScript host bindings: the
  binding catalog lists major runtime classes and methods, and the linked
  AngelScript engine registers them as typed interfaces during extension
  compilation.
- RPC extension UI frames now follow the upstream sub-protocol shape: native
  `extension_ui_request` events are flat objects with `id`, `method`, and
  method-specific payload fields; `extension_ui_response` frames are accepted
  from clients, preserve `value` / `confirmed` / `cancelled` and future fields,
  and are routed by opaque id for extension runtime consumers.
- Runtime model loading now reads upstream-style `models.json` from the user
  app home. Custom provider/model entries and provider/model overrides are
  merged into the normal model catalog before runtime model resolution, while
  invalid custom config is reported as a diagnostic and leaves built-in models
  usable.
- Restored startup models and interactive `/model` changes now update the
  selected catalog record, provider options, model ref, context metadata, and
  agent generation cap together. The implicit generation cap follows Pi's
  `min(model maxTokens, 32000)` behavior instead of retaining limits from the
  previously selected model. OpenAI-compatible request serialization also
  clamps that cap against both the active model ceiling and the estimated
  prompt footprint, preventing an advertised completion maximum from being
  sent as an impossible prompt-plus-output request.
- Provider-scoped model ids are now treated as opaque when their provider is
  supplied separately. Persisted OpenRouter ids such as `qwen/qwen3.5-9b`
  therefore retain the `openrouter` provider across restart instead of being
  parsed as provider `qwen` and silently replaced by another authenticated
  provider. Invalid persisted pairs now fail visibly rather than changing the
  user's selection.
- Normal startup no longer performs synchronous OpenRouter model discovery. It
  merges last-known cache metadata, including stale-but-usable entries, while
  explicit model-list refresh remains network-backed. Configured memory-task
  GGUFs are selected before filesystem discovery, and saved llama command
  models are preloaded only when llama is also the active top-level provider.
- Interactive sessions now refresh the public OpenRouter model catalog on a
  background worker at startup and every five minutes. `/model refresh` forces
  an immediate refresh and reports completion without blocking terminal input.
  The live catalog replaces models from the prior discovery snapshot while
  preserving configured and extension-contributed entries; authorization is
  attached when available but is no longer required for the public endpoint.
  Explicit refresh failures are reported while the prior live catalog remains
  usable instead of presenting a cache fallback as a successful refresh.
- An open `/model` picker now receives background catalog updates immediately,
  preserving its search, scope, and selected model when that model remains
  visible. RPC model listing, selection, cycling, and thinking-level helpers
  read the latest persisted OpenRouter catalog, including stale last-known
  metadata, instead of using only built-in models. RPC reads the cache on each
  command; it does not itself initiate a network refresh.
- OpenRouter catalog normalization now uses the reference implementation's
  independent 4096-token fallbacks for missing context and completion limits.
  When OpenRouter reports a narrower top-provider context, native metadata uses
  that conservative effective window and clamps completion limits accordingly.
  Cached metadata is revalidated on load, and its global path follows
  `QT_CODING_AGENT_DIR` before falling back to `~/.qt`.
- Qtindex is now integrated in-process for model tools. The native app links
  index scanner, build metadata, compile database, file-search, code-index,
  Tree-sitter metadata, filesystem, git, serial, debug, build graph, RTOS,
  fetch, and utility payload components directly. These operations are exposed
  through a single grouped `index` router tool with tree discovery and call
  dispatch, rather than a large flat top-level tool list. The interactive slash
  surface now mirrors this grouping through `/index`, including category
  discovery, schema discovery, status/rescan shortcuts, and direct operation
  calls with JSON arguments. Interactive startup also starts a shared
  low-priority background index service so `/index` can report current scan
  state and reuse warmed metadata instead of constructing a fresh scanner for
  each call. The background service persists scan/code-index snapshots into a
  SQLite database with FTS5 content and symbol rows, reloads snapshots as
  stale-but-usable data on the background worker so large symbol/call-graph
  snapshots do not block the first interactive frame, and refreshes the
  database transactionally
- Index status now exposes Tree-sitter results by default, including whether
  parsing was attempted, parser availability, generated artifacts, file counts,
  and diagnostics. Explicit `include_tree_sitter=false` or `0` omits this block.
  The existing background round remains enabled by default; a regression probe
  exercises parsing and artifact refresh after C++ source changes.
- The MCP adapter slash surface now has a native `/mcp` entry matching the
  reference extension command pattern. `/mcp` opens an overlay submenu, and
  `/mcp status`, `/mcp tools`, `/mcp resources [server]`, `/mcp reconnect
  [server]`, and `/mcp setup` provide status, registered tool inventory,
  resource listing, discovery refresh, and config precedence/starter guidance.
  after successful scans.
- `--discover-local-models` now persists discovered GGUF models into the user
  config `settings.json` under `llama.models`. Saved models are loaded into the
  llama.cpp catalog on later runs, so users do not need to pass the discovery
  flag every time.
- When the selected startup model is a llama.cpp model, the native runtime now
  begins loading that model on a background thread during startup. The llama
  slash command also includes `/llama prompt <text>` for running an ad hoc
  prompt through the current llama model without submitting a normal agent turn.
- `/model` and `/llama select` now persist separate model choices. The
  top-level `/model` path stores `defaultProvider/defaultModel` for normal chat
  turns, while `/llama select` stores `llama.selectedModel` for llama-specific
  commands without replacing or delaying an active cloud model.
- The llama provider now keeps independent tagged model/context instances for
  `embed`, `rank`, `instruct`, and `chat`. `/llama run <tag> <text>` targets
  the requested instance without unloading the other tags; the `instruct` run
  path includes the framework's currently active tool definitions in the llama
  request and can surface JSON tool-call responses back into native tool-call
  content.
- `/llama status` now reports runtime memory details from the pinned vendored
  llama.cpp staging API wrapper (`llama-ext.h`), including model/context/compute
  allocations, host allocation, device total/free/used memory, and whether a
  reported device is GPU/iGPU memory or another backend device type. When
  `gpuLayers` is omitted, model loading also queries free memory on all usable
  GPU/iGPU devices, reserves 1 GiB per device, and chooses full or proportional
  layer offload from the GGUF size and block count. Allocation failures retry a
  bounded descending layer ladder before CPU fallback. Explicit `gpuLayers`
  remains authoritative and `/llama status` reports the attempted and selected
  layer counts.
- The native llama.cpp build now auto-enables compatible installed ggml GPU
  backends and links any generated `ggml-*` backend static libraries. Vulkan
  auto-detection includes the SPIR-V C++ headers required by ggml, and explicit
  Vulkan builds report a missing header dependency before CMake compilation.
  Builders
  can force a CPU-only build or request explicit/all GPU backends with
  `QT_LLAMA_GPU_BACKENDS`. The default `native` PlatformIO target only enables
  GPU backends with corresponding local hardware present. The `native_full`
  target builds one Linux app with CPU, CUDA, Vulkan, and HIP support included;
  runtime backend/device detection determines which acceleration path is usable
  on the target host.
- The built-in llama provider now vendors the pinned PrismML llama.cpp fork and
  supports PrismML's ternary group-128 `Q2_0` GGUF format in process, without an
  MCP server or an external inference daemon. Discovery maps
  `general.file_type=41` and `Q2_0` filenames to the native catalog, while the
  runtime loads GGML tensor type 42 through the fork's CPU, Metal, CUDA, and
  Vulkan implementations. This compatibility is specifically for the
  128-value, 34-byte block format used by current Ternary Bonsai GGUF files; it
  must not be conflated with llama.cpp `TQ2_0` or Prism's migration group-64
  `Q2_0` files.
- Local llama/GGUF catalog behavior is covered as additive native functionality:
  local entries use the distinct `llama` provider and `llama.cpp` API, preserve
  remote provider/model selections when merged, expose common model metadata,
  and report missing discovery roots as diagnostics instead of blocking startup.
- Llama generation now defaults unset sampling policy to `temperature=0.3` and
  `topP=0.9`, while preserving explicit settings from config, model metadata,
  request metadata, and request options.
- ChatGPT subscription requests through the OpenAI Codex Responses provider now
  omit the native default temperature and normalize input message ids to the
  `msg*` shape required by the live Codex endpoint. Provider HTTP errors include
  the parsed response detail, and the OpenAI Codex catalog now hides stale model
  ids that the configured ChatGPT account rejects.
- Agent tool preflight now matches the reference permission order more closely:
  inactive tools are rejected before mutation approval is queued, registry
  aliases are normalized before extension-style hooks inspect a call, hook
  denials return structured `blocked_by_extension` tool results, and queued
  write/edit/bash mutations leave files and processes untouched until the
  session-scoped approval id is accepted.
- Runtime tool activation now applies the selected tool allowlist to built-in
  and extension-registered tools before model-facing descriptors are built.
  Reference aliases such as `read`, `grep`, and `ls` are accepted for built-in
  filtering, and the resulting descriptors are sorted by public name for
  deterministic prompt/tool schemas. The explicit no-tools runtime path now
  suppresses extension-registered model-facing tools as well as built-ins, and
  extension tool descriptors are rebuilt from current manifests on runtime
  reload so removed tool contributions disappear from subsequent turns.
- AgentHarness decomposition now has explicit regression coverage for routed
  provider hook order, single-turn session/save-point recording, and
  steering/follow-up/abort queue transitions, in addition to existing
  move-safety coverage for the provider proxy binding.
- Extension `input` hooks now run in the reference prompt path before
  skill-command and prompt-template expansion continues. Subprocess handlers
  receive the raw user text, can return `continue`, `transform`, or `handled`,
  and transformed text/images are chained through later handlers before the
  interactive runtime submits the final prompt.
- Interactive session replacement now runs `session_before_switch` extension
  hooks before the active runtime/session pointer is replaced. A handler can
  cancel the replacement, preventing extension commands from observing the
  target session state.
- Interactive startup/session replacement now dispatches `session_start`
  extension hooks, and terminal teardown/session replacement dispatches
  `session_shutdown` once for the active session before resources are stopped
  or replaced.
- Provider credential resolution now follows the documented reference order
  through one shared helper: runtime `--api-key`, auth storage API key/OAuth
  token, then provider-specific environment credentials. Environment aliases
  mirror `env-api-keys.ts`, Google Vertex ADC and AWS Bedrock ambient
  credentials surface as authenticated environment sources, and missing-key
  diagnostics name the expected env var plus `/login <provider>`.
- The default native model catalog now represents every reference
  `KnownProvider` id from `reference/pi/packages/ai/src/types.ts`. Providers
  without native transport support are explicit unsupported catalog entries with
  stable diagnostics, unknown provider/model selection errors include valid
  alternatives, and native-only llama/GGUF entries remain additive.
- Provider request shaping now follows the reference provider-family contracts
  more closely. Model metadata clamps unsupported thinking levels before
  runtime, interactive, and RPC state changes; OpenAI-compatible chat
  completions downgrade `developer` messages to `system` when the selected
  model is not reasoning-capable or the provider rejects developer roles;
  Responses tools send `strict: false`, while ChatGPT Codex Responses sends
  `strict: null`; Anthropic, Google, Bedrock, and Mistral request bodies are
  covered for ordered system/developer/user prompt shaping, provider-native tool
  descriptors, thinking, cache, and max-token fields. Provider HTTP transport
  uses Boost.Beast directly so cloud model traffic does not shell out through
  curl.
- Provider response normalization now follows the reference stream contracts
  across OpenAI Responses, OpenAI-compatible chat completions, Anthropic,
  Google, Bedrock, and Mistral. Tool calls from each family are projected into
  native `ToolCallContent`, OpenAI Responses no longer emits a zero-valued
  usage event when the provider omitted usage, explicit zero usage still emits
  a usage event, OpenAI-compatible stream usage applies cache and selected-model
  cost normalization, refusals remain assistant text with refusal metadata, and
  content-filter finishes surface as structured provider errors.
- Incomplete provider streams now surface a retryable error when they end
  without a known completion signal, including after a tool result. The agent
  rejects partial tool calls before execution and uses its configured bounded
  retry policy. Explicit stream errors and aborts cannot be overwritten by a
  later completion event; unknown non-streaming completions also fail.
- Multimodal request shaping now carries provider-neutral image content into
  OpenAI-compatible, Anthropic, Google, and Mistral provider payloads, preserving
  MIME types and user ordering. Text-only OpenAI-compatible targets receive a
  clear omitted-image placeholder instead of malformed image content.
- Interactive startup now defaults reasoning-capable models to a visible
  thinking level, preferring `high` when the model supports it, unless the CLI,
  user settings, or an explicit session thinking change selects another level
  or `off`. This keeps Codex reasoning summaries visible in transcript output.
- Context handling now mirrors the reference loader more closely: global agent
  context is followed by project context files from outermost ancestor to the
  active working directory, even when those files are above the repository root.
- Interactive session replacement now cancels an active model turn before
  swapping session/runtime state. `InteractiveTurnPump::cancel_and_wait()`
  requests harness abort, waits for the active prompt future to settle, drains
  queued steering/follow-up operations, and then lets the session switch clear
  live turn UI state, preventing the previous turn from leaking into the new
  active session.
- Repeated model-backed compaction now calculates `tokensBefore` from the
  rebuilt model-visible session context, so older history replaced by a prior
  compaction is not counted again. The previous compaction entry is also kept
  out of the next serialized conversation body; its summary is supplied through
  the iterative previous-summary path and its pi-generated file-operation
  details still carry forward.
- Manual harness compaction has fixture coverage for the reference lifecycle:
  model-backed summarization appends a compaction entry, emits manual
  compaction start/end events, and rebuilds the model-visible context with the
  compaction summary plus kept messages.
- Before-compact cancellation now has fixture coverage for the reference
  invariant that cancelled compaction leaves session state unchanged: no
  provider call, no compaction entry, unchanged active leaf, unchanged encoded
  session entries, and an aborted manual compaction end event.
- Runtime host creation now applies the requested selected leaf to its owned
  `SessionManager`, not only to the seeded model context. This keeps tree
  navigation and branch summarization aligned with the actually active branch.
- Branch-summary tree navigation has fixture coverage for appending a summary
  at the target position, emitting that summary in the session-tree event, and
  preserving it after a resumed context rebuild.
- Harness auto-compaction now has threshold fixture coverage. When enabled in
  config metadata, provider usage is compared with
  `contextWindow - reserveTokens`; threshold compaction runs only after the
  agent turn end event, emits `threshold` compaction events, and stays idle
  below threshold.
- Interactive manual and threshold compaction now clear the visible native TUI
  transcript and replace it with the new compaction summary, matching the
  reference compaction-end chat rebuild behavior while preventing pre-compaction
  UI transcript entries from remaining resident.
- Native TUI transcript history now stores tool calls/results as semantic entry
  kinds when they are appended through live tool rendering, and classifies legacy
  `tool_call:`/`tool_result:` lines on load. Older oversized tool payloads and
  mutable tool components can be selectively replaced with compact summary lines
  while recent tool entries remain intact. Tool entries outside the recent-entry
  window are oversized when their retained payload or rendered/source line
  exceeds 240 characters. The per-tool allow-list is configurable through
  `terminal.toolEntryCompactionEnabledTools`; by default `read_text`,
  `read_mcp_resource`, and the grouped `index` tool are left out so source and
  code-index context remains visible unless users opt those tools in. `/settings`
  exposes a Tool compaction submenu with built-in tool switches that persists
  the same setting.
- During active model turns, the native TUI now updates the terminal title with
  OSC 0 using the current working-indicator spinner/status plus a stable
  `qt - session - cwd` suffix, and restores that base title when model
  processing becomes idle.
- RPC turn event replay now emits assistant thinking blocks as
  `thinking_delta` message updates in content order, instead of replaying only
  text blocks, so app clients that render streamed message updates can display
  model thinking text. RPC session persistence, stats, and event replay also
  fall back to the latest collected turn response when the primary response
  slot is empty.
- Skill and prompt resource discovery remains scoped to configured
  resource/repository roots, and the system prompt emits the reference
  `# Project Context`, `<available_skills>`, current-date, and
  current-working-directory sections.
- Interactive `/context` and `/context all` render full available contents:
  the assembled system prompt, loaded instruction files, skills, prompt templates,
  extension manifests, tool schemas, resolved and active session messages,
  pending messages, the last completed provider request, and the latest prepared
  memory augmentation. Named subsections filter the output without truncating
  contents. Memory augmentation is scoped to the current projection; a future
  provider request is only available after dispatch. Loaded instruction files also
  produce `InstructionLoadEvent` records and extension lifecycle routing plans
  for `qt.event.instructions_loaded`.
- The model-visible system prompt now includes a structured
  `<environment_context>` block, matching the Codex-style context-fragment
  pattern for runtime state. It exposes cwd, workspace root, shell, current
  date, timezone, network mode, tool mutation permission posture, and active
  tool count when available; `/context` renders the same environment summary.
- The interactive TUI render path now caches stable render work without
  changing visible output: diff row normalization is carried across frames,
  cursor-marker diffing no longer copies the full render buffer, transcript
  line entries cache wrapped output by width, the bottom prompt/footer region
  is rendered once per frame for identical constraints, cached bottom sizing
  avoids vector copies, markdown components cache rendered buffers by
  revision/constraints while avoiding duplicate md4c validation parses, editor
  visual wrapping is revision-cached independently of cursor position, cached
  wrap geometry is returned without vector copies, Unicode grapheme boundaries
  are cached per row for cursor movement and wrapping, editor word segments are
  cached per row, idle footer session statistics refresh only at explicit state
  changes, and the notcurses live-region plane is kept persistent. The interactive
  render handoff now moves the frame into the diff renderer and presents the
  retained current frame rather than copying the full render buffer.
  Non-scrollback notcurses rendering applies row-dirty `FrameDiff` updates
  instead of rebuilding every row operation. Live model/thinking/tool deltas
  are coalesced behind a 33 ms frame gate while structural and final events
  render immediately. Shared TUI line splitting now provides zero-copy views
  for hot wrapping paths plus a common copy wrapper for editor/markdown
  callers. The interactive session also exposes render metrics snapshots for
  render, transcript, bottom, diff, present, and row-count profiling. Generic
  fuzzy filtering now prepares query/candidate match data once per item,
  filesystem autocomplete prepares query/candidate file match text once per
  refresh/candidate, and the swapped alpha/numeric query fallback avoids regex.
  Native scrollback mode now treats terminal scrollback as the transcript owner
  for live-bottom frames, so old transcript rows are not reassembled into every
  fixed-region repaint as history grows. Trailing mutable transcript components
  stay in the repaintable live region until a stable following transcript entry
  is appended or the turn explicitly finalizes the components, so streamed assistant text/tool output is not stranded as a stale
  one-time scrollback snapshot.
- Fallback terminal input now uses adaptive nonblocking bulk reads and wakes the
  UI through a condition variable instead of polling in a busy loop. Small
  fragments are coalesced into bounded batches, and `StdinBuffer` advances a
  cursor through accumulated bytes rather than repeatedly erasing the string
  front. The upstream 10 ms incomplete-sequence deadline and emitted input
  ordering remain unchanged.
- Ctrl+C now treats non-empty prompt text as an unconditional clear operation
  and resets the empty-editor double-press exit window. Overlay/autocomplete
  cancellation, empty-editor double-Ctrl+C exit, Escape interruption, and
  external SIGINT shutdown retain their existing precedence. POSIX raw mode
  disables `ISIG`, matching event-driven terminal input so a physical Ctrl+C
  reaches this editor path on terminals without CSI-u support.
- Slash command dispatch now follows the reference prompt expansion order for
  native command resolution. Extension commands are registered and matched
  before built-ins, duplicate extension command names receive suffixed
  invocation names while dispatch still targets the original extension command,
  built-ins remain authoritative over prompt and skill expansions, extension
  command arguments preserve the parsed text exactly after the command
  separator, interactive extension slash commands execute subprocess JSON-RPC
  command contributions through the shared contribution runner with those raw
  arguments as the command `args` payload, prompt templates expand before
  assistant submission, prompt template placeholders match the reference
  handling for positional, all-argument, missing-argument, and no-placeholder
  cases, and failed command expansion returns an error instead of starting a
  model turn.
- Active model turns no longer impose a generic block on built-in slash
  commands. The interactive runtime keeps an explicit unavailable list for
  `/compact`, `/context`, `/reload`, and mutating `/llama` operations; other
  built-ins can run immediately, while session replacement paths retain their
  existing cancel-and-join behavior.
- Built-in tool descriptors now expose the reference tool names `read`,
  `write`, `edit`, `bash`, `grep`, `find`, and `ls` to providers while keeping
  native implementation names internal. The registry prepares model requests by
  resolving reference aliases to implementation tools before invocation, with
  reference-facing schemas for `write`, `grep`, and `ls` and deterministic
  validation errors through the shared `ToolResultStatus` categories.
- Tool-call preparation now follows Pi's prepare, validate, hook, execute order
  in both the generic agent and local interactive llama paths. Active
  model-facing JSON schemas validate and coerce nested arguments before side
  effects; failures return received arguments, the expected schema, and an
  explicit retry signal. Public read/write/edit/grep schemas expose canonical
  required fields while legacy aliases are normalized before validation.
- Native code navigation now exposes a direct typed symbol tool backed by the
  existing C/C++ project index. Active-tool prompt guidance and built-in
  subagents prefer it for identifier and definition lookup and reserve grep
  for literal, regex, and unsupported-language searches. The generic index
  router also publishes typed operation schemas, validates forwarded
  arguments, and repairs the observed nested category/operation call shape.
- Native index navigation additionally exposes four bounded task-oriented
  tools: `analyze_file` for indexed file structure and call edges, `references`
  for approximate C++ callers/callees, `build_context` for exact-file compiler
  and include configuration, and `search_index` for persisted full-text
  discovery. They share index-readiness and retry behavior, publish snapshot
  freshness metadata, guard large outputs, and use strict schemas with
  coercible bounds. Plain full-text queries are safely translated to FTS5;
  raw FTS5 syntax must be explicitly selected. These are native enhancements
  because the Pi parity source has no built-in project-index contract.
- `search_text` now accepts the grep/search parameters found across references:
  primary `pattern`/`path`/`glob`/`ignoreCase`/`literal`/`context`/`limit`,
  Claude-style `output_mode`, `head_limit`, `offset`, `-A`, `-B`, `-C`, `-n`,
  `-i`, and `multiline`, plus Hermes' `files_only` alias. Native output modes
  cover `content`, `files_with_matches`, and per-file `count` results.
- Built-in tool parity now also covers secondary reference aliases and behavior:
  `read_text` accepts `file_path`/`filePath`, suggests similar paths, lists
  directories as a fallback, and guards repeated unchanged reads; `write_text`
  accepts `file_path`/`filePath`; `edit` accepts `file_path`/`filePath`,
  `old_string`/`new_string`, `oldString`/`newString`, and `replace_all`; `bash`
  accepts `workdir`/`cwd`/`working_dir`, `description`, and `timeout_ms`;
  `search_text` accepts `file_glob`/`include`, `type`, hidden/no-ignore flags,
  `literal_text`, and direct default current-directory searches; `find` accepts
  `glob`/`include`, `type`, hidden/no-ignore flags, and directory matching;
  `list_directory` supports recursive/depth listings and ignore patterns;
  `clipboard` can execute the selected platform copy command; and `apply_patch`
  validates a Codex-style patch envelope before applying changes and accepts
  `patchText`.
  Model-facing `read`, `write`, and `edit` descriptors advertise one
  canonical required path spelling; the registry converts supported historical
  aliases before the shared runtime validation path.
  `apply_patch` also rejects invalid syntax before touching files, reports
  ambiguous or missing hunk context with deterministic diagnostics, queues
  mutation approval before writes, and reports changed files without including
  partial file contents.
  Recursive `search_text` uses `rg --files` for candidate discovery when
  available, preserving ripgrep hidden, ignore, and VCS filtering semantics,
  with the native walker retained as a fallback.
- Direct MCP/web tools now cover the remaining secondary-reference surfaces:
  `list_mcp_resources` aggregates resource metadata by server,
  `read_mcp_resource` reads a server/URI pair from runtime-provided MCP resource
  metadata, both MCP resource tools accept the `mcp_name` server alias,
  `web_fetch` exposes bounded HTTP/HTTPS content fetching, and
  `web_search` wraps a public HTML search endpoint into structured result
  links.
- `AgentHarness` provider interception now uses a shared binding state that is
  rebound during move construction and move assignment. Provider request hooks
  keep routing through the destination harness after moves instead of retaining
  a pointer to moved-from state.
- `AgentHarness` now rebuilds its native `Agent` only when model, thinking, or
  active-tool configuration changes. Filtered tools and harness hook callables
  are cached between turns, while required rebuilds preserve transcript and
  queue state and moved harnesses rebind callbacks. Queue event ordering and
  per-message enqueue visibility are unchanged.
- Bash execution now follows the reference streaming contract more closely:
  the shared bash tool emits start, throttled accumulated-output, and final
  updates; large outputs include truncation metadata plus `fullOutputPath`;
  timeout, cancellation, exit code, and signal termination are separated; and
  interactive/RPC shell execution routes through the same backend.
- Agent tool execution now preserves conversation invariants when queueing or
  cancellation interrupts tool batches: sequential steering stops later tool
  invocations and records interrupted tool results for the remaining calls,
  parallel aborts finalize the batch with aborted results, and late tool
  updates are suppressed once abort is requested. Parallel tool execution now
  also preserves provider tool-call order for committed results and uses the
  descriptor `destructive` flag as a sequential execution constraint.

## 2026-05-17 cpp_guide cleanup

- Footer rendering now reads the cached git branch through a `const`
  `ReferenceFooterDataProvider` API instead of using `const_cast`. The rendered
  footer contract is unchanged.
- The edit tool now models detected line-ending style with a small enum instead
  of a boolean flag. CRLF-preservation behavior is unchanged.
- Process waiting now returns a structured internal result containing both exit
  code and timeout state instead of using a `bool&` output parameter. Public
  `ProcessResult` behavior is unchanged.
- `ToolsManager` ensure calls now accept a named `ToolEnsureOptions` struct
  instead of a positional boolean. Tool discovery behavior is unchanged.
- Selector widget `select_list()` accessors now return non-owning `const Select&`
  references instead of exposing `shared_ptr` ownership details. Widget behavior
  is unchanged.
- Agent retry defaults are now named constants, and `Agent` rejects a missing
  provider at construction so the required dependency invariant is established
  before turns run.
- `ToolRegistry` now owns registered tools with `unique_ptr` and returns
  non-owning `Tool*` views from lookup. Tool aliases and invocation behavior are
  unchanged.
- Reference extension loading now uses a named options struct for discovered
  extension loading instead of a positional `no_extensions` boolean.
- POSIX `execvp` argument adaptation is isolated behind small helpers, keeping
  the required legacy `char*` cast at the process boundary.
- UUID entropy fallback now catches standard random-device failures explicitly.
- `ProviderRegistry` and `BeastHttpTransport` now use exclusive ownership for
  dependencies that are not shared.
- Compaction synthetic session entries now initialize upstream metadata
  explicitly.
- Agent harness phase cleanup now uses RAII instead of cleanup-only catch blocks,
  preserving idle-state restoration during exceptions.
- CLI CSV parsing now uses a named empty-field policy instead of a positional
  boolean flag.
- Search result path formatting now uses a named search scope instead of a
  positional recursive flag.
- Agent turn failures now translate standard exceptions to turn-failed events
  without swallowing non-standard exceptions.
- `LlamaProvider` now owns its backend adapter exclusively.
- Runtime metadata boolean lookups now use a named default policy instead of a
  positional boolean.
- Package-manager mutation records now use a named condition policy instead of
  positional conditional booleans.
- OpenAI-family providers now own their HTTP transport exclusively; provider
  request behavior and endpoint selection are unchanged.
- EXIF metadata parsing now passes TIFF byte order as a named enum instead of a
  positional endian boolean. Parsed orientation behavior is unchanged.
- Read-text truncation metadata now uses a named first-line limit state instead
  of a positional boolean. Emitted JSON fields are unchanged.
- Package-manager write helpers now return a structured write outcome instead
  of reporting changes through mutable `bool&` output parameters.
- Theme color parsing and ANSI rendering now use named policy enums instead of
  positional booleans. Required token validation and escape output are unchanged.
- Reference runtime provider construction now returns provider selection metadata
  as a structured value instead of mutating a `bool&` output parameter.
- Conversation compaction serialization now keeps separator state inside a
  formatter object instead of passing mutable `bool&` state between helpers.
- Install-method detection now accepts a named Bun runtime context instead of
  positional Bun booleans.
- Repository context discovery now uses a named root kind when adding skill or
  prompt roots instead of positional booleans.
- App resource filtering now uses named resource-kind and match-mode enums
  instead of positional skill/exact-match booleans.
- Keybinding hint formatting now uses a named capitalization policy instead of
  a positional boolean.
- A PlatformIO `clang-tidy` target now writes actionable static-analysis
  findings for owned native sources. Initial report-driven fixes remove an eager
  optional dereference, clarify optional sweep filtering, and widen arithmetic
  before size/time conversions. The first provider-transport pass also removes
  analyzer-reported dead stores, avoidable temporary strings, empty catch blocks,
  and move-blocking `const` locals without changing request/response semantics.
- Session JSONL storage now writes reference-compatible persistent session
  headers for new sessions and keeps implemented tree entries in the upstream
  schema. Reference-created sessions load implemented entry types, keep unknown
  future upstream entries as ordered raw records, and preserve metadata fields
  such as cwd, parent session, prompt history, labels, model state, and thinking
  level.
- Startup session lookup now treats partial session IDs as selectable only when
  they are unambiguous in the selected scope. Current-working-directory matches
  keep priority, but ambiguous prefixes now produce a deterministic diagnostic
  instead of picking a session by path or mtime.
- Session cloning now follows the reference `/clone` behavior by writing only
  the active branch path into the new session file, instead of copying every
  branch stored in the source session JSONL.
- Session forking now has regression coverage for the reference selected-message
  boundary: the forked file contains history before the selected user message,
  while the selected prompt remains available to seed replacement input.
- Extension provider registrations are now folded into the runtime model catalog
  before model resolution, matching the reference behavior where providers
  registered during extension startup are available to model selection and
  listing immediately.
- Extension tool registrations now surface in model-facing tool descriptors,
  and subprocess JSON-RPC extension tools execute through the shared process
  runner. Unsupported transports return a deterministic `not_implemented` tool
  result instead of an ambiguous unregistered-tool failure.
- Extension host API compatibility is now explicit for reference-module and
  AngelScript extension loading. Pending host/UI APIs emit stable
  `not_implemented` diagnostics with source locations instead of being silently
  ignored during discovery.
- Skill slash command expansion follows the documented `/skill:name` behavior:
  standard `SKILL.md` frontmatter is stripped, command arguments are appended as
  `User: <args>`, source paths are retained for command metadata, invalid skill
  files do not block unrelated valid skills, and duplicate names keep the first
  discovered skill.
- Tool-call lifecycle hooks now support both reference-style denial and argument
  transformation before tool side effects. Native agent hooks can rewrite the
  prepared tool request, and extension `tool_call` routing results can return a
  replacement `input` object.
- Session stats now aggregate the same reference-facing message counts, tool
  counts, token totals, cost totals, context usage, and per-model usage buckets
  from persisted JSONL. Native snake_case provider usage fields reload into the
  same totals, and assistant messages with missing usage are surfaced as
  unknown rather than being treated as zero-cost turns.
- HTML session export now follows the reference visible transcript shape more
  closely: paired tool results are folded into their assistant tool-call blocks,
  standalone tool-result records and hidden custom messages are omitted, unsafe
  content is escaped, and the standalone document includes session id, model,
  message count, title, source path, and export-time metadata.
- JSON-mode output now filters internal provider diagnostics from rendered
  stdout, preserves reference lifecycle/message/tool event ordering for simple
  and tool-call turns, emits camelCase retry event fields, and puts native
  failure metadata under the additive `qt` namespace.
- Session cwd handling now has explicit coverage across startup/session-dir
  resolution, runtime tool contexts, fork/clone metadata, and HTML export
  metadata. Relative tool operations receive the active session cwd through the
  runtime tool context rather than depending on a later process-global cwd.
- Runtime diagnostics now flow through the pinned `spdlog` dependency behind the
  native logging facade. Logging stays opt-in via `--log-file`, `--log-stderr`,
  `--log-level`, or matching `QT_LOG_*` environment variables so reference-style
  JSON/RPC stdout remains clean.
- Beast-backed provider, MCP, index fetch, and OAuth HTTP readers now set the
  response parser body limit explicitly so large chunked responses follow the
  reference fetch behavior instead of failing at Boost.Beast's small default
  parser cap.
- Provider requests now carry the reference-style `retry.provider.timeoutMs`,
  `maxRetries`, and `maxRetryDelayMs` settings into native `ProviderOptions`.
  Active agent aborts also propagate a shared abort signal into Beast-backed
  provider HTTP requests so an interrupted or stalled model read can unwind and
  clear interactive Working state. Stream parser error events populate
  `errorMessage` as well as `error`, preserving retry/error diagnostics.
- External MCP server configuration now follows the Pi MCP adapter discovery
  order for standard files: `~/.config/mcp/mcp.json`, `<agent_home>/mcp.json`,
  `<workspace>/.mcp.json`, then `<workspace>/.pi/mcp.json`. Native runtime
  settings also accept `mcpServers`/`mcp_servers` objects. Stdio MCP servers can
  be discovered and registered as runtime tools through a manager-owned
  background event loop with filtered subprocess environments and redacted error
  messages.
  Streamable HTTP configurations use Boost.Beast JSON-RPC over HTTP and parse
  either JSON or event-stream responses while retaining MCP session ids. Legacy
  SSE configurations keep a Beast/Asio event-stream reader, resolve endpoint
  events, and POST JSON-RPC messages back through Beast. OAuth flows and
  metadata caching remain pending parity work. Stdio and SSE MCP
  `sampling/createMessage` requests are bridged into the selected runtime
  provider, including text, image, tool-use, and tool-result message conversion
  plus MCP text/tool-use result conversion.
- Built-in Ollama support extends the `pi-ollama` single-endpoint behavior to
  multiple named machines. Each saved instance becomes a distinct
  `ollama:<id>` OpenAI-compatible provider, while `/ollama` exposes native
  discovery, status, running-model, info, pull, load/unload, configuration, and
  guarded deletion operations. Last-known discovery metadata is persisted in
  global qt settings so unreachable servers never block startup; `/model` and
  `--list-models` include enabled cached Ollama models.
- `/memory` adds a native skill-folder import workflow beyond Pi's runtime skill
  loading surface. Its folder navigator offers current-folder or recursive
  scans, respects the external-filesystem access boundary, and returns control
  immediately while a background worker reports per-skill and final results in
  the transcript. Traversal follows Pi's `SKILL.md` package boundaries (without
  applying Pi's frontmatter validation or ignore-file semantics); each readable
  package becomes a governed active skill memory with bounded related text,
  redacted local provenance, idempotent hashes, serialized concurrent imports,
  and supersession on source changes.
- Native memory recall responses expose score components (lexical, graph, dense,
  and reciprocal-rank fusion), graph paths and direction, eligibility, historical
  current canonical or captured historical revision, source provenance, and warnings. RPC recall and dashboard memory
  entries also expose UTF-8 byte counts with a clearly labelled approximate
  input-token estimate; exact model token accounting is unavailable at this
  surface. The dashboard summary reports live retrieval worker, job, chunk,
  cache, and embedding-error status; it does not infer code attribution for
  memory-only recall results.
- Retrieval maintenance is an explicit, authorized operation: the memory RPC
  `retrieval_rebuild` action and dashboard `POST /dashboard/retrieval/rebuild`
  require `confirm: true`, accept an optional exact owner key, and return
  invalidated chunks/vectors plus superseded and newly queued jobs. Requests
  without confirmation fail before touching storage; the dashboard exposes the
  same scoped report as the RPC surface.
- Native llama embedding requests now default to a 250 ms pending-queue
  deadline. Pending cancellation and timeout remove the queued callback under
  the worker's dequeue lock; a worker-side deadline check also rejects expired
  requests before model loading. Retrieval retains its existing lexical/graph
  fallback on these errors. Active embedding calls still join their backend
  operation and use cooperative cancellation; the deadline does not interrupt
  model loading or active device work. Negative deadlines are rejected, and a
  zero deadline expires immediately. Other model operations retain their
  existing queue behavior.
  Explicit embedding cancellation additionally reaches real model-load progress
  checkpoints and is checked after context creation. Cancelled loads do not
  retry GPU/CPU offload plans or publish a partial loaded model, and their borrowed
  cancellation callback is excluded from retained load settings. Cancellation
  remains cooperative; this adds no hard deadline to metadata/context allocation
  or uninterruptible device operations.
- Memory recall and final context authority now inspect the complete unresolved
  critical-conflict queue, including the canonical scope of global memories.
  SQLite streams a single consistent read snapshot and deduplicates affected
  memory IDs. UI review pagination no longer determines whether a memory may
  enter guidance or serve as a graph bridge. Lookup failures remain fail-closed;
  ordinary conflict warnings and explicit lifecycle decisions retain their
  existing meanings.
- Review statistics are reported for the queue the Review Inbox lists: the
  exact turn scope merged with its canonical durable scope. `memory status`
  previously read the turn scope alone, so its `review_statistics` block
  reported zero pending items while `memory review list` listed the durable
  queue. The merge now lives in one shared implementation used by the CLI,
  interactive, dashboard, and RPC surfaces, which previously could drift apart.
  Stored review rows, scope keys, queue ordering, and lifecycle authority are
  unchanged; only the reported aggregate is corrected.

## Public bundle hygiene

Dashboard builds normalize Emscripten preload package identities to `index.data`
instead of embedding the build machine's absolute output path. Asset URLs and
the `/static` virtual filesystem mapping are unchanged.
