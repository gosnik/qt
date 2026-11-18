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

## In-process llama.cpp (`qt-llama-sys`, `qt-llama`)

| Crate | Why |
| --- | --- |
| `bindgen` 0.72 (build only) | Generates `qt-llama-sys` FFI bindings for `llama.h`, `ggml-backend.h` and the C shim over `llama-ext.h`, so struct layouts always match the pinned fork |

`qt-llama` otherwise reuses workspace crates (`serde_json`, `regex`, `thiserror`, `tokio`,
`tokio-util`, `dirs`). The C++ library is the PrismML llama.cpp fork, a git submodule at
`third_party/llama.cpp` pinned to `9fcaed763ccda38ea81068ad9d7f991aaddca451` (it adds the
group-128 `Q2_0` ternary format with CPU, Metal, CUDA and Vulkan kernels).
`crates/qt-llama-sys/build.rs` builds it with CMake as static libraries, replacing the
archived `scripts/build_llama_cpp.py`:

- Source: `QT_LLAMA_CPP_DIR` (default `third_party/llama.cpp`). Without the source the crates
  still build, without llama.cpp, and model calls report why (`QT_LLAMA_REQUIRE=1` makes a
  missing source fatal instead).
- GPU backends: `QT_LLAMA_GPU_BACKENDS=auto` (default) enables CUDA, HIP, Vulkan or Metal when
  the SDK and a device are present; `cpu`/`off`/`none` force CPU only; `all` or a list such as
  `cuda,vulkan` request backends explicitly. An auto-detected GPU build that fails is retried
  CPU-only. `cargo build -vv -p qt-llama-sys` prints the selection, and runtime status reports
  `compiledBackends`. `QT_LLAMA_BUILD_PROFILE=full` is the release profile (CUDA+Vulkan+HIP, CUDA
  architectures `75;80;86;89;90`, NCCL/RCCL off).
- `QT_LLAMA_CMAKE_ARGS` passes extra CMake arguments, `QT_LLAMA_CMAKE_GENERATOR` picks the
  generator (Ninja when installed) and `QT_LLAMA_BUILD_JOBS` the parallelism.
- Build tools: `cmake`, a C/C++17 compiler and libclang (for bindgen). Vulkan needs the
  loader and headers, `glslc` and SPIR-V headers (`libvulkan-dev glslc spirv-headers` on
  Debian/Ubuntu); CUDA/HIP need their toolkits. `QT_LLAMA_LOG=1` keeps llama.cpp's log output.

## System packages

- **Linux:** `pkg-config`, a C toolchain, and development files for xkbcommon (including
  `libxkbcommon-x11-dev`), Wayland, xcb, Vulkan, fontconfig, freetype and OpenSSL. See the
  README for the apt command. At runtime GPUI needs a Vulkan driver; software rendering
  (Mesa lavapipe) works, for example under Xvfb.
- **macOS:** Xcode command-line tools (Metal is used for rendering).
- **Windows:** MSVC build tools (DirectX is used for rendering).

The archived C++ implementation's dependencies are documented in
`archived/docs/dependencies.md`.
