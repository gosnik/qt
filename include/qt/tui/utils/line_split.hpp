#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct SplitLinesOptions {
    bool trim_trailing_cr = false;
    bool keep_empty_input = true;
    bool keep_trailing_empty = true;
};

[[nodiscard]] inline std::vector<std::string_view> split_lines_view(
    std::string_view text, SplitLinesOptions options = {}) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const std::size_t line_end =
            end == std::string_view::npos ? text.size() : end;
        auto line = text.substr(start, line_end - start);
        if (options.trim_trailing_cr && !line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);

        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
        if (start == text.size()) {
            if (options.keep_trailing_empty) {
                lines.emplace_back();
            }
            break;
        }
    }

    if (text.empty() && !options.keep_empty_input) {
        lines.clear();
    }
    return lines;
}

[[nodiscard]] inline std::vector<std::string> split_lines_copy(
    std::string_view text, SplitLinesOptions options = {}) {
    const auto views = split_lines_view(text, options);
    std::vector<std::string> lines;
    lines.reserve(views.size());
    for (const auto line : views) {
        lines.emplace_back(line);
    }
    return lines;
}

}  // namespace qt::tui
