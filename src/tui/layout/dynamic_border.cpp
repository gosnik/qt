#include "qt/tui/layout/dynamic_border.hpp"

#include "qt/tui/core/component.hpp"

#include <string>
#include <utility>

namespace qt::tui {

DynamicBorder::DynamicBorder(ColorFunction color)
    : color_(std::move(color)) {}

void DynamicBorder::set_color(ColorFunction color) {
    color_ = std::move(color);
}

foundation::Result<RenderBuffer> DynamicBorder::render(
    const RenderConstraints& constraints) const {
    if (constraints.max_height == 0) {
        return foundation::Result<RenderBuffer>::success(RenderBuffer{});
    }

    const std::size_t width = std::max<std::size_t>(1U, constraints.max_width);
    std::string line(width, '-');

    if (color_) {
        line = color_(line);
    }

    RenderBuffer buffer;
    buffer.width = width;
    buffer.lines = {std::move(line)};
    return buffer;
}

}  // namespace qt::tui
