#pragma once

#include "index/mcp_http_request.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json fetch_payload(const HttpRequest& request);

}  // namespace qt::index
