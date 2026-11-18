#include "qt/memory/skill_import.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/hash.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace qt::memory {
namespace {

bool ignored_directory(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  static const std::set<std::string> ignored = {
      ".git", ".pio", ".svn", "node_modules", "build", "dist", "__pycache__"};
  return ignored.find(name) != ignored.end();
}

bool hidden_name(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  return !name.empty() && name.front() == '.';
}

bool likely_text_file(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  if (name.empty() || name.front() == '.') {
    return false;
  }
  static const std::set<std::string> extensions = {
      ".md", ".txt", ".json", ".jsonl", ".yaml", ".yml", ".toml",
      ".ini", ".cfg", ".conf", ".xml", ".csv", ".sh", ".bash", ".zsh",
      ".py", ".js", ".jsx", ".ts", ".tsx", ".as", ".c", ".cc", ".cpp",
      ".h", ".hh", ".hpp", ".rs", ".go", ".java", ".rb", ".php"};
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return extensions.find(extension) != extensions.end();
}

bool within(const std::filesystem::path& path,
            const std::filesystem::path& root) {
  std::error_code error;
  const auto relative = std::filesystem::relative(path, root, error);
  return !error && !relative.empty() && *relative.begin() != "..";
}

bool regular_non_symlink(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  return !error && std::filesystem::is_regular_file(status) &&
         !std::filesystem::is_symlink(status);
}

std::string bounded_file_text(const std::filesystem::path& path,
                              const std::size_t limit) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("unable to read " + path.generic_string());
  }
  std::string content;
  content.resize(limit + 1U);
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  content.resize(static_cast<std::size_t>(input.gcount()));
  if (content.find('\0') != std::string::npos) {
    throw std::runtime_error("binary content");
  }
  if (content.size() > limit) {
    static constexpr std::string_view marker = "\n\n[truncated]";
    if (limit > marker.size()) {
      content.resize(limit - marker.size());
      content += marker;
    } else {
      content.resize(limit);
    }
  }
  return content;
}

struct WalkState {
  std::size_t entries = 0U;
  bool truncated = false;
};

std::vector<std::filesystem::directory_entry> sorted_entries(
    const std::filesystem::path& directory, WalkState& state,
    const SkillImportOptions& options) {
  std::vector<std::filesystem::directory_entry> entries;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    if (state.entries >= options.maximum_walk_entries) {
      state.truncated = true;
      break;
    }
    ++state.entries;
    entries.push_back(*iterator);
    iterator.increment(error);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& left, const auto& right) {
              return left.path().generic_string() < right.path().generic_string();
            });
  return entries;
}

void find_skills_below(const std::filesystem::path& directory,
                       const SkillImportOptions& options,
                       const std::size_t depth, WalkState& state,
                       std::vector<std::filesystem::path>& result) {
  if (state.truncated || result.size() >= options.maximum_skills) {
    state.truncated = true;
    return;
  }
  if (depth >= options.maximum_depth) {
    state.truncated = true;
    return;
  }
  for (const auto& entry : sorted_entries(directory, state, options)) {
    if (state.truncated || result.size() >= options.maximum_skills) {
      state.truncated = true;
      return;
    }
    std::error_code error;
    if (entry.is_symlink(error) || error || !entry.is_directory(error) || error ||
        ignored_directory(entry.path())) {
      continue;
    }
    const auto skill = entry.path() / "SKILL.md";
    if (regular_non_symlink(skill)) {
      result.push_back(skill);
      continue;
    }
    find_skills_below(entry.path(), options, depth + 1U, state, result);
  }
}

std::vector<std::filesystem::path> find_skills(
    const std::filesystem::path& root, const SkillImportOptions& options) {
  std::vector<std::filesystem::path> result;
  const auto direct = root / "SKILL.md";
  if (regular_non_symlink(direct)) {
    result.push_back(direct);
    return result;
  }
  if (options.recursive) {
    WalkState state;
    find_skills_below(root, options, 0U, state, result);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::string relative_display(const std::filesystem::path& path,
                             const std::filesystem::path& base) {
  std::error_code error;
  const auto relative = std::filesystem::relative(path, base, error);
  return error ? path.filename().generic_string() : relative.generic_string();
}

std::string skill_snapshot(const std::filesystem::path& skill,
                           const SkillImportOptions& options,
                           std::size_t& related_count) {
  const auto package_root = skill.parent_path();
  std::ostringstream content;
  content << "# Skill memory: " << package_root.filename().generic_string() << "\n\n";
  content << "## SKILL.md\n\n";
  content << bounded_file_text(skill, options.maximum_skill_bytes) << "\n";

  std::vector<std::filesystem::path> candidates;
  WalkState state;
  const auto collect = [&](const auto& self, const std::filesystem::path& directory,
                           const std::size_t depth) -> void {
    if (state.truncated || depth >= options.maximum_depth) {
      state.truncated = true;
      return;
    }
    for (const auto& entry : sorted_entries(directory, state, options)) {
      if (state.truncated) {
        return;
      }
      std::error_code error;
      if (entry.is_symlink(error) || error) {
        continue;
      }
      if (entry.is_directory(error) && !error) {
        if (!ignored_directory(entry.path()) && !hidden_name(entry.path()) &&
            !regular_non_symlink(entry.path() / "SKILL.md")) {
          self(self, entry.path(), depth + 1U);
        }
      } else if (entry.is_regular_file(error) && !error && entry.path() != skill &&
                 likely_text_file(entry.path())) {
        candidates.push_back(entry.path());
      }
    }
  };
  collect(collect, package_root, 0U);
  std::sort(candidates.begin(), candidates.end());
  if (candidates.size() > options.maximum_package_files) {
    candidates.resize(options.maximum_package_files);
  }
  std::vector<std::filesystem::path> inventory = candidates;
  std::vector<std::filesystem::path> related = candidates;
  if (related.size() > options.maximum_related_files) {
    related.resize(options.maximum_related_files);
  }

  std::size_t remaining = options.maximum_skill_bytes > content.str().size()
                              ? options.maximum_skill_bytes - content.str().size()
                              : 0U;
  for (const auto& path : related) {
    if (remaining < 128U) {
      break;
    }
    try {
      const std::size_t file_limit =
          std::min(options.maximum_related_file_bytes, remaining - 64U);
      const std::string text = bounded_file_text(path, file_limit);
      content << "\n## Related: " << relative_display(path, package_root)
              << "\n\n" << text << "\n";
      remaining = options.maximum_skill_bytes > content.str().size()
                      ? options.maximum_skill_bytes - content.str().size()
                      : 0U;
      ++related_count;
    } catch (const std::exception&) {
    }
  }
  if (!inventory.empty()) {
    content << "\n## Package inventory\n\n";
    const std::size_t inventory_limit = std::min<std::size_t>(inventory.size(), 128U);
    for (std::size_t index = 0U; index < inventory_limit; ++index) {
      content << "- " << relative_display(inventory[index], package_root) << "\n";
    }
    if (inventory.size() > inventory_limit) {
      content << "- … " << (inventory.size() - inventory_limit)
              << " additional files omitted\n";
    }
  }
  std::string result = content.str();
  if (result.size() > options.maximum_skill_bytes) {
    static constexpr std::string_view marker = "\n\n[skill memory truncated]";
    result.resize(options.maximum_skill_bytes - marker.size());
    result += marker;
  }
  return MemoryService::normalize_content(std::move(result));
}

}  // namespace

SkillImportReport import_skill_memories(MemoryService& memories,
                                        GovernanceService& governance,
                                        const MemoryScope& scope,
                                        const SkillImportOptions& options) {
  if (options.root.empty()) {
    throw std::invalid_argument("skill scan root is required");
  }
  static std::mutex import_mutex;
  const std::lock_guard<std::mutex> import_lock(import_mutex);

  std::error_code error;
  const auto root = std::filesystem::weakly_canonical(options.root, error);
  if (error || !std::filesystem::is_directory(root, error) || error) {
    throw std::invalid_argument("skill scan root is not a readable directory: " +
                                options.root.generic_string());
  }
  if (!options.containment_root.empty()) {
    const auto containment =
        std::filesystem::weakly_canonical(options.containment_root, error);
    if (error || !std::filesystem::is_directory(containment, error) || error ||
        !within(root, containment)) {
      throw std::invalid_argument(
          "skill scan root is outside the allowed workspace");
    }
  }
  if (options.maximum_skill_bytes < 1024U ||
      options.maximum_skill_bytes > 1024U * 1024U ||
      options.maximum_related_file_bytes == 0U ||
      options.maximum_related_file_bytes > options.maximum_skill_bytes ||
      options.maximum_skills == 0U || options.maximum_skills > 10000U ||
      options.maximum_walk_entries == 0U ||
      options.maximum_walk_entries > 1000000U || options.maximum_depth == 0U ||
      options.maximum_depth > 256U) {
    throw std::invalid_argument("skill import limits are invalid");
  }
  if (scope.kind != ScopeKind::Repository) {
    throw std::invalid_argument("skill imports require repository memory scope");
  }

  SkillImportReport report;
  report.root = root;
  report.recursive = options.recursive;
  const auto skills = find_skills(root, options);
  report.discovered = skills.size();

  MemoryQuery existing_query;
  existing_query.type = MemoryType::Skill;
  existing_query.policy = RecallPolicy::Strict;
  existing_query.limit = 0U;
  const auto existing = memories.query(scope, existing_query);

  for (const auto& skill : skills) {
    SkillImportItem item;
    item.skill_path = skill;
    try {
      std::size_t related_count = 0U;
      const std::string snapshot = skill_snapshot(skill, options, related_count);
      item.related_files = related_count;
      const std::string source_key = foundation::stable_hash_hex(
          "file://" + skill.generic_string());
      const std::string uri = "skill://local/" + source_key;
      const auto prior = std::find_if(existing.begin(), existing.end(),
                                      [&](const MemoryEntry& memory) {
        return memory.provenance.source_id == source_key;
      });
      const std::string snapshot_hash = foundation::stable_hash_hex(snapshot);
      if (prior != existing.end() &&
          prior->provenance.source_hash == snapshot_hash) {
        item.action = "unchanged";
        item.memory_id = prior->id;
        ++report.unchanged;
      } else {
        MemoryWrite write;
        write.type = MemoryType::Skill;
        write.status = MemoryStatus::Active;
        write.provenance.source_id = source_key;
        write.provenance.source_type = SourceType::File;
        write.provenance.source_uri = uri;
        write.provenance.source_hash = snapshot_hash;
        write.provenance.created_by = "import";
        write.provenance.evidence_strength = 0.95;
        write.source = "user:skill-scan";
        write.content = snapshot;
        write.tags = {"skill", "skill:imported", "skill-source:" + source_key,
                      "skill:name:" + skill.parent_path().filename().generic_string()};
        write.confidence = 0.95;
        write.quality_score = 0.9;
        item.memory_id = memories.remember(scope, std::move(write));
        if (prior != existing.end()) {
          try {
            governance.supersede(scope, prior->id, item.memory_id,
                                 "user:skill-scan", "skill source changed");
          } catch (...) {
            memories.forget(scope, item.memory_id);
            item.memory_id.clear();
            throw;
          }
        }
        if (prior != existing.end()) {
          item.action = "updated";
          ++report.updated;
        } else {
          item.action = "created";
          ++report.created;
        }
      }
    } catch (const std::exception& exception) {
      item.action = "failed";
      item.diagnostic = exception.what();
      ++report.failed;
    }
    report.items.push_back(std::move(item));
    if (options.progress) {
      try {
        options.progress(report.items.back());
      } catch (...) {
        // UI/reporting failures must not roll back a durable skill import.
      }
    }
  }
  return report;
}

}  // namespace qt::memory
