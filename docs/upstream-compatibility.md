# Compatibility notes

How the Rust qt relates to Pi (`reference/pi`, the behavioural reference) and to the
archived C++ qt (`archived/`).

## Compatible

- **Agent home:** same `~/.qt` layout and `QT_CODING_AGENT_DIR` override as the archived qt.
  `settings.json` keys `defaultProvider`, `defaultModel` and `ollama.instances` are read with
  the same meaning. Unknown keys are preserved.
- **Credentials:** `auth.json` `{ "version": 1, "providers": { "<id>": { "apiKey" } } }` as written
  by the archived qt. OAuth entries (for example `openai-codex`) are preserved but not used.
- **Sessions:** Pi v3 JSONL. Messages use Pi's `role`-tagged shapes (`text`, `thinking` with
  `thinkingSignature`, `toolCall`, `image`; `toolResult` with `toolCallId`, `isError`,
  `details`). Usage is written with the archived qt's snake_case fields and read with Pi's
  camelCase names as aliases. Session ids and file names follow the archived qt
  (`native-<micros>.jsonl`, stored flat in `sessions/` with `cwd` in the header).
- **Tools:** names, JSON schemas, descriptions, prompt snippets and guidelines, the
  2000-line / 50 KB limits and continuation notices follow Pi's coding-agent tools. `edit`
  accepts Pi's `edits[]` form and the legacy single `oldText`/`newText` form.
- **System prompt:** Pi's structure, with "qt" in place of "pi" and without the Pi
  documentation section. Context files are searched in the same way (`AGENTS.md`, then
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`.

## Differences

- **Session tree:** only the chain of `message` entries from the last entry to the root is
  replayed. Branch, compaction, label and model-change entries are kept on disk but ignored.
- **`edit` fallback:** when an exact match fails, qt matches line by line, ignoring trailing
  whitespace and typographic quote, dash and space variants, and replaces the original
  lines. Pi applies its replacement to fuzzy-normalised file content instead.
- **`grep` / `find`:** implemented in-process with the `ignore` crate rather than by calling
  `rg`/`fd`. Output format is `path:line: text` (matches) and `path-line- text` (context).
  Hidden files are skipped, as with ripgrep's defaults.
- **`read`:** images over 5 MB are rejected rather than resized.
- **Replaying history:** assistant turns containing only reasoning are omitted from
  OpenAI-compatible requests, because some servers (Ollama) reject assistant messages that
  have neither content nor tool calls. Thinking blocks are replayed to Anthropic only when
  they carry a signature from Anthropic.
- **Not yet implemented** (see `PLAN.md`): compaction, slash commands, skills, OAuth
  logins, in-process llama.cpp, memory, extensions, MCP, subagents and RPC mode. The
  project index exists as the `qt-index` crate but is not yet registered by the app or CLI.

## Project index (`qt-index`)

Ported from the archived `index` subsystem and `tools/index_tool.cpp`.

- **Same as archived:** file classification and skip rules, `.gitignore` handling, build
  emulators (CMake Makefile/Ninja build trees, Make/Kbuild with `.config`, qmake, waf,
  PlatformIO incl. `.pio/libdeps`, Cargo/npm/Python), the `compile_commands.json` written to
  the project root (inferred `cc -std=c17` / `c++ -std=c++17` commands, eight defines,
  28 includes, `.index/objects/<file>.o` outputs), the line-based C/C++ code index, the
  SQLite schema (`meta`, `project_files`, `compilation_databases`, `build_file_counts`,
  `classes`, `methods`, `functions`, `call_graph`, `skipped_code_files`,
  `scan_fingerprints`, FTS5 `index_fts`), 30 s rescans with low-priority pacing,
  `index_stale` after loading a persisted snapshot, the six tools (`symbol`,
  `analyze_file`, `references`, `build_context`, `search_index`, `index`) with their
  schemas, limits, `invalid_arguments` / `invalid_index_arguments` / `index_not_ready`
  payloads, output guards (64 KiB strings, 1000 array items) and the registry rewrites for
  `index` calls (hoisting `operation`/`category`/`include_schemas`, `find_symbol` /
  `symbol_lookup` / `lookup_symbol` → `code.symbol`, `pattern` → `name`).
- **Router groups:** only `core`, `code`, `fs`, `git` and `build`. The `serial`, `debug`,
  `rtos`, `fetch` and `util` operations are not ported (decision 2026-11-18); the router
  description and `category` hint list only the ported groups, and calls to the others
  fail with `unknown index operation`.
- **State directory:** `<agent_home>/index/<hash>/` where `<hash>` is the decimal FNV-1a
  64-bit hash of the canonical project root. The archived build used `std::hash`, which is
  not stable across builds, so archived state directories are not reused (the index is
  simply rebuilt).
- **Tree-sitter:** the `tree-sitter` / `tree-sitter-cpp` crates replace the vendored C
  runtime; tree files hold the same S-expression text. Manifest `mtime` values are Unix
  nanoseconds rather than `file_time_type` ticks, so an archived manifest triggers a
  one-time reparse.
- **Fingerprints:** per-file `fnv1a64:` fingerprints are always recorded (the archived
  service did so only when memory code retrieval was enabled). The memory retrieval
  snapshot itself belongs to the governed-memory milestone.
- **Fixes:** `.gitignore` rules are re-read on every scan (the archived matcher cached them
  for the life of the process); an FTS5 syntax error now surfaces as
  `invalid FTS5 query: ...` instead of silently returning no matches; scans triggered by
  tools and by the worker are serialised.
- **Tool results:** a success result's text is the compact JSON output; an error result's
  text is the error message, followed by the structured payload as JSON on the last line
  when there is one. `details.status` carries the archived status (`success`,
  `invalid_request`, `execution_error`).
