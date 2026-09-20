#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace qt::tui {

class Markdown final : public Component {
  public:
    using StyleFunction = std::function<std::string(std::string_view)>;

    explicit Markdown(std::string text, std::size_t padding_x = 0,
                      std::size_t padding_y = 0, StyleFunction background = {});

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void set_text(std::string text);
    void set_padding(std::size_t padding_x, std::size_t padding_y) noexcept;
    void set_background(StyleFunction background);
    [[nodiscard]] const std::string& text() const noexcept;

  private:
    struct RenderCache {
        bool valid = false;
        std::size_t revision = 0;
        std::size_t max_width = 0;
        std::size_t max_height = 0;
        RenderBuffer buffer;
    };

    std::string text_;
    std::size_t padding_x_ = 0;
    std::size_t padding_y_ = 0;
    StyleFunction background_;
    std::size_t revision_ = 0;
    mutable RenderCache render_cache_;
};

}  // namespace qt::tui
