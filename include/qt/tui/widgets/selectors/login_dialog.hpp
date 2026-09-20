#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

class LoginDialogComponent final : public Component, public Focusable {
 public:
  using CompleteCallback = std::function<void(bool success, std::string message)>;

  LoginDialogComponent(std::string provider_id,
                       std::string provider_name,
                       CompleteCallback on_complete,
                       std::string title = {});

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override;
  void handle_input(std::string_view bytes) override;
  void set_focused(bool focused) override;
  [[nodiscard]] bool focused() const noexcept override;

  void show_auth(std::string url, std::string instructions = {});
  void show_prompt(std::string message, std::string placeholder = {},
                   Input::SubmitCallback on_submit = {});
  void show_manual_input(std::string prompt, Input::SubmitCallback on_submit = {});
  void show_info(std::vector<std::string> lines);
  void show_waiting(std::string message);
  void show_progress(std::string message);
  void cancel();

  [[nodiscard]] Input& input() noexcept;
  [[nodiscard]] const std::string& provider_id() const noexcept;

 private:
  void rebuild();
  void set_content(std::vector<std::string> lines);

  std::string provider_id_;
  std::string provider_name_;
  std::string title_;
  CompleteCallback on_complete_;
  std::vector<std::string> content_lines_;
  std::string prompt_message_;
  std::string placeholder_;
  std::shared_ptr<Input> input_;
  std::shared_ptr<Box> box_;
  bool focused_ = false;
  bool prompting_ = false;
};

}  // namespace qt::tui
