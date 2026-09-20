#pragma once

#include "qt/foundation/error.hpp"
#include "qt/foundation/filesystem.hpp"

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qt::foundation {

enum class ProcessLaunchMode {
    shell,
    argv,
};

struct ProcessEnvironmentVariable {
    std::string name;
    std::optional<std::string> value;
};

struct ProcessTimeout {
    std::int64_t milliseconds = 0;
};

class ProcessCancellationToken {
  public:
    void cancel() noexcept;
    bool is_cancelled() const noexcept;

  private:
    std::atomic_bool cancelled_{false};
};

struct ProcessOutputUpdate {
    std::string stdout_text;
    std::string stderr_text;
};

struct ProcessRequest {
    ProcessLaunchMode mode = ProcessLaunchMode::shell;
    std::string command;
    std::vector<std::string> argv;
    std::string stdin_text;
    std::optional<Path> cwd;
    std::vector<ProcessEnvironmentVariable> environment;
    std::optional<ProcessTimeout> timeout;
    std::optional<std::int64_t> timeout_ms;
    std::shared_ptr<ProcessCancellationToken> cancellation_token;
    std::function<void(const ProcessOutputUpdate&)> output_callback;
    std::int64_t output_update_interval_ms = 100;
};

struct ProcessCommandPlan {
    ProcessLaunchMode mode = ProcessLaunchMode::shell;
    std::string command;
    std::vector<std::string> argv;
    std::string display_command;
    std::string stdin_text;
    std::optional<Path> cwd;
    std::vector<ProcessEnvironmentVariable> environment;
    std::optional<ProcessTimeout> timeout;
    std::shared_ptr<ProcessCancellationToken> cancellation_token;
    std::function<void(const ProcessOutputUpdate&)> output_callback;
    std::int64_t output_update_interval_ms = 100;
};

struct ProcessResult {
    ProcessLaunchMode mode = ProcessLaunchMode::shell;
    std::string command;
    std::vector<std::string> argv;
    std::string display_command;
    std::string stdin_text;
    std::optional<Path> cwd;
    std::vector<ProcessEnvironmentVariable> environment;
    std::optional<ProcessTimeout> timeout;
    std::optional<std::int64_t> timeout_ms;
    bool timed_out = false;
    bool cancelled = false;
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

Result<ProcessCommandPlan> plan_process(const ProcessRequest& request);
ProcessResult make_process_result(const ProcessCommandPlan& plan,
                                  bool timed_out,
                                  bool cancelled,
                                  int exit_code,
                                  std::string stdout_text,
                                  std::string stderr_text);
Result<ProcessResult> run_process(const ProcessRequest& request);
Result<ProcessResult> run_shell_command(std::string command,
                                        std::optional<Path> cwd = std::nullopt,
                                        std::optional<std::int64_t> timeout_ms = std::nullopt,
                                        std::shared_ptr<ProcessCancellationToken> cancellation_token = nullptr);
Result<ProcessResult> run_argv_process(std::vector<std::string> argv,
                                       std::optional<Path> cwd = std::nullopt,
                                       std::optional<std::int64_t> timeout_ms = std::nullopt,
                                       std::shared_ptr<ProcessCancellationToken> cancellation_token = nullptr);

}  // namespace qt::foundation
