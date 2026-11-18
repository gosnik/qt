#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>

namespace qt::tui {

class Spacer final : public Component {
  public:
    explicit Spacer(std::size_t lines = 1);

    void set_lines(std::size_t lines) noexcept;
    [[nodiscard]] std::size_t lines() const noexcept;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    std::size_t lines_ = 1;
};

}  // namespace qt::tui
