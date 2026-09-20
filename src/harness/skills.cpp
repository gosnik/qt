#include <qt/harness/skills.hpp>

#include <qt/harness/repository.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <unordered_map>
#include <system_error>

namespace qt::harness {
namespace {

using Path = std::filesystem::path;

constexpr std::size_t kMaxSkillNameLength = 64U;
constexpr std::size_t kMaxSkillDescriptionLength = 1024U;
constexpr const char* kIgnoreFileNames[] = {".gitignore", ".ignore",
                                            ".fdignore"};

struct ParsedSkillDocument {
  bool had_frontmatter = false;
  std::optional<std::string> name;
  std::optional<std::string> description;
  bool disable_model_invocation = false;
  std::string body;
};

std::string path_to_string(const Path& path) {
  return path.lexically_normal().generic_string();
}

std::string canonical_path_string(const std::string& path) {
  std::error_code error;
  Path canonical = std::filesystem::weakly_canonical(Path(path), error);
  if (error) {
    canonical = Path(path).lexically_normal();
  }
  return canonical.generic_string();
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

bool has_markdown_extension(const Path& path) {
  return path.extension().generic_string() == ".md";
}

std::string regex_escape(char ch) {
  static constexpr std::string_view kSpecial = R"(\.^$|()[]{}+)";
  if (kSpecial.find(ch) != std::string_view::npos) {
    return std::string("\\") + ch;
  }
  return std::string(1U, ch);
}

std::regex glob_to_regex(const std::string& glob) {
  std::string pattern = "^";
  for (std::size_t index = 0; index < glob.size(); ++index) {
    if (index == 0U && glob.rfind("**/", 0U) == 0U) {
      pattern += "(?:.*/)?";
      index = 2U;
      continue;
    }
    const char ch = glob[index];
    if (ch == '*') {
      if (index + 1U < glob.size() && glob[index + 1U] == '*') {
        pattern += ".*";
        ++index;
      } else {
        pattern += "[^/]*";
      }
    } else if (ch == '?') {
      pattern += "[^/]";
    } else {
      pattern += regex_escape(ch);
    }
  }
  pattern += "$";
  return std::regex(pattern);
}

std::string normalize_newlines(std::string text) {
  std::string normalized;
  normalized.reserve(text.size());

  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\r') {
      normalized.push_back('\n');
      if (index + 1 < text.size() && text[index + 1] == '\n') {
        ++index;
      }
      continue;
    }
    normalized.push_back(text[index]);
  }

  return normalized;
}

std::string trim_copy(std::string_view value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }

  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return std::string(value.substr(start, end - start));
}

std::string relative_display_path(const Path& path, const Path& root) {
  std::error_code error;
  const auto relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty()) {
    return path.filename().generic_string();
  }
  return relative.generic_string();
}

std::optional<std::string> prefixed_ignore_pattern(std::string line,
                                                   const std::string& prefix) {
  line = trim_copy(line);
  if (line.empty()) {
    return std::nullopt;
  }
  if (line.front() == '#' &&
      !(line.size() > 1U && line[0] == '\\' && line[1] == '#')) {
    return std::nullopt;
  }

  bool negated = false;
  if (line.front() == '!') {
    negated = true;
    line.erase(line.begin());
  } else if (line.size() > 1U && line[0] == '\\' && line[1] == '!') {
    line.erase(line.begin());
  }
  if (!line.empty() && line.front() == '/') {
    line.erase(line.begin());
  }
  if (line.empty()) {
    return std::nullopt;
  }

  std::string pattern = prefix + line;
  if (negated) {
    pattern.insert(pattern.begin(), '!');
  }
  return pattern;
}

void add_ignore_patterns(const RepositoryView& repository, const Path& directory,
                         const Path& root,
                         std::vector<std::string>& patterns) {
  const std::string relative_directory = relative_display_path(directory, root);
  const std::string prefix =
      directory == root ? std::string{} : relative_directory + "/";

  for (const char* filename : kIgnoreFileNames) {
    const Path ignore_path = directory / filename;
    if (!path_is_regular_file(ignore_path)) {
      continue;
    }
    const auto content = repository.read_text_file(path_to_string(ignore_path));
    if (!content) {
      continue;
    }

    std::size_t offset = 0U;
    while (offset <= content->size()) {
      const std::size_t newline = content->find('\n', offset);
      std::string line =
          newline == std::string::npos
              ? content->substr(offset)
              : content->substr(offset, newline - offset);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (auto pattern = prefixed_ignore_pattern(std::move(line), prefix)) {
        patterns.push_back(std::move(*pattern));
      }
      if (newline == std::string::npos) {
        break;
      }
      offset = newline + 1U;
    }
  }
}

bool ignore_pattern_matches(const std::string& display_path,
                            const std::string& pattern) {
  std::string normalized = pattern;
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  if (normalized.empty()) {
    return false;
  }
  const bool directory_pattern = normalized.back() == '/';
  if (directory_pattern) {
    normalized.pop_back();
    return display_path == normalized ||
           display_path.rfind(normalized + "/", 0U) == 0U;
  }

  try {
    const auto matcher = glob_to_regex(normalized);
    if (std::regex_match(display_path, matcher)) {
      return true;
    }
    if (normalized.find('/') == std::string::npos) {
      const auto slash = display_path.find_last_of('/');
      const std::string basename =
          slash == std::string::npos ? display_path
                                     : display_path.substr(slash + 1U);
      return std::regex_match(basename, matcher);
    }
  } catch (const std::regex_error&) {
    return false;
  }
  return false;
}

bool ignored_by_patterns(const Path& path, const Path& root,
                         bool directory,
                         const std::vector<std::string>& patterns) {
  std::string display_path = relative_display_path(path, root);
  if (directory && !display_path.empty() && display_path.back() != '/') {
    display_path.push_back('/');
  }

  bool ignored = false;
  for (const auto& pattern : patterns) {
    if (pattern.empty()) {
      continue;
    }
    const bool negated = pattern.front() == '!';
    const std::string effective_pattern =
        negated ? pattern.substr(1U) : pattern;
    if (!effective_pattern.empty() &&
        ignore_pattern_matches(display_path, effective_pattern)) {
      ignored = !negated;
    }
  }
  return ignored;
}

std::string maybe_unquote(std::string value) {
  if (value.size() < 2U) {
    return value;
  }

  const char first = value.front();
  const char last = value.back();
  if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
    return value.substr(1U, value.size() - 2U);
  }
  return value;
}

bool is_valid_skill_name_character(char value) {
  const auto ch = static_cast<unsigned char>(value);
  return std::islower(ch) != 0 || std::isdigit(ch) != 0 || value == '-';
}

void add_skill_name_diagnostics(const std::string& name,
                                const std::string& parent_directory_name,
                                const std::string& skill_path,
                                SkillResources& resources) {
  if (name != parent_directory_name) {
    resources.add_diagnostic("name \"" + name +
                             "\" does not match parent directory \"" +
                             parent_directory_name + "\": " + skill_path);
  }
  if (name.size() > kMaxSkillNameLength) {
    resources.add_diagnostic("name exceeds 64 characters (" +
                             std::to_string(name.size()) + "): " + skill_path);
  }
  if (!std::all_of(name.begin(), name.end(), is_valid_skill_name_character)) {
    resources.add_diagnostic(
        "name contains invalid characters (must be lowercase a-z, 0-9, hyphens only): " +
        skill_path);
  }
  if (!name.empty() && (name.front() == '-' || name.back() == '-')) {
    resources.add_diagnostic("name must not start or end with a hyphen: " +
                             skill_path);
  }
  if (name.find("--") != std::string::npos) {
    resources.add_diagnostic("name must not contain consecutive hyphens: " +
                             skill_path);
  }
}

ParsedSkillDocument parse_skill_document(const std::string& raw_content,
                                         const std::string& skill_path,
                                         SkillResources& resources) {
  ParsedSkillDocument document;
  const std::string normalized = normalize_newlines(raw_content);
  const std::size_t first_line_end = normalized.find('\n');
  const std::string first_line = normalized.substr(0, first_line_end);
  if (first_line != "---") {
    document.body = trim_copy(normalized);
    return document;
  }

  document.had_frontmatter = true;
  std::size_t offset =
      first_line_end == std::string::npos ? normalized.size() : first_line_end + 1U;
  bool found_closing_delimiter = false;

  while (offset <= normalized.size()) {
    const std::size_t line_end = normalized.find('\n', offset);
    const std::string line = normalized.substr(
        offset, line_end == std::string::npos ? line_end : line_end - offset);

    if (line == "---") {
      found_closing_delimiter = true;
      if (line_end == std::string::npos) {
        document.body.clear();
      } else {
        document.body = trim_copy(normalized.substr(line_end + 1U));
      }
      break;
    }

    const std::string trimmed = trim_copy(line);
    if (!trimmed.empty() && trimmed.front() != '#') {
      const std::size_t separator = trimmed.find(':');
      if (separator == std::string::npos) {
        resources.add_diagnostic("failed to parse skill file: " + skill_path +
                                 " (frontmatter line is missing ':' separator)");
        return document;
      }

      const std::string key = trim_copy(std::string_view(trimmed).substr(0, separator));
      const std::string value =
          maybe_unquote(trim_copy(std::string_view(trimmed).substr(separator + 1U)));
      if (key == "name" && !value.empty()) {
        document.name = value;
      } else if (key == "description") {
        document.description = value;
      } else if (key == "disable-model-invocation") {
        document.disable_model_invocation = value == "true";
      }
    }

    if (line_end == std::string::npos) {
      break;
    }
    offset = line_end + 1U;
  }

  if (!found_closing_delimiter) {
    resources.add_diagnostic("failed to parse skill file: " + skill_path +
                             " (frontmatter block is missing closing delimiter)");
  }

  return document;
}

Path normalize_skill_root_path(const std::string& root) {
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

std::vector<Path> list_subdirectories(const Path& directory) {
  std::vector<Path> entries;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
       it.increment(error)) {
    const Path path = it->path();
    if (path_is_directory(path)) {
      entries.push_back(path);
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const Path& lhs, const Path& rhs) {
              return lhs.generic_string() < rhs.generic_string();
            });
  return entries;
}

std::vector<Path> list_markdown_files(const Path& directory) {
  std::vector<Path> files;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    const Path path = it->path();
    if (path_is_regular_file(path) && has_markdown_extension(path)) {
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
    const RepositoryView& repository, const Path& package_json, const char* key,
    SkillResources& resources) {
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

void load_skill_file(const RepositoryView& repository, const std::string& root,
                     const Path& skill_file, SkillResources& resources) {
  const std::string skill_path = path_to_string(skill_file);
  if (!path_is_regular_file(skill_file)) {
    resources.add_diagnostic("skill entry is not a readable file: " + skill_path);
    return;
  }

  const auto content = repository.read_text_file(skill_path);
  if (!content) {
    resources.add_diagnostic("failed to read skill file: " + skill_path);
    return;
  }

  const bool standard_skill = skill_file.filename() == "SKILL.md";
  const Path directory = standard_skill ? skill_file.parent_path()
                                        : skill_file.parent_path();
  const std::string parent_directory_name = directory.filename().generic_string();
  const ParsedSkillDocument parsed =
      parse_skill_document(*content, skill_path, resources);
  const std::string name =
      parsed.name.value_or(standard_skill ? parent_directory_name
                                          : skill_file.stem().generic_string());
  std::string description = parsed.description.value_or(std::string{});

  if (parsed.had_frontmatter) {
    add_skill_name_diagnostics(name, parent_directory_name, skill_path, resources);
    if (!parsed.description || trim_copy(*parsed.description).empty()) {
      resources.add_diagnostic("description is required: " + skill_path);
      return;
    }
    if (description.size() > kMaxSkillDescriptionLength) {
      resources.add_diagnostic("description exceeds 1024 characters (" +
                               std::to_string(description.size()) + "): " +
                               skill_path);
    }
  }

  resources.loaded_skills.push_back(LoadedSkillRecord{
      root,
      path_to_string(directory),
      skill_path,
      name,
      description,
      parsed.body,
      parsed.disable_model_invocation,
  });
}

void load_skill_directory(const RepositoryView& repository,
                         const std::string& root,
                         const Path& directory,
                         SkillResources& resources,
                         const Path& scan_root,
                         bool include_root_files,
                         std::vector<std::string> ignore_patterns = {}) {
  add_ignore_patterns(repository, directory, scan_root, ignore_patterns);

  const Path package_json = directory / "package.json";
  if (path_is_regular_file(package_json)) {
    const auto declared =
        read_pi_manifest_entries(repository, package_json, "skills", resources);
    if (!declared.empty()) {
      for (const auto& entry : declared) {
        const Path resolved = Path(entry).is_absolute()
                                  ? Path(entry)
                                  : directory / Path(entry);
        if (ignored_by_patterns(resolved, scan_root, path_is_directory(resolved),
                                ignore_patterns)) {
          continue;
        }
        if (path_is_directory(resolved)) {
          load_skill_directory(repository, root, resolved, resources, scan_root,
                               true, ignore_patterns);
        } else if (path_is_regular_file(resolved) && has_markdown_extension(resolved)) {
          load_skill_file(repository, root, resolved, resources);
        } else {
          resources.add_diagnostic("package skill entry does not exist: " +
                                   path_to_string(resolved));
        }
      }
      return;
    }
  }

  const Path skill_file = directory / "SKILL.md";
  if (path_exists(skill_file)) {
    if (ignored_by_patterns(skill_file, scan_root, false, ignore_patterns)) {
      return;
    }
    load_skill_file(repository, root, skill_file, resources);
    return;
  }

  if (include_root_files) {
    for (const auto& markdown_file : list_markdown_files(directory)) {
      if (ignored_by_patterns(markdown_file, scan_root, false, ignore_patterns)) {
        continue;
      }
      load_skill_file(repository, root, markdown_file, resources);
    }
  }

  for (const auto& child : list_subdirectories(directory)) {
    if (ignored_by_patterns(child, scan_root, true, ignore_patterns)) {
      continue;
    }
    load_skill_directory(repository, root, child, resources, scan_root,
                         false, ignore_patterns);
  }
}

bool is_agents_skills_root(const Path& root_path) {
  return root_path.filename() == "skills" &&
         root_path.parent_path().filename() == ".agents";
}

}  // namespace

SkillResources load_skill_resources(const RepositoryView& repository,
                                   const std::vector<std::string>& skill_roots) {
  SkillResources resources;
  resources.roots = skill_roots;

  for (const auto& root : skill_roots) {
    if (root.empty()) {
      resources.add_diagnostic("skill root was empty");
      continue;
    }

    const Path root_path = normalize_skill_root_path(root);
    const std::string resolved_root = path_to_string(root_path);
    if (path_is_regular_file(root_path) && has_markdown_extension(root_path)) {
      load_skill_file(repository, path_to_string(root_path.parent_path()),
                      root_path, resources);
      continue;
    }
    if (!path_exists(root_path)) {
      resources.add_diagnostic("skill root does not exist: " + resolved_root);
      continue;
    }
    if (!path_is_directory(root_path)) {
      resources.add_diagnostic("skill root is not a directory: " + resolved_root);
      continue;
    }

    load_skill_directory(repository, resolved_root, root_path, resources, root_path,
                         !is_agents_skills_root(root_path));
  }

  std::vector<LoadedSkillRecord> filtered;
  std::set<std::string> seen_paths;
  std::unordered_map<std::string, std::string> winner_paths_by_name;
  filtered.reserve(resources.loaded_skills.size());
  for (auto& skill : resources.loaded_skills) {
    const std::string canonical = canonical_path_string(skill.path);
    if (seen_paths.find(canonical) != seen_paths.end()) {
      continue;
    }
    const auto winner = winner_paths_by_name.find(skill.name);
    if (winner != winner_paths_by_name.end()) {
      resources.add_diagnostic("name \"" + skill.name + "\" collision: " +
                               skill.path + " (winner: " + winner->second +
                               ")");
      continue;
    }
    seen_paths.insert(canonical);
    winner_paths_by_name.emplace(skill.name, skill.path);
    filtered.push_back(std::move(skill));
  }
  resources.loaded_skills = std::move(filtered);
  return resources;
}

SkillResources discover_and_load_skills(const RepositoryView& repository,
                                        const RepoContextDiscovery& discovery) {
  return load_skill_resources(repository, discovery.skill_roots);
}

}  // namespace qt::harness
