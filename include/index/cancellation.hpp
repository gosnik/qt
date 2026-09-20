#pragma once

#include <atomic>
#include <stdexcept>

namespace qt::index {

inline thread_local const std::atomic_bool* active_scan_cancellation = nullptr;

class ScopedScanCancellation final {
public:
    explicit ScopedScanCancellation(const std::atomic_bool& cancellation)
        : previous_(active_scan_cancellation) {
        active_scan_cancellation = &cancellation;
    }

    ~ScopedScanCancellation() {
        active_scan_cancellation = previous_;
    }

    ScopedScanCancellation(const ScopedScanCancellation&) = delete;
    ScopedScanCancellation& operator=(const ScopedScanCancellation&) = delete;

private:
    const std::atomic_bool* previous_ = nullptr;
};

inline void scan_cancellation_checkpoint() {
    if (active_scan_cancellation != nullptr &&
        active_scan_cancellation->load(std::memory_order_relaxed)) {
        throw std::runtime_error("index scan cancelled");
    }
}

}  // namespace qt::index
