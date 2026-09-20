#pragma once

#include "index/mcp_http_request.hpp"
#include "index/service.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json filesystem_stat_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json filesystem_list_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json filesystem_tree_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json filesystem_read_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json filesystem_search_payload(const BackgroundService& service, const HttpRequest& request);

}  // namespace qt::index
