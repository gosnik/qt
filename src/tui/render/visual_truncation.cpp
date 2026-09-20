#include "qt/tui/render/visual_truncation.hpp"
#include "qt/tui/render/text.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace qt::tui {

VisualTruncationResult truncate_to_visual_lines(std::string_view text,
                                                 std::size_t max_visual_lines,
                                                 std::size_t width,
                                                 std::size_t padding_x) {
    VisualTruncationResult result;
    if (text.empty()) {
        return result;
    }

    // Use Text component to get all visual lines
    qt::tui::Text temp_text(std::string(text), padding_x, 0);
    RenderConstraints constraints{width, std::numeric_limits<std::size_t>::max()};
    auto rendered = temp_text.render(constraints);
    if (!rendered) {
        return result;
    }

    const auto& all_lines = rendered.value().lines;
    if (all_lines.size() <= max_visual_lines) {
        result.visual_lines = all_lines;
        result.skipped_count = 0;
        return result;
    }

    // Take the last N visual lines
    result.skipped_count = all_lines.size() - max_visual_lines;
    result.visual_lines.assign(all_lines.end() - static_cast<std::ptrdiff_t>(max_visual_lines),
                                all_lines.end());
    return result;
}

}  // namespace qt::tui
