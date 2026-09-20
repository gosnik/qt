#include "qt/tui/widgets/countdown_timer.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace qt::tui {

CountdownTimer::CountdownTimer(std::size_t timeout_ms,
                               TickCallback on_tick,
                               ExpireCallback on_expire)
    : start_(Clock::now()),
      duration_ms_(timeout_ms),
      last_notified_second_(0),
      on_tick_(std::move(on_tick)),
      on_expire_(std::move(on_expire)) {
    const auto total_seconds = static_cast<std::size_t>(
        (timeout_ms + 999) / 1000);
    if (on_tick_) {
        on_tick_(total_seconds);
    }
}

bool CountdownTimer::tick() {
    if (expired_ || disposed_) {
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start_).count();
    const auto elapsed_ms = static_cast<std::size_t>(std::max<long long>(0, elapsed));

    if (elapsed_ms >= duration_ms_) {
        expired_ = true;
        if (on_expire_) {
            on_expire_();
        }
        return false;
    }

    const auto remaining_ms = duration_ms_ - elapsed_ms;
    const auto remaining_seconds = static_cast<std::size_t>(
        (remaining_ms + 999) / 1000);
    const auto elapsed_seconds = static_cast<std::size_t>(elapsed_ms / 1000);

    if (elapsed_seconds > last_notified_second_) {
        last_notified_second_ = elapsed_seconds;
        if (on_tick_) {
            on_tick_(remaining_seconds);
        }
    }

    return true;
}

void CountdownTimer::dispose() noexcept {
    disposed_ = true;
}

bool CountdownTimer::expired() const noexcept {
    return expired_;
}

bool CountdownTimer::disposed() const noexcept {
    return disposed_;
}

std::size_t CountdownTimer::remaining_seconds() const noexcept {
    if (expired_ || disposed_) {
        return 0;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start_).count();
    const auto elapsed_ms = static_cast<std::size_t>(std::max<long long>(0, elapsed));
    if (elapsed_ms >= duration_ms_) {
        return 0;
    }
    const auto remaining_ms = duration_ms_ - elapsed_ms;
    return static_cast<std::size_t>((remaining_ms + 999) / 1000);
}

foundation::Result<RenderBuffer> CountdownTimer::render(
    const RenderConstraints& constraints) const {
    const auto remaining = remaining_seconds();
    std::string text;
    if (expired_ || disposed_) {
        text = "0s";
    } else {
        text = std::to_string(remaining) + "s";
    }
    return make_buffer({std::move(text)}, constraints);
}

}
