#include "qt/tui/widgets/selectors/login_dialog.hpp"

#include "qt/tui/layout/dynamic_border.hpp"
#include "qt/tui/input/keybinding_hints.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <utility>

namespace qt::tui {

LoginDialogComponent::LoginDialogComponent(std::string provider_id,
                                           std::string provider_name,
                                           CompleteCallback on_complete,
                                           std::string title)
    : provider_id_(std::move(provider_id)),
      provider_name_(std::move(provider_name)),
      title_(title.empty() ? "Login to " + provider_name_ : std::move(title)),
      on_complete_(std::move(on_complete)),
      input_(std::make_shared<Input>()),
      box_(std::make_shared<Box>(0, 0)) {
  input_->set_escape_callback([this]() { cancel(); });
  rebuild();
}

foundation::Result<RenderBuffer> LoginDialogComponent::render(
    const RenderConstraints& constraints) const {
  return box_->render(constraints);
}

void LoginDialogComponent::handle_input(std::string_view bytes) {
  if (get_keybindings_const().matches(bytes, "tui.select.cancel")) {
    cancel();
    return;
  }
  if (prompting_) {
    input_->handle_input(bytes);
  }
}

void LoginDialogComponent::set_focused(bool focused) {
  focused_ = focused;
  input_->set_focused(focused);
}

bool LoginDialogComponent::focused() const noexcept { return focused_; }

void LoginDialogComponent::show_auth(std::string url, std::string instructions) {
  std::vector<std::string> lines;
  lines.push_back(terminal_hyperlink("Open authentication URL", url));
  lines.push_back("Ctrl+click to open");
  if (!instructions.empty()) {
    lines.push_back(std::move(instructions));
  }
  prompting_ = false;
  set_content(std::move(lines));
}

void LoginDialogComponent::show_prompt(std::string message,
                                       std::string placeholder,
                                       Input::SubmitCallback on_submit) {
  prompt_message_ = std::move(message);
  placeholder_ = std::move(placeholder);
  input_->set_value("");
  input_->set_submit_callback(std::move(on_submit));
  prompting_ = true;
  rebuild();
}

void LoginDialogComponent::show_manual_input(std::string prompt,
                                             Input::SubmitCallback on_submit) {
  show_prompt(std::move(prompt), {}, std::move(on_submit));
}

void LoginDialogComponent::show_info(std::vector<std::string> lines) {
  prompting_ = false;
  set_content(std::move(lines));
}

void LoginDialogComponent::show_waiting(std::string message) {
  content_lines_.push_back(std::move(message));
  prompting_ = false;
  rebuild();
}

void LoginDialogComponent::show_progress(std::string message) {
  content_lines_.push_back(std::move(message));
  rebuild();
}

void LoginDialogComponent::cancel() {
  prompting_ = false;
  auto callback = on_complete_;
  if (callback) {
    callback(false, "Login cancelled");
  }
}

Input& LoginDialogComponent::input() noexcept { return *input_; }

const std::string& LoginDialogComponent::provider_id() const noexcept {
  return provider_id_;
}

void LoginDialogComponent::set_content(std::vector<std::string> lines) {
  content_lines_ = std::move(lines);
  rebuild();
}

void LoginDialogComponent::rebuild() {
  box_->clear();
  box_->add_child(std::make_shared<DynamicBorder>());
  box_->add_child(std::make_shared<Text>(title_, 1, 0));
  box_->add_child(std::make_shared<Spacer>(1));
  for (const auto& line : content_lines_) {
    box_->add_child(std::make_shared<Text>(line, 1, 0));
  }
  if (prompting_) {
    if (!prompt_message_.empty()) {
      box_->add_child(std::make_shared<Text>(prompt_message_, 1, 0));
    }
    if (!placeholder_.empty()) {
      box_->add_child(std::make_shared<Text>("e.g., " + placeholder_, 1, 0));
    }
    box_->add_child(input_);
    box_->add_child(std::make_shared<Text>(
        key_hint("tui.select.cancel", "cancel") + "  " +
            key_hint("tui.select.confirm", "submit"),
        1, 0));
  } else {
    box_->add_child(std::make_shared<Text>(
        key_hint("tui.select.cancel", "close"), 1, 0));
  }
  box_->add_child(std::make_shared<DynamicBorder>());
}

}  // namespace qt::tui
