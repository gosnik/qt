#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace qt::tui {

using KeyId = std::string;

void set_kitty_protocol_active(bool active) noexcept;
[[nodiscard]] bool is_kitty_protocol_active() noexcept;

std::optional<KeyId> parse_key(std::string_view input);
std::optional<std::string> decode_printable_key(std::string_view input);
std::optional<KeyId> key_id_from_codepoint(std::uint32_t codepoint,
                                           unsigned modifiers = 0U);
std::optional<std::string> input_bytes_for_key_id(std::string_view key_id);
[[nodiscard]] bool is_key_release(std::string_view input) noexcept;
[[nodiscard]] bool is_key_repeat(std::string_view input) noexcept;
bool matches_key(std::string_view input, std::string_view key_id);

}  // namespace qt::tui
