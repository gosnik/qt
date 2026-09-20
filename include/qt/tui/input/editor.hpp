#pragma once

#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input_decoder.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/input/kill_ring.hpp"
#include "qt/tui/input/undo_stack.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {

enum class WordSegmentKind : std::uint8_t {
    word,
    punctuation,
    whitespace,
    marker,
};

struct WordSegment {
    std::size_t begin = 0;
    std::size_t end = 0;
    WordSegmentKind kind = WordSegmentKind::word;
};

class Editor final : public Component, public Focusable {
  public:
    using SubmitCallback = std::function<void(const std::string&)>;
    using ChangeCallback = std::function<void(const std::string&)>;
    using PasteImageCallback = std::function<std::optional<std::string>()>;

    Editor();
    explicit Editor(KeybindingManager keybindings);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void handle_input(std::string_view bytes) override;
    void flush_input();
    void handle_event(const InputEvent& event);
    void handle_events(const std::vector<InputEvent>& events);

    void set_text(std::string_view text);
    void set_contents(std::vector<std::string> lines, std::size_t row,
                      std::size_t column);
    void apply_completion_contents(std::vector<std::string> lines, std::size_t row,
                                   std::size_t column);
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string expanded_text() const;
    void add_to_history(std::string_view text);
    void set_history_limit(std::size_t max_entries);
    void set_history(std::vector<std::string> history);
    [[nodiscard]] const std::vector<std::string>& history() const noexcept;
    [[nodiscard]] bool browsing_history() const noexcept;

    void set_submit_callback(SubmitCallback callback);
    void set_change_callback(ChangeCallback callback);
    void set_paste_image_callback(PasteImageCallback callback);
    void set_padding_x(std::size_t padding_x) noexcept;
    [[nodiscard]] std::size_t padding_x() const noexcept;
    void set_max_visible_lines(std::size_t max_visible_lines) noexcept;
    [[nodiscard]] std::size_t max_visible_lines() const noexcept;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const noexcept override;

    [[nodiscard]] std::size_t cursor_row() const noexcept;
    [[nodiscard]] std::size_t cursor_column() const noexcept;

    void insert_newline();
    bool submit();

  private:
    void replace_contents(std::vector<std::string> lines, std::size_t row, std::size_t column,
                          bool notify);
    void capture_undo_state();
    void undo();
    void insert_event_text(const InputEvent& event);
    void insert_text(std::string_view text);
    void insert_text_impl(std::string_view text, bool record_undo);
    bool insert_large_paste_marker(std::string_view text);
    void insert_newline_impl(bool record_undo);
    void move_left();
    void move_right();
    void move_up();
    void move_down();
    void page_move(int direction);
    void move_line_start();
    void move_line_end();
    [[nodiscard]] const std::vector<WordSegment>& word_segments_for_row(
        std::size_t row) const;
    [[nodiscard]] std::size_t previous_word_boundary(std::size_t row,
                                                     std::size_t column) const;
    [[nodiscard]] std::size_t next_word_boundary(std::size_t row,
                                                 std::size_t column) const;
    struct VisualLine {
        std::size_t logical_row = 0;
        std::size_t start_column = 0;
        std::size_t end_column = 0;
        std::string text;
        bool has_cursor = false;
        std::size_t cursor_column = 0;
    };
    struct VisualLayoutCache {
        std::size_t revision = 0;
        std::size_t width = 0;
        std::size_t cursor_row = 0;
        std::size_t cursor_column = 0;
        std::size_t cursor_visual_index = 0;
        std::vector<VisualLine> layout;
    };
    struct GraphemeBoundaryCacheEntry {
        bool valid = false;
        std::size_t revision = 0;
        std::vector<std::size_t> boundaries;
    };
    struct WordSegmentCacheEntry {
        bool valid = false;
        std::string line;
        std::vector<WordSegment> segments;
    };
    [[nodiscard]] const std::vector<std::size_t>& grapheme_boundaries_for_row(
        std::size_t row) const;
    [[nodiscard]] std::size_t previous_grapheme_boundary(
        std::size_t row, std::size_t column) const;
    [[nodiscard]] std::size_t next_grapheme_boundary(
        std::size_t row, std::size_t column) const;
    [[nodiscard]] const std::vector<VisualLine>& build_visual_lines(
        std::size_t width) const;
    [[nodiscard]] std::size_t current_visual_line_index(
        const std::vector<VisualLine>& layout) const;
    [[nodiscard]] bool is_on_first_visual_line() const;
    [[nodiscard]] bool is_on_last_visual_line() const;
    void move_visual(int direction);
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
    valid_paste_marker_at(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
    valid_paste_marker_ending_at(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
    paste_marker_containing(std::size_t row, std::size_t column) const;
    void reset_preferred_column() noexcept;
    void erase_backward();
    void erase_forward();
    void erase_word_backward();
    void erase_word_forward();
    void erase_to_line_start();
    void erase_to_line_end();
    void yank();
    void yank_pop();
    void delete_previous_yank(std::string_view yanked_text);
    enum class JumpDirection : std::uint8_t { forward, backward };
    bool handle_pending_jump(const InputEvent& event);
    void jump_to_character(std::string_view character, JumpDirection direction);
    bool replace_trailing_backslash_with_newline();
    bool is_empty() const noexcept;
    bool navigate_history(int direction);
    void exit_history_browse() noexcept;
    void reset_kill_action() noexcept;
    void invalidate_visual_layout() const noexcept;
    void notify_change();

    [[nodiscard]] std::vector<std::string> rendered_lines(std::size_t width) const;
    struct Snapshot {
        std::vector<std::string> lines;
        std::size_t cursor_row = 0;
        std::size_t cursor_column = 0;
        std::vector<std::pair<std::size_t, std::string>> pastes;
        std::size_t paste_counter = 0;
    };

    KeybindingManager keybindings_;
    InputDecoder decoder_;
    std::vector<std::string> lines_;
    KillRing kill_ring_;
    UndoStack<Snapshot> undo_stack_;
    std::vector<std::pair<std::size_t, std::string>> pastes_;
    std::vector<std::string> history_;
    std::size_t history_limit_ = 500U;
    std::optional<Snapshot> history_draft_;
    int history_index_ = -1;
    bool last_action_was_kill_ = false;
    bool last_action_was_yank_ = false;
    std::optional<JumpDirection> pending_jump_;
    std::size_t cursor_row_ = 0;
    std::size_t cursor_column_ = 0;
    std::size_t paste_counter_ = 0;
    std::size_t padding_x_ = 0;
    std::size_t max_visible_lines_ = 5;
    std::optional<std::size_t> preferred_display_column_;
    mutable std::size_t last_layout_width_ = 80;
    mutable std::size_t visual_layout_revision_ = 0;
    mutable std::optional<VisualLayoutCache> visual_layout_cache_;
    mutable std::vector<GraphemeBoundaryCacheEntry> grapheme_boundary_cache_;
    mutable std::vector<WordSegmentCacheEntry> word_segment_cache_;
    bool focused_ = false;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    PasteImageCallback on_paste_image_;
};

}  // namespace qt::tui
