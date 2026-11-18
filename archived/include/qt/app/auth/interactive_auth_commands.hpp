#pragma once

#include "qt/app/interactive/interactive_shell.hpp"
#include "qt/app/interactive/interactive_terminal_session.hpp"
#include "qt/app/runtime/runtime_services.hpp"
#include "qt/foundation/error.hpp"

#include <functional>
#include <optional>
#include <string>

namespace qt::app {

struct InteractiveAuthCommandContext {
  RuntimeServices& services;
  std::function<foundation::Status(InteractiveTerminalSession&, std::string)>
      reload_runtime;
  std::function<void(
      InteractiveTerminalSession&, std::string, std::string, std::string,
      const std::function<void(const std::string&)>&)>
      prompt_for_secret;
  std::function<void(std::function<void(InteractiveTerminalSession&)>)> post_ui;
};

foundation::Result<std::optional<InteractiveSlashCommandResponse>>
try_handle_interactive_auth_command(
    InteractiveTerminalSession& ui, const InteractiveShellEvent& event,
    InteractiveAuthCommandContext& context);

}  // namespace qt::app
