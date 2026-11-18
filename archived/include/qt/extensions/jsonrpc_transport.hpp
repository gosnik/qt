#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

#include <qt/foundation/error.hpp>

namespace qt::extensions {

using Json = nlohmann::json;

constexpr const char* kJsonRpcVersion = "2.0";

enum class JsonRpcMessageKind {
  kRequest,
  kNotification,
  kResponse,
  kErrorResponse,
};

struct JsonRpcRequest {
  std::string method;
  Json params = Json::object();
  Json id = nullptr;
};

struct JsonRpcNotification {
  std::string method;
  Json params = Json::object();
};

struct JsonRpcError {
  int code = 0;
  std::string message;
  Json data = nullptr;
};

struct JsonRpcResponse {
  Json id = nullptr;
  Json result = nullptr;
};

struct JsonRpcErrorResponse {
  Json id = nullptr;
  Json error = Json::object();
};

std::string to_string(JsonRpcMessageKind kind);

bool is_valid_jsonrpc_request_id(const Json& id) noexcept;
bool is_valid_jsonrpc_response_id(const Json& id) noexcept;
bool is_valid_jsonrpc_params(const Json& params) noexcept;

qt::foundation::Result<JsonRpcRequest> make_jsonrpc_request(
    std::string method, Json params, Json id);
qt::foundation::Result<JsonRpcNotification> make_jsonrpc_notification(
    std::string method, Json params = Json::object());
qt::foundation::Result<JsonRpcResponse> make_jsonrpc_response(Json id,
                                                              Json result);
qt::foundation::Result<JsonRpcError> make_jsonrpc_error(int code,
                                                        std::string message,
                                                        Json data = nullptr);
qt::foundation::Result<JsonRpcErrorResponse> make_jsonrpc_error_response(
    Json id, int code, std::string message, Json data = nullptr);

qt::foundation::Result<JsonRpcMessageKind> classify_jsonrpc_message(
    const Json& json);

qt::foundation::Result<JsonRpcRequest> decode_jsonrpc_request(const Json& json);
qt::foundation::Result<JsonRpcNotification> decode_jsonrpc_notification(
    const Json& json);
qt::foundation::Result<JsonRpcResponse> decode_jsonrpc_response(
    const Json& json);
qt::foundation::Result<JsonRpcError> decode_jsonrpc_error(const Json& json);
qt::foundation::Result<JsonRpcErrorResponse> decode_jsonrpc_error_response(
    const Json& json);

qt::foundation::Result<JsonRpcMessageKind> classify_jsonrpc_line(
    std::string_view line);
qt::foundation::Result<JsonRpcRequest> decode_jsonrpc_request_line(
    std::string_view line);
qt::foundation::Result<JsonRpcNotification> decode_jsonrpc_notification_line(
    std::string_view line);
qt::foundation::Result<JsonRpcResponse> decode_jsonrpc_response_line(
    std::string_view line);
qt::foundation::Result<JsonRpcErrorResponse> decode_jsonrpc_error_response_line(
    std::string_view line);

qt::foundation::Result<std::string> encode_jsonrpc_request_line(
    const JsonRpcRequest& request);
qt::foundation::Result<std::string> encode_jsonrpc_notification_line(
    const JsonRpcNotification& notification);
qt::foundation::Result<std::string> encode_jsonrpc_response_line(
    const JsonRpcResponse& response);
qt::foundation::Result<std::string> encode_jsonrpc_error_response_line(
    const JsonRpcErrorResponse& response);

void to_json(Json& json, const JsonRpcRequest& request);
void from_json(const Json& json, JsonRpcRequest& request);

void to_json(Json& json, const JsonRpcNotification& notification);
void from_json(const Json& json, JsonRpcNotification& notification);

void to_json(Json& json, const JsonRpcError& error);
void from_json(const Json& json, JsonRpcError& error);

void to_json(Json& json, const JsonRpcResponse& response);
void from_json(const Json& json, JsonRpcResponse& response);

void to_json(Json& json, const JsonRpcErrorResponse& response);
void from_json(const Json& json, JsonRpcErrorResponse& response);

}  // namespace qt::extensions
