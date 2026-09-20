#include "qt/app/interactive/terminal_suspend.hpp"

#include <cerrno>
#include <csignal>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace qt::app {
namespace {

qt::foundation::Status success_if_empty(
    const TerminalSuspendControllerOptions::Step& step) {
  if (!step) {
    return qt::foundation::Status::success();
  }
  return step();
}

qt::foundation::Status default_suspend_process() {
#if defined(_WIN32)
  return qt::foundation::Status::failure(
      qt::foundation::ErrorCode::invalid_argument,
      "suspend to background is not supported on Windows");
#else
  const auto previous_sigint = std::signal(SIGINT, SIG_IGN);
  const int rc = ::kill(0, SIGTSTP);
  std::signal(SIGINT, previous_sigint);
  if (rc != 0) {
    return qt::foundation::Status::failure(
        qt::foundation::ErrorCode::io_error,
        "failed to suspend process group", errno);
  }
  return qt::foundation::Status::success();
#endif
}

}  // namespace

TerminalSuspendController::TerminalSuspendController(
    TerminalSuspendControllerOptions options)
    : options_(std::move(options)) {}

qt::foundation::Status TerminalSuspendController::suspend_and_resume() {
  if (!options_.supported) {
    return qt::foundation::Status::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "suspend to background is not supported on this platform");
  }

  auto status = success_if_empty(options_.stop_tui);
  if (!status) {
    return status;
  }

  status = options_.suspend_process ? options_.suspend_process()
                                    : default_suspend_process();
  if (!status) {
    const auto restarted = success_if_empty(options_.start_tui);
    (void)restarted;
    return status;
  }

  status = success_if_empty(options_.start_tui);
  if (!status) {
    return status;
  }

  return success_if_empty(options_.render_tui);
}

}  // namespace qt::app
