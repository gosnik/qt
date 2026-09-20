#pragma once

#include "index/mcp_http_request.hpp"
#include "index/service.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json debug_gdb_command_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json debug_breakpoint_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json debug_memory_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json debug_registers_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json debug_backtrace_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json debug_step_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json debug_rtos_tasks_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json openocd_payload(const HttpRequest& request);
nlohmann::json debug_tools_payload(const HttpRequest& request);

}  // namespace qt::index
