#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qt/foundation/error.hpp"

namespace qt::app {

class PromptInputState {
 public:
  [[nodiscard]] const std::string& text() const noexcept;
  [[nodiscard]] std::size_t cursor() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  void set_text(std::string text);
  void insert(std::string_view text);
  bool erase_backward();
  bool move_left();
  bool move_right();
  std::string submit();
  void clear();

 private:
  std::string text_;
  std::size_t cursor_ = 0;
};

enum class InteractiveCommandKind {
  kEmpty,
  kPrompt,
  kShellCommand,
  kSlashCommand,
  kHelp,
  kQuit,
};

struct InteractiveCommand {
  InteractiveCommandKind kind = InteractiveCommandKind::kEmpty;
  std::string name;
  std::string arguments;
  std::string text;
  bool exclude_from_context = false;
};

foundation::Result<InteractiveCommand> parse_interactive_command(
    std::string_view line);

enum class InteractiveShellEventKind {
  kPromptReady,
  kInputSubmitted,
  kPromptDispatched,
  kShellCommandDispatched,
  kSlashCommandDispatched,
  kHelpDisplayed,
  kQuitRequested,
  kError,
};

struct InteractiveShellEvent {
  InteractiveShellEventKind kind = InteractiveShellEventKind::kPromptReady;
  std::string text;
  std::string command;
  std::string arguments;
  bool exclude_from_context = false;
};

struct InteractiveDispatchResult {
  bool ok = true;
  std::string output;
};

class InteractiveShellRuntime {
 public:
  using PromptHandler = std::function<InteractiveDispatchResult(std::string_view)>;
  using ShellCommandHandler =
      std::function<InteractiveDispatchResult(std::string_view, bool)>;
  using SlashCommandHandler =
      std::function<InteractiveDispatchResult(std::string_view)>;

  explicit InteractiveShellRuntime(std::string prompt = "qt> ");

  [[nodiscard]] const PromptInputState& input() const noexcept;
  [[nodiscard]] const std::vector<InteractiveShellEvent>& events() const noexcept;
  [[nodiscard]] const std::vector<std::string>& history() const noexcept;
  [[nodiscard]] bool should_quit() const noexcept;

  void set_prompt_handler(PromptHandler handler);
  void set_shell_command_handler(ShellCommandHandler handler);
  void register_slash_command(std::string name, SlashCommandHandler handler);

  void reset();
  foundation::Result<std::vector<InteractiveShellEvent>> submit_line(
      std::string_view line);
  foundation::Result<std::vector<InteractiveShellEvent>> run_script(
      const std::vector<std::string>& lines);
  std::optional<std::string> recall_previous_history(std::string_view draft);
  std::optional<std::string> recall_next_history();

 private:
  void remember_history(std::string_view line);
  void record(InteractiveShellEvent event,
              std::vector<InteractiveShellEvent>& emitted);
  InteractiveDispatchResult dispatch_prompt(std::string_view text) const;
  InteractiveDispatchResult dispatch_shell_command(std::string_view command,
                                                   bool exclude_from_context) const;
  foundation::Result<InteractiveDispatchResult> dispatch_slash(
      const InteractiveCommand& command) const;

  std::string prompt_;
  PromptInputState input_;
  PromptHandler prompt_handler_;
  ShellCommandHandler shell_command_handler_;
  std::map<std::string, SlashCommandHandler> slash_handlers_;
  std::vector<InteractiveShellEvent> events_;
  std::vector<std::string> history_;
  std::optional<std::size_t> history_cursor_;
  std::string history_draft_;
  bool should_quit_ = false;
};

const char* to_string(InteractiveCommandKind kind) noexcept;
const char* to_string(InteractiveShellEventKind kind) noexcept;

}  // namespace qt::app
