#pragma once

#include "qt/tools/registry.hpp"

#include <functional>
#include <string>

namespace qt::tools {

void register_index_tools(ToolRegistry& registry);
void register_index_tools(
    ToolRegistry& registry,
    const std::function<bool(const std::string&)>& enabled);

}  // namespace qt::tools
