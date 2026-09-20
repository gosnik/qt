#pragma once

#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/error.hpp"
#include "qt/tools/tool.hpp"

#include <optional>
#include <string>
#include <vector>

namespace qt::tools {

enum class PackageManagerKind {
    npm,
    pnpm,
    yarn,
    cargo,
    cmake,
};

enum class PackageCommandKind {
    install,
    remove,
    update,
    list,
};

enum class PackageUpdateTargetKind {
    all,
    self,
    extensions,
};

enum class PackageSourceScope {
    user,
    project,
};

enum class PackageResourceKind {
    extensions,
    skills,
    prompts,
    themes,
};

enum class PackageMutationRecordKind {
    settings,
    cache,
    lock,
};

enum class PackageConfigCommandKind {
    list,
    enable,
    disable,
};

struct PackageManagerDetection {
    PackageManagerKind kind = PackageManagerKind::npm;
    qt::foundation::Path root;
    std::vector<std::string> evidence;
};

struct PackageManagerCommandPlan {
    PackageManagerKind kind = PackageManagerKind::npm;
    qt::foundation::Path root;
    std::string action;
    std::string command;
    std::vector<std::string> argv;
};

struct PackageMutationRecord {
    PackageMutationRecordKind kind = PackageMutationRecordKind::cache;
    std::string operation;
    std::optional<std::string> source;
    std::optional<PackageSourceScope> scope;
    std::optional<qt::foundation::Path> path;
    bool conditional = false;
    std::optional<std::string> reason;
};

struct PackageMutationExecutionResult {
    bool changed = false;
    std::vector<std::string> applied_operations;
    std::vector<std::string> skipped_operations;
    std::vector<qt::foundation::Path> touched_paths;
};

struct PackageSourceCommandPlan {
    PackageCommandKind command = PackageCommandKind::install;
    PackageSourceScope scope = PackageSourceScope::user;
    std::string source;
    std::optional<std::string> source_type;
    std::optional<std::string> source_identity;
    std::string command_text;
    std::vector<std::string> argv;
    std::optional<qt::foundation::Path> working_directory;
    std::optional<qt::foundation::Path> install_root;
    std::optional<qt::foundation::Path> install_path;
    bool conditional = false;
    bool skipped = false;
    std::optional<std::string> reason;
    std::vector<PackageMutationRecord> mutation_records;
};

struct PackageUpdateTarget {
    PackageUpdateTargetKind kind = PackageUpdateTargetKind::all;
    std::optional<std::string> source;
};

struct PackageCommandParseResult {
    PackageCommandKind command = PackageCommandKind::list;
    std::optional<std::string> source;
    std::optional<PackageUpdateTarget> update_target;
    bool local = false;
    bool force = false;
    bool help = false;
    std::optional<std::string> invalid_option;
    std::optional<std::string> invalid_argument;
    std::optional<std::string> missing_option_value;
    std::optional<std::string> conflicting_options;
};

struct PackageConfigCommandParseResult {
    PackageConfigCommandKind command = PackageConfigCommandKind::list;
    std::optional<std::string> source;
    PackageSourceScope scope = PackageSourceScope::user;
    std::optional<PackageResourceKind> resource_kind;
    std::optional<qt::foundation::Path> package_root;
    std::optional<qt::foundation::Path> resource_path;
    bool help = false;
    std::optional<std::string> invalid_option;
    std::optional<std::string> invalid_argument;
    std::optional<std::string> missing_option_value;
};

struct PackageConfiguredSource {
    std::string source;
    PackageSourceScope scope = PackageSourceScope::user;
    bool filtered = false;
    std::optional<std::string> installed_path;
    std::optional<std::string> update_skip_reason;
    std::vector<std::string> extensions;
    std::vector<std::string> skills;
    std::vector<std::string> prompts;
    std::vector<std::string> themes;
};

struct PackageManagerCommandSettings {
    std::optional<std::vector<std::string>> npm_command;
    std::vector<PackageConfiguredSource> user_packages;
    std::vector<PackageConfiguredSource> project_packages;
};

struct PackageSettingsExecutionResult {
    bool changed = false;
    bool settings_changed = false;
    std::vector<std::string> applied_operations;
    std::vector<std::string> skipped_operations;
    std::vector<qt::foundation::Path> touched_paths;
    PackageManagerCommandSettings resulting_settings;
};

struct PackageSettingsPlan {
    PackageCommandKind command = PackageCommandKind::list;
    std::optional<std::string> source;
    std::optional<PackageSourceScope> scope;
    std::optional<PackageResourceKind> resource_kind;
    std::optional<std::string> resource_path;
    std::optional<std::string> resource_pattern;
    std::optional<bool> resource_enabled;
    bool settings_changed = false;
    bool self_update = false;
    std::vector<PackageConfiguredSource> configured_packages;
    std::vector<PackageConfiguredSource> package_updates;
    std::vector<PackageConfiguredSource> skipped_package_updates;
    std::vector<PackageSourceCommandPlan> source_command_plans;
    std::optional<std::string> suggested_source;
    std::optional<std::string> error;
};

struct PackageResourceToggleRequest {
    std::string source;
    PackageSourceScope scope = PackageSourceScope::user;
    PackageResourceKind resource_kind = PackageResourceKind::extensions;
    qt::foundation::Path package_root;
    qt::foundation::Path resource_path;
    bool enabled = true;
};

const char* to_string(PackageManagerKind kind) noexcept;
const char* to_string(PackageCommandKind command) noexcept;
const char* to_string(PackageUpdateTargetKind kind) noexcept;
const char* to_string(PackageSourceScope scope) noexcept;
const char* to_string(PackageResourceKind kind) noexcept;
const char* to_string(PackageMutationRecordKind kind) noexcept;
const char* to_string(PackageConfigCommandKind command) noexcept;

std::optional<PackageCommandParseResult> parse_package_command_args(
    const std::vector<std::string>& args);
std::optional<PackageConfigCommandParseResult> parse_package_config_command_args(
    const std::vector<std::string>& args);

std::vector<PackageManagerDetection> detect_package_managers(
    const qt::foundation::Path& start_directory,
    const std::optional<qt::foundation::Path>& workspace_root = std::nullopt);

std::vector<PackageManagerCommandPlan> make_package_manager_command_plans(
    const std::vector<PackageManagerDetection>& detections, const std::string& action);
std::vector<PackageManagerCommandPlan> make_package_manager_command_plans(
    const std::vector<PackageManagerDetection>& detections, const std::string& action,
    const PackageManagerCommandSettings& settings);
std::vector<PackageSourceCommandPlan> make_package_source_command_plans(
    PackageCommandKind command,
    const std::string& source,
    PackageSourceScope scope,
    const qt::foundation::Path& cwd,
    const PackageManagerCommandSettings& settings = PackageManagerCommandSettings{});
std::vector<PackageSourceCommandPlan> make_package_source_command_plans(
    PackageCommandKind command,
    const std::string& source,
    PackageSourceScope scope,
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home,
    const PackageManagerCommandSettings& settings = PackageManagerCommandSettings{});
qt::foundation::Result<PackageManagerCommandSettings> load_package_command_settings(
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home);
PackageSettingsPlan make_package_settings_plan(
    const PackageCommandParseResult& parsed,
    const PackageManagerCommandSettings& settings = PackageManagerCommandSettings{});
PackageSettingsPlan make_package_settings_plan(
    const PackageCommandParseResult& parsed,
    const qt::foundation::Path& cwd,
    const PackageManagerCommandSettings& settings = PackageManagerCommandSettings{});
PackageSettingsPlan make_package_resource_toggle_plan(
    const PackageResourceToggleRequest& request,
    const PackageManagerCommandSettings& settings = PackageManagerCommandSettings{});
qt::foundation::Result<PackageMutationExecutionResult>
execute_package_source_cache_mutations(const PackageSourceCommandPlan& plan);
qt::foundation::Result<PackageSettingsExecutionResult>
execute_package_settings_plan(
    const PackageSettingsPlan& plan,
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home);

Json to_json(const PackageManagerDetection& detection);
Json to_json(const PackageManagerCommandPlan& plan);
Json to_json(const PackageSourceCommandPlan& plan);
Json to_json(const PackageMutationRecord& record);
Json to_json(const PackageMutationExecutionResult& result);
Json to_json(const PackageSettingsExecutionResult& result);
Json to_json(const PackageUpdateTarget& target);
Json to_json(const PackageCommandParseResult& result);
Json to_json(const PackageConfigCommandParseResult& result);
Json to_json(const PackageConfiguredSource& source);
Json to_json(const PackageSettingsPlan& plan);

class PackageManagerTool final : public Tool {
  public:
    PackageManagerTool();

    const ToolDescriptor& descriptor() const override;
    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override;

  private:
    ToolDescriptor descriptor_;
};

}  // namespace qt::tools
