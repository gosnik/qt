# Native Memory Review Implementation Plan

## Purpose

Implement a complete, user-friendly, governed memory-review workflow for native
qt. The primary interaction becomes a resumable Review Inbox rather than raw
JSON, copied identifiers, and typed confirmation tokens. Every displayed action
must preview and perform a concrete, auditable effect while preserving memory
scope, trust, provenance, and recall safety.

This plan covers interactive TUI, CLI, RPC, model-facing tools, durable storage,
governance application, statistics, migration, documentation, and verification.
It does not add MCP or a new dependency.

## Sources and Ownership

- Primary parity source: `reference/pi`. It has no built-in persistent-memory
  review contract, so existing slash-menu, selector, overlay, keyboard,
  confirmation, RPC, and structured-output behavior remains the UI/transport
  baseline.
- Approved secondary index: `REFS.md`, especially the native approval-overlay
  patterns in `reference/codex` and TUI behavior in `reference/hermes-agent`.
- Memory behavior source: sibling `qtmem`, particularly:
  - `doc/16_governance_and_review.md`
  - `doc/15_dashboard_experience.md`
  - `doc/03_trust_lifecycle.md`
  - `doc/09_conflict_management.md`
- Native owners:
  - Review domain and persistence: `include/qt/memory/**`, `src/memory/**`
  - Interactive workflow: `src/app/interactive/interactive_mode.cpp`,
    `include/qt/tui/**`, and `src/tui/**`
  - CLI: `src/app/cli/memory_command.cpp`
  - RPC: `src/app/rpc/rpc_mode.cpp`, `src/app/rpc/rpc_protocol.cpp`
  - Tool surface: `src/tools/memory_tool.cpp`, `src/tools/registry.cpp`
  - Settings: `include/qt/app/config/settings.hpp`,
    `src/app/config/settings.cpp`
  - Probes: `scripts/**`, `test/test_angelscript/**`
- State tracking: update `MEM.md`, `PLAN.md`,
  `docs/reference-test-map.md`, and `docs/upstream-compatibility.md` atomically as
  implementation components reach their Definition of Done.

## Current Baseline and Gaps

The governed Review Inbox implementation now covers the workflow described by
this plan. `/memory review` provides a resumable, scope-aware interactive
Inbox with hydrated detail, evidence inspection, filtering, kind-specific
actions, conflict/staleness handling, batch operations, and durable history.
CLI, RPC, model-facing tools, and the TUI share typed review services and
preview/apply semantics. SQLite compare-and-set, idempotency keys, atomic
effects, audit records, redaction, backup/restore, restart migration, and
durable statistics are covered by the focused review probes and verification
matrix below.

The source and probe records are the completion evidence for this plan:
`scripts/memory_review_probe.py`, `scripts/memory_review_ui_probe.py`,
`scripts/memory_review_rpc_contract_probe.py`, and the review coverage in
`scripts/memory_probe.cpp`. Future review changes should add a new unchecked
contract or probe case rather than preserving the superseded pre-Inbox gap
list here.

Reported review statistics now merge the exact turn scope with its canonical
durable scope through one shared implementation used by the CLI, interactive,
dashboard, and RPC surfaces. `memory status` previously read the turn scope
alone and reported zero pending items while `memory review list` listed the
durable queue. `scripts/memory_review_probe.py` now computes its
`review_statistics_reconciled` result from a real turn-scope-to-durable-scope
comparison instead of reporting a fixed value, and `pio run` plus the review,
RPC-contract, and Dreaming probes pass with the change.

## Product Contract

### Review Inbox

- [x] `/memory review` opens a scope-aware, resumable Review Inbox; the normal
  workflow never requires copying a review ID.
- [x] The landing view shows total pending, urgent/high-priority counts,
  category counts, deferred items due, latest Dreaming results, and history.
- [x] Primary entry points are `Review next`, `Browse by category`,
  `Review latest Dream`, `Deferred`, and `History`.
- [x] Queue position and filters survive opening details, cancellation,
  completed decisions, overlay close/reopen, and process restart.
- [x] The empty state explains that no review is required and links back to
  memory status rather than displaying an empty JSON array.

### Ordering and Filtering

- [x] Rank by explicit priority and durable risk/impact signals: conflict
  severity, critical memory type, low trust, staleness, provenance quality,
  recall/use frequency, recent context inclusion, procedure failures, and age.
- [x] Preserve deterministic ordering with stable review ID tie-breaking.
- [x] Support filters for status, kind, priority, scope, source type, tags,
  Dreaming cycle, stale/conflict flags, and created/decided age.
- [x] Display active filters and provide a single action to clear them.

### Review Card and Detail

- [x] Each queue card shows position, kind, priority, concise proposal, reason,
  trust/confidence, scope, age, and important warning badges.
- [x] Detail hydrates the current subject memory, proposed content, relevant
  source excerpts, related memories/edges, provenance, evidence strength,
  source/model/hash, usage/outcome history, stale/conflict warnings, and tags.
- [x] Detail states whether the item currently participates in strict,
  balanced, or exploratory recall.
- [x] Detail includes a kind-specific `What accepting will do` preview listing
  exact memory, lifecycle, graph, scope, and recall mutations—and an explicit
  `Will not` section for validation or promotion that will not occur.
- [x] Missing/deleted subjects, stale source hashes, invalid candidate payloads,
  and cross-scope references appear as blocking diagnostics rather than generic
  failures.

### Primary Interaction

- [x] Provide outcome-oriented actions: `Approve as hypothesis/active`,
  `Edit & approve`, `Reject`, `Defer/Snooze`, `Inspect sources`, `Next`, and
  `Back`; do not use a context-free `Accepted` label.
- [x] Provide keyboard triage with discoverable bindings (`A`, `E`, `R`, `D`,
  `S`, arrows or `j/k`, Enter, Escape) without conflicting with terminal input.
- [x] A single confirmation sheet shows the exact mutation, scope, warnings,
  optional note, and authority boundary. It must not be followed by a second
  generic mutation confirmation for the same explicit action.
- [x] After a decision, show a concise receipt and advance to the next item
  while preserving the selected category and filter.
- [x] Cancellation performs no review or memory mutation.

### Edit Before Approval

- [x] Allow editing only fields supported by the review kind: content, type,
  tags, confidence, target status, scope narrowing, and replacement links.
- [x] Start from a typed candidate—not raw `candidate_json`—and validate inline.
- [x] Show a final preview of the exact stored memory and recall implications.
- [x] Store the previewed value atomically with the review decision and audit
  receipt; partial edit/application is prohibited.
- [x] Cancel returns to the same item and queue position with no durable change.

### Deferred, History, Reopen, and Undo

- [x] Defer accepts a preset or explicit resurface time and optional note.
- [x] Deferred items remain browsable and automatically return to pending when
  due; deferral never changes the subject memory.
- [x] History lists accepted, rejected, deferred, reopened, failed, and
  compensated actions with filters and linked receipts.
- [x] Reopening is an explicit human action with reason and audit record; it
  cannot silently overwrite a terminal decision.
- [x] Undo is implemented only as a new typed compensating governance action.
  Audit history is never deleted or rewritten.

### Notifications

- [x] Background capture, consolidation, Dreaming, conflict detection, and
  staleness scans report `N new review items` in the transcript with an action
  that opens the relevant filtered queue.
- [x] Notifications do not interrupt active model work or prompt entry.
- [x] Memory status shows pending/urgent/due counts and oldest-item age.

## Kind-Specific Review Semantics

Every supported kind must implement `preview`, `validate`, and `apply`. A review
must not expose an approval action until its promised effect is defined.

### Dream Artifact

- [x] Show the proposed dream content and governed source memories, not only
  model/cycle metadata.
- [x] Default approval promotes `dream_artifact` to `hypothesis` under the
  default `review_only` policy; only the explicit `disabled` promotion policy
  blocks review-driven promotion.
- [x] Approval never means validated truth and preserves Dreaming provenance,
  source links, low evidence strength, and review history.
- [x] `Edit & approve` stores the edited hypothesis while retaining lineage to
  the original artifact and governed source set.
- [x] Research-required artifacts clearly offer `Attach evidence` and show
  linked research successors separately.

### Dream Research Successor

- [x] Present the original dream beside supplied research evidence and source
  provenance.
- [x] Validate that evidence is independent and source-backed; model
  self-assertion cannot count as external validation.
- [x] Approval applies only the configured, previewed lifecycle transition to
  the research memory and never retroactively validates the dream itself.

### Semantic Consolidation

- [x] Present selected memories side by side with scope, trust, provenance,
  conflicts, and usage.
- [x] Offer typed outcomes: `Relate`, `Merge canonical memory`, `Supersede`,
  `Mark contradiction`, `No relationship`, and `Defer`.
- [x] Merge previews canonical content and every source/replacement edge.
- [x] Acceptance cannot be a status-only no-op; the selected outcome must be
  applied atomically or reported as failed.

### Compression Candidate

- [x] Show both originals and the proposed compressed canonical memory.
- [x] Allow editing the canonical content before approval.
- [x] Apply canonical creation, source links, and supersession/deprecation
  transitions as one transaction.

### Conflict

- [x] Show old/new or conflicting memories and their evidence side by side.
- [x] Classify conflict severity and explain current recall risk.
- [x] Offer `New supersedes old`, `Old remains current`, `Narrow scopes`,
  `Merge`, `Keep both with warning`, and `Not a conflict`.
- [x] Critical unresolved conflicts must not enter hard-guidance recall unless
  an explicit policy permits it.

### Procedure Revision and Failed Procedure

- [x] Show current and proposed steps, success/failure history, failure modes,
  confidence, provenance, and supersession chain.
- [x] Approval updates procedure lifecycle and replacement links exactly as
  previewed; validation requires evidence or explicit user confirmation.

### General Candidate, Staleness, and Provenance Repair

- [x] Candidate review supports approve to a permitted trust state, edit,
  reject, defer, attach evidence, and narrow scope.
- [x] Staleness means `needs review`, not `false`; actions include revalidate,
  update, deprecate, supersede, and retain with a future review date.
- [x] Unprovenanced active-memory review supports attaching evidence,
  downgrading trust, narrowing scope, or rejecting the record.

## Governance and Safety Invariants

- [x] Agent and Dreaming output stays isolated from trusted recall until an
  explicit governed transition.
- [x] Approval/promotion is distinct from validation.
- [x] Constraints, decisions, procedures, user preferences, and
  security-sensitive facts require evidence or explicit human confirmation for
  validation.
- [x] Exact project/repository/branch/session/task scope is always visible and
  is preserved unless explicitly edited and confirmed.
- [x] Suggestions and advisory model summaries never mutate memory.
- [x] Rejected memories never participate in current recall; reopening them
  requires explicit human authority.
- [x] Supersession links old and replacement memories. Superseded records remain
  auditable but stop guiding current recall.
- [x] Deletion/forget remains a separate destructive action with explicit
  confirmation.
- [x] Batch validation of critical memory types is prohibited.
- [x] Batch approval is limited to homogeneous, low-risk, source-backed items
  and requires one aggregate mutation preview.
- [x] Every apply operation is atomic and idempotent. Per-item batch failures
  are reported without falsely marking unapplied work complete.
- [x] Audit records identify reviewer, time, before/after state, decision,
  reason, evidence, replacement, originating review, and created/changed IDs.

## Architecture

### Typed Domain Contracts

- [x] Replace raw candidate handling at the application boundary with a typed
  `ReviewCandidate` variant covering each supported review kind.
- [x] Add `ReviewQuery` for status/kind/priority/scope/tag/time filters,
  deterministic ordering, pagination, and due-deferred behavior.
- [x] Add `HydratedReview` containing subject, sources, related memories,
  provenance, risk, recall impact, and typed candidate data.
- [x] Add `ReviewAction` variants with kind-specific payloads instead of a
  generic status string.
- [x] Add `ReviewMutationPreview` with before/after records, graph/lifecycle
  effects, warnings, required authority, and an idempotency token.
- [x] Add `ReviewApplicationResult` that distinguishes decision persistence,
  domain mutations, no-op, partial batch failure, and compensation.
- [x] Keep focused interfaces for query, preview, and application; do not grow
  the repository interfaces into an unbounded review god-interface.

### Review Service

- [x] Introduce a constructor-injected review application service owning
  hydration, policy checks, preview generation, typed validation, and atomic
  application.
- [x] Implement one handler per review kind through composition.
- [x] Make all CLI, RPC, interactive, and tool operations call this service.
- [x] Remove duplicated CLI-only Dreaming promotion behavior after parity is
  verified.
- [x] Return `applied=true` only after the advertised domain mutation commits.
- [x] Reject stale previews when the review or subject changed between preview
  and confirmation; refresh the card with a clear explanation.

### Persistence and Transactions

- [x] Add a schema migration for deferred-until time, revision/version,
  application status/error, action/idempotency key, and any normalized fields
  required for efficient filtering and ordering.
- [x] Persist typed candidate envelopes with a version and kind discriminator;
  retain backward decoding for existing `candidate_json` records.
- [x] Add indexed query support for status, priority, kind, due time, creation
  age, and scope without loading the entire queue.
- [x] Implement compare-and-set pending-to-terminal decisions in the same SQLite
  transaction as memory, graph, lifecycle, and audit mutations.
- [x] Persist linked review-create, review-action, memory-write, graph-change,
  and compensation audit events.
- [x] Migration must preserve all existing review IDs, decisions, notes,
  provenance, tags, timestamps, and candidate payloads.
- [x] Backup/export/import/retention/redaction/integrity behavior must include
  every new review field and event.

### Prioritization

- [x] Implement deterministic review scoring as a pure/testable policy with
  explicit weighted components and a human-readable explanation.
- [x] Include conflict severity, critical type, trust, provenance, stale age,
  recall/use counts, recent context inclusion, procedure failures, and waiting
  age.
- [x] Explicit priority remains an input but cannot hide a critical conflict.
- [x] Record score components in hydrated output; do not persist an opaque score
  that silently becomes stale.

### Optional Advisory Summaries

- [x] Intentionally do not expose model-generated advisory explanation/risk
  summaries; deterministic governed risk components remain the only review
  explanation path, so no child inference boundary is required.
- [x] Because advisory summaries are not exposed, no unlabeled model suggestion
  can select or apply a decision; any future advisory feature must identify its
  source IDs/model and remain non-authoritative.
- [x] The entire review workflow remains functional when advisory inference is
  disabled or fails.

## Surface Contracts

### Interactive TUI

- [x] Build reusable Review Inbox, Review Detail, Confirmation, Edit, Source
  Inspector, Conflict Resolver, History, and Batch Summary overlays.
- [x] Use existing selector/overlay/layout primitives and established terminal
  resize/focus/viewport behavior.
- [x] Replace the active review overlay on navigation so Escape returns one
  logical level and exits the Inbox from its landing screen; nested text prompts
  consume standalone Escape and restore the prior review screen.
- [x] Restore editor contents and focus after cancellation or completion.
- [x] Keep long content scrollable, preserve Unicode grapheme behavior, expose
  visible keyboard hints, and remain usable in narrow terminals.
- [x] Transcript receipts summarize outcomes without dumping raw JSON.

### CLI

- [x] Support:
  - `qt memory review list [filters]`
  - `qt memory review show <id>`
  - `qt memory review next [filters]`
  - `qt memory review preview <id> <action> [action options]`
  - `qt memory review decide <id> <action> --yes [--note ...]`
  - `qt memory review defer <id> --until ... --yes [--note ...]`
  - `qt memory review reopen <id> --yes --note ...`
  - `qt memory review history [filters]`
- [x] Human output uses readable cards/tables; `--json` returns stable typed
  objects without encoding typed candidates as nested JSON strings.
- [x] Mutating commands preserve explicit authority and never prompt on
  structured stdout.

### RPC

- [x] Add typed list/show/next/preview/decide/defer/reopen/history/batch
  operations with stable error codes and schema validation.
- [x] Separate preview from apply using a preview revision/idempotency token.
- [x] Stream or poll background batch progress without blocking unrelated RPC
  requests.
- [x] Preserve stdout as valid NDJSON and route diagnostics to stderr/events.

### Model-Facing Tools

- [x] Split broad review behavior into focused read-only list/show/preview tools
  and explicitly authorized mutation tools, or expose equivalent discriminated
  actions with strict schemas.
- [x] Tool descriptions direct the model to indexed review/source lookup rather
  than text searching durable files.
- [x] Models may suggest, preview, and prepare notes but cannot self-authorize a
  risky review decision.
- [x] Tool results match the RPC domain representation and include application
  receipts rather than generic success booleans.

## Batch Workflow

- [x] Multi-select displays count, kinds, scopes, risk distribution, and exact
  aggregate effects before confirmation.
- [x] Bulk reject/defer is supported with a shared or per-item note.
- [x] Bulk approval requires homogeneous action semantics and low-risk,
  source-backed candidates.
- [x] Critical validation, conflict resolution, merge editing, supersession,
  and destructive actions remain item-by-item.
- [x] Apply each item transactionally and return ordered successes/failures;
  retry tokens prevent duplicate mutation.
- [x] Preserve selection and filters after partial failure so users can retry or
  inspect failed items.

## Statistics and Observability

- [x] Persist and expose pending count and oldest/median age by kind, priority,
  scope, and risk class.
- [x] Track reviewed, approved, edited-and-approved, rejected, deferred,
  resurfaced, reopened, merged, superseded, validated, compensated, and failed
  totals.
- [x] Track time to first decision and review duration percentiles without
  storing prompt content.
- [x] Track batch size, partial failure, retry, idempotent replay, compensation,
  and application-error counts.
- [x] Track unresolved critical conflicts, stale validated memories,
  unprovenanced active memories, and high-use low-trust memories.
- [x] Track Dreaming review yield/outcomes separately from Dreaming generation
  runs and never count generated artifacts as trusted memories.
- [x] Show a concise review section in `/memory status`, a detailed section in
  Review Inbox, and the full stable structure through CLI JSON and RPC.
- [x] Reconcile statistics from durable review/audit state across restart; do
  not rely only on process-local counters.

## Implementation Sequence

### Phase 0 — Characterization and Contract Fixtures

- [x] Capture current CLI, RPC, tool, and interactive review behavior in focused
  probes before changing it.
- [x] Seed fixtures for every existing review kind, trust state, scope level,
  priority, malformed legacy payload, missing subject, and terminal status.
- [x] Add fixtures for conflicts, stale memories, critical types, Dreaming,
  research successors, procedure failures, and cross-scope references.
- [x] Define stable JSON examples for list, detail, preview, result, history,
  batch result, and errors.

Exit: existing behavior and all target contracts have executable fixtures.

### Phase 1 — Correct Governed Backend

- [x] Implement typed contracts, migration, query/hydration, prioritization,
  preview, kind handlers, atomic apply, idempotency, and auditing.
- [x] Implement real effects for all exposed approval actions or hide unsupported
  actions until their handler is complete.
- [x] Unify interactive, CLI, RPC, and tool callers on the review service.
- [x] Add deferred scheduling, terminal-decision guards, history, reopen, and
  compensating actions.

Exit: every advertised action has one previewed, atomic, audited, idempotent
effect and all surfaces observe identical results.

### Phase 2 — Headless Surfaces

- [x] Implement complete CLI human/JSON commands.
- [x] Implement typed RPC operations and errors.
- [x] Implement focused model-facing review tools and authority checks.
- [x] Add durable review statistics to memory status surfaces.

Exit: the entire workflow is usable and testable without the TUI.

### Phase 3 — Review Inbox TUI

- [x] Implement inbox, cards, filters, details, sources, confirmation, keyboard
  triage, receipts, and resumable position.
- [x] Implement edit-and-approve, deferred/history views, direct notification
  routing, and kind-specific conflict/procedure layouts.
- [x] Verify focus, resize, scroll, narrow-terminal, cancellation, and prompt
  restoration behavior.

Exit: ordinary users can complete a queue accurately without copying IDs,
reading JSON, or learning internal lifecycle verbs.

### Phase 4 — Batch and Advisory Enhancements

- [x] Implement constrained batch selection, preview, progress, partial-failure
  recovery, and retry.
- [x] Keep optional isolated advisory summaries out of scope for this workstream;
  deterministic details and kind handlers provide the complete review path.
- [x] Add idle, non-blocking review reminders and queue-health indicators.

Exit: high-volume review is efficient without weakening critical-item safety.

### Phase 5 — Hardening and Compatibility

- [x] Complete migration, export/import, backup/restore, retention, redaction,
  integrity, restart, and concurrent-client tests.
- [x] Verify old review rows remain readable and actionable after migration.
- [x] Update `MEM.md`, workstream plans, dependency docs if applicable,
  reference test map, and upstream compatibility documentation.
- [x] Remove obsolete duplicated command paths only after parity probes pass.

Exit: review behavior is durable, compatible, documented, and production-ready.

## Verification Matrix

### Domain and Persistence

- [x] Empty, single-item, mixed-kind, mixed-priority, filtered, paginated,
  deferred/due, and restart-persistent queues.
- [x] Stable priority ordering and score explanations.
- [x] Legacy candidate decoding and lossless schema migration.
- [x] Pending-only compare-and-set under concurrent decisions.
- [x] Idempotent retries and stale-preview rejection.
- [x] Atomic rollback when any memory/graph/audit mutation fails.
- [x] Export/import/backup/retention/redaction/integrity coverage.

### Governance Effects

- [x] Dream approval produces only the previewed hypothesis transition.
- [x] Edited approval stores exactly the previewed content and lineage.
- [x] Rejection prevents trusted recall without deleting audit history.
- [x] Deferral performs no subject mutation and resurfaces when due.
- [x] Consolidation/compression creates the previewed canonical record and
  links/retires sources correctly.
- [x] Conflict outcomes update supersession, scope, warning edges, and recall
  behavior exactly as previewed.
- [x] Critical validation fails without required evidence/authority.
- [x] Reopen and compensation preserve the complete action chain.

### Scope and Security

- [x] Cross-user/project/repository/branch/session/task access is denied unless
  explicitly permitted by the review policy.
- [x] Hydration never leaks source content from a broader or unrelated scope.
- [x] Candidate payloads remain untrusted data and cannot inject instructions
  into advisory agents or UI control flow.
- [x] Secrets are redacted from previews, receipts, audit metadata, and errors.
- [x] External filesystem policy is honored when opening source evidence.

### Surface Parity

- [x] CLI human output, CLI JSON, RPC, tools, and TUI return the same typed
  preview and application semantics.
- [x] Structured stdout remains clean.
- [x] Interactive confirmation supplies authority exactly once.
- [x] Keyboard navigation, edit cancellation, queue position, filters, direct
  transcript routing, resize, and narrow-terminal layout work correctly.
- [x] Batch unsafe combinations are rejected before mutation; partial failures
  remain recoverable.

### Statistics

- [x] Counts and age distributions reconcile with durable review/audit rows.
- [x] Statistics persist across restart and remain scope-correct.
- [x] Dreaming generation and Dreaming review outcomes are not conflated.
- [x] Failed/partial/idempotent/compensated actions are classified correctly.

### Required Commands

- [x] Native release build: `pio run -e native`
- [x] Focused review lifecycle and migration probe.
- [x] `python3 scripts/memory_probe.py --binary .pio/build/native/qt`
- [x] Focused interactive Review Inbox probe.
- [x] RPC structured-output and concurrent-decision probe.
- [x] AngelScript governance/memory host coverage.
- [x] `git diff --check` and applicable script syntax checks.

## Definition of Done

The review workstream is complete only when:

- `/memory review` provides a fast, understandable, resumable Inbox and no
  common workflow requires raw JSON or copied IDs.
- Every card contains enough governed content, evidence, provenance, scope, and
  recall-impact information for an informed decision.
- Every approval label describes its exact outcome, and the corresponding
  backend handler performs that outcome atomically.
- Unsupported/no-op acceptance paths do not exist.
- Agent and Dreaming output cannot enter trusted recall through presentation,
  batch, retry, or surface inconsistencies.
- Deferred/history/reopen/compensation workflows are durable and auditable.
- CLI, RPC, tool, and TUI behavior share one typed service and agree under
  success, failure, retry, and concurrency.
- Persistent statistics make queue health, outcomes, failures, and Dreaming
  review yield visible in memory status.
- Migration and all verification matrices pass with compatibility documentation
  updated.
