#include "qt/foundation/process.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <fstream>
#include <filesystem>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace qt::foundation {
namespace {

constexpr int kTimeoutExitCode = 124;

class ScopedFileDescriptor {
  public:
    explicit ScopedFileDescriptor(int descriptor = -1) : descriptor_(descriptor) {}

    ~ScopedFileDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
    ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

    ScopedFileDescriptor(ScopedFileDescriptor&& other) noexcept
        : descriptor_(other.descriptor_) {
        other.descriptor_ = -1;
    }

    ScopedFileDescriptor& operator=(ScopedFileDescriptor&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset(other.descriptor_);
        other.descriptor_ = -1;
        return *this;
    }

    int get() const noexcept { return descriptor_; }

    void reset(int descriptor = -1) noexcept {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
        descriptor_ = descriptor;
    }

  private:
    int descriptor_ = -1;
};

class ScopedTempPath {
  public:
    explicit ScopedTempPath(Path path) : path_(std::move(path)) {}
    ~ScopedTempPath() {
        if (!path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    const Path& path() const noexcept { return path_; }

  private:
    Path path_;
};

Result<Path> make_temp_capture_path(std::string_view label) {
    std::string pattern = "/tmp/qt-process-";
    pattern.append(label.data(), label.size());
    pattern.append("-XXXXXX");

    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');

    const int fd = ::mkstemp(buffer.data());
    if (fd < 0) {
        return Result<Path>::failure(ErrorCode::io_error,
                                     "unable to create temporary capture file",
                                     errno);
    }
    ::close(fd);
    return Path(buffer.data());
}

Result<std::string> read_capture_file(const Path& path) {
    return read_text_file(path);
}

bool append_capture_delta(const Path& path, off_t& offset, std::string& target) {
    ScopedFileDescriptor fd(::open(path.c_str(), O_RDONLY));
    if (fd.get() < 0) {
        return false;
    }
    if (::lseek(fd.get(), offset, SEEK_SET) < 0) {
        return false;
    }

    bool appended = false;
    std::array<char, 8192> buffer{};
    while (true) {
        const ssize_t bytes_read = ::read(fd.get(), buffer.data(), buffer.size());
        if (bytes_read > 0) {
            target.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            offset += bytes_read;
            appended = true;
            continue;
        }
        if (bytes_read == 0) {
            return appended;
        }
        if (errno == EINTR) {
            continue;
        }
        return appended;
    }
}

int normalized_exit_code(int wait_status) {
    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        return 128 + WTERMSIG(wait_status);
    }
    return wait_status;
}

std::string shell_quote_argument(std::string_view argument) {
    if (argument.empty()) {
        return "''";
    }

    bool safe = true;
    for (const char character : argument) {
        const bool alpha_numeric =
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        const bool punctuation = character == '_' || character == '-' ||
                                 character == '.' || character == '/' ||
                                 character == ':' || character == '=';
        if (!alpha_numeric && !punctuation) {
            safe = false;
            break;
        }
    }
    if (safe) {
        return std::string(argument);
    }

    std::string quoted = "'";
    for (const char character : argument) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string display_command_for(const ProcessRequest& request) {
    if (request.mode == ProcessLaunchMode::shell) {
        return request.command;
    }

    std::string display;
    for (const auto& entry : request.argv) {
        if (!display.empty()) {
            display.push_back(' ');
        }
        display += shell_quote_argument(entry);
    }
    return display;
}

std::optional<ProcessTimeout> normalized_timeout(const ProcessRequest& request) {
    if (request.timeout.has_value()) {
        return request.timeout;
    }
    if (request.timeout_ms.has_value()) {
        return ProcessTimeout{*request.timeout_ms};
    }
    return std::nullopt;
}

Result<Path> validate_cwd(const std::optional<Path>& cwd) {
    if (!cwd.has_value()) {
        return Path();
    }
    std::error_code error;
    if (!std::filesystem::exists(*cwd, error)) {
        return Result<Path>::failure(ErrorCode::not_found,
                                     "working directory does not exist");
    }
    if (!std::filesystem::is_directory(*cwd, error)) {
        return Result<Path>::failure(ErrorCode::invalid_argument,
                                     "working directory is not a directory");
    }
    return *cwd;
}

Result<ProcessCommandPlan> validate_request(const ProcessRequest& request) {
    if (request.mode == ProcessLaunchMode::shell) {
        if (request.command.empty()) {
            return Result<ProcessCommandPlan>::failure(
                ErrorCode::invalid_argument, "shell command must not be empty");
        }
    } else {
        if (request.argv.empty()) {
            return Result<ProcessCommandPlan>::failure(ErrorCode::invalid_argument,
                                                       "argv must not be empty");
        }
        for (const auto& entry : request.argv) {
            if (entry.empty()) {
                return Result<ProcessCommandPlan>::failure(
                    ErrorCode::invalid_argument, "argv entries must not be empty");
            }
        }
    }

    if (request.timeout.has_value() && request.timeout->milliseconds < 0) {
        return Result<ProcessCommandPlan>::failure(
            ErrorCode::invalid_argument, "timeout milliseconds must be non-negative");
    }
    if (request.timeout_ms.has_value() && *request.timeout_ms < 0) {
        return Result<ProcessCommandPlan>::failure(ErrorCode::invalid_argument,
                                                   "timeout_ms must be non-negative");
    }
    if (request.timeout.has_value() && request.timeout_ms.has_value() &&
        request.timeout->milliseconds != *request.timeout_ms) {
        return Result<ProcessCommandPlan>::failure(
            ErrorCode::invalid_argument,
            "timeout and timeout_ms must describe the same duration");
    }

    for (const auto& variable : request.environment) {
        if (variable.name.empty()) {
            return Result<ProcessCommandPlan>::failure(
                ErrorCode::invalid_argument,
                "environment variable names must not be empty");
        }
        if (variable.name.find('=') != std::string::npos) {
            return Result<ProcessCommandPlan>::failure(
                ErrorCode::invalid_argument,
                "environment variable names must not contain '='");
        }
    }

    auto cwd = validate_cwd(request.cwd);
    if (!cwd) {
        return Result<ProcessCommandPlan>(cwd.error());
    }

    ProcessCommandPlan plan;
    plan.mode = request.mode;
    plan.command = request.command;
    plan.argv = request.argv;
    plan.display_command = display_command_for(request);
    plan.stdin_text = request.stdin_text;
    plan.cwd = request.cwd;
    plan.environment = request.environment;
    plan.timeout = normalized_timeout(request);
    plan.cancellation_token = request.cancellation_token;
    plan.output_callback = request.output_callback;
    plan.output_update_interval_ms =
        request.output_update_interval_ms <= 0 ? 100
                                               : request.output_update_interval_ms;
    return plan;
}

bool timeout_is_active(const ProcessCommandPlan& plan) {
    return plan.timeout.has_value() && plan.timeout->milliseconds > 0;
}

[[noreturn]] void write_child_error_and_exit(int error_fd, const char* message) {
    (void)::write(error_fd, message, std::strlen(message));
    (void)::write(error_fd, "\n", 1);
    _exit(127);
}

[[noreturn]] void write_errno_and_exit(int error_fd) {
    write_child_error_and_exit(error_fd, std::strerror(errno));
}

std::vector<char*> make_execvp_argv(const std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const auto& argument : arguments) {
        // POSIX execvp does not take ownership or mutate argv strings before
        // replacing the process image; plan.argv owns the storage until then.
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    return argv;
}

void apply_environment_or_exit(const ProcessCommandPlan& plan, int error_fd) {
    for (const auto& variable : plan.environment) {
        if (variable.value.has_value()) {
            if (::setenv(variable.name.c_str(), variable.value->c_str(), 1) != 0) {
                write_errno_and_exit(error_fd);
            }
        } else if (::unsetenv(variable.name.c_str()) != 0) {
            write_errno_and_exit(error_fd);
        }
    }
}

[[noreturn]] void exec_shell_or_exit(const ProcessCommandPlan& plan, int error_fd) {
    ::execl("/bin/bash", "bash", "-lc", plan.command.c_str(),
            static_cast<char*>(nullptr));

    write_errno_and_exit(error_fd);
}

[[noreturn]] void exec_argv_or_exit(const ProcessCommandPlan& plan, int error_fd) {
    std::vector<char*> argv = make_execvp_argv(plan.argv);

    ::execvp(argv[0], argv.data());

    write_errno_and_exit(error_fd);
}

struct WaitForChildResult {
    int exit_code = 0;
    bool timed_out = false;
    bool cancelled = false;
};

void collect_descendants(pid_t parent, std::vector<pid_t>& descendants) {
    // Freeze each process before inspecting its threads so it cannot create
    // another child between enumeration and recursive termination.
    (void)::kill(parent, SIGSTOP);
    const auto task_directory = std::filesystem::path("/proc") /
                                std::to_string(parent) / "task";
    std::error_code error;
    std::filesystem::directory_iterator task(task_directory, error), end;
    while (!error && task != end) {
        std::ifstream input(task->path() / "children");
        pid_t child = 0;
        while (input >> child) {
            if (child > 0) {
                descendants.push_back(child);
                collect_descendants(child, descendants);
            }
        }
        task.increment(error);
    }
}

void kill_process_tree(pid_t child_pid) {
    if (child_pid <= 0) {
        return;
    }
    // A nested qt/pi child creates its own process group. Walk the kernel's
    // descendant list first so those groups are terminated as well.
    std::vector<pid_t> descendants;
    collect_descendants(child_pid, descendants);
    for (auto it = descendants.rbegin(); it != descendants.rend(); ++it) {
        if (::kill(-*it, SIGKILL) != 0 && errno == ESRCH) {
            (void)::kill(*it, SIGKILL);
        }
    }
    if (::kill(-child_pid, SIGKILL) != 0 && errno == ESRCH) {
        (void)::kill(child_pid, SIGKILL);
    }
}

Result<WaitForChildResult> wait_for_child(pid_t child_pid,
                                          const ProcessCommandPlan& plan,
                                          const std::function<void()>& poll_output) {
    int wait_status = 0;

    const auto deadline = timeout_is_active(plan)
                              ? std::optional<std::chrono::steady_clock::time_point>(
                                    std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(plan.timeout->milliseconds))
                              : std::nullopt;

    while (true) {
        if (poll_output) {
            poll_output();
        }

        const pid_t waited = ::waitpid(child_pid, &wait_status, WNOHANG);
        if (waited == child_pid) {
            return WaitForChildResult{normalized_exit_code(wait_status), false,
                                      false};
        }
        if (waited < 0) {
            return Result<WaitForChildResult>::failure(
                ErrorCode::io_error, "unable to wait for process", errno);
        }

        if (plan.cancellation_token && plan.cancellation_token->is_cancelled()) {
            kill_process_tree(child_pid);
            while (::waitpid(child_pid, &wait_status, 0) < 0) {
                if (errno != EINTR) {
                    return Result<WaitForChildResult>::failure(
                        ErrorCode::io_error,
                        "unable to wait for cancelled process", errno);
                }
            }
            return WaitForChildResult{0, false, true};
        }

        if (deadline.has_value() && std::chrono::steady_clock::now() >= *deadline) {
            kill_process_tree(child_pid);
            while (::waitpid(child_pid, &wait_status, 0) < 0) {
                if (errno != EINTR) {
                    return Result<WaitForChildResult>::failure(
                        ErrorCode::io_error,
                        "unable to wait for timed out process", errno);
                }
            }
            return WaitForChildResult{kTimeoutExitCode, true, false};
        }
        ::usleep(1000);
    }
}

Result<ProcessResult> execute_request(const ProcessCommandPlan& plan) {
    const auto stdout_path = make_temp_capture_path("stdout");
    if (!stdout_path) {
        return Result<ProcessResult>(stdout_path.error());
    }
    const auto stderr_path = make_temp_capture_path("stderr");
    if (!stderr_path) {
        return Result<ProcessResult>(stderr_path.error());
    }

    ScopedTempPath stdout_capture(stdout_path.value());
    ScopedTempPath stderr_capture(stderr_path.value());

    std::array<int, 2> stdin_pipe{{-1, -1}};
    ScopedFileDescriptor stdin_read;
    ScopedFileDescriptor stdin_write;
    if (!plan.stdin_text.empty()) {
        if (::pipe(stdin_pipe.data()) != 0) {
            return Result<ProcessResult>::failure(
                ErrorCode::io_error, "unable to create stdin pipe", errno);
        }
        stdin_read.reset(stdin_pipe[0]);
        stdin_write.reset(stdin_pipe[1]);
    }

    ScopedFileDescriptor stdout_fd(
        ::open(stdout_capture.path().c_str(), O_WRONLY | O_TRUNC));
    if (stdout_fd.get() < 0) {
        return Result<ProcessResult>::failure(ErrorCode::io_error,
                                              "unable to open stdout capture file",
                                              errno);
    }
    ScopedFileDescriptor stderr_fd(
        ::open(stderr_capture.path().c_str(), O_WRONLY | O_TRUNC));
    if (stderr_fd.get() < 0) {
        return Result<ProcessResult>::failure(ErrorCode::io_error,
                                              "unable to open stderr capture file",
                                              errno);
    }

    std::array<int, 2> exec_error_pipe{{-1, -1}};
    if (::pipe(exec_error_pipe.data()) != 0) {
        return Result<ProcessResult>::failure(ErrorCode::io_error,
                                              "unable to create execution error pipe",
                                              errno);
    }

    ScopedFileDescriptor exec_error_read(exec_error_pipe[0]);
    ScopedFileDescriptor exec_error_write(exec_error_pipe[1]);
    if (::fcntl(exec_error_write.get(), F_SETFD, FD_CLOEXEC) != 0) {
        return Result<ProcessResult>::failure(
            ErrorCode::io_error, "unable to configure execution error pipe", errno);
    }

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        return Result<ProcessResult>::failure(ErrorCode::io_error,
                                              "unable to fork process", errno);
    }

    if (child_pid == 0) {
        exec_error_read.reset();
        if (stdin_write.get() >= 0) {
            stdin_write.reset();
        }

        (void)::setpgid(0, 0);
        if (plan.cwd.has_value() && ::chdir(plan.cwd->c_str()) != 0) {
            write_errno_and_exit(exec_error_write.get());
        }
        if (stdin_read.get() >= 0 &&
            ::dup2(stdin_read.get(), STDIN_FILENO) < 0) {
            write_errno_and_exit(exec_error_write.get());
        }
        if (::dup2(stdout_fd.get(), STDOUT_FILENO) < 0) {
            write_errno_and_exit(exec_error_write.get());
        }
        if (::dup2(stderr_fd.get(), STDERR_FILENO) < 0) {
            write_errno_and_exit(exec_error_write.get());
        }

        stdin_read.reset();
        stdout_fd.reset();
        stderr_fd.reset();

        apply_environment_or_exit(plan, exec_error_write.get());

        if (plan.mode == ProcessLaunchMode::shell) {
            exec_shell_or_exit(plan, exec_error_write.get());
        }
        exec_argv_or_exit(plan, exec_error_write.get());
    }

    (void)::setpgid(child_pid, child_pid);
    exec_error_write.reset();
    stdin_read.reset();
    stdout_fd.reset();
    stderr_fd.reset();

    if (stdin_write.get() >= 0) {
        std::string_view remaining(plan.stdin_text);
        while (!remaining.empty()) {
            const ssize_t written =
                ::write(stdin_write.get(), remaining.data(), remaining.size());
            if (written > 0) {
                remaining.remove_prefix(static_cast<std::size_t>(written));
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written < 0 && errno == EPIPE) {
                break;
            }
            kill_process_tree(child_pid);
            return Result<ProcessResult>::failure(
                ErrorCode::io_error, "unable to write process stdin", errno);
        }
        stdin_write.reset();
    }

    off_t stdout_offset = 0;
    off_t stderr_offset = 0;
    std::string observed_stdout;
    std::string observed_stderr;
    bool output_dirty = false;
    auto next_output_poll = std::chrono::steady_clock::now();
    auto next_output_update = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(
                                  plan.output_update_interval_ms);
    const auto poll_output = [&]() {
        if (!plan.output_callback) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < next_output_poll) {
            return;
        }
        next_output_poll = now + std::chrono::milliseconds(20);
        const bool stdout_appended =
            append_capture_delta(stdout_capture.path(), stdout_offset,
                                 observed_stdout);
        const bool stderr_appended =
            append_capture_delta(stderr_capture.path(), stderr_offset,
                                 observed_stderr);
        output_dirty = output_dirty || stdout_appended || stderr_appended;
        if (!output_dirty || now < next_output_update) {
            return;
        }
        next_output_update =
            now + std::chrono::milliseconds(plan.output_update_interval_ms);
        output_dirty = false;
        plan.output_callback(
            ProcessOutputUpdate{observed_stdout, observed_stderr});
    };

    const auto wait_result = wait_for_child(child_pid, plan, poll_output);
    if (!wait_result) {
        return Result<ProcessResult>(wait_result.error());
    }

    std::string exec_error_text;
    {
        std::array<char, 256> buffer{};
        ssize_t bytes_read = 0;
        while ((bytes_read = ::read(exec_error_read.get(), buffer.data(),
                                    buffer.size())) > 0) {
            exec_error_text.append(buffer.data(),
                                   static_cast<std::size_t>(bytes_read));
        }
        if (bytes_read < 0) {
            return Result<ProcessResult>::failure(
                ErrorCode::io_error, "unable to read execution error pipe", errno);
        }
    }

    const auto stdout_text = read_capture_file(stdout_capture.path());
    if (!stdout_text) {
        return Result<ProcessResult>(stdout_text.error());
    }
    const auto stderr_text = read_capture_file(stderr_capture.path());
    if (!stderr_text) {
        return Result<ProcessResult>(stderr_text.error());
    }

    if (!exec_error_text.empty()) {
        while (!exec_error_text.empty() &&
               (exec_error_text.back() == '\n' || exec_error_text.back() == '\r')) {
            exec_error_text.pop_back();
        }
        return Result<ProcessResult>::failure(ErrorCode::not_found, exec_error_text);
    }

    return make_process_result(plan, wait_result.value().timed_out,
                               wait_result.value().cancelled,
                               wait_result.value().exit_code,
                               stdout_text.value(), stderr_text.value());
}

}  // namespace

void ProcessCancellationToken::cancel() noexcept {
    cancelled_.store(true, std::memory_order_release);
}

bool ProcessCancellationToken::is_cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
}

Result<ProcessCommandPlan> plan_process(const ProcessRequest& request) {
    return validate_request(request);
}

ProcessResult make_process_result(const ProcessCommandPlan& plan,
                                  bool timed_out,
                                  bool cancelled,
                                  int exit_code,
                                  std::string stdout_text,
                                  std::string stderr_text) {
    ProcessResult result;
    result.mode = plan.mode;
    result.command = plan.command;
    result.argv = plan.argv;
    result.display_command = plan.display_command;
    result.stdin_text = plan.stdin_text;
    result.cwd = plan.cwd;
    result.environment = plan.environment;
    result.timeout = plan.timeout;
    if (plan.timeout.has_value()) {
        result.timeout_ms = plan.timeout->milliseconds;
    }
    result.timed_out = timed_out;
    result.cancelled = cancelled;
    result.exit_code = exit_code;
    result.stdout_text = std::move(stdout_text);
    result.stderr_text = std::move(stderr_text);
    return result;
}

Result<ProcessResult> run_process(const ProcessRequest& request) {
    const auto plan = plan_process(request);
    if (!plan) {
        return Result<ProcessResult>(plan.error());
    }
    return execute_request(plan.value());
}

Result<ProcessResult> run_shell_command(std::string command, std::optional<Path> cwd,
                                        std::optional<std::int64_t> timeout_ms,
                                        std::shared_ptr<ProcessCancellationToken> cancellation_token) {
    ProcessRequest request;
    request.mode = ProcessLaunchMode::shell;
    request.command = std::move(command);
    request.cwd = std::move(cwd);
    request.timeout_ms = timeout_ms;
    request.cancellation_token = std::move(cancellation_token);
    return run_process(request);
}

Result<ProcessResult> run_argv_process(std::vector<std::string> argv,
                                       std::optional<Path> cwd,
                                       std::optional<std::int64_t> timeout_ms,
                                       std::shared_ptr<ProcessCancellationToken> cancellation_token) {
    ProcessRequest request;
    request.mode = ProcessLaunchMode::argv;
    request.argv = std::move(argv);
    request.cwd = std::move(cwd);
    request.timeout_ms = timeout_ms;
    request.cancellation_token = std::move(cancellation_token);
    return run_process(request);
}

}  // namespace qt::foundation
