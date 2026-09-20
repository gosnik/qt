#pragma once

#include "qt/tui/core/component.hpp"

namespace qt::tui {

struct BorderStyle {
    char top_left = '+';
    char top_right = '+';
    char bottom_left = '+';
    char bottom_right = '+';
    char horizontal = '-';
    char vertical = '|';
};

class Border final : public Component {
  public:
    explicit Border(ComponentPtr child, BorderStyle style = {});

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    ComponentPtr child_;
    BorderStyle style_;
};

}  // namespace qt::tui
