#include "qt/tui/widgets/extension/extension_selector.hpp"
#include "qt/tui/layout/box.hpp"
#include "qt/tui/layout/dynamic_border.hpp"
#include "qt/tui/input/keybinding_hints.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/core/style.hpp"
#include "qt/tui/render/text.hpp"

#include <memory>
#include <string>
#include <vector>

namespace qt::tui {

ExtensionSelector::ExtensionSelector(std::string title,
                                     std::vector<std::string> options,
                                     SelectCallback on_select,
                                     CancelCallback on_cancel) {
    box_ = std::make_shared<Box>(1, 0);
    box_->add_child(std::make_shared<DynamicBorder>());
    box_->add_child(std::make_shared<Spacer>(1));

    // Title
    box_->add_child(std::make_shared<Text>(style::bold(title), 1, 0));
    box_->add_child(std::make_shared<Spacer>(1));

    // Build select items from string options
    std::vector<SelectItem> items;
    items.reserve(options.size());
    for (const auto& opt : options) {
        items.push_back({opt, opt, std::string{}});
    }

    SelectLayoutOptions layout;
    layout.min_primary_column_width = 12U;
    layout.max_primary_column_width = 32U;

    select_ = std::make_shared<Select>(
        std::optional<std::string>{}, std::move(items), 0U,
        std::min<std::size_t>(10U, options.size()), layout);

    select_->set_select_callback(
        [on_select = std::move(on_select)](const SelectItem& item) {
            on_select(item.value);
        });

    select_->set_cancel_callback(std::move(on_cancel));

    box_->add_child(select_);
    box_->add_child(std::make_shared<Spacer>(1));

    // Keyboard hints
    std::string hints = raw_key_hint("up/down", "navigate") +
                        "  " + key_hint("tui.select.confirm", "select") +
                        "  " + key_hint("tui.select.cancel", "cancel");
    box_->add_child(std::make_shared<Text>(style::dim(hints), 1, 0));
    box_->add_child(std::make_shared<Spacer>(1));
    box_->add_child(std::make_shared<DynamicBorder>());
}

foundation::Result<RenderBuffer> ExtensionSelector::render(
    const RenderConstraints& constraints) const {
    return box_->render(constraints);
}

void ExtensionSelector::handle_input(std::string_view bytes) {
    if (select_) {
        select_->handle_input(bytes);
    }
}

const Select& ExtensionSelector::select_list() const noexcept {
    return *select_;
}

}  // namespace qt::tui
