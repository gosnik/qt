#include "qt/app/cli/cli.hpp"

#include <argparse/argparse.hpp>

#include "qt/foundation/logging.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace qt::app {
namespace {

enum class EmptyCsvFields {
  discard,
  keep,
};

std::vector<std::string> split_csv(
    std::string_view text,
    EmptyCsvFields empty_fields = EmptyCsvFields::discard) {
  std::vector<std::string> values;
  std::size_t start = 0;
  while (start <= text.size()) {
    const auto end = text.find(',', start);
    const auto part = text.substr(start, end == std::string_view::npos
                                             ? std::string_view::npos
                                             : end - start);
    std::string trimmed(part);
    trimmed.erase(trimmed.begin(),
                  std::find_if(trimmed.begin(), trimmed.end(), [](char ch) {
                    return std::isspace(static_cast<unsigned char>(ch)) == 0;
                  }));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](char ch) {
                    return std::isspace(static_cast<unsigned char>(ch)) == 0;
                  }).base(),
                  trimmed.end());
    if (!trimmed.empty() || empty_fields == EmptyCsvFields::keep) {
      values.push_back(std::move(trimmed));
    }
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }
  return values;
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

constexpr std::string_view kArgparseEscapedValuePrefix = "\x1f";

bool is_optional_like(std::string_view text) {
  return !text.empty() && text.front() == '-';
}

bool is_file_arg(std::string_view text) {
  return !text.empty() && text.front() == '@';
}

bool is_print_prompt_candidate(std::string_view text) {
  return !is_file_arg(text) &&
         (!is_optional_like(text) || starts_with(text, "---"));
}

bool list_models_takes_search(const std::vector<std::string>& args,
                              std::size_t index) {
  return index + 1 < args.size() && !args[index + 1].empty() &&
         args[index + 1].front() != '-' && args[index + 1].front() != '@';
}

std::optional<std::string> canonical_value_option(std::string_view arg) {
  if (arg == "--mode") {
    return "--mode";
  }
  if (arg == "--provider") {
    return "--provider";
  }
  if (arg == "--model") {
    return "--model";
  }
  if (arg == "--api-key") {
    return "--api-key";
  }
  if (arg == "--system-prompt") {
    return "--system-prompt";
  }
  if (arg == "--append-system-prompt") {
    return "--append-system-prompt";
  }
  if (arg == "--session") {
    return "--session";
  }
  if (arg == "--fork") {
    return "--fork";
  }
  if (arg == "--session-dir") {
    return "--session-dir";
  }
  if (arg == "--models") {
    return "--models";
  }
  if (arg == "--tools" || arg == "-t") {
    return "--tools";
  }
  if (arg == "--thinking") {
    return "--thinking";
  }
  if (arg == "--export") {
    return "--export";
  }
  if (arg == "--extension" || arg == "-e") {
    return "--extension";
  }
  if (arg == "--skill") {
    return "--skill";
  }
  if (arg == "--prompt-template") {
    return "--prompt-template";
  }
  if (arg == "--theme") {
    return "--theme";
  }
  if (arg == "--discover-local-models") {
    return "--discover-local-models";
  }
  return std::nullopt;
}

void configure_cli_program(argparse::ArgumentParser& program) {
  program.add_argument("--help", "-h")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--version", "-v")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--mode")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--continue", "-c")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--resume", "-r")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--provider")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--model")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--api-key")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--system-prompt")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--append-system-prompt")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--no-session")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--session")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--fork")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--session-dir")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--models")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--no-tools", "-nt")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--no-builtin-tools", "-nbt")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--no-memory")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--tools", "-t")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--thinking")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--print", "-p")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--export")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--extension", "-e")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--no-extensions", "-ne")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--skill")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--prompt-template")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--theme")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--no-skills", "-ns")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--no-prompt-templates", "-np")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--no-themes")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--no-context-files", "-nc")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--discover-local-models")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--list-models")
      .default_value(std::vector<std::string>{})
      .append()
      .nargs(argparse::nargs_pattern::optional);
  program.add_argument("--verbose")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--offline")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--log-file")
      .default_value(std::vector<std::string>{})
      .append();
  program.add_argument("--log-stderr")
      .default_value(false)
      .implicit_value(true);
  program.add_argument("--log-level")
      .default_value(std::vector<std::string>{})
      .append();
}

std::string escape_argparse_value(std::string_view value) {
  return std::string(kArgparseEscapedValuePrefix) + std::string(value);
}

std::string unescape_argparse_value(std::string value) {
  if (starts_with(value, kArgparseEscapedValuePrefix)) {
    return value.substr(kArgparseEscapedValuePrefix.size());
  }
  return value;
}

std::vector<std::string> unescape_argparse_values(
    const std::vector<std::string>& values) {
  std::vector<std::string> unescaped;
  unescaped.reserve(values.size());
  for (const auto& value : values) {
    unescaped.push_back(unescape_argparse_value(value));
  }
  return unescaped;
}

std::vector<std::string> normalize_cli_args_for_argparse(
    const std::vector<std::string>& args) {
  std::vector<std::string> normalized;
  normalized.reserve(args.size());
  for (std::size_t index = 0; index < args.size(); ++index) {
    const auto& arg = args[index];
    if (arg == "--list-models") {
      if (list_models_takes_search(args, index)) {
        normalized.push_back(arg);
        normalized.push_back(escape_argparse_value(args[++index]));
      } else {
        normalized.push_back(arg);
      }
      continue;
    }

    const auto canonical = canonical_value_option(arg);
    if (canonical && index + 1 < args.size()) {
      normalized.push_back(*canonical);
      normalized.push_back(escape_argparse_value(args[++index]));
      continue;
    }

    normalized.push_back(arg);
  }
  return normalized;
}

std::optional<std::string> last_value(const std::vector<std::string>& values) {
  if (values.empty()) {
    return std::nullopt;
  }
  return values.back();
}

void classify_remaining_cli_args(const std::vector<std::string>& args,
                                 ReferenceCliArgs& parsed) {
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (!arg.empty() && arg.front() == '@') {
      parsed.file_args.push_back(arg.substr(1));
    } else if (arg.rfind("--", 0) == 0) {
      const auto eq = arg.find('=');
      if (eq != std::string::npos) {
        parsed.unknown_flags[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
      } else {
        const std::string flag_name = arg.substr(2);
        if (i + 1 < args.size() && !args[i + 1].empty() &&
            args[i + 1].front() != '-' && args[i + 1].front() != '@') {
          parsed.unknown_flags[flag_name] = args[++i];
        } else {
          parsed.unknown_flags[flag_name] = true;
        }
      }
    } else if (arg.size() > 1 && arg.front() == '-') {
      parsed.diagnostics.push_back(
          {ReferenceCliDiagnosticSeverity::error, "Unknown option: " + arg});
    } else {
      parsed.messages.push_back(arg);
    }
  }
}

void preserve_missing_value_flags(const std::vector<std::string>& raw_args,
                                  ReferenceCliArgs& parsed) {
  for (std::size_t index = 0; index < raw_args.size(); ++index) {
    const auto canonical = canonical_value_option(raw_args[index]);
    if (!canonical) {
      continue;
    }
    if (index + 1 >= raw_args.size()) {
      parsed.unknown_flags[canonical->substr(2)] = true;
    }
  }
}

void recover_print_prompt_message(const std::vector<std::string>& raw_args,
                                  ReferenceCliArgs& parsed) {
  if (!parsed.print) {
    return;
  }
  for (std::size_t index = 0; index < raw_args.size(); ++index) {
    const auto& arg = raw_args[index];
    if (arg != "--print" && arg != "-p") {
      continue;
    }
    if (index + 1 >= raw_args.size() ||
        !is_print_prompt_candidate(raw_args[index + 1])) {
      return;
    }
    const auto& prompt = raw_args[index + 1];
    if (starts_with(prompt, "---")) {
      parsed.unknown_flags.erase(prompt.substr(2));
    }
    if (std::find(parsed.messages.begin(), parsed.messages.end(), prompt) ==
        parsed.messages.end()) {
      parsed.messages.insert(parsed.messages.begin(), prompt);
    }
    return;
  }
}

}  // namespace

bool is_valid_thinking_level(std::string_view level) {
  static const std::vector<std::string_view> levels = {
      "off", "minimal", "low", "medium", "high", "xhigh"};
  return std::find(levels.begin(), levels.end(), level) != levels.end();
}

ReferenceCliArgs parse_cli_args(const std::vector<std::string>& args) {
  ReferenceCliArgs parsed;
  argparse::ArgumentParser program("qt");
  configure_cli_program(program);

  const auto normalized_args = normalize_cli_args_for_argparse(args);
  std::vector<std::string> argv_like;
  argv_like.reserve(normalized_args.size() + 1);
  argv_like.push_back("qt");
  argv_like.insert(argv_like.end(), normalized_args.begin(), normalized_args.end());

  const auto unknown_args = program.parse_known_args(argv_like);

  parsed.help = program.get<bool>("--help");
  parsed.version = program.get<bool>("--version");
  if (const auto value = last_value(unescape_argparse_values(
          program.get<std::vector<std::string>>("--mode")));
      value && (*value == "text" || *value == "json" || *value == "rpc")) {
    parsed.mode = *value;
  }
  parsed.continue_session = program.get<bool>("--continue");
  parsed.resume = program.get<bool>("--resume");
  parsed.provider =
      last_value(unescape_argparse_values(program.get<std::vector<std::string>>(
          "--provider")));
  parsed.model =
      last_value(unescape_argparse_values(program.get<std::vector<std::string>>(
          "--model")));
  parsed.api_key =
      last_value(unescape_argparse_values(program.get<std::vector<std::string>>(
          "--api-key")));
  parsed.system_prompt = last_value(unescape_argparse_values(
      program.get<std::vector<std::string>>("--system-prompt")));
  parsed.append_system_prompt = unescape_argparse_values(
      program.get<std::vector<std::string>>("--append-system-prompt"));
  parsed.no_session = program.get<bool>("--no-session");
  parsed.session = last_value(unescape_argparse_values(
      program.get<std::vector<std::string>>("--session")));
  parsed.fork = last_value(unescape_argparse_values(
      program.get<std::vector<std::string>>("--fork")));
  parsed.session_dir = last_value(unescape_argparse_values(
      program.get<std::vector<std::string>>("--session-dir")));
  if (const auto value = last_value(unescape_argparse_values(
          program.get<std::vector<std::string>>("--models")));
      value) {
    parsed.models = split_csv(*value, EmptyCsvFields::keep);
  }
  parsed.no_tools = program.get<bool>("--no-tools");
  parsed.no_builtin_tools = program.get<bool>("--no-builtin-tools");
  parsed.no_memory = program.get<bool>("--no-memory");
  if (const auto value = last_value(unescape_argparse_values(
          program.get<std::vector<std::string>>("--tools")));
      value) {
    parsed.tools = split_csv(*value);
  }
  if (const auto value = last_value(unescape_argparse_values(
          program.get<std::vector<std::string>>("--thinking")));
      value) {
    if (is_valid_thinking_level(*value)) {
      parsed.thinking = *value;
    } else {
      parsed.diagnostics.push_back(
          {ReferenceCliDiagnosticSeverity::warning,
           "Invalid thinking level \"" + *value +
               "\". Valid values: off, minimal, low, medium, high, xhigh"});
    }
  }
  parsed.print = program.get<bool>("--print");
  parsed.export_path = last_value(unescape_argparse_values(
      program.get<std::vector<std::string>>("--export")));
  parsed.extensions = unescape_argparse_values(
      program.get<std::vector<std::string>>("--extension"));
  parsed.no_extensions = program.get<bool>("--no-extensions");
  parsed.skills = unescape_argparse_values(
      program.get<std::vector<std::string>>("--skill"));
  parsed.prompt_templates = unescape_argparse_values(
      program.get<std::vector<std::string>>("--prompt-template"));
  parsed.themes = unescape_argparse_values(
      program.get<std::vector<std::string>>("--theme"));
  parsed.no_skills = program.get<bool>("--no-skills");
  parsed.no_prompt_templates = program.get<bool>("--no-prompt-templates");
  parsed.no_themes = program.get<bool>("--no-themes");
  parsed.no_context_files = program.get<bool>("--no-context-files");
  for (const auto& value : unescape_argparse_values(
           program.get<std::vector<std::string>>("--discover-local-models"))) {
    const auto directories = split_csv(value);
    parsed.local_model_dirs.insert(parsed.local_model_dirs.end(),
                                   directories.begin(), directories.end());
  }
  if (program.is_used("--list-models")) {
    if (const auto value =
            last_value(unescape_argparse_values(
                program.get<std::vector<std::string>>("--list-models")));
        value && !value->empty()) {
      parsed.list_models = *value;
    } else {
      parsed.list_models_all = true;
    }
  }
  parsed.verbose = program.get<bool>("--verbose");
  parsed.offline = program.get<bool>("--offline");
  parsed.log_files = unescape_argparse_values(
      program.get<std::vector<std::string>>("--log-file"));
  parsed.log_stderr = program.get<bool>("--log-stderr");
  parsed.log_level = last_value(unescape_argparse_values(
      program.get<std::vector<std::string>>("--log-level")));
  if (parsed.log_level && !qt::foundation::parse_log_level(*parsed.log_level)) {
    parsed.diagnostics.push_back(
        {ReferenceCliDiagnosticSeverity::warning,
         "Invalid log level \"" + *parsed.log_level +
             "\". Valid values: trace, debug, info, warn, error, critical, off"});
  }

  std::vector<std::string> remaining;
  remaining.reserve(unknown_args.size());
  remaining = unknown_args;
  classify_remaining_cli_args(remaining, parsed);
  preserve_missing_value_flags(args, parsed);
  recover_print_prompt_message(args, parsed);

  return parsed;
}

bool has_native_mode(const std::vector<std::string>& args) {
  static const std::vector<std::string> mode_flags = {
      "--inspect-context",
      "--inspect-context-resources",
      "--inspect-prompt-assembly",
      "--inspect-skills",
      "--inspect-prompt-templates",
      "--inspect-slash-commands",
      "--inspect-settings",
      "--inspect-app-resources",
      "--inspect-system-prompt",
      "--inspect-extensions",
      "--inspect-session",
      "--resume-session",
      "--inspect-session-catalog",
      "--create-session",
      "--fork-session",
      "--clone-session",
      "--compact-session",
      "--inspect-extension-runners",
      "--rpc-envelope",
      "--rpc-frame",
      "--read-text",
      "--list-directory",
      "--complete-path",
      "--search-text",
      "--write-text",
      "--append-text",
      "--edit",
      "--bash",
      "--demo-agent-write",
  };
  return std::any_of(args.begin(), args.end(), [&](const std::string& arg) {
    return std::find(mode_flags.begin(), mode_flags.end(), arg) != mode_flags.end();
  });
}

ReferenceStartupMode resolve_startup_mode(const ReferenceCliArgs& parsed,
                                                    bool stdin_is_tty) {
  if (parsed.mode == "rpc") {
    return ReferenceStartupMode::rpc;
  }
  if (parsed.mode == "json") {
    return ReferenceStartupMode::json;
  }
  if (parsed.print || !stdin_is_tty) {
    return ReferenceStartupMode::print;
  }
  return ReferenceStartupMode::interactive;
}

void print_help(std::ostream& out) {
  out
      << "qt - AI coding assistant with read, bash, edit, write tools\n\n"
      << "Usage:\n"
      << "  qt [options] [@files...] [messages...]\n\n"
      << "Commands:\n"
      << "  qt install <source> [-l]     Install extension source and add to settings\n"
      << "  qt remove <source> [-l]      Remove extension source from settings\n"
      << "  qt uninstall <source> [-l]   Alias for remove\n"
      << "  qt update [source|self|pi]   Update qt and installed extensions\n"
      << "  qt list                      List installed extensions from settings\n"
      << "  qt config                    Open TUI to enable/disable package resources\n"
      << "  qt memory <command>          Manage built-in governed memory\n"
      << "  qt <command> --help          Show help for install/remove/uninstall/update/list\n\n"
      << "Options:\n"
      << "  --provider <name>              Provider name (default: google)\n"
      << "  --model <pattern>              Model pattern or ID (supports \"provider/id\" and optional \":<thinking>\")\n"
      << "  --api-key <key>                API key (defaults to env vars)\n"
      << "  --system-prompt <text>         System prompt (default: coding assistant prompt)\n"
      << "  --append-system-prompt <text>  Append text or file contents to the system prompt (can be used multiple times)\n"
      << "  --mode <mode>                  Output mode: text (default), json, or rpc\n"
      << "  --print, -p                    Non-interactive mode: process prompt and exit\n"
      << "  --continue, -c                 Continue previous session\n"
      << "  --resume, -r                   Select a session to resume\n"
      << "  --session <path|id>            Use specific session file or partial UUID\n"
      << "  --fork <path|id>               Fork specific session file or partial UUID into a new session\n"
      << "  --session-dir <dir>            Directory for session storage and lookup\n"
      << "  --no-session                   Don't save session (ephemeral)\n"
      << "  --models <patterns>            Comma-separated model patterns for Ctrl+P cycling\n"
      << "                                 Supports globs (anthropic/*, *sonnet*) and fuzzy matching\n"
      << "  --no-tools, -nt                Disable all tools by default (built-in and extension)\n"
      << "  --no-builtin-tools, -nbt       Disable built-in tools by default but keep extension/custom tools enabled\n"
      << "  --no-memory                    Disable built-in memory for this invocation\n"
      << "  --tools, -t <tools>            Comma-separated allowlist of tool names to enable\n"
      << "                                 Applies to built-in, extension, and custom tools\n"
      << "  --thinking <level>             Set thinking level: off, minimal, low, medium, high, xhigh\n"
      << "  --extension, -e <path>         Load an extension file (can be used multiple times)\n"
      << "  --no-extensions, -ne           Disable extension discovery (explicit -e paths still work)\n"
      << "  --skill <path>                 Load a skill file or directory (can be used multiple times)\n"
      << "  --no-skills, -ns               Disable skills discovery and loading\n"
      << "  --prompt-template <path>       Load a prompt template file or directory (can be used multiple times)\n"
      << "  --no-prompt-templates, -np     Disable prompt template discovery and loading\n"
      << "  --theme <path>                 Load a theme file or directory (can be used multiple times)\n"
      << "  --no-themes                    Disable theme discovery and loading\n"
      << "  --no-context-files, -nc        Disable AGENTS.md and CLAUDE.md discovery and loading\n"
      << "  --discover-local-models <dir>  Scan local llama.cpp/GGUF model directory (repeatable; comma-separated dirs accepted)\n"
      << "  --export <file>                Export session file to HTML and exit\n"
      << "  --list-models [search]         List available models (with optional fuzzy search)\n"
      << "  --verbose                      Force verbose startup (overrides quietStartup setting)\n"
      << "  --offline                      Disable startup network operations (same as PI_OFFLINE=1)\n"
      << "  --log-file <path>              Write structured runtime logs to a file (repeatable)\n"
      << "  --log-stderr                   Also write runtime logs to stderr\n"
      << "  --log-level <level>            Log level: trace, debug, info, warn, error, critical, off\n"
      << "  --help, -h                     Show this help\n"
      << "  --version, -v                  Show version number\n\n"
      << "Extensions can register additional flags (e.g., --plan from plan-mode extension).\n\n"
      << "Examples:\n"
      << "  # Interactive mode\n"
      << "  qt\n\n"
      << "  # Interactive mode with initial prompt\n"
      << "  qt \"List all .ts files in src/\"\n\n"
      << "  # Include files in initial message\n"
      << "  qt @prompt.md @image.png \"What color is the sky?\"\n\n"
      << "  # Non-interactive mode (process and exit)\n"
      << "  qt -p \"List all .ts files in src/\"\n\n"
      << "  # Multiple messages (interactive)\n"
      << "  qt \"Read package.json\" \"What dependencies do we have?\"\n\n"
      << "  # Continue previous session\n"
      << "  qt --continue \"What did we discuss?\"\n\n"
      << "  # Use different model\n"
      << "  qt --provider openai --model gpt-4o-mini \"Help me refactor this code\"\n\n"
      << "  # Use model with provider prefix (no --provider needed)\n"
      << "  qt --model openai/gpt-4o \"Help me refactor this code\"\n\n"
      << "  # Use model with thinking level shorthand\n"
      << "  qt --model sonnet:high \"Solve this complex problem\"\n\n"
      << "  # Limit model cycling to specific models\n"
      << "  qt --models claude-sonnet,claude-haiku,gpt-4o\n\n"
      << "  # Limit to a specific provider with glob pattern\n"
      << "  qt --models \"github-copilot/*\"\n\n"
      << "  # Cycle models with fixed thinking levels\n"
      << "  qt --models sonnet:high,haiku:low\n\n"
      << "  # Start with a specific thinking level\n"
      << "  qt --thinking high \"Solve this complex problem\"\n\n"
      << "  # Read-only mode (no file modifications possible)\n"
      << "  qt --tools read,grep,find,ls -p \"Review the code in src/\"\n\n"
      << "  # Export a session file to HTML\n"
      << "  qt --export ~/.qt/sessions/--path--/session.jsonl\n"
      << "  qt --export session.jsonl output.html\n\n"
      << "Environment Variables:\n"
      << "  ANTHROPIC_API_KEY                - Anthropic Claude API key\n"
      << "  ANTHROPIC_OAUTH_TOKEN            - Anthropic OAuth token (alternative to API key)\n"
      << "  OPENAI_API_KEY                   - OpenAI GPT API key\n"
      << "  AZURE_OPENAI_API_KEY             - Azure OpenAI API key\n"
      << "  AZURE_OPENAI_BASE_URL            - Azure OpenAI/Cognitive Services base URL (e.g. https://{resource}.openai.azure.com)\n"
      << "  AZURE_OPENAI_RESOURCE_NAME       - Azure OpenAI resource name (alternative to base URL)\n"
      << "  AZURE_OPENAI_API_VERSION         - Azure OpenAI API version (default: v1)\n"
      << "  AZURE_OPENAI_DEPLOYMENT_NAME_MAP - Azure OpenAI model=deployment map (comma-separated)\n"
      << "  DEEPSEEK_API_KEY                 - DeepSeek API key\n"
      << "  GEMINI_API_KEY                   - Google Gemini API key\n"
      << "  GROQ_API_KEY                     - Groq API key\n"
      << "  CEREBRAS_API_KEY                 - Cerebras API key\n"
      << "  XAI_API_KEY                      - xAI Grok API key\n"
      << "  FIREWORKS_API_KEY                - Fireworks API key\n"
      << "  TOGETHER_API_KEY                 - Together AI API key\n"
      << "  OPENROUTER_API_KEY               - OpenRouter API key\n"
      << "  AI_GATEWAY_API_KEY               - Vercel AI Gateway API key\n"
      << "  ZAI_API_KEY                      - ZAI API key\n"
      << "  MISTRAL_API_KEY                  - Mistral API key\n"
      << "  MINIMAX_API_KEY                  - MiniMax API key\n"
      << "  MOONSHOT_API_KEY                 - Moonshot AI API key\n"
      << "  OPENCODE_API_KEY                 - OpenCode Zen/OpenCode Go API key\n"
      << "  KIMI_API_KEY                     - Kimi For Coding API key\n"
      << "  CLOUDFLARE_API_KEY               - Cloudflare API token (Workers AI and AI Gateway)\n"
      << "  CLOUDFLARE_ACCOUNT_ID            - Cloudflare account id (required for both)\n"
      << "  CLOUDFLARE_GATEWAY_ID            - Cloudflare AI Gateway slug (required for AI Gateway)\n"
      << "  XIAOMI_API_KEY                   - Xiaomi MiMo API key (api.xiaomimimo.com billing)\n"
      << "  XIAOMI_TOKEN_PLAN_CN_API_KEY     - Xiaomi MiMo Token Plan API key (China region)\n"
      << "  XIAOMI_TOKEN_PLAN_AMS_API_KEY    - Xiaomi MiMo Token Plan API key (Amsterdam region)\n"
      << "  XIAOMI_TOKEN_PLAN_SGP_API_KEY    - Xiaomi MiMo Token Plan API key (Singapore region)\n"
      << "  AWS_PROFILE                      - AWS profile for Amazon Bedrock\n"
      << "  AWS_ACCESS_KEY_ID                - AWS access key for Amazon Bedrock\n"
      << "  AWS_SECRET_ACCESS_KEY            - AWS secret key for Amazon Bedrock\n"
      << "  AWS_BEARER_TOKEN_BEDROCK         - Bedrock API key (bearer token)\n"
      << "  AWS_REGION                       - AWS region for Amazon Bedrock (e.g., us-east-1)\n"
      << "  QT_CODING_AGENT_DIR              - Config directory (default: ~/.qt)\n"
      << "  QT_SUBAGENT_EXECUTABLE           - Override the qt executable used for child agents\n"
      << "  QT_SUBAGENT_MAX_DEPTH            - Maximum nested subagent depth (default: 2)\n"
      << "  PI_CODING_AGENT_SESSION_DIR      - Session storage directory (overridden by --session-dir)\n"
      << "  PI_PACKAGE_DIR                   - Override package directory (for Nix/Guix store paths)\n"
      << "  PI_OFFLINE                       - Disable startup network operations when set to 1/true/yes\n"
      << "  PI_TELEMETRY                     - Override install telemetry when set to 1/true/yes or 0/false/no\n"
      << "  PI_SHARE_VIEWER_URL              - Base URL for /share command (default: https://pi.dev/session/)\n\n"
      << "Built-in Tool Names:\n"
      << "  read   - Read file contents\n"
      << "  bash   - Execute bash commands\n"
      << "  edit   - Edit files with find/replace\n"
      << "  write  - Write files (creates/overwrites)\n"
      << "  grep   - Search file contents (read-only, off by default)\n"
      << "  find   - Find files by glob pattern (read-only, off by default)\n"
      << "  ls     - List directory contents (read-only, off by default)\n"
      << "  symbol - Find C/C++ definitions with the project code index\n"
      << "  analyze_file - Analyze indexed symbols and calls in one source file\n"
      << "  references - Trace indexed C/C++ callers and callees\n"
      << "  build_context - Show per-file compiler flags and include edges\n"
      << "  search_index - Search the persisted project full-text index\n"
      << "  index  - Discover and invoke grouped in-process project index operations\n"
      << "  subagent - Run named isolated child agents (single, parallel, or chain)\n";
}

}  // namespace qt::app
