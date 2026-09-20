#include "qt/tui/widgets/settings_list.hpp"

#include "qt/foundation/utf8.hpp"
#include "qt/tui/utils/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace qt::tui {

namespace {

std::size_t visible_width(std::string_view text) {
    auto width = foundation::utf8_display_width(text);
    return width ? width.value() : text.size();
}

std::string truncate(std::string_view text, std::size_t width) {
    auto truncated = foundation::utf8_truncate(text, width);
    return truncated ? truncated.value() : std::string(text);
}

std::vector<std::string> wrap_words(std::string_view text, std::size_t width) {
    if (width == 0) {
        return {""};
    }

    std::vector<std::string> lines;
    std::string current;
    std::size_t current_width = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        const auto start = index;
        while (index < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        if (start == index) {
            break;
        }

        std::string word(text.substr(start, index - start));
        const std::size_t word_width = visible_width(word);
        const std::size_t next_width = current_width + (current.empty() ? 0U : 1U) + word_width;
        if (!current.empty() && next_width > width) {
            lines.push_back(std::move(current));
            current.clear();
            current_width = 0;
        }
        if (!current.empty()) {
            current.push_back(' ');
            ++current_width;
        }
        current += word;
        current_width += word_width;
    }

    if (!current.empty()) {
        lines.push_back(std::move(current));
    }
    if (lines.empty()) {
        lines.emplace_back("");
    }
    return lines;
}

}  // namespace

SettingsList::SettingsList(std::vector<SettingItem> items, std::size_t max_visible,
                           SettingsListTheme theme, SettingsListOptions options)
    : items_(std::move(items)),
      max_visible_(std::max<std::size_t>(1, max_visible)),
      theme_(std::move(theme)),
      options_(options),
      keybindings_(get_keybindings_const()) {
    search_input_.set_change_callback([this](const std::string&) { apply_filter(); });
    apply_filter();
}

foundation::Result<RenderBuffer> SettingsList::render(
    const RenderConstraints& constraints) const {
    if (submenu_component_) {
        if (dynamic_cast<const Input*>(submenu_component_.get())) {
            // A bare input otherwise looks like the list's search field and
            // gives no indication which setting is being edited.
            std::vector<std::string> lines;
            const auto* item = selected_item();
            if (item && constraints.max_height >= 2U) {
                lines.push_back("Edit " + item->label);
            }
            auto input = submenu_component_->render(
                RenderConstraints{constraints.max_width, 1U});
            if (!input) {
                return input.error();
            }
            lines.insert(lines.end(), input.value().lines.begin(), input.value().lines.end());
            if (constraints.max_height > lines.size()) {
                lines.push_back("Enter to save · Esc to cancel");
            }
            if (item && constraints.max_height > lines.size()) {
                auto description = wrap_words(item->description, constraints.max_width);
                lines.insert(lines.end(), description.begin(), description.end());
            }
            return make_buffer(std::move(lines), constraints);
        }
        return submenu_component_->render(constraints);
    }

    std::vector<std::string> lines;

    if (options_.enable_search) {
        auto search = search_input_.render(RenderConstraints{constraints.max_width, 1});
        if (!search) {
            return search.error();
        }
        lines.insert(lines.end(), search.value().lines.begin(), search.value().lines.end());
        lines.emplace_back("");
    }

    if (items_.empty()) {
        lines.push_back(theme_.hint_prefix + "  No settings available");
        lines.emplace_back("");
        lines.push_back(theme_.hint_prefix + "  Enter/Space to change · Esc to cancel");
        return make_buffer(std::move(lines), constraints);
    }

    if (filtered_indices_.empty()) {
        lines.push_back(theme_.hint_prefix + "  No matching settings");
        lines.emplace_back("");
        lines.push_back(theme_.hint_prefix +
                        "  Type to search · Enter/Space to change · Esc to cancel");
        return make_buffer(std::move(lines), constraints);
    }

    std::size_t label_width = 0;
    for (const auto& item : items_) {
        label_width = std::max(label_width, visible_width(item.label));
    }
    label_width = std::min<std::size_t>(30, label_width);

    const std::size_t start =
        std::min(selected_index_ > max_visible_ / 2 ? selected_index_ - max_visible_ / 2 : 0,
                 filtered_indices_.size() > max_visible_ ? filtered_indices_.size() - max_visible_ : 0);
    const std::size_t end = std::min(filtered_indices_.size(), start + max_visible_);
    for (std::size_t row = start; row < end; ++row) {
        const auto& item = items_[filtered_indices_[row]];
        std::string label = truncate(item.label, label_width);
        const std::size_t width = visible_width(label);
        if (width < label_width) {
            label.append(label_width - width, ' ');
        }

        const std::string prefix = row == selected_index_ ? theme_.cursor : "  ";
        const std::size_t used = visible_width(prefix) + label_width + 2;
        const std::size_t value_width = constraints.max_width > used ? constraints.max_width - used : 0;
        lines.push_back(prefix + label + "  " + truncate(item.current_value, value_width));
    }

    if (start > 0 || end < filtered_indices_.size()) {
        lines.push_back(theme_.hint_prefix + "  (" +
                        std::to_string(selected_index_ + 1) + "/" +
                        std::to_string(filtered_indices_.size()) + ")");
    }

    if (const auto* item = selected_item(); item && !item->description.empty()) {
        lines.emplace_back("");
        const std::size_t description_width =
            constraints.max_width > 2 ? constraints.max_width - 2 : constraints.max_width;
        for (const auto& line : wrap_words(item->description, description_width)) {
            lines.push_back("  " + line);
        }
    }

    lines.emplace_back("");
    const auto* selected = selected_item();
    const bool editable = selected && (selected->submenu || !selected->values.empty());
    std::string hint = options_.enable_search ? "  Type to search · " : "  ";
    hint += editable ? "Enter/Space to change · " : "Read-only · ";
    lines.push_back(theme_.hint_prefix + hint + "Esc to cancel");
    return make_buffer(std::move(lines), constraints);
}

void SettingsList::handle_input(std::string_view bytes) {
    if (submenu_component_) {
        // Submission/cancellation can close the submenu from inside its input
        // callback. Keep it alive until input dispatch has returned.
        auto submenu = submenu_component_;
        if (auto* input_handler = dynamic_cast<InputHandler*>(submenu.get())) {
            input_handler->handle_input(bytes);
            return;
        }
        if (keybindings_.matches(bytes, "tui.input.cancel") ||
            keybindings_.matches(bytes, "tui.select.cancel")) {
            close_submenu();
        }
        return;
    }

    if (keybindings_.matches(bytes, "tui.select.up") ||
        keybindings_.matches(bytes, "tui.cursor.up")) {
        move_selection(-1);
        return;
    }
    if (keybindings_.matches(bytes, "tui.select.down") ||
        keybindings_.matches(bytes, "tui.cursor.down")) {
        move_selection(1);
        return;
    }
    if (keybindings_.matches(bytes, "tui.select.confirm") ||
        keybindings_.matches(bytes, "tui.input.submit") || bytes == " ") {
        activate_selected();
        return;
    }
    if (keybindings_.matches(bytes, "tui.input.cancel") ||
        keybindings_.matches(bytes, "tui.select.cancel")) {
        if (on_cancel_) {
            on_cancel_();
        }
        return;
    }
    if (options_.enable_search) {
        std::string sanitized;
        sanitized.reserve(bytes.size());
        for (const char ch : bytes) {
            if (ch != ' ') {
                sanitized.push_back(ch);
            }
        }
        if (!sanitized.empty()) {
            search_input_.handle_input(sanitized);
        }
    }
}

void SettingsList::set_focused(bool focused) {
    focused_ = focused;
    search_input_.set_focused(focused && options_.enable_search && !submenu_component_);
    if (auto* focusable = dynamic_cast<Focusable*>(submenu_component_.get())) {
        focusable->set_focused(focused);
    }
}

bool SettingsList::focused() const noexcept { return focused_; }

void SettingsList::set_change_callback(ChangeCallback callback) {
    on_change_ = std::move(callback);
}

void SettingsList::set_cancel_callback(CancelCallback callback) {
    on_cancel_ = std::move(callback);
}

void SettingsList::update_value(const std::string& id, std::string value) {
    for (auto& item : items_) {
        if (item.id == id) {
            item.current_value = std::move(value);
            return;
        }
    }
}

const std::vector<SettingItem>& SettingsList::items() const noexcept { return items_; }

std::vector<SettingItem> SettingsList::visible_items() const {
    std::vector<SettingItem> result;
    result.reserve(filtered_indices_.size());
    for (const auto index : filtered_indices_) {
        result.push_back(items_[index]);
    }
    return result;
}

std::size_t SettingsList::selected_index() const noexcept { return selected_index_; }

std::string SettingsList::search_query() const { return search_input_.value(); }

void SettingsList::apply_filter() {
    filtered_indices_.clear();
    std::vector<std::size_t> indices;
    indices.reserve(items_.size());
    for (std::size_t index = 0; index < items_.size(); ++index) {
        indices.push_back(index);
    }

    if (options_.enable_search) {
        const auto filtered = fuzzy_filter(indices, search_input_.value(), [this](std::size_t index) {
            return items_[index].label;
        });
        filtered_indices_ = filtered;
    } else {
        filtered_indices_ = std::move(indices);
    }
    selected_index_ = 0;
}

void SettingsList::move_selection(int delta) noexcept {
    if (filtered_indices_.empty() || delta == 0) {
        return;
    }
    if (delta < 0) {
        selected_index_ = selected_index_ == 0 ? filtered_indices_.size() - 1
                                               : selected_index_ - 1;
        return;
    }
    selected_index_ = selected_index_ + 1 >= filtered_indices_.size()
                          ? 0
                          : selected_index_ + 1;
}

void SettingsList::activate_selected() {
    if (filtered_indices_.empty()) {
        return;
    }
    auto& item = items_[filtered_indices_[selected_index_]];
    if (item.submenu) {
        const std::size_t item_index = filtered_indices_[selected_index_];
        submenu_item_index_ = selected_index_;
        submenu_component_ = item.submenu(item.current_value, [this, item_index](std::optional<std::string> value) {
            if (value) {
                auto& item = items_[item_index];
                item.current_value = *value;
                if (on_change_) {
                    on_change_(item.id, item.current_value);
                }
            }
            close_submenu();
        });
        search_input_.set_focused(false);
        if (auto* focusable = dynamic_cast<Focusable*>(submenu_component_.get())) {
            focusable->set_focused(focused_);
        }
        return;
    }
    if (item.values.empty()) {
        return;
    }
    const auto it = std::find(item.values.begin(), item.values.end(), item.current_value);
    const std::size_t next =
        it == item.values.end()
            ? 0
            : (static_cast<std::size_t>(std::distance(item.values.begin(), it)) + 1) %
                  item.values.size();
    item.current_value = item.values[next];
    if (on_change_) {
        on_change_(item.id, item.current_value);
    }
}

void SettingsList::close_submenu() {
    if (auto* focusable = dynamic_cast<Focusable*>(submenu_component_.get())) {
        focusable->set_focused(false);
    }
    submenu_component_.reset();
    if (submenu_item_index_) {
        selected_index_ = *submenu_item_index_;
        submenu_item_index_.reset();
    }
    search_input_.set_focused(focused_ && options_.enable_search);
}

const SettingItem* SettingsList::selected_item() const {
    if (filtered_indices_.empty()) {
        return nullptr;
    }
    return &items_[filtered_indices_[selected_index_]];
}

}  // namespace qt::tui
