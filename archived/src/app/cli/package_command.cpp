#include "qt/app/cli/package_command.hpp"

#include "qt/app/config/config.hpp"
#include "qt/app/config/settings.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/harness/repository.hpp"
#include "qt/tools/package_manager_tool.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace qt::app {
namespace {

using Json = nlohmann::json;

std::string env_or_empty(const char* name) {
  if (const char* value = std::getenv(name)) {
    return std::string(value);
  }
  return {};
}

std::string default_cwd() {
  return std::filesystem::current_path().generic_string();
}

std::string default_agent_home() {
  const auto env = env_or_empty("QT_CODING_AGENT_DIR");
  if (!env.empty()) {
    if (env == "~") {
      const auto home = env_or_empty("HOME");
      return home.empty() ? env : home;
    }
    if (env.rfind("~/", 0) == 0) {
      const auto home = env_or_empty("HOME");
      return home.empty() ? env : home + env.substr(1);
    }
    return env;
  }
  const auto home = env_or_empty("HOME");
  return home.empty() ? ".qt" : home + "/.qt";
}

std::string package_command_usage(qt::tools::PackageCommandKind command) {
  switch (command) {
    case qt::tools::PackageCommandKind::install:
      return "qt install <source> [-l]";
    case qt::tools::PackageCommandKind::remove:
      return "qt remove <source> [-l]";
    case qt::tools::PackageCommandKind::update:
      return "qt update [source|self|pi] [--self] [--extensions] [--extension <source>] [--force]";
    case qt::tools::PackageCommandKind::list:
      return "qt list";
  }
  return "qt list";
}

std::string package_config_command_usage(
    qt::tools::PackageConfigCommandKind command) {
  switch (command) {
    case qt::tools::PackageConfigCommandKind::list:
      return "qt config [list]";
    case qt::tools::PackageConfigCommandKind::enable:
      return "qt config enable <source> <extensions|skills|prompts|themes> <resource-path> --package-root <path> [-l]";
    case qt::tools::PackageConfigCommandKind::disable:
      return "qt config disable <source> <extensions|skills|prompts|themes> <resource-path> --package-root <path> [-l]";
  }
  return "qt config [list]";
}

void print_package_command_help(qt::tools::PackageCommandKind command) {
  if (command == qt::tools::PackageCommandKind::list) {
    std::cout << "Usage:\n"
              << "  " << package_command_usage(command) << "\n\n"
              << "List installed packages from user and project settings.\n";
    return;
  }
  std::cout << "Usage:\n  " << package_command_usage(command) << '\n';
}

void print_config_command_help() {
  std::cout << "Usage:\n"
            << "  qt config\n"
            << "  qt config list\n"
            << "  qt config enable <source> <extensions|skills|prompts|themes> <resource-path> --package-root <path> [-l]\n"
            << "  qt config disable <source> <extensions|skills|prompts|themes> <resource-path> --package-root <path> [-l]\n\n"
            << "Show or update settings-backed package resource filters.\n";
}

void print_package_list(
    const std::vector<qt::tools::PackageConfiguredSource>& packages) {
  if (packages.empty()) {
    std::cout << "No packages installed.\n";
    return;
  }

  const auto print_group = [&](const char* title,
                               qt::tools::PackageSourceScope scope) {
    bool any = false;
    for (const auto& package : packages) {
      if (package.scope == scope) {
        any = true;
        break;
      }
    }
    if (!any) {
      return false;
    }

    std::cout << title << ":\n";
    for (const auto& package : packages) {
      if (package.scope != scope) {
        continue;
      }
      std::cout << "  " << package.source;
      if (package.filtered) {
        std::cout << " (filtered)";
      }
      std::cout << '\n';
      if (package.installed_path) {
        std::cout << "    " << *package.installed_path << '\n';
      }
    }
    return true;
  };

  const bool printed_user =
      print_group("User packages", qt::tools::PackageSourceScope::user);
  if (printed_user) {
    for (const auto& package : packages) {
      if (package.scope == qt::tools::PackageSourceScope::project) {
        std::cout << '\n';
        break;
      }
    }
  }
  print_group("Project packages", qt::tools::PackageSourceScope::project);
}

void print_config_filter_list(const char* label,
                              const std::vector<std::string>& filters) {
  std::cout << "    " << label << ":\n";
  if (filters.empty()) {
    std::cout << "      (none)\n";
    return;
  }
  for (const auto& filter : filters) {
    std::cout << "      - " << filter << '\n';
  }
}

void print_config_root_list(const char* label,
                            const std::vector<std::string>& roots) {
  std::cout << "    " << label << ":\n";
  if (roots.empty()) {
    std::cout << "      (none)\n";
    return;
  }
  for (const auto& root : roots) {
    std::cout << "      - " << root << '\n';
  }
}

void print_config_resource_summary(const qt::app::AppSettings& settings) {
  std::cout << "Resource configuration:\n";
  std::cout << "  prompt_templates:\n";
  std::cout << "    enabled: "
            << (settings.features.load_prompt_templates ? "true" : "false")
            << '\n';
  print_config_root_list("roots", settings.prompt_roots);
  print_config_filter_list("filters", settings.resource_filters.prompt_templates);

  std::cout << "  skills:\n";
  std::cout << "    enabled: "
            << (settings.features.load_skills ? "true" : "false") << '\n';
  print_config_root_list("roots", settings.skill_roots);
  print_config_filter_list("filters", settings.resource_filters.skills);

  std::cout << "  themes:\n";
  std::cout << "    enabled: "
            << (settings.features.load_themes ? "true" : "false") << '\n';
  print_config_root_list("roots", settings.theme_roots);
  print_config_filter_list("filters", settings.resource_filters.themes);
}

void print_configured_package_resources(
    const std::vector<qt::tools::PackageConfiguredSource>& packages) {
  std::cout << "\nConfigured packages:\n";
  if (packages.empty()) {
    std::cout << "  (none)\n";
    return;
  }

  const auto print_group = [&](const char* label,
                               qt::tools::PackageSourceScope scope) {
    bool any = false;
    for (const auto& package : packages) {
      if (package.scope == scope) {
        any = true;
        break;
      }
    }
    if (!any) {
      return;
    }

    std::cout << "  " << label << ":\n";
    for (const auto& package : packages) {
      if (package.scope != scope) {
        continue;
      }

      std::cout << "    - " << package.source;
      if (package.filtered) {
        std::cout << " (filtered)";
      }
      std::cout << '\n';

      const auto print_entries = [](const char* name,
                                    const std::vector<std::string>& values) {
        if (values.empty()) {
          return;
        }
        std::cout << "      " << name << ":\n";
        for (const auto& value : values) {
          std::cout << "        - " << value << '\n';
        }
      };

      print_entries("extensions", package.extensions);
      print_entries("skills", package.skills);
      print_entries("prompts", package.prompts);
      print_entries("themes", package.themes);
    }
  };

  print_group("user", qt::tools::PackageSourceScope::user);
  print_group("project", qt::tools::PackageSourceScope::project);
}

void print_package_resource_toggle_result(
    const qt::tools::PackageSettingsPlan& plan) {
  std::cout << (plan.resource_enabled.value_or(false) ? "Enabled " : "Disabled ")
            << qt::tools::to_string(
                   plan.resource_kind.value_or(
                       qt::tools::PackageResourceKind::extensions))
            << " resource " << plan.resource_pattern.value_or(std::string{})
            << " for " << plan.source.value_or(std::string{}) << '\n';
}

Json package_settings_entry_json(
    const qt::tools::PackageConfiguredSource& package) {
  const bool has_filters = package.filtered || !package.extensions.empty() ||
                           !package.skills.empty() || !package.prompts.empty() ||
                           !package.themes.empty();
  if (!has_filters) {
    return package.source;
  }

  Json entry{{"source", package.source}};
  if (!package.extensions.empty()) {
    entry["extensions"] = package.extensions;
  }
  if (!package.skills.empty()) {
    entry["skills"] = package.skills;
  }
  if (!package.prompts.empty()) {
    entry["prompts"] = package.prompts;
  }
  if (!package.themes.empty()) {
    entry["themes"] = package.themes;
  }
  return entry;
}

qt::foundation::Result<Json> read_package_settings_document(
    const qt::foundation::Path& path) {
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error)) {
    return Json::object();
  }
  if (exists_error) {
    return qt::foundation::Result<Json>::failure(
        qt::foundation::ErrorCode::io_error, exists_error.message(),
        exists_error.value());
  }

  const auto content = qt::foundation::read_text_file(path);
  if (!content) {
    return content.error();
  }
  try {
    auto parsed = Json::parse(content.value());
    if (!parsed.is_object()) {
      return qt::foundation::Result<Json>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "settings document must be a JSON object");
    }
    return parsed;
  } catch (const Json::parse_error& error) {
    return qt::foundation::Result<Json>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        std::string("invalid settings JSON in ") +
            qt::foundation::path_for_display(path) + ": " + error.what());
  }
}

qt::foundation::Status write_package_settings_scope(
    const qt::foundation::Path& path,
    qt::tools::PackageSourceScope scope,
    const std::vector<qt::tools::PackageConfiguredSource>& packages) {
  auto document = read_package_settings_document(path);
  if (!document) {
    return qt::foundation::Status(document.error());
  }

  Json package_entries = Json::array();
  for (const auto& package : packages) {
    if (package.scope == scope) {
      package_entries.push_back(package_settings_entry_json(package));
    }
  }
  document.value()["packages"] = std::move(package_entries);

  const auto written =
      qt::foundation::write_text_file(path, document.value().dump(2) + "\n");
  return written;
}

bool package_source_plans_require_execution(
    const std::vector<qt::tools::PackageSourceCommandPlan>& plans) {
  return std::any_of(plans.begin(), plans.end(),
                     [](const qt::tools::PackageSourceCommandPlan& plan) {
                       return !plan.skipped;
                     });
}

void print_package_source_command_plans(
    const std::vector<qt::tools::PackageSourceCommandPlan>& plans,
    std::string_view heading) {
  if (plans.empty()) {
    return;
  }

  std::cout << heading << ":\n";
  for (const auto& plan : plans) {
    std::cout << "  [" << qt::tools::to_string(plan.scope) << "] "
              << plan.source << '\n';
    if (plan.skipped) {
      std::cout << "    skipped";
      if (plan.reason) {
        std::cout << ": " << *plan.reason;
      }
      std::cout << '\n';
      continue;
    }
    std::cout << "    " << plan.command_text << '\n';
  }
}

void print_package_update_plan(const qt::tools::PackageSettingsPlan& plan) {
  bool printed_section = false;

  if (!plan.source_command_plans.empty()) {
    print_package_source_command_plans(plan.source_command_plans,
                                       "Planned package updates");
    printed_section = true;
  }

  if (!plan.skipped_package_updates.empty()) {
    if (printed_section) {
      std::cout << '\n';
    }
    std::cout << "Skipped package updates:\n";
    for (const auto& skipped : plan.skipped_package_updates) {
      std::cout << "  [" << qt::tools::to_string(skipped.scope) << "] "
                << skipped.source << '\n';
    }
    printed_section = true;
  }

  if (plan.self_update) {
    if (printed_section) {
      std::cout << '\n';
    }
    qt::app::SelfUpdateEnvironment environment;
    std::cout << "Self update:\n"
              << "  unavailable for this installation\n"
              << "  " << qt::app::get_update_instruction("qt", environment)
              << '\n';
    printed_section = true;
  }

  if (!printed_section) {
    std::cout << "No package updates planned.\n";
  }
}

}  // namespace

std::optional<int> run_package_or_config_command(
    const std::vector<std::string>& raw_args) {
  if (raw_args.empty()) {
    return std::nullopt;
  }

  if (raw_args.front() == "config") {
    const auto parsed = qt::tools::parse_package_config_command_args(raw_args);
    if (!parsed) {
      return std::nullopt;
    }
    if (parsed->help) {
      print_config_command_help();
      return EXIT_SUCCESS;
    }
    if (parsed->invalid_option) {
      std::cerr << "Unknown option " << *parsed->invalid_option
                << " for \"config\".\n"
                << "Use \"qt --help\" or \"qt config\".\n";
      return EXIT_FAILURE;
    }
    if (parsed->missing_option_value) {
      std::cerr << "Missing value for " << *parsed->missing_option_value
                << ".\n"
                << "Usage: " << package_config_command_usage(parsed->command)
                << '\n';
      return EXIT_FAILURE;
    }
    if (parsed->invalid_argument) {
      std::cerr << "Unexpected argument " << *parsed->invalid_argument
                << ".\n"
                << "Usage: " << package_config_command_usage(parsed->command)
                << '\n';
      return EXIT_FAILURE;
    }

    const std::string cwd = default_cwd();
    const std::string agent_home = default_agent_home();

    const auto package_settings =
        qt::tools::load_package_command_settings(cwd, agent_home);
    if (!package_settings) {
      std::cerr << "qt: config command failed: "
                << package_settings.error().summary() << '\n';
      return EXIT_FAILURE;
    }

    if (parsed->command == qt::tools::PackageConfigCommandKind::list) {
      qt::harness::FilesystemRepository repository(cwd);

      qt::app::LoadSettingsRequest request;
      request.cwd = cwd;
      if (!agent_home.empty()) {
        request.agent_home = agent_home;
      }

      const auto settings = qt::app::load_app_settings(repository, request);
      if (!settings) {
        std::cerr << "qt: config command failed: "
                  << settings.error().summary() << '\n';
        return EXIT_FAILURE;
      }

      print_config_resource_summary(settings.value());
      print_configured_package_resources(
          qt::tools::make_package_settings_plan(
              qt::tools::PackageCommandParseResult{}, package_settings.value())
              .configured_packages);
      return EXIT_SUCCESS;
    }

    if (!parsed->source) {
      std::cerr << "Missing config source.\n"
                << "Usage: " << package_config_command_usage(parsed->command)
                << '\n';
      return EXIT_FAILURE;
    }
    if (!parsed->resource_kind) {
      std::cerr << "Missing config resource type.\n"
                << "Usage: " << package_config_command_usage(parsed->command)
                << '\n';
      return EXIT_FAILURE;
    }
    if (!parsed->resource_path) {
      std::cerr << "Missing config resource path.\n"
                << "Usage: " << package_config_command_usage(parsed->command)
                << '\n';
      return EXIT_FAILURE;
    }
    if (!parsed->package_root) {
      std::cerr << "Missing --package-root.\n"
                << "Usage: " << package_config_command_usage(parsed->command)
                << '\n';
      return EXIT_FAILURE;
    }

    qt::foundation::Path package_root = *parsed->package_root;
    if (package_root.is_relative()) {
      package_root = qt::foundation::Path(cwd) / package_root;
    }
    qt::foundation::Path resource_path = *parsed->resource_path;
    if (resource_path.is_relative()) {
      resource_path = package_root / resource_path;
    }

    qt::tools::PackageResourceToggleRequest toggle;
    toggle.source = *parsed->source;
    toggle.scope = parsed->scope;
    toggle.resource_kind = *parsed->resource_kind;
    toggle.package_root = package_root.lexically_normal();
    toggle.resource_path = resource_path.lexically_normal();
    toggle.enabled =
        parsed->command == qt::tools::PackageConfigCommandKind::enable;

    const auto plan = qt::tools::make_package_resource_toggle_plan(
        toggle, package_settings.value());
    if (plan.error) {
      std::cerr << *plan.error << '\n';
      return EXIT_FAILURE;
    }
    if (plan.settings_changed) {
      const auto path =
          plan.scope == qt::tools::PackageSourceScope::project
              ? qt::foundation::Path(cwd) / ".pi" / "settings.json"
              : qt::foundation::Path(agent_home) / "settings.json";
      const auto written =
          write_package_settings_scope(path, *plan.scope, plan.configured_packages);
      if (!written) {
        std::cerr << "Error: " << written.error().message << '\n';
        return EXIT_FAILURE;
      }
    }

    print_package_resource_toggle_result(plan);
    return EXIT_SUCCESS;
  }

  const auto parsed = qt::tools::parse_package_command_args(raw_args);
  if (!parsed) {
    return std::nullopt;
  }

  if (parsed->help) {
    print_package_command_help(parsed->command);
    return EXIT_SUCCESS;
  }
  if (parsed->invalid_option) {
    std::cerr << "Unknown option " << *parsed->invalid_option << " for \""
              << qt::tools::to_string(parsed->command) << "\".\n"
              << "Use \"qt --help\" or \"" << package_command_usage(parsed->command)
              << "\".\n";
    return EXIT_FAILURE;
  }
  if (parsed->missing_option_value) {
    std::cerr << "Missing value for " << *parsed->missing_option_value << ".\n"
              << "Usage: " << package_command_usage(parsed->command) << '\n';
    return EXIT_FAILURE;
  }
  if (parsed->invalid_argument) {
    std::cerr << "Unexpected argument " << *parsed->invalid_argument << ".\n"
              << "Usage: " << package_command_usage(parsed->command) << '\n';
    return EXIT_FAILURE;
  }
  if (parsed->conflicting_options) {
    std::cerr << *parsed->conflicting_options << "\n"
              << "Usage: " << package_command_usage(parsed->command) << '\n';
    return EXIT_FAILURE;
  }
  if ((parsed->command == qt::tools::PackageCommandKind::install ||
       parsed->command == qt::tools::PackageCommandKind::remove) &&
      !parsed->source) {
    std::cerr << "Missing " << qt::tools::to_string(parsed->command)
              << " source.\n"
              << "Usage: " << package_command_usage(parsed->command) << '\n';
    return EXIT_FAILURE;
  }

  const std::string cwd = default_cwd();
  const std::string agent_home = default_agent_home();
  const auto settings =
      qt::tools::load_package_command_settings(cwd, agent_home);
  if (!settings) {
    std::cerr << "Error: " << settings.error().message << '\n';
    return EXIT_FAILURE;
  }

  const auto plan =
      qt::tools::make_package_settings_plan(*parsed, cwd, settings.value());
  if (plan.error) {
    std::cerr << *plan.error << '\n';
    return EXIT_FAILURE;
  }

  if (parsed->command == qt::tools::PackageCommandKind::list) {
    print_package_list(plan.configured_packages);
    return EXIT_SUCCESS;
  }

  if (plan.settings_changed) {
    const auto path =
        plan.scope == qt::tools::PackageSourceScope::project
            ? qt::foundation::Path(cwd) / ".pi" / "settings.json"
            : qt::foundation::Path(agent_home) / "settings.json";
    const auto written =
        write_package_settings_scope(path, *plan.scope, plan.configured_packages);
    if (!written) {
      std::cerr << "Error: " << written.error().message << '\n';
      return EXIT_FAILURE;
    }
  }

  if (parsed->command == qt::tools::PackageCommandKind::install) {
    std::cout << "Installed " << *parsed->source << '\n';
    if (package_source_plans_require_execution(plan.source_command_plans)) {
      print_package_source_command_plans(plan.source_command_plans,
                                         "Planned package-manager commands");
    }
    return EXIT_SUCCESS;
  }
  if (parsed->command == qt::tools::PackageCommandKind::remove) {
    std::cout << "Removed " << *parsed->source << '\n';
    if (package_source_plans_require_execution(plan.source_command_plans)) {
      print_package_source_command_plans(plan.source_command_plans,
                                         "Planned package-manager commands");
    }
    return EXIT_SUCCESS;
  }
  if (parsed->command == qt::tools::PackageCommandKind::update) {
    print_package_update_plan(plan);
    return EXIT_SUCCESS;
  }

  return EXIT_FAILURE;
}

}  // namespace qt::app
