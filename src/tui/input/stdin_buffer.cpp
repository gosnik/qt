#include "qt/tui/input/stdin_buffer.hpp"

#include <cstdint>
#include <cctype>
#include <optional>
#include <utility>

namespace qt::tui {
namespace {

bool is_arrow_suffix(char value) {
    return value == 'A' || value == 'B' || value == 'C' || value == 'D';
}

bool is_ss3_final_byte(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool is_csi_final_byte(char value) {
    return value >= '@' && value <= '~';
}

std::optional<std::uint32_t> parse_unmodified_kitty_printable_codepoint(
    std::string_view bytes) {
    constexpr std::string_view prefix = "\x1b[";
    if (bytes.size() < prefix.size() + 2U || bytes.substr(0, prefix.size()) != prefix ||
        bytes.back() != 'u') {
        return std::nullopt;
    }

    std::uint32_t codepoint = 0;
    std::size_t offset = prefix.size();
    bool saw_digit = false;
    while (offset < bytes.size() && bytes[offset] >= '0' && bytes[offset] <= '9') {
        saw_digit = true;
        codepoint = codepoint * 10U + static_cast<std::uint32_t>(bytes[offset] - '0');
        ++offset;
    }
    if (!saw_digit || codepoint < 32U) {
        return std::nullopt;
    }

    if (offset == bytes.size() - 1U) {
        return codepoint;
    }

    std::size_t groups = 0;
    while (offset < bytes.size() - 1U) {
        if (bytes[offset] != ':' || groups >= 2U) {
            return std::nullopt;
        }
        ++groups;
        ++offset;
        while (offset < bytes.size() - 1U && bytes[offset] >= '0' && bytes[offset] <= '9') {
            ++offset;
        }
    }

    return codepoint;
}

std::optional<std::size_t> terminated_control_sequence_size(std::string_view pending) {
    if (pending.size() < 2U) {
        return std::nullopt;
    }
    const char second = pending[1];
    if (second != ']' && second != 'P' && second != '_') {
        return std::nullopt;
    }

    for (std::size_t index = 2; index < pending.size(); ++index) {
        if (pending[index] == '\x07') {
            return index + 1U;
        }
        if (pending[index] == '\x1b' && index + 1U < pending.size() &&
            pending[index + 1U] == '\\') {
            return index + 2U;
        }
    }
    return std::nullopt;
}

}  // namespace

void StdinBuffer::push(char byte) {
    pending_deadline_.reset();
    const unsigned char raw = static_cast<unsigned char>(byte);
    if (raw > 0x7fU) {
        pending_.push_back('\x1b');
        pending_.push_back(static_cast<char>(raw - 0x80U));
        parse_available_sequences();
        return;
    }

    pending_.push_back(byte);
    parse_available_sequences();
}

void StdinBuffer::push(std::string_view bytes) {
    if (bytes.size() == 1U) {
        push(bytes.front());
        return;
    }

    pending_deadline_.reset();
    if (unconsumed_bytes().empty() &&
        bytes.find('\x1b') == std::string_view::npos) {
        completed_.reserve(completed_.size() + bytes.size());
    }
    pending_.append(bytes.data(), bytes.size());
    parse_available_sequences();
}

std::vector<InputSequence> StdinBuffer::drain() {
    auto completed = std::move(completed_);
    completed_.clear();
    return completed;
}

void StdinBuffer::flush() {
    while (!unconsumed_bytes().empty()) {
        const auto pending = unconsumed_bytes();
        if (pending.front() == '\x1b') {
            emit_escape_sequence(std::string(1, pending.front()));
            consume_bytes(1U);
            parse_available_sequences();
            continue;
        }

        emit_text_byte(pending.front());
        consume_bytes(1U);
    }
    pending_deadline_.reset();
}

bool StdinBuffer::flush_expired(std::chrono::steady_clock::time_point now) {
    if (unconsumed_bytes().empty()) {
        pending_deadline_.reset();
        return false;
    }
    if (!pending_deadline_) {
        pending_deadline_ = now + kStdinBufferSequenceTimeout;
        return false;
    }
    if (now < *pending_deadline_) {
        return false;
    }
    flush();
    return true;
}

bool StdinBuffer::empty() const noexcept {
    return unconsumed_bytes().empty() && completed_.empty();
}

std::string_view StdinBuffer::pending_bytes() const noexcept {
    return unconsumed_bytes();
}

void StdinBuffer::parse_available_sequences() {
    const auto wait_for_more = [this]() { refresh_timeout(); };
    while (!unconsumed_bytes().empty()) {
        auto pending = unconsumed_bytes();
        if (pending.front() != '\x1b') {
            emit_text_byte(pending.front());
            consume_bytes(1U);
            continue;
        }

        if (pending.size() == 1U) {
            wait_for_more();
            return;
        }

        const char second = pending[1];
        if (second == ']' || second == 'P' || second == '_') {
            if (const auto sequence_size = terminated_control_sequence_size(pending)) {
                emit_escape_sequence(std::string(pending.substr(0, *sequence_size)));
                consume_bytes(*sequence_size);
                continue;
            }
            wait_for_more();
            return;
        }

        if (second == '[' || second == 'O') {
            if (pending.size() < 3U) {
                wait_for_more();
                return;
            }

            if (pending.rfind("\x1b[200~", 0) == 0U) {
                const auto end = pending.find("\x1b[201~", 6);
                if (end == std::string::npos) {
                    wait_for_more();
                    return;
                }

                const std::size_t sequence_size = end + 6;
                emit_escape_sequence(std::string(pending.substr(0, sequence_size)));
                consume_bytes(sequence_size);
                continue;
            }

            if (second == '[' && pending[2] == 'M') {
                constexpr std::size_t old_mouse_sequence_size = 6U;
                if (pending.size() < old_mouse_sequence_size) {
                    wait_for_more();
                    return;
                }
                emit_escape_sequence(
                    std::string(pending.substr(0, old_mouse_sequence_size)));
                consume_bytes(old_mouse_sequence_size);
                continue;
            }

            if (second == 'O' && is_ss3_final_byte(pending[2])) {
                emit_escape_sequence(std::string(pending.substr(0, 3)));
                consume_bytes(3U);
                continue;
            }

            if (is_arrow_suffix(pending[2])) {
                emit_escape_sequence(std::string(pending.substr(0, 3)));
                consume_bytes(3U);
                continue;
            }

            if (second == '[') {
                bool emitted = false;
                for (std::size_t index = 2; index < pending.size(); ++index) {
                    if (is_csi_final_byte(pending[index])) {
                        emit_escape_sequence(
                            std::string(pending.substr(0, index + 1U)));
                        consume_bytes(index + 1U);
                        emitted = true;
                        break;
                    }
                }
                if (!emitted) {
                    wait_for_more();
                    return;
                }
                continue;
            }
        }

        if (pending.size() >= 2U &&
            std::isprint(static_cast<unsigned char>(pending[1])) != 0) {
            emit_escape_sequence(std::string(pending.substr(0, 2)));
            consume_bytes(2U);
            continue;
        }

        if (pending.size() >= 2U &&
            static_cast<unsigned char>(pending[1]) <= 0x1fU) {
            emit_escape_sequence(std::string(pending.substr(0, 2)));
            consume_bytes(2U);
            continue;
        }

        emit_escape_sequence(std::string(1, pending.front()));
        consume_bytes(1U);
    }
    refresh_timeout();
}

void StdinBuffer::refresh_timeout() {
    if (unconsumed_bytes().empty()) {
        pending_deadline_.reset();
        return;
    }
    if (!pending_deadline_) {
        pending_deadline_ =
            std::chrono::steady_clock::now() + kStdinBufferSequenceTimeout;
    }
}

std::string_view StdinBuffer::unconsumed_bytes() const noexcept {
    return std::string_view(pending_).substr(pending_offset_);
}

void StdinBuffer::consume_bytes(std::size_t count) {
    pending_offset_ += count;
    if (pending_offset_ == pending_.size()) {
        pending_.clear();
        pending_offset_ = 0;
        return;
    }

    constexpr std::size_t kCompactionThreshold = 4096U;
    if (pending_offset_ >= kCompactionThreshold &&
        pending_offset_ >= pending_.size() / 2U) {
        pending_.erase(0, pending_offset_);
        pending_offset_ = 0;
    }
}

void StdinBuffer::emit_text_byte(char byte) {
    const auto codepoint = static_cast<std::uint32_t>(
        static_cast<unsigned char>(byte));
    if (pending_kitty_printable_codepoint_ &&
        *pending_kitty_printable_codepoint_ == codepoint) {
        pending_kitty_printable_codepoint_.reset();
        return;
    }
    pending_kitty_printable_codepoint_.reset();
    completed_.push_back(InputSequence{std::string(1, byte), InputSequenceKind::text});
}

void StdinBuffer::emit_escape_sequence(std::string bytes) {
    pending_kitty_printable_codepoint_ =
        parse_unmodified_kitty_printable_codepoint(bytes);
    completed_.push_back(InputSequence{std::move(bytes), InputSequenceKind::escape});
}

}  // namespace qt::tui
