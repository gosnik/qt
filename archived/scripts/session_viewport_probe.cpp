#include "qt/app/interactive/interactive_shell.hpp"
#include "qt/app/interactive/interactive_terminal_session.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool contains(const qt::tui::RenderBuffer& buffer, std::string_view text) {
  for (const auto& line : buffer.lines) {
    if (line.find(text) != std::string::npos) return true;
  }
  return false;
}

void verify_viewport_order_and_resize() {
  qt::app::InteractiveShellRuntime shell;
  qt::tui::BufferTerminalSink sink;
  qt::tui::Terminal terminal(sink);
  qt::app::InteractiveTerminalSessionOptions options;
  options.constraints = {32U, 6U};
  options.clear_on_start = false;
  options.hide_cursor = false;
  qt::app::InteractiveTerminalSession session(shell, terminal, std::move(options));
  for (std::size_t index = 0; index < 20U; ++index) {
    session.append_transcript_line("entry-" + std::to_string(index));
  }
  const auto latest = session.base_render_buffer({32U, 6U});
  require(latest && contains(latest.value(), "entry-19"),
          "viewport omitted the newest transcript line");
  require(latest && !contains(latest.value(), "entry-0"),
          "viewport retained off-screen history");
  const auto zero_height = session.base_render_buffer({32U, 0U});
  require(zero_height && zero_height.value().lines.empty(),
          "zero-height rendering emitted visible rows");
  session.set_extension_header(std::vector<std::string>{"extension header"});
  const auto scroll = session.handle_input("\x1b[<64;1;1M");
  require(static_cast<bool>(scroll), "manual scroll input failed");
  const auto scrolled = session.base_render_buffer({32U, 6U});
  require(scrolled && contains(scrolled.value(), "entry-16") &&
              !contains(scrolled.value(), "entry-19") &&
              contains(scrolled.value(), "extension header"),
          "manual viewport offset or extension header was applied incorrectly");
  require(static_cast<bool>(session.handle_input("\x1b[<65;1;1M")),
          "scroll reset input failed");
  session.set_extension_header(std::nullopt);

  require(static_cast<bool>(session.apply_terminal_resize({16U, 4U})), "resize render failed");
  const auto resized = session.base_render_buffer({16U, 4U});
  require(resized && resized.value().height() == 4U,
          "resize did not constrain the viewport");
  require(resized && contains(resized.value(), "entry-19"),
          "resize lost transcript ordering");
}

}  // namespace

int main() {
  try {
    verify_viewport_order_and_resize();
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
