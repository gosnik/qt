#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/model_catalog.hpp"
#include "qt/ai/models.hpp"
#include "qt/app/export/resource_inspection.hpp"
#include "qt/app/runtime/model_resolution.hpp"
#include "qt/app/config/settings.hpp"
#include "qt/extensions/contributions.hpp"
#include "qt/extensions/harness_lifecycle.hpp"
#include "qt/extensions/loader.hpp"
#include "qt/foundation/error.hpp"
#include "qt/harness/context_resources.hpp"
#include "qt/harness/prompt_assembly.hpp"
#include "qt/harness/system_prompt.hpp"
#include "qt/tools/registry.hpp"
#include "qt/tools/tool.hpp"

namespace qt::ai {
class LlamaProvider;
}

namespace qt::index {
class BackgroundService;
}

namespace qt::tools {
class McpClientManager;
}

namespace qt::harness {
class RepositoryView;
}

namespace qt::memory {
class MemoryRuntime;
}

namespace qt::app {

namespace runtime { class SubagentCoordinator; }

struct RuntimeServicesRequest {
  std::optional<std::string> parent_session_id;
  std::string cwd;
  std::optional<std::string> agent_home;
  std::optional<std::string> workspace_root;
  std::optional<AppSettingsPatch> override_settings;
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::optional<ai::ModelCatalog> model_catalog;
  std::vector<std::string> additional_extension_paths;
  std::vector<std::string> additional_skill_paths;
  std::vector<std::string> additional_prompt_template_paths;
  std::vector<std::string> additional_theme_paths;
  std::vector<std::string> local_model_discovery_roots;
  std::optional<std::string> system_prompt;
  std::vector<std::string> append_system_prompt;
  std::vector<std::string> enabled_tools;
  bool include_builtin_tools = true;
  bool include_extension_tools = true;
  bool include_extensions = true;
  bool include_skills = true;
  bool include_prompt_templates = true;
  bool include_themes = true;
  bool include_context_files = true;
  bool include_memory = true;
};

struct RuntimeResourceBundle {
  harness::RepoContextResources context_resources;
  harness::RepoPromptContributionResources prompt_contributions;
  harness::PromptAssembly prompt_assembly;
  harness::PromptTemplateResources prompt_templates;
  harness::SkillResources skills;
  extensions::ExtensionLoadResult extensions;
  extensions::ExtensionHostRuntime extension_host;
  std::vector<extensions::ExtensionHarnessLifecycleRoutingPlan>
      instruction_loaded_routing_plans;
  harness::SystemPromptInput system_prompt_input;
  harness::SystemPrompt system_prompt;
  std::vector<std::string> diagnostics;
};

struct RuntimeServices {
  std::shared_ptr<runtime::SubagentCoordinator> subagent_coordinator;
  AppSettings settings;
  ai::AuthStorage auth_storage;
  std::string auth_storage_path;
  ai::ModelCatalog model_catalog;
  ModelResolutionRequest model_request;
  ResolvedModelSelection model_selection;
  std::shared_ptr<ai::LlamaProvider> llama_provider;
  std::optional<std::string> selected_llama_model_id;
  std::shared_ptr<qt::index::BackgroundService> index_service;
  std::shared_ptr<tools::McpClientManager> mcp_client_manager;
  std::shared_ptr<tools::ToolRegistry> tool_registry;
  std::shared_ptr<qt::memory::MemoryRuntime> memory_runtime;
  std::vector<ai::ToolDefinition> tool_definitions;
  tools::ToolContext tool_context;
  RuntimeResourceBundle resource_bundle;
  std::vector<std::string> diagnostics;
};

struct MemoryTaskModelPlan {
  std::optional<std::string> document_embedding;
  std::optional<std::string> short_embedding;
  std::optional<std::string> worthiness;
  std::optional<std::string> extraction;
  std::optional<std::string> recall_prehandler;
  std::optional<std::string> context_compilation;
  std::int64_t context_compilation_timeout_floor_ms = 600000;
  std::size_t context_compilation_input_token_limit = 6000U;
  std::vector<std::string> discovery_roots;
};

MemoryTaskModelPlan resolve_memory_task_models(const AppSettings& settings);

tools::ToolContext make_runtime_tool_context(const std::string& cwd,
                                             const std::string& workspace_root,
                                             bool allow_external_filesystem_access = false);
std::shared_ptr<tools::ToolRegistry> make_builtin_tool_registry();
std::shared_ptr<tools::ToolRegistry> make_builtin_tool_registry(
    const std::vector<std::string>& enabled_tools);
std::vector<ai::ToolDefinition> build_tool_definitions(
    const tools::ToolRegistry& registry);

foundation::Result<RuntimeResourceBundle> load_runtime_resource_bundle(
    const harness::RepositoryView& repository, const AppSettings& settings);

foundation::Result<RuntimeServices> load_runtime_services(
    const harness::RepositoryView& repository,
    const RuntimeServicesRequest& request);

}  // namespace qt::app
