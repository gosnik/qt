#pragma once

#include "qt/ai/auth_storage.hpp"
#include "qt/ai/provider_transport.hpp"
#include "qt/ai/models.hpp"
#include "qt/app/config/settings.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qt::app {

struct ReferenceLatestCliRelease {
  std::string version;
  std::optional<std::string> package_name;
};

std::optional<int> compare_package_versions(
    std::string_view left_version, std::string_view right_version);
bool is_newer_package_version(std::string_view candidate_version,
                                        std::string_view current_version);
std::optional<ReferenceLatestCliRelease> get_latest_cli_release(
    std::string_view current_version, ai::HttpTransport& transport,
    std::int64_t timeout_ms = 10000);
std::optional<ReferenceLatestCliRelease> check_for_new_cli_release(
    std::string_view current_version, ai::HttpTransport& transport,
    std::int64_t timeout_ms = 10000);
std::optional<ReferenceLatestCliRelease> check_for_new_cli_release(
    std::string_view current_version, std::int64_t timeout_ms = 10000);
std::vector<std::string> make_qt_startup_banner_lines();
std::vector<std::string> make_new_version_notification_lines(
    std::string_view new_version);

std::vector<std::string> collect_interactive_startup_diagnostics(
    const AppSettings& settings,
    const std::vector<std::string>& runtime_diagnostics);

std::optional<std::string> make_anthropic_subscription_auth_warning(
    const ai::AuthStorage& auth_storage, const ai::ModelRef& model);

}  // namespace qt::app
