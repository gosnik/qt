#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/widgets/loader.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qt::tui {

/// Rich rendering of a bash command execution with streaming output.
///
/// Follows the reference bash-execution.ts component: command header with $
/// prefix, streaming output accumulation, completion status with exit code,
/// and expand/collapse for long output.
class BashExecutionComponent final : public Component, public InputHandler {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;
    using ExpandCallback = std::function<void()>;

    /// Maximum preview lines when collapsed.
    static constexpr std::size_t PREVIEW_LINES = 20;

    /// Construct for a bash command. @p exclude_from_context dims the border
    /// for !! commands that are excluded from LLM context.
    explicit BashExecutionComponent(
        std::string command,
        bool exclude_from_context = false,
        ColorFunction border_color = {},
        ColorFunction header_color = {},
        ColorFunction output_color = {},
        ColorFunction error_color = {},
        ColorFunction warning_color = {});

    /// Append streaming output chunks. Strips ANSI and normalizes line endings.
    void append_output(std::string chunk);

    /// Mark the command as complete with exit code and optional cancellation.
    void set_complete(int exit_code, bool cancelled = false);

    /// Toggle expanded view (full output vs. preview only).
    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const noexcept;

    /// Set callback for when user requests expand/collapse.
    void set_on_expand_toggle(ExpandCallback callback);

    void handle_input(std::string_view bytes) override;

    [[nodiscard]] const std::string& command() const noexcept;
    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] bool is_complete() const noexcept;
    [[nodiscard]] int exit_code() const noexcept;
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] const std::vector<std::string>& output_lines() const noexcept;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    void rebuild_content();

    std::string command_;
    std::vector<std::string> output_lines_;
    enum class Status { running, complete, cancelled, error };
    Status status_ = Status::running;
    int exit_code_ = 0;
    bool expanded_ = false;
    bool exclude_from_context_;
    ColorFunction border_color_;
    ColorFunction header_color_;
    ColorFunction output_color_;
    ColorFunction error_color_;
    ColorFunction warning_color_;
    ExpandCallback on_expand_toggle_;

    std::shared_ptr<Box> root_;
    std::shared_ptr<Loader> loader_;
};

}
