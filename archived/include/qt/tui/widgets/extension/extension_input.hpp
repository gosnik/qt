#pragma once

#include <qt/tui/layout/box.hpp>
#include <qt/tui/core/component.hpp>
#include <qt/tui/widgets/countdown_timer.hpp>
#include <qt/tui/layout/dynamic_border.hpp>
#include <qt/tui/input/input.hpp>
#include <qt/tui/layout/spacer.hpp>
#include <qt/tui/render/text.hpp>
#include <functional>
#include <memory>
#include <string>

namespace qt::tui {

// Simple text input overlay for extension-provided input.
// Composes DynamicBorder + Input + keybinding hints + optional countdown timer.
// Matches the reference ExtensionInputComponent API.
class ExtensionInputComponent : public Component, public InputHandler {
 public:
  using SubmitCallback = std::function<void(std::string)>;
  using CancelCallback = std::function<void()>;

  struct Options {
    std::int64_t timeout_ms;
    Options() : timeout_ms(0) {}
    explicit Options(std::int64_t ms) : timeout_ms(ms) {}
  };

  ExtensionInputComponent(std::string title,
                          SubmitCallback on_submit,
                          CancelCallback on_cancel,
                          const Options& options = Options{});

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override {
    return box_->render(constraints);
  }

  // Focus propagation to the inner input.
  [[nodiscard]] bool focused() const noexcept { return focused_; }
  void set_focused(bool value) noexcept;

  // Input routing.
  void handle_input(std::string_view bytes) override;

  // Access the inner input for value manipulation.
  [[nodiscard]] Input& input() noexcept { return *input_; }
  [[nodiscard]] const Input& input() const noexcept { return *input_; }

  // Dispose the countdown timer (must be called before destruction).
  void dispose();

 private:

  std::string base_title_;
  SubmitCallback on_submit_;
  CancelCallback on_cancel_;
  std::shared_ptr<Input> input_;
  std::shared_ptr<Text> title_text_;
  std::shared_ptr<Box> box_;
  bool focused_ = false;
  std::shared_ptr<CountdownTimer> countdown_;
};

}  // namespace qt::tui
