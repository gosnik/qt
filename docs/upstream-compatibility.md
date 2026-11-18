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
  accepts Pi's `edits[]` form and the legacy single `oldText`/`newText` form. Bash
  truncation notices use Pi's `[Showing lines S-E of T. Full output: <path>]` wording.
- **Archived tool extras:** the archived qt's implementation names (`read_text`,
  `write_text`, `edit_text`, `search_text`, `list_directory`, `find_files`) resolve to the
  Pi-named tools, and its argument aliases are accepted (`file_path`/`filePath`, `text` for
  `write` content, `old_string`/`oldString`/`new_string`/`newString`/`replace_all`, grep
  `query`/`include`/`file_glob`/`-i`/`-A`/`-B`/`-C`/`literal_text`, find `glob`/`include`,
  bash `cwd`/`working_dir`, apply_patch `patchText`). Advertised schemas stay Pi-shaped plus
  the archived options (`edit` `replace_all`; grep `output_mode`, `head_limit`, `offset`,
  `multiline`, `type`, `hidden`, `no_ignore`; find `type`, `directories`, `hidden`,
  `no_ignore`; ls `recursive`, `depth`, `ignore`; bash `timeout_ms`, `workdir`). The
  archived-only tools `append_text`, `apply_patch`, `clipboard`, `complete_path`,
  `package_manager` (detect and plan only), `web_fetch`, `web_search` and `web_run` are
  available through `ToolRegistry::builtin()`; `--tools` globs, `--no-tools`,
  `--no-builtin-tools` and offline mode are modelled by `ToolSelection`.
- **Workspace policy:** `workspaceRoot` (default: enclosing git repository, else cwd) and
  `allowExternalFilesystemAccess` (global settings only; `.pi/settings.json` may only narrow)
  confine every path argument and the bash working directory, with symlinks resolved. Refusals
  read `path escapes workspace_root: <path>` / `working directory escapes workspace_root`.
  `ToolContext::new(cwd)` confines to `cwd`. The prompt sentence matches the archived
  `<tool_permissions>` text.
- **Mutation queue:** with `MutationPolicy::Queue`, `write`, `edit`, `append_text`,
  `apply_patch` and `bash` return a `pending_approval` result with a `mutation-<n>` id;
  approved mutations re-run once via `ToolRegistry::execute_approved`.
- **Read guard:** the second and third identical reads of an unchanged file range carry the
  archived "This unchanged file range has already been read." warning and the fourth is
  refused with the archived `read_text refused a duplicate unchanged read…` message.
- **Web tools:** the public-URL policy (no credentials, localhost, private/reserved IPv4,
  IPv6 literals; every resolved address must be public; connections pinned to the checked
  addresses), five-redirect limit, 2 MiB download cap, simplified-text conversion,
  DuckDuckGo `uddg` decoding, `web_run` `turnNsearchM` / `turnNfetchM` ref ids and limits
  follow the archived qt. `PI_OFFLINE` or offline mode disables them with
  `web access is disabled in offline mode`.
- **System prompt:** Pi's structure, with "qt" in place of "pi" and without the Pi
  documentation section. Context files are searched in the same way (`AGENTS.md`, then
  `CLAUDE.md`, per ancestor directory), plus the global `~/.qt/AGENTS.md`.

## Differences

- **Session tree:** only the chain of `message` entries from the last entry to the root is
  replayed. Branch, compaction, label and model-change entries are kept on disk but ignored.
- **`edit` fallback:** when an exact match fails, qt matches line by line on NFKC-folded
  text, ignoring trailing whitespace and typographic quote, dash and space variants, and
  replaces the original lines. Pi and the archived qt apply the replacement to
  fuzzy-normalised file content instead. `replace_all` replaces every non-overlapping match;
  without it Pi's uniqueness errors apply.
- **`grep` / `find`:** implemented in-process with the `ignore` crate rather than by calling
  `rg`/`fd`. Output format is `path:line: text` (matches) and `path-line- text` (context),
  with `--` between separated context groups. Hidden files are skipped unless `hidden` is
  set (`.git` is always skipped). `find` lists files only unless `directories` is set or
  `type` is `d`, and never descends into `node_modules` (archived behaviour). grep
  `multiline` really spans lines (the archived qt accepted the flag but ignored it).
- **`ls`:** recursive listings do not follow symlinked directories or list `.git` contents.
- **`read`:** directories return their listing (archived) instead of an error; missing
  files suggest similarly named siblings. Images are detected by content. Without a resize
  hook (`ImageOptions::resize`, owned by the image subsystem) images over 5 MB are omitted
  with a note rather than resized.
- **`bash`:** runs `/bin/bash -lc` when available (archived), else `bash -lc` from `PATH`,
  else `/bin/sh -c`. Relative `workdir` values resolve against the workspace root. The full
  output of truncated commands is written to `qt-bash-<id>.log` in the temp directory and
  stays readable by `read` even outside the workspace.
- **`apply_patch`:** each `@@` hunk is matched separately and in order (the archived qt
  concatenated all hunks of a file into one block); `*** Move to:` and `*** End of File` are
  supported; a bare empty line inside a hunk is an empty context line. The result text is
  Codex's `Success. Updated the following files:` list.
- **`web_fetch`:** non-HTML responses are returned as-is (the archived qt stripped tags from
  every response); bodies over the cap are cut with a note instead of failing; HTTP errors
  are reported as tool errors. URL fragments are dropped instead of rejected.
- **`clipboard`:** image copies pass `-t <mime>` to xclip and `--type <mime>` to wl-copy.
- **Replaying history:** assistant turns containing only reasoning are omitted from
  OpenAI-compatible requests, because some servers (Ollama) reject assistant messages that
  have neither content nor tool calls. Thinking blocks are replayed to Anthropic only when
  they carry a signature from Anthropic.
- **Not yet implemented** (see `PLAN.md`): compaction, slash commands, skills, OAuth
  logins, in-process llama.cpp, memory, extensions, MCP, subagents, the project index and
  RPC mode.
