#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

enum class OverlayAnchor {
    center,
    top_left,
    top_right,
    bottom_left,
    bottom_right,
    top_center,
    bottom_center,
    left_center,
    right_center,
};

struct OverlaySize {
    enum class Kind { columns, percent };

    Kind kind = Kind::columns;
    std::size_t value = 0;

    static OverlaySize columns(std::size_t value);
    static OverlaySize percent(std::size_t value);
};

struct OverlayMargin {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;

    static OverlayMargin all(int value);
};

struct OverlayOptions {
    std::optional<OverlaySize> width;
    std::optional<std::size_t> min_width;
    std::optional<OverlaySize> max_height;
    OverlayAnchor anchor = OverlayAnchor::center;
    int offset_x = 0;
    int offset_y = 0;
    std::optional<OverlaySize> row;
    std::optional<OverlaySize> col;
    OverlayMargin margin;
    std::function<bool(std::size_t width, std::size_t height)> visible;
    bool non_capturing = false;
    bool frame = true;
};

class OverlaySurface;

class OverlayHandle {
  public:
    OverlayHandle() = default;

    void hide() const;
    void set_hidden(bool hidden) const;
    [[nodiscard]] bool is_hidden() const;
    void focus() const;
    void unfocus() const;
    [[nodiscard]] bool is_focused() const;

  private:
    friend class OverlaySurface;

    OverlayHandle(OverlaySurface* owner, std::size_t id);

    OverlaySurface* owner_ = nullptr;
    std::size_t id_ = 0;
};

class OverlaySurface final : public Component {
  public:
    struct InputListenerResult {
        bool consume = false;
        std::optional<std::string> data;
    };
    using InputListener = std::function<InputListenerResult(std::string_view)>;
    using InvalidationCallback = std::function<void()>;

    explicit OverlaySurface(ComponentPtr base = nullptr);

    void set_base(ComponentPtr base);
    void set_focus(ComponentPtr component);
    [[nodiscard]] ComponentPtr focused_component() const;
    void set_invalidation_callback(InvalidationCallback callback);

    OverlayHandle show_overlay(ComponentPtr component, OverlayOptions options = {});
    void hide_overlay();
    void clear_overlays();
    [[nodiscard]] bool has_overlay() const;

    void handle_input(std::string_view bytes);
    [[nodiscard]] std::size_t add_input_listener(InputListener listener);
    void remove_input_listener(std::size_t listener_id);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;
    void invalidate() const;

  private:
    friend class OverlayHandle;

    struct Entry {
        std::size_t id = 0;
        ComponentPtr component;
        OverlayOptions options;
        ComponentPtr pre_focus;
        bool hidden = false;
        std::size_t focus_order = 0;
    };

    struct Layout {
        std::size_t width = 1;
        std::size_t row = 0;
        std::size_t col = 0;
        std::optional<std::size_t> max_height;
    };

    Entry* find_entry(std::size_t id);
    const Entry* find_entry(std::size_t id) const;
    const Entry* topmost_visible_capturing() const;
    bool is_visible(const Entry& entry, std::size_t width, std::size_t height) const;
    bool is_visible_now(const Entry& entry) const;
    void restore_focus_after(const Entry& entry);
    void bump_focus_order(Entry& entry);

    [[nodiscard]] Layout resolve_layout(const OverlayOptions& options, std::size_t overlay_height,
                                        std::size_t term_width,
                                        std::size_t term_height) const;
    [[nodiscard]] std::string composite_line(std::string_view base, std::string_view overlay,
                                             std::size_t start_col,
                                             std::size_t overlay_width,
                                             std::size_t total_width) const;

    ComponentPtr base_;
    ComponentPtr focused_;
    std::vector<Entry> overlays_;
    std::size_t next_id_ = 1;
    std::size_t next_focus_order_ = 1;
    std::size_t next_listener_id_ = 1;
    std::vector<std::pair<std::size_t, InputListener>> input_listeners_;
    InvalidationCallback invalidation_callback_;
    mutable std::size_t last_width_ = 0;
    mutable std::size_t last_height_ = 0;
};

}  // namespace qt::tui
