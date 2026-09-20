#pragma once

#include "index/mcp_http_request.hpp"
#include "index/service.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json read_context_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json files_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json file_info_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json search_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json ast_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json project_summary_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json file_analysis_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json symbol_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json memory_graph_payload(const BackgroundService& service);
nlohmann::json graph_schema_payload(const BackgroundService& service);
nlohmann::json graph_schema_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json search_graph_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json trace_call_path_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json dead_code_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json code_snippet_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json code_index_payload(const BackgroundService& service);
nlohmann::json persist_memory_graph(const BackgroundService& service, nlohmann::json graph);
nlohmann::json classes_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json functions_payload(const BackgroundService& service, const HttpRequest& request);
nlohmann::json callgraph_payload(const BackgroundService& service, const HttpRequest& request);

}  // namespace qt::index
