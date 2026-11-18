# Dependencies

All crates are declared once in the root `Cargo.toml` under `[workspace.dependencies]`
and pinned by `Cargo.lock`. Add a crate only together with an entry here.

## UI (`qt-app`)

| Crate | Why |
| --- | --- |
| `gpui-kit` 0.10 | Pins GPUI (`gpui-pre` 0.3.12), `gpui-base`, `gpui-component` and the Lucide icon assets. Provides the window, markdown `TextView`, `Textarea`, `Select`, `MessageScroller`, dialogs, notifications and themes. |

## Core runtime (`qt-core`)

| Crate | Why |
| --- | --- |
| `tokio`, `tokio-util` | Async runtime, processes, timers; `CancellationToken` for aborts |
| `reqwest` (rustls) | HTTPS streaming to model providers |
| `futures`, `async-channel` | Stream handling; executor-agnostic event channel to the UI |
| `serde`, `serde_json` | Session, settings and wire formats (`preserve_order` keeps settings key order) |
| `anyhow`, `thiserror` | Error handling |
| `chrono` | Timestamps and prompt date |
| `dirs` | Home directory lookup |
| `ignore`, `globset`, `regex` | `.gitignore`-aware `grep`/`find`, glob and regex matching |
| `similar` | Unified diffs for `edit` results |
| `base64` | Image attachments |
| `rand` | Session entry ids |
| `libc` (unix) | Killing a bash process group on timeout or abort |
| `tempfile` (dev) | Test fixtures |

## System packages

- **Linux:** `pkg-config`, a C toolchain, and development files for xkbcommon (including
  `libxkbcommon-x11-dev`), Wayland, xcb, Vulkan, fontconfig, freetype and OpenSSL. See the
  README for the apt command. At runtime GPUI needs a Vulkan driver; software rendering
  (Mesa lavapipe) works, for example under Xvfb.
- **macOS:** Xcode command-line tools (Metal is used for rendering).
- **Windows:** MSVC build tools (DirectX is used for rendering).

The archived C++ implementation's dependencies are documented in
`archived/docs/dependencies.md`.
