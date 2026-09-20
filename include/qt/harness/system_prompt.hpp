#pragma once

#include <optional>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <qt/harness/prompt_assembly.hpp>
#include <qt/harness/skills.hpp>

namespace qt::harness {

struct SystemPromptInput {
  std::vector<PromptSection> sections;
  std::vector<std::string> diagnostics;
  std::optional<std::string> custom_prompt_override;
  std::optional<std::string> custom_guidance;
  std::optional<std::string> append_system_prompt;
  std::vector<std::string> prompt_guidelines;
  std::vector<LoadedSkillRecord> skills;
  std::optional<std::string> working_directory;
  std::optional<std::string> workspace_root;
  std::optional<std::string> shell;
  std::optional<std::string> current_date;
  std::optional<std::string> timezone;
  std::optional<std::string> network_access;
  std::optional<std::string> tool_permissions;
  std::optional<std::size_t> active_tool_count;
  bool plan_mode_active = false;
  std::optional<std::string> active_goal_objective;
  std::optional<std::int64_t> goal_tokens_used;
  std::optional<std::int64_t> goal_token_budget;
  std::optional<std::int64_t> goal_time_used_seconds;

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

struct SystemPrompt {
  std::string prompt;
  std::vector<std::string> diagnostics;

  bool empty() const noexcept { return prompt.empty(); }
};

SystemPromptInput make_system_prompt_input(const PromptAssembly& assembly);

SystemPrompt build_system_prompt(const SystemPromptInput& input);

}  // namespace qt::harness
