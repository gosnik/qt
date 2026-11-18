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
  (`native-<micros>.jsonl`, `fork-<id>.jsonl`, `clone-<id>.jsonl`, `-2`… suffixes on
  collision), stored flat in the session directory with `cwd` and `parentSession` in the
  header; listing scans the directory recursively so Pi's per-cwd folders are found too.
- **Session entries:** all Pi v3 entry types (`message`, `model_change`,
  `thinking_level_change`, `compaction`, `branch_summary`, `custom`, `custom_message`,
  `label`, `session_info`) and message roles `custom` and `bashExecution`; v1 (no ids,
  `firstKeptEntryIndex`) and v2 (`hookMessage`) files are migrated to v3 on open and
  rewritten. Like Pi, a session file is first written when it holds an assistant message.
  Context replay follows Pi `buildSessionContext` (compaction summary, kept entries from
  `firstKeptEntryId`, branch summaries, custom messages), with Pi's summary prefixes.
- **Session directory:** `--session-dir` > `PI_CODING_AGENT_SESSION_DIR` > `sessionDir`
  setting > `<agent home>/sessions`. Id-prefix resolution and its error wording
  (`No session found matching '…'`, `Session prefix '…' is ambiguous; matches: …`) and
  most-recent selection (this cwd first, then modification time) follow the archived qt.
- **Tree and forks:** `navigateTree`, `branch_with_summary`, labels, `fork` (before a user
  message, or at an entry), clone, import and `forkFrom` follow Pi; summaries use Pi's
  branch-summary prompt and preamble with read/modified file lists.
- **Compaction:** Pi's prompts (initial, update, split-turn prefix), cut-point rules
  (never at tool results, split-turn detection), chars/4 estimates, file tracking in
  `details`, `Already compacted` / `Nothing to compact (session too small)` errors and
  `compaction.*` defaults (enabled, reserve 16384, keep 20000). Auto-compaction runs on
  the threshold and recovers from context overflow by compacting and retrying once.
- **Auto-retry:** Pi's retryable-error pattern and `retry.*` defaults (enabled, 3 retries,
  2 s base, exponential); the failed response stays in the session but leaves the context.
- **Queues:** steering / follow-up / next-turn delivery and `steeringMode` /
  `followUpMode` (`one-at-a-time` default, legacy `queueMode` honoured) follow Pi's loop.
- **Active tools:** persisted as the archived qt's `custom` entry `tools-config`
  (`{"enabledTools": [...]}`) and restored from the current branch.
- **Goals:** `/goal` wording, prompts, budget rules and elapsed/token formatting follow the
  archived qt.
- **Tools:** names, JSON schemas, descriptions, prompt snippets and guidelines, the
  2000-line / 50 KB limits and continuation notices follow Pi's coding-agent tools. `edit`
  accepts Pi's `edits[]` form and the legacy single `oldText`/`newText` form.
- **System prompt:** Pi's structure, with "qt" in place of "pi" and without the Pi
  documentation section. Context files are searched in the same way (`AGENTS.md`, then
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`.

## Differences

- **Invalid session files:** a file without a valid header is an error; Pi silently starts
  a fresh session over it. Entries of unknown types, or known types with invalid fields,
  are kept verbatim (they survive rewrites) and reported as diagnostics, never replayed.
- **Threshold compaction** needs a known context window (from model discovery or
  `Conversation::set_context_window`); without one only overflow recovery runs. Summaries
  are requested with thinking off and `maxOutputTokens` as the model output limit.
- **Overflow detection** also treats qt's own body-less `HTTP 400/413 …:` transport errors
  as overflow, matching Pi's `4xx (no body)` pattern.
- **Goals:** the archived qt kept goals in memory and its prompts named an `update_goal`
  tool that did not exist. qt persists the goal as a `custom` entry `goal`, offers an
  `update_goal` tool (`complete` / `blocked`) while a goal is active, accounts tokens
  (input + output + cache) and wall time per run, injects the budget-limit prompt once as
  a steering message, and stops auto-continuation after a continuation turn that made no
  tool calls.
- **Parallel tools:** a batch runs concurrently only when every tool in it reports
  `parallel_safe()`; Pi runs batches in parallel unless a tool is marked sequential.
  Results are committed in source order either way.
- **Hooks:** `before_provider_request` rewrites qt's `Request` (model, system prompt,
  messages, tools, limits) rather than the provider JSON payload.
- **Manual `/compact`** refuses to run while the agent is running instead of aborting it.
- **Events:** one `AgentStart` / `AgentEnd` pair spans a whole prompt including retries,
  compaction and follow-ups (Pi emits `agent_end` per loop run).
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
- **Not yet implemented** (see `PLAN.md`): GUI tree / fork / resume pickers and the
  missing-cwd dialog, provider-level retry (`retry.provider.*`, `Retry-After`), slash commands, skills, OAuth
  logins, in-process llama.cpp, memory, extensions, MCP, subagents, the project index and
  RPC mode.
