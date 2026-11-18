#include "qt/tui/render/text.hpp"

#include "qt/foundation/utf8.hpp"
#include "qt/tui/terminal/ansi.hpp"

#include <uni_algo/ranges_grapheme.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

std::string normalize_tabs(std::string_view text) {
    std::string normalized;
    for (const char ch : text) {
        if (ch == '\t') {
            normalized += "   ";
        } else {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

std::vector<std::string_view> logical_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
        if (start == text.size()) {
            lines.emplace_back();
            break;
        }
    }
    return lines;
}

foundation::Result<std::size_t> visible_width(std::string_view text) {
    std::size_t width = 0;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (auto sequence = extract_ansi_sequence(text, offset)) {
            offset += sequence->length;
            continue;
        }
        if (text[offset] == '\x1b') {
            ++offset;
            continue;
        }

        const auto next_escape = text.find('\x1b', offset);
        const auto end = next_escape == std::string_view::npos ? text.size()
                         : next_escape == offset ? offset + 1U : next_escape;
        auto span_width = foundation::utf8_display_width(text.substr(offset, end - offset));
        if (!span_width) {
            return span_width.error();
        }
        width += span_width.value();
        offset = end;
    }
    return width;
}

foundation::Result<std::string> truncate_visible(std::string_view text, std::size_t width,
                                               std::size_t next_escape) {
    std::string result;
    std::size_t columns = 0;
    std::size_t offset = 0;

    while (offset < text.size()) {
        if (auto sequence = extract_ansi_sequence(text, offset)) {
            result.append(text.substr(offset, sequence->length));
            offset += sequence->length;
            continue;
        }
        if (text[offset] == '\x1b') {
            ++offset;
            continue;
        }

        if (next_escape != std::string_view::npos && next_escape < offset) {
            next_escape = text.find('\x1b', offset);
        }
        const auto plain_end = next_escape == std::string_view::npos ? text.size()
                               : next_escape == offset ? offset + 1U : next_escape;
        const auto plain = text.substr(offset, plain_end - offset);
        std::size_t consumed = 0;

        auto grapheme_range = una::ranges::grapheme::utf8_view<std::string_view>{plain};
        auto it = grapheme_range.begin();
        auto end = grapheme_range.end();

        while (it != end) {
            std::string_view grapheme = *it;

            // Handle regional indicator pairs: two consecutive RIs form one flag
            if (foundation::is_regional_indicator_grapheme(grapheme)) {
                auto next = it;
                ++next;
                if (next != end && foundation::is_regional_indicator_grapheme(*next)) {
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
                    ++it;
                    ++it;
                    continue;
                }
            }

            auto glyph_width = foundation::utf8_display_width(grapheme);
            if (!glyph_width) {
                return glyph_width.error();
            }
            if (columns + glyph_width.value() > width) {
                return result;
            }
            result.append(grapheme);
            columns += glyph_width.value();
            consumed += grapheme.size();
            ++it;
        }
        offset += consumed;
    }

    return result;
}

std::size_t safe_width(std::string_view text) {
    auto width = visible_width(text);
    return width ? width.value() : text.size();
}

std::vector<std::string_view> split_words(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
        }
        const auto start = index;
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index])) == 0) {
            ++index;
        }
        if (start != index) {
            words.emplace_back(text.substr(start, index - start));
        }
    }
    return words;
}

std::vector<std::string> split_chunk(std::string_view text, std::size_t width,
                                     std::size_t max_chunks) {
    if (text.empty()) {
        return {""};
    }
    std::vector<std::string> chunks;
    std::size_t offset = 0;
    auto next_escape = text.find('\x1b');
    while (offset < text.size() && chunks.size() < max_chunks) {
        const auto remaining = text.substr(offset);
        if (next_escape != std::string_view::npos && next_escape < offset) {
            next_escape = text.find('\x1b', offset);
        }
        auto chunk = truncate_visible(remaining, width,
            next_escape == std::string_view::npos ? next_escape : next_escape - offset);
        if (!chunk || chunk.value().empty()) {
            chunks.emplace_back(remaining);
            break;
        }
        chunks.push_back(chunk.value());
        offset += chunk.value().size();
    }
    return chunks;
}

std::vector<std::string> wrap_line(std::string_view text, std::size_t width,
                                   std::size_t max_lines) {
    if (max_lines == 0U) return {};
    width = std::max<std::size_t>(1, width);
    const auto words = split_words(text);
    if (words.empty()) {
        return {""};
    }

    std::vector<std::string> lines;
    std::string current;
    std::size_t current_width = 0;

    for (const auto& word : words) {
        const auto chunks = split_chunk(word, width, max_lines - lines.size() + 1U);
        for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
            const auto& chunk = chunks[chunk_index];
            const std::size_t chunk_width = safe_width(chunk);
            const bool needs_space = !current.empty() && chunk_index == 0;
            const std::size_t next_width = current_width + (needs_space ? 1U : 0U) + chunk_width;

            if (!current.empty() && (chunk_index > 0 || next_width > width)) {
                lines.push_back(std::move(current));
                if (lines.size() == max_lines) return lines;
                current.clear();
                current_width = 0;
            }
            if (!current.empty() && chunk_index == 0) {
                current.push_back(' ');
                ++current_width;
            }
            current += chunk;
            current_width += chunk_width;
        }
    }

    if (!current.empty()) {
        lines.push_back(std::move(current));
    }
    return lines.empty() ? std::vector<std::string>{""} : lines;
}

}  // namespace

namespace {

foundation::Result<RenderBuffer> styled_buffer(std::vector<std::string> lines,
                                               const RenderConstraints& constraints,
                                               const TextStyleFunction& background) {
    if (constraints.max_height < lines.size()) {
        lines.resize(constraints.max_height);
    }

    for (auto& line : lines) {
        auto clamped = clamp_line(line, constraints.max_width);
        if (!clamped) {
            return clamped.error();
        }
        line = background ? background(clamped.value()) : std::move(clamped.value());
    }

    return RenderBuffer{constraints.max_width, std::move(lines)};
}

}  // namespace

Text::Text(std::string text, std::size_t padding_x, std::size_t padding_y,
           TextStyleFunction background)
    : text_(std::move(text)),
      padding_x_(padding_x),
      padding_y_(padding_y),
      background_(std::move(background)) {}

foundation::Result<RenderBuffer> Text::render(const RenderConstraints& constraints) const {
    if (render_cache_ && render_cache_->constraints.max_width == constraints.max_width &&
        render_cache_->constraints.max_height == constraints.max_height) {
        return render_cache_->buffer;
    }
    if (constraints.max_height == 0U) return RenderBuffer{constraints.max_width, {}};
    if (text_.empty() || std::all_of(text_.begin(), text_.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        })) {
        return styled_buffer({}, constraints, background_);
    }

    std::vector<std::string> lines;
    const std::string empty_line(constraints.max_width, ' ');
    for (std::size_t index = 0; index < padding_y_; ++index) {
        lines.push_back(empty_line);
    }

    const std::size_t horizontal_padding = padding_x_ * 2U;
    const std::size_t content_width =
        constraints.max_width > horizontal_padding ? constraints.max_width - horizontal_padding : 1U;
    const std::string left_padding(padding_x_, ' ');
    const std::string right_padding(padding_x_, ' ');
    const std::string normalized = normalize_tabs(text_);

    for (const auto line : logical_lines(normalized)) {
        if (lines.size() >= constraints.max_height) break;
        for (const auto& wrapped : wrap_line(line, content_width,
                                             constraints.max_height - lines.size())) {
            lines.push_back(left_padding + wrapped + right_padding);
        }
    }

    for (std::size_t index = 0; index < padding_y_; ++index) {
        lines.push_back(empty_line);
    }

    auto buffer = styled_buffer(std::move(lines), constraints, background_);
    if (buffer) render_cache_ = RenderCache{constraints, buffer.value()};
    return buffer;
}

void Text::set_text(std::string text) {
    if (text_ == text) return;
    text_ = std::move(text);
    render_cache_.reset();
}

void Text::set_padding(std::size_t padding_x, std::size_t padding_y) noexcept {
    padding_x_ = padding_x;
    padding_y_ = padding_y;
    render_cache_.reset();
}

void Text::set_background(TextStyleFunction background) {
    background_ = std::move(background);
    render_cache_.reset();
}

const std::string& Text::text() const noexcept { return text_; }

std::size_t Text::padding_x() const noexcept { return padding_x_; }

std::size_t Text::padding_y() const noexcept { return padding_y_; }

}  // namespace qt::tui
