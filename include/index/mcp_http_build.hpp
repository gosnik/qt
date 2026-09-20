#pragma once

#include "index/mcp_http_request.hpp"
#include "index/service.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json cmake_compile_commands_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json cmake_compile_flags_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json cmake_include_graph_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json cmake_target_graph_payload(const BackgroundService& service, const HttpRequest& request);

}  // namespace qt::index
