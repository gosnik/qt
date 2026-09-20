#include "qt/tools/bash_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/process.hpp"
#include "qt/tools/tool_validation.hpp"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace qt::tools {

namespace {

qt::foundation::Result<qt::foundation::Path> resolve_working_directory(
    const ToolRequest& request, const ToolContext& context) {
    if (!context.workspace_root) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "bash requires workspace_root in tool context",
        };
    }

    const auto workspace_root =
        qt::foundation::canonicalize_path(*context.workspace_root);
    if (!workspace_root) {
        return workspace_root.error();
    }

    qt::foundation::Path raw_working_directory =
        context.current_directory.value_or(workspace_root.value());
    std::optional<std::string> requested_working_directory;
    for (const char* key : {"workdir", "cwd", "working_dir"}) {
        if (!request.arguments.contains(key)) {
            continue;
        }
        if (requested_working_directory.has_value()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "bash accepts only one of workdir, cwd, or working_dir",
            };
        }
        if (!request.arguments.at(key).is_string()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                std::string("bash requires string ") + key,
            };
        }
        requested_working_directory = request.arguments.at(key).get<std::string>();
    }
    if (requested_working_directory && !requested_working_directory->empty()) {
        raw_working_directory = *requested_working_directory;
        if (raw_working_directory.is_relative()) {
            raw_working_directory = workspace_root.value() / raw_working_directory;
        }
    }
    const auto working_directory =
        qt::foundation::canonicalize_path(raw_working_directory);
    if (!working_directory) {
        return working_directory.error();
    }

    if (!is_workspace_path_allowed(working_directory.value(),
                                   workspace_root.value(), context)) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "working directory escapes workspace_root",
        };
    }

    return working_directory.value();
}

constexpr std::size_t kBashMaxLines = 2000U;
constexpr std::size_t kBashMaxBytes = std::size_t{50} * 1024U;

struct BashTextPreview {
    std::string text;
    Json truncation = Json::object();
    std::optional<std::string> full_output_path;
    bool truncated = false;
};

qt::foundation::Result<std::optional<std::int64_t>> timeout_ms_from_request(
    const ToolRequest& request) {
    const bool has_timeout = request.arguments.contains("timeout");
    const bool has_timeout_ms = request.arguments.contains("timeout_ms");
    if (has_timeout && has_timeout_ms) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "bash accepts timeout or timeout_ms, not both",
        };
    }
    if (!has_timeout && !has_timeout_ms) {
        return std::optional<std::int64_t>{};
    }
    const Json& timeout = request.arguments.at(has_timeout ? "timeout" : "timeout_ms");
    if (!timeout.is_number()) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            has_timeout ? "bash requires timeout to be a number of seconds"
                        : "bash requires timeout_ms to be a number of milliseconds",
        };
    }

    const double value = timeout.get<double>();
    if (value < 0.0) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            has_timeout ? "bash timeout must be non-negative"
                        : "bash timeout_ms must be non-negative",
        };
    }
    if (value == 0.0) {
        return std::optional<std::int64_t>{};
    }

    const double milliseconds = has_timeout ? std::ceil(value * 1000.0)
                                            : std::ceil(value);
    if (milliseconds > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "bash timeout is too large",
        };
    }

    return std::optional<std::int64_t>{static_cast<std::int64_t>(milliseconds)};
}

std::string combined_process_text(const qt::foundation::ProcessResult& process) {
    std::string text;
    if (!process.stdout_text.empty()) {
        text += process.stdout_text;
    }
    if (!process.stderr_text.empty()) {
        if (!text.empty() && text.back() != '\n') {
            text.push_back('\n');
        }
        text += process.stderr_text;
    }
    return text;
}

std::string append_status_text(std::string text, const std::string& status) {
    if (!text.empty()) {
        text += "\n\n";
    }
    text += status;
    return text;
}

std::string resolved_command_from_context(const std::string& command,
                                          const ToolContext& context) {
    const auto bash = context.metadata.find("bash");
    if (bash == context.metadata.end() || !bash->is_object()) {
        return command;
    }
    const auto prefix = bash->find("commandPrefix");
    if (prefix == bash->end() || !prefix->is_string() ||
        prefix->get<std::string>().empty()) {
        return command;
    }
    return prefix->get<std::string>() + "\n" + command;
}

std::vector<qt::foundation::ProcessEnvironmentVariable> environment_from_context(
    const ToolContext& context) {
    std::vector<qt::foundation::ProcessEnvironmentVariable> environment;
    const auto bash = context.metadata.find("bash");
    if (bash == context.metadata.end() || !bash->is_object()) {
        return environment;
    }
    const auto env = bash->find("env");
    if (env == bash->end() || !env->is_object()) {
        return environment;
    }
    for (const auto& item : env->items()) {
        if (item.value().is_string()) {
            environment.push_back(qt::foundation::ProcessEnvironmentVariable{
                item.key(), item.value().get<std::string>()});
        } else if (item.value().is_null()) {
            environment.push_back(qt::foundation::ProcessEnvironmentVariable{
                item.key(), std::nullopt});
        }
    }
    return environment;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t offset = 0U;
    while (offset <= text.size()) {
        const auto newline = text.find('\n', offset);
        if (newline == std::string_view::npos) {
            lines.emplace_back(text.substr(offset));
            break;
        }
        lines.emplace_back(text.substr(offset, newline - offset));
        offset = newline + 1U;
    }
    return lines;
}

std::string truncate_string_to_bytes_from_end(const std::string& text,
                                              std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t start = text.size() - max_bytes;
    while (start < text.size() &&
           (static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U) {
        ++start;
    }
    return text.substr(start);
}

std::string format_size(std::size_t bytes) {
    if (bytes < 1024U) {
        return std::to_string(bytes) + "B";
    }
    if (bytes < std::size_t{1024} * 1024U) {
        const double kb = static_cast<double>(bytes) / 1024.0;
        const auto rounded = static_cast<int>(std::round(kb * 10.0));
        return std::to_string(rounded / 10) + "." +
               std::to_string(rounded % 10) + "KB";
    }
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    const auto rounded = static_cast<int>(std::round(mb * 10.0));
    return std::to_string(rounded / 10) + "." +
           std::to_string(rounded % 10) + "MB";
}

std::optional<std::string> persist_full_bash_output(std::string_view text) {
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::nullopt;
    }

    const auto path_template = (directory / "pi-bash-XXXXXX").string();
    std::vector<char> writable_template(path_template.begin(), path_template.end());
    writable_template.push_back('\0');

    const int fd = mkstemp(writable_template.data());
    if (fd == -1) {
        return std::nullopt;
    }

    std::size_t offset = 0U;
    bool ok = true;
    while (offset < text.size()) {
        const ssize_t written =
            write(fd, text.data() + offset, text.size() - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        ok = false;
        break;
    }

    if (close(fd) == -1) {
        ok = false;
    }

    std::string path(writable_template.data());
    if (!ok) {
        std::filesystem::remove(path, error);
        return std::nullopt;
    }
    return path;
}

BashTextPreview bash_text_preview(std::string text,
                                  bool persist_full_output = true) {
    if (text.empty()) {
        return {"(no output)", Json::object(), std::nullopt, false};
    }

    const std::size_t total_bytes = text.size();
    const std::vector<std::string> lines = split_lines(text);
    const std::size_t total_lines = lines.size();
    if (total_lines <= kBashMaxLines && total_bytes <= kBashMaxBytes) {
        return {std::move(text), Json::object(), std::nullopt, false};
    }

    const auto full_output_path =
        persist_full_output ? persist_full_bash_output(text) : std::nullopt;

    std::vector<std::string> kept;
    std::size_t output_bytes_count = 0U;
    std::string truncated_by = "lines";
    bool last_line_partial = false;
    for (std::size_t reverse_index = lines.size(); reverse_index > 0U; --reverse_index) {
        if (kept.size() >= kBashMaxLines) {
            break;
        }
        const auto& line = lines.at(reverse_index - 1U);
        const std::size_t line_bytes = line.size() + (kept.empty() ? 0U : 1U);
        if (output_bytes_count + line_bytes > kBashMaxBytes) {
            truncated_by = "bytes";
            if (kept.empty()) {
                kept.insert(kept.begin(),
                            truncate_string_to_bytes_from_end(line, kBashMaxBytes));
                output_bytes_count = kept.front().size();
                last_line_partial = true;
            }
            break;
        }
        kept.insert(kept.begin(), line);
        output_bytes_count += line_bytes;
    }
    if (kept.size() >= kBashMaxLines && output_bytes_count <= kBashMaxBytes) {
        truncated_by = "lines";
    }

    std::string preview;
    for (std::size_t index = 0U; index < kept.size(); ++index) {
        if (index > 0U) {
            preview.push_back('\n');
        }
        preview += kept.at(index);
    }
    const std::size_t output_lines = kept.empty() ? 0U : kept.size();
    const std::size_t start_display =
        output_lines == 0U ? total_lines : total_lines - output_lines + 1U;
    const std::size_t output_bytes = preview.size();
    preview += "\n\n[";
    if (last_line_partial) {
        const std::size_t last_line_bytes =
            lines.empty() ? 0U : lines.back().size();
        preview += "Showing last " + format_size(output_bytes) + " of line " +
                   std::to_string(total_lines) + " (line is " +
                   format_size(last_line_bytes) + ")";
    } else if (truncated_by == "lines") {
        preview += "Showing lines " + std::to_string(start_display) + "-" +
                   std::to_string(total_lines) + " of " +
                   std::to_string(total_lines);
    } else {
        preview += "Showing lines " + std::to_string(start_display) + "-" +
                   std::to_string(total_lines) + " of " +
                   std::to_string(total_lines) + " (" +
                   format_size(kBashMaxBytes) + " limit)";
    }
    if (full_output_path.has_value()) {
        preview += ". Full output: " + *full_output_path;
    }
    preview += "]";

    BashTextPreview result;
    result.text = std::move(preview);
    result.truncated = true;
    result.full_output_path = full_output_path;
    result.truncation = Json{
        {"truncated", true},
        {"truncatedBy", truncated_by},
        {"totalLines", total_lines},
        {"totalBytes", total_bytes},
        {"outputLines", output_lines},
        {"outputBytes", output_bytes},
        {"lastLinePartial", last_line_partial},
        {"firstLineExceedsLimit", false},
        {"maxLines", kBashMaxLines},
        {"maxBytes", kBashMaxBytes},
    };
    return result;
}

Json bash_output_json(const std::string& command,
                      const std::string& resolved_command,
                      const qt::foundation::Path& working_directory,
                      const qt::foundation::ProcessResult& process,
                      const ToolRequest& request) {
    const auto text_preview =
        bash_text_preview(combined_process_text(process));
    Json output = Json{
        {"command", command},
        {"cwd", qt::foundation::path_for_display(working_directory)},
        {"exit_code", process.exit_code},
        {"timed_out", process.timed_out},
        {"cancelled", process.cancelled},
        {"stdout", process.stdout_text},
        {"stderr", process.stderr_text},
        {"text", text_preview.text},
    };
    if (process.cancelled) {
        output["termination_reason"] = "cancelled";
    } else if (process.timed_out) {
        output["termination_reason"] = "timeout";
    } else if (process.exit_code == 0) {
        output["termination_reason"] = "success";
    } else if (process.exit_code > 128) {
        output["termination_reason"] = "signal";
        output["signal"] = process.exit_code - 128;
    } else {
        output["termination_reason"] = "exit_code";
    }
    if (resolved_command != command) {
        output["resolved_command"] = resolved_command;
    }
    if (text_preview.truncated) {
        output["details"] = Json{{"truncation", text_preview.truncation}};
        if (text_preview.full_output_path.has_value()) {
            output["details"]["fullOutputPath"] = *text_preview.full_output_path;
        }
    }
    if (request.arguments.contains("timeout")) {
        output["timeout"] = request.arguments.at("timeout");
    }
    if (request.arguments.contains("timeout_ms")) {
        output["timeout_ms_requested"] = request.arguments.at("timeout_ms");
    }
    if (process.timeout_ms.has_value()) {
        output["timeout_ms"] = *process.timeout_ms;
    }
    return output;
}

Json bash_running_output_json(const std::string& command,
                              const std::string& resolved_command,
                              const qt::foundation::Path& working_directory,
                              const std::string& stdout_text,
                              const std::string& stderr_text) {
    std::string combined_text = stdout_text;
    if (!stderr_text.empty()) {
        if (!combined_text.empty() && combined_text.back() != '\n') {
            combined_text.push_back('\n');
        }
        combined_text += stderr_text;
    }
    const auto text_preview =
        bash_text_preview(std::move(combined_text), false);
    Json output = Json{
        {"command", command},
        {"cwd", qt::foundation::path_for_display(working_directory)},
        {"running", true},
        {"stdout", stdout_text},
        {"stderr", stderr_text},
        {"text", text_preview.text},
    };
    if (resolved_command != command) {
        output["resolved_command"] = resolved_command;
    }
    if (text_preview.truncated) {
        output["details"] = Json{{"truncation", text_preview.truncation}};
    }
    return output;
}

void emit_bash_update(const ToolRequest& request, const ToolContext& context,
                      Json output) {
    if (!context.update_callback) {
        return;
    }
    ToolResult update;
    update.status = ToolResultStatus::success;
    update.call_id = request.call_id;
    update.output = std::move(output);
    context.update_callback(update);
}

}  // namespace

BashTool::BashTool() {
    descriptor_.name = "bash";
    descriptor_.description =
        "Run a bash command from the current workspace directory and capture stdout and stderr.";
    descriptor_.deterministic = false;
    descriptor_.destructive = true;
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"command",
              {
                  {"type", "string"},
                  {"description", "Shell command to execute with bash -lc."},
              }},
             {"timeout",
              {
                  {"type", "number"},
                  {"description", "Timeout in seconds. Omit or pass 0 for no timeout."},
              }},
             {"timeout_ms",
              {
                  {"type", "number"},
                  {"description", "Timeout in milliseconds. Omit or pass 0 for no timeout."},
              }},
             {"workdir",
              {
                  {"type", "string"},
                  {"description",
                   "Working directory for the command, relative to the workspace or absolute within it."},
              }},
             {"cwd",
              {
                  {"type", "string"},
                  {"description", "Alias for workdir."},
              }},
             {"working_dir",
              {
                  {"type", "string"},
                  {"description", "Crush-compatible alias for workdir."},
              }},
             {"description",
              {
                  {"type", "string"},
                  {"description", "Short human-readable command description."},
              }},
         }},
        {"required", Json::array({"command"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& BashTool::descriptor() const { return descriptor_; }

ToolResult BashTool::invoke(const ToolRequest& request,
                            const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match bash", request);
    }
    if (!has_only_arguments(request, {"command", "timeout", "timeout_ms",
                                      "workdir", "cwd", "working_dir",
                                      "description"})) {
        return make_invalid_request(
            "bash accepts only the command, timeout, timeout_ms, workdir, cwd, working_dir, and description arguments",
                                    request);
    }

    const auto command =
        require_string_argument(request, "command", descriptor_.name);
    if (!command) {
        return make_invalid_request(command.error().summary(), request);
    }

    const auto timeout_ms = timeout_ms_from_request(request);
    if (!timeout_ms) {
        return make_invalid_request(timeout_ms.error().summary(), request);
    }

    const auto working_directory = resolve_working_directory(request, context);
    if (!working_directory) {
        const auto status =
            working_directory.error().code == qt::foundation::ErrorCode::invalid_argument
                ? ToolResultStatus::invalid_request
                : ToolResultStatus::execution_error;
        if (working_directory.error().message == "working directory escapes workspace_root") {
            return make_execution_error(ToolResultStatus::permission_denied,
                                        working_directory.error().summary(), request);
        }
        return make_execution_error(status, working_directory.error().summary(), request);
    }

    if (auto queued = require_mutation_approval(
            ToolMutationKind::command, "run bash command: " + command.value(), request,
            context)) {
        return *queued;
    }

    const std::string resolved_command =
        resolved_command_from_context(command.value(), context);
    qt::foundation::ProcessRequest process_request;
    process_request.mode = qt::foundation::ProcessLaunchMode::shell;
    process_request.command = resolved_command;
    process_request.cwd = working_directory.value();
    process_request.timeout_ms = timeout_ms.value();
    process_request.environment = environment_from_context(context);
    process_request.cancellation_token = context.cancellation_token;
    if (context.update_callback) {
        process_request.output_callback =
            [&](const qt::foundation::ProcessOutputUpdate& update) {
                emit_bash_update(
                    request, context,
                    bash_running_output_json(command.value(), resolved_command,
                                             working_directory.value(),
                                             update.stdout_text,
                                             update.stderr_text));
            };
    }
    emit_bash_update(
        request, context,
        Json{{"command", command.value()},
             {"cwd", qt::foundation::path_for_display(working_directory.value())},
             {"running", true},
             {"text", ""}});
    const auto process = qt::foundation::run_process(process_request);
    if (!process) {
        return make_execution_error(ToolResultStatus::execution_error,
                                    process.error().summary(), request);
    }

    ToolResult result;
    result.status = process.value().cancelled || process.value().timed_out ||
                            process.value().exit_code != 0
                        ? ToolResultStatus::execution_error
                        : ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = bash_output_json(command.value(), resolved_command,
                                     working_directory.value(),
                                     process.value(), request);
    if (process.value().cancelled) {
        result.error_message = append_status_text(
            combined_process_text(process.value()), "Command aborted");
    } else if (process.value().timed_out) {
        const auto timeout_seconds = request.arguments.contains("timeout")
                                         ? request.arguments.at("timeout").dump()
                                         : std::string("0");
        result.error_message = append_status_text(
            combined_process_text(process.value()),
            "Command timed out after " + timeout_seconds + " seconds");
    } else if (process.value().exit_code != 0) {
        result.error_message = append_status_text(
            combined_process_text(process.value()),
            "Command exited with code " + std::to_string(process.value().exit_code));
    }
    apply_output_guards(result, output_guard_options_from_context(context));
    emit_bash_update(request, context, result.output);
    return result;
}

}  // namespace qt::tools
