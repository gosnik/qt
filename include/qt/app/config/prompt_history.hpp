#pragma once

#include "qt/foundation/error.hpp"
#include "qt/foundation/filesystem.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qt::app {

inline constexpr std::size_t kDefaultPromptHistoryLimit = 500U;
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

}  // namespace qt::app
