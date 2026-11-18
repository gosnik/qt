//! `qt-cli --help` (archived `print_help`, with the binary name and the
//! desktop-app note of the Rust port).

pub const HELP: &str = "\
qt-cli - AI coding assistant with read, bash, edit, write tools

Usage:
  qt-cli [options] [@files...] [messages...]

Commands:
  qt-cli install <source> [-l]     Install extension source and add to settings
  qt-cli remove <source> [-l]      Remove extension source from settings
  qt-cli uninstall <source> [-l]   Alias for remove
  qt-cli update [source|self|pi]   Update qt and installed extensions
  qt-cli list                      List installed extensions from settings
  qt-cli config                    Enable/disable package resources
  qt-cli memory <command>          Manage built-in governed memory
  qt-cli <command> --help          Show help for install/remove/uninstall/update/list

Options:
  --provider <name>              Provider name
  --model <pattern>              Model pattern or ID (supports \"provider/id\" and optional \":<thinking>\")
  --api-key <key>                API key (defaults to env vars)
  --system-prompt <text>         System prompt (default: coding assistant prompt)
  --append-system-prompt <text>  Append text or file contents to the system prompt (can be used multiple times)
  --mode <mode>                  Output mode: text (default), json, or rpc
  --print, -p                    Non-interactive mode: process prompt and exit
  --continue, -c                 Continue previous session
  --resume, -r                   Select a session to resume
  --session <path|id>            Use specific session file or partial UUID
  --fork <path|id>               Fork specific session file or partial UUID into a new session
  --session-dir <dir>            Directory for session storage and lookup
  --no-session                   Don't save session (ephemeral)
  --models <patterns>            Comma-separated model patterns for model cycling
                                 Supports globs (anthropic/*, *sonnet*) and fuzzy matching
  --no-tools, -nt                Disable all tools by default (built-in and extension)
  --no-builtin-tools, -nbt       Disable built-in tools by default but keep extension/custom tools enabled
  --no-memory                    Disable built-in memory for this invocation
  --tools, -t <tools>            Comma-separated allowlist of tool names to enable
                                 Applies to built-in, extension, and custom tools
  --thinking <level>             Set thinking level: off, minimal, low, medium, high, xhigh
  --extension, -e <path>         Load an extension file (can be used multiple times)
  --no-extensions, -ne           Disable extension discovery (explicit -e paths still work)
  --skill <path>                 Load a skill file or directory (can be used multiple times)
  --no-skills, -ns               Disable skills discovery and loading
  --prompt-template <path>       Load a prompt template file or directory (can be used multiple times)
  --no-prompt-templates, -np     Disable prompt template discovery and loading
  --theme <path>                 Load a theme file or directory (can be used multiple times)
  --no-themes                    Disable theme discovery and loading
  --no-context-files, -nc        Disable AGENTS.md and CLAUDE.md discovery and loading
  --discover-local-models <dir>  Scan local llama.cpp/GGUF model directory (repeatable; comma-separated dirs accepted)
  --export <file>                Export session file to HTML and exit
  --list-models [search]         List available models (with optional fuzzy search)
  --verbose                      Force verbose startup (overrides quietStartup setting)
  --offline                      Disable startup network operations (same as PI_OFFLINE=1)
  --log-file <path>              Write structured runtime logs to a file (repeatable)
  --log-stderr                   Also write runtime logs to stderr
  --log-level <level>            Log level: trace, debug, info, warn, error, critical, off
  --help, -h                     Show this help
  --version, -v                  Show version number

Extensions can register additional flags (e.g., --plan from plan-mode extension).
The interactive interface is the qt desktop app; qt-cli runs prompts and exits.

Examples:
  # Run a prompt and print the answer
  qt-cli -p \"List all .ts files in src/\"

  # Include files in the initial message
  qt-cli @prompt.md @image.png \"What color is the sky?\"

  # Several messages, one after the other
  qt-cli \"Read package.json\" \"What dependencies do we have?\"

  # Continue the previous session
  qt-cli --continue \"What did we discuss?\"

  # Use a different model
  qt-cli --provider openai --model gpt-4o-mini \"Help me refactor this code\"

  # Use a model with provider prefix (no --provider needed)
  qt-cli --model openai/gpt-4o \"Help me refactor this code\"

  # Use a model with thinking level shorthand
  qt-cli --model sonnet:high \"Solve this complex problem\"

  # Read-only mode (no file modifications possible)
  qt-cli --tools read,grep,find,ls -p \"Review the code in src/\"

  # Pi JSON event stream
  qt-cli --mode json \"Summarise this repository\"

  # Export a session file to HTML
  qt-cli --export ~/.qt/sessions/--path--/session.jsonl
  qt-cli --export session.jsonl output.html

Environment Variables:
  ANTHROPIC_API_KEY                - Anthropic Claude API key
  ANTHROPIC_OAUTH_TOKEN            - Anthropic OAuth token (alternative to API key)
  OPENAI_API_KEY                   - OpenAI GPT API key
  AZURE_OPENAI_API_KEY             - Azure OpenAI API key
  AZURE_OPENAI_BASE_URL            - Azure OpenAI/Cognitive Services base URL (e.g. https://{resource}.openai.azure.com)
  AZURE_OPENAI_RESOURCE_NAME       - Azure OpenAI resource name (alternative to base URL)
  AZURE_OPENAI_API_VERSION         - Azure OpenAI API version (default: v1)
  AZURE_OPENAI_DEPLOYMENT_NAME_MAP - Azure OpenAI model=deployment map (comma-separated)
  DEEPSEEK_API_KEY                 - DeepSeek API key
  GROQ_API_KEY                     - Groq API key
  CEREBRAS_API_KEY                 - Cerebras API key
  XAI_API_KEY                      - xAI Grok API key
  FIREWORKS_API_KEY                - Fireworks API key
  TOGETHER_API_KEY                 - Together AI API key
  OPENROUTER_API_KEY               - OpenRouter API key
  AI_GATEWAY_API_KEY               - Vercel AI Gateway API key
  ZAI_API_KEY                      - ZAI API key
  MINIMAX_API_KEY                  - MiniMax API key
  MOONSHOT_API_KEY                 - Moonshot AI API key
  OPENCODE_API_KEY                 - OpenCode Zen/OpenCode Go API key
  KIMI_API_KEY                     - Kimi For Coding API key
  CLOUDFLARE_API_KEY               - Cloudflare API token (Workers AI and AI Gateway)
  CLOUDFLARE_ACCOUNT_ID            - Cloudflare account id (required for both)
  CLOUDFLARE_GATEWAY_ID            - Cloudflare AI Gateway slug (required for AI Gateway)
  XIAOMI_API_KEY                   - Xiaomi MiMo API key (api.xiaomimimo.com billing)
  QT_CODING_AGENT_DIR              - Config directory (default: ~/.qt)
  QT_SUBAGENT_EXECUTABLE           - Override the qt-cli executable used for child agents
  QT_SUBAGENT_MAX_DEPTH            - Maximum nested subagent depth (default: 2)
  QT_LOG_FILE, QT_LOG_STDERR, QT_LOG_LEVEL - Runtime logging (see --log-*)
  PI_CODING_AGENT_SESSION_DIR      - Session storage directory (overridden by --session-dir)
  PI_OFFLINE                       - Disable startup network operations when set to 1/true/yes
  PI_SHARE_VIEWER_URL              - Base URL for /share command (default: https://pi.dev/session/)

Built-in Tool Names:
  read   - Read file contents
  bash   - Execute bash commands
  edit   - Edit files with find/replace
  write  - Write files (creates/overwrites)
  grep   - Search file contents
  find   - Find files by glob pattern
  ls     - List directory contents
  symbol - Find code definitions with the project code index
  analyze_file - Analyze indexed symbols and calls in one source file
  references - Trace indexed callers and callees
  build_context - Show per-file compiler flags and include edges
  search_index - Search the persisted project full-text index
  index  - Discover and invoke grouped in-process project index operations
  subagent - Run named isolated child agents (single, parallel, or chain)
";
