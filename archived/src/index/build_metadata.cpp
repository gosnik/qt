#include "index/build_metadata.hpp"

#include "index/build_emulator.hpp"
#include "index/cancellation.hpp"
#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>

namespace qt::index {
namespace {

using json = nlohmann::json;

void low_priority_checkpoint(bool low_priority, std::size_t operations = 0) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    if (operations == 0 || operations % 16U == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void scan_tokens_for_metadata(const std::vector<std::string>& tokens,
                              const std::filesystem::path& base,
                              std::set<std::string>& include_paths,
                              std::set<std::string>& defines);

namespace grammar {
namespace dsl = lexy::dsl;

struct word : lexy::token_production {
    static constexpr auto rule = dsl::capture(dsl::token(dsl::while_one(dsl::ascii::graph)));
    static constexpr auto value = lexy::as_string<std::string>;
};

struct words {
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = dsl::terminator(dsl::eof).opt_list(dsl::p<word>);
    static constexpr auto value = lexy::as_list<std::vector<std::string>>;
};

struct assignment_key : lexy::token_production {
    static constexpr auto rule = [] {
        auto lead = dsl::ascii::alpha_underscore;
        auto rest = dsl::ascii::alpha_digit_underscore / dsl::lit_c<'-'> / dsl::lit_c<'.'>;
        return dsl::identifier(lead, rest);
    }();
    static constexpr auto value = lexy::as_string<std::string>;
};

struct assignment_value : lexy::token_production {
    static constexpr auto rule = dsl::capture(dsl::token(dsl::while_one(dsl::ascii::print)));
    static constexpr auto value = lexy::as_string<std::string>;
};

struct assignment {
    static constexpr auto whitespace = dsl::ascii::blank;
    static constexpr auto rule = dsl::p<assignment_key> + (dsl::lit_c<'='> | dsl::lit_c<':'>) +
                                 dsl::p<assignment_value> + dsl::eof;
    static constexpr auto value = lexy::callback<std::pair<std::string, std::string>>(
        [](std::string key, std::string value) {
            return std::make_pair(std::move(key), std::move(value));
        });
};

}  // namespace grammar

bool is_source_or_build_directory_skip(const std::filesystem::path& path) {
    for (const auto& part : path) {
        const auto name = part.string();
        if (name == ".git" || name == ".hg" || name == ".svn" || name == ".pio" ||
            name == ".idea" || name == ".vscode" || name == "node_modules" ||
            name == "__pycache__" || name == "third_party" || name == "vendor" ||
            name == "external") {
            return true;
        }
    }
    return false;
}

std::string strip_inline_comment(std::string line, char comment_marker) {
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == comment_marker) {
            line.resize(index);
            break;
        }
    }
    return line;
}

std::string normalize_token_separators(std::string text) {
    char quote = '\0';
    bool escaped = false;
    for (char& ch : text) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' ||
            ch == ',' || ch == ';') {
            ch = ' ';
        }
    }
    return text;
}

std::vector<std::string> parse_lexy_words(std::string text) {
    text = normalize_token_separators(std::move(text));
    const lexy::string_input input(text.data(), text.size());
    auto result = lexy::parse<grammar::words>(input, lexy::noop);
    if (!result) {
        return {};
    }
    return result.value();
}

std::optional<std::pair<std::string, std::string>> parse_lexy_assignment(std::string line) {
    line = strip_inline_comment(std::move(line), '#');
    const lexy::string_input input(line.data(), line.size());
    auto result = lexy::parse<grammar::assignment>(input, lexy::noop);
    if (!result) {
        return std::nullopt;
    }
    return result.value();
}

std::string strip_quotes(std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

void add_include_path(std::set<std::string>& paths,
                      const std::filesystem::path& base,
                      const std::string& raw_path) {
    std::string value = strip_quotes(trim_ascii(raw_path));
    if (value.empty() || value == "SYSTEM" || value == "BEFORE" || value == "AFTER" ||
        value == "PUBLIC" || value == "PRIVATE" || value == "INTERFACE") {
        return;
    }
    if (value.find("$<") != std::string::npos) {
        return;
    }
    if (value.find("${") != std::string::npos) {
        return;
    }
    if (value.rfind("-I", 0) == 0) {
        value = value.substr(2);
    }
    if (value.empty()) {
        return;
    }

    const auto path = normalize_path(base, value);
    if (std::filesystem::is_directory(path)) {
        paths.insert(path.string());
    }
}

void add_define(std::set<std::string>& defines, std::string value) {
    value = strip_quotes(trim_ascii(std::move(value)));
    if (value.empty()) {
        return;
    }
    if (value.rfind("-D", 0) == 0) {
        value = value.substr(2);
    }
    const auto is_valid_define_lead = [](unsigned char ch) {
        return std::isalpha(ch) || ch == '_';
    };
    if (value.empty() || !is_valid_define_lead(static_cast<unsigned char>(value.front())) ||
        value == "#" || value.find("${") != std::string::npos ||
        value.find("$<") != std::string::npos || value.find('/') != std::string::npos ||
        value.find('<') != std::string::npos || value.find('>') != std::string::npos ||
        value.find('$') != std::string::npos || value.find('\'') != std::string::npos ||
        value.find('"') != std::string::npos ||
        value.rfind("-U", 0) == 0 || value.rfind("-include", 0) == 0 ||
        value.rfind("MODULE_NAME=", 0) == 0) {
        return;
    }
    defines.insert(value);
}

std::string resolve_cmake_variables(std::string value,
                                    const std::filesystem::path& root,
                                    const std::filesystem::path& cmake_dir) {
    const auto replace_all = [&](const std::string& needle, const std::string& replacement) {
        std::size_t pos = 0;
        while ((pos = value.find(needle, pos)) != std::string::npos) {
            value.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    };

    replace_all("${CMAKE_CURRENT_SOURCE_DIR}", cmake_dir.string());
    replace_all("${CMAKE_CURRENT_LIST_DIR}", cmake_dir.string());
    replace_all("${PROJECT_SOURCE_DIR}", root.string());
    replace_all("${CMAKE_SOURCE_DIR}", root.string());
    replace_all("${PX4_SOURCE_DIR}", root.string());
    replace_all("$(PROJECT_SOURCE_DIR)", root.string());
    replace_all("$(CMAKE_SOURCE_DIR)", root.string());
    replace_all("$(PX4_SOURCE_DIR)", root.string());
    return value;
}

enum class BuildFileFamily {
    Unknown,
    CMake,
    Make,
    Ninja,
    Meson,
    Autotools,
    Bazel,
    Buck,
    QMake,
    SCons,
    Premake,
    Xmake,
    Waf,
    GN,
    MSBuild,
    Xcode,
    Embedded,
    Package,
    Nix,
    Container,
};

BuildFileFamily build_file_family(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    const std::string lower_name = lowercase_ascii(name);
    const std::string ext = lowercase_ascii(path.extension().string());

    if (name == "CMakeLists.txt" || name == "CMakeLists.txt.in" || ext == ".cmake" ||
        name == "CMakePresets.json" || name == "CMakeUserPresets.json") {
        return BuildFileFamily::CMake;
    }
    if (name == "Makefile" || name == "makefile" || name == "GNUmakefile" || ext == ".mk") {
        return BuildFileFamily::Make;
    }
    if (lower_name == "build.ninja" || lower_name == "rules.ninja" || ext == ".ninja") {
        return BuildFileFamily::Ninja;
    }
    if (lower_name == "meson.build" || lower_name == "meson.options" ||
        lower_name == "meson_options.txt" || lower_name == "cross-file.txt" ||
        lower_name == "native-file.txt") {
        return BuildFileFamily::Meson;
    }
    if (lower_name == "configure.ac" || lower_name == "configure.in" ||
        lower_name == "makefile.am" || lower_name == "makefile.in" || lower_name == "aclocal.m4") {
        return BuildFileFamily::Autotools;
    }
    if (name == "WORKSPACE" || name == "WORKSPACE.bazel" || name == "MODULE.bazel" ||
        name == "BUILD" || name == "BUILD.bazel" || name == ".bazelrc") {
        return BuildFileFamily::Bazel;
    }
    if (name == "BUCK" || name == "BUCK.v2" || name == ".buckconfig") {
        return BuildFileFamily::Buck;
    }
    if (ext == ".pro" || ext == ".pri" || ext == ".prf") {
        return BuildFileFamily::QMake;
    }
    if (lower_name == "sconstruct" || lower_name == "sconscript") {
        return BuildFileFamily::SCons;
    }
    if (lower_name == "premake4.lua" || lower_name == "premake5.lua") {
        return BuildFileFamily::Premake;
    }
    if (lower_name == "xmake.lua") {
        return BuildFileFamily::Xmake;
    }
    if (lower_name == "wscript") {
        return BuildFileFamily::Waf;
    }
    if (name == "BUILD.gn" || ext == ".gni" || name == ".gn" || name == "args.gn") {
        return BuildFileFamily::GN;
    }
    if (ext == ".sln" || ext == ".vcxproj" || ext == ".filters" || ext == ".props" ||
        ext == ".targets") {
        return BuildFileFamily::MSBuild;
    }
    if (ext == ".xcodeproj" || ext == ".xcworkspace" || name == "project.pbxproj" ||
        name == "Package.swift") {
        return BuildFileFamily::Xcode;
    }
    if (lower_name == "platformio.ini" || lower_name == "library.json" ||
        lower_name == "module.json" || lower_name == "library.properties" ||
        lower_name == "sketch.yaml" || ext == ".px4board" || ext == ".board") {
        return BuildFileFamily::Embedded;
    }
    if (lower_name == "vcpkg.json" || lower_name == "conanfile.txt" ||
        lower_name == "conanfile.py" || lower_name == "conan.lock" ||
        lower_name == "manifest.json" || lower_name == "packages.config") {
        return BuildFileFamily::Package;
    }
    if (lower_name == "flake.nix" || lower_name == "flake.lock" ||
        lower_name == "shell.nix" || lower_name == "default.nix") {
        return BuildFileFamily::Nix;
    }
    if (name == "Dockerfile" || lower_name == "docker-compose.yml" ||
        lower_name == "docker-compose.yaml" || lower_name == "devcontainer.json") {
        return BuildFileFamily::Container;
    }
    return BuildFileFamily::Unknown;
}

void record_compile_command(const ProjectScanResult& scan,
                            const json& object,
                            std::set<std::string>& include_paths,
                            BuildMetadata& metadata) {
    if (!object.is_object()) {
        return;
    }
    const auto directory =
        object.value("directory", scan.root.string());
    const auto file = object.find("file");
    if (file == object.end() || !file->is_string()) {
        return;
    }

    std::vector<std::string> tokens;
    const auto arguments = object.find("arguments");
    if (arguments != object.end() && arguments->is_array()) {
        tokens.reserve(arguments->size());
        for (const auto& argument : *arguments) {
            if (argument.is_string()) {
                tokens.push_back(argument.get<std::string>());
            }
        }
    }
    if (tokens.empty()) {
        const auto command = object.find("command");
        if (command == object.end() || !command->is_string()) {
            return;
        }
        tokens = split_shell_like(command->get<std::string>());
    }
    if (tokens.empty()) {
        return;
    }

    const auto normalized_file = normalize_path(directory, file->get<std::string>()).string();
    auto& observed = metadata.observed_compile_commands[normalized_file];
    if (observed.arguments.empty()) {
        observed.directory = normalize_path(scan.root, directory);
        observed.arguments = tokens;
        observed.backend = "compile_commands";
        observed.origin = "observed";
        observed.confidence = 100;
    }

    for (std::size_t token_index = 0; token_index < tokens.size(); ++token_index) {
        const auto& token = tokens[token_index];
        if (token == "-I" || token == "-isystem") {
            if (token_index + 1 < tokens.size()) {
                add_include_path(include_paths, directory, tokens[++token_index]);
            }
        } else if (token.rfind("-I", 0) == 0) {
            add_include_path(include_paths, directory, token.substr(2));
        }
    }
}

void scan_compile_command_flags(const ProjectScanResult& scan,
                                std::set<std::string>& include_paths,
                                BuildMetadata& metadata) {
    for (const auto& relative_database : scan.existing_compilation_databases) {
        if (relative_database == std::filesystem::path{"compile_commands.json"}) {
            continue;
        }
        const auto database_path = scan.root / relative_database;
        std::ifstream input(database_path);
        if (!input) {
            continue;
        }
        const auto database = json::parse(input, nullptr, false);
        if (!database.is_array()) {
            continue;
        }
        for (const auto& command : database) {
            record_compile_command(scan, command, include_paths, metadata);
        }
    }
}

std::vector<std::string> cmake_call_arguments(const std::string& text,
                                              const std::string& function_name,
                                              std::size_t& search_pos) {
    const auto found = text.find(function_name, search_pos);
    if (found == std::string::npos) {
        search_pos = std::string::npos;
        return {};
    }

    const auto open = text.find('(', found + function_name.size());
    if (open == std::string::npos) {
        search_pos = std::string::npos;
        return {};
    }

    int depth = 1;
    char quote = '\0';
    std::string body;
    for (std::size_t index = open + 1; index < text.size(); ++index) {
        const char ch = text[index];
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
            body.push_back(ch);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            body.push_back(ch);
            continue;
        }
        if (ch == '(') {
            ++depth;
        } else if (ch == ')') {
            --depth;
            if (depth == 0) {
                search_pos = index + 1;
                return parse_lexy_words(body);
            }
        }
        body.push_back(ch);
    }

    search_pos = std::string::npos;
    return {};
}

std::string family_name(BuildFileFamily family) {
    switch (family) {
        case BuildFileFamily::CMake:
            return "CMake";
        case BuildFileFamily::Make:
            return "Make";
        case BuildFileFamily::Ninja:
            return "Ninja";
        case BuildFileFamily::Meson:
            return "Meson";
        case BuildFileFamily::Autotools:
            return "Autotools";
        case BuildFileFamily::Bazel:
            return "Bazel";
        case BuildFileFamily::Buck:
            return "Buck";
        case BuildFileFamily::QMake:
            return "QMake";
        case BuildFileFamily::SCons:
            return "SCons";
        case BuildFileFamily::Premake:
            return "Premake";
        case BuildFileFamily::Xmake:
            return "Xmake";
        case BuildFileFamily::Waf:
            return "Waf";
        case BuildFileFamily::GN:
            return "GN";
        case BuildFileFamily::MSBuild:
            return "MSBuild";
        case BuildFileFamily::Xcode:
            return "Xcode";
        case BuildFileFamily::Embedded:
            return "Embedded";
        case BuildFileFamily::Package:
            return "Package";
        case BuildFileFamily::Nix:
            return "Nix";
        case BuildFileFamily::Container:
            return "Container";
        case BuildFileFamily::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

bool include_key(const std::string& key) {
    const auto lower = lowercase_ascii(key);
    return lower.find("include") != std::string::npos || lower.find("incdir") != std::string::npos ||
           lower.find("cpppath") != std::string::npos ||
           lower.find("header_search_paths") != std::string::npos;
}

bool define_key(const std::string& key) {
    const auto lower = lowercase_ascii(key);
    return lower.find("define") != std::string::npos || lower.find("cppdefines") != std::string::npos ||
           lower == "defs" || lower == "defines";
}

bool metadata_relevant_text(const std::string& text) {
    const auto lower = lowercase_ascii(text);
    return lower.find("include") != std::string::npos || lower.find("incdir") != std::string::npos ||
           lower.find("cpppath") != std::string::npos || lower.find("define") != std::string::npos ||
           lower.find("cppdefines") != std::string::npos || lower.find("cflags") != std::string::npos ||
           lower.find("cxxflags") != std::string::npos || lower.find("-i") != std::string::npos ||
           lower.find("-d") != std::string::npos || lower.find("/i") != std::string::npos ||
           lower.find("/d") != std::string::npos || lower.find("header_search_paths") != std::string::npos ||
           lower.find("preprocessor") != std::string::npos;
}

void scan_tokens_for_metadata(const std::vector<std::string>& tokens,
                              const std::filesystem::path& base,
                              std::set<std::string>& include_paths,
                              std::set<std::string>& defines) {
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token == "-I" || token == "-isystem" || token == "/I") {
            if (index + 1 < tokens.size()) {
                add_include_path(include_paths, base, tokens[++index]);
            }
        } else if (token.rfind("-I", 0) == 0) {
            add_include_path(include_paths, base, token.substr(2));
        } else if (token.rfind("/I", 0) == 0) {
            add_include_path(include_paths, base, token.substr(2));
        } else if (token == "-D" || token == "/D") {
            if (index + 1 < tokens.size()) {
                add_define(defines, tokens[++index]);
            }
        } else if (token.rfind("-D", 0) == 0) {
            add_define(defines, token.substr(2));
        } else if (token.rfind("/D", 0) == 0) {
            add_define(defines, token.substr(2));
        }
    }
}

void scan_assignment_for_metadata(const std::string& line,
                                  const std::filesystem::path& base,
                                  std::set<std::string>& include_paths,
                                  std::set<std::string>& defines) {
    const auto assignment = parse_lexy_assignment(line);
    if (!assignment) {
        return;
    }

    const auto tokens = parse_lexy_words(assignment->second);
    if (include_key(assignment->first)) {
        for (const auto& token : tokens) {
            add_include_path(include_paths, base, token);
        }
    }
    if (define_key(assignment->first)) {
        for (const auto& token : tokens) {
            add_define(defines, token);
        }
    }
    scan_tokens_for_metadata(tokens, base, include_paths, defines);
}

void scan_lexy_token_file(const std::filesystem::path& path,
                          char comment_marker,
                          std::set<std::string>& include_paths,
                          std::set<std::string>& defines) {
    std::ifstream input(path);
    if (!input) {
        return;
    }

    const auto base = path.parent_path();
    std::string line;
    while (std::getline(input, line)) {
        line = strip_inline_comment(std::move(line), comment_marker);
        if (!metadata_relevant_text(line)) {
            continue;
        }
        scan_assignment_for_metadata(line, base, include_paths, defines);
        scan_tokens_for_metadata(parse_lexy_words(line), base, include_paths, defines);
    }
}

void scan_lua_style_file(const std::filesystem::path& path,
                         std::set<std::string>& include_paths,
                         std::set<std::string>& defines) {
    std::ifstream input(path);
    if (!input) {
        return;
    }

    const auto base = path.parent_path();
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find("--");
        if (comment != std::string::npos) {
            line.resize(comment);
        }
        if (!metadata_relevant_text(line)) {
            continue;
        }
        scan_assignment_for_metadata(line, base, include_paths, defines);
        scan_tokens_for_metadata(parse_lexy_words(line), base, include_paths, defines);
    }
}

void scan_xml_like_file(const std::filesystem::path& path,
                        std::set<std::string>& include_paths,
                        std::set<std::string>& defines) {
    const auto text = read_text_file(path);
    if (text.empty()) {
        return;
    }

    const auto base = path.parent_path();
    if (metadata_relevant_text(text)) {
        scan_tokens_for_metadata(parse_lexy_words(text), base, include_paths, defines);
    }

    for (const std::string key : {"AdditionalIncludeDirectories",
                                  "IncludePath",
                                  "HEADER_SEARCH_PATHS",
                                  "GCC_PREPROCESSOR_DEFINITIONS",
                                  "PreprocessorDefinitions"}) {
        std::size_t pos = 0;
        while ((pos = text.find(key, pos)) != std::string::npos) {
            const auto close = text.find('<', pos + key.size());
            const auto open = text.rfind('>', pos);
            if (open != std::string::npos && close != std::string::npos && open < close) {
                const auto tokens = parse_lexy_words(text.substr(open + 1, close - open - 1));
                if (include_key(key)) {
                    for (const auto& token : tokens) {
                        add_include_path(include_paths, base, token);
                    }
                } else {
                    for (const auto& token : tokens) {
                        add_define(defines, token);
                    }
                }
            }
            pos += key.size();
        }
    }
}

void scan_cmake_file(const ProjectScanResult& scan,
                     const std::filesystem::path& relative_path,
                     std::set<std::string>& include_paths,
                     std::set<std::string>& defines) {
    const auto path = scan.root / relative_path;
    auto text = read_text_file(path);
    if (text.empty()) {
        return;
    }

    const auto cmake_dir = path.parent_path();
    for (const std::string function_name : {"include_directories", "target_include_directories"}) {
        std::size_t pos = 0;
        while (pos != std::string::npos) {
            auto args = cmake_call_arguments(text, function_name, pos);
            if (args.empty()) {
                continue;
            }
            const std::size_t first_include_arg = function_name == "target_include_directories" ? 1 : 0;
            for (std::size_t index = first_include_arg; index < args.size(); ++index) {
                const auto resolved = resolve_cmake_variables(args[index], scan.root, cmake_dir);
                add_include_path(include_paths, cmake_dir, resolved);
            }
        }
    }

    for (const std::string function_name : {"add_definitions", "add_compile_definitions"}) {
        std::size_t pos = 0;
        while (pos != std::string::npos) {
            auto args = cmake_call_arguments(text, function_name, pos);
            for (auto& arg : args) {
                add_define(defines, resolve_cmake_variables(arg, scan.root, cmake_dir));
            }
        }
    }
}

void scan_build_file_by_family(const ProjectScanResult& scan,
                               const std::filesystem::path& relative_path,
                               BuildFileFamily family,
                               std::set<std::string>& include_paths,
                               std::set<std::string>& defines) {
    const auto path = scan.root / relative_path;
    switch (family) {
        case BuildFileFamily::CMake:
            scan_cmake_file(scan, relative_path, include_paths, defines);
            scan_lexy_token_file(path, '#', include_paths, defines);
            break;
        case BuildFileFamily::Make:
        case BuildFileFamily::Ninja:
        case BuildFileFamily::Meson:
        case BuildFileFamily::Autotools:
        case BuildFileFamily::Bazel:
        case BuildFileFamily::Buck:
        case BuildFileFamily::QMake:
        case BuildFileFamily::SCons:
        case BuildFileFamily::GN:
        case BuildFileFamily::Embedded:
        case BuildFileFamily::Package:
        case BuildFileFamily::Nix:
        case BuildFileFamily::Container:
            scan_lexy_token_file(path, '#', include_paths, defines);
            break;
        case BuildFileFamily::Premake:
        case BuildFileFamily::Xmake:
            scan_lua_style_file(path, include_paths, defines);
            break;
        case BuildFileFamily::Waf:
            scan_lexy_token_file(path, '#', include_paths, defines);
            break;
        case BuildFileFamily::MSBuild:
        case BuildFileFamily::Xcode:
            scan_xml_like_file(path, include_paths, defines);
            break;
        case BuildFileFamily::Unknown:
            break;
    }
}

std::size_t build_file_scan_limit(BuildFileFamily family) {
    switch (family) {
        case BuildFileFamily::CMake:
            return 400;
        case BuildFileFamily::Make:
            return 300;
        case BuildFileFamily::Ninja:
            return 200;
        case BuildFileFamily::MSBuild:
        case BuildFileFamily::Xcode:
            return 100;
        case BuildFileFamily::Unknown:
            return 0;
        default:
            return 120;
    }
}

bool should_scan_build_metadata_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    constexpr std::uintmax_t kMaxBuildMetadataFileSize = 512U * 1024U;
    return size <= kMaxBuildMetadataFileSize;
}

void scan_conventional_include_dirs(const ProjectScanResult& scan, std::set<std::string>& paths) {
    const std::vector<std::filesystem::path> seed_paths{
        scan.root,
        scan.root / "src",
        scan.root / "src/include",
        scan.root / "boards",
        scan.root / "platforms/common/include",
        scan.root / "platforms/nuttx/src/px4/common/include",
        scan.root / "platforms/posix/include",
        scan.root / "msg",
    };

    for (const auto& path : seed_paths) {
        if (std::filesystem::is_directory(path)) {
            paths.insert(std::filesystem::weakly_canonical(path).string());
        }
    }

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             scan.root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        if (is_source_or_build_directory_skip(it->path())) {
            it.disable_recursion_pending();
            continue;
        }
        const auto name = it->path().filename().string();
        if (name == "include" || name == "inc") {
            paths.insert(std::filesystem::weakly_canonical(it->path()).string());
        }
    }
}

}  // namespace

BuildMetadata scan_build_metadata(const ProjectScanResult& scan) {
    return scan_build_metadata(scan, false);
}

BuildMetadata scan_build_metadata(const ProjectScanResult& scan, bool low_priority) {
    BuildMetadata metadata;
    std::set<std::string> include_paths;
    std::set<std::string> defines;

    scan_compile_command_flags(scan, include_paths, metadata);
    low_priority_checkpoint(low_priority);
    const auto emulation = emulate_build_systems(scan, low_priority);
    low_priority_checkpoint(low_priority);
    for (const auto& [source, command] : emulation.compile_commands) {
        auto& observed = metadata.observed_compile_commands[source];
        if (observed.arguments.empty()) {
            observed = command;
        }
    }
    for (const auto& path : emulation.include_paths) {
        include_paths.insert(path.string());
    }
    defines.insert(emulation.defines.begin(), emulation.defines.end());
    metadata.emulated_edges_by_backend = emulation.edges_by_backend;
    scan_conventional_include_dirs(scan, include_paths);
    low_priority_checkpoint(low_priority);

    std::map<BuildFileFamily, std::size_t> scanned_by_family;
    std::size_t visited = 0;
    for (const auto& file : scan.files) {
        low_priority_checkpoint(low_priority, ++visited);
        if (file.kind != ProjectFileKind::BuildFile) {
            continue;
        }

        const auto family = build_file_family(file.path);
        if (family == BuildFileFamily::Unknown) {
            continue;
        }

        auto& scanned_count = scanned_by_family[family];
        if (scanned_count >= build_file_scan_limit(family)) {
            continue;
        }

        const auto full_path = scan.root / file.path;
        if (!should_scan_build_metadata_file(full_path)) {
            continue;
        }

        ++scanned_count;
        ++metadata.build_files_scanned_by_family[family_name(family)];
        scan_build_file_by_family(scan, file.path, family, include_paths, defines);
        low_priority_checkpoint(low_priority);
    }

    for (const auto& path : include_paths) {
        metadata.include_paths.emplace_back(path);
    }
    metadata.defines.assign(defines.begin(), defines.end());
    return metadata;
}

std::string build_metadata_summary(const BuildMetadata& metadata) {
    std::string summary = "include_paths=" + std::to_string(metadata.include_paths.size()) +
                          " defines=" + std::to_string(metadata.defines.size()) +
                          " observed_commands=" +
                          std::to_string(metadata.observed_compile_commands.size()) +
                          " emulated_edges=";
    bool first_edge = true;
    for (const auto& [backend, count] : metadata.emulated_edges_by_backend) {
        if (!first_edge) {
            summary += ',';
        }
        first_edge = false;
        summary += backend + ':' + std::to_string(count);
    }
    summary += " build_files=";
    bool first = true;
    for (const auto& [family, count] : metadata.build_files_scanned_by_family) {
        if (!first) {
            summary += ',';
        }
        first = false;
        summary += family + ':' + std::to_string(count);
    }
    return summary;
}

}  // namespace qt::index
