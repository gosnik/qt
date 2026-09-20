#include "qt/tui/widgets/messages/bash_execution.hpp"

#include "qt/tui/layout/dynamic_border.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace qt::tui {

namespace {

/// Strip ANSI escape sequences from a string.
std::string strip_ansi(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            // Skip CSI sequence
            i += 2;
            while (i < text.size()) {
                unsigned char ch = static_cast<unsigned char>(text[i]);
                if (ch >= 0x40 && ch <= 0x7e) {
                    ++i;
                    break;
                }
                ++i;
            }
        } else if (text[i] == '\x1b' && i + 1 < text.size() &&
                   (text[i + 1] == ']' || text[i + 1] == '_' || text[i + 1] == 'G')) {
            // Skip OSC/APC sequence until ST or BEL
            i += 2;
            while (i < text.size()) {
                if (text[i] == '\x07') {
                    ++i;
                    break;
                }
                if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '\\') {
                    i += 2;
                    break;
                }
                ++i;
            }
        } else {
            result.push_back(text[i]);
            ++i;
        }
    }
    return result;
}

/// Normalize line endings: \r\n and \r become \n.
std::string normalize_line_endings(std::string text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            result.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i; // skip \n after \r
            }
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

} // namespace

BashExecutionComponent::BashExecutionComponent(
    std::string command,
    bool exclude_from_context,
    ColorFunction border_color,
    ColorFunction header_color,
    ColorFunction output_color,
    ColorFunction error_color,
    ColorFunction warning_color)
    : command_(std::move(command)),
      exclude_from_context_(exclude_from_context),
      border_color_(std::move(border_color)),
      header_color_(std::move(header_color)),
      output_color_(std::move(output_color)),
      error_color_(std::move(error_color)),
      warning_color_(std::move(warning_color)),
      root_(std::make_shared<Box>(0, 0)),
      loader_(std::make_shared<Loader>("Running...")) {
    rebuild_content();
}

void BashExecutionComponent::append_output(std::string chunk) {
    auto clean = normalize_line_endings(strip_ansi(chunk));

    // Split into lines manually to preserve trailing empty lines
    std::vector<std::string> new_lines;
    std::size_t start = 0;
    while (start <= clean.size()) {
        auto pos = clean.find('\n', start);
        if (pos == std::string::npos) {
            new_lines.push_back(clean.substr(start));
            break;
        }
        new_lines.push_back(clean.substr(start, pos - start));
        start = pos + 1;
    }

    if (new_lines.empty()) return;

    if (output_lines_.empty()) {
        output_lines_ = std::move(new_lines);
    } else {
        // Append first new line to last existing line (continuation)
        output_lines_.back() += new_lines.front();
        // Append remaining lines
        for (std::size_t i = 1; i < new_lines.size(); ++i) {
            output_lines_.push_back(std::move(new_lines[i]));
        }
    }

    rebuild_content();
}

void BashExecutionComponent::set_complete(int exit_code, bool cancelled) {
    exit_code_ = exit_code;
    if (cancelled) {
        status_ = Status::cancelled;
    } else if (exit_code != 0) {
        status_ = Status::error;
    } else {
        status_ = Status::complete;
    }
    rebuild_content();
}

void BashExecutionComponent::set_expanded(bool expanded) {
    if (expanded_ != expanded) {
        expanded_ = expanded;
        rebuild_content();
    }
}

bool BashExecutionComponent::expanded() const noexcept { return expanded_; }

void BashExecutionComponent::set_on_expand_toggle(ExpandCallback callback) {
    on_expand_toggle_ = std::move(callback);
}

void BashExecutionComponent::handle_input(std::string_view bytes) {
    // Toggle expand/collapse on enter or space
    if (bytes == "\r" || bytes == "\n" || bytes == " ") {
        expanded_ = !expanded_;
        if (on_expand_toggle_) {
            on_expand_toggle_();
        }
        rebuild_content();
    }
}

const std::string& BashExecutionComponent::command() const noexcept {
    return command_;
}

bool BashExecutionComponent::is_running() const noexcept {
    return status_ == Status::running;
}

bool BashExecutionComponent::is_complete() const noexcept {
    return status_ != Status::running;
}

int BashExecutionComponent::exit_code() const noexcept { return exit_code_; }

bool BashExecutionComponent::cancelled() const noexcept {
    return status_ == Status::cancelled;
}

const std::vector<std::string>& BashExecutionComponent::output_lines() const noexcept {
    return output_lines_;
}

void BashExecutionComponent::rebuild_content() {
    root_->clear();

    auto border_fn = border_color_;
    if (!border_fn && exclude_from_context_) {
        border_fn = [](std::string_view s) { return std::string(s); };
    }

    // Spacer
    root_->add_child(std::make_shared<Spacer>(1));

    // Top border
    root_->add_child(std::make_shared<DynamicBorder>(border_fn));

    // Command header
    std::string header_text = "$ " + command_;
    if (header_color_) {
        header_text = header_color_(header_text);
    }
    root_->add_child(std::make_shared<Text>(std::move(header_text), 1, 0));

    // Output lines
    if (!output_lines_.empty()) {
        // Determine which lines to show
        std::vector<std::string> display_lines;
        std::size_t hidden_count = 0;

        if (expanded_) {
            display_lines = output_lines_;
        } else {
            // Show last PREVIEW_LINES
            if (output_lines_.size() > PREVIEW_LINES) {
                hidden_count = output_lines_.size() - PREVIEW_LINES;
                display_lines.assign(
                    output_lines_.end() - PREVIEW_LINES,
                    output_lines_.end());
            } else {
                display_lines = output_lines_;
            }
        }

        // Render output as a single text block
        std::string output_text;
        for (const auto& line : display_lines) {
            if (!output_text.empty()) {
                output_text += '\n';
            }
            output_text += line;
        }
        if (output_color_) {
            output_text = output_color_(output_text);
        }
        root_->add_child(std::make_shared<Text>(std::move(output_text), 1, 0));

        // Hidden line count / expand hint
        if (hidden_count > 0 && !expanded_) {
            std::string more_text = "... " + std::to_string(hidden_count) + " more lines (press enter to expand)";
            if (warning_color_) {
                more_text = warning_color_(more_text);
            }
            root_->add_child(std::make_shared<Text>(std::move(more_text), 1, 0));
        } else if (expanded_ && output_lines_.size() > PREVIEW_LINES) {
            std::string collapse_text = "(press enter to collapse)";
            if (warning_color_) {
                collapse_text = warning_color_(collapse_text);
            }
            root_->add_child(std::make_shared<Text>(std::move(collapse_text), 1, 0));
        }
    }

    // Loader or status
    if (status_ == Status::running) {
        root_->add_child(loader_);
    } else {
        std::string status_text;
        if (status_ == Status::cancelled) {
            status_text = "(cancelled)";
            if (warning_color_) {
                status_text = warning_color_(status_text);
            }
        } else if (status_ == Status::error) {
            status_text = "(exit " + std::to_string(exit_code_) + ")";
            if (error_color_) {
                status_text = error_color_(status_text);
            }
        }
        if (!status_text.empty()) {
            root_->add_child(std::make_shared<Text>(std::move(status_text), 1, 0));
        }
    }

    // Bottom border
    root_->add_child(std::make_shared<DynamicBorder>(border_fn));
}

foundation::Result<RenderBuffer> BashExecutionComponent::render(
    const RenderConstraints& constraints) const {
    return root_->render(constraints);
}

}
