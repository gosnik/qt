#include "qt/tui/input/input.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace qt::tui {
// InputDecoder links terminal report handling, which this focused probe does
// not exercise.
void set_terminal_cell_dimensions(CellDimensions) noexcept {}
}  // namespace qt::tui

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void verify_standalone_escape_is_delivered_immediately() {
  qt::tui::Input input;
  bool cancelled = false;
  input.set_escape_callback([&cancelled]() { cancelled = true; });

  input.handle_input("\x1b");

  require(cancelled, "standalone Escape remained pending in nested input");
}

void verify_complete_sequences_and_text_are_unchanged() {
  qt::tui::Input input;
  input.handle_input("ab");
  input.handle_input("\x1b[D");
  input.handle_input("x");
  require(input.value() == "axb", "complete cursor sequence handling changed");
}

}  // namespace

int main() {
  try {
    verify_standalone_escape_is_delivered_immediately();
    verify_complete_sequences_and_text_are_unchanged();
    std::cout << "input escape probe passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "input escape probe failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
