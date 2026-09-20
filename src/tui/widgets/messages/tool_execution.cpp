#include "qt/tui/widgets/messages/tool_execution.hpp"

#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace qt::tui {

ToolExecutionComponent::ToolExecutionComponent(
    std::string tool_name,
    std::string tool_call_id,
    std::string args_summary,
    ColorFunction title_color,
    ColorFunction args_color,
    ColorFunction result_color,
    ColorFunction error_color,
    ColorFunction background_color)
    : tool_name_(std::move(tool_name)),
      tool_call_id_(std::move(tool_call_id)),
      args_summary_(std::move(args_summary)),
      title_color_(std::move(title_color)),
      args_color_(std::move(args_color)),
      result_color_(std::move(result_color)),
      error_color_(std::move(error_color)),
      background_color_(std::move(background_color)),
      root_(std::make_shared<Box>(1, 0, background_color_)),
      loader_(std::make_shared<Loader>("Running...")) {
    mark_dirty();
}

void ToolExecutionComponent::set_running() {
    status_ = Status::running;
    mark_dirty();
}

void ToolExecutionComponent::set_result(std::string text, bool is_error) {
    result_text_ = std::move(text);
    is_error_ = is_error;
    status_ = is_error ? Status::error : Status::complete;
    mark_dirty();
}

void ToolExecutionComponent::append_result(std::string chunk) {
    result_text_ += chunk;
    mark_dirty();
}

void ToolExecutionComponent::set_background_colors(ColorFunction pending,
                                                   ColorFunction success,
                                                   ColorFunction error) {
    background_color_ = std::move(pending);
    success_background_color_ = std::move(success);
    error_background_color_ = std::move(error);
    mark_dirty();
}

void ToolExecutionComponent::set_expanded(bool expanded) {
    if (expanded_ != expanded) {
        expanded_ = expanded;
        mark_dirty();
    }
}

bool ToolExecutionComponent::expanded() const noexcept { return expanded_; }

void ToolExecutionComponent::set_on_expand_toggle(ExpandCallback callback) {
    on_expand_toggle_ = std::move(callback);
}

void ToolExecutionComponent::handle_input(std::string_view bytes) {
    if (bytes == "\r" || bytes == "\n" || bytes == " ") {
        expanded_ = !expanded_;
        if (on_expand_toggle_) {
            on_expand_toggle_();
        }
        mark_dirty();
    }
}

const std::string& ToolExecutionComponent::tool_name() const noexcept {
    return tool_name_;
}

const std::string& ToolExecutionComponent::tool_call_id() const noexcept {
    return tool_call_id_;
}

ToolExecutionComponent::Status ToolExecutionComponent::status() const noexcept {
    return status_;
}

bool ToolExecutionComponent::is_error() const noexcept { return is_error_; }

const std::string& ToolExecutionComponent::result_text() const noexcept {
    return result_text_;
}

void ToolExecutionComponent::mark_dirty() const noexcept { dirty_ = true; }

void ToolExecutionComponent::rebuild_content() const {
    dirty_ = false;
    if (is_error_ && error_background_color_) {
        root_->set_background(error_background_color_);
    } else if (status_ == Status::complete && success_background_color_) {
        root_->set_background(success_background_color_);
    } else {
        root_->set_background(background_color_);
    }

    root_->clear();

    // Tool title
    std::string title = tool_name_;
    if (title_color_) {
        title = title_color_(title);
    }
    root_->add_child(std::make_shared<Text>(std::move(title), 1, 0));

    // Args summary (if provided)
    if (!args_summary_.empty()) {
        std::string args = args_summary_;
        if (args_color_) {
            args = args_color_(args);
        }
        root_->add_child(std::make_shared<Text>(std::move(args), 1, 0));
    }

    // Content based on status
    if (status_ == Status::running && result_text_.empty()) {
        root_->add_child(loader_);
    } else if (!result_text_.empty()) {
        // Determine which lines to show
        std::istringstream stream(result_text_);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }

        std::vector<std::string> display_lines;
        std::size_t hidden_count = 0;

        if (expanded_) {
            display_lines = lines;
        } else {
            if (lines.size() > PREVIEW_LINES) {
                hidden_count = lines.size() - PREVIEW_LINES;
                display_lines.assign(lines.end() - PREVIEW_LINES, lines.end());
            } else {
                display_lines = lines;
            }
        }

        // Render result lines
        std::string result_block;
        for (const auto& l : display_lines) {
            if (!result_block.empty()) {
                result_block += '\n';
            }
            result_block += l;
        }

        if (is_error_ && error_color_) {
            result_block = error_color_(result_block);
        } else if (result_color_) {
            result_block = result_color_(result_block);
        }

        root_->add_child(std::make_shared<Text>(std::move(result_block), 1, 0));

        // Expand/collapse hint
        if (hidden_count > 0 && !expanded_) {
            std::string more_text = "... " + std::to_string(hidden_count) +
                                    " more lines (press enter to expand)";
            root_->add_child(std::make_shared<Text>(std::move(more_text), 1, 0));
        } else if (expanded_ && lines.size() > PREVIEW_LINES) {
            root_->add_child(
                std::make_shared<Text>("(press enter to collapse)", 1, 0));
        }

        // Error indicator
        if (is_error_) {
            std::string error_label = "(error)";
            if (error_color_) {
                error_label = error_color_(error_label);
            }
            root_->add_child(std::make_shared<Text>(std::move(error_label), 1, 0));
        }
    }
}

foundation::Result<RenderBuffer> ToolExecutionComponent::render(
    const RenderConstraints& constraints) const {
    if (dirty_) {
        rebuild_content();
    }
    if (constraints.max_height == 0) {
        return make_buffer({}, constraints);
    }

    const std::size_t vertical_spacing =
        constraints.max_height >= 3U ? 2U : constraints.max_height > 1U ? 1U : 0U;
    const std::size_t body_height =
        constraints.max_height > vertical_spacing
            ? constraints.max_height - vertical_spacing
            : constraints.max_height;
    auto rendered = root_->render(RenderConstraints{constraints.max_width, body_height});
    if (!rendered) {
        return rendered.error();
    }

    std::vector<std::string> lines;
    lines.reserve(std::min<std::size_t>(
        constraints.max_height, rendered.value().lines.size() + vertical_spacing));
    if (constraints.max_height >= 2U) {
        lines.emplace_back();
    }
    for (auto& line : rendered.value().lines) {
        if (lines.size() >= constraints.max_height) {
            break;
        }
        lines.push_back(std::move(line));
    }
    if (constraints.max_height >= 3U && lines.size() < constraints.max_height) {
        lines.emplace_back();
    }
    return make_buffer(std::move(lines), constraints);
}

}
