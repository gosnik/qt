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
  logins, in-process llama.cpp, memory, extensions, MCP, subagents, the project index and
  RPC mode.

## Governed memory (`qt-memory`)

Pi has no persistent-memory contract; the archived qt is the reference.

- **Store:** `<agent_home>/memory/memory.sqlite3` with `blocks/` and `exports/` beside it
  (directories 0700, database 0600), WAL, `synchronous=FULL`, foreign keys and a jittered
  busy-retry handler. Migrations v1–v7 are byte-identical to the archived SQL (a test
  extracts them from `archived/`), so databases written by the C++ binary open unchanged and
  partially migrated ones are upgraded. A standalone qtmem database (legacy `meta` table)
  is refused with "standalone qtmem database requires explicit offline import"; schema
  versions above 7 are refused.
- **Model and wording:** memory types, statuses, source types, scope kinds and dimensions,
  scope keys, recall policies, review kinds, actions, application statuses and batch job
  statuses use the archived spellings, and error messages match the archived text. Stored
  JSON uses the archived encoding (compact, sorted keys).
- **Same behaviour:** write policy and secret/path redaction, governance transitions and
  audit, review preview/apply with revision compare-and-set and idempotent action keys,
  atomic review mutations, scheduled resurfacing, constrained batches and background batch
  jobs, persisted view state, review statistics, native export/import with ID remapping,
  retention, historical scrub, read-only qtmem v3 import and skill import. `memory.*`
  settings use the archived defaults, validation and the project-may-not-broaden rule.
- **Differences:**
  - Text fields are truncated to their byte limits on a UTF-8 character boundary (the C++
    store could split a multi-byte character).
  - A graph-edge upsert reports the ID of the stored edge; the archived store returned a
    freshly generated ID that was never written when the edge already existed.
  - Import with replace clears the scope's assertion revision boundaries, so restoring a
    backup into the same scope (preserved IDs) no longer fails on the insert trigger.
  - Export reads review actions with the export's own limit; the archived export applied
    the interactive 10 000-row history cap and failed for backups (limit 100 000).
  - Lenient JSON readers: a field of the wrong type in an imported document falls back to
    its default instead of aborting the import.
- **Not yet ported:** recall (lexical, graph, dense, code), context rendering, the
  pre-handler, capture/ingestion, post-processing, Dreaming, research, the verifier, memory
  tools, the `qt memory` CLI and the GUI.
