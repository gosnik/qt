#include "qt/tui/input/kill_ring.hpp"

#include <utility>

namespace qt::tui {

void KillRing::push(std::string_view text, KillRingPushOptions options) {
    if (text.empty()) {
        return;
    }

    if (options.accumulate && !ring_.empty()) {
        if (options.prepend) {
            ring_.back().insert(0, text);
        } else {
            ring_.back().append(text);
        }
        return;
    }

    ring_.emplace_back(text);
}

std::optional<std::string_view> KillRing::peek() const noexcept {
    if (ring_.empty()) {
        return std::nullopt;
    }
    return std::string_view(ring_.back());
}

void KillRing::rotate() {
    if (ring_.size() <= 1U) {
        return;
    }

    std::string last = std::move(ring_.back());
    ring_.pop_back();
    ring_.insert(ring_.begin(), std::move(last));
}

void KillRing::clear() noexcept { ring_.clear(); }

std::size_t KillRing::size() const noexcept { return ring_.size(); }

bool KillRing::empty() const noexcept { return ring_.empty(); }

}  // namespace qt::tui
