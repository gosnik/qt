#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct KillRingPushOptions {
    bool prepend = false;
    bool accumulate = false;
};

class KillRing {
  public:
    void push(std::string_view text, KillRingPushOptions options = {});
    [[nodiscard]] std::optional<std::string_view> peek() const noexcept;
    void rotate();
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    std::vector<std::string> ring_;
};

}  // namespace qt::tui
