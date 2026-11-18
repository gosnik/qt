#include "qt/tui/input/editor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

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

void insert(Editor& editor, std::string_view text) {
  InputEvent event;
  event.kind = InputEventKind::insert;
  event.text = std::string(text);
  editor.handle_event(event);
}

void undo(Editor& editor) {
  InputEvent event;
  event.kind = InputEventKind::key;
  event.bytes = "\x1f";
  editor.handle_event(event);
}

std::size_t peak_rss_kib() {
#if !defined(_WIN32)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    return static_cast<std::size_t>(usage.ru_maxrss);
  }
#endif
  return 0U;
}

void verify_typed_undo_and_shared_large_paste() {
  {
    Editor history;
    history.add_to_history(std::string(4U * 1024U * 1024U, 'a'));
    history.add_to_history(std::string(4U * 1024U * 1024U, 'b'));
    history.add_to_history(std::string(4U * 1024U * 1024U, 'c'));
    require(history.history().size() == 2U && history.history().front().front() == 'c',
            "prompt history byte retention did not preserve the newest whole entries");
    history.add_to_history(std::string(9U * 1024U * 1024U, 'd'));
    require(history.history().size() == 2U && history.history().front().front() == 'c',
            "oversized history entry discarded previously retained prompts");
  }
    Editor editor;
    std::size_t change_count = 0U;
    editor.set_change_callback([&change_count](const std::string&) { ++change_count; });
    insert(editor, "a");
  insert(editor, "b");
  insert(editor, "c");
  undo(editor);
  require(editor.text().empty(), "typed word did not coalesce into one undo");

  insert(editor, "a");
  insert(editor, " ");
  insert(editor, "b");
  undo(editor);
  require(editor.text() == "a", "space and following word were not one undo unit");
  undo(editor);
  require(editor.text().empty(), "initial word undo failed after spaces");

  change_count = 0U;
  insert(editor, "first second third");
  require(change_count == 1U,
          "one multi-grapheme input event emitted per-grapheme change callbacks");
  undo(editor);
  require(editor.text() == "first second",
          "batched typed input changed final word undo semantics");

  Editor paste_editor;
  const std::string large_paste(1024U * 1024U, 'p');
  InputEvent paste;
  paste.kind = InputEventKind::insert;
  paste.bytes = "\x1b[200~" + large_paste + "\x1b[201~";
  paste.text = large_paste;
  paste_editor.handle_event(paste);
  require(paste_editor.expanded_text() == large_paste,
          "large paste marker did not preserve expanded content");
  const std::size_t before_edits = peak_rss_kib();
  for (std::size_t index = 0; index < 128U; ++index) {
    insert(paste_editor, "x");
    move(paste_editor, left_arrow);
    move(paste_editor, right_arrow);
  }
  const std::size_t after_edits = peak_rss_kib();
  if (before_edits != 0U && after_edits != 0U) {
    require(after_edits - before_edits < 24U * 1024U,
            "undo snapshots copied the large paste payload per edit");
  }
  for (std::size_t index = 0; index < 128U; ++index) {
    undo(paste_editor);
  }
  require(paste_editor.expanded_text() == large_paste,
          "undo snapshots did not preserve the large paste state");
  undo(paste_editor);
  require(paste_editor.text().empty(), "large paste was not atomic for undo");
}

void verify_large_paste_marker_render_cost() {
  constexpr std::size_t repetitions = 12U;
  auto render_after_paste = [](std::size_t payload_size) {
    Editor editor;
    InputEvent paste;
    paste.kind = InputEventKind::insert;
    paste.bytes = "\x1b[200~" + std::string(payload_size, 'p') + "\x1b[201~";
    paste.text.assign(payload_size, 'p');
    editor.handle_event(paste);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < repetitions; ++index) {
      insert(editor, "x");
      require(static_cast<bool>(editor.render(RenderConstraints{80U, 6U})),
              "large marker render failed");
    }
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
  };
  const double one_mib = render_after_paste(1024U * 1024U);
  const double eight_mib = render_after_paste(8U * 1024U * 1024U);
  require(eight_mib < std::max(500.0, one_mib * 5.0),
          "paste marker rendering rescanned the original paste payload");
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
    verify_typed_undo_and_shared_large_paste();
    verify_large_paste_marker_render_cost();
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
