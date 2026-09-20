#include "qt/tui/input/input_decoder.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {
namespace {

bool is_text_insertion(std::string_view bytes) {
    if (bytes.empty()) {
        return false;
    }

    for (const char byte : bytes) {
        const auto value = static_cast<std::uint8_t>(byte);
        if (value < 0x20U || value == 0x7fU) {
            return false;
        }
    }

    return true;
}

bool is_bracketed_paste(std::string_view bytes) {
    constexpr std::string_view begin = "\x1b[200~";
    constexpr std::string_view end = "\x1b[201~";
    return bytes.size() >= begin.size() + end.size() &&
           bytes.substr(0, begin.size()) == begin &&
           bytes.substr(bytes.size() - end.size()) == end;
}

std::string bracketed_paste_text(std::string_view bytes) {
    constexpr std::string_view begin = "\x1b[200~";
    constexpr std::string_view end = "\x1b[201~";
    return std::string(bytes.substr(begin.size(), bytes.size() - begin.size() - end.size()));
}

std::optional<int> parse_positive_int(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }

        value = value * 10 + (ch - '0');
    }
    return value;
}

std::vector<int> parse_semicolon_ints(std::string_view text) {
    std::vector<int> values;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(';', start);
        const auto part = end == std::string_view::npos ? text.substr(start)
                                                        : text.substr(start, end - start);
        const auto value = parse_positive_int(part);
        if (!value) {
            return {};
        }
        values.push_back(*value);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

std::optional<TerminalSize> parse_resize_report(std::string_view bytes) {
    constexpr std::string_view prefix = "\x1b[8;";
    if (bytes.size() < prefix.size() + 2 || bytes.substr(0, prefix.size()) != prefix ||
        bytes.back() != 't') {
        return std::nullopt;
    }

    const auto values =
        parse_semicolon_ints(bytes.substr(prefix.size(), bytes.size() - prefix.size() - 1));
    if (values.size() != 2 || values[0] <= 0 || values[1] <= 0) {
        return std::nullopt;
    }

    return TerminalSize{static_cast<std::size_t>(values[1]),
                        static_cast<std::size_t>(values[0])};
}

std::optional<CellDimensions> parse_cell_size_report(std::string_view bytes) {
    constexpr std::string_view prefix = "\x1b[6;";
    if (bytes.size() < prefix.size() + 2 || bytes.substr(0, prefix.size()) != prefix ||
        bytes.back() != 't') {
        return std::nullopt;
    }

    const auto values =
        parse_semicolon_ints(bytes.substr(prefix.size(), bytes.size() - prefix.size() - 1));
    if (values.size() != 2 || values[0] <= 0 || values[1] <= 0) {
        return std::nullopt;
    }

    return CellDimensions{static_cast<std::size_t>(values[1]),
                          static_cast<std::size_t>(values[0])};
}

std::optional<ScrollEvent> parse_sgr_mouse_scroll(std::string_view bytes) {
    constexpr std::string_view prefix = "\x1b[<";
    if (bytes.size() < prefix.size() + 2 || bytes.substr(0, prefix.size()) != prefix ||
        bytes.back() != 'M') {
        return std::nullopt;
    }

    const auto values =
        parse_semicolon_ints(bytes.substr(prefix.size(), bytes.size() - prefix.size() - 1));
    if (values.size() != 3 || values[1] <= 0 || values[2] <= 0) {
        return std::nullopt;
    }

    const int button = values[0];
    if (button != 64 && button != 65) {
        return std::nullopt;
    }

    return ScrollEvent{
        button == 64 ? 3 : -3,
        static_cast<std::size_t>(values[1]),
        static_cast<std::size_t>(values[2]),
    };
}

bool is_kitty_keyboard_protocol_response(std::string_view bytes) {
    constexpr std::string_view prefix = "\x1b[?";
    if (bytes.size() <= prefix.size() + 1 || bytes.substr(0, prefix.size()) != prefix ||
        bytes.back() != 'u') {
        return false;
    }

    for (const char ch : bytes.substr(prefix.size(), bytes.size() - prefix.size() - 1U)) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

}  // namespace

void InputDecoder::set_kitty_keyboard_response_handler(
    KittyKeyboardResponseHandler handler) {
    kitty_keyboard_response_handler_ = std::move(handler);
}

void InputDecoder::push(char byte) {
    buffer_.push(byte);
    decode_available_sequences();
}

void InputDecoder::push(std::string_view bytes) {
    buffer_.push(bytes);
    decode_available_sequences();
}

std::vector<InputEvent> InputDecoder::drain() {
    emit_pending_text();

    auto completed = std::move(completed_);
    completed_.clear();
    return completed;
}

void InputDecoder::flush() {
    buffer_.flush();
    decode_available_sequences();
}

bool InputDecoder::flush_expired() {
    const bool flushed = buffer_.flush_expired();
    if (flushed) {
        decode_available_sequences();
    }
    return flushed;
}

bool InputDecoder::empty() const noexcept {
    return buffer_.empty() && pending_text_.empty() && completed_.empty();
}

std::string_view InputDecoder::pending_bytes() const noexcept {
    return buffer_.pending_bytes();
}

void InputDecoder::decode_available_sequences() {
    const auto sequences = buffer_.drain();
    for (const auto& sequence : sequences) {
        decode_sequence(sequence);
    }
}

void InputDecoder::decode_sequence(const InputSequence& sequence) {
    if (sequence.kind == InputSequenceKind::escape && is_bracketed_paste(sequence.bytes)) {
        emit_pending_text();
        const auto text = bracketed_paste_text(sequence.bytes);
        InputEvent event;
        event.kind = InputEventKind::insert;
        event.bytes = sequence.bytes;
        event.text = text;
        completed_.push_back(std::move(event));
        return;
    }

    if (sequence.kind == InputSequenceKind::escape) {
        if (is_kitty_keyboard_protocol_response(sequence.bytes)) {
            emit_pending_text();
            if (kitty_keyboard_response_handler_) {
                kitty_keyboard_response_handler_();
            } else {
                set_kitty_protocol_active(true);
            }
            return;
        }

        if (const auto dimensions = parse_cell_size_report(sequence.bytes)) {
            emit_pending_text();
            set_terminal_cell_dimensions(*dimensions);
            return;
        }

        if (const auto size = parse_resize_report(sequence.bytes)) {
            emit_pending_text();
            emit_resize_event(sequence.bytes, *size);
            return;
        }

        if (const auto scroll = parse_sgr_mouse_scroll(sequence.bytes)) {
            emit_pending_text();
            emit_scroll_event(sequence.bytes, *scroll);
            return;
        }
    }

    if (sequence.kind == InputSequenceKind::text && is_text_insertion(sequence.bytes)) {
        pending_text_ += sequence.bytes;
        return;
    }

    emit_pending_text();

    if (const auto printable = decode_printable_key(sequence.bytes)) {
        InputEvent event;
        event.kind = InputEventKind::insert;
        event.bytes = sequence.bytes;
        event.text = *printable;
        completed_.push_back(std::move(event));
        return;
    }

    if (const auto key = parse_key(sequence.bytes)) {
        emit_key_event(sequence.bytes, *key);
        return;
    }

    emit_raw_event(sequence.bytes);
}

void InputDecoder::emit_pending_text() {
    if (pending_text_.empty()) {
        return;
    }

    InputEvent event;
    event.kind = InputEventKind::insert;
    event.bytes = pending_text_;
    event.text = pending_text_;
    completed_.push_back(std::move(event));
    pending_text_.clear();
}

void InputDecoder::emit_key_event(std::string bytes, KeyId key) {
    InputEvent event;
    event.kind = InputEventKind::key;
    event.bytes = std::move(bytes);
    event.key = std::move(key);
    completed_.push_back(std::move(event));
}

void InputDecoder::emit_resize_event(std::string bytes, TerminalSize size) {
    InputEvent event;
    event.kind = InputEventKind::resize;
    event.bytes = std::move(bytes);
    event.resize = size;
    completed_.push_back(std::move(event));
}

void InputDecoder::emit_scroll_event(std::string bytes, ScrollEvent scroll) {
    InputEvent event;
    event.kind = InputEventKind::scroll;
    event.bytes = std::move(bytes);
    event.scroll = scroll;
    completed_.push_back(std::move(event));
}

void InputDecoder::emit_raw_event(std::string bytes) {
    InputEvent event;
    event.kind = InputEventKind::raw;
    event.bytes = std::move(bytes);
    completed_.push_back(std::move(event));
}

}  // namespace qt::tui
