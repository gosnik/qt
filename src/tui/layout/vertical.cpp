#include "qt/tui/layout/vertical.hpp"

#include <utility>
#include <vector>

namespace qt::tui {

VerticalLayout::VerticalLayout(std::vector<ComponentPtr> children)
    : children_(std::move(children)) {}

void VerticalLayout::add_child(ComponentPtr child) {
    if (child) {
        children_.push_back(std::move(child));
    }
}

foundation::Result<RenderBuffer> VerticalLayout::render(
    const RenderConstraints& constraints) const {
    std::vector<std::string> lines;
    lines.reserve(constraints.max_height);

    for (const auto& child : children_) {
        if (!child) {
            continue;
        }
        if (lines.size() >= constraints.max_height) {
            break;
        }

        RenderConstraints child_constraints = constraints;
        child_constraints.max_height = constraints.max_height - lines.size();
        auto rendered = child->render(child_constraints);
        if (!rendered) {
            return rendered.error();
        }

        for (const auto& line : rendered.value().lines) {
            if (lines.size() >= constraints.max_height) {
                break;
            }
            lines.push_back(line);
        }
    }

    return make_buffer(std::move(lines), constraints);
}

}  // namespace qt::tui
