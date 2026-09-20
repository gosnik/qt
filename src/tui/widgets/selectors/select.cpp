#include "qt/tui/widgets/selectors/select.hpp"

#include "qt/foundation/utf8.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/core/style.hpp"

#include <uni_algo/case.h>
#include <uni_algo/norm.h>
#include <uni_algo/ranges_grapheme.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace qt::tui {

namespace {

constexpr std::size_t kPrimaryColumnGap = 2;
constexpr std::size_t kMinDescriptionWidth = 10;

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return std::string(text.substr(start, end - start));
}

std::string normalize_for_match(std::string_view value) {
    const std::string compatible = una::norm::to_nfkc_utf8(value);
    const std::string folded = una::cases::to_casefold_utf8(compatible);
    return una::norm::to_nfkc_utf8(folded);
}

std::vector<std::string> normalized_graphemes(std::string_view value) {
    std::vector<std::string> units;
    const std::string normalized = normalize_for_match(value);
    for (const std::string_view unit :
         una::ranges::grapheme::utf8_view<std::string_view>{normalized}) {
        units.emplace_back(unit);
    }
    return units;
}

bool starts_with_normalized(std::string_view value, std::string_view prefix) {
    const auto value_units = normalized_graphemes(value);
    const auto prefix_units = normalized_graphemes(prefix);
    if (prefix_units.empty()) {
        return true;
    }
    if (prefix_units.size() > value_units.size()) {
        return false;
    }
    return std::equal(prefix_units.begin(), prefix_units.end(), value_units.begin());
}

SelectItem make_item(std::string value) {
    return SelectItem{value, value, {}};
}

std::vector<SelectItem> make_items(std::vector<std::string> values) {
    std::vector<SelectItem> result;
    result.reserve(values.size());
    for (auto& value : values) {
        result.push_back(make_item(std::move(value)));
    }
    return result;
}

std::string display_value(const SelectItem& item) {
    return item.label.empty() ? item.value : item.label;
}

std::size_t safe_width(std::string_view text) {
    auto width = foundation::utf8_display_width(text);
    return width ? width.value() : text.size();
}

std::string truncate(std::string_view text, std::size_t width) {
    auto truncated = foundation::utf8_truncate(text, width);
    return truncated ? truncated.value() : std::string(text);
}

std::string truncate_primary_text(const SelectLayoutOptions& layout,
                                  const SelectItem& item, bool selected,
                                  std::size_t max_width,
                                  std::size_t column_width) {
    const std::string display = display_value(item);
    const std::string truncated =
        layout.truncate_primary
            ? layout.truncate_primary(display, max_width, column_width, item,
                                      selected)
            : truncate(display, max_width);
    return truncate(truncated, max_width);
}

std::string normalize_single_line(std::string_view text) {
    std::string normalized;
    bool last_was_replaced_newline = false;
    for (const char ch : text) {
        if (ch == '\r' || ch == '\n') {
            if (!last_was_replaced_newline) {
                normalized.push_back(' ');
            }
            last_was_replaced_newline = true;
            continue;
        }
        normalized.push_back(ch);
        last_was_replaced_newline = false;
    }
    return trim(normalized);
}

std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> words;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() &&
               std::isspace(static_cast<unsigned char>(text[index]))) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }

        std::size_t end = index;
        while (end < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[end]))) {
            ++end;
        }
        words.emplace_back(text.substr(index, end - index));
        index = end;
    }
    return words;
}

std::vector<std::string> split_chunk(std::string_view text, std::size_t width) {
    if (width == 0 || text.empty()) {
        return {""};
    }

    std::vector<std::string> chunks;
    std::string remaining(text);
    while (!remaining.empty()) {
        auto chunk = foundation::utf8_truncate(remaining, width);
        if (!chunk || chunk.value().empty()) {
            chunks.push_back(remaining);
            break;
        }

        chunks.push_back(chunk.value());
        remaining.erase(0, chunk.value().size());
    }

    return chunks;
}

std::vector<std::string> wrap_text(std::string_view text, std::string_view first_prefix,
                                   std::string_view rest_prefix, std::size_t width) {
    std::vector<std::string> lines;
    const auto words = split_words(text);
    const std::size_t first_prefix_width = safe_width(first_prefix);
    const std::size_t rest_prefix_width = safe_width(rest_prefix);

    if (width == 0) {
        lines.emplace_back("");
        return lines;
    }

    if (words.empty()) {
        lines.emplace_back(std::string(first_prefix));
        return lines;
    }

    std::string current(first_prefix);
    std::size_t current_width = first_prefix_width;
    bool first_line = true;

    for (const auto& word : words) {
        const std::size_t prefix_width = first_line ? first_prefix_width : rest_prefix_width;
        const std::size_t limit = width > prefix_width ? width - prefix_width : 1;
        const auto word_chunks = split_chunk(word, limit);

        for (std::size_t chunk_index = 0; chunk_index < word_chunks.size(); ++chunk_index) {
            const auto& chunk = word_chunks[chunk_index];
            const std::size_t chunk_width = safe_width(chunk);
            const bool need_space = current_width > prefix_width;
            const std::size_t candidate_width =
                current_width + (need_space ? 1U : 0U) + chunk_width;

            if (chunk_index > 0 || candidate_width > width) {
                lines.push_back(std::move(current));
                current = std::string(rest_prefix);
                current_width = rest_prefix_width;
                first_line = false;
            }

            if (current_width > (first_line ? first_prefix_width : rest_prefix_width)) {
                current.push_back(' ');
                ++current_width;
            }

            current += chunk;
            current_width += chunk_width;

            if (first_line) {
                first_line = false;
            }
        }
    }

    lines.push_back(std::move(current));
    return lines;
}

std::vector<std::string> render_title(const std::optional<std::string>& title, std::size_t width) {
    if (!title || trim(*title).empty()) {
        return {};
    }
    auto lines = wrap_text(*title, "", "", width);
    for (auto& line : lines) {
        line = style::bold(line);
    }
    return lines;
}

std::vector<std::string> render_wrapped_item(std::string_view item, bool selected,
                                             std::size_t width) {
    const std::string_view marker = selected ? "→ " : "  ";
    auto lines = wrap_text(item, marker, "  ", width);
    if (selected) {
        for (auto& line : lines) {
            line = style::cyan(line);
        }
    }
    return lines;
}

std::size_t block_height(const std::vector<std::string>& lines) {
    return lines.size();
}

std::size_t primary_column_width(const std::vector<SelectItem>& items,
                                 const SelectLayoutOptions& layout) {
    const std::size_t low = std::max<std::size_t>(
        1, std::min(layout.min_primary_column_width, layout.max_primary_column_width));
    const std::size_t high = std::max<std::size_t>(
        1, std::max(layout.min_primary_column_width, layout.max_primary_column_width));
    std::size_t widest = 0;
    for (const auto& item : items) {
        widest = std::max(widest, safe_width(display_value(item)) + kPrimaryColumnGap);
    }
    return std::max(low, std::min(widest, high));
}

std::string render_tabular_item(const SelectItem& item, bool selected, std::size_t width,
                                std::size_t primary_column_width,
                                const SelectLayoutOptions& layout) {
    const std::string prefix = selected ? "→ " : "  ";
    const std::size_t prefix_width = safe_width(prefix);
    const std::string description = normalize_single_line(item.description);

    if (!description.empty() && width > 40 && width > prefix_width + 4) {
        const std::size_t effective_primary =
            std::max<std::size_t>(1, std::min(primary_column_width, width - prefix_width - 4));
        const std::size_t max_primary =
            effective_primary > kPrimaryColumnGap ? effective_primary - kPrimaryColumnGap : 1;
        const std::string primary_text =
            truncate_primary_text(layout, item, selected, max_primary, effective_primary);
        const std::size_t primary_width = safe_width(primary_text);
        const std::string spacing(std::max<std::size_t>(1, effective_primary - primary_width), ' ');
        const std::size_t used = prefix_width + primary_width + spacing.size();
        const std::size_t remaining = width > used + 2 ? width - used - 2 : 0;
        if (remaining > kMinDescriptionWidth) {
            const std::string line =
                prefix + primary_text + spacing + truncate(description, remaining);
            if (selected) {
                return style::cyan(line);
            }
            return prefix + primary_text + spacing +
                   style::dim(truncate(description, remaining));
        }
    }

    const std::size_t max_primary = width > prefix_width + 2 ? width - prefix_width - 2 : 0;
    const std::string line =
        prefix + truncate_primary_text(layout, item, selected, max_primary, max_primary);
    return selected ? style::cyan(line) : line;
}

}  // namespace

Select::Select(std::vector<std::string> items) : items_(make_items(std::move(items))) {}

Select::Select(std::optional<std::string> title, std::vector<std::string> items,
               std::size_t selected_index)
    : title_(std::move(title)),
      items_(make_items(std::move(items))),
      selected_index_(selected_index) {}

Select::Select(std::vector<SelectItem> items, std::size_t max_visible,
               SelectLayoutOptions layout)
    : items_(std::move(items)),
      max_visible_(std::max<std::size_t>(1, max_visible)),
      layout_(layout) {}

Select::Select(std::optional<std::string> title, std::vector<SelectItem> items,
               std::size_t selected_index, std::size_t max_visible,
               SelectLayoutOptions layout)
    : title_(std::move(title)),
      items_(std::move(items)),
      selected_index_(selected_index),
      max_visible_(std::max<std::size_t>(1, max_visible)),
      layout_(layout) {}

foundation::Result<RenderBuffer> Select::render(
    const RenderConstraints& constraints) const {
    std::vector<std::string> lines = render_title(title_, constraints.max_width);
    if (constraints.max_height <= lines.size()) {
        return make_buffer(std::move(lines), constraints);
    }

    const auto items = visible_items();
    if (items.empty()) {
        if (!filter_.empty() && lines.size() < constraints.max_height) {
            lines.push_back(style::dim("  No matching commands"));
        }
        return make_buffer(std::move(lines), constraints);
    }

    const bool has_descriptions =
        std::any_of(items.begin(), items.end(),
                    [](const SelectItem& item) { return !item.description.empty(); });
    const std::size_t available_height = constraints.max_height - lines.size();
    const std::size_t selected = normalized_selected_index();

    if (has_descriptions) {
        const std::size_t visible_limit =
            std::min({max_visible_, items.size(), available_height});
        const std::size_t start =
            std::min(selected > visible_limit / 2 ? selected - visible_limit / 2 : 0,
                     items.size() > visible_limit ? items.size() - visible_limit : 0);
        const std::size_t end = std::min(items.size(), start + visible_limit);
        const std::size_t primary_width = primary_column_width(items, layout_);

        for (std::size_t index = start; index < end; ++index) {
            lines.push_back(render_tabular_item(items[index], index == selected,
                                                constraints.max_width, primary_width,
                                                layout_));
        }

        if ((start > 0 || end < items.size()) && lines.size() < constraints.max_height) {
            lines.push_back(style::dim(truncate("  (" + std::to_string(selected + 1) + "/" +
                                                    std::to_string(items.size()) + ")",
                                                constraints.max_width)));
        }
        return make_buffer(std::move(lines), constraints);
    }

    std::vector<std::vector<std::string>> item_blocks;
    item_blocks.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        item_blocks.push_back(render_wrapped_item(display_value(items[index]), index == selected,
                                                  constraints.max_width));
    }

    std::size_t used_height = block_height(item_blocks[selected]);
    std::size_t start = selected;
    std::size_t end = selected + 1;

    while (start > 0 && used_height < available_height) {
        const std::size_t candidate = block_height(item_blocks[start - 1]);
        if (used_height + candidate > available_height) {
            break;
        }
        --start;
        used_height += candidate;
    }

    while (end < item_blocks.size() && used_height < available_height) {
        const std::size_t candidate = block_height(item_blocks[end]);
        if (used_height + candidate > available_height) {
            break;
        }
        used_height += candidate;
        ++end;
    }

    for (std::size_t index = start; index < end; ++index) {
        const auto& block = item_blocks[index];
        lines.insert(lines.end(), block.begin(), block.end());
    }

    return make_buffer(std::move(lines), constraints);
}

void Select::handle_input(std::string_view bytes) {
    const auto& keybindings = get_keybindings_const();
    if (keybindings.matches(bytes, "tui.select.up") ||
        keybindings.matches(bytes, "tui.cursor.up")) {
        scroll_by(-1);
        notify_selection_change();
        return;
    }
    if (keybindings.matches(bytes, "tui.select.down") ||
        keybindings.matches(bytes, "tui.cursor.down")) {
        scroll_by(1);
        notify_selection_change();
        return;
    }
    if (keybindings.matches(bytes, "tui.select.confirm") ||
        keybindings.matches(bytes, "tui.input.submit")) {
        if (on_select_) {
            if (const auto* item = selected_item()) {
                on_select_(*item);
            }
        }
        return;
    }
    if (keybindings.matches(bytes, "tui.select.cancel") ||
        keybindings.matches(bytes, "tui.input.cancel")) {
        if (on_cancel_) {
            on_cancel_();
        }
    }
}

void Select::set_title(std::optional<std::string> title) { title_ = std::move(title); }

void Select::set_items(std::vector<std::string> items) {
    set_items(make_items(std::move(items)));
}

void Select::set_items(std::vector<SelectItem> items) {
    items_ = std::move(items);
    apply_filter();
}

void Select::set_filter(std::string filter) {
    filter_ = std::move(filter);
    apply_filter();
}

void Select::clear_filter() {
    filter_.clear();
    apply_filter();
}

void Select::set_selected_index(std::size_t index) noexcept { selected_index_ = index; }

void Select::scroll_by(int delta) noexcept {
    const auto count = filter_.empty() ? items_.size() : filtered_indices_.size();
    if (count == 0 || delta == 0) {
        return;
    }

    const std::size_t current = normalized_selected_index();
    if (delta < 0) {
        const auto distance = static_cast<std::size_t>(-delta) % count;
        selected_index_ = (current + count - distance) % count;
        return;
    }

    selected_index_ = (current + static_cast<std::size_t>(delta)) % count;
}

void Select::set_max_visible(std::size_t max_visible) noexcept {
    max_visible_ = std::max<std::size_t>(1, max_visible);
}

void Select::set_layout(SelectLayoutOptions layout) noexcept { layout_ = layout; }

void Select::set_select_callback(SelectCallback callback) { on_select_ = std::move(callback); }

void Select::set_cancel_callback(CancelCallback callback) { on_cancel_ = std::move(callback); }

void Select::set_selection_change_callback(SelectCallback callback) {
    on_selection_change_ = std::move(callback);
}

const std::optional<std::string>& Select::title() const noexcept { return title_; }

std::vector<std::string> Select::items() const {
    std::vector<std::string> values;
    values.reserve(items_.size());
    for (const auto& item : items_) {
        values.push_back(display_value(item));
    }
    return values;
}

const std::vector<SelectItem>& Select::select_items() const noexcept { return items_; }

const std::string& Select::filter() const noexcept { return filter_; }

std::size_t Select::selected_index() const noexcept { return selected_index_; }

const SelectItem* Select::selected_item() const noexcept {
    const auto count = filter_.empty() ? items_.size() : filtered_indices_.size();
    if (count == 0) {
        return nullptr;
    }
    const auto selected = normalized_selected_index();
    const auto item_index = filter_.empty() ? selected : filtered_indices_[selected];
    return item_index < items_.size() ? &items_[item_index] : nullptr;
}

std::size_t Select::normalized_selected_index() const noexcept {
    const auto count = filter_.empty() ? items_.size() : filtered_indices_.size();
    if (count == 0) {
        return 0;
    }
    return std::min(selected_index_, count - 1);
}

std::vector<SelectItem> Select::visible_items() const {
    if (filter_.empty()) {
        return items_;
    }

    std::vector<SelectItem> items;
    items.reserve(filtered_indices_.size());
    for (const auto index : filtered_indices_) {
        if (index < items_.size()) {
            items.push_back(items_[index]);
        }
    }
    return items;
}

void Select::apply_filter() {
    filtered_indices_.clear();
    if (!filter_.empty()) {
        for (std::size_t index = 0; index < items_.size(); ++index) {
            if (starts_with_normalized(items_[index].value, filter_) ||
                starts_with_normalized(display_value(items_[index]), filter_)) {
                filtered_indices_.push_back(index);
            }
        }
    }
    selected_index_ = 0;
}

void Select::notify_selection_change() {
    if (!on_selection_change_) {
        return;
    }
    if (const auto* item = selected_item()) {
        on_selection_change_(*item);
    }
}

}  // namespace qt::tui
