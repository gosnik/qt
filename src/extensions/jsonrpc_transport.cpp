#include <qt/extensions/jsonrpc_transport.hpp>

#include <stdexcept>
#include <utility>

namespace qt::extensions {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

bool is_integer_number(const Json& value) {
  return value.is_number_integer() || value.is_number_unsigned();
}

bool is_jsonrpc_version(const Json& json) {
  return json.value("jsonrpc", std::string(kJsonRpcVersion)) ==
         std::string(kJsonRpcVersion);
}

void require_jsonrpc_version(const Json& json) {
  if (!is_jsonrpc_version(json)) {
    throw std::runtime_error("jsonrpc field must equal '2.0'");
  }
}

void require_object(const Json& json, const char* context) {
  if (!json.is_object()) {
    throw std::runtime_error(std::string(context) + " must be a JSON object");
  }
}

void require_method(const std::string& method, const char* context) {
  if (method.empty()) {
    throw std::runtime_error(std::string(context) + " method must not be empty");
  }
}

void require_params(const Json& params, const char* context) {
  if (!is_valid_jsonrpc_params(params)) {
    throw std::runtime_error(std::string(context) +
                             " params must be a JSON object or array");
  }
}

void require_request_id(const Json& id, const char* context) {
  if (!is_valid_jsonrpc_request_id(id)) {
    throw std::runtime_error(std::string(context) +
                             " id must be a string or integer");
  }
}

void require_response_id(const Json& id, const char* context) {
  if (!is_valid_jsonrpc_response_id(id)) {
    throw std::runtime_error(std::string(context) +
                             " id must be null, a string, or an integer");
  }
}

std::string trim_single_line_ending(std::string_view line) {
  if (line.empty()) {
    throw std::runtime_error("jsonrpc frame must not be empty");
  }

  std::string payload(line);
  if (!payload.empty() && payload.back() == '\n') {
    payload.pop_back();
    if (!payload.empty() && payload.back() == '\r') {
      payload.pop_back();
    }
  }

  if (payload.empty()) {
    throw std::runtime_error("jsonrpc frame payload must not be empty");
  }
  if (payload.find('\n') != std::string::npos ||
      payload.find('\r') != std::string::npos) {
    throw std::runtime_error("jsonrpc frame must contain exactly one line");
  }

  return payload;
}

Result<Json> parse_jsonrpc_line(std::string_view line) {
  try {
    return Json::parse(trim_single_line_ending(line));
  } catch (const std::exception& error) {
    return Result<Json>::failure(ErrorCode::encoding_error, error.what());
  }
}

template <typename T>
Result<T> decode_jsonrpc_value(const Json& json) {
  try {
    return json.get<T>();
  } catch (const std::exception& error) {
    return Result<T>::failure(ErrorCode::encoding_error, error.what());
  }
}

template <typename T>
Result<T> decode_jsonrpc_line_value(std::string_view line) {
  const auto parsed = parse_jsonrpc_line(line);
  if (!parsed) {
    return parsed.error();
  }
  return decode_jsonrpc_value<T>(parsed.value());
}

Result<std::string> encode_jsonrpc_line(const Json& json) {
  try {
    require_object(json, "jsonrpc message");
    return json.dump() + "\n";
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

}  // namespace

std::string to_string(JsonRpcMessageKind kind) {
  switch (kind) {
    case JsonRpcMessageKind::kRequest:
      return "request";
    case JsonRpcMessageKind::kNotification:
      return "notification";
    case JsonRpcMessageKind::kResponse:
      return "response";
    case JsonRpcMessageKind::kErrorResponse:
      return "error_response";
  }

  return "request";
}

bool is_valid_jsonrpc_request_id(const Json& id) noexcept {
  return id.is_string() || is_integer_number(id);
}

bool is_valid_jsonrpc_response_id(const Json& id) noexcept {
  return id.is_null() || is_valid_jsonrpc_request_id(id);
}

bool is_valid_jsonrpc_params(const Json& params) noexcept {
  return params.is_object() || params.is_array();
}

Result<JsonRpcRequest> make_jsonrpc_request(std::string method, Json params,
                                            Json id) {
  try {
    require_method(method, "jsonrpc request");
    require_params(params, "jsonrpc request");
    require_request_id(id, "jsonrpc request");
    return JsonRpcRequest{std::move(method), std::move(params), std::move(id)};
  } catch (const std::exception& error) {
    return Result<JsonRpcRequest>::failure(ErrorCode::invalid_argument,
                                           error.what());
  }
}

Result<JsonRpcNotification> make_jsonrpc_notification(std::string method,
                                                      Json params) {
  try {
    require_method(method, "jsonrpc notification");
    require_params(params, "jsonrpc notification");
    return JsonRpcNotification{std::move(method), std::move(params)};
  } catch (const std::exception& error) {
    return Result<JsonRpcNotification>::failure(ErrorCode::invalid_argument,
                                                error.what());
  }
}

Result<JsonRpcResponse> make_jsonrpc_response(Json id, Json result) {
  try {
    require_response_id(id, "jsonrpc response");
    return JsonRpcResponse{std::move(id), std::move(result)};
  } catch (const std::exception& error) {
    return Result<JsonRpcResponse>::failure(ErrorCode::invalid_argument,
                                            error.what());
  }
}

Result<JsonRpcError> make_jsonrpc_error(int code, std::string message,
                                        Json data) {
  if (message.empty()) {
    return Result<JsonRpcError>::failure(ErrorCode::invalid_argument,
                                         "jsonrpc error message must not be empty");
  }

  return JsonRpcError{code, std::move(message), std::move(data)};
}

Result<JsonRpcErrorResponse> make_jsonrpc_error_response(Json id, int code,
                                                         std::string message,
                                                         Json data) {
  try {
    require_response_id(id, "jsonrpc error response");
  } catch (const std::exception& error) {
    return Result<JsonRpcErrorResponse>::failure(ErrorCode::invalid_argument,
                                                 error.what());
  }

  const auto error_result =
      make_jsonrpc_error(code, std::move(message), std::move(data));
  if (!error_result) {
    return Result<JsonRpcErrorResponse>::failure(error_result.error().code,
                                                 error_result.error().message);
  }

  Json error_json;
  to_json(error_json, error_result.value());
  return JsonRpcErrorResponse{std::move(id), std::move(error_json)};
}

Result<JsonRpcMessageKind> classify_jsonrpc_message(const Json& json) {
  try {
    require_object(json, "jsonrpc message");
    require_jsonrpc_version(json);

    const bool has_method = json.contains("method");
    const bool has_id = json.contains("id");
    const bool has_result = json.contains("result");
    const bool has_error = json.contains("error");

    if (has_method) {
      if (has_result || has_error) {
        throw std::runtime_error(
            "jsonrpc method message must not contain result or error");
      }

      if (has_id) {
        return JsonRpcMessageKind::kRequest;
      }
      return JsonRpcMessageKind::kNotification;
    }

    if (!has_id) {
      throw std::runtime_error("jsonrpc response must contain id");
    }
    if (has_result == has_error) {
      throw std::runtime_error(
          "jsonrpc response must contain exactly one of result or error");
    }

    if (has_error) {
      return JsonRpcMessageKind::kErrorResponse;
    }
    return JsonRpcMessageKind::kResponse;
  } catch (const std::exception& error) {
    return Result<JsonRpcMessageKind>::failure(ErrorCode::encoding_error,
                                               error.what());
  }
}

Result<JsonRpcRequest> decode_jsonrpc_request(const Json& json) {
  return decode_jsonrpc_value<JsonRpcRequest>(json);
}

Result<JsonRpcNotification> decode_jsonrpc_notification(const Json& json) {
  return decode_jsonrpc_value<JsonRpcNotification>(json);
}

Result<JsonRpcResponse> decode_jsonrpc_response(const Json& json) {
  return decode_jsonrpc_value<JsonRpcResponse>(json);
}

Result<JsonRpcError> decode_jsonrpc_error(const Json& json) {
  return decode_jsonrpc_value<JsonRpcError>(json);
}

Result<JsonRpcErrorResponse> decode_jsonrpc_error_response(const Json& json) {
  return decode_jsonrpc_value<JsonRpcErrorResponse>(json);
}

Result<JsonRpcMessageKind> classify_jsonrpc_line(std::string_view line) {
  const auto parsed = parse_jsonrpc_line(line);
  if (!parsed) {
    return parsed.error();
  }
  return classify_jsonrpc_message(parsed.value());
}

Result<JsonRpcRequest> decode_jsonrpc_request_line(std::string_view line) {
  return decode_jsonrpc_line_value<JsonRpcRequest>(line);
}

Result<JsonRpcNotification> decode_jsonrpc_notification_line(
    std::string_view line) {
  return decode_jsonrpc_line_value<JsonRpcNotification>(line);
}

Result<JsonRpcResponse> decode_jsonrpc_response_line(std::string_view line) {
  return decode_jsonrpc_line_value<JsonRpcResponse>(line);
}

Result<JsonRpcErrorResponse> decode_jsonrpc_error_response_line(
    std::string_view line) {
  return decode_jsonrpc_line_value<JsonRpcErrorResponse>(line);
}

Result<std::string> encode_jsonrpc_request_line(const JsonRpcRequest& request) {
  try {
    Json json = request;
    return encode_jsonrpc_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

Result<std::string> encode_jsonrpc_notification_line(
    const JsonRpcNotification& notification) {
  try {
    Json json = notification;
    return encode_jsonrpc_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

Result<std::string> encode_jsonrpc_response_line(
    const JsonRpcResponse& response) {
  try {
    Json json = response;
    return encode_jsonrpc_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

Result<std::string> encode_jsonrpc_error_response_line(
    const JsonRpcErrorResponse& response) {
  try {
    Json json = response;
    return encode_jsonrpc_line(json);
  } catch (const std::exception& error) {
    return Result<std::string>::failure(ErrorCode::encoding_error,
                                        error.what());
  }
}

void to_json(Json& json, const JsonRpcRequest& request) {
  require_method(request.method, "jsonrpc request");
  require_params(request.params, "jsonrpc request");
  require_request_id(request.id, "jsonrpc request");

  json = Json{
      {"jsonrpc", kJsonRpcVersion},
      {"method", request.method},
      {"params", request.params},
      {"id", request.id},
  };
}

void from_json(const Json& json, JsonRpcRequest& request) {
  const auto kind = classify_jsonrpc_message(json);
  if (!kind) {
    throw std::runtime_error(kind.error().message);
  }
  if (kind.value() != JsonRpcMessageKind::kRequest) {
    throw std::runtime_error("jsonrpc message is not a request");
  }

  request.method = json.at("method").get<std::string>();
  require_method(request.method, "jsonrpc request");
  request.params = json.value("params", Json::object());
  require_params(request.params, "jsonrpc request");
  request.id = json.at("id");
  require_request_id(request.id, "jsonrpc request");
}

void to_json(Json& json, const JsonRpcNotification& notification) {
  require_method(notification.method, "jsonrpc notification");
  require_params(notification.params, "jsonrpc notification");

  json = Json{
      {"jsonrpc", kJsonRpcVersion},
      {"method", notification.method},
      {"params", notification.params},
  };
}

void from_json(const Json& json, JsonRpcNotification& notification) {
  const auto kind = classify_jsonrpc_message(json);
  if (!kind) {
    throw std::runtime_error(kind.error().message);
  }
  if (kind.value() != JsonRpcMessageKind::kNotification) {
    throw std::runtime_error("jsonrpc message is not a notification");
  }

  notification.method = json.at("method").get<std::string>();
  require_method(notification.method, "jsonrpc notification");
  notification.params = json.value("params", Json::object());
  require_params(notification.params, "jsonrpc notification");
}

void to_json(Json& json, const JsonRpcError& error) {
  if (error.message.empty()) {
    throw std::runtime_error("jsonrpc error message must not be empty");
  }

  json = Json{
      {"code", error.code},
      {"message", error.message},
  };
  if (!error.data.is_null()) {
    json["data"] = error.data;
  }
}

void from_json(const Json& json, JsonRpcError& error) {
  require_object(json, "jsonrpc error");
  error.code = json.at("code").get<int>();
  error.message = json.at("message").get<std::string>();
  if (error.message.empty()) {
    throw std::runtime_error("jsonrpc error message must not be empty");
  }
  error.data = json.contains("data") ? json.at("data") : Json(nullptr);
}

void to_json(Json& json, const JsonRpcResponse& response) {
  require_response_id(response.id, "jsonrpc response");

  json = Json{
      {"jsonrpc", kJsonRpcVersion},
      {"id", response.id},
      {"result", response.result},
  };
}

void from_json(const Json& json, JsonRpcResponse& response) {
  const auto kind = classify_jsonrpc_message(json);
  if (!kind) {
    throw std::runtime_error(kind.error().message);
  }
  if (kind.value() != JsonRpcMessageKind::kResponse) {
    throw std::runtime_error("jsonrpc message is not a response");
  }

  response.id = json.at("id");
  require_response_id(response.id, "jsonrpc response");
  response.result = json.at("result");
}

void to_json(Json& json, const JsonRpcErrorResponse& response) {
  require_response_id(response.id, "jsonrpc error response");

  JsonRpcError error = response.error.get<JsonRpcError>();
  json = Json{
      {"jsonrpc", kJsonRpcVersion},
      {"id", response.id},
      {"error", error},
  };
}

void from_json(const Json& json, JsonRpcErrorResponse& response) {
  const auto kind = classify_jsonrpc_message(json);
  if (!kind) {
    throw std::runtime_error(kind.error().message);
  }
  if (kind.value() != JsonRpcMessageKind::kErrorResponse) {
    throw std::runtime_error("jsonrpc message is not an error response");
  }

  response.id = json.at("id");
  require_response_id(response.id, "jsonrpc error response");

  JsonRpcError error = json.at("error").get<JsonRpcError>();
  response.error = error;
}

}  // namespace qt::extensions
