#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <string>

namespace qt::tui {

class TruncatedText final : public Component {
  public:
    explicit TruncatedText(std::string text, std::size_t padding_x = 0,
                           std::size_t padding_y = 0, std::string ellipsis = "");

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    std::string text_;
    std::size_t padding_x_ = 0;
    std::size_t padding_y_ = 0;
    std::string ellipsis_;
};

}  // namespace qt::tui
