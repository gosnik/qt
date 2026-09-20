#include "qt/tui/widgets/selectors/session_selector_search.hpp"

#include "qt/tui/layout/dynamic_border.hpp"
#include "qt/tui/utils/fuzzy.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <algorithm>
#include <utility>

namespace qt::tui {

SessionSelectorSearchComponent::SessionSelectorSearchComponent(
    std::vector<SessionSelectorSearchItem> sessions,
    SelectCallback on_select,
    CancelCallback on_cancel)
    : sessions_(std::move(sessions)),
      on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)),
      search_input_(std::make_shared<Input>()),
      select_(std::make_shared<Select>()),
      box_(std::make_shared<Box>(0, 0)) {
  search_input_->set_change_callback([this](const std::string&) { apply_filter(); });
  search_input_->set_submit_callback([this](const std::string&) {
    if (const auto* item = select_->selected_item(); item != nullptr) {
      for (const auto& session : filtered_) {
        if (session.session_id == item->value) {
          if (on_select_) {
            on_select_(session);
          }
          return;
        }
      }
    }
  });
  search_input_->set_escape_callback([this]() {
    if (on_cancel_) {
      on_cancel_();
    }
  });
  select_->set_select_callback([this](const SelectItem& item) {
    for (const auto& session : filtered_) {
      if (session.session_id == item.value) {
        if (on_select_) {
          on_select_(session);
        }
        return;
      }
    }
  });
  select_->set_cancel_callback([this]() {
    if (on_cancel_) {
      on_cancel_();
    }
  });
  rebuild();
  apply_filter();
}

foundation::Result<RenderBuffer> SessionSelectorSearchComponent::render(
    const RenderConstraints& constraints) const {
  return box_->render(constraints);
}

void SessionSelectorSearchComponent::handle_input(std::string_view bytes) {
  const auto& keybindings = get_keybindings_const();
  if (keybindings.matches(bytes, "tui.select.up") ||
      keybindings.matches(bytes, "tui.cursor.up") ||
      keybindings.matches(bytes, "tui.select.down") ||
      keybindings.matches(bytes, "tui.cursor.down") ||
      keybindings.matches(bytes, "tui.select.pageUp") ||
      keybindings.matches(bytes, "tui.select.pageDown") ||
      keybindings.matches(bytes, "tui.select.confirm")) {
    select_->handle_input(bytes);
    return;
  }
  if (keybindings.matches(bytes, "tui.select.cancel")) {
    if (on_cancel_) {
      on_cancel_();
    }
    return;
  }
  search_input_->handle_input(bytes);
}

void SessionSelectorSearchComponent::set_focused(bool focused) {
  focused_ = focused;
  search_input_->set_focused(focused);
}

bool SessionSelectorSearchComponent::focused() const noexcept { return focused_; }

Input& SessionSelectorSearchComponent::search_input() noexcept {
  return *search_input_;
}

const std::vector<SessionSelectorSearchItem>&
SessionSelectorSearchComponent::filtered_sessions() const noexcept {
  return filtered_;
}

void SessionSelectorSearchComponent::apply_filter() {
  const std::string query = search_input_->value();
  if (filter_initialized_ && query == last_filter_query_) {
    return;
  }

  auto filtered = fuzzy_filter(
      sessions_, search_input_->value(), [](const SessionSelectorSearchItem& item) {
        return item.session_id + " " + item.label + " " + item.path;
      });
  const bool unchanged =
      filter_initialized_ && filtered.size() == filtered_.size() &&
      std::equal(filtered.begin(), filtered.end(), filtered_.begin(),
                 [](const auto& left, const auto& right) {
                   return left.session_id == right.session_id &&
                          left.label == right.label && left.path == right.path;
                 });
  last_filter_query_ = query;
  filter_initialized_ = true;
  if (unchanged) {
    return;
  }
  filtered_ = std::move(filtered);

  std::vector<SelectItem> items;
  items.reserve(filtered_.size());
  for (const auto& item : filtered_) {
    items.push_back(SelectItem{item.session_id,
                               item.label.empty() ? item.session_id : item.label,
                               item.path});
  }
  select_->set_items(std::move(items));
  select_->set_max_visible(12U);
}

void SessionSelectorSearchComponent::rebuild() {
  box_->clear();
  box_->add_child(std::make_shared<DynamicBorder>());
  box_->add_child(std::make_shared<Text>("Resume session", 0, 0));
  box_->add_child(search_input_);
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(select_);
  box_->add_child(std::make_shared<DynamicBorder>());
}

}  // namespace qt::tui
