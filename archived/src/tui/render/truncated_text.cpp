#include "qt/tui/render/truncated_text.hpp"

#include "qt/foundation/utf8.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

std::string_view first_line(std::string_view text) {
    const auto newline = text.find('\n');
    if (newline == std::string_view::npos) {
        return text;
    }
    return text.substr(0, newline);
}

foundation::Result<std::string> truncate_with_ellipsis(std::string_view text, std::size_t width,
                                                       std::string_view ellipsis) {
    if (width == 0) {
        return std::string();
    }
    if (ellipsis.empty()) {
        return clamp_line(text, width);
    }

    auto text_width = foundation::utf8_display_width(text);
    if (!text_width) {
        return text_width.error();
    }
    if (text_width.value() <= width) {
        return std::string(text);
    }

    auto ellipsis_width = foundation::utf8_display_width(ellipsis);
    if (!ellipsis_width) {
        return ellipsis_width.error();
    }
    if (ellipsis_width.value() >= width) {
        return foundation::utf8_truncate(ellipsis, width);
    }

    return foundation::utf8_truncate(text, width, ellipsis);
}

foundation::Result<std::string> fit_to_width(std::string line, std::size_t width) {
    auto measured = foundation::utf8_display_width(line);
    if (!measured) {
        return measured.error();
    }

    if (measured.value() > width) {
        auto truncated = foundation::utf8_truncate(line, width);
        if (!truncated) {
            return truncated.error();
        }
        line = std::move(truncated.value());
        measured = foundation::utf8_display_width(line);
        if (!measured) {
            return measured.error();
        }
    }

    if (measured.value() < width) {
        line.append(width - measured.value(), ' ');
    }
    return line;
}

RenderBuffer make_exact_buffer(std::vector<std::string> lines, const RenderConstraints& constraints) {
    if (constraints.max_height < lines.size()) {
        lines.resize(constraints.max_height);
    }

    RenderBuffer buffer;
    buffer.width = constraints.max_width;
    buffer.lines = std::move(lines);
    return buffer;
}

}  // namespace

TruncatedText::TruncatedText(std::string text, std::size_t padding_x, std::size_t padding_y,
                             std::string ellipsis)
    : text_(std::move(text)),
      padding_x_(padding_x),
      padding_y_(padding_y),
      ellipsis_(std::move(ellipsis)) {}

foundation::Result<RenderBuffer> TruncatedText::render(const RenderConstraints& constraints) const {
    std::vector<std::string> lines;
    lines.reserve((padding_y_ * 2U) + 1U);

    const std::string empty_line(constraints.max_width, ' ');
    for (std::size_t index = 0; index < padding_y_; ++index) {
        lines.push_back(empty_line);
    }

    const std::size_t horizontal_padding = padding_x_ * 2U;
    const std::size_t available_width = std::max<std::size_t>(
        1U, constraints.max_width > horizontal_padding ? constraints.max_width - horizontal_padding
                                                       : 0U);

    auto display_text = truncate_with_ellipsis(first_line(text_), available_width, ellipsis_);
    if (!display_text) {
        return display_text.error();
    }

    std::string line;
    line.append(std::min(padding_x_, constraints.max_width), ' ');
    line += display_text.value();
    line.append(std::min(padding_x_, constraints.max_width), ' ');

    auto padded = fit_to_width(std::move(line), constraints.max_width);
    if (!padded) {
        return padded.error();
    }
    lines.push_back(std::move(padded.value()));

    for (std::size_t index = 0; index < padding_y_; ++index) {
        lines.push_back(empty_line);
    }

    return make_exact_buffer(std::move(lines), constraints);
}

}  // namespace qt::tui
