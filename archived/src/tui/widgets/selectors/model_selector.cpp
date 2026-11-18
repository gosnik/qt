#include "qt/tui/widgets/selectors/model_selector.hpp"

#include "qt/tui/layout/dynamic_border.hpp"
#include "qt/tui/input/keybinding_hints.hpp"
#include "qt/tui/input/keybindings.hpp"
#include "qt/tui/layout/spacer.hpp"
#include "qt/tui/render/text.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace qt::tui {
namespace {

std::string label_for(const ModelSelectorItem& item) {
  std::string label = item.id;
  if (item.current) {
    label += " (current)";
  }
  return label;
}

std::string trim_decimal_zeros(std::string value) {
  const auto dot = value.find('.');
  if (dot == std::string::npos) {
    return value;
  }
  while (!value.empty() && value.back() == '0') {
    value.pop_back();
  }
  if (!value.empty() && value.back() == '.') {
    value.pop_back();
  }
  return value;
}

std::string format_usd_per_million(double value) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(value < 1.0 ? 4 : 2) << value;
  return "$" + trim_decimal_zeros(stream.str()) + "/1M";
}

std::string description_for(const ModelSelectorItem& item) {
  std::string description = "[" + item.provider + "]";
  if (!item.display_name.empty()) {
    description += " " + item.display_name;
  }
  if (item.input_usd_per_million || item.output_usd_per_million) {
    description += " ";
    description += "in ";
    description += format_usd_per_million(item.input_usd_per_million.value_or(0.0));
    description += " / out ";
    description += format_usd_per_million(item.output_usd_per_million.value_or(0.0));
  }
  return description;
}

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::vector<std::string> filter_tokens(std::string_view query) {
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < query.size()) {
    while (index < query.size() &&
           std::isspace(static_cast<unsigned char>(query[index])) != 0) {
      ++index;
    }
    const std::size_t start = index;
    while (index < query.size() &&
           std::isspace(static_cast<unsigned char>(query[index])) == 0) {
      ++index;
    }
    if (start < index) {
      tokens.push_back(lowercase_ascii(std::string(query.substr(start, index - start))));
    }
  }
  return tokens;
}

std::string searchable_text_for(const ModelSelectorItem& item) {
  return lowercase_ascii(item.provider + " " + item.id + " " +
                         item.provider + "/" + item.id + " " +
                         item.display_name);
}

bool matches_filter(const ModelSelectorItem& item,
                    const std::vector<std::string>& tokens) {
  if (tokens.empty()) {
    return true;
  }
  const std::string searchable = searchable_text_for(item);
  return std::all_of(tokens.begin(), tokens.end(), [&searchable](const auto& token) {
    return searchable.find(token) != std::string::npos;
  });
}

}  // namespace

ModelSelectorComponent::ModelSelectorComponent(std::vector<ModelSelectorItem> models,
                                               SelectCallback on_select,
                                               CancelCallback on_cancel,
                                               std::string initial_search)
    : on_select_(std::move(on_select)),
      on_cancel_(std::move(on_cancel)),
      search_input_(std::make_shared<Input>()),
      select_(std::make_shared<Select>()),
      box_(std::make_shared<Box>(0, 0)) {
  search_input_->set_value(std::move(initial_search));
  search_input_->set_change_callback([this](const std::string&) { apply_filter(); });
  search_input_->set_submit_callback([this](const std::string&) {
    if (const auto* item = select_->selected_item(); item != nullptr) {
      for (const auto& model : filtered_models_) {
        if (model_key(model) == item->value) {
          if (on_select_) {
            on_select_(model);
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
    for (const auto& model : filtered_models_) {
      if (model_key(model) == item.value) {
        if (on_select_) {
          on_select_(model);
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

  set_models(std::move(models));
}

foundation::Result<RenderBuffer> ModelSelectorComponent::render(
    const RenderConstraints& constraints) const {
  return box_->render(constraints);
}

void ModelSelectorComponent::handle_input(std::string_view bytes) {
  const auto& keybindings = get_keybindings_const();
  if (keybindings.matches(bytes, "tui.input.tab")) {
    toggle_scope();
    return;
  }
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

void ModelSelectorComponent::set_focused(bool focused) {
  focused_ = focused;
  search_input_->set_focused(focused);
}

bool ModelSelectorComponent::focused() const noexcept { return focused_; }

void ModelSelectorComponent::set_models(std::vector<ModelSelectorItem> models) {
  const auto* selected = select_->selected_item();
  const auto selected_key = selected ? std::optional<std::string>(selected->value)
                                     : std::nullopt;
  all_models_ = std::move(models);
  has_scoped_models_ =
      std::any_of(all_models_.begin(), all_models_.end(),
                  [](const ModelSelectorItem& item) { return item.scoped; });
  if (!has_scoped_models_) {
    scoped_only_ = false;
  }

  std::stable_sort(all_models_.begin(), all_models_.end(),
                   [](const ModelSelectorItem& left,
                      const ModelSelectorItem& right) {
                     if (left.current != right.current) {
                       return left.current;
                     }
                     if (left.provider != right.provider) {
                       return left.provider < right.provider;
                     }
                     return left.id < right.id;
                   });

  apply_filter();
  if (selected_key) {
    const auto found = std::find_if(
        filtered_models_.begin(), filtered_models_.end(),
        [&](const ModelSelectorItem& item) { return model_key(item) == *selected_key; });
    if (found != filtered_models_.end()) {
      select_->set_selected_index(
          static_cast<std::size_t>(found - filtered_models_.begin()));
    }
  }
}

Input& ModelSelectorComponent::search_input() noexcept { return *search_input_; }

const std::vector<ModelSelectorItem>& ModelSelectorComponent::filtered_models()
    const noexcept {
  return filtered_models_;
}

bool ModelSelectorComponent::scoped_only() const noexcept { return scoped_only_; }

void ModelSelectorComponent::rebuild() {
  box_->clear();
  box_->add_child(std::make_shared<DynamicBorder>());
  if (has_scoped_models_) {
    box_->add_child(std::make_shared<Text>(
        std::string("Scope: ") + (scoped_only_ ? "scoped" : "all"), 0, 0));
    box_->add_child(std::make_shared<Text>(
        key_hint("tui.input.tab", "scope") + " (all/scoped)", 0, 0));
  } else {
    box_->add_child(std::make_shared<Text>(
        "Only showing models from configured providers. Use /login to add providers.",
        0, 0));
  }
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(search_input_);
  box_->add_child(std::make_shared<Spacer>(1));
  box_->add_child(select_);
  box_->add_child(std::make_shared<DynamicBorder>());
}

void ModelSelectorComponent::apply_filter() {
  const auto tokens = filter_tokens(search_input_->value());
  std::vector<ModelSelectorItem> active;
  active.reserve(all_models_.size());
  for (const auto& item : all_models_) {
    if ((!scoped_only_ || item.scoped) && matches_filter(item, tokens)) {
      active.push_back(item);
    }
  }

  filtered_models_ = std::move(active);

  std::vector<SelectItem> items;
  items.reserve(filtered_models_.size());
  for (const auto& model : filtered_models_) {
    items.push_back(
        SelectItem{model_key(model), label_for(model), description_for(model)});
  }
  select_->set_items(std::move(items));
  select_->set_max_visible(10U);
  rebuild();
}

void ModelSelectorComponent::toggle_scope() {
  if (!has_scoped_models_) {
    return;
  }
  scoped_only_ = !scoped_only_;
  apply_filter();
}

std::string ModelSelectorComponent::model_key(
    const ModelSelectorItem& item) const {
  return item.provider + "/" + item.id;
}

}  // namespace qt::tui
