#include "qt/app/cli/cli_startup.hpp"

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/llama_model_discovery.hpp"
#include "qt/ai/model_catalog.hpp"
#include "qt/ai/openrouter_model_discovery.hpp"
#include "qt/ai/provider.hpp"
#include "qt/app/cli/cli.hpp"
#include "qt/app/config/migrations.hpp"
#include "qt/app/config/settings.hpp"
#include "qt/app/cli/memory_command.hpp"
#include "qt/app/export/export_html.hpp"
#include "qt/app/interactive/interactive_mode.hpp"
#include "qt/app/runtime/print_json_mode.hpp"
#include "qt/app/runtime/openai_model_refresh.hpp"
#include "qt/app/runtime/model_resolution.hpp"
#include "qt/app/runtime/extensions.hpp"
#include "qt/app/cli/package_command.hpp"
#include "qt/app/rpc/rpc_mode.hpp"
#include "qt/app/sessions/session_cwd.hpp"
#include "qt/app/cli/startup.hpp"
#include "qt/app/export/stream_output.hpp"
#include "qt/extensions/contributions.hpp"
#include "qt/foundation/logging.hpp"
#include "qt/harness/repository.hpp"
#include "qt/tui/utils/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define QT_ISATTY _isatty
#define QT_FILENO _fileno
#else
#include <unistd.h>
#define QT_ISATTY isatty
#define QT_FILENO fileno
#endif

namespace qt::app {
namespace {

std::string env_or_empty(const char* name) {
  if (const char* value = std::getenv(name)) {
    return std::string(value);
  }
  return {};
}

std::string default_cwd() {
  return std::filesystem::current_path().generic_string();
}

std::string join_strings(const std::vector<std::string>& values,
                         std::string_view separator) {
  std::ostringstream out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0U) {
      out << separator;
    }
    out << values[index];
  }
  return out.str();
}

std::string resolve_cli_resource_path(const std::string& cwd,
                                      const std::string& path) {
  if (path.empty()) {
    return path;
  }
  const std::filesystem::path candidate(path);
  if (candidate.is_absolute()) {
    return candidate.lexically_normal().generic_string();
  }
  return (std::filesystem::path(cwd) / candidate)
      .lexically_normal()
      .generic_string();
}

std::vector<std::string> resolve_cli_resource_paths(
    const std::string& cwd, const std::vector<std::string>& paths) {
  std::vector<std::string> resolved;
  resolved.reserve(paths.size());
  for (const auto& path : paths) {
    resolved.push_back(resolve_cli_resource_path(cwd, path));
  }
  return resolved;
}

bool model_matches_search(const qt::ai::ModelCatalogModelRecord& model,
                          std::string_view search) {
  if (search.empty()) {
    return true;
  }
  const auto match = qt::tui::fuzzy_match(
      search, model.ref.provider + std::string(" ") + model.ref.name);
  return match.matches;
}

std::string format_token_count(std::int64_t count) {
  if (count >= 1'000'000) {
    const auto millions = static_cast<double>(count) / 1'000'000.0;
    const auto whole_millions = count % 1'000'000 == 0;
    std::ostringstream out;
    out << (whole_millions ? static_cast<long long>(millions) : millions);
    out << 'M';
    return out.str();
  }
  if (count >= 1'000) {
    const auto thousands = static_cast<double>(count) / 1'000.0;
    const auto whole_thousands = count % 1'000 == 0;
    std::ostringstream out;
    out << (whole_thousands ? static_cast<long long>(thousands) : thousands);
    out << 'K';
    return out.str();
  }
  return std::to_string(count);
}

std::int64_t model_metadata_integer(const qt::ai::ModelCatalogModelRecord& model,
                                    const char* field) {
  const auto it = model.metadata.find(field);
  if (it == model.metadata.end() || !it->is_number_integer()) {
    return 0;
  }
  return it->get<std::int64_t>();
}

bool provider_metadata_flag(const qt::ai::ModelCatalogProviderRecord& provider,
                            std::string_view key) {
  if (!provider.metadata.is_object() ||
      !provider.metadata.contains(std::string(key))) {
    return false;
  }
  const auto& value = provider.metadata.at(std::string(key));
  return value.is_boolean() && value.get<bool>();
}

bool model_is_available_for_listing(
    const qt::ai::ModelCatalog& catalog, const qt::ai::AuthStorage& auth_storage,
    const qt::ai::ModelCatalogModelRecord& model) {
  const auto* provider =
      qt::ai::find_provider_record(catalog, model.ref.provider);
  if (provider == nullptr) {
    return false;
  }
  if (provider_metadata_flag(*provider, "local") ||
      provider_metadata_flag(*provider, "direct") ||
      (provider->metadata.is_object() &&
       provider->metadata.contains("requiresAuth") &&
       provider->metadata.at("requiresAuth").is_boolean() &&
       !provider->metadata.at("requiresAuth").get<bool>())) {
    return true;
  }
  if (qt::ai::has_provider_auth_record(auth_storage, model.ref.provider) ||
      qt::ai::get_env_api_key(model.ref.provider).has_value()) {
    return true;
  }
  qt::app::ModelResolutionRequest request;
  request.catalog = catalog;
  request.auth_storage = auth_storage;
  request.provider = model.ref.provider;
  request.model = model.ref.name;
  const auto resolved = qt::app::resolve_model_selection(request);
  return resolved && resolved.value().auth.has_value();
}

std::string pad_right(const std::string& value, std::size_t width) {
  if (value.size() >= width) {
    return value;
  }
  return value + std::string(width - value.size(), ' ');
}

int run_list_models(const ReferenceCliArgs& parsed,
                              const std::string& agent_home) {
  auto catalog = qt::ai::default_model_catalog();
  qt::harness::FilesystemRepository repository(default_cwd());
  qt::app::LoadSettingsRequest settings_request;
  settings_request.cwd = default_cwd();
  settings_request.agent_home = agent_home;
  const auto settings = qt::app::load_app_settings(repository, settings_request);
  if (settings) {
    catalog = qt::ai::merge_catalogs(
        catalog, qt::ai::model_catalog_from_llama_settings(settings.value().llama));
    catalog = qt::ai::merge_catalogs(
        catalog,
        qt::ai::model_catalog_from_ollama_settings(
            settings.value().ollama_instances));
  } else {
    std::cerr << "Warning: " << settings.error().message << '\n';
  }
  if (!parsed.local_model_dirs.empty()) {
    const auto discovered = qt::ai::discover_llama_models(
        qt::ai::LlamaModelDiscoveryOptions{parsed.local_model_dirs, true});
    for (const auto& diagnostic : discovered.diagnostics) {
      std::cerr << "Warning: " << diagnostic << '\n';
    }
    const auto persisted = qt::app::persist_discovered_llama_models(
        agent_home, default_cwd(), discovered);
    if (persisted) {
      for (const auto& diagnostic : persisted.value().diagnostics) {
        std::cerr << "Info: " << diagnostic << '\n';
      }
    } else {
      std::cerr << "Warning: local model discovery could not update settings: "
                << persisted.error().message << '\n';
    }
    catalog = qt::ai::merge_catalogs(
        catalog, qt::ai::model_catalog_from_llama_discovery(discovered));
  }
  qt::ai::AuthStorage auth_storage;
  const auto loaded_auth = qt::ai::load_auth_storage(
      (std::filesystem::path(agent_home) / "auth.json").generic_string());
  if (loaded_auth) {
    auth_storage = loaded_auth.value();
  }
  std::ifstream models_input(std::filesystem::path(agent_home) / "models.json");
  if (models_input) {
    try {
      const auto configured = qt::ai::apply_models_config_to_model_catalog(
          catalog, qt::ai::Json::parse(models_input));
      if (configured) {
        catalog = configured.value();
      } else {
        std::cerr << "Warning: " << configured.error().message << '\n';
      }
    } catch (const qt::ai::Json::exception&) {
      std::cerr << "Warning: could not parse models.json for model listing\n";
    }
  }
  if (!parsed.offline) {
    const auto openai_catalog = qt::app::refresh_openai_models(
        catalog, auth_storage,
        parsed.provider == std::optional<std::string>{"openai"}
            ? parsed.api_key : std::nullopt);
    if (openai_catalog) {
      catalog = qt::ai::merge_catalogs(catalog, openai_catalog.value());
    }
    if (parsed.provider != std::optional<std::string>{"openai"}) {
      const auto openrouter_catalog =
          qt::ai::discover_openrouter_models(auth_storage);
      if (openrouter_catalog) {
        catalog = qt::ai::merge_catalogs(catalog, openrouter_catalog.value());
      }
    }
  }
  const std::string search = parsed.list_models.value_or(std::string{});
  struct ModelRow {
    std::string provider;
    std::string model;
    std::string context;
    std::string max_out;
    std::string thinking;
    std::string images;
    std::string capability;
    std::string usage;
  };
  std::vector<ModelRow> rows;
  std::map<std::string, bool> provider_availability;

  for (const auto& model : catalog.models) {
    auto available = provider_availability.find(model.ref.provider);
    if (available == provider_availability.end()) {
      const bool runtime_key = parsed.provider ==
          std::optional<std::string>{model.ref.provider} &&
          parsed.api_key && !parsed.api_key->empty();
      available = provider_availability.emplace(
          model.ref.provider, runtime_key ||
          model_is_available_for_listing(catalog, auth_storage, model)).first;
    }
    if (!available->second) {
      continue;
    }
    if (!model_matches_search(model, search)) {
      continue;
    }

    rows.push_back(ModelRow{
        model.ref.provider,
        model.ref.name,
        format_token_count(model_metadata_integer(model, "contextWindow")),
        format_token_count(model_metadata_integer(model, "maxTokens")),
        model.capabilities.reasoning ? "yes" : "no",
        model.capabilities.vision ? "yes" : "no",
        model.metadata.value("localDiscovery", qt::ai::Json::object())
            .value("capability", std::string{}),
        model.metadata.value("localDiscovery", qt::ai::Json::object())
            .value("bestUsage", std::string{}),
    });
  }

  if (rows.empty()) {
    if (search.empty()) {
      std::cout << "No models available. Run /login or configure credentials.\n";
    } else {
      std::cout << "No models matching \"" << search << "\"\n";
    }
    return EXIT_SUCCESS;
  }

  std::sort(rows.begin(), rows.end(), [](const ModelRow& lhs, const ModelRow& rhs) {
    return std::tie(lhs.provider, lhs.model) < std::tie(rhs.provider, rhs.model);
  });

  const std::string provider_header = "provider";
  const std::string model_header = "model";
  const std::string context_header = "context";
  const std::string max_out_header = "max-out";
  const std::string thinking_header = "thinking";
  const std::string images_header = "images";
  const std::string capability_header = "capability";
  const std::string usage_header = "usage";

  std::size_t provider_width = provider_header.size();
  std::size_t model_width = model_header.size();
  std::size_t context_width = context_header.size();
  std::size_t max_out_width = max_out_header.size();
  std::size_t thinking_width = thinking_header.size();
  std::size_t images_width = images_header.size();
  std::size_t capability_width = capability_header.size();
  std::size_t usage_width = usage_header.size();
  for (const auto& row : rows) {
    provider_width = std::max(provider_width, row.provider.size());
    model_width = std::max(model_width, row.model.size());
    context_width = std::max(context_width, row.context.size());
    max_out_width = std::max(max_out_width, row.max_out.size());
    thinking_width = std::max(thinking_width, row.thinking.size());
    images_width = std::max(images_width, row.images.size());
    capability_width = std::max(capability_width, row.capability.size());
    usage_width = std::max(usage_width, row.usage.size());
  }

  std::cout << pad_right(provider_header, provider_width) << "  "
            << pad_right(model_header, model_width) << "  "
            << pad_right(context_header, context_width) << "  "
            << pad_right(max_out_header, max_out_width) << "  "
            << pad_right(thinking_header, thinking_width) << "  "
            << pad_right(images_header, images_width) << "  "
            << pad_right(capability_header, capability_width) << "  "
            << pad_right(usage_header, usage_width) << '\n';
  for (const auto& row : rows) {
    std::cout << pad_right(row.provider, provider_width) << "  "
              << pad_right(row.model, model_width) << "  "
              << pad_right(row.context, context_width) << "  "
              << pad_right(row.max_out, max_out_width) << "  "
              << pad_right(row.thinking, thinking_width) << "  "
              << pad_right(row.images, images_width) << "  "
              << pad_right(row.capability, capability_width) << "  "
              << pad_right(row.usage, usage_width) << '\n';
  }
  return EXIT_SUCCESS;
}

std::string read_stdin_to_string() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  std::string content = buffer.str();
  while (!content.empty() &&
         std::isspace(static_cast<unsigned char>(content.front())) != 0) {
    content.erase(content.begin());
  }
  while (!content.empty() &&
         std::isspace(static_cast<unsigned char>(content.back())) != 0) {
    content.pop_back();
  }
  return content;
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

struct ResolvedExtensionFlagDefinitions {
  std::map<std::string, qt::extensions::ExtensionFlagContributionDescriptor>
      definitions;
  nlohmann::json defaults = nlohmann::json::object();
};

ResolvedExtensionFlagDefinitions collect_extension_flag_definitions(
    const qt::extensions::ExtensionContributionRegistry& registry) {
  ResolvedExtensionFlagDefinitions resolved;
  for (const auto& flag : registry.flags) {
    if (resolved.definitions.find(flag.name) != resolved.definitions.end()) {
      continue;
    }
    resolved.definitions.emplace(flag.name, flag);
    if (!flag.default_value.is_null()) {
      resolved.defaults[flag.name] = flag.default_value;
    }
  }
  return resolved;
}

qt::foundation::Result<nlohmann::json> resolve_extension_cli_flags(
    const ReferenceCliArgs& parsed, const std::string& cwd,
    const std::string& agent_home, const std::string& workspace_root) {
  qt::harness::FilesystemRepository repository(cwd);
  const auto loaded = qt::app::load_extensions(
      repository, cwd, agent_home, workspace_root,
      resolve_cli_resource_paths(default_cwd(), parsed.extensions),
      qt::app::LoadReferenceExtensionOptions{!parsed.no_extensions});
  const auto host = qt::extensions::make_extension_host_runtime(loaded);
  if (!host) {
    return qt::foundation::Result<nlohmann::json>::failure(
        host.error().code, host.error().message, host.error().system_error);
  }

  const auto definitions =
      collect_extension_flag_definitions(host.value().registry);
  nlohmann::json values = definitions.defaults;
  if (!parsed.unknown_flags.is_object()) {
    return qt::foundation::Result<nlohmann::json>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "unknown CLI flags must be an object");
  }

  for (auto it = parsed.unknown_flags.begin();
       it != parsed.unknown_flags.end(); ++it) {
    const auto found = definitions.definitions.find(it.key());
    if (found == definitions.definitions.end()) {
      return qt::foundation::Result<nlohmann::json>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "Unknown option: --" + it.key());
    }

    const auto& flag = found->second;
    if (flag.type == "boolean") {
      if (it->is_boolean()) {
        values[it.key()] = it->get<bool>();
        continue;
      }
      if (it->is_string()) {
        const std::string text = it->get<std::string>();
        if (text == "true" || text == "1") {
          values[it.key()] = true;
          continue;
        }
        if (text == "false" || text == "0") {
          values[it.key()] = false;
          continue;
        }
      }
      return qt::foundation::Result<nlohmann::json>::failure(
          qt::foundation::ErrorCode::invalid_argument,
          "Extension flag --" + it.key() + " expects a boolean value");
    }

    if (flag.type == "string") {
      if (!it->is_string()) {
        return qt::foundation::Result<nlohmann::json>::failure(
            qt::foundation::ErrorCode::invalid_argument,
            "Extension flag --" + it.key() + " requires a value");
      }
      values[it.key()] = it.value();
      continue;
    }

    return qt::foundation::Result<nlohmann::json>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "Extension flag --" + it.key() + " has unsupported type " +
            flag.type);
  }

  return qt::foundation::Result<nlohmann::json>::success(std::move(values));
}

}  // namespace

int run_cli_startup(const std::vector<std::string>& raw_args) {
  auto log = qt::foundation::logger("app", "cli_startup");
  log.info("startup_begin",
           {qt::foundation::log_field("raw_args", raw_args.size())});
  if (const auto memory_result = run_memory_command(raw_args)) {
    log.info("memory_command_exit",
             {qt::foundation::log_field("code", *memory_result)});
    return *memory_result;
  }
  if (const auto package_result = run_package_or_config_command(raw_args)) {
    log.info("package_command_exit",
             {qt::foundation::log_field("code", *package_result)});
    return *package_result;
  }

  if (!raw_args.empty()) {
    static const std::vector<std::string> package_commands = {
        "install", "remove", "uninstall", "update", "config"};
    if (std::find(package_commands.begin(), package_commands.end(), raw_args.front()) !=
        package_commands.end()) {
      std::cerr << "qt: package/config command '" << raw_args.front()
                << "' is recognized but not implemented in the native runtime yet\n";
      return EXIT_FAILURE;
    }
  }

  ReferenceCliArgs parsed = parse_cli_args(raw_args);
  log.debug("cli_parsed",
            {qt::foundation::log_field("messages", parsed.messages.size()),
             qt::foundation::log_field("files", parsed.file_args.size()),
             qt::foundation::log_field("print", parsed.print),
             qt::foundation::log_field("mode",
                                       parsed.mode.value_or("interactive"))});
  if (parsed.offline || env_or_empty("PI_OFFLINE") == "1") {
#if defined(_WIN32)
    _putenv_s("PI_OFFLINE", "1");
    _putenv_s("PI_SKIP_VERSION_CHECK", "1");
#else
    setenv("PI_OFFLINE", "1", 1);
    setenv("PI_SKIP_VERSION_CHECK", "1", 1);
#endif
  }

  bool has_error_diagnostic = false;
  for (const auto& diagnostic : parsed.diagnostics) {
    const bool is_error =
        diagnostic.severity == ReferenceCliDiagnosticSeverity::error;
    std::cerr << (is_error ? "Error: " : "Warning: ") << diagnostic.message
              << '\n';
    has_error_diagnostic = has_error_diagnostic || is_error;
  }
  if (has_error_diagnostic) {
    log.warn("cli_diagnostics_failed");
    return EXIT_FAILURE;
  }
  if (parsed.version) {
    std::cout << "0.0.0" << '\n';
    return EXIT_SUCCESS;
  }
  if (parsed.help) {
    const bool stdin_is_tty = QT_ISATTY(QT_FILENO(stdin)) != 0;
    const auto mode = resolve_startup_mode(parsed, stdin_is_tty);
    print_help(mode == ReferenceStartupMode::interactive ? std::cout
                                                                   : std::cerr);
    return EXIT_SUCCESS;
  }
  const std::string cwd = default_cwd();
  const std::string agent_home = default_agent_home();
  (void)qt::app::run_startup_migrations(cwd, agent_home);
  if (parsed.export_path) {
    std::error_code exists_error;
    const bool input_exists =
        std::filesystem::exists(*parsed.export_path, exists_error);
    if (exists_error || !input_exists) {
      std::cerr << "Error: File not found: " << *parsed.export_path << '\n';
      return EXIT_FAILURE;
    }
    std::optional<std::string_view> requested_output;
    if (!parsed.messages.empty()) {
      requested_output = parsed.messages.front();
    }
    const auto exported = qt::app::export_session_file_to_html(
        *parsed.export_path, requested_output, "qt");
    if (!exported) {
      std::cerr << "Error: " << exported.error().message << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "Exported to: " << exported.value() << '\n';
    return EXIT_SUCCESS;
  }
  if (parsed.list_models || parsed.list_models_all) {
    log.info("list_models");
    return run_list_models(parsed, agent_home);
  }
  if (parsed.mode == "rpc" && !parsed.file_args.empty()) {
    std::cerr << "Error: @file arguments are not supported in RPC mode\n";
    return EXIT_FAILURE;
  }

  if (parsed.fork) {
    std::vector<std::string> conflicting_flags;
    if (parsed.session) {
      conflicting_flags.push_back("--session");
    }
    if (parsed.continue_session) {
      conflicting_flags.push_back("--continue");
    }
    if (parsed.resume) {
      conflicting_flags.push_back("--resume");
    }
    if (parsed.no_session) {
      conflicting_flags.push_back("--no-session");
    }
    if (!conflicting_flags.empty()) {
      std::cerr << "Error: --fork cannot be combined with "
                << join_strings(conflicting_flags, ", ") << '\n';
      return EXIT_FAILURE;
    }
  }
  const auto session_dir =
      resolve_startup_session_dir(parsed, cwd, agent_home);
  if (!session_dir) {
    std::cerr << "Error: " << session_dir.error().message << '\n';
    return EXIT_FAILURE;
  }
  if (const auto missing_session_arg =
          missing_session_lookup_arg(parsed, session_dir.value())) {
    std::cerr << "No session found matching '" << *missing_session_arg << "'\n";
    return EXIT_FAILURE;
  }
  if (parsed.fork && session_arg_looks_like_path(*parsed.fork) &&
      !session_file_has_header(*parsed.fork)) {
    std::cerr << "Error: Cannot fork: source session file is empty or invalid: "
              << *parsed.fork << '\n';
    return EXIT_FAILURE;
  }
  if (parsed.api_key && !parsed.model && parsed.models.empty()) {
    std::cerr << "Error: --api-key requires a model to be specified via "
                 "--model, --provider/--model, or --models\n";
    return EXIT_FAILURE;
  }
  auto startup_session =
      resolve_startup_session(parsed, session_dir.value(), cwd);
  if (startup_session.stdout_message) {
    StreamOutputPlan output;
    output.exit_code = startup_session.exit_code;
    output.stdout_chunks.push_back(*startup_session.stdout_message + "\n");
    emit_stream_output(output, std::cout, std::cerr);
    return startup_session.exit_code;
  }
  if (startup_session.error) {
    if (startup_session.error->rfind("No session found matching", 0) == 0) {
      std::cerr << *startup_session.error << '\n';
    } else {
      std::cerr << "Error: " << *startup_session.error << '\n';
    }
    return EXIT_FAILURE;
  }

  const bool stdin_is_tty = QT_ISATTY(QT_FILENO(stdin)) != 0;
  auto mode = resolve_startup_mode(parsed, stdin_is_tty);
  auto missing_session_cwd =
      missing_session_cwd_issue(startup_session.session, cwd);
  if (!missing_session_cwd && parsed.session &&
      session_arg_looks_like_path(*parsed.session)) {
    missing_session_cwd =
        missing_session_cwd_issue_from_file(*parsed.session, cwd);
  }
  if (missing_session_cwd) {
    const bool force_prompt =
        env_or_empty("QT_FORCE_MISSING_SESSION_CWD_PROMPT") == "1";
    if (mode == ReferenceStartupMode::interactive || force_prompt) {
      std::cout << format_missing_session_cwd_prompt(
                       missing_session_cwd.value())
                << "\n\n[Continue/Cancel] " << std::flush;
      std::string choice;
      if (!std::getline(std::cin, choice) ||
          !missing_session_cwd_choice_is_continue(choice)) {
        std::cout << '\n';
        return EXIT_SUCCESS;
      }
      startup_session.session.working_directory = cwd;
    } else {
      std::cerr
          << format_missing_session_cwd_error(missing_session_cwd.value())
          << '\n';
      return EXIT_FAILURE;
    }
  }

  const auto extension_flags = resolve_extension_cli_flags(
      parsed, startup_session.session.working_directory.value_or(cwd),
      agent_home, startup_session.session.working_directory.value_or(cwd));
  if (!extension_flags) {
    std::cerr << "Error: " << extension_flags.error().message << '\n';
    return EXIT_FAILURE;
  }
  parsed.extension_flags = extension_flags.value();

  std::optional<std::string> stdin_content;
  if (mode != ReferenceStartupMode::rpc && !stdin_is_tty) {
    stdin_content = read_stdin_to_string();
    if (stdin_content && stdin_content->empty()) {
      stdin_content.reset();
    }
    if (mode == ReferenceStartupMode::interactive && stdin_content) {
      mode = ReferenceStartupMode::print;
    }
  }

  if (mode == ReferenceStartupMode::rpc) {
    log.info("startup_mode",
             {qt::foundation::log_field("mode", "rpc")});
    return run_rpc_mode(parsed, startup_session.session, cwd, agent_home);
  }
  if (mode == ReferenceStartupMode::print || mode == ReferenceStartupMode::json) {
    log.info("startup_mode",
             {qt::foundation::log_field(
                 "mode", mode == ReferenceStartupMode::json ? "json" : "print")});
    return run_print_or_json_mode(parsed, mode,
                                            startup_session.session,
                                            stdin_content);
  }
  log.info("startup_mode", {qt::foundation::log_field("mode", "interactive")});
  return run_interactive_mode(
      ReferenceInteractiveModeRequest{
          parsed,
          startup_session.session,
          stdin_content,
          cwd,
          session_dir.value(),
      });
}

}  // namespace qt::app
