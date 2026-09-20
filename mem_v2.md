# Memory V2: rapid implementation plan

Status: implementation plan; unchecked items are acceptance gates, not an audit
of any in-progress code in the working tree.

Implementation checkpoint (2026-09-19): schema-six structured storage, immutable
assertions, revision-bound chunks/vectors, indexing leases, atomic governance and
capture receipts have focused probes. Memory-only graph evidence bundles and
independent dense candidate fusion are implemented. Runtime indexing is verified
with a synthetic encoder, including cancellation, scope fallback and invalidation;
the shared llama binding also passes a real Nomic Q8_0 encoder smoke test and
nonlexical SQLite/dense recall. Full corpus quality/resource gates remain open. Independent
code candidates reach runtime context with source revalidation and separate code
IDs. Code retrieval snapshot work is opt-in. The read-only code snapshot and mixed
graph adapters exist. Runtime code and memory seeds now produce bounded mixed
paths and atomic evidence bundles with final source/conflict, revision and
persisted-relation validation. Focused probes cover memory-to-code discovery,
two-hop paths and omitted stale/incomplete bundles. Current and historical
assertion context now retains explicit polarity, qualifiers, validity and source
evidence in the same budgeted item. Opt-in graph recall now traverses scoped
entity assertions from lexical/dense or explicit entity seeds, retaining every
claim owner and revision in an atomic context path. Negative claims terminate
traversal; stale evidence or changed bridge revisions invalidate the path.
Canonical global-scope critical reviews are checked before graph expansion;
the storage probe verifies the same repository request before and after review.
Authority checks now read the full unresolved queue rather than the first 1,000
reviews. A regression with 1,001 unrelated higher-priority critical reviews
keeps the blocked bridge outside the UI query window and verifies exclusion.
Direct entity/code cross-store paths and multi-claim assertion seeds now preserve
their complete governed entity bridges in mixed context bundles. The production
entity/code probe verifies negative terminal claims, literal assertions, forged
revision rejection, source drift and exact context-budget boundaries. Full
management integration remains open. RPC `memory explain` and dashboard context
previews now share memory/code/path serializers and the runtime context builder,
with explicit non-injection, selection and approximate full-context cost.
Standalone CLI explain now uses the configured llama encoder and a persisted,
fingerprint-verified code snapshot to query already-published indexes without
starting capture, review or indexing workers. A real Nomic probe verifies dense
memory/code retrieval, exact symbols, source edits, missing-model fallback and
unchanged pending jobs, capture files, access counts and code-index bytes.
Context previews now attribute selected and omitted
items to their memory/code/block members, distinguish validation from budget
failures, and separate item bytes from shared context overhead. Legacy code
snapshots without captured fingerprints require a rescan. Canonical
export/import now has same-database clone coverage for entities, links, historical
prose/evidence, removed assertions and rebuilt indexing jobs. Historical context
revalidation also has production-path coverage. Scoped retention/redaction has
dedicated dry-run, secret-scrub, alias-collision and bounded-cleanup probes.
Scoped rebuild controls now pass executable RPC/dashboard probes; the dashboard
snapshot exposes live retrieval status. The measured four-way production report
is in `docs/memory-v2-encoder-evaluation.md` and `docs/memory-v2-evaluation.json`.
Combined Recall@1 is 0.7292 versus lexical 0.6042, but every mode still injects
the fixture's active old cache policy. Dense code candidates now retain their
matched representation chunks as transient supporting evidence, exposed through
`matched_chunks` in retrieval explanations while the canonical source window
remains the only rendered chunk. RPC and dashboard retrieval status now expose
effective code freshness and query-cache accounting. Contradiction handling,
complete explanations and broader quality/resource rollout
gates remain open.
The CLI status review-statistics scope was reconciled with the Review Inbox
listing through one shared merge of the exact and canonical durable scope, and
the review probe now computes that reconciliation instead of reporting a fixed
value. The Dreaming probe runner was repaired to link the retrieval-engine,
indexer, graph, and code-snapshot objects the runtime depends on, so the
Dreaming lifecycle probe builds and passes again.
Keep phase checkboxes open until their
complete contracts and gates pass, including production-path integration.

## Objective and scope

Build a native C++17 multi-representation memory system: keep evidence-backed
structured records canonical, maintain rebuildable retrieval indexes, and connect
memory to the existing code indexer. The first release combines structured and
local graph retrieval with BM25 and dense search across memories and code.

The first useful outcome is answering questions such as “What should I consider
before changing this function?” with current code, relevant decisions, previous
incidents, and traceable relationships between them.

This extends [MEM.md](MEM.md), not a replacement for its governance, review,
Dreaming, scope, persistence, or context-injection contracts. Track implementation
here as the memory V2 workstream. Keep all dependencies
within [platformio.ini](platformio.ini) and [docs/dependencies.md](docs/dependencies.md).
No new runtime, vector database, graph database, or language dependency is required
for the first release.

Defer SPLADE, ColBERT, global graph/community synthesis, approximate vector
indexing, and LarQL residual injection until the baseline is measured. Design
representation metadata to accommodate them without implementing unused engines.

## Existing implementation and constraints

| Native owner | Reuse | Gap to close |
| --- | --- | --- |
| `include/qt/memory/models.hpp`, `src/memory/storage/sqlite_memory_database.cpp` | Memories, provenance, graph edges, revisions, SQLite and FTS | Structured entities/assertions, representation revisions, durable indexing jobs |
| `src/memory/recall_service.cpp` | Scope/status filtering, conflict/source checks | FTS currently gates candidates; hashed-token scoring is not neural retrieval; graph use is limited to conflict warnings |
| `src/memory/coordinator.cpp`, `src/memory/context_service.cpp` | Recall orchestration, isolated pre-handler, bounded transient context | Mixed code/memory results and evidence-path context bundles |
| `include/qt/ai/llama_provider.hpp`, `src/ai/llama_provider.cpp` | Native embeddings and tagged model contexts | Inject into retrieval; enforce one compatible query/document embedding space |
| `include/index/code_index.hpp`, `src/index/code_index.cpp` | Classes, methods, functions and heuristic call edges | Reliable identities, ambiguous target handling, source fingerprints and chunk boundaries |
| `src/index/sqlite_store.cpp`, `src/index/service.cpp` | Code/file FTS, persisted snapshots and generations | Generation-aware reads, current-source validation and derived embedding invalidation |
| `src/index/mcp_http_code_index.cpp` | Existing `index-codebase-memory-v1` graph export | Existing name-based IDs can collide; bare-name call resolution must not silently pick a target |
| `src/app/runtime/runtime_services.cpp` | Ownership of code-index and llama services | Constructor-injected read-only adapters with explicit shutdown ordering |

The code indexer is not a compiler-resolved program graph. Its symbol/call
extraction is a C/C++ heuristic; other scanned languages can still contribute
file text. General references, include dependencies, overload resolution and
cross-language symbol resolution must not be advertised as implemented.
Snapshots are rebuilt as a whole. Tree-sitter metadata has incremental tracking,
but that does not make the symbol, FTS or call-graph indexes incremental.

## Architecture

```text
Canonical memory records + evidence             Current code-index snapshot
      |                                                 |
      +-- memory FTS / dense chunks          code FTS / dense chunks --+
      |                                                 |             |
      +-- entities / assertions / links -- typed code references       |
                          |                                           |
Query + scope + time constraints                                      |
      +-- BM25 candidates --------------------------------------------+
      +-- dense candidates -------------------------------------------+
      +-- structured graph queries / bounded local traversal ---------+
                          |
               deduplicate + rank fusion
                          |
          current-source and governance revalidation
                          |
           evidence bundles + context token budget
```

The memory database owns human/agent knowledge and its history. The code indexer
owns current code structure and source locations. A retrieval layer joins them
through typed references; it must not duplicate the entire code graph into
long-lived memory or force code nodes into the current memory-only edge API.
Expose a read-only union graph adapter with typed endpoint IDs and separate
memory and code adjacency providers. Resolve code access from the authorized
repository/worktree selected for the request; a memory link alone never grants
access to another checkout. Branch-scoped links must match the selected branch;
cross-worktree traversal requires an explicitly authorized target.

## Canonical data and derived representations

Use additive SQLite migrations and focused repositories. The following are
logical records; finalize physical table names during V2-00.

| Record | Required fields and behavior |
| --- | --- |
| Entity | Stable ID, kind, scope, canonical name and scoped aliases; ambiguous aliases remain unresolved |
| Memory/assertion | Existing memory ID and revision, kind, subject, predicate, object entity or typed literal, qualifiers, polarity, status, confidence and original explanatory text |
| Evidence link | Source ID/revision, content fingerprint, locator/span, recorded time and extraction/author metadata; an assertion may have multiple sources |
| Temporal validity | Optional valid-from/valid-to separate from when the assertion was recorded; preserve historical claims rather than overwriting them |
| Relation | Directed typed endpoints, scope, supporting assertion/evidence, validity and resolution status; distinguish factual, provenance and similarity relations |
| Code reference | Repository/worktree identity, relative path, node kind, symbol identity, snapshot generation, source fingerprint and locator |
| Search chunk | Owner kind/ID/revision, chunk ordinal, text, source span, rendered-text hash and rendering/chunking version |
| Representation | Chunk ID/revision, representation kind, model/tokenizer fingerprint, dimensions, normalization/prefix settings, payload and index generation |
| Index job | Owner/revision, representation generation, state, lease, retry count and error; unique idempotency key |

Accepted source-backed assertions are canonical. Model-extracted assertions
remain candidates until existing governance permits their use. Preserve original
evidence and prose: triples alone lose rationale, exceptions and procedural order.

Start with relations such as `supported_by`, `derived_from`, `supersedes`,
`contradicts`, `discusses`, and `applies_to`. Project code relations such as
`contains_file`, `defines`, and heuristic `calls` from the index adapter.
Do not infer causation from proximity or similarity, and do not make all relation
types transitively traversable. New dependency or reference edges require a
producer that can substantiate them.

## Non-negotiable retrieval invariants

- Every candidate and every graph hop obeys the requested scope, status and time
  policy. Check endpoint visibility as well as edge visibility; an excluded node
  cannot become a bridge into otherwise unreachable content.
- Relevance is separate from authority. High BM25, cosine or graph scores never
  approve a candidate or override a contradiction/source-drift exclusion.
- Every derived representation identifies the exact content revision it encodes.
  Ignore stale vectors immediately even while replacement indexing is pending.
- Memory schema, code snapshot and each derived index have independent version
  namespaces. No coincidentally equal generation number implies compatibility.
- One dense index generation uses one compatible embedding space. Independently
  selected document/short-text models must not silently be paired for retrieval.
- Canonical changes and indexing-job creation commit together. Code and memory
  databases need no distributed transaction: references pin a code generation,
  and publication/revalidation detects mismatches.
- A retrieval request pins an immutable code snapshot or detects a generation
  change and retries within a bound. Source edits after a scan are detected by
  fingerprint validation before current-code excerpts enter context.
- Model failures preserve lexical/graph fallback. Memory-disabled behavior stays
  unchanged. Cancel and bound background work; provider lifetime exceeds workers.
- Keep context untrusted and transient under existing MEM.md rules. Indexing code
  or tool output must not confer instruction authority on it.

## Retrieval contract

Input: query text, optional explicit entity/relation constraints, scope/recall
policy, optional as-of time, candidate/edge/token budgets and cancellation.

Use current time by default and half-open validity intervals `[from, to)`;
missing bounds are unbounded. Recorded time remains distinct: an as-of knowledge
query excludes claims recorded after its cutoff as well as claims invalid then.
Code results describe the pinned current snapshot; do not present them as
historical code without a separately available historical snapshot.

Historical results carry the captured assertion revision, prose and provenance;
context must never substitute the current memory text for an older assertion.
Current governance still applies: rejection, deletion or an access restriction
blocks historical guidance. Verify cutoffs between edits, revisions that remove
an assertion, exclusion of later-recorded knowledge and rejection after capture.

Freeze the FTS tokenizer, Unicode normalization and field weights with the
baseline. Preserve exact symbol/path matching alongside natural-language BM25;
document language limitations and version any ranking-affecting changes.

Output: typed memory/code candidates with source revision, eligibility,
per-retriever rank/score, matched entities, graph path, evidence locators,
freshness state and exclusion/omission reasons. Return a small evidence bundle
when a path needs several records to remain intelligible.

1. Resolve explicit entities using scoped aliases and code identities. Use BM25
   and dense hits as additional seeds when wording is ambiguous. Keep multiple
   plausible matches rather than guessing a single entity.
2. Run BM25 and dense search independently over eligible memory and code chunks.
   Dense candidates must not be restricted to prior FTS matches.
3. Run direct relationship queries and bounded local graph traversal. Start with
   depth 2, 16 seeds, 256 examined edges and 64 emitted graph candidates as
   configurable tuning defaults, not proven optimal limits. Cap per-node fanout,
   track visited nodes, respect direction and relation-specific traversal rules.
4. Deduplicate by typed owner/revision, retain supporting chunks/paths, and fuse
   rankings using reciprocal rank fusion initially. Aggregate multiple chunks
   from one owner so repeated code or prose cannot dominate. Do not directly add
   uncalibrated BM25, cosine and graph scores or count duplicated evidence twice.
   Break equal ranks deterministically using typed owner IDs and chunk ordinals.

   Ranking revision `owner-rrf-v2` aggregates dense chunks by their best cosine
   before applying the owner limit. Equal scores use owner key, numeric chunk
   ordinal and chunk ID. Recall and code fusion retain one best rank per owner
   per retriever; repeated hits cannot consume the owner budget or add votes.
   Raw storage chunk search remains available for index diagnostics. This
   revision does not change the frozen FTS tokenizer, normalization or weights.
5. Revalidate governance, source fingerprints and code snapshot identity. Rank
   graph candidates using seed relevance, relation usefulness and bounded path
   length; keep confidence/provenance visible rather than treating them as proof.
6. Pack complete evidence bundles, with diversity and source limits, into the
   existing context budget. Record separately: retrieved, eligible, selected,
   injected and subsequently used IDs. An omitted bridge must not leave a
   misleading disconnected conclusion.

## Rapid delivery sequence

Deliver each phase as a small reviewable change. All items below remain unchecked
until their behavior and acceptance gates are verified.

| Phase | Deliverable | Depends on |
| --- | --- | --- |
| V2-00 | Stable contracts, correctness repairs and frozen baseline | Existing memory contracts |
| V2-01 | Structured store, BM25 and recoverable indexing | V2-00 |
| V2-02 | Versioned, read-only code adapter | V2-00; parallel with V2-01 |
| V2-03 | Structured queries and local graph evidence in context | V2-01 and V2-02 |
| V2-04 | Compatible dense retrieval and rank fusion | V2-01 chunk contracts; integrate after V2-03 |
| V2-05 | Explanations, measured evaluation and controlled rollout | Fixtures begin at V2-00; final gate follows V2-04 |

Ship the lexical/structured/graph milestone first behind explicit controls, then
add dense augmentation. Each slice includes its migration or API contract,
production-path integration, focused verification and documentation update.

### First end-to-end slice

Use one concrete workflow to drive integration: a query names a function; the
code index resolves its current symbol; an explicit `applies_to` link retrieves
a governed decision; context includes the current source window, decision and
supporting evidence. The same query must exclude a deleted symbol, an obsolete
decision and a link outside the authorized scope.

Implement in this order:

1. Freeze typed IDs, source revisions and the evidence-bundle contract. Add the
   workflow above to the evaluation corpus before changing retrieval.
2. Persist one structured decision and its evidence atomically with indexing
   work. Preserve ordinary prose memories and existing BM25 behavior.
3. Inject the code snapshot adapter and implement exact-symbol/BM25 seeds plus
   one-hop links. Extend to bounded two-hop traversal after the vertical slice
   reaches context assembly with complete evidence and freshness checks.
4. Add independent dense candidates using the existing embedding provider,
   revision-bound vectors and reciprocal rank fusion. Verify fallback when the
   model is unavailable before evaluating paraphrase recall with a real encoder.
5. Expose retrieval explanations, measure the four retrieval configurations and
   enable only modes that pass their correctness and resource gates.

This sequence is an integration priority, not permission to skip the phase gates
below. Keep the first delivery small by using SQLite, exact vector search and
whole code snapshots; defer new parsers, approximate indexes and neural rerankers.

### V2-00 — Freeze contracts and unblock correctness

- [ ] Define typed references, candidate/evidence-bundle output and storage
  migrations; preserve existing memory IDs and legacy graph/import behavior.
- [ ] Establish a labelled retrieval corpus and capture the current baseline
  before tuning. Include real repository questions and held-out paraphrases.
- [x] Reconcile exact-scope mutation checks, lifecycle transitions and atomic
  governance effects identified in the memory audit.
- [x] Repair capture/job recovery paths where a durable claim can suppress a
  failed operation's retry; separate recall access from actual context inclusion.
- [ ] Define bounded cancellation/shutdown and embedding-provider ownership.

Gate: old memory/review contracts still pass; crash/retry and cross-scope probes
show no lost work or unauthorized mutations. Schema migration is transactional;
failed migrations preserve a usable old database.

### V2-01 — Canonical structure and versioned indexing

- [x] Add entities, aliases, qualified assertions, evidence links and typed
  cross-store references without requiring all historical memories to be parsed.
- [x] Backfill existing memories as text-bearing records with their existing
  provenance; expose incomplete structure explicitly.
- [ ] Implement chunk rendering, representation metadata, durable outbox jobs,
  lease recovery, bounded retries and index-generation publication.
- [ ] Invalidate derived data on edit, deletion, supersession and source change.
  Apply existing history-retention rules; removal from recall is immediate.
- [x] Preserve canonical entities, assertions and evidence through export/import
  and retention operations; rebuild indexes rather than trusting imported vectors.
- [ ] Implement BM25 over suitable memory fields and reuse code FTS through a
  read-only adapter. Rebuild derived indexes without modifying canonical records.

Gate: repeated backfills are idempotent; interrupted indexing resumes; stale or
deleted representations never reappear in eligible search results.

Verified recovery checkpoint: schema 7 persists each indexing job's retry ceiling
(default five, explicit failure policies one to 100). Claim recovery atomically
fails exhausted expired leases or due retries, preserving exact scope, current
revision, generation selection and active leases. The storage probe verifies
repeated database reopening after crashes, custom policy persistence, stale
worker rejection, independent job progress and schema migration/reopening.
Broader phase acceptance remains under audit; this checkpoint does not close
the full indexing/invalidation gate.
The code retrieval probe also verifies unchanged-text vector reuse across a new
snapshot generation and an edit outside the rendered source windows. Reused
chunks keep their revision/hash, returned references carry the refreshed source
fingerprint/generation, and validation rejects the previous references.

### V2-02 — Code-index integration

- [x] Add a read-only snapshot adapter injected into retrieval; use native
  interfaces rather than serializing through the MCP HTTP endpoint internally.
- [x] Introduce collision-resistant symbol identities using repository/worktree,
  relative file, kind, qualified name and signature/discriminator. Separate
  identity from source version. Preserve explicit ambiguity when extraction is
  insufficient; do not promise identity continuity across arbitrary renames.
- [x] Project existing file/symbol/call relationships into the common graph
  contract. Replace last-match-wins target resolution with explicit alternatives
  or unresolved targets; label calls with heuristic provenance.
- [x] Attach source fingerprints and generations; reject stale current-code
  excerpts. Handle moved/deleted symbols without silently rebinding memory links.
- [x] Produce bounded code chunks with names, paths and source context. Until
  reliable function extents exist, use bounded source windows and label them.
- [x] Use whole-snapshot generation changes for the first release. Compare chunk
  hashes to reuse embeddings; defer a full incremental symbol index redesign.

Gate: same-name functions, overloads, two worktrees, edits between scan and query,
renames, deletions and ambiguous callees retain correct identity/freshness behavior.
For arbitrary renames, correctness means explicitly stale or unresolved old links
and validated new identities, not guaranteed identity continuity.
Existing code-index clients remain compatible or receive an explicit versioned
contract; do not silently change `index-codebase-memory-v1` semantics.

Verification: `scripts/mcp_v1_contract_probe.py` exercises production graph and
snippet serializers, asserts the legacy keys/types and source coordinates, and
passes alongside the rename/overload/worktree retrieval probes. The legacy graph
serializer remains byte-identical to its committed implementation.

### V2-03 — Structured and local graph retrieval

- [ ] Add directional adjacency queries and reverse indexes with endpoint scope
  checks. Implement direct entity/relation/as-of queries.
- [ ] Implement bounded traversal across projected code nodes, memories and
  assertions, with relation policies, cycle detection and per-node fanout caps.
- [x] Add candidate creation from graph paths independently of FTS candidates;
  support code-to-memory and memory-to-current-code traversal.
- [ ] Return evidence bundles and path explanations; distinguish stored facts,
  inferred links, heuristic code edges and unresolved relationships.
- [ ] Fuse graph and BM25 candidates and integrate bundles into context assembly.

Gate: recover an indirectly related decision that keyword search misses; preserve
its connecting evidence. Cycles, high-degree nodes, contradictory claims and
forbidden intermediate nodes cannot escape budgets or governance checks.

Verification: `scripts/memory_code_retrieval_probe.py` and
`scripts/memory_entity_code_probe.py` exercise production retrieval and final
context assembly in both directions, including explicit entity seeds without
lexical hits, multi-claim continuation, terminal negation, source drift and
forged revisions. The entity probe checks one token below and exactly at the
complete bundle budget, and recovery when an invalid duplicate precedes a valid
path. Invalid paths cannot reappear as standalone code excerpts.

### V2-04 — Dense retrieval for memory and code

- [x] Inject the existing llama embedding service with a verified compatible
  model, tokenizer, prefix, pooling and normalization contract.
- [x] Index memory and code chunks in bounded background batches. Store vectors
  in SQLite with their exact representation metadata and content revision.
- [x] Implement scoped exact cosine/dot-product search in C++ as the reference
  implementation; validate dimensions and finite values before scoring.
- [ ] Cache query embeddings by complete model/configuration fingerprint and
  query text; bound cache memory and foreground/background contention.
- [ ] Union dense candidates with BM25 and graph results, then fuse/deduplicate.
  Model absence, cancellation or indexing lag must leave fallback usable.

Dense-memory evidence checkpoint: the production callback now returns typed
`DenseMemoryCandidate` values with the matched chunk, captured owner revision,
source locator, cosine score and index generation. Recall retains this evidence
when merging lexical and dense owners. Final context validation checks the
captured revision even after identical prose is restored; unavailable authority
omits only the affected item. Explanations retain the capture's revision and
explicitly report whether it is still current. Storage validates captured chunk
metadata against the exact persisted representation. Native build and focused
recall/engine/context/explanation probes pass, including forged metadata and
same-prose stale revisions. Evidence is transient rather than canonical memory.
Standalone CLI, RPC, dashboard and core-memory probes also pass. The real Nomic
run retains the matched evidence through production recall; the unchanged
four-way fixture keeps every prior ranking and injected label, including the
known contradiction-quality failure.
Remaining candidate-contract gap: code candidates replace matched dense chunks
with a canonical snippet; supporting matched chunks still need to be retained.

Code matched-chunk checkpoint: `RetrievalCandidate` now carries transient
`matched_chunks` (the exact dense hits with similarity and index generation)
separately from the canonical rendered `chunks`. `resolve_code_hits` populates
the matched hits, the engine merges them across lexical/dense/graph owners with
id/generation/revision deduplication, and `retrieval_candidate_json` exposes
them as `matched_chunks`. The canonical source window remains the only chunk
rendered into context, so context bytes and budgets are unchanged. The code
retrieval probe verifies the retained hit's owner revision, generation,
similarity, content hash and distinct chunk id, the serialized explanation, and
absence of duplicate matches. Native build, code/entity/engine, RPC, dashboard
and real Nomic standalone-preview probes pass; the refreshed four-way report
keeps every prior ranking and injected label.

Foreground scheduling now gates each embedding chunk and publication checkpoint.
A paused job retains its spool and renews the same durable lease every second;
resuming does not consume another attempt. Stop wakes the condition wait and
cancellation prevents publication. A call already past its checkpoint can still
complete; configuration/loading and active device calls remain cooperative, so
this does not close the hard shutdown or full contention gate.

Gate: retrieve held-out paraphrases with no lexical match; reject mixed embedding
spaces and outdated vectors. Model changes publish a separate index generation;
queries never search a partially mixed generation.

Verification: the real Nomic encoder and standalone CLI probes exercise the
shared production llama binding, GGUF/tokenizer fingerprint, query/document
prefixes, mean pooling and normalized 768-dimensional vectors. Dense probes
cover generation-specific cache invalidation, LRU eviction, explicit cache
clearing, cancellation after embedding and retry without caching cancelled work.
The query cache additionally caps retained key/vector/entry storage at 32 MiB;
allocator overhead is separately bounded by the 1,024-entry ceiling. A probe
uses 65,536-dimensional vectors to verify byte-driven eviction before the entry
limit and correct accounting after clearing.
Provider-queue waits now have a 250 ms default deadline, checked both by the
waiting caller and before worker execution. Cancellation removes pending work
under the worker's dequeue lock, releasing borrowed callbacks before return;
expired work never loads a model or reaches the backend. Active operations are
joined and retain cooperative decode cancellation. This bounds pending waits
(with scheduler/polling overhead), not active model loading or device execution;
the full bounded shutdown contract remains open.
Active embedding model loads now receive the cancellation predicate through the
backend load request. The real adapter checks it at llama's tensor-load progress
checkpoints and before publishing the initialized context; callback exceptions
also cancel safely at the C boundary. Cancellation stops offload retries and
releases model/context resources before the backend guard. Callbacks are not
retained in loaded configuration. `scripts/llama_embedding_load_probe.py` verifies
real-loader cancellation, callback exceptions, cleanup/retry, callback lifetime,
serialized caller/worker predicate evaluation and preservation of a previously
loaded model when cancellation precedes replacement loading. The mock GPU-first
plan proves cancellation skips CPU fallback; runtime status records the cancelled
attempt and clears it after retry. Queue, model-routing, retrieval-engine and
real standalone-preview probes and the native build pass. Metadata parsing,
context allocation and device work still have no hard deadline.
`scripts/llama_embedding_queue_probe.py` passes pending cancellation behind a
held backend, timeout without execution, retry and cooperative active cancellation,
plus negative/zero deadlines. Real Nomic standalone previews and the retrieval
engine/model-routing probes also pass after this queue change.
The indexer now computes one vector per batch (at most 256 KiB), spools completed
vectors to an anonymous temporary file, then streams them into one atomic SQLite
publication. Model calls hold no database publication lock. The existing durable
job/lease controls recovery; temporary vectors are discarded on failure or exit.
Publication validates lease, scope, revision, chunk coverage and vector metadata;
reader failures roll back the whole owner. The 4,096-chunk and 65,536-dimension
limits also bound temporary vector storage to 1 GiB per job. Provider contention
and complete recovery/rollout gates remain open.
`scripts/memory_dense_stream_probe.py` verifies streamed reader/validation
rollback through direct database row counts, stale-job rejection before reading,
cancellation and retry, and all 547 chunks of a 1 MiB record at 65,536 dimensions.
The measured embedding-stage RSS growth was 2,452 KiB (64 MiB probe ceiling).

### V2-05 — Explanations, evaluation and rollout

Current implementation work: mixed-path explanations and context rendering now
share relation provenance, preserving the code snapshot's heuristic flag for
both symbol definitions and calls. Final validation compares that flag with the
pinned snapshot. The production code probe verifies memory-to-file-to-symbol
propagation, serialized provenance, context rendering and forged-flag rejection.
The interactive `/context` command now exposes complete prompt/session contents
and the latest prepared transient memory without running recall. The native build, RPC/dashboard, core memory/model-routing and terminal probes
pass. Terminal coverage verifies full 12 KiB source output, message/tool contents,
subsection filters, default output, no provider calls and active-turn rejection.
This completes context visibility for the available runtime data; remaining
quality/resource and phase acceptance gates stay open.

Management status checkpoint: RPC `memory status` and both dashboard retrieval
serializers now expose effective code freshness (`retrieval.code` with
`enabled`, `available`, `stale`, `generation`, `repository`, `worktree`) and
query-cache accounting (`queries.cache_entries`, `queries.cache_bytes`)
alongside the existing modes, dense generation, job counts and failures. The
code retrieval probe asserts the engine status for an enabled snapshot and for
a disabled adapter; the RPC and dashboard probes assert the serialized fields.
Rebuild status is still only returned by the rebuild command, not queryable as
status, so this item remains open.

- [ ] Expose effective retrieval modes, model/index generations, pending jobs,
  failures, code freshness and rebuild status through existing management APIs.
- [ ] Add a retrieval explanation showing source ranks, paths, source revisions,
  selection/exclusion reasons and context token cost; reuse it in CLI/RPC/UI.
- [ ] Add disabled-by-default rollout controls for graph/dense augmentation,
  budgets and code-context inclusion; preserve existing memory enablement policy.
- [ ] Compare BM25, BM25+graph, BM25+dense and the combined pipeline on the same
  held-out corpus. Report Recall@K, ranking quality, evidence correctness, scope
  violations, stale results, p50/p95 latency, index time, disk and peak RAM.
- [ ] Add meaningful production-path probes for multi-hop questions, negation,
  historical validity, exact symbols, source deletion, index rebuilds, interrupted
  jobs and model upgrades. Do not substitute hard-coded quality scores.
- [ ] Run `pio run` and applicable existing memory/review/model-routing probes;
  verify relevant CLI/RPC/UI paths and update `docs/upstream-compatibility.md` for
  changed behavior. Record commands/results and limitations in the rollout report.

Gate: zero scope/status bypasses in the regression corpus; demonstrable retrieval
improvement over baseline without loss of exact-identifier cases. Set and record
latency/RAM budgets on named hardware before rollout. Failed quality/performance
gates leave the affected mode opt-in rather than marking it complete.

Initial evaluation targets and hardware caveats are recorded in
[docs/memory-v2-rollout.md](docs/memory-v2-rollout.md). Measured results must be
reported separately from those targets.

## Parallel work and integration order

After V2-00 freezes contracts, independent work can proceed on canonical storage
(V2-01), the code adapter (V2-02), and labelled evaluation fixtures (V2-05).
Graph traversal follows the storage/code contracts. Dense encoding/scoring can
proceed alongside traversal after chunk and index-generation contracts stabilize.
Integrate and verify in this order:

1. BM25 baseline with canonical revisions and explanations.
2. Code adapter plus structured/local graph retrieval.
3. Dense retrieval plus rank fusion and fallback.
4. Full recovery, evaluation and rollout gates.

The smallest useful milestone is step 2. Do not hold it for an ANN index, a new
parser, a query language, or a complete neural-runtime port.

## Follow-on representations: evaluate before committing

| Capability | Required work | Admission criterion |
| --- | --- | --- |
| SPLADE | Verified compatible trained encoder/head, vocabulary-weight output, versioned weighted postings and sparse scoring; adding expansion words to FTS is not equivalent | Better candidate recall than the baseline at acceptable indexing/query cost |
| ColBERT-style late interaction | Verified trained token projection, masks and normalization; per-token matrices; bounded MaxSim reranking and storage budget | Better shortlist ranking; measure missed candidates separately because reranking cannot recover them |
| Approximate dense search | Rebuildable candidate index, deletion handling and recall measurement against exact search | Exact search exceeds measured latency/resource budgets |
| Global graph summaries | Versioned community summaries with source links and invalidation | Real cross-project/theme questions justify extraction and query cost |
| LarQL policy port | Adapt qualified compact records, source recovery and scoped promotion to text-memory contracts | Adds measurable correctness beyond existing governance |
| LarQL residual storage/injection | Model-specific intermediate capture, injection/partial execution and compatibility validation | Separate local-inference experiment; not required for semantic memory |

Qt's current embedding API returns a single vector; `pooling=none` currently
returns the final token vector. SPLADE and ColBERT need explicit output contracts
and model/runtime validation, not merely configuration changes. No future option
implicitly authorizes adding dependencies.

## Research references

The following user-discussed references inform the design, not claims of native
feature parity. Before borrowing implementation patterns, register applicable
sources in [REFS.md](REFS.md) under the repository's secondary-reference workflow.

- [LarQL, reviewed revision 3915016](https://github.com/chrishayuk/larql/tree/391501611e28d7be65c47ef148ae6646831841a9): source authority, reversible changes and scoped promotion; residual inference remains separate.
- [SQLite FTS5](https://www.sqlite.org/fts5.html): existing lexical retrieval and BM25.
- [Reciprocal rank fusion](https://cormack.uwaterloo.ca/cormacksigir09-rrf.pdf): initial heterogeneous-rank fusion.
- [GraphRAG query overview](https://microsoft.github.io/graphrag/query/overview/): local entity retrieval versus later global community synthesis.
- [SPLADE](https://arxiv.org/abs/2107.05720): learned sparse retrieval.
- [ColBERT](https://arxiv.org/abs/2004.12832) and [ColBERTv2](https://arxiv.org/abs/2112.01488): late interaction and representation compression.

## Completion rule

Verification checkpoint: `scripts/memory_v2_storage_probe.py` covers migration,
leases, invalidation, atomic capture and dense generations, plus production
historical recall/context rendering and rejection after current governance changes;
`scripts/memory_v2_governance_probe.py` covers exact-scope lifecycle CAS/rollback;
`scripts/memory_probe.py` covers durable spool restart and final-context mutation
revalidation. `scripts/memory_retrieval_engine_probe.py` verifies asynchronous
indexing and nonlexical candidate retrieval with synthetic vectors, scoped
fallback, cancellation and model-change validation. These checks do not establish
learned semantic accuracy or the unfinished mixed code-to-context contract.

Check an item only when implementation, edge cases and relevant verification pass.
Update this plan immediately as slices land. Preserve `MEM.md` guarantees and
explicitly document remaining partial capabilities. This document alone changes
no runtime behavior and does not mark any retrieval capability implemented.
