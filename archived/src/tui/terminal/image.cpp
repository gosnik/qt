#include "qt/tui/terminal/image.hpp"

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace qt::tui {

namespace {

std::uint32_t allocate_image_id() {
    static std::random_device random_device;
    static std::mt19937 generator(random_device());
    static std::uniform_int_distribution<std::uint32_t> distribution(
        1U, std::numeric_limits<std::uint32_t>::max());
    return distribution(generator);
}

}  // namespace

Image::Image(std::string base64_data, std::string mime_type, ImageOptions options,
             std::optional<ImageDimensions> dimensions)
    : base64_data_(std::move(base64_data)),
      mime_type_(std::move(mime_type)),
      options_(std::move(options)),
      dimensions_(dimensions.value_or(parse_image_dimensions(base64_data_, mime_type_)
                                          .value_or(ImageDimensions{800, 600}))),
      image_id_(options_.image_id) {}

std::optional<std::uint32_t> Image::image_id() const noexcept { return image_id_; }

std::uint32_t Image::ensure_image_id() const {
    if (!image_id_) {
        image_id_ = allocate_image_id();
    }
    return *image_id_;
}

foundation::Result<RenderBuffer> Image::render(const RenderConstraints& constraints) const {
    const auto capabilities = terminal_capabilities();
    const auto cell_dimensions = terminal_cell_dimensions();
    const RenderCacheKey cache_key{constraints, capabilities, cell_dimensions};
    const auto cache_key_matches = [](const RenderCacheKey& left,
                                      const RenderCacheKey& right) noexcept {
        return left.constraints.max_width == right.constraints.max_width &&
               left.constraints.max_height == right.constraints.max_height &&
               left.capabilities.images == right.capabilities.images &&
               left.capabilities.true_color == right.capabilities.true_color &&
               left.capabilities.hyperlinks == right.capabilities.hyperlinks &&
               left.cell_dimensions.width_px == right.cell_dimensions.width_px &&
               left.cell_dimensions.height_px == right.cell_dimensions.height_px;
    };
    if (cached_render_ && cache_key_matches(cached_render_->key, cache_key)) {
        return cached_render_->buffer;
    }

    const std::size_t width_minus_margin =
        constraints.max_width > 2U ? constraints.max_width - 2U : constraints.max_width;
    const std::size_t max_width =
        std::min(width_minus_margin, options_.max_width_cells.value_or(60U));

    if (capabilities.images != ImageProtocol::none && max_width > 0U) {
        TerminalImageRenderOptions render_options;
        render_options.max_width_cells = max_width;
        render_options.move_cursor = false;
        render_options.preserve_aspect_ratio = true;
        if (capabilities.images == ImageProtocol::kitty) {
            render_options.image_id = ensure_image_id();
        } else {
            render_options.image_id = image_id_;
        }

        auto rendered = render_terminal_image(base64_data_, dimensions_, render_options);
        if (rendered) {
            if (rendered->image_id) {
                image_id_ = rendered->image_id;
            }
            std::size_t rows = rendered->rows;
            if (options_.max_height_cells) {
                rows = std::min(rows, *options_.max_height_cells);
            }
            if (constraints.max_height > 0U) {
                rows = std::min(rows, constraints.max_height);
            }
            rows = std::max<std::size_t>(1, rows);

            std::vector<std::string> lines;
            lines.reserve(rows);
            for (std::size_t index = 1; index < rows; ++index) {
                lines.emplace_back();
            }
            const std::size_t row_offset = rows - 1U;
            std::string image_line;
            if (row_offset > 0U) {
                image_line = "\x1b[" + std::to_string(row_offset) + "A";
            }
            image_line += rendered->sequence;
            if (capabilities.images == ImageProtocol::kitty && row_offset > 0U) {
                image_line += "\x1b[" + std::to_string(row_offset) + "B";
            }
            lines.push_back(std::move(image_line));
            auto buffer = make_buffer(std::move(lines), constraints);
            if (buffer) {
                cached_render_ = CachedRender{cache_key, buffer.value()};
            }
            return buffer;
        }
    }

    auto buffer = make_buffer({terminal_image_fallback(mime_type_, dimensions_, options_.filename)},
                              constraints);
    if (buffer) {
        cached_render_ = CachedRender{cache_key, buffer.value()};
    }
    return buffer;
}

}  // namespace qt::tui
