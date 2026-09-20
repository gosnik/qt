#include "qt/tui/core/component.hpp"

#include "qt/foundation/utf8.hpp"

#include <uni_algo/ranges_grapheme.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace qt::tui {

namespace {

std::optional<std::size_t> ansi_sequence_length(std::string_view text, std::size_t offset) {
    if (offset >= text.size() || text[offset] != '\x1b' || offset + 1 >= text.size()) {
        return std::nullopt;
    }

    const char introducer = text[offset + 1];
    if (introducer == '[') {
        std::size_t index = offset + 2;
        while (index < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[index]);
            if (ch >= 0x40U && ch <= 0x7EU) {
                return index - offset + 1;
            }
            ++index;
        }
        return std::nullopt;
    }

    if (introducer == ']' || introducer == '_' || introducer == 'G') {
        std::size_t index = offset + 2;
        while (index < text.size()) {
            if (text[index] == '\x07') {
                return index - offset + 1;
            }
            if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '\\') {
                return index - offset + 2;
            }
            ++index;
        }
    }

    return std::nullopt;
}

foundation::Result<std::size_t> line_width_impl(std::string_view text) {
    std::size_t width = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (auto sequence_length = ansi_sequence_length(text, offset)) {
            offset += *sequence_length;
            continue;
        }

        const auto next_escape = text.find('\x1b', offset);
        const auto end = next_escape == std::string_view::npos ? text.size() : next_escape;
        auto span_width = foundation::utf8_display_width(text.substr(offset, end - offset));
        if (!span_width) {
            return span_width.error();
        }
        width += span_width.value();
        offset = end;
    }
    return width;
}

foundation::Result<std::string> truncate_line_impl(std::string_view text, std::size_t width) {
    std::string result;
    std::size_t columns = 0;
    std::size_t offset = 0;

    while (offset < text.size()) {
        if (auto sequence_length = ansi_sequence_length(text, offset)) {
            result.append(text.substr(offset, *sequence_length));
            offset += *sequence_length;
            continue;
        }

        const auto next_escape = text.find('\x1b', offset);
        const auto plain_end = next_escape == std::string_view::npos ? text.size() : next_escape;
        const auto plain = text.substr(offset, plain_end - offset);
        std::string pending_joined;
        std::size_t consumed = 0;

        auto grapheme_range = una::ranges::grapheme::utf8_view<std::string_view>{plain};
        auto it = grapheme_range.begin();
        auto end = grapheme_range.end();

        while (it != end) {
            std::string_view grapheme = *it;
            std::string_view glyph = grapheme;
            if (!pending_joined.empty()) {
                pending_joined.append(grapheme);
                glyph = pending_joined;
            }

            if (glyph.size() >= 3 && glyph.substr(glyph.size() - 3) == "\xE2\x80\x8D") {
                if (pending_joined.empty()) {
                    pending_joined.assign(glyph);
                }
                ++it;
                continue;
            }

            // Handle regional indicator pairs: two consecutive RIs form one flag (2 cells)
            if (foundation::is_regional_indicator_grapheme(grapheme)) {
                auto next = it;
                ++next;
                if (next != end && foundation::is_regional_indicator_grapheme(*next)) {
                    // Combine the pair
                    std::string pair(grapheme);
                    pair.append(*next);
                    auto pair_width = foundation::utf8_display_width(pair);
                    if (!pair_width) {
                        return pair_width.error();
                    }
                    if (columns + pair_width.value() > width) {
                        return result;
                    }
                    result.append(pair);
                    columns += pair_width.value();
                    consumed += grapheme.size() + (*next).size();
                    pending_joined.clear();
                    ++it; // skip the first RI
                    ++it; // skip the second RI
                    continue;
                }
            }

            auto glyph_width = foundation::utf8_display_width(glyph);
            if (!glyph_width) {
                return glyph_width.error();
            }
            if (columns + glyph_width.value() > width) {
                return result;
            }
            result.append(glyph);
            columns += glyph_width.value();
            consumed += glyph.size();
            pending_joined.clear();
            ++it;
        }
        if (!pending_joined.empty()) {
            auto glyph_width = foundation::utf8_display_width(pending_joined);
            if (!glyph_width) {
                return glyph_width.error();
            }
            if (columns + glyph_width.value() > width) {
                return result;
            }
            result.append(pending_joined);
            columns += glyph_width.value();
            consumed += pending_joined.size();
        }
        offset += consumed;
    }

    return result;
}

}  // namespace

foundation::Result<std::string> clamp_line(std::string_view text, std::size_t width) {
    auto truncated = truncate_line_impl(text, width);
    if (!truncated) {
        return truncated.error();
    }

    auto measured = line_width_impl(truncated.value());
    if (!measured) {
        return measured.error();
    }

    std::string line = truncated.value();
    while (measured.value() < width) {
        line.push_back(' ');
        ++measured.value();
    }

    return line;
}

foundation::Result<std::size_t> measure_line_width(std::string_view text) {
    return line_width_impl(text);
}

foundation::Result<std::string> truncate_line(std::string_view text, std::size_t width) {
    return truncate_line_impl(text, width);
}

foundation::Result<RenderBuffer> make_buffer(std::vector<std::string> lines,
                                             const RenderConstraints& constraints) {
    if (constraints.max_height < lines.size()) {
        lines.resize(constraints.max_height);
    }

    for (auto& line : lines) {
        auto clamped = clamp_line(line, constraints.max_width);
        if (!clamped) {
            return clamped.error();
        }
        line = std::move(clamped.value());
    }

    RenderBuffer buffer;
    buffer.width = constraints.max_width;
    buffer.lines = std::move(lines);
    return buffer;
}

std::optional<CursorPosition> extract_cursor_marker(std::vector<std::string>& lines,
                                                    std::size_t viewport_height) {
    const std::size_t viewport_top =
        lines.size() > viewport_height ? lines.size() - viewport_height : 0U;
    for (std::size_t row = lines.size(); row > viewport_top; --row) {
        auto& line = lines[row - 1U];
        const auto marker = line.find(cursor_marker);
        if (marker == std::string::npos) {
            continue;
        }

        const auto before_marker = line.substr(0, marker);
        auto column = line_width_impl(before_marker);
        if (!column) {
            column = before_marker.size();
        }
        line.erase(marker, cursor_marker.size());
        return CursorPosition{row - 1U, column.value()};
    }
    return std::nullopt;
}

}  // namespace qt::tui
