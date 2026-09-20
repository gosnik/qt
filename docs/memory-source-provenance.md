# Built-in Memory Source Provenance

The built-in qt memory implementation is derived from the same-owner sibling
repository `qtmem` at commit `3324ace` (inspected
2026-07-17). The user explicitly authorized using that repository as both a
reference and a code source when requesting implementation of `MEM.md`.

The port may adapt these first-party sources:

- `include/qtmem/memory_store.hpp`
- `include/qtmem/core/**`
- `include/qtmem/storage/repositories.hpp`
- `include/qtmem/governance/lifecycle_policy.hpp`
- `include/qtmem/services/**`
- `src/storage/memory_store.cpp`
- `src/storage/sqlite_memory_store.cpp`
- the governed recall/context logic in `src/service/service_common.cpp`,
  `src/service/service_recall.cpp`, and `src/service/service_detail.hpp`
- `dashboard_wasm/**`, `src/ui/dashboard.cpp`, and dashboard read-model
  behavior in `src/service/service_dashboard_models.cpp`
- behavioral fixtures from `tests/**`

The native port changes namespaces to `qt::memory`, uses qt's existing
foundation/config/logging/process/model abstractions, splits broad store/service
interfaces into focused roles, and places large implementations in `.cpp`
files.

The following sibling components are deliberately excluded:

- general MCP, HTTP, REST, and standalone service transports;
- standalone CLI and application entry points;
- Codex hook scripts and remote import scripts as runtime dependencies;
- Ollama/curl integration and the sibling logging facade;
- the JSON snapshot backend as a live global store.

The dashboard exception carries qtmem's vendored Dear ImGui 1.92.5 client,
Emscripten SDL/WebGL build configuration, and JetBrains Mono/DejaVu Sans fonts.
Their license notices are retained beside the copied sources/assets. The
checked-in WASM bundle avoids adding Emscripten to ordinary native builds;
rebuilding that bundle is an explicit optional developer workflow.
