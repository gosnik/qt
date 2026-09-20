# V2 real encoder evaluation

The focused harness in `scripts/memory_v2_real_encoder_probe.py` links the
already-built native Qt objects, the production `LlamaProvider`, and
`bind_llama_retrieval`. It uses the Nomic Q8_0 GGUF with the official
`search_query: ` and `search_document: ` prefixes. It does not change runtime
configuration or download a model.

Run from the repository root after the native PIO objects exist:

```sh
python3 scripts/memory_v2_real_encoder_probe.py
```

Measured on 2026-09-19 with `models/nomic-embed-text-v1.5.Q8_0.gguf`:

| measurement | result |
| --- | --- |
| GGUF size | 146,146,432 bytes |
| dimensions | 768 |
| pooling | mean |
| normalization | enabled; query norm was unit length within 0.02 |
| embedding semantic smoke | passed; positive cosine 0.812247 vs distractor 0.476845 |
| production nonlexical retrieval | passed through SQLite indexing, DenseRetrievalService, and RecallService; query `credential lifecycle` returned the credential-reset memory first while sharing no document token |
| cancellation | callback observed and failed closed |
| RSS before/after load | 17,880 / 306,648 KiB |
| GPU execution | full-offload request (`gpu_layers=99`) reported Vulkan support; model 120,477,696 + compute 22,553,600 = 143,031,296 allocated bytes |

The observed RSS is far below the 8 GiB design budget in this CPU baseline.
The harness also accepts `99` as a second argument to request full offload and
prints the provider runtime memory report. The available RTX 5080 is 16 GiB,
so the 8 GiB requirement remains a design limit rather than a hardware claim.

The production fixture also confirms SQLite indexing, dense vector activation,
DenseRetrievalService search, RecallService fusion, and a nonlexical query.

The production four-way run uses the unchanged labelled fixture, including its
original provenance and explicit global-user access policy. All four modes use
RecallService and final ContextService packing. Dense modes index through
RetrievalIndexer into SQLite and query DenseRetrievalService; no in-memory
cosine shortcut supplies candidates. The lexical mode includes the existing
native lexical scoring policy, rather than exposing raw FTS rank alone.

Reproduce the recorded CPU run on the Intel Core Ultra 7 270K Plus:

```sh
python3 scripts/memory_v2_real_encoder_probe.py models/nomic-embed-text-v1.5.Q8_0.gguf 0 --report docs/memory-v2-evaluation.json
```

The runner builds native objects before linking. `--skip-build` is only for
already synchronized objects when no concurrent build is running.
[Machine-readable results](memory-v2-evaluation.json) retain per-case rankings,
actual injected labels, forbidden injected labels, and resource measurements.
The four-way report was refreshed on 2026-09-20 for ranking revision `owner-rrf-v2` with the supplemental
governed-policy scenario; original relevance labels, rankings and injected
labels remain unchanged.
Warm latency covers recall plus context packing, with three repeats of each of
eight queries (24 samples per mode); query caches reset between modes. These
small-corpus measurements are not a large-repository benchmark.

The latest refresh includes typed dense-memory evidence through production
recall. The real encoder probe checks the captured chunk/revision, generation
and finite cosine; the standalone CLI probe compares explanation metadata with
the persisted SQLite chunk. Rankings and injected labels remain unchanged.

| Mode | Recall@1 | Recall@3 | Recall@5 | MRR | Warm p50/p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Lexical | 0.6042 | 0.7708 | 0.8125 | 0.8229 | 137.72 / 206.27 |
| Lexical + graph | 0.6042 | 0.8125 | 0.8750 | 0.8229 | 220.47 / 302.07 |
| Lexical + dense | 0.7292 | 0.8750 | 0.8750 | 0.8906 | 294.02 / 329.02 |
| Combined | 0.7292 | 0.8750 | 0.8750 | 0.8906 | 382.60 / 427.01 |

The separate 100-record workload completed in 2,712.98 ms with exactly 100
canonical records, chunks, vectors, and document embedding calls. Embedding
p50/p95 was 24.5799 / 25.0374 ms; process peak RSS after the workload was
325,944 KiB; SQLite including its live WAL and SHM used 5,238,528 bytes.
The provider exposes no tokenizer count, so token count is explicitly
unavailable rather than estimated.

Each mode injected one fixture-forbidden result: `alpha-cache-policy-old` in
the contradictory-cache case. It remains active with no canonical supersession
or unresolved conflict review, so existing governance allows it. This is a
failed contradiction-quality gate, not a scope bypass. The report does not
relabel it or silently add a review to improve the score. The design requires
canonical relation/review authority; it does not authorize retrieval to infer a
lifecycle exclusion from prose alone. The unchanged fixture therefore retains
this quality failure. A separately reported `governed_policy` scenario now passes in all four modes.
After the original quality/resource measurements, it applies an explicit
policy-owner supersession through `GovernanceService`, with actor, reason and
an evidence URI. Both policies enter context in the pre-resolution control.
After resolution, fresh recall and results captured before the mutation both
exclude the superseded policy and retain the validated replacement. The old
policy had a published dense vector before the change and is excluded before
any reindexing; one indexing job remains pending. The report retains the audit
and `superseded_by` relationship IDs and confirms revision history grew from
zero to one. These checks verify explicit lifecycle authority, not automatic
contradiction detection or review-queue resolution. Broader evidence-completeness
evaluation remains open.

These results supersede preliminary measurements that used different provenance,
omitted global-user inclusion, and used an in-memory dense candidate shortcut.
With the corrected setup, lexical MRR matches the frozen baseline (0.8229),
graph improves Recall@3/5 without the previously reported Recall@1 regression,
and dense improves Recall@1 and MRR. All augmentation remains opt-in.

The latest combined warm p95 is 427.01 ms, below the predeclared 500 ms
target. This single cached run passes the named latency target; broader
repeatability across workloads remains unverified.
The 100-record indexing workload passes its 120-second target; tokenizer usage
remains unavailable and is recorded as null, not estimated as a measured count.
