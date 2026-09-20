#pragma once

#include <optional>
#include <qt/app/cli/cli.hpp>
#include <qt/app/cli/startup.hpp>
#include <string>

namespace qt::app {

int run_print_or_json_mode(
    const ReferenceCliArgs& parsed, ReferenceStartupMode mode,
    const ReferenceStartupSession& startup_session,
    const std::optional<std::string>& stdin_content);

}  // namespace qt::app
