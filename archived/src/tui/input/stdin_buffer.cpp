#include "qt/tui/input/stdin_buffer.hpp"

#include <algorithm>
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

std::optional<std::size_t> terminated_control_sequence_size(std::string_view pending,
                                                            std::size_t start) {
    if (pending.size() < 2U) {
        return std::nullopt;
    }
    const char second = pending[1];
    if (second != ']' && second != 'P' && second != '_') {
        return std::nullopt;
    }

    for (std::size_t index = std::max<std::size_t>(2U, start); index < pending.size(); ++index) {
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

std::size_t utf8_sequence_size(std::string_view bytes) {
    if (bytes.empty()) {
        return 0U;
    }
    const auto first = static_cast<unsigned char>(bytes.front());
    if (first < 0x80U) {
        return 1U;
    }
    std::size_t size = 0U;
    std::uint32_t codepoint = 0U;
    std::uint32_t minimum = 0U;
    if (first >= 0xc2U && first <= 0xdfU) {
        size = 2U; codepoint = first & 0x1fU; minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        size = 3U; codepoint = first & 0x0fU; minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        size = 4U; codepoint = first & 0x07U; minimum = 0x10000U;
    } else {
        return 1U;
    }
    if (bytes.size() < size) {
        return 0U;
    }
    for (std::size_t index = 1U; index < size; ++index) {
        const auto byte = static_cast<unsigned char>(bytes[index]);
        if ((byte & 0xc0U) != 0x80U) {
            return 1U;
        }
        codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }
    return (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU)) ? 1U : size;
}

std::optional<std::uint32_t> utf8_codepoint(std::string_view bytes) {
    const std::size_t size = utf8_sequence_size(bytes);
    if (size == 0U || size != bytes.size()) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(bytes.front());
    if (size == 1U) {
        return static_cast<std::uint32_t>(first);
    }
    std::uint32_t codepoint = size == 2U ? first & 0x1fU
                              : size == 3U ? first & 0x0fU
                                            : first & 0x07U;
    for (std::size_t index = 1U; index < size; ++index) {
        codepoint = (codepoint << 6U) |
                    (static_cast<unsigned char>(bytes[index]) & 0x3fU);
    }
    return codepoint;
}

}  // namespace

void StdinBuffer::push(char byte) {
    pending_deadline_.reset();
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

        const std::size_t size = utf8_sequence_size(pending);
        if (size == 0U) {
            emit_text(pending.substr(0U, 1U));
            consume_bytes(1U);
            continue;
        }
        emit_text(pending.substr(0U, size));
        consume_bytes(size);
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
            const std::size_t size = utf8_sequence_size(pending);
            if (size == 0U) {
                wait_for_more();
                return;
            }
            emit_text(pending.substr(0U, size));
            consume_bytes(size);
            continue;
        }

        if (pending.size() == 1U) {
            wait_for_more();
            return;
        }

        const char second = pending[1];
        if (second == ']' || second == 'P' || second == '_') {
            if (const auto sequence_size =
                    terminated_control_sequence_size(pending, scan_offset_)) {
                emit_escape_sequence(std::string(pending.substr(0, *sequence_size)));
                consume_bytes(*sequence_size);
                continue;
            }
            scan_offset_ = pending.empty() ? 0U : pending.size() - 1U;
            wait_for_more();
            return;
        }

        if (second == '[' || second == 'O') {
            if (pending.size() < 3U) {
                wait_for_more();
                return;
            }

            if (pending.rfind("\x1b[200~", 0) == 0U) {
                const auto end = pending.find(
                    "\x1b[201~", std::max<std::size_t>(6U,
                        scan_offset_ > 5U ? scan_offset_ - 5U : 0U));
                if (end == std::string::npos) {
                    scan_offset_ = pending.size();
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
                for (std::size_t index = std::max<std::size_t>(2U, scan_offset_);
                     index < pending.size(); ++index) {
                    if (is_csi_final_byte(pending[index])) {
                        emit_escape_sequence(
                            std::string(pending.substr(0, index + 1U)));
                        consume_bytes(index + 1U);
                        emitted = true;
                        break;
                    }
                }
                if (!emitted) {
                    scan_offset_ = pending.size();
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
    scan_offset_ = 0U;
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

void StdinBuffer::emit_text(std::string_view bytes) {
    const auto codepoint = utf8_codepoint(bytes);
    if (pending_kitty_printable_codepoint_ &&
        codepoint && *pending_kitty_printable_codepoint_ == *codepoint) {
        pending_kitty_printable_codepoint_.reset();
        return;
    }
    pending_kitty_printable_codepoint_.reset();
    completed_.push_back(InputSequence{std::string(bytes), InputSequenceKind::text});
}

void StdinBuffer::emit_escape_sequence(std::string bytes) {
    pending_kitty_printable_codepoint_ =
        parse_unmodified_kitty_printable_codepoint(bytes);
    completed_.push_back(InputSequence{std::move(bytes), InputSequenceKind::escape});
}

}  // namespace qt::tui
