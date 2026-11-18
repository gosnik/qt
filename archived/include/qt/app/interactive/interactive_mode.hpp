#pragma once

#include <optional>
#include <string>

#include "qt/app/cli/cli.hpp"
#include "qt/app/cli/startup.hpp"

namespace qt::app {

struct ReferenceInteractiveModeRequest {
  const ReferenceCliArgs& parsed;
  ReferenceStartupSession startup_session;
  std::optional<std::string> stdin_content;
  std::string cwd;
  std::string session_dir;
};

int run_interactive_mode(
    const ReferenceInteractiveModeRequest& request);

}  // namespace qt::app
