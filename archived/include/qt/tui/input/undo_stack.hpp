#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <deque>

namespace qt::tui {

template <typename Snapshot>
class UndoStack {
  public:
    // A zero budget preserves the unbounded compatibility behavior.  A bounded
    // stack evicts the oldest snapshot before accepting a new one.
    void set_byte_limit(std::size_t limit) {
        byte_limit_ = limit;
        trim();
    }

    [[nodiscard]] std::size_t byte_limit() const noexcept { return byte_limit_; }
    [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }

    void push(const Snapshot& snapshot, std::size_t bytes = 0U) {
        Snapshot copy(snapshot);
        push(std::move(copy), bytes);
    }

    void push(Snapshot&& snapshot, std::size_t bytes = 0U) {
        if (byte_limit_ != 0U && bytes > byte_limit_) {
            clear();
            return;
        }
        stack_.push_back(std::move(snapshot));
        costs_.push_back(bytes);
        retained_bytes_ += bytes;
        trim();
    }

    [[nodiscard]] std::optional<Snapshot> pop() {
        if (stack_.empty()) {
            return std::nullopt;
        }

        Snapshot snapshot = std::move(stack_.back());
        retained_bytes_ -= costs_.back();
        stack_.pop_back();
        costs_.pop_back();
        return snapshot;
    }

    void clear() noexcept {
        stack_.clear();
        costs_.clear();
        retained_bytes_ = 0U;
    }
    [[nodiscard]] std::size_t size() const noexcept { return stack_.size(); }
    [[nodiscard]] bool empty() const noexcept { return stack_.empty(); }

  private:
    void trim() {
        if (byte_limit_ == 0U) {
            return;
        }
        while (!stack_.empty() && retained_bytes_ > byte_limit_) {
            retained_bytes_ -= costs_.front();
            stack_.pop_front();
            costs_.pop_front();
        }
    }

    std::deque<Snapshot> stack_;
    std::deque<std::size_t> costs_;
    std::size_t byte_limit_ = 0U;
    std::size_t retained_bytes_ = 0U;
};

}  // namespace qt::tui
