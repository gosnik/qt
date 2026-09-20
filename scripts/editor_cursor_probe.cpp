#include "qt/tui/input/editor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qt::tui {
// InputDecoder's resize-event path is linked into this focused probe, but the
// probe never owns a terminal or emits a resize sequence.
void set_terminal_cell_dimensions(CellDimensions) noexcept {}
}  // namespace qt::tui

namespace {

using qt::tui::Editor;
using qt::tui::InputEvent;
using qt::tui::InputEventKind;
using qt::tui::RenderConstraints;

constexpr std::string_view left_arrow = "\x1b[D";
constexpr std::string_view right_arrow = "\x1b[C";

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void move(Editor& editor, std::string_view bytes) {
  InputEvent event;
  event.kind = InputEventKind::key;
  event.bytes = std::string(bytes);
  editor.handle_event(event);
}

void verify_unicode_cursor_contract() {
  const std::string unicode = u8"Ae\u0301👨‍👩‍👧‍👦🇦🇺界Z";
  const std::vector<std::size_t> boundaries{0U, 1U, 4U, 29U, 37U, 40U, 41U};

  Editor editor;
  editor.set_contents({unicode}, 0U, unicode.size());
  for (auto boundary = boundaries.rbegin() + 1; boundary != boundaries.rend();
       ++boundary) {
    move(editor, left_arrow);
    require(editor.cursor_column() == *boundary,
            "left movement split a Unicode grapheme");
  }
  for (auto boundary = boundaries.begin() + 1; boundary != boundaries.end();
       ++boundary) {
    move(editor, right_arrow);
    require(editor.cursor_column() == *boundary,
            "right movement split a Unicode grapheme");
  }

  editor.set_contents({"ab", unicode}, 1U, 0U);
  move(editor, left_arrow);
  require(editor.cursor_row() == 0U && editor.cursor_column() == 2U,
          "left movement did not cross a logical line boundary");
  move(editor, right_arrow);
  require(editor.cursor_row() == 1U && editor.cursor_column() == 0U,
          "right movement did not cross a logical line boundary");

  editor.set_focused(true);
  const auto rendered = editor.render(RenderConstraints{8U, 8U});
  require(static_cast<bool>(rendered), "narrow editor render failed");
  std::size_t marker_count = 0U;
  for (const auto& line : rendered.value().lines) {
    std::size_t offset = 0U;
    while ((offset = line.find(qt::tui::cursor_marker, offset)) !=
           std::string::npos) {
      ++marker_count;
      offset += qt::tui::cursor_marker.size();
    }
  }
  require(marker_count == 1U, "render did not emit exactly one cursor marker");
}

double benchmark_cursor_render(std::size_t prompt_size,
                               std::size_t repetitions) {
  Editor editor;
  editor.set_focused(true);
  editor.set_contents({std::string(prompt_size, 'x')}, 0U, prompt_size);
  const RenderConstraints constraints{80U, 8U};
  require(static_cast<bool>(editor.render(constraints)),
          "editor warm-up render failed");

  const auto started = std::chrono::steady_clock::now();
  for (std::size_t iteration = 0; iteration < repetitions; ++iteration) {
    move(editor, left_arrow);
    require(static_cast<bool>(editor.render(constraints)),
            "left cursor render failed");
    move(editor, right_arrow);
    require(static_cast<bool>(editor.render(constraints)),
            "right cursor render failed");
  }
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - started)
      .count();
}

}  // namespace

int main() {
  try {
    verify_unicode_cursor_contract();
    constexpr std::size_t repetitions = 32U;
    const double small_ms = benchmark_cursor_render(8U * 1024U, repetitions);
    const double large_ms = benchmark_cursor_render(32U * 1024U, repetitions);
    const double scaling = large_ms / std::max(0.001, small_ms);
    require(large_ms < 3000.0,
            "32 KiB cursor movement exceeded the responsiveness guard");
    require(scaling < 6.0,
            "cursor movement still scales with total prompt layout size");
    std::cout << "editor cursor probe: ok; 8KiB=" << small_ms
              << "ms 32KiB=" << large_ms << "ms scaling=" << scaling << "x\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "editor cursor probe: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
