#pragma once

#include <memory>

#include "qt/app/runtime/runtime.hpp"

namespace qt::app {

// Binds one weak runtime callback identity to every loaded extension record.
// The callback is intentionally weak so extension records cannot keep a
// startup runtime alive after mode teardown.
void bind_extension_host_call(
    const std::shared_ptr<ReferenceStartupRuntime>& runtime,
    ReferenceCliArgs parsed);

}  // namespace qt::app
