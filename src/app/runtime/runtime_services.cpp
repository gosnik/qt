#include "qt/app/runtime/runtime_services.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>

#include "qt/ai/llama_model_discovery.hpp"
#include "qt/ai/llama_provider.hpp"
#include "qt/ai/model_catalog.hpp"
#include "qt/ai/openrouter_model_discovery.hpp"
#include "qt/app/export/resource_inspection.hpp"
#include "qt/app/runtime/extension_contribution_runner.hpp"
#include "qt/extensions/contributions.hpp"
#include "qt/extensions/loader.hpp"
#include "qt/foundation/logging.hpp"
#include "qt/harness/context_resources.hpp"
#include "qt/harness/prompt_assembly.hpp"
#include "qt/harness/system_prompt.hpp"
#include "qt/memory/runtime.hpp"
#include "qt/memory/llama_retrieval.hpp"
#include "qt/tools/append_text_tool.hpp"
#include "qt/tools/apply_patch_tool.hpp"
#include "qt/tools/bash_tool.hpp"
#include "qt/tools/clipboard_tool.hpp"
#include "qt/tools/complete_path_tool.hpp"
#include "qt/tools/edit_text_tool.hpp"
#include "qt/tools/find_files_tool.hpp"
#include "qt/tools/list_directory_tool.hpp"
#include "qt/tools/mcp_client.hpp"
#include "qt/tools/mcp_resource_tool.hpp"
#include "qt/tools/memory_tool.hpp"
#include "qt/tools/package_manager_tool.hpp"
#include "qt/tools/index_tool.hpp"
#include "qt/tools/read_text_tool.hpp"
#include "qt/tools/search_text_tool.hpp"
#include "qt/tools/subagent_tool.hpp"
#include "qt/app/runtime/subagent_coordinator.hpp"
#include "qt/foundation/uuid.hpp"
#include "qt/foundation/hash.hpp"
#include "qt/tools/web_tool.hpp"
#include "qt/tools/write_text_tool.hpp"
#include "index/service.hpp"

namespace qt::app {

namespace {

std::string current_date_string() {
  std::time_t now = std::time(nullptr);
  const std::tm* utc = std::gmtime(&now);
  if (utc == nullptr) {
    return "1970-01-01";
  }

  std::ostringstream out;
  out << std::put_time(utc, "%Y-%m-%d");
  return out.str();
}

std::string current_timezone_string() {
  std::time_t now = std::time(nullptr);
  const std::tm* local = std::localtime(&now);
  if (local == nullptr) {
    return {};
  }

  std::ostringstream out;
  out << std::put_time(local, "%Z %z");
  return out.str();
}

std::string configured_shell(const AppSettings& settings) {
  if (settings.shell_path && !settings.shell_path->empty()) {
    return *settings.shell_path;
  }
  if (const char* shell = std::getenv("SHELL")) {
    if (*shell != '\0') {
      return std::string(shell);
    }
  }
  return {};
}

std::optional<std::string> models_json_path(const AppSettings& settings) {
  if (settings.agent_home.empty()) {
    return std::nullopt;
  }
  return (std::filesystem::path(settings.agent_home) / "models.json")
      .generic_string();
}

std::string subagent_executable() {
  if (const char* configured = std::getenv("QT_SUBAGENT_EXECUTABLE");
      configured != nullptr && *configured != '\0') {
    return configured;
  }
  std::error_code error;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error && !executable.empty()) {
    return executable.generic_string();
  }
  return "qt";
}

foundation::Result<std::optional<ai::ModelCatalog>> load_models_json_catalog(
    const AppSettings& settings, const ai::ModelCatalog& base_catalog) {
  using foundation::ErrorCode;
  using foundation::Result;

  const auto path = models_json_path(settings);
  if (!path) {
    return std::optional<ai::ModelCatalog>{};
  }

  std::ifstream input(*path);
  if (!input) {
    return std::optional<ai::ModelCatalog>{};
  }

  try {
    const ai::Json config = ai::Json::parse(input);
    const auto catalog =
        ai::apply_models_config_to_model_catalog(base_catalog, config);
    if (!catalog) {
      return Result<std::optional<ai::ModelCatalog>>::failure(
          catalog.error().code,
          catalog.error().message + "\n\nFile: " + *path,
          catalog.error().system_error);
    }
    return std::optional<ai::ModelCatalog>{catalog.value()};
  } catch (const ai::Json::parse_error& error) {
    return Result<std::optional<ai::ModelCatalog>>::failure(
        ErrorCode::invalid_argument,
        std::string("Failed to parse models.json: ") + error.what() +
            "\n\nFile: " + *path);
  }
}

bool env_flag_enabled(const char* name) {
  const char* raw = std::getenv(name);
  if (raw == nullptr) {
    return false;
  }
  std::string value(raw);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::string network_access_summary() {
  return env_flag_enabled("PI_OFFLINE") ? "disabled by PI_OFFLINE"
                                        : "enabled";
}

std::string mutation_policy_summary(
    qt::tools::ToolMutationPolicy mutation_policy) {
  switch (mutation_policy) {
    case qt::tools::ToolMutationPolicy::execute:
      return "tool mutations execute immediately";
    case qt::tools::ToolMutationPolicy::queue:
      return "tool mutations require queued approval";
  }
  return "unknown";
}

}  // namespace

foundation::Result<RuntimeResourceBundle> load_runtime_resource_bundle_impl(
    const harness::RepositoryView& repository, const AppSettings& settings,
    const RuntimeServicesRequest& request);

namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

bool wildcard_match(std::string_view pattern, std::string_view value) {
  std::size_t pattern_index = 0U;
  std::size_t value_index = 0U;
  std::size_t star_index = std::string_view::npos;
  std::size_t match_index = 0U;

  while (value_index < value.size()) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' ||
         pattern[pattern_index] == value[value_index])) {
      ++pattern_index;
      ++value_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_index = pattern_index++;
      match_index = value_index;
      continue;
    }
    if (star_index != std::string_view::npos) {
      pattern_index = star_index + 1U;
      value_index = ++match_index;
      continue;
    }
    return false;
  }

  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::vector<tools::McpServerConfig> mcp_server_configs_from_settings(
    const AppSettings& settings) {
  std::vector<tools::McpServerConfig> configs;
  configs.reserve(settings.mcp_servers.size());
  const std::string stderr_log_path =
      settings.agent_home.empty()
          ? std::string{}
          : (std::filesystem::path(settings.agent_home) / "logs" /
             "mcp-stderr.log")
                .generic_string();
  for (const auto& server : settings.mcp_servers) {
    tools::McpServerConfig config;
    config.name = server.name;
    config.enabled = server.enabled;
    config.transport = server.transport;
    config.command = server.command;
    config.args = server.args;
    config.env = server.env;
    config.url = server.url;
    config.headers = server.headers;
    config.sampling = server.sampling;
    config.timeout_seconds = server.timeout_seconds;
    config.connect_timeout_seconds = server.connect_timeout_seconds;
    config.stderr_log_path = stderr_log_path;
    configs.push_back(std::move(config));
  }
  return configs;
}

struct RuntimeModelServicesState {
  ai::AuthStorage auth_storage;
  std::string auth_storage_path;
  ai::ModelCatalog model_catalog;
  ModelResolutionRequest model_request;
  ResolvedModelSelection model_selection;
  std::vector<std::string> diagnostics;
};

struct RuntimeToolServicesState {
  std::shared_ptr<runtime::SubagentCoordinator> subagent_coordinator;
  std::shared_ptr<tools::ToolRegistry> tool_registry;
  std::shared_ptr<tools::McpClientManager> mcp_client_manager;
  std::vector<ai::ToolDefinition> tool_definitions;
  tools::ToolContext tool_context;
};

struct RuntimeModelDiscoveryState {
  AppSettings settings;
  ai::LlamaModelDiscoveryResult registered_discovery;
  std::vector<std::string> diagnostics;
};

class ExtensionContributionTool final : public tools::Tool {
 public:
  explicit ExtensionContributionTool(
      extensions::ExtensionContributionDescriptor contribution)
      : contribution_(std::move(contribution)) {
    descriptor_.name = contribution_.name;
    descriptor_.description =
        contribution_.description.value_or(
            contribution_.title.value_or("Extension tool " + contribution_.name));
    descriptor_.input_schema = tools::Json{
        {"type", "object"},
        {"additionalProperties", true},
    };
    descriptor_.deterministic = false;
    descriptor_.destructive = false;
    descriptor_.local_only = false;
  }

  const tools::ToolDescriptor& descriptor() const override {
    return descriptor_;
  }

  tools::ToolResult invoke(const tools::ToolRequest& request,
                           const tools::ToolContext& context) const override {
    auto contribution_result = run_extension_contribution_subprocess(
        contribution_, request.arguments,
        ExtensionContributionRunOptions{30'000, context.cancellation_token, {}});
    if (!contribution_result) {
      const bool not_implemented =
          contribution_result.error().message.rfind("not_implemented:", 0) == 0;
      return make_error_result(request,
                               not_implemented ? "not_implemented"
                                               : "execution_error",
                               contribution_result.error().message);
    }

    return make_tool_result(request, contribution_result.value());
  }

 private:
  tools::ToolResult make_error_result(const tools::ToolRequest& request,
                                      std::string code,
                                      std::string message) const {
    tools::ToolResult result;
    result.status = tools::ToolResultStatus::execution_error;
    result.call_id = request.call_id;
    result.error_message = std::move(message);
    result.output = tools::Json{
        {"code", std::move(code)},
        {"tool", contribution_.name},
        {"extension", contribution_.extension.path},
        {"message", *result.error_message},
    };
    return result;
  }

  tools::ToolResult make_tool_result(
      const tools::ToolRequest& request,
      const extensions::ExtensionContributionResult& contribution_result) const {
    tools::ToolResult result;
    result.call_id = request.call_id;
    if (contribution_result.ok) {
      result.status = tools::ToolResultStatus::success;
      result.output = contribution_result.value.is_object()
                          ? contribution_result.value
                          : tools::Json{{"result", contribution_result.value}};
      return result;
    }

    result.status = tools::ToolResultStatus::execution_error;
    if (contribution_result.error.has_value()) {
      result.error_message = contribution_result.error->message;
      result.output = tools::Json{
          {"code", "extension_error"},
          {"jsonrpcCode", contribution_result.error->code},
          {"tool", contribution_.name},
          {"extension", contribution_.extension.path},
          {"message", contribution_result.error->message},
          {"data", contribution_result.error->data},
      };
    } else {
      result.error_message = "extension tool returned an error";
      result.output = tools::Json{
          {"code", "extension_error"},
          {"tool", contribution_.name},
          {"extension", contribution_.extension.path},
          {"message", *result.error_message},
      };
    }
    return result;
  }

  extensions::ExtensionContributionDescriptor contribution_;
  tools::ToolDescriptor descriptor_;
};

Result<ai::AuthStorage> load_runtime_auth_storage(const AppSettings& settings) {
  const std::string path = default_auth_storage_path(settings);
  if (path.empty()) {
    return ai::AuthStorage{};
  }
  return ai::load_auth_storage(path);
}

void append_unique_path(std::vector<std::string>& paths, std::string path) {
  if (path.empty()) {
    return;
  }
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    paths.push_back(std::move(path));
  }
}

void append_unique_paths(std::vector<std::string>& paths,
                         const std::vector<std::string>& additions) {
  for (const auto& path : additions) {
    append_unique_path(paths, path);
  }
}

void prepend_unique_paths(std::vector<std::string>& paths,
                          const std::vector<std::string>& additions) {
  std::vector<std::string> reordered;
  for (const auto& path : additions) {
    append_unique_path(reordered, path);
  }
  append_unique_paths(reordered, paths);
  paths = std::move(reordered);
}

std::string join_lines(const std::vector<std::string>& lines) {
  std::string joined;
  for (const auto& line : lines) {
    if (line.empty()) {
      continue;
    }
    if (!joined.empty()) {
      joined.push_back('\n');
    }
    joined += line;
  }
  return joined;
}

AppSettings apply_runtime_resource_overrides(
    AppSettings settings, const RuntimeServicesRequest& request) {
  if (!request.include_prompt_templates) {
    settings.features.load_prompt_templates = false;
    settings.prompt_roots.clear();
  } else {
    append_unique_paths(settings.prompt_roots,
                        request.additional_prompt_template_paths);
  }

  if (!request.include_skills) {
    settings.skill_roots.clear();
    append_unique_paths(settings.skill_roots, request.additional_skill_paths);
    settings.features.load_skills = !settings.skill_roots.empty();
  } else {
    prepend_unique_paths(settings.skill_roots, request.additional_skill_paths);
  }

  if (!request.include_themes) {
    settings.features.load_themes = false;
    settings.theme_roots.clear();
  } else {
    append_unique_paths(settings.theme_roots, request.additional_theme_paths);
  }

  return settings;
}

std::vector<std::string> discover_runtime_extension_roots(
    const harness::RepositoryView& repository, const AppSettings& settings,
    const RuntimeServicesRequest& request) {
  std::vector<std::string> roots;
  append_unique_paths(roots, request.additional_extension_paths);

  if (!request.include_extensions) {
    return roots;
  }

  const auto add_if_directory = [&](const std::string& path) {
    if (path.empty() || !repository.directory_exists(path)) {
      return;
    }
    append_unique_path(roots, path);
  };

  add_if_directory((std::filesystem::path(settings.cwd) / ".pi" / "extensions")
                       .generic_string());
  add_if_directory((std::filesystem::path(settings.agent_home) / "extensions")
                       .generic_string());

  return roots;
}

void append_bundle_diagnostic(std::vector<std::string>& diagnostics,
                              std::string message) {
  if (!message.empty()) {
    diagnostics.push_back(std::move(message));
  }
}

bool is_benign_runtime_diagnostic(const std::string& message) {
  return message == "no context resources available for prompt assembly";
}

void append_string_diagnostics(std::vector<std::string>& diagnostics,
                               const std::vector<std::string>& messages) {
  for (const auto& message : messages) {
    if (is_benign_runtime_diagnostic(message)) {
      continue;
    }
    append_bundle_diagnostic(diagnostics, message);
  }
}

void append_prompt_template_diagnostics(
    std::vector<std::string>& diagnostics,
    const harness::PromptTemplateResources& resources) {
  for (const auto& diagnostic : resources.diagnostics) {
    append_bundle_diagnostic(diagnostics, diagnostic.message);
  }
}

void apply_runtime_prompt_overrides(RuntimeResourceBundle& bundle,
                                    const RuntimeServicesRequest& request) {
  if (request.system_prompt && !request.system_prompt->empty()) {
    bundle.prompt_contributions.system_prompt =
        harness::LoadedPromptContributionRecord{"<cli:system-prompt>",
                                                *request.system_prompt};
  }

  const std::string cli_append_prompt =
      join_lines(request.append_system_prompt);
  if (cli_append_prompt.empty()) {
    return;
  }
  if (bundle.prompt_contributions.append_system_prompt &&
      !bundle.prompt_contributions.append_system_prompt->content.empty()) {
    bundle.prompt_contributions.append_system_prompt->content += "\n";
    bundle.prompt_contributions.append_system_prompt->content += cli_append_prompt;
  } else {
    bundle.prompt_contributions.append_system_prompt =
        harness::LoadedPromptContributionRecord{
            "<cli:append-system-prompt>", cli_append_prompt};
  }
}

RuntimeModelDiscoveryState apply_runtime_model_discovery(
    AppSettings settings, const RuntimeServicesRequest& request) {
  RuntimeModelDiscoveryState state;
  state.settings = std::move(settings);
  if (request.local_model_discovery_roots.empty()) {
    return state;
  }

  const auto discovered = ai::discover_llama_models(
      ai::LlamaModelDiscoveryOptions{request.local_model_discovery_roots, true});
  append_string_diagnostics(state.diagnostics, discovered.diagnostics);
  const auto persisted = persist_discovered_llama_models(
      state.settings.agent_home, state.settings.cwd, discovered);
  if (persisted) {
    append_string_diagnostics(state.diagnostics, persisted.value().diagnostics);
  } else {
    state.diagnostics.push_back("local model discovery could not update settings: " +
                                persisted.error().message);
  }

  std::unordered_set<std::string> existing_ids;
  for (const auto& model : state.settings.llama.models) {
    existing_ids.insert(lowercase_copy(model.id));
  }

  for (const auto& model : discovered.models) {
    const std::string normalized_id = lowercase_copy(model.id);
    if (existing_ids.find(normalized_id) != existing_ids.end()) {
      state.diagnostics.push_back(
          "local model discovery skipped duplicate llama model id: " + model.id);
      continue;
    }
    existing_ids.insert(normalized_id);
    state.settings.llama.models.push_back(
        ai::llama_model_settings_from_discovery(model));
    state.registered_discovery.models.push_back(model);
  }
  if (!state.registered_discovery.models.empty()) {
    state.settings.llama.models_configured = true;
  }
  return state;
}

Result<RuntimeModelServicesState> load_runtime_model_services(
    const RuntimeServicesRequest& request, const AppSettings& settings,
    const ai::LlamaModelDiscoveryResult& registered_discovery,
    const std::vector<extensions::ExtensionProviderContributionDescriptor>&
        extension_providers) {
  const auto auth_storage = load_runtime_auth_storage(settings);
  if (!auth_storage) {
    return Result<RuntimeModelServicesState>::failure(
        auth_storage.error().code, auth_storage.error().message,
        auth_storage.error().system_error);
  }

  RuntimeModelServicesState state;
  state.auth_storage = auth_storage.value();
  state.auth_storage_path = default_auth_storage_path(settings);
  state.model_catalog =
      request.model_catalog.value_or(ai::default_model_catalog());
  const auto custom_catalog =
      load_models_json_catalog(settings, state.model_catalog);
  if (!custom_catalog) {
    state.diagnostics.push_back(custom_catalog.error().message);
  } else if (custom_catalog.value().has_value()) {
    state.model_catalog = std::move(*custom_catalog.value());
  }
  auto llama_catalog = ai::model_catalog_from_llama_settings(settings.llama);
  ai::enrich_model_catalog_with_llama_discovery(llama_catalog,
                                                registered_discovery);
  state.model_catalog = ai::merge_catalogs(state.model_catalog, llama_catalog);
  state.model_catalog = ai::merge_catalogs(
      state.model_catalog,
      ai::model_catalog_from_ollama_settings(settings.ollama_instances));
  // Runtime startup must not wait on OpenRouter's network catalog. Explicit
  // model-list refreshes update this cache; provider-scoped saved IDs remain
  // resolvable even when the cache is absent or stale.
  state.model_catalog = ai::merge_catalogs(
      state.model_catalog, ai::cached_openrouter_models());
  for (const auto& provider : extension_providers) {
    auto applied = ai::apply_provider_config_to_model_catalog(
        state.model_catalog, provider.name, provider.config);
    if (!applied) {
      state.diagnostics.push_back("extension provider '" + provider.name +
                                  "' from " + provider.extension.path +
                                  " was ignored: " +
                                  applied.error().message);
      continue;
    }
    state.model_catalog = std::move(applied.value());
  }
  state.model_request.settings = settings;
  state.model_request.catalog = state.model_catalog;
  state.model_request.auth_storage = state.auth_storage;
  state.model_request.provider = request.provider;
  state.model_request.model = request.model;

  auto selection = resolve_model_selection(state.model_request);
  if (!selection) {
    return Result<RuntimeModelServicesState>::failure(
        selection.error().code, selection.error().message,
        selection.error().system_error);
  }

  state.model_selection = selection.value();
  return state;
}

Result<RuntimeToolServicesState> load_runtime_tool_services(
    const AppSettings& settings, bool include_builtin_tools,
    bool include_extension_tools,
    const std::vector<std::string>& enabled_tools,
    const std::vector<extensions::ExtensionContributionDescriptor>&
        extension_tools, const std::optional<std::string>& parent_session_id) {
  RuntimeToolServicesState state;
  state.tool_registry = include_builtin_tools
                            ? make_builtin_tool_registry(enabled_tools)
                            : std::make_shared<tools::ToolRegistry>();
  const bool filtered = !enabled_tools.empty();
  const auto matches_filter = [&](const std::string& name) {
    if (std::find(enabled_tools.begin(), enabled_tools.end(), name) !=
        enabled_tools.end()) {
      return true;
    }
    for (const std::string& pattern : enabled_tools) {
      if (pattern.find_first_of("*?") != std::string::npos &&
          wildcard_match(pattern, name)) {
        return true;
      }
    }
    return false;
  };
  if (include_builtin_tools && (!filtered || matches_filter("subagent"))) {
    tools::SubagentToolOptions subagent_options;
    subagent_options.agent_home = settings.agent_home;
    subagent_options.cwd = settings.cwd;
    subagent_options.executable = subagent_executable();
    runtime::SubagentCoordinatorOptions coordinator_options;
    coordinator_options.parent_session_id = parent_session_id.value_or(
        qt::foundation::generate_uuid_v4_string());
    coordinator_options.directory = qt::foundation::Path(settings.agent_home) /
        "subagents" / qt::foundation::stable_hash_hex(coordinator_options.parent_session_id);
    if (const auto* path = std::getenv("QT_SUBAGENT_PATH")) {
      coordinator_options.parent_path = path;
    }
    coordinator_options.runner.executable = subagent_options.executable;
    try {
      state.subagent_coordinator = std::make_shared<runtime::SubagentCoordinator>(
          std::move(coordinator_options));
    } catch (const std::exception& error) {
      return Result<RuntimeToolServicesState>::failure(
          qt::foundation::ErrorCode::io_error,
          std::string("cannot restore persistent subagents: ") + error.what());
    }
    subagent_options.coordinator = state.subagent_coordinator;
    state.tool_registry->register_tool(std::make_unique<tools::SubagentTool>(
        std::move(subagent_options)));
  }
  if (include_extension_tools) {
    for (const auto& tool : extension_tools) {
      if (filtered && !matches_filter(tool.name)) {
        continue;
      }
      state.tool_registry->register_tool(
          std::make_unique<ExtensionContributionTool>(tool));
    }
  }
  if (!settings.mcp_servers.empty()) {
    state.mcp_client_manager = std::make_shared<tools::McpClientManager>(
        mcp_server_configs_from_settings(settings));
    std::vector<std::string> diagnostics;
    tools::register_mcp_tools(
        *state.tool_registry, state.mcp_client_manager, &diagnostics,
        filtered ? std::function<bool(const std::string&)>(matches_filter)
                 : std::function<bool(const std::string&)>{});
    for (const auto& diagnostic : diagnostics) {
      qt::foundation::logger("runtime", "services")
          .warn("mcp_tool_registration",
                {qt::foundation::log_field("message", diagnostic)});
    }
  }
  state.tool_definitions = build_tool_definitions(*state.tool_registry);
  state.tool_context =
      make_runtime_tool_context(settings.cwd, settings.workspace_root,
                                settings.allow_external_filesystem_access);

  for (const auto& tool : state.tool_definitions) {
    const auto errors = ai::validate(tool);
    if (!errors.empty()) {
      return Result<RuntimeToolServicesState>::failure(
          ErrorCode::internal_error,
          "invalid builtin tool definition for " + tool.name + ": " +
              errors.front());
    }
  }

  return state;
}

}  // namespace

tools::ToolContext make_runtime_tool_context(const std::string& cwd,
                                             const std::string& workspace_root,
                                             bool allow_external_filesystem_access) {
  tools::ToolContext context;
  context.current_directory = std::filesystem::path(cwd);
  context.workspace_root =
      std::filesystem::path(workspace_root.empty() ? cwd : workspace_root);
  context.allow_external_filesystem_access =
      allow_external_filesystem_access;
  return context;
}

MemoryTaskModelPlan resolve_memory_task_models(const AppSettings& settings) {
  MemoryTaskModelPlan plan;
  ai::LlamaSettings candidates = settings.llama;
  const auto select = [&](const ai::LlamaMemoryTask task)
      -> std::optional<std::string> {
    const auto* model = ai::select_llama_model_for_memory_task(candidates, task);
    return model == nullptr
               ? std::nullopt
               : std::optional<std::string>("llama/" + model->id);
  };
  const auto select_all = [&]() {
    plan.document_embedding = select(ai::LlamaMemoryTask::document_embedding);
    plan.short_embedding = select(ai::LlamaMemoryTask::short_embedding);
    plan.worthiness = select(ai::LlamaMemoryTask::worthiness);
    plan.extraction = select(ai::LlamaMemoryTask::extraction);
    plan.recall_prehandler = select(ai::LlamaMemoryTask::recall_prehandler);
    plan.context_compilation =
        select(ai::LlamaMemoryTask::context_compilation);
  };
  select_all();
  const auto all_tasks_resolved = [&]() {
    return plan.document_embedding && plan.short_embedding &&
           plan.worthiness && plan.extraction && plan.recall_prehandler &&
           plan.context_compilation;
  };

  // Configured readable GGUFs are authoritative. Reopening every persisted
  // model and reparsing large vocab metadata on every launch adds seconds of
  // cold-start work without changing task routing.
  if (all_tasks_resolved()) {
    return plan;
  }

  std::vector<std::string> roots;
  for (const auto& root : {
           std::filesystem::path(settings.cwd) / "models",
           std::filesystem::path(settings.agent_home) / "models"}) {
    std::error_code error;
    if (std::filesystem::is_directory(root, error)) {
      const std::string normalized = root.lexically_normal().generic_string();
      if (std::find(roots.begin(), roots.end(), normalized) == roots.end()) {
        roots.push_back(normalized);
      }
    }
  }
  if (!roots.empty()) {
    const auto discovered = ai::discover_llama_models(
        ai::LlamaModelDiscoveryOptions{roots, true});
    std::unordered_set<std::string> paths;
    for (const auto& model : candidates.models) {
      paths.insert(std::filesystem::path(model.path)
                       .lexically_normal()
                       .generic_string());
    }
    for (const auto& model : discovered.models) {
      if (paths.insert(std::filesystem::path(model.path)
                           .lexically_normal()
                           .generic_string())
              .second) {
        candidates.models.push_back(
            ai::llama_model_settings_from_discovery(model));
      }
    }
  }
  select_all();
  const std::unordered_set<std::string> configured_ids = [&]() {
    std::unordered_set<std::string> ids;
    for (const auto& model : settings.llama.models) {
      ids.insert(lowercase_copy(model.id));
    }
    return ids;
  }();
  for (const auto& selected : {plan.recall_prehandler,
                               plan.context_compilation}) {
    if (!selected) continue;
    const std::string id = selected->substr(selected->find('/') + 1U);
    if (configured_ids.find(lowercase_copy(id)) != configured_ids.end()) {
      continue;
    }
    const auto model = std::find_if(
        candidates.models.begin(), candidates.models.end(),
        [&](const auto& item) {
          return lowercase_copy(item.id) == lowercase_copy(id);
        });
    if (model != candidates.models.end()) {
      append_unique_path(plan.discovery_roots, model->path);
    }
  }
  return plan;
}

std::shared_ptr<qt::index::BackgroundService> make_runtime_index_service(
    const AppSettings& settings) {
  qt::index::ServiceOptions options;
  options.project_root =
      settings.workspace_root.empty() ? settings.cwd : settings.workspace_root;
  const std::filesystem::path state_root =
      settings.agent_home.empty()
          ? std::filesystem::path(options.project_root) / ".index"
          : std::filesystem::path(settings.agent_home) / "index";
  const auto project_key =
      std::to_string(std::hash<std::string>{}(
          std::filesystem::absolute(options.project_root).generic_string()));
  options.state_directory = state_root / project_key;
  options.compile_commands_output =
      std::filesystem::path(options.project_root) / "compile_commands.json";
  options.tree_sitter.output_directory =
      options.state_directory / "tree-sitter";
  options.low_priority_background_scans = true;
  // Persisted repositories can contain large symbol/call-graph snapshots.
  // Restore them on the service worker so constructing the interactive runtime
  // does not deserialize the complete index before drawing the first frame.
  options.defer_snapshot_load = true;
  options.retrieval_enabled = settings.memory.enabled && settings.memory.retrieval.code_enabled;
  return std::make_shared<qt::index::BackgroundService>(std::move(options));
}

std::shared_ptr<tools::ToolRegistry> make_builtin_tool_registry() {
  return make_builtin_tool_registry({});
}

std::shared_ptr<tools::ToolRegistry> make_builtin_tool_registry(
    const std::vector<std::string>& enabled_tools) {
  const std::unordered_set<std::string> filter(enabled_tools.begin(),
                                               enabled_tools.end());
  const bool filtered = !filter.empty();
  const auto matches_filter = [&](const std::string& name) {
    if (filter.find(name) != filter.end()) {
      return true;
    }
    for (const std::string& pattern : enabled_tools) {
      if (pattern.find_first_of("*?") != std::string::npos &&
          wildcard_match(pattern, name)) {
        return true;
      }
    }
    return false;
  };
  const auto enabled = [&](const std::string& native_name,
                           std::initializer_list<const char*> aliases = {}) {
    if (!filtered || matches_filter(native_name)) {
      return true;
    }
    for (const char* alias : aliases) {
      if (matches_filter(alias)) {
        return true;
      }
    }
    return false;
  };

  auto registry = std::make_shared<tools::ToolRegistry>();
  const bool dream_research = env_flag_enabled("QT_MEMORY_DREAM_RESEARCH");
  const auto research_web_budget =
      dream_research
          ? std::make_shared<tools::WebInvocationBudget>(3U)
          : std::shared_ptr<tools::WebInvocationBudget>{};
  if (enabled("append_text")) {
    registry->register_tool(std::make_unique<tools::AppendTextTool>());
  }
  if (enabled("apply_patch")) {
    registry->register_tool(std::make_unique<tools::ApplyPatchTool>());
  }
  if (enabled("bash")) {
    registry->register_tool(std::make_unique<tools::BashTool>());
  }
  if (enabled("clipboard")) {
    registry->register_tool(std::make_unique<tools::ClipboardTool>());
  }
  if (enabled("complete_path")) {
    registry->register_tool(std::make_unique<tools::CompletePathTool>());
  }
  if (enabled("edit_text", {"edit"})) {
    registry->register_tool(std::make_unique<tools::EditTextTool>());
  }
  if (enabled("find")) {
    registry->register_tool(std::make_unique<tools::FindFilesTool>());
  }
  if (enabled("list_directory", {"ls"})) {
    registry->register_tool(std::make_unique<tools::ListDirectoryTool>());
  }
  if (enabled("list_mcp_resources")) {
    registry->register_tool(std::make_unique<tools::ListMcpResourcesTool>());
  }
  if (enabled("package_manager")) {
    registry->register_tool(std::make_unique<tools::PackageManagerTool>());
  }
  if (enabled("index") || enabled("symbol") || enabled("analyze_file") ||
      enabled("references") || enabled("build_context") ||
      enabled("search_index")) {
    tools::register_index_tools(*registry, [&](const std::string& name) {
      return enabled(name);
    });
  }
  if (enabled("read_text", {"read"})) {
    registry->register_tool(std::make_unique<tools::ReadTextTool>());
  }
  if (enabled("read_mcp_resource")) {
    registry->register_tool(std::make_unique<tools::ReadMcpResourceTool>());
  }
  if (enabled("search_text", {"grep"})) {
    registry->register_tool(std::make_unique<tools::SearchTextTool>());
  }
  if (enabled("web_fetch")) {
    registry->register_tool(std::make_unique<tools::WebFetchTool>(
        research_web_budget, dream_research));
  }
  if (enabled("web_search")) {
    registry->register_tool(std::make_unique<tools::WebSearchTool>(
        research_web_budget, dream_research));
  }
  if (enabled("web_run")) {
    tools::WebRunOptions options;
    if (dream_research) {
      options.max_queries = 3U;
      options.max_operations = 6U;
      options.max_search_results = 6U;
      options.max_response_bytes = 512U * 1024U;
      options.max_query_characters = 256U;
      options.max_invocations = 3U;
      options.allow_direct_urls = false;
      options.include_evidence_content = true;
      options.invocation_budget = research_web_budget;
    }
    registry->register_tool(
        std::make_unique<tools::WebRunTool>(std::move(options)));
  }
  if (enabled("write_text", {"write"})) {
    registry->register_tool(std::make_unique<tools::WriteTextTool>());
  }
  return registry;
}

std::vector<ai::ToolDefinition> build_tool_definitions(
    const tools::ToolRegistry& registry) {
  std::vector<ai::ToolDefinition> definitions;
  for (const auto& descriptor : registry.descriptors()) {
    definitions.push_back(ai::ToolDefinition{
        descriptor.name,
        descriptor.description,
        descriptor.input_schema,
    });
  }
  return definitions;
}

foundation::Result<RuntimeResourceBundle> load_runtime_resource_bundle(
    const harness::RepositoryView& repository, const AppSettings& settings) {
  RuntimeServicesRequest request;
  request.cwd = settings.cwd;
  request.agent_home = settings.agent_home;
  request.workspace_root = settings.workspace_root;
  return load_runtime_resource_bundle_impl(repository, settings, request);
}

foundation::Result<RuntimeResourceBundle> load_runtime_resource_bundle_impl(
    const harness::RepositoryView& repository, const AppSettings& settings,
    const RuntimeServicesRequest& request) {
  RuntimeResourceBundle bundle;

  harness::RepoDiscoveryRequest discovery_request;
  discovery_request.cwd = settings.cwd;
  discovery_request.agent_home = settings.agent_home;
  discovery_request.repository_root_hint = settings.workspace_root;
  discovery_request.include_context_files = request.include_context_files;
  bundle.context_resources =
      harness::discover_and_load_repo_context(repository, discovery_request);
  bundle.prompt_contributions = harness::load_repo_prompt_contributions(
      repository, settings.cwd, std::optional<std::string>(settings.agent_home));
  apply_runtime_prompt_overrides(bundle, request);
  bundle.prompt_assembly =
      harness::assemble_prompt_sections(bundle.context_resources);

  const auto inspection = inspect_app_resources(repository, settings);
  bundle.prompt_templates = inspection.prompt_templates;
  bundle.skills = inspection.skills;

  const auto extension_roots =
      discover_runtime_extension_roots(repository, settings, request);
  bundle.extensions =
      extensions::load_extension_manifests(repository, extension_roots);
  const auto extension_host =
      extensions::make_extension_host_runtime(bundle.extensions);
  if (!extension_host) {
    return Result<RuntimeResourceBundle>::failure(
        extension_host.error().code, extension_host.error().message,
        extension_host.error().system_error);
  }
  bundle.extension_host = extension_host.value();
  auto instruction_loaded_plans =
      extensions::make_extension_instructions_loaded_routing_plans(
          bundle.extension_host,
          bundle.context_resources.instruction_load_events, "startup");
  if (!instruction_loaded_plans) {
    append_string_diagnostics(
        bundle.diagnostics,
        {"failed to plan instructions_loaded extension hooks: " +
         instruction_loaded_plans.error().message});
  } else {
    bundle.instruction_loaded_routing_plans =
        std::move(instruction_loaded_plans.value());
  }

  bundle.system_prompt_input =
      harness::make_system_prompt_input(bundle.prompt_assembly);
  bundle.system_prompt_input.working_directory = settings.cwd;
  bundle.system_prompt_input.workspace_root = settings.workspace_root.empty()
                                                  ? settings.cwd
                                                  : settings.workspace_root;
  bundle.system_prompt_input.shell = configured_shell(settings);
  bundle.system_prompt_input.current_date = current_date_string();
  bundle.system_prompt_input.timezone = current_timezone_string();
  bundle.system_prompt_input.network_access = network_access_summary();
  bundle.system_prompt_input.skills = bundle.skills.loaded_skills;
  if (bundle.prompt_contributions.system_prompt.has_value()) {
    bundle.system_prompt_input.custom_prompt_override =
        bundle.prompt_contributions.system_prompt->content;
  }
  if (bundle.prompt_contributions.append_system_prompt.has_value()) {
    bundle.system_prompt_input.append_system_prompt =
        bundle.prompt_contributions.append_system_prompt->content;
  }
  append_string_diagnostics(bundle.system_prompt_input.diagnostics,
                            bundle.prompt_contributions.diagnostics);
  bundle.system_prompt = harness::build_system_prompt(bundle.system_prompt_input);

  append_string_diagnostics(bundle.diagnostics,
                            bundle.context_resources.diagnostics);
  append_string_diagnostics(bundle.diagnostics,
                            bundle.prompt_contributions.diagnostics);
  append_prompt_template_diagnostics(bundle.diagnostics, bundle.prompt_templates);
  append_string_diagnostics(bundle.diagnostics, bundle.skills.diagnostics);
  append_string_diagnostics(bundle.diagnostics, bundle.extensions.diagnostics);
  append_string_diagnostics(bundle.diagnostics, bundle.system_prompt.diagnostics);

  return bundle;
}

foundation::Result<RuntimeServices> load_runtime_services(
    const harness::RepositoryView& repository,
    const RuntimeServicesRequest& request) {
  auto log = qt::foundation::logger("runtime", "services");
  log.info("services_load_begin",
           {qt::foundation::log_field("cwd", request.cwd),
            qt::foundation::log_field("agent_home",
                                      request.agent_home.value_or("")),
            qt::foundation::log_field("builtin_tools",
                                      request.include_builtin_tools),
            qt::foundation::log_field("extension_tools",
                                      request.include_extension_tools),
            qt::foundation::log_field("extensions",
                                      request.include_extensions)});
  const auto load_started = std::chrono::steady_clock::now();
  const auto log_phase = [&](std::string_view phase) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - load_started);
    log.debug("services_load_phase",
              {qt::foundation::log_field("phase", phase),
               qt::foundation::log_field(
                   "elapsed_ms", static_cast<std::int64_t>(elapsed.count()))});
  };
  LoadSettingsRequest settings_request;
  settings_request.cwd = request.cwd;
  settings_request.agent_home = request.agent_home;
  settings_request.workspace_root = request.workspace_root;
  settings_request.override_settings = request.override_settings;

  const auto settings = load_app_settings(repository, settings_request);
  if (!settings) {
    log.error("settings_load_failed",
              {qt::foundation::log_field("error", settings.error().message)});
    return settings.error();
  }
  auto effective_settings =
      apply_runtime_resource_overrides(settings.value(), request);
  if (env_flag_enabled("QT_SUBAGENT_CHILD")) {
    const auto* external = std::getenv("QT_SUBAGENT_ALLOW_EXTERNAL");
    if (external != nullptr && std::string_view(external) == "0") {
      effective_settings.allow_external_filesystem_access = false;
    }
  }
  log_phase("settings");
  const bool memory_prehandler = env_flag_enabled("QT_MEMORY_PREHANDLER");
  const bool memory_dreaming = env_flag_enabled("QT_MEMORY_DREAMING");
  const bool memory_dream_research =
      env_flag_enabled("QT_MEMORY_DREAM_RESEARCH");
  const bool memory_dream_verify = env_flag_enabled("QT_MEMORY_DREAM_VERIFY");
  if (memory_prehandler || memory_dreaming || memory_dream_research || memory_dream_verify) {
    // Isolated recall, consolidation, and Dreaming workers use bounded
    // protocols and do not need the full advertised model context. A smaller
    // dedicated context substantially reduces local GGUF load latency and KV
    // memory without changing the main chat provider's configuration.
    effective_settings.llama.context.context_size = 8192;
    const unsigned int hardware_threads = std::thread::hardware_concurrency();
    const int memory_threads = static_cast<int>(
        std::min<unsigned int>(16U, hardware_threads == 0U ? 8U
                                                           : hardware_threads));
    effective_settings.llama.context.threads = memory_threads;
    effective_settings.llama.context.batch_threads = memory_threads;
    effective_settings.llama.generation.temperature =
        memory_dreaming || memory_dream_research ? 0.2 : 0.0;
    effective_settings.llama.generation.seed = 0U;
    effective_settings.llama.generation.max_output_tokens =
        memory_dream_verify ? 1536 : memory_dreaming || memory_dream_research ? 1024 : 256;
    effective_settings.llama.kv_cache.policy = "off";
    if (memory_dream_research || memory_dream_verify) {
      // Tool-call envelopes are model/template specific. The trusted parent
      // validates the final research protocol and cited successful web results.
      effective_settings.llama.grammar.default_grammar.reset();
    } else {
      effective_settings.llama.grammar.default_grammar = memory_dreaming
          ? R"(root ::= "{" ws "\"version\"" ws ":" ws "1" "," ws "\"source_memory_ids\"" ws ":" ws source-ids "," ws "\"artifact_kind\"" ws ":" ws artifact-kind "," ws "\"content\"" ws ":" ws nonempty-string "," ws "\"confidence\"" ws ":" ws confidence "," ws "\"research_required\"" ws ":" ws boolean "}" ws
source-ids ::= "[" ws id-string "," ws id-string ("," ws id-string)* "]" ws
artifact-kind ::= "\"association\"" ws | "\"canonicalization_candidate\"" ws | "\"contradiction\"" ws | "\"hypothesis\"" ws | "\"pattern_discovery\"" ws | "\"procedure_revision\"" ws
id-string ::= "\"" json-char{1,256} "\"" ws
nonempty-string ::= "\"" json-char{1,512} "\"" ws
json-char ::= [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4})
confidence ::= ("0" ("." [0-9]+)? | "1" ("." "0"+)?) ws
boolean ::= ("true" | "false") ws
ws ::= [ \t\n\r]{0,8})"
        : R"(root ::= "{" ws "\"version\"" ws ":" ws uint "," ws "\"logical_input_sequence\"" ws ":" ws uint "," ws "\"context_markdown\"" ws ":" ws "\"\"" "," ws "\"memory_ids\"" ws ":" ws strings "," ws "\"block_ids\"" ws ":" ws strings "," ws "\"warnings\"" ws ":" ws "[]" "," ws "\"estimated_tokens\"" ws ":" ws "0" "," ws "\"omission_reason\"" ws ":" ws string "," ws "\"candidate_pack_hash\"" ws ":" ws string "," ws "\"output_hash\"" ws ":" ws "\"\"" "}" ws
strings ::= "[" ws (string ("," ws string)*)? "]" ws
string ::= "\"" chars "\"" ws
chars ::= ([^"\\] | "\\" (["\\/bfnrt] | "u" [0-9a-fA-F]{4}))*
uint ::= ("0" | [1-9] [0-9]*) ws
ws ::= [ \t\n\r]{0,8})";
    }
  }
  if (env_flag_enabled("QT_MEMORY_FORCE_CPU")) {
    effective_settings.llama.backend.gpu_layers = 0;
  }
  auto discovery_state =
      apply_runtime_model_discovery(effective_settings, request);
  log_phase("model_discovery");
  const auto resource_bundle =
      load_runtime_resource_bundle_impl(repository, discovery_state.settings,
                                        request);
  if (!resource_bundle) {
    log.error("resources_load_failed",
              {qt::foundation::log_field("error",
                                         resource_bundle.error().message)});
    return Result<RuntimeServices>::failure(resource_bundle.error().code,
                                            resource_bundle.error().message,
                                            resource_bundle.error().system_error);
  }
  log_phase("resources");
  const auto model_services =
      load_runtime_model_services(request, discovery_state.settings,
                                  discovery_state.registered_discovery,
                                  resource_bundle.value()
                                      .extension_host.registry.providers);
  if (!model_services) {
    log.error("model_services_load_failed",
              {qt::foundation::log_field("error",
                                         model_services.error().message)});
    return Result<RuntimeServices>::failure(
        model_services.error().code, model_services.error().message,
        model_services.error().system_error);
  }
  log_phase("models");
  if (memory_dream_verify &&
      (model_services.value().model_selection.model.ref.provider != ai::kLlamaProviderId ||
       model_services.value().model_selection.provider.descriptor.id != ai::kLlamaProviderId ||
       !request.model || *request.model != model_services.value().model_selection.model.ref.name ||
       request.include_builtin_tools || request.include_extension_tools ||
       request.include_extensions || request.include_skills)) {
    return Result<RuntimeServices>::failure(
        ErrorCode::invalid_argument,
        "Dream verification requires the built-in llama provider with no tools, extensions, or skills");
  }
  const auto tool_services =
      load_runtime_tool_services(discovery_state.settings,
                                 request.include_builtin_tools,
                                 request.include_extension_tools,
                                 request.enabled_tools,
                                 resource_bundle.value()
                                     .extension_host.registry.tools,
                                 request.parent_session_id);
  if (!tool_services) {
    log.error("tool_services_load_failed",
              {qt::foundation::log_field("error",
                                         tool_services.error().message)});
    return Result<RuntimeServices>::failure(
        tool_services.error().code, tool_services.error().message,
        tool_services.error().system_error);
  }
  log_phase("tools");

  RuntimeServices services;
  services.subagent_coordinator = tool_services.value().subagent_coordinator;
  services.settings = discovery_state.settings;
  services.auth_storage = model_services.value().auth_storage;
  services.auth_storage_path = model_services.value().auth_storage_path;
  services.model_catalog = model_services.value().model_catalog;
  services.model_request = model_services.value().model_request;
  services.model_selection = model_services.value().model_selection;
  services.llama_provider = std::dynamic_pointer_cast<ai::LlamaProvider>(
      ai::make_llama_provider(services.settings.llama));
  log_phase("llama_provider");
  services.selected_llama_model_id = services.settings.llama.selected_model_id;
  if (!services.selected_llama_model_id &&
      services.model_selection.model.ref.provider == ai::kLlamaProviderId) {
    services.selected_llama_model_id = services.model_selection.model.ref.name;
  }
  services.index_service = make_runtime_index_service(services.settings);
  services.diagnostics = discovery_state.diagnostics;
  append_string_diagnostics(services.diagnostics,
                            model_services.value().diagnostics);
  services.tool_registry = tool_services.value().tool_registry;
  services.mcp_client_manager = tool_services.value().mcp_client_manager;
  services.tool_definitions = tool_services.value().tool_definitions;
  services.tool_context = tool_services.value().tool_context;
  services.tool_context.index_service = services.index_service;
  ai::Json tool_model_metadata = services.model_selection.model.metadata;
  tool_model_metadata["provider"] = services.model_selection.model.ref.provider;
  tool_model_metadata["api"] = services.model_selection.model.ref.api;
  tool_model_metadata["id"] = services.model_selection.model.ref.name;
  services.tool_context.metadata["model"] = std::move(tool_model_metadata);
  services.resource_bundle = resource_bundle.value();
  services.resource_bundle.system_prompt_input.active_tool_count =
      services.tool_definitions.size();
  const auto has_tool = [&](std::string_view name) {
    return std::any_of(
        services.tool_definitions.begin(), services.tool_definitions.end(),
        [&](const ai::ToolDefinition& tool) { return tool.name == name; });
  };
  if (has_tool("symbol")) {
    services.resource_bundle.system_prompt_input.prompt_guidelines.push_back(
        "For C/C++ class, function, method, definition, or identifier lookup, "
        "call symbol first. Use grep for literal text, regex, or languages not "
        "covered by the C/C++ index.");
  } else if (has_tool("index")) {
    services.resource_bundle.system_prompt_input.prompt_guidelines.push_back(
        "For C/C++ symbol navigation, prefer index with action=call and "
        "operation=code.symbol over grep; put operation at the top level and "
        "the symbol name inside arguments.");
  }
  if (has_tool("analyze_file")) {
    services.resource_bundle.system_prompt_input.prompt_guidelines.push_back(
        "Use analyze_file for a known file's indexed C/C++ structure and call "
        "edges; verify important results against current source.");
  }
  if (has_tool("references")) {
    services.resource_bundle.system_prompt_input.prompt_guidelines.push_back(
        "Use references for approximate indexed C/C++ callers and callees; "
        "verify material edges against current source.");
  }
  if (has_tool("build_context")) {
    services.resource_bundle.system_prompt_input.prompt_guidelines.push_back(
        "Use build_context for a C/C++ file's compiler flags, definitions, "
        "include paths, and resolved include edges.");
  }
  if (has_tool("search_index")) {
    services.resource_bundle.system_prompt_input.prompt_guidelines.push_back(
        "Use search_index for ranked persisted repository text, and grep for "
        "regexes, unsupported languages, or freshly changed text.");
  }
  if (!services.tool_definitions.empty()) {
    services.resource_bundle.system_prompt_input.prompt_guidelines.push_back(
        "Match each tool's advertised JSON schema exactly. If a tool returns "
        "invalid_arguments with retryable=true, correct the listed fields and "
        "retry the call.");
  }
  services.resource_bundle.system_prompt_input.tool_permissions =
      mutation_policy_summary(services.tool_context.mutation_policy) +
      "; filesystem path arguments are " +
      (services.settings.allow_external_filesystem_access
           ? std::string("allowed outside the workspace")
           : std::string("confined to the workspace"));
  services.resource_bundle.system_prompt =
      harness::build_system_prompt(services.resource_bundle.system_prompt_input);
  log_phase("system_prompt");

  if (request.include_memory && services.settings.memory.enabled) {
    try {
      qt::memory::MemoryRuntimeOptions memory_options;
      const auto memory_models = resolve_memory_task_models(services.settings);
      memory_options.agent_home = services.settings.agent_home;
      memory_options.cwd = services.settings.cwd;
      memory_options.coordinator.enabled = true;
      memory_options.coordinator.automatic_recall =
          services.settings.memory.automatic_recall;
      memory_options.coordinator.recall_policy =
          services.settings.memory.recall_policy == "strict"
              ? qt::memory::RecallPolicy::Strict
              : services.settings.memory.recall_policy == "exploratory"
                    ? qt::memory::RecallPolicy::Broad
                    : qt::memory::RecallPolicy::Balanced;
      memory_options.coordinator.context_token_budget =
          services.settings.memory.context_token_budget;
      memory_options.coordinator.candidate_limit =
          services.settings.memory.candidate_limit;
      const auto& retrieval = services.settings.memory.retrieval;
      memory_options.coordinator.graph.enabled = retrieval.graph_enabled;
      memory_options.coordinator.graph.max_depth = retrieval.max_depth;
      memory_options.coordinator.graph.max_seeds = retrieval.max_seeds;
      memory_options.coordinator.graph.max_edges = retrieval.max_edges;
      memory_options.coordinator.graph.max_results = retrieval.max_graph_results;
      memory_options.coordinator.graph.max_fanout = retrieval.max_fanout;
      memory_options.coordinator.dense_enabled = retrieval.dense_enabled;
      memory_options.retrieval.enabled = retrieval.graph_enabled ||
          retrieval.dense_enabled || retrieval.code_enabled;
      memory_options.retrieval.dense_enabled = retrieval.dense_enabled;
      memory_options.retrieval.code_enabled = retrieval.code_enabled;
      memory_options.retrieval.code_graph_enabled = retrieval.code_enabled && retrieval.graph_enabled;
      memory_options.retrieval.graph_max_seeds = retrieval.max_seeds;
      memory_options.retrieval.graph_max_depth = retrieval.max_depth;
      memory_options.retrieval.graph_max_fanout = retrieval.max_fanout;
      memory_options.retrieval.graph_max_edges = retrieval.max_edges;
      memory_options.retrieval.graph_max_results = retrieval.max_graph_results;
      if (retrieval.code_enabled && services.index_service) {
        const auto index = services.index_service;
        memory_options.retrieval.code_snapshot = [index] { return index->retrieval_snapshot(); };
        memory_options.retrieval.code_lexical = [index](const std::string& query, std::size_t limit) {
          std::vector<qt::memory::RetrievalReference> references;
          const auto snapshot = index->retrieval_snapshot();
          const auto hits = index->retrieval_search(query, limit);
          if (!snapshot || hits.stale || hits.generation != snapshot->generation()) return references;
          for (const auto& hit : hits.hits) {
            references.push_back({qt::memory::RetrievalOwnerKind::Code, hit.node_id, 0,
                snapshot->root().generic_string(), snapshot->root().generic_string(),
                hit.path, hit.source_fingerprint, hit.generation});
          }
          return references;
        };
      }
      memory_options.retrieval.cache_entries = retrieval.query_cache_entries;
      memory_options.retrieval.policy = memory_options.coordinator.recall_policy;
      memory_options.retrieval.include_global_user =
          memory_options.coordinator.include_global_user;
      if (retrieval.dense_enabled) {
        const auto diagnostic = retrieval.embedding_model
            ? "dense embeddingModel is not registered in llama.models: " + *retrieval.embedding_model
            : std::string("dense retrieval requires memory.retrieval.embeddingModel");
        memory_options.retrieval.configure = [diagnostic](const qt::memory::RetrievalCancellation&)
            -> qt::memory::DenseIndexConfig { throw std::runtime_error(diagnostic); };
      }
      if (retrieval.dense_enabled && retrieval.embedding_model) {
        const auto& configured_models = services.settings.llama.models;
        const auto selected = std::find_if(configured_models.begin(), configured_models.end(),
            [&](const auto& model) { return model.id == *retrieval.embedding_model; });
        if (selected != configured_models.end()) {
          qt::memory::bind_llama_retrieval(memory_options.retrieval,
              services.llama_provider, *selected, retrieval.query_prefix,
              retrieval.document_prefix,
              services.settings.llama.embeddings.normalize.value_or(true));
        }
      }
      memory_options.coordinator.model =
          services.settings.memory.prehandler.model.value_or(
              memory_models.recall_prehandler.value_or(std::string{}));
      memory_options.coordinator.model_task = "recall_prehandler";
      memory_options.coordinator.thinking =
          services.settings.memory.prehandler.thinking;
      memory_options.coordinator.timeout_ms =
          services.settings.memory.prehandler.timeout_ms;
      if (!services.settings.memory.prehandler.model.has_value() &&
          memory_models.recall_prehandler.has_value()) {
        memory_options.coordinator.timeout_ms = std::max<std::int64_t>(
            memory_options.coordinator.timeout_ms,
            120000);
      }
      memory_options.coordinator.fail_open =
          services.settings.memory.prehandler.fail_open;
      memory_options.coordinator.max_model_input_tokens =
          std::min<std::size_t>(
              services.settings.memory.prehandler.max_model_input_tokens,
              memory_models.recall_prehandler.has_value() ? 3000U : 12000U);
      memory_options.coordinator.model_discovery_roots =
          memory_models.discovery_roots;
      if (memory_models.context_compilation.has_value() &&
          *memory_models.context_compilation !=
              memory_options.coordinator.model) {
        memory_options.consolidation_model =
            *memory_models.context_compilation;
        memory_options.consolidation_timeout_ms =
            memory_models.context_compilation_timeout_floor_ms;
        memory_options.consolidation_max_model_input_tokens =
            memory_models.context_compilation_input_token_limit;
        memory_options.consolidation_model_discovery_roots =
            memory_models.discovery_roots;
      }
      memory_options.dreaming.enabled =
          services.settings.memory.dreaming.enabled;
      memory_options.dreaming.trigger =
          services.settings.memory.dreaming.trigger;
      memory_options.dreaming.interval_seconds = static_cast<std::int64_t>(
          services.settings.memory.dreaming.interval_seconds);
      memory_options.dreaming.idle_seconds = static_cast<std::int64_t>(
          services.settings.memory.dreaming.idle_seconds);
      memory_options.dreaming.candidate_threshold =
          services.settings.memory.dreaming.candidate_threshold;
      memory_options.dreaming.model =
          services.settings.memory.dreaming.model.value_or(std::string{});
      memory_options.dreaming.verifier_model =
          services.settings.memory.dreaming.verifier_model.value_or(std::string{});
      memory_options.dreaming.timeout_ms =
          services.settings.memory.dreaming.timeout_ms;
      memory_options.dreaming.max_model_input_tokens =
          services.settings.memory.dreaming.max_model_input_tokens;
      memory_options.dreaming.max_candidates =
          services.settings.memory.dreaming.max_candidates;
      memory_options.dreaming.research_policy =
          services.settings.memory.dreaming.research_policy;
      memory_options.dreaming.promotion_policy =
          services.settings.memory.dreaming.promotion_policy;
      memory_options.isolated_agent.executable = subagent_executable();
      memory_options.automatic_capture =
          services.settings.memory.automatic_capture;
      memory_options.capture_post_process =
          services.settings.memory.capture.post_process;
      memory_options.max_evidence_characters =
          services.settings.memory.capture.max_evidence_chars;
      services.memory_runtime =
          std::make_shared<qt::memory::MemoryRuntime>(
              std::move(memory_options));
      if (request.include_builtin_tools && services.tool_registry) {
        const auto memory_tool_enabled = [&](const std::string& name) {
          if (request.enabled_tools.empty()) return true;
          if (std::find(request.enabled_tools.begin(), request.enabled_tools.end(),
                        name) != request.enabled_tools.end()) {
            return true;
          }
          return std::any_of(
              request.enabled_tools.begin(), request.enabled_tools.end(),
              [&](const std::string& pattern) {
                return pattern.find_first_of("*?") != std::string::npos &&
                       wildcard_match(pattern, name);
              });
        };
        tools::register_memory_tools(*services.tool_registry,
                                     services.memory_runtime,
                                     memory_tool_enabled);
        services.tool_definitions =
            build_tool_definitions(*services.tool_registry);
        for (const auto& tool : services.tool_definitions) {
          const auto errors = ai::validate(tool);
          if (!errors.empty()) {
            throw std::runtime_error(
                "invalid built-in memory tool definition for " + tool.name +
                ": " + errors.front());
          }
        }
        services.resource_bundle.system_prompt_input.active_tool_count =
            services.tool_definitions.size();
        services.resource_bundle.system_prompt = harness::build_system_prompt(
            services.resource_bundle.system_prompt_input);
      }
    } catch (const std::exception& error) {
      if (!services.settings.memory.prehandler.fail_open) {
        return Result<RuntimeServices>::failure(
            ErrorCode::io_error,
            std::string("unable to initialize built-in memory: ") +
                error.what());
      }
      services.diagnostics.push_back(
          std::string("built-in memory disabled after initialization error: ") +
          error.what());
    }
  }

  log_phase("memory");

  log.info("services_load_complete",
           {qt::foundation::log_field(
                "provider", services.model_selection.provider.descriptor.id),
            qt::foundation::log_field(
                "model", services.model_selection.model.ref.name),
            qt::foundation::log_field(
                "tools",
                static_cast<std::uint64_t>(
                    services.tool_definitions.size())),
            qt::foundation::log_field(
                "skills",
                static_cast<std::uint64_t>(
                    services.resource_bundle.skills.loaded_skills.size())),
            qt::foundation::log_field(
                "extensions",
                static_cast<std::uint64_t>(
                    services.resource_bundle.extensions.loaded_extensions
                        .size())),
            qt::foundation::log_field(
                "diagnostics",
                static_cast<std::uint64_t>(services.diagnostics.size()))});
  return services;
}

}  // namespace qt::app
