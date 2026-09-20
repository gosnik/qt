#include "qt/tui/widgets/selectors/show_images_selector.hpp"
#include "qt/tui/layout/box.hpp"
#include "qt/tui/layout/dynamic_border.hpp"

#include <memory>
#include <string>
#include <vector>

namespace qt::tui {

ShowImagesSelector::ShowImagesSelector(bool current_value,
                                       SelectCallback on_select,
                                       CancelCallback on_cancel) {
    box_ = std::make_shared<Box>(1, 0);
    box_->add_child(std::make_shared<DynamicBorder>());

    std::vector<SelectItem> items = {
        {"yes", "Yes", "Show images inline in terminal"},
        {"no", "No", "Show text placeholder instead"},
    };

    SelectLayoutOptions layout;
    layout.min_primary_column_width = 12U;
    layout.max_primary_column_width = 32U;

    select_ = std::make_shared<Select>(
        std::optional<std::string>{"Show images?"},
        std::move(items), current_value ? 0U : 1U, 5U, layout);

    select_->set_select_callback(
        [on_select = std::move(on_select)](const SelectItem& item) {
            on_select(item.value == "yes");
        });

    select_->set_cancel_callback(std::move(on_cancel));

    box_->add_child(select_);
    box_->add_child(std::make_shared<DynamicBorder>());
}

foundation::Result<RenderBuffer> ShowImagesSelector::render(
    const RenderConstraints& constraints) const {
    return box_->render(constraints);
}

void ShowImagesSelector::handle_input(std::string_view bytes) {
    if (select_) {
        select_->handle_input(bytes);
    }
}

const Select& ShowImagesSelector::select_list() const noexcept {
    return *select_;
}

}  // namespace qt::tui
