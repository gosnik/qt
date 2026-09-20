#include "index/compilation_database.hpp"

#include "index/build_metadata.hpp"
#include "index/cancellation.hpp"
#include "index/gitignore.hpp"
#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <ostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace qt::index {
namespace {

bool is_inferred_native_translation_unit(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource || kind == ProjectFileKind::CxxSource;
}

bool is_native_source_path(const std::filesystem::path& source) {
    const auto ext = lowercase_ascii(source.extension().string());
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".c++" ||
           ext == ".ino";
}

using json = nlohmann::json;

void low_priority_checkpoint(bool low_priority, std::size_t operations) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    if (operations % 64U == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::filesystem::path object_path_for(const std::filesystem::path& relative_source) {
    std::filesystem::path output = ".index/objects";
    output /= relative_source;
    output += ".o";
    return output;
}

std::filesystem::path relative_source_for_output(const std::filesystem::path& root,
                                                 const std::filesystem::path& source) {
    const auto normalized_source = source.lexically_normal();
    if (path_has_prefix(normalized_source, root)) {
        std::error_code ec;
        auto relative = std::filesystem::relative(normalized_source, root, ec);
        if (!ec && !relative.empty()) {
            return relative;
        }
    }
    return normalized_source.filename();
}

bool source_file_exists(const std::filesystem::path& source) {
    std::error_code ec;
    return std::filesystem::is_regular_file(source, ec);
}

std::string shell_escape_argument(const std::string& value) {
    if (value.empty()) {
        return "''";
    }

    bool plain = true;
    for (const unsigned char ch : value) {
        if (std::isspace(ch) || ch == '\'' || ch == '"' || ch == '\\' || ch == '$' || ch == '`' ||
            ch == ';' || ch == '&' || ch == '|' || ch == '<' || ch == '>' || ch == '(' ||
            ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '*') {
            plain = false;
            break;
        }
    }

    if (plain) {
        return value;
    }

    std::string escaped = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
        } else {
            escaped += ch;
        }
    }
    escaped += "'";
    return escaped;
}

std::string command_from_arguments(const std::vector<std::string>& arguments) {
    std::string command;
    for (const auto& argument : arguments) {
        if (!command.empty()) {
            command += ' ';
        }
        command += shell_escape_argument(argument);
    }
    return command;
}

std::vector<std::string> arguments_with_output_path(std::vector<std::string> arguments,
                                                    const std::string& output) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == "-o" && index + 1 < arguments.size()) {
            arguments[index + 1] = output;
            return arguments;
        }
        if (arguments[index].rfind("-o", 0) == 0 && arguments[index].size() > 2) {
            arguments[index] = "-o" + output;
            return arguments;
        }
    }

    arguments.emplace_back("-o");
    arguments.emplace_back(output);
    return arguments;
}

std::vector<std::string> arguments_without_nix_store_executable(
    std::vector<std::string> arguments) {
    if (!arguments.empty() && is_nix_store_path(arguments.front())) {
        arguments.front() = std::filesystem::path(arguments.front()).filename().string();
    }
    return arguments;
}

bool include_argument_allowed(const std::filesystem::path& directory,
                              const std::string& raw_path,
                              const GitIgnoreMatcher& gitignore,
                              std::unordered_map<std::string, bool>& cache) {
    if (raw_path.empty() || raw_path.find('$') != std::string::npos) {
        return true;
    }
    const auto include_path = normalize_path(directory, raw_path);
    const auto key = include_path.string();
    const auto cached = cache.find(key);
    if (cached != cache.end()) {
        return cached->second;
    }
    bool dependency_include = false;
    for (const auto& part : include_path) {
        const auto name = part.string();
        if (name == "libdeps" || name == "boost_1_86_0") {
            dependency_include = true;
            break;
        }
    }
    const bool allowed =
        !is_nix_store_path(include_path) && (dependency_include || !gitignore.ignores(include_path, true));
    cache.emplace(key, allowed);
    return allowed;
}

std::vector<std::string> arguments_without_ignored_includes(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& directory,
    const GitIgnoreMatcher& gitignore,
    std::unordered_map<std::string, bool>& include_allow_cache) {
    std::vector<std::string> filtered;
    filtered.reserve(arguments.size());

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if ((argument == "-I" || argument == "-isystem" || argument == "/I") &&
            index + 1 < arguments.size()) {
            if (include_argument_allowed(directory, arguments[index + 1], gitignore,
                                         include_allow_cache)) {
                filtered.push_back(argument);
                filtered.push_back(arguments[index + 1]);
            }
            ++index;
            continue;
        }
        if (argument.rfind("-I", 0) == 0 && argument.size() > 2) {
            if (include_argument_allowed(directory, argument.substr(2), gitignore,
                                         include_allow_cache)) {
                filtered.push_back(argument);
            }
            continue;
        }
        if (argument.rfind("/I", 0) == 0 && argument.size() > 2) {
            if (include_argument_allowed(directory, argument.substr(2), gitignore,
                                         include_allow_cache)) {
                filtered.push_back(argument);
            }
            continue;
        }
        filtered.push_back(argument);
    }

    return filtered;
}

const char* compiler_for(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource ? "cc" : "c++";
}

const char* standard_for(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource ? "-std=c17" : "-std=c++17";
}

void add_existing_include(std::vector<std::filesystem::path>& selected,
                          std::set<std::string>& seen,
                          const std::filesystem::path& path,
                          const GitIgnoreMatcher& gitignore,
                          bool allow_ignored = false) {
    const auto effective = path.lexically_normal();
    if (is_nix_store_path(effective) || (!allow_ignored && gitignore.ignores(effective, true))) {
        return;
    }
    if (seen.insert(effective.string()).second) {
        selected.push_back(effective);
    }
}

void add_platformio_dependency_includes(std::vector<std::filesystem::path>& selected,
                                        std::set<std::string>& seen,
                                        const std::filesystem::path& root,
                                        const GitIgnoreMatcher& gitignore) {
    const auto libdeps = root / ".pio" / "libdeps" / "native";
    std::error_code ec;
    if (std::filesystem::is_directory(root / "lib" / "boost_1_86_0", ec)) {
        add_existing_include(selected, seen, root / "lib" / "boost_1_86_0", gitignore, true);
    }
    if (!std::filesystem::is_directory(libdeps, ec)) {
        return;
    }

    for (std::filesystem::directory_iterator it(libdeps, ec);
         !ec && it != std::filesystem::directory_iterator{};
         it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const auto dependency_root = it->path();
        add_existing_include(selected, seen, dependency_root / "include", gitignore, true);
        add_existing_include(selected, seen, dependency_root / "src", gitignore, true);

        const auto name = dependency_root.filename().string();
        if (name == "FFS") {
            add_existing_include(selected, seen, dependency_root, gitignore, true);
        }
    }
}

std::vector<std::filesystem::path> include_paths_for(const ProjectScanResult& scan,
                                                     const BuildMetadata& metadata,
                                                     const ProjectFile& file,
                                                     const GitIgnoreMatcher& gitignore) {
    constexpr std::size_t kMaxIncludesPerCommand = 28;

    std::vector<std::filesystem::path> selected;
    std::set<std::string> seen;
    const auto source = (scan.root / file.path).lexically_normal();
    const auto source_dir = source.parent_path();

    for (const auto& path : {scan.root,
                             scan.root / "src",
                             scan.root / "src/include",
                             scan.root / "platforms/common/include",
                             scan.root / "platforms/nuttx/src/px4/common/include",
                             scan.root / "platforms/nuttx/src/px4/rpi/rpi_common/include",
                             scan.root / "platforms/nuttx/src/px4/rpi/rp23xx/include",
                             scan.root / "platforms/nuttx/NuttX/include",
                             scan.root / "platforms/nuttx/NuttX/nuttx/include",
                             scan.root / "platforms/nuttx/NuttX/nuttx/arch/arm/include",
                             scan.root / "platforms/nuttx/NuttX/nuttx/arch/arm/src/rp23xx",
                             scan.root / "platforms/nuttx/NuttX/nuttx/arch/arm/src/rp23xx/hardware",
                             scan.root / "boards",
                             scan.root / "msg"}) {
        add_existing_include(selected, seen, path, gitignore);
    }
    add_platformio_dependency_includes(selected, seen, scan.root, gitignore);

    const auto parts = std::vector<std::filesystem::path>(file.path.begin(), file.path.end());
    if (parts.size() >= 3 && parts[0] == "boards") {
        add_existing_include(selected,
                             seen,
                             scan.root / "boards" / parts[1] / parts[2] / "nuttx-config" /
                                 "include",
                             gitignore);
    }

    for (auto dir = source_dir; !dir.empty() && path_has_prefix(dir, scan.root);
         dir = dir.parent_path()) {
        add_existing_include(selected, seen, dir, gitignore);
        if (dir == scan.root) {
            break;
        }
    }

    for (const auto& include_path : metadata.include_paths) {
        if (selected.size() >= kMaxIncludesPerCommand) {
            break;
        }
        if (path_has_prefix(source_dir, include_path) || path_has_prefix(include_path, source_dir) ||
            include_path.filename() == "include") {
            add_existing_include(selected, seen, include_path, gitignore);
        }
    }

    return selected;
}

std::vector<std::string> defines_for(const BuildMetadata& metadata) {
    constexpr std::size_t kMaxDefinesPerCommand = 8;

    std::vector<std::string> selected;
    selected.reserve(std::min(metadata.defines.size(), kMaxDefinesPerCommand));
    for (const auto& define : metadata.defines) {
        if (selected.size() >= kMaxDefinesPerCommand) {
            break;
        }
        selected.push_back(define);
    }
    return selected;
}

std::vector<std::string> arguments_for(const ProjectScanResult& scan,
                                       const BuildMetadata& metadata,
                                       const ProjectFile& file,
                                       const GitIgnoreMatcher& gitignore) {
    std::vector<std::string> arguments;
    const auto include_paths = include_paths_for(scan, metadata, file, gitignore);
    const auto defines = defines_for(metadata);

    arguments.reserve(6 + include_paths.size() + defines.size());
    arguments.emplace_back(compiler_for(file.kind));
    arguments.emplace_back(standard_for(file.kind));
    for (const auto& define : defines) {
        arguments.emplace_back("-D" + define);
    }
    for (const auto& include_path : include_paths) {
        arguments.emplace_back("-I" + include_path.string());
    }
    arguments.emplace_back("-c");
    arguments.emplace_back((scan.root / file.path).lexically_normal().string());
    arguments.emplace_back("-o");
    arguments.emplace_back(object_path_for(file.path).generic_string());
    return arguments;
}

struct CompileCommandEntry {
    std::filesystem::path directory;
    std::vector<std::string> arguments;
};

bool has_argument(const std::vector<std::string>& arguments, const std::string& value) {
    return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
}

std::vector<std::string> enrich_observed_arguments(const ProjectScanResult& scan,
                                                   const BuildMetadata& metadata,
                                                   const ProjectFile& file,
                                                   std::vector<std::string> arguments,
                                                   const GitIgnoreMatcher& gitignore) {
    const auto include_paths = include_paths_for(scan, metadata, file, gitignore);
    const auto defines = defines_for(metadata);

    auto insert_pos = std::find(arguments.begin(), arguments.end(), "-c");
    for (const auto& define : defines) {
        const auto argument = "-D" + define;
        if (!has_argument(arguments, argument)) {
            insert_pos = arguments.insert(insert_pos, argument);
            ++insert_pos;
        }
    }
    for (const auto& include_path : include_paths) {
        const auto argument = "-I" + include_path.string();
        if (!has_argument(arguments, argument)) {
            insert_pos = arguments.insert(insert_pos, argument);
            ++insert_pos;
        }
    }
    return arguments;
}

CompileCommandEntry command_entry_for(const ProjectScanResult& scan,
                                      const BuildMetadata& metadata,
                                      const ProjectFile& file,
                                      const GitIgnoreMatcher& gitignore) {
    const auto source = (scan.root / file.path).lexically_normal().string();
    const auto observed = metadata.observed_compile_commands.find(source);
    if (observed != metadata.observed_compile_commands.end() &&
        !observed->second.arguments.empty()) {
        return {observed->second.directory,
                enrich_observed_arguments(scan, metadata, file, observed->second.arguments, gitignore)};
    }

    if (!is_inferred_native_translation_unit(file.kind)) {
        return {};
    }

    return {scan.root, arguments_for(scan, metadata, file, gitignore)};
}

}  // namespace

void write_compilation_database(const ProjectScanResult& scan,
                                const BuildMetadata& metadata,
                                std::ostream& out) {
    write_compilation_database(scan, metadata, out, false);
}

void write_compilation_database(const ProjectScanResult& scan,
                                const BuildMetadata& metadata,
                                std::ostream& out,
                                bool low_priority) {
    out << "[\n";
    bool first = true;
    std::unordered_set<std::string> emitted_sources;
    const auto gitignore = GitIgnoreMatcher::load(scan.root);
    std::unordered_map<std::string, bool> include_allow_cache;
    const auto emit_entry = [&](const std::string& source,
                                const std::filesystem::path& directory,
                                const std::vector<std::string>& arguments) {
        if (!source_file_exists(source)) {
            return;
        }
        if (is_nix_store_path(source)) {
            return;
        }
        if (!emitted_sources.insert(source).second) {
            return;
        }
        if (gitignore.ignores(source, false)) {
            return;
        }

        const auto output =
            object_path_for(relative_source_for_output(scan.root, source)).generic_string();
        const auto output_arguments = arguments_without_nix_store_executable(
            arguments_without_ignored_includes(arguments_with_output_path(arguments, output),
                                               directory,
                                               gitignore,
                                               include_allow_cache));
        const auto command = command_from_arguments(output_arguments);

        const json entry{{"directory", directory.string()},
                         {"file", source},
                         {"command", command},
                         {"arguments", output_arguments},
                         {"output", output}};
        if (!first) {
            out << ",\n";
        }
        first = false;
        out << entry.dump(2);
    };

    std::size_t operations = 0;
    for (const auto& file : scan.files) {
        low_priority_checkpoint(low_priority, ++operations);
        if (!is_inferred_native_translation_unit(file.kind)) {
            continue;
        }
        if (is_ignored_inferred_translation_unit_path(file.path)) {
            continue;
        }

        const auto source = (scan.root / file.path).lexically_normal().string();
        const auto command_entry = command_entry_for(scan, metadata, file, gitignore);
        if (command_entry.arguments.empty()) {
            continue;
        }
        emit_entry(source, command_entry.directory, command_entry.arguments);
    }

    for (const auto& [source, observed] : metadata.observed_compile_commands) {
        low_priority_checkpoint(low_priority, ++operations);
        if (!observed.arguments.empty() && is_native_source_path(source)) {
            emit_entry(source, observed.directory, observed.arguments);
        }
    }

    out << "\n]\n";
}

std::size_t write_compilation_database(const ProjectScanResult& scan,
                                       const BuildMetadata& metadata,
                                       const std::filesystem::path& output_path) {
    return write_compilation_database(scan, metadata, output_path, false);
}

std::size_t write_compilation_database(const ProjectScanResult& scan,
                                       const BuildMetadata& metadata,
                                       const std::filesystem::path& output_path,
                                       bool low_priority) {
    std::ofstream out(output_path);
    if (!out) {
        throw std::runtime_error("could not open compilation database output: " +
                                 output_path.string());
    }

    write_compilation_database(scan, metadata, out, low_priority);

    std::unordered_set<std::string> sources;
    const auto gitignore = GitIgnoreMatcher::load(scan.root);
    std::size_t operations = 0;
    for (const auto& file : scan.files) {
        low_priority_checkpoint(low_priority, ++operations);
        if (is_ignored_inferred_translation_unit_path(file.path)) {
            continue;
        }
        const auto source = (scan.root / file.path).lexically_normal();
        if (is_inferred_native_translation_unit(file.kind) && source_file_exists(source) &&
            !is_nix_store_path(source) && !gitignore.ignores(source, false)) {
            sources.insert(source.string());
        }
    }
    for (const auto& [source, observed] : metadata.observed_compile_commands) {
        low_priority_checkpoint(low_priority, ++operations);
        if (!observed.arguments.empty() && is_native_source_path(source) && source_file_exists(source) &&
            !is_nix_store_path(source) && !gitignore.ignores(source, false)) {
            sources.insert(source);
        }
    }
    return sources.size();
}

}  // namespace qt::index
