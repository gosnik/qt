#include "qt/tui/widgets/selectors/model_selector.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}
qt::tui::ModelSelectorItem model(std::string id, bool scoped = false) {
  return {"openrouter", std::move(id), "", std::nullopt, std::nullopt,
          false, scoped};
}
}  // namespace

int main() {
  try {
    std::string selected;
    qt::tui::ModelSelectorComponent picker(
        {model("vendor/b", true), model("vendor/d", true)},
        [&](qt::tui::ModelSelectorItem item) { selected = item.id; }, [] {},
        "vendor");
    picker.set_focused(true);
    picker.handle_input("\x1b[B");
    picker.set_models({model("vendor/a"), model("vendor/b", true),
                       model("vendor/d", true), model("other/new")});
    require(picker.search_input().value() == "vendor", "search was lost");
    require(picker.focused(), "focus was lost");
    require(picker.filtered_models().size() == 3, "new matching model missing");
    picker.handle_input("\r");
    require(selected == "vendor/d", "refresh changed selected model identity");
    picker.handle_input("\t");
    require(picker.scoped_only(), "scope did not toggle");
    picker.set_models({model("vendor/a"), model("vendor/b", true),
                       model("vendor/d", true), model("vendor/e", true)});
    require(picker.scoped_only(), "refresh lost scoped view");
    require(picker.filtered_models().size() == 3, "scoped discovery not updated");
    picker.set_models({model("vendor/a")});
    require(!picker.scoped_only(), "removed scope left picker unusable");
    picker.handle_input("\r");
    require(selected == "vendor/a", "removed selection did not fall back");
    picker.set_models({});
    require(picker.filtered_models().empty(), "removed models remain visible");
    selected.clear();
    picker.handle_input("\r");
    require(selected.empty(), "empty picker selected removed model");
    picker.set_models({model("vendor/recovered")});
    require(picker.filtered_models().size() == 1, "empty picker failed recovery");
    std::cout << "model selector refresh probe passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
