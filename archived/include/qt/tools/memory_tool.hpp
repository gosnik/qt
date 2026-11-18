#pragma once

#include "qt/tools/registry.hpp"

#include <functional>
#include <memory>
#include <string>

namespace qt::memory {
class MemoryRuntime;
}

namespace qt::tools {

void register_memory_tools(
    ToolRegistry& registry,
    std::shared_ptr<qt::memory::MemoryRuntime> runtime,
    const std::function<bool(const std::string&)>& enabled =
        [](const std::string&) { return true; });

}  // namespace qt::tools
