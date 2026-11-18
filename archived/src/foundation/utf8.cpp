#include "qt/foundation/utf8.hpp"

#include <uni_algo/ranges_grapheme.h>
#include <uni_algo/prop.h>

#include <array>
#include <string_view>

namespace qt::foundation {

namespace {

struct Interval {
    char32_t first;
    char32_t last;
};

constexpr std::array<Interval, 14> kZeroWidth = {{
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x0610, 0x061A}, {0x064B, 0x065F},
    {0x0670, 0x0670}, {0x06D6, 0x06DD}, {0x200B, 0x200F}, {0x202A, 0x202E},
    {0x2060, 0x2064}, {0xFE00, 0xFE0F},
}};

constexpr std::array<Interval, 18> kWide = {{
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x2705, 0x2705}, {0x1F300, 0x1FAFF},
    {0x2E80, 0xA4CF}, {0xAC00, 0xD7A3},
}};

template <std::size_t N>
bool in_intervals(char32_t code_point, const std::array<Interval, N>& intervals) {
    for (const auto& interval : intervals) {
        if (code_point >= interval.first && code_point <= interval.last) {
            return true;
        }
    }
    return false;
}

bool is_regional_indicator(char32_t code_point) {
    return code_point >= 0x1F1E6 && code_point <= 0x1F1FF;
}

bool is_zero_width_joiner(char32_t code_point) {
    return code_point == 0x200D;
}

Result<std::size_t> decode_one(std::string_view text, std::size_t offset, char32_t& code_point,
                               std::size_t& next_offset);

int code_point_width(char32_t code_point) {
    if (code_point == U'\t') {
        return 4;
    }
    if (code_point < 0x20 || (code_point >= 0x7F && code_point < 0xA0)) {
        return 0;
    }
    const auto category = una::codepoint::get_general_category(code_point);
    if (category == una::codepoint::general_category::Mn ||
        category == una::codepoint::general_category::Mc ||
        category == una::codepoint::general_category::Me ||
        category == una::codepoint::general_category::Cf ||
        category == una::codepoint::general_category::Cs ||
        in_intervals(code_point, kZeroWidth)) {
        return 0;
    }
    if (code_point == 0x0e33 || code_point == 0x0eb3) return 1;
    if (is_regional_indicator(code_point)) {
        return 2;
    }
    if (in_intervals(code_point, kWide)) {
        return 2;
    }
    return 1;
}

Result<std::size_t> grapheme_width(std::string_view grapheme) {
    std::size_t width = 0;
    bool has_joiner = false;
    bool has_wide = false;
    bool has_regional_indicator = false;

    std::size_t offset = 0;
    while (offset < grapheme.size()) {
        char32_t code_point = 0;
        std::size_t next_offset = offset;
        auto decoded = decode_one(grapheme, offset, code_point, next_offset);
        if (!decoded) {
            return decoded.error();
        }

        has_joiner = has_joiner || is_zero_width_joiner(code_point);
        has_wide = has_wide || code_point_width(code_point) == 2;
        has_regional_indicator =
            has_regional_indicator || is_regional_indicator(code_point);
        width += static_cast<std::size_t>(code_point_width(code_point));
        offset = next_offset;
    }

    if (has_regional_indicator) {
        return static_cast<std::size_t>(2);
    }
    if (has_joiner && has_wide) {
        return static_cast<std::size_t>(2);
    }
    return width;
}

Result<std::size_t> decode_one(std::string_view text, std::size_t offset, char32_t& code_point,
                               std::size_t& next_offset) {
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80) {
        code_point = lead;
        next_offset = offset + 1;
        return next_offset;
    }

    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        value = lead & 0x1FU;
        minimum = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        value = lead & 0x0FU;
        minimum = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        value = lead & 0x07U;
        minimum = 0x10000;
    } else {
        return Result<std::size_t>::failure(ErrorCode::encoding_error, "invalid utf-8 lead byte");
    }

    if (offset + length > text.size()) {
        return Result<std::size_t>::failure(ErrorCode::encoding_error, "truncated utf-8 sequence");
    }

    for (std::size_t index = offset + 1; index < offset + length; ++index) {
        const unsigned char continuation = static_cast<unsigned char>(text[index]);
        if ((continuation & 0xC0U) != 0x80U) {
            return Result<std::size_t>::failure(ErrorCode::encoding_error,
                                                "invalid utf-8 continuation byte");
        }
        value = static_cast<char32_t>((value << 6U) | (continuation & 0x3FU));
    }

    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return Result<std::size_t>::failure(ErrorCode::encoding_error, "invalid utf-8 code point");
    }

    code_point = value;
    next_offset = offset + length;
    return next_offset;
}

Result<std::size_t> validate_utf8(std::string_view text);

Result<std::size_t> measure_width(std::string_view text, std::size_t stop_after) {
    auto validation = validate_utf8(text);
    if (!validation) {
        return validation.error();
    }

    std::size_t columns = 0;
    auto grapheme_range = una::ranges::grapheme::utf8_view<std::string_view>{text};
    auto it = grapheme_range.begin();
    auto end = grapheme_range.end();

    while (it != end) {
        std::string_view grapheme = *it;

        // Handle regional indicator pairs
        if (is_regional_indicator_grapheme(grapheme)) {
            auto next = it;
            ++next;
            if (next != end && is_regional_indicator_grapheme(*next)) {
                columns += 2;
                ++it;
                ++it;
                continue;
            }
        }

        auto width = grapheme_width(grapheme);
        if (!width) {
            return width.error();
        }
        columns += width.value();
        if (columns > stop_after) {
            return columns;
        }
        ++it;
    }
    return columns;
}

Result<std::size_t> validate_utf8(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        char32_t code_point = 0;
        std::size_t next_offset = offset;
        auto decoded = decode_one(text, offset, code_point, next_offset);
        if (!decoded) {
            return decoded.error();
        }
        offset = next_offset;
    }
    return offset;
}

}  // namespace

bool is_regional_indicator_grapheme(std::string_view grapheme) {
    if (grapheme.empty()) {
        return false;
    }
    char32_t code_point = 0;
    std::size_t next_offset = 0;
    auto decoded = decode_one(grapheme, 0, code_point, next_offset);
    if (!decoded) {
        return false;
    }
    // Must be exactly one code point that is a regional indicator
    return next_offset >= grapheme.size() && is_regional_indicator(code_point);
}

Result<std::size_t> utf8_display_width(std::string_view text) {
    return measure_width(text, static_cast<std::size_t>(-1));
}

Result<std::string> utf8_truncate(std::string_view text, std::size_t max_columns,
                                  std::string_view ellipsis) {
    auto ellipsis_width = utf8_display_width(ellipsis);
    if (!ellipsis_width) {
        return ellipsis_width.error();
    }
    if (ellipsis_width.value() > max_columns) {
        return Result<std::string>::failure(ErrorCode::invalid_argument,
                                            "ellipsis wider than max_columns");
    }

    auto text_width = utf8_display_width(text);
    if (!text_width) {
        return text_width.error();
    }
    if (text_width.value() <= max_columns) {
        return std::string(text);
    }

    const std::size_t target_columns = max_columns - ellipsis_width.value();
    std::size_t columns = 0;
    std::size_t offset = 0;
    auto grapheme_range = una::ranges::grapheme::utf8_view<std::string_view>{text};
    auto it = grapheme_range.begin();
    auto end = grapheme_range.end();

    while (it != end) {
        std::string_view grapheme = *it;

        // Handle regional indicator pairs
        if (is_regional_indicator_grapheme(grapheme)) {
            auto next = it;
            ++next;
            if (next != end && is_regional_indicator_grapheme(*next)) {
                if (columns + 2 > target_columns) {
                    break;
                }
                columns += 2;
                offset += grapheme.size() + (*next).size();
                ++it;
                ++it;
                continue;
            }
        }

        auto width = grapheme_width(grapheme);
        if (!width) {
            return width.error();
        }

        if (columns + width.value() > target_columns) {
            break;
        }
        columns += width.value();
        offset += grapheme.size();
        ++it;
    }

    return std::string(text.substr(0, offset)) + std::string(ellipsis);
}

}  // namespace qt::foundation
