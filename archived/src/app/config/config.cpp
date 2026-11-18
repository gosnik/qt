#include "qt/app/config/config.hpp"

#include <algorithm>
#include <cctype>

namespace qt::app {
namespace {

std::string normalize_path_text(std::string_view text) {
  std::string normalized(text);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  std::transform(
      normalized.begin(), normalized.end(), normalized.begin(),
      [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return normalized;
}

SelfUpdateCommandStep make_self_update_command_step(
    const std::string& command, const std::vector<std::string>& args) {
  SelfUpdateCommandStep step;
  step.command = command;
  step.args = args;
  step.display = command;
  for (const auto& arg : args) {
    step.display.push_back(' ');
    if (arg.find_first_of(" \t") != std::string::npos) {
      step.display.push_back('"');
      step.display += arg;
      step.display.push_back('"');
    } else {
      step.display += arg;
    }
  }
  return step;
}

std::optional<SelfUpdateCommand> make_self_update_command_for_method(
    InstallMethod method, std::string_view installed_package_name,
    std::string_view update_package_name,
    const std::optional<std::vector<std::string>>& npm_command,
    const qt::foundation::Path& package_dir) {
  const std::string installed(installed_package_name);
  const std::string updated =
      update_package_name.empty() ? installed : std::string(update_package_name);

  if (method == InstallMethod::kBunBinary || method == InstallMethod::kUnknown) {
    return std::nullopt;
  }

  SelfUpdateCommandStep install_step;
  std::optional<SelfUpdateCommandStep> uninstall_step;

  switch (method) {
    case InstallMethod::kPnpm:
      install_step =
          make_self_update_command_step("pnpm", {"install", "-g", updated});
      if (updated != installed) {
        uninstall_step = make_self_update_command_step(
            "pnpm", {"remove", "-g", installed});
      }
      break;
    case InstallMethod::kYarn:
      install_step =
          make_self_update_command_step("yarn", {"global", "add", updated});
      if (updated != installed) {
        uninstall_step = make_self_update_command_step(
            "yarn", {"global", "remove", installed});
      }
      break;
    case InstallMethod::kBun:
      install_step =
          make_self_update_command_step("bun", {"install", "-g", updated});
      if (updated != installed) {
        uninstall_step = make_self_update_command_step(
            "bun", {"uninstall", "-g", installed});
      }
      break;
    case InstallMethod::kNpm: {
      std::string command = "npm";
      std::vector<std::string> prefix_args;
      if (npm_command && !npm_command->empty()) {
        command = npm_command->front().empty() ? "npm" : npm_command->front();
        prefix_args.assign(npm_command->begin() + 1, npm_command->end());
      } else if (const auto inferred = infer_npm_prefix_from_package_dir(package_dir)) {
        prefix_args = {"--prefix", qt::foundation::path_for_display(*inferred)};
      }

      auto install_args = prefix_args;
      install_args.push_back("install");
      install_args.push_back("-g");
      install_args.push_back(updated);
      install_step = make_self_update_command_step(command, install_args);

      if (updated != installed) {
        auto uninstall_args = prefix_args;
        uninstall_args.push_back("uninstall");
        uninstall_args.push_back("-g");
        uninstall_args.push_back(installed);
        uninstall_step = make_self_update_command_step(command, uninstall_args);
      }
      break;
    }
    case InstallMethod::kBunBinary:
    case InstallMethod::kUnknown:
      return std::nullopt;
  }

  SelfUpdateCommand command;
  command.command = install_step.command;
  command.args = install_step.args;
  command.display = install_step.display;
  if (uninstall_step) {
    command.display = uninstall_step->display + " && " + install_step.display;
    command.steps.push_back(*uninstall_step);
  }
  command.steps.push_back(install_step);
  return command;
}

}  // namespace

std::string to_string(InstallMethod method) {
  switch (method) {
    case InstallMethod::kBunBinary:
      return "bun-binary";
    case InstallMethod::kNpm:
      return "npm";
    case InstallMethod::kPnpm:
      return "pnpm";
    case InstallMethod::kYarn:
      return "yarn";
    case InstallMethod::kBun:
      return "bun";
    case InstallMethod::kUnknown:
      return "unknown";
  }
  return "unknown";
}

InstallMethod detect_install_method(std::string_view exec_path,
                                    std::string_view module_dir,
                                    InstallMethodDetectionContext context) {
  if (context.bun_binary) {
    return InstallMethod::kBunBinary;
  }

  const std::string normalized =
      normalize_path_text(std::string(module_dir) + "\0" + std::string(exec_path));
  if (normalized.find("/pnpm/") != std::string::npos ||
      normalized.find("/.pnpm/") != std::string::npos) {
    return InstallMethod::kPnpm;
  }
  if (normalized.find("/yarn/") != std::string::npos ||
      normalized.find("/.yarn/") != std::string::npos) {
    return InstallMethod::kYarn;
  }
  if (context.bun_runtime ||
      normalized.find("/install/global/node_modules/") != std::string::npos) {
    return InstallMethod::kBun;
  }
  if (normalized.find("/npm/") != std::string::npos ||
      normalized.find("/node_modules/") != std::string::npos) {
    return InstallMethod::kNpm;
  }
  return InstallMethod::kUnknown;
}

std::optional<qt::foundation::Path> infer_npm_prefix_from_package_dir(
    const qt::foundation::Path& package_dir) {
  if (package_dir.empty()) {
    return std::nullopt;
  }

  const auto parent = package_dir.parent_path();
  if (parent.empty()) {
    return std::nullopt;
  }

  qt::foundation::Path node_modules_root;
  if (!parent.filename().empty() &&
      parent.filename().generic_string().front() == '@' &&
      parent.parent_path().filename() == "node_modules") {
    node_modules_root = parent.parent_path();
  } else if (parent.filename() == "node_modules") {
    node_modules_root = parent;
  } else {
    return std::nullopt;
  }

  const auto root_parent = node_modules_root.parent_path();
  if (root_parent.filename() == "lib") {
    return root_parent.parent_path();
  }
  return std::nullopt;
}

bool is_managed_by_global_package_manager(
    const SelfUpdateEnvironment& environment) {
  if (environment.package_dir.empty()) {
    return false;
  }

  const std::string package_dir =
      normalize_path_text(qt::foundation::path_for_display(environment.package_dir));
  for (const auto& root : environment.global_package_roots) {
    const std::string normalized_root =
        normalize_path_text(qt::foundation::path_for_display(root));
    if (normalized_root.empty()) {
      continue;
    }
    const std::string prefix =
        normalized_root.back() == '/' ? normalized_root : normalized_root + "/";
    if (package_dir == normalized_root || package_dir.rfind(prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

std::optional<SelfUpdateCommand> make_self_update_command(
    std::string_view installed_package_name,
    const SelfUpdateEnvironment& environment,
    std::string_view update_package_name) {
  const auto command = make_self_update_command_for_method(
      environment.install_method, installed_package_name, update_package_name,
      environment.npm_command, environment.package_dir);
  if (!command) {
    return std::nullopt;
  }
  if (!is_managed_by_global_package_manager(environment) ||
      !environment.package_path_writable) {
    return std::nullopt;
  }
  return command;
}

std::string get_self_update_unavailable_instruction(
    std::string_view installed_package_name,
    const SelfUpdateEnvironment& environment,
    std::string_view update_package_name) {
  const std::string updated =
      update_package_name.empty() ? std::string(installed_package_name)
                                  : std::string(update_package_name);
  if (environment.install_method == InstallMethod::kBunBinary) {
    return "Download from: "
           "https://github.com/earendil-works/pi-mono/releases/latest";
  }

  const auto command = make_self_update_command_for_method(
      environment.install_method, installed_package_name, update_package_name,
      environment.npm_command, environment.package_dir);
  if (command) {
    if (is_managed_by_global_package_manager(environment) &&
        !environment.package_path_writable) {
      return "This installation is managed by a global " +
             to_string(environment.install_method) +
             " install, but the install path is not writable. Update it "
             "yourself with: " +
             command->display;
    }
    return "This installation is not managed by a global " +
           to_string(environment.install_method) +
           " install. Update it with the package manager, wrapper, or source "
           "checkout that provides it.";
  }

  return "Update " + updated +
         " using the package manager, wrapper, or source checkout that "
         "provides this installation.";
}

std::string get_update_instruction(std::string_view installed_package_name,
                                   const SelfUpdateEnvironment& environment) {
  const auto command = make_self_update_command_for_method(
      environment.install_method, installed_package_name, {},
      environment.npm_command, environment.package_dir);
  if (command) {
    return "Run: " + command->display;
  }
  return get_self_update_unavailable_instruction(installed_package_name,
                                                 environment);
}

}  // namespace qt::app
