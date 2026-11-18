#include "qt/tui/widgets/messages/assistant_message.hpp"

#include "qt/tui/render/markdown.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <variant>

namespace qt::tui {

namespace {

// OSC133 zone markers for shell integration (iTerm2, WezTerm, etc.)
constexpr std::string_view OSC133_ZONE_START = "\x1b]133;A\x07";
constexpr std::string_view OSC133_ZONE_END   = "\x1b]133;B\x07";
constexpr std::string_view OSC133_ZONE_FINAL = "\x1b]133;C\x07";

bool has_visible_text(const ai::Message& msg) {
    for (const auto& content : msg.content) {
        if (auto* tc = std::get_if<ai::TextContent>(&content)) {
            if (!tc->text.empty() &&
                std::any_of(tc->text.begin(), tc->text.end(),
                            [](unsigned char c) { return !std::isspace(c); })) {
                return true;
            }
        }
        if (auto* th = std::get_if<ai::ThinkingContent>(&content)) {
            if (!th->text.empty() &&
                std::any_of(th->text.begin(), th->text.end(),
                            [](unsigned char c) { return !std::isspace(c); })) {
                return true;
            }
        }
    }
    return false;
}

bool has_tool_calls(const ai::Message& msg) {
    return std::any_of(msg.content.begin(), msg.content.end(),
                       [](const auto& c) {
                           return std::holds_alternative<ai::ToolCallContent>(c);
                       });
}

/// Check if there is visible text or thinking content after index @p after.
bool has_visible_content_after(const ai::Message& msg, std::size_t after) {
    for (std::size_t i = after + 1; i < msg.content.size(); ++i) {
        const auto& c = msg.content[i];
        if (auto* tc = std::get_if<ai::TextContent>(&c)) {
            if (!tc->text.empty() &&
                std::any_of(tc->text.begin(), tc->text.end(),
                            [](unsigned char ch) { return !std::isspace(ch); })) {
                return true;
            }
        }
        if (auto* th = std::get_if<ai::ThinkingContent>(&c)) {
            if (!th->text.empty() &&
                std::any_of(th->text.begin(), th->text.end(),
                            [](unsigned char ch) { return !std::isspace(ch); })) {
                return true;
            }
        }
    }
    return false;
}

/// Trim leading/trailing whitespace from a string.
std::string trimmed(std::string_view s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) return {};
    auto end = s.find_last_not_of(" \t\n\r");
    return std::string(s.substr(start, end - start + 1));
}

} // namespace

AssistantMessageComponent::AssistantMessageComponent(
    const ai::Message* message,
    bool hide_thinking,
    std::string hidden_thinking_label,
    ColorFunction thinking_color,
    ColorFunction error_color)
    : message_(message ? std::optional<ai::Message>(*message) : std::nullopt),
      hide_thinking_(hide_thinking),
      hidden_thinking_label_(std::move(hidden_thinking_label)),
      thinking_color_(std::move(thinking_color)),
      error_color_(std::move(error_color)),
      root_(std::make_shared<Box>(0, 0)) {
    if (message_) {
        rebuild();
    }
}

void AssistantMessageComponent::update_content(const ai::Message& message) {
    message_ = message;
    rebuild();
}

void AssistantMessageComponent::set_stop_info(
    ai::StopReason reason, std::string error_message) {
    stop_reason_ = reason;
    error_message_ = std::move(error_message);
    if (message_) {
        rebuild();
    }
}

void AssistantMessageComponent::set_hide_thinking(bool hide) {
    if (hide_thinking_ != hide) {
        hide_thinking_ = hide;
        if (message_) {
            rebuild();
        }
    }
}

bool AssistantMessageComponent::hide_thinking() const noexcept {
    return hide_thinking_;
}

void AssistantMessageComponent::set_hidden_thinking_label(std::string label) {
    hidden_thinking_label_ = std::move(label);
    if (message_ && hide_thinking_) {
        rebuild();
    }
}

void AssistantMessageComponent::rebuild() {
    root_->clear();

    if (!message_) return;

    const auto& msg = *message_;
    content_renderers_.resize(msg.content.size());
    content_renderer_thinking_.resize(msg.content.size());
    has_tool_calls_ = has_tool_calls(msg);
    const bool visible = has_visible_text(msg);

    if (visible) {
        root_->add_child(std::make_shared<Spacer>(1));
    }

    for (std::size_t i = 0; i < msg.content.size(); ++i) {
        const auto& content = msg.content[i];

        if (auto* tc = std::get_if<ai::TextContent>(&content)) {
            const auto text = trimmed(tc->text);
            if (!text.empty()) {
                auto& md = content_renderers_[i];
                if (content_renderer_thinking_[i]) md.reset();
                content_renderer_thinking_[i] = false;
                if (!md) md = std::make_shared<Markdown>(text, 1, 0);
                else if (md->text() != text) md->set_text(text);
                root_->add_child(md);
            }
        } else if (auto* th = std::get_if<ai::ThinkingContent>(&content)) {
            const auto text = trimmed(th->text);
            if (!text.empty()) {
                const bool has_after = has_visible_content_after(msg, i);

                if (hide_thinking_) {
                    auto label = hidden_thinking_label_;
                    if (thinking_color_) {
                        label = thinking_color_(label);
                    }
                    root_->add_child(std::make_shared<Text>(
                        std::move(label), 1, 0));
                } else {
                    // Render thinking text with dimmed/italic styling
                    auto& md = content_renderers_[i];
                    if (!content_renderer_thinking_[i]) md.reset();
                    content_renderer_thinking_[i] = true;
                    if (!md) {
                        md = std::make_shared<Markdown>(text, 1, 0);
                        if (thinking_color_) md->set_background(thinking_color_);
                    } else if (md->text() != text) {
                        md->set_text(text);
                    }
                    root_->add_child(md);
                }

                if (has_after) {
                    root_->add_child(std::make_shared<Spacer>(1));
                }
            }
        }
        // ToolCallContent and ToolResultContent are handled by
        // ToolExecutionComponent; we only track their presence here.
        else {
            content_renderers_[i].reset();
        }
    }

    // Error/abort states (only when no tool calls are pending)
    if (!has_tool_calls_) {
        if (stop_reason_ == ai::StopReason::aborted) {
            if (visible) {
                root_->add_child(std::make_shared<Spacer>(1));
            }
            std::string abort_msg = "Operation aborted";
            if (error_color_) {
                abort_msg = error_color_(abort_msg);
            }
            root_->add_child(std::make_shared<Text>(std::move(abort_msg), 1, 0));
        } else if (stop_reason_ == ai::StopReason::error) {
            std::string error_msg = "Error: Unknown error";
            if (!error_message_.empty()) {
                error_msg = "Error: " + error_message_;
            }
            if (error_color_) {
                error_msg = error_color_(error_msg);
            }
            root_->add_child(std::make_shared<Spacer>(1));
            root_->add_child(std::make_shared<Text>(std::move(error_msg), 1, 0));
        }
    }
}

foundation::Result<RenderBuffer> AssistantMessageComponent::render(
    const RenderConstraints& constraints) const {
    auto result = root_->render(constraints);
    if (!result.ok()) {
        return result;
    }

    // Wrap with OSC133 zone markers for terminal integration
    // (only when there are no tool calls, matching the reference behavior)
    if (!has_tool_calls_ && !result.value().lines.empty()) {
        auto& lines = result.value().lines;
        lines.front().insert(0, OSC133_ZONE_START);
        lines.back().append(OSC133_ZONE_END);
        lines.back().append(OSC133_ZONE_FINAL);
    }

    return result;
}

}
