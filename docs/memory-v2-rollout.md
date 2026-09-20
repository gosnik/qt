# Memory V2 rollout gates

These are acceptance targets, not measured results. Keep augmentation opt-in
until the corresponding correctness and resource gates pass.

## Current state and verification

The active implementation plan and per-gate status live in
[`mem_v2.md`](../mem_v2.md). This document retains rollout constraints rather
than a chronological implementation diary.

Focused native probes cover revision-bound dense/code evidence, scoped graph
and conflict authority, lease/retry recovery, bounded streamed indexing, and
RPC/dashboard explanation serialization. Reproduce the current production
encoder measurements with
`python3 scripts/memory_v2_real_encoder_probe.py`; the measured setup and
results are recorded in the [encoder evaluation](memory-v2-encoder-evaluation.md)
and [machine report](memory-v2-evaluation.json). The frozen lexical baseline is
documented in [memory-v2-baseline.md](memory-v2-baseline.md).

Known limitations remain explicit:

- The frozen-corpus evaluation still injects a forbidden contradictory policy, so
  the contradiction-quality gate remains unresolved; do not treat encoder or
  synthetic retrieval success as a quality sign-off.
- Provider loading, device allocation, and active model execution use
  cooperative cancellation checkpoints only. They do not provide a hard
  shutdown deadline.
- Rebuild progress is returned by the rebuild command, not exposed as durable
  `memory status` state; the V2-05 management/status gap remains open.

Before changing an opt-in gate, run the relevant focused probes from
[`docs/reference-test-map.md`](reference-test-map.md), plus the native build,
and record any changed measurements in the evaluation documents above.

## Local encoder profile

- Model: existing `models/nomic-embed-text-v1.5.Q8_0.gguf`.
- Context: 2,048 tokens; normalized mean pooling; full 768-dimensional vectors.
- Prefixes: `search_query: ` and `search_document: `.
- GPU requirement: fit an 8 GiB GPU. Target at most 2 GiB of model/context/device
  allocations, leaving room for the application and other workloads.
- Host process peak RSS target: at most 2 GiB for the evaluation corpus.
- Warm single-query retrieval p95 target: at most 500 ms on the named evaluation
  hardware, measured separately from model loading and background indexing.
- Background indexing target: at most 120 seconds for 100 short memory records;
  report total tokens and individual chunk latency so this is reproducible.

The available evaluation GPU is an NVIDIA GeForce RTX 5080 with 16 GiB. A
measured footprint below the 8 GiB limit demonstrates memory headroom; it does
not establish the same throughput on every 8 GiB GPU. Record GPU allocations,
host RSS and workload sizes separately. Do not infer runtime footprint from
the model file size alone.

## Retrieval quality

Use the same held-out questions and relevance labels for lexical, lexical+graph,
lexical+dense and combined retrieval. Preserve the original corpus in
`scripts/fixtures/memory_v2_retrieval.json`; additional cases supplement it.

Report macro Recall@1/3/5, hit rate at those cutoffs, MRR, eligible/injected
forbidden results, complete evidence bundles, source freshness, and p50/p95
latency. Recall counts the fraction of all labelled relevant memories retrieved;
hit rate counts queries with at least one relevant result.

Mandatory gates:

- Zero scope/status bypasses or stale current-code injection in the regression
  corpus, including graph bridges and changes after candidate retrieval.
- No loss of exact-identifier cases compared with the lexical baseline.
- Improved held-out paraphrase or relationship recall, with traceable evidence.
- No partial mixed evidence bundle injected when its full bundle exceeds budget.
- Interrupted jobs recover, deleted records remain excluded, and incompatible
  embedding generations never mix.

Report failed gates and measurements even when a mode remains disabled. A small
encoder smoke test proves compatibility only; it does not satisfy the four-way
retrieval comparison or establish general semantic accuracy.
