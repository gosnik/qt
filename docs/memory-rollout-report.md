# Built-in Memory Rollout Baseline

Measured on 2026-07-17 with the credential-free deterministic provider in
`python3 scripts/memory_probe.py --binary .pio/build/native/qt`. The fixture
runs the same repository question with memory disabled and enabled, serves both
the isolated pre-handler and main model locally, and inspects the actual main
provider request.

| Metric | Memory disabled | Memory enabled |
| --- | ---: | ---: |
| Task quality | 0.0 | 1.0 |
| Retrieval precision | n/a | 1.0 |
| Correction rate | 1.0 | 0.0 |
| Fixture latency | 421 ms | 1,130 ms |
| Canonical-memory growth | 0 | 0 |

The enabled request injected an estimated 34 of 1,092 input tokens (3.1%). The
probe also proved that the enabled turn ran the isolated child and that only the
main provider request received its memory context. `--no-memory` ran neither
preprocessing nor injection.

This is a deterministic regression baseline, not evidence for enabling memory
by default. It intentionally exposes the child-process/token latency tradeoff;
the default remains off until representative real-provider and workload data
supports a separate compatibility decision.
