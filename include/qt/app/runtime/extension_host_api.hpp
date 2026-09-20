#pragma once

#include "qt/app/runtime/runtime.hpp"
#include <string_view>

namespace qt::app {

// Invoke on the runtime's owning thread. UI and turn-start actions are supplied
// by the mode-specific presenter, which also persists successful session writes.
foundation::Result<ai::Json> dispatch_extension_host_call(
    ReferenceStartupRuntime& runtime, const ReferenceCliArgs& parsed,
    std::string_view method, const ai::Json& arguments);

}  // namespace qt::app
