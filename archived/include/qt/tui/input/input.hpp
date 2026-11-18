#pragma once

#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input_decoder.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/input/kill_ring.hpp"
#include "qt/tui/input/undo_stack.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

class Input final : public Component, public Focusable {
  public:
    using SubmitCallback = std::function<void(const std::string&)>;
    using ChangeCallback = std::function<void(const std::string&)>;
    using EscapeCallback = std::function<void()>;

    Input();
    explicit Input(KeybindingManager keybindings);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void handle_input(std::string_view bytes) override;
    void flush_input();
    void handle_event(const InputEvent& event);
    void handle_events(const std::vector<InputEvent>& events);

    void set_value(std::string value);
    [[nodiscard]] std::string value() const;
    [[nodiscard]] std::size_t cursor_column() const noexcept;

    void set_submit_callback(SubmitCallback callback);
    void set_change_callback(ChangeCallback callback);
    void set_escape_callback(EscapeCallback callback);
    // Defaults to a 1 MiB retained-snapshot budget. Zero disables eviction;
    // an oversized snapshot is not retained.
    void set_undo_byte_limit(std::size_t max_bytes);
    [[nodiscard]] std::size_t undo_byte_limit() const noexcept;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const noexcept override;

  private:
    struct State {
        std::string value;
        std::size_t cursor = 0;
    };

    enum class LastAction { none, kill, yank, type_word };

    void push_undo();
    void undo();
    void insert_text(std::string_view text);
    void insert_text_without_undo(std::string_view text);
    void erase_backward();
    void erase_forward();
    void erase_word_backward();
    void erase_word_forward();
    void erase_to_start();
    void erase_to_end();
    void move_left();
    void move_right();
    void move_word_left();
    void move_word_right();
    void move_start();
    void move_end();
    void notify_change();
    [[nodiscard]] std::size_t undo_state_bytes() const noexcept;
    [[nodiscard]] const std::vector<std::size_t>& grapheme_boundaries() const;

    [[nodiscard]] std::string rendered_line(std::size_t width) const;

    KeybindingManager keybindings_;
    InputDecoder decoder_;
    KillRing kill_ring_;
    UndoStack<State> undo_stack_;
    std::string value_;
    std::string last_yank_;
    std::size_t cursor_ = 0;
    mutable std::vector<std::size_t> grapheme_boundaries_;
    mutable bool grapheme_boundaries_valid_ = false;
    LastAction last_action_ = LastAction::none;
    bool focused_ = false;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    EscapeCallback on_escape_;
};

}  // namespace qt::tui
