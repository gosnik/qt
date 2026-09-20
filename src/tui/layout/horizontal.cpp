#include "qt/tui/layout/horizontal.hpp"

#include <string>
#include <utility>
#include <vector>

namespace qt::tui {

HorizontalLayout::HorizontalLayout(std::vector<ComponentPtr> children)
    : children_(std::move(children)) {}

void HorizontalLayout::add_child(ComponentPtr child) {
    if (child) {
        children_.push_back(std::move(child));
    }
}

foundation::Result<RenderBuffer> HorizontalLayout::render(
    const RenderConstraints& constraints) const {
    std::vector<RenderBuffer> rendered_children;
    rendered_children.reserve(children_.size());

    std::size_t remaining_width = constraints.max_width;
    std::size_t child_slots = 0;
    for (const auto& child : children_) {
        if (child) {
            ++child_slots;
        }
    }

    for (const auto& child : children_) {
        if (!child) {
            continue;
        }

        const std::size_t width =
            child_slots == 0 ? 0 : remaining_width / child_slots;
        auto rendered = child->render(RenderConstraints{width, constraints.max_height});
        if (!rendered) {
            return rendered.error();
        }

        remaining_width -= width;
        --child_slots;
        rendered_children.push_back(std::move(rendered.value()));
    }

    std::vector<std::string> lines;
    lines.reserve(constraints.max_height);
    std::vector<std::string> blanks;
    blanks.reserve(rendered_children.size());
    for (const auto& child : rendered_children) {
        auto blank = clamp_line("", child.width);
        if (!blank) {
            return blank.error();
        }
        blanks.push_back(std::move(blank.value()));
    }

    for (std::size_t row = 0; row < constraints.max_height; ++row) {
        std::string line;
        for (std::size_t index = 0; index < rendered_children.size(); ++index) {
            const auto& child = rendered_children[index];
            if (row < child.lines.size()) {
                line += child.lines[row];
                continue;
            }

            line += blanks[index];
        }

        lines.push_back(std::move(line));
    }

    return make_buffer(std::move(lines), constraints);
}

}  // namespace qt::tui
