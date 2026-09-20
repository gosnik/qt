#include "qt/tui/layout/box.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

std::size_t saturating_subtract(std::size_t value, std::size_t amount) noexcept {
    return amount >= value ? 0 : value - amount;
}

}  // namespace

Box::Box(std::size_t padding_x, std::size_t padding_y, BoxStyleFunction background)
    : padding_x_(padding_x), padding_y_(padding_y), background_(std::move(background)) {}

void Box::add_child(ComponentPtr child) {
    if (child) {
        children_.push_back(std::move(child));
    }
}

void Box::remove_child(const ComponentPtr& child) {
    children_.erase(std::remove(children_.begin(), children_.end(), child), children_.end());
}

void Box::clear() { children_.clear(); }

void Box::set_background(BoxStyleFunction background) { background_ = std::move(background); }

const std::vector<ComponentPtr>& Box::children() const noexcept { return children_; }

foundation::Result<std::string> Box::apply_background(std::string_view line,
                                                      std::size_t width) const {
    auto clamped = clamp_line(line, width);
    if (!clamped) {
        return clamped.error();
    }
    if (!background_) {
        return clamped.value();
    }
    return background_(clamped.value());
}

foundation::Result<RenderBuffer> Box::render(const RenderConstraints& constraints) const {
    if (children_.empty() || constraints.max_height == 0) {
        return make_buffer({}, constraints);
    }

    const std::size_t content_width =
        std::max<std::size_t>(1, saturating_subtract(constraints.max_width, padding_x_ * 2U));
    std::vector<std::string> child_lines;
    child_lines.reserve(constraints.max_height);

    const std::string left_padding(padding_x_, ' ');
    const std::size_t content_height =
        saturating_subtract(constraints.max_height, padding_y_ * 2U);
    for (const auto& child : children_) {
        if (!child || child_lines.size() >= content_height) {
            continue;
        }
        auto rendered = child->render(
            RenderConstraints{content_width, content_height - child_lines.size()});
        if (!rendered) {
            return rendered.error();
        }
        for (const auto& line : rendered.value().lines) {
            if (child_lines.size() >= content_height) {
                break;
            }
            child_lines.push_back(left_padding + line);
        }
    }

    if (child_lines.empty()) {
        return make_buffer({}, constraints);
    }

    std::vector<std::string> lines;
    lines.reserve(constraints.max_height);

    for (std::size_t index = 0; index < padding_y_ && lines.size() < constraints.max_height;
         ++index) {
        auto padded = apply_background("", constraints.max_width);
        if (!padded) {
            return padded.error();
        }
        lines.push_back(std::move(padded.value()));
    }

    for (const auto& child_line : child_lines) {
        if (lines.size() >= constraints.max_height) {
            break;
        }
        auto padded = apply_background(child_line, constraints.max_width);
        if (!padded) {
            return padded.error();
        }
        lines.push_back(std::move(padded.value()));
    }

    for (std::size_t index = 0; index < padding_y_ && lines.size() < constraints.max_height;
         ++index) {
        auto padded = apply_background("", constraints.max_width);
        if (!padded) {
            return padded.error();
        }
        lines.push_back(std::move(padded.value()));
    }

    return make_buffer(std::move(lines), constraints);
}

}  // namespace qt::tui
