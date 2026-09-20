#pragma once

#include "qt/foundation/filesystem.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::app {

enum class InstallMethod {
  kBunBinary,
  kNpm,
  kPnpm,
  kYarn,
  kBun,
  kUnknown,
};

struct SelfUpdateCommandStep {
  std::string command;
  std::vector<std::string> args;
  std::string display;
};

struct SelfUpdateCommand {
  std::string command;
  std::vector<std::string> args;
  std::string display;
  std::vector<SelfUpdateCommandStep> steps;
};

struct SelfUpdateEnvironment {
  InstallMethod install_method = InstallMethod::kUnknown;
  qt::foundation::Path package_dir;
  std::vector<qt::foundation::Path> global_package_roots;
  std::optional<std::vector<std::string>> npm_command;
  bool package_path_writable = true;
};

struct InstallMethodDetectionContext {
  bool bun_runtime = false;
  bool bun_binary = false;
};

std::string to_string(InstallMethod method);

InstallMethod detect_install_method(std::string_view exec_path,
                                    std::string_view module_dir = {},
                                    InstallMethodDetectionContext context = {});

std::optional<qt::foundation::Path> infer_npm_prefix_from_package_dir(
    const qt::foundation::Path& package_dir);

bool is_managed_by_global_package_manager(const SelfUpdateEnvironment& environment);

std::optional<SelfUpdateCommand> make_self_update_command(
    std::string_view installed_package_name,
    const SelfUpdateEnvironment& environment,
    std::string_view update_package_name = {});

std::string get_self_update_unavailable_instruction(
    std::string_view installed_package_name,
    const SelfUpdateEnvironment& environment,
    std::string_view update_package_name = {});

std::string get_update_instruction(std::string_view installed_package_name,
                                   const SelfUpdateEnvironment& environment);

}  // namespace qt::app
