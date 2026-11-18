#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/render/markdown.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace qt::tui {

struct CustomMessageRenderState {
  std::string custom_type;
  std::string text;
  bool expanded = false;
};

class CustomMessageComponent final : public Component, public InputHandler {
 public:
  using ColorFunction = std::function<std::string(std::string_view)>;
  using Renderer = std::function<ComponentPtr(const CustomMessageRenderState&)>;

  CustomMessageComponent(std::string custom_type,
                         std::string text,
                         Renderer renderer = {},
                         ColorFunction label_color = {},
                         ColorFunction text_color = {},
                         ColorFunction dim_color = {},
                         ColorFunction background_color = {});

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override;
  void handle_input(std::string_view bytes) override;

  void set_expanded(bool expanded);
  [[nodiscard]] bool expanded() const noexcept;
  void set_text(std::string text);
  [[nodiscard]] const std::string& custom_type() const noexcept;
  [[nodiscard]] const std::string& text() const noexcept;

 private:
  void rebuild();

  std::string custom_type_;
  std::string text_;
  Renderer renderer_;
  ColorFunction label_color_;
  ColorFunction text_color_;
  ColorFunction dim_color_;
  ColorFunction background_color_;
  bool expanded_ = false;
  std::shared_ptr<Box> box_;
  ComponentPtr rendered_;
};

}  // namespace qt::tui
