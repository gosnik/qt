#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace qt::tui {

using BoxStyleFunction = std::function<std::string(std::string_view)>;

class Box final : public Component {
  public:
    Box(std::size_t padding_x = 1, std::size_t padding_y = 1,
        BoxStyleFunction background = {});

    void add_child(ComponentPtr child);
    void remove_child(const ComponentPtr& child);
    void clear();
    void set_background(BoxStyleFunction background);

    [[nodiscard]] const std::vector<ComponentPtr>& children() const noexcept;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    [[nodiscard]] foundation::Result<std::string> apply_background(
        std::string_view line, std::size_t width) const;

    std::vector<ComponentPtr> children_;
    std::size_t padding_x_ = 1;
    std::size_t padding_y_ = 1;
    BoxStyleFunction background_;
};

}  // namespace qt::tui
