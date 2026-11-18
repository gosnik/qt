#include <qt/harness/repository.hpp>

#include <qt/foundation/filesystem.hpp>

#include <filesystem>

namespace qt::harness {
namespace {

using qt::foundation::Path;

std::optional<std::string> canonicalize_to_string(const Path& path) {
  const auto canonical = qt::foundation::canonicalize_path(path);
  if (!canonical) {
    return std::nullopt;
  }
  return qt::foundation::path_for_display(canonical.value());
}

bool path_exists(const Path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool path_is_regular_file(const Path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool path_is_directory(const Path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

}  // namespace

FilesystemRepository::FilesystemRepository(std::string working_directory)
    : working_directory_(std::move(working_directory)) {
  if (const auto canonical = canonicalize_path(working_directory_)) {
    working_directory_ = *canonical;
  }
}

std::string FilesystemRepository::cwd() const { return working_directory_; }

std::optional<std::string> FilesystemRepository::canonicalize_path(
    const std::string& path) const {
  if (path.empty()) {
    return std::nullopt;
  }
  return canonicalize_to_string(Path(path));
}

std::optional<std::string> FilesystemRepository::parent_directory(
    const std::string& path) const {
  const auto canonical = canonicalize_path(path);
  if (!canonical) {
    return std::nullopt;
  }

  const Path current(*canonical);
  const Path parent = current.parent_path();
  if (parent.empty() || parent == current) {
    return std::nullopt;
  }
  return qt::foundation::path_for_display(parent);
}

std::optional<std::string> FilesystemRepository::find_repository_root(
    const std::string& start_path) const {
  auto current = canonicalize_path(start_path);
  if (!current) {
    return std::nullopt;
  }

  if (path_is_regular_file(Path(*current))) {
    current = parent_directory(*current);
    if (!current) {
      return std::nullopt;
    }
  }

  while (current) {
    const Path current_path(*current);
    const Path git_marker = current_path / ".git";
    const Path pi_marker = current_path / ".pi";

    if (path_exists(git_marker) || path_is_directory(pi_marker)) {
      return *current;
    }

    const auto parent = parent_directory(*current);
    if (!parent || *parent == *current) {
      break;
    }
    current = parent;
  }

  return std::nullopt;
}

bool FilesystemRepository::file_exists(const std::string& path) const {
  return path_is_regular_file(Path(path));
}

bool FilesystemRepository::directory_exists(const std::string& path) const {
  return path_is_directory(Path(path));
}

std::optional<std::string> FilesystemRepository::read_text_file(
    const std::string& path) const {
  const auto content = qt::foundation::read_text_file(Path(path));
  if (!content) {
    return std::nullopt;
  }
  return content.value();
}

}  // namespace qt::harness
