#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace qt::tui {

enum class KeyTextCapitalization : std::uint8_t {
  preserve,
  capitalize_first,
};

[[nodiscard]] std::string format_key_text(
    std::string_view key,
    KeyTextCapitalization capitalization = KeyTextCapitalization::preserve);
[[nodiscard]] std::string key_display_text(std::string_view keybinding_name);
[[nodiscard]] std::string key_hint(std::string_view keybinding_name, std::string_view description);
[[nodiscard]] std::string raw_key_hint(std::string_view key, std::string_view description);

}  // namespace qt::tui
