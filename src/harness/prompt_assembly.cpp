#include <qt/harness/prompt_assembly.hpp>

namespace qt::harness {

namespace {

std::size_t scope_precedence(const ContextFileScope scope) noexcept {
  switch (scope) {
    case ContextFileScope::kGlobal:
      return 0;
    case ContextFileScope::kAncestor:
      return 1;
    case ContextFileScope::kCurrentDirectory:
      return 2;
  }
  return 3;
}

std::vector<const LoadedContextRecord*> normalized_record_order(
    const std::vector<LoadedContextRecord>& records) {
  std::vector<const LoadedContextRecord*> ordered;
  ordered.reserve(records.size());
  for (const auto& record : records) {
    ordered.push_back(&record);
  }

  std::stable_sort(
      ordered.begin(), ordered.end(),
      [](const LoadedContextRecord* lhs, const LoadedContextRecord* rhs) {
        return scope_precedence(lhs->scope) < scope_precedence(rhs->scope);
      });
  return ordered;
}

void append_sections(PromptAssembly& assembly,
                     const std::vector<LoadedContextRecord>& records) {
  if (records.empty()) {
    assembly.add_diagnostic("no context resources available for prompt assembly");
    return;
  }

  for (const LoadedContextRecord* record : normalized_record_order(records)) {
    assembly.sections.push_back(PromptSection{
        record->path,
        record->kind,
        record->scope,
        record->content,
    });
    if (record->content.empty()) {
      assembly.add_diagnostic("context file was empty: " + record->path);
    }
  }
}

}  // namespace

PromptAssembly assemble_prompt_sections(
    const std::vector<LoadedContextRecord>& records) {
  PromptAssembly assembly;
  append_sections(assembly, records);
  return assembly;
}

PromptAssembly assemble_prompt_sections(const RepoContextResources& resources) {
  PromptAssembly assembly;
  for (const auto& diagnostic : resources.diagnostics) {
    assembly.add_diagnostic(diagnostic);
  }
  append_sections(assembly, resources.loaded_context_files);
  return assembly;
}

}  // namespace qt::harness
