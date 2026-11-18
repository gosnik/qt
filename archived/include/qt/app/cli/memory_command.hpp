#pragma once

#include <optional>
#include <string>
#include <vector>

namespace qt::app {

std::optional<int> run_memory_command(
    const std::vector<std::string>& raw_args);

}  // namespace qt::app
