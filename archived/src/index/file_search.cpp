#include "index/file_search.hpp"

#include "index/gitignore.hpp"
#include "index/path_util.hpp"

#if __has_include(<recls/recls.h>)
#include <recls/recls.h>
#define QTINDEX_HAS_RECLS_HEADERS 1
#else
#define QTINDEX_HAS_RECLS_HEADERS 0
#endif

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace qt::index {
namespace {

using PathList = std::vector<std::filesystem::path>;
using FfsIndex = std::unordered_map<std::string, PathList>;

bool is_regular_or_directory(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    return entry.is_regular_file(ec) || entry.is_directory(ec);
}

PathList stable_sorted(PathList paths) {
    std::sort(paths.begin(), paths.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.generic_string() < rhs.generic_string();
    });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

PathList search_portable(const std::filesystem::path& root, const std::string& fragment) {
    PathList matches;
    std::error_code ec;
    const auto gitignore = GitIgnoreMatcher::load(root);

    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (is_nix_store_path(it->path()) || should_skip_search_directory(it->path()) ||
                gitignore.ignores(it->path(), true)) {
                it.disable_recursion_pending();
                continue;
            }
        } else if (is_nix_store_path(it->path()) || gitignore.ignores(it->path(), false)) {
            continue;
        }

        if (!is_regular_or_directory(*it)) {
            continue;
        }

        const auto name = it->path().filename().string();
        if (name.find(fragment) != std::string::npos) {
            matches.push_back(it->path());
        }
    }

    return stable_sorted(std::move(matches));
}

FfsIndex build_ffs_index(const std::filesystem::path& root) {
    FfsIndex index;
    std::error_code ec;
    const auto gitignore = GitIgnoreMatcher::load(root);

    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (is_nix_store_path(it->path()) || should_skip_search_directory(it->path()) ||
                gitignore.ignores(it->path(), true)) {
                it.disable_recursion_pending();
                continue;
            }
        } else if (is_nix_store_path(it->path()) || gitignore.ignores(it->path(), false)) {
            continue;
        }

        if (!is_regular_or_directory(*it)) {
            continue;
        }

        index[it->path().filename().string()].push_back(it->path());
    }

    return index;
}

PathList search_ffs_index(const std::filesystem::path& root, const std::string& fragment) {
    PathList matches;
    FfsIndex index = build_ffs_index(root);

    for (const auto& [name, paths] : index) {
        if (name.find(fragment) != std::string::npos) {
            matches.insert(matches.end(), paths.begin(), paths.end());
        }
    }

    return stable_sorted(std::move(matches));
}

PathList search_recls_compatible(const std::filesystem::path& root, const std::string& fragment) {
#if QTINDEX_HAS_RECLS_HEADERS
    static_assert(RECLS_VER_MAJOR >= 1, "unexpected recls major version");
#endif
    return search_portable(root, fragment);
}

}  // namespace

FileSearchBackend parse_file_search_backend(const std::string& name) {
    if (name == "portable") {
        return FileSearchBackend::PortableFilesystem;
    }
    if (name == "ffs-index") {
        return FileSearchBackend::FfsIndex;
    }
    if (name == "recls") {
        return FileSearchBackend::ReclsCompatible;
    }

    throw std::invalid_argument("unknown file search backend: " + name);
}

const char* file_search_backend_name(FileSearchBackend backend) {
    switch (backend) {
        case FileSearchBackend::PortableFilesystem:
            return "portable";
        case FileSearchBackend::FfsIndex:
            return "ffs-index";
        case FileSearchBackend::ReclsCompatible:
            return "recls";
    }

    return "unknown";
}

std::vector<std::filesystem::path> search_files(const FileSearchOptions& options) {
    if (options.name_fragment.empty()) {
        return {};
    }

    if (!std::filesystem::exists(options.root)) {
        throw std::runtime_error("file search root does not exist: " + options.root.string());
    }

    switch (options.backend) {
        case FileSearchBackend::PortableFilesystem:
            return search_portable(options.root, options.name_fragment);
        case FileSearchBackend::FfsIndex:
            return search_ffs_index(options.root, options.name_fragment);
        case FileSearchBackend::ReclsCompatible:
            return search_recls_compatible(options.root, options.name_fragment);
    }

    return {};
}

std::string file_search_integration_summary() {
#if QTINDEX_HAS_RECLS_HEADERS
    return "recls " + std::to_string(RECLS_VER_MAJOR) + "." + std::to_string(RECLS_VER_MINOR) +
           " headers; FFS-style index backend";
#else
    return "FFS-style index backend";
#endif
}

}  // namespace qt::index
