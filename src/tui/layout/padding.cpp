#include "qt/tui/layout/padding.hpp"

#include <algorithm>
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

foundation::Result<std::string> blank_line(std::size_t width) {
    return clamp_line("", width);
}

}  // namespace

Padding::Padding(ComponentPtr child, Insets insets)
    : child_(std::move(child)), insets_(insets) {}

foundation::Result<RenderBuffer> Padding::render(const RenderConstraints& constraints) const {
    const std::size_t inner_width =
        saturating_subtract(constraints.max_width, insets_.left + insets_.right);
    const std::size_t inner_height =
        saturating_subtract(constraints.max_height, insets_.top + insets_.bottom);

    std::vector<std::string> lines;
    lines.reserve(constraints.max_height);

    auto spacer = blank_line(constraints.max_width);
    if (!spacer) {
        return spacer.error();
    }

    const std::size_t top_count = std::min(insets_.top, constraints.max_height);
    for (std::size_t index = 0; index < top_count; ++index) {
        lines.push_back(spacer.value());
    }

    if (lines.size() < constraints.max_height) {
        std::vector<std::string> child_lines;
        if (child_) {
            auto rendered = child_->render(RenderConstraints{inner_width, inner_height});
            if (!rendered) {
                return rendered.error();
            }
            child_lines = std::move(rendered.value().lines);
        }

        for (const auto& child_line : child_lines) {
            if (lines.size() >= constraints.max_height) {
                break;
            }

            std::string line(insets_.left, ' ');
            line += child_line;
            line.append(insets_.right, ' ');

            auto clamped = clamp_line(line, constraints.max_width);
            if (!clamped) {
                return clamped.error();
            }

            lines.push_back(std::move(clamped.value()));
        }
    }

    const std::size_t remaining = constraints.max_height - lines.size();
    const std::size_t bottom_count = std::min(insets_.bottom, remaining);
    for (std::size_t index = 0; index < bottom_count; ++index) {
        lines.push_back(spacer.value());
    }

    return make_buffer(std::move(lines), constraints);
}

}  // namespace qt::tui
