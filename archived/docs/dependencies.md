# Dependencies

Native builds use only the dependencies pinned in `platformio.ini`, repository
submodules, and the explicitly vendored browser-dashboard sources/assets below.

The built-in browser memory dashboard vendors the qtmem commit `3324ace`
ImGui 1.92.5 WebAssembly client, Emscripten SDL2/WebGL glue, and its JetBrains
Mono and DejaVu Sans font assets under `dashboard_wasm/`. Deterministic
`index.js`, `index.wasm`, and `index.data` outputs are checked in and embedded
by the native build, so Emscripten is not required for ordinary builds. Set
`QT_MEMORY_DASHBOARD_BUILD=1` to rebuild them; that optional workflow requires
Emscripten (`emcmake`/`emcc`), CMake 3.24 or newer, and Emscripten's SDL2 port.

## PlatformIO Libraries

- `argparse` pinned at `d924b84eba1f0f0adf38b20b7b4829f6f65b6570`
- `nlohmann/json` pinned at `4fad4468974a7b1b26d374b1c5955d2ac7d449b0`
- `spdlog` pinned at `2e71fdf3c1e8d6299b8129e402ab705e6fb494ba`
- `AngelScript` pinned at `0601da029d846a658bf23f2888e953a45a94450a`
- `uni-algo` pinned at `6c091fa266ac03a852128429af3b7481cf50a0ab`
- `md4c` pinned at `5f9b246fd01e90afa8075c1b6c2904ea44375fe3`
- `notcurses` pinned at `da0943ab11fb81874776455c6c02b89cace39eaa`
- `scnlib` pinned at `4b8a7400ce34208fa47b9abec492a6497a7ab48c`
- `lexy` pinned at `c1358c4117752393e9ce27d6c885d9146846f2f9`
- `recls` pinned at `33d5b03d1f789405385c462b7e43510743bb7a15`
- `STLSoft-1.11` pinned at `21743a52244e464f6bda0951b4c570f99a63d060`
- `FFS` pinned at `73e5727cb30cbec09a9bb3bb3d59401b9dac4326`

Qtindex core and direct tool payload sources are vendored in-tree under
`include/index` and `src/index`. The C Tree-sitter runtime and C++ grammar
files needed by index live under `include/tree_sitter` and
`src/index/tree_sitter_*`, so no separate Tree-sitter package manager
dependency is required for native builds.

## Submodules

`spdlog` backs the native runtime logging facade in
`qt/foundation/logging.hpp`. Logging is opt-in at process startup via
`--log-file`, `--log-stderr`, `--log-level`, or the matching `QT_LOG_*`
environment variables so diagnostics do not pollute structured stdout.

- `lib/llama.cpp` uses the PrismML llama.cpp fork and is pinned as a git
  submodule at `9fcaed763ccda38ea81068ad9d7f991aaddca451`. The fork retains
  upstream llama.cpp behavior and adds PrismML's group-128 `Q2_0` ternary
  tensor format and CPU, Metal, CUDA, and Vulkan kernels. This format is
  distinct from llama.cpp's `TQ2_0` and Prism's group-64 migration format for
  newer upstream llama.cpp releases.

The native PlatformIO environment builds llama.cpp through
`scripts/build_llama_cpp.py` into `.pio/build/<env>/llama.cpp` and links the
static `llama`, `ggml`, `ggml-cpu`, `ggml-base`, and any generated
`ggml-*` backend libraries. The default build auto-enables installed llama.cpp
GPU backends and disables llama.cpp examples, tools, tests, UI assets, OpenMP,
and BLAS.

Required local tools for the native llama build:

- `cmake`
- `ninja` is used when available; otherwise CMake chooses its default generator.
- A C and C++ compiler compatible with the active PlatformIO native environment.

## System Libraries

- `sqlite3` is linked by the native index service and built-in memory. It backs
  persistent index snapshots, global governed memory state, and FTS5 search.
  The build uses the system SQLite C API (`-lsqlite3`) rather than adding a
  PlatformIO library dependency. Memory uses WAL, foreign keys, bounded busy
  retries, and stores its database below the resolved qt agent home.

Optional backend/GPU builds can be enabled by passing CMake cache overrides
through `QT_LLAMA_GPU_BACKENDS` or `QT_LLAMA_CMAKE_ARGS`, for example:

```sh
pio run -e native
pio run -e native_full
QT_LLAMA_GPU_BACKENDS=cuda,vulkan pio run -e native
QT_LLAMA_GPU_BACKENDS=all pio run -e native
QT_LLAMA_CMAKE_ARGS="-DGGML_VULKAN=ON" pio run -e native
```

Vulkan builds require the Vulkan loader and headers, `glslc`, and SPIR-V
Headers. On Debian and Ubuntu these are provided by `libvulkan-dev`, `glslc`,
and `spirv-headers`. A standalone Vulkan SDK can instead be selected with
`VULKAN_SDK` or `Vulkan_ROOT`; custom include roots can be supplied through
`CPATH` or `CPLUS_INCLUDE_PATH`.

`native` is the default local developer target. It uses
`QT_LLAMA_GPU_BACKENDS=auto` and enables CUDA, HIP, Vulkan, or Metal only when
the corresponding SDK and local hardware/device signal are present and
compatible with the pinned llama.cpp build. For CUDA/HIP, `native` also passes
the detected local accelerator architecture to CMake instead of building a broad
release architecture set. `native_full` is the release target and requests one
unified app with CUDA, Vulkan, and HIP compiled in, using CUDA architectures
`75;80;86;89;90` and disabling NCCL/RCCL. `off`, `cpu`, or `none` force a
CPU-only build. `all` asks CMake to enable every GPU backend and will fail
unless every corresponding SDK is present. OpenCL and SYCL remain explicit
opt-in backends through `QT_LLAMA_GPU_BACKENDS=opencl` or
`QT_LLAMA_GPU_BACKENDS=sycl`. Those optional backends require the matching
system SDKs and drivers installed outside this repository. The checked-in
default target still does not require CUDA, HIP, Metal, Vulkan, SYCL, OpenCL,
BLAS, or OpenMP on machines that do not provide them.

GitHub Actions release builds use `pio run -e native_full` and produce a single
Linux app artifact with CPU, CUDA, Vulkan, and HIP support compiled in.
llama.cpp registers the compiled backends at startup and reports usable devices
through runtime status, so offload decisions are based on the devices and
drivers present on the target machine rather than on separate backend-specific
binaries.

## Llama Runtime Troubleshooting

- Missing model file: confirm the `llama.models[].path` value resolves from the settings file directory, workspace cwd, or `~`, and that the process can read the GGUF file.
- Unsupported embedding mode: set `llama.embeddings.enabled` to `true` before loading the model, select a model that supports embeddings, and use a pooling mode accepted by the pinned llama.cpp API.
- Grammar parse failure: validate GBNF against the pinned `lib/llama.cpp/grammars` examples, check the configured `root`, and prefer a configured grammar file alias for reusable grammars.
- GPU backend mismatch: confirm `pio run -e native` reports the expected
  `llama.cpp GPU backends:` list, install the matching system SDK/driver, and
  reload the model after changing offload settings. With `gpuLayers` omitted,
  local llama models query available GPU memory and automatically choose full
  or partial layer offload while retaining 1 GiB of headroom per device.
  Allocation or context creation failures retry smaller layer counts and
  finally CPU-only execution. Explicit positive `gpuLayers` values are tried as
  configured before CPU fallback; `gpuLayers=0` remains CPU-only.
- PrismML ternary model mismatch: current PrismML `*-Q2_0.gguf` files use the
  fork's group-128 `Q2_0` layout. Do not substitute llama.cpp `TQ2_0` files
  when validating the Prism format.
