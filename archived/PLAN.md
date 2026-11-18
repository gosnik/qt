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

## TUI responsiveness follow-up scan

- [x] Audit remaining work that grows with history, response length, editor
  contents, paste payloads, input fragmentation, and event queues.
- [x] Measure warmed transcript/overlay rendering, unchanged compaction,
  paste editing, and assistant content updates using current native objects.
- [x] Independently check findings and publish the ranked scan in PERFORMANCE.md.

This is a scan, with production code preserved. Temporary benchmark sources
and executables are under `/tmp/qt-tui-responsiveness-scan`. Measurements isolate
CPU-side component/session work rather than end-to-end live terminal latency.
Both temporary benchmarks compiled and passed; independent review confirmed the
ranked findings and their frequency/complexity claims.

## TUI responsiveness fix pass

Preserve all existing uncommitted fixes and Pi-visible behavior while closing
all measured and source-confirmed follow-up findings. No new dependencies.

- [ ] Reuse assistant widgets and make streaming layout/update work incremental
  or viewport-bounded; preserve Markdown, thinking, message/tool order and final output.
- [ ] Cache paste marker metadata, batch ordinary input mutation/notifications,
  avoid unused full-text callbacks, and bound ordinary undo retention.
- [ ] Make transcript overlay/manual-scroll frame assembly viewport-aware with
  cached line counts and an incremental tool-compaction frontier.
- [ ] Remove full-session copies for footer/extension counts and update statistics
  incrementally while preserving compaction/session-switch semantics.
- [ ] Replace prefix-erasure text/Markdown wrapping with offsets/views and make
  fragmented control/paste parsing linear in received bytes.
- [ ] Move recursive attachment completion off the UI thread with bounded,
  cancellable work, stale-result rejection, and nonblocking forced Tab.
- [ ] Move prompt-history lock/read/rewrite/fsync off the UI path while preserving
  ordering, bounded retention, cross-process locking, errors and clean shutdown.
- [ ] Bound input/live-event/UI-post/extension-prompt producer queues without
  losing committed content or lifecycle events; avoid vector front erasure.
- [ ] Add meaningful regressions, rerun benchmarks and native/framework/TUI checks,
  obtain independent review, document compatibility/performance, restore binary.

## Deep TUI scan

- [x] Scan native input/editor, rendering/ANSI/Unicode, overlays, notcurses,
  images, streaming, extension input, and shutdown against checked-in Pi contracts.
- [x] Reproduce scan findings and run the existing seven focused TUI probes.
- [x] Make ANSI width, truncation, wrapping, Markdown, diff, and overlay scanners
  advance on unsupported or incomplete ESC sequences.
- [x] Own extension rewritten input until decoder consumption completes.
- [x] Preserve separate assistant/thinking responses around tool calls; own
  assistant message snapshots across source mutation and destruction.
- [x] Share immutable large-paste payloads across undo snapshots rather than
  copying megabytes on each edit.
- [x] Retain fragmented UTF-8 until complete instead of interpreting lead bytes
  as Meta keys.
- [x] Deduplicate Kitty printable input by decoded Unicode codepoint.
- [x] Normalize bracketed CRLF paste once, preserving its line count.
- [x] Coalesce typed-word undo boundaries with whitespace/non-typing boundaries.
- [x] Strip terminal controls from notcurses text operations while retaining SGR.
- [x] Restore inherited SGR and OSC8 state at uncovered overlay tails.
- [x] Use pinned Unicode properties for combining-mark display widths.
- [x] Cache one image render variant keyed by constraints, terminal capabilities,
  and cell dimensions; preserve Kitty image identity.
- [x] Coalesce assistant/thinking content rebuilds within each drained batch.
- [x] Cancel provider/tool/bash work before exit waits and teardown, including
  fatal-error paths; verify delayed provider exit, tool tokens, and active bash.
- [x] Complete native/framework/focused checks, independent review,
  compatibility/performance documentation, and production binary restoration.

Verification: native build and focused editor/input/render/image/model/
extension-input probes passed. A 1 MiB paste followed by 128 independent undo
snapshots stays below the Linux 24 MiB incremental peak-RSS guard (the original
100-edit reproducer grew about 96 MiB). Render regressions cover malformed ESC,
notcurses controls, combining width, overlay SGR/OSC8, wrapped hyperlinks, and
owned assistant snapshots. PTY checks cover assistant/tool scrollback ordering
and exact CRLF provider payloads. Cross-thread shutdown tool-token regression
passes in both 128-turn cache modes. Independent review cleared the final API
and cleanup. Framework smoke and prompt-history filters passed after fixing a
stale-output acknowledgement race in the prompt-history probe.

The final PTY probe passes delayed-provider Ctrl-D/SIGTERM exit, exit after a
recoverable editor error, and active bash cancellation with restored termios
and no surviving bash process. The production binary was restored. Existing
resize and earlier runtime-retention work was preserved. No live RSS plateau, sanitizer coverage,
or exhaustive terminal/backend matrix is claimed; conversation and transcript
content still scales with session length. Fatal-error cleanup was independently
reviewed, without fault-injected terminal I/O coverage.

## Long-session memory growth

- [x] Bound runtime-host diagnostic hook histories and full-session save-point
  snapshots; preserve live hook delivery, latest provider inspection, session
  persistence, and monotonically increasing save-point sequences. Keep explicit
  full-history capture available for low-level diagnostic probes.
- [x] Add a repeated-turn regression that checks retention, hook delivery,
  save-point sequencing, move behavior, and persisted conversation continuity.
- [x] Run native build, focused probes, framework smoke, and independent review;
  document the compatibility change and remaining memory-growth limitations.

Verification: native build and framework smoke passed; the cache probe verifies
128 turns through the production runtime factory, bounded diagnostic/save-point
retention, every hook delivery, sequence continuity, and complete session
messages. Streaming completion probe and whitespace checks passed. Independent
review found no correctness blocker; its factory-coverage gap was closed and
both focused probes passed again. Production executable was restored. Retained
conversation/TUI content still scales with session length; no live RSS plateau
or idle-growth diagnosis is claimed.

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
