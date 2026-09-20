#pragma once

#include "qt/foundation/error.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/terminal/terminal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qt::tui {

struct RowUpdate {
    CursorPosition cursor;
    std::string text;
};

struct FrameDiff {
    std::size_t width = 0;
    std::size_t height = 0;
    std::size_t previous_viewport_top = 0;
    std::size_t viewport_top = 0;
    std::size_t rows_scanned = 0;
    bool full_redraw = false;
    bool clear_scrollback = false;
    std::vector<std::uint32_t> deleted_image_ids;
    std::vector<RowUpdate> updates;
    std::optional<CursorPosition> cursor_position;
};

struct DiffRendererNormalizedRowCacheEntry {
    bool valid = false;
    std::size_t width = 0;
    std::string raw;
    std::string normalized;
};

class DiffRenderer {
  public:
    [[nodiscard]] bool has_previous_frame() const noexcept;
    [[nodiscard]] const RenderBuffer* previous_frame() const noexcept;

    [[nodiscard]] FrameDiff diff(const RenderBuffer& next) const;
    [[nodiscard]] FrameDiff present(RenderBuffer next);
    [[nodiscard]] foundation::Result<FrameDiff> render(
        const Component& component, const RenderConstraints& constraints);

    void reset() noexcept;

  private:
    std::optional<RenderBuffer> previous_;
    mutable std::vector<DiffRendererNormalizedRowCacheEntry> previous_row_cache_;
    mutable std::vector<DiffRendererNormalizedRowCacheEntry> next_row_cache_;
    std::size_t previous_width_ = 0;
    std::size_t previous_height_ = 0;
    std::size_t previous_viewport_top_ = 0;
    std::size_t max_lines_rendered_ = 0;
};

[[nodiscard]] std::string ansi_move_cursor(CursorPosition cursor);
[[nodiscard]] SynchronizedOutput synchronized_output_for_diff(const FrameDiff& frame_diff);

}  // namespace qt::tui
