#include "qt/app/interactive/interactive_shell.hpp"
#include "qt/app/interactive/interactive_terminal_session.hpp"
#include "qt/foundation/error.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void verify_extension_terminal_input_rewrite_lifetime() {
  qt::app::InteractiveShellRuntime shell;
  qt::tui::BufferTerminalSink sink;
  qt::tui::Terminal terminal(sink);
  qt::app::InteractiveTerminalSessionOptions options;
  options.clear_on_start = false;
  options.hide_cursor = false;
  qt::app::InteractiveTerminalSession session(shell, terminal, std::move(options));
  const std::string rewritten(64U * 1024U, 'r');

  session.set_extension_terminal_input_handler(
      [rewritten](std::string_view bytes)
          -> qt::foundation::Result<std::optional<std::string>> {
        if (bytes == "rewrite") {
          return std::optional<std::string>{rewritten};
        }
        if (bytes == "consume") {
          return std::optional<std::string>{};
        }
        if (bytes == "error") {
          return qt::foundation::Result<std::optional<std::string>>::failure(
              qt::foundation::ErrorCode::invalid_argument, "extension rejected input");
        }
        return std::optional<std::string>{std::string(bytes)};
      });

  const auto rewritten_result = session.handle_input("rewrite");
  require(rewritten_result.ok(), "long extension rewrite failed");
  require(session.editor().text() == rewritten,
          "long extension rewrite was not retained through input decoding");

  const auto consumed_result = session.handle_input("consume");
  require(consumed_result.ok(), "consumed extension input failed");
  require(session.editor().text() == rewritten,
          "consumed extension input reached the editor");

  const auto error_result = session.handle_input("error");
  require(!error_result.ok(), "extension input error was swallowed");
  require(error_result.error().message == "extension rejected input",
          "extension input error changed during propagation");
}

}  // namespace

int main() {
  try {
    verify_extension_terminal_input_rewrite_lifetime();
    std::cout << "interactive terminal input probe passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "interactive terminal input probe failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
