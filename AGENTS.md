# qt Agent Instructions

## Scope & Priorities
- **Target:** Rust workspace at the repo root (`crates/qt-core`, `crates/qt-app`, `crates/qt-cli`), with a GPUI desktop UI.
- **Parity Source:** `reference/pi` (TypeScript). Treat it as behavioral truth for tools, sessions, prompts and agent-loop semantics. Do not edit unless explicitly requested.
- **Archived Implementation:** `archived/` holds the previous C++17/TUI qt. Use it as a reference for qt-specific behavior (agent home layout, settings keys, memory, extensions, llama.cpp integration). Do not build, modify or extend it.
- **Secondary References:** `archived/REFS.md` indexes approved non-Pi reference material when the primary source is silent or ambiguous.
- **Dependency Lock:** Use only crates declared in the root `Cargo.toml` `[workspace.dependencies]` and documented in `docs/dependencies.md`. Adding a crate requires updating both, with a one-line justification.

## Parity Workflow
1. **Locate Contract:** Find the reference contract in `reference/pi/packages/**` or `reference/pi/docs/**`.
2. **Locate Native Owner:** Find the destination module (`qt-core` for anything that is not presentation, `qt-app` for GPUI views, `qt-cli` for headless output).
3. **Record the Gap:** Add or update the item in `PLAN.md` with acceptance criteria before coding.
4. **Behavior Over Structure:** Replicate observable outcomes (tool output wording, session files, request shapes). Do not port TypeScript structure line by line.

## Architecture Directives
- **Core stays UI-agnostic:** `qt-core` must not depend on GPUI. Front ends talk to it only through `Engine`, `Conversation` and `AgentEvent`.
- **Async boundary:** network and tool work runs on the engine's Tokio runtime; GPUI code awaits `async_channel` receivers or Tokio `JoinHandle`s and never blocks the UI thread.
- **Ownership:** prefer owned data and explicit `Arc`/`Mutex` boundaries; hold locks only for short, non-`await` critical sections.
- **Lean interfaces:** use traits only where runtime dispatch is needed (`Tool`). No god objects or single-implementation wrappers.
- **Errors:** model-facing failures become tool or assistant error messages; user-facing failures become GPUI notifications. No panics on user input or network data.

## State Tracking (`PLAN.md`)
- Read `PLAN.md` before writing implementation code.
- Mark `[x]` only when code and edge cases fully satisfy the item; otherwise leave `[ ]` and state the gap.

## Verification & Definition of Done
- `cargo fmt --all --check`
- `cargo clippy --workspace --all-targets` (no warnings)
- `cargo test --workspace`
- For UI changes, run the app (`cargo run -p qt-app`) and check the affected view. Never inject synthetic input into the user's live desktop session; use an isolated display (for example `Xvfb :99`) for scripted UI checks. Launch the app there with `env -u WAYLAND_DISPLAY DISPLAY=:99 ...`: GPUI prefers Wayland, so a leftover `WAYLAND_DISPLAY` opens the window on the user's desktop.
- Update `docs/upstream-compatibility.md` whenever behavior relative to Pi or the archived qt changes.
