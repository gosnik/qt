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

using PlatformIOSections = std::map<std::string, std::map<std::string, std::string>>;

PlatformIOSections parse_platformio_ini(const std::filesystem::path& path) {
    PlatformIOSections sections;
    std::ifstream input(path);
    if (!input) {
        return sections;
    }

    std::string section;
    std::string current_key;
    std::string line;
    while (std::getline(input, line)) {
        const bool continuation =
            !current_key.empty() && !line.empty() &&
            std::isspace(static_cast<unsigned char>(line.front()));
        auto stripped = strip_ini_comment(line);
        if (stripped.empty()) {
            continue;
        }
        if (stripped.front() == '[' && stripped.back() == ']') {
            section = trim_ascii(stripped.substr(1, stripped.size() - 2));
            current_key.clear();
            continue;
        }
        if (section.empty()) {
            continue;
        }
        if (continuation) {
            auto& value = sections[section][current_key];
            if (!value.empty()) {
                value += ' ';
            }
            value += stripped;
            continue;
        }
        const auto assignment = parse_assignment(stripped);
        if (!assignment) {
            continue;
        }
        current_key = assignment->first;
        auto& value = sections[section][current_key];
        if (!value.empty()) {
            value += ' ';
        }
        value += assignment->second;
    }
    return sections;
}

std::string platformio_expand_value(const PlatformIOSections& sections,
                                    const std::string& current_section,
                                    std::string value,
                                    int depth = 0) {
    if (depth > 12) {
        return value;
    }
    for (std::size_t pos = 0; pos < value.size();) {
        const auto start = value.find("${", pos);
        if (start == std::string::npos) {
            break;
        }
        const auto end = value.find('}', start + 2);
        if (end == std::string::npos) {
            break;
        }
        const auto key = value.substr(start + 2, end - start - 2);
        std::string section_name;
        std::string option_name;
        const auto dot = key.find('.');
        if (dot == std::string::npos) {
            section_name = current_section;
            option_name = key;
        } else {
            section_name = key.substr(0, dot);
            option_name = key.substr(dot + 1);
            if (section_name == "this") {
                section_name = current_section;
            }
        }
        if (option_name == "__env__" && current_section.rfind("env:", 0) == 0) {
            const auto env_name = current_section.substr(4);
            value.replace(start, end - start + 1, env_name);
            pos = start + env_name.size();
            continue;
        }
        const auto section = sections.find(section_name);
        if (section == sections.end()) {
            pos = end + 1;
            continue;
        }
        const auto option = section->second.find(option_name);
        if (option == section->second.end()) {
            pos = end + 1;
            continue;
        }
        const auto replacement =
            platformio_expand_value(sections, section_name, option->second, depth + 1);
        value.replace(start, end - start + 1, replacement);
        pos = start + replacement.size();
    }
    return value;
}

std::vector<std::string> platformio_default_env_sections(const PlatformIOSections& sections) {
    std::vector<std::string> result;
    const auto platformio = sections.find("platformio");
    if (platformio != sections.end()) {
        const auto default_envs = platformio->second.find("default_envs");
        if (default_envs != platformio->second.end()) {
            for (auto env : split_shell_like(default_envs->second)) {
                env.erase(std::remove(env.begin(), env.end(), ','), env.end());
                env = trim_ascii(std::move(env));
                if (!env.empty()) {
                    result.push_back("env:" + env);
                }
            }
        }
    }
    if (result.empty()) {
        for (const auto& [section, values] : sections) {
            (void)values;
            if (section.rfind("env:", 0) == 0) {
                result.push_back(section);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::string platformio_combined_value(const PlatformIOSections& sections,
    const std::string& env_section,
    const std::string& key) {
    std::string value;
    const std::vector<std::string> section_names{"env", env_section};
    for (const std::string& section_name : section_names) {
        const auto section = sections.find(section_name);
        if (section == sections.end()) {
            continue;
        }
        const auto option = section->second.find(key);
        if (option == section->second.end()) {
            continue;
        }
        const auto expanded = platformio_expand_value(sections, section_name, option->second);
        if (!value.empty()) {
            value += ' ';
        }
        value += expanded;
    }
    return value;
}

std::filesystem::path platformio_filter_to_glob(const std::filesystem::path& project_root,
                                                std::string pattern) {
    const auto source_root = project_root / "src";
    pattern = trim_ascii(std::move(pattern));
    if (pattern.rfind("../", 0) == 0) {
        return (source_root / pattern).lexically_normal();
    }
    if (!pattern.empty() && pattern.front() == '/') {
        return std::filesystem::path(pattern).lexically_normal();
    }
    if (pattern == "*" || pattern.rfind("**", 0) == 0) {
        return (source_root / pattern).lexically_normal();
    }
    return (source_root / pattern).lexically_normal();
}

bool platformio_pattern_matches(const std::filesystem::path& project_root,
                                const std::filesystem::path& source,
                                const std::string& pattern) {
    const auto glob = platformio_filter_to_glob(project_root, pattern).string();
    const auto text = source.string();
    std::size_t pattern_index = 0;
    std::size_t text_index = 0;
    std::size_t star_index = std::string::npos;
    std::size_t star_text_index = 0;

    while (text_index < text.size()) {
        if (pattern_index < glob.size() &&
            (glob[pattern_index] == '?' || glob[pattern_index] == text[text_index])) {
            ++pattern_index;
            ++text_index;
        } else if (pattern_index < glob.size() && glob[pattern_index] == '*') {
            star_index = pattern_index++;
            star_text_index = text_index;
        } else if (star_index != std::string::npos) {
            pattern_index = star_index + 1;
            text_index = ++star_text_index;
        } else {
            return false;
        }
    }

    while (pattern_index < glob.size() && glob[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == glob.size();
}

bool platformio_source_selected(const std::filesystem::path& project_root,
                                const std::filesystem::path& source,
                                const std::string& filter_text) {
    auto tokens = split_shell_like(filter_text);
    std::vector<std::string> includes;
    std::vector<std::string> excludes;
    for (auto token : tokens) {
        token = trim_ascii(std::move(token));
        if (token.size() < 4 || token[1] != '<' || token.back() != '>') {
            continue;
        }
        auto pattern = token.substr(2, token.size() - 3);
        if (token.front() == '+') {
            includes.push_back(std::move(pattern));
        } else if (token.front() == '-') {
            excludes.push_back(std::move(pattern));
        }
    }

    bool included = includes.empty();
    for (const auto& pattern : includes) {
        if (platformio_pattern_matches(project_root, source, pattern)) {
            included = true;
            break;
        }
    }
    if (!included) {
        return false;
    }
    for (const auto& pattern : excludes) {
        if (platformio_pattern_matches(project_root, source, pattern)) {
            return false;
        }
    }
    return true;
}

bool should_skip_platformio_libdep_dir(const std::filesystem::path& path) {
    const auto name = lowercase_ascii(path.filename().string());
    return name == ".git" || name == ".github" || name == ".vscode" || name == "example" ||
           name == "examples" || name == "test" || name == "tests" || name == "docs" ||
           name == "doc" || name == "scripts" || name == "extras";
}

std::vector<std::filesystem::path> platformio_libdep_roots(const std::filesystem::path& project_root,
                                                           const std::string& env_name) {
    std::vector<std::filesystem::path> roots;
    const auto libdeps_root = project_root / ".pio" / "libdeps" / env_name;
    std::error_code ec;
    if (!std::filesystem::is_directory(libdeps_root, ec)) {
        return roots;
    }

    for (std::filesystem::directory_iterator it(
             libdeps_root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator{};
         it.increment(ec)) {
        if (it->is_directory(ec) && !should_skip_platformio_libdep_dir(it->path())) {
            roots.push_back(it->path().lexically_normal());
        }
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

std::vector<std::filesystem::path> platformio_libdep_include_roots(
    const std::vector<std::filesystem::path>& lib_roots) {
    std::vector<std::filesystem::path> include_paths;
    std::set<std::string> seen;
    for (const auto& root : lib_roots) {
        add_existing_include(include_paths, seen, root / "include");
        add_existing_include(include_paths, seen, root / "src");
        add_existing_include(include_paths, seen, root);
    }
    return include_paths;
}

std::vector<std::filesystem::path> platformio_libdep_sources(
    const std::vector<std::filesystem::path>& lib_roots) {
    std::vector<std::filesystem::path> sources;
    std::error_code ec;
    for (const auto& root : lib_roots) {
        if (std::filesystem::is_directory(root / "src", ec)) {
            for (std::filesystem::recursive_directory_iterator it(
                     root / "src",
                     std::filesystem::directory_options::skip_permission_denied,
                     ec);
                 !ec && it != std::filesystem::recursive_directory_iterator{};
                 it.increment(ec)) {
                if (it->is_directory(ec)) {
                    if (should_skip_platformio_libdep_dir(it->path())) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it->is_regular_file(ec) && is_source_path(it->path())) {
                    sources.push_back(it->path().lexically_normal());
                }
            }
            continue;
        }

        for (std::filesystem::directory_iterator it(
                 root, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator{};
             it.increment(ec)) {
            if (it->is_regular_file(ec) && is_source_path(it->path())) {
                sources.push_back(it->path().lexically_normal());
            }
        }
    }
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
}

void append_platformio_include_paths(std::vector<std::string>& arguments,
                                     std::set<std::string>& includes,
                                     const std::vector<std::filesystem::path>& include_paths) {
    for (const auto& include : include_paths) {
        includes.insert(include.string());
        arguments.emplace_back("-I" + include.string());
    }
}

void record_platformio_source(BuildEmulationResult& result,
                              std::set<std::string>& includes,
                              const std::filesystem::path& project_root,
                              const std::string& env_name,
                              const std::filesystem::path& source,
                              const std::vector<std::string>& flag_tokens,
                              const std::vector<std::filesystem::path>& include_paths,
                              const std::string& backend) {
    std::vector<std::string> arguments;
    arguments.emplace_back(is_c_source_path(source) ? "cc" : "c++");
    arguments.emplace_back(is_c_source_path(source) ? "-std=gnu11" : "-std=gnu++20");
    append_compile_tokens(arguments, flag_tokens, project_root);
    append_platformio_include_paths(arguments, includes, include_paths);

    arguments.emplace_back("-c");
    arguments.emplace_back(source.string());
    arguments.emplace_back("-o");
    const auto relative_source = std::filesystem::relative(source, project_root);
    arguments.emplace_back((project_root / "build" / "index-platformio" / env_name /
                            relative_source)
                               .string() +
                           ".o");

    record_build_system_command(result, source, project_root, std::move(arguments), backend);
}

void emulate_platformio_project(BuildEmulationResult& result,
                                std::set<std::string>& includes,
                                std::set<std::string>& defines,
                                const ProjectScanResult& scan,
                                const std::filesystem::path& platformio_ini) {
    const auto sections = parse_platformio_ini(platformio_ini);
    const auto env_sections = platformio_default_env_sections(sections);
    if (env_sections.empty()) {
        return;
    }

    const auto project_root = platformio_ini.parent_path();
    for (const auto& env_section : env_sections) {
        const auto env_name = env_section.rfind("env:", 0) == 0 ? env_section.substr(4) : env_section;
        const auto flag_text = platformio_combined_value(sections, env_section, "build_flags");
        const auto lib_deps_text = platformio_combined_value(sections, env_section, "lib_deps");
        const auto filter_text = platformio_combined_value(sections, env_section, "build_src_filter");
        auto flag_tokens = split_shell_like(flag_text);
        flag_tokens.erase(std::remove_if(flag_tokens.begin(),
                                         flag_tokens.end(),
                                         [](const std::string& token) {
                                             return token.find('$') != std::string::npos ||
                                                    token.find("-Wl,") == 0 ||
                                                    token.find("-L") == 0;
                                         }),
                          flag_tokens.end());
        scan_tokens_for_metadata(flag_tokens, project_root, includes, defines);

        std::error_code libdeps_ec;
        const auto generated_libdeps_root = project_root / ".pio" / "libdeps" / env_name;
        const bool should_scan_libdeps =
            !trim_ascii(lib_deps_text).empty() ||
            std::filesystem::is_directory(generated_libdeps_root, libdeps_ec);
        const auto libdep_roots = should_scan_libdeps
                                      ? platformio_libdep_roots(project_root, env_name)
                                      : std::vector<std::filesystem::path>{};
        const auto libdep_includes = platformio_libdep_include_roots(libdep_roots);

        for (const auto& file : scan.files) {
            if (!is_native_source_kind(file.kind) || is_ignored_dependency_path(file.path)) {
                continue;
            }
            const auto source = (scan.root / file.path).lexically_normal();
            if (!path_has_prefix(source, project_root)) {
                continue;
            }
            if (!filter_text.empty() &&
                !platformio_source_selected(project_root, source, filter_text)) {
                continue;
            }

            std::vector<std::filesystem::path> source_includes;
            std::set<std::string> seen_source_includes;
            add_existing_include(source_includes, seen_source_includes, project_root);
            add_existing_include(source_includes, seen_source_includes, project_root / "src");
            add_existing_include(source_includes, seen_source_includes, project_root / "include");
            add_existing_include(source_includes, seen_source_includes, source.parent_path());
            for (auto dir = source.parent_path(); !dir.empty() && path_has_prefix(dir, project_root);
                 dir = dir.parent_path()) {
                add_existing_include(source_includes, seen_source_includes, dir);
                if (dir == project_root) {
                    break;
                }
            }
            source_includes.insert(source_includes.end(), libdep_includes.begin(), libdep_includes.end());

            record_platformio_source(result,
                                     includes,
                                     project_root,
                                     env_name,
                                     source,
                                     flag_tokens,
                                     source_includes,
                                     "platformio");
        }

        for (const auto& source : platformio_libdep_sources(libdep_roots)) {
            std::vector<std::filesystem::path> source_includes = libdep_includes;
            std::set<std::string> seen_source_includes;
            for (const auto& include : source_includes) {
                seen_source_includes.insert(include.string());
            }
            add_existing_include(source_includes, seen_source_includes, source.parent_path());
            record_platformio_source(result,
                                     includes,
                                     project_root,
                                     env_name,
                                     source,
                                     flag_tokens,
                                     source_includes,
                                     "platformio-libdeps");
        }
    }
}

void emulate_platformio_projects(BuildEmulationResult& result,
                                 std::set<std::string>& includes,
                                 std::set<std::string>& defines,
                                 const ProjectScanResult& scan,
                                 const ProjectIndex& index) {
    for (const auto& platformio_ini : project_build_files(index, "platformio.ini")) {
        emulate_platformio_project(result, includes, defines, scan, platformio_ini);
    }
}

}  // namespace qt::index::build_emulator_internal
