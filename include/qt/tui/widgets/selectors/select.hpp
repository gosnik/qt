#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct SelectItem {
    std::string value;
    std::string label;
    std::string description;
};

struct SelectLayoutOptions {
    std::size_t min_primary_column_width = 32;
    std::size_t max_primary_column_width = 32;
    std::function<std::string(std::string_view text, std::size_t max_width,
                              std::size_t column_width, const SelectItem& item,
                              bool is_selected)>
        truncate_primary;
};

class Select final : public Component, public InputHandler {
  public:
    using SelectCallback = std::function<void(const SelectItem&)>;
    using CancelCallback = std::function<void()>;

    Select() = default;
    explicit Select(std::vector<std::string> items);
    Select(std::optional<std::string> title, std::vector<std::string> items,
           std::size_t selected_index = 0);
    Select(std::vector<SelectItem> items, std::size_t max_visible = 5,
           SelectLayoutOptions layout = SelectLayoutOptions{});
    Select(std::optional<std::string> title, std::vector<SelectItem> items,
           std::size_t selected_index = 0, std::size_t max_visible = 5,
           SelectLayoutOptions layout = SelectLayoutOptions{});

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void handle_input(std::string_view bytes) override;

    void set_title(std::optional<std::string> title);
    void set_items(std::vector<std::string> items);
    void set_items(std::vector<SelectItem> items);
    void set_filter(std::string filter);
    void clear_filter();
    void set_selected_index(std::size_t index) noexcept;
    void scroll_by(int delta) noexcept;
    void set_max_visible(std::size_t max_visible) noexcept;
    void set_layout(SelectLayoutOptions layout) noexcept;
    void set_select_callback(SelectCallback callback);
    void set_cancel_callback(CancelCallback callback);
    void set_selection_change_callback(SelectCallback callback);

    [[nodiscard]] const std::optional<std::string>& title() const noexcept;
    [[nodiscard]] std::vector<std::string> items() const;
    [[nodiscard]] const std::vector<SelectItem>& select_items() const noexcept;
    [[nodiscard]] const std::string& filter() const noexcept;
    [[nodiscard]] std::size_t selected_index() const noexcept;
    [[nodiscard]] const SelectItem* selected_item() const noexcept;

  private:
    [[nodiscard]] std::size_t normalized_selected_index() const noexcept;
    [[nodiscard]] std::vector<SelectItem> visible_items() const;
    void apply_filter();
    void notify_selection_change();

    std::optional<std::string> title_;
    std::vector<SelectItem> items_;
    std::string filter_;
    std::vector<std::size_t> filtered_indices_;
    std::size_t selected_index_ = 0;
    std::size_t max_visible_ = 5;
    SelectLayoutOptions layout_;
    SelectCallback on_select_;
    CancelCallback on_cancel_;
    SelectCallback on_selection_change_;
};

}  // namespace qt::tui
