#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/widgets/selectors/select.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qt::tui {

class ThinkingSelectorComponent final : public Component, public InputHandler {
 public:
  using SelectCallback = std::function<void(std::string level)>;
  using CancelCallback = std::function<void()>;

  ThinkingSelectorComponent(std::string current_level,
                            std::vector<std::string> available_levels,
                            SelectCallback on_select,
                            CancelCallback on_cancel);

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override;
  void handle_input(std::string_view bytes) override;

  [[nodiscard]] const Select& select_list() const noexcept;

 private:
  std::shared_ptr<Box> box_;
  std::shared_ptr<Select> select_;
};

}  // namespace qt::tui
