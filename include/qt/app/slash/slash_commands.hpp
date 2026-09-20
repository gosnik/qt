#pragma once

#include <qt/foundation/error.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <qt/extensions/loader.hpp>
#include <qt/harness/prompt_templates.hpp>
#include <qt/harness/skills.hpp>

namespace qt::app {

enum class SlashCommandSource {
  kBuiltin,
  kExtension,
  kPrompt,
  kSkill,
};

struct SlashCommand {
  std::string name;
  std::string target_name;
  std::string description;
  SlashCommandSource source = SlashCommandSource::kBuiltin;
  std::string source_path;
  std::optional<std::string> argument_hint;
  std::string expansion_template;
};

struct SlashCommandRegistry {
  std::vector<SlashCommand> commands;
  std::vector<std::string> diagnostics;
};

struct SlashCommandRegistryOptions {
  bool enable_skill_commands = true;
};

enum class SlashCommandExecutionKind {
  kBuiltin,
  kExtension,
  kPromptExpansion,
  kSkillExpansion,
};

enum class SlashCommandRuntimeOperationKind {
  kCompactSessionContext,
  kNavigateSessionTree,
  kExportSessionHtml,
  kExportSessionJsonl,
  kShareSessionGist,
  kOpenSettings,
  kOpenPackageResourceConfig,
  kSelectModel,
  kSelectScopedModels,
  kImportSessionJsonl,
  kCopyLastAssistantText,
  kSetSessionName,
  kShowSessionInfo,
  kShowContextInfo,
  kShowChangelog,
  kShowHotkeys,
  kForkSessionFromUserMessage,
  kCloneSession,
  kOpenLoginSelector,
  kOpenLogoutSelector,
  kStartNewSession,
  kResumeSession,
  kReloadRuntimeResources,
  kShowRuntimeStatus,
  kOpenToolsSelector,
  kSetThinkingLevel,
  kOpenPackageResourceSelector,
  kExecuteExtensionCommand,
  kLlamaProviderOperation,
  kOllamaProviderOperation,
  kSubagentOperation,
  kIndexOperation,
  kMemoryOperation,
  kMcpOperation,
  kQuitApplication,
  kTogglePlanMode,
  kGoalOperation,
};

struct SlashCommandInvocation {
  std::string name;
  std::string arguments;
};

struct SlashCommandRuntimeOperation {
  SlashCommandRuntimeOperationKind kind =
      SlashCommandRuntimeOperationKind::kCompactSessionContext;
  std::optional<std::string> custom_instructions;
  std::optional<std::string> target_path;
  std::optional<std::string> value;
  std::vector<std::string> steps;
};

struct SlashCommandExecution {
  SlashCommandExecutionKind kind = SlashCommandExecutionKind::kBuiltin;
  SlashCommand command;
  std::optional<SlashCommandRuntimeOperation> runtime_operation;
  std::string expanded_prompt;
  std::string output;
};

std::string to_string(SlashCommandSource source);
std::string to_string(SlashCommandExecutionKind kind);
std::string to_string(SlashCommandRuntimeOperationKind kind);

const std::vector<SlashCommand>& builtin_slash_commands();

SlashCommandRegistry build_slash_command_registry(
    const qt::harness::PromptTemplateResources& prompt_templates,
    const qt::harness::SkillResources& skills,
    const qt::extensions::ExtensionLoadResult& extensions,
    SlashCommandRegistryOptions options = {});

std::vector<SlashCommand> filter_slash_commands_by_prefix(
    const SlashCommandRegistry& registry, std::string_view prefix);

std::optional<SlashCommand> find_slash_command(
    const SlashCommandRegistry& registry, std::string_view name);

qt::foundation::Result<SlashCommandInvocation> parse_slash_command_invocation(
    std::string_view input);

qt::foundation::Result<SlashCommandExecution> execute_slash_command(
    const SlashCommandRegistry& registry, std::string_view input);

}  // namespace qt::app
