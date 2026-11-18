#include "qt/app/interactive/interactive_turn_pump.hpp"

#include <chrono>
#include <utility>

namespace qt::app {

InteractiveTurnPump::InteractiveTurnPump(PromptExecutor executor)
    : executor_(std::move(executor)) {}

InteractiveTurnPump::~InteractiveTurnPump() {
  (void)poll(true);
}

bool InteractiveTurnPump::active() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.has_value();
}

foundation::Result<bool> InteractiveTurnPump::submit(
    InteractiveTurnPumpRequest request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_.has_value()) {
    return foundation::Result<bool>::success(false);
  }

  pending_.emplace(std::async(
      std::launch::async,
      [executor = executor_, request = std::move(request)]() mutable {
        return executor(std::move(request));
      }));
  return foundation::Result<bool>::success(true);
}

foundation::Result<std::optional<foundation::Result<std::string>>>
InteractiveTurnPump::poll(bool wait_for_completion) {
  std::future<foundation::Result<std::string>> ready;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_.has_value()) {
      return foundation::Result<
          std::optional<foundation::Result<std::string>>>::success(std::nullopt);
    }
    if (!wait_for_completion &&
        pending_->wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready) {
      return foundation::Result<
          std::optional<foundation::Result<std::string>>>::success(std::nullopt);
    }
    ready = std::move(*pending_);
    pending_.reset();
  }

  return foundation::Result<
      std::optional<foundation::Result<std::string>>>::success(ready.get());
}

foundation::Result<std::optional<foundation::Result<std::string>>>
InteractiveTurnPump::cancel_and_wait(CancellationHandler cancellation_handler) {
  if (!active()) {
    return foundation::Result<
        std::optional<foundation::Result<std::string>>>::success(std::nullopt);
  }
  if (cancellation_handler) {
    cancellation_handler();
  }
  auto completed = poll(true);
  if (!completed) {
    return completed.error();
  }
  (void)drain_queued_operations();
  return completed;
}

bool InteractiveTurnPump::queue_operation(
    InteractiveTurnPumpOperation operation) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  constexpr std::size_t kMaxOperations = 128U;
  constexpr std::size_t kMaxOperationBytes = 8U * 1024U * 1024U;
  if (!pending_.has_value() || queued_operations_.size() >= kMaxOperations ||
      operation.text.size() > kMaxOperationBytes - queued_operation_bytes_) {
    return false;
  }
  queued_operation_bytes_ += operation.text.size();
  queued_operations_.push_back(std::move(operation));
  return true;
}

std::vector<InteractiveTurnPumpOperation>
InteractiveTurnPump::drain_queued_operations() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<InteractiveTurnPumpOperation> result;
  result.swap(queued_operations_);
  queued_operation_bytes_ = 0U;
  return result;
}

}  // namespace qt::app
