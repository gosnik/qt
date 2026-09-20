#include "qt/tools/read_text_tool.hpp"

#include "qt/ai/images.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace qt::tools {

namespace {

constexpr std::size_t kDefaultReadMaxLines = 2000U;
constexpr std::size_t kDefaultReadMaxBytes = std::size_t{50} * 1024U;
constexpr std::size_t kRepeatedReadWarnCount = 2U;
constexpr std::size_t kRepeatedReadBlockCount = 4U;
constexpr const char* kPathArgument = "path";
constexpr const char* kFilePathArgument = "file_path";
constexpr const char* kCamelFilePathArgument = "filePath";

struct DirectoryEntryRecord {
    std::string display;
    std::string path;
    std::string kind;
};

std::vector<std::string> split_lines_preserve_trailing(std::string_view text) {
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

std::string join_lines(const std::vector<std::string>& lines,
                       std::size_t begin,
                       std::size_t end) {
    std::ostringstream output;
    for (std::size_t index = begin; index < end; ++index) {
        if (index > begin) {
            output << '\n';
        }
        output << lines[index];
    }
    return output.str();
}

std::size_t byte_size_with_separators(const std::vector<std::string>& lines,
                                      std::size_t begin,
                                      std::size_t end) {
    std::size_t bytes = 0U;
    for (std::size_t index = begin; index < end; ++index) {
        bytes += lines[index].size();
        if (index > begin) {
            ++bytes;
        }
    }
    return bytes;
}

std::string format_size(std::size_t bytes) {
    if (bytes < 1024U) {
        return std::to_string(bytes) + "B";
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(1);
    if (bytes < std::size_t{1024} * 1024U) {
        output << (static_cast<double>(bytes) / 1024.0) << "KB";
    } else {
        output << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << "MB";
    }
    return output.str();
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string entry_kind(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    if (entry.is_directory(error)) {
        return "directory";
    }
    error.clear();
    if (entry.is_regular_file(error)) {
        return "file";
    }
    return "other";
}

std::vector<DirectoryEntryRecord> list_directory_records(
    const qt::foundation::Path& directory) {
    std::error_code error;
    std::vector<DirectoryEntryRecord> records;
    for (std::filesystem::directory_iterator iterator(directory, error);
         !error && iterator != std::filesystem::directory_iterator(); ++iterator) {
        const auto kind = entry_kind(*iterator);
        const auto name = iterator->path().filename().generic_string();
        records.push_back(DirectoryEntryRecord{
            kind == "directory" ? name + "/" : name,
            qt::foundation::path_for_display(iterator->path()),
            kind,
        });
    }
    std::sort(records.begin(), records.end(),
              [](const DirectoryEntryRecord& lhs, const DirectoryEntryRecord& rhs) {
                  const auto lhs_key = lowercase_ascii(lhs.display);
                  const auto rhs_key = lowercase_ascii(rhs.display);
                  return lhs_key == rhs_key ? lhs.display < rhs.display
                                            : lhs_key < rhs_key;
              });
    return records;
}

std::string directory_listing_text(const std::vector<DirectoryEntryRecord>& records) {
    if (records.empty()) {
        return "(empty directory)";
    }
    std::ostringstream output;
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (index > 0U) {
            output << '\n';
        }
        output << records[index].display;
    }
    return output.str();
}

Json similar_path_suggestions(const qt::foundation::Path& missing_path) {
    Json suggestions = Json::array();
    const auto parent = missing_path.parent_path().empty()
                            ? qt::foundation::Path(".")
                            : missing_path.parent_path();
    std::error_code error;
    if (!std::filesystem::is_directory(parent, error)) {
        return suggestions;
    }
    const std::string wanted = lowercase_ascii(missing_path.filename().generic_string());
    for (std::filesystem::directory_iterator iterator(parent, error);
         !error && iterator != std::filesystem::directory_iterator(); ++iterator) {
        const std::string candidate_name = iterator->path().filename().generic_string();
        const std::string candidate_lower = lowercase_ascii(candidate_name);
        if ((!wanted.empty() && candidate_lower.find(wanted) != std::string::npos) ||
            (!candidate_lower.empty() && wanted.find(candidate_lower) != std::string::npos)) {
            suggestions.push_back(qt::foundation::path_for_display(iterator->path()));
            if (suggestions.size() >= 5U) {
                break;
            }
        }
    }
    return suggestions;
}

std::string read_fingerprint(const qt::foundation::Path& path,
                             const ToolRequest& request) {
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(path, error);
    const auto file_size = std::filesystem::file_size(path, error);
    std::ostringstream output;
    output << qt::foundation::path_for_display(path) << '|'
           << (request.arguments.contains("offset") ? request.arguments.at("offset").dump()
                                                     : "") << '|'
           << (request.arguments.contains("limit") ? request.arguments.at("limit").dump()
                                                    : "") << '|';
    if (!error) {
        output << file_size << '|'
               << write_time.time_since_epoch().count();
    }
    return output.str();
}

std::size_t repeated_read_count(const std::string& fingerprint) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::size_t> counts;
    std::lock_guard<std::mutex> lock(mutex);
    return ++counts[fingerprint];
}

enum class FirstLineLimitState : std::uint8_t { within_limit, exceeds_limit };

Json make_truncation_details(const std::string& truncated_by,
                             std::size_t total_lines,
                             std::size_t total_bytes,
                             std::size_t output_lines,
                             std::size_t output_bytes,
                             FirstLineLimitState first_line_limit_state) {
    return Json{
        {"truncated", true},
        {"truncatedBy", truncated_by},
        {"totalLines", total_lines},
        {"totalBytes", total_bytes},
        {"outputLines", output_lines},
        {"outputBytes", output_bytes},
        {"lastLinePartial", false},
        {"firstLineExceedsLimit",
         first_line_limit_state == FirstLineLimitState::exceeds_limit},
        {"maxLines", kDefaultReadMaxLines},
        {"maxBytes", kDefaultReadMaxBytes},
    };
}

std::optional<std::string> image_mime_type_for_path(
    const std::string& bytes) {
    const std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
    const auto inspected = qt::ai::inspect_image_bytes(data);
    if (inspected && inspected->media_type.rfind("image/", 0U) == 0U) {
        return inspected->media_type;
    }
    return std::nullopt;
}

std::optional<std::string> non_vision_image_note(const ToolContext& context) {
    const Json* model = &context.metadata;
    const auto model_iterator = context.metadata.find("model");
    if (model_iterator != context.metadata.end() && model_iterator->is_object()) {
        model = &*model_iterator;
    }

    const auto input_iterator = model->find("input");
    if (input_iterator == model->end() || !input_iterator->is_array()) {
        return std::nullopt;
    }
    for (const auto& input : *input_iterator) {
        if (input.is_string() && input.get<std::string>() == "image") {
            return std::nullopt;
        }
    }
    return std::string{
        "[Current model does not support images. The image will be omitted from "
        "this request.]"};
}

bool auto_resize_images_enabled(const ToolContext& context) {
    const auto images = context.metadata.find("images");
    if (images != context.metadata.end() && images->is_object()) {
        const auto auto_resize = images->find("autoResize");
        if (auto_resize != images->end() && auto_resize->is_boolean()) {
            return auto_resize->get<bool>();
        }
    }
    return true;
}

qt::ai::ImageResizePlanOptions image_resize_options_from_context(
    const ToolContext& context) {
    qt::ai::ImageResizePlanOptions options;
    const auto images = context.metadata.find("images");
    if (images == context.metadata.end() || !images->is_object()) {
        return options;
    }

    const auto max_base64_bytes = images->find("maxBase64Bytes");
    if (max_base64_bytes != images->end() &&
        max_base64_bytes->is_number_unsigned()) {
        options.max_base64_bytes = max_base64_bytes->get<std::size_t>();
    }
    return options;
}

}  // namespace

ReadTextTool::ReadTextTool() {
    descriptor_.name = "read_text";
    descriptor_.description = "Read a UTF-8 or plain text file from the local filesystem.";
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description", "Relative or absolute path to a text file."},
              }},
             {kFilePathArgument,
              {
                  {"type", "string"},
                  {"description", "Alias for path used by Claude/Crush-compatible callers."},
              }},
             {kCamelFilePathArgument,
              {
                  {"type", "string"},
                  {"description", "opencode-compatible alias for path."},
              }},
             {"offset",
              {
                  {"type", "number"},
                  {"description", "1-indexed line number to start reading from."},
              }},
             {"limit",
              {
                  {"type", "number"},
                  {"description", "Maximum number of lines to read."},
              }},
         }},
        {"required", Json::array({"path"})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& ReadTextTool::descriptor() const { return descriptor_; }

ToolResult ReadTextTool::invoke(const ToolRequest& request,
                                const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match read_text", request);
    }
    if (!has_only_arguments(request, {kPathArgument, kFilePathArgument,
                                      kCamelFilePathArgument, "offset", "limit"})) {
        return make_invalid_request(
            "read_text accepts only the path, file_path, filePath, offset, and limit arguments", request);
    }

    const bool has_path = request.arguments.contains(kPathArgument);
    const bool has_file_path = request.arguments.contains(kFilePathArgument);
    const bool has_camel_file_path = request.arguments.contains(kCamelFilePathArgument);
    const int path_count = (has_path ? 1 : 0) + (has_file_path ? 1 : 0) +
                           (has_camel_file_path ? 1 : 0);
    if (path_count != 1) {
        return make_invalid_request(
            path_count > 1 ? "read_text accepts path, file_path, or filePath, not more than one"
                           : "read_text requires a path, file_path, or filePath argument",
            request);
    }
    ToolRequest path_request = request;
    if (!has_path) {
        path_request.arguments[kPathArgument] = has_file_path
            ? path_request.arguments.at(kFilePathArgument)
            : path_request.arguments.at(kCamelFilePathArgument);
    }

    const auto resolved_path = resolve_workspace_path(path_request, context, descriptor_.name);
    if (!resolved_path) {
        return make_invalid_request(resolved_path.error().summary(), request);
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

    std::error_code status_error;
    const auto status = std::filesystem::status(resolved_path.value(), status_error);
    if (status_error) {
        if (status_error == std::errc::no_such_file_or_directory) {
            ToolResult result = make_execution_error(
                ToolResultStatus::not_found,
                qt::foundation::path_for_display(resolved_path.value()), request);
            const auto suggestions = similar_path_suggestions(resolved_path.value());
            if (!suggestions.empty()) {
                result.output = Json{{"suggestions", suggestions}};
                result.error_message =
                    *result.error_message + ". Did you mean " +
                    suggestions.front().get<std::string>() + "?";
            }
            return result;
        }
        return make_execution_error(ToolResultStatus::execution_error,
                                    qt::foundation::path_for_display(resolved_path.value()) +
                                        ": " + status_error.message(),
                                    request);
    }
    if (!std::filesystem::exists(status)) {
        ToolResult result = make_execution_error(
            ToolResultStatus::not_found,
            qt::foundation::path_for_display(resolved_path.value()), request);
        const auto suggestions = similar_path_suggestions(resolved_path.value());
        if (!suggestions.empty()) {
            result.output = Json{{"suggestions", suggestions}};
            result.error_message =
                *result.error_message + ". Did you mean " +
                suggestions.front().get<std::string>() + "?";
        }
        return result;
    }
    if (std::filesystem::is_directory(status)) {
        const auto records = list_directory_records(resolved_path.value());
        Json entries = Json::array();
        for (const auto& record : records) {
            entries.push_back(Json{
                {"display", record.display},
                {"path", record.path},
                {"kind", record.kind},
            });
        }
        ToolResult result;
        result.status = ToolResultStatus::success;
        result.call_id = request.call_id;
        result.output = Json{
            {"path", qt::foundation::path_for_display(resolved_path.value())},
            {"text", directory_listing_text(records)},
            {"entries", std::move(entries)},
            {"count", records.size()},
            {"directory", true},
        };
        apply_output_guards(result, output_guard_options_from_context(context));
        return result;
    }

    const auto repeat_count =
        repeated_read_count(read_fingerprint(resolved_path.value(), request));
    if (repeat_count >= kRepeatedReadBlockCount) {
        return make_invalid_request(
            "read_text refused a duplicate unchanged read. Reuse the prior result or read a different offset/limit.",
            request);
    }

    const auto text = qt::foundation::read_text_file(resolved_path.value());
    if (!text) {
        ToolResultStatus status = ToolResultStatus::execution_error;
        switch (text.error().code) {
            case qt::foundation::ErrorCode::not_found:
                status = ToolResultStatus::not_found;
                break;
            case qt::foundation::ErrorCode::invalid_argument:
                status = ToolResultStatus::invalid_request;
                break;
            default:
                status = ToolResultStatus::execution_error;
                break;
        }
        ToolResult result = make_execution_error(status, text.error().summary(), request);
        if (status == ToolResultStatus::not_found) {
            const auto suggestions = similar_path_suggestions(resolved_path.value());
            if (!suggestions.empty()) {
                result.output = Json{{"suggestions", suggestions}};
                result.error_message =
                    *result.error_message + ". Did you mean " +
                    suggestions.front().get<std::string>() + "?";
            }
        }
        return result;
    }

    if (const auto mime_type = image_mime_type_for_path(text.value())) {
        const std::vector<std::uint8_t> bytes(text.value().begin(), text.value().end());
        const std::string base64 = qt::ai::encode_image_base64(bytes);
        qt::ai::ImageContent image{*mime_type, base64};
        std::string note = "Read image file [" + image.media_type + "]";
        if (const auto model_note = non_vision_image_note(context)) {
            note += "\n" + *model_note;
        }
        bool include_image = true;

        if (auto_resize_images_enabled(context)) {
            const auto resized = qt::ai::unsupported_image_converter().convert(
                image, image_resize_options_from_context(context));
            if (!resized.image) {
                include_image = false;
                note = "Read image file [" + *mime_type + "]\n"
                       "[Image omitted: could not be resized below the inline "
                       "image size limit.]";
                if (const auto model_note = non_vision_image_note(context)) {
                    note += "\n" + *model_note;
                }
            } else {
                image = *resized.image;
                note = "Read image file [" + image.media_type + "]";
                if (resized.dimension_note) {
                    note += "\n" + *resized.dimension_note;
                }
                if (const auto model_note = non_vision_image_note(context)) {
                    note += "\n" + *model_note;
                }
            }
        }

        ToolResult result;
        result.status = ToolResultStatus::success;
        result.call_id = request.call_id;
        result.output = Json{
            {"path", qt::foundation::path_for_display(resolved_path.value())},
            {"text", note},
            {"bytes", text.value().size()},
            {"content", Json::array({Json{{"type", "text"}, {"text", note}}})},
        };
        if (include_image) {
            result.output["content"].push_back(
                Json{{"type", "image"},
                     {"data", image.data},
                     {"mimeType", image.media_type}});
        }
        apply_output_guards(result, output_guard_options_from_context(context));
        return result;
    }

    std::string output_text = text.value();
    Json details = Json::object();
    std::optional<std::size_t> offset;
    std::optional<std::size_t> limit;
    for (const char* key : {"offset", "limit"}) {
        if (!request.arguments.contains(key)) {
            continue;
        }
        if (!request.arguments.at(key).is_number_unsigned() &&
            !request.arguments.at(key).is_number_integer()) {
            return make_invalid_request(std::string("read_text requires numeric ") + key,
                                        request);
        }
        const auto value = request.arguments.at(key).get<long long>();
        if (value <= 0) {
            return make_invalid_request(std::string("read_text requires positive ") + key,
                                        request);
        }
        if (std::string_view(key) == "offset") {
            offset = static_cast<std::size_t>(value);
        } else {
            limit = static_cast<std::size_t>(value);
        }
    }

    {
        const auto lines = split_lines_preserve_trailing(text.value());
        const std::size_t start_line = offset.value_or(1U);
        if (start_line > lines.size()) {
            return make_execution_error(
                ToolResultStatus::invalid_request,
                "Offset " + std::to_string(start_line) +
                    " is beyond end of file (" + std::to_string(lines.size()) +
                    " lines total)",
                request);
        }
        const std::size_t start_index = start_line - 1U;
        const std::size_t total_selected_lines = lines.size() - start_index;
        const std::size_t total_selected_bytes =
            byte_size_with_separators(lines, start_index, lines.size());
        std::size_t end_index =
            limit ? std::min(lines.size(), start_index + *limit)
                  : std::min(lines.size(), start_index + kDefaultReadMaxLines);
        bool truncated_by_bytes = false;
        if (!limit) {
            while (end_index > start_index &&
                   byte_size_with_separators(lines, start_index, end_index) >
                       kDefaultReadMaxBytes) {
                truncated_by_bytes = true;
                --end_index;
            }
        }
        bool first_line_exceeds_limit = false;
        output_text = join_lines(lines, start_index, end_index);
        if (!limit && end_index == start_index && start_index < lines.size()) {
            first_line_exceeds_limit = true;
            details["truncation"] = make_truncation_details(
                "bytes", total_selected_lines, total_selected_bytes, 0U, 0U,
                FirstLineLimitState::exceeds_limit);
            output_text = "[Line " + std::to_string(start_line) + " is " +
                          format_size(lines[start_index].size()) +
                          ", exceeds " + format_size(kDefaultReadMaxBytes) +
                          " limit. Use bash: sed -n '" +
                          std::to_string(start_line) + "p' " +
                          path_request.arguments.at(kPathArgument).get<std::string>() +
                          " | head -c " + std::to_string(kDefaultReadMaxBytes) +
                          "]";
        }
        if (limit && end_index < lines.size()) {
            const std::size_t remaining = lines.size() - end_index;
            output_text += "\n\n[" + std::to_string(remaining) +
                           " more lines in file. Use offset=" +
                           std::to_string(end_index + 1U) + " to continue.]";
        } else if (!limit && !first_line_exceeds_limit && end_index < lines.size()) {
            const std::size_t output_lines = end_index - start_index;
            const std::size_t output_bytes =
                byte_size_with_separators(lines, start_index, end_index);
            details["truncation"] = make_truncation_details(
                truncated_by_bytes ? "bytes" : "lines", total_selected_lines,
                total_selected_bytes, output_lines, output_bytes,
                FirstLineLimitState::within_limit);
            output_text += "\n\n[Showing lines " + std::to_string(start_line) +
                           "-" + std::to_string(end_index) + " of " +
                           std::to_string(lines.size()) + ". Use offset=" +
                           std::to_string(end_index + 1U) + " to continue.]";
        }
    }

    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = Json{
        {"path", qt::foundation::path_for_display(resolved_path.value())},
        {"text", output_text},
        {"bytes", text.value().size()},
    };
    if (!details.empty()) {
        result.output["details"] = std::move(details);
    }
    if (repeat_count >= kRepeatedReadWarnCount) {
        result.output["details"]["repeatedRead"] = Json{
            {"count", repeat_count},
            {"message", "This unchanged file range has already been read."},
        };
    }
    apply_output_guards(result, output_guard_options_from_context(context));
    return result;
}

}  // namespace qt::tools
