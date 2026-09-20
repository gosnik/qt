#include "qt/tui/widgets/extension/extension_editor.hpp"

#include <qt/tui/input/keybinding_hints.hpp>
#include <qt/tui/core/style.hpp>

#include <memory>
#include <string>

namespace qt::tui {

ExtensionEditorComponent::ExtensionEditorComponent(
    std::string title,
    std::string prefill,
    SubmitCallback on_submit,
    CancelCallback on_cancel)
    : title_(std::move(title)),
      on_submit_(std::move(on_submit)),
      on_cancel_(std::move(on_cancel)),
      editor_(std::make_shared<Editor>(KeybindingManager{})),
      box_(std::make_shared<Box>(1, 0)) {
  // Wire up the editor submit callback.
  editor_->set_submit_callback([this](const std::string& text) {
    if (on_submit_) {
      on_submit_(text);
    }
  });

  if (!prefill.empty()) {
    editor_->set_text(std::move(prefill));
  }

  // Build the layout.
  box_->add_child(std::make_shared<DynamicBorder>());
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(std::make_shared<Text>(style::bold(title_)));
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(editor_);
  box_->add_child(std::make_shared<Spacer>(1));

  const std::string hints =
      key_hint("tui.select.confirm", "submit") + "  " +
      key_hint("tui.input.newLine", "newline") + "  " +
      key_hint("tui.select.cancel", "cancel");
  box_->add_child(std::make_shared<Text>(style::dim(hints)));
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(std::make_shared<DynamicBorder>());
}

void ExtensionEditorComponent::set_focused(bool value) noexcept {
  focused_ = value;
  if (editor_) {
    editor_->set_focused(value);
  }
}

void ExtensionEditorComponent::handle_input(std::string_view bytes) {
  const std::string key_data(bytes);
  if (key_data == "\x1b" || key_data == "\x03") {
    if (on_cancel_) {
      on_cancel_();
    }
    return;
  }
  if (editor_) {
    editor_->handle_input(key_data);
  }
}

}  // namespace qt::tui
