#include "qt/app/interactive/interactive_shell.hpp"
#include "qt/app/slash/slash_commands.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace qt::app {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

std::string trim_ascii(std::string_view text) {
  auto first = text.begin();
  auto last = text.end();

  while (first != last &&
         std::isspace(static_cast<unsigned char>(*first)) != 0) {
    ++first;
  }
  while (first != last) {
    const auto before_last = last - 1;
    if (std::isspace(static_cast<unsigned char>(*before_last)) == 0) {
      break;
    }
    last = before_last;
  }

  return std::string(first, last);
}

bool is_valid_slash_name(std::string_view name) {
  if (name.empty()) {
    return false;
  }

  return std::all_of(name.begin(), name.end(), [](char value) {
    const auto ch = static_cast<unsigned char>(value);
    return std::isalnum(ch) != 0 || value == '-' || value == '_' ||
           value == '.' || value == ':';
  });
}

InteractiveShellEvent make_event(InteractiveShellEventKind kind, std::string text = {},
                                 std::string command = {},
                                 std::string arguments = {},
                                 bool exclude_from_context = false) {
  InteractiveShellEvent event;
  event.kind = kind;
  event.text = std::move(text);
  event.command = std::move(command);
  event.arguments = std::move(arguments);
  event.exclude_from_context = exclude_from_context;
  return event;
}

const SlashCommandRegistry& deterministic_slash_registry() {
  static const SlashCommandRegistry registry =
      build_slash_command_registry(qt::harness::PromptTemplateResources{},
                                   qt::harness::SkillResources{},
                                   qt::extensions::ExtensionLoadResult{});
  return registry;
}

bool is_supported_deterministic_runtime_operation(
    SlashCommandRuntimeOperationKind kind) {
  switch (kind) {
    case SlashCommandRuntimeOperationKind::kCompactSessionContext:
    case SlashCommandRuntimeOperationKind::kNavigateSessionTree:
    case SlashCommandRuntimeOperationKind::kExportSessionHtml:
    case SlashCommandRuntimeOperationKind::kExportSessionJsonl:
    case SlashCommandRuntimeOperationKind::kShareSessionGist:
    case SlashCommandRuntimeOperationKind::kOpenSettings:
    case SlashCommandRuntimeOperationKind::kOpenPackageResourceConfig:
    case SlashCommandRuntimeOperationKind::kSelectModel:
    case SlashCommandRuntimeOperationKind::kSelectScopedModels:
    case SlashCommandRuntimeOperationKind::kImportSessionJsonl:
    case SlashCommandRuntimeOperationKind::kCopyLastAssistantText:
    case SlashCommandRuntimeOperationKind::kSetSessionName:
    case SlashCommandRuntimeOperationKind::kShowSessionInfo:
    case SlashCommandRuntimeOperationKind::kShowContextInfo:
    case SlashCommandRuntimeOperationKind::kShowChangelog:
    case SlashCommandRuntimeOperationKind::kShowHotkeys:
    case SlashCommandRuntimeOperationKind::kOpenLoginSelector:
    case SlashCommandRuntimeOperationKind::kOpenLogoutSelector:
    case SlashCommandRuntimeOperationKind::kStartNewSession:
    case SlashCommandRuntimeOperationKind::kReloadRuntimeResources:
    case SlashCommandRuntimeOperationKind::kShowRuntimeStatus:
    case SlashCommandRuntimeOperationKind::kOpenToolsSelector:
    case SlashCommandRuntimeOperationKind::kSetThinkingLevel:
    case SlashCommandRuntimeOperationKind::kResumeSession:
    case SlashCommandRuntimeOperationKind::kForkSessionFromUserMessage:
    case SlashCommandRuntimeOperationKind::kCloneSession:
    case SlashCommandRuntimeOperationKind::kExecuteExtensionCommand:
    case SlashCommandRuntimeOperationKind::kLlamaProviderOperation:
    case SlashCommandRuntimeOperationKind::kOllamaProviderOperation:
    case SlashCommandRuntimeOperationKind::kSubagentOperation:
    case SlashCommandRuntimeOperationKind::kIndexOperation:
    case SlashCommandRuntimeOperationKind::kMemoryOperation:
    case SlashCommandRuntimeOperationKind::kQuitApplication:
    case SlashCommandRuntimeOperationKind::kTogglePlanMode:
    case SlashCommandRuntimeOperationKind::kGoalOperation:
      return true;
    default:
      return false;
  }
}

bool is_runtime_resource_selector(std::string_view name) {
  return name == "extensions" || name == "skills" || name == "prompts" ||
         name == "themes";
}

std::string format_runtime_operation(
    const SlashCommandRuntimeOperation& operation) {
  std::string output =
      "planned runtime action: " + to_string(operation.kind);

  if (operation.value && !operation.value->empty()) {
    output += "\nvalue: " + *operation.value;
  }
  if (operation.target_path && !operation.target_path->empty()) {
    output += "\npath: " + *operation.target_path;
  }
  if (operation.custom_instructions && !operation.custom_instructions->empty()) {
    output += "\ninstructions: " + *operation.custom_instructions;
  }
  for (const auto& step : operation.steps) {
    output += "\n- " + step;
  }

  return output;
}

Result<InteractiveDispatchResult> dispatch_deterministic_builtin_slash(
    const InteractiveCommand& command) {
  if (is_runtime_resource_selector(command.name)) {
    return InteractiveDispatchResult{true, ""};
  }

  std::string invocation = "/";
  invocation += command.name;
  if (!command.arguments.empty()) {
    invocation += " ";
    invocation += command.arguments;
  }

  const auto execution =
      execute_slash_command(deterministic_slash_registry(), invocation);
  if (!execution) {
    if (execution.error().code == ErrorCode::not_found) {
      return InteractiveDispatchResult{true, ""};
    }
    return Result<InteractiveDispatchResult>(execution.error());
  }

  if (!execution.value().runtime_operation.has_value()) {
    return InteractiveDispatchResult{true, ""};
  }

  const auto& operation = *execution.value().runtime_operation;
  if (!is_supported_deterministic_runtime_operation(operation.kind)) {
    return Result<InteractiveDispatchResult>::failure(
        ErrorCode::not_found, "unknown slash command: /" + command.name);
  }

  return InteractiveDispatchResult{true, format_runtime_operation(operation)};
}

}  // namespace

const std::string& PromptInputState::text() const noexcept { return text_; }

std::size_t PromptInputState::cursor() const noexcept { return cursor_; }

bool PromptInputState::empty() const noexcept { return text_.empty(); }

void PromptInputState::set_text(std::string text) {
  text_ = std::move(text);
  cursor_ = text_.size();
}

void PromptInputState::insert(std::string_view text) {
  if (text.empty()) {
    return;
  }

  text_.insert(cursor_, text);
  cursor_ += text.size();
}

bool PromptInputState::erase_backward() {
  if (cursor_ == 0) {
    return false;
  }

  text_.erase(cursor_ - 1, 1);
  --cursor_;
  return true;
}

bool PromptInputState::move_left() {
  if (cursor_ == 0) {
    return false;
  }

  --cursor_;
  return true;
}

bool PromptInputState::move_right() {
  if (cursor_ >= text_.size()) {
    return false;
  }

  ++cursor_;
  return true;
}

std::string PromptInputState::submit() {
  std::string submitted = std::move(text_);
  clear();
  return submitted;
}

void PromptInputState::clear() {
  text_.clear();
  cursor_ = 0;
}

Result<InteractiveCommand> parse_interactive_command(std::string_view line) {
  const std::string trimmed = trim_ascii(line);
  std::string_view leading_trimmed = line;
  while (!leading_trimmed.empty() &&
         (leading_trimmed.front() == ' ' || leading_trimmed.front() == '\t' ||
          leading_trimmed.front() == '\n' || leading_trimmed.front() == '\r')) {
    leading_trimmed.remove_prefix(1);
  }
  InteractiveCommand command;

  if (trimmed.empty()) {
    command.kind = InteractiveCommandKind::kEmpty;
    return command;
  }

  if (trimmed.front() == '!') {
    const bool exclude_from_context =
        trimmed.size() > 1U && trimmed[1] == '!';
    const std::string command_text =
        trim_ascii(std::string_view(trimmed).substr(exclude_from_context ? 2U : 1U));
    if (!command_text.empty()) {
      command.kind = InteractiveCommandKind::kShellCommand;
      command.name = "bash";
      command.arguments = command_text;
      command.text = command_text;
      command.exclude_from_context = exclude_from_context;
      return command;
    }
  }

  if (trimmed.front() != '/') {
    command.kind = InteractiveCommandKind::kPrompt;
    command.text = trimmed;
    return command;
  }

  const std::string_view slash_line(leading_trimmed.data() + 1,
                                    leading_trimmed.size() - 1);
  const auto split = slash_line.find_first_of(" \t\n\r");
  const std::string name =
      split == std::string_view::npos
          ? std::string(slash_line)
          : std::string(slash_line.substr(0, split));

  if (!is_valid_slash_name(name)) {
    return Result<InteractiveCommand>::failure(
        ErrorCode::invalid_argument,
        "slash command names may contain letters, digits, '-', '_', '.', and ':'");
  }

  command.name = name;
  if (split != std::string_view::npos) {
    std::string_view arguments = slash_line.substr(split + 1);
    while (!arguments.empty() &&
           (arguments.front() == ' ' || arguments.front() == '\t' ||
            arguments.front() == '\n' || arguments.front() == '\r')) {
      arguments.remove_prefix(1);
    }
    command.arguments = std::string(arguments);
  }

  if (name == "quit" || name == "exit") {
    command.kind = InteractiveCommandKind::kQuit;
  } else if (name == "help") {
    command.kind = InteractiveCommandKind::kHelp;
  } else {
    command.kind = InteractiveCommandKind::kSlashCommand;
  }

  return command;
}

InteractiveShellRuntime::InteractiveShellRuntime(std::string prompt)
    : prompt_(std::move(prompt)) {}

const PromptInputState& InteractiveShellRuntime::input() const noexcept {
  return input_;
}

const std::vector<InteractiveShellEvent>& InteractiveShellRuntime::events()
    const noexcept {
  return events_;
}

const std::vector<std::string>& InteractiveShellRuntime::history() const noexcept {
  return history_;
}

bool InteractiveShellRuntime::should_quit() const noexcept { return should_quit_; }

void InteractiveShellRuntime::set_prompt_handler(PromptHandler handler) {
  prompt_handler_ = std::move(handler);
}

void InteractiveShellRuntime::set_shell_command_handler(
    ShellCommandHandler handler) {
  shell_command_handler_ = std::move(handler);
}

void InteractiveShellRuntime::register_slash_command(std::string name,
                                                     SlashCommandHandler handler) {
  slash_handlers_[std::move(name)] = std::move(handler);
}

void InteractiveShellRuntime::reset() {
  input_.clear();
  events_.clear();
  history_.clear();
  history_cursor_.reset();
  history_draft_.clear();
  should_quit_ = false;
}

Result<std::vector<InteractiveShellEvent>> InteractiveShellRuntime::submit_line(
    std::string_view line) {
  std::vector<InteractiveShellEvent> emitted;
  if (should_quit_) {
    return Result<std::vector<InteractiveShellEvent>>::failure(
        ErrorCode::invalid_argument, "interactive shell has already exited");
  }

  record(make_event(InteractiveShellEventKind::kPromptReady, prompt_), emitted);
  input_.set_text(std::string(line));
  const std::string submitted = input_.submit();
  record(make_event(InteractiveShellEventKind::kInputSubmitted, submitted), emitted);
  remember_history(submitted);

  const auto parsed = parse_interactive_command(submitted);
  if (!parsed) {
    record(make_event(InteractiveShellEventKind::kError, parsed.error().message),
           emitted);
    return emitted;
  }

  const auto& command = parsed.value();
  switch (command.kind) {
    case InteractiveCommandKind::kEmpty:
      return emitted;

    case InteractiveCommandKind::kPrompt: {
      const auto result = dispatch_prompt(command.text);
      record(make_event(result.ok ? InteractiveShellEventKind::kPromptDispatched
                                  : InteractiveShellEventKind::kError,
                        result.output),
             emitted);
      return emitted;
    }

    case InteractiveCommandKind::kShellCommand: {
      const auto result =
          dispatch_shell_command(command.text, command.exclude_from_context);
      record(make_event(result.ok ? InteractiveShellEventKind::kShellCommandDispatched
                                  : InteractiveShellEventKind::kError,
                        result.output, command.name, command.arguments,
                        command.exclude_from_context),
             emitted);
      return emitted;
    }

    case InteractiveCommandKind::kSlashCommand: {
      const auto result = dispatch_slash(command);
      if (!result) {
        record(make_event(InteractiveShellEventKind::kError, result.error().message,
                          command.name, command.arguments),
               emitted);
        return emitted;
      }
      record(make_event(result.value().ok
                            ? InteractiveShellEventKind::kSlashCommandDispatched
                            : InteractiveShellEventKind::kError,
                        result.value().output, command.name, command.arguments),
             emitted);
      return emitted;
    }

    case InteractiveCommandKind::kHelp:
      record(make_event(InteractiveShellEventKind::kHelpDisplayed,
                        "Available commands: /help, /quit", command.name,
                        command.arguments),
             emitted);
      return emitted;

    case InteractiveCommandKind::kQuit:
      should_quit_ = true;
      record(make_event(InteractiveShellEventKind::kQuitRequested, {}, command.name,
                        command.arguments),
             emitted);
      return emitted;
  }

  return emitted;
}

Result<std::vector<InteractiveShellEvent>> InteractiveShellRuntime::run_script(
    const std::vector<std::string>& lines) {
  std::vector<InteractiveShellEvent> emitted;
  for (const auto& line : lines) {
    if (should_quit_) {
      break;
    }

    auto step = submit_line(line);
    if (!step) {
      return step;
    }
    emitted.insert(emitted.end(), step.value().begin(), step.value().end());
  }
  return emitted;
}

std::optional<std::string> InteractiveShellRuntime::recall_previous_history(
    std::string_view draft) {
  if (history_.empty()) {
    return std::nullopt;
  }

  if (!history_cursor_.has_value()) {
    history_cursor_ = history_.size();
    history_draft_ = std::string(draft);
  }

  if (*history_cursor_ == 0) {
    return history_.front();
  }

  --(*history_cursor_);
  return history_[*history_cursor_];
}

std::optional<std::string> InteractiveShellRuntime::recall_next_history() {
  if (!history_cursor_.has_value()) {
    return std::nullopt;
  }

  if (*history_cursor_ + 1 >= history_.size()) {
    history_cursor_.reset();
    std::string draft = std::move(history_draft_);
    history_draft_.clear();
    return draft;
  }

  ++(*history_cursor_);
  return history_[*history_cursor_];
}

void InteractiveShellRuntime::remember_history(std::string_view line) {
  const std::string trimmed = trim_ascii(line);
  history_cursor_.reset();
  history_draft_.clear();
  if (trimmed.empty()) {
    return;
  }
  if (!history_.empty() && history_.back() == trimmed) {
    return;
  }
  history_.push_back(trimmed);
}

void InteractiveShellRuntime::record(InteractiveShellEvent event,
                                     std::vector<InteractiveShellEvent>& emitted) {
  events_.push_back(event);
  emitted.push_back(std::move(event));
}

InteractiveDispatchResult InteractiveShellRuntime::dispatch_prompt(
    std::string_view text) const {
  if (prompt_handler_) {
    return prompt_handler_(text);
  }
  return InteractiveDispatchResult{true, std::string(text)};
}

InteractiveDispatchResult InteractiveShellRuntime::dispatch_shell_command(
    std::string_view command, bool exclude_from_context) const {
  if (shell_command_handler_) {
    return shell_command_handler_(command, exclude_from_context);
  }

  std::string output = "planned shell command: ";
  output += command;
  output += "\nexcludeFromContext: ";
  output += exclude_from_context ? "true" : "false";
  return InteractiveDispatchResult{true, std::move(output)};
}

Result<InteractiveDispatchResult> InteractiveShellRuntime::dispatch_slash(
    const InteractiveCommand& command) const {
  const auto handler = slash_handlers_.find(command.name);
  if (handler != slash_handlers_.end()) {
    return handler->second(command.arguments);
  }

  return dispatch_deterministic_builtin_slash(command);
}

const char* to_string(InteractiveCommandKind kind) noexcept {
  switch (kind) {
    case InteractiveCommandKind::kEmpty:
      return "empty";
    case InteractiveCommandKind::kPrompt:
      return "prompt";
    case InteractiveCommandKind::kShellCommand:
      return "shell_command";
    case InteractiveCommandKind::kSlashCommand:
      return "slash_command";
    case InteractiveCommandKind::kHelp:
      return "help";
    case InteractiveCommandKind::kQuit:
      return "quit";
  }
  return "unknown";
}

const char* to_string(InteractiveShellEventKind kind) noexcept {
  switch (kind) {
    case InteractiveShellEventKind::kPromptReady:
      return "prompt_ready";
    case InteractiveShellEventKind::kInputSubmitted:
      return "input_submitted";
    case InteractiveShellEventKind::kPromptDispatched:
      return "prompt_dispatched";
    case InteractiveShellEventKind::kShellCommandDispatched:
      return "shell_command_dispatched";
    case InteractiveShellEventKind::kSlashCommandDispatched:
      return "slash_command_dispatched";
    case InteractiveShellEventKind::kHelpDisplayed:
      return "help_displayed";
    case InteractiveShellEventKind::kQuitRequested:
      return "quit_requested";
    case InteractiveShellEventKind::kError:
      return "error";
  }
  return "unknown";
}

}  // namespace qt::app
