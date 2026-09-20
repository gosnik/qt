#pragma once

#include "qt/ai/models.hpp"
#include "qt/foundation/error.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/process.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qt::app::runtime {

struct IsolatedAgentRunnerOptions {
  std::string executable = "qt";
  std::vector<std::string> executable_arguments;
};

struct IsolatedAgentOutputUpdate {
  std::string stdout_text;
  std::string stderr_text;
  std::string final_output;
  qt::ai::Json final_messages = qt::ai::Json::array();
};

struct IsolatedAgentRequest {
  std::vector<std::string> arguments;
  // Framed protocols should include their terminating newline here. The
  // payload is written to the child's stdin and never exposed in argv.
  std::string stdin_payload;
  std::optional<qt::foundation::Path> cwd;
  std::vector<qt::foundation::ProcessEnvironmentVariable> environment;
  std::optional<std::int64_t> timeout_ms;
  std::shared_ptr<qt::foundation::ProcessCancellationToken> cancellation_token;
  std::function<void(const IsolatedAgentOutputUpdate&)> output_callback;
};

struct IsolatedAgentResult {
  int exit_code = 0;
  bool cancelled = false;
  bool timed_out = false;
  std::string stdout_text;
  std::string stderr_text;
  std::vector<qt::ai::Json> events;
  qt::ai::Json final_messages = qt::ai::Json::array();
  std::string final_output;
  std::vector<std::string> diagnostics;

  bool completed() const noexcept {
    return exit_code == 0 && !cancelled && !timed_out;
  }
};

class IsolatedAgentRunner final {
 public:
  explicit IsolatedAgentRunner(IsolatedAgentRunnerOptions options = {});

  const IsolatedAgentRunnerOptions& options() const noexcept { return options_; }
  qt::foundation::Result<IsolatedAgentResult> run(
      const IsolatedAgentRequest& request) const;

  // Parses qt JSON-mode NDJSON without launching a child. Streaming callers
  // receive the latest complete agent_end projection through this same path.
  static IsolatedAgentOutputUpdate parse_output_update(
      std::string stdout_text, std::string stderr_text);

 private:
  IsolatedAgentRunnerOptions options_;
};

}  // namespace qt::app::runtime
