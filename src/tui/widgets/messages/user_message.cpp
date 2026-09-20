#include "qt/tui/widgets/messages/user_message.hpp"

#include <utility>

namespace qt::tui {
namespace {

constexpr std::string_view kOsc133ZoneStart = "\x1b]133;A\x07";
constexpr std::string_view kOsc133ZoneEnd = "\x1b]133;B\x07";
constexpr std::string_view kOsc133ZoneFinal = "\x1b]133;C\x07";

std::string identity(std::string_view text) { return std::string(text); }

}  // namespace

UserMessageComponent::UserMessageComponent(std::string text,
                                           ColorFunction text_color,
                                           ColorFunction background_color,
                                           std::string prefix)
    : text_(std::move(text)),
      text_color_(text_color ? std::move(text_color) : identity),
      background_color_(background_color ? std::move(background_color) : identity),
      prefix_(std::move(prefix)),
      box_(std::make_shared<Box>(1, 1, background_color_)) {
  rebuild();
}

foundation::Result<RenderBuffer> UserMessageComponent::render(
    const RenderConstraints& constraints) const {
  auto rendered = box_->render(constraints);
  if (!rendered || rendered.value().lines.empty()) {
    return rendered;
  }

  auto& lines = rendered.value().lines;
  lines.front().insert(0, std::string(kOsc133ZoneStart));
  lines.back().insert(0, std::string(kOsc133ZoneEnd) +
                             std::string(kOsc133ZoneFinal));
  return rendered;
}

void UserMessageComponent::set_text(std::string text) {
  text_ = std::move(text);
  rebuild();
}

const std::string& UserMessageComponent::text() const noexcept { return text_; }

void UserMessageComponent::rebuild() {
  box_->clear();
  const std::string rendered_text =
      prefix_.empty() ? text_ : prefix_ + text_;
  box_->add_child(std::make_shared<Markdown>(rendered_text, 0, 0, text_color_));
}

}  // namespace qt::tui
