# qt

A native C++17 coding agent for the terminal, with local model inference,
persistent sessions, repository tools, and opt-in governed memory.

qt follows the [Pi](https://github.com/earendil-works/pi-mono) coding-agent architecture
while implementing its runtime natively. It is under active development, not a
claim of complete upstream parity. See the [compatibility notes](docs/upstream-compatibility.md)
for supported behavior and differences.

## What it does

- **Work in your repository:** chat in a terminal UI, inspect code, read and edit
  files, run commands, and use the project index.
- **Choose your inference backend:** use configured remote providers, Ollama,
  or GGUF models through built-in llama.cpp.
- **Keep work across sessions:** resume conversations, branch session history,
  compact context, and export sessions.
- **Extend the agent:** load skills, prompt templates, native AngelScript
  extensions, MCP integrations, and isolated subagents.
- **Review what it remembers:** enable scoped memory, inspect evidence and
  review decisions, and open the browser memory dashboard.

## Build and launch

The current build and release workflows target Linux. Other platforms are not
covered by those workflows. A normal build needs Git, Python 3, PlatformIO Core,
CMake, a C/C++ compiler, and development libraries for SQLite, terminal info,
libunistring, and zlib. Ninja is optional. Full dependency details and GPU
requirements are in [docs/dependencies.md](docs/dependencies.md).

On Debian/Ubuntu, a baseline toolchain can be installed with:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build git pkg-config \
  python3 python3-pip python3-venv libsqlite3-dev libtinfo-dev \
  libunistring-dev zlib1g-dev
```

If PlatformIO is not installed, create a local Python environment from the
repository root:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install platformio
```

From a checkout of this repository:

```sh
git submodule update --init --recursive
pio run -e native
.pio/build/native/qt
```

The first build downloads pinned dependencies and builds the bundled llama.cpp
runtime. The executable is `.pio/build/native/qt`.

The default target detects supported local GPU toolchains. To build explicitly
for CPU-only inference:

```sh
QT_LLAMA_GPU_BACKENDS=cpu pio run -e native
```

Ordinary native builds use the checked-in dashboard assets; they do **not**
require Emscripten. `native_full` is the multi-GPU release target and requires
additional SDKs—it is not the minimal getting-started build.

### First session

Launch qt from the repository you want to work on. Configure authentication
with `/login` for a supported remote provider, or configure a local model as
described below. Use `/model` to select a model, then enter your request.

| Command | Purpose |
| --- | --- |
| `/help` | Show available slash commands. |
| `/model` | Choose a model. |
| `/settings` | Open application settings. |
| `/context` | Inspect the current prompt and context. |
| `/session` | Inspect session information and usage. |
| `/new` / `/resume` | Start fresh or resume another session. |
| `/compact` | Compact the current session context. |
| `/quit` | Exit qt. |

qt can modify files and execute commands. Use version control, inspect changes,
and configure tools and permissions for the workspace you are working in.

## Models and configuration

The default agent home is `~/.qt`. Global settings live in
`~/.qt/settings.json`; `QT_CODING_AGENT_DIR` selects a different agent home.
Provider authentication and session state should stay outside version control.
Inspect the effective configuration with:

```sh
.pio/build/native/qt --inspect-settings --json
```

### Local GGUF models

No model weights are bundled with qt.

Merge the following into your global settings, replacing the model path with
an existing GGUF file. Do not overwrite unrelated settings:

```json
{
  "llama": {
    "models": [
      {
        "id": "local-model",
        "name": "My local model",
        "path": "~/models/model.gguf"
      }
    ]
  }
}
```

Then select it with `/model`, or launch it directly:

```sh
.pio/build/native/qt --provider llama --model local-model
```

`/llama` exposes local-model status and configuration. Models run in-process;
no separate llama.cpp server is required. GPU offload depends on the compiled
backends, available hardware, and model configuration. See the
[runtime troubleshooting guide](docs/dependencies.md#llama-runtime-troubleshooting)
for memory, grammar, embedding, and backend issues. `/ollama` manages separately
running Ollama instances and their models.

### Non-interactive use

With a provider and model configured:

```sh
# Process one request and exit.
.pio/build/native/qt --print "Explain the architecture of this repository"

# Emit JSON-mode output for automation.
.pio/build/native/qt --mode json --print "Summarize the current changes"

# Start the RPC interface.
.pio/build/native/qt --mode rpc

# Show the complete command-line reference.
.pio/build/native/qt --help
```

## Memory, review, and Dreaming

Built-in memory is disabled by default. Enable it deliberately through
`/memory settings`; enabling memory also makes its configured automatic recall
and capture behavior relevant. Some settings take effect on the next runtime.

| Command | Purpose |
| --- | --- |
| `/memory settings` | Configure memory, recall, capture, and Dreaming. |
| `/memory status` | Inspect runtime state and statistics. |
| `/memory review` | Open the governed review workflow. |
| `/memory dashboard` | Open the browser memory dashboard. |

Dreaming is a separate opt-in background exploration workflow. It produces
reviewable artifacts rather than silently treating generated ideas as facts.
Web research is disabled by default and has its own policy setting.

The default promotion policy is `review_only`. The optional `auto_validate`
policy runs a separate, local llama.cpp verifier over fetched research
evidence. Only a supported research successor can become validated; the
original Dream is preserved. Missing evidence, unavailable models, or failed
verification leave the successor pending. This is automated evidence checking,
not a guarantee of factual accuracy.

Pre-handler, Dreaming, and verifier models have searchable selection lists in
`/memory settings`. The verifier picker is restricted to local llama.cpp models.
Graph, dense, and code retrieval remain opt-in; their outstanding acceptance
gates are documented in the [Memory V2 rollout guide](docs/memory-v2-rollout.md).

## Development and verification

```sh
# Build the application.
pio run -e native

# Run the native AngelScript test suite.
pio test -e native

# Run only the framework smoke checks.
QT_AS_TEST_FILTER=framework_smoke pio test -e native

# Restore the production executable after tests.
pio run -e native
```

Tests live under `test/`; focused integration probes live under `scripts/`.
The terminal probes require libvterm development headers and pkg-config; the
full TUI suite also uses tmux. Consult the
[test map](docs/reference-test-map.md) for prerequisites and focused commands.
Framework smoke tests alone are not full feature coverage.

To rebuild the browser dashboard, install Emscripten and CMake 3.24 or newer:

```sh
QT_MEMORY_DASHBOARD_BUILD=1 pio run -e native
```

Keep the generated `dashboard_wasm/web/dist/` assets with changes to the
dashboard source. They are embedded application inputs, not disposable cache
files. The `reference/` checkouts are optional local development references and
are not shipped with this repository.

Runtime logging is opt-in. For interactive debugging, use a file so logs do
not interfere with the terminal or structured output:

```sh
.pio/build/native/qt --log-file qt-debug.log --log-level debug
```

Logs, session exports, and memory databases can contain workspace or prompt
content. Review them before sharing and keep them out of commits.

## Documentation

- [Dependencies and backend setup](docs/dependencies.md)
- [Native extensions](docs/native-extensions.md)
- [Persistent subagents](docs/persistent-subagents.md)
- [Memory source provenance](docs/memory-source-provenance.md)
- [Compatibility and known differences](docs/upstream-compatibility.md)
- [Tests and verification](docs/reference-test-map.md)
- [Current work and remaining gaps](TODO.md)
- [Contributor/agent instructions](AGENTS.md)

## Licensing

The repository does not yet include a top-level license for qt. Third-party code and
assets retain their own notices; see [dependencies](docs/dependencies.md) and
[source provenance](docs/memory-source-provenance.md). Resolve the project
license and review bundled notices before publishing a release.
