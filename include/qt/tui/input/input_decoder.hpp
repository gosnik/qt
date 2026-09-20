#pragma once

#include "qt/tui/input/keys.hpp"
#include "qt/tui/input/stdin_buffer.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

enum class InputEventKind {
    insert,
    key,
    resize,
    scroll,
    ime_composition,
    raw,
};

enum class ImeCompositionPhase {
    start,
    update,
    commit,
    cancel,
};

struct ScrollEvent {
    int delta_lines = 0;
    std::size_t column = 0;
    std::size_t row = 0;
};

struct ImeCompositionEvent {
    ImeCompositionPhase phase = ImeCompositionPhase::update;
    std::string text;
};

struct InputEvent {
    InputEventKind kind = InputEventKind::raw;
    std::string bytes;
    std::string text;
    std::optional<KeyId> key;
    std::optional<TerminalSize> resize;
    std::optional<ScrollEvent> scroll;
    std::optional<ImeCompositionEvent> ime;
};

class InputDecoder {
  public:
    using KittyKeyboardResponseHandler = std::function<void()>;

    void set_kitty_keyboard_response_handler(
        KittyKeyboardResponseHandler handler);

    void push(char byte);
    void push(std::string_view bytes);

    [[nodiscard]] std::vector<InputEvent> drain();
    void flush();
    bool flush_expired();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string_view pending_bytes() const noexcept;

  private:
    void decode_available_sequences();
    void decode_sequence(const InputSequence& sequence);
    void emit_pending_text();
    void emit_key_event(std::string bytes, KeyId key);
    void emit_resize_event(std::string bytes, TerminalSize size);
    void emit_scroll_event(std::string bytes, ScrollEvent scroll);
    void emit_raw_event(std::string bytes);

    StdinBuffer buffer_;
    KittyKeyboardResponseHandler kitty_keyboard_response_handler_;
    std::string pending_text_;
    std::vector<InputEvent> completed_;
};

}  // namespace qt::tui
