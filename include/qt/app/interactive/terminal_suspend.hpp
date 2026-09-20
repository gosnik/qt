#pragma once

#include "qt/foundation/error.hpp"

#include <functional>

namespace qt::app {

struct TerminalSuspendControllerOptions {
  using Step = std::function<qt::foundation::Status()>;

  bool supported = true;
  Step stop_tui;
  Step suspend_process;
  Step start_tui;
  Step render_tui;
};

class TerminalSuspendController {
 public:
  explicit TerminalSuspendController(TerminalSuspendControllerOptions options);

  [[nodiscard]] qt::foundation::Status suspend_and_resume();

 private:
  TerminalSuspendControllerOptions options_;
};

}  // namespace qt::app
