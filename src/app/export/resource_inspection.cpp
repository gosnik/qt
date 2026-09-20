#include <qt/app/export/resource_inspection.hpp>

#include <qt/harness/repository.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <regex>
#include <system_error>

namespace qt::app {
namespace {

using Path = std::filesystem::path;

std::string path_to_string(const Path& path) {
  return path.lexically_normal().generic_string();
}

bool path_exists(const Path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool path_is_directory(const Path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

bool path_is_regular_file(const Path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool has_theme_extension(const Path& path) {
  const std::string extension = path.extension().generic_string();
  return extension == ".json";
}

Path normalize_resource_root_path(const std::string& root) {
  if (root == "~" || root.rfind("~/", 0U) == 0U ||
      (root.size() > 1U && root.front() == '~' && root[1U] != '/')) {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      if (root == "~") {
        return Path(home);
      }
      const std::string suffix = root.rfind("~/", 0U) == 0U ? root.substr(2U)
                                                            : root.substr(1U);
      return Path(home) / Path(suffix);
    }
  }
  return Path(root);
}

std::string normalize_pattern(std::string pattern) {
  if (pattern.rfind("./", 0) == 0) {
    pattern.erase(0, 2);
  }
  std::replace(pattern.begin(), pattern.end(), '\\', '/');
  return pattern;
}

std::string regex_escape(char character) {
  switch (character) {
    case '.':
    case '^':
    case '$':
    case '|':
    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '+':
    case '\\':
      return std::string("\\") + character;
    default:
      return std::string(1, character);
  }
}

std::regex glob_to_regex(const std::string& pattern) {
  std::string expression = "^";
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    const char character = pattern[index];
    if (character == '*') {
      if (index + 1 < pattern.size() && pattern[index + 1] == '*') {
        if (index + 2 < pattern.size() && pattern[index + 2] == '/') {
          expression += "(?:.*/)?";
          index += 2;
        } else {
          expression += ".*";
          ++index;
        }
      } else {
        expression += "[^/]*";
      }
      continue;
    }
    if (character == '?') {
      expression += "[^/]";
      continue;
    }
    expression += regex_escape(character);
  }
  expression += "$";
  return std::regex(expression);
}

std::string path_relative_to(const std::string& path,
                             const std::string& base_directory) {
  if (base_directory.empty()) {
    return normalize_pattern(path);
  }

  std::error_code error;
  Path relative = std::filesystem::relative(Path(path), Path(base_directory), error);
  if (error) {
    return normalize_pattern(path);
  }
  return normalize_pattern(relative.generic_string());
}

std::vector<std::string> resource_match_candidates(
    const std::string& path,
    const std::string& base_directory,
    AppResourceMatchKind kind) {
  std::vector<std::string> candidates;
  const Path resource_path(path);
  candidates.push_back(path_relative_to(path, base_directory));
  candidates.push_back(normalize_pattern(resource_path.filename().generic_string()));
  candidates.push_back(normalize_pattern(resource_path.lexically_normal().generic_string()));

  if (kind == AppResourceMatchKind::skill &&
      resource_path.filename() == "SKILL.md") {
    const Path directory = resource_path.parent_path();
    candidates.push_back(path_relative_to(path_to_string(directory), base_directory));
    candidates.push_back(normalize_pattern(directory.filename().generic_string()));
    candidates.push_back(normalize_pattern(directory.lexically_normal().generic_string()));
  }

  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  return candidates;
}

bool matches_glob_candidate(const std::string& candidate,
                            const std::string& pattern) {
  return std::regex_match(candidate, glob_to_regex(pattern));
}

bool matches_exact_candidate(const std::string& candidate,
                             const std::string& pattern) {
  return candidate == pattern;
}

enum class ResourceFilterMatchMode { glob, exact };

bool app_resource_path_matches_filter_impl(const std::string& path,
                                           const std::string& base_directory,
                                           const std::string& raw_pattern,
                                           AppResourceMatchKind kind,
                                           ResourceFilterMatchMode mode) {
  std::string pattern = normalize_pattern(raw_pattern);
  if (pattern.empty()) {
    return false;
  }
  for (const auto& candidate :
       resource_match_candidates(path, base_directory, kind)) {
    if (mode == ResourceFilterMatchMode::exact
            ? matches_exact_candidate(candidate, pattern)
            : matches_glob_candidate(candidate, pattern)) {
      return true;
    }
  }
  return false;
}

bool resource_matches_any_filter(const std::string& path,
                                 const std::string& root,
                                 const std::string& fallback_base,
                                 const std::vector<std::string>& patterns,
                                 AppResourceMatchKind kind,
                                 ResourceFilterMatchMode mode) {
  std::vector<std::string> bases;
  if (!fallback_base.empty()) {
    bases.push_back(fallback_base);
  }
  if (!root.empty()) {
    const Path root_path(root);
    bases.push_back(path_to_string(root_path.parent_path()));
    bases.push_back(path_to_string(root_path));
  }
  if (bases.empty()) {
    bases.push_back("");
  }

  std::sort(bases.begin(), bases.end());
  bases.erase(std::unique(bases.begin(), bases.end()), bases.end());

  for (const auto& pattern : patterns) {
    for (const auto& base : bases) {
      if (app_resource_path_matches_filter_impl(path, base, pattern,
                                                kind, mode)) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::string> strip_filter_prefix(
    const std::vector<std::string>& filters, char prefix) {
  std::vector<std::string> patterns;
  for (const auto& filter : filters) {
    if (!filter.empty() && filter.front() == prefix) {
      patterns.push_back(filter.substr(1));
    }
  }
  return patterns;
}

template <typename Record, typename PathAccessor>
void apply_loaded_resource_filters(std::vector<Record>& records,
                                   const std::vector<std::string>& filters,
                                   const std::string& fallback_base,
                                   AppResourceMatchKind kind,
                                   PathAccessor path_accessor) {
  if (filters.empty()) {
    return;
  }

  const std::vector<std::string> excludes = strip_filter_prefix(filters, '!');
  const std::vector<std::string> force_includes = strip_filter_prefix(filters, '+');
  const std::vector<std::string> force_excludes = strip_filter_prefix(filters, '-');

  records.erase(std::remove_if(records.begin(), records.end(),
                               [&](const Record& record) {
                                 bool enabled = true;
                                 const std::string path = path_accessor(record);
                                 if (resource_matches_any_filter(
                                         path, record.root, fallback_base,
                                         excludes, kind,
                                         ResourceFilterMatchMode::glob)) {
                                   enabled = false;
                                 }
                                 if (resource_matches_any_filter(
                                         path, record.root, fallback_base,
                                         force_includes, kind,
                                         ResourceFilterMatchMode::exact)) {
                                   enabled = true;
                                 }
                                 if (resource_matches_any_filter(
                                         path, record.root, fallback_base,
                                         force_excludes, kind,
                                         ResourceFilterMatchMode::exact)) {
                                   enabled = false;
                                 }
                                 return !enabled;
                               }),
                records.end());
}

std::vector<Path> list_theme_files(const Path& root) {
  std::vector<Path> files;
  std::error_code error;
  for (std::filesystem::directory_iterator it(root, error), end; !error && it != end;
       it.increment(error)) {
    const Path path = it->path();
    if (has_theme_extension(path) && path_is_regular_file(path)) {
      files.push_back(path);
    }
  }

  std::sort(files.begin(), files.end(),
            [](const Path& lhs, const Path& rhs) {
              return lhs.filename().generic_string() <
                     rhs.filename().generic_string();
            });
  return files;
}

std::vector<std::string> read_pi_manifest_entries(
    const harness::RepositoryView& repository, const Path& package_json,
    const char* key, ThemeResources& resources) {
  const std::string package_path = path_to_string(package_json);
  const auto content = repository.read_text_file(package_path);
  if (!content) {
    resources.add_diagnostic("failed to read package manifest: " + package_path);
    return {};
  }

  try {
    const auto json = nlohmann::json::parse(*content);
    const auto pi = json.find("pi");
    if (pi == json.end() || !pi->is_object()) {
      return {};
    }
    const auto entries = pi->find(key);
    if (entries == pi->end() || !entries->is_array()) {
      return {};
    }
    return entries->get<std::vector<std::string>>();
  } catch (const std::exception& error) {
    resources.add_diagnostic("failed to decode package manifest: " +
                             package_path + " (" + error.what() + ")");
    return {};
  }
}

void load_theme_file(const harness::RepositoryView& repository,
                     const std::string& root, const Path& path,
                     ThemeResources& resources) {
  const std::string theme_path = path_to_string(path);
  if (!path_is_regular_file(path)) {
    resources.add_diagnostic("theme entry is not a readable file: " + theme_path);
    return;
  }

  const auto content = repository.read_text_file(theme_path);
  if (!content) {
    resources.add_diagnostic("failed to read theme file: " + theme_path);
    return;
  }

  const auto parsed = parse_theme_document(theme_path, *content);
  if (!parsed) {
    resources.add_diagnostic(parsed.error().message + ": " + theme_path);
    return;
  }

  resources.loaded_themes.push_back(LoadedThemeRecord{
      root,
      theme_path,
      parsed.value().name,
      parsed.value().canonical_name,
      *content,
  });
}

bool load_package_themes(const harness::RepositoryView& repository,
                         const std::string& root, const Path& directory,
                         ThemeResources& resources) {
  const Path package_json = directory / "package.json";
  if (!path_is_regular_file(package_json)) {
    return false;
  }

  const auto declared =
      read_pi_manifest_entries(repository, package_json, "themes", resources);
  if (declared.empty()) {
    return false;
  }

  for (const auto& entry : declared) {
    const Path resolved = Path(entry).is_absolute()
                              ? Path(entry)
                              : directory / Path(entry);
    if (path_is_directory(resolved)) {
      for (const auto& path : list_theme_files(resolved)) {
        load_theme_file(repository, root, path, resources);
      }
    } else if (path_is_regular_file(resolved) && has_theme_extension(resolved)) {
      load_theme_file(repository, root, resolved, resources);
    } else {
      resources.add_diagnostic("package theme entry does not exist: " +
                               path_to_string(resolved));
    }
  }

  return true;
}

}  // namespace

const char* to_string(AppResourceKind kind) noexcept {
  switch (kind) {
    case AppResourceKind::kPromptTemplate:
      return "prompt_template";
    case AppResourceKind::kSkill:
      return "skill";
    case AppResourceKind::kTheme:
      return "theme";
  }
  return "unknown";
}

bool app_resource_path_matches_filter(const std::string& path,
                                      const std::string& base_directory,
                                      const std::string& pattern,
                                      AppResourceMatchKind kind) {
  return app_resource_path_matches_filter_impl(path, base_directory, pattern,
                                               kind,
                                               ResourceFilterMatchMode::glob);
}

ThemeResources load_theme_resources(const harness::RepositoryView& repository,
                                    const std::vector<std::string>& theme_roots) {
  ThemeResources resources;
  resources.roots = theme_roots;

  for (const auto& root : theme_roots) {
    if (root.empty()) {
      resources.add_diagnostic("theme root was empty");
      continue;
    }

    const Path root_path = normalize_resource_root_path(root);
    const std::string resolved_root = path_to_string(root_path);
    if (path_is_regular_file(root_path) && has_theme_extension(root_path)) {
      load_theme_file(repository, path_to_string(root_path.parent_path()),
                      root_path, resources);
      continue;
    }
    if (!path_exists(root_path)) {
      resources.add_diagnostic("theme root does not exist: " + resolved_root);
      continue;
    }
    if (!path_is_directory(root_path)) {
      resources.add_diagnostic("theme root is not a directory: " + resolved_root);
      continue;
    }

    if (load_package_themes(repository, resolved_root, root_path, resources)) {
      continue;
    }

    for (const auto& path : list_theme_files(root_path)) {
      load_theme_file(repository, resolved_root, path, resources);
    }
  }

  return resources;
}

AppResourceInspection inspect_app_resources(
    const harness::RepositoryView& repository, const AppSettings& settings) {
  AppResourceInspection inspection;
  inspection.settings = settings;
  inspection.plan = {
      AppResourceLoadPlanEntry{AppResourceKind::kPromptTemplate,
                               settings.features.load_prompt_templates,
                               settings.prompt_roots},
      AppResourceLoadPlanEntry{AppResourceKind::kSkill,
                               settings.features.load_skills,
                               settings.skill_roots},
      AppResourceLoadPlanEntry{AppResourceKind::kTheme,
                               settings.features.load_themes,
                               settings.theme_roots},
  };

  inspection.prompt_templates.roots = settings.prompt_roots;
  if (settings.features.load_prompt_templates) {
    inspection.prompt_templates = qt::harness::load_prompt_template_resources(
        repository, settings.prompt_roots);
    apply_loaded_resource_filters(
        inspection.prompt_templates.loaded_prompt_templates,
        settings.resource_filters.prompt_templates, settings.agent_home,
        AppResourceMatchKind::generic,
        [](const qt::harness::LoadedPromptTemplateRecord& record) {
          return record.path;
        });
  }

  inspection.skills.roots = settings.skill_roots;
  if (settings.features.load_skills) {
    inspection.skills =
        qt::harness::load_skill_resources(repository, settings.skill_roots);
    apply_loaded_resource_filters(
        inspection.skills.loaded_skills, settings.resource_filters.skills,
        settings.agent_home, AppResourceMatchKind::skill,
        [](const qt::harness::LoadedSkillRecord& record) {
          return record.path;
        });
  }

  inspection.themes.roots = settings.theme_roots;
  if (settings.features.load_themes) {
    inspection.themes = load_theme_resources(repository, settings.theme_roots);
    apply_loaded_resource_filters(
        inspection.themes.loaded_themes, settings.resource_filters.themes,
        settings.agent_home, AppResourceMatchKind::generic,
        [](const LoadedThemeRecord& record) {
          return record.path;
        });
  }

  return inspection;
}

}  // namespace qt::app
