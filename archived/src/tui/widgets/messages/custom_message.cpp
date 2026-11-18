#include "qt/tui/widgets/messages/custom_message.hpp"

#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/core/style.hpp"

#include <utility>

namespace qt::tui {
namespace {

std::string bold(std::string_view text) {
  return style::bold(text);
}

std::string identity(std::string_view text) { return std::string(text); }

std::string dim(std::string_view text) {
  return style::dim(text);
}

}  // namespace

CustomMessageComponent::CustomMessageComponent(
    std::string custom_type,
    std::string text,
    Renderer renderer,
    ColorFunction label_color,
    ColorFunction text_color,
    ColorFunction dim_color,
    ColorFunction background_color)
    : custom_type_(std::move(custom_type)),
      text_(std::move(text)),
      renderer_(std::move(renderer)),
      label_color_(label_color ? std::move(label_color) : bold),
      text_color_(text_color ? std::move(text_color) : identity),
      dim_color_(dim_color ? std::move(dim_color) : dim),
      background_color_(background_color ? std::move(background_color) : identity),
      box_(std::make_shared<Box>(1, 1, background_color_)) {
  rebuild();
}

foundation::Result<RenderBuffer> CustomMessageComponent::render(
    const RenderConstraints& constraints) const {
  return rendered_ ? rendered_->render(constraints) : box_->render(constraints);
}

void CustomMessageComponent::handle_input(std::string_view bytes) {
  const auto& keybindings = get_keybindings_const();
  if (keybindings.matches(bytes, "tui.select.confirm") ||
      keybindings.matches(bytes, "tui.input.submit") || bytes == " ") {
    set_expanded(!expanded_);
  }
}

void CustomMessageComponent::set_expanded(bool expanded) {
  if (expanded_ == expanded) {
    return;
  }
  expanded_ = expanded;
  rebuild();
}

bool CustomMessageComponent::expanded() const noexcept { return expanded_; }

void CustomMessageComponent::set_text(std::string text) {
  text_ = std::move(text);
  rebuild();
}

const std::string& CustomMessageComponent::custom_type() const noexcept {
  return custom_type_;
}

const std::string& CustomMessageComponent::text() const noexcept { return text_; }

void CustomMessageComponent::rebuild() {
  const CustomMessageRenderState state{custom_type_, text_, expanded_};
  if (renderer_) {
    rendered_ = renderer_(state);
    if (rendered_) {
      return;
    }
  }
  rendered_.reset();

  box_->clear();
  box_->add_child(std::make_shared<Text>(
      label_color_("[" + custom_type_ + "]"), 0, 0));
  if (!expanded_) {
    box_->add_child(std::make_shared<Text>(
        dim_color_("press enter to expand"), 0, 0));
    return;
  }
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(std::make_shared<Markdown>(text_, 0, 0, text_color_));
}

}  // namespace qt::tui
