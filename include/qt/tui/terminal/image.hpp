#pragma once

#include "qt/tui/core/component.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace qt::tui {

struct ImageOptions {
    std::optional<std::size_t> max_width_cells;
    std::optional<std::size_t> max_height_cells;
    std::string filename;
    std::optional<std::uint32_t> image_id;
};

class Image final : public Component {
  public:
    Image(std::string base64_data, std::string mime_type, ImageOptions options = {},
          std::optional<ImageDimensions> dimensions = {});

    [[nodiscard]] std::optional<std::uint32_t> image_id() const noexcept;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    [[nodiscard]] std::uint32_t ensure_image_id() const;

    std::string base64_data_;
    std::string mime_type_;
    ImageOptions options_;
    ImageDimensions dimensions_;
    mutable std::optional<std::uint32_t> image_id_;
};

}  // namespace qt::tui
