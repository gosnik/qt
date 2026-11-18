#pragma once

#include "qt/foundation/error.hpp"
#include "qt/foundation/filesystem.hpp"

#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace qt::app {

inline constexpr std::size_t kDefaultPromptHistoryLimit = 500U;
inline constexpr std::size_t kMaximumPromptHistoryBytes = 8U * 1024U * 1024U;
inline constexpr std::string_view kPromptHistoryFilename = "history.jsonl";

struct PromptHistorySnapshot {
  std::vector<std::string> prompts;
  bool file_exists = false;
};

class PromptHistoryStore final {
 public:
  PromptHistoryStore(qt::foundation::Path agent_home,
                     std::size_t max_entries);

  [[nodiscard]] const qt::foundation::Path& path() const noexcept;
  [[nodiscard]] std::size_t max_entries() const noexcept;

  [[nodiscard]] qt::foundation::Result<PromptHistorySnapshot> load() const;
  qt::foundation::Status append(std::string_view prompt) const;
  qt::foundation::Status save(
      const std::vector<std::string>& newest_first) const;

 private:
  qt::foundation::Path path_;
  std::size_t max_entries_ = kDefaultPromptHistoryLimit;
};

// Serializes durable history mutations away from the terminal/UI thread. The
// bounded FIFO preserves prompt order; an overload is reported to the caller
// instead of silently discarding a prompt.
class PromptHistoryWriter final {
 public:
  explicit PromptHistoryWriter(PromptHistoryStore store,
                               std::size_t max_pending = 64U,
                               std::size_t max_pending_bytes = 1024U * 1024U);
  PromptHistoryWriter(const PromptHistoryWriter&) = delete;
  PromptHistoryWriter& operator=(const PromptHistoryWriter&) = delete;
  ~PromptHistoryWriter();

  [[nodiscard]] qt::foundation::Status append(std::string prompt,
                                               std::size_t max_entries,
                                               std::function<qt::foundation::Status()> after = {},
                                               std::size_t retained_bytes = 0U);
  [[nodiscard]] qt::foundation::Status clear(
      std::size_t max_entries,
      std::function<qt::foundation::Status()> after = {});
  [[nodiscard]] qt::foundation::Status save(std::vector<std::string> newest_first,
                                             std::size_t max_entries);
  [[nodiscard]] std::vector<std::string> latest_accepted_history() const;
  [[nodiscard]] std::vector<qt::foundation::Error> take_errors();
  [[nodiscard]] qt::foundation::Status flush();
  void stop();

 private:
  enum class OperationKind { append, clear, save };
  struct Operation {
    OperationKind kind = OperationKind::append;
    std::string prompt;
    std::vector<std::string> history;
    std::size_t max_entries = kDefaultPromptHistoryLimit;
    std::function<qt::foundation::Status()> after;
    std::size_t retained_bytes = 0U;
  };

  void run();

  PromptHistoryStore store_;
  std::size_t max_pending_ = 64U;
  std::size_t max_pending_bytes_ = 1024U * 1024U;
  std::size_t pending_bytes_ = 0U;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable drained_;
  std::deque<Operation> operations_;
  std::vector<qt::foundation::Error> errors_;
  std::vector<std::string> latest_accepted_;
  bool stopping_ = false;
  bool writing_ = false;
};

}  // namespace qt::app
