#pragma once

#include "qt/tui/widgets/loader.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace qt::app {

class InteractiveWorkingIndicator {
 public:
  InteractiveWorkingIndicator();

  void set_default_message(std::string message);
  void set_message(std::optional<std::string> message);
  void set_indicator(qt::tui::LoaderIndicator indicator);
  void set_visible(bool visible) noexcept { visible_ = visible; }

  void start(std::chrono::steady_clock::time_point now =
                 std::chrono::steady_clock::now());
  void stop() noexcept;

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool advance_if_due(std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::optional<std::string> status_line() const;

 private:
  [[nodiscard]] std::string resolved_message() const;

  std::string default_message_ = "Working...";
  std::optional<std::string> message_override_;
  qt::tui::LoaderIndicator indicator_;
  qt::tui::Loader loader_;
  bool active_ = false;
  bool visible_ = true;
  std::chrono::steady_clock::time_point last_advance_{};
};

}  // namespace qt::app
