#include "qt/app/interactive/interactive_startup_diagnostics.hpp"

#include "qt/foundation/pi_user_agent.hpp"
#include "qt/tools/package_manager_tool.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace qt::app {
namespace {

constexpr std::string_view kAnthropicSubscriptionAuthWarning =
    "Anthropic subscription auth is active. Third-party harness usage draws "
    "from extra usage and is billed per token, not your Claude plan limits. "
    "Manage extra usage at https://claude.ai/settings/usage.";
constexpr std::string_view kLatestVersionUrl =
    "https://pi.dev/api/latest-version";

struct ParsedVersion {
  int major = 0;
  int minor = 0;
  int patch = 0;
  std::optional<std::string> prerelease;
};

bool env_flag_enabled(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }

  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return normalized == "1" || normalized == "true" || normalized == "yes" ||
         normalized == "on";
}

bool is_anthropic_subscription_auth_key(std::string_view secret) {
  return secret.rfind("sk-ant-oat", 0) == 0;
}

void append_unique_message(std::vector<std::string>& diagnostics,
                           std::string message) {
  if (message.empty()) {
    return;
  }
  if (std::find(diagnostics.begin(), diagnostics.end(), message) ==
      diagnostics.end()) {
    diagnostics.push_back(std::move(message));
  }
}

std::optional<ParsedVersion> parse_package_version(
    std::string_view version) {
  std::string trimmed(version);
  trimmed.erase(trimmed.begin(),
                std::find_if(trimmed.begin(), trimmed.end(), [](char ch) {
                  return std::isspace(static_cast<unsigned char>(ch)) == 0;
                }));
  trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), [](char ch) {
                  return std::isspace(static_cast<unsigned char>(ch)) == 0;
                }).base(),
                trimmed.end());
  if (trimmed.empty()) {
    return std::nullopt;
  }

  std::size_t cursor = 0;
  if (trimmed[cursor] == 'v' || trimmed[cursor] == 'V') {
    ++cursor;
  }

  const auto parse_number = [&](int& out) -> bool {
    if (cursor >= trimmed.size() ||
        std::isdigit(static_cast<unsigned char>(trimmed[cursor])) == 0) {
      return false;
    }
    int value = 0;
    while (cursor < trimmed.size() &&
           std::isdigit(static_cast<unsigned char>(trimmed[cursor])) != 0) {
      value = (value * 10) + (trimmed[cursor] - '0');
      ++cursor;
    }
    out = value;
    return true;
  };

  ParsedVersion parsed;
  if (!parse_number(parsed.major) || cursor >= trimmed.size() ||
      trimmed[cursor] != '.') {
    return std::nullopt;
  }
  ++cursor;
  if (!parse_number(parsed.minor) || cursor >= trimmed.size() ||
      trimmed[cursor] != '.') {
    return std::nullopt;
  }
  ++cursor;
  if (!parse_number(parsed.patch)) {
    return std::nullopt;
  }

  if (cursor < trimmed.size() && trimmed[cursor] == '-') {
    const std::size_t prerelease_start = ++cursor;
    while (cursor < trimmed.size() && trimmed[cursor] != '+') {
      ++cursor;
    }
    if (cursor == prerelease_start) {
      return std::nullopt;
    }
    parsed.prerelease = trimmed.substr(prerelease_start, cursor - prerelease_start);
  }

  if (cursor < trimmed.size() && trimmed[cursor] == '+') {
    cursor = trimmed.size();
  }
  if (cursor != trimmed.size()) {
    return std::nullopt;
  }

  return parsed;
}

std::optional<std::string> make_package_update_warning(
    const AppSettings& settings) {
  if (settings.cwd.empty() || settings.agent_home.empty() ||
      env_flag_enabled("PI_OFFLINE")) {
    return std::nullopt;
  }

  const auto package_settings = qt::tools::load_package_command_settings(
      settings.workspace_root.empty() ? settings.cwd : settings.workspace_root,
      settings.agent_home);
  if (!package_settings) {
    return std::nullopt;
  }

  qt::tools::PackageCommandParseResult parsed;
  parsed.command = qt::tools::PackageCommandKind::update;
  parsed.update_target = qt::tools::PackageUpdateTarget{
      qt::tools::PackageUpdateTargetKind::extensions, std::nullopt};

  const auto plan = qt::tools::make_package_settings_plan(
      parsed,
      settings.workspace_root.empty() ? settings.cwd : settings.workspace_root,
      package_settings.value());
  if (plan.package_updates.empty()) {
    return std::nullopt;
  }

  std::string joined_sources;
  for (const auto& source : plan.package_updates) {
    if (!joined_sources.empty()) {
      joined_sources += ", ";
    }
    joined_sources += source.source;
  }

  return "Package updates available: " + joined_sources;
}

}  // namespace

std::optional<int> compare_package_versions(
    std::string_view left_version, std::string_view right_version) {
  const auto left = parse_package_version(left_version);
  const auto right = parse_package_version(right_version);
  if (!left || !right) {
    return std::nullopt;
  }

  if (left->major != right->major) {
    return left->major - right->major;
  }
  if (left->minor != right->minor) {
    return left->minor - right->minor;
  }
  if (left->patch != right->patch) {
    return left->patch - right->patch;
  }
  if (left->prerelease == right->prerelease) {
    return 0;
  }
  if (!left->prerelease) {
    return 1;
  }
  if (!right->prerelease) {
    return -1;
  }
  return left->prerelease->compare(*right->prerelease);
}

bool is_newer_package_version(std::string_view candidate_version,
                                        std::string_view current_version) {
  if (const auto comparison = compare_package_versions(
          candidate_version, current_version)) {
    return *comparison > 0;
  }
  return std::string(candidate_version) != std::string(current_version);
}

std::optional<ReferenceLatestCliRelease> get_latest_cli_release(
    std::string_view current_version, ai::HttpTransport& transport,
    std::int64_t timeout_ms) {
  if (env_flag_enabled("PI_SKIP_VERSION_CHECK") ||
      env_flag_enabled("PI_OFFLINE")) {
    return std::nullopt;
  }

  ai::HttpRequest request;
  request.method = "GET";
  request.url = std::string(kLatestVersionUrl);
  request.headers["User-Agent"] =
      qt::foundation::pi_user_agent(current_version);
  request.headers["accept"] = "application/json";
  request.timeout_ms = timeout_ms;

  try {
    const auto response = transport.send(request);
    if (response.status < 200 || response.status >= 300) {
      return std::nullopt;
    }

    const auto payload = nlohmann::json::parse(response.body, nullptr, false);
    if (!payload.is_object()) {
      return std::nullopt;
    }

    const auto version_it = payload.find("version");
    if (version_it == payload.end() || !version_it->is_string()) {
      return std::nullopt;
    }

    const std::string version = version_it->get<std::string>();
    if (version.empty()) {
      return std::nullopt;
    }

    ReferenceLatestCliRelease release;
    release.version = version;
    const auto package_name_it = payload.find("packageName");
    if (package_name_it != payload.end() && package_name_it->is_string()) {
      const std::string package_name = package_name_it->get<std::string>();
      if (!package_name.empty()) {
        release.package_name = package_name;
      }
    }
    return release;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<ReferenceLatestCliRelease> check_for_new_cli_release(
    std::string_view current_version, ai::HttpTransport& transport,
    std::int64_t timeout_ms) {
  const auto release =
      get_latest_cli_release(current_version, transport, timeout_ms);
  if (!release ||
      !is_newer_package_version(release->version, current_version)) {
    return std::nullopt;
  }
  return release;
}

std::optional<ReferenceLatestCliRelease> check_for_new_cli_release(
    std::string_view current_version, std::int64_t timeout_ms) {
  auto transport = std::make_unique<ai::BeastHttpTransport>();
  return check_for_new_cli_release(current_version, *transport,
                                             timeout_ms);
}

std::vector<std::string> make_qt_startup_banner_lines() {
  return {"       _   ",
          "  __ _| |_ ",
          " / _` | __|",
          "| (_| | |_ ",
          " \\__, |\\__|",
          "    |_|   "};
}

std::vector<std::string> make_new_version_notification_lines(
    std::string_view) {
  return {};
}

std::vector<std::string> collect_interactive_startup_diagnostics(
    const AppSettings& settings,
    const std::vector<std::string>& runtime_diagnostics) {
  std::vector<std::string> diagnostics = runtime_diagnostics;
  if (const auto package_updates =
          make_package_update_warning(settings)) {
    append_unique_message(diagnostics, *package_updates);
  }
  return diagnostics;
}

std::optional<std::string> make_anthropic_subscription_auth_warning(
    const ai::AuthStorage& auth_storage, const ai::ModelRef& model) {
  if (model.provider != "anthropic") {
    return std::nullopt;
  }

  if (const auto* stored = ai::find_provider_auth_record(auth_storage,
                                                         "anthropic")) {
    if (stored->metadata.value("type", std::string()) == "oauth") {
      return std::string(kAnthropicSubscriptionAuthWarning);
    }
    if (stored->api_key && is_anthropic_subscription_auth_key(*stored->api_key)) {
      return std::string(kAnthropicSubscriptionAuthWarning);
    }
    if (stored->token && is_anthropic_subscription_auth_key(*stored->token)) {
      return std::string(kAnthropicSubscriptionAuthWarning);
    }
  }

  if (const auto env_secret = ai::get_env_api_key("anthropic");
      env_secret && is_anthropic_subscription_auth_key(*env_secret)) {
    return std::string(kAnthropicSubscriptionAuthWarning);
  }

  return std::nullopt;
}

}  // namespace qt::app
