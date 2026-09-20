#pragma once

#include "qt/foundation/error.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct RenderConstraints {
    std::size_t max_width = 0;
    std::size_t max_height = 0;
};

struct RenderBuffer {
    std::size_t width = 0;
    std::vector<std::string> lines;

    [[nodiscard]] std::size_t height() const { return lines.size(); }
};

class Component {
  public:
    virtual ~Component() = default;

    [[nodiscard]] virtual foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const = 0;
};

class InputHandler {
  public:
    virtual ~InputHandler() = default;

    virtual void handle_input(std::string_view bytes) = 0;
    [[nodiscard]] virtual bool wants_key_release() const noexcept { return false; }
};

class Focusable : public InputHandler {
  public:
    ~Focusable() override = default;

    virtual void set_focused(bool focused) = 0;
    [[nodiscard]] virtual bool focused() const noexcept = 0;
};

struct CursorPosition {
    std::size_t row = 0;
    std::size_t column = 0;
};

inline constexpr std::string_view cursor_marker = "\x1b_pi:c\x07";

using ComponentPtr = std::shared_ptr<Component>;

foundation::Result<std::string> clamp_line(std::string_view text, std::size_t width);
foundation::Result<std::size_t> measure_line_width(std::string_view text);
foundation::Result<std::string> truncate_line(std::string_view text, std::size_t width);
foundation::Result<RenderBuffer> make_buffer(std::vector<std::string> lines,
                                             const RenderConstraints& constraints);
[[nodiscard]] std::optional<CursorPosition> extract_cursor_marker(
    std::vector<std::string>& lines, std::size_t viewport_height);

}  // namespace qt::tui
