#pragma once

#include "qt/app/config/settings.hpp"
#include "qt/memory/runtime.hpp"

#include <memory>
#include <string>
#include <vector>

namespace qt::memory {
class MemoryRuntime;
struct MemoryScope;
}  // namespace qt::memory

namespace qt::app::runtime {

struct MemoryPreviewServices {
  std::unique_ptr<qt::memory::MemoryRuntime> runtime;
  std::vector<std::string> warnings;
};

MemoryPreviewServices make_memory_preview_services(
    const AppSettings&, const qt::memory::MemoryScope&);

}  // namespace qt::app::runtime
