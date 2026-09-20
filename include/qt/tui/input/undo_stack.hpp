#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace qt::tui {

template <typename Snapshot>
class UndoStack {
  public:
    void push(const Snapshot& snapshot) { stack_.push_back(snapshot); }
    void push(Snapshot&& snapshot) { stack_.push_back(std::move(snapshot)); }

    [[nodiscard]] std::optional<Snapshot> pop() {
        if (stack_.empty()) {
            return std::nullopt;
        }

        Snapshot snapshot = std::move(stack_.back());
        stack_.pop_back();
        return snapshot;
    }

    void clear() noexcept { stack_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return stack_.size(); }
    [[nodiscard]] bool empty() const noexcept { return stack_.empty(); }

  private:
    std::vector<Snapshot> stack_;
};

}  // namespace qt::tui
