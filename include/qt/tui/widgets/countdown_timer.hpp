#pragma once

#include "qt/tui/core/component.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>

namespace qt::tui {

/// Reusable countdown timer for dialog components (auto-retry, etc.).
/// The owner calls tick() from the event loop; the timer invokes callbacks
/// on each second boundary and on expiry.
class CountdownTimer final : public Component {
  public:
    using TickCallback = std::function<void(std::size_t remaining_seconds)>;
    using ExpireCallback = std::function<void()>;

    /// Construct a countdown that lasts @p timeout_ms milliseconds.
    /// @p on_tick is called every second with remaining seconds.
    /// @p on_expire is called once when the countdown reaches zero.
    CountdownTimer(std::size_t timeout_ms,
                   TickCallback on_tick,
                   ExpireCallback on_expire);

    /// Drive the timer from the event loop. Returns true if the timer
    /// is still running (not expired and not disposed).
    [[nodiscard]] bool tick();

    /// Stop the timer permanently. No further callbacks fire.
    void dispose() noexcept;

    [[nodiscard]] bool expired() const noexcept;
    [[nodiscard]] bool disposed() const noexcept;
    [[nodiscard]] std::size_t remaining_seconds() const noexcept;

    [[nodiscard]] foundation::Result<RenderBuffer> render(
        const RenderConstraints& constraints) const override;

  private:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    TimePoint start_;
    std::size_t duration_ms_;
    std::size_t last_notified_second_;
    TickCallback on_tick_;
    ExpireCallback on_expire_;
    bool expired_ = false;
    bool disposed_ = false;
};

}
