#pragma once

#include "index/mcp_http_request.hpp"
#include "index/service.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json rtos_task_graph_payload(const BackgroundService& service, const HttpRequest& request);

}  // namespace qt::index
