#include "qt/tui/layout/border.hpp"

#include <string>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

std::size_t saturating_subtract(std::size_t value, std::size_t amount) {
    if (amount >= value) {
        return 0;
    }
    return value - amount;
}

foundation::Result<std::string> repeat_char(char value, std::size_t width) {
    return clamp_line(std::string(width, value), width);
}

foundation::Result<std::string> make_border_row(char left, char middle, char right,
                                                std::size_t width) {
    if (width == 0) {
        return std::string();
    }
    if (width == 1) {
        return std::string(1, left);
    }

    std::string line;
    line.reserve(width);
    line.push_back(left);
    line.append(width - 2, middle);
    line.push_back(right);
    return line;
}

}  // namespace

Border::Border(ComponentPtr child, BorderStyle style)
    : child_(std::move(child)), style_(style) {}

foundation::Result<RenderBuffer> Border::render(const RenderConstraints& constraints) const {
    std::vector<std::string> lines;
    lines.reserve(constraints.max_height);

    const std::size_t inner_width = saturating_subtract(constraints.max_width, 2);
    const std::size_t inner_height = saturating_subtract(constraints.max_height, 2);

    if (constraints.max_height > 0) {
        auto top = make_border_row(style_.top_left, style_.horizontal, style_.top_right,
                                   constraints.max_width);
        if (!top) {
            return top.error();
        }
        lines.push_back(std::move(top.value()));
    }

    if (constraints.max_height > 1) {
        std::vector<std::string> child_lines;
        if (child_) {
            auto rendered = child_->render(RenderConstraints{inner_width, inner_height});
            if (!rendered) {
                return rendered.error();
            }
            child_lines = std::move(rendered.value().lines);
        }

        for (const auto& child_line : child_lines) {
            if (lines.size() >= constraints.max_height - 1) {
                break;
            }

            std::string line;
            line.reserve(constraints.max_width);
            line.push_back(style_.vertical);
            line += child_line;
            line.push_back(style_.vertical);

            auto clamped = clamp_line(line, constraints.max_width);
            if (!clamped) {
                return clamped.error();
            }
            lines.push_back(std::move(clamped.value()));
        }

        auto blank_inner = repeat_char(' ', inner_width);
        if (!blank_inner) {
            return blank_inner.error();
        }

        while (lines.size() < constraints.max_height - 1) {
            std::string line;
            line.reserve(constraints.max_width);
            line.push_back(style_.vertical);
            line += blank_inner.value();
            line.push_back(style_.vertical);

            auto clamped = clamp_line(line, constraints.max_width);
            if (!clamped) {
                return clamped.error();
            }
            lines.push_back(std::move(clamped.value()));
        }
    }

    if (constraints.max_height > 1) {
        auto bottom = make_border_row(style_.bottom_left, style_.horizontal,
                                      style_.bottom_right, constraints.max_width);
        if (!bottom) {
            return bottom.error();
        }
        lines.push_back(std::move(bottom.value()));
    }

    return make_buffer(std::move(lines), constraints);
}

}  // namespace qt::tui
