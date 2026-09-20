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

/// Rich rendering of a tool execution with status, arguments, and results.
///
/// Follows the reference tool-execution.ts component: shows tool name,
/// arguments summary, execution status, and expandable result content.
class ToolExecutionComponent final : public Component, public InputHandler {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;
    using ExpandCallback = std::function<void()>;

    /// Maximum preview lines for result when collapsed.
    static constexpr std::size_t PREVIEW_LINES = 10;

    enum class Status {
        pending,
        running,
        complete,
        error,
    };

    /// Construct for a tool execution.
    explicit ToolExecutionComponent(
        std::string tool_name,
        std::string tool_call_id,
        std::string args_summary = {},
        ColorFunction title_color = {},
        ColorFunction args_color = {},
        ColorFunction result_color = {},
        ColorFunction error_color = {},
        ColorFunction background_color = {});

    /// Mark execution as started.
    void set_running();

    /// Set the result content.
    void set_result(std::string text, bool is_error = false);

    /// Append to result content (for streaming).
    void append_result(std::string chunk);

    /// Set pending, success, and error background styles.
    void set_background_colors(ColorFunction pending, ColorFunction success,
                               ColorFunction error);

    /// Toggle expanded view.
    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const noexcept;

    /// Set callback for expand/collapse toggle.
    void set_on_expand_toggle(ExpandCallback callback);

    void handle_input(std::string_view bytes) override;

    [[nodiscard]] const std::string& tool_name() const noexcept;
    [[nodiscard]] const std::string& tool_call_id() const noexcept;
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] bool is_error() const noexcept;
    [[nodiscard]] const std::string& result_text() const noexcept;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    void mark_dirty() const noexcept;
    void rebuild_content() const;

    std::string tool_name_;
    std::string tool_call_id_;
    std::string args_summary_;
    std::string result_text_;
    Status status_ = Status::pending;
    bool is_error_ = false;
    bool expanded_ = false;
    ColorFunction title_color_;
    ColorFunction args_color_;
    ColorFunction result_color_;
    ColorFunction error_color_;
    ColorFunction background_color_;
    ColorFunction success_background_color_;
    ColorFunction error_background_color_;
    ExpandCallback on_expand_toggle_;

    mutable std::shared_ptr<Box> root_;
    mutable std::shared_ptr<Loader> loader_;
    mutable bool dirty_ = true;
};

}
