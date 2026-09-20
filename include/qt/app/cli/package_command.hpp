#pragma once

#include <optional>
#include <string>
#include <vector>

namespace qt::app {

std::optional<int> run_package_or_config_command(
    const std::vector<std::string>& raw_args);

}  // namespace qt::app
