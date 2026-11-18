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

## In-process llama.cpp (`qt-llama`)

The `qt-llama` crate ports the archived qt llama integration as a library; it is not yet
wired into the engine, model picker or a `/llama` panel.

- **Compatible:** `settings.llama` keys, defaults, error and validation messages; path
  resolution from the settings file directory with `~`; GGUF discovery roots, ids,
  quantisation names (including `Q2_0` = 41), classification, usage notes and memory-task
  model selection; persisting discovered models to `settings.json`; offload planning (1 GiB
  headroom per device, then 75/50/25 %/CPU retries); sampler chain order and defaults
  (temperature 0.3, top-p 0.9, 256 output tokens); grammar resolution; the prompt protocol
  (OpenAI-style request JSON with sorted keys, controller messages, `tool_choice` forcing on
  file/search/model words, first-JSON-object tool-call parsing); embedding queue timeout and
  cancellation; KV cache operations and runtime status fields.
- **Differences:** the `session` KV policy reuses the longest cached token prefix of the new
  prompt (the archived qt appended every full prompt after the previous one, duplicating
  context, and pruned at a 70 % watermark); generation is cancellable between tokens and
  during decode; streamed text is split only at UTF-8 boundaries; tool results are sent as
  the result text (with `is_error` when set) because Rust tool results have no structured
  payload; tool calls are only parsed when tools were offered; the request `maxTokens`
  caps the configured output limit; `tensorSplit` is padded to `llama_max_devices()`;
  embedding contexts size the batch to the context so encoder models accept full inputs.
  `kvCache.maxSessions` and `kvCache.clearOnModelChange` are parsed and validated but, as in
  the archived qt, have no runtime effect.
