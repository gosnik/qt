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
- **MCP client (`qt-mcp`):** same config sources and merge order as the archived qt
  (settings `mcpServers`/`mcp_servers`, then `~/.config/mcp/mcp.json`, `<agent_home>/mcp.json`,
  `<workspace>/.mcp.json`, `<workspace>/.pi/mcp.json`; later files replace servers of the same
  name; servers ordered by name), the same per-server keys and validation messages, and MCP
  files cannot set `allowExternalFilesystemAccess`. Protocol `2025-03-26`, client name `qt`;
  stdio servers get the archived qt's filtered environment and append stderr to
  `<agent_home>/logs/mcp-stderr.log` (0600). Proxy tools are named
  `mcp_<server>_<tool>` with description `MCP <server>/<tool>: <description>` and the remote
  `inputSchema`; tool text is the `text` items joined by newlines, else the result JSON.
  Error text is redacted with the archived qt's secret markers. Transport failures are retried
  five times with 100 ms exponential backoff. Sampling applies the per-server `sampling`
  policy (`enabled`, `model`, `allowed_models`, `max_tokens_cap` default 4096, `timeout`
  default 30 s) and returns `{role, content, model, stopReason}` as before.

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
- **MCP:** `tools/list` and `resources/list` follow `nextCursor` pagination; `initialize`
  advertises the `sampling` capability when a handler is installed and the server's policy
  allows it (the archived qt sent empty capabilities); streamable HTTP answers sampling
  requests that arrive on a response stream, re-initializes after a `404` for an expired
  session, sends the negotiated protocol version and ends the session with `DELETE`;
  server `ping` requests are answered; aborting a proxy tool sends
  `notifications/cancelled`. Only transport failures are retried (the archived qt also
  retried JSON-RPC errors and timeouts). `list_mcp_resources`/`read_mcp_resource` query the
  servers live (`resources/list`, `resources/read`) instead of a static metadata snapshot;
  read results render text contents, image blobs as images and other blobs as a summary
  line. MCP proxy results also carry image content blocks. MCP files contribute only their
  servers, not other settings keys.
- **Not yet implemented** (see `PLAN.md`): compaction, slash commands, skills, OAuth
  logins, in-process llama.cpp, memory, extensions, wiring MCP into the engine and the
  `/mcp` panel, subagents, the project index and RPC mode.
