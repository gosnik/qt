#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input_decoder.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace qt::tui {

/// Reusable expandable message component for skill invocations, branch
/// summaries, and compaction summaries.
///
/// Renders a label with collapsed/expanded state:
/// - Collapsed: label + summary hint (e.g. "Press enter to expand")
/// - Expanded: label + full markdown/text content
class ExpandableMessageComponent final : public Component, public InputHandler {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;
    using ExpandCallback = std::function<void()>;

    /// Construct an expandable message.
    /// @p tag Display tag (e.g. "[skill]", "[branch]", "[compaction]")
    /// @p collapsed_text Text shown when collapsed (e.g. skill name)
    /// @p expanded_content Full content shown when expanded (e.g. markdown)
    /// @p expanded_header Optional header prepended to expanded content
    explicit ExpandableMessageComponent(
        std::string tag,
        std::string collapsed_text = {},
        std::string expanded_content = {},
        std::string expanded_header = {},
        ColorFunction label_color = {},
        ColorFunction text_color = {},
        ColorFunction dim_color = {},
        ColorFunction background_color = {});

    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const noexcept;

    void set_collapsed_text(std::string text);
    void set_expanded_content(std::string content, std::string header = {});
    void set_on_expand_toggle(ExpandCallback callback);

    void handle_input(std::string_view bytes) override;

    [[nodiscard]] const std::string& tag() const noexcept;
    [[nodiscard]] const std::string& collapsed_text() const noexcept;
    [[nodiscard]] const std::string& expanded_content() const noexcept;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    void rebuild();

    std::string tag_;
    std::string collapsed_text_;
    std::string expanded_content_;
    std::string expanded_header_;
    ColorFunction label_color_;
    ColorFunction text_color_;
    ColorFunction dim_color_;
    ColorFunction background_color_;
    bool expanded_ = false;
    ExpandCallback on_expand_toggle_;

    std::shared_ptr<Box> root_;
};

// ---- Specialized wrappers following the reference component patterns ----

/// Skill invocation message with skill name and content.
class SkillInvocationComponent final : public Component, public InputHandler {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;
    using ExpandCallback = std::function<void()>;

    explicit SkillInvocationComponent(
        std::string skill_name,
        std::string skill_content = {},
        ColorFunction label_color = {},
        ColorFunction text_color = {},
        ColorFunction dim_color = {},
        ColorFunction background_color = {});

    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const noexcept;
    void set_on_expand_toggle(ExpandCallback callback);
    void set_skill_content(std::string content);

    void handle_input(std::string_view bytes) override;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    ExpandableMessageComponent inner_;
};

/// Branch summary message.
class BranchSummaryComponent final : public Component, public InputHandler {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;
    using ExpandCallback = std::function<void()>;

    explicit BranchSummaryComponent(
        std::string summary = {},
        ColorFunction label_color = {},
        ColorFunction text_color = {},
        ColorFunction dim_color = {},
        ColorFunction background_color = {});

    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const noexcept;
    void set_on_expand_toggle(ExpandCallback callback);
    void set_summary(std::string summary);

    void handle_input(std::string_view bytes) override;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    ExpandableMessageComponent inner_;
};

/// Compaction summary message.
class CompactionSummaryComponent final : public Component, public InputHandler {
  public:
    using ColorFunction = std::function<std::string(std::string_view)>;
    using ExpandCallback = std::function<void()>;

    explicit CompactionSummaryComponent(
        std::size_t tokens_before = 0,
        std::string summary = {},
        ColorFunction label_color = {},
        ColorFunction text_color = {},
        ColorFunction dim_color = {},
        ColorFunction background_color = {});

    void set_expanded(bool expanded);
    [[nodiscard]] bool expanded() const noexcept;
    void set_on_expand_toggle(ExpandCallback callback);
    void set_summary(std::string summary);

    void handle_input(std::string_view bytes) override;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    void rebuild_inner();

    ExpandableMessageComponent inner_;
    std::size_t tokens_before_ = 0;
};

}