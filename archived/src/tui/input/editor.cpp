#include "qt/tui/input/editor.hpp"

#include "qt/foundation/error.hpp"
#include "qt/foundation/utf8.hpp"
#include "qt/tui/utils/line_split.hpp"

#include <uni_algo/ranges_grapheme.h>
#include <uni_algo/ranges_conv.h>
#include <uni_algo/ranges_word.h>
#include <uni_algo/prop.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {
namespace {

constexpr std::size_t kDefaultUndoByteLimit = 8U * 1024U * 1024U;

std::vector<std::string> split_lines(std::string_view text) {
    return split_lines_copy(text);
}

bool is_bracketed_paste_bytes(std::string_view bytes) {
    constexpr std::string_view begin = "\x1b[200~";
    constexpr std::string_view end = "\x1b[201~";
    return bytes.size() >= begin.size() + end.size() &&
           bytes.substr(0, begin.size()) == begin &&
           bytes.substr(bytes.size() - end.size()) == end;
}

std::optional<char> decode_csi_u_ctrl_letter(std::string_view text, std::size_t offset,
                                             std::size_t& sequence_size) {
    constexpr std::string_view prefix = "\x1b[";
    constexpr std::string_view suffix = ";5u";
    if (offset >= text.size() || text.substr(offset, prefix.size()) != prefix) {
        return std::nullopt;
    }

    const std::size_t suffix_offset = text.find(suffix, offset + prefix.size());
    if (suffix_offset == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t codepoint = 0;
    for (const char ch : text.substr(offset + prefix.size(),
                                    suffix_offset - offset - prefix.size())) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        codepoint = codepoint * 10U + static_cast<std::size_t>(ch - '0');
    }

    sequence_size = suffix_offset + suffix.size() - offset;
    if (codepoint >= static_cast<std::size_t>('a') &&
        codepoint <= static_cast<std::size_t>('z')) {
        return static_cast<char>(codepoint - static_cast<std::size_t>('a') + 1U);
    }
    if (codepoint >= static_cast<std::size_t>('A') &&
        codepoint <= static_cast<std::size_t>('Z')) {
        return static_cast<char>(codepoint - static_cast<std::size_t>('A') + 1U);
    }
    return std::nullopt;
}

std::string decode_paste_control_sequences(std::string_view text) {
    std::string decoded;
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::size_t sequence_size = 0;
        if (const auto control =
                decode_csi_u_ctrl_letter(text, offset, sequence_size)) {
            decoded.push_back(*control);
            offset += sequence_size;
            continue;
        }
        decoded.push_back(text[offset]);
        ++offset;
    }
    return decoded;
}

std::string normalize_bracketed_paste(std::string_view text) {
    const auto decoded = decode_paste_control_sequences(text);
    std::string normalized;
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        const char ch = decoded[index];
        if (ch == '\r') {
            if (index + 1U < decoded.size() && decoded[index + 1U] == '\n') {
                ++index;
            }
            normalized.push_back('\n');
            continue;
        }
        if (ch == '\t') {
            normalized += "    ";
            continue;
        }
        if (ch == '\n' || static_cast<unsigned char>(ch) >= 0x20U) {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

bool is_file_pathish_paste(std::string_view text) {
    return !text.empty() && (text.front() == '/' || text.front() == '~' || text.front() == '.');
}

bool is_ascii_word_byte(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

std::size_t paste_marker_end(std::string_view text, std::size_t offset) {
    constexpr std::string_view prefix = "[paste #";
    if (offset >= text.size() || text.substr(offset, prefix.size()) != prefix) {
        return std::string_view::npos;
    }

    const std::size_t end = text.find(']', offset + prefix.size());
    if (end == std::string_view::npos) {
        return std::string_view::npos;
    }
    return end + 1U;
}

std::size_t next_paste_marker_start(std::string_view text, std::size_t offset) {
    constexpr std::string_view prefix = "[paste #";
    std::size_t candidate = text.find(prefix, offset);
    while (candidate != std::string_view::npos) {
        if (paste_marker_end(text, candidate) != std::string_view::npos) {
            return candidate;
        }
        candidate = text.find(prefix, candidate + 1U);
    }
    return std::string_view::npos;
}

WordSegmentKind classify_word_segment(std::string_view segment, bool unicode_word_space,
                                      bool unicode_word_punctuation) {
    if (unicode_word_space) {
        return WordSegmentKind::whitespace;
    }
    if (unicode_word_punctuation) {
        return WordSegmentKind::punctuation;
    }

    bool saw_code_point = false;
    bool all_whitespace = true;
    bool all_punctuation = true;
    for (const char32_t code_point : segment | una::views::utf8) {
        saw_code_point = true;
        const una::codepoint::prop properties{code_point};
        all_whitespace = all_whitespace && properties.White_Space();
        all_punctuation = all_punctuation && properties.General_Category_P();
    }

    if (saw_code_point && all_whitespace) {
        return WordSegmentKind::whitespace;
    }
    if (saw_code_point && all_punctuation) {
        return WordSegmentKind::punctuation;
    }
    return WordSegmentKind::word;
}

std::vector<WordSegment> word_segments_for_slice(std::string_view text, std::size_t base) {
    std::vector<WordSegment> segments;
    auto view = una::ranges::word::utf8_view<std::string_view>{text};
    for (auto it = view.begin(); it != view.end(); ++it) {
        const auto begin =
            static_cast<std::size_t>(std::distance(text.begin(), it.begin()));
        const auto end = static_cast<std::size_t>(std::distance(text.begin(), it.end()));
        if (begin == end) {
            continue;
        }

        const WordSegmentKind kind =
            classify_word_segment(*it, it.is_segspace() || it.is_newline(),
                                  it.is_punctuation());
        segments.push_back(WordSegment{base + begin, base + end, kind});
    }
    return segments;
}

std::vector<WordSegment> word_segments(std::string_view text) {
    std::vector<WordSegment> segments;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t marker_start = next_paste_marker_start(text, offset);
        const std::size_t slice_end =
            marker_start == std::string_view::npos ? text.size() : marker_start;

        auto slice_segments = word_segments_for_slice(text.substr(offset, slice_end - offset),
                                                      offset);
        segments.insert(segments.end(), slice_segments.begin(), slice_segments.end());

        if (marker_start == std::string_view::npos) {
            break;
        }

        const std::size_t marker_end = paste_marker_end(text, marker_start);
        segments.push_back(WordSegment{marker_start, marker_end, WordSegmentKind::marker});
        offset = marker_end;
    }
    return segments;
}

std::size_t segment_before_or_at(const std::vector<WordSegment>& segments,
                                 std::size_t column) {
    std::size_t index = segments.size();
    while (index > 0) {
        const auto& segment = segments[index - 1U];
        if (segment.begin < column || (segment.begin == column && segment.end == column)) {
            return index - 1U;
        }
        --index;
    }
    return segments.size();
}

std::size_t segment_at_or_after(const std::vector<WordSegment>& segments,
                                std::size_t column) {
    for (std::size_t index = 0; index < segments.size(); ++index) {
        if (segments[index].end > column) {
            return index;
        }
    }
    return segments.size();
}

bool word_run_kind(WordSegmentKind kind) {
    return kind != WordSegmentKind::whitespace && kind != WordSegmentKind::punctuation &&
           kind != WordSegmentKind::marker;
}

std::string paste_marker(std::size_t paste_id, std::size_t line_count,
                         std::size_t byte_count) {
    std::ostringstream marker;
    marker << "[paste #" << paste_id;
    if (line_count > 10U) {
        marker << " +" << line_count << " lines";
    } else {
        marker << ' ' << byte_count << " chars";
    }
    marker << ']';
    return marker.str();
}

void replace_all(std::string& text, const std::string& needle,
                 const std::string& replacement) {
    if (needle.empty()) {
        return;
    }

    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        text.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
}

std::string trim_copy(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return std::string(text.substr(start, end - start));
}

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

std::size_t previous_word_boundary_from_segments(
    std::string_view text, std::size_t column,
    const std::vector<WordSegment>& segments) {
    std::size_t cursor = std::min(column, text.size());
    std::size_t index = segment_before_or_at(segments, cursor);
    if (index == segments.size()) {
        return cursor;
    }

    while (index < segments.size() && segments[index].kind == WordSegmentKind::whitespace) {
        cursor = segments[index].begin;
        if (index == 0) {
            return cursor;
        }
        --index;
    }

    if (segments[index].kind == WordSegmentKind::marker) {
        return segments[index].begin;
    }

    const WordSegmentKind kind = segments[index].kind;
    while (index < segments.size()) {
        const auto& segment = segments[index];
        if (kind == WordSegmentKind::punctuation) {
            if (segment.kind != WordSegmentKind::punctuation) {
                break;
            }
        } else if (!word_run_kind(segment.kind)) {
            break;
        }
        cursor = segment.begin;
        if (index == 0) {
            break;
        }
        --index;
    }
    return cursor;
}

std::size_t next_word_boundary_from_segments(
    std::string_view text, std::size_t column,
    const std::vector<WordSegment>& segments) {
    std::size_t cursor = std::min(column, text.size());
    std::size_t index = segment_at_or_after(segments, cursor);
    if (index == segments.size()) {
        return cursor;
    }

    while (index < segments.size() && segments[index].kind == WordSegmentKind::whitespace) {
        cursor = segments[index].end;
        ++index;
    }

    if (index >= segments.size()) {
        return cursor;
    }
    if (segments[index].kind == WordSegmentKind::marker) {
        return segments[index].end;
    }

    const WordSegmentKind kind = segments[index].kind;
    while (index < segments.size()) {
        const auto& segment = segments[index];
        if (kind == WordSegmentKind::punctuation) {
            if (segment.kind != WordSegmentKind::punctuation) {
                break;
            }
        } else if (!word_run_kind(segment.kind)) {
            break;
        }
        cursor = segment.end;
        ++index;
    }
    return cursor;
}

std::size_t display_column_for_byte(std::string_view text, std::size_t column) {
    const auto prefix = text.substr(0, std::min(column, text.size()));
    auto width = foundation::utf8_display_width(prefix);
    return width ? width.value() : prefix.size();
}

std::size_t display_width(std::string_view text) {
    auto width = foundation::utf8_display_width(text);
    return width ? width.value() : text.size();
}

std::size_t byte_column_for_display(std::string_view text, std::size_t target_column) {
    std::size_t columns = 0;
    std::size_t offset = 0;
    for (const std::string_view grapheme :
         una::ranges::grapheme::utf8_view<std::string_view>{text}) {
        auto glyph_width = foundation::utf8_display_width(grapheme);
        if (!glyph_width || columns + glyph_width.value() > target_column) {
            break;
        }
        columns += glyph_width.value();
        offset += grapheme.size();
    }
    return offset;
}

bool is_whitespace_text(std::string_view text) {
    if (text.empty()) {
        return false;
    }

    bool saw_code_point = false;
    for (const char32_t code_point : text | una::views::utf8) {
        saw_code_point = true;
        const una::codepoint::prop properties{code_point};
        if (!properties.White_Space()) {
            return false;
        }
    }
    return saw_code_point;
}

}  // namespace

Editor::Editor() : keybindings_(get_keybindings_const()), lines_{""} {
    undo_stack_.set_byte_limit(kDefaultUndoByteLimit);
}

Editor::Editor(KeybindingManager keybindings)
    : keybindings_(std::move(keybindings)), lines_{""} {
    undo_stack_.set_byte_limit(kDefaultUndoByteLimit);
}

foundation::Result<RenderBuffer> Editor::render(
    const RenderConstraints& constraints) const {
    return make_buffer(rendered_lines(constraints.max_width), constraints);
}

void Editor::handle_input(std::string_view bytes) {
    decoder_.push(bytes);
    handle_events(decoder_.drain());
}

void Editor::flush_input() {
    decoder_.flush();
    handle_events(decoder_.drain());
}

void Editor::handle_event(const InputEvent& event) {
    if (handle_pending_jump(event)) {
        return;
    }

    switch (event.kind) {
        case InputEventKind::insert:
            reset_kill_action();
            insert_event_text(event);
            return;
        case InputEventKind::raw:
        case InputEventKind::resize:
        case InputEventKind::scroll:
        case InputEventKind::ime_composition:
            return;
        case InputEventKind::key:
            break;
    }
    last_action_was_type_word_ = false;

    if (event.bytes == "\n" || keybindings_.matches(event.bytes, "tui.input.newLine")) {
        reset_kill_action();
        insert_newline();
        return;
    }

    if (keybindings_.matches(event.bytes, "app.clipboard.pasteImage")) {
        reset_kill_action();
        if (on_paste_image_) {
            const auto pasted_path = on_paste_image_();
            if (pasted_path && !pasted_path->empty()) {
                insert_text(*pasted_path);
            }
        }
        return;
    }

    if (keybindings_.matches(event.bytes, "tui.input.submit")) {
        reset_kill_action();
        if (replace_trailing_backslash_with_newline()) {
            return;
        }
        submit();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.undo")) {
        reset_kill_action();
        undo();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.jump_forward") ||
        keybindings_.matches(event.bytes, "tui.editor.jumpForward")) {
        reset_kill_action();
        pending_jump_ = JumpDirection::forward;
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.jump_backward") ||
        keybindings_.matches(event.bytes, "tui.editor.jumpBackward")) {
        reset_kill_action();
        pending_jump_ = JumpDirection::backward;
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.input.tab")) {
        reset_kill_action();
        insert_text("    ");
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.cursor.left") ||
        keybindings_.matches(event.bytes, "tui.editor.cursorLeft")) {
        reset_kill_action();
        move_left();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.cursor.right") ||
        keybindings_.matches(event.bytes, "tui.editor.cursorRight")) {
        reset_kill_action();
        move_right();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.cursor.up") ||
        keybindings_.matches(event.bytes, "tui.editor.cursorUp")) {
        reset_kill_action();
        if (is_empty()) {
            navigate_history(-1);
            return;
        }
        if (is_on_first_visual_line()) {
            if (history_index_ >= 0) {
                navigate_history(-1);
            } else {
                move_line_start();
            }
            return;
        }
        move_up();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.cursor.down") ||
        keybindings_.matches(event.bytes, "tui.editor.cursorDown")) {
        reset_kill_action();
        if (is_on_last_visual_line()) {
            if (history_index_ >= 0) {
                navigate_history(1);
            } else {
                move_line_end();
            }
            return;
        }
        move_down();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.backspace") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteCharBackward") ||
        matches_key(event.bytes, "shift+backspace")) {
        reset_kill_action();
        erase_backward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.delete") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteCharForward") ||
        matches_key(event.bytes, "shift+delete")) {
        reset_kill_action();
        erase_forward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.delete_word_backward") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteWordBackward")) {
        last_action_was_yank_ = false;
        erase_word_backward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.delete_word_forward") ||
        keybindings_.matches(event.bytes, "tui.editor.deleteWordForward")) {
        last_action_was_yank_ = false;
        erase_word_forward();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.yank") ||
        keybindings_.matches(event.bytes, "tui.editor.yank")) {
        yank();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.edit.yank_pop") ||
        keybindings_.matches(event.bytes, "tui.editor.yankPop")) {
        yank_pop();
        return;
    }

    if (event.key == KeyId{"ctrl+a"} ||
        keybindings_.matches(event.bytes, "tui.editor.cursorLineStart")) {
        reset_kill_action();
        move_line_start();
        return;
    }
    if (event.key == KeyId{"ctrl+e"} ||
        keybindings_.matches(event.bytes, "tui.editor.cursorLineEnd")) {
        reset_kill_action();
        move_line_end();
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.cursorWordLeft")) {
        reset_kill_action();
        reset_preferred_column();
        if (cursor_column_ == 0 && cursor_row_ > 0) {
            --cursor_row_;
            cursor_column_ = lines_[cursor_row_].size();
        } else {
            cursor_column_ = previous_word_boundary(cursor_row_, cursor_column_);
        }
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.cursorWordRight")) {
        reset_kill_action();
        reset_preferred_column();
        if (cursor_column_ >= lines_[cursor_row_].size() &&
            cursor_row_ + 1U < lines_.size()) {
            ++cursor_row_;
            cursor_column_ = 0;
        } else {
            cursor_column_ = next_word_boundary(cursor_row_, cursor_column_);
        }
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.pageUp")) {
        reset_kill_action();
        page_move(-1);
        return;
    }
    if (keybindings_.matches(event.bytes, "tui.editor.pageDown")) {
        reset_kill_action();
        page_move(1);
        return;
    }
    if (event.key == KeyId{"ctrl+u"} ||
        keybindings_.matches(event.bytes, "tui.editor.deleteToLineStart")) {
        last_action_was_yank_ = false;
        erase_to_line_start();
        return;
    }
    if (event.key == KeyId{"ctrl+k"} ||
        keybindings_.matches(event.bytes, "tui.editor.deleteToLineEnd")) {
        last_action_was_yank_ = false;
        erase_to_line_end();
    }
}

void Editor::handle_events(const std::vector<InputEvent>& events) {
    for (const auto& event : events) {
        handle_event(event);
    }
}

bool Editor::handle_pending_jump(const InputEvent& event) {
    if (!pending_jump_) {
        return false;
    }

    if (event.kind == InputEventKind::key) {
        if ((*pending_jump_ == JumpDirection::forward &&
             (keybindings_.matches(event.bytes, "tui.editor.jump_forward") ||
              keybindings_.matches(event.bytes, "tui.editor.jumpForward"))) ||
            (*pending_jump_ == JumpDirection::backward &&
             (keybindings_.matches(event.bytes, "tui.editor.jump_backward") ||
              keybindings_.matches(event.bytes, "tui.editor.jumpBackward"))) ||
            event.key == KeyId{"escape"}) {
            pending_jump_.reset();
            return true;
        }

        pending_jump_.reset();
        return false;
    }

    if (event.kind != InputEventKind::insert || event.text.empty()) {
        pending_jump_.reset();
        return false;
    }

    const auto direction = *pending_jump_;
    pending_jump_.reset();
    std::size_t character_size = event.text.size();
    for (const std::string_view grapheme :
         una::ranges::grapheme::utf8_view<std::string_view>{event.text}) {
        character_size = grapheme.size();
        break;
    }
    jump_to_character(std::string_view(event.text).substr(0, character_size), direction);
    return true;
}

void Editor::jump_to_character(std::string_view character, JumpDirection direction) {
    if (character.empty() || lines_.empty()) {
        return;
    }

    reset_kill_action();
    reset_preferred_column();
    exit_history_browse();

    if (direction == JumpDirection::forward) {
        for (std::size_t row = cursor_row_; row < lines_.size(); ++row) {
            const std::string_view line = lines_[row];
            const std::size_t start =
                row == cursor_row_ ? next_grapheme(line, cursor_column_) : 0U;
            std::size_t offset = 0;
            for (const std::string_view grapheme :
                 una::ranges::grapheme::utf8_view<std::string_view>{line}) {
                if (offset >= start && grapheme == character) {
                    cursor_row_ = row;
                    cursor_column_ = offset;
                    return;
                }
                offset += grapheme.size();
            }
        }
        return;
    }

    for (std::size_t row = cursor_row_ + 1U; row > 0U; --row) {
        const std::size_t current_row = row - 1U;
        const std::string_view line = lines_[current_row];
        const std::size_t limit =
            current_row == cursor_row_ ? std::min(cursor_column_, line.size()) : line.size();

        bool found_match = false;
        std::size_t match = 0;
        std::size_t offset = 0;
        for (const std::string_view grapheme :
             una::ranges::grapheme::utf8_view<std::string_view>{line}) {
            if (offset >= limit) {
                break;
            }
            if (grapheme == character) {
                found_match = true;
                match = offset;
            }
            offset += grapheme.size();
        }

        if (found_match) {
            cursor_row_ = current_row;
            cursor_column_ = match;
            return;
        }
    }
}

void Editor::set_text(std::string_view text) {
    auto lines = split_lines(text);
    const std::size_t row = lines.empty() ? 0 : lines.size() - 1;
    const std::size_t column = lines.empty() ? 0 : lines[row].size();
    set_contents(std::move(lines), row, column);
}

void Editor::set_contents(std::vector<std::string> lines, std::size_t row,
                          std::size_t column) {
    last_action_was_type_word_ = false;
    reset_kill_action();
    reset_preferred_column();
    pending_jump_.reset();
    undo_stack_.clear();
    pastes_.clear();
    paste_counter_ = 0;
    exit_history_browse();
    replace_contents(std::move(lines), row, column, false);
}

void Editor::apply_completion_contents(std::vector<std::string> lines, std::size_t row,
                                       std::size_t column) {
    last_action_was_type_word_ = false;
    reset_kill_action();
    reset_preferred_column();
    pending_jump_.reset();
    capture_undo_state();
    exit_history_browse();
    replace_contents(std::move(lines), row, column, true);
}

std::string Editor::text() const {
    std::string combined;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (index > 0) {
            combined.push_back('\n');
        }
        combined += lines_[index];
    }
    return combined;
}

std::string Editor::expanded_text() const {
    std::string expanded = text();
    for (const auto& paste : pastes_) {
        replace_all(expanded, paste.marker, *paste.text);
    }
    return expanded;
}

void Editor::add_to_history(std::string_view text) {
    if (history_limit_ == 0U) {
        return;
    }
    auto trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return;
    }

    if (trimmed.size() > 8U * 1024U * 1024U) return;

    if (!history_.empty() && history_.front() == trimmed) {
        return;
    }

    history_.insert(history_.begin(), std::move(trimmed));
    trim_history_to_limits();
}

void Editor::set_history_limit(std::size_t max_entries) {
    history_limit_ = max_entries;
    trim_history_to_limits();
    exit_history_browse();
}

void Editor::set_history(std::vector<std::string> history) {
    history_.clear();
    if (history_limit_ == 0U) {
        exit_history_browse();
        return;
    }
    history_.reserve(std::min(history.size(), history_limit_));
    for (auto& item : history) {
        auto trimmed = trim_copy(item);
        if (trimmed.empty()) {
            continue;
        }
        if (!history_.empty() && history_.back() == trimmed) {
            continue;
        }
        history_.push_back(std::move(trimmed));
        if (history_.size() >= history_limit_) {
            break;
        }
    }
    trim_history_to_limits();
    exit_history_browse();
}

void Editor::trim_history_to_limits() {
    constexpr std::size_t kHistoryByteLimit = 8U * 1024U * 1024U;
    std::size_t bytes = 0U;
    std::size_t retained = 0U;
    for (const auto& prompt : history_) {
        if (retained == history_limit_ || prompt.size() > kHistoryByteLimit - bytes) break;
        bytes += prompt.size();
        ++retained;
    }
    history_.resize(retained);
}

const std::vector<std::string>& Editor::history() const noexcept {
    return history_;
}

bool Editor::browsing_history() const noexcept {
    return history_index_ >= 0;
}

void Editor::set_submit_callback(SubmitCallback callback) {
    on_submit_ = std::move(callback);
}

void Editor::set_change_callback(ChangeCallback callback) {
    on_change_ = std::move(callback);
}

void Editor::set_change_observer(ChangeObserver observer) {
    on_change_observer_ = std::move(observer);
}

void Editor::set_undo_byte_limit(std::size_t max_bytes) {
    undo_stack_.set_byte_limit(max_bytes);
}

std::size_t Editor::undo_byte_limit() const noexcept {
    return undo_stack_.byte_limit();
}

void Editor::set_paste_image_callback(PasteImageCallback callback) {
    on_paste_image_ = std::move(callback);
}

void Editor::set_padding_x(std::size_t padding_x) noexcept { padding_x_ = padding_x; }

std::size_t Editor::padding_x() const noexcept { return padding_x_; }

void Editor::set_max_visible_lines(std::size_t max_visible_lines) noexcept {
    max_visible_lines_ = std::max<std::size_t>(1, max_visible_lines);
}

std::size_t Editor::max_visible_lines() const noexcept { return max_visible_lines_; }

void Editor::set_focused(bool focused) { focused_ = focused; }

bool Editor::focused() const noexcept { return focused_; }

std::size_t Editor::cursor_row() const noexcept { return cursor_row_; }

std::size_t Editor::cursor_column() const noexcept { return cursor_column_; }

void Editor::insert_newline() {
    insert_newline_impl(true);
}

void Editor::insert_newline_impl(bool record_undo) {
    last_action_was_type_word_ = false;
    if (record_undo) {
        capture_undo_state();
    }
    reset_preferred_column();
    exit_history_browse();
    auto before = lines_[cursor_row_].substr(0, cursor_column_);
    auto after = lines_[cursor_row_].substr(cursor_column_);
    lines_[cursor_row_] = std::move(before);
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_ + 1),
                  std::move(after));
    ++cursor_row_;
    cursor_column_ = 0;
    notify_change();
}

bool Editor::submit() {
    const bool submitted = static_cast<bool>(on_submit_);
    const std::string result = trim_copy(expanded_text());
    lines_ = {""};
    cursor_row_ = 0;
    cursor_column_ = 0;
    pastes_.clear();
    paste_counter_ = 0;
    history_index_ = -1;
    history_draft_.reset();
    pending_jump_.reset();
    reset_preferred_column();
    reset_kill_action();
    last_action_was_type_word_ = false;
    undo_stack_.clear();
    notify_change();

    if (on_submit_) {
        on_submit_(result);
    }
    return submitted;
}

void Editor::replace_contents(std::vector<std::string> lines, std::size_t row, std::size_t column,
                              bool notify) {
    if (lines.empty()) {
        lines.emplace_back("");
    }

    lines_ = std::move(lines);
    cursor_row_ = std::min(row, lines_.size() - 1);
    cursor_column_ = std::min(column, lines_[cursor_row_].size());
    pending_jump_.reset();

    if (notify) {
        notify_change();
    } else {
        invalidate_visual_layout();
    }
}

void Editor::capture_undo_state() {
    undo_stack_.push(Snapshot{lines_, cursor_row_, cursor_column_, pastes_,
                              paste_counter_}, undo_snapshot_bytes());
}

void Editor::undo() {
    auto snapshot = undo_stack_.pop();
    if (!snapshot) {
        return;
    }

    reset_preferred_column();
    exit_history_browse();
    pending_jump_.reset();
    last_action_was_type_word_ = false;
    lines_ = std::move(snapshot->lines);
    pastes_ = std::move(snapshot->pastes);
    paste_counter_ = snapshot->paste_counter;
    if (lines_.empty()) {
        lines_.emplace_back("");
    }
    cursor_row_ = std::min(snapshot->cursor_row, lines_.size() - 1U);
    cursor_column_ = std::min(snapshot->cursor_column, lines_[cursor_row_].size());
    notify_change();
}

void Editor::insert_event_text(const InputEvent& event) {
    if (is_bracketed_paste_bytes(event.bytes)) {
        last_action_was_type_word_ = false;
        std::string pasted = normalize_bracketed_paste(event.text);
        if (is_file_pathish_paste(pasted) && cursor_column_ > 0U) {
            const char before = lines_[cursor_row_][cursor_column_ - 1U];
            if (is_ascii_word_byte(before)) {
                pasted.insert(pasted.begin(), ' ');
            }
        }
        if (insert_large_paste_marker(pasted)) {
            return;
        }
        insert_text(pasted);
        return;
    }
    insert_typed_text(event.text);
}

void Editor::insert_typed_text(std::string_view text) {
    std::size_t offset = 0U;
    std::size_t group_start = 0U;
    bool group_open = false;
    for (const std::string_view grapheme :
         una::ranges::grapheme::utf8_view<std::string_view>{text}) {
        const bool boundary = is_whitespace_text(grapheme) || !last_action_was_type_word_;
        if (boundary) {
            if (group_open) {
                insert_text_impl(text.substr(group_start, offset - group_start), false, false);
            }
            capture_undo_state();
            group_start = offset;
            group_open = true;
        }
        if (!group_open) {
            group_start = offset;
            group_open = true;
        }
        last_action_was_type_word_ = true;
        offset += grapheme.size();
    }
    if (group_open) {
        insert_text_impl(text.substr(group_start, offset - group_start), false, false);
        notify_change();
    }
}

bool Editor::insert_large_paste_marker(std::string_view text) {
    if (text.empty()) {
        return false;
    }

    const auto pasted_lines = split_lines(text);
    if (pasted_lines.size() <= 10U && text.size() <= 1000U) {
        return false;
    }

    capture_undo_state();
    const auto paste_id = ++paste_counter_;
    const auto marker = paste_marker(paste_id, pasted_lines.size(), text.size());
    pastes_.push_back(Paste{paste_id, std::make_shared<const std::string>(text),
                            pasted_lines.size(), marker});
    insert_text_impl(marker, false);
    return true;
}

void Editor::insert_text(std::string_view text) {
    last_action_was_type_word_ = false;
    insert_text_impl(text, true);
}

void Editor::insert_text_impl(std::string_view text, bool record_undo, bool notify) {
    if (text.empty()) {
        return;
    }

    if (record_undo) {
        capture_undo_state();
    }
    reset_preferred_column();
    exit_history_browse();
    auto inserted_lines = split_lines(text);
    const std::string prefix = lines_[cursor_row_].substr(0, cursor_column_);
    const std::string suffix = lines_[cursor_row_].substr(cursor_column_);
    lines_[cursor_row_] = prefix + inserted_lines.front();

    if (inserted_lines.size() == 1U) {
        lines_[cursor_row_] += suffix;
        cursor_column_ += inserted_lines.front().size();
        if (notify) {
            notify_change();
        }
        return;
    }

    for (std::size_t index = 1; index < inserted_lines.size(); ++index) {
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_ + index),
                      inserted_lines[index]);
    }

    cursor_row_ += inserted_lines.size() - 1;
    lines_[cursor_row_] += suffix;
    cursor_column_ = inserted_lines.back().size();
    if (notify) {
        notify_change();
    }
}

void Editor::move_left() {
    reset_preferred_column();
    if (auto marker = valid_paste_marker_ending_at(cursor_row_, cursor_column_)) {
        cursor_column_ = marker->first;
        return;
    }

    if (cursor_column_ > 0) {
        cursor_column_ = previous_grapheme_boundary(cursor_row_, cursor_column_);
        return;
    }

    if (cursor_row_ == 0) {
        return;
    }

    --cursor_row_;
    cursor_column_ = lines_[cursor_row_].size();
}

void Editor::move_right() {
    reset_preferred_column();
    if (auto marker = valid_paste_marker_at(cursor_row_, cursor_column_)) {
        cursor_column_ = marker->second;
        return;
    }

    if (cursor_column_ < lines_[cursor_row_].size()) {
        cursor_column_ = next_grapheme_boundary(cursor_row_, cursor_column_);
        return;
    }

    if (cursor_row_ + 1 >= lines_.size()) {
        return;
    }

    ++cursor_row_;
    cursor_column_ = 0;
}

void Editor::move_up() {
    move_visual(-1);
}

void Editor::move_down() {
    move_visual(1);
}

void Editor::page_move(int direction) {
    const std::size_t page_size = std::max<std::size_t>(1, max_visible_lines_);
    for (std::size_t step = 0; step < page_size; ++step) {
        const std::size_t before_row = cursor_row_;
        const std::size_t before_column = cursor_column_;
        if (direction < 0) {
            move_up();
        } else {
            move_down();
        }
        if (cursor_row_ == before_row && cursor_column_ == before_column) {
            break;
        }
    }
}

void Editor::move_line_start() {
    reset_preferred_column();
    cursor_column_ = 0;
}

void Editor::move_line_end() {
    reset_preferred_column();
    cursor_column_ = lines_[cursor_row_].size();
}

const std::vector<WordSegment>& Editor::word_segments_for_row(
    std::size_t row) const {
    static const std::vector<WordSegment> empty_segments;
    if (row >= lines_.size()) {
        return empty_segments;
    }
    if (word_segment_cache_.size() <= row) {
        word_segment_cache_.resize(row + 1U);
    }

    auto& entry = word_segment_cache_[row];
    if (!entry.valid || entry.line != lines_[row]) {
        entry.valid = true;
        entry.line = lines_[row];
        entry.segments = word_segments(entry.line);
    }
    return entry.segments;
}

std::size_t Editor::previous_word_boundary(std::size_t row,
                                           std::size_t column) const {
    if (row >= lines_.size()) {
        return column;
    }
    return previous_word_boundary_from_segments(lines_[row], column,
                                                word_segments_for_row(row));
}

std::size_t Editor::next_word_boundary(std::size_t row,
                                       std::size_t column) const {
    if (row >= lines_.size()) {
        return column;
    }
    return next_word_boundary_from_segments(lines_[row], column,
                                            word_segments_for_row(row));
}

const std::vector<std::size_t>& Editor::grapheme_boundaries_for_row(
    std::size_t row) const {
    static const std::vector<std::size_t> empty_boundaries{0U};
    if (row >= lines_.size()) {
        return empty_boundaries;
    }
    if (grapheme_boundary_cache_.size() <= row) {
        grapheme_boundary_cache_.resize(row + 1U);
    }

    auto& entry = grapheme_boundary_cache_[row];
    if (!entry.valid || entry.revision != visual_layout_revision_) {
        entry.valid = true;
        entry.revision = visual_layout_revision_;
        entry.boundaries.clear();
        entry.boundaries.reserve(lines_[row].size() + 1U);
        entry.boundaries.push_back(0U);
        std::size_t offset = 0U;
        for (const std::string_view grapheme :
             una::ranges::grapheme::utf8_view<std::string_view>{lines_[row]}) {
            offset += grapheme.size();
            entry.boundaries.push_back(offset);
        }
    }
    return entry.boundaries;
}

std::size_t Editor::previous_grapheme_boundary(std::size_t row,
                                               std::size_t column) const {
    if (row >= lines_.size() || column == 0U) {
        return 0U;
    }
    const auto& boundaries = grapheme_boundaries_for_row(row);
    auto boundary = std::lower_bound(boundaries.begin(), boundaries.end(),
                                     std::min(column, lines_[row].size()));
    if (boundary == boundaries.begin()) {
        return 0U;
    }
    return *--boundary;
}

std::size_t Editor::next_grapheme_boundary(std::size_t row,
                                           std::size_t column) const {
    if (row >= lines_.size() || column >= lines_[row].size()) {
        return row < lines_.size() ? lines_[row].size() : column;
    }
    const auto& boundaries = grapheme_boundaries_for_row(row);
    const auto boundary =
        std::upper_bound(boundaries.begin(), boundaries.end(), column);
    return boundary == boundaries.end() ? lines_[row].size() : *boundary;
}

const std::vector<Editor::VisualLine>& Editor::build_visual_lines(
    std::size_t width) const {
    const std::size_t wrap_width = std::max<std::size_t>(1, width);

    auto update_cursor = [&](VisualLayoutCache& cache) {
        if (cache.layout.empty()) {
            return;
        }
        if (cache.cursor_visual_index < cache.layout.size()) {
            cache.layout[cache.cursor_visual_index].has_cursor = false;
        }

        auto owns_cursor = [&](std::size_t index) {
            const auto& chunk = cache.layout[index];
            const bool is_last_chunk =
                index + 1U >= cache.layout.size() ||
                cache.layout[index + 1U].logical_row != chunk.logical_row;
            return chunk.logical_row == cursor_row_ &&
                   cursor_column_ >= chunk.start_column &&
                   (cursor_column_ < chunk.end_column ||
                    (is_last_chunk && cursor_column_ <= chunk.end_column));
        };

        std::size_t cursor_index = cache.cursor_visual_index;
        if (cursor_index >= cache.layout.size() || !owns_cursor(cursor_index)) {
            if (cursor_index + 1U < cache.layout.size() &&
                owns_cursor(cursor_index + 1U)) {
                ++cursor_index;
            } else if (cursor_index > 0U && owns_cursor(cursor_index - 1U)) {
                --cursor_index;
            } else {
                cursor_index = 0U;
                while (cursor_index + 1U < cache.layout.size() &&
                       !owns_cursor(cursor_index)) {
                    ++cursor_index;
                }
            }
        }

        auto& chunk = cache.layout[cursor_index];
        chunk.has_cursor = true;
        chunk.cursor_column =
            std::min(cursor_column_, chunk.end_column) - chunk.start_column;
        cache.cursor_row = cursor_row_;
        cache.cursor_column = cursor_column_;
        cache.cursor_visual_index = cursor_index;
    };

    if (visual_layout_cache_ && visual_layout_cache_->revision == visual_layout_revision_ &&
        visual_layout_cache_->width == wrap_width) {
        update_cursor(*visual_layout_cache_);
        return visual_layout_cache_->layout;
    }

    struct WrapUnit {
        std::size_t start = 0;
        std::size_t end = 0;
        std::size_t width = 0;
        bool whitespace = false;
    };

    std::vector<VisualLine> layout;

    auto wrap_row = [&](std::size_t row) {
        std::vector<VisualLine> chunks;
        const std::string& line = lines_[row];
        if (line.empty()) {
            chunks.push_back(VisualLine{row, 0, 0, "", false, 0});
            return chunks;
        }

        std::vector<WrapUnit> units;
        const auto& boundaries = grapheme_boundaries_for_row(row);
        std::size_t boundary_index = 1U;
        std::size_t offset = 0;
        while (offset < line.size()) {
            if (const auto marker = valid_paste_marker_at(row, offset)) {
                const std::string_view text(line.data() + marker->first,
                                            marker->second - marker->first);
                units.push_back(
                    WrapUnit{marker->first, marker->second, display_width(text), false});
                offset = marker->second;
                boundary_index = static_cast<std::size_t>(
                    std::upper_bound(boundaries.begin(), boundaries.end(), offset) -
                    boundaries.begin());
                continue;
            }

            while (boundary_index < boundaries.size() &&
                   boundaries[boundary_index] <= offset) {
                ++boundary_index;
            }
            const std::size_t next = boundary_index < boundaries.size()
                                         ? boundaries[boundary_index++]
                                         : line.size();
            const std::string_view text(line.data() + offset, next - offset);
            units.push_back(WrapUnit{offset, next, display_width(text),
                                     is_whitespace_text(text)});
            offset = next;
        }

        auto push_chunk = [&](std::size_t start, std::size_t end) {
            if (end < start) {
                end = start;
            }
            chunks.push_back(VisualLine{
                row,
                start,
                end,
                line.substr(start, end - start),
                false,
                0,
            });
        };

        std::size_t chunk_start = 0;
        std::size_t current_width = 0;
        std::size_t wrap_opp_index = std::string_view::npos;
        std::size_t wrap_opp_width = 0;

        for (std::size_t index = 0; index < units.size(); ++index) {
            const auto& unit = units[index];
            if (current_width > 0 && current_width + unit.width > wrap_width) {
                if (wrap_opp_index != std::string_view::npos &&
                    current_width >= wrap_opp_width &&
                    current_width - wrap_opp_width + unit.width <= wrap_width) {
                    push_chunk(chunk_start, wrap_opp_index);
                    chunk_start = wrap_opp_index;
                    current_width -= wrap_opp_width;
                } else {
                    push_chunk(chunk_start, unit.start);
                    chunk_start = unit.start;
                    current_width = 0;
                }
                wrap_opp_index = std::string_view::npos;
                wrap_opp_width = 0;
            }

            current_width += unit.width;
            const bool has_next = index + 1U < units.size();
            if (unit.whitespace && has_next && !units[index + 1U].whitespace) {
                wrap_opp_index = units[index + 1U].start;
                wrap_opp_width = current_width;
            }
        }

        if (chunk_start < line.size()) {
            push_chunk(chunk_start, line.size());
        }
        if (chunks.empty()) {
            push_chunk(0, 0);
        }
        return chunks;
    };

    for (std::size_t row = 0; row < lines_.size(); ++row) {
        auto chunks = wrap_row(row);
        for (auto& chunk : chunks) {
            layout.push_back(std::move(chunk));
        }
    }

    if (layout.empty()) {
        layout.push_back(VisualLine{0, 0, 0, "", true, 0});
    }
    visual_layout_cache_ = VisualLayoutCache{visual_layout_revision_, wrap_width,
                                             cursor_row_, cursor_column_, 0U,
                                             std::move(layout)};
    update_cursor(*visual_layout_cache_);
    return visual_layout_cache_->layout;
}

std::size_t Editor::current_visual_line_index(
    const std::vector<VisualLine>& layout) const {
    if (layout.empty()) {
        return 0;
    }

    if (visual_layout_cache_ && &layout == &visual_layout_cache_->layout &&
        visual_layout_cache_->cursor_row == cursor_row_ &&
        visual_layout_cache_->cursor_column == cursor_column_) {
        return visual_layout_cache_->cursor_visual_index;
    }

    for (std::size_t index = 0; index < layout.size(); ++index) {
        if (layout[index].has_cursor) {
            return index;
        }
    }
    return layout.size() - 1U;
}

bool Editor::is_on_first_visual_line() const {
    const auto& layout = build_visual_lines(last_layout_width_);
    return current_visual_line_index(layout) == 0U;
}

bool Editor::is_on_last_visual_line() const {
    const auto& layout = build_visual_lines(last_layout_width_);
    return current_visual_line_index(layout) + 1U >= layout.size();
}

void Editor::move_visual(int direction) {
    const auto& layout = build_visual_lines(last_layout_width_);
    if (layout.empty()) {
        return;
    }

    const std::size_t current_index = current_visual_line_index(layout);
    if (direction < 0 && current_index == 0U) {
        return;
    }
    if (direction > 0 && current_index + 1U >= layout.size()) {
        return;
    }

    const auto& current = layout[current_index];
    const std::size_t target_index =
        direction < 0 ? current_index - 1U : current_index + 1U;
    const auto& target = layout[target_index];

    if (!preferred_display_column_) {
        const std::size_t current_offset =
            cursor_column_ > current.start_column
                ? std::min(cursor_column_ - current.start_column, current.text.size())
                : 0U;
        preferred_display_column_ = display_column_for_byte(current.text, current_offset);
    }

    const bool target_is_last_for_logical_line =
        target_index + 1U >= layout.size() ||
        layout[target_index + 1U].logical_row != target.logical_row;
    std::size_t target_offset =
        byte_column_for_display(target.text, *preferred_display_column_);
    if (!target_is_last_for_logical_line && !target.text.empty() &&
        target_offset >= target.text.size()) {
        target_offset = previous_grapheme(target.text, target.text.size());
    }

    cursor_row_ = target.logical_row;
    cursor_column_ =
        std::min(target.start_column + target_offset, target.end_column);
    if (const auto marker = paste_marker_containing(cursor_row_, cursor_column_)) {
        cursor_column_ = marker->first;
    }
}

std::optional<std::pair<std::size_t, std::size_t>>
Editor::valid_paste_marker_at(std::size_t row, std::size_t column) const {
    if (row >= lines_.size() || column >= lines_[row].size()) {
        return std::nullopt;
    }

    for (const auto& paste : pastes_) {
        const auto& marker = paste.marker;
        if (column + marker.size() <= lines_[row].size() &&
            lines_[row].compare(column, marker.size(), marker) == 0) {
            return std::make_pair(column, column + marker.size());
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>>
Editor::valid_paste_marker_ending_at(std::size_t row, std::size_t column) const {
    if (row >= lines_.size() || column == 0 || column > lines_[row].size()) {
        return std::nullopt;
    }

    for (const auto& paste : pastes_) {
        const auto& marker = paste.marker;
        if (marker.size() <= column) {
            const std::size_t start = column - marker.size();
            if (lines_[row].compare(start, marker.size(), marker) == 0) {
                return std::make_pair(start, column);
            }
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>>
Editor::paste_marker_containing(std::size_t row, std::size_t column) const {
    if (row >= lines_.size() || column >= lines_[row].size()) {
        return std::nullopt;
    }

    for (const auto& paste : pastes_) {
        const auto& marker = paste.marker;
        std::size_t start = lines_[row].find(marker);
        while (start != std::string::npos) {
            const std::size_t end = start + marker.size();
            if (column > start && column < end) {
                return std::make_pair(start, end);
            }
            start = lines_[row].find(marker, start + 1U);
        }
    }

    return std::nullopt;
}

void Editor::reset_preferred_column() noexcept { preferred_display_column_.reset(); }

void Editor::erase_backward() {
    reset_preferred_column();
    if (cursor_column_ > 0) {
        capture_undo_state();
        exit_history_browse();
        const auto marker = valid_paste_marker_ending_at(cursor_row_, cursor_column_);
        const std::size_t previous =
            marker ? marker->first
                   : previous_grapheme_boundary(cursor_row_, cursor_column_);
        lines_[cursor_row_].erase(previous, cursor_column_ - previous);
        cursor_column_ = previous;
        notify_change();
        return;
    }

    if (cursor_row_ == 0) {
        return;
    }

    exit_history_browse();
    capture_undo_state();
    const std::size_t prior_size = lines_[cursor_row_ - 1].size();
    lines_[cursor_row_ - 1] += lines_[cursor_row_];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_));
    --cursor_row_;
    cursor_column_ = prior_size;
    notify_change();
}

void Editor::erase_forward() {
    reset_preferred_column();
    if (cursor_column_ < lines_[cursor_row_].size()) {
        capture_undo_state();
        exit_history_browse();
        last_action_was_yank_ = false;
        const auto marker = valid_paste_marker_at(cursor_row_, cursor_column_);
        const std::size_t next =
            marker ? marker->second
                   : next_grapheme_boundary(cursor_row_, cursor_column_);
        kill_ring_.push(lines_[cursor_row_].substr(cursor_column_, next - cursor_column_),
                        KillRingPushOptions{false, last_action_was_kill_});
        last_action_was_kill_ = true;
        lines_[cursor_row_].erase(cursor_column_, next - cursor_column_);
        notify_change();
        return;
    }

    if (cursor_row_ + 1 >= lines_.size()) {
        return;
    }

    exit_history_browse();
    capture_undo_state();
    last_action_was_yank_ = false;
    kill_ring_.push("\n", KillRingPushOptions{false, last_action_was_kill_});
    last_action_was_kill_ = true;
    lines_[cursor_row_] += lines_[cursor_row_ + 1];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_ + 1));
    notify_change();
}

void Editor::erase_word_backward() {
    reset_preferred_column();
    if (cursor_column_ == 0) {
        erase_backward();
        return;
    }

    const std::size_t start = previous_word_boundary(cursor_row_, cursor_column_);
    if (start == cursor_column_) {
        return;
    }

    exit_history_browse();
    capture_undo_state();
    last_action_was_yank_ = false;
    kill_ring_.push(lines_[cursor_row_].substr(start, cursor_column_ - start),
                    KillRingPushOptions{true, last_action_was_kill_});
    last_action_was_kill_ = true;
    lines_[cursor_row_].erase(start, cursor_column_ - start);
    cursor_column_ = start;
    notify_change();
}

void Editor::erase_word_forward() {
    reset_preferred_column();
    if (cursor_column_ >= lines_[cursor_row_].size()) {
        erase_forward();
        return;
    }

    const std::size_t end = next_word_boundary(cursor_row_, cursor_column_);
    if (end == cursor_column_) {
        return;
    }

    exit_history_browse();
    capture_undo_state();
    last_action_was_yank_ = false;
    kill_ring_.push(lines_[cursor_row_].substr(cursor_column_, end - cursor_column_),
                    KillRingPushOptions{false, last_action_was_kill_});
    last_action_was_kill_ = true;
    lines_[cursor_row_].erase(cursor_column_, end - cursor_column_);
    notify_change();
}

void Editor::erase_to_line_start() {
    reset_preferred_column();
    if (cursor_column_ == 0) {
        if (cursor_row_ == 0) {
            return;
        }

        exit_history_browse();
        capture_undo_state();
        last_action_was_yank_ = false;
        kill_ring_.push("\n", KillRingPushOptions{true, last_action_was_kill_});
        last_action_was_kill_ = true;
        const std::size_t previous_size = lines_[cursor_row_ - 1].size();
        lines_[cursor_row_ - 1] += lines_[cursor_row_];
        lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_));
        --cursor_row_;
        cursor_column_ = previous_size;
        notify_change();
        return;
    }

    exit_history_browse();
    capture_undo_state();
    last_action_was_yank_ = false;
    kill_ring_.push(lines_[cursor_row_].substr(0, cursor_column_),
                    KillRingPushOptions{true, last_action_was_kill_});
    last_action_was_kill_ = true;
    lines_[cursor_row_].erase(0, cursor_column_);
    cursor_column_ = 0;
    notify_change();
}

void Editor::erase_to_line_end() {
    reset_preferred_column();
    if (cursor_column_ < lines_[cursor_row_].size()) {
        exit_history_browse();
        capture_undo_state();
        last_action_was_yank_ = false;
        kill_ring_.push(lines_[cursor_row_].substr(cursor_column_),
                        KillRingPushOptions{false, last_action_was_kill_});
        last_action_was_kill_ = true;
        lines_[cursor_row_].erase(cursor_column_);
        notify_change();
        return;
    }

    if (cursor_row_ + 1 >= lines_.size()) {
        return;
    }

    exit_history_browse();
    capture_undo_state();
    last_action_was_yank_ = false;
    kill_ring_.push("\n", KillRingPushOptions{false, last_action_was_kill_});
    last_action_was_kill_ = true;
    lines_[cursor_row_] += lines_[cursor_row_ + 1];
    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_ + 1));
    notify_change();
}

void Editor::yank() {
    reset_kill_action();
    const auto text = kill_ring_.peek();
    if (!text) {
        return;
    }

    insert_text(*text);
    last_action_was_yank_ = true;
}

void Editor::yank_pop() {
    if (!last_action_was_yank_ || kill_ring_.size() <= 1U) {
        return;
    }

    const auto current = kill_ring_.peek();
    if (!current) {
        return;
    }

    delete_previous_yank(*current);
    kill_ring_.rotate();
    const auto replacement = kill_ring_.peek();
    if (!replacement) {
        last_action_was_yank_ = false;
        return;
    }

    insert_text(*replacement);
    last_action_was_kill_ = false;
    last_action_was_yank_ = true;
}

void Editor::delete_previous_yank(std::string_view yanked_text) {
    const auto yank_lines = split_lines(yanked_text);
    if (yank_lines.size() == 1U) {
        const std::size_t length = yank_lines.front().size();
        if (cursor_column_ < length) {
            return;
        }
        const std::size_t start = cursor_column_ - length;
        lines_[cursor_row_].erase(start, length);
        cursor_column_ = start;
        notify_change();
        return;
    }

    const std::size_t line_count = yank_lines.size();
    if (line_count == 0 || cursor_row_ + 1 < line_count) {
        return;
    }

    const std::size_t start_row = cursor_row_ - (line_count - 1U);
    if (start_row >= lines_.size()) {
        return;
    }

    const std::size_t first_line_length = yank_lines.front().size();
    if (lines_[start_row].size() < first_line_length) {
        return;
    }
    const std::size_t start_col = lines_[start_row].size() - first_line_length;
    const std::string before = lines_[start_row].substr(0, start_col);
    const std::string after = lines_[cursor_row_].substr(cursor_column_);

    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(start_row),
                 lines_.begin() + static_cast<std::ptrdiff_t>(cursor_row_ + 1U));
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(start_row),
                  before + after);
    cursor_row_ = start_row;
    cursor_column_ = start_col;
    notify_change();
}

bool Editor::replace_trailing_backslash_with_newline() {
    if (cursor_column_ == 0 || lines_[cursor_row_][cursor_column_ - 1] != '\\') {
        return false;
    }

    capture_undo_state();
    exit_history_browse();
    lines_[cursor_row_].erase(cursor_column_ - 1, 1);
    --cursor_column_;
    insert_newline_impl(false);
    return true;
}

bool Editor::is_empty() const noexcept {
    return lines_.size() == 1U && lines_.front().empty();
}

bool Editor::navigate_history(int direction) {
    if (history_.empty()) {
        return false;
    }

    const int next_index = history_index_ - direction;
    if (next_index < -1 || next_index >= static_cast<int>(history_.size())) {
        return false;
    }

    if (history_index_ == -1 && next_index >= 0) {
        capture_undo_state();
        history_draft_ = Snapshot{lines_, cursor_row_, cursor_column_, pastes_,
                                  paste_counter_};
    }
    history_index_ = next_index;
    if (history_index_ == -1) {
        if (history_draft_) {
            auto draft = std::move(*history_draft_);
            history_draft_.reset();
            replace_contents(std::move(draft.lines), draft.cursor_row,
                             draft.cursor_column, false);
            pastes_ = std::move(draft.pastes);
            paste_counter_ = draft.paste_counter;
            notify_change();
        } else {
            replace_contents({""}, 0, 0, true);
        }
    } else {
        auto lines = split_lines(history_[static_cast<std::size_t>(history_index_)]);
        const std::size_t row = lines.size() - 1;
        const std::size_t column = lines[row].size();
        replace_contents(std::move(lines), row, column, true);
    }
    return true;
}

void Editor::exit_history_browse() noexcept {
    history_index_ = -1;
    history_draft_.reset();
}

void Editor::reset_kill_action() noexcept {
    last_action_was_kill_ = false;
    last_action_was_yank_ = false;
}

void Editor::invalidate_visual_layout() const noexcept {
    ++visual_layout_revision_;
    visual_layout_cache_.reset();
}

void Editor::notify_change() {
    invalidate_visual_layout();
    if (on_change_observer_) {
        on_change_observer_();
    }
    if (on_change_) {
        on_change_(text());
    }
}

std::size_t Editor::undo_snapshot_bytes() const noexcept {
    std::size_t bytes = sizeof(Snapshot);
    for (const auto& line : lines_) {
        bytes += line.size() + sizeof(std::string);
    }
    for (const auto& paste : pastes_) {
        bytes += sizeof(Paste) + paste.marker.size();
    }
    return bytes;
}

std::vector<std::string> Editor::rendered_lines(std::size_t width) const {
    if (width == 0) {
        return {};
    }

    const std::size_t max_padding = (width > 1U) ? (width - 1U) / 2U : 0U;
    const std::size_t padding_x = std::min(padding_x_, max_padding);
    const std::size_t content_width =
        std::max<std::size_t>(1U, width > padding_x * 2U ? width - padding_x * 2U : 1U);
    const std::size_t layout_width =
        std::max<std::size_t>(1U, content_width - (padding_x == 0 ? 1U : 0U));
    last_layout_width_ = layout_width;

    const auto& layout = build_visual_lines(layout_width);
    const std::size_t cursor_visual_line = current_visual_line_index(layout);

    const std::size_t visible_limit = std::max<std::size_t>(1, max_visible_lines_);
    const std::size_t start =
        layout.size() <= visible_limit
            ? 0
            : std::min(cursor_visual_line > visible_limit / 2
                           ? cursor_visual_line - visible_limit / 2
                           : 0,
                       layout.size() - visible_limit);
    const std::size_t end = std::min(layout.size(), start + visible_limit);

    std::vector<std::string> rendered;
    rendered.reserve((end - start) + 2U);
    const auto horizontal = std::string("\u2500");
    auto border = [&](std::string text) {
        while (measure_line_width(text).value_or(text.size()) < width) {
            text += horizontal;
        }
        auto clamped = clamp_line(text, width);
        return clamped ? clamped.value() : text;
    };
    auto repeat_border = [&]() {
        std::string line;
        line.reserve(width * horizontal.size());
        for (std::size_t index = 0; index < width; ++index) {
            line += horizontal;
        }
        return line;
    };

    if (start > 0) {
        std::string indicator = "\u2500\u2500\u2500 \u2191 " + std::to_string(start) + " more ";
        rendered.push_back(border(std::move(indicator)));
    } else {
        rendered.push_back(repeat_border());
    }

    const std::string left_padding(padding_x, ' ');
    const std::string right_padding(padding_x, ' ');
    for (std::size_t index = start; index < end; ++index) {
        std::string display = layout[index].text;
        if (focused_ && layout[index].has_cursor) {
            const std::size_t cursor =
                std::min(layout[index].cursor_column, display.size());
            const std::string before = display.substr(0, cursor);
            const std::string after = display.substr(cursor);
            const std::size_t logical_cursor = layout[index].start_column + cursor;
            const auto marker =
                valid_paste_marker_at(layout[index].logical_row, logical_cursor);
            auto decorate_cursor = [&](std::string_view glyph,
                                       std::string_view suffix) {
                std::string decorated = before;
                decorated += cursor_marker;
                decorated += "\x1b[7m";
                decorated += glyph;
                decorated += "\x1b[0m";
                decorated += suffix;
                display = std::move(decorated);
            };
            if (marker && marker->second <= layout[index].end_column) {
                const std::size_t marker_size = marker->second - marker->first;
                decorate_cursor(after.substr(0, marker_size), after.substr(marker_size));
            } else if (!after.empty()) {
                const std::size_t next = next_grapheme(after, 0);
                decorate_cursor(after.substr(0, next), after.substr(next));
            } else {
                decorate_cursor(" ", "");
            }
        }

        std::string line = left_padding + display;
        auto measured = measure_line_width(line);
        const std::size_t line_width = measured ? measured.value() : line.size();
        if (line_width < content_width + padding_x) {
            line.append(content_width + padding_x - line_width, ' ');
        }
        line += right_padding;
        rendered.push_back(std::move(line));
    }

    const std::size_t lines_below = layout.size() - end;
    if (lines_below > 0) {
        std::string indicator =
            "\u2500\u2500\u2500 \u2193 " + std::to_string(lines_below) + " more ";
        rendered.push_back(border(std::move(indicator)));
    } else {
        rendered.push_back(repeat_border());
    }

    return rendered;
}

}  // namespace qt::tui
