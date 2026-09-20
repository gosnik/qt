#pragma once

#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/process.hpp"
#include "qt/tools/models.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace qt::index {
class BackgroundService;
}

namespace qt::tools {

enum class ToolMutationPolicy {
    execute,
    queue,
};

struct ToolContext {
    std::optional<qt::foundation::Path> workspace_root;
    std::optional<qt::foundation::Path> current_directory;
    bool allow_external_filesystem_access = false;
    Json metadata = Json::object();
    ToolMutationPolicy mutation_policy = ToolMutationPolicy::execute;
    std::shared_ptr<ToolMutationQueue> mutation_queue;
    std::shared_ptr<qt::index::BackgroundService> index_service;
    std::shared_ptr<qt::foundation::ProcessCancellationToken> cancellation_token;
    std::function<void(const ToolResult&)> update_callback;
};

class Tool {
  public:
    virtual ~Tool() = default;

    virtual const ToolDescriptor& descriptor() const = 0;
    virtual ToolResult invoke(const ToolRequest& request,
                              const ToolContext& context) const = 0;
};

}  // namespace qt::tools
