# Extensions

qt extensions add slash commands, model tools, CLI flags, model providers,
skills/prompts/themes, keyboard shortcuts, message renderers and lifecycle hooks.
They are implemented by the `qt-extensions` crate. Three kinds are supported:

| Kind | Files | Runs |
| --- | --- | --- |
| **Rhai script** | `*.rhai` | In-process, sandboxed (this document) |
| **Subprocess** | `extension.json` with `"transport": "subprocess_jsonrpc"` | A child process speaking JSON-RPC 2.0 on stdio |
| **Pi module** | `*.ts` / `*.js` | Never executed; registrations are read by a static scan so they can be listed |

Rhai replaces the archived qt's AngelScript host (decision 2026-11-18) with the
same contract: one engine per extension, `activate()` once, persistent state, the
same host methods and lifecycle events.

## Loading

```sh
qt -e path/to/extension.rhai          # a file
qt -e path/to/directory               # every extension under a directory
qt --no-extensions -e my.rhai         # only explicit paths
```

Discovery order (an extension reached twice, compared by canonical path, loads once):

1. `-e` paths (kept with `--no-extensions`);
2. `<workspace>/.pi/extensions`;
3. `<agent_home>/extensions` (`~/.qt/extensions`);
4. installed settings packages, project packages first, then user packages
   (see [Packages](#packages)).

Inside a directory:

- a directory with `extension.json` is one extension (manifest below);
- otherwise a `package.json` with `pi.extensions` lists entry files or
  directories, with `!glob` (exclude), `+path` (force include) and `-path`
  (force exclude) entries;
- otherwise `index.rhai`, `index.ts` or `index.js` (first found) is the entry;
- otherwise every `*.rhai`, `*.ts` and `*.js` file is an extension, and each
  subdirectory is scanned the same way.

Hidden entries and `node_modules` are skipped. An extension's name is its file
stem, or its directory name for `index.*`, lowercased with other characters
replaced by `-`.

### `extension.json` (manifest v1)

```json
{
  "manifest_version": 1,
  "name": "my-ext",
  "display_name": "My extension",
  "transport": "subprocess_jsonrpc",
  "executable": "bin/run.py",
  "args": ["--stdio"],
  "working_directory": ".",
  "commands": [{ "name": "hello", "title": "Hello", "description": "Say hello" }],
  "tools": [{ "name": "lookup", "description": "Find things", "parameters": { "type": "object" } }],
  "flags": [{ "name": "plan", "type": "boolean", "default": false, "description": "Plan mode" }],
  "providers": [{ "name": "corp", "config": { "baseUrl": "https://llm.corp/v1", "models": [] } }],
  "metadata": { "lifecycle_handlers": [{ "event": "tool_call" }] }
}
```

`transport` is `subprocess_jsonrpc` (default; needs `executable`), `rhai`
(`entrypoint` names the script; its registrations are added to the manifest's)
or `reference_module`. Names use lowercase letters, digits, `.`, `_` and `-`
and are unique per list; flag types are `boolean` or `string` and defaults must
match. Validation failures are reported as
`extension manifest failed validation: <path> (invalid_argument [field]: message; ...)`.

## Rhai scripts

```rhai
fn activate() {
    this.calls = 0;
    registerCommand("hello", "Hello", "Show a greeting");
    registerFlag("greeting", "string", "Hello", "Greeting text");
    on("input", "on_input");
}

fn command_hello(args) {
    this.calls += 1;
    let greeting = pi::getFlag(#{ name: "greeting" });
    ctx::ui::notify(#{ message: `${greeting}, ${args.args}! (${this.calls})` });
    #{ text: "Command completed" }
}

fn on_input(event) {
    if event.text == "/secret" { return #{ action: "handled" }; }
    #{ action: "continue" }
}
```

### Lifecycle of a script

- The script's top level is evaluated, then `fn activate()` runs **once**.
  Registration functions work only during `activate()`; calling them later
  throws `registration APIs are only available during activate()`.
- Handlers are script functions with zero or one parameter. The parameter is
  the event or arguments as a Rhai object map; the return value (any value:
  map, array, string, number, bool or `()`) is converted to JSON.
- **State:** `this` is a persistent object map shared by all handler calls of
  the extension (`this.calls += 1`). Rhai functions cannot see top-level
  variables; top-level `const` values are readable as `global::NAME`. The top
  level is re-evaluated before every handler call (that is how Rhai loads
  constants), so keep it to `const`/`let` declarations and do one-time work
  in `activate()`. `this` is only bound in the handler itself; helper
  functions that need it are called as methods (`this.helper()`).
- `reload` (`Extensions::reload`, `/reload`) creates fresh engines.

### Sandbox

| Limit | Value |
| --- | --- |
| Source size | 256 KB (`Rhai source exceeds sandbox size limit`) |
| Operations per invocation (load + `activate()` counts as one) | 100 000 (`sandbox step budget exceeded`) |
| `import`, `eval` | disabled (`Rhai sandbox disallows module imports (line N)`) |
| Call depth / nested invocations | 64 script levels / 8 nested extension calls |
| String / array / map size | 16 MiB / 1 000 000 / 1 000 000 |

Rhai has no file, network or process access of its own; scripts reach the
system only through the host API (`pi::exec` runs a process). A runtime that
another thread is executing rejects entry with
`extension runtime invocation is reentrant`; nested calls on the same thread
(component callbacks) are allowed up to depth 8. `print`/`debug` go to stderr
prefixed with the extension name.

### Registration API

Every function also has a `qt_`-prefixed alias (`qt_register_command`, `qt_on`, ...).

| Function | Notes |
| --- | --- |
| `registerCommand(name)`, `(name, title)`, `(name, title, description)`, `(name, #{title\|label, description})` | Handler `command_<name>` (characters other than letters, digits and `_` become `_`) receives `#{args: "<text after the command>"}` |
| `registerTool(name)`, `(name, title)`, `(name, title, description)`, `(#{name, title\|label, description, parameters, handler})` | Handler `tool_<name>` or `handler` (name or `Fn("f")`) receives the arguments map. `parameters` is the JSON schema shown to the model (default: any object) |
| `registerFlag(name)` | Boolean flag without default |
| `registerFlag(name, type, default, description)`, `(name, #{type, "default", description})` | `type` is `"boolean"` or `"string"`; `default` must match (`default` is a Rhai keyword: quote it as a map key) |
| `registerResource(type, path)`, `(type, path, name)` | `type` is `skill`, `prompt` or `theme`; `path` is relative to the script's directory and may not contain `..` |
| `registerProvider(name, config)` | `config` is a map (or JSON string) in `models.json` provider form (`baseUrl`, `api`, `apiKey`, `headers`, `models`, ...) |
| `on(event, handler)` | `handler` is a function name or `Fn("name")` (closures are rejected); `on(event)` alone is an error |
| `registerShortcut(key)`, `(key, handler, description)` | One-argument form uses `key` as the handler name |
| `registerMessageRenderer(customType, handler)` | Renders custom messages of that type (below) |

Validation messages follow the archived host, for example
`invalid extension lifecycle event: <event>`,
`invalid extension contribution name: <name>`,
`extension flag default type mismatch`.

### Host API

Host methods are namespaced functions taking one optional map:
`pi::getFlag(#{ name: "greeting" })`, `ctx::isIdle()`,
`ctx::ui::notify(#{ message: "hi" })`. `host_call("pi.getFlag", #{ name: "x" })`
is the generic form, and `qt_host_call(method, json_text)` returns JSON text (the
archived signature). Failures throw a string that `try { } catch (e) { }` can catch.

| Method | Arguments → result |
| --- | --- |
| `pi::getFlag` | `{name}` → explicit CLI value, else the registered default, else `()` |
| `pi::getCommands` | → `[{name, description, source}]` (front ends supply the full slash list; default: extension commands) |
| `pi::getSessionName` / `pi::setSessionName` | → name or `()` / `{name}` |
| `pi::appendEntry` | `{customType, data}` → persists a `custom` session entry (written at the next save point while running) |
| `pi::setLabel` | `{entryId, label}`; `label: ()` clears |
| `pi::sendMessage` | `{customType, content, display = true, details, triggerTurn = false, deliverAs = "steer"\|"followUp"\|"nextTurn"}` → appends (idle), starts a turn (`triggerTurn`) or queues (running) a custom message |
| `pi::sendUserMessage` | `{content, deliverAs = "steer"\|"followUp"}` → prompts when idle, queues while running |
| `pi::getActiveTools` / `pi::getAllTools` | → names / `[{name, description, parameters}]` |
| `pi::setActiveTools` | `{toolNames}`; unknown names fail with `unknown active tool: <name>` |
| `pi::getThinkingLevel` / `pi::setThinkingLevel` | `{level}` one of `off minimal low medium high xhigh`, clamped to the model (`invalid thinking level`) |
| `pi::setModel` | `{provider, id}` → `true`, or `false` when the provider has no credentials |
| `pi::exec` | `{command, args, cwd, timeout = 30000}` → `{stdout, stderr, code, killed}` (argv, no shell; `cwd` relative to the project) |
| `ctx::getModel` | → current model; `{provider, id}` inspects a catalog model. Public fields only: `provider id name api reasoning capabilities contextWindow maxTokens input cost thinkingLevelMap` (never keys or headers) |
| `ctx::getModelCatalog` | → `{providers, models}` in the same public shape |
| `ctx::getSystemPrompt` | → the system prompt of the next run (before hooks) |
| `ctx::getContextUsage` | → `{tokens, contextWindow, percent}` (`tokens`/`percent` are `()` when unknown, e.g. right after compaction); `()` without a context window |
| `ctx::getContext` | → `{systemPrompt, messages, thinkingLevel, model, usage}` (the persisted context, before context hooks) |
| `ctx::compact` | `{customInstructions}` → `{summary, firstKeptEntryId, tokensBefore, details}`; synchronous and idle-only (`extension runtime mutation requires an idle harness`) |
| `ctx::isIdle`, `ctx::hasPendingMessages`, `ctx::abort` | turn state and cancellation |

Message `content` is a string or an array of `#{type: "text", text}` and
`#{type: "image", mimeType, data}` (base64) blocks.

### `ctx::ui`

| Method | Arguments → result (headless result) |
| --- | --- |
| `select` | `{title, options, timeout}` → chosen option or `()` (`()`) |
| `confirm` | `{title, message, timeout}` → bool (`false`) |
| `input` | `{title, placeholder, timeout}` → text or `()` (`()`) |
| `editor` | `{title, prefill, timeout}` → text or `()` (`()`) |
| `notify` | `{message, notifyType = "info"\|"warning"\|"error"\|"success"}` (stderr) |
| `setStatus` | `{statusKey, statusText}` (`()` text removes) |
| `setWorkingMessage`, `setWorkingVisible`, `setWorkingIndicator`, `setHiddenThinkingLabel` | `{message}`, `{visible}`, `{frames, intervalMs = 80}`, `{label}` |
| `setWidget` | `{key, content: [lines]}` (`()` removes) |
| `setHeader`, `setFooter` | `{lines}` (or `content`); `()` restores the default |
| `setTitle` | `{title}` |
| `getToolsExpanded`, `setToolsExpanded` | `{expanded}` (`false`) |
| `getEditorText`, `setEditorText`, `pasteToEditor` | `{text}` (`""`) |
| `getAllThemes`, `getTheme`, `setTheme` | `{name}` (`[]`, `()`) |
| `custom` | `{handler}` → the component's final value (`()`) |
| `setEditorComponent`, `getEditorComponent` | `{handler}` / → `{handler}` or `()` |
| `addAutocompleteProvider` | `{handler}` |
| `onTerminalInput` | terminal-only; `{handler: ()}` is accepted, a handler is unsupported in qt |

`timeout` is in milliseconds; an empty submitted string is distinct from
cancellation. Dialogs block the extension until the user answers, so front ends
run commands off their UI thread.

**Components** (`custom`, `setEditorComponent`): the handler receives
`#{type: "render", width, height}` and returns `#{lines: [...]}`; key input
arrives as `#{type: "input", data}` and may return
`#{handled: true, done: true, value: ...}` (or `done: <value>`) to finish.
**Autocomplete providers** receive `#{lines, row, column, force}` and return
`()` or `#{prefix, items: [#{value, label, description}]}`; providers with the
same prefix are merged. **Message renderers** receive
`#{type: "render", message, content, details, width, expanded}` and return
`#{lines: [...]}`. **Shortcuts** receive `#{type: "shortcut", key}`.

### JSON helpers

`json_parse(text)` → value, `json_stringify(value)` → compact JSON;
archived helpers `qt_json_get(document, key)` → the field as JSON text (`"null"`
when missing) and `qt_json_string(json_text)` → the decoded string.

## Lifecycle events

Register with `on(event, handler)`. Each handler receives `#{type: "<event>", ...}`.
Handlers run in load order; results marked "chain" feed the next handler.

| Event | Payload | Result |
| --- | --- | --- |
| `input` | `text, source ("interactive"\|"rpc"\|"extension"), images?` | `#{action: "continue"}`, `#{action: "handled"}` (stop) or `#{action: "transform", text, images?}` (chain). Required; a failure aborts the input |
| `before_agent_start` | `prompt, images?, systemPrompt, systemPromptOptions{cwd}` | `#{message: #{customType, content, display?, details?}, systemPrompt}`: messages are added after the prompt; `systemPrompt` (chain) replaces it for this run, empty included |
| `context` | `messages` | `#{messages}` replaces the context for this request (chain) |
| `before_provider_request` | `model{provider,id}, turnIndex, request{model, systemPrompt, messages, tools, maxTokens, thinkingLevel, temperature}` | `#{systemPrompt?, messages?, maxTokens?, temperature?}` (chain) |
| `after_provider_response` | `model, turnIndex, message` | ignored |
| `tool_call` | `toolCallId, toolName, input` | `#{block: true, reason}` blocks before side effects; the reason is the tool error the model sees (first block wins) |
| `tool_result` | `toolCallId, toolName, input, content, details, isError` | `#{content?, details?, isError?}` (chain) |
| `session_start` | `reason (startup new resume fork clone import reload), sessionId, sessionFile, previousSessionFile` | ignored |
| `session_shutdown` | `reason (quit reload new resume fork), targetSessionFile?` | ignored |
| `session_before_switch` | `reason, targetSessionFile` | `#{cancel}` |
| `session_before_fork` | `entryId, position` | `#{cancel, skipConversationRestore}` |
| `session_before_compact` | `reason (manual threshold overflow), preparation, branchEntries, customInstructions` | `#{cancel}` or `#{compaction: #{summary, firstKeptEntryId, tokensBefore, details}}` |
| `session_compact` | `compactionEntry, fromExtension` | ignored |
| `session_before_tree` | `preparation` | `#{cancel, summary: #{summary, ...}, customInstructions, replaceInstructions, label}` |
| `session_tree` | `oldLeafId, newLeafId, summaryEntryId` | ignored |
| `agent_start`, `agent_end` | `{}`, `aborted, usage` | ignored |
| `turn_start`, `turn_end` | `turnIndex, timestamp` / `turnIndex, message, toolResults` | ignored |
| `message_start`, `message_update`, `message_end` | `message` / `assistantMessageEvent` / `message` | ignored |
| `tool_execution_start`, `_update`, `_end` | `toolCallId, toolName, args` / `partialResult` / `result, isError` | ignored |
| `model_select`, `thinking_level_select` | `model, previousModel, source` / `level, previousLevel` | ignored |
| `user_bash` | `command, excludeFromContext, cwd` | ignored |
| `resources_discover` | `cwd, reason (startup\|reload)` | `#{skillPaths, promptPaths, themePaths}` (relative to the extension directory) |
| `instructions_loaded` | `files` | ignored |

Malformed results are reported (`extension <event> hook failed: <name>: <message>`,
archived wording such as `extension tool_call result field 'block' must be a boolean`)
and leave the event unchanged. Hooks run synchronously on the engine's threads;
`ctx::compact` and dialogs are not available there.

## Subprocess extensions

The executable (relative to the manifest directory) is started for every
invocation in `working_directory` (default: the manifest directory). qt writes
one JSON-RPC 2.0 request line to its stdin and closes it, then reads stdout until
the response with the same `id` (30 s limit). Every stdout line must be a JSON-RPC
message; stderr lines are reported as diagnostics.

| Method | Params | Result |
| --- | --- | --- |
| `qt.command.invoke` | `{name, arguments: {args}}` | any JSON (shown as text: `message`, `text`, `output`, `content[].text`, or the JSON) |
| `qt.tool.invoke` | `{name, arguments}` | Pi tool result `{content: [...], details}`, a string, or any JSON |
| `qt.event.<event>` | the event payload above | as for scripts |

Events are sent only for events listed in `metadata.lifecycle_handlers`
(`[{"event": "tool_call"}]` or `["tool_call"]`). An error response fails the
call with its `message`.

## Pi TypeScript modules

`.ts`/`.js` modules are scanned, not run: `pi.registerCommand`,
`pi.registerTool`, `pi.registerFlag`, `pi.registerProvider`,
`pi.unregisterProvider` and `pi.on` calls with literal arguments (or object
constants) are listed. Invoking their commands or tools fails with
`not_implemented: ... reference modules are not executed by the native extension host`,
and uses of runtime-only APIs (`pi.sendMessage`, `ctx.ui.*`, ...) are reported
as load diagnostics. Port such extensions to Rhai.

## Packages

```sh
qt install <source> [-l]      # add to settings (user, or project with -l) and install
qt remove <source> [-l]       # also: uninstall
qt update [source|self] [--extensions] [--self] [--extension <source>] [--force]
qt list
qt config [list]
qt config enable|disable <source> <extensions|skills|prompts|themes> <resource-path> --package-root <path> [-l]
```

Sources:

- `npm:<package>[@version]`: user scope runs `npm install -g`; project scope
  installs into `<cwd>/.pi/npm` (`npm install <spec> --prefix <cwd>/.pi/npm`).
  `npmCommand` in user settings replaces `npm` (for example `["pnpm"]`).
- git (`git:host/owner/repo[@ref]`, `https://…`, `ssh://…`, `git://…`,
  `git:git@host:owner/repo`): cloned to `<agent_home>/git/<host>/<path>` (user)
  or `<cwd>/.pi/git/<host>/<path>` (project), checked out at `@ref`/`#ref`, then
  `npm install` when the checkout has a `package.json`.
- anything else is a local path (relative to the agent home for user packages,
  to `<cwd>/.pi` for project packages); nothing is installed.

Settings hold `"packages": ["npm:x", {"source": "git:…", "extensions": ["a.rhai"], "skills": ["-skills/old"]}]`.
An object entry filters each resource kind with the patterns above; a kind
without a list is disabled for that package. `qt config enable|disable` adds a
`+path`/`-path` entry. `qt update` skips pinned sources (`npm:x@1`, git `@ref`)
and local paths, and does nothing in offline mode (`PI_OFFLINE`).
