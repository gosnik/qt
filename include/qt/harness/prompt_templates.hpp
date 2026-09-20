#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <qt/harness/context_discovery.hpp>

namespace qt::harness {

class RepositoryView;

enum class PromptTemplateDiagnosticCode {
  kEmptyRoot,
  kRootDoesNotExist,
  kRootIsNotDirectory,
  kPromptFileNotReadable,
  kPromptFileReadFailed,
  kInvalidFrontmatter,
};

struct PromptTemplateDiagnostic {
  PromptTemplateDiagnosticCode code =
      PromptTemplateDiagnosticCode::kEmptyRoot;
  std::string path;
  std::string message;
};

struct LoadedPromptTemplateRecord {
  std::string root;
  std::string path;
  std::string name;
  std::string description;
  std::optional<std::string> argument_hint;
  std::string content;
};

struct PromptTemplateResources {
  std::vector<std::string> roots;
  std::vector<LoadedPromptTemplateRecord> loaded_prompt_templates;
  std::vector<PromptTemplateDiagnostic> diagnostics;

  bool has_loaded_prompt_templates() const noexcept {
    return !loaded_prompt_templates.empty();
  }

  void add_diagnostic(PromptTemplateDiagnostic diagnostic) {
    if (!diagnostic.message.empty()) {
      diagnostics.push_back(std::move(diagnostic));
    }
  }
};

PromptTemplateResources load_prompt_template_resources(
    const RepositoryView& repository,
    const std::vector<std::string>& prompt_roots);

PromptTemplateResources discover_and_load_prompt_templates(
    const RepositoryView& repository,
    const RepoContextDiscovery& discovery);

std::vector<std::string> parse_prompt_template_arguments(
    std::string_view arguments);

std::string expand_prompt_template_content(
    std::string_view content,
    const std::vector<std::string>& arguments);

std::string expand_prompt_template_content(
    std::string_view content,
    std::string_view arguments);

}  // namespace qt::harness
