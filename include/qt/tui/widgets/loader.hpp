#pragma once

#include "qt/tui/core/component.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct LoaderIndicator {
    std::vector<std::string> frames;
    std::size_t interval_ms = 80;
};

class Loader final : public Component {
  public:
    Loader();
    explicit Loader(std::string message);
    Loader(std::string message, LoaderIndicator indicator);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void set_message(std::string message);
    void set_indicator(LoaderIndicator indicator);
    void advance_frame() noexcept;

    [[nodiscard]] const std::string& message() const noexcept;
    [[nodiscard]] std::string current_frame() const;
    [[nodiscard]] std::size_t interval_ms() const noexcept;

  private:
    std::string message_ = "Loading...";
    LoaderIndicator indicator_;
    std::size_t current_frame_ = 0;
};

class CancellableLoader final : public Component, public InputHandler {
  public:
    using AbortCallback = std::function<void()>;

    CancellableLoader();
    explicit CancellableLoader(std::string message);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

    void handle_input(std::string_view bytes) override;
    void cancel() noexcept;
    void set_abort_callback(AbortCallback callback);
    [[nodiscard]] bool aborted() const noexcept;

    Loader& loader() noexcept;
    [[nodiscard]] const Loader& loader() const noexcept;

  private:
    Loader loader_;
    bool aborted_ = false;
    AbortCallback on_abort_;
};

}  // namespace qt::tui
