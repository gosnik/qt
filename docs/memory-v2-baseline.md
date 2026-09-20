# Memory V2 retrieval baseline

This is the frozen V2-00 characterization corpus and the measured baseline for
the production `RecallService` before retrieval changes. The corpus lives in
[`scripts/fixtures/memory_v2_retrieval.json`](../scripts/fixtures/memory_v2_retrieval.json)
and includes exact symbol questions, one held-out paraphrase, repository scope
isolation, multi-hop and incident questions, current-versus-historical policy,
contradictory claims, freshness, and global-user security guidance.

Run it with:

```sh
python3 scripts/memory_v2_baseline_probe.py
```

The runner compiles the standalone probe against the production memory models,
SQLite database, scope rules, and `RecallService`. Each query is executed against
a fresh database. Rankings and scores are computed from the returned production
objects; the metrics are derived from the fixture's relevance and forbidden
labels rather than encoded expected scores. Use `--report /tmp/baseline.json`
when a machine-readable per-case report is needed.

The baseline run on 2026-09-19 produced eight cases:

| Metric | Result |
| --- | ---: |
| Macro Recall@1 | 0.6042 |
| Macro Recall@3 | 0.7708 |
| Macro Recall@5 | 0.8125 |
| Hit@1 | 0.750 |
| Hit@3 | 0.875 |
| Hit@5 | 0.875 |
| MRR | 0.8229 |
| Forbidden-label hit rate | 0.125 |

Report version 2 corrects the original Recall@K label, which had measured hit
rate (at least one relevant result). Macro recall divides each query's retrieved
relevant count by all its labelled relevant records before averaging. The rerun
preserves the original rankings, hit rates and MRR; only the metric definition
was corrected. Multiple supporting memories matter for the relationship cases.

The exact symbol, paraphrase, multi-hop, temporal, freshness, and contradiction
cases returned a relevant item at rank one. The scope-isolation case returned a
relevant item within the top three while excluding the beta repository. The security case
returned the global-user item at rank eleven, showing the current broad-scope
score penalty and lexical competition. The contradiction case also returned
the conflicting old policy in its top five, which is intentionally surfaced by
the current service rather than resolved as a temporal/assertion decision.

This is a retrieval baseline, not evidence that V2 contracts are implemented.
The baseline configuration is FTS-gated and uses hashed-token similarity,
confidence, quality, freshness, and scope weighting. It does not enable dense retrieval,
typed entity resolution, bounded graph traversal, as-of temporal filtering, or
evidence-bundle construction. The multi-hop labels therefore characterize the
baseline text ranking and do not claim graph traversal support. Future probes
must keep this fixture and query labels stable so BM25, graph, dense, and fused
comparisons use the same held-out cases.
