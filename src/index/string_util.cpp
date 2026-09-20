#include "index/string_util.hpp"

#include <algorithm>
#include <cctype>

namespace qt::index {

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim_ascii(std::string value) {
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    const auto begin = std::find_if(value.begin(), value.end(), not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::vector<std::string> split_shell_like(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    bool escaped = false;

    for (const char ch : text) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

}  // namespace qt::index
