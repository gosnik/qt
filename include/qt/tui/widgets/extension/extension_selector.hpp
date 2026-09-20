#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/widgets/selectors/select.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qt::tui {

class ExtensionSelector final : public Component, public InputHandler {
  public:
    using SelectCallback = std::function<void(std::string value)>;
    using CancelCallback = std::function<void()>;

    ExtensionSelector(std::string title, std::vector<std::string> options,
                      SelectCallback on_select, CancelCallback on_cancel);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;
    void handle_input(std::string_view bytes) override;

    [[nodiscard]] const Select& select_list() const noexcept;

  private:
    std::shared_ptr<Box> box_;
    std::shared_ptr<Select> select_;
};

}  // namespace qt::tui
