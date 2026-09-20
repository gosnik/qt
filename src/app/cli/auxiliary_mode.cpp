#include "qt/app/cli/auxiliary_mode.hpp"

#include "qt/agent/agent.hpp"
#include "qt/ai/provider.hpp"
#include "qt/app/runtime/extensions.hpp"
#include "qt/app/export/resource_inspection.hpp"
#include "qt/app/rpc/rpc_protocol.hpp"
#include "qt/app/config/settings.hpp"
#include "qt/app/slash/slash_commands.hpp"
#include "qt/extensions/loader.hpp"
#include "qt/extensions/runner.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/harness/compaction.hpp"
#include "qt/harness/context_discovery.hpp"
#include "qt/harness/context_resources.hpp"
#include "qt/harness/prompt_assembly.hpp"
#include "qt/harness/prompt_templates.hpp"
#include "qt/harness/repository.hpp"
#include "qt/harness/session_catalog.hpp"
#include "qt/harness/session_lifecycle.hpp"
#include "qt/harness/session_repository.hpp"
#include "qt/harness/session_resume.hpp"
#include "qt/harness/skills.hpp"
#include "qt/harness/system_prompt.hpp"
#include "qt/tools/append_text_tool.hpp"
#include "qt/tools/bash_tool.hpp"
#include "qt/tools/clipboard_tool.hpp"
#include "qt/tools/complete_path_tool.hpp"
#include "qt/tools/edit_text_tool.hpp"
#include "qt/tools/list_directory_tool.hpp"
#include "qt/tools/package_manager_tool.hpp"
#include "qt/tools/read_text_tool.hpp"
#include "qt/tools/registry.hpp"
#include "qt/tools/search_text_tool.hpp"
#include "qt/tools/write_text_tool.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

using Json = nlohmann::json;

class DemoProvider final : public qt::ai::Provider {
 public:
  DemoProvider(std::string output_path, std::string output_text)
      : output_path_(std::move(output_path)), output_text_(std::move(output_text)) {
    descriptor_.id = "demo";
    descriptor_.display_name = "Demo Provider";
    descriptor_.api = "demo";
    descriptor_.capabilities.streaming = false;
    descriptor_.capabilities.tool_calls = true;
  }

  const qt::ai::ProviderDescriptor& descriptor() const override {
    return descriptor_;
  }

  qt::ai::CompletionResponse complete(const qt::ai::CompletionRequest& request,
                                      const qt::ai::ProviderOptions& options) override {
    (void)request;
    (void)options;

    qt::ai::CompletionResponse response;
    response.message.role = qt::ai::MessageRole::assistant;
    if (!issued_tool_call_) {
      issued_tool_call_ = true;
      response.message.id = "demo-assistant-1";
      response.message.content = {
          qt::ai::TextContent{"Writing requested file.", std::nullopt},
          qt::ai::ToolCallContent{"call-write-1", "write_text",
                                  Json{{"path", output_path_},
                                       {"text", output_text_}}},
      };
      response.stop_reason = qt::ai::StopReason::tool_call;
      response.usage.input_tokens = 8;
      response.usage.output_tokens = 12;
    } else {
      response.message.id = "demo-assistant-2";
      response.message.content = {
          qt::ai::TextContent{"Demo output written.", std::nullopt},
      };
      response.stop_reason = qt::ai::StopReason::end_turn;
      response.usage.input_tokens = 4;
      response.usage.output_tokens = 6;
    }
    qt::ai::normalize(response.usage);
    response.metadata = Json{{"demo", true}};
    return response;
  }

 private:
  std::string output_path_;
  std::string output_text_;
  bool issued_tool_call_ = false;
  qt::ai::ProviderDescriptor descriptor_;
};

std::string current_date_string() {
  std::time_t now = std::time(nullptr);
  const std::tm* utc = std::gmtime(&now);
  if (!utc) {
    return "1970-01-01";
  }

  std::ostringstream out;
  out << std::put_time(utc, "%Y-%m-%d");
  return out.str();
}

std::string current_timezone_string() {
  std::time_t now = std::time(nullptr);
  const std::tm* local = std::localtime(&now);
  if (!local) {
    return {};
  }

  std::ostringstream out;
  out << std::put_time(local, "%Z %z");
  return out.str();
}

std::string shell_from_environment() {
  if (const char* shell = std::getenv("SHELL")) {
    if (*shell != '\0') {
      return std::string(shell);
    }
  }
  return {};
}

bool env_flag_enabled(const char* name) {
  const char* raw = std::getenv(name);
  if (!raw) {
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

std::string context_kind_name(qt::harness::ContextFileKind kind) {
  switch (kind) {
    case qt::harness::ContextFileKind::kAgents:
      return "AGENTS.md";
    case qt::harness::ContextFileKind::kClaude:
      return "CLAUDE.md";
  }
  return "unknown";
}

std::string context_scope_name(qt::harness::ContextFileScope scope) {
  switch (scope) {
    case qt::harness::ContextFileScope::kGlobal:
      return "global";
    case qt::harness::ContextFileScope::kAncestor:
      return "ancestor";
    case qt::harness::ContextFileScope::kCurrentDirectory:
      return "cwd";
  }
  return "unknown";
}

struct SessionInspectionSummary {
  std::size_t entry_count = 0;
  std::size_t metadata_count = 0;
  std::size_t label_count = 0;
  std::size_t summary_count = 0;
  std::size_t custom_count = 0;
  std::optional<qt::harness::SessionMetadata> metadata;
};

SessionInspectionSummary summarize_session_entries(
    const std::vector<qt::harness::SessionEntry>& entries) {
  SessionInspectionSummary summary;
  summary.entry_count = entries.size();
  for (const auto& entry : entries) {
    std::visit(
        [&summary](const auto& value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, qt::harness::SessionMetadataEntry>) {
            ++summary.metadata_count;
            if (!summary.metadata) {
              summary.metadata = value.metadata;
            }
          } else if constexpr (std::is_same_v<Value,
                                               qt::harness::SessionLabelEntry>) {
            ++summary.label_count;
          } else if constexpr (std::is_same_v<Value,
                                               qt::harness::SessionSummaryEntry>) {
            ++summary.summary_count;
          } else if constexpr (std::is_same_v<Value,
                                               qt::harness::SessionCustomEntry>) {
            ++summary.custom_count;
          }
        },
        entry.data);
  }
  return summary;
}

Json to_json(const qt::harness::RepoContextDiscovery& discovery) {
  Json context_files = Json::array();
  for (const auto& file : discovery.context_files) {
    context_files.push_back({
        {"path", file.path},
        {"directory", file.directory},
        {"kind", context_kind_name(file.kind)},
        {"scope", context_scope_name(file.scope)},
    });
  }

  return Json{
      {"cwd", discovery.cwd},
      {"repository_root", discovery.repository_root ? Json(*discovery.repository_root)
                                                    : Json(nullptr)},
      {"context_files", std::move(context_files)},
      {"skill_roots", discovery.skill_roots},
      {"prompt_roots", discovery.prompt_roots},
      {"diagnostics", discovery.diagnostics},
  };
}

Json to_json(const qt::harness::LoadedContextRecord& record) {
  return Json{
      {"path", record.path},
      {"directory", record.directory},
      {"kind", context_kind_name(record.kind)},
      {"scope", context_scope_name(record.scope)},
      {"content", record.content},
  };
}

Json to_json(const qt::harness::InstructionLoadEvent& event) {
  return Json{
      {"path", event.path},
      {"directory", event.directory},
      {"kind", context_kind_name(event.kind)},
      {"scope", context_scope_name(event.scope)},
      {"content_bytes", event.content_bytes},
      {"reason", event.reason},
  };
}

Json to_json(const qt::harness::RepoContextResources& resources) {
  Json loaded_context_files = Json::array();
  for (const auto& record : resources.loaded_context_files) {
    loaded_context_files.push_back(to_json(record));
  }

  Json instruction_load_events = Json::array();
  for (const auto& event : resources.instruction_load_events) {
    instruction_load_events.push_back(to_json(event));
  }

  return Json{
      {"discovery", to_json(resources.discovery)},
      {"loaded_context_files", std::move(loaded_context_files)},
      {"instruction_load_events", std::move(instruction_load_events)},
      {"diagnostics", resources.diagnostics},
  };
}

Json to_json(const qt::harness::LoadedSkillRecord& skill) {
  return Json{
      {"root", skill.root},
      {"directory", skill.directory},
      {"path", skill.path},
      {"name", skill.name},
      {"description", skill.description},
      {"content", skill.content},
      {"disable_model_invocation", skill.disable_model_invocation},
  };
}

Json to_json(const qt::harness::SkillResources& resources) {
  Json skills = Json::array();
  for (const auto& skill : resources.loaded_skills) {
    skills.push_back(to_json(skill));
  }

  return Json{
      {"roots", resources.roots},
      {"loaded_skills", std::move(skills)},
      {"diagnostics", resources.diagnostics},
  };
}

Json to_json(const qt::harness::PromptTemplateDiagnostic& diagnostic) {
  std::string code = "empty_root";
  switch (diagnostic.code) {
    case qt::harness::PromptTemplateDiagnosticCode::kEmptyRoot:
      code = "empty_root";
      break;
    case qt::harness::PromptTemplateDiagnosticCode::kRootDoesNotExist:
      code = "root_does_not_exist";
      break;
    case qt::harness::PromptTemplateDiagnosticCode::kRootIsNotDirectory:
      code = "root_is_not_directory";
      break;
    case qt::harness::PromptTemplateDiagnosticCode::kPromptFileNotReadable:
      code = "prompt_file_not_readable";
      break;
    case qt::harness::PromptTemplateDiagnosticCode::kPromptFileReadFailed:
      code = "prompt_file_read_failed";
      break;
    case qt::harness::PromptTemplateDiagnosticCode::kInvalidFrontmatter:
      code = "invalid_frontmatter";
      break;
  }

  return Json{
      {"code", code},
      {"path", diagnostic.path},
      {"message", diagnostic.message},
  };
}

Json to_json(const qt::harness::LoadedPromptTemplateRecord& prompt_template) {
  Json payload{
      {"root", prompt_template.root},
      {"path", prompt_template.path},
      {"name", prompt_template.name},
      {"description", prompt_template.description},
      {"content", prompt_template.content},
  };
  if (prompt_template.argument_hint) {
    payload["argument_hint"] = *prompt_template.argument_hint;
  }
  return payload;
}

Json to_json(const qt::harness::PromptTemplateResources& resources) {
  Json templates = Json::array();
  for (const auto& prompt_template : resources.loaded_prompt_templates) {
    templates.push_back(to_json(prompt_template));
  }

  Json diagnostics = Json::array();
  for (const auto& diagnostic : resources.diagnostics) {
    diagnostics.push_back(to_json(diagnostic));
  }

  return Json{
      {"roots", resources.roots},
      {"loaded_prompt_templates", std::move(templates)},
      {"diagnostics", std::move(diagnostics)},
  };
}

Json to_json(const qt::app::FeatureSettings& settings);
Json to_json(const qt::app::AppSettings& settings);

Json to_json(const qt::app::LoadedThemeRecord& theme) {
  return Json{
      {"root", theme.root},
      {"path", theme.path},
      {"name", theme.name},
      {"content", theme.content},
  };
}

Json to_json(const qt::app::ThemeResources& resources) {
  Json themes = Json::array();
  for (const auto& theme : resources.loaded_themes) {
    themes.push_back(to_json(theme));
  }

  return Json{
      {"roots", resources.roots},
      {"loaded_themes", std::move(themes)},
      {"diagnostics", resources.diagnostics},
  };
}

Json to_json(const qt::app::AppResourceLoadPlanEntry& entry) {
  return Json{
      {"kind", qt::app::to_string(entry.kind)},
      {"enabled", entry.enabled},
      {"roots", entry.roots},
  };
}

Json to_json(const qt::app::AppResourceInspection& inspection) {
  Json plan = Json::array();
  for (const auto& entry : inspection.plan) {
    plan.push_back(to_json(entry));
  }

  return Json{
      {"settings", to_json(inspection.settings)},
      {"plan", std::move(plan)},
      {"prompt_templates", to_json(inspection.prompt_templates)},
      {"skills", to_json(inspection.skills)},
      {"themes", to_json(inspection.themes)},
  };
}

Json to_json(const qt::app::FeatureSettings& settings) {
  return Json{
      {"load_prompt_templates", settings.load_prompt_templates},
      {"load_skills", settings.load_skills},
      {"load_themes", settings.load_themes},
      {"persist_sessions", settings.persist_sessions},
  };
}

Json to_json(const qt::app::MemorySettings& settings) {
  return Json{
      {"enabled", settings.enabled},
      {"automatic_recall", settings.automatic_recall},
      {"automatic_capture", settings.automatic_capture},
      {"recall_policy", settings.recall_policy},
      {"context_token_budget", settings.context_token_budget},
      {"candidate_limit", settings.candidate_limit},
      {"prehandler",
       Json{{"model", settings.prehandler.model
                           ? Json(*settings.prehandler.model)
                           : Json(nullptr)},
            {"thinking", settings.prehandler.thinking},
            {"timeout_ms", settings.prehandler.timeout_ms},
            {"fail_open", settings.prehandler.fail_open},
            {"max_model_input_tokens",
             settings.prehandler.max_model_input_tokens}}},
      {"capture",
       Json{{"post_process", settings.capture.post_process},
            {"default_status", settings.capture.default_status},
            {"max_evidence_chars", settings.capture.max_evidence_chars}}},
      {"dreaming",
       Json{{"enabled", settings.dreaming.enabled},
            {"trigger", settings.dreaming.trigger},
            {"interval_seconds", settings.dreaming.interval_seconds},
            {"idle_seconds", settings.dreaming.idle_seconds},
            {"candidate_threshold", settings.dreaming.candidate_threshold},
            {"model", settings.dreaming.model
                          ? Json(*settings.dreaming.model)
                          : Json(nullptr)},
            {"verifier_model", settings.dreaming.verifier_model
                                   ? Json(*settings.dreaming.verifier_model)
                                   : Json(nullptr)},
            {"timeout_ms", settings.dreaming.timeout_ms},
            {"max_model_input_tokens",
             settings.dreaming.max_model_input_tokens},
            {"max_candidates", settings.dreaming.max_candidates},
            {"research_policy", settings.dreaming.research_policy},
            {"promotion_policy", settings.dreaming.promotion_policy}}},
  };
}

Json to_json(const qt::app::AppSettings& settings) {
  const auto optional_string = [](const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
  };
  const auto optional_string_array =
      [](const std::optional<std::vector<std::string>>& value) {
        return value ? Json(*value) : Json(nullptr);
      };
  return Json{
      {"cwd", settings.cwd},
      {"workspace_root", settings.workspace_root},
      {"session_dir", settings.session_dir},
      {"agent_home", settings.agent_home},
      {"default_provider", optional_string(settings.default_provider)},
      {"default_model", optional_string(settings.default_model)},
      {"default_thinking_level", optional_string(settings.default_thinking_level)},
      {"theme", settings.theme},
      {"quiet_startup", settings.quiet_startup},
      {"collapse_changelog", settings.collapse_changelog},
      {"hide_thinking_block", settings.hide_thinking_block},
      {"enable_skill_commands", settings.enable_skill_commands},
      {"allow_external_filesystem_access",
       settings.allow_external_filesystem_access},
      {"terminal",
       Json{{"show_progress", settings.terminal_show_progress},
            {"prompt_history_limit", settings.terminal_prompt_history_limit},
            {"tool_entry_compaction_enabled_tools",
             settings.terminal_tool_entry_compaction_enabled_tools}}},
      {"steering_mode", settings.steering_mode},
      {"follow_up_mode", settings.follow_up_mode},
      {"transport", settings.transport},
      {"shell_path", optional_string(settings.shell_path)},
      {"shell_command_prefix", optional_string(settings.shell_command_prefix)},
      {"npm_command", optional_string_array(settings.npm_command)},
      {"enabled_models", settings.enabled_models},
      {"prompt_roots", settings.prompt_roots},
      {"skill_roots", settings.skill_roots},
      {"theme_roots", settings.theme_roots},
      {"features", to_json(settings.features)},
      {"memory", to_json(settings.memory)},
      {"llama", settings.llama},
  };
}

Json to_json(const qt::app::SlashCommand& command) {
  Json payload{
      {"name", command.name},
      {"description", command.description},
      {"source", qt::app::to_string(command.source)},
      {"source_path", command.source_path},
  };
  if (command.argument_hint) {
    payload["argument_hint"] = *command.argument_hint;
  }
  if (!command.target_name.empty() && command.target_name != command.name) {
    payload["target_name"] = command.target_name;
  }
  return payload;
}

Json to_json(const qt::app::SlashCommandRegistry& registry) {
  Json commands = Json::array();
  for (const auto& command : registry.commands) {
    commands.push_back(to_json(command));
  }
  return Json{
      {"commands", std::move(commands)},
      {"diagnostics", registry.diagnostics},
  };
}

Json to_json(const SessionInspectionSummary& summary) {
  Json payload{
      {"entry_count", summary.entry_count},
      {"metadata_count", summary.metadata_count},
      {"label_count", summary.label_count},
      {"summary_count", summary.summary_count},
      {"custom_count", summary.custom_count},
  };
  if (summary.metadata) {
    payload["session"] = *summary.metadata;
  }
  return payload;
}

Json to_json(const qt::ai::Message& message) { return Json(message); }
Json to_json(const qt::ai::CompletionRequest& request) { return Json(request); }
Json to_json(const qt::ai::CompletionResponse& response) { return Json(response); }
Json to_json(const qt::ai::StreamEvent& event) { return Json(event); }
Json to_json(const qt::tools::ToolRequest& request) { return Json(request); }

Json to_json(const qt::tools::ToolResult& result) {
  Json payload = Json(result);
  payload["status"] = qt::tools::to_string(result.status);
  return payload;
}

qt::tools::ToolContext make_tool_context(const std::string& cwd,
                                         const std::string& workspace_root) {
  qt::tools::ToolContext context;
  context.current_directory = std::filesystem::path(cwd);
  context.workspace_root =
      std::filesystem::path(workspace_root.empty() ? cwd : workspace_root);
  return context;
}

qt::tools::ToolRegistry make_builtin_tool_registry() {
  qt::tools::ToolRegistry registry;
  registry.register_tool(std::make_unique<qt::tools::AppendTextTool>());
  registry.register_tool(std::make_unique<qt::tools::BashTool>());
  registry.register_tool(std::make_unique<qt::tools::ClipboardTool>());
  registry.register_tool(std::make_unique<qt::tools::CompletePathTool>());
  registry.register_tool(std::make_unique<qt::tools::EditTextTool>());
  registry.register_tool(std::make_unique<qt::tools::ListDirectoryTool>());
  registry.register_tool(std::make_unique<qt::tools::PackageManagerTool>());
  registry.register_tool(std::make_unique<qt::tools::ReadTextTool>());
  registry.register_tool(std::make_unique<qt::tools::SearchTextTool>());
  registry.register_tool(std::make_unique<qt::tools::WriteTextTool>());
  return registry;
}

int print_tool_error_or_json(bool emit_json, const std::string& tool_name,
                             const qt::tools::ToolResult& result) {
  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
  } else {
    std::cerr << "qt: " << tool_name << " failed";
    if (result.error_message) {
      std::cerr << ": " << *result.error_message;
    }
    std::cerr << '\n';
  }
  return EXIT_FAILURE;
}

const qt::tools::Tool* require_registered_tool(
    const qt::tools::ToolRegistry& registry, const std::string& tool_name) {
  const auto tool = registry.find(tool_name);
  if (!tool) {
    throw std::runtime_error(tool_name + " tool is not registered");
  }
  return tool;
}

Json normalize_rpc_envelope(const Json& input) {
  if (!input.is_object()) {
    throw std::runtime_error("rpc envelope must be a JSON object");
  }

  const auto jsonrpc_it = input.find("jsonrpc");
  if (jsonrpc_it != input.end() && !jsonrpc_it->is_string()) {
    throw std::runtime_error("rpc envelope field 'jsonrpc' must be a string when present");
  }

  const std::string jsonrpc =
      jsonrpc_it == input.end() ? "2.0" : jsonrpc_it->get<std::string>();
  if (jsonrpc != "2.0") {
    throw std::runtime_error("rpc envelope field 'jsonrpc' must be '2.0'");
  }

  const auto method_it = input.find("method");
  const auto result_it = input.find("result");
  const auto error_it = input.find("error");
  const bool has_method = method_it != input.end();
  const bool has_result = result_it != input.end();
  const bool has_error = error_it != input.end();

  if (has_method) {
    if (!method_it->is_string() || method_it->get<std::string>().empty()) {
      throw std::runtime_error(
          "rpc envelope must contain a non-empty string method");
    }
    if (has_result || has_error) {
      throw std::runtime_error(
          "rpc envelope request must not contain result or error");
    }
  } else if (has_result == has_error) {
    throw std::runtime_error(
        "rpc envelope must contain exactly one of method, result, or error");
  }

  Json normalized{{"jsonrpc", jsonrpc}};
  if (has_method) {
    normalized["kind"] = input.contains("id") ? "request" : "notification";
    normalized["method"] = method_it->get<std::string>();
    normalized["params"] =
        input.contains("params") ? input.at("params") : Json::object();
    normalized["id"] = input.contains("id") ? input.at("id") : Json(nullptr);
  } else {
    if (!input.contains("id")) {
      throw std::runtime_error("rpc envelope response must contain id");
    }
    normalized["id"] = input.at("id");
    if (has_result) {
      normalized["kind"] = "response";
      normalized["result"] = *result_it;
    } else {
      if (!error_it->is_object()) {
        throw std::runtime_error("rpc envelope error must be an object");
      }
      const auto code_it = error_it->find("code");
      const auto message_it = error_it->find("message");
      if (code_it == error_it->end() || !code_it->is_number_integer()) {
        throw std::runtime_error(
            "rpc envelope error must contain integer code");
      }
      if (message_it == error_it->end() || !message_it->is_string() ||
          message_it->get<std::string>().empty()) {
        throw std::runtime_error(
            "rpc envelope error must contain non-empty string message");
      }

      normalized["kind"] = "error";
      normalized["error"] = Json{{"code", code_it->get<std::int64_t>()},
                                 {"message", message_it->get<std::string>()}};
      const auto data_it = error_it->find("data");
      if (data_it != error_it->end()) {
        normalized["error"]["data"] = *data_it;
      }
    }
  }

  if (input.contains("meta")) {
    normalized["meta"] = input.at("meta");
  }
  return normalized;
}

Json to_json(const qt::harness::PromptSection& section) {
  return Json{
      {"path", section.path},
      {"kind", context_kind_name(section.kind)},
      {"scope", context_scope_name(section.scope)},
      {"content", section.content},
  };
}

Json to_json(const qt::harness::PromptAssembly& assembly) {
  Json sections = Json::array();
  for (const auto& section : assembly.sections) {
    sections.push_back(to_json(section));
  }
  return Json{
      {"sections", std::move(sections)},
      {"diagnostics", assembly.diagnostics},
  };
}

Json to_json(const qt::harness::SystemPrompt& system_prompt) {
  return Json{
      {"prompt", system_prompt.prompt},
      {"diagnostics", system_prompt.diagnostics},
  };
}

Json to_json(const qt::harness::PersistentSessionRecord& record) {
  return Json{
      {"metadata", record.metadata},
      {"repository_path", record.repository_path},
  };
}

Json to_json(const qt::harness::SessionBranchSnapshot& snapshot) {
  Json payload{
      {"metadata", snapshot.metadata},
  };
  if (snapshot.latest_label) {
    payload["latest_label"] = *snapshot.latest_label;
  }
  if (snapshot.latest_summary) {
    payload["latest_summary"] = *snapshot.latest_summary;
  }
  return payload;
}

Json to_json(const qt::harness::ResumedSessionState& resumed) {
  Json entries = Json::array();
  for (const auto& entry : resumed.entries) {
    entries.push_back(Json(entry));
  }

  return Json{
      {"repository_path", resumed.repository_path},
      {"metadata", resumed.metadata},
      {"snapshot", to_json(resumed.snapshot)},
      {"entries", std::move(entries)},
  };
}

Json to_json(const qt::harness::SessionCompactionResult& result) {
  Json kept_entries = Json::array();
  for (const auto& entry : result.kept_entries) {
    kept_entries.push_back(Json(entry));
  }

  return Json{
      {"snapshot", to_json(result.snapshot)},
      {"original_entry_count", result.original_entry_count},
      {"kept_entry_count", result.kept_entry_count},
      {"compacted_entry_count", result.compacted_entry_count},
      {"kept_entries", std::move(kept_entries)},
      {"synthetic_entry", Json(result.synthetic_entry)},
  };
}

Json to_json(const qt::harness::CompactSessionRepositoryResult& result) {
  Json rewritten_entries = Json::array();
  for (const auto& entry : result.rewritten_entries) {
    rewritten_entries.push_back(Json(entry));
  }

  return Json{
      {"repository_path", result.repository_path},
      {"persisted", result.persisted},
      {"included_synthetic_entry", result.included_synthetic_entry},
      {"rewritten_entry_count", result.rewritten_entries.size()},
      {"rewritten_entries", std::move(rewritten_entries)},
      {"compaction", to_json(result.compaction)},
  };
}

Json to_json(const qt::extensions::LoadedExtensionRecord& extension) {
  return Json{
      {"root", extension.root},
      {"directory", extension.directory},
      {"path", extension.path},
      {"manifest", extension.manifest},
  };
}

Json to_json(const qt::extensions::ExtensionLoadResult& result) {
  Json loaded_extensions = Json::array();
  for (const auto& extension : result.loaded_extensions) {
    loaded_extensions.push_back(to_json(extension));
  }

  return Json{
      {"roots", result.roots},
      {"loaded_extensions", std::move(loaded_extensions)},
      {"diagnostics", result.diagnostics},
  };
}

Json to_json(const qt::extensions::ExtensionLaunchPlan& plan) {
  return Json{
      {"extension", to_json(plan.extension)},
      {"transport", qt::extensions::to_string(plan.transport)},
      {"executable_path", plan.executable_path},
      {"arguments", plan.arguments},
      {"working_directory", plan.working_directory},
  };
}

Json to_json(const qt::extensions::ExtensionRunnerLifecycle& lifecycle) {
  Json payload{
      {"launch_plan", to_json(lifecycle.launch_plan)},
      {"state", qt::extensions::to_string(lifecycle.state)},
      {"start_count", lifecycle.start_count},
      {"stop_count", lifecycle.stop_count},
  };
  if (lifecycle.process_id) {
    payload["process_id"] = *lifecycle.process_id;
  }
  return payload;
}

Json to_json(const qt::harness::SessionCatalogRecord& record) {
  return Json{
      {"repository_path", record.repository_path},
      {"snapshot", to_json(record.snapshot)},
  };
}

Json to_json(const qt::harness::SessionGraph& graph) {
  Json nodes = Json::array();
  for (const auto& node : graph.nodes) {
    nodes.push_back(Json{
        {"session", to_json(node.session)},
        {"child_session_ids", node.child_session_ids},
    });
  }

  return Json{
      {"nodes", std::move(nodes)},
      {"root_session_ids", graph.root_session_ids},
      {"unresolved_parent_session_ids", graph.unresolved_parent_session_ids},
  };
}

Json to_json(const qt::harness::SessionCatalog& catalog) {
  Json records = Json::array();
  for (const auto& record : catalog.session_records) {
    records.push_back(to_json(record));
  }

  return Json{
      {"sessions_directory", catalog.sessions_directory},
      {"session_records", std::move(records)},
      {"graph", to_json(catalog.graph)},
      {"root_session_ids", catalog.root_session_ids},
      {"diagnostics", catalog.diagnostics},
  };
}

Json to_json(const qt::agent::AgentEvent& event) {
  Json payload{
      {"sequence", event.sequence},
      {"kind", qt::agent::to_string(event.kind)},
      {"turn_index", event.turn_index},
  };
  if (event.queued_message) {
    payload["queued_message"] = {
        {"sequence", event.queued_message->sequence},
        {"disposition", qt::agent::to_string(event.queued_message->disposition)},
        {"message", to_json(event.queued_message->message)},
    };
  }
  if (event.request) {
    payload["request"] = to_json(*event.request);
  }
  if (event.provider_event) {
    payload["provider_event"] = to_json(*event.provider_event);
  }
  if (event.tool_request) {
    payload["tool_request"] = to_json(*event.tool_request);
  }
  if (event.tool_result) {
    payload["tool_result"] = to_json(*event.tool_result);
  }
  if (event.committed_message) {
    payload["committed_message"] = to_json(*event.committed_message);
  }
  if (event.response) {
    payload["response"] = to_json(*event.response);
  }
  if (event.error) {
    payload["error"] = *event.error;
  }
  if (event.retry_attempt) {
    payload["attempt"] = *event.retry_attempt;
  }
  if (event.retry_max_attempts) {
    payload["max_attempts"] = *event.retry_max_attempts;
  }
  if (event.retry_delay_ms) {
    payload["delay_ms"] = *event.retry_delay_ms;
  }
  if (event.retry_success) {
    payload["success"] = *event.retry_success;
  }
  return payload;
}

void print_context_plain(const qt::harness::RepoContextDiscovery& discovery,
                         std::ostream& out) {
  out << "cwd: " << discovery.cwd << '\n';
  out << "repository_root: "
      << (discovery.repository_root ? *discovery.repository_root : "(none)") << '\n';

  out << "context_files:\n";
  if (discovery.context_files.empty()) {
    out << "  (none)\n";
  } else {
    for (const auto& file : discovery.context_files) {
      out << "  - [" << context_scope_name(file.scope) << "] "
          << context_kind_name(file.kind) << " " << file.path << '\n';
    }
  }

  out << "skill_roots:\n";
  if (discovery.skill_roots.empty()) {
    out << "  (none)\n";
  } else {
    for (const auto& root : discovery.skill_roots) {
      out << "  - " << root << '\n';
    }
  }

  out << "prompt_roots:\n";
  if (discovery.prompt_roots.empty()) {
    out << "  (none)\n";
  } else {
    for (const auto& root : discovery.prompt_roots) {
      out << "  - " << root << '\n';
    }
  }

  if (!discovery.diagnostics.empty()) {
    out << "diagnostics:\n";
    for (const auto& diagnostic : discovery.diagnostics) {
      out << "  - " << diagnostic << '\n';
    }
  }
}

void print_context_resources_plain(const qt::harness::RepoContextResources& resources,
                                   std::ostream& out) {
  print_context_plain(resources.discovery, out);

  out << "loaded_context_files: " << resources.loaded_context_files.size() << '\n';
  for (const auto& record : resources.loaded_context_files) {
    out << "  - [" << context_scope_name(record.scope) << "] "
        << context_kind_name(record.kind) << " " << record.path
        << " (" << record.content.size() << " bytes)" << '\n';
  }

  if (!resources.diagnostics.empty()) {
    out << "resource_diagnostics:\n";
    for (const auto& diagnostic : resources.diagnostics) {
      out << "  - " << diagnostic << '\n';
    }
  }
}

int run_context_mode(bool emit_json, const std::string& cwd,
                     const std::string& agent_home,
                     const std::string& repository_root_hint) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::harness::RepoDiscoveryRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.repository_root_hint = repository_root_hint;
  }

  const auto discovery = qt::harness::discover_repo_context(repository, request);
  if (emit_json) {
    std::cout << to_json(discovery).dump(2) << '\n';
  } else {
    print_context_plain(discovery, std::cout);
  }
  return EXIT_SUCCESS;
}

int run_context_resources_mode(bool emit_json, const std::string& cwd,
                               const std::string& agent_home,
                               const std::string& repository_root_hint) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::harness::RepoDiscoveryRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.repository_root_hint = repository_root_hint;
  }

  const auto resources =
      qt::harness::discover_and_load_repo_context(repository, request);
  if (emit_json) {
    std::cout << to_json(resources).dump(2) << '\n';
  } else {
    print_context_resources_plain(resources, std::cout);
  }
  return EXIT_SUCCESS;
}

int run_prompt_assembly_mode(bool emit_json, const std::string& cwd,
                             const std::string& agent_home,
                             const std::string& repository_root_hint) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::harness::RepoDiscoveryRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.repository_root_hint = repository_root_hint;
  }

  const auto resources =
      qt::harness::discover_and_load_repo_context(repository, request);
  const auto assembly = qt::harness::assemble_prompt_sections(resources);
  if (emit_json) {
    std::cout << Json{{"resources", to_json(resources)},
                      {"assembly", to_json(assembly)}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  print_context_resources_plain(resources, std::cout);
  std::cout << "prompt_sections: " << assembly.sections.size() << '\n';
  for (const auto& section : assembly.sections) {
    std::cout << "  - [" << context_scope_name(section.scope) << "] "
              << context_kind_name(section.kind) << " " << section.path
              << " (" << section.content.size() << " bytes)" << '\n';
  }
  if (!assembly.diagnostics.empty()) {
    std::cout << "prompt_diagnostics:\n";
    for (const auto& diagnostic : assembly.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_skills_mode(bool emit_json, const std::string& cwd,
                    const std::string& agent_home,
                    const std::string& repository_root_hint) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::harness::RepoDiscoveryRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.repository_root_hint = repository_root_hint;
  }

  const auto discovery = qt::harness::discover_repo_context(repository, request);
  const auto skills = qt::harness::discover_and_load_skills(repository, discovery);
  if (emit_json) {
    std::cout << Json{{"discovery", to_json(discovery)},
                      {"skills", to_json(skills)}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  print_context_plain(discovery, std::cout);
  std::cout << "loaded_skills: " << skills.loaded_skills.size() << '\n';
  for (const auto& skill : skills.loaded_skills) {
    std::cout << "  - " << skill.name << " " << skill.path << " ("
              << skill.content.size() << " bytes)" << '\n';
  }
  if (!skills.diagnostics.empty()) {
    std::cout << "skill_diagnostics:\n";
    for (const auto& diagnostic : skills.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_prompt_templates_mode(bool emit_json, const std::string& cwd,
                              const std::string& agent_home,
                              const std::string& repository_root_hint) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::harness::RepoDiscoveryRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.repository_root_hint = repository_root_hint;
  }

  const auto discovery = qt::harness::discover_repo_context(repository, request);
  const auto prompt_templates =
      qt::harness::discover_and_load_prompt_templates(repository, discovery);
  if (emit_json) {
    std::cout << Json{{"discovery", to_json(discovery)},
                      {"prompt_templates", to_json(prompt_templates)}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  print_context_plain(discovery, std::cout);
  std::cout << "loaded_prompt_templates: "
            << prompt_templates.loaded_prompt_templates.size() << '\n';
  for (const auto& prompt_template : prompt_templates.loaded_prompt_templates) {
    std::cout << "  - /" << prompt_template.name;
    if (prompt_template.argument_hint) {
      std::cout << " " << *prompt_template.argument_hint;
    }
    if (!prompt_template.description.empty()) {
      std::cout << " - " << prompt_template.description;
    }
    std::cout << '\n';
  }
  if (!prompt_templates.diagnostics.empty()) {
    std::cout << "prompt_template_diagnostics:\n";
    for (const auto& diagnostic : prompt_templates.diagnostics) {
      std::cout << "  - " << diagnostic.message << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_slash_commands_mode(bool emit_json, const std::string& cwd,
                            const std::string& agent_home,
                            const std::string& repository_root_hint,
                            const std::vector<std::string>& cli_extensions,
                            const qt::app::LoadReferenceExtensionOptions&
                                extension_options) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::harness::RepoDiscoveryRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.repository_root_hint = repository_root_hint;
  }

  const auto discovery = qt::harness::discover_repo_context(repository, request);
  qt::app::LoadSettingsRequest settings_request;
  settings_request.cwd = cwd;
  if (!agent_home.empty()) {
    settings_request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    settings_request.workspace_root = repository_root_hint;
  }
  const auto settings = qt::app::load_app_settings(repository, settings_request);
  if (!settings) {
    std::cerr << "qt: inspect-slash-commands failed: "
              << settings.error().summary() << '\n';
    return EXIT_FAILURE;
  }
  const auto resources = qt::app::inspect_app_resources(repository, settings.value());
  const auto extensions = load_extensions(
      repository, cwd, settings.value().agent_home, settings.value().workspace_root,
      cli_extensions, extension_options);
  qt::app::SlashCommandRegistryOptions registry_options;
  registry_options.enable_skill_commands = settings.value().enable_skill_commands;
  const auto registry = qt::app::build_slash_command_registry(
      resources.prompt_templates, resources.skills, extensions, registry_options);

  if (emit_json) {
    std::cout << Json{{"discovery", to_json(discovery)},
                      {"settings", to_json(settings.value())},
                      {"slash_commands", to_json(registry)}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  print_context_plain(discovery, std::cout);
  std::cout << "slash_commands: " << registry.commands.size() << '\n';
  for (const auto& command : registry.commands) {
    std::cout << "  - /" << command.name;
    if (command.argument_hint) {
      std::cout << " " << *command.argument_hint;
    }
    std::cout << " [" << qt::app::to_string(command.source) << "]";
    if (!command.description.empty()) {
      std::cout << " - " << command.description;
    }
    std::cout << '\n';
  }
  if (!registry.diagnostics.empty()) {
    std::cout << "slash_command_diagnostics:\n";
    for (const auto& diagnostic : registry.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_settings_mode(bool emit_json, const std::string& cwd,
                      const std::string& agent_home,
                      const std::string& repository_root_hint) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::app::LoadSettingsRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.workspace_root = repository_root_hint;
  }

  const auto settings = qt::app::load_app_settings(repository, request);
  if (!settings) {
    if (emit_json) {
      std::cout << Json{{"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(settings.error().code)},
                          {"message", settings.error().message},
                          {"system_error", settings.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: inspect-settings failed: "
                << settings.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  if (emit_json) {
    std::cout << Json{{"ok", true}, {"settings", to_json(settings.value())}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "cwd: " << settings.value().cwd << '\n';
  std::cout << "workspace_root: " << settings.value().workspace_root << '\n';
  std::cout << "session_dir: " << settings.value().session_dir << '\n';
  std::cout << "agent_home: " << settings.value().agent_home << '\n';
  if (settings.value().default_provider) {
    std::cout << "default_provider: " << *settings.value().default_provider << '\n';
  }
  if (settings.value().default_model) {
    std::cout << "default_model: " << *settings.value().default_model << '\n';
  }
  if (settings.value().default_thinking_level) {
    std::cout << "default_thinking_level: "
              << *settings.value().default_thinking_level << '\n';
  }
  std::cout << "theme: " << settings.value().theme << '\n';
  std::cout << "enable_skill_commands: "
            << (settings.value().enable_skill_commands ? "true" : "false")
            << '\n';
  std::cout << "allow_external_filesystem_access: "
            << (settings.value().allow_external_filesystem_access ? "true"
                                                                  : "false")
            << '\n';
  std::cout << "terminal:\n";
  std::cout << "  - show_progress: "
            << (settings.value().terminal_show_progress ? "true" : "false")
            << '\n';
  std::cout << "  - prompt_history_limit: "
            << settings.value().terminal_prompt_history_limit << '\n';
  std::cout << "  - tool_entry_compaction_enabled_tools:\n";
  for (const auto& tool :
       settings.value().terminal_tool_entry_compaction_enabled_tools) {
    std::cout << "    - " << tool << '\n';
  }
  std::cout << "steering_mode: " << settings.value().steering_mode << '\n';
  std::cout << "follow_up_mode: " << settings.value().follow_up_mode << '\n';
  std::cout << "transport: " << settings.value().transport << '\n';
  std::cout << "prompt_roots:\n";
  for (const auto& root : settings.value().prompt_roots) {
    std::cout << "  - " << root << '\n';
  }
  std::cout << "skill_roots:\n";
  for (const auto& root : settings.value().skill_roots) {
    std::cout << "  - " << root << '\n';
  }
  std::cout << "theme_roots:\n";
  for (const auto& root : settings.value().theme_roots) {
    std::cout << "  - " << root << '\n';
  }
  std::cout << "features:\n";
  std::cout << "  - load_prompt_templates: "
            << (settings.value().features.load_prompt_templates ? "true" : "false")
            << '\n';
  std::cout << "  - load_skills: "
            << (settings.value().features.load_skills ? "true" : "false")
            << '\n';
  std::cout << "  - load_themes: "
            << (settings.value().features.load_themes ? "true" : "false")
            << '\n';
  std::cout << "  - persist_sessions: "
            << (settings.value().features.persist_sessions ? "true" : "false")
            << '\n';
  std::cout << "memory:\n";
  std::cout << "  - enabled: "
            << (settings.value().memory.enabled ? "true" : "false") << '\n';
  std::cout << "  - automatic_recall: "
            << (settings.value().memory.automatic_recall ? "true" : "false")
            << '\n';
  std::cout << "  - automatic_capture: "
            << (settings.value().memory.automatic_capture ? "true" : "false")
            << '\n';
  std::cout << "  - recall_policy: "
            << settings.value().memory.recall_policy << '\n';
  std::cout << "  - context_token_budget: "
            << settings.value().memory.context_token_budget << '\n';
  std::cout << "  - candidate_limit: "
            << settings.value().memory.candidate_limit << '\n';
  std::cout << "  - prehandler:\n";
  std::cout << "    - model: "
            << settings.value().memory.prehandler.model.value_or("null") << '\n';
  std::cout << "    - thinking: "
            << settings.value().memory.prehandler.thinking << '\n';
  std::cout << "    - timeout_ms: "
            << settings.value().memory.prehandler.timeout_ms << '\n';
  std::cout << "    - fail_open: "
            << (settings.value().memory.prehandler.fail_open ? "true" : "false")
            << '\n';
  std::cout << "    - max_model_input_tokens: "
            << settings.value().memory.prehandler.max_model_input_tokens << '\n';
  std::cout << "  - capture:\n";
  std::cout << "    - post_process: "
            << settings.value().memory.capture.post_process << '\n';
  std::cout << "    - default_status: "
            << settings.value().memory.capture.default_status << '\n';
  std::cout << "    - max_evidence_chars: "
            << settings.value().memory.capture.max_evidence_chars << '\n';
  std::cout << "  - dreaming:\n";
  std::cout << "    - enabled: "
            << (settings.value().memory.dreaming.enabled ? "true" : "false")
            << '\n';
  std::cout << "    - trigger: "
            << settings.value().memory.dreaming.trigger << '\n';
  std::cout << "    - interval_seconds: "
            << settings.value().memory.dreaming.interval_seconds << '\n';
  std::cout << "    - idle_seconds: "
            << settings.value().memory.dreaming.idle_seconds << '\n';
  std::cout << "    - candidate_threshold: "
            << settings.value().memory.dreaming.candidate_threshold << '\n';
  std::cout << "    - model: "
            << settings.value().memory.dreaming.model.value_or("auto") << '\n';
  std::cout << "    - verifier_model: "
            << settings.value().memory.dreaming.verifier_model.value_or("unset")
            << '\n';
  std::cout << "    - timeout_ms: "
            << settings.value().memory.dreaming.timeout_ms << '\n';
  std::cout << "    - max_model_input_tokens: "
            << settings.value().memory.dreaming.max_model_input_tokens << '\n';
  std::cout << "    - max_candidates: "
            << settings.value().memory.dreaming.max_candidates << '\n';
  std::cout << "    - research_policy: "
            << settings.value().memory.dreaming.research_policy << '\n';
  std::cout << "    - promotion_policy: "
            << settings.value().memory.dreaming.promotion_policy << '\n';
  return EXIT_SUCCESS;
}

int run_app_resources_mode(bool emit_json, const std::string& cwd,
                           const std::string& agent_home,
                           const std::string& repository_root_hint) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::app::LoadSettingsRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.workspace_root = repository_root_hint;
  }

  const auto settings = qt::app::load_app_settings(repository, request);
  if (!settings) {
    if (emit_json) {
      std::cout << Json{{"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(settings.error().code)},
                          {"message", settings.error().message},
                          {"system_error", settings.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: inspect-app-resources failed: "
                << settings.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  const auto inspection =
      qt::app::inspect_app_resources(repository, settings.value());
  if (emit_json) {
    std::cout << Json{{"ok", true}, {"resources", to_json(inspection)}}.dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "resource_plan:\n";
  for (const auto& entry : inspection.plan) {
    std::cout << "  - " << qt::app::to_string(entry.kind) << " enabled="
              << (entry.enabled ? "true" : "false")
              << " roots=" << entry.roots.size() << '\n';
  }
  std::cout << "loaded_prompt_templates: "
            << inspection.prompt_templates.loaded_prompt_templates.size() << '\n';
  std::cout << "loaded_skills: " << inspection.skills.loaded_skills.size() << '\n';
  std::cout << "loaded_themes: " << inspection.themes.loaded_themes.size() << '\n';
  if (!inspection.prompt_templates.diagnostics.empty() ||
      !inspection.skills.diagnostics.empty() ||
      !inspection.themes.diagnostics.empty()) {
    std::cout << "resource_diagnostics:\n";
    for (const auto& diagnostic : inspection.prompt_templates.diagnostics) {
      std::cout << "  - " << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : inspection.skills.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
    for (const auto& diagnostic : inspection.themes.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_system_prompt_mode(bool emit_json, const std::string& cwd,
                           const std::string& agent_home,
                           const std::string& repository_root_hint,
                           const std::string& custom_guidance) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::harness::RepoDiscoveryRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!repository_root_hint.empty()) {
    request.repository_root_hint = repository_root_hint;
  }

  const auto resources =
      qt::harness::discover_and_load_repo_context(repository, request);
  const auto skills =
      qt::harness::discover_and_load_skills(repository, resources.discovery);
  const auto assembly = qt::harness::assemble_prompt_sections(resources);
  auto input = qt::harness::make_system_prompt_input(assembly);
  input.working_directory = cwd;
  input.workspace_root = resources.discovery.repository_root.value_or(cwd);
  input.shell = shell_from_environment();
  input.current_date = current_date_string();
  input.timezone = current_timezone_string();
  input.network_access = network_access_summary();
  input.tool_permissions = "tool mutations execute immediately";
  input.skills = skills.loaded_skills;
  if (!custom_guidance.empty()) {
    input.custom_guidance = custom_guidance;
  }
  const auto prompt = qt::harness::build_system_prompt(input);

  if (emit_json) {
    std::cout << Json{{"resources", to_json(resources)},
                      {"skills", to_json(skills)},
                      {"assembly", to_json(assembly)},
                      {"system_prompt", to_json(prompt)}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  print_context_resources_plain(resources, std::cout);
  std::cout << "system_prompt:\n\n" << prompt.prompt << '\n';
  if (!prompt.diagnostics.empty()) {
    std::cout << "\nsystem_prompt_diagnostics:\n";
    for (const auto& diagnostic : prompt.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_extensions_mode(bool emit_json, const std::string& cwd,
                        const std::string& agent_home,
                        const std::string& workspace_root_hint,
                        const std::vector<std::string>& cli_extensions,
                        const qt::app::LoadReferenceExtensionOptions&
                            extension_options) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::app::LoadSettingsRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!workspace_root_hint.empty()) {
    request.workspace_root = workspace_root_hint;
  }
  const auto settings = qt::app::load_app_settings(repository, request);
  if (!settings) {
    std::cerr << "qt: inspect-extensions failed: "
              << settings.error().summary() << '\n';
    return EXIT_FAILURE;
  }
  const auto loaded = load_extensions(
      repository, cwd, settings.value().agent_home, settings.value().workspace_root,
      cli_extensions, extension_options);
  if (emit_json) {
    std::cout << to_json(loaded).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "extension_roots: " << loaded.roots.size() << '\n';
  for (const auto& root : loaded.roots) {
    std::cout << "  - " << root << '\n';
  }
  std::cout << "loaded_extensions: " << loaded.loaded_extensions.size() << '\n';
  for (const auto& extension : loaded.loaded_extensions) {
    std::cout << "  - " << extension.manifest.name << " " << extension.path
              << '\n';
  }
  if (!loaded.diagnostics.empty()) {
    std::cout << "extension_diagnostics:\n";
    for (const auto& diagnostic : loaded.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  return EXIT_SUCCESS;
}

void print_session_plain(const std::string& path,
                         const SessionInspectionSummary& summary,
                         std::ostream& out) {
  out << "path: " << path << '\n';
  out << "entries: " << summary.entry_count << '\n';
  out << "metadata_entries: " << summary.metadata_count << '\n';
  out << "labels: " << summary.label_count << '\n';
  out << "summaries: " << summary.summary_count << '\n';
  out << "custom_entries: " << summary.custom_count << '\n';

  if (!summary.metadata) {
    return;
  }

  out << "session_id: " << summary.metadata->session_id << '\n';
  out << "working_directory: " << summary.metadata->working_directory << '\n';
  out << "persistent: " << (summary.metadata->persistent ? "true" : "false")
      << '\n';
  if (summary.metadata->display_name) {
    out << "display_name: " << *summary.metadata->display_name << '\n';
  }
  if (summary.metadata->parent_session_id) {
    out << "parent_session_id: " << *summary.metadata->parent_session_id
        << '\n';
  }
  if (summary.metadata->created_at) {
    out << "created_at: " << *summary.metadata->created_at << '\n';
  }
}

int run_inspect_session_mode(bool emit_json, const std::string& path) {
  const qt::harness::SessionRepository repository(path);
  const auto loaded = repository.load();
  if (!loaded) {
    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(loaded.error().code)},
                          {"message", loaded.error().message},
                          {"system_error", loaded.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: inspect-session failed: "
                << loaded.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  const auto summary = summarize_session_entries(loaded.value());
  if (emit_json) {
    Json entries = Json::array();
    for (const auto& entry : loaded.value()) {
      entries.push_back(Json(entry));
    }
    std::cout << Json{{"path", path},
                      {"ok", true},
                      {"summary", to_json(summary)},
                      {"entries", std::move(entries)}}
                     .dump(2)
              << '\n';
  } else {
    print_session_plain(path, summary, std::cout);
  }
  return EXIT_SUCCESS;
}

int run_resume_session_mode(bool emit_json, const std::string& path) {
  const auto resumed = qt::harness::resume_session(path);
  if (!resumed) {
    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(resumed.error().code)},
                          {"message", resumed.error().message},
                          {"system_error", resumed.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: resume-session failed: " << resumed.error().summary()
                << '\n';
    }
    return EXIT_FAILURE;
  }

  const auto summary = summarize_session_entries(resumed.value().entries);
  if (emit_json) {
    std::cout << Json{{"ok", true}, {"session", to_json(resumed.value())}}.dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  print_session_plain(path, summary, std::cout);
  if (resumed.value().snapshot.latest_label) {
    std::cout << "latest_label: " << *resumed.value().snapshot.latest_label
              << '\n';
  }
  if (resumed.value().snapshot.latest_summary) {
    std::cout << "latest_summary: " << *resumed.value().snapshot.latest_summary
              << '\n';
  }
  return EXIT_SUCCESS;
}

int run_create_session_mode(bool emit_json, const std::string& path,
                            const std::string& session_id,
                            const std::string& cwd,
                            const std::string& session_name,
                            const std::string& created_at) {
  const auto created = qt::harness::create_persistent_session(
      qt::harness::CreatePersistentSessionRequest{
          session_id,
          cwd,
          path,
          session_name.empty() ? std::nullopt
                               : std::optional<std::string>(session_name),
          created_at.empty() ? std::nullopt
                             : std::optional<std::string>(created_at),
      });
  if (!created) {
    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(created.error().code)},
                          {"message", created.error().message},
                          {"system_error", created.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: create-session failed: "
                << created.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  if (emit_json) {
    std::cout << Json{{"ok", true}, {"session", to_json(created.value())}}.dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "ok: true\n";
  std::cout << "session_id: " << created.value().metadata.session_id << '\n';
  std::cout << "working_directory: "
            << created.value().metadata.working_directory << '\n';
  std::cout << "repository_path: " << created.value().repository_path << '\n';
  return EXIT_SUCCESS;
}

int run_fork_session_mode(bool emit_json, const std::string& parent_path,
                          const std::string& child_path,
                          const std::string& child_session_id,
                          const std::string& child_working_directory,
                          const std::string& session_name,
                          const std::string& created_at) {
  const auto parent_snapshot =
      qt::harness::load_session_branch_snapshot(parent_path);
  if (!parent_snapshot) {
    if (emit_json) {
      std::cout << Json{{"path", parent_path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(parent_snapshot.error().code)},
                          {"message", parent_snapshot.error().message},
                          {"system_error", parent_snapshot.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: fork-session failed to load parent: "
                << parent_snapshot.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  const auto child = qt::harness::fork_child_session(
      qt::harness::ForkSessionRequest{
          parent_snapshot.value(),
          child_session_id,
          child_path,
          child_working_directory.empty()
              ? std::nullopt
              : std::optional<std::string>(child_working_directory),
          session_name.empty() ? std::nullopt
                               : std::optional<std::string>(session_name),
          created_at.empty() ? std::nullopt
                             : std::optional<std::string>(created_at),
      });
  if (!child) {
    if (emit_json) {
      std::cout << Json{{"path", child_path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(child.error().code)},
                          {"message", child.error().message},
                          {"system_error", child.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: fork-session failed: " << child.error().summary()
                << '\n';
    }
    return EXIT_FAILURE;
  }

  if (emit_json) {
    std::cout << Json{{"ok", true},
                      {"parent_snapshot", to_json(parent_snapshot.value())},
                      {"child_session", to_json(child.value())}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "ok: true\n";
  std::cout << "parent_session_id: "
            << parent_snapshot.value().metadata.session_id << '\n';
  std::cout << "child_session_id: " << child.value().metadata.session_id << '\n';
  std::cout << "child_repository_path: " << child.value().repository_path << '\n';
  return EXIT_SUCCESS;
}

int run_clone_session_mode(bool emit_json, const std::string& source_path,
                           const std::string& cloned_path,
                           const std::string& cloned_session_id) {
  const auto cloned = qt::harness::clone_session_repository(
      qt::harness::CloneSessionRepositoryRequest{
          source_path,
          cloned_session_id,
          cloned_path,
      });
  if (!cloned) {
    if (emit_json) {
      std::cout << Json{{"path", cloned_path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(cloned.error().code)},
                          {"message", cloned.error().message},
                          {"system_error", cloned.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: clone-session failed: " << cloned.error().summary()
                << '\n';
    }
    return EXIT_FAILURE;
  }

  if (emit_json) {
    std::cout << Json{{"ok", true}, {"session", to_json(cloned.value())}}.dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "ok: true\n";
  std::cout << "session_id: " << cloned.value().metadata.session_id << '\n';
  if (cloned.value().metadata.parent_session_id) {
    std::cout << "parent_session_id: "
              << *cloned.value().metadata.parent_session_id << '\n';
  }
  std::cout << "repository_path: " << cloned.value().repository_path << '\n';
  return EXIT_SUCCESS;
}

int run_session_catalog_mode(bool emit_json,
                             const std::string& sessions_directory) {
  const auto catalog = qt::harness::load_session_catalog(sessions_directory);
  if (!catalog) {
    if (emit_json) {
      std::cout << Json{{"path", sessions_directory},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(catalog.error().code)},
                          {"message", catalog.error().message},
                          {"system_error", catalog.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: inspect-session-catalog failed: "
                << catalog.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  if (emit_json) {
    std::cout << Json{{"ok", true}, {"catalog", to_json(catalog.value())}}.dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "sessions_directory: " << catalog.value().sessions_directory << '\n';
  std::cout << "session_records: " << catalog.value().session_records.size()
            << '\n';
  std::cout << "root_sessions: " << catalog.value().root_session_ids.size()
            << '\n';
  for (const auto& root_id : catalog.value().root_session_ids) {
    std::cout << "  - " << root_id << '\n';
  }
  const auto tree_rows = qt::harness::render_session_tree(catalog.value().graph);
  if (!tree_rows) {
    std::cerr << "qt: inspect-session-catalog tree render failed: "
              << tree_rows.error().summary() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "session_tree:\n";
  for (const auto& row : tree_rows.value()) {
    std::cout << row.row_number << ". " << row.display_text << '\n';
  }
  if (!catalog.value().diagnostics.empty()) {
    std::cout << "catalog_diagnostics:\n";
    for (const auto& diagnostic : catalog.value().diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_compact_session_mode(bool emit_json, const std::string& path,
                             bool apply_compaction) {
  const auto compacted = qt::harness::compact_session_repository(
      qt::harness::CompactSessionRepositoryRequest{
          path,
          apply_compaction,
          true,
      });
  if (!compacted) {
    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(compacted.error().code)},
                          {"message", compacted.error().message},
                          {"system_error", compacted.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: compact-session failed: "
                << compacted.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  const auto summary =
      qt::harness::render_compact_session_repository_summary(compacted.value());
  if (!summary) {
    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", false},
                        {"error",
                         {{"code",
                           qt::foundation::to_string(summary.error().code)},
                          {"message", summary.error().message},
                          {"system_error", summary.error().system_error}}}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: compact-session failed to render summary: "
                << summary.error().summary() << '\n';
    }
    return EXIT_FAILURE;
  }

  if (emit_json) {
    std::cout << Json{{"ok", true},
                      {"compacted_session", to_json(compacted.value())},
                      {"summary_lines", summary.value().lines}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  for (const auto& line : summary.value().lines) {
    std::cout << line << '\n';
  }
  return EXIT_SUCCESS;
}

int run_extension_runners_mode(bool emit_json, const std::string& cwd,
                               const std::string& agent_home,
                               const std::string& workspace_root_hint,
                               const std::vector<std::string>& cli_extensions,
                               const qt::app::LoadReferenceExtensionOptions&
                                   extension_options) {
  qt::harness::FilesystemRepository repository(cwd);
  qt::app::LoadSettingsRequest request;
  request.cwd = cwd;
  if (!agent_home.empty()) {
    request.agent_home = agent_home;
  }
  if (!workspace_root_hint.empty()) {
    request.workspace_root = workspace_root_hint;
  }
  const auto settings = qt::app::load_app_settings(repository, request);
  if (!settings) {
    std::cerr << "qt: inspect-extension-runners failed: "
              << settings.error().summary() << '\n';
    return EXIT_FAILURE;
  }
  const auto loaded = load_extensions(
      repository, cwd, settings.value().agent_home, settings.value().workspace_root,
      cli_extensions, extension_options);
  Json runners = Json::array();
  Json runner_diagnostics = Json::array();
  for (const auto& extension : loaded.loaded_extensions) {
    const auto lifecycle =
        qt::extensions::make_extension_runner_lifecycle(extension);
    if (!lifecycle) {
      runner_diagnostics.push_back(Json{
          {"path", extension.path},
          {"name", extension.manifest.name},
          {"code", qt::foundation::to_string(lifecycle.error().code)},
          {"message", lifecycle.error().message},
          {"system_error", lifecycle.error().system_error},
      });
      continue;
    }
    runners.push_back(to_json(lifecycle.value()));
  }

  if (emit_json) {
    std::cout << Json{{"ok", true},
                      {"extensions", to_json(loaded)},
                      {"runners", std::move(runners)},
                      {"runner_diagnostics", std::move(runner_diagnostics)}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "runner_extensions: " << runners.size() << '\n';
  for (const auto& runner : runners) {
    std::cout << "  - "
              << runner["launch_plan"]["extension"]["manifest"]["name"]
                     .get<std::string>()
              << " -> "
              << runner["launch_plan"]["executable_path"].get<std::string>()
              << '\n';
  }
  if (!loaded.diagnostics.empty()) {
    std::cout << "extension_diagnostics:\n";
    for (const auto& diagnostic : loaded.diagnostics) {
      std::cout << "  - " << diagnostic << '\n';
    }
  }
  if (!runner_diagnostics.empty()) {
    std::cout << "runner_diagnostics:\n";
    for (const auto& diagnostic : runner_diagnostics) {
      std::cout << "  - " << diagnostic.at("name").get<std::string>() << ": "
                << diagnostic.at("message").get<std::string>() << '\n';
    }
  }
  return EXIT_SUCCESS;
}

int run_rpc_envelope_mode(bool emit_json, const std::string& path) {
  try {
    std::ifstream input(path);
    if (!input) {
      throw std::runtime_error("unable to open rpc envelope file");
    }

    Json parsed;
    input >> parsed;
    const Json normalized = normalize_rpc_envelope(parsed);

    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", true},
                        {"envelope", normalized}}
                       .dump(2)
                << '\n';
      return EXIT_SUCCESS;
    }

    std::cout << "path: " << path << '\n';
    std::cout << "ok: true\n";
    std::cout << "jsonrpc: " << normalized.at("jsonrpc").get<std::string>() << '\n';
    std::cout << "kind: " << normalized.at("kind").get<std::string>() << '\n';
    std::cout << "method: " << normalized.at("method").get<std::string>() << '\n';
    std::cout << "has_id: " << (!normalized.at("id").is_null() ? "true" : "false")
              << '\n';
    std::cout << "params_type: " << normalized.at("params").type_name() << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", false},
                        {"error", error.what()}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: rpc-envelope failed: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
  }
}

int run_rpc_frame_mode(bool emit_json, const std::string& path) {
  try {
    std::ifstream input(path);
    if (!input) {
      throw std::runtime_error("unable to open rpc frame file");
    }

    std::string line;
    std::getline(input, line);
    if (!input && line.empty()) {
      throw std::runtime_error("rpc frame file was empty");
    }

    const auto decoded = qt::app::decode_rpc_frame_line(line);
    if (!decoded) {
      if (emit_json) {
        std::cout << Json{{"path", path},
                          {"ok", false},
                          {"error",
                           {{"code",
                             qt::foundation::to_string(decoded.error().code)},
                            {"message", decoded.error().message},
                            {"system_error", decoded.error().system_error}}}}
                         .dump(2)
                  << '\n';
      } else {
        std::cerr << "qt: rpc-frame failed: " << decoded.error().summary()
                  << '\n';
      }
      return EXIT_FAILURE;
    }

    std::string frame_kind;
    Json payload = Json::object();
    std::visit(
        [&](const auto& value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, qt::app::RpcCommand>) {
            frame_kind = "command";
            payload = std::visit([](const auto& command) { return Json(command); },
                                 value);
          } else if constexpr (std::is_same_v<Value, qt::app::RpcResponse>) {
            frame_kind = "response";
            payload = std::visit([](const auto& response) { return Json(response); },
                                 value);
          } else if constexpr (std::is_same_v<Value,
                                               qt::app::ExtensionUiResponseFrame>) {
            frame_kind = "extension_ui_response";
            payload = Json(value);
          } else {
            frame_kind = "event";
            payload = std::visit([](const auto& event) { return Json(event); },
                                 value);
          }
        },
        decoded.value().payload);

    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", true},
                        {"kind", frame_kind},
                        {"frame", std::move(payload)}}
                       .dump(2)
                << '\n';
      return EXIT_SUCCESS;
    }

    std::cout << "path: " << path << '\n';
    std::cout << "ok: true\n";
    std::cout << "kind: " << frame_kind << '\n';
    std::cout << payload.dump(2) << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    if (emit_json) {
      std::cout << Json{{"path", path},
                        {"ok", false},
                        {"error", error.what()}}
                       .dump(2)
                << '\n';
    } else {
      std::cerr << "qt: rpc-frame failed: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
  }
}

int run_read_text_mode(bool emit_json, const std::string& path,
                       const std::string& cwd,
                       const std::string& workspace_root) {
  auto registry = make_builtin_tool_registry();
  const auto tool = require_registered_tool(registry, "read_text");
  const auto context = make_tool_context(cwd, workspace_root);

  qt::tools::ToolRequest request;
  request.tool_name = "read_text";
  request.arguments = Json{{"path", path}};
  request.call_id = "cli.read_text";

  const auto result = tool->invoke(request, context);
  if (!qt::tools::is_success(result)) {
    return print_tool_error_or_json(emit_json, request.tool_name, result);
  }

  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "path: " << result.output.value("path", "") << '\n';
  std::cout << "bytes: " << result.output.value("bytes", 0) << '\n';
  std::cout << '\n';
  std::cout << result.output.value("text", "") << '\n';
  return EXIT_SUCCESS;
}

int run_list_directory_mode(bool emit_json, const std::string& path,
                            const std::string& cwd,
                            const std::string& workspace_root) {
  auto registry = make_builtin_tool_registry();
  const auto tool = require_registered_tool(registry, "list_directory");
  const auto context = make_tool_context(cwd, workspace_root);

  qt::tools::ToolRequest request;
  request.tool_name = "list_directory";
  request.arguments = Json{{"path", path}};
  request.call_id = "cli.list_directory";

  const auto result = tool->invoke(request, context);
  if (!qt::tools::is_success(result)) {
    return print_tool_error_or_json(emit_json, request.tool_name, result);
  }

  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "path: " << result.output.value("path", "") << '\n';
  std::cout << "entries: " << result.output.value("count", 0) << '\n';
  const auto& entries = result.output["entries"];
  if (entries.empty()) {
    std::cout << "  (empty)\n";
    return EXIT_SUCCESS;
  }

  for (const auto& entry : entries) {
    std::cout << "  - [" << entry.value("kind", "other") << "] "
              << entry.value("name", "") << '\n';
  }
  return EXIT_SUCCESS;
}

int run_complete_path_mode(bool emit_json, const std::string& path,
                           const std::string& cwd,
                           const std::string& workspace_root) {
  auto registry = make_builtin_tool_registry();
  const auto tool = require_registered_tool(registry, "complete_path");
  const auto context = make_tool_context(cwd, workspace_root);

  qt::tools::ToolRequest request;
  request.tool_name = "complete_path";
  request.arguments = Json{{"path", path}};
  request.call_id = "cli.complete_path";

  const auto result = tool->invoke(request, context);
  if (!qt::tools::is_success(result)) {
    return print_tool_error_or_json(emit_json, request.tool_name, result);
  }

  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "prefix: " << result.output.value("prefix", "") << '\n';
  std::cout << "parent: " << result.output.value("parent", "") << '\n';
  std::cout << "matches: " << result.output.value("count", 0) << '\n';
  const auto& matches = result.output["matches"];
  if (matches.empty()) {
    std::cout << "  (empty)\n";
    return EXIT_SUCCESS;
  }

  for (const auto& match : matches) {
    std::cout << "  - [" << match.value("kind", "other") << "] "
              << match.value("completion", "") << '\n';
  }
  return EXIT_SUCCESS;
}

int run_text_mutation_mode(bool emit_json, const std::string& tool_name,
                           const std::string& path, const std::string& text,
                           const std::string& cwd,
                           const std::string& workspace_root) {
  auto registry = make_builtin_tool_registry();
  const auto tool = require_registered_tool(registry, tool_name);
  const auto context = make_tool_context(cwd, workspace_root);

  qt::tools::ToolRequest request;
  request.tool_name = tool_name;
  request.arguments = Json{{"path", path}, {"text", text}};
  request.call_id = "cli." + tool_name;

  const auto result = tool->invoke(request, context);
  if (!qt::tools::is_success(result)) {
    return print_tool_error_or_json(emit_json, request.tool_name, result);
  }

  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "path: " << result.output.value("path", "") << '\n';
  std::cout << "bytes: " << result.output.value("bytes", 0) << '\n';
  if (tool_name == "write_text") {
    std::cout << "written: "
              << (result.output.value("written", false) ? "true" : "false")
              << '\n';
  } else if (tool_name == "append_text") {
    std::cout << "appended: "
              << (result.output.value("appended", false) ? "true" : "false")
              << '\n';
  }
  return EXIT_SUCCESS;
}

int run_edit_mode(bool emit_json, const std::string& path,
                  const std::string& old_text, const std::string& new_text,
                  const std::string& cwd,
                  const std::string& workspace_root) {
  auto registry = make_builtin_tool_registry();
  const auto tool = require_registered_tool(registry, "edit");
  const auto context = make_tool_context(cwd, workspace_root);

  qt::tools::ToolRequest request;
  request.tool_name = "edit";
  request.arguments = Json{
      {"path", path},
      {"oldText", old_text},
      {"newText", new_text},
  };
  request.call_id = "cli.edit";

  const auto result = tool->invoke(request, context);
  if (!qt::tools::is_success(result)) {
    return print_tool_error_or_json(emit_json, request.tool_name, result);
  }

  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "path: " << result.output.value("path", "") << '\n';
  std::cout << "replaced: "
            << (result.output.value("replaced", false) ? "true" : "false")
            << '\n';
  std::cout << "written: "
            << (result.output.value("written", false) ? "true" : "false")
            << '\n';
  std::cout << "match_count: " << result.output.value("match_count", 0) << '\n';
  std::cout << "bytes: " << result.output.value("bytes", 0) << '\n';
  return EXIT_SUCCESS;
}

int run_bash_mode(bool emit_json, const std::string& command,
                  const std::string& cwd,
                  const std::string& workspace_root) {
  auto registry = make_builtin_tool_registry();
  const auto tool = require_registered_tool(registry, "bash");
  const auto context = make_tool_context(cwd, workspace_root);

  qt::tools::ToolRequest request;
  request.tool_name = "bash";
  request.arguments = Json{{"command", command}};
  request.call_id = "cli.bash";

  const auto result = tool->invoke(request, context);
  if (!qt::tools::is_success(result)) {
    return print_tool_error_or_json(emit_json, request.tool_name, result);
  }

  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "cwd: " << result.output.value("cwd", "") << '\n';
  std::cout << "exit_code: " << result.output.value("exit_code", 0) << '\n';
  std::cout << "stdout:\n" << result.output.value("stdout", "") << '\n';
  std::cout << "stderr:\n" << result.output.value("stderr", "") << '\n';
  return EXIT_SUCCESS;
}

int run_search_text_mode(bool emit_json, const std::string& path,
                         const std::string& query, const std::string& cwd,
                         const std::string& workspace_root) {
  auto registry = make_builtin_tool_registry();
  const auto tool = require_registered_tool(registry, "search_text");
  const auto context = make_tool_context(cwd, workspace_root);

  qt::tools::ToolRequest request;
  request.tool_name = "search_text";
  request.arguments = Json{{"path", path}, {"query", query}};
  request.call_id = "cli.search_text";

  const auto result = tool->invoke(request, context);
  if (!qt::tools::is_success(result)) {
    return print_tool_error_or_json(emit_json, request.tool_name, result);
  }

  if (emit_json) {
    std::cout << to_json(result).dump(2) << '\n';
    return EXIT_SUCCESS;
  }

  std::cout << "path: " << result.output.value("path", "") << '\n';
  std::cout << "query: " << result.output.value("query", "") << '\n';
  std::cout << "recursive: "
            << (result.output.value("recursive", false) ? "true" : "false")
            << '\n';
  std::cout << "files_searched: " << result.output.value("files_searched", 0)
            << '\n';
  const auto& matches = result.output["matches"];
  if (matches.empty()) {
    std::cout << "matches: 0\n";
    return EXIT_SUCCESS;
  }

  std::cout << "matches: " << matches.size() << '\n';
  for (const auto& match : matches) {
    std::cout << "  - " << match.value("path", "") << " ("
              << match.value("match_count", 0) << ")\n";
  }
  return EXIT_SUCCESS;
}

std::vector<qt::ai::ToolDefinition> build_tool_definitions(
    const qt::tools::ToolRegistry& registry) {
  std::vector<qt::ai::ToolDefinition> definitions;
  for (const auto& descriptor : registry.descriptors()) {
    definitions.push_back(qt::ai::ToolDefinition{
        descriptor.name,
        descriptor.description,
        descriptor.input_schema,
    });
  }
  return definitions;
}

Json make_demo_agent_payload(const qt::agent::Agent& agent,
                             const qt::agent::TurnResult& result,
                             const qt::agent::VectorEventSink& sink) {
  Json transcript = Json::array();
  for (const auto& message : agent.state().transcript) {
    transcript.push_back(to_json(message));
  }

  Json events = Json::array();
  for (const auto& event : sink.events()) {
    events.push_back(to_json(event));
  }

  Json tool_requests = Json::array();
  for (const auto& request : result.tool_requests) {
    tool_requests.push_back(to_json(request));
  }

  Json tool_results = Json::array();
  for (const auto& tool_result : result.tool_results) {
    tool_results.push_back(to_json(tool_result));
  }

  Json provider_events = Json::array();
  for (const auto& provider_event : result.provider_events) {
    provider_events.push_back(to_json(provider_event));
  }

  Json requests = Json::array();
  for (const auto& request : result.requests) {
    requests.push_back(to_json(request));
  }

  Json responses = Json::array();
  for (const auto& response : result.responses) {
    responses.push_back(to_json(response));
  }

  Json payload{
      {"ok", result.ok},
      {"turn_index", result.turn_index},
      {"committed_sequences", result.committed_sequences},
      {"requests", std::move(requests)},
      {"responses", std::move(responses)},
      {"tool_requests", std::move(tool_requests)},
      {"tool_results", std::move(tool_results)},
      {"provider_events", std::move(provider_events)},
      {"events", std::move(events)},
      {"transcript", std::move(transcript)},
      {"completed_turns", agent.state().completed_turns},
      {"completed_tool_calls", agent.state().completed_tool_calls},
  };
  if (result.request) {
    payload["request"] = to_json(*result.request);
  }
  if (result.response) {
    payload["response"] = to_json(*result.response);
  }
  if (result.error) {
    payload["error"] = *result.error;
  }
  return payload;
}

int run_demo_agent_mode(bool emit_json, const std::string& output_path,
                        const std::string& output_text, const std::string& cwd,
                        const std::string& workspace_root) {
  auto registry =
      std::make_shared<qt::tools::ToolRegistry>(make_builtin_tool_registry());

  qt::agent::AgentConfig config;
  config.model = qt::ai::ModelRef{"demo", "demo", "demo-agent"};
  config.tools = build_tool_definitions(*registry);
  config.tool_choice = qt::ai::ToolChoice{qt::ai::ToolChoiceKind::auto_select,
                                          std::nullopt};
  config.temperature = 0.0;
  config.max_output_tokens = 64;
  config.metadata = Json{{"mode", "demo-agent"}};
  config.tool_registry = registry;
  config.tool_context = make_tool_context(cwd, workspace_root);

  auto provider = std::make_shared<DemoProvider>(output_path, output_text);
  qt::agent::Agent agent(std::move(config), provider);
  qt::agent::VectorEventSink sink;

  qt::ai::Message user_message;
  user_message.role = qt::ai::MessageRole::user;
  user_message.content = {
      qt::ai::TextContent{"Write the demo output file.", std::nullopt}};
  user_message.metadata = Json{{"mode", "demo-agent"}};

  agent.enqueue(std::move(user_message), &sink);
  const auto result = agent.run_turn(sink);
  if (emit_json) {
    std::cout << make_demo_agent_payload(agent, result, sink).dump(2) << '\n';
  } else {
    std::cout << "turn: " << result.turn_index << '\n';
    std::cout << "ok: " << (result.ok ? "true" : "false") << '\n';
    std::cout << "provider_completions: " << result.responses.size() << '\n';
    if (result.response) {
      std::cout << "stop_reason: "
                << qt::ai::to_string(result.response->stop_reason) << '\n';
    }
    std::cout << "tool_calls: " << result.tool_results.size() << '\n';
    for (const auto& tool_result : result.tool_results) {
      std::cout << "  - status: " << qt::tools::to_string(tool_result.status);
      if (tool_result.output.contains("path")) {
        std::cout << ", path: " << tool_result.output.at("path").get<std::string>();
      }
      if (tool_result.output.contains("bytes")) {
        std::cout << ", bytes: " << tool_result.output.at("bytes").get<std::size_t>();
      }
      if (tool_result.error_message) {
        std::cout << ", error: " << *tool_result.error_message;
      }
      std::cout << '\n';
    }
    std::cout << "transcript_messages: " << agent.state().transcript.size() << '\n';
  }

  return result.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int run_auxiliary_mode(const ReferenceAuxiliaryModeArgs& args) {
  const bool has_inspect_session = !args.inspect_session_path.empty();
  const bool has_resume_session = !args.resume_session_path.empty();
  const bool has_inspect_session_catalog = !args.inspect_session_catalog_path.empty();
  const bool has_create_session = !args.create_session_path.empty();
  const bool has_fork_session = !args.fork_session_path.empty();
  const bool has_clone_session = !args.clone_session_path.empty();
  const bool has_compact_session = !args.compact_session_path.empty();
  const bool has_rpc_envelope = !args.rpc_envelope_path.empty();
  const bool has_rpc_frame = !args.rpc_frame_path.empty();
  const bool has_read_text = !args.read_text_path.empty();
  const bool has_list_directory = !args.list_directory_path.empty();
  const bool has_complete_path = !args.complete_path_prefix.empty();
  const bool has_search_text = !args.search_text_path.empty();
  const bool has_write_text = !args.write_text_path.empty();
  const bool has_append_text = !args.append_text_path.empty();
  const bool has_edit = !args.edit_path.empty();
  const bool has_bash = !args.bash_command.empty();
  const bool has_demo_agent_write = !args.demo_agent_write_path.empty();
  const int selected_modes =
      (args.inspect_context ? 1 : 0) + (args.inspect_context_resources ? 1 : 0) +
      (args.inspect_prompt_assembly ? 1 : 0) +
      (args.inspect_skills ? 1 : 0) + (args.inspect_prompt_templates ? 1 : 0) +
      (args.inspect_slash_commands ? 1 : 0) +
      (args.inspect_settings ? 1 : 0) + (args.inspect_app_resources ? 1 : 0) +
      (args.inspect_system_prompt ? 1 : 0) +
      (args.inspect_extensions ? 1 : 0) +
      (has_inspect_session ? 1 : 0) + (has_resume_session ? 1 : 0) +
      (has_inspect_session_catalog ? 1 : 0) + (has_create_session ? 1 : 0) +
      (has_fork_session ? 1 : 0) + (has_clone_session ? 1 : 0) +
      (has_compact_session ? 1 : 0) +
      (args.inspect_extension_runners ? 1 : 0) + (has_rpc_envelope ? 1 : 0) +
      (has_rpc_frame ? 1 : 0) +
      (has_read_text ? 1 : 0) + (has_list_directory ? 1 : 0) +
      (has_complete_path ? 1 : 0) + (has_search_text ? 1 : 0) +
      (has_write_text ? 1 : 0) + (has_append_text ? 1 : 0) +
      (has_edit ? 1 : 0) + (has_bash ? 1 : 0) + (has_demo_agent_write ? 1 : 0);
  if (selected_modes != 1) {
    std::cerr
        << "qt: select exactly one mode: --inspect-context, "
           "--inspect-context-resources, --inspect-prompt-assembly, "
           "--inspect-skills, --inspect-prompt-templates, "
           "--inspect-slash-commands, --inspect-settings, "
           "--inspect-app-resources, "
           "--inspect-system-prompt, --inspect-extensions, "
           "--inspect-extension-runners, --inspect-session PATH, "
           "--resume-session PATH, --inspect-session-catalog PATH, "
           "--create-session PATH, --fork-session PATH, --clone-session PATH, "
           "--compact-session PATH, --rpc-envelope PATH, --rpc-frame PATH, --read-text PATH, "
           "--list-directory PATH, --complete-path PATH, --search-text PATH, --write-text PATH, "
           "--append-text PATH, "
           "--edit PATH, --bash COMMAND, or --demo-agent-write PATH\n";
    std::cerr << args.usage_text;
    return EXIT_FAILURE;
  }

  if (args.inspect_context) {
    return run_context_mode(args.emit_json, args.cwd, args.agent_home,
                            args.repository_root_hint);
  }
  if (args.inspect_context_resources) {
    return run_context_resources_mode(args.emit_json, args.cwd, args.agent_home,
                                      args.repository_root_hint);
  }
  if (args.inspect_prompt_assembly) {
    return run_prompt_assembly_mode(args.emit_json, args.cwd, args.agent_home,
                                    args.repository_root_hint);
  }
  if (args.inspect_skills) {
    return run_skills_mode(args.emit_json, args.cwd, args.agent_home,
                           args.repository_root_hint);
  }
  if (args.inspect_prompt_templates) {
    return run_prompt_templates_mode(args.emit_json, args.cwd, args.agent_home,
                                     args.repository_root_hint);
  }
  if (args.inspect_slash_commands) {
    const qt::app::LoadReferenceExtensionOptions extension_options{
        !args.no_extensions};
    return run_slash_commands_mode(args.emit_json, args.cwd, args.agent_home,
                                   args.repository_root_hint, args.extension_args,
                                   extension_options);
  }
  if (args.inspect_settings) {
    return run_settings_mode(args.emit_json, args.cwd, args.agent_home,
                             args.repository_root_hint);
  }
  if (args.inspect_app_resources) {
    return run_app_resources_mode(args.emit_json, args.cwd, args.agent_home,
                                  args.repository_root_hint);
  }
  if (args.inspect_system_prompt) {
    return run_system_prompt_mode(args.emit_json, args.cwd, args.agent_home,
                                  args.repository_root_hint, args.custom_guidance);
  }
  if (args.inspect_extensions) {
    const qt::app::LoadReferenceExtensionOptions extension_options{
        !args.no_extensions};
    return run_extensions_mode(args.emit_json, args.cwd, args.agent_home,
                               args.repository_root_hint, args.extension_args,
                               extension_options);
  }
  if (args.inspect_extension_runners) {
    const qt::app::LoadReferenceExtensionOptions extension_options{
        !args.no_extensions};
    return run_extension_runners_mode(args.emit_json, args.cwd, args.agent_home,
                                      args.repository_root_hint, args.extension_args,
                                      extension_options);
  }
  if (has_inspect_session) {
    return run_inspect_session_mode(args.emit_json, args.inspect_session_path);
  }
  if (has_resume_session) {
    return run_resume_session_mode(args.emit_json, args.resume_session_path);
  }
  if (has_inspect_session_catalog) {
    return run_session_catalog_mode(args.emit_json,
                                    args.inspect_session_catalog_path);
  }
  if (has_create_session) {
    return run_create_session_mode(args.emit_json, args.create_session_path,
                                   args.session_id, args.cwd, args.session_name,
                                   args.created_at);
  }
  if (has_fork_session) {
    return run_fork_session_mode(args.emit_json, args.fork_session_path,
                                 args.child_session_path, args.session_id,
                                 args.child_working_directory, args.session_name,
                                 args.created_at);
  }
  if (has_clone_session) {
    return run_clone_session_mode(args.emit_json, args.clone_session_path,
                                  args.child_session_path, args.session_id);
  }
  if (has_compact_session) {
    return run_compact_session_mode(args.emit_json, args.compact_session_path,
                                    args.apply_session_compaction);
  }
  if (has_rpc_envelope) {
    return run_rpc_envelope_mode(args.emit_json, args.rpc_envelope_path);
  }
  if (has_rpc_frame) {
    return run_rpc_frame_mode(args.emit_json, args.rpc_frame_path);
  }
  if (has_list_directory) {
    return run_list_directory_mode(args.emit_json, args.list_directory_path,
                                   args.cwd, args.workspace_root);
  }
  if (has_complete_path) {
    return run_complete_path_mode(args.emit_json, args.complete_path_prefix,
                                  args.cwd, args.workspace_root);
  }
  if (has_search_text) {
    return run_search_text_mode(args.emit_json, args.search_text_path, args.query,
                                args.cwd, args.workspace_root);
  }
  if (has_write_text) {
    return run_text_mutation_mode(args.emit_json, "write_text",
                                  args.write_text_path, args.text, args.cwd,
                                  args.workspace_root);
  }
  if (has_append_text) {
    return run_text_mutation_mode(args.emit_json, "append_text",
                                  args.append_text_path, args.text, args.cwd,
                                  args.workspace_root);
  }
  if (has_edit) {
    return run_edit_mode(args.emit_json, args.edit_path, args.old_text,
                         args.new_text, args.cwd, args.workspace_root);
  }
  if (has_bash) {
    return run_bash_mode(args.emit_json, args.bash_command, args.cwd,
                         args.workspace_root);
  }
  if (has_demo_agent_write) {
    return run_demo_agent_mode(args.emit_json, args.demo_agent_write_path,
                               args.demo_text, args.cwd, args.workspace_root);
  }
  return run_read_text_mode(args.emit_json, args.read_text_path, args.cwd,
                            args.workspace_root);
}

}  // namespace qt::app
