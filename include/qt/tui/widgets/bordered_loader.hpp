#pragma once

#include "qt/tui/core/component.hpp"
#include "qt/tui/widgets/loader.hpp"

#include <functional>
#include <memory>
#include <string>

namespace qt::tui {

class Box;

class BorderedLoader final : public Component, public InputHandler {
  public:
    using AbortCallback = std::function<void()>;

    BorderedLoader(std::string message, bool cancellable = true);

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;
    void handle_input(std::string_view bytes) override;

    [[nodiscard]] bool aborted() const noexcept;
    void set_abort_callback(AbortCallback callback);
    void cancel() noexcept;

    Loader& loader() noexcept;
    [[nodiscard]] const Loader& loader() const noexcept;

  private:
    std::shared_ptr<Box> box_;
    std::shared_ptr<CancellableLoader> cancellable_loader_;
    std::shared_ptr<Loader> plain_loader_;
    bool cancellable_ = true;
};

}  // namespace qt::tui
