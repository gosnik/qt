#include "qt/app/cli/auxiliary_cli.hpp"
#include "qt/app/cli/auxiliary_mode.hpp"
#include "qt/app/cli/cli.hpp"
#include "qt/app/cli/cli_startup.hpp"
#include "qt/foundation/logging.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::optional<std::string> option_value(std::string_view arg,
                                        std::string_view prefix) {
  if (arg.rfind(prefix, 0) != 0 || arg.size() <= prefix.size()) {
    return std::nullopt;
  }
  return std::string(arg.substr(prefix.size()));
}

bool looks_like_option(std::string_view arg) {
  return arg.size() > 1 && arg[0] == '-';
}

qt::foundation::LoggingOptions logging_options_from_args(
    const std::vector<std::string>& args) {
  auto options = qt::foundation::logging_options_from_environment();
  for (std::size_t index = 0; index < args.size(); ++index) {
    const auto& arg = args[index];
    if (arg == "--log-file" && index + 1 < args.size() &&
        !looks_like_option(args[index + 1])) {
      options.files.push_back(args[++index]);
      continue;
    }
    if (const auto value = option_value(arg, "--log-file=")) {
      options.files.push_back(*value);
      continue;
    }
    if (arg == "--log-stderr") {
      options.stderr_enabled = true;
      continue;
    }
    if (arg == "--log-level" && index + 1 < args.size() &&
        !looks_like_option(args[index + 1])) {
      if (const auto level = qt::foundation::parse_log_level(args[++index])) {
        options.level = *level;
      }
      continue;
    }
    if (const auto value = option_value(arg, "--log-level=")) {
      if (const auto level = qt::foundation::parse_log_level(*value)) {
        options.level = *level;
      }
      continue;
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> raw_args;
  raw_args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int i = 1; i < argc; ++i) {
    raw_args.emplace_back(argv[i]);
  }

#if defined(_WIN32)
  _putenv_s("PI_CODING_AGENT", "true");
#else
  setenv("PI_CODING_AGENT", "true", 1);
#endif

  qt::foundation::configure_logging(logging_options_from_args(raw_args));
  auto log = qt::foundation::logger("app", "main");
  log.info("process_start",
           {qt::foundation::log_field("argc", argc),
            qt::foundation::log_field("native_mode",
                                      qt::app::has_native_mode(raw_args))});

  if (!qt::app::has_native_mode(raw_args)) {
    const int code = qt::app::run_cli_startup(raw_args);
    log.info("process_exit", {qt::foundation::log_field("code", code)});
    qt::foundation::shutdown_logging();
    return code;
  }

  const auto parsed =
      qt::app::parse_auxiliary_cli_args(argc, argv);
  if (!parsed.ok) {
    log.error("auxiliary_parse_failed",
              {qt::foundation::log_field("error", parsed.error_text)});
    std::cerr << parsed.error_text;
    qt::foundation::shutdown_logging();
    return EXIT_FAILURE;
  }

  try {
    const int code = qt::app::run_auxiliary_mode(parsed.args);
    log.info("process_exit", {qt::foundation::log_field("code", code)});
    qt::foundation::shutdown_logging();
    return code;
  } catch (const std::exception& error) {
    log.error("process_exception",
              {qt::foundation::log_field("error", error.what())});
    std::cerr << "qt: " << error.what() << '\n';
    qt::foundation::shutdown_logging();
    return EXIT_FAILURE;
  }
}
