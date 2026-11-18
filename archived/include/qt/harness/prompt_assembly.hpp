#pragma once

#include <string>
#include <vector>

#include <qt/harness/context_resources.hpp>

namespace qt::harness {

struct PromptSection {
  std::string path;
  ContextFileKind kind = ContextFileKind::kAgents;
  ContextFileScope scope = ContextFileScope::kCurrentDirectory;
  std::string content;

  bool empty() const noexcept { return content.empty(); }
};

struct PromptAssembly {
  std::vector<PromptSection> sections;
  std::vector<std::string> diagnostics;

  bool has_sections() const noexcept { return !sections.empty(); }

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

// Normalizes prompt section precedence to global -> ancestor ->
// current_directory while preserving stable order within each scope.
PromptAssembly assemble_prompt_sections(
    const std::vector<LoadedContextRecord>& records);

PromptAssembly assemble_prompt_sections(const RepoContextResources& resources);

}  // namespace qt::harness
