#pragma once

#include "index/mcp_http_request.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json serial_list_payload(const HttpRequest& request);
nlohmann::json serial_open_payload(const HttpRequest& request);
nlohmann::json serial_close_payload(const HttpRequest& request);
nlohmann::json serial_write_payload(const HttpRequest& request);
nlohmann::json serial_read_payload(const HttpRequest& request);
nlohmann::json serial_clear_payload(const HttpRequest& request);
nlohmann::json serial_transact_payload(const HttpRequest& request);

}  // namespace qt::index
