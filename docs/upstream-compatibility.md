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
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`. `SYSTEM.md`
  replaces the body and `APPEND_SYSTEM.md` follows it (`<cwd>/.pi/` wins over the agent
  home), as in the archived qt; `--system-prompt` replaces the discovered file and
  `--append-system-prompt` values are appended to it, and (as in Pi) a value naming an
  existing file is replaced by its contents. The prompt ends with the archived qt's
  `# Environment Context` / `<environment_context>` block (cwd, workspace root, shell,
  UTC date, timezone, network, tool permissions, active tool count) instead of Pi's
  `Current date:` / `Current working directory:` lines.
- **Skills (`qt_core::resources::skills`):** archived discovery order (`<agent_home>/skills`,
  `~/.agents/skills` for a `~/.pi/agent` home, `<repo>/.pi/skills`, `.agents/skills` from the
  repository root down to cwd), `settings.skillRoots`/`skills` with `!`/`+`/`-` filters,
  `features.loadSkills`, `.gitignore`/`.ignore`/`.fdignore`, `package.json` `pi.skills`,
  `SKILL.md` vs root `*.md`, frontmatter checks and collision diagnostics with the archived
  wording. The `<available_skills>` block is the shared Pi/archived text and, as in the
  archived qt, is emitted whether or not the `read` tool is active. `/skill:<name> args`
  expands to the skill body plus `\n\nUser: <args>` (archived wording, not Pi's `<skill>`
  wrapper), gated by `enableSkillCommands`.
- **Prompt templates (`qt_core::resources::templates`):** archived roots, frontmatter
  (`description`, `argument-hint`), 60-character first-line fallback, shell-like argument
  splitting and `$N`, `$@`, `$ARGUMENTS`, `${@:N}`, `${@:N:L}`, `{{args}}`, `{{arguments}}`,
  `${N}`, `{{N}}` placeholders, expanded in one pass (substituted text is not re-expanded,
  unlike Pi's sequential replaces) with saturating index arithmetic.
- **Themes (`qt_core::resources::themes`):** Pi's JSON theme format with the 51 required
  tokens, `vars`, hex and ANSI-256 values and the optional `export` section; Pi's built-in
  `dark`/`light`; theme roots `<agent_home>/themes`, `<repo>/.pi/themes`,
  `<repo>/.agents/themes` plus `themeRoots` and filters; archived error wording. Theme names
  are matched case-insensitively with Unicode lowercasing (the archived qt used NFKC case
  folding). Custom theme files are watched with debounced reload (`ThemeWatcher`).

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
- **Resource settings:** a `skillRoots`/`promptRoots`/`themeRoots` array holding only filter
  entries narrows the discovered roots instead of replacing them with none (the archived qt
  dropped every root in that case). Invalid resource settings become diagnostics rather than
  aborting startup. `.gitignore` patterns without a trailing slash also ignore directories
  (the archived matcher missed directories because it matched `name/`).
- **Not yet implemented** (see `PLAN.md`): compaction, slash commands, OAuth
  logins, in-process llama.cpp, memory, extensions, MCP, subagents, the project index and
  RPC mode.
