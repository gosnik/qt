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

The binaries are `target/release/qt` and `target/release/qt-cli`.

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

Other keys, for example those written by the archived C++ qt, are kept unchanged when
qt rewrites the file.

## Layout

```text
crates/qt-core   UI-agnostic runtime: config, providers, tools, agent loop, sessions
crates/qt-app    GPUI desktop application (binary: qt)
crates/qt-cli    headless print mode (binary: qt-cli)
archived/        previous C++17 TUI implementation (reference only)
reference/       Pi source used as the behavioural reference (not tracked)
```

See [PLAN.md](PLAN.md) for status and remaining work, and
[docs/upstream-compatibility.md](docs/upstream-compatibility.md) for differences from Pi and
the archived qt.
