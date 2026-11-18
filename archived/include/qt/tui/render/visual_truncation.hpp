#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct VisualTruncationResult {
    /// The visual lines to display (last N lines)
    std::vector<std::string> visual_lines;
    /// Number of visual lines that were skipped (hidden from top)
    std::size_t skipped_count = 0;
};

/// Truncate text to a maximum number of visual lines (from the end).
/// This accounts for line wrapping based on terminal width.
///
/// @param text The text content (may contain newlines)
/// @param max_visual_lines Maximum number of visual lines to show
/// @param width Terminal/render width
/// @param padding_x Horizontal padding (0 when result will be placed in a Box)
/// @returns The truncated visual lines and count of skipped lines
[[nodiscard]] VisualTruncationResult truncate_to_visual_lines(
    std::string_view text, std::size_t max_visual_lines, std::size_t width,
    std::size_t padding_x = 0);

}  // namespace qt::tui
