#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/render/markdown.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace qt::tui {

class UserMessageComponent final : public Component {
 public:
  using ColorFunction = std::function<std::string(std::string_view)>;

  explicit UserMessageComponent(std::string text,
                                ColorFunction text_color = {},
                                ColorFunction background_color = {},
                                std::string prefix = {});

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override;

  void set_text(std::string text);
  [[nodiscard]] const std::string& text() const noexcept;

 private:
  void rebuild();

  std::string text_;
  ColorFunction text_color_;
  ColorFunction background_color_;
  std::string prefix_;
  std::shared_ptr<Box> box_;
};

}  // namespace qt::tui
