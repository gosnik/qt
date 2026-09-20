#pragma once

#include "qt/ai/models.hpp"
#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qt::tui {

/// Rich rendering of a complete assistant message with thinking blocks,
/// text content (via Markdown), tool call indicators, and error states.
///
/// Follows the reference assistant-message.ts component: text content renders
/// through Markdown, thinking blocks are collapsible, and tool call presence
/// suppresses OSC133 zone markers.
///
/// Stop reason and error info are provided separately since the native
/// ai::Message struct does not carry these fields directly (they live in
/// CompletionResponse and StreamEvent).
class AssistantMessageComponent final : public Component {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;

    /// Construct from an assistant message. May be updated later via update_content().
    explicit AssistantMessageComponent(
        const ai::Message* message = nullptr,
        bool hide_thinking = false,
        std::string hidden_thinking_label = "Thinking...",
        ColorFunction thinking_color = {},
        ColorFunction error_color = {});

    /// Replace the displayed message content. Rebuilds internal child layout.
    void update_content(const ai::Message& message);

    /// Set stop reason and optional error message for error/abort rendering.
    void set_stop_info(ai::StopReason reason, std::string error_message = {});

    /// Toggle thinking block visibility.
    void set_hide_thinking(bool hide);
    [[nodiscard]] bool hide_thinking() const noexcept;

    /// Set the label shown when thinking blocks are hidden.
    void set_hidden_thinking_label(std::string label);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    void rebuild();

    const ai::Message* message_ = nullptr;
    bool hide_thinking_ = false;
    std::string hidden_thinking_label_;
    ColorFunction thinking_color_;
    ColorFunction error_color_;
    bool has_tool_calls_ = false;
    ai::StopReason stop_reason_ = ai::StopReason::unknown;
    std::string error_message_;

    std::shared_ptr<Box> root_;
};

}
