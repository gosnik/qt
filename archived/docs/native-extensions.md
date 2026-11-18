# Native AngelScript extensions

Load a native extension with `qt -e path/to/extension.as`. Explicit paths remain
active with `--no-extensions`, which disables automatic discovery. Each loaded
extension owns an AngelScript engine: `activate()` runs once per load, globals
survive command/tool/event invocations, and reload creates a new instance.
Reference JavaScript/TypeScript files still support registration discovery only.

Use `/extensions` to inspect loaded extensions, including explicit `-e` paths
and extensions discovered in project/user directories, alongside installed
package resources. Standalone entries show `loaded` and their source path and
are read-only; remove their CLI path or discovery file to stop loading them.
Package entries retain their `on`/`off` toggles unless also loaded through a CLI
or auto-discovery path, which package settings cannot disable. Those overlapping
entries appear once as read-only `loaded` rows. `/extensions <filter>` matches
names, descriptions, and paths case-insensitively.

## Registration and execution

```angelscript
int calls = 0;

void activate() {
  registerCommand("hello", "Hello", "Show a native extension dialog");
  registerFlag("greeting", "string", "\"Hello\"", "Greeting text");
}

string command_hello(const string &in arguments_json) {
  calls++;
  qt_host_call("ctx.ui.notify", "{\"message\":\"Hello from AngelScript\"}");
  return "{\"text\":\"Command completed\"}";
}
```

Command handlers are named `command_<name>` and tool handlers `tool_<name>`.
Non-alphanumeric characters in contribution names become underscores. Avoid
names that normalize to the same handler. `qt_on(event, handler)` registers an
explicit lifecycle handler name. `registerShortcut(key, handler, description)`
registers a native shortcut; active overlays retain input focus.
`registerMessageRenderer(customType, handler)` (or
`qt_register_message_renderer`) registers a renderer returning `{lines:[...]}`.
It receives `type:"render"`, message content/details, width, and expansion state.

Handlers accept one JSON-encoded string and return a JSON-encoded string.
`void handler(const string &in)` and `void handler()` are also supported.
Registration functions and their `qt_register_*` aliases run during `activate()`;
calling them later raises an exception rather than silently ignoring the call.

`qt_host_call(method, arguments_json)` invokes a native API and returns JSON.
Arguments are objects. Host failures become script exceptions. Use
`qt_json_get(document, key)` to read an encoded field and
`qt_json_string(value)` to decode a JSON string; missing fields return JSON null.
No JavaScript evaluator or additional runtime dependency is required.

## Host calls

| Method | Arguments / result |
| --- | --- |
| `pi.getFlag` | `{name}`; explicit CLI value or registered default |
| `pi.getCommands` | Available command descriptions |
| `pi.getSessionName`, `pi.setSessionName` | Get the name or set `{name}` |
| `pi.appendEntry` | `{customType, data}`; persist extension state |
| `pi.setLabel` | `{entryId, label}`; null label clears it |
| `pi.sendMessage` | `{customType, content, display, details, triggerTurn, deliverAs}`; append or queue a custom message |
| `pi.sendUserMessage` | `{content, deliverAs}`; submit or queue a user turn; interactive turns start after the handler returns |
| `pi.getActiveTools`, `pi.getAllTools` | Active names or configured tool descriptions/schemas |
| `pi.setActiveTools` | `{toolNames: [...]}`; validates names before changing them |
| `pi.getThinkingLevel`, `pi.setThinkingLevel` | Get or set `{level}`, clamped to the model |
| `pi.setModel` | `{provider, id}`; false when selection would use the offline echo provider |
| `pi.exec` | `{command, args, cwd, timeout}`; argv execution and stdout/stderr/code/killed |
| `ctx.getModel` | Current model metadata; optional `{provider, id}` inspects a catalog model |
| `ctx.getModelCatalog` | Configured providers and models, including capabilities and public metadata |
| `ctx.getSystemPrompt`, `ctx.getContextUsage` | Current system prompt and `{tokens, contextWindow, percent}` usage |
| `ctx.getContext` | Resolved persisted-session messages (before hooks/augmentation), system prompt, thinking level, model identity, and usage |
| `ctx.compact` | `{customInstructions?}`; synchronously compact an idle session and return the summary result |
| `ctx.isIdle`, `ctx.hasPendingMessages`, `ctx.abort` | Turn status and cancellation |

Session-name and extension-state writes made during a turn are queued by the
harness. Messages support string content or text/image blocks (`mimeType` and
base64 `data` for images), with `steer` or `followUp` delivery while running.
Other direct runtime mutations require an idle harness. Interactive
command persistence writes every new session entry, including changes made
before a later handler error. Script exceptions do not roll back earlier effects.
`ctx.compact` follows that idle-only native rule because `qt_host_call` is
synchronous. It returns `{summary, firstKeptEntryId, tokensBefore, details}`;
failure becomes a script exception. After compaction, context usage retains the
window but reports unknown `tokens` and `percent` until another model response.
Otherwise usage starts with the last valid assistant's provider total and adds
native token estimates for messages appended after that response.
`ctx.getContext` is a resolved persisted-session snapshot and does not run
context hooks, memory augmentation, or initiate a provider request. Its model
and thinking level are the active runtime values. Model inspection exposes only
an explicit public metadata allowlist; provider credentials and arbitrary
configured headers are never returned.

## UI calls

Native dialogs use `ctx.ui.select` (`title`, `options`), `confirm` (`title`,
`message`), `input` (`title`, optional `placeholder`), and `editor` (`title`,
optional `prefill`). Cancellation returns null, or false for confirmation; an
empty submitted string remains distinct from cancellation. `timeout` is in
milliseconds. Notifications use `notify` with `message` and optional `notifyType`.

`setEditorText` and `pasteToEditor` accept `text`; `getEditorText` returns it.
`setStatus` uses `statusKey` and nullable `statusText`. Working-indicator methods
configure message, visibility, and frame strings. `setWidget` accepts keyed line
arrays; `setHeader` and `setFooter` accept static line arrays or null to reset.
These are native data contracts, not JavaScript component factories.

`ctx.ui.custom({handler: "panel"})` opens a scripted component. Its handler
receives `{type:"render",width,height}` and returns `{lines:[...]}`; input events
contain `{type:"input",data}` and may return `{handled:true,done:...}` to finish.
Use the JSON helpers instead of relying on object key order. Component errors
propagate to the invoking command, and modal callbacks are released on close.

`onTerminalInput({handler})` calls a named handler with `{type:"input",data}`;
return a string to forward input or null to consume it. Set `handler` to null to
remove the interceptor. `setEditorComponent({handler})` uses the same render/input
events as a custom panel; null restores the built-in editor. `getEditorComponent`
returns the configured handler descriptor or null.

`addAutocompleteProvider({handler})` calls the handler with `{lines,row,column,force}`.
Return null or `{prefix,items:[{value,label,description}]}`. Providers with matching
prefixes combine their items. Themes are available through `getAllThemes`,
`getTheme({name})`, and `setTheme({name})`; loaded theme accent colors affect the
terminal display. `getToolsExpanded`/`setToolsExpanded({expanded})` control tool
components. Reload clears extension input, editor, and autocomplete callbacks.

## Lifecycle hooks

`before_agent_start` receives prompt text, images, and the system prompt. Its
returned `message` injects a custom message; `systemPrompt` replaces the system
prompt for that turn, including an empty replacement. `context` can replace
`messages`. `tool_call` can return `{block:true,reason}` before tool side effects;
`tool_result` can replace `content` and `isError` before the next model request.
Print/JSON mode saves session entries created by startup, turn, and shutdown
handlers, including on error exits. Hook failures are reported on stderr.

Headless modes return cancelled dialog values and write notifications to stderr.
Their supported presentation setters have no visual effect; unknown methods
return errors. Scripts retain execution step limits. Nested component callbacks
are bounded, and concurrent entry into one script runtime is rejected.

This bridge is connected to interactive and print/JSON startup runtimes. RPC
still uses its separate runtime and does not execute these native host/UI calls.
Interactive lifecycle callbacks on the UI thread can use the presenter; worker
thread callbacks use the headless host. Full RPC presenter integration remains
tracked in EXT-01.

## Astra automatic compaction example

Load the opt-in plugin from the repository root:

```sh
qt -e examples/extensions/astra-auto-compact.as
```

The plugin checks the current model ID and display name for the substring
`astra` (ASCII case-insensitive). Before the next idle interactive input, it calls
`ctx.compact` only when `ctx.getContextUsage().tokens` is **greater than 200000**.
Exactly 200000, unknown usage, other models, and active turns are skipped. This
uses current context occupancy, not cumulative session billing totals.

The input text and images continue unchanged. If compaction fails, the plugin
warns and allows the prompt to proceed with the existing context; later input
can retry. Idle input-hook session entries are persisted, including a completed
compaction before a later hook error. Successful `ctx.compact` also replaces
the live agent transcript with the resolved summary and retained messages,
without discarding queued input. The plugin does not alter built-in
auto-compaction settings or install itself globally.

This is intentionally an interactive `input` hook, not Pi's asynchronous
`turn_end` example: native `turn_end` and `agent_end` run before the harness is
idle. It does not compact mid-turn or automatically in print/RPC mode.

## Verification

- `python3 scripts/extension_runtime_probe.py`: real engine state, host calls,
  exceptions, nested invocation, JSON helpers, execution limits, and Astra plugin
  threshold/model matching, busy/unknown-usage guards, and failure recovery.
- `python3 scripts/extension_host_probe.py`: real interactive loading, flags,
  repeated invocation, dialog cancellation, scripted components, and session writes.
- `python3 scripts/extension_lifecycle_probe.py`: local provider requests verify
  system-prompt replacement, custom message injection, tool blocking, result
  replacement, and lifecycle session persistence.
- `python3 scripts/angelscript_context_probe.py`: context and model inspection,
  custom-instruction compaction, persisted summaries, and post-compaction usage.
- `python3 scripts/astra_auto_compact_probe.py`: actual Astra plugin with a local
  provider, threshold boundaries, prompt continuation, and durable compaction.
