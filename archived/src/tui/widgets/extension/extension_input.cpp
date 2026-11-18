#include "qt/tui/widgets/extension/extension_input.hpp"

#include <qt/tui/input/keybinding_hints.hpp>
#include <qt/tui/core/style.hpp>
#include <memory>
#include <string>

namespace qt::tui {

ExtensionInputComponent::ExtensionInputComponent(
    std::string title,
    SubmitCallback on_submit,
    CancelCallback on_cancel,
    const Options& options)
    :
      base_title_(std::move(title)),
      on_submit_(std::move(on_submit)),
      on_cancel_(std::move(on_cancel)),
      input_(std::make_shared<Input>(KeybindingManager{})),
      title_text_(std::make_shared<Text>(style::bold(base_title_))),
      box_(std::make_shared<Box>(1, 0)) {
  input_->set_submit_callback([this](const std::string& value) {
    if (on_submit_) {
      on_submit_(value);
    }
  });

  // Build the layout.
  box_->add_child(std::make_shared<DynamicBorder>());
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(title_text_);
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(input_);
  box_->add_child(std::make_shared<Spacer>(1));

  const std::string hints =
      key_hint("tui.select.confirm", "submit") + "  " +
      key_hint("tui.select.cancel", "cancel");
  box_->add_child(std::make_shared<Text>(style::dim(hints)));
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(std::make_shared<DynamicBorder>());

  // Optional countdown timer.
  if (options.timeout_ms > 0) {
    countdown_ = std::make_shared<CountdownTimer>(
        static_cast<std::size_t>(options.timeout_ms),
        [this](std::size_t remaining_seconds) {
          title_text_->set_text(style::bold(base_title_ + " (" +
                                            std::to_string(remaining_seconds) +
                                            "s)"));
        },
        [this]() {
          if (on_cancel_) {
            on_cancel_();
          }
        });
  }
}

void ExtensionInputComponent::set_focused(bool value) noexcept {
  focused_ = value;
  if (input_) {
    input_->set_focused(value);
  }
}

void ExtensionInputComponent::handle_input(std::string_view bytes) {
  // Enter to submit.
  if (bytes == "\r" || bytes == "\n") {
    if (on_submit_) {
      on_submit_(input_->value());
    }
    return;
  }

  // Escape or Ctrl+C to cancel.
  if (bytes == "\x1b" || bytes == "\x03") {
    if (on_cancel_) {
      on_cancel_();
    }
    return;
  }

  // Forward everything else to the input.
  if (input_) {
    input_->handle_input(bytes);
  }
}

void ExtensionInputComponent::dispose() {
  if (countdown_) {
    countdown_->dispose();
    countdown_.reset();
  }
}

}  // namespace qt::tui
