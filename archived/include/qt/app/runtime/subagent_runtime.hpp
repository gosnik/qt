#pragma once
#include "qt/app/runtime/runtime.hpp"

namespace qt::app {
void bind_subagent_runtime(
    const std::shared_ptr<ReferenceStartupRuntime> &runtime);
}
