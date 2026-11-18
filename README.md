# qt

A desktop coding agent written in Rust with a [GPUI](https://www.gpui.rs) interface. It
reads and edits files and runs commands in your project, streaming the model's answers
and tool calls into a native window.

qt follows the [Pi](https://github.com/earendil-works/pi-mono) coding-agent architecture
(tools, prompts, session format). The previous C++17 terminal implementation is kept for
reference under [`archived/`](archived/README.md).

## Features

- **Native desktop UI** on Linux, macOS and Windows, built with GPUI and
  [gpui-component](https://github.com/longbridge/gpui-kit): streamed markdown, collapsible
  thinking, tool cards with live bash output and coloured edit diffs, light and dark themes.
- **Providers:** Anthropic, OpenAI, OpenRouter, and any number of Ollama instances. Models are
  discovered from each provider and chosen from a searchable picker.
- **Pi tool set:** `read`, `bash`, `edit`, `write`, `grep`, `find`, `ls`. It uses Pi's schemas,
  2000-line / 50 KB output limits and `.gitignore`-aware search.
- **Sessions** are saved as Pi v3 JSONL in `~/.qt/sessions`. You can resume them from the
  sidebar, including sessions written by the archived C++ qt.
- **Project instructions:** `AGENTS.md` / `CLAUDE.md` files from the project's ancestor
  directories, plus `~/.qt/AGENTS.md`, are added to the system prompt.
- **Headless mode:** `qt-cli` runs one prompt and streams the answer to stdout.

## Build

You need a current stable Rust toolchain from [rustup](https://rustup.rs) (developed with 1.98). On Linux, GPUI also needs the
development packages for its windowing and font stack:

```sh
sudo apt-get install build-essential pkg-config libxkbcommon-dev libxkbcommon-x11-dev \
  libwayland-dev libxcb1-dev libvulkan-dev libfontconfig-dev libfreetype-dev libssl-dev
```

macOS needs the Xcode command-line tools. Windows needs the MSVC toolchain.

```sh
cargo run --release -p qt-app            # desktop app for the current directory
cargo run --release -p qt-app -- ~/code/myproject
cargo run --release -p qt-cli -- "summarise this repository"
```

The binaries are `target/release/qt` and `target/release/qt-cli`. Both are built by the
`qt-runtime` composition root, so they share the llama.cpp provider, project index, MCP
servers, governed memory, extensions and subagents. Subagent workers, the memory
pre-handler and Dreaming run `qt-cli` children, so keep `qt-cli` next to `qt` (or set
`QT_SUBAGENT_EXECUTABLE`). In-process llama.cpp is built when its source is present
(`git submodule update --init third_party/llama.cpp` or `QT_LLAMA_CPP_DIR`).

To install, `packaging/scripts/package.sh` builds both binaries and packages them for
the host: a `.tar.gz` (with `install.sh` for `~/.local`), AppImage and `.deb` on Linux,
an unsigned `qt.app` (zipped) on macOS, and a `.zip` plus `.msi` on Windows, all in
`target/dist/`. It uses [cargo-packager](https://crates.io/crates/cargo-packager)
(`cargo install cargo-packager --version 0.11.8 --locked`); see
[`packaging/README.md`](packaging/README.md). CI (`.github/workflows/ci.yml`) runs fmt,
clippy and tests on Linux, macOS and Windows, and `.github/workflows/release.yml` builds
the same packages as workflow artifacts.

## First run

1. Open **API keys** in the sidebar and add a key for Anthropic, OpenAI or OpenRouter. Keys are
   stored in `~/.qt/auth.json` with owner-only permissions. qt also reads
   `ANTHROPIC_API_KEY`, `OPENAI_API_KEY` and `OPENROUTER_API_KEY`. A local
   [Ollama](https://ollama.com) server at `http://127.0.0.1:11434` is detected automatically.
2. Pick a model in the header. The choice is saved as `defaultProvider` / `defaultModel`.
3. Type a request:
   - **Enter** sends.
   - **Shift+Enter** inserts a newline.
   - **Esc** stops the agent.
   - **Ctrl/Cmd+N** starts a new session.
   - **Ctrl/Cmd+B** toggles the sidebar.

qt can modify files and execute commands in the project directory. Use version control and
review its changes.

### Command line

`qt-cli` runs prompts without the window and exits (`qt-cli --help` lists everything). The
desktop app is the interactive interface; `qt` itself only takes a project directory.

```sh
qt-cli -p "summarise this repository"            # print the final answer
git diff | qt-cli -p "review this diff"           # stdin becomes the prompt
qt-cli @notes.md @screenshot.png "what is wrong?" # attach files and images
qt-cli --mode json "fix the failing test"         # Pi JSON event lines (session header first)
qt-cli -c "and now the docs"                      # continue the latest session
qt-cli --model anthropic/claude-sonnet-4-5:high "plan the refactor"
qt-cli --tools read,grep,find,ls -p "review src/"  # read-only run
qt-cli --list-models sonnet                       # model table, optional fuzzy search
qt-cli --export session.jsonl out.html            # session to HTML
qt-cli install npm:@scope/pkg | remove | update | list | config
qt-cli memory status                              # governed memory commands
```

| Flag | Meaning |
| --- | --- |
| `-p`, `--print`; `--mode text\|json` | Print the final answer, or stream Pi JSON events (exit code 1 when a run fails). JSON mode waits for persistent subagent workers and prints `subagent_event` frames |
| `--provider`, `--model <id\|provider/id[:thinking]>`, `--thinking`, `--api-key`, `--models` | Model selection (`--api-key` is used for this run only), thinking level and model scope |
| `-c`/`--continue`, `-r`/`--resume`, `--session <path\|id>`, `--fork <path\|id>`, `--no-session`, `--session-dir` | Session selection and storage |
| `-t`/`--tools a,b`, `-nt`/`--no-tools`, `-nbt`/`--no-builtin-tools`, `--no-memory` | Tool allowlist (built-in, index, memory, subagent, MCP and extension tools) and memory |
| `--system-prompt`, `--append-system-prompt`, `--skill`, `-ns`, `--prompt-template`, `-np`, `--theme`, `--no-themes`, `-nc`/`--no-context-files` | Prompt resources |
| `-e`/`--extension <file>`, `-ne`/`--no-extensions`, `--<flag>` | Extensions and the flags they register |
| `--discover-local-models <dir>`, `--offline`, `--log-file`, `--log-stderr`, `--log-level`, `-v`, `-h` | Local GGUF discovery, no network, logging (`QT_LOG_FILE`, `QT_LOG_STDERR`, `QT_LOG_LEVEL`) |

## Configuration

The agent home is `~/.qt`. Set `QT_CODING_AGENT_DIR` to use a different directory.
`settings.json` keys used by this version:

| Key | Meaning |
| --- | --- |
| `defaultProvider`, `defaultModel` | Model selected at startup |
| `maxOutputTokens` | Output budget per response (default 16384) |
| `ollama.instances[]` | `{ "id", "baseUrl", "enabled", "models" }` Ollama servers |
| `providers.<id>.baseUrl` | Override the endpoint for `anthropic`, `openai` or `openrouter` |
| `gui.theme` | `"light"` or `"dark"`; unset follows the system |
| `theme` | Pi theme name (built-in `dark`/`light` or a theme file) |
| `workspaceRoot`, `allowExternalFilesystemAccess` | Workspace confinement of path tools (a project file may only narrow it) |
| `sessionDir` | Session directory (`--session-dir` and `PI_CODING_AGENT_SESSION_DIR` win) |
| `enabledModels` | Model scope for the picker (`--models` for one run) |
| `compaction.*`, `retry.*`, `steeringMode`, `followUpMode` | Auto-compaction, auto-retry and queue modes |
| `llama.*` | In-process llama.cpp: backend, context, generation, KV cache, grammar, embeddings, `models[]` |
| `mcpServers` | MCP servers (also read from `mcp.json` files) |
| `memory.*` | Governed memory (`memory.enabled`, recall, capture, pre-handler, Dreaming, retrieval) |
| `packages` | Extension packages managed by `qt-cli install/remove/update/config` |

Other keys, for example those written by the archived C++ qt, are kept unchanged when
qt rewrites the file.

## Layout

```text
crates/qt-core        UI-agnostic runtime: config, providers, tools, agent loop, sessions,
                      slash commands, prompt resources, hooks
crates/qt-runtime     composition root: builds the engine with every subsystem, the
                      /index, /mcp, /llama, /subagent commands and package resources
crates/qt-app         GPUI desktop application (binary: qt): transcript, composer,
                      panels (/index, /mcp, /llama, /subagent, /ollama, /tools, ...)
                      and the extension UI
crates/qt-cli         headless print / JSON modes and subcommands (binary: qt-cli)
crates/qt-llama       in-process llama.cpp provider: settings, GGUF discovery,
                      offload planning, sampling, runtime
crates/qt-llama-sys   llama.cpp build and bindings (third_party/llama.cpp)
crates/qt-index       project index: scanner, build metadata, C/C++ code index,
                      SQLite/FTS5 store, index tools
crates/qt-mcp         MCP client: config discovery, stdio/HTTP/SSE transports,
                      proxy and resource tools, sampling
crates/qt-memory      governed memory: store, recall, capture, review, Dreaming
crates/qt-extensions  extensions: discovery, Rhai runtime, JSON-RPC subprocesses,
                      packages, ctx.ui API
crates/qt-subagents   subagent definitions, the subagent tool and persistent workers
archived/             previous C++17 TUI implementation (reference only)
reference/            Pi source used as the behavioural reference (not tracked)
```

See [PLAN.md](PLAN.md) for status and remaining work, and
[docs/upstream-compatibility.md](docs/upstream-compatibility.md) for differences from Pi and
the archived qt.
