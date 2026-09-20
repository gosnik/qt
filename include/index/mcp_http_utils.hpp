#pragma once

#include "index/mcp_http_request.hpp"

#include <nlohmann/json_fwd.hpp>

namespace qt::index {

nlohmann::json util_json_validate_payload(const HttpRequest& request);
nlohmann::json util_json_format_payload(const HttpRequest& request);
nlohmann::json util_regex_test_payload(const HttpRequest& request);
nlohmann::json util_base64_encode_payload(const HttpRequest& request);
nlohmann::json util_base64_decode_payload(const HttpRequest& request);
nlohmann::json util_uuid_payload(const HttpRequest& request);
nlohmann::json util_hash_payload(const HttpRequest& request);
nlohmann::json util_jwt_decode_payload(const HttpRequest& request);
nlohmann::json util_text_stats_payload(const HttpRequest& request);
nlohmann::json util_slugify_payload(const HttpRequest& request);
nlohmann::json util_url_parse_payload(const HttpRequest& request);
nlohmann::json util_markdown_to_html_payload(const HttpRequest& request);
nlohmann::json util_html_to_markdown_payload(const HttpRequest& request);
nlohmann::json util_csv_to_json_payload(const HttpRequest& request);
nlohmann::json util_json_to_csv_payload(const HttpRequest& request);
nlohmann::json util_timestamp_payload(const HttpRequest& request);
nlohmann::json util_datetime_format_payload(const HttpRequest& request);
nlohmann::json util_cron_next_payload(const HttpRequest& request);
nlohmann::json util_text_diff_payload(const HttpRequest& request);

}  // namespace qt::index
