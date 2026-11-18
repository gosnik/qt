#include "qt/tui/widgets/expandable_text.hpp"

#include "qt/tui/render/text.hpp"

#include <utility>

namespace qt::tui {

ExpandableText::ExpandableText(
    TextProvider collapsed_text,
    TextProvider expanded_text,
    bool expanded,
    std::size_t padding_x,
    std::size_t padding_y)
    : collapsed_text_(std::move(collapsed_text)),
      expanded_text_(std::move(expanded_text)),
      expanded_(expanded),
      padding_x_(padding_x),
      padding_y_(padding_y) {}

ExpandableText::ExpandableText(
    std::string collapsed_text,
    std::string expanded_text,
    bool expanded,
    std::size_t padding_x,
    std::size_t padding_y)
    : collapsed_text_([t = std::move(collapsed_text)]() { return t; }),
      expanded_text_([t = std::move(expanded_text)]() { return t; }),
      expanded_(expanded),
      padding_x_(padding_x),
      padding_y_(padding_y) {}

void ExpandableText::set_expanded(bool expanded) {
    expanded_ = expanded;
}

bool ExpandableText::expanded() const noexcept { return expanded_; }

void ExpandableText::set_on_expand_toggle(ExpandCallback callback) {
    on_expand_toggle_ = std::move(callback);
}

void ExpandableText::handle_input(std::string_view bytes) {
    if (bytes == "\r" || bytes == "\n" || bytes == " ") {
        expanded_ = !expanded_;
        if (on_expand_toggle_) {
            on_expand_toggle_();
        }
    }
}

foundation::Result<RenderBuffer> ExpandableText::render(
    const RenderConstraints& constraints) const {
    const auto& text = expanded_ ? expanded_text_() : collapsed_text_();
    Text component(text, padding_x_, padding_y_);
    return component.render(constraints);
}

}