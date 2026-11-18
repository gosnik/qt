#pragma once

#include "qt/ai/models.hpp"
#include "qt/foundation/error.hpp"
#include "qt/tools/tool.hpp"

#include <future>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace qt::app {

struct InteractiveTurnPumpRequest {
  std::string prompt;
  std::string message_id;
  std::vector<qt::ai::ImageContent> images;
  qt::ai::Json metadata = qt::ai::Json::object();
  std::optional<qt::tools::ToolRequest> tool_request;
};

enum class InteractiveTurnPumpOperationKind {
  kSteer,
  kFollowUp,
};

struct InteractiveTurnPumpOperation {
  InteractiveTurnPumpOperationKind kind = InteractiveTurnPumpOperationKind::kSteer;
  std::string text;
};

class InteractiveTurnPump {
 public:
  using PromptExecutor =
      std::function<foundation::Result<std::string>(InteractiveTurnPumpRequest)>;
  using CancellationHandler = std::function<void()>;

  explicit InteractiveTurnPump(PromptExecutor executor);
  ~InteractiveTurnPump();

  InteractiveTurnPump(const InteractiveTurnPump&) = delete;
  InteractiveTurnPump& operator=(const InteractiveTurnPump&) = delete;

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] foundation::Result<bool> submit(InteractiveTurnPumpRequest request);
  [[nodiscard]] foundation::Result<std::optional<foundation::Result<std::string>>>
  poll(bool wait_for_completion = false);
  [[nodiscard]] foundation::Result<std::optional<foundation::Result<std::string>>>
  cancel_and_wait(CancellationHandler cancellation_handler);

  // Queue a steer or follow-up operation to be executed on the UI thread.
  // Returns false when inactive or over capacity; the caller must report or
  // retry that unaccepted operation. Accepted operations retain FIFO order.
  [[nodiscard]] bool queue_operation(InteractiveTurnPumpOperation operation) noexcept;

  // Drain all pending queue operations. Called on the UI thread after poll().
  // Returns the operations and clears the internal queue.
  std::vector<InteractiveTurnPumpOperation> drain_queued_operations() noexcept;

 private:
  PromptExecutor executor_;
  mutable std::mutex mutex_;
  std::optional<std::future<foundation::Result<std::string>>> pending_;
  std::vector<InteractiveTurnPumpOperation> queued_operations_;
  std::size_t queued_operation_bytes_ = 0U;
};

}  // namespace qt::app
