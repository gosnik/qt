#include "qt/tui/widgets/selectors/theme_selector.hpp"
#include "qt/tui/layout/box.hpp"
#include "qt/tui/layout/dynamic_border.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace qt::tui {

ThemeSelector::ThemeSelector(std::string current_theme,
                             std::vector<std::string> available_themes,
                             SelectCallback on_select, CancelCallback on_cancel,
                             PreviewCallback on_preview)
    : on_preview_(std::move(on_preview)) {
    box_ = std::make_shared<Box>(1, 0);
    box_->add_child(std::make_shared<DynamicBorder>());

    // Build select items
    std::vector<SelectItem> items;
    items.reserve(available_themes.size());

    std::size_t selected_index = 0;
    for (std::size_t i = 0; i < available_themes.size(); ++i) {
        const auto& name = available_themes[i];
        std::string description;
        if (name == current_theme) {
            description = "(current)";
            selected_index = i;
        }
        items.push_back({name, name, description});
    }

    SelectLayoutOptions layout;
    layout.min_primary_column_width = 12U;
    layout.max_primary_column_width = 32U;

    select_ = std::make_shared<Select>(
        std::optional<std::string>{"Select theme"},
        std::move(items), selected_index, 10U, layout);

    select_->set_select_callback(
        [on_select = std::move(on_select)](const SelectItem& item) {
            on_select(item.value);
        });

    select_->set_cancel_callback(std::move(on_cancel));

    if (on_preview_) {
        select_->set_selection_change_callback(
            [this](const SelectItem& item) { on_preview_(item.value); });
    }

    box_->add_child(select_);
    box_->add_child(std::make_shared<DynamicBorder>());
}

foundation::Result<RenderBuffer> ThemeSelector::render(
    const RenderConstraints& constraints) const {
    return box_->render(constraints);
}

void ThemeSelector::handle_input(std::string_view bytes) {
    if (select_) {
        select_->handle_input(bytes);
    }
}

const Select& ThemeSelector::select_list() const noexcept {
    return *select_;
}

}  // namespace qt::tui
