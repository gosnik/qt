#pragma once

#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input.hpp"
#include "qt/tui/input/keybindings.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct SettingItem {
    using SubmenuDone = std::function<void(std::optional<std::string>)>;
    using SubmenuFactory = std::function<ComponentPtr(const std::string&, SubmenuDone)>;

    std::string id;
    std::string label;
    std::string description;
    std::string current_value;
    std::vector<std::string> values;
    SubmenuFactory submenu;
};

struct SettingsListTheme {
    std::string cursor = "> ";
    std::string hint_prefix;
};

struct SettingsListOptions {
    bool enable_search = false;
};

class SettingsList final : public Component, public Focusable {
  public:
    using ChangeCallback = std::function<void(const std::string&, const std::string&)>;
    using CancelCallback = std::function<void()>;

    SettingsList(std::vector<SettingItem> items, std::size_t max_visible = 8,
                 SettingsListTheme theme = SettingsListTheme{},
                 SettingsListOptions options = SettingsListOptions{});

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void handle_input(std::string_view bytes) override;
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const noexcept override;
    void set_change_callback(ChangeCallback callback);
    void set_cancel_callback(CancelCallback callback);
    void update_value(const std::string& id, std::string value);

    [[nodiscard]] const std::vector<SettingItem>& items() const noexcept;
    [[nodiscard]] std::vector<SettingItem> visible_items() const;
    [[nodiscard]] std::size_t selected_index() const noexcept;
    [[nodiscard]] std::string search_query() const;

  private:
    void apply_filter();
    void move_selection(int delta) noexcept;
    void activate_selected();
    void close_submenu();
    [[nodiscard]] const SettingItem* selected_item() const;

    std::vector<SettingItem> items_;
    std::vector<std::size_t> filtered_indices_;
    std::size_t selected_index_ = 0;
    std::size_t max_visible_ = 8;
    SettingsListTheme theme_;
    SettingsListOptions options_;
    Input search_input_;
    KeybindingManager keybindings_;
    ComponentPtr submenu_component_;
    std::optional<std::size_t> submenu_item_index_;
    bool focused_ = false;
    ChangeCallback on_change_;
    CancelCallback on_cancel_;
};

}  // namespace qt::tui
