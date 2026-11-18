#include "qt/tui/render/diff_component.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

constexpr std::size_t kMaxWordDiffCells = 20000U;

/// Default color functions using ANSI escape codes
std::string default_removed_color(std::string_view text) {
    return std::string("\x1b[31m") + std::string(text) + "\x1b[0m";
}

std::string default_added_color(std::string_view text) {
    return std::string("\x1b[32m") + std::string(text) + "\x1b[0m";
}

std::string default_context_color(std::string_view text) {
    return std::string("\x1b[90m") + std::string(text) + "\x1b[0m";
}

std::string default_inverse_color(std::string_view text) {
    return std::string("\x1b[7m") + std::string(text) + "\x1b[27m";
}

/// Simple word-level diff using longest common subsequence on words.
struct WordDiffPart {
    enum Kind { equal, removed, added };
    Kind kind;
    std::string text;
};

std::vector<WordDiffPart> diff_words(std::string_view old_text, std::string_view new_text) {
    // Split into words (keeping whitespace attached)
    auto split_words = [](std::string_view text) -> std::vector<std::string> {
        std::vector<std::string> words;
        std::size_t i = 0;
        while (i < text.size()) {
            // Collect whitespace
            std::size_t ws_start = i;
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
                ++i;
            }
            if (i > ws_start) {
                words.emplace_back(text.substr(ws_start, i - ws_start));
            }
            // Collect non-whitespace
            std::size_t word_start = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
                ++i;
            }
            if (i > word_start) {
                words.emplace_back(text.substr(word_start, i - word_start));
            }
        }
        return words;
    };

    auto old_words = split_words(old_text);
    auto new_words = split_words(new_text);

    const std::size_t m = old_words.size();
    const std::size_t n = new_words.size();
    if (m > 0 && n > 0 && m > kMaxWordDiffCells / n) {
        return {{WordDiffPart::removed, std::string(old_text)},
                {WordDiffPart::added, std::string(new_text)}};
    }

    // LCS table
    std::vector<std::vector<std::size_t>> dp(m + 1, std::vector<std::size_t>(n + 1, 0));
    for (std::size_t i = 1; i <= m; ++i) {
        for (std::size_t j = 1; j <= n; ++j) {
            if (old_words[i - 1] == new_words[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
            }
        }
    }

    // Backtrack to produce diff
    std::vector<WordDiffPart> result;
    std::size_t i = m, j = n;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && old_words[i - 1] == new_words[j - 1]) {
            result.push_back({WordDiffPart::equal, old_words[i - 1]});
            --i;
            --j;
        } else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) {
            result.push_back({WordDiffPart::added, new_words[j - 1]});
            --j;
        } else {
            result.push_back({WordDiffPart::removed, old_words[i - 1]});
            --i;
        }
    }

    std::reverse(result.begin(), result.end());
    return result;
}

} // namespace

DiffComponent::DiffComponent(
    std::string diff_text,
    ColorFunction removed_color,
    ColorFunction added_color,
    ColorFunction context_color,
    ColorFunction inverse_color)
    : diff_text_(std::move(diff_text)),
      removed_color_(removed_color ? std::move(removed_color) : default_removed_color),
      added_color_(added_color ? std::move(added_color) : default_added_color),
      context_color_(context_color ? std::move(context_color) : default_context_color),
      inverse_color_(inverse_color ? std::move(inverse_color) : default_inverse_color) {}

void DiffComponent::set_diff_text(std::string diff_text) {
    diff_text_ = std::move(diff_text);
    cached_colored_.reset();
    cached_diff_text_.clear();
}

const std::string& DiffComponent::diff_text() const noexcept {
    return diff_text_;
}

std::optional<DiffComponent::ParsedLine> DiffComponent::parse_diff_line(
    std::string_view line) {
    // Format: "+123 content" or "-123 content" or " 123 content" or "     ..."
    if (line.empty()) return std::nullopt;

    const char prefix = line[0];
    if (prefix != '+' && prefix != '-' && prefix != ' ') return std::nullopt;

    // Skip prefix, parse optional line number and content
    std::size_t pos = 1;
    std::string line_num;

    // Read digits (possibly with leading spaces)
    while (pos < line.size() && (line[pos] == ' ' || std::isdigit(static_cast<unsigned char>(line[pos])))) {
        if (std::isdigit(static_cast<unsigned char>(line[pos]))) {
            line_num.push_back(line[pos]);
        }
        ++pos;
    }

    // Expect a space separator before content
    if (pos < line.size() && line[pos] == ' ') {
        ++pos;
    }

    std::string content(pos < line.size() ? line.substr(pos) : std::string_view{});
    return ParsedLine{prefix, std::move(line_num), std::move(content)};
}

std::string DiffComponent::replace_tabs(std::string text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        if (ch == '\t') {
            result += "   ";
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

DiffComponent::IntraLineResult DiffComponent::render_intra_line_diff(
    std::string_view old_content,
    std::string_view new_content) const {
    auto old_replaced = replace_tabs(std::string(old_content));
    auto new_replaced = replace_tabs(std::string(new_content));

    auto parts = diff_words(old_replaced, new_replaced);

    std::string removed_line;
    std::string added_line;
    bool is_first_removed = true;
    bool is_first_added = true;

    for (const auto& part : parts) {
        if (part.kind == WordDiffPart::removed) {
            std::string value = part.text;
            // Strip leading whitespace from inverse to avoid highlighting indentation
            if (is_first_removed) {
                auto ws_end = value.find_first_not_of(" \t");
                if (ws_end != std::string::npos && ws_end > 0) {
                    removed_line += value.substr(0, ws_end);
                    value = value.substr(ws_end);
                }
                is_first_removed = false;
            }
            if (!value.empty()) {
                removed_line += inverse_color_(value);
            }
        } else if (part.kind == WordDiffPart::added) {
            std::string value = part.text;
            if (is_first_added) {
                auto ws_end = value.find_first_not_of(" \t");
                if (ws_end != std::string::npos && ws_end > 0) {
                    added_line += value.substr(0, ws_end);
                    value = value.substr(ws_end);
                }
                is_first_added = false;
            }
            if (!value.empty()) {
                added_line += inverse_color_(value);
            }
        } else {
            removed_line += part.text;
            added_line += part.text;
        }
    }

    return {std::move(removed_line), std::move(added_line)};
}

std::string DiffComponent::render_colored() const {
    std::istringstream stream(diff_text_);
    std::string line;
    std::vector<std::string> output_lines;

    std::vector<std::string> all_lines;
    while (std::getline(stream, line)) {
        all_lines.push_back(line);
    }

    std::size_t i = 0;
    while (i < all_lines.size()) {
        const auto& current = all_lines[i];
        const auto parsed = parse_diff_line(current);

        if (!parsed) {
            output_lines.push_back(context_color_(current));
            ++i;
            continue;
        }

        if (parsed->prefix == '-') {
            // Collect consecutive removed lines
            std::vector<ParsedLine> removed_lines;
            while (i < all_lines.size()) {
                auto p = parse_diff_line(all_lines[i]);
                if (!p || p->prefix != '-') break;
                removed_lines.push_back(std::move(*p));
                ++i;
            }

            // Collect consecutive added lines
            std::vector<ParsedLine> added_lines;
            while (i < all_lines.size()) {
                auto p = parse_diff_line(all_lines[i]);
                if (!p || p->prefix != '+') break;
                added_lines.push_back(std::move(*p));
                ++i;
            }

            // If exactly one removed and one added, show intra-line diff
            if (removed_lines.size() == 1 && added_lines.size() == 1) {
                auto [removed_highlighted, added_highlighted] = render_intra_line_diff(
                    removed_lines[0].content, added_lines[0].content);

                auto removed_str = "-" + removed_lines[0].line_num + " " + removed_highlighted;
                auto added_str = "+" + added_lines[0].line_num + " " + added_highlighted;

                output_lines.push_back(removed_color_(removed_str));
                output_lines.push_back(added_color_(added_str));
            } else {
                // Show all removed, then all added
                for (const auto& r : removed_lines) {
                    auto text = "-" + r.line_num + " " + replace_tabs(r.content);
                    output_lines.push_back(removed_color_(text));
                }
                for (const auto& a : added_lines) {
                    auto text = "+" + a.line_num + " " + replace_tabs(a.content);
                    output_lines.push_back(added_color_(text));
                }
            }
        } else if (parsed->prefix == '+') {
            // Lone added line (no preceding removed)
            auto text = "+" + parsed->line_num + " " + replace_tabs(parsed->content);
            output_lines.push_back(added_color_(text));
            ++i;
        } else {
            // Context line
            auto text = " " + parsed->line_num + " " + replace_tabs(parsed->content);
            output_lines.push_back(context_color_(text));
            ++i;
        }
    }

    // Join with newlines
    std::string result;
    for (std::size_t j = 0; j < output_lines.size(); ++j) {
        if (j > 0) result += '\n';
        result += output_lines[j];
    }
    return result;
}

foundation::Result<RenderBuffer> DiffComponent::render(
    const RenderConstraints& constraints) const {
    // Render the diff with colors, then pass through make_buffer for width clamping
    if (!cached_colored_ || cached_diff_text_ != diff_text_) {
        cached_diff_text_ = diff_text_;
        cached_colored_ = render_colored();
    }
    auto colored = *cached_colored_;
    return make_buffer({std::move(colored)}, constraints);
}

}
