#pragma once

#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input_decoder.hpp"

#include <functional>
#include <string>

namespace qt::tui {

/// A Text component that switches between collapsed and expanded text on toggle.
///
/// Follows the reference ExpandableText class. Useful for startup headers
/// and keybinding hint sections.
class ExpandableText final : public Component, public InputHandler {
  public:
    using TextProvider = std::function<std::string()>;
    using ExpandCallback = std::function<void()>;

    /// Construct with text providers for collapsed and expanded states.
    explicit ExpandableText(
        TextProvider collapsed_text,
        TextProvider expanded_text,
        bool expanded = false,
        std::size_t padding_x = 0,
        std::size_t padding_y = 0);

    /// Construct with static strings (convenience overload).
    explicit ExpandableText(
        std::string collapsed_text,
        std::string expanded_text,
        bool expanded = false,
        std::size_t padding_x = 0,
        std::size_t padding_y = 0);

    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const noexcept;

    void set_on_expand_toggle(ExpandCallback callback);

    void handle_input(std::string_view bytes) override;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    TextProvider collapsed_text_;
    TextProvider expanded_text_;
    bool expanded_;
    std::size_t padding_x_;
    std::size_t padding_y_;
    ExpandCallback on_expand_toggle_;
};

}