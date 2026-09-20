#include <qt/harness/system_prompt.hpp>

#include <sstream>
#include <string_view>

namespace qt::harness {

namespace {

void append_line(std::ostringstream& out, const std::string& line) {
  out << line << '\n';
}

std::string escape_xml(std::string_view value);

void append_default_core_guidance(std::ostringstream& out) {
  append_line(out, "You are qt, a repository-aware coding harness.");
  append_line(out,
              "Follow the user's request and any repository instructions "
              "provided below.");
  append_line(out,
              "Prefer precise, minimal, deterministic changes that preserve "
              "existing behavior unless the task requires otherwise.");
  append_line(out,
              "Repository instructions are ordered by precedence: global, "
              "then ancestor, then current_directory.");
  append_line(out,
              "Preserve the listed order within each precedence scope, and "
              "apply any custom guidance after all repository instructions.");
}

void append_prompt_guidelines(std::ostringstream& out,
                              const std::vector<std::string>& guidelines) {
  if (guidelines.empty()) {
    return;
  }

  append_line(out, "");
  append_line(out, "Additional prompt guidelines:");
  for (const auto& guideline : guidelines) {
    if (!guideline.empty()) {
      append_line(out, "- " + guideline);
    }
  }
}

void append_runtime_metadata(std::ostringstream& out,
                             const SystemPromptInput& input) {
  const bool has_context =
      (input.working_directory.has_value() && !input.working_directory->empty()) ||
      (input.workspace_root.has_value() && !input.workspace_root->empty()) ||
      (input.shell.has_value() && !input.shell->empty()) ||
      (input.current_date.has_value() && !input.current_date->empty()) ||
      (input.timezone.has_value() && !input.timezone->empty()) ||
      (input.network_access.has_value() && !input.network_access->empty()) ||
      (input.tool_permissions.has_value() && !input.tool_permissions->empty()) ||
      input.active_tool_count.has_value();
  if (!has_context) {
    return;
  }

  append_line(out, "# Environment Context");
  append_line(out, "");
  append_line(out, "<environment_context>");
  if (input.working_directory.has_value() && !input.working_directory->empty()) {
    append_line(out, "  <cwd>" + escape_xml(*input.working_directory) + "</cwd>");
  }
  if (input.workspace_root.has_value() && !input.workspace_root->empty()) {
    append_line(out, "  <workspace_root>" + escape_xml(*input.workspace_root) +
                         "</workspace_root>");
  }
  if (input.shell.has_value() && !input.shell->empty()) {
    append_line(out, "  <shell>" + escape_xml(*input.shell) + "</shell>");
  }
  if (input.current_date.has_value() && !input.current_date->empty()) {
    append_line(out, "  <current_date>" + escape_xml(*input.current_date) +
                         "</current_date>");
  }
  if (input.timezone.has_value() && !input.timezone->empty()) {
    append_line(out, "  <timezone>" + escape_xml(*input.timezone) +
                         "</timezone>");
  }
  if (input.network_access.has_value() && !input.network_access->empty()) {
    append_line(out, "  <network>" + escape_xml(*input.network_access) +
                         "</network>");
  }
  if (input.tool_permissions.has_value() && !input.tool_permissions->empty()) {
    append_line(out, "  <tool_permissions>" +
                         escape_xml(*input.tool_permissions) +
                         "</tool_permissions>");
  }
  if (input.active_tool_count.has_value()) {
    append_line(out, "  <active_tools>" +
                         std::to_string(*input.active_tool_count) +
                         "</active_tools>");
  }
  append_line(out, "</environment_context>");
}

void append_context_sections(std::ostringstream& out,
                             const std::vector<PromptSection>& sections) {
  if (sections.empty()) {
    return;
  }

  append_line(out, "");
  append_line(out, "# Project Context");
  append_line(out, "");
  append_line(out, "Project-specific instructions and guidelines:");
  for (const auto& section : sections) {
    append_line(out, "");
    out << "## " << section.path << '\n';
    if (section.content.empty()) {
      append_line(out, "(empty context file)");
      continue;
    }
    append_line(out, section.content);
  }
}

void append_custom_guidance(std::ostringstream& out,
                            const std::optional<std::string>& custom_guidance) {
  if (!custom_guidance.has_value() || custom_guidance->empty()) {
    return;
  }

  append_line(out, "");
  append_line(out,
              "Additional guidance (applies after repository instructions):");
  append_line(out, *custom_guidance);
}

void append_system_prompt_tail(
    std::ostringstream& out,
    const std::optional<std::string>& append_system_prompt) {
  if (!append_system_prompt.has_value() || append_system_prompt->empty()) {
    return;
  }

  append_line(out, "");
  append_line(out, *append_system_prompt);
}

std::string escape_xml(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&apos;";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

void append_goal_continuation(std::ostringstream& out,
                               const SystemPromptInput& input) {
  if (!input.active_goal_objective.has_value() ||
      input.active_goal_objective->empty()) {
    return;
  }

  append_line(out, "");
  append_line(out, "# Active Goal");
  append_line(out, "");
  append_line(out,
              "You are working on the following goal. Continue making progress "
              "toward completing it.");
  append_line(out, "");
  append_line(out, "<active_goal>");
  append_line(out, "  <objective>" + escape_xml(*input.active_goal_objective) +
                         "</objective>");

  if (input.goal_tokens_used.has_value()) {
    append_line(out, "  <tokens_used>" +
                         std::to_string(*input.goal_tokens_used) +
                         "</tokens_used>");
  }
  if (input.goal_token_budget.has_value()) {
    append_line(out, "  <token_budget>" +
                         std::to_string(*input.goal_token_budget) +
                         "</token_budget>");
    if (input.goal_tokens_used.has_value()) {
      const auto remaining =
          (*input.goal_token_budget - *input.goal_tokens_used);
      append_line(out, "  <remaining_tokens>" +
                           std::to_string(remaining > 0 ? remaining : 0) +
                           "</remaining_tokens>");
    }
  }
  if (input.goal_time_used_seconds.has_value()) {
    append_line(out, "  <time_used_seconds>" +
                         std::to_string(*input.goal_time_used_seconds) +
                         "</time_used_seconds>");
  }

  append_line(out, "</active_goal>");
  append_line(out, "");
  append_line(out,
              "Work autonomously toward the goal. Use non-mutating exploration "
              "first, then make targeted changes. Report progress and ask for "
              "guidance only when truly blocked.");
}

void append_plan_mode_instructions(std::ostringstream& out) {
  append_line(out, "");
  append_line(out, "# Plan Mode (Active)");
  append_line(out, "");
  append_line(out,
              "You are in Plan Mode. Work in 3 phases and chat your way to a "
              "detailed plan before finalizing it.");
  append_line(out,
              "A great plan is decision complete: the implementer does not need "
              "to make any decisions.");
  append_line(out, "");
  append_line(out, "## Mode rules (strict)");
  append_line(out, "");
  append_line(out,
              "You are in Plan Mode until the user explicitly switches to "
              "Default mode.");
  append_line(out,
              "Plan Mode is not changed by user intent, tone, or imperative "
              "language. If a user asks for execution while still in Plan Mode, "
              "treat it as a request to plan the execution, not perform it.");
  append_line(out, "");
  append_line(out, "## Allowed (non-mutating, plan-improving)");
  append_line(out, "");
  append_line(out,
              "Actions that gather truth, reduce ambiguity, or validate "
              "feasibility without changing repo-tracked state:");
  append_line(out,
              "- Reading or searching files, configs, schemas, types, "
              "manifests, and docs");
  append_line(out,
              "- Static analysis, inspection, and repo exploration");
  append_line(out,
              "- Dry-run style commands when they do not edit repo-tracked "
              "files");
  append_line(out,
              "- Tests, builds, or checks that may write to caches or build "
              "artifacts so long as they do not edit repo-tracked files");
  append_line(out, "");
  append_line(out, "## Not allowed (mutating, plan-executing)");
  append_line(out, "");
  append_line(out,
              "Actions that implement the plan or change repo-tracked state:");
  append_line(out, "- Editing or writing files");
  append_line(out, "- Running formatters or linters that rewrite files");
  append_line(out,
              "- Applying patches, migrations, or codegen that updates "
              "repo-tracked files");
  append_line(out,
              "- Side-effectful commands whose purpose is to carry out the "
              "plan rather than refine it");
  append_line(out, "");
  append_line(out, "## PHASE 1 - Ground in the environment");
  append_line(out, "");
  append_line(out,
              "Begin by exploring the environment. Eliminate unknowns by "
              "discovering facts, not by asking the user. Perform at least one "
              "targeted non-mutating exploration pass before asking questions.");
  append_line(out, "");
  append_line(out, "## PHASE 2 - Intent chat");
  append_line(out, "");
  append_line(out,
              "Keep asking until you can clearly state: goal + success "
              "criteria, audience, in/out of scope, constraints, current state, "
              "and key preferences/tradeoffs.");
  append_line(out, "");
  append_line(out, "## PHASE 3 - Implementation chat");
  append_line(out, "");
  append_line(out,
              "Once intent is stable, keep asking until the spec is decision "
              "complete: approach, interfaces, data flow, edge cases, testing, "
              "and any migration/compat constraints.");
  append_line(out, "");
  append_line(out, "## Finalization");
  append_line(out, "");
  append_line(out,
              "Only output the final plan when it is decision complete. Wrap "
              "it in a <proposed_plan> block:");
  append_line(out, "");
  append_line(out, "<proposed_plan>");
  append_line(out, "plan content in markdown");
  append_line(out, "</proposed_plan>");
  append_line(out, "");
  append_line(out,
              "The plan must include: a clear title, brief summary, key "
              "changes, test plan, and assumptions.");
  append_line(out,
              "Do not ask 'should I proceed?' in the final output. The user "
              "can switch out of Plan mode to request implementation.");
  append_line(out,
              "Only produce at most one <proposed_plan> block per turn.");
}

void append_skills(std::ostringstream& out,
                   const std::vector<LoadedSkillRecord>& skills) {
  bool has_visible_skill = false;
  for (const auto& skill : skills) {
    if (!skill.disable_model_invocation) {
      has_visible_skill = true;
      break;
    }
  }
  if (!has_visible_skill) {
    return;
  }

  append_line(out, "");
  append_line(out, "");
  append_line(out,
              "The following skills provide specialized instructions for "
              "specific tasks.");
  append_line(out,
              "Use the read tool to load a skill's file when the task matches "
              "its description.");
  append_line(out,
              "When a skill file references a relative path, resolve it "
              "against the skill directory (parent of SKILL.md / dirname of "
              "the path) and use that absolute path in tool commands.");
  append_line(out, "");
  append_line(out, "<available_skills>");
  for (const auto& skill : skills) {
    if (skill.disable_model_invocation) {
      continue;
    }
    append_line(out, "  <skill>");
    append_line(out, "    <name>" + escape_xml(skill.name) + "</name>");
    append_line(out, "    <description>" + escape_xml(skill.description) +
                         "</description>");
    append_line(out, "    <location>" + escape_xml(skill.path) + "</location>");
    append_line(out, "  </skill>");
  }
  append_line(out, "</available_skills>");
}

}  // namespace

SystemPromptInput make_system_prompt_input(const PromptAssembly& assembly) {
  SystemPromptInput input;
  input.sections = assembly.sections;
  input.diagnostics = assembly.diagnostics;
  return input;
}

SystemPrompt build_system_prompt(const SystemPromptInput& input) {
  SystemPrompt result;
  result.diagnostics = input.diagnostics;

  std::ostringstream out;
  if (input.custom_prompt_override.has_value() &&
      !input.custom_prompt_override->empty()) {
    append_line(out, *input.custom_prompt_override);
  } else {
    append_default_core_guidance(out);
    append_prompt_guidelines(out, input.prompt_guidelines);
  }
  append_system_prompt_tail(out, input.append_system_prompt);
  append_context_sections(out, input.sections);
  append_skills(out, input.skills);
  append_custom_guidance(out, input.custom_guidance);
  if (input.plan_mode_active) {
    append_plan_mode_instructions(out);
  }
  append_goal_continuation(out, input);
  if (input.current_date.has_value() || input.working_directory.has_value() ||
      input.workspace_root.has_value() || input.shell.has_value() ||
      input.timezone.has_value() || input.network_access.has_value() ||
      input.tool_permissions.has_value() || input.active_tool_count.has_value()) {
    append_line(out, "");
    append_runtime_metadata(out, input);
  }

  result.prompt = out.str();
  if (!result.prompt.empty() && result.prompt.back() == '\n') {
    result.prompt.pop_back();
  }
  return result;
}

}  // namespace qt::harness
