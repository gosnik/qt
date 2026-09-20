#include "qt/tui/input/keybinding_hints.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/input/keys.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {
namespace {

std::vector<std::string> split(std::string_view text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(delimiter, start);
        if (end == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

std::string format_key_part(std::string_view part,
                            KeyTextCapitalization capitalization) {
    std::string display(part);
    // macOS: alt -> option
#ifdef __APPLE__
    if (display == "alt") {
        display = "option";
    }
#endif
    if (capitalization == KeyTextCapitalization::capitalize_first &&
        !display.empty()) {
        display[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(display[0])));
    }
    return display;
}

std::string format_single_key(std::string_view key,
                              KeyTextCapitalization capitalization) {
    const auto chords = split(key, '+');
    std::ostringstream result;
    for (std::size_t i = 0; i < chords.size(); ++i) {
        if (i > 0) {
            result << '+';
        }
        result << format_key_part(chords[i], capitalization);
    }
    return result.str();
}

}  // namespace

std::string format_key_text(std::string_view key,
                            KeyTextCapitalization capitalization) {
    const auto alternatives = split(key, '/');
    std::ostringstream result;
    for (std::size_t i = 0; i < alternatives.size(); ++i) {
        if (i > 0) {
            result << '/';
        }
        result << format_single_key(alternatives[i], capitalization);
    }
    return result.str();
}

std::string key_display_text(std::string_view keybinding_name) {
    const auto& kb = get_keybindings_const();
    const auto keys = kb.keys_for(std::string(keybinding_name));
    std::ostringstream result;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) {
            result << '/';
        }
        result << format_key_text(keys[i],
                                  KeyTextCapitalization::capitalize_first);
    }
    return result.str();
}

std::string key_hint(std::string_view keybinding_name, std::string_view description) {
    const auto& kb = get_keybindings_const();
    const auto keys = kb.keys_for(std::string(keybinding_name));
    std::ostringstream result;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) {
            result << '/';
        }
        result << format_key_text(keys[i]);
    }
    result << ' ' << description;
    return result.str();
}

std::string raw_key_hint(std::string_view key, std::string_view description) {
    return format_key_text(key) + " " + std::string(description);
}

}  // namespace qt::tui
