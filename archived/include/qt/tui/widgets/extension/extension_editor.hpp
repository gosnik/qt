#pragma once

#include <qt/tui/layout/box.hpp>
#include <qt/tui/core/component.hpp>
#include <qt/tui/layout/dynamic_border.hpp>
#include <qt/tui/input/editor.hpp>
#include <qt/tui/layout/spacer.hpp>
#include <qt/tui/render/text.hpp>

#include <functional>
#include <memory>
#include <string>

namespace qt::tui {

// Multi-line editor overlay for extension-provided editing.
// Composes DynamicBorder + Editor + keybinding hints.
// Matches the reference ExtensionEditorComponent API.
class ExtensionEditorComponent : public Component, public InputHandler {
 public:
  using SubmitCallback = std::function<void(std::string)>;
  using CancelCallback = std::function<void()>;

  ExtensionEditorComponent(std::string title,
                           std::string prefill,
                           SubmitCallback on_submit,
                           CancelCallback on_cancel);

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override {
    return box_->render(constraints);
  }

  // Focus propagation to the inner editor.
  [[nodiscard]] bool focused() const noexcept { return focused_; }
  void set_focused(bool value) noexcept;

  // Input routing.
  void handle_input(std::string_view bytes) override;

  // Access the inner editor for text manipulation.
  [[nodiscard]] Editor& editor() noexcept { return *editor_; }
  [[nodiscard]] const Editor& editor() const noexcept { return *editor_; }

 private:

  std::string title_;
  SubmitCallback on_submit_;
  CancelCallback on_cancel_;
  std::shared_ptr<Editor> editor_;
  std::shared_ptr<Box> box_;
  bool focused_ = false;
};

}  // namespace qt::tui
