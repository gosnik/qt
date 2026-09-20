#pragma once

#include <filesystem>
#include <string>

namespace qt::index {

std::string read_text_file(const std::filesystem::path& path);
std::filesystem::path normalize_path(const std::filesystem::path& base,
                                     const std::filesystem::path& path);
bool path_has_prefix(const std::filesystem::path& path, const std::filesystem::path& prefix);
bool is_nix_store_path(const std::filesystem::path& path);
bool is_ignored_dependency_path(const std::filesystem::path& path);
bool is_ignored_inferred_translation_unit_path(const std::filesystem::path& path);
bool should_skip_scan_directory(const std::filesystem::path& path);
bool should_skip_search_directory(const std::filesystem::path& path);

}  // namespace qt::index
