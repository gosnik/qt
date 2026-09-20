#include "index/build_emulator_internal.hpp"

#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace qt::index::build_emulator_internal {

std::vector<std::filesystem::path> parse_waf_include_tokens(const std::filesystem::path& root,
                                                            const std::string& text) {
    std::vector<std::filesystem::path> includes;
    std::set<std::string> seen;
    const std::vector<std::string> keys{"includes", "export_includes"};

    for (const auto& key : keys) {
        std::size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string::npos) {
            const auto equals = text.find('=', pos + key.size());
            if (equals == std::string::npos) {
                pos += key.size();
                continue;
            }
            const auto end = text.find_first_of("\n)", equals + 1);
            const auto fragment = text.substr(equals + 1, end == std::string::npos ? end : end - equals - 1);
            for (auto token : split_shell_like(fragment)) {
                token.erase(std::remove(token.begin(), token.end(), ','), token.end());
                token.erase(std::remove(token.begin(), token.end(), '['), token.end());
                token.erase(std::remove(token.begin(), token.end(), ']'), token.end());
                token = trim_ascii(std::move(token));
                if (token.empty() || token.find('$') != std::string::npos ||
                    token.find('%') != std::string::npos || token == "+" || token == ":") {
                    continue;
                }
                add_existing_include(includes, seen, root / token);
            }
            pos = end == std::string::npos ? text.size() : end;
        }
    }

    return includes;
}

std::vector<std::filesystem::path> waf_include_roots(
    const std::filesystem::path& root,
    const std::filesystem::path& source,
    const std::vector<std::filesystem::path>& script_includes) {
    std::vector<std::filesystem::path> includes;
    std::set<std::string> seen;

    add_existing_include(includes, seen, root);
    add_existing_include(includes, seen, source.parent_path());
    for (const auto& path : script_includes) {
        add_existing_include(includes, seen, path);
    }

    for (auto dir = source.parent_path(); !dir.empty() && path_has_prefix(dir, root);
         dir = dir.parent_path()) {
        add_existing_include(includes, seen, dir);
        if (dir == root) {
            break;
        }
    }

    for (const auto& conventional : {"include", "inc", "src"}) {
        add_existing_include(includes, seen, root / conventional);
    }
    for (const auto& top_level : {"libraries", "modules"}) {
        const auto base = root / top_level;
        add_existing_include(includes, seen, base);
        std::error_code ec;
        if (std::filesystem::is_directory(base, ec)) {
            for (std::filesystem::directory_iterator it(base, ec);
                 !ec && it != std::filesystem::directory_iterator{};
                 it.increment(ec)) {
                if (it->is_directory(ec)) {
                    add_existing_include(includes, seen, it->path());
                    add_existing_include(includes, seen, it->path() / "include");
                    add_existing_include(includes, seen, it->path() / "utility");
                }
            }
        }
    }

    return includes;
}

std::vector<std::filesystem::path> inherited_waf_includes(
    const std::vector<std::filesystem::path>& roots,
    const std::map<std::string, std::vector<std::filesystem::path>>& includes_by_root,
    const std::filesystem::path& owner) {
    std::vector<std::filesystem::path> includes;
    std::set<std::string> seen;
    for (const auto& root : roots) {
        if (!path_has_prefix(owner, root)) {
            continue;
        }
        const auto found = includes_by_root.find(root.string());
        if (found == includes_by_root.end()) {
            continue;
        }
        for (const auto& include : found->second) {
            add_existing_include(includes, seen, include);
        }
    }
    return includes;
}

std::vector<std::filesystem::path> waf_roots(const ProjectScanResult& scan) {
    std::set<std::string> roots;
    for (const auto& file : scan.files) {
        if (file.kind == ProjectFileKind::BuildFile && file.path.filename() == "wscript") {
            roots.insert((scan.root / file.path).parent_path().lexically_normal().string());
        }
    }
    std::vector<std::filesystem::path> result;
    result.reserve(roots.size());
    for (const auto& root : roots) {
        result.emplace_back(root);
    }
    return result;
}

std::optional<std::filesystem::path> owning_waf_root(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& source) {
    std::optional<std::filesystem::path> best;
    std::size_t best_size = 0;
    for (const auto& root : roots) {
        if (!path_has_prefix(source, root)) {
            continue;
        }
        const auto size = root.string().size();
        if (!best || size > best_size) {
            best = root;
            best_size = size;
        }
    }
    return best;
}

bool should_skip_waf_source(const std::filesystem::path& relative) {
    if (is_ignored_dependency_path(relative)) {
        return true;
    }
    for (const auto& part : relative) {
        const auto name = part.string();
        if (name == "build" || name == "docs") {
            return true;
        }
    }
    return false;
}

void emulate_waf_projects(BuildEmulationResult& result,
                          std::set<std::string>& includes,
                          const ProjectScanResult& scan) {
    const auto roots = waf_roots(scan);
    std::map<std::string, std::vector<std::filesystem::path>> includes_by_root;
    for (const auto& root : roots) {
        const auto wscript_text = read_text_file(root / "wscript");
        includes_by_root[root.string()] = parse_waf_include_tokens(root, wscript_text);
    }

    for (const auto& file : scan.files) {
        if (!is_native_source_kind(file.kind) || should_skip_waf_source(file.path)) {
            continue;
        }

        const auto source = (scan.root / file.path).lexically_normal();
        const auto root = owning_waf_root(roots, source);
        if (!root) {
            continue;
        }

        std::vector<std::string> arguments;
        arguments.emplace_back(file.kind == ProjectFileKind::CSource ? "cc" : "c++");
        arguments.emplace_back(file.kind == ProjectFileKind::CSource ? "-std=gnu11" : "-std=gnu++17");

        const auto script_includes = inherited_waf_includes(roots, includes_by_root, *root);
        for (const auto& include : waf_include_roots(
                 *root,
                 source,
                 script_includes)) {
            includes.insert(include.string());
            arguments.emplace_back("-I" + include.string());
        }

        arguments.emplace_back("-c");
        arguments.emplace_back(source.string());
        arguments.emplace_back("-o");
        const auto relative_source = std::filesystem::relative(source, scan.root);
        arguments.emplace_back((scan.root / "build/index-waf" / relative_source).string() + ".o");

        record_build_system_command(result, source, *root, std::move(arguments), "waf");
    }
}

}  // namespace qt::index::build_emulator_internal
