#pragma once

#include <string>
#include <utility>
#include <vector>

#include <qt/app/config/settings.hpp>
#include <qt/app/config/themes.hpp>
#include <qt/harness/prompt_templates.hpp>
#include <qt/harness/skills.hpp>

namespace qt::harness {
class RepositoryView;
}  // namespace qt::harness

namespace qt::app {

enum class AppResourceKind {
  kPromptTemplate,
  kSkill,
  kTheme,
};

struct AppResourceLoadPlanEntry {
  AppResourceKind kind = AppResourceKind::kPromptTemplate;
  bool enabled = true;
  std::vector<std::string> roots;
};

struct LoadedThemeRecord {
  std::string root;
  std::string path;
  std::string name;
  std::string canonical_name;
  std::string content;
};

struct ThemeResources {
  std::vector<std::string> roots;
  std::vector<LoadedThemeRecord> loaded_themes;
  std::vector<std::string> diagnostics;

  bool has_loaded_themes() const noexcept { return !loaded_themes.empty(); }

  void add_diagnostic(std::string message) {
    if (!message.empty()) {
      diagnostics.push_back(std::move(message));
    }
  }
};

struct AppResourceInspection {
  AppSettings settings;
  std::vector<AppResourceLoadPlanEntry> plan;
  qt::harness::PromptTemplateResources prompt_templates;
  qt::harness::SkillResources skills;
  ThemeResources themes;
};

const char* to_string(AppResourceKind kind) noexcept;

enum class AppResourceMatchKind { generic, skill };

bool app_resource_path_matches_filter(const std::string& path,
                                      const std::string& base_directory,
                                      const std::string& pattern,
                                      AppResourceMatchKind kind);

ThemeResources load_theme_resources(const harness::RepositoryView& repository,
                                    const std::vector<std::string>& theme_roots);

AppResourceInspection inspect_app_resources(
    const harness::RepositoryView& repository, const AppSettings& settings);

}  // namespace qt::app
