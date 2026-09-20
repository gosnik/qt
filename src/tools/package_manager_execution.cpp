#include "package_manager_internal.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/package_manager_tool.hpp"

#include <filesystem>
#include <string_view>

namespace qt::tools {
namespace {

constexpr const char* kPiCacheGitIgnore = "*\n!.gitignore\n";
constexpr const char* kPiNpmProjectManifest =
    "{\n  \"name\": \"pi-extensions\",\n  \"private\": true\n}";

Json string_array_json(const std::vector<std::string>& values) {
    Json json = Json::array();
    for (const auto& value : values) {
        json.push_back(value);
    }
    return json;
}

Json settings_json_for_package_source(const PackageConfiguredSource& source) {
    const bool has_filters =
        source.filtered || !source.extensions.empty() || !source.skills.empty() ||
        !source.prompts.empty() || !source.themes.empty();
    if (!has_filters) {
        return source.source;
    }

    Json json{{"source", source.source}};
    if (!source.extensions.empty()) {
        json["extensions"] = string_array_json(source.extensions);
    }
    if (!source.skills.empty()) {
        json["skills"] = string_array_json(source.skills);
    }
    if (!source.prompts.empty()) {
        json["prompts"] = string_array_json(source.prompts);
    }
    if (!source.themes.empty()) {
        json["themes"] = string_array_json(source.themes);
    }
    return json;
}

Json settings_packages_json(const std::vector<PackageConfiguredSource>& sources) {
    Json packages = Json::array();
    for (const auto& source : sources) {
        packages.push_back(settings_json_for_package_source(source));
    }
    return packages;
}

enum class WriteOutcome { unchanged, wrote };

qt::foundation::Result<WriteOutcome> write_settings_packages(
    const qt::foundation::Path& settings_path,
    const std::vector<PackageConfiguredSource>& sources) {
    auto settings_json =
        package_manager_internal::read_settings_json_if_present(settings_path);
    if (!settings_json) {
        return settings_json.error();
    }

    Json next = settings_json.value();
    next["packages"] = settings_packages_json(sources);
    const std::string next_text = next.dump(2) + "\n";

    std::error_code exists_error;
    if (std::filesystem::exists(settings_path, exists_error)) {
        if (exists_error) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::io_error,
                qt::foundation::path_for_display(settings_path) + ": " +
                    exists_error.message(),
                exists_error.value()};
        }
        auto current = qt::foundation::read_text_file(settings_path);
        if (!current) {
            return current.error();
        }
        if (current.value() == next_text) {
            return WriteOutcome::unchanged;
        }
    }

    auto status = qt::foundation::write_text_file(settings_path, next_text);
    if (!status.ok()) {
        return status.error();
    }
    return WriteOutcome::wrote;
}

std::string mutation_label(const PackageMutationRecord& record) {
    std::string label = std::string(to_string(record.kind)) + ":" + record.operation;
    if (record.path) {
        label += ":" + qt::foundation::path_for_display(*record.path);
    }
    return label;
}

std::optional<std::string> default_write_if_missing_content(
    const qt::foundation::Path& path) {
    const auto filename = path.filename().generic_string();
    if (filename == ".gitignore") {
        return std::string(kPiCacheGitIgnore);
    }
    if (filename == "package.json") {
        return std::string(kPiNpmProjectManifest);
    }
    return std::nullopt;
}

qt::foundation::Result<WriteOutcome> write_file_if_missing(
    const qt::foundation::Path& path,
    std::string_view content) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        if (error) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::io_error,
                qt::foundation::path_for_display(path) + ": " + error.message(),
                error.value()};
        }
        return WriteOutcome::unchanged;
    }
    auto status = qt::foundation::write_text_file(path, content);
    if (!status.ok()) {
        return status.error();
    }
    return WriteOutcome::wrote;
}

qt::foundation::Result<std::size_t> prune_empty_parent_directories(
    const qt::foundation::Path& install_root,
    const qt::foundation::Path& removed_path) {
    if (install_root.empty()) {
        return std::size_t{0};
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(install_root, error);
    if (error) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::io_error,
            qt::foundation::path_for_display(install_root) + ": " +
                error.message(),
            error.value(),
        };
    }

    std::size_t pruned = 0;
    qt::foundation::Path current = removed_path.parent_path().lexically_normal();
    while (!current.empty()) {
        const auto canonical_current =
            std::filesystem::weakly_canonical(current, error);
        if (error) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::io_error,
                qt::foundation::path_for_display(current) + ": " +
                    error.message(),
                error.value(),
            };
        }

        if (canonical_current == root) {
            break;
        }

        const auto relative = canonical_current.lexically_relative(root);
        if (relative.empty() || relative.native().rfind("..", 0) == 0) {
            break;
        }

        if (!std::filesystem::exists(canonical_current, error)) {
            if (error) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::io_error,
                    qt::foundation::path_for_display(canonical_current) + ": " +
                        error.message(),
                    error.value(),
                };
            }
            current = current.parent_path();
            continue;
        }

        if (!std::filesystem::is_directory(canonical_current, error)) {
            if (error) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::io_error,
                    qt::foundation::path_for_display(canonical_current) + ": " +
                        error.message(),
                    error.value(),
                };
            }
            break;
        }

        if (!std::filesystem::is_empty(canonical_current, error)) {
            if (error) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::io_error,
                    qt::foundation::path_for_display(canonical_current) + ": " +
                        error.message(),
                    error.value(),
                };
            }
            break;
        }

        std::filesystem::remove(canonical_current, error);
        if (error) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::io_error,
                qt::foundation::path_for_display(canonical_current) + ": " +
                    error.message(),
                error.value(),
            };
        }
        ++pruned;
        current = current.parent_path();
    }

    return pruned;
}

void record_changed_mutation(PackageMutationExecutionResult& result,
                             const qt::foundation::Path& path,
                             std::string label) {
    result.changed = true;
    result.applied_operations.push_back(std::move(label));
    result.touched_paths.push_back(path);
}

void record_settings_write(PackageSettingsExecutionResult& result,
                           const qt::foundation::Path& path,
                           WriteOutcome outcome) {
    const auto label =
        std::string("settings:write:") + qt::foundation::path_for_display(path);
    if (outcome == WriteOutcome::wrote) {
        result.changed = true;
        result.settings_changed = true;
        result.applied_operations.push_back(label);
        result.touched_paths.push_back(path);
        return;
    }
    result.skipped_operations.push_back(label);
}

qt::foundation::Status write_scoped_settings(
    PackageSettingsExecutionResult& result,
    PackageSourceScope scope,
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home) {
    const auto* sources = &result.resulting_settings.user_packages;
    qt::foundation::Path path;
    const char* missing_path_message = nullptr;

    if (scope == PackageSourceScope::project) {
        sources = &result.resulting_settings.project_packages;
        path = cwd / ".pi" / "settings.json";
        missing_path_message = "cwd is required to write project package settings";
    } else {
        path = agent_home / "settings.json";
        missing_path_message =
            "agent_home is required to write user package settings";
    }

    if (path.empty()) {
        return qt::foundation::Status::failure(
            qt::foundation::ErrorCode::invalid_argument, missing_path_message);
    }

    auto write = write_settings_packages(path, *sources);
    if (!write) {
        return qt::foundation::Status(write.error());
    }
    record_settings_write(result, path, write.value());
    return qt::foundation::Status::success();
}

void merge_execution_result(PackageSettingsExecutionResult& result,
                            PackageMutationExecutionResult&& executed) {
    if (executed.changed) {
        result.changed = true;
    }
    result.applied_operations.insert(result.applied_operations.end(),
                                     executed.applied_operations.begin(),
                                     executed.applied_operations.end());
    result.skipped_operations.insert(result.skipped_operations.end(),
                                     executed.skipped_operations.begin(),
                                     executed.skipped_operations.end());
    result.touched_paths.insert(result.touched_paths.end(),
                                executed.touched_paths.begin(),
                                executed.touched_paths.end());
}

}  // namespace

qt::foundation::Result<PackageMutationExecutionResult>
execute_package_source_cache_mutations(const PackageSourceCommandPlan& plan) {
    PackageMutationExecutionResult result;

    for (const auto& record : plan.mutation_records) {
        const auto label = mutation_label(record);
        if (record.kind != PackageMutationRecordKind::cache || !record.path) {
            result.skipped_operations.push_back(label);
            continue;
        }

        const auto& path = *record.path;
        if (record.operation == "ensure_directory") {
            std::error_code exists_error;
            const bool existed_before = std::filesystem::exists(path, exists_error);
            if (exists_error) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::io_error,
                    qt::foundation::path_for_display(path) + ": " +
                        exists_error.message(),
                    exists_error.value(),
                };
            }
            auto status = qt::foundation::ensure_directory(path);
            if (!status.ok()) {
                return status.error();
            }
            if (existed_before) {
                result.skipped_operations.push_back(label);
            } else {
                record_changed_mutation(result, path, label);
            }
            continue;
        }

        if (record.operation == "write_if_missing") {
            const auto content = default_write_if_missing_content(path);
            if (!content) {
                result.skipped_operations.push_back(label);
                continue;
            }
            auto write = write_file_if_missing(path, *content);
            if (!write) {
                return write.error();
            }
            if (write.value() == WriteOutcome::wrote) {
                record_changed_mutation(result, path, label);
            } else {
                result.skipped_operations.push_back(label);
            }
            continue;
        }

        if (record.operation == "remove") {
            std::error_code error;
            const auto removed = std::filesystem::remove_all(path, error);
            if (error) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::io_error,
                    qt::foundation::path_for_display(path) + ": " + error.message(),
                    error.value(),
                };
            }
            if (removed > 0) {
                record_changed_mutation(result, path, label);
            } else {
                result.skipped_operations.push_back(label);
            }
            continue;
        }

        if (record.operation == "prune_empty_parents") {
            if (!plan.install_path) {
                result.skipped_operations.push_back(label);
                continue;
            }
            auto pruned = prune_empty_parent_directories(path, *plan.install_path);
            if (!pruned) {
                return pruned.error();
            }
            if (pruned.value() > 0) {
                record_changed_mutation(result, path, label);
            } else {
                result.skipped_operations.push_back(label);
            }
            continue;
        }

        result.skipped_operations.push_back(label);
    }

    return result;
}

qt::foundation::Result<PackageSettingsExecutionResult>
execute_package_settings_plan(
    const PackageSettingsPlan& plan,
    const qt::foundation::Path& cwd,
    const qt::foundation::Path& agent_home) {
    PackageSettingsExecutionResult result;

    if (plan.error) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            *plan.error,
        };
    }

    for (const auto& configured : plan.configured_packages) {
        if (configured.scope == PackageSourceScope::project) {
            result.resulting_settings.project_packages.push_back(configured);
        } else {
            result.resulting_settings.user_packages.push_back(configured);
        }
    }

    if (plan.settings_changed && plan.scope.has_value()) {
        auto status = write_scoped_settings(result, *plan.scope, cwd, agent_home);
        if (!status.ok()) {
            return status.error();
        }
    }

    for (const auto& source_plan : plan.source_command_plans) {
        if (source_plan.skipped) {
            std::string label =
                std::string("command:skip:") + to_string(source_plan.command) + ":" +
                source_plan.source;
            if (source_plan.reason) {
                label += ":" + *source_plan.reason;
            }
            result.skipped_operations.push_back(std::move(label));
            continue;
        }

        auto executed = execute_package_source_cache_mutations(source_plan);
        if (!executed) {
            return executed.error();
        }
        merge_execution_result(result, std::move(executed.value()));
    }

    return result;
}

}  // namespace qt::tools
