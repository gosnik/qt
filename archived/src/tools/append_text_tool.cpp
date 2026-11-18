#include "qt/tools/append_text_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <cerrno>
#include <fstream>

namespace qt::tools {

namespace {

qt::foundation::Status append_text_file(const qt::foundation::Path& path,
                                        std::string_view content) {
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        const auto directory_status = qt::foundation::ensure_directory(parent);
        if (!directory_status) {
            return directory_status;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) {
        return qt::foundation::Status::failure(
            qt::foundation::ErrorCode::io_error,
            qt::foundation::path_for_display(path),
            errno);
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        return qt::foundation::Status::failure(
            qt::foundation::ErrorCode::io_error,
            qt::foundation::path_for_display(path),
            errno);
    }

    output.close();
    if (!output) {
        return qt::foundation::Status::failure(
            qt::foundation::ErrorCode::io_error,
            qt::foundation::path_for_display(path),
            errno);
    }

    return qt::foundation::Status::success();
}

}  // namespace

AppendTextTool::AppendTextTool() {
    descriptor_.name = "append_text";
    descriptor_.description =
        "Append text to a path permitted by the runtime filesystem policy.";
    descriptor_.destructive = true;
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description", "Relative or absolute path to the text file to append to."},
              }},
             {"text",
              {
                  {"type", "string"},
                  {"description", "Text content to append to the file."},
              }},
         }},
        {"required", Json::array({"path", "text"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& AppendTextTool::descriptor() const { return descriptor_; }

ToolResult AppendTextTool::invoke(const ToolRequest& request,
                                  const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match append_text", request);
    }
    if (!has_only_arguments(request, {"path", "text"})) {
        return make_invalid_request("append_text accepts only the path and text arguments",
                                    request);
    }

    const auto resolved_path = resolve_workspace_path(request, context, descriptor_.name);
    if (!resolved_path) {
        return make_invalid_request(resolved_path.error().summary(), request);
    }

    const auto text = require_string_argument(request, "text", descriptor_.name);
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
            ToolMutationKind::file_append,
            "append " + std::to_string(text.value().size()) + " bytes to " +
                qt::foundation::path_for_display(resolved_path.value()),
            request, context)) {
        return *queued;
    }

    const auto append_status = append_text_file(resolved_path.value(), text.value());
    if (!append_status) {
        ToolResultStatus status = ToolResultStatus::execution_error;
        if (append_status.error().code == qt::foundation::ErrorCode::invalid_argument) {
            status = ToolResultStatus::invalid_request;
        }
        return make_execution_error(status, append_status.error().summary(), request);
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{
        {"path", qt::foundation::path_for_display(resolved_path.value())},
        {"bytes", text.value().size()},
        {"appended", true},
    };
    return result;
}

}  // namespace qt::tools
