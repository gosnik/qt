#include "qt/tools/write_text_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

namespace qt::tools {

namespace {

constexpr const char* kPathArgument = "path";
constexpr const char* kFilePathArgument = "file_path";
constexpr const char* kCamelFilePathArgument = "filePath";
constexpr const char* kTextArgument = "text";
constexpr const char* kContentArgument = "content";

qt::foundation::Result<bool> has_string_argument(
    const ToolRequest& request,
    const char* name,
    const std::string& tool_name) {
    if (!request.arguments.contains(name)) {
        return qt::foundation::Result<bool>::success(false);
    }
    const auto value = require_string_argument(request, name, tool_name);
    if (!value) {
        return qt::foundation::Result<bool>::failure(value.error().code,
                                                     value.error().message,
                                                     value.error().system_error);
    }
    return qt::foundation::Result<bool>::success(true);
}

qt::foundation::Result<bool> has_non_empty_string_argument(
    const ToolRequest& request,
    const char* name,
    const std::string& tool_name) {
    if (!request.arguments.contains(name)) {
        return qt::foundation::Result<bool>::success(false);
    }
    const auto value = require_string_argument(request, name, tool_name);
    if (!value) {
        return qt::foundation::Result<bool>::failure(value.error().code,
                                                     value.error().message,
                                                     value.error().system_error);
    }
    return qt::foundation::Result<bool>::success(!value.value().empty());
}

}  // namespace

WriteTextTool::WriteTextTool() {
    descriptor_.name = "write_text";
    descriptor_.description =
        "Write UTF-8 or plain text to a path permitted by the runtime filesystem policy.";
    descriptor_.destructive = true;
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description", "Relative or absolute path to the text file to write."},
              }},
             {kFilePathArgument,
              {
                  {"type", "string"},
                  {"description",
                   "Claude/opencode-compatible alias for path. Absolute paths are accepted when allowed by the workspace policy."},
              }},
             {kCamelFilePathArgument,
              {
                  {"type", "string"},
                  {"description", "opencode-compatible alias for path."},
              }},
             {kContentArgument,
              {
                  {"type", "string"},
                  {"description", "Text content to write to the file."},
              }},
             {kTextArgument,
              {
                  {"type", "string"},
                  {"description", "Legacy text content to write to the file."},
              }},
         }},
        {"required", Json::array({kPathArgument})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& WriteTextTool::descriptor() const { return descriptor_; }

ToolResult WriteTextTool::invoke(const ToolRequest& request,
                                 const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match write_text", request);
    }
    if (!has_only_arguments(request, {kPathArgument, kFilePathArgument,
                                      kCamelFilePathArgument,
                                      kContentArgument, kTextArgument})) {
        return make_invalid_request(
            "write_text accepts only the path, file_path, filePath, content, and text arguments", request);
    }
    const auto has_path =
        has_non_empty_string_argument(request, kPathArgument, descriptor_.name);
    if (!has_path) {
        return make_invalid_request(has_path.error().summary(), request);
    }
    const auto has_file_path =
        has_non_empty_string_argument(request, kFilePathArgument, descriptor_.name);
    if (!has_file_path) {
        return make_invalid_request(has_file_path.error().summary(), request);
    }
    const auto has_camel_file_path =
        has_non_empty_string_argument(request, kCamelFilePathArgument, descriptor_.name);
    if (!has_camel_file_path) {
        return make_invalid_request(has_camel_file_path.error().summary(), request);
    }
    const int path_count = (has_path.value() ? 1 : 0) +
                           (has_file_path.value() ? 1 : 0) +
                           (has_camel_file_path.value() ? 1 : 0);
    if (path_count != 1) {
        return make_invalid_request(
            path_count > 1 ? "write_text accepts path, file_path, or filePath, not more than one"
                           : "write_text requires a path, file_path, or filePath argument",
            request);
    }
    const auto has_content =
        has_string_argument(request, kContentArgument, descriptor_.name);
    if (!has_content) {
        return make_invalid_request(has_content.error().summary(), request);
    }
    const auto has_legacy_text =
        has_non_empty_string_argument(request, kTextArgument, descriptor_.name);
    if (!has_legacy_text) {
        return make_invalid_request(has_legacy_text.error().summary(), request);
    }

    if (has_content.value() && has_legacy_text.value()) {
        return make_invalid_request(
            "write_text accepts content or legacy text, not both", request);
    }

    ToolRequest path_request = request;
    if (!has_path.value()) {
        path_request.arguments[kPathArgument] = has_file_path.value()
            ? path_request.arguments.at(kFilePathArgument)
            : path_request.arguments.at(kCamelFilePathArgument);
    }
    const auto resolved_path =
        resolve_workspace_path(path_request, context, descriptor_.name);
    if (!resolved_path) {
        return make_invalid_request(resolved_path.error().summary(), request);
    }

    const auto has_any_legacy_text =
        request.arguments.contains(kTextArgument);
    const auto content_key = has_content.value() ? kContentArgument : kTextArgument;
    if (!has_content.value() && !has_any_legacy_text) {
        return make_invalid_request("write_text requires a content argument", request);
    }
    const auto text = require_string_argument(request, content_key, descriptor_.name);
    if (!text) {
        return make_invalid_request(text.error().summary(), request);
    }

    if (context.workspace_root) {
        const auto root = qt::foundation::canonicalize_path(*context.workspace_root);
        if (!root) {
            return make_execution_error(ToolResultStatus::execution_error,
                                        root.error().summary(), request);
        }
        if (!is_workspace_path_allowed(resolved_path.value(), root.value(),
                                       context)) {
            return make_execution_error(ToolResultStatus::permission_denied,
                                        "path escapes workspace_root", request);
        }
    }

    if (auto queued = require_mutation_approval(
            ToolMutationKind::file_write,
            "write " + std::to_string(text.value().size()) + " bytes to " +
                qt::foundation::path_for_display(resolved_path.value()),
            request, context)) {
        return *queued;
    }

    const auto write_status =
        qt::foundation::write_text_file(resolved_path.value(), text.value());
    if (!write_status) {
        ToolResultStatus status = ToolResultStatus::execution_error;
        if (write_status.error().code == qt::foundation::ErrorCode::invalid_argument) {
            status = ToolResultStatus::invalid_request;
        }
        return make_execution_error(status, write_status.error().summary(), request);
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    const std::string display_path =
        qt::foundation::path_for_display(resolved_path.value());
    const auto requested_path =
        path_request.arguments.at(kPathArgument).get<std::string>();
    result.output = Json{
        {"path", display_path},
        {"text", "Successfully wrote " + std::to_string(text.value().size()) +
                     " bytes to " + requested_path},
        {"bytes", text.value().size()},
        {"written", true},
    };
    return result;
}

}  // namespace qt::tools
