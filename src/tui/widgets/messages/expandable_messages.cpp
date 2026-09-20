#include "qt/tui/widgets/messages/expandable_messages.hpp"

#include "qt/tui/render/markdown.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <string>
#include <utility>

namespace qt::tui {

namespace {

std::string default_label_color(std::string_view text) {
    return std::string("\x1b[1m") + std::string(text) + "\x1b[22m";
}

std::string default_text_color(std::string_view text) {
    return std::string(text);
}

std::string default_dim_color(std::string_view text) {
    return std::string("\x1b[90m") + std::string(text) + "\x1b[0m";
}

std::string default_bg_color(std::string_view text) {
    return std::string(text);
}

} // namespace

// ---- ExpandableMessageComponent ----

ExpandableMessageComponent::ExpandableMessageComponent(
    std::string tag,
    std::string collapsed_text,
    std::string expanded_content,
    std::string expanded_header,
    ColorFunction label_color,
    ColorFunction text_color,
    ColorFunction dim_color,
    ColorFunction background_color)
    : tag_(std::move(tag)),
      collapsed_text_(std::move(collapsed_text)),
      expanded_content_(std::move(expanded_content)),
      expanded_header_(std::move(expanded_header)),
      label_color_(label_color ? std::move(label_color) : default_label_color),
      text_color_(text_color ? std::move(text_color) : default_text_color),
      dim_color_(dim_color ? std::move(dim_color) : default_dim_color),
      background_color_(background_color ? std::move(background_color) : default_bg_color),
      root_(std::make_shared<Box>(1, 1, background_color_)) {
    rebuild();
}

void ExpandableMessageComponent::set_expanded(bool expanded) {
    if (expanded_ != expanded) {
        expanded_ = expanded;
        rebuild();
    }
}

bool ExpandableMessageComponent::expanded() const noexcept { return expanded_; }

void ExpandableMessageComponent::set_collapsed_text(std::string text) {
    collapsed_text_ = std::move(text);
    if (!expanded_) rebuild();
}

void ExpandableMessageComponent::set_expanded_content(
    std::string content, std::string header) {
    expanded_content_ = std::move(content);
    if (!header.empty()) expanded_header_ = std::move(header);
    if (expanded_) rebuild();
}

void ExpandableMessageComponent::set_on_expand_toggle(ExpandCallback callback) {
    on_expand_toggle_ = std::move(callback);
}

void ExpandableMessageComponent::handle_input(std::string_view bytes) {
    if (bytes == "\r" || bytes == "\n" || bytes == " ") {
        expanded_ = !expanded_;
        if (on_expand_toggle_) on_expand_toggle_();
        rebuild();
    }
}

const std::string& ExpandableMessageComponent::tag() const noexcept { return tag_; }
const std::string& ExpandableMessageComponent::collapsed_text() const noexcept {
    return collapsed_text_;
}
const std::string& ExpandableMessageComponent::expanded_content() const noexcept {
    return expanded_content_;
}

void ExpandableMessageComponent::rebuild() {
    root_->clear();

    // Label
    std::string label = label_color_(tag_);
    root_->add_child(std::make_shared<Text>(std::move(label), 0, 0));

    if (expanded_) {
        // Full content
        root_->add_child(std::make_shared<Spacer>(1));
        std::string full_text;
        if (!expanded_header_.empty()) {
            full_text = expanded_header_ + "\n\n";
        }
        full_text += expanded_content_;
        if (text_color_) {
            full_text = text_color_(full_text);
        }
        root_->add_child(std::make_shared<Text>(std::move(full_text), 0, 0));
    } else {
        // Collapsed hint
        std::string hint;
        if (!collapsed_text_.empty()) {
            hint = text_color_(collapsed_text_) + " ";
        }
        hint += dim_color_("(press enter to expand)");
        root_->add_child(std::make_shared<Text>(std::move(hint), 0, 0));
    }
}

foundation::Result<RenderBuffer> ExpandableMessageComponent::render(
    const RenderConstraints& constraints) const {
    return root_->render(constraints);
}

// ---- SkillInvocationComponent ----

SkillInvocationComponent::SkillInvocationComponent(
    std::string skill_name,
    std::string skill_content,
    ColorFunction label_color,
    ColorFunction text_color,
    ColorFunction dim_color,
    ColorFunction background_color)
    : inner_(" [skill]",
             std::move(skill_name),
             std::move(skill_content),
             {}, // header is set dynamically
             label_color ? label_color : default_label_color,
             text_color ? text_color : default_text_color,
             dim_color ? dim_color : default_dim_color,
             background_color ? background_color : default_bg_color) {}

void SkillInvocationComponent::set_expanded(bool e) { inner_.set_expanded(e); }
bool SkillInvocationComponent::expanded() const noexcept { return inner_.expanded(); }
void SkillInvocationComponent::set_on_expand_toggle(ExpandCallback cb) {
    inner_.set_on_expand_toggle(std::move(cb));
}
void SkillInvocationComponent::set_skill_content(std::string content) {
    inner_.set_expanded_content(std::move(content));
}
void SkillInvocationComponent::handle_input(std::string_view bytes) {
    inner_.handle_input(bytes);
}
foundation::Result<RenderBuffer> SkillInvocationComponent::render(
    const RenderConstraints& c) const {
    return inner_.render(c);
}

// ---- BranchSummaryComponent ----

BranchSummaryComponent::BranchSummaryComponent(
    std::string summary,
    ColorFunction label_color,
    ColorFunction text_color,
    ColorFunction dim_color,
    ColorFunction background_color)
    : inner_(" [branch]",
             "Branch summary",
             std::move(summary),
             "**Branch Summary**",
             label_color ? label_color : default_label_color,
             text_color ? text_color : default_text_color,
             dim_color ? dim_color : default_dim_color,
             background_color ? background_color : default_bg_color) {}

void BranchSummaryComponent::set_expanded(bool e) { inner_.set_expanded(e); }
bool BranchSummaryComponent::expanded() const noexcept { return inner_.expanded(); }
void BranchSummaryComponent::set_on_expand_toggle(ExpandCallback cb) {
    inner_.set_on_expand_toggle(std::move(cb));
}
void BranchSummaryComponent::set_summary(std::string summary) {
    inner_.set_expanded_content(std::move(summary));
}
void BranchSummaryComponent::handle_input(std::string_view bytes) {
    inner_.handle_input(bytes);
}
foundation::Result<RenderBuffer> BranchSummaryComponent::render(
    const RenderConstraints& c) const {
    return inner_.render(c);
}

// ---- CompactionSummaryComponent ----

CompactionSummaryComponent::CompactionSummaryComponent(
    std::size_t tokens_before,
    std::string summary,
    ColorFunction label_color,
    ColorFunction text_color,
    ColorFunction dim_color,
    ColorFunction background_color)
    : inner_(" [compaction]",
             {},
             std::move(summary),
             {},
             label_color ? label_color : default_label_color,
             text_color ? text_color : default_text_color,
             dim_color ? dim_color : default_dim_color,
             background_color ? background_color : default_bg_color),
      tokens_before_(tokens_before) {
    rebuild_inner();
}

void CompactionSummaryComponent::rebuild_inner() {
    auto token_str = std::to_string(tokens_before_);
    inner_.set_collapsed_text("Compacted from " + token_str + " tokens");
    // Update the expanded header
    inner_.set_expanded_content(
        inner_.expanded_content(),
        "**Compacted from " + token_str + " tokens**");
}

void CompactionSummaryComponent::set_expanded(bool e) { inner_.set_expanded(e); }
bool CompactionSummaryComponent::expanded() const noexcept { return inner_.expanded(); }
void CompactionSummaryComponent::set_on_expand_toggle(ExpandCallback cb) {
    inner_.set_on_expand_toggle(std::move(cb));
}
void CompactionSummaryComponent::set_summary(std::string summary) {
    auto token_str = std::to_string(tokens_before_);
    inner_.set_expanded_content(
        std::move(summary),
        "**Compacted from " + token_str + " tokens**");
}
void CompactionSummaryComponent::handle_input(std::string_view bytes) {
    inner_.handle_input(bytes);
}
foundation::Result<RenderBuffer> CompactionSummaryComponent::render(
    const RenderConstraints& c) const {
    return inner_.render(c);
}

}