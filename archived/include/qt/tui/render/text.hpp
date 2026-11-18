#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace qt::tui {

using TextStyleFunction = std::function<std::string(std::string_view)>;

class Text final : public Component {
  public:
    explicit Text(std::string text = "", std::size_t padding_x = 1,
                  std::size_t padding_y = 1, TextStyleFunction background = {});

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void set_text(std::string text);
    void set_padding(std::size_t padding_x, std::size_t padding_y) noexcept;
    void set_background(TextStyleFunction background);
    [[nodiscard]] const std::string& text() const noexcept;
    [[nodiscard]] std::size_t padding_x() const noexcept;
    [[nodiscard]] std::size_t padding_y() const noexcept;

  private:
    std::string text_;
    std::size_t padding_x_ = 1;
    std::size_t padding_y_ = 1;
    TextStyleFunction background_;
    struct RenderCache {
        RenderConstraints constraints;
        RenderBuffer buffer;
    };
    mutable std::optional<RenderCache> render_cache_;
};

}  // namespace qt::tui
