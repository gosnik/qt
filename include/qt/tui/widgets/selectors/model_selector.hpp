#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input.hpp"
#include "qt/tui/widgets/selectors/select.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct ModelSelectorItem {
  std::string provider;
  std::string id;
  std::string display_name;
  std::optional<double> input_usd_per_million;
  std::optional<double> output_usd_per_million;
  bool current = false;
  bool scoped = false;
};

class ModelSelectorComponent final : public Component, public Focusable {
 public:
  using SelectCallback = std::function<void(ModelSelectorItem model)>;
  using CancelCallback = std::function<void()>;

  ModelSelectorComponent(std::vector<ModelSelectorItem> models,
                         SelectCallback on_select,
                         CancelCallback on_cancel,
                         std::string initial_search = {});

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override;
  void handle_input(std::string_view bytes) override;
  void set_focused(bool focused) override;
  [[nodiscard]] bool focused() const noexcept override;

  void set_models(std::vector<ModelSelectorItem> models);
  [[nodiscard]] Input& search_input() noexcept;
  [[nodiscard]] const std::vector<ModelSelectorItem>& filtered_models() const noexcept;
  [[nodiscard]] bool scoped_only() const noexcept;

 private:
  void rebuild();
  void apply_filter();
  void toggle_scope();
  std::string model_key(const ModelSelectorItem& item) const;

  std::vector<ModelSelectorItem> all_models_;
  std::vector<ModelSelectorItem> filtered_models_;
  SelectCallback on_select_;
  CancelCallback on_cancel_;
  std::shared_ptr<Input> search_input_;
  std::shared_ptr<Select> select_;
  std::shared_ptr<Box> box_;
  bool focused_ = false;
  bool scoped_only_ = false;
  bool has_scoped_models_ = false;
};

}  // namespace qt::tui
