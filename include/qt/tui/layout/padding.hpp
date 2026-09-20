#pragma once

#include "qt/tui/core/component.hpp"

namespace qt::tui {

struct Insets {
    std::size_t top = 0;
    std::size_t right = 0;
    std::size_t bottom = 0;
    std::size_t left = 0;

    static Insets uniform(std::size_t value) {
        return Insets{value, value, value, value};
    }
};

class Padding final : public Component {
  public:
    Padding(ComponentPtr child, Insets insets);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    ComponentPtr child_;
    Insets insets_;
};

}  // namespace qt::tui
