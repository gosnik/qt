# TUI Performance Analysis and Status

Last updated: 2026-07-17

This document tracks the current TUI render-path performance work. It is both
the analysis record and the implementation status checklist for the bottlenecks
found in the native interactive TUI.

## Critical Path

The hot path is:

```text
InteractiveTerminalSession::render()
  -> OverlaySurface::render()
  -> InteractiveTerminalSession::base_render_buffer()
  -> DiffRenderer::present()
  -> DiffRenderer::diff()
  -> NotcursesSessionAdapter / ANSI presenter
```

The main costs are repeated transcript wrapping, repeated bottom prompt/footer
rendering, row normalization in the diff renderer, editor visual wrapping, and
notcurses plane/plan churn.

## Implementation Status

| Item | Status | Evidence |
| --- | --- | --- |
| Cache normalized diff rows | Done | `DiffRendererNormalizedRowCacheEntry` in `include/qt/tui/terminal/diff_renderer.hpp`; carried through `previous_row_cache_` and `next_row_cache_` in `src/tui/terminal/diff_renderer.cpp`. |
| Remove full `RenderBuffer` copy for cursor extraction | Done | `find_cursor_marker()` scans the next buffer without mutating it; `diff()` no longer creates `next_without_cursor`. |
| Cache stable transcript line rendering | Done | `TranscriptRenderCacheEntry` in `InteractiveTerminalSession`; line entries cache wrapped/styled output by width. Mutable component entries remain live. |
| Skip old transcript frame assembly in native scrollback mode | Done | `InteractiveTerminalSession::base_render_buffer()` renders only the fixed live region while terminal scrollback owns transcript history. |
| Cache bottom prompt/footer rendering within a frame | Done | `BottomRenderCache` caches `bottom_render_lines()` by constraints and is invalidated at the start of each render and on relevant state changes. |
| Cache interactive footer session statistics across idle frames | Done | `interactive_mode` refreshes `cached_footer_lines` only after turn, bash, slash-command, model/thinking selector, session-switch, compaction, or provider-status changes. |
| Avoid cached bottom line copies for sizing | Done | `bottom_render_lines_ref()` returns the cached bottom line vector by reference for line-count and layout callers. |
| Cache editor visual line layout | Done | `VisualLayoutCache` in `Editor`, keyed by text revision and width; cursor-only changes update cached decoration in place. |
| Cache editor grapheme boundaries | Done | `GraphemeBoundaryCacheEntry` builds each changed logical row in one Unicode-safe pass and serves binary-search left/right movement plus linear wrapping. |
| Avoid interactive frame handoff copy | Done | `InteractiveTerminalSession::render()` moves the rendered frame into `DiffRenderer` and presents the retained current frame through `previous_frame()`. |
| Reuse notcurses live-region plane | Done | `NotcursesBackend::present_at()` moves/resizes the live plane and only recreates it after resize failure or first use. |
| Make notcurses `last_plan_` diagnostic-only | Done | Active non-dry-run rendering uses a local plan and clears `last_plan_`; dry-run keeps `last_plan_` for probes. |
| Remove fuzzy swapped-query regex | Done | `swapped_alpha_numeric_query()` in `src/tui/utils/fuzzy.cpp` now uses manual ASCII scanning. |
| Full notcurses row-dirty rendering | Done | `notcurses_render_plan(const FrameDiff&)` converts only `FrameDiff::updates` into notcurses operations; `NotcursesSessionAdapter::present(buffer, frame_diff)` uses it for notcurses rendering. |
| Markdown AST/render caching | Done | `Markdown::RenderCache` stores rendered buffers by revision and constraints; `Markdown::render()` no longer runs a separate validation parse before the md4c document parse. |
| Streaming frame-rate throttle | Done | `LiveTurnTranscript` exposes pending/immediate checks; ordinary content deltas are coalesced behind a 33 ms render gate while final/structural events force a render. |
| Async autocomplete/fuzzy candidate cache | Done | Generic fuzzy filters use `PreparedFuzzyQuery` / `PreparedFuzzyText`; filesystem autocomplete prepares query and file-entry match text once per refresh/candidate. |
| Editor word-segment cache | Done | `Editor::word_segments_for_row()` caches per-row word segments by line text and is used by word movement and word deletion. |
| Shared zero-copy `split_lines()` | Done | `include/qt/tui/utils/line_split.hpp` provides `split_lines_view()` and `split_lines_copy()`; hot TUI/editor/markdown split helpers now route through it. |
| Frame metrics instrumentation | Done | `RenderMetricsRecorder` tracks render/transcript/bottom/diff/present timings and row counts; `InteractiveTerminalSession` exposes snapshot/reset accessors. |

## Completed Changes

### Diff Renderer

Files:
- `include/qt/tui/terminal/diff_renderer.hpp`
- `src/tui/terminal/diff_renderer.cpp`

Changes:
- Added normalized row caches for previous and next frames.
- Reuses cached normalized rows when the raw row text and width are unchanged.
- Carries the next-frame row cache forward after `present()`.
- Replaced cursor extraction via full buffer copy with non-mutating marker
  scanning.
- Interactive rendering now moves the completed frame into `DiffRenderer` and
  presents the retained current frame instead of copying the full buffer for the
  diff/present handoff.

Expected impact:
- Reduces repeated ANSI parsing, width measurement, padding, and reset appends
  for stable rows.
- Avoids copying every row string just to remove the cursor marker.

Remaining limitation:
- `contains_sgr()` is still evaluated in `diff_row()` when comparing changed
  rows. This is acceptable for now because the row cache reduces normalization
  churn first; a later row-metadata cache can store `has_sgr` too.

### Transcript Rendering

Files:
- `include/qt/app/interactive/interactive_terminal_session.hpp`
- `src/app/interactive/interactive_terminal_session.cpp`

Changes:
- Added `TranscriptRenderCacheEntry` for stable line entries.
- Cache key is `(entry index, max_width, line text)`.
- Live mutable component entries still render every frame so thinking/tool-call
  widgets continue to update.

Expected impact:
- Historical transcript entries no longer re-wrap and restyle every frame.
- Streaming turns still repaint live tail components correctly.
- Native scrollback mode no longer walks and copies every cached transcript
  entry into each frame when the user is at the live bottom; only newly appended
  transcript entries are rendered once during scrollback flush.

Remaining limitation:
- Component-level caching needs explicit component revision support before it is
  safe to cache mutable tool/thinking widgets.

### Bottom Region Rendering

Files:
- `include/qt/app/interactive/interactive_terminal_session.hpp`
- `src/app/interactive/interactive_terminal_session.cpp`

Changes:
- Added `BottomRenderCache` keyed by render constraints.
- Cache is invalidated at the start of each render pass and when editor,
  status, transcript, overlay, or resize state changes.
- Added a cached-line reference helper so line-count and live-region sizing
  callers do not copy the cached bottom vector just to inspect its size.
- The interactive footer provider returns its existing cached lines during
  cursor-only frames. Session statistics and footer data refresh at explicit
  state-change sites instead of copying and scanning every session entry for
  each keypress.

Expected impact:
- `bottom_render_lines()` no longer re-renders pending messages, status,
  editor, and footer multiple times for identical constraints during one frame.
- Idle cursor movement no longer recomputes full-session token statistics.
- Native fallback-terminal PTY measurements improved median Left latency from
  73.55 ms to 17.80 ms for a 21-byte prompt, 95.82 ms to 40.12 ms for 500-byte
  ASCII, and 78.25 ms to 21.50 ms for 500-byte Unicode (2.39-4.13x faster).

### Editor Visual Layout

Files:
- `include/qt/tui/input/editor.hpp`
- `src/tui/input/editor.cpp`

Changes:
- Added `VisualLayoutCache`.
- Cache key is `(visual_layout_revision_, width)`; cursor row/column decorate the
  cached geometry without rebuilding it.
- Cached visual lines are returned by reference instead of copying every
  wrapped string.
- Added per-row grapheme-boundary caches. Left/right movement uses binary
  search, and wrapping consumes the same boundaries in a single pass instead
  of rescanning the row from byte zero for every grapheme.
- `notify_change()` and non-notifying content replacement invalidate layout.

Expected impact:
- `render()`, `is_on_first_visual_line()`, `is_on_last_visual_line()`, and
  `move_visual()` reuse the same visual wrap layout across cursor-only changes.
- Horizontal cursor movement no longer rebuilds quadratic Unicode wrap state
  for long single-line prompts.
- `python3 scripts/editor_cursor_probe.py` verifies Unicode cluster movement,
  logical-line transitions, cursor rendering, and 8 KiB/32 KiB scaling.

### Notcurses Backend

Files:
- `src/tui/terminal/notcurses_backend.cpp`

Changes:
- `present_at()` keeps the live-region plane persistent.
- Existing plane is resized and moved when possible.
- Plane is recreated only when absent or resize fails.
- Active non-dry-run rendering no longer stores `last_plan_`; dry-run keeps it
  for probe/debug assertions.

Expected impact:
- Avoids per-frame `ncplane_destroy()` / `ncplane_create()` churn in the fixed
  live region.
- Reduces diagnostic allocation during normal rendering.

Remaining limitation:
- Scrollback live-region repainting still uses the existing direct ANSI
  `present_at()` path because that surface is intentionally hybrid
  scrollback/direct output.
- The full-buffer `present()` path remains available for diagnostics and
  callers that do not yet own a `FrameDiff`.

### Dirty-Row Notcurses Rendering

Files:
- `include/qt/tui/terminal/notcurses_backend.hpp`
- `src/tui/terminal/notcurses_backend.cpp`
- `src/app/interactive/interactive_terminal_session.cpp`

Changes:
- Added `notcurses_render_plan(const FrameDiff&)`.
- Added `NotcursesBackend::present(const FrameDiff&)`.
- Added `NotcursesSessionAdapter::present(buffer, frame_diff)` and routed the
  interactive session through it for non-scrollback notcurses rendering.
- `full_redraw` frames still erase the plane; ordinary frames apply only row
  updates from `FrameDiff::updates`.

Expected impact:
- Avoids rebuilding notcurses text operations for unchanged rows.
- Avoids `ncplane_erase()` on ordinary frames, reducing repaint churn and
  preserving stable plane contents between row updates.

Remaining limitation:
- Fixed bottom scrollback mode still repaints the live region directly because
  the transcript area is written to native scrollback and the bottom region is a
  separate overlay.

### Fuzzy Matching

File:
- `src/tui/utils/fuzzy.cpp`
- `include/qt/tui/utils/fuzzy.hpp`

Changes:
- Removed `std::regex` from swapped alpha/numeric query detection.
- Manual ASCII scanning preserves the previous matching contract:
  `letters + digits` swaps to `digits + letters`, and vice versa.
- Added `PreparedFuzzyQuery` and `PreparedFuzzyText`.
- Generic `fuzzy_filter_scored()` prepares each query token once and each
  candidate text once before matching all tokens.
- ASCII-only input uses a lowercase fast path before falling back to Unicode
  NFKC/casefold normalization.
- Filesystem autocomplete prepares the query tail once per refresh, prepares
  filename/path match data once per candidate, and reuses normalized path text
  for exact/prefix/substring/fuzzy scoring.
- Direct directory completion prepares the normalized search-prefix graphemes
  once instead of rebuilding them for every directory entry.

Expected impact:
- Removes regex construction/execution overhead from each fuzzy match attempt.
- Reduces repeated normalization and grapheme allocation in generic fuzzy
  filtering, including selector/search uses.
- Reduces repeated normalization and fuzzy-query preparation while scanning
  filesystem autocomplete candidates.

Remaining limitation:
- `fuzzy_match()` still prepares both sides for direct one-off callers; hot
  callers should use the prepared API.

### Editor Word Segments

Files:
- `include/qt/tui/input/editor.hpp`
- `src/tui/input/editor.cpp`

Changes:
- Moved `WordSegmentKind` and `WordSegment` into the editor header so `Editor`
  can own a cache.
- Added `WordSegmentCacheEntry` and `Editor::word_segments_for_row()`.
- Routed Ctrl+Left, Ctrl+Right, backward word deletion, and forward word
  deletion through cached row segments.
- Cache entries validate against the current row text, so changed or shifted
  rows refresh automatically.

Expected impact:
- Repeated word navigation/deletion on the same logical line no longer rebuilds
  the Unicode word segment vector each time.

Remaining limitation:
- The cache is per row and line text, not per fine-grained line revision. That
  keeps invalidation simple while avoiding stale results.

### Markdown Render Cache

Files:
- `include/qt/tui/render/markdown.hpp`
- `src/tui/render/markdown.cpp`

Changes:
- Added a per-component `Markdown::RenderCache` keyed by text/style revision,
  `max_width`, and `max_height`.
- `set_text()`, `set_padding()`, and `set_background()` invalidate the cache
  by bumping the revision.
- Removed the duplicate no-op md4c validation parse from `render()`; the real
  document parse is now the single parse/error source.

Expected impact:
- Stable markdown components no longer reparse and rewrap on every render pass.
- Each markdown render now avoids the old second full-document `md_parse()` call
  even on a cache miss.

Remaining limitation:
- Streaming text still invalidates the whole markdown component when the text
  itself changes. A future tail-block parser can reuse stable prefix blocks.

### Streaming Frame Throttle

File:
- `src/app/interactive/interactive_mode.cpp`

Changes:
- Added pending/immediate update checks to `LiveTurnTranscript`.
- Added a 33 ms streaming frame interval around live transcript flushes.
- Ordinary content deltas remain queued until the next frame slot.
- Tool starts/completions, provider block completion, retry/error events, and
  forced final turn flushes bypass the throttle.

Expected impact:
- Coalesces high-rate model/thinking/tool-output deltas into roughly 30 FPS
  presentation instead of rendering each delta immediately.
- Keeps important structural state transitions visible without waiting for the
  next frame slot.

Remaining limitation:
- This throttles live agent transcript updates. Other non-streaming status
  renders still happen immediately by design.

### Shared Line Splitting

Files:
- `include/qt/tui/utils/line_split.hpp`
- `src/app/interactive/interactive_terminal_session.cpp`
- `src/app/interactive/interactive_mode.cpp`
- `src/tui/input/editor.cpp`
- `src/tui/render/markdown.cpp`

Changes:
- Added `split_lines_view()` for zero-copy line views with configurable CR and
  trailing-empty behavior.
- Added `split_lines_copy()` as the common copy wrapper for callers that need
  owned strings.
- Routed TUI transcript, editor, interactive mode, and markdown splitting
  through the shared utility.
- The transcript wrapping loop now iterates `split_lines_view()` directly for
  per-line wrapping/styling.

Expected impact:
- Removes repeated hand-rolled splitting logic from hot TUI paths.
- Avoids intermediate string allocation in the transcript wrapping loop when a
  stable source line can be split as views.

Remaining limitation:
- Tool/index helpers still have domain-specific splitters where behavior differs
  for truncation, empty-line dropping, or line-ending preservation.

### Frame Metrics

Files:
- `include/qt/tui/terminal/render_metrics.hpp`
- `include/qt/app/interactive/interactive_terminal_session.hpp`
- `src/app/interactive/interactive_terminal_session.cpp`

Changes:
- Added `RenderMetricsRecorder` and `RenderMetricsSnapshot`.
- Recorded total render, transcript render, bottom render, diff, and present
  durations in microseconds.
- Recorded diff rows scanned and rows changed.
- Exposed `InteractiveTerminalSession::render_metrics()` and
  `reset_render_metrics()` for diagnostics and future debug surfaces.

Expected impact:
- Gives future profiling and regression checks a central counter surface without
  emitting diagnostics during normal interactive use.

Remaining limitation:
- Markdown time is currently included in transcript/component rendering rather
  than tracked as a separate counter.
- Terminal byte counts are still renderer/backend specific and are not yet
  exposed through this metrics surface.

## Remaining Backlog

The earlier optimization pass is complete. Remaining workload-dependent costs
are recorded in the responsiveness follow-up scan below.

## Verification

Commands used for this pass:

```sh
pio run -e native
scripts/tui_pty_probe.py --binary .pio/build/native/qt
git diff --check
```

Expected coverage:
- `pio run -e native` verifies compile/link across the native app.
- `scripts/tui_pty_probe.py` verifies slash overlay rendering, DECSTBM/native
  scrollback behavior, and tmux capture-pane behavior.
- `git diff --check` verifies whitespace hygiene.

## Completion Checklist

| Requirement | Evidence |
| --- | --- |
| Document the top TUI performance bottlenecks | This file lists completed and remaining bottlenecks with file-level owners. |
| Update the document as fixes land | Completed fixes are marked `Done` with concrete code evidence. |
| Preserve known TUI behavior | PTY/tmux probe covers slash overlay and native scrollback behavior. |
| Keep future work actionable | Remaining backlog includes next implementation steps and risk notes. |
| Keep repo hygiene clean | `git diff --check` must pass after this file is updated. |

## Deep TUI scan fix pass

Undo snapshots now share immutable large-paste payloads rather than duplicating
those payloads per edit. Typed words coalesce undo entries. Image components
retain only their latest render variant, keyed by constraints, capabilities, and
cell dimensions. Streaming assistant/thinking widgets rebuild content once per
drained batch and retain separate components across provider responses.

Malformed escape sequences cannot stall the audited width, wrapping, truncation,
overlay, diff, or Markdown scanners. Exit requests cooperative cancellation
before joining active work. Focused regressions are listed in
`docs/reference-test-map.md`.

Conversation and transcript content still grow with session length. These
changes do not establish a live process RSS plateau or bound the size of an
individual provider request, tool result, or retained transcript component.

Verification passed: native build; editor/input, render, image-cache, model
selector, extension-input, and interactive-final probes; 128-turn harness cache
regressions including cross-thread tool cancellation; framework smoke and
prompt-history filters; independent review; and whitespace checks. The Linux
large-paste guard exercises 128 independent undo snapshots after a 1 MiB paste
and requires less than 24 MiB incremental peak RSS. Delayed-provider and active
bash PTY exits restore terminal input state within three seconds.

## TUI responsiveness follow-up scan

The current native sources still contain work that grows with history or active
content size. This scan preserves the existing implementation and prior fixes.
The highest-priority remaining costs are streaming response rebuilds, large-paste
marker metadata scans, and full-history rendering during overlays/manual scroll.

### Measured costs

Temporary benchmarks used current native objects, synthetic content, an 80x24
viewport, and steady-clock averages. History rendering used a NullSink, warmed
caches, and ten frames per measurement. Compaction used 100 calls. Paste editing
used ten operations; assistant updates/rendering used three operations. These
numbers exclude real terminal I/O, network/provider delays, and filesystem I/O.
They are workload measurements on this build, not portable latency guarantees.
Sources and executables are under `/tmp/qt-tui-responsiveness-scan`.

| Historical entries (120 ASCII characters each) | Native scrollback frame | Overlay frame | Unchanged compaction scan |
| --- | ---: | ---: | ---: |
| 100 | 1.024 ms | 4.689 ms | <0.001 ms |
| 1,000 | 0.711 ms | 4.905 ms | 0.001 ms |
| 10,000 | 0.833 ms | 7.940 ms | 0.020 ms |
| 50,000 | 0.882 ms | 21.222 ms | 0.169 ms |

| Retained marker-paste payload plus 100 ordinary characters | Cursor pair + render | Edit + render |
| --- | ---: | ---: |
| None | 0.313 ms | 0.337 ms |
| 1 MiB | 0.525 ms | 2.448 ms |
| 8 MiB | 6.604 ms | 53.863 ms |

| Assistant text | Content update + render, words | Content update + render, unbroken text | Cached render |
| --- | ---: | ---: | ---: |
| 10,000 bytes | 9.087 ms | 8.210 ms | 1.622–1.809 ms |
| 100,000 bytes | 61.077 ms | 56.300 ms | 1.695–1.807 ms |
| 1,000,000 bytes | 587.982 ms | 614.122 ms | 1.758–1.761 ms |

### Ranked remaining findings

1. **High: streaming still reparses and wraps the entire response per content
   flush.** `LiveTurnTranscript::flush_message_content_locked`
   (`src/app/interactive/interactive_mode.cpp:1010`) builds the complete message;
   `AssistantMessageComponent::update_content/rebuild`
   (`src/tui/widgets/messages/assistant_message.cpp:98,132`) copies it and replaces
   the Markdown widgets. Their caches are lost. `Markdown::render`
   (`src/tui/render/markdown.cpp:1646`) parses and wraps all content before
   applying the height limit. The prior batch fix removes per-delta rebuilds,
   but each dirty flush still costs the full accumulated response. For fixed
   output chunks this yields quadratic total prefix work across the response.
   The measured 100,000-byte update exceeds the 33 ms streaming frame interval.
   Preserve widgets/cache state and investigate incremental parsing/layout or
   rendering only required visible rows.

2. **High for large pasted prompts: marker lookup reparses the payload repeatedly.**
   `Editor::valid_paste_marker_at`, `valid_paste_marker_ending_at`, and
   `paste_marker_containing` (`src/tui/input/editor.cpp:1490,1508,1528`) call
   `split_lines(*paste.second)` to recreate marker metadata. Visual layout calls
   marker lookup at each grapheme (`:1317`), including ordinary text after a
   marker. Editing invalidates the layout (`:1856`). Sharing immutable payloads
   fixed snapshot duplication but leaves this multiplicative scan/allocation
   cost. Cache line count and marker text at paste insertion and index markers.

3. **Medium: overlay/manual-scroll frame assembly copies complete history.**
   `InteractiveTerminalSession::base_render_buffer`
   (`src/app/interactive/interactive_terminal_session.cpp:1322`) selects full
   history outside the native fast path and during ANSI overlay repaint or
   restoration. `transcript_render_lines` copies cached line strings (`:1395`)
   before `layout_lines` trims to the viewport (`:1627`). Manual scrolling also
   materializes the full transcript to determine height (`:1936`). Normal native
   scrollback stays flat in the benchmark; overlay costs scale with history.
   Keep line-count metadata and copy/render only the required visual window.

4. **Medium: session refresh/persistence copies full entry payloads.**
   `compute_footer_lines` (`src/app/interactive/interactive_mode.cpp:6088`)
   calls the by-value `SessionManager::get_entries`
   (`include/qt/harness/session_manager.hpp:38`) before the full stats scan
   (`src/harness/session.cpp:1601`). Extension lifecycle/shortcut paths also call
   it merely for `.size()`; `persist_native_extension_entries` (`:135`) additionally
   synchronizes/copies the session manager. Footer refresh is cached and runs on
   relevant state changes, not every idle frame. Nonetheless, completed turns,
   status changes, and extension interactions can pause increasingly with total
   session payload size. Use the existing const-reference `entries()` accessor
   or count accessor where appropriate, and incremental stats where possible.

5. **Medium for long edited prompts: full-document mutation and unbounded undo.**
   `Editor::insert_typed_text` (`src/tui/input/editor.cpp:1012`) mutates and notifies
   once per grapheme. `insert_text_impl` (`:1046`) copies the active line, and
   `notify_change` (`:1861`) materializes full text even though the production
   session callback ignores its argument (`interactive_terminal_session.cpp:391`).
   A large ordinary input batch can therefore do quadratic copying.
   `capture_undo_state` (`editor.cpp:967`) still copies all ordinary document lines
   into an unbounded `UndoStack` (`include/qt/tui/input/undo_stack.hpp:16`). Word
   coalescing and shared paste payloads help, but independent edits to a large
   normal prompt retain O(edit boundaries × document size) snapshots. Batch
   mutation/notification and consider bounded/delta undo without changing semantics.

6. **Secondary source-confirmed costs needing workload profiling:**
   - `compact_transcript_tool_entries` (`interactive_terminal_session.cpp:807`)
     rescans all eligible history on prompt submission and turn completion even
     when no entry changes. Its measured cost is much smaller than rendering;
     a compaction frontier would avoid cumulative scans.
   - `Text`/Markdown `split_chunk` (`src/tui/render/text.cpp:177`,
     `src/tui/render/markdown.cpp:1024`) repeatedly erase the string prefix, moving
     its remaining suffix. Long unbroken tokens have quadratic wrapping-copy
     work at fixed width; use offsets/views. The measured unbroken-text case
     was not substantially worse than ordinary words at tested sizes.
   - Incomplete CSI/OSC/DCS and bracketed paste are scanned from the beginning
     on each new fragment (`src/tui/input/stdin_buffer.cpp:62,248`), producing
     quadratic work for pathological fragmentation; retain parser scan state.
   - Attachment completion synchronously runs `fd` with a 500 ms limit or a
     capped recursive walk (`src/tui/widgets/autocomplete.cpp:759`). Debouncing
     reduces frequency but forced Tab can still block the UI in large trees.
   - Prompt history append (`src/app/config/prompt_history.cpp:419`) takes an
     exclusive lock, reads/rewrites history, and fsyncs (`:285`) synchronously
     through the submission callback. Its 500-entry default bounds count, not
     payload bytes, so large prompts or slow storage can cause submit stalls.
   - `extension_prompts` is an uncapped vector drained with front erasure
     (`src/app/interactive/interactive_mode.cpp:12321`); bursts incur quadratic
     shifts and queued turns. Input, UI-post, and live-event queues are also
     unbounded during producer/consumer imbalance; backlogs need stress profiling.

Independent review verified the ranked findings and found no material false
positives. Both temporary benchmarks compiled and passed.

No independent idle-only slowdown or live RSS plateau was established. Source
inspection and measurements agree that retained content affects particular UI
operations rather than every normal native-scrollback frame. Previously fixed
ANSI progress, UTF-8 lifetime/input, image-cache, and shutdown defects were not
counted again in this follow-up.
