#include "qt/app/interactive/interactive_working_indicator.hpp"

#include <utility>

namespace qt::app {
namespace {

qt::tui::LoaderIndicator default_indicator() {
  return qt::tui::LoaderIndicator{
      {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}, 80};
}

}  // namespace

InteractiveWorkingIndicator::InteractiveWorkingIndicator()
    : indicator_(default_indicator()), loader_(default_message_, indicator_) {}

void InteractiveWorkingIndicator::set_default_message(std::string message) {
  default_message_ = std::move(message);
  loader_.set_message(resolved_message());
}

void InteractiveWorkingIndicator::set_message(std::optional<std::string> message) {
  message_override_ = std::move(message);
  loader_.set_message(resolved_message());
}

void InteractiveWorkingIndicator::set_indicator(qt::tui::LoaderIndicator indicator) {
  indicator_ = std::move(indicator);
  loader_.set_indicator(indicator_);
  loader_.set_message(resolved_message());
}

void InteractiveWorkingIndicator::start(std::chrono::steady_clock::time_point now) {
  active_ = true;
  loader_.set_indicator(indicator_);
  loader_.set_message(resolved_message());
  last_advance_ = now;
}

void InteractiveWorkingIndicator::stop() noexcept { active_ = false; }

bool InteractiveWorkingIndicator::active() const noexcept { return active_; }

bool InteractiveWorkingIndicator::advance_if_due(
    std::chrono::steady_clock::time_point now) {
  if (!active() || indicator_.frames.size() <= 1U) {
    return false;
  }

  const auto interval = std::chrono::milliseconds(loader_.interval_ms());
  if (now - last_advance_ < interval) {
    return false;
  }

  bool advanced = false;
  while (now - last_advance_ >= interval) {
    loader_.advance_frame();
    last_advance_ += interval;
    advanced = true;
  }
  return advanced;
}

std::optional<std::string> InteractiveWorkingIndicator::status_line() const {
  if (!active() || !visible_) {
    return std::nullopt;
  }

  std::string line;
  const auto frame = loader_.current_frame();
  if (!frame.empty()) {
    line = frame + " ";
  }
  line += loader_.message();
  return line;
}

std::string InteractiveWorkingIndicator::resolved_message() const {
  return message_override_.value_or(default_message_);
}

}  // namespace qt::app
