#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

inline constexpr std::chrono::milliseconds kStdinBufferSequenceTimeout{10};

enum class InputSequenceKind {
    text,
    escape,
};

struct InputSequence {
    std::string bytes;
    InputSequenceKind kind = InputSequenceKind::text;
};

class StdinBuffer {
  public:
    void push(char byte);
    void push(std::string_view bytes);

    [[nodiscard]] std::vector<InputSequence> drain();
    void flush();
    bool flush_expired(std::chrono::steady_clock::time_point now =
                           std::chrono::steady_clock::now());

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string_view pending_bytes() const noexcept;

  private:
    void parse_available_sequences();
    void refresh_timeout();
    [[nodiscard]] std::string_view unconsumed_bytes() const noexcept;
    void consume_bytes(std::size_t count);
    void emit_text_byte(char byte);
    void emit_escape_sequence(std::string bytes);

    std::string pending_;
    std::size_t pending_offset_ = 0;
    std::vector<InputSequence> completed_;
    std::optional<std::uint32_t> pending_kitty_printable_codepoint_;
    std::optional<std::chrono::steady_clock::time_point> pending_deadline_;
};

}  // namespace qt::tui
