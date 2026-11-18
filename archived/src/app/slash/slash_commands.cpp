#include <qt/app/slash/slash_commands.hpp>

#include "qt/app/slash/llama_slash_commands.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

SlashCommand make_builtin(std::string name, std::string description,
                          std::optional<std::string> argument_hint = std::nullopt) {
  SlashCommand command;
  command.name = std::move(name);
  command.description = std::move(description);
  command.argument_hint = std::move(argument_hint);
  command.source = SlashCommandSource::kBuiltin;
  return command;
}

bool has_name(const std::set<std::string>& names, const std::string& name) {
  return names.find(name) != names.end();
}

bool starts_with(std::string_view text, std::string_view prefix) {
  return prefix.size() <= text.size() &&
         text.substr(0, prefix.size()) == prefix;
}

bool ends_with_case_insensitive(std::string_view text, std::string_view suffix) {
  if (suffix.size() > text.size()) {
    return false;
  }

  const std::size_t offset = text.size() - suffix.size();
  for (std::size_t index = 0; index < suffix.size(); ++index) {
    const char lhs =
        static_cast<char>(std::tolower(static_cast<unsigned char>(text[offset + index])));
    const char rhs =
        static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[index])));
    if (lhs != rhs) {
      return false;
    }
  }
  return true;
}

bool is_space(char value) {
  return std::isspace(static_cast<unsigned char>(value)) != 0;
}

bool is_package_resource_settings_target(std::string_view value) {
  return value == "extensions" || value == "skills" || value == "prompts" ||
         value == "themes" || value == "resources" || value == "packages";
}

bool is_package_resource_type(std::string_view value) {
  return value == "extensions" || value == "skills" || value == "prompts" ||
         value == "themes";
}

std::string trim_copy(std::string_view text) {
  std::size_t start = 0;
  while (start < text.size() && is_space(text[start])) {
    ++start;
  }

  std::size_t end = text.size();
  while (end > start && is_space(text[end - 1])) {
    --end;
  }

  return std::string(text.substr(start, end - start));
}

std::string normalize_command_name(std::string_view name) {
  if (!name.empty() && name.front() == '/') {
    name.remove_prefix(1);
  }
  return std::string(name);
}

std::pair<std::string, std::string> split_first_argument(
    std::string_view arguments) {
  std::string_view text = arguments;
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return {"", ""};
  }

  std::size_t first_end = 0;
  while (first_end < text.size() && !is_space(text[first_end])) {
    ++first_end;
  }

  std::string_view rest = text.substr(first_end);
  while (!rest.empty() && is_space(rest.front())) {
    rest.remove_prefix(1);
  }
  return {std::string(text.substr(0, first_end)), trim_copy(rest)};
}

std::optional<std::string> parse_optional_path_argument(
    std::string_view arguments) {
  const std::string trimmed = trim_copy(arguments);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  if (trimmed.front() != '"' && trimmed.front() != '\'') {
    return trimmed;
  }

  const char quote = trimmed.front();
  std::string path;
  for (std::size_t index = 1; index < trimmed.size(); ++index) {
    const char value = trimmed[index];
    if (value == quote) {
      return path;
    }
    path.push_back(value);
  }

  return path;
}

bool replace_all(std::string& text, const std::string& needle,
                 const std::string& replacement) {
  bool replaced = false;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string::npos) {
    text.replace(offset, needle.size(), replacement);
    offset += replacement.size();
    replaced = true;
  }
  return replaced;
}

void normalize_legacy_prompt_placeholders(std::string& template_text) {
  replace_all(template_text, "{{args}}", "__QT_RAW_ARGUMENTS__");
  replace_all(template_text, "{{arguments}}", "__QT_RAW_ARGUMENTS__");

  for (std::size_t index = 1; index < template_text.size(); ++index) {
    if (template_text[index - 1] == '$' && template_text[index] == '{') {
      std::size_t end = index + 1;
      while (end < template_text.size() && std::isdigit(
                                             static_cast<unsigned char>(
                                                 template_text[end])) != 0) {
        ++end;
      }
      if (end > index + 1 && end < template_text.size() &&
          template_text[end] == '}') {
        const std::string digits = template_text.substr(index + 1, end - index - 1);
        template_text.replace(index - 1, end - index + 2, "$" + digits);
        index += digits.size();
      }
    }

    if (index + 3 >= template_text.size() || template_text[index - 1] != '{' ||
        template_text[index] != '{') {
      continue;
    }

    std::size_t end = index + 1;
    while (end < template_text.size() && std::isdigit(
                                           static_cast<unsigned char>(
                                               template_text[end])) != 0) {
      ++end;
    }
    if (end == index + 1 || end + 1 >= template_text.size() ||
        template_text[end] != '}' || template_text[end + 1] != '}') {
      continue;
    }

    const std::string digits = template_text.substr(index + 1, end - index - 1);
    template_text.replace(index - 1, end - index + 3, "$" + digits);
    index += digits.size();
  }
}

std::string expand_prompt_template(std::string template_text,
                                   std::string_view arguments) {
  normalize_legacy_prompt_placeholders(template_text);

  const std::string trimmed_arguments = trim_copy(arguments);
  const std::string expanded = qt::harness::expand_prompt_template_content(
      template_text, std::string(arguments));
  std::string finalized = expanded;
  replace_all(finalized, "__QT_RAW_ARGUMENTS__", trimmed_arguments);
  return finalized;
}

std::string expand_skill_command(const SlashCommand& command,
                                 std::string_view arguments) {
  std::string output =
      command.expansion_template.empty()
          ? "Use the skill \"" + command.name.substr(6) + "\"."
          : command.expansion_template;
  const std::string trimmed_arguments = trim_copy(arguments);
  if (!trimmed_arguments.empty()) {
    output += "\n\nUser: " + trimmed_arguments;
  }
  return output;
}

std::string render_slash_command_help(const SlashCommandRegistry& registry,
                                      std::string_view arguments) {
  const std::string prefix = trim_copy(arguments);
  const std::vector<SlashCommand> commands =
      prefix.empty() ? registry.commands
                     : filter_slash_commands_by_prefix(registry, prefix);

  std::string output = "Available slash commands";
  if (!prefix.empty()) {
    output += " matching '/" + normalize_command_name(prefix) + "'";
  }
  output += ":";

  for (const auto& command : commands) {
    output += "\n/";
    output += command.name;
    if (command.argument_hint) {
      output += " ";
      output += *command.argument_hint;
    }
    output += " [";
    output += to_string(command.source);
    output += "]";
    if (!command.description.empty()) {
      output += " - ";
      output += command.description;
    }
  }

  if (commands.empty()) {
    output += "\n(no matching slash commands)";
  }
  return output;
}

qt::foundation::Result<std::string> execute_builtin_command(
    const SlashCommandRegistry& registry, const SlashCommandInvocation& invocation) {
  if (invocation.name == "help") {
    return render_slash_command_help(registry, invocation.arguments);
  }
  if (invocation.name == "model") {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "builtin slash command '/model' requires runtime model selection");
  }
  if (invocation.name == "session") {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "builtin slash command '/session' requires runtime session inspection");
  }

  return qt::foundation::Result<std::string>::failure(
      qt::foundation::ErrorCode::invalid_argument,
      "builtin slash command '/" + invocation.name +
          "' has no deterministic app-layer execution");
}

std::optional<SlashCommandRuntimeOperation> plan_builtin_runtime_operation(
    const SlashCommandInvocation& invocation) {
  if (invocation.name == "settings" &&
      trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kOpenSettings;
    operation.steps.push_back("Open the settings selector.");
    operation.steps.push_back(
        "Runtime should apply selected settings changes and refresh visible state.");
    return operation;
  }

  if (invocation.name == "settings") {
    SlashCommandRuntimeOperation operation;
    const auto target_and_filter = split_first_argument(invocation.arguments);
    const std::string& target = target_and_filter.first;
    if (is_package_resource_settings_target(target)) {
      operation.kind = SlashCommandRuntimeOperationKind::kOpenPackageResourceConfig;
      operation.value = target;
      if (!target_and_filter.second.empty()) {
        operation.custom_instructions = target_and_filter.second;
      }
      operation.steps.push_back(
          "Open the package resource selector focused on the requested resource type.");
      if (!target_and_filter.second.empty()) {
        operation.steps.push_back(
            "Use the remaining command arguments as the initial resource filter.");
      }
      operation.steps.push_back(
          "Runtime should enable or disable package-backed and local resources in "
          "user or project settings without leaving interactive mode.");
      return operation;
    }

    operation.kind = SlashCommandRuntimeOperationKind::kOpenSettings;
    operation.value = target;
    operation.steps.push_back(
        "Open the settings selector focused on the requested setting.");
    operation.steps.push_back(
        "Runtime should apply selected settings changes and refresh visible state.");
    return operation;
  }

  if (invocation.name == "model") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kSelectModel;
    const std::string search = trim_copy(invocation.arguments);
    if (!search.empty()) {
      operation.value = search;
      operation.steps.push_back(
          "Open the model selector with the command arguments as the search term.");
    } else {
      operation.steps.push_back("Open the model selector.");
    }
    operation.steps.push_back(
        "Runtime should update the active session model and refresh footer state.");
    return operation;
  }

  if (invocation.name == "llama") {
    return plan_llama_slash_command(invocation.arguments);
  }

  if (invocation.name == "ollama") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kOllamaProviderOperation;
    const std::string arguments = trim_copy(invocation.arguments);
    if (!arguments.empty()) {
      operation.custom_instructions = arguments;
      operation.value = split_first_argument(arguments).first;
    }
    operation.steps.push_back(
        arguments.empty() ? "Open the Ollama instance menu."
                          : "Route the command to the configured Ollama instance.");
    return operation;
  }

  if (invocation.name == "index") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kIndexOperation;
    const std::string arguments = trim_copy(invocation.arguments);
    if (!arguments.empty()) {
      operation.custom_instructions = arguments;
      operation.value = split_first_argument(arguments).first;
      operation.steps.push_back(
          "Route the command to the in-process grouped index tool.");
    } else {
      operation.steps.push_back("Open the index operation submenu.");
    }
    operation.steps.push_back(
        "Runtime should support discovery through /index tree and full operation "
        "execution through /index call <operation> [json-arguments].");
    return operation;
  }

  if (invocation.name == "subagent") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kSubagentOperation;
    const std::string arguments = trim_copy(invocation.arguments);
    if (!arguments.empty()) {
      operation.custom_instructions = arguments;
      operation.value = split_first_argument(arguments).first;
      operation.steps.push_back(
          "Route the command to the native subagent discovery surface.");
    } else {
      operation.steps.push_back("Open the configured subagent menu.");
    }
    operation.steps.push_back(
        "Subagent execution remains isolated and uses the existing model-facing "
        "subagent tool contract.");
    return operation;
  }

  if (invocation.name == "memory") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kMemoryOperation;
    const std::string arguments = trim_copy(invocation.arguments);
    if (!arguments.empty()) {
      operation.custom_instructions = arguments;
      operation.value = split_first_argument(arguments).first;
      operation.steps.push_back(
          "Route the command to the in-process governed memory services.");
    } else {
      operation.steps.push_back("Open the governed memory operation submenu.");
    }
    return operation;
  }

  if (invocation.name == "mcp") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kMcpOperation;
    const std::string arguments = trim_copy(invocation.arguments);
    if (!arguments.empty()) {
      operation.custom_instructions = arguments;
      operation.value = split_first_argument(arguments).first;
      operation.steps.push_back(
          "Route the command to the in-process MCP command handler.");
    } else {
      operation.steps.push_back("Open the MCP server submenu.");
    }
    operation.steps.push_back(
        "Runtime should show server status, registered tools, resources, "
        "configuration paths, and allow reconnect/reload of MCP tools.");
    return operation;
  }

  if (invocation.name == "scoped-models" &&
      trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kSelectScopedModels;
    operation.steps.push_back("Open the scoped-models selector.");
    operation.steps.push_back(
        "Runtime should persist enabled model patterns and refresh cycling state.");
    return operation;
  }

  if (invocation.name == "compact") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kCompactSessionContext;
    const std::string custom_instructions = trim_copy(invocation.arguments);
    if (!custom_instructions.empty()) {
      operation.custom_instructions = custom_instructions;
      operation.steps.push_back(
          "Request manual session compaction with custom instructions.");
    } else {
      operation.steps.push_back("Request manual session compaction.");
    }
    operation.steps.push_back(
        "Runtime should emit compaction_start, compact the active session context, "
        "and emit compaction_end.");
    return operation;
  }

  if (invocation.name == "tree") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kNavigateSessionTree;
    const std::string selection_hint = trim_copy(invocation.arguments);
    if (!selection_hint.empty()) {
      operation.value = selection_hint;
      operation.steps.push_back(
          "Open the active session tree navigator with the requested initial selection hint.");
      operation.steps.push_back(
          "Use the command arguments as the initial tree selection hint.");
    } else {
      operation.steps.push_back("Open the active session tree navigator.");
    }
    operation.steps.push_back(
        "Runtime should select a target entry, optionally summarize the branch, "
        "navigate the active session, and refresh rendered messages.");
    return operation;
  }

  if (invocation.name == "export") {
    SlashCommandRuntimeOperation operation;
    operation.target_path = parse_optional_path_argument(invocation.arguments);
    const bool export_jsonl =
        operation.target_path &&
        ends_with_case_insensitive(*operation.target_path, ".jsonl");
    operation.kind = export_jsonl
                         ? SlashCommandRuntimeOperationKind::kExportSessionJsonl
                         : SlashCommandRuntimeOperationKind::kExportSessionHtml;
    if (export_jsonl) {
      operation.steps.push_back(
          "Export the active session to JSONL at the requested path.");
    } else if (operation.target_path) {
      operation.steps.push_back(
          "Export the active session to HTML at the requested path.");
    } else {
      operation.steps.push_back(
          "Export the active session to HTML using the runtime default path.");
    }
    if (export_jsonl) {
      operation.steps.push_back(
          "Runtime should serialize the active session JSONL without mutating "
          "the active leaf.");
    } else {
      operation.steps.push_back(
          "Runtime should render the active session with session-safe escaping "
          "and write the HTML export.");
    }
    return operation;
  }

  if (invocation.name == "import") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kImportSessionJsonl;
    operation.target_path = parse_optional_path_argument(invocation.arguments);
    if (operation.target_path) {
      operation.steps.push_back(
          "Import and resume the requested JSONL session path.");
    } else {
      operation.steps.push_back("Report usage: /import <path.jsonl>.");
    }
    operation.steps.push_back(
        "Runtime should validate the source file, resolve any missing cwd, and "
        "switch to the imported session.");
    return operation;
  }

  if (invocation.name == "share" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kShareSessionGist;
    operation.steps.push_back(
        "Export the active session to shareable HTML in a temporary runtime path.");
    operation.steps.push_back(
        "Runtime should upload the HTML as a private GitHub gist, derive the "
        "configured share viewer URL, and present it to the user.");
    return operation;
  }

  if (invocation.name == "copy" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kCopyLastAssistantText;
    operation.steps.push_back("Copy the last assistant text message to the clipboard.");
    operation.steps.push_back(
        "Runtime should report when no assistant message is available.");
    return operation;
  }

  if (invocation.name == "name") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kSetSessionName;
    const std::string name = trim_copy(invocation.arguments);
    if (!name.empty()) {
      operation.value = name;
      operation.steps.push_back("Set the active session display name.");
    } else {
      operation.steps.push_back(
          "Show the current session name or report usage when unnamed.");
    }
    operation.steps.push_back(
        "Runtime should persist the session name and refresh session metadata.");
    return operation;
  }

  if (invocation.name == "changelog" &&
      trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kShowChangelog;
    operation.steps.push_back("Show changelog entries in the conversation.");
    return operation;
  }

  if (invocation.name == "hotkeys" &&
      trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kShowHotkeys;
    operation.steps.push_back("Show keyboard shortcuts in the conversation.");
    return operation;
  }

  if (invocation.name == "fork" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kForkSessionFromUserMessage;
    operation.steps.push_back("Open the user-message selector for forking.");
    operation.steps.push_back(
        "Runtime should create a new session from the selected user message.");
    return operation;
  }

  if (invocation.name == "clone" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kCloneSession;
    operation.steps.push_back("Clone the current active branch into a new session.");
    operation.steps.push_back(
        "Runtime should report when there is no session content to clone.");
    return operation;
  }

  if (invocation.name == "login") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kOpenLoginSelector;
    const std::string provider = trim_copy(invocation.arguments);
    if (!provider.empty()) {
      operation.value = provider;
      operation.steps.push_back(
          "Start login for the requested provider without a full provider picker when possible.");
    } else {
      operation.steps.push_back("Open the login provider selector.");
    }
    operation.steps.push_back(
        "Runtime should run the selected OAuth or API-key login flow.");
    return operation;
  }

  if (invocation.name == "logout") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kOpenLogoutSelector;
    const std::string provider = trim_copy(invocation.arguments);
    if (!provider.empty()) {
      operation.value = provider;
      operation.steps.push_back(
          "Target the requested provider for logout without a full provider picker when possible.");
    } else {
      operation.steps.push_back("Open the logout provider selector.");
    }
    operation.steps.push_back(
        "Runtime should remove stored credentials only for the selected provider.");
    return operation;
  }

  if (invocation.name == "new" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kStartNewSession;
    operation.steps.push_back("Start a new session.");
    operation.steps.push_back(
        "Runtime should create and switch to a fresh active session.");
    return operation;
  }

  if (invocation.name == "resume" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kResumeSession;
    operation.steps.push_back("Open the session resume selector.");
    operation.steps.push_back(
        "Runtime should switch to the selected previous session.");
    return operation;
  }

  if (invocation.name == "reload" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kReloadRuntimeResources;
    operation.steps.push_back(
        "Reload keybindings, extensions, skills, prompts, themes, and context files.");
    operation.steps.push_back(
        "Runtime should refuse reload while a response or compaction is active.");
    return operation;
  }

  if (invocation.name == "session") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kShowSessionInfo;
    const std::string focus = trim_copy(invocation.arguments);
    if (!focus.empty()) {
      operation.value = focus;
      operation.steps.push_back(
          "Show current session file, id, message count, tokens, and cost with the requested focus.");
      operation.steps.push_back(
          "Use the command arguments as a session inspection subsection or filter hint.");
    } else {
      operation.steps.push_back(
          "Show current session file, id, message count, tokens, and cost.");
    }
    operation.steps.push_back(
        "Runtime should render persisted session metadata without mutating session state.");
    return operation;
  }

  if (invocation.name == "context") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kShowContextInfo;
    const std::string focus = trim_copy(invocation.arguments);
    if (!focus.empty()) {
      operation.value = focus;
      operation.steps.push_back(
          "Show active context details focused on the requested subsection.");
    } else {
      operation.steps.push_back(
          "Show prompt context files, skills, prompt templates, extensions, "
          "tools, and restored session context.");
    }
    operation.steps.push_back(
        "Runtime should render context visibility without mutating session state.");
    return operation;
  }

  if (invocation.name == "status") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kShowRuntimeStatus;
    const std::string focus = trim_copy(invocation.arguments);
    if (!focus.empty()) {
      operation.value = focus;
      operation.steps.push_back(
          "Show active runtime status focused on the requested subsection.");
    } else {
      operation.steps.push_back(
          "Show active session, model, thinking, tool, provider, and extension status.");
    }
    operation.steps.push_back(
        "Runtime should render status without mutating session state.");
    return operation;
  }

  if (is_package_resource_type(invocation.name)) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kOpenPackageResourceSelector;
    operation.value = invocation.name;
    const std::string filter = trim_copy(invocation.arguments);
    if (!filter.empty()) {
      operation.custom_instructions = filter;
      operation.steps.push_back(
          "Open the package resource selector with the requested resource type and initial filter.");
    } else {
      operation.steps.push_back(
          "Open the package resource selector focused on the requested resource type.");
    }
    operation.steps.push_back(
        "Runtime should enable or disable package-backed and local resources in "
        "user or project settings without leaving interactive mode.");
    return operation;
  }

  if (invocation.name == "tools") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kOpenToolsSelector;
    const std::string filter = trim_copy(invocation.arguments);
    if (!filter.empty()) {
      operation.value = filter;
      operation.steps.push_back(
          "Open the tools selector with the command arguments as an initial filter.");
    } else {
      operation.steps.push_back("Open the tools selector.");
    }
    operation.steps.push_back(
        "Runtime should persist enabled tool choices and refresh tool availability.");
    return operation;
  }

  if (invocation.name == "thinking") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kSetThinkingLevel;
    const std::string level = trim_copy(invocation.arguments);
    if (!level.empty()) {
      operation.value = level;
      operation.steps.push_back(
          "Set the active thinking level from the command arguments.");
    } else {
      operation.steps.push_back("Open the thinking level selector.");
    }
    operation.steps.push_back(
        "Runtime should validate model support, persist the level, and refresh footer state.");
    return operation;
  }

  if (invocation.name == "quit" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kQuitApplication;
    operation.steps.push_back("Quit the application.");
    return operation;
  }

  if (invocation.name == "plan" && trim_copy(invocation.arguments).empty()) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kTogglePlanMode;
    operation.steps.push_back(
        "Toggle Plan mode for non-mutating planning phase.");
    operation.steps.push_back(
        "Runtime should inject plan mode system prompt, restrict mutating "
        "tools, and display plan mode indicator in the footer.");
    return operation;
  }

  if (invocation.name == "goal") {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kGoalOperation;
    const std::string subcommand = trim_copy(invocation.arguments);
    operation.value = subcommand;
    if (subcommand.empty()) {
      operation.steps.push_back("Show current goal status or usage help.");
    } else if (subcommand == "clear") {
      operation.steps.push_back("Clear the current session goal.");
    } else if (subcommand == "edit") {
      operation.steps.push_back("Open goal edit prompt.");
    } else if (subcommand == "pause") {
      operation.steps.push_back("Pause the active goal.");
    } else if (subcommand == "resume") {
      operation.steps.push_back("Resume a paused goal.");
    } else if (subcommand == "status") {
      operation.steps.push_back("Show detailed goal status.");
    } else {
      operation.custom_instructions = subcommand;
      operation.steps.push_back("Set a new goal with the provided objective.");
    }
    operation.steps.push_back(
        "Runtime should manage the session goal through the GoalStore and "
        "display appropriate feedback.");
    return operation;
  }

  return std::nullopt;
}

SlashCommandExecutionKind execution_kind_for_source(SlashCommandSource source) {
  switch (source) {
    case SlashCommandSource::kBuiltin:
      return SlashCommandExecutionKind::kBuiltin;
    case SlashCommandSource::kExtension:
      return SlashCommandExecutionKind::kExtension;
    case SlashCommandSource::kPrompt:
      return SlashCommandExecutionKind::kPromptExpansion;
    case SlashCommandSource::kSkill:
      return SlashCommandExecutionKind::kSkillExpansion;
  }

  return SlashCommandExecutionKind::kBuiltin;
}

std::string target_name_for_command(const SlashCommand& command) {
  return command.target_name.empty() ? command.name : command.target_name;
}

}  // namespace

std::string to_string(SlashCommandSource source) {
  switch (source) {
    case SlashCommandSource::kBuiltin:
      return "builtin";
    case SlashCommandSource::kExtension:
      return "extension";
    case SlashCommandSource::kPrompt:
      return "prompt";
    case SlashCommandSource::kSkill:
      return "skill";
  }

  return "builtin";
}

std::string to_string(SlashCommandExecutionKind kind) {
  switch (kind) {
    case SlashCommandExecutionKind::kBuiltin:
      return "builtin";
    case SlashCommandExecutionKind::kExtension:
      return "extension";
    case SlashCommandExecutionKind::kPromptExpansion:
      return "prompt_expansion";
    case SlashCommandExecutionKind::kSkillExpansion:
      return "skill_expansion";
  }

  return "builtin";
}

std::string to_string(SlashCommandRuntimeOperationKind kind) {
  switch (kind) {
    case SlashCommandRuntimeOperationKind::kCompactSessionContext:
      return "compact_session_context";
    case SlashCommandRuntimeOperationKind::kNavigateSessionTree:
      return "navigate_session_tree";
    case SlashCommandRuntimeOperationKind::kExportSessionHtml:
      return "export_session_html";
    case SlashCommandRuntimeOperationKind::kExportSessionJsonl:
      return "export_session_jsonl";
    case SlashCommandRuntimeOperationKind::kShareSessionGist:
      return "share_session_gist";
    case SlashCommandRuntimeOperationKind::kOpenSettings:
      return "open_settings";
    case SlashCommandRuntimeOperationKind::kOpenPackageResourceConfig:
      return "open_package_resource_config";
    case SlashCommandRuntimeOperationKind::kSelectModel:
      return "select_model";
    case SlashCommandRuntimeOperationKind::kSelectScopedModels:
      return "select_scoped_models";
    case SlashCommandRuntimeOperationKind::kImportSessionJsonl:
      return "import_session_jsonl";
    case SlashCommandRuntimeOperationKind::kCopyLastAssistantText:
      return "copy_last_assistant_text";
    case SlashCommandRuntimeOperationKind::kSetSessionName:
      return "set_session_name";
    case SlashCommandRuntimeOperationKind::kShowSessionInfo:
      return "show_session_info";
    case SlashCommandRuntimeOperationKind::kShowContextInfo:
      return "show_context_info";
    case SlashCommandRuntimeOperationKind::kShowChangelog:
      return "show_changelog";
    case SlashCommandRuntimeOperationKind::kShowHotkeys:
      return "show_hotkeys";
    case SlashCommandRuntimeOperationKind::kForkSessionFromUserMessage:
      return "fork_session_from_user_message";
    case SlashCommandRuntimeOperationKind::kCloneSession:
      return "clone_session";
    case SlashCommandRuntimeOperationKind::kOpenLoginSelector:
      return "open_login_selector";
    case SlashCommandRuntimeOperationKind::kOpenLogoutSelector:
      return "open_logout_selector";
    case SlashCommandRuntimeOperationKind::kStartNewSession:
      return "start_new_session";
    case SlashCommandRuntimeOperationKind::kResumeSession:
      return "resume_session";
    case SlashCommandRuntimeOperationKind::kReloadRuntimeResources:
      return "reload_runtime_resources";
    case SlashCommandRuntimeOperationKind::kShowRuntimeStatus:
      return "show_runtime_status";
    case SlashCommandRuntimeOperationKind::kOpenToolsSelector:
      return "open_tools_selector";
    case SlashCommandRuntimeOperationKind::kSetThinkingLevel:
      return "set_thinking_level";
    case SlashCommandRuntimeOperationKind::kOpenPackageResourceSelector:
      return "open_package_resource_selector";
    case SlashCommandRuntimeOperationKind::kExecuteExtensionCommand:
      return "execute_extension_command";
    case SlashCommandRuntimeOperationKind::kLlamaProviderOperation:
      return "llama_provider_operation";
    case SlashCommandRuntimeOperationKind::kOllamaProviderOperation:
      return "ollama_provider_operation";
    case SlashCommandRuntimeOperationKind::kSubagentOperation:
      return "subagent_operation";
    case SlashCommandRuntimeOperationKind::kIndexOperation:
      return "index_operation";
    case SlashCommandRuntimeOperationKind::kMemoryOperation:
      return "memory_operation";
    case SlashCommandRuntimeOperationKind::kMcpOperation:
      return "mcp_operation";
    case SlashCommandRuntimeOperationKind::kQuitApplication:
      return "quit_application";
    case SlashCommandRuntimeOperationKind::kTogglePlanMode:
      return "toggle_plan_mode";
    case SlashCommandRuntimeOperationKind::kGoalOperation:
      return "goal_operation";
  }

  return "compact_session_context";
}

const std::vector<SlashCommand>& builtin_slash_commands() {
  static const std::vector<SlashCommand> commands = {
      make_builtin("help", "Show slash command help"),
      make_builtin("settings", "Open settings menu"),
      make_builtin("model", "Select model", "[provider/model|search|refresh]"),
      make_builtin("llama", "Inspect and configure local llama.cpp provider",
                   "<status|models|select|load|unload|reload|policy|grammar|kv|gpu|embeddings|prompt|run>"),
      make_builtin("ollama", "Manage Ollama instances and models",
                   "<instances|add|configure|remove|status|models|running|info|pull|load|unload|delete>"),
      make_builtin("subagent", "Inspect and use isolated subagents",
                   "<list|help>"),
      make_builtin("index", "Inspect and use project index operations",
                   "<tree|call|status|rescan|code|fs|git|build|debug|serial|util>"),
      make_builtin("memory", "Inspect and use built-in governed memory",
                   "<dashboard|status|settings|scan-skills|dream|recall|remember|review|feedback|forget>"),
      make_builtin("mcp", "Inspect and manage MCP servers",
                   "<status|tools|resources|reconnect|setup>"),
      make_builtin("scoped-models", "Enable or disable models for cycling"),
      make_builtin("export",
                   "Export session (HTML default, or specify path: .html/.jsonl)"),
      make_builtin("import", "Import and resume a session"),
      make_builtin("share", "Share session"),
      make_builtin("copy", "Copy last agent message"),
      make_builtin("name", "Set session display name"),
      make_builtin("session", "Show session info and stats"),
      make_builtin("context", "Inspect full current prompt, session context, resources, diagnostics, memory, and usage", "[all|environment|prompt|messages|resources|diagnostics|memory|usage]"),
      make_builtin("changelog", "Show changelog entries"),
      make_builtin("hotkeys", "Show keyboard shortcuts"),
      make_builtin("fork", "Create a new fork from a previous user message"),
      make_builtin("clone", "Duplicate the current session"),
      make_builtin("tree", "Navigate session tree"),
      make_builtin("login", "Configure provider authentication"),
      make_builtin("logout", "Remove provider authentication"),
      make_builtin("new", "Start a new session"),
      make_builtin("compact", "Manually compact the session context"),
      make_builtin("resume", "Resume a different session"),
      make_builtin("status", "Show runtime status"),
      make_builtin("tools", "Configure enabled tools"),
      make_builtin("thinking", "Configure thinking level"),
      make_builtin("plan", "Toggle Plan mode (non-mutating planning phase)"),
      make_builtin("goal", "Set or manage session goal",
                   "[<objective>|clear|edit|pause|resume|status]"),
      make_builtin("extensions", "List loaded extensions and configure package resources"),
      make_builtin("skills", "Configure skill package resources"),
      make_builtin("prompts", "Configure prompt package resources"),
      make_builtin("themes", "Configure theme package resources"),
      make_builtin("reload", "Reload keybindings, extensions, skills, prompts, and themes"),
      make_builtin("quit", "Quit qt"),
  };
  return commands;
}

SlashCommandRegistry build_slash_command_registry(
    const qt::harness::PromptTemplateResources& prompt_templates,
    const qt::harness::SkillResources& skills,
    const qt::extensions::ExtensionLoadResult& extensions,
    SlashCommandRegistryOptions options) {
  SlashCommandRegistry registry;
  std::set<std::string> reserved_names;

  std::map<std::string, std::size_t> extension_name_counts;
  for (const auto& extension : extensions.loaded_extensions) {
    for (const auto& contribution : extension.manifest.commands) {
      if (!contribution.name.empty()) {
        ++extension_name_counts[contribution.name];
      }
    }
  }

  std::map<std::string, std::size_t> extension_name_seen;
  for (const auto& extension : extensions.loaded_extensions) {
    for (const auto& contribution : extension.manifest.commands) {
      if (contribution.name.empty()) {
        continue;
      }

      std::string invocation_name = contribution.name;
      if (extension_name_counts[contribution.name] > 1) {
        invocation_name += ":" +
                           std::to_string(++extension_name_seen[contribution.name]);
      }
      if (has_name(reserved_names, invocation_name)) {
        registry.diagnostics.push_back(
            "extension command '/" + invocation_name +
            "' conflicts with an existing extension slash command and was skipped: " +
            extension.path);
        continue;
      }

      SlashCommand command;
      command.name = invocation_name;
      command.target_name = contribution.name;
      command.description =
          contribution.description.value_or(contribution.title.value_or(""));
      command.source = SlashCommandSource::kExtension;
      command.source_path = extension.path;
      registry.commands.push_back(std::move(command));
      reserved_names.insert(invocation_name);
    }
  }

  for (const auto& command : builtin_slash_commands()) {
    registry.commands.push_back(command);
    reserved_names.insert(command.name);
  }

  for (const auto& prompt_template : prompt_templates.loaded_prompt_templates) {
    if (prompt_template.name.empty()) {
      continue;
    }
    if (has_name(reserved_names, prompt_template.name)) {
      registry.diagnostics.push_back(
          "prompt command '/" + prompt_template.name +
          "' conflicts with an existing slash command and was skipped: " +
          prompt_template.path);
      continue;
    }

    SlashCommand command;
    command.name = prompt_template.name;
    command.description = prompt_template.description;
    command.source = SlashCommandSource::kPrompt;
    command.source_path = prompt_template.path;
    command.argument_hint = prompt_template.argument_hint;
    command.expansion_template = prompt_template.content;
    registry.commands.push_back(std::move(command));
    reserved_names.insert(prompt_template.name);
  }

  if (options.enable_skill_commands) {
    for (const auto& skill : skills.loaded_skills) {
      if (skill.name.empty()) {
        continue;
      }
      const std::string command_name = "skill:" + skill.name;
      if (has_name(reserved_names, command_name)) {
        registry.diagnostics.push_back(
            "skill command '/" + command_name +
            "' conflicts with an existing slash command and was skipped: " +
            skill.path);
        continue;
      }

      SlashCommand command;
      command.name = command_name;
      command.description =
          skill.description.empty() ? "Use skill " + skill.name
                                    : skill.description;
      command.source = SlashCommandSource::kSkill;
      command.source_path = skill.path;
      command.expansion_template = skill.content;
      registry.commands.push_back(std::move(command));
      reserved_names.insert(command_name);
    }
  }

  return registry;
}

std::vector<SlashCommand> filter_slash_commands_by_prefix(
    const SlashCommandRegistry& registry, std::string_view prefix) {
  std::vector<SlashCommand> matches;
  const std::string normalized = normalize_command_name(prefix);

  for (const auto& command : registry.commands) {
    if (starts_with(command.name, normalized)) {
      matches.push_back(command);
    }
  }
  return matches;
}

std::optional<SlashCommand> find_slash_command(
    const SlashCommandRegistry& registry, std::string_view name) {
  const std::string normalized = normalize_command_name(name);

  for (const auto& command : registry.commands) {
    if (command.name == normalized) {
      return command;
    }
  }
  return std::nullopt;
}

qt::foundation::Result<SlashCommandInvocation> parse_slash_command_invocation(
    std::string_view input) {
  std::string_view text = input;
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }

  if (text.empty()) {
    return qt::foundation::Result<SlashCommandInvocation>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "slash command input is empty");
  }
  if (text.front() != '/') {
    return qt::foundation::Result<SlashCommandInvocation>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "slash command input must start with '/'");
  }

  text.remove_prefix(1);
  std::size_t name_end = 0;
  while (name_end < text.size() && !is_space(text[name_end])) {
    ++name_end;
  }

  if (name_end == 0) {
    return qt::foundation::Result<SlashCommandInvocation>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "slash command name is empty");
  }

  SlashCommandInvocation invocation;
  invocation.name = std::string(text.substr(0, name_end));
  std::string_view arguments = text.substr(name_end);
  while (!arguments.empty() && is_space(arguments.front())) {
    arguments.remove_prefix(1);
  }
  invocation.arguments = std::string(arguments);
  return invocation;
}

qt::foundation::Result<SlashCommandExecution> execute_slash_command(
    const SlashCommandRegistry& registry, std::string_view input) {
  const auto invocation = parse_slash_command_invocation(input);
  if (!invocation) {
    return qt::foundation::Result<SlashCommandExecution>(invocation.error());
  }

  const auto command = find_slash_command(registry, invocation.value().name);
  if (!command) {
    return qt::foundation::Result<SlashCommandExecution>::failure(
        qt::foundation::ErrorCode::not_found,
        "unknown slash command '/" + invocation.value().name + "'");
  }

  SlashCommandExecution execution;
  execution.kind = execution_kind_for_source(command->source);
  execution.command = *command;
  if (command->source == SlashCommandSource::kPrompt) {
    execution.expanded_prompt =
        expand_prompt_template(command->expansion_template,
                               invocation.value().arguments);
  } else if (command->source == SlashCommandSource::kSkill) {
    execution.expanded_prompt =
        expand_skill_command(*command, invocation.value().arguments);
  } else if (command->source == SlashCommandSource::kBuiltin) {
    execution.runtime_operation =
        plan_builtin_runtime_operation(invocation.value());
    if (execution.runtime_operation) {
      execution.output = "Runtime operation planned: " +
                         to_string(execution.runtime_operation->kind);
      return execution;
    }
    const auto output = execute_builtin_command(registry, invocation.value());
    if (!output) {
      return qt::foundation::Result<SlashCommandExecution>(output.error());
    }
    execution.output = output.value();
  } else if (command->source == SlashCommandSource::kExtension) {
    SlashCommandRuntimeOperation operation;
    operation.kind = SlashCommandRuntimeOperationKind::kExecuteExtensionCommand;
    operation.value = target_name_for_command(*command);
    operation.target_path = command->source_path;
    const std::string& arguments = invocation.value().arguments;
    if (!arguments.empty()) {
      operation.custom_instructions = arguments;
      operation.steps.push_back(
          "Dispatch the extension slash command with its raw command arguments.");
    } else {
      operation.steps.push_back(
          "Dispatch the extension slash command without additional arguments.");
    }
    operation.steps.push_back(
        "Runtime should invoke the contributing extension command from the "
        "registered extension source and surface any produced UI or messages.");
    execution.runtime_operation = std::move(operation);
    execution.output = "Runtime operation planned: " +
                       to_string(execution.runtime_operation->kind);
    return execution;
  }
  return execution;
}

}  // namespace qt::app
