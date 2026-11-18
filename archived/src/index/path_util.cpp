#include "index/path_util.hpp"

#include <fstream>
#include <sstream>

namespace qt::index {

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::filesystem::path normalize_path(const std::filesystem::path& base,
                                     const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolute = path.is_absolute() ? path : base / path;
    auto normalized = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) {
        normalized = absolute.lexically_normal();
    }
    return normalized;
}

bool path_has_prefix(const std::filesystem::path& path, const std::filesystem::path& prefix) {
    auto path_it = path.begin();
    auto prefix_it = prefix.begin();
    for (; prefix_it != prefix.end(); ++prefix_it, ++path_it) {
        if (path_it == path.end() || *path_it != *prefix_it) {
            return false;
        }
    }
    return true;
}

bool is_nix_store_path(const std::filesystem::path& path) {
    auto it = path.begin();
    if (it == path.end() || *it != "/") {
        return false;
    }
    ++it;
    if (it == path.end() || it->string() != "nix") {
        return false;
    }
    ++it;
    return it != path.end() && it->string() == "store";
}

bool is_ignored_dependency_path(const std::filesystem::path& path) {
    if (is_nix_store_path(path)) {
        return true;
    }
    for (const auto& part : path) {
        const auto name = part.string();
        if (name == "node_modules" || name == "target" || name == ".git" || name == ".pio" ||
            name.rfind(".waf3-", 0) == 0 || name == "venv" || name == ".venv" ||
            name == "site-packages" || name == "__pycache__") {
            return true;
        }
    }
    return false;
}

bool is_ignored_inferred_translation_unit_path(const std::filesystem::path& path) {
    for (const auto& part : path) {
        const auto name = part.string();
        if (name == "venv" || name == ".venv" || name == "site-packages" ||
            name == "__pycache__") {
            return true;
        }
    }
    return false;
}

bool should_skip_scan_directory(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name == ".git" || name == ".hg" || name == ".svn" || name == ".pio" ||
           name == ".idea" || name == ".vscode" || name == "build" || name == "builds" ||
           name == "cmake-build-debug" || name == "cmake-build-release" ||
           name == "__pycache__" || name == "node_modules" || name == "third_party" ||
           name == "vendor" || name == "external";
}

bool should_skip_search_directory(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    return name == ".git" || name == ".pio" || name == ".idea" || name == "build" ||
           name == "builds" || name == "cmake-build-debug" || name == "cmake-build-release" ||
           name == "third_party" || name == "vendor" || name == "external";
}

}  // namespace qt::index
