# Active implementation plan

## Workstream index

Use [TODO.md](TODO.md) for current tracking and verification entry points.
Memory contracts and remaining work are tracked in:

- [MEM.md](MEM.md): governed memory and Dreaming.
- [REVIEW.md](REVIEW.md): typed memory review workflows.
- [mem_v2.md](mem_v2.md): retrieval V2 and its still-open acceptance gates.
- [PERFORMANCE.md](PERFORMANCE.md): TUI performance status and verification.

Completed feature behavior belongs in
[upstream compatibility](docs/upstream-compatibility.md); reproducible coverage
belongs in the [test map](docs/reference-test-map.md). Avoid duplicating
chronological tool logs or completed-task diaries here.

## Public repository cleanup

- [x] Remove the legacy workstream directory and repair documentation links;
  preserve its uncommitted contents in an external recovery archive.
- [x] Remove superseded performance analysis, continuation handoff, and the
  accidental empty control-character filename; preserve recoverable copies
  outside the repository.
- [x] Remove machine-specific home paths from first-party documentation and
  source-provenance fixture metadata; ignore local credentials/runtime state.
- [x] Condense redundant rollout notes while preserving open acceptance gates.
- [x] Normalize generated dashboard package identities to omit developer paths.
- [x] Rewrite the README around first-time setup, model selection, usage,
  opt-in memory, development checks, and explicit release limitations.
- [x] Check documentation references, artifact/privacy findings, native build,
  framework smoke, and independent review.

Verification: documentation links and fixture JSON passed; native and WASM
Release builds passed; framework smoke passed (2/2); dashboard integration
probe passed. Loader syntax, absolute-path rejection, normalization idempotence,
ignore rules, and `git diff --check` passed. Scoped working-tree credential
checks found no matches; this was not a full-history secret audit. Independent
review checked the README commands and bundle build changes.

This cleanup does not publish the repository, rewrite Git history, change
licenses, or mark unfinished feature work complete. Before publication, review
historical secrets and the licensing of first-party and incorporated code.
