#include "qt/tui/input/input.hpp"
#include "qt/tui/input/stdin_buffer.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
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

void verify_fragmented_utf8_and_kitty_deduplication() {
  qt::tui::Input input;
  input.handle_input("\xc3");
  require(input.value().empty(), "fragmented UTF-8 was decoded prematurely");
  input.handle_input("\xa9");
  require(input.value() == u8"é", "fragmented UTF-8 became a Meta key");

  qt::tui::Input kitty;
  kitty.handle_input("\x1b[233u");
  kitty.handle_input("\xc3");
  kitty.handle_input("\xa9");
  require(kitty.value() == u8"é",
          "Kitty Unicode key and UTF-8 text were not deduplicated");

  qt::tui::Input mismatch;
  mismatch.handle_input("\x1b[233u");
  mismatch.handle_input("x");
  require(mismatch.value() == u8"éx",
          "Kitty deduplication discarded a mismatched printable character");
}

void verify_fragmented_bracketed_paste_is_linear_and_atomic() {
  qt::tui::StdinBuffer buffer;
  constexpr std::size_t payload_size = 256U * 1024U;
  const std::string sequence = "\x1b[200~" + std::string(payload_size, 'x') +
                               "\x1b[201~";
  for (const char byte : sequence) {
    buffer.push(byte);
  }
  const auto completed = buffer.drain();
  require(completed.size() == 1U &&
              completed.front().kind == qt::tui::InputSequenceKind::escape &&
              completed.front().bytes == sequence,
          "fragmented bracketed paste was not retained as one sequence");
}

double benchmark_cursor_moves(std::size_t value_size) {
  qt::tui::Input input;
  input.set_value(std::string(value_size, 'x'));
  input.handle_input("\x1b[D");
  input.handle_input("\x1b[C");
  const auto started = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < 1024U; ++index) {
    input.handle_input("\x1b[D");
    input.handle_input("\x1b[C");
  }
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - started)
      .count();
}

void verify_long_input_cursor_cache() {
  const double small_ms = benchmark_cursor_moves(8U * 1024U);
  const double large_ms = benchmark_cursor_moves(64U * 1024U);
  require(large_ms < std::max(750.0, small_ms * 4.0),
          "single-line cursor movement still rescans the complete value");
}

}  // namespace

int main() {
  try {
    verify_standalone_escape_is_delivered_immediately();
    verify_complete_sequences_and_text_are_unchanged();
    verify_fragmented_utf8_and_kitty_deduplication();
    verify_fragmented_bracketed_paste_is_linear_and_atomic();
    verify_long_input_cursor_cache();
    std::cout << "input escape probe passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "input escape probe failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
