#pragma once

#include "qt/foundation/error.hpp"
#include "qt/tools/tool.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace qt::tools {

struct OutputGuardOptions {
    std::size_t max_string_bytes = 64U * 1024U;
    std::size_t max_array_items = 1000U;
};

ToolResult make_invalid_request(std::string message, const ToolRequest& request);
ToolResult make_execution_error(ToolResultStatus status, std::string message,
                                const ToolRequest& request);
std::optional<ToolResult> require_mutation_approval(
    ToolMutationKind kind, std::string summary, const ToolRequest& request,
    const ToolContext& context);
OutputGuardOptions output_guard_options_from_context(const ToolContext& context);
void apply_output_guards(ToolResult& result, OutputGuardOptions options);

qt::foundation::Result<std::string> require_string_argument(const ToolRequest& request,
                                                            std::string_view name,
                                                            std::string_view tool_name);

qt::foundation::Result<qt::foundation::Path> resolve_workspace_path(
    const ToolRequest& request, const ToolContext& context, std::string_view tool_name);

bool is_within_workspace_root(const qt::foundation::Path& path,
                              const qt::foundation::Path& root);
bool is_workspace_path_allowed(const qt::foundation::Path& path,
                               const qt::foundation::Path& root,
                               const ToolContext& context);

}  // namespace qt::tools
