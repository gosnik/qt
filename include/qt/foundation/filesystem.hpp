#pragma once

#include "qt/foundation/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace qt::foundation {

using Path = std::filesystem::path;

Status ensure_directory(const Path& path);
Result<Path> canonicalize_path(const Path& path);
Result<std::string> read_text_file(const Path& path);
Result<std::vector<std::string>> read_lines(const Path& path);
Status write_text_file(const Path& path, std::string_view content);
std::string path_for_display(const Path& path);

}  // namespace qt::foundation
