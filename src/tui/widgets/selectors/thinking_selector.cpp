#include "qt/tui/widgets/selectors/thinking_selector.hpp"

#include "qt/tui/layout/dynamic_border.hpp"

#include <algorithm>
#include <utility>

namespace qt::tui {
namespace {

std::string thinking_description(const std::string& level) {
  if (level == "off") return "No reasoning";
  if (level == "minimal") return "Very brief reasoning";
  if (level == "low") return "Light reasoning";
  if (level == "medium") return "Moderate reasoning";
  if (level == "high") return "Deep reasoning";
  if (level == "xhigh") return "Maximum reasoning";
  return {};
}

}  // namespace

ThinkingSelectorComponent::ThinkingSelectorComponent(
    std::string current_level,
    std::vector<std::string> available_levels,
    SelectCallback on_select,
    CancelCallback on_cancel)
    : box_(std::make_shared<Box>(0, 0)) {
  std::vector<SelectItem> items;
  items.reserve(available_levels.size());
  std::size_t selected = 0;
  for (std::size_t index = 0; index < available_levels.size(); ++index) {
    const auto& level = available_levels[index];
    if (level == current_level) {
      selected = index;
    }
    items.push_back(SelectItem{level, level, thinking_description(level)});
  }

  SelectLayoutOptions layout;
  layout.min_primary_column_width = 12U;
  layout.max_primary_column_width = 32U;
  select_ = std::make_shared<Select>(std::optional<std::string>{"Thinking level"},
                                     std::move(items), selected,
                                     available_levels.empty()
                                         ? 1U
                                         : available_levels.size(),
                                     layout);
  select_->set_select_callback(
      [callback = std::move(on_select)](const SelectItem& item) {
        if (callback) {
          callback(item.value);
        }
      });
  select_->set_cancel_callback(std::move(on_cancel));

  box_->add_child(std::make_shared<DynamicBorder>());
  box_->add_child(select_);
  box_->add_child(std::make_shared<DynamicBorder>());
}

foundation::Result<RenderBuffer> ThinkingSelectorComponent::render(
    const RenderConstraints& constraints) const {
  return box_->render(constraints);
}

void ThinkingSelectorComponent::handle_input(std::string_view bytes) {
  select_->handle_input(bytes);
}

const Select& ThinkingSelectorComponent::select_list() const noexcept {
  return *select_;
}

}  // namespace qt::tui
