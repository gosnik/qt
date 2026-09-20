#include "qt/tools/package_manager_tool.hpp"

#include "package_manager_internal.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace qt::tools {

namespace {

constexpr const char* kDefaultBuildDirectory = "build";

bool file_exists(const qt::foundation::Path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::vector<std::string> existing_files(
    const qt::foundation::Path& directory,
    std::initializer_list<const char*> filenames) {
    std::vector<std::string> result;
    for (const char* filename : filenames) {
        if (file_exists(directory / filename)) {
            result.emplace_back(filename);
        }
    }
    return result;
}

std::optional<PackageManagerDetection> detect_javascript_manager(
    const qt::foundation::Path& directory) {
    auto pnpm_evidence = existing_files(directory, {"pnpm-lock.yaml"});
    if (!pnpm_evidence.empty()) {
        if (file_exists(directory / "package.json")) {
            pnpm_evidence.emplace_back("package.json");
        }
        return PackageManagerDetection{
            PackageManagerKind::pnpm,
            directory,
            std::move(pnpm_evidence),
        };
    }

    auto yarn_evidence = existing_files(directory, {"yarn.lock"});
    if (!yarn_evidence.empty()) {
        if (file_exists(directory / "package.json")) {
            yarn_evidence.emplace_back("package.json");
        }
        return PackageManagerDetection{
            PackageManagerKind::yarn,
            directory,
            std::move(yarn_evidence),
        };
    }

    auto npm_evidence =
        existing_files(directory, {"package-lock.json", "npm-shrinkwrap.json"});
    if (!npm_evidence.empty()) {
        if (file_exists(directory / "package.json")) {
            npm_evidence.emplace_back("package.json");
        }
        return PackageManagerDetection{
            PackageManagerKind::npm,
            directory,
            std::move(npm_evidence),
        };
    }

    npm_evidence = existing_files(directory, {"package.json"});
    if (!npm_evidence.empty()) {
        return PackageManagerDetection{
            PackageManagerKind::npm,
            directory,
            std::move(npm_evidence),
        };
    }

    return std::nullopt;
}

std::vector<PackageManagerDetection> detect_at_directory(
    const qt::foundation::Path& directory) {
    std::vector<PackageManagerDetection> result;

    if (auto javascript = detect_javascript_manager(directory)) {
        result.push_back(std::move(*javascript));
    }

    auto cargo_evidence = existing_files(directory, {"Cargo.lock", "Cargo.toml"});
    if (!cargo_evidence.empty()) {
        result.push_back(PackageManagerDetection{
            PackageManagerKind::cargo,
            directory,
            std::move(cargo_evidence),
        });
    }

    auto cmake_evidence = existing_files(directory, {"CMakePresets.json", "CMakeLists.txt"});
    if (!cmake_evidence.empty()) {
        result.push_back(PackageManagerDetection{
            PackageManagerKind::cmake,
            directory,
            std::move(cmake_evidence),
        });
    }

    return result;
}

int kind_rank(PackageManagerKind kind) noexcept {
    switch (kind) {
        case PackageManagerKind::pnpm:
            return 0;
        case PackageManagerKind::yarn:
            return 1;
        case PackageManagerKind::npm:
            return 2;
        case PackageManagerKind::cargo:
            return 3;
        case PackageManagerKind::cmake:
            return 4;
    }
    return 5;
}

bool is_javascript_manager(PackageManagerKind kind) noexcept {
    return kind == PackageManagerKind::npm || kind == PackageManagerKind::pnpm ||
           kind == PackageManagerKind::yarn;
}

std::vector<std::string> vector_from_args(std::initializer_list<const char*> args) {
    std::vector<std::string> result;
    result.reserve(args.size());
    for (const char* arg : args) {
        result.emplace_back(arg);
    }
    return result;
}

std::vector<std::string> npm_command_prefix(
    const PackageManagerCommandSettings& settings) {
    if (!settings.npm_command || settings.npm_command->empty()) {
        return vector_from_args({"npm"});
    }
    if (settings.npm_command->front().empty()) {
        return {};
    }
    return *settings.npm_command;
}

std::vector<std::string> append_args(std::vector<std::string> argv,
                                     std::initializer_list<const char*> args) {
    argv.reserve(argv.size() + args.size());
    for (const char* arg : args) {
        argv.emplace_back(arg);
    }
    return argv;
}

std::vector<std::string> append_string_args(std::vector<std::string> argv,
                                            std::initializer_list<std::string> args) {
    argv.reserve(argv.size() + args.size());
    for (const auto& arg : args) {
        argv.push_back(arg);
    }
    return argv;
}

std::string join_command(const std::vector<std::string>& argv) {
    std::string command;
    for (const auto& arg : argv) {
        if (!command.empty()) {
            command += " ";
        }
        command += arg;
    }
    return command;
}

std::optional<std::vector<std::string>> argv_for_action(PackageManagerKind kind,
                                                        const std::string& action,
                                                        const PackageManagerCommandSettings& settings) {
    switch (kind) {
        case PackageManagerKind::npm:
        {
            auto prefix = npm_command_prefix(settings);
            if (prefix.empty()) {
                return std::nullopt;
            }
            if (action == "install") {
                return append_args(std::move(prefix), {"install"});
            }
            if (action == "build" || action == "test" || action == "check") {
                return append_args(std::move(prefix), {"run", action.c_str()});
            }
            break;
        }
        case PackageManagerKind::pnpm:
            if (action == "install") {
                return vector_from_args({"pnpm", "install"});
            }
            if (action == "build" || action == "test" || action == "check") {
                return std::vector<std::string>{"pnpm", "run", action};
            }
            break;
        case PackageManagerKind::yarn:
            if (action == "install") {
                return vector_from_args({"yarn", "install"});
            }
            if (action == "build" || action == "test" || action == "check") {
                return std::vector<std::string>{"yarn", action};
            }
            break;
        case PackageManagerKind::cargo:
            if (action == "install") {
                return vector_from_args({"cargo", "fetch"});
            }
            if (action == "build") {
                return vector_from_args({"cargo", "build"});
            }
            if (action == "test") {
                return vector_from_args({"cargo", "test"});
            }
            if (action == "check") {
                return vector_from_args({"cargo", "check"});
            }
            break;
        case PackageManagerKind::cmake:
            if (action == "install") {
                return vector_from_args({"cmake", "-S", ".", "-B", kDefaultBuildDirectory});
            }
            if (action == "build") {
                return vector_from_args({"cmake", "--build", kDefaultBuildDirectory});
            }
            if (action == "test" || action == "check") {
                return vector_from_args({"ctest", "--test-dir", kDefaultBuildDirectory});
            }
            break;
    }
    return std::nullopt;
}

qt::foundation::Result<qt::foundation::Path> resolve_start_directory(
    const ToolRequest& request, const ToolContext& context,
    const ToolDescriptor& descriptor) {
    qt::foundation::Path path = ".";
    if (request.arguments.contains("path")) {
        const auto path_text = require_string_argument(request, "path", descriptor.name);
        if (!path_text) {
            return path_text.error();
        }
        path = path_text.value();
    }

    if (path.is_relative()) {
        if (context.current_directory) {
            path = *context.current_directory / path;
        } else if (context.workspace_root) {
            path = *context.workspace_root / path;
        }
    }

    auto resolved = qt::foundation::canonicalize_path(path);
    if (!resolved) {
        return resolved.error();
    }

    std::error_code error;
    const auto status = std::filesystem::status(resolved.value(), error);
    if (error) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::io_error,
            qt::foundation::path_for_display(resolved.value()) + ": " + error.message(),
            error.value(),
        };
    }
    if (!std::filesystem::exists(status)) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::not_found,
            qt::foundation::path_for_display(resolved.value()),
        };
    }
    if (std::filesystem::is_regular_file(status)) {
        resolved = qt::foundation::canonicalize_path(resolved.value().parent_path());
    } else if (!std::filesystem::is_directory(status)) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            qt::foundation::path_for_display(resolved.value()) + " is not a directory",
        };
    }

    return resolved;
}

Json to_json_array(const std::vector<std::string>& values) {
    Json result = Json::array();
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

Json to_json_path_array(const std::vector<qt::foundation::Path>& values) {
    Json result = Json::array();
    for (const auto& value : values) {
        result.push_back(qt::foundation::path_for_display(value));
    }
    return result;
}

bool starts_with(const std::string& value, const char* prefix) {
    const std::string prefix_text(prefix);
    return value.rfind(prefix_text, 0) == 0;
}

bool package_offline_mode_enabled() {
    const char* value = std::getenv("PI_OFFLINE");
    return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
}

std::optional<qt::foundation::Path> expand_home_package_path(std::string_view text) {
    if (text.empty() || text.front() != '~') {
        return std::nullopt;
    }

    const char* home_value = std::getenv("HOME");
    if (home_value == nullptr || home_value[0] == '\0') {
        return std::nullopt;
    }

    const qt::foundation::Path home(home_value);
    if (text == "~") {
        return home;
    }
    if (text.size() >= 2 && text[1] == '/') {
        return home / std::string(text.substr(2));
    }
    return home / std::string(text.substr(1));
}

std::optional<std::string> git_source_identity(const std::string& source);

struct GitSourceDetails {
    std::string host;
    std::string path;
    std::string repo;
    std::optional<std::string> ref;
};

bool is_local_package_source(const std::string& source) {
    if (starts_with(source, "npm:")) {
        return false;
    }
    if (git_source_identity(source)) {
        return false;
    }
    return true;
}

std::string normalize_local_package_source_identity(std::string source) {
    std::replace(source.begin(), source.end(), '\\', '/');
    while (source.size() > 1 && source.back() == '/') {
        source.pop_back();
    }
    if (auto expanded = expand_home_package_path(source)) {
        return qt::foundation::path_for_display(expanded->lexically_normal());
    }
    if (source == "./") {
        return ".";
    }
    if (source == "../") {
        return "..";
    }
    return source;
}

qt::foundation::Path package_settings_base_dir(const qt::foundation::Path& cwd,
                                               PackageSourceScope scope) {
    if (cwd.empty() || scope == PackageSourceScope::user) {
        return {};
    }
    return cwd / ".pi";
}

std::string normalize_local_package_source_identity(
    std::string source,
    const qt::foundation::Path& base_dir) {
    std::replace(source.begin(), source.end(), '\\', '/');
    while (source.size() > 1 && source.back() == '/') {
        source.pop_back();
    }
    if (base_dir.empty()) {
        return normalize_local_package_source_identity(std::move(source));
    }

    qt::foundation::Path path =
        expand_home_package_path(source).value_or(qt::foundation::Path(source));
    if (path.is_relative()) {
        path = base_dir / path;
    }
    path = path.lexically_normal();
    auto canonical = qt::foundation::canonicalize_path(path);
    return qt::foundation::path_for_display(canonical ? canonical.value() : path);
}

std::string npm_source_name(const std::string& spec) {
    if (spec.empty()) {
        return spec;
    }
    const auto at = spec.find('@', spec.front() == '@' ? 1U : 0U);
    if (at == std::string::npos) {
        return spec;
    }
    return spec.substr(0, at);
}

bool npm_source_is_pinned(const std::string& spec) {
    return npm_source_name(spec) != spec;
}

std::optional<std::string> npm_spec_from_package_source(const std::string& source) {
    if (!starts_with(source, "npm:")) {
        return std::nullopt;
    }
    const std::string spec = source.substr(4);
    if (spec.empty()) {
        return std::nullopt;
    }
    return spec;
}

std::pair<std::string, std::optional<std::string>> split_git_path_and_ref(
    std::string path) {
    const auto ref_separator = path.find_first_of("@#");
    if (ref_separator == std::string::npos || ref_separator == 0 ||
        ref_separator + 1 >= path.size()) {
        return {std::move(path), std::nullopt};
    }
    auto ref = path.substr(ref_separator + 1);
    path = path.substr(0, ref_separator);
    return {std::move(path), std::move(ref)};
}

std::optional<GitSourceDetails> git_source_details(const std::string& source) {
    std::string value = source;
    const bool has_git_prefix = starts_with(value, "git:");
    if (has_git_prefix) {
        value = value.substr(4);
    } else if (!starts_with(value, "http://") && !starts_with(value, "https://") &&
               !starts_with(value, "ssh://") && !starts_with(value, "git://")) {
        return std::nullopt;
    }

    GitSourceDetails details;
    if (starts_with(value, "git@")) {
        const auto colon = value.find(':');
        if (colon == std::string::npos) {
            return std::nullopt;
        }
        details.host = value.substr(4, colon - 4);
        auto split = split_git_path_and_ref(value.substr(colon + 1));
        details.path = std::move(split.first);
        details.ref = std::move(split.second);
        details.repo = "git@" + details.host + ":" + details.path;
    } else if (value.find("://") != std::string::npos) {
        const auto scheme_end = value.find("://");
        const std::string scheme = value.substr(0, scheme_end);
        std::string rest = value.substr(scheme_end + 3);
        const auto slash = rest.find('/');
        if (slash == std::string::npos) {
            return std::nullopt;
        }
        details.host = rest.substr(0, slash);
        const auto at = details.host.rfind('@');
        std::string authority = details.host;
        if (at != std::string::npos) {
            details.host = details.host.substr(at + 1);
        }
        auto split = split_git_path_and_ref(rest.substr(slash + 1));
        details.path = std::move(split.first);
        details.ref = std::move(split.second);
        details.repo = scheme + "://" + authority + "/" + details.path;
    } else {
        const auto slash = value.find('/');
        if (slash == std::string::npos || !has_git_prefix) {
            return std::nullopt;
        }
        details.host = value.substr(0, slash);
        auto split = split_git_path_and_ref(value.substr(slash + 1));
        details.path = std::move(split.first);
        details.ref = std::move(split.second);
        details.repo = details.host + "/" + details.path;
    }

    if (details.path.size() > 4 &&
        details.path.substr(details.path.size() - 4) == ".git") {
        details.path.resize(details.path.size() - 4);
    }
    while (!details.path.empty() && details.path.front() == '/') {
        details.path.erase(details.path.begin());
    }
    if (details.host.empty() || details.path.empty() ||
        details.path.find('/') == std::string::npos) {
        return std::nullopt;
    }
    return details;
}

qt::foundation::Path git_install_root(const qt::foundation::Path& cwd,
                                      const qt::foundation::Path& agent_home,
                                      PackageSourceScope scope);

qt::foundation::Path git_install_path(const GitSourceDetails& source,
                                      const qt::foundation::Path& cwd,
                                      const qt::foundation::Path& agent_home,
                                      PackageSourceScope scope);

std::optional<std::string> git_source_identity(const std::string& source) {
    const auto details = git_source_details(source);
    if (!details) {
        return std::nullopt;
    }
    return "git:" + details->host + "/" + details->path;
}

bool git_source_is_pinned(const std::string& source) {
    std::string value = source;
    if (starts_with(value, "git:")) {
        value = value.substr(4);
    }

    if (starts_with(value, "git@")) {
        const auto colon = value.find(':');
        if (colon == std::string::npos) {
            return false;
        }
        const std::string path = value.substr(colon + 1);
        const auto ref_separator = path.find_first_of("@#");
        return ref_separator != std::string::npos && ref_separator > 0 &&
               ref_separator + 1 < path.size();
    }

    if (value.find("://") != std::string::npos) {
        const auto scheme_end = value.find("://");
        const std::string rest = value.substr(scheme_end + 3);
        const auto slash = rest.find('/');
        if (slash == std::string::npos) {
            return false;
        }
        const std::string path = rest.substr(slash + 1);
        const auto ref_separator = path.find_first_of("@#");
        return ref_separator != std::string::npos && ref_separator > 0 &&
               ref_separator + 1 < path.size();
    }

    const auto slash = value.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    const std::string path = value.substr(slash + 1);
    const auto ref_separator = path.find_first_of("@#");
    return ref_separator != std::string::npos && ref_separator > 0 &&
           ref_separator + 1 < path.size();
}

std::string package_source_identity(const std::string& source,
                                    const qt::foundation::Path& base_dir = {}) {
    if (starts_with(source, "npm:")) {
        return "npm:" + npm_source_name(source.substr(4));
    }
    if (const auto git_identity = git_source_identity(source)) {
        return *git_identity;
    }
    return "local:" + normalize_local_package_source_identity(source, base_dir);
}

bool package_source_is_update_candidate(const std::string& source) {
    if (starts_with(source, "npm:")) {
        return !npm_source_is_pinned(source.substr(4));
    }
    if (git_source_identity(source)) {
        return !git_source_is_pinned(source);
    }
    if (is_local_package_source(source)) {
        return false;
    }
    return false;
}

std::string package_update_skip_reason(const std::string& source) {
    if (starts_with(source, "npm:") && npm_source_is_pinned(source.substr(4))) {
        return "pinned npm package";
    }
    if (git_source_identity(source) && git_source_is_pinned(source)) {
        return "pinned git ref";
    }
    if (is_local_package_source(source)) {
        return "local package sources are not updated";
    }
    return "package source is not an update candidate";
}

PackageConfiguredSource skipped_update_source(PackageConfiguredSource source) {
    source.update_skip_reason = package_update_skip_reason(source.source);
    return source;
}

PackageConfiguredSource skipped_update_source(PackageConfiguredSource source,
                                              std::string reason) {
    source.update_skip_reason = std::move(reason);
    return source;
}

std::optional<std::string> suggest_npm_prefixed_source(
    const std::string& source,
    const std::vector<PackageConfiguredSource>& configured_packages) {
    if (starts_with(source, "npm:")) {
        return std::nullopt;
    }

    const std::string npm_identity = "npm:" + npm_source_name(source);
    for (const auto& configured : configured_packages) {
        if (starts_with(configured.source, "npm:") &&
            package_source_identity(configured.source) == npm_identity) {
            return configured.source;
        }
    }
    return std::nullopt;
}

std::vector<PackageConfiguredSource> configured_packages_from_settings(
    const PackageManagerCommandSettings& settings) {
    std::vector<PackageConfiguredSource> result;
    result.reserve(settings.user_packages.size() + settings.project_packages.size());
    for (auto source : settings.user_packages) {
        source.scope = PackageSourceScope::user;
        result.push_back(std::move(source));
    }
    for (auto source : settings.project_packages) {
        source.scope = PackageSourceScope::project;
        result.push_back(std::move(source));
    }
    return result;
}

qt::foundation::Path project_npm_path_for_source(const qt::foundation::Path& cwd,
                                                 const std::string& source) {
    return cwd / ".pi" / "npm" / "node_modules" /
           npm_source_name(source.substr(4));
}

std::optional<std::string> existing_installed_path(
    const std::string& source,
    PackageSourceScope scope,
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home) {
    std::vector<qt::foundation::Path> candidates;
    if (starts_with(source, "npm:") && scope == PackageSourceScope::project) {
        candidates.push_back(project_npm_path_for_source(cwd, source));
    } else if (const auto git = git_source_details(source)) {
        if (scope == PackageSourceScope::project || !agent_home.empty()) {
            candidates.push_back(git_install_path(*git, cwd, agent_home, scope));
        }
    } else if (is_local_package_source(source)) {
        qt::foundation::Path base =
            scope == PackageSourceScope::project ? cwd / ".pi" : agent_home;
        qt::foundation::Path path(source);
        if (path.is_relative()) {
            path = base / path;
        }
        candidates.push_back(path);
    }

    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && !error) {
            auto canonical = qt::foundation::canonicalize_path(candidate);
            return qt::foundation::path_for_display(
                canonical ? canonical.value() : candidate);
        }
    }
    return std::nullopt;
}

qt::foundation::Result<std::vector<std::string>> optional_string_array(
    const Json& object,
    const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::vector<std::string>{};
    }
    if (!it->is_array()) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            std::string(key) + " must be an array",
        };
    }
    std::vector<std::string> result;
    for (const auto& value : *it) {
        if (!value.is_string()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                std::string(key) + " entries must be strings",
            };
        }
        result.push_back(value.get<std::string>());
    }
    return result;
}

qt::foundation::Result<std::vector<PackageConfiguredSource>> parse_settings_packages(
    const Json& settings_json,
    PackageSourceScope scope,
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home) {
    const auto packages = settings_json.find("packages");
    if (packages == settings_json.end()) {
        return std::vector<PackageConfiguredSource>{};
    }
    if (!packages->is_array()) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "packages must be an array",
        };
    }

    std::vector<PackageConfiguredSource> result;
    for (const auto& package : *packages) {
        PackageConfiguredSource source;
        source.scope = scope;
        if (package.is_string()) {
            source.source = package.get<std::string>();
        } else if (package.is_object()) {
            const auto source_it = package.find("source");
            if (source_it == package.end() || !source_it->is_string()) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::invalid_argument,
                    "package source must be a string",
                };
            }
            source.source = source_it->get<std::string>();
            source.filtered = true;

            auto extensions = optional_string_array(package, "extensions");
            if (!extensions) {
                return extensions.error();
            }
            auto skills = optional_string_array(package, "skills");
            if (!skills) {
                return skills.error();
            }
            auto prompts = optional_string_array(package, "prompts");
            if (!prompts) {
                return prompts.error();
            }
            auto themes = optional_string_array(package, "themes");
            if (!themes) {
                return themes.error();
            }
            source.extensions = std::move(extensions.value());
            source.skills = std::move(skills.value());
            source.prompts = std::move(prompts.value());
            source.themes = std::move(themes.value());
        } else {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "package entries must be strings or objects",
            };
        }
        source.installed_path =
            existing_installed_path(source.source, scope, cwd, agent_home);
        result.push_back(std::move(source));
    }
    return result;
}

}  // namespace

namespace package_manager_internal {

qt::foundation::Result<Json> read_settings_json_if_present(
    const qt::foundation::Path& path) {
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error)) {
        return Json::object();
    }
    if (exists_error) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::io_error,
            exists_error.message(),
            exists_error.value(),
        };
    }

    const auto content = qt::foundation::read_text_file(path);
    if (!content) {
        return content.error();
    }
    try {
        auto json = Json::parse(content.value());
        if (!json.is_object()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "settings document must be a JSON object",
            };
        }
        return json;
    } catch (const Json::parse_error& error) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            std::string("invalid settings JSON in ") +
                qt::foundation::path_for_display(path) + ": " + error.what(),
        };
    }
}

}  // namespace package_manager_internal

namespace {

std::vector<PackageConfiguredSource> package_scope_vector(
    const PackageManagerCommandSettings& settings, PackageSourceScope scope) {
    return scope == PackageSourceScope::project ? settings.project_packages
                                                : settings.user_packages;
}

std::string scoped_package_source_identity(const std::string& source,
                                           PackageSourceScope scope,
                                           const qt::foundation::Path& cwd) {
    return package_source_identity(source, package_settings_base_dir(cwd, scope));
}

std::vector<std::string>& filters_for_resource(PackageConfiguredSource& source,
                                               PackageResourceKind kind) {
    switch (kind) {
        case PackageResourceKind::extensions:
            return source.extensions;
        case PackageResourceKind::skills:
            return source.skills;
        case PackageResourceKind::prompts:
            return source.prompts;
        case PackageResourceKind::themes:
            return source.themes;
    }
    return source.extensions;
}

std::string strip_filter_prefix(const std::string& pattern) {
    if (!pattern.empty() &&
        (pattern.front() == '!' || pattern.front() == '+' || pattern.front() == '-')) {
        return pattern.substr(1);
    }
    return pattern;
}

std::string resource_pattern_for_settings(
    const qt::foundation::Path& package_root,
    const qt::foundation::Path& resource_path) {
    const auto relative = resource_path.lexically_relative(package_root);
    if (!relative.empty() && relative.native().rfind("..", 0) != 0) {
        return relative.generic_string();
    }
    return resource_path.generic_string();
}

std::string normalize_filter_pattern(std::string pattern) {
    std::replace(pattern.begin(), pattern.end(), '\\', '/');
    if (pattern.rfind("./", 0) == 0) {
        return pattern.substr(2);
    }
    return pattern;
}

bool glob_match_after_starstar(const std::string& text, std::size_t text_index,
                               const std::string& pattern,
                               std::size_t pattern_index);

bool glob_match_from(const std::string& text, std::size_t text_index,
                     const std::string& pattern, std::size_t pattern_index) {
    while (pattern_index < pattern.size()) {
        if (pattern[pattern_index] == '*') {
            const bool double_star = pattern_index + 1 < pattern.size() &&
                                     pattern[pattern_index + 1] == '*';
            if (double_star) {
                return glob_match_after_starstar(text, text_index, pattern,
                                                 pattern_index + 2);
            }

            ++pattern_index;
            for (std::size_t next = text_index; next <= text.size(); ++next) {
                if (next > text_index && text[next - 1] == '/') {
                    break;
                }
                if (glob_match_from(text, next, pattern, pattern_index)) {
                    return true;
                }
            }
            return false;
        }

        if (text_index >= text.size()) {
            return false;
        }
        if (pattern[pattern_index] == '?') {
            if (text[text_index] == '/') {
                return false;
            }
        } else if (pattern[pattern_index] != text[text_index]) {
            return false;
        }
        ++text_index;
        ++pattern_index;
    }

    return text_index == text.size();
}

bool glob_match_after_starstar(const std::string& text, std::size_t text_index,
                               const std::string& pattern,
                               std::size_t pattern_index) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == '/') {
        if (glob_match_from(text, text_index, pattern, pattern_index + 1)) {
            return true;
        }
    }
    for (std::size_t next = text_index; next <= text.size(); ++next) {
        if (glob_match_from(text, next, pattern, pattern_index)) {
            return true;
        }
    }
    return false;
}

bool glob_match(const std::string& text, const std::string& pattern) {
    return glob_match_from(text, 0, normalize_filter_pattern(pattern), 0);
}

std::vector<std::string> resource_match_candidates(
    const std::string& relative_pattern,
    const qt::foundation::Path& resource_path) {
    std::vector<std::string> candidates{
        relative_pattern,
        resource_path.filename().generic_string(),
        resource_path.generic_string(),
    };

    if (resource_path.filename() == "SKILL.md") {
        const auto parent = resource_path.parent_path();
        const auto parent_relative =
            qt::foundation::Path(relative_pattern).parent_path().generic_string();
        if (!parent_relative.empty()) {
            candidates.push_back(parent_relative);
        }
        candidates.push_back(parent.filename().generic_string());
        candidates.push_back(parent.generic_string());
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    return candidates;
}

bool matches_filter_pattern(const std::string& pattern,
                            const std::string& relative_pattern,
                            const qt::foundation::Path& resource_path) {
    const auto candidates =
        resource_match_candidates(relative_pattern, resource_path);
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const std::string& candidate) {
                           return glob_match(candidate, pattern);
                       });
}

bool matches_exact_filter_pattern(const std::string& pattern,
                                  const std::string& relative_pattern,
                                  const qt::foundation::Path& resource_path) {
    const auto normalized = normalize_filter_pattern(pattern);
    const auto candidates =
        resource_match_candidates(relative_pattern, resource_path);
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const std::string& candidate) {
                           return candidate == normalized;
                       });
}

bool resource_enabled_by_filters(const std::vector<std::string>& filters,
                                 const std::string& relative_pattern,
                                 const qt::foundation::Path& resource_path) {
    if (filters.empty()) {
        return true;
    }

    std::vector<std::string> includes;
    std::vector<std::string> excludes;
    std::vector<std::string> force_includes;
    std::vector<std::string> force_excludes;
    for (const auto& filter : filters) {
        if (filter.empty()) {
            continue;
        }
        if (filter.front() == '+') {
            force_includes.push_back(filter.substr(1));
        } else if (filter.front() == '-') {
            force_excludes.push_back(filter.substr(1));
        } else if (filter.front() == '!') {
            excludes.push_back(filter.substr(1));
        } else {
            includes.push_back(filter);
        }
    }

    bool enabled = includes.empty();
    if (!includes.empty()) {
        enabled = std::any_of(includes.begin(), includes.end(),
                              [&](const std::string& filter) {
                                  return matches_filter_pattern(
                                      filter, relative_pattern, resource_path);
                              });
    }
    if (enabled && !excludes.empty()) {
        enabled = !std::any_of(excludes.begin(), excludes.end(),
                               [&](const std::string& filter) {
                                   return matches_filter_pattern(
                                       filter, relative_pattern, resource_path);
                               });
    }
    if (std::any_of(force_includes.begin(), force_includes.end(),
                    [&](const std::string& filter) {
                        return matches_exact_filter_pattern(
                            filter, relative_pattern, resource_path);
                    })) {
        enabled = true;
    }
    if (std::any_of(force_excludes.begin(), force_excludes.end(),
                    [&](const std::string& filter) {
                        return matches_exact_filter_pattern(
                            filter, relative_pattern, resource_path);
                    })) {
        enabled = false;
    }

    return enabled;
}

qt::foundation::Path project_npm_install_root(const qt::foundation::Path& cwd) {
    return cwd / ".pi" / "npm";
}

qt::foundation::Path npm_install_path_for_source(
    const qt::foundation::Path& install_root,
    const std::string& source) {
    return install_root / "node_modules" / npm_source_name(source.substr(4));
}

qt::foundation::Path npm_lockfile_path(const qt::foundation::Path& install_root) {
    return install_root / "package-lock.json";
}

qt::foundation::Path npm_project_manifest_path(const qt::foundation::Path& install_root) {
    return install_root / "package.json";
}

qt::foundation::Path package_cache_ignore_path(const qt::foundation::Path& install_root) {
    return install_root / ".gitignore";
}

qt::foundation::Path git_install_root(const qt::foundation::Path& cwd,
                                      const qt::foundation::Path& agent_home,
                                      PackageSourceScope scope) {
    if (scope == PackageSourceScope::project) {
        return cwd / ".pi" / "git";
    }
    return agent_home / "git";
}

qt::foundation::Path git_install_path(const GitSourceDetails& source,
                                      const qt::foundation::Path& cwd,
                                      const qt::foundation::Path& agent_home,
                                      PackageSourceScope scope) {
    return git_install_root(cwd, agent_home, scope) / source.host / source.path;
}

PackageSourceCommandPlan make_skipped_source_command_plan(
    PackageCommandKind command,
    const std::string& source,
    PackageSourceScope scope,
    std::string reason) {
    PackageSourceCommandPlan skipped;
    skipped.command = command;
    skipped.scope = scope;
    skipped.source = source;
    skipped.skipped = true;
    skipped.reason = std::move(reason);
    if (starts_with(source, "npm:")) {
        skipped.source_type = "npm";
        skipped.source_identity = package_source_identity(source);
    } else if (auto identity = git_source_identity(source)) {
        skipped.source_type = "git";
        skipped.source_identity = std::move(identity);
    } else if (is_local_package_source(source)) {
        skipped.source_type = "local";
        skipped.source_identity = package_source_identity(source);
    }
    return skipped;
}

enum class MutationCondition { unconditional, conditional };

PackageMutationRecord make_mutation_record(
    PackageMutationRecordKind kind,
    std::string operation,
    const std::string& source,
    PackageSourceScope scope,
    std::optional<qt::foundation::Path> path = std::nullopt,
    MutationCondition condition = MutationCondition::unconditional,
    std::optional<std::string> reason = std::nullopt) {
    PackageMutationRecord record;
    record.kind = kind;
    record.operation = std::move(operation);
    record.source = source;
    record.scope = scope;
    record.path = std::move(path);
    record.conditional = condition == MutationCondition::conditional;
    record.reason = std::move(reason);
    return record;
}

void add_project_npm_mutation_records(PackageSourceCommandPlan& plan,
                                      const qt::foundation::Path& install_root,
                                      const qt::foundation::Path& install_path) {
    plan.mutation_records.push_back(make_mutation_record(
        PackageMutationRecordKind::cache, "ensure_directory", plan.source,
        plan.scope, install_root));
    plan.mutation_records.push_back(make_mutation_record(
        PackageMutationRecordKind::cache, "write_if_missing", plan.source,
        plan.scope, package_cache_ignore_path(install_root)));
    plan.mutation_records.push_back(make_mutation_record(
        PackageMutationRecordKind::cache, "write_if_missing", plan.source,
        plan.scope, npm_project_manifest_path(install_root)));
    if (plan.command == PackageCommandKind::remove) {
        plan.mutation_records.push_back(make_mutation_record(
            PackageMutationRecordKind::cache, "remove", plan.source, plan.scope,
            install_path, MutationCondition::conditional));
    } else {
        plan.mutation_records.push_back(make_mutation_record(
            PackageMutationRecordKind::cache,
            plan.command == PackageCommandKind::update ? "update" : "install",
            plan.source, plan.scope, install_path));
    }
    plan.mutation_records.push_back(make_mutation_record(
        PackageMutationRecordKind::lock, "update", plan.source, plan.scope,
        npm_lockfile_path(install_root)));
}

void add_global_npm_mutation_record(PackageSourceCommandPlan& plan) {
    plan.mutation_records.push_back(make_mutation_record(
        PackageMutationRecordKind::cache,
        plan.command == PackageCommandKind::remove
            ? "remove"
            : plan.command == PackageCommandKind::update ? "update" : "install",
        plan.source, plan.scope, std::nullopt, MutationCondition::unconditional,
        "global npm package path is resolved by npm"));
    plan.mutation_records.push_back(make_mutation_record(
        PackageMutationRecordKind::lock, "none", plan.source, plan.scope,
        std::nullopt, MutationCondition::unconditional,
        "global npm installs do not use a pi-managed lockfile"));
}

void add_git_cache_mutation_record(PackageSourceCommandPlan& plan,
                                   const qt::foundation::Path& path,
                                   std::string operation,
                                   MutationCondition condition =
                                       MutationCondition::unconditional) {
    plan.mutation_records.push_back(make_mutation_record(
        PackageMutationRecordKind::cache, std::move(operation), plan.source,
        plan.scope, path, condition));
}

}  // namespace

const char* to_string(PackageManagerKind kind) noexcept {
    switch (kind) {
        case PackageManagerKind::npm:
            return "npm";
        case PackageManagerKind::pnpm:
            return "pnpm";
        case PackageManagerKind::yarn:
            return "yarn";
        case PackageManagerKind::cargo:
            return "cargo";
        case PackageManagerKind::cmake:
            return "cmake";
    }
    return "unknown";
}

const char* to_string(PackageCommandKind command) noexcept {
    switch (command) {
        case PackageCommandKind::install:
            return "install";
        case PackageCommandKind::remove:
            return "remove";
        case PackageCommandKind::update:
            return "update";
        case PackageCommandKind::list:
            return "list";
    }
    return "unknown";
}

const char* to_string(PackageUpdateTargetKind kind) noexcept {
    switch (kind) {
        case PackageUpdateTargetKind::all:
            return "all";
        case PackageUpdateTargetKind::self:
            return "self";
        case PackageUpdateTargetKind::extensions:
            return "extensions";
    }
    return "unknown";
}

const char* to_string(PackageSourceScope scope) noexcept {
    switch (scope) {
        case PackageSourceScope::user:
            return "user";
        case PackageSourceScope::project:
            return "project";
    }
    return "unknown";
}

const char* to_string(PackageResourceKind kind) noexcept {
    switch (kind) {
        case PackageResourceKind::extensions:
            return "extensions";
        case PackageResourceKind::skills:
            return "skills";
        case PackageResourceKind::prompts:
            return "prompts";
        case PackageResourceKind::themes:
            return "themes";
    }
    return "unknown";
}

const char* to_string(PackageMutationRecordKind kind) noexcept {
    switch (kind) {
        case PackageMutationRecordKind::settings:
            return "settings";
        case PackageMutationRecordKind::cache:
            return "cache";
        case PackageMutationRecordKind::lock:
            return "lock";
    }
    return "unknown";
}

const char* to_string(PackageConfigCommandKind command) noexcept {
    switch (command) {
        case PackageConfigCommandKind::list:
            return "list";
        case PackageConfigCommandKind::enable:
            return "enable";
        case PackageConfigCommandKind::disable:
            return "disable";
    }
    return "unknown";
}

std::optional<PackageResourceKind> parse_package_resource_kind(
    const std::string& value) {
    if (value == "extensions" || value == "extension") {
        return PackageResourceKind::extensions;
    }
    if (value == "skills" || value == "skill") {
        return PackageResourceKind::skills;
    }
    if (value == "prompts" || value == "prompt" ||
        value == "prompt-templates" || value == "prompt-template") {
        return PackageResourceKind::prompts;
    }
    if (value == "themes" || value == "theme") {
        return PackageResourceKind::themes;
    }
    return std::nullopt;
}

std::optional<PackageCommandParseResult> parse_package_command_args(
    const std::vector<std::string>& args) {
    if (args.empty()) {
        return std::nullopt;
    }

    PackageCommandKind command;
    if (args.front() == "uninstall") {
        command = PackageCommandKind::remove;
    } else if (args.front() == "install") {
        command = PackageCommandKind::install;
    } else if (args.front() == "remove") {
        command = PackageCommandKind::remove;
    } else if (args.front() == "update") {
        command = PackageCommandKind::update;
    } else if (args.front() == "list") {
        command = PackageCommandKind::list;
    } else {
        return std::nullopt;
    }

    PackageCommandParseResult result;
    result.command = command;

    bool self_flag = false;
    bool extensions_flag = false;
    std::optional<std::string> extension_flag_source;

    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "-h" || arg == "--help") {
            result.help = true;
            continue;
        }

        if (arg == "-l" || arg == "--local") {
            if (command == PackageCommandKind::install ||
                command == PackageCommandKind::remove) {
                result.local = true;
            } else if (!result.invalid_option) {
                result.invalid_option = arg;
            }
            continue;
        }

        if (arg == "--self") {
            if (command == PackageCommandKind::update) {
                self_flag = true;
            } else if (!result.invalid_option) {
                result.invalid_option = arg;
            }
            continue;
        }

        if (arg == "--extensions") {
            if (command == PackageCommandKind::update) {
                extensions_flag = true;
            } else if (!result.invalid_option) {
                result.invalid_option = arg;
            }
            continue;
        }

        if (arg == "--force") {
            if (command == PackageCommandKind::update) {
                result.force = true;
            } else if (!result.invalid_option) {
                result.invalid_option = arg;
            }
            continue;
        }

        if (arg == "--extension") {
            if (command != PackageCommandKind::update) {
                if (!result.invalid_option) {
                    result.invalid_option = arg;
                }
                continue;
            }

            const bool has_value = index + 1 < args.size() &&
                                   !args[index + 1].empty() &&
                                   args[index + 1].front() != '-';
            if (!has_value) {
                if (!result.missing_option_value) {
                    result.missing_option_value = arg;
                }
            } else if (extension_flag_source) {
                if (!result.conflicting_options) {
                    result.conflicting_options =
                        "--extension can only be provided once";
                }
                ++index;
            } else {
                extension_flag_source = args[++index];
            }
            continue;
        }

        if (!arg.empty() && arg.front() == '-') {
            if (!result.invalid_option) {
                result.invalid_option = arg;
            }
            continue;
        }

        if (!result.source) {
            result.source = arg;
        } else if (!result.invalid_argument) {
            result.invalid_argument = arg;
        }
    }

    if (command == PackageCommandKind::update) {
        PackageUpdateTarget target;
        if (extension_flag_source) {
            if (self_flag || extensions_flag) {
                result.conflicting_options =
                    result.conflicting_options.value_or(
                        "--extension cannot be combined with --self or --extensions");
            }
            if (result.source) {
                result.conflicting_options =
                    result.conflicting_options.value_or(
                        "--extension cannot be combined with a positional source");
            }
            target.kind = PackageUpdateTargetKind::extensions;
            target.source = extension_flag_source;
        } else if (result.source) {
            const bool source_is_self =
                *result.source == "self" || *result.source == "pi";
            if (source_is_self) {
                target.kind = extensions_flag ? PackageUpdateTargetKind::all
                                              : PackageUpdateTargetKind::self;
            } else {
                if (extensions_flag || self_flag) {
                    result.conflicting_options =
                        result.conflicting_options.value_or(
                            "positional update targets cannot be combined with --self or --extensions");
                }
                target.kind = PackageUpdateTargetKind::extensions;
                target.source = result.source;
            }
        } else if (self_flag && extensions_flag) {
            target.kind = PackageUpdateTargetKind::all;
        } else if (self_flag) {
            target.kind = PackageUpdateTargetKind::self;
        } else if (extensions_flag) {
            target.kind = PackageUpdateTargetKind::extensions;
        } else {
            target.kind = PackageUpdateTargetKind::all;
        }
        result.update_target = std::move(target);
    }

    return result;
}

std::optional<PackageConfigCommandParseResult> parse_package_config_command_args(
    const std::vector<std::string>& args) {
    if (args.empty() || args.front() != "config") {
        return std::nullopt;
    }

    PackageConfigCommandParseResult result;
    std::size_t index = 1;
    if (index < args.size() && !args[index].empty() &&
        args[index].front() != '-') {
        if (args[index] == "list") {
            result.command = PackageConfigCommandKind::list;
            ++index;
        } else if (args[index] == "enable") {
            result.command = PackageConfigCommandKind::enable;
            ++index;
        } else if (args[index] == "disable") {
            result.command = PackageConfigCommandKind::disable;
            ++index;
        }
    }

    std::vector<std::string> positional;
    for (; index < args.size(); ++index) {
        const std::string& arg = args[index];
        if (arg == "-h" || arg == "--help") {
            result.help = true;
            continue;
        }
        if (arg == "-l" || arg == "--local") {
            if (result.command == PackageConfigCommandKind::enable ||
                result.command == PackageConfigCommandKind::disable) {
                result.scope = PackageSourceScope::project;
            } else if (!result.invalid_option) {
                result.invalid_option = arg;
            }
            continue;
        }
        if (arg == "--package-root") {
            const bool has_value = index + 1 < args.size() &&
                                   !args[index + 1].empty() &&
                                   args[index + 1].front() != '-';
            if (!has_value) {
                if (!result.missing_option_value) {
                    result.missing_option_value = arg;
                }
            } else {
                result.package_root = qt::foundation::Path(args[++index]);
            }
            continue;
        }
        if (!arg.empty() && arg.front() == '-') {
            if (!result.invalid_option) {
                result.invalid_option = arg;
            }
            continue;
        }
        positional.push_back(arg);
    }

    if (result.command == PackageConfigCommandKind::list) {
        if (!positional.empty() && !result.invalid_argument) {
            result.invalid_argument = positional.front();
        }
        return result;
    }

    if (!positional.empty()) {
        result.source = positional[0];
    }
    if (positional.size() > 1) {
        result.resource_kind = parse_package_resource_kind(positional[1]);
        if (!result.resource_kind && !result.invalid_argument) {
            result.invalid_argument = positional[1];
        }
    }
    if (positional.size() > 2) {
        result.resource_path = qt::foundation::Path(positional[2]);
    }
    if (positional.size() > 3 && !result.invalid_argument) {
        result.invalid_argument = positional[3];
    }
    return result;
}

std::vector<PackageManagerDetection> detect_package_managers(
    const qt::foundation::Path& start_directory,
    const std::optional<qt::foundation::Path>& workspace_root) {
    std::vector<PackageManagerDetection> result;
    std::set<PackageManagerKind> seen;
    bool seen_javascript = false;

    auto current = qt::foundation::canonicalize_path(start_directory);
    if (!current) {
        return result;
    }

    std::optional<qt::foundation::Path> root;
    if (workspace_root) {
        auto canonical_root = qt::foundation::canonicalize_path(*workspace_root);
        if (!canonical_root) {
            return result;
        }
        root = canonical_root.value();
        if (!is_within_workspace_root(current.value(), *root)) {
            return result;
        }
    }

    while (true) {
        auto local = detect_at_directory(current.value());
        std::sort(local.begin(), local.end(),
                  [](const PackageManagerDetection& lhs,
                     const PackageManagerDetection& rhs) {
                      return kind_rank(lhs.kind) < kind_rank(rhs.kind);
                  });
        for (auto& detection : local) {
            if (is_javascript_manager(detection.kind) && seen_javascript) {
                continue;
            }
            if (seen.insert(detection.kind).second) {
                if (is_javascript_manager(detection.kind)) {
                    seen_javascript = true;
                }
                result.push_back(std::move(detection));
            }
        }

        if (root && current.value() == *root) {
            break;
        }
        if (!root && current.value() == current.value().root_path()) {
            break;
        }

        const auto parent = current.value().parent_path();
        if (parent.empty() || parent == current.value()) {
            break;
        }
        current = qt::foundation::canonicalize_path(parent);
        if (!current) {
            break;
        }
    }

    return result;
}

std::vector<PackageManagerCommandPlan> make_package_manager_command_plans(
    const std::vector<PackageManagerDetection>& detections, const std::string& action) {
    return make_package_manager_command_plans(
        detections, action, PackageManagerCommandSettings{});
}

std::vector<PackageManagerCommandPlan> make_package_manager_command_plans(
    const std::vector<PackageManagerDetection>& detections, const std::string& action,
    const PackageManagerCommandSettings& settings) {
    std::vector<PackageManagerCommandPlan> result;
    for (const auto& detection : detections) {
        auto argv = argv_for_action(detection.kind, action, settings);
        if (!argv) {
            continue;
        }
        const std::string command = join_command(argv.value());
        result.push_back(PackageManagerCommandPlan{
            detection.kind,
            detection.root,
            action,
            command,
            std::move(argv.value()),
        });
    }
    return result;
}

std::vector<PackageSourceCommandPlan> make_package_source_command_plans(
    PackageCommandKind command,
    const std::string& source,
    PackageSourceScope scope,
    const qt::foundation::Path& cwd,
    const PackageManagerCommandSettings& settings) {
    return make_package_source_command_plans(command, source, scope, cwd,
                                             qt::foundation::Path{}, settings);
}

std::vector<PackageSourceCommandPlan> make_package_source_command_plans(
    PackageCommandKind command,
    const std::string& source,
    PackageSourceScope scope,
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home,
    const PackageManagerCommandSettings& settings) {
    std::vector<PackageSourceCommandPlan> plans;
    const auto npm_spec = npm_spec_from_package_source(source);
    if (!npm_spec) {
        const auto git = git_source_details(source);
        if (!git) {
            plans.push_back(make_skipped_source_command_plan(
                command, source, scope,
                is_local_package_source(source)
                    ? "local package sources do not require package-manager commands"
                    : "only npm and git package command plans are deterministic without installation state"));
            return plans;
        }

        const auto root = git_install_root(cwd, agent_home, scope);
        const auto target = git_install_path(*git, cwd, agent_home, scope);
        const std::string identity = "git:" + git->host + "/" + git->path;

        if (scope == PackageSourceScope::user && agent_home.empty()) {
            plans.push_back(make_skipped_source_command_plan(
                command, source, scope,
                "agent_home is required for user git package command paths"));
            return plans;
        }

        if (command == PackageCommandKind::install) {
            PackageSourceCommandPlan clone;
            clone.command = command;
            clone.scope = scope;
            clone.source = source;
            clone.source_type = "git";
            clone.source_identity = identity;
            clone.install_root = root;
            clone.install_path = target;
            clone.argv = {"git", "clone", git->repo, target.generic_string()};
            clone.command_text = join_command(clone.argv);
            add_git_cache_mutation_record(clone, root, "ensure_directory");
            add_git_cache_mutation_record(clone, package_cache_ignore_path(root),
                                          "write_if_missing");
            add_git_cache_mutation_record(clone, target, "install");
            plans.push_back(std::move(clone));

            if (git->ref) {
                PackageSourceCommandPlan checkout;
                checkout.command = command;
                checkout.scope = scope;
                checkout.source = source;
                checkout.source_type = "git";
                checkout.source_identity = identity;
                checkout.working_directory = target;
                checkout.install_root = root;
                checkout.install_path = target;
                checkout.argv = {"git", "checkout", *git->ref};
                checkout.command_text = join_command(checkout.argv);
                add_git_cache_mutation_record(checkout, target, "checkout");
                plans.push_back(std::move(checkout));
            }

            PackageSourceCommandPlan dependencies;
            dependencies.command = command;
            dependencies.scope = scope;
            dependencies.source = source;
            dependencies.source_type = "git";
            dependencies.source_identity = identity;
            dependencies.working_directory = target;
            dependencies.install_root = root;
            dependencies.install_path = target;
            dependencies.conditional = true;
            auto npm_prefix = npm_command_prefix(settings);
            if (npm_prefix.empty()) {
                dependencies.skipped = true;
                dependencies.reason = "npmCommand is invalid";
            } else {
                dependencies.argv = append_args(std::move(npm_prefix), {"install"});
                add_git_cache_mutation_record(dependencies, target / "node_modules",
                                              "install_dependencies",
                                              MutationCondition::conditional);
                dependencies.mutation_records.push_back(make_mutation_record(
                    PackageMutationRecordKind::lock, "update", source, scope,
                    target / "package-lock.json",
                    MutationCondition::conditional));
            }
            dependencies.command_text = join_command(dependencies.argv);
            plans.push_back(std::move(dependencies));
            return plans;
        }

        if (command == PackageCommandKind::remove) {
            PackageSourceCommandPlan remove;
            remove.command = command;
            remove.scope = scope;
            remove.source = source;
            remove.source_type = "git";
            remove.source_identity = identity;
            remove.install_root = root;
            remove.install_path = target;
            remove.argv = {"rm", "-rf", target.generic_string()};
            remove.command_text = join_command(remove.argv);
            add_git_cache_mutation_record(remove, target, "remove");
            add_git_cache_mutation_record(remove, root, "prune_empty_parents",
                                          MutationCondition::conditional);
            plans.push_back(std::move(remove));
            return plans;
        }

        if (command == PackageCommandKind::update) {
            PackageSourceCommandPlan fetch;
            fetch.command = command;
            fetch.scope = scope;
            fetch.source = source;
            fetch.source_type = "git";
            fetch.source_identity = identity;
            fetch.working_directory = target;
            fetch.install_root = root;
            fetch.install_path = target;
            fetch.argv = {"git", "fetch", "--prune", "origin"};
            fetch.command_text = join_command(fetch.argv);
            add_git_cache_mutation_record(fetch, target, "fetch");
            add_git_cache_mutation_record(fetch, target, "reset",
                                          MutationCondition::conditional);
            add_git_cache_mutation_record(fetch, target, "clean",
                                          MutationCondition::conditional);
            plans.push_back(std::move(fetch));

            PackageSourceCommandPlan dependencies;
            dependencies.command = command;
            dependencies.scope = scope;
            dependencies.source = source;
            dependencies.source_type = "git";
            dependencies.source_identity = identity;
            dependencies.working_directory = target;
            dependencies.install_root = root;
            dependencies.install_path = target;
            dependencies.conditional = true;
            auto npm_prefix = npm_command_prefix(settings);
            if (npm_prefix.empty()) {
                dependencies.skipped = true;
                dependencies.reason = "npmCommand is invalid";
            } else {
                dependencies.argv = append_args(std::move(npm_prefix), {"install"});
                add_git_cache_mutation_record(dependencies, target / "node_modules",
                                              "install_dependencies",
                                              MutationCondition::conditional);
                dependencies.mutation_records.push_back(make_mutation_record(
                    PackageMutationRecordKind::lock, "update", source, scope,
                    target / "package-lock.json",
                    MutationCondition::conditional));
            }
            dependencies.command_text = join_command(dependencies.argv);
            plans.push_back(std::move(dependencies));
            return plans;
        }

        plans.push_back(make_skipped_source_command_plan(
            command, source, scope,
            "list does not require package-manager commands"));
        return plans;
    }

    auto prefix = npm_command_prefix(settings);
    if (prefix.empty()) {
        plans.push_back(make_skipped_source_command_plan(
            command, source, scope, "npmCommand is invalid"));
        return plans;
    }

    const std::string package_name = npm_source_name(*npm_spec);
    PackageSourceCommandPlan plan;
    plan.command = command;
    plan.scope = scope;
    plan.source = source;
    plan.source_type = "npm";
    plan.source_identity = package_source_identity(source);

    if (command == PackageCommandKind::install) {
        if (scope == PackageSourceScope::user) {
            plan.argv = append_string_args(std::move(prefix),
                                           {"install", "-g", *npm_spec});
            add_global_npm_mutation_record(plan);
        } else {
            const auto root = project_npm_install_root(cwd);
            plan.install_root = root;
            plan.install_path = npm_install_path_for_source(root, source);
            plan.argv = append_string_args(
                std::move(prefix),
                {"install", *npm_spec, "--prefix", root.generic_string()});
            add_project_npm_mutation_records(plan, root, *plan.install_path);
        }
    } else if (command == PackageCommandKind::remove) {
        if (scope == PackageSourceScope::user) {
            plan.argv = append_string_args(std::move(prefix),
                                           {"uninstall", "-g", package_name});
            add_global_npm_mutation_record(plan);
        } else {
            const auto root = project_npm_install_root(cwd);
            plan.install_root = root;
            plan.install_path = npm_install_path_for_source(root, source);
            plan.argv = append_string_args(
                std::move(prefix),
                {"uninstall", package_name, "--prefix", root.generic_string()});
            add_project_npm_mutation_records(plan, root, *plan.install_path);
        }
    } else if (command == PackageCommandKind::update) {
        const std::string latest = package_name + "@latest";
        if (scope == PackageSourceScope::user) {
            plan.argv = append_string_args(std::move(prefix),
                                           {"install", "-g", latest});
            add_global_npm_mutation_record(plan);
        } else {
            const auto root = project_npm_install_root(cwd);
            plan.install_root = root;
            plan.install_path = npm_install_path_for_source(root, source);
            plan.argv = append_string_args(
                std::move(prefix),
                {"install", latest, "--prefix", root.generic_string()});
            add_project_npm_mutation_records(plan, root, *plan.install_path);
        }
    } else {
        plan.skipped = true;
        plan.reason = "list does not require package-manager commands";
    }

    plan.command_text = join_command(plan.argv);
    plans.push_back(std::move(plan));
    return plans;
}

qt::foundation::Result<PackageManagerCommandSettings> load_package_command_settings(
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home) {
    auto user_json =
        package_manager_internal::read_settings_json_if_present(
            agent_home / "settings.json");
    if (!user_json) {
        return user_json.error();
    }
    auto project_json =
        package_manager_internal::read_settings_json_if_present(
            cwd / ".pi" / "settings.json");
    if (!project_json) {
        return project_json.error();
    }

    PackageManagerCommandSettings settings;
    if (const auto npm = user_json.value().find("npmCommand");
        npm != user_json.value().end()) {
        if (!npm->is_array()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "npmCommand must be an array",
            };
        }
        std::vector<std::string> npm_command;
        for (const auto& entry : *npm) {
            if (!entry.is_string()) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::invalid_argument,
                    "npmCommand entries must be strings",
                };
            }
            npm_command.push_back(entry.get<std::string>());
        }
        settings.npm_command = std::move(npm_command);
    }

    const auto user_packages = parse_settings_packages(
        user_json.value(), PackageSourceScope::user, cwd, agent_home);
    if (!user_packages) {
        return user_packages.error();
    }
    const auto project_packages = parse_settings_packages(
        project_json.value(), PackageSourceScope::project, cwd, agent_home);
    if (!project_packages) {
        return project_packages.error();
    }

    settings.user_packages = user_packages.value();
    settings.project_packages = project_packages.value();
    return settings;
}

PackageSettingsPlan make_package_settings_plan(
    const PackageCommandParseResult& parsed,
    const PackageManagerCommandSettings& settings) {
    return make_package_settings_plan(parsed, qt::foundation::Path{}, settings);
}

PackageSettingsPlan make_package_settings_plan(
    const PackageCommandParseResult& parsed,
    const qt::foundation::Path& cwd,
    const PackageManagerCommandSettings& settings) {
    PackageSettingsPlan plan;
    plan.command = parsed.command;
    plan.source = parsed.source;
    plan.configured_packages = configured_packages_from_settings(settings);

    if (parsed.invalid_option || parsed.invalid_argument ||
        parsed.missing_option_value || parsed.conflicting_options) {
        if (parsed.invalid_option) {
            plan.error = "Unknown option " + *parsed.invalid_option + " for \"" +
                         to_string(parsed.command) + "\".";
        } else if (parsed.missing_option_value) {
            plan.error = "Missing value for " + *parsed.missing_option_value + ".";
        } else if (parsed.invalid_argument) {
            plan.error = "Unexpected argument " + *parsed.invalid_argument + ".";
        } else {
            plan.error = *parsed.conflicting_options;
        }
        return plan;
    }

    if ((parsed.command == PackageCommandKind::install ||
         parsed.command == PackageCommandKind::remove) &&
        !parsed.source) {
        plan.error = std::string("Missing ") + to_string(parsed.command) + " source.";
        return plan;
    }

    if (parsed.command == PackageCommandKind::list) {
        return plan;
    }

    if (parsed.command == PackageCommandKind::install) {
        const PackageSourceScope scope =
            parsed.local ? PackageSourceScope::project : PackageSourceScope::user;
        plan.scope = scope;
        const auto existing = package_scope_vector(settings, scope);
        const std::string input_identity =
            scoped_package_source_identity(*parsed.source, scope, cwd);
        const bool already_configured =
            std::any_of(existing.begin(), existing.end(),
                        [&](const PackageConfiguredSource& configured) {
                            return scoped_package_source_identity(
                                       configured.source, configured.scope, cwd) ==
                                   input_identity;
                        });
        if (!already_configured) {
            PackageConfiguredSource added;
            added.source = *parsed.source;
            added.scope = scope;
            plan.configured_packages.push_back(std::move(added));
            plan.settings_changed = true;
            if (!cwd.empty()) {
                plan.source_command_plans = make_package_source_command_plans(
                    PackageCommandKind::install, *parsed.source, scope, cwd, settings);
            }
        }
        return plan;
    }

    if (parsed.command == PackageCommandKind::remove) {
        const PackageSourceScope scope =
            parsed.local ? PackageSourceScope::project : PackageSourceScope::user;
        plan.scope = scope;
        const std::string input_identity =
            scoped_package_source_identity(*parsed.source, scope, cwd);
        std::vector<PackageConfiguredSource> next;
        next.reserve(plan.configured_packages.size());
        for (const auto& configured : plan.configured_packages) {
            if (configured.scope == scope &&
                scoped_package_source_identity(configured.source, configured.scope, cwd) ==
                    input_identity) {
                plan.settings_changed = true;
                if (!cwd.empty()) {
                    auto commands = make_package_source_command_plans(
                        PackageCommandKind::remove, configured.source, configured.scope,
                        cwd, settings);
                    plan.source_command_plans.insert(
                        plan.source_command_plans.end(),
                        std::make_move_iterator(commands.begin()),
                        std::make_move_iterator(commands.end()));
                }
                continue;
            }
            next.push_back(configured);
        }
        if (!plan.settings_changed) {
            plan.error = "No matching package found for " + *parsed.source;
        }
        plan.configured_packages = std::move(next);
        return plan;
    }

    if (parsed.command == PackageCommandKind::update) {
        const auto target = parsed.update_target.value_or(PackageUpdateTarget{});
        plan.self_update = target.kind == PackageUpdateTargetKind::all ||
                           target.kind == PackageUpdateTargetKind::self;
        const bool offline = package_offline_mode_enabled();
        if (target.kind == PackageUpdateTargetKind::all ||
            target.kind == PackageUpdateTargetKind::extensions) {
            if (target.source) {
                for (const auto& configured : plan.configured_packages) {
                    const std::string input_identity =
                        scoped_package_source_identity(*target.source,
                                                       configured.scope,
                                                       cwd);
                    if (scoped_package_source_identity(configured.source,
                                                       configured.scope,
                                                       cwd) == input_identity) {
                        if (offline) {
                            plan.skipped_package_updates.push_back(
                                skipped_update_source(
                                    configured,
                                    "offline mode disables package updates"));
                        } else if (package_source_is_update_candidate(configured.source)) {
                            plan.package_updates.push_back(configured);
                        } else {
                            plan.skipped_package_updates.push_back(
                                skipped_update_source(configured));
                        }
                    }
                }
                if (plan.package_updates.empty() &&
                    plan.skipped_package_updates.empty()) {
                    plan.suggested_source =
                        suggest_npm_prefixed_source(*target.source, plan.configured_packages);
                    plan.error = "No matching package found for " + *target.source;
                    if (plan.suggested_source) {
                        *plan.error += ". Did you mean " + *plan.suggested_source + "?";
                    }
                }
            } else {
                for (const auto& configured : plan.configured_packages) {
                    if (offline) {
                        plan.skipped_package_updates.push_back(
                            skipped_update_source(
                                configured,
                                "offline mode disables package updates"));
                    } else if (package_source_is_update_candidate(configured.source)) {
                        plan.package_updates.push_back(configured);
                    } else {
                        plan.skipped_package_updates.push_back(
                            skipped_update_source(configured));
                    }
                }
            }
        }
        if (!cwd.empty()) {
            for (const auto& update : plan.package_updates) {
                auto commands = make_package_source_command_plans(
                    PackageCommandKind::update, update.source, update.scope, cwd, settings);
                plan.source_command_plans.insert(
                    plan.source_command_plans.end(),
                    std::make_move_iterator(commands.begin()),
                    std::make_move_iterator(commands.end()));
            }
        }
        return plan;
    }

    return plan;
}

PackageSettingsPlan make_package_resource_toggle_plan(
    const PackageResourceToggleRequest& request,
    const PackageManagerCommandSettings& settings) {
    PackageSettingsPlan plan;
    plan.command = PackageCommandKind::list;
    plan.source = request.source;
    plan.scope = request.scope;
    plan.resource_kind = request.resource_kind;
    plan.resource_path = request.resource_path.generic_string();
    plan.resource_enabled = request.enabled;
    plan.configured_packages = configured_packages_from_settings(settings);

    const std::string input_identity =
        scoped_package_source_identity(request.source, request.scope, {});
    const std::string pattern =
        resource_pattern_for_settings(request.package_root, request.resource_path);
    plan.resource_pattern = pattern;

    for (auto& configured : plan.configured_packages) {
        if (configured.scope != request.scope ||
            scoped_package_source_identity(configured.source, configured.scope, {}) !=
                input_identity) {
            continue;
        }

        auto& filters = filters_for_resource(configured, request.resource_kind);
        if (resource_enabled_by_filters(filters, pattern, request.resource_path) ==
            request.enabled) {
            return plan;
        }
        const std::string next_filter =
            std::string(request.enabled ? "+" : "-") + pattern;
        filters.erase(std::remove_if(filters.begin(), filters.end(),
                                     [&](const std::string& existing) {
                                         return strip_filter_prefix(existing) == pattern;
                                     }),
                      filters.end());
        filters.push_back(next_filter);
        configured.filtered = true;
        plan.settings_changed = true;
        return plan;
    }

    plan.error = "No matching package found for " + request.source;
    return plan;
}

Json to_json(const PackageManagerDetection& detection) {
    return Json{
        {"manager", to_string(detection.kind)},
        {"root", qt::foundation::path_for_display(detection.root)},
        {"evidence", to_json_array(detection.evidence)},
    };
}

Json to_json(const PackageManagerCommandPlan& plan) {
    return Json{
        {"manager", to_string(plan.kind)},
        {"root", qt::foundation::path_for_display(plan.root)},
        {"action", plan.action},
        {"command", plan.command},
        {"argv", to_json_array(plan.argv)},
    };
}

Json to_json(const PackageMutationRecord& record) {
    Json json{
        {"kind", to_string(record.kind)},
        {"operation", record.operation},
        {"conditional", record.conditional},
    };
    if (record.source) {
        json["source"] = *record.source;
    }
    if (record.scope) {
        json["scope"] = to_string(*record.scope);
    }
    if (record.path) {
        json["path"] = qt::foundation::path_for_display(*record.path);
    }
    if (record.reason) {
        json["reason"] = *record.reason;
    }
    return json;
}

Json to_json(const PackageMutationExecutionResult& result) {
    return Json{
        {"changed", result.changed},
        {"applied_operations", to_json_array(result.applied_operations)},
        {"skipped_operations", to_json_array(result.skipped_operations)},
        {"touched_paths", to_json_path_array(result.touched_paths)},
    };
}

Json to_json(const PackageSourceCommandPlan& plan) {
    Json mutation_records = Json::array();
    for (const auto& mutation : plan.mutation_records) {
        mutation_records.push_back(to_json(mutation));
    }

    Json json{
        {"command", to_string(plan.command)},
        {"scope", to_string(plan.scope)},
        {"source", plan.source},
        {"command_text", plan.command_text},
        {"argv", to_json_array(plan.argv)},
        {"conditional", plan.conditional},
        {"skipped", plan.skipped},
        {"mutation_records", std::move(mutation_records)},
    };
    if (plan.source_type) {
        json["source_type"] = *plan.source_type;
    }
    if (plan.source_identity) {
        json["source_identity"] = *plan.source_identity;
    }
    if (plan.working_directory) {
        json["working_directory"] =
            qt::foundation::path_for_display(*plan.working_directory);
    }
    if (plan.install_root) {
        json["install_root"] =
            qt::foundation::path_for_display(*plan.install_root);
    }
    if (plan.install_path) {
        json["install_path"] =
            qt::foundation::path_for_display(*plan.install_path);
    }
    if (plan.reason) {
        json["reason"] = *plan.reason;
    }
    return json;
}

Json to_json(const PackageUpdateTarget& target) {
    Json json{{"type", to_string(target.kind)}};
    if (target.source) {
        json["source"] = *target.source;
    }
    return json;
}

Json to_json(const PackageCommandParseResult& result) {
    Json json{
        {"command", to_string(result.command)},
        {"local", result.local},
        {"force", result.force},
        {"help", result.help},
    };
    if (result.source) {
        json["source"] = *result.source;
    }
    if (result.update_target) {
        json["update_target"] = to_json(*result.update_target);
    }
    if (result.invalid_option) {
        json["invalid_option"] = *result.invalid_option;
    }
    if (result.invalid_argument) {
        json["invalid_argument"] = *result.invalid_argument;
    }
    if (result.missing_option_value) {
        json["missing_option_value"] = *result.missing_option_value;
    }
    if (result.conflicting_options) {
        json["conflicting_options"] = *result.conflicting_options;
    }
    return json;
}

Json to_json(const PackageConfigCommandParseResult& result) {
    Json json{
        {"command", to_string(result.command)},
        {"scope", to_string(result.scope)},
        {"help", result.help},
    };
    if (result.source) {
        json["source"] = *result.source;
    }
    if (result.resource_kind) {
        json["resource_kind"] = to_string(*result.resource_kind);
    }
    if (result.package_root) {
        json["package_root"] =
            qt::foundation::path_for_display(*result.package_root);
    }
    if (result.resource_path) {
        json["resource_path"] =
            qt::foundation::path_for_display(*result.resource_path);
    }
    if (result.invalid_option) {
        json["invalid_option"] = *result.invalid_option;
    }
    if (result.invalid_argument) {
        json["invalid_argument"] = *result.invalid_argument;
    }
    if (result.missing_option_value) {
        json["missing_option_value"] = *result.missing_option_value;
    }
    return json;
}

Json to_json(const PackageConfiguredSource& source) {
    Json json{
        {"source", source.source},
        {"scope", to_string(source.scope)},
        {"filtered", source.filtered},
    };
    if (source.installed_path) {
        json["installed_path"] = *source.installed_path;
    }
    if (source.update_skip_reason) {
        json["update_skip_reason"] = *source.update_skip_reason;
    }
    if (!source.extensions.empty()) {
        json["extensions"] = to_json_array(source.extensions);
    }
    if (!source.skills.empty()) {
        json["skills"] = to_json_array(source.skills);
    }
    if (!source.prompts.empty()) {
        json["prompts"] = to_json_array(source.prompts);
    }
    if (!source.themes.empty()) {
        json["themes"] = to_json_array(source.themes);
    }
    return json;
}

Json to_json(const PackageSettingsExecutionResult& result) {
    Json user_packages = Json::array();
    for (const auto& source : result.resulting_settings.user_packages) {
        user_packages.push_back(to_json(source));
    }

    Json project_packages = Json::array();
    for (const auto& source : result.resulting_settings.project_packages) {
        project_packages.push_back(to_json(source));
    }

    Json settings{
        {"user_packages", std::move(user_packages)},
        {"project_packages", std::move(project_packages)},
    };
    if (result.resulting_settings.npm_command) {
        settings["npm_command"] =
            to_json_array(result.resulting_settings.npm_command.value());
    }

    return Json{
        {"changed", result.changed},
        {"settings_changed", result.settings_changed},
        {"applied_operations", to_json_array(result.applied_operations)},
        {"skipped_operations", to_json_array(result.skipped_operations)},
        {"touched_paths", to_json_path_array(result.touched_paths)},
        {"resulting_settings", std::move(settings)},
    };
}

Json to_json(const PackageSettingsPlan& plan) {
    Json configured = Json::array();
    for (const auto& source : plan.configured_packages) {
        configured.push_back(to_json(source));
    }

    Json updates = Json::array();
    for (const auto& source : plan.package_updates) {
        updates.push_back(to_json(source));
    }

    Json skipped_updates = Json::array();
    for (const auto& source : plan.skipped_package_updates) {
        skipped_updates.push_back(to_json(source));
    }

    Json source_command_plans = Json::array();
    for (const auto& source_plan : plan.source_command_plans) {
        source_command_plans.push_back(to_json(source_plan));
    }

    Json json{
        {"command", to_string(plan.command)},
        {"settings_changed", plan.settings_changed},
        {"self_update", plan.self_update},
        {"configured_packages", std::move(configured)},
        {"package_updates", std::move(updates)},
        {"skipped_package_updates", std::move(skipped_updates)},
        {"source_command_plans", std::move(source_command_plans)},
    };
    if (plan.source) {
        json["source"] = *plan.source;
    }
    if (plan.scope) {
        json["scope"] = to_string(*plan.scope);
    }
    if (plan.resource_kind) {
        json["resource_kind"] = to_string(*plan.resource_kind);
    }
    if (plan.resource_path) {
        json["resource_path"] = *plan.resource_path;
    }
    if (plan.resource_pattern) {
        json["resource_pattern"] = *plan.resource_pattern;
    }
    if (plan.resource_enabled) {
        json["resource_enabled"] = *plan.resource_enabled;
    }
    if (plan.suggested_source) {
        json["suggested_source"] = *plan.suggested_source;
    }
    if (plan.error) {
        json["error"] = *plan.error;
    }
    return json;
}

PackageManagerTool::PackageManagerTool() {
    descriptor_.name = "package_manager";
    descriptor_.description =
        "Detect package managers from project marker files and generate command plans without executing them.";
    descriptor_.deterministic = true;
    descriptor_.destructive = false;
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description",
                   "Directory or file path to start detection from. Defaults to the current directory."},
              }},
             {"action",
              {
                  {"type", "string"},
                  {"enum", Json::array({"detect", "install", "build", "test", "check"})},
                  {"description",
                   "Action to plan. detect returns detections only; other actions also return command plans."},
              }},
         }},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& PackageManagerTool::descriptor() const { return descriptor_; }

ToolResult PackageManagerTool::invoke(const ToolRequest& request,
                                      const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match package_manager",
                                    request);
    }
    if (!has_only_arguments(request, {"path", "action"})) {
        return make_invalid_request(
            "package_manager accepts only the path and action arguments", request);
    }

    std::string action = "detect";
    if (request.arguments.contains("action")) {
        const auto action_text = require_string_argument(request, "action", descriptor_.name);
        if (!action_text) {
            return make_invalid_request(action_text.error().summary(), request);
        }
        action = action_text.value();
    }
    if (action != "detect" && action != "install" && action != "build" &&
        action != "test" && action != "check") {
        return make_invalid_request("package_manager action is not supported", request);
    }

    const auto start_directory = resolve_start_directory(request, context, descriptor_);
    if (!start_directory) {
        if (start_directory.error().code == qt::foundation::ErrorCode::not_found) {
            return make_execution_error(ToolResultStatus::not_found,
                                        start_directory.error().summary(), request);
        }
        if (start_directory.error().code == qt::foundation::ErrorCode::invalid_argument) {
            return make_invalid_request(start_directory.error().summary(), request);
        }
        return make_execution_error(ToolResultStatus::execution_error,
                                    start_directory.error().summary(), request);
    }

    std::optional<qt::foundation::Path> workspace_root;
    if (context.workspace_root) {
        auto root = qt::foundation::canonicalize_path(*context.workspace_root);
        if (!root) {
            return make_execution_error(ToolResultStatus::execution_error,
                                        root.error().summary(), request);
        }
        if (!is_workspace_path_allowed(start_directory.value(), root.value(),
                                       context)) {
            return make_execution_error(ToolResultStatus::permission_denied,
                                        "path escapes workspace_root", request);
        }
        if (!context.allow_external_filesystem_access) {
            workspace_root = root.value();
        }
    }

    const auto detections =
        detect_package_managers(start_directory.value(), workspace_root);

    Json detection_json = Json::array();
    for (const auto& detection : detections) {
        detection_json.push_back(to_json(detection));
    }

    Json plans_json = Json::array();
    if (action != "detect") {
        const auto plans = make_package_manager_command_plans(detections, action);
        for (const auto& plan : plans) {
            plans_json.push_back(to_json(plan));
        }
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{
        {"start_directory", qt::foundation::path_for_display(start_directory.value())},
        {"action", action},
        {"detections", std::move(detection_json)},
        {"command_plans", std::move(plans_json)},
    };
    apply_output_guards(result, output_guard_options_from_context(context));
    return result;
}

}  // namespace qt::tools
