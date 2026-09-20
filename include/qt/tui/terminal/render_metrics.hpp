#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>

namespace qt::tui {

struct RenderMetricsSnapshot {
    std::uint64_t render_count = 0;
    std::uint64_t total_render_us = 0;
    std::uint64_t transcript_render_us = 0;
    std::uint64_t bottom_render_us = 0;
    std::uint64_t diff_us = 0;
    std::uint64_t present_us = 0;
    std::uint64_t rows_scanned = 0;
    std::uint64_t rows_changed = 0;
};

class RenderMetricsRecorder {
  public:
    void record_total(std::chrono::steady_clock::duration duration) noexcept {
        render_count_.fetch_add(1U, std::memory_order_relaxed);
        total_render_us_.fetch_add(microseconds(duration), std::memory_order_relaxed);
    }

    void record_transcript(std::chrono::steady_clock::duration duration) noexcept {
        transcript_render_us_.fetch_add(microseconds(duration), std::memory_order_relaxed);
    }

    void record_bottom(std::chrono::steady_clock::duration duration) noexcept {
        bottom_render_us_.fetch_add(microseconds(duration), std::memory_order_relaxed);
    }

    void record_diff(std::chrono::steady_clock::duration duration,
                     std::size_t rows_scanned, std::size_t rows_changed) noexcept {
        diff_us_.fetch_add(microseconds(duration), std::memory_order_relaxed);
        rows_scanned_.fetch_add(static_cast<std::uint64_t>(rows_scanned),
                                std::memory_order_relaxed);
        rows_changed_.fetch_add(static_cast<std::uint64_t>(rows_changed),
                                std::memory_order_relaxed);
    }

    void record_present(std::chrono::steady_clock::duration duration) noexcept {
        present_us_.fetch_add(microseconds(duration), std::memory_order_relaxed);
    }

    [[nodiscard]] RenderMetricsSnapshot snapshot() const noexcept {
        return RenderMetricsSnapshot{
            render_count_.load(std::memory_order_relaxed),
            total_render_us_.load(std::memory_order_relaxed),
            transcript_render_us_.load(std::memory_order_relaxed),
            bottom_render_us_.load(std::memory_order_relaxed),
            diff_us_.load(std::memory_order_relaxed),
            present_us_.load(std::memory_order_relaxed),
            rows_scanned_.load(std::memory_order_relaxed),
            rows_changed_.load(std::memory_order_relaxed),
        };
    }

    void reset() noexcept {
        render_count_.store(0U, std::memory_order_relaxed);
        total_render_us_.store(0U, std::memory_order_relaxed);
        transcript_render_us_.store(0U, std::memory_order_relaxed);
        bottom_render_us_.store(0U, std::memory_order_relaxed);
        diff_us_.store(0U, std::memory_order_relaxed);
        present_us_.store(0U, std::memory_order_relaxed);
        rows_scanned_.store(0U, std::memory_order_relaxed);
        rows_changed_.store(0U, std::memory_order_relaxed);
    }

  private:
    static std::uint64_t microseconds(
        std::chrono::steady_clock::duration duration) noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
    }

    std::atomic<std::uint64_t> render_count_{0};
    std::atomic<std::uint64_t> total_render_us_{0};
    std::atomic<std::uint64_t> transcript_render_us_{0};
    std::atomic<std::uint64_t> bottom_render_us_{0};
    std::atomic<std::uint64_t> diff_us_{0};
    std::atomic<std::uint64_t> present_us_{0};
    std::atomic<std::uint64_t> rows_scanned_{0};
    std::atomic<std::uint64_t> rows_changed_{0};
};

}  // namespace qt::tui
