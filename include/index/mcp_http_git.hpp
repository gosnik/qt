#pragma once

#include "index/mcp_http_request.hpp"
#include "index/service.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json git_status_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json git_log_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json git_diff_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json git_show_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json git_blame_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json git_branches_payload(const BackgroundService& service, const HttpRequest& request);

}  // namespace qt::index
