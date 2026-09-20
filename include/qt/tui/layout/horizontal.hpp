#pragma once

#include "qt/tui/core/component.hpp"

#include <vector>

namespace qt::tui {

class HorizontalLayout final : public Component {
  public:
    HorizontalLayout() = default;
    explicit HorizontalLayout(std::vector<ComponentPtr> children);

    void add_child(ComponentPtr child);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    std::vector<ComponentPtr> children_;
};

}  // namespace qt::tui
