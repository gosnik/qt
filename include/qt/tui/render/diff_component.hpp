#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

/// Renders diff text with colored lines and intra-line change highlighting.
///
/// Follows the reference diff.ts renderDiff() function:
/// - Context lines (space prefix): dim/gray
/// - Removed lines (- prefix): red with inverse on changed tokens
/// - Added lines (+ prefix): green with inverse on changed tokens
/// - Pairs of removed+added lines get word-level intra-line diff highlighting
class DiffComponent final : public Component {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;

    /// Construct with optional color functions for diff regions.
    explicit DiffComponent(
        std::string diff_text = {},
        ColorFunction removed_color = {},
        ColorFunction added_color = {},
        ColorFunction context_color = {},
        ColorFunction inverse_color = {});

    void set_diff_text(std::string diff_text);
    [[nodiscard]] const std::string& diff_text() const noexcept;

    /// Render the diff with colors applied.
    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    struct ParsedLine {
        char prefix = ' ';  // '-', '+', or ' '
        std::string line_num;
        std::string content;
    };

    /// Parse a diff line into prefix, line number, and content.
    static std::optional<ParsedLine> parse_diff_line(std::string_view line);

    /// Replace tabs with spaces for consistent rendering.
    static std::string replace_tabs(std::string text);

    /// Compute word-level intra-line diff between removed and added content.
    struct IntraLineResult {
        std::string removed_line;
        std::string added_line;
    };

    [[nodiscard]] IntraLineResult render_intra_line_diff(
        std::string_view old_content,
        std::string_view new_content) const;

    /// Render the full diff to a single colored string.
    [[nodiscard]] std::string render_colored() const;

    std::string diff_text_;
    ColorFunction removed_color_;
    ColorFunction added_color_;
    ColorFunction context_color_;
    ColorFunction inverse_color_;
    mutable std::optional<std::string> cached_colored_;
    mutable std::string cached_diff_text_;
};

}
