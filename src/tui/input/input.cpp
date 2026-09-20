#include "qt/tui/input/input.hpp"

#include "qt/foundation/utf8.hpp"

#include <uni_algo/prop.h>
#include <uni_algo/ranges_conv.h>
#include <uni_algo/ranges_grapheme.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {
namespace {

std::size_t previous_grapheme(std::string_view text, std::size_t column) {
    if (column == 0) {
        return 0;
    }
    const std::size_t target = std::min(column, text.size());
    std::size_t offset = 0;
    for (const std::string_view grapheme :
         una::ranges::grapheme::utf8_view<std::string_view>{text}) {
        const std::size_t next = offset + grapheme.size();
        if (next >= target) {
            return offset;
        }
        offset = next;
    }
    return offset;
}

std::size_t next_grapheme(std::string_view text, std::size_t column) {
    if (column >= text.size()) {
        return text.size();
    }
    std::size_t offset = 0;
    for (const std::string_view grapheme :
         una::ranges::grapheme::utf8_view<std::string_view>{text}) {
        const std::size_t next = offset + grapheme.size();
        if (column < next) {
            return next;
        }
        offset = next;
    }
    return text.size();
}

bool is_whitespace(std::string_view grapheme) {
    for (const char32_t code_point : grapheme | una::views::utf8) {
        if (!una::codepoint::prop{code_point}.White_Space()) {
            return false;
        }
    }
    return !grapheme.empty();
}

bool is_punctuation(std::string_view grapheme) {
    for (const char32_t code_point : grapheme | una::views::utf8) {
        if (!una::codepoint::prop{code_point}.General_Category_P()) {
            return false;
        }
    }
    return !grapheme.empty();
}

enum class UnitKind { word, punctuation, whitespace };

UnitKind classify(std::string_view grapheme) {
    if (is_whitespace(grapheme)) {
        return UnitKind::whitespace;
    }
    if (is_punctuation(grapheme)) {
        return UnitKind::punctuation;
    }
    return UnitKind::word;
}

std::size_t previous_word_boundary(std::string_view text, std::size_t column) {
    std::size_t cursor = std::min(column, text.size());
    while (cursor > 0) {
        const std::size_t previous = previous_grapheme(text, cursor);
        if (!is_whitespace(text.substr(previous, cursor - previous))) {
            break;
        }
        cursor = previous;
    }
    if (cursor == 0) {
        return 0;
    }

    const std::size_t first = previous_grapheme(text, cursor);
    const UnitKind kind = classify(text.substr(first, cursor - first));
    cursor = first;
    while (cursor > 0) {
        const std::size_t previous = previous_grapheme(text, cursor);
        if (classify(text.substr(previous, cursor - previous)) != kind) {
            break;
        }
        cursor = previous;
    }
    return cursor;
}

std::size_t next_word_boundary(std::string_view text, std::size_t column) {
    std::size_t cursor = std::min(column, text.size());
    while (cursor < text.size()) {
        const std::size_t next = next_grapheme(text, cursor);
        if (!is_whitespace(text.substr(cursor, next - cursor))) {
            break;
        }
        cursor = next;
    }
    if (cursor >= text.size()) {
        return text.size();
    }

    const std::size_t first_next = next_grapheme(text, cursor);
    const UnitKind kind = classify(text.substr(cursor, first_next - cursor));
    cursor = first_next;
    while (cursor < text.size()) {
        const std::size_t next = next_grapheme(text, cursor);
        if (classify(text.substr(cursor, next - cursor)) != kind) {
            break;
        }
        cursor = next;
    }
    return cursor;
}

std::string single_line_text(std::string_view text) {
    std::string cleaned;
    for (const char ch : text) {
        if (ch == '\t') {
            cleaned += "    ";
        } else if (ch != '\n' && ch != '\r') {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

bool is_word_typing(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const std::string_view grapheme :
         una::ranges::grapheme::utf8_view<std::string_view>{text}) {
        if (is_whitespace(grapheme)) {
            return false;
        }
    }
    return true;
}

std::string slice_by_column(std::string_view text, std::size_t start_column,
                            std::size_t length, bool strict = false) {
    const std::size_t end_column = start_column + length;
    std::string result;
    std::size_t column = 0;
    for (const std::string_view grapheme :
         una::ranges::grapheme::utf8_view<std::string_view>{text}) {
        auto glyph_width = foundation::utf8_display_width(grapheme);
        const std::size_t width = glyph_width ? glyph_width.value() : grapheme.size();
        const std::size_t next_column = column + width;
        if (next_column <= start_column) {
            column = next_column;
            continue;
        }
        if (column >= end_column || (strict && next_column > end_column)) {
            break;
        }
        result.append(grapheme);
        column = next_column;
    }
    return result;
}

}  // namespace

Input::Input() : keybindings_(get_keybindings_const()) {}

Input::Input(KeybindingManager keybindings) : keybindings_(std::move(keybindings)) {}

foundation::Result<RenderBuffer> Input::render(const RenderConstraints& constraints) const {
    return make_buffer({rendered_line(constraints.max_width)}, constraints);
}

void Input::handle_input(std::string_view bytes) {
    // InputHandler receives complete terminal events from the owning surface.
    // A lone ESC has already passed the terminal decoder's ambiguity timeout;
    // feeding it through this widget's decoder would leave it pending forever
    // because widgets are not polled with empty input to expire that timeout.
    if (bytes == "\x1b") {
        InputEvent event;
        event.kind = InputEventKind::key;
        event.bytes = std::string(bytes);
        event.key = KeyId{"escape"};
        handle_event(event);
        return;
    }
    decoder_.push(bytes);
    handle_events(decoder_.drain());
}

void Input::flush_input() {
    decoder_.flush();
    handle_events(decoder_.drain());
}

void Input::handle_event(const InputEvent& event) {
    if (event.kind == InputEventKind::insert) {
        insert_text(event.text);
        return;
    }
    if (event.kind != InputEventKind::key) {
        return;
    }

    if (keybindings_.matches(event.bytes, "tui.input.submit")) {
        auto callback = on_submit_;
        const std::string value = value_;
        if (callback) {
            callback(value);
        }
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.input.cancel") ||
        keybindings_.matches(event.bytes, "tui.select.cancel")) {
        auto callback = on_escape_;
        if (callback) {
            callback();
        }
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.undo")) {
        undo();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.yank") ||
        keybindings_.matches(event.bytes, "tui.editor.yank")) {
        if (const auto text = kill_ring_.peek()) {
            push_undo();
            last_yank_ = std::string(*text);
            insert_text_without_undo(last_yank_);
            last_action_ = LastAction::yank;
            notify_change();
        }
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.yank_pop") ||
        keybindings_.matches(event.bytes, "tui.editor.yankPop")) {
        if (last_action_ == LastAction::yank && kill_ring_.size() > 1U &&
            !last_yank_.empty() && cursor_ >= last_yank_.size()) {
            const std::size_t start = cursor_ - last_yank_.size();
            value_.erase(start, last_yank_.size());
            cursor_ = start;
            kill_ring_.rotate();
            if (const auto text = kill_ring_.peek()) {
                last_yank_ = std::string(*text);
                insert_text_without_undo(last_yank_);
                notify_change();
            }
        }
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.cursor.left") ||
        keybindings_.matches(event.bytes, "tui.editor.cursorLeft")) {
        last_action_ = LastAction::none;
        move_left();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.cursor.right") ||
        keybindings_.matches(event.bytes, "tui.editor.cursorRight")) {
        last_action_ = LastAction::none;
        move_right();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.backspace") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteCharBackward")) {
        erase_backward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.delete") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteCharForward")) {
        erase_forward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.delete_word_backward") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteWordBackward")) {
        erase_word_backward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.delete_word_forward") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteWordForward")) {
        erase_word_forward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.cursorWordLeft")) {
        last_action_ = LastAction::none;
        move_word_left();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.cursorWordRight")) {
        last_action_ = LastAction::none;
        move_word_right();
        return;
    }
    if (event.key == KeyId{"ctrl+a"} ||
        keybindings_.matches(event.bytes, "tui.editor.cursorLineStart")) {
        last_action_ = LastAction::none;
        move_start();
        return;
    }
    if (event.key == KeyId{"ctrl+e"} ||
        keybindings_.matches(event.bytes, "tui.editor.cursorLineEnd")) {
        last_action_ = LastAction::none;
        move_end();
        return;
    }
    if (event.key == KeyId{"ctrl+u"} ||
        keybindings_.matches(event.bytes, "tui.editor.deleteToLineStart")) {
        erase_to_start();
        return;
    }
    if (event.key == KeyId{"ctrl+k"} ||
        keybindings_.matches(event.bytes, "tui.editor.deleteToLineEnd")) {
        erase_to_end();
    }
}

void Input::handle_events(const std::vector<InputEvent>& events) {
    for (const auto& event : events) {
        handle_event(event);
    }
}

void Input::set_value(std::string value) {
    value_ = single_line_text(value);
    cursor_ = std::min(cursor_, value_.size());
    last_action_ = LastAction::none;
    last_yank_.clear();
}

std::string Input::value() const { return value_; }

std::size_t Input::cursor_column() const noexcept { return cursor_; }

void Input::set_submit_callback(SubmitCallback callback) { on_submit_ = std::move(callback); }

void Input::set_change_callback(ChangeCallback callback) { on_change_ = std::move(callback); }

void Input::set_escape_callback(EscapeCallback callback) { on_escape_ = std::move(callback); }

void Input::set_focused(bool focused) { focused_ = focused; }

bool Input::focused() const noexcept { return focused_; }

void Input::push_undo() { undo_stack_.push(State{value_, cursor_}); }

void Input::undo() {
    const auto state = undo_stack_.pop();
    if (!state) {
        return;
    }
    value_ = state->value;
    cursor_ = std::min(state->cursor, value_.size());
    last_action_ = LastAction::none;
    last_yank_.clear();
    notify_change();
}

void Input::insert_text(std::string_view text) {
    const std::string cleaned = single_line_text(text);
    text = cleaned;
    if (text.empty()) {
        return;
    }
    if (last_action_ != LastAction::type_word || !is_word_typing(text)) {
        push_undo();
    }
    insert_text_without_undo(text);
    last_action_ = is_word_typing(text) ? LastAction::type_word : LastAction::none;
    notify_change();
}

void Input::insert_text_without_undo(std::string_view text) {
    value_.insert(cursor_, text);
    cursor_ += text.size();
}

void Input::erase_backward() {
    last_action_ = LastAction::none;
    if (cursor_ == 0) {
        return;
    }
    push_undo();
    const auto previous = previous_grapheme(value_, cursor_);
    value_.erase(previous, cursor_ - previous);
    cursor_ = previous;
    notify_change();
}

void Input::erase_forward() {
    last_action_ = LastAction::none;
    if (cursor_ >= value_.size()) {
        return;
    }
    push_undo();
    const auto next = next_grapheme(value_, cursor_);
    value_.erase(cursor_, next - cursor_);
    notify_change();
}

void Input::erase_word_backward() {
    if (cursor_ == 0) {
        last_action_ = LastAction::none;
        return;
    }
    const std::size_t start = previous_word_boundary(value_, cursor_);
    if (start >= cursor_) {
        return;
    }
    const bool accumulate = last_action_ == LastAction::kill;
    push_undo();
    kill_ring_.push(value_.substr(start, cursor_ - start),
                    KillRingPushOptions{true, accumulate});
    value_.erase(start, cursor_ - start);
    cursor_ = start;
    last_action_ = LastAction::kill;
    notify_change();
}

void Input::erase_word_forward() {
    if (cursor_ >= value_.size()) {
        last_action_ = LastAction::none;
        return;
    }
    const std::size_t end = next_word_boundary(value_, cursor_);
    if (end <= cursor_) {
        return;
    }
    const bool accumulate = last_action_ == LastAction::kill;
    push_undo();
    kill_ring_.push(value_.substr(cursor_, end - cursor_),
                    KillRingPushOptions{false, accumulate});
    value_.erase(cursor_, end - cursor_);
    last_action_ = LastAction::kill;
    notify_change();
}

void Input::erase_to_start() {
    if (cursor_ == 0) {
        last_action_ = LastAction::none;
        return;
    }
    const bool accumulate = last_action_ == LastAction::kill;
    push_undo();
    kill_ring_.push(value_.substr(0, cursor_), KillRingPushOptions{true, accumulate});
    value_.erase(0, cursor_);
    cursor_ = 0;
    last_action_ = LastAction::kill;
    notify_change();
}

void Input::erase_to_end() {
    if (cursor_ >= value_.size()) {
        last_action_ = LastAction::none;
        return;
    }
    const bool accumulate = last_action_ == LastAction::kill;
    push_undo();
    kill_ring_.push(value_.substr(cursor_), KillRingPushOptions{false, accumulate});
    value_.erase(cursor_);
    last_action_ = LastAction::kill;
    notify_change();
}

void Input::move_left() { cursor_ = previous_grapheme(value_, cursor_); }

void Input::move_right() { cursor_ = next_grapheme(value_, cursor_); }

void Input::move_word_left() { cursor_ = previous_word_boundary(value_, cursor_); }

void Input::move_word_right() { cursor_ = next_word_boundary(value_, cursor_); }

void Input::move_start() { cursor_ = 0; }

void Input::move_end() { cursor_ = value_.size(); }

void Input::notify_change() {
    if (on_change_) {
        on_change_(value_);
    }
}

std::string Input::rendered_line(std::size_t width) const {
    constexpr std::string_view prompt = "> ";
    if (width <= prompt.size()) {
        return std::string(prompt);
    }

    const std::size_t content_width = width - prompt.size();
    if (content_width == 0) {
        return std::string(prompt);
    }

    const std::size_t total_width =
        foundation::utf8_display_width(value_).value_or(value_.size());
    std::string visible_text;
    std::size_t cursor_display = cursor_;

    if (total_width < content_width) {
        visible_text = value_;
    } else {
        const std::size_t scroll_width =
            cursor_ == value_.size() && content_width > 0 ? content_width - 1U : content_width;
        const std::size_t cursor_column =
            foundation::utf8_display_width(value_.substr(0, cursor_)).value_or(cursor_);
        if (scroll_width > 0) {
            const std::size_t half_width = scroll_width / 2U;
            std::size_t start_column = 0;
            if (cursor_column < half_width) {
                start_column = 0;
            } else if (cursor_column > total_width - std::min(total_width, half_width)) {
                start_column = total_width > scroll_width ? total_width - scroll_width : 0;
            } else {
                start_column = cursor_column - half_width;
            }
            visible_text = slice_by_column(value_, start_column, scroll_width, true);
            const std::string before_cursor =
                slice_by_column(value_, start_column,
                                cursor_column > start_column ? cursor_column - start_column : 0,
                                true);
            cursor_display = before_cursor.size();
        } else {
            cursor_display = 0;
        }
    }

    const std::string before_cursor =
        visible_text.substr(0, std::min(cursor_display, visible_text.size()));
    const std::string after_cursor_start =
        cursor_display <= visible_text.size() ? visible_text.substr(cursor_display) : "";
    std::string at_cursor = " ";
    std::string after_cursor;
    if (!after_cursor_start.empty()) {
        const std::size_t next = next_grapheme(after_cursor_start, 0);
        at_cursor = after_cursor_start.substr(0, next);
        after_cursor = after_cursor_start.substr(next);
    }

    std::string display = before_cursor;
    if (focused_) {
        display += cursor_marker;
    }
    display += "\x1b[7m" + at_cursor + "\x1b[27m" + after_cursor;

    auto display_width = measure_line_width(display).value_or(display.size());
    if (display_width < content_width) {
        display.append(content_width - display_width, ' ');
    }
    auto clamped = clamp_line(display, content_width);
    return std::string(prompt) + (clamped ? clamped.value() : display);
}

}  // namespace qt::tui
