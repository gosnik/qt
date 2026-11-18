#pragma once

#include "qt/tui/core/component.hpp"

#include <functional>
#include <string>

namespace qt::tui {

class DynamicBorder final : public Component {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;

    explicit DynamicBorder(ColorFunction color = {});
    void set_color(ColorFunction color);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    ColorFunction color_;
};

}  // namespace qt::tui
