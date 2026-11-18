#pragma once

#include "qt/app/cli/cli.hpp"
#include "qt/app/cli/startup.hpp"

#include <string>

namespace qt::app {

int run_rpc_mode(const ReferenceCliArgs& parsed,
                           const ReferenceStartupSession& startup_session,
                           const std::string& cwd,
                           const std::string& agent_home);

}  // namespace qt::app
