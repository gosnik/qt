#pragma once

#include "qt/app/runtime/isolated_agent_runner.hpp"
#include "qt/app/runtime/subagent_coordinator.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <memory>

namespace qt::tools {

enum class SubagentSource {
    builtin,
    user,
    project,
};

const char* to_string(SubagentSource source) noexcept;

struct SubagentDefinition {
    std::string name;
    std::string description;
    std::vector<std::string> tools;
    std::optional<std::string> model;
    std::optional<std::string> thinking;
    std::string system_prompt;
    std::string system_prompt_mode = "replace";
    bool inherit_project_context = false;
    bool inherit_skills = false;
    std::optional<std::size_t> max_subagent_depth;
    bool disabled = false;
    SubagentSource source = SubagentSource::user;
    qt::foundation::Path file_path;
};

struct SubagentDiscoveryOptions {
    qt::foundation::Path agent_home;
    qt::foundation::Path cwd;
    std::string scope = "user";
};

struct SubagentDiscoveryResult {
    std::vector<SubagentDefinition> agents;
    std::vector<std::string> diagnostics;
    std::optional<qt::foundation::Path> project_agents_directory;
};

SubagentDiscoveryResult discover_subagents(
    const SubagentDiscoveryOptions& options);

struct SubagentToolOptions {
    qt::foundation::Path agent_home;
    qt::foundation::Path cwd;
    std::string executable = "qt";
    std::vector<std::string> executable_arguments;
    std::size_t max_parallel_tasks = 8U;
    std::size_t default_concurrency = 4U;
    std::size_t max_subagent_depth = 2U;
    std::shared_ptr<qt::app::runtime::SubagentCoordinator> coordinator;
};

class SubagentTool final : public Tool {
  public:
    explicit SubagentTool(SubagentToolOptions options);

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    SubagentToolOptions options_;
    qt::app::runtime::IsolatedAgentRunner runner_;
    ToolDescriptor descriptor_;
};

}  // namespace qt::tools
