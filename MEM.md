# Built-in qt Memory Integration Plan

## Objective

Integrate the governed memory behavior from the sibling `qtmem` repository into the
native qt executable as direct C++17 functionality.

The completed feature must:

- run in the qt process/runtime and never require an MCP server, REST endpoint,
  listening port, hook script, or separately managed `qtmem` daemon;
- persist every durable memory artifact below qt's resolved global config
  directory (`AppSettings::agent_home`, normally `~/.qt` and overridable with
  `QT_CODING_AGENT_DIR`);
- run automatic memory preprocessing in an isolated qt sub-agent with its own
  model context before adding memory context to the main model request;
- inject recalled memory transiently into the main model input without adding
  duplicate developer messages to the session transcript;
- preserve qtmem's scope, provenance, lifecycle, warnings, review, procedure,
  graph, audit, and conservative candidate-creation behavior;
- use only dependencies already pinned in `platformio.ini` and documented in
  `docs/dependencies.md`.

This file is the feature plan and checklist. A checkbox is marked complete only
after its code, edge cases, probes, and documentation meet the definition of
done below.

## Sources and Contract

- Memory behavioral source: sibling repository `qtmem`, inspected at commit
  `3324ace` on 2026-07-17.
- General agent/session lifecycle source: `reference/pi`.
- Secondary implementation references: only the entries approved by
  `REFS.md`, especially the Codex config loader and Claude Code isolated-agent
  subsystem.
- Native owners: `src/**` and `include/qt/**` in this repository.

`reference/pi` does not provide built-in persistent memory or built-in
sub-agents. Its relevant contract is the awaited pre-agent lifecycle, isolated
in-memory/no-session agent behavior, turn-scoped context injection, fail-open
hook behavior, and use of one resolved global agent/config directory. Memory
semantics therefore come from `qtmem`; pi informs where and how they attach to
the qt turn lifecycle.

Track active work in `PLAN.md` and the memory requirements below. Integration
touches isolated subagents and model tools; session evidence and compaction;
context assembly and lifecycle ordering; CLI/RPC/slash commands; and interactive
status and diagnostics. Record verified behavior in the compatibility notes and
test map rather than maintaining duplicate workstream plans.

## Locked Architecture Decisions

### Built-in, not MCP

Memory services are constructor-injected C++ services owned by
`RuntimeServices`. Built-in memory tools and commands call those services
directly. Do not copy or register qtmem's MCP, general HTTP/REST service, or
hook transport layers, and do not reserve a fixed port or read `QTMEM_ENDPOINT`
during normal operation. The user-started browser dashboard is the narrow
exception: it owns a process-local, authenticated loopback listener on an
ephemeral port and adapts directly to the current injected `MemoryRuntime`.

### Global persistence root

Derive the memory root only after `load_app_settings()` has resolved and
canonicalized `AppSettings::agent_home`:

```text
<agent_home>/memory/
  memory.sqlite3
  blocks/
  exports/
```

With defaults this is `~/.qt/memory`. Project settings may enable memory and
tune policies, but may not redirect the database into a repository. The
existing `QT_CODING_AGENT_DIR` override redirects the complete qt global config
root, including memory.

### Isolated preprocessing sub-agent

Automatic preprocessing uses a child qt process through a reusable isolated
agent runner extracted from `SubagentTool`. A child process gives the memory
pre-handler a distinct context window and prevents the main transcript,
extensions, skills, tools, and session repository from leaking into it.

The child is launched with the equivalent of:

```text
qt --mode json --no-session --no-extensions --no-context-files
   --no-skills --no-tools --no-memory
```

It receives a framed JSON request on stdin, not command-line arguments or a
temporary prompt file. It inherits the resolved agent home and cwd, receives a
fixed memory-preflight system prompt, and returns one strict JSON result. Set a
`QT_MEMORY_PREHANDLER=1` sentinel as defense in depth; `--no-memory` is the
primary recursion guard.

The child may use the selected main model by default. Settings may select a
different configured model and thinking level. It never receives general qt
tools. Deterministic native services retrieve and scope candidate memories
before launch; the child only selects, warns, and compiles those candidates
into a bounded context contribution.

### Actual model-input seam

Do not implement automatic memory solely as a `BeforeAgentStartHook`. That
hook misses some queued steer/follow-up paths and its returned messages remain
in the in-memory transcript.

Attach memory at the provider-side model-context transformation in
`AgentHarnessProviderProxy`, after normal context hooks have produced the model
request and before provider request/payload hooks. This is the common path for
interactive, print, JSON, RPC, steer, follow-up, retries, and tool-loop model
calls.

Give each logical user input an explicit harness sequence key. Message IDs are
insufficient because normal and RPC user messages may not have one. Run the
pre-handler once for each new logical input key, cache its result for retries
and subsequent tool-loop calls, and run it again when steer or follow-up adds a
new user input. Reset tracking when the session/host changes, a branch or
compaction rebuilds the transcript, or model-input truncation invalidates the
tracked projection. Do not preprocess restored historical user messages on
startup.

Inject the validated result as a named developer message immediately before
the applicable user input on every provider call for that logical turn. Keep it
out of `Agent::state().transcript`, session JSONL, compaction input, exports,
and prompt history. Persist only a receipt containing memory IDs, warnings,
policy/version hashes, token counts, and outcome attribution.

### Conservative writes and rollout

Recall happens before the current prompt is recorded as durable evidence, so a
prompt cannot recall itself. Raw turn evidence is recorded after preprocessing;
candidate extraction and canonical writes happen after the turn. Agent-created
claims default to `candidate` or `hypothesis`, never `validated`.

The feature is compiled in and management commands are available, but
`memory.enabled` defaults to `false` for the first release because enabling it
changes privacy, latency, storage, and model-token behavior relative to pi.
`--no-memory` is an unconditional per-invocation override and is also used by
all isolated memory children. Changing the default requires measured rollout
evidence and an explicit compatibility update.

### Dependency boundary

The baseline recall path uses SQLite FTS5, lexical ranking, and qtmem's
deterministic hashed-token similarity. It must not describe the latter as neural
embeddings. When compatible GGUF files are present in the repository or global
model directory, qt resolves task-specific built-in llama.cpp models without
adding a dependency or using Ollama/curl. Neural embedding models are classified
separately from generative models and are never loaded in generation mode.

## End-to-End Flow

```text
expanded/queued user input
        |
        v
AgentHarness builds the real provider model request
        |
        v
MemoryCoordinator derives strict user/repo/branch/session/task scope
        |
        v
read-only FTS/governed recall builds bounded candidate pack
        |
        v
IsolatedAgentRunner starts qt memory pre-handler
  - fresh context and no session
  - no extensions, skills, project context, or tools
  - prompt/model-input projection + candidate pack on stdin
        |
        v
validate MemoryContextContribution
  - allowed IDs and scope only
  - lifecycle and warning preservation
  - token/size budget
        |
        v
transient developer context is inserted into main provider request
        |
        v
main model/tool loop completes
        |
        v
raw evidence + recall receipt + outcomes are stored
        |
        v
post-turn extraction/dedup creates reviewable candidates/hypotheses
```

If retrieval or the child agent fails, the main prompt continues. Use a
validated deterministic context pack as the fallback when candidates are
available; otherwise inject nothing. Cancellation and timeout stop the child
and return control to the main turn. Diagnostics go to qt logging/TUI status,
never structured stdout.

## Source Import Boundary

Copy code only after recording the source commit and verifying same-owner reuse
authorization, because qtmem currently has no root LICENSE/SPDX file.

| qtmem source | Native destination/handling |
| --- | --- |
| `include/qtmem/memory_store.hpp` | Split live DTOs into `include/qt/memory/models.hpp` and focused repository interfaces. Do not retain a god interface. |
| `include/qtmem/storage/repositories.hpp` | Adapt the focused memory, source, review, audit, graph, procedure, block, and state roles under `qt::memory`. |
| `src/storage/sqlite_memory_store.cpp` | Port storage and schema logic into `src/memory/storage/**`, then add multi-process safety and ordered migrations before enablement. |
| `src/storage/memory_store.cpp` | Reuse normalization, serialization, deduplication, graph relation, and JSON import/export helpers; exclude the JSON backend from authoritative storage. |
| `include/qtmem/core/{ids,memory_types,provenance,scope}.hpp` | Reconcile with live DTO values and use as the single typed model. Do not copy a second unused model hierarchy. |
| `include/qtmem/governance/lifecycle_policy.hpp` and focused service headers | Port lifecycle, memory, source, graph, review, audit, procedure, state, and governance behavior; move large inline implementations into `.cpp` files. |
| `include/qtmem/services/memory_processing_service.hpp` | Port deterministic transcript parsing and candidate extraction into a compiled `MemoryIngestionService`. |
| `include/qtmem/services/post_processing_service.hpp` | Port relevance, association, compression, and post-processing selectively; replace external AI callbacks with the injected qt isolated-agent runner. |
| `src/service/service_recall.cpp`, `service_common.cpp`, `service_detail.hpp` | Extract governed recall, score components, budget allocation, pinned blocks, warnings, duplicate suppression, evidence links, and markdown rendering into typed transport-neutral services. Do not copy general service/Ollama coupling. |
| `dashboard_wasm/**`, `src/ui/dashboard.cpp`, `doc/15_dashboard_experience.md` | Migrate qtmem's ImGui/WebAssembly dashboard and adapt its read models to the injected native runtime. Keep generated WASM deterministic and checked in so ordinary native builds do not require Emscripten; do not import qtmem's general REST service or storage owner. |
| qtmem storage, service, governance, and context tests/goldens | Convert behavioral cases into native probes and AngelScript coverage. Preserve scope, ranking, lifecycle, budget, restart, migration, and future-schema cases. |

Explicitly exclude:

- `src/service/service_http.cpp` and `src/service/service_mcp.cpp`;
- `src/app/**`, standalone `cli.hpp`, `service.hpp`, and qtmem `main.cpp`;
- qtmem's general Boost networking service and dashboard-owned memory store;
  the dashboard's pinned ImGui/SDL WASM source, generated bundle, and fonts are
  the explicit UI-only exception;
- Codex hook and import Python scripts as runtime components;
- `integrations/ollama.cpp`, the qtmem logging facade, external curl calls, and
  remote AI server configuration;
- the whole-snapshot JSON store as a live global backend. It may be retained in
  an offline import/export adapter only.

## Native Module Layout

Use focused, constructor-injected modules:

```text
include/qt/memory/
  models.hpp
  scope.hpp
  repositories.hpp
  database.hpp
  memory_service.hpp
  governance_service.hpp
  recall_service.hpp
  context_service.hpp
  ingestion_service.hpp
  post_processing_service.hpp
  coordinator.hpp
  prehandler_protocol.hpp

src/memory/
  storage/
  memory_service.cpp
  governance_service.cpp
  recall_service.cpp
  context_service.cpp
  ingestion_service.cpp
  post_processing_service.cpp
  coordinator.cpp
  prehandler_protocol.cpp

include/qt/app/runtime/isolated_agent_runner.hpp
src/app/runtime/isolated_agent_runner.cpp
```

`MemoryCoordinator` owns orchestration, not persistence policy. It receives
focused repositories/services, a scope resolver, clock, token estimator,
isolated agent runner, logging sink, and cancellation source through its
constructor. `RuntimeServices` owns the long-lived shared coordinator and
database handles.

## Data and Safety Contract

### Database and filesystem

Port the qtmem schema version 3 data, including memories, tags, graph edges,
procedure runs, revisions, sources/events, review items, audit events, session
events, and FTS5. Dashboard aggregate tables are not required by the built-in
runtime.

Before using a global database:

- add an ordered migration table and one transaction per schema migration;
- enable WAL, foreign keys, and a bounded `busy_timeout`;
- use bounded retry with jitter for `SQLITE_BUSY`/`SQLITE_LOCKED`;
- make recall queries read-only; update access/outcome telemetry in a short
  deferred write transaction rather than `BEGIN IMMEDIATE` during recall;
- remove startup aggregate rebuild writes;
- use RAII statements/transactions and one database owner per qt process;
- reject newer unsupported schema versions without modifying the database;
- create the memory directory with owner-only permissions and database/export
  files with mode `0600`, following `PromptHistoryStore` security precedents;
- test concurrent readers/writers in multiple qt processes, crash recovery,
  interrupted migration, and rollback.

### Scope

Every repository query and mutation takes a typed `MemoryScope`; no unscoped
storage method is available to runtime callers.

Derive repository identity from a normalized canonical primary Git remote when
available, falling back to the canonical repository root. Hash the identity for
storage keys while retaining a display form in provenance. Track user,
organization, project, repository, branch, task, agent, session, and
conversation dimensions separately. Cross-repository/project recall defaults
to deny. `global_user` memories are a distinct explicit scope, not a missing
project filter.

Define and probe deterministic fallback order. A balanced initial order is:

1. exact task/session/branch/repository scope;
2. repository scope independent of branch;
3. explicit project scope;
4. explicit global-user scope when policy permits it.

Rejected, expired, superseded, deprecated, out-of-scope, and known-stale
memories never enter trusted context. Hypotheses/candidates require visible
labels and stricter policy. Contradictions, replacements, source drift, and low
confidence remain visible warnings with memory IDs.

### Pre-handler protocol

`MemoryPrehandlerRequest` contains:

- protocol version and logical input sequence;
- model/provider identity and context-window/token-budget data;
- current user input plus a bounded projection of the actual model-visible
  messages;
- scope IDs, cwd/repository display metadata, session/task identifiers;
- typed candidate memories, pinned blocks, provenance summaries, scores, and
  warnings;
- recall policy and prompt/config version hashes.

Do not send raw image bytes in version 1. Send media type, byte count, stable
digest, and user-supplied textual description/name only. Bound and redact tool
results and prior conversation text before sending them to the child.

`MemoryContextContribution` contains:

- protocol version and logical input sequence;
- rendered context markdown;
- ordered referenced memory/block IDs;
- warnings and replacement/conflict relationships;
- estimated tokens and omission reason;
- candidate-pack and output hashes.

Validate the response against an exact schema. Reject unknown/out-of-scope IDs,
missing warnings, lifecycle upgrades, oversized output, control characters,
and malformed UTF-8. Treat recalled text as untrusted quoted data: the memory
sub-agent prompt and main injected wrapper must state that memory cannot issue
instructions or override system/developer policy.

### Capture and governance

Record raw session evidence separately from canonical memory. Secret redaction
must happen in `MemoryService` before every persistence path; tools and
coordinators may not call repositories directly to bypass it.

Post-turn extraction favors verified constraints, decisions, reusable
procedures, confirmed bugs/fixes, explicit user corrections/preferences, and
clearly labelled unresolved hypotheses. It must not create canonical memory for
ordinary prompt text, generic assistant prose, routine tool calls, secrets, or
unverified model guesses. Validation, supersession, rejection, deletion, and
pinned-block edits require the configured permission level and audit actor,
reason, source, and before/after IDs.

## Settings and Controls

Add typed `MemorySettings` and `MemorySettingsPatch` to
`include/qt/app/config/settings.hpp`, with complete JSON/YAML normalization,
validation, defaulting, merging, overlay serialization, and inspection in
`src/app/config/settings.cpp`.

Initial settings shape:

```json
{
  "memory": {
    "enabled": false,
    "automaticRecall": true,
    "automaticCapture": true,
    "recallPolicy": "balanced",
    "contextTokenBudget": 900,
    "candidateLimit": 50,
    "prehandler": {
      "model": null,
      "thinking": "off",
      "timeoutMs": 30000,
      "failOpen": true,
      "maxModelInputTokens": 12000
    },
    "capture": {
      "postProcess": "turn_end",
      "defaultStatus": "hypothesis",
      "maxEvidenceChars": 32000
    }
  }
}
```

Storage paths are deliberately absent and derived from `agent_home`.
Project-level settings may reduce/disable behavior but may not broaden scope or
permissions beyond global policy. Add `--no-memory` to every execution mode and
propagate it as `RuntimeServicesRequest::include_memory = false`.

## Implementation Workstreams

### MEM-00 - Freeze contracts and fixtures

- [x] Record qtmem source commit, copied-file provenance, and reuse
  authorization in this plan or a dedicated provenance document.
- [x] Extract versioned golden fixtures for store round trips, FTS recall,
  governed context packs, warnings, lifecycle transitions, review/audit/session
  events, and schema rejection.
- [x] Link memory requirements from `PLAN.md` without marking unfinished
  implementation work complete.
- [x] Define versioned pre-handler request/result JSON schemas and prompt text.

Exit: behavioral inputs/outputs are frozen independently of qtmem's HTTP/MCP
envelopes.

### MEM-01 - Port the typed domain and global SQLite storage

- [x] Add the single typed memory/scope/provenance/lifecycle model under
  `qt::memory`; reconcile every live qtmem string value before deleting the
  parallel stringly model.
- [x] Add focused repository interfaces and a composed SQLite implementation.
- [x] Port data, tags, graph, procedure, source, revision, review, audit,
  session-event, FTS, import, and export behavior.
- [x] Add real ordered migrations, WAL, foreign keys, busy handling, deferred
  access telemetry, permissions, and crash-safe transactions.
- [x] Derive `<agent_home>/memory` only from resolved settings and prove
  `QT_CODING_AGENT_DIR` relocation.
- [x] Add strict scope predicates to every query and mutation.

Exit: two or more concurrent native processes can safely recall/write isolated
repository scopes in the same global database, restart cleanly, and preserve
the frozen store fixtures.

### MEM-02 - Port transport-neutral memory services

- [x] Port memory normalization/type inference/secret redaction and prevent
  direct store bypass.
- [x] Port lifecycle governance, revisions, supersession, conflicts, staleness,
  review, audit, procedure feedback, graph relations, and state import/export.
- [x] Extract governed recall with explainable component scores, FTS,
  deterministic hashed similarity, duplicate suppression, lifecycle filtering,
  and scope fallback.
- [x] Extract typed context-pack allocation/rendering for pinned blocks,
  relevant memories, procedures, recent episodes, evidence links, and warnings.
- [x] Port deterministic transcript chunking, candidate extraction,
  deduplication, association, and compression into compiled `.cpp` services.
- [x] Keep Dreaming output isolated as reviewable `dream_artifact`/candidate
  data and outside trusted recall.

Exit: native service calls reproduce relevant qtmem goldens without network,
MCP, Ollama, or dashboard code.

### MEM-03 - Wire typed settings and runtime ownership

- [x] Implement `MemorySettings` parsing, validation, defaults, merge order,
  persistence, and inspection.
- [x] Add `--no-memory` to CLI parsing/help and every runtime construction path.
- [x] Add the memory database/services/coordinator to `RuntimeServices` using
  constructor injection and one long-lived owner per qt process.
- [x] Refactor the separate RPC/demo host construction onto the shared runtime
  service path or attach the identical coordinator explicitly.
- [x] Resolve the existing system-prompt delivery gap independently; memory
  injection must target the actual provider request and not assume the resource
  bundle prompt is already connected.

Exit: interactive, print, JSON, and RPC modes resolve identical settings,
global storage, scope, and coordinator ownership.

### MEM-04 - Build the isolated pre-handler and transient injection

- [x] Extract `IsolatedAgentRunner` from `SubagentTool` process launch, NDJSON
  parsing, cancellation, timeout, final-output extraction, and diagnostics.
- [x] Keep `SubagentTool` behavior unchanged while moving it onto the shared
  runner.
- [x] Implement stdin-framed memory pre-handler mode with fixed isolation flags,
  no tools/resources/session, selected model, clean stdout, and recursion guards.
- [x] Add explicit logical input sequence metadata to harness model-context
  events; do not rely on optional message IDs or content hashes alone.
- [x] Chain ordinary context hooks correctly, then run memory augmentation on
  the resulting actual provider request.
- [x] Detect each new initial/queued/steer/follow-up user input, run once, cache
  across retries/tool calls, and reset on session/branch/compaction/input rewrite.
- [x] Validate the contribution schema, scope, IDs, lifecycle, warnings,
  UTF-8, and token budget before injection.
- [x] Inject only transient, explicitly untrusted user-role reference context
  and store only a recall receipt. This keeps memory below policy privilege and
  works with model templates that ignore the developer role.
- [x] Implement deterministic fallback, fail-open diagnostics, timeout, and
  cancellation without delaying structured-output cleanup.
- [x] Bypass model-backed child launch when recall produced no governed
  candidates, pinned blocks, warnings, or required context; record the
  deterministic empty contribution without cold-loading the selected model.

Exit: a process/PID and transcript probe proves the pre-handler has its own
context and no inherited tools/resources/session; every qt prompt path receives
the same transient memory behavior without recursion or session pollution.

### MEM-05 - Capture completed episodes and learn conservatively

- [x] Capture bounded user, assistant, tool-call/result, build/test, abort,
  correction, model, token, and recall-receipt evidence after recall.
- [x] Store raw evidence/session events before generating canonical candidates,
  preserving provenance and repository state hashes.
- [x] Run deterministic post-turn processing without blocking the next user
  input; serialize per-project mutation stages where required.
- [x] Deduplicate and create conservative candidates/hypotheses; never validate
  model-generated claims automatically.
- [x] Record procedure outcomes and positive/negative/unused attribution for
  recalled memory IDs.
- [x] Handle turn abort, provider failure, tool failure, session shutdown,
  resume, fork/clone, branch, and compaction without double ingestion.

Exit: the current prompt cannot recall itself, routine activity does not cause
unbounded canonical growth, and later sessions recall only governed post-turn
memory.

### MEM-06 - Add native management and model-facing surfaces

- [x] Add native `qt memory` commands for status, recall/search, remember,
  review, conflicts, stale, health, feedback, forget, export, and import.
- [x] Add an interactive `/memory` family using the same services, with clear
  enabled/disabled, database, scope, warning, and pre-handler status.
- [x] Add a `/memory` Settings submenu covering every typed memory option,
  persist changes in global qt settings without discarding unknown keys, and
  clearly identify that the current runtime keeps its startup snapshot.
- [x] Add native RPC commands where remote qt clients need the same management
  operations; these are qt RPC operations, not MCP registrations.
- [x] Add focused built-in tools such as `memory_recall`, `memory_remember`,
  `memory_feedback`, and `memory_governance`, directly backed by injected
  services.
- [x] Apply existing mutation/approval policy: read is automatic, write defaults
  to hypothesis, governance and deletion require explicit authority.
- [x] Ensure tool filters, `--no-tools`, `--no-memory`, child isolation, and
  structured stdout work consistently.

Exit: users and the main agent can use the governed memory API without a
network transport, and permission-denied operations leave no partial writes.

### MEM-07 - Migration and operational hardening

- [x] Add explicit offline import for qtmem SQLite schema v3 and qtmem JSON
  exports, with dry-run, backup, scope mapping, duplicate report, and rollback.
- [x] Never auto-discover or mutate a sibling `.qtmem` directory at startup.
- [x] Add bounded retention/export/deletion behavior for raw evidence,
  canonical memory, audit records, blocks, and user-requested forgetting.
- [x] Add database health/integrity diagnostics and actionable recovery output
  that stays off protocol stdout.
- [x] Verify secrets, absolute paths, image metadata, and tool output are
  redacted/bounded in database, logs, receipts, exports, and child requests.

Exit: existing qtmem users can deliberately migrate with a reversible command,
and normal qt startup never depends on or damages external qtmem state.

### MEM-08 - Verification, documentation, and rollout gate

- [x] Add `scripts/memory_probe.cpp` and `scripts/memory_probe.py` for compiled
  runtime/storage/sub-agent scenarios.
- [x] Bind the probe in `test/test_angelscript/main.cpp` and add
  `test/test_angelscript/scripts/memory_probe.as`.
- [x] Cover global path overrides, file modes, schema migrations/future-schema
  rejection, restart, multi-process contention, crash recovery, FTS/ranking,
  scope isolation, lifecycle, context budgets, warnings, secret redaction,
  recall-before-write, and conservative capture.
- [x] Cover isolated child recursion, inherited-resource denial, strict output,
  timeout, cancellation, fail-open/fallback, one-run-per-logical-input caching,
  steer/follow-up, retry/tool loops, branch/compaction resets, and transient-only
  injection.
- [x] Cover interactive, print, JSON, RPC, `--no-memory`, management commands,
  built-in tools, permission denial, and structured stdout cleanliness.
- [x] Add memory cases to `docs/reference-test-map.md` and document the exact
  upstream divergence in `docs/upstream-compatibility.md`.
- [x] Update `docs/dependencies.md` to state that SQLite also backs built-in
  memory; add no dependency.
- [x] Run `pio run -e native`, the targeted memory/subagent probes, and the full
  AngelScript framework checks. Run `pio run -e native_full` before release.
- [x] Measure task quality, retrieval precision, injected/total tokens, latency,
  correction rate, and canonical growth with memory disabled and enabled before
  considering a default-on change.

Exit: all checks pass, compatibility/docs are current, and the rollout report
shows no scope leakage, trusted stale/deprecated use, session pollution, or
structured-output regression.

### MEM-09 - Background skill-folder ingestion

- [x] Add a workspace-policy-aware folder navigation submenu under `/memory`
  with current-folder and recursive scan choices.
- [x] Add deterministic Pi-compatible package-boundary traversal for `SKILL.md`
  with skill-root stopping semantics and bounded related-package text capture.
- [x] Persist one active `MemoryType::Skill` per discovered package with redacted
  local provenance, stable source/hash metadata, repository scope, secret redaction,
  and deterministic ordering.
- [x] Make rescans idempotent, supersede changed source versions, isolate
  per-skill failures, skip symlink/cache/binary content, and enforce file/package
  bounds.
- [x] Run scans in the background and marshal per-skill plus final progress into
  the transcript without blocking prompt input or model work.
- [x] Probe non-recursive/recursive discovery, nested and hidden-directory skill
  boundaries, related text/binary/hidden-file handling, containment and symlink
  rejection, concurrent duplicate rescans, changed-source supersession, and active
  lifecycle state; drive the menu, background transcript, and external-boundary
  behavior through a PTY; update compatibility/test maps.

Exit: selecting a folder returns immediately and every discovered readable
skill is durably queryable as governed skill memory with bounded package context.

### MEM-10 - Task-specific built-in llama.cpp memory workers

- [x] Discover configured and repository/global GGUF files at runtime without
  requiring ignored model artifacts to be committed.
- [x] Classify embedding-only and generative GGUFs from filename and metadata,
  including the Nomic context-length metadata key, without treating generic
  metadata containing the word `embedding` as an embedding capability.
- [x] Resolve deterministic task choices: Nomic for document embeddings, BGE
  for short text embeddings, SmolLM2 for worthiness gates, Qwen 0.5B for
  extraction/rewrites, and Qwen 3B for latency-sensitive relevance selection;
  prefer the appropriate available quantization.
- [x] Route automatic recall through the selected Qwen 3B GGUF in an isolated
  llama.cpp child, with an explicit user model override taking precedence.
- [x] Bound the newest model-input projection, governed candidate count, and
  query-centred candidate excerpts before launching the worker.
- [x] Make the model decide relevance IDs only; normalize protocol hashes,
  sequence, warnings, required evidence, and rendered context in the trusted
  parent before validation and injection.
- [x] Require an explicitly named active skill memory independently of the
  model's ranking so a smaller relevance model cannot discard the referenced
  skill, while retaining model selection for other candidates.
- [x] Share the runtime llama provider with child runtime construction and
  enable embeddings only for embedding requests, preventing duplicate GGUF
  loads and generation/embedding context conflicts.
- [x] Select complete configured task-model coverage before scanning GGUF roots,
  so normal startup does not reopen and reparse every persisted model.
- [x] Record selected task/model, bounded/original candidate counts, input
  budget, fallback state, and redacted rejection diagnostics in recall receipts.

Exit: a real stored-skill query records `isolated_agent: true`,
`deterministic_fallback: false`, selects the named skill ID through the local
Qwen worker, and injects a bounded parent-rendered contribution without an
Ollama pre-handler process.

### MEM-11 - Scoped memory statistics

- [x] Port qtmem's dashboard-statistics contract as a focused typed repository
  API: total memories, counts by type/status, hypotheses, low-confidence and
  unprovenanced entries, deprecated entries, failed procedures, heavily reused
  hypotheses, and conflict state.
- [x] Add native usage/governance totals for accessed memories, total accesses,
  successful/failed outcomes, and pending reviews from existing durable fields.
- [x] Compute statistics as a read-only live SQLite snapshot over authoritative
  scoped tables; do not add dashboard aggregate tables, startup rebuild writes,
  a schema migration, or another dependency.
- [x] Expose one stable nested statistics contract from `qt memory status
  --json` and RPC memory status while retaining `scope_memory_count` as a
  compatibility alias.
- [x] Render readable catalog, usage, and governance statistics in `/memory
  status`; report statistics as unavailable when the memory runtime is inactive.
- [x] Probe zero and populated snapshots, type/status aggregates, access and
  outcome counters, conflicts, pending reviews, scope isolation, restart
  persistence, CLI/RPC JSON, and interactive transcript rendering.

Exit: all status surfaces report the same scoped authoritative statistics, and
repeated status reads neither mutate the database nor initialize disabled
memory runtimes.

### MEM-12 - High-capacity semantic consolidation review

- [x] Classify Ternary Bonsai 27B Q2_0 as a high-capacity generative memory
  worker and read Qwen3.5 GGUF context-length metadata during discovery.
- [x] Select Ternary Bonsai 27B deterministically for context compilation while
  retaining Qwen 3B for synchronous recall; fall back to Qwen 3B when the 27B
  model is unavailable and preserve explicit recall-model overrides.
- [x] Run the 27B worker only after a completed turn produces new durable
  candidates, outside the canonical mutation lock, with at most eight governed
  records, a 6,000-token model-input ceiling, and a bounded ten-minute
  isolated-child timeout.
- [x] Reuse the strict selection-only protocol and grammar. Treat selected IDs
  as untrusted evidence and create only a pending semantic-consolidation review;
  never let the model directly merge records or alter lifecycle, graph, or
  canonical memory state.
- [x] Persist idempotent claim and receipt events containing the selected model,
  bounded pack hash, fallback/error state, selected IDs, and review result.
- [x] Raise isolated CPU worker threads to a hardware-bounded maximum of 16 and
  retry a Vulkan out-of-device-memory failure once on CPU, recording the backend
  fallback in the receipt.
- [x] Automatically choose full or partial GPU layer offload from available
  device memory and GGUF size/block metadata, with explicit layer overrides,
  bounded smaller-offload retries, and per-model CPU fallback when model or
  context allocation cannot use the GPU.
- [x] Probe deterministic per-task routing and fallback/tie behavior, isolated
  child flags and environment, parent ID validation, review-only persistence,
  native compilation, and real Ternary Bonsai 27B constrained generation.

Exit: ordinary recall remains responsive on Qwen 3B, while newly learned
related or conflicting records are asynchronously grouped by Ternary Bonsai
27B into governed pending reviews with an auditable receipt and no automatic
truth mutation.

### MEM-13 - Native governed Dreaming

- [x] Add a distinct Dreaming pipeline that replays a bounded governed memory
  set and produces structured hypotheses, pattern discoveries, conflict
  suspicions, procedure revisions, or candidate memories; do not treat
  selection-only semantic consolidation as Dreaming.
- [x] Run initial Dream synthesis through the injected isolated qt child with no
  tools, extensions, skills, context files, session, or recursive memory, and
  validate every source ID and artifact field in the trusted parent. When the
  explicit research policy requires follow-up, use a separate isolated child
  allowlisted to restricted read-only `web_run`, `web_search`, and `web_fetch`;
  validate cited URLs against successful opened or fetched tool results before
  creating a reviewable successor.
- [x] Persist per-cycle idempotent Dreaming claims and receipts,
  `dream_artifact` memories, pending reviews, explicit `derived_from` edges,
  bounded provenance, and no direct trusted-recall or canonical-state mutation.
  Version the protocol in claim identities so a corrected contract can safely
  retry an older rejected candidate pack, and persist safe typed rejection
  diagnostics plus durable run statistics without retaining raw model output.
- [x] Select each bounded Dreaming input from a larger governed pool using
  least-recently-used participation with cycle-seeded randomized tie-breaking
  and prompt ordering. Persist the selection algorithm, cycle seed, pool hash,
  pool count, and exact input IDs so new cycles explore different material while
  each individual selection remains auditable and reproducible.
- [x] Support post-turn, candidate-threshold, interval, and explicit Dreaming
  triggers with bounded input/output, model selection, failure isolation, and
  observable runtime status. Gate interval and post-turn runs on a configurable
  idle duration and never run them during foreground input, model work, or
  compaction.
- [x] Store manual supplied evidence and required isolated web research as
  separate reviewable successors linked to the dream; never accept model
  self-validation or a citation absent from successful web tool evidence.
- [x] Apply approved dream reviews conservatively by promoting the artifact only
  to a hypothesis. Keep research successors and all stronger lifecycle changes
  behind their own review/governance decisions.
- [x] Add opt-in local evidence-backed auto-validation of research successors,
  preserving review-only defaults and original Dream artifacts; record verifier,
  evidence and rationale, and fail closed on missing evidence or stale subjects.
- [x] Expose typed global settings and `/memory` status/settings controls for
  Dreaming. Core synthesis has no network dependency; required web research is
  explicit, bounded across all three web tools, honors `PI_OFFLINE`, and cannot access MCP, shell,
  filesystem, Ollama, extensions, sessions, skills, or recursive memory.
- [x] Probe generation, repeated manual and interval cycles, randomized source
  diversity, selection audit metadata, source validation, scope isolation, trusted-recall
  exclusion, review application, research successors, idempotency, receipts,
  restart persistence, trigger bounds, interval and post-turn idle gating,
  durable run statistics, and typed malformed/failed child output.

Exit: qt can run bounded exploratory memory analysis as native built-in
functionality while every generated claim remains auditable, reviewable, and
outside trusted recall until an explicit governed transition.

### MEM-14 - Governed memory Review Inbox

- [x] Replace raw review-list interaction with a scope-aware, resumable
  `/memory review` Inbox providing ranked queues, category/status/priority/tag
  filters, latest-Dream and deferred/history entry points, hydrated detail,
  evidence inspection, keyboard triage, preview confirmation, audit notes, and
  concise transcript receipts.
- [x] Add versioned typed review candidates, hydrated review/risk/recall views,
  typed actions, mutation previews, application receipts, batch contracts, and
  one shared JSON representation for CLI, RPC, tools, and runtime callers while
  preserving diagnostic decoding of legacy candidate payloads.
- [x] Migrate SQLite review state with revisions, scheduled deferral,
  application status/error, idempotency keys, durable action history, and
  resumable view state. Guard decisions with pending-state/revision
  compare-and-set and commit review, memory, graph, and audit effects in one
  transaction.
- [x] Implement governed handlers for approval/edit, reject, defer/resurface,
  reopen, relate, merge, supersede, contradiction/warning, scope narrowing,
  revalidation, deprecation, evidence attachment, and compensating correction;
  reject unsupported/no-op approvals before persistence.
- [x] Add `qt memory review` list/show/next/preview/decide/defer/reopen/history
  operations, typed RPC review and constrained batch operations, and focused
  indexed list/show/preview/apply tools with explicit mutation authority.
- [x] Derive durable queue/outcome/age/risk/Dreaming-review statistics and expose
  them through memory status JSON/RPC and the interactive Inbox.
- [x] Add focused schema/lifecycle fixtures and a compiled review probe covering
  every supported kind, malformed legacy and missing/cross-scope subjects,
  scheduled deferral, reopen/history, concurrent decisions, idempotent replay,
  atomic rollback, statistics, and stable JSON contract examples.
- [x] Finish the `REVIEW.md` hardening: complete kind-specific
  conflict/staleness workflows and interactive filtering, and prove
  backup/restore, security-surface redaction, narrow-terminal, focus, resize,
  and all-surface parity.

Exit: every exposed review outcome previews and commits a concrete governed
effect through the shared service, with conflict/staleness workflows,
interactive filtering, backup/restore, redaction, terminal behavior, and
cross-surface parity covered by the completed `REVIEW.md` verification matrix
and dedicated review probes.

### MEM-15 - Local browser dashboard

- [x] Migrate qtmem's ImGui/WebAssembly engineering dashboard, its embedded
  bundle, and read-model contracts for Overview, Memory Explorer, graph,
  sources, agents/sessions, context, and system status.
- [x] Add a lazy RAII server bound only to `127.0.0.1` on an ephemeral port,
  protect the shell and JSON snapshot with a per-process token, apply restrictive
  browser security headers, and stop/join the listener with interactive mode.
- [x] Resolve the live `MemoryRuntime` on every refresh so session/runtime
  replacement cannot leave the dashboard attached to stale services or reopen
  the SQLite database.
- [x] Adapt qtmem's write/process and safe lifecycle controls conservatively:
  dashboard writes cannot enter a trusted state, governance transitions retain
  native audit policy, and validation/deletion stay behind evidence or explicit
  native confirmation rather than inheriting qtmem's weaker buttons.
- [x] Add `dashboard` to `/memory` menu selection, direct slash dispatch, help,
  and autocomplete; reuse an already-running listener and always print the URL
  when automatic browser launch is unavailable.
- [x] Probe menu discovery, authenticated shell and API access, scoped empty
  snapshots, idempotent startup, and listener teardown on interactive exit.

Exit: `/memory dashboard` opens an inspectable, scoped control panel without a
qtmem daemon, fixed port, or second memory database owner. Ordinary native
builds consume the checked-in WASM bundle; Emscripten is optional and needed
only when rebuilding dashboard assets.

## Definition of Done

The integration is complete only when all of the following are true:

- no automatic memory path creates an MCP client/server, hook process, HTTP
  listener, or dependency on the standalone qtmem service; only the explicit
  `/memory dashboard` action may create its authenticated loopback listener;
- all durable data lives below the resolved global qt config directory and is
  safe under concurrent qt processes;
- strict scope tests show zero cross-project/repository leakage;
- preprocessing demonstrably runs as an isolated sub-agent with its own context
  and cannot recurse;
- every logical main-model input path receives at most one pre-handler run, with
  cached reuse for retries/tool calls and correct reruns for new queued input;
- injected memory is transient, bounded, provenance-bearing, warning-preserving,
  and absent from session transcript/export/compaction content;
- current prompts are recalled before capture, and post-turn learning produces
  conservative reviewable memory rather than automatic truth;
- native CLI, slash, RPC, and model-tool operations share the same injected
  services and permission policy;
- migration is explicit, dry-runnable, backed up, and reversible;
- production builds, targeted probes, AngelScript coverage, compatibility
  documentation, and dependency documentation all pass and are updated.
