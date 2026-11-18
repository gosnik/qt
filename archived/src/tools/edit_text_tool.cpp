#include "qt/tools/edit_text_tool.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"

#include <uni_algo/norm.h>

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::tools {

namespace {

constexpr const char* kPathArgument = "path";
constexpr const char* kFilePathArgument = "file_path";
constexpr const char* kCamelFilePathArgument = "filePath";
constexpr const char* kEditsArgument = "edits";
constexpr const char* kOldTextArgument = "oldText";
constexpr const char* kNewTextArgument = "newText";
constexpr const char* kOldStringArgument = "old_string";
constexpr const char* kNewStringArgument = "new_string";
constexpr const char* kCamelOldStringArgument = "oldString";
constexpr const char* kCamelNewStringArgument = "newString";
constexpr const char* kReplaceAllArgument = "replace_all";

struct ReplacementEdit {
    std::string old_text;
    std::string new_text;
    std::size_t position = 0U;
    std::size_t match_length = 0U;
    bool replace_all = false;
};

struct EditMatch {
    bool found = false;
    std::size_t position = 0U;
    std::size_t match_length = 0U;
    bool used_fuzzy_match = false;
};

enum class LineEndingStyle {
    lf,
    crlf,
};

bool contains_nul_byte(std::string_view text) {
    return std::find(text.begin(), text.end(), '\0') != text.end();
}

bool starts_with_at(std::string_view text, std::size_t offset,
                    std::string_view needle) {
    return offset + needle.size() <= text.size() &&
           text.substr(offset, needle.size()) == needle;
}

void append_normalized_fuzzy_byte(std::string& output, char byte) {
    if (byte == ' ' || byte == '\t') {
        output.push_back(byte);
    } else {
        output.push_back(byte);
    }
}

std::string normalize_for_fuzzy_match(std::string_view text) {
    std::string normalized = una::norm::to_nfkc_utf8(std::string(text));
    std::string replaced;
    replaced.reserve(normalized.size());

    for (std::size_t index = 0; index < normalized.size();) {
        if (starts_with_at(normalized, index, "\xE2\x80\x98") ||
            starts_with_at(normalized, index, "\xE2\x80\x99") ||
            starts_with_at(normalized, index, "\xE2\x80\x9A") ||
            starts_with_at(normalized, index, "\xE2\x80\x9B")) {
            replaced.push_back('\'');
            index += 3U;
        } else if (starts_with_at(normalized, index, "\xE2\x80\x9C") ||
                   starts_with_at(normalized, index, "\xE2\x80\x9D") ||
                   starts_with_at(normalized, index, "\xE2\x80\x9E") ||
                   starts_with_at(normalized, index, "\xE2\x80\x9F")) {
            replaced.push_back('"');
            index += 3U;
        } else if (starts_with_at(normalized, index, "\xE2\x80\x90") ||
                   starts_with_at(normalized, index, "\xE2\x80\x91") ||
                   starts_with_at(normalized, index, "\xE2\x80\x92") ||
                   starts_with_at(normalized, index, "\xE2\x80\x93") ||
                   starts_with_at(normalized, index, "\xE2\x80\x94") ||
                   starts_with_at(normalized, index, "\xE2\x80\x95") ||
                   starts_with_at(normalized, index, "\xE2\x88\x92")) {
            replaced.push_back('-');
            index += 3U;
        } else if (starts_with_at(normalized, index, "\xC2\xA0") ||
                   starts_with_at(normalized, index, "\xE2\x80\xAF") ||
                   starts_with_at(normalized, index, "\xE2\x81\x9F") ||
                   starts_with_at(normalized, index, "\xE3\x80\x80")) {
            replaced.push_back(' ');
            index += starts_with_at(normalized, index, "\xC2\xA0") ? 2U : 3U;
        } else if (index + 3U <= normalized.size() &&
                   static_cast<unsigned char>(normalized[index]) == 0xE2U &&
                   static_cast<unsigned char>(normalized[index + 1U]) == 0x80U &&
                   static_cast<unsigned char>(normalized[index + 2U]) >= 0x82U &&
                   static_cast<unsigned char>(normalized[index + 2U]) <= 0x8AU) {
            replaced.push_back(' ');
            index += 3U;
        } else {
            append_normalized_fuzzy_byte(replaced, normalized[index]);
            ++index;
        }
    }

    std::string trimmed;
    trimmed.reserve(replaced.size());
    std::size_t line_start = 0U;
    while (line_start <= replaced.size()) {
        const auto line_end = replaced.find('\n', line_start);
        const std::size_t end =
            line_end == std::string::npos ? replaced.size() : line_end;
        std::size_t trimmed_end = end;
        while (trimmed_end > line_start &&
               (replaced[trimmed_end - 1U] == ' ' ||
                replaced[trimmed_end - 1U] == '\t')) {
            --trimmed_end;
        }
        trimmed.append(replaced.substr(line_start, trimmed_end - line_start));
        if (line_end == std::string::npos) {
            break;
        }
        trimmed.push_back('\n');
        line_start = line_end + 1U;
    }
    return trimmed;
}

EditMatch find_edit_match(std::string_view content, std::string_view old_text) {
    const auto exact = content.find(old_text);
    if (exact != std::string_view::npos) {
        return {true, exact, old_text.size(), false};
    }

    const std::string fuzzy_content = normalize_for_fuzzy_match(content);
    const std::string fuzzy_old_text = normalize_for_fuzzy_match(old_text);
    const auto fuzzy = fuzzy_content.find(fuzzy_old_text);
    if (fuzzy == std::string::npos) {
        return {};
    }
    return {true, fuzzy, fuzzy_old_text.size(), true};
}

std::string normalize_to_lf(std::string_view text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1U < text.size() && text[index + 1U] == '\n') {
                ++index;
            }
            normalized.push_back('\n');
        } else {
            normalized.push_back(text[index]);
        }
    }
    return normalized;
}

LineEndingStyle line_ending_style_for(std::string_view text) {
    const auto crlf = text.find("\r\n");
    const auto lf = text.find('\n');
    if (lf != std::string_view::npos && crlf != std::string_view::npos && crlf <= lf) {
        return LineEndingStyle::crlf;
    }
    return LineEndingStyle::lf;
}

std::string restore_line_endings(std::string_view text, LineEndingStyle style) {
    if (style == LineEndingStyle::lf) {
        return std::string(text);
    }
    std::string restored;
    restored.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\n') {
            restored += "\r\n";
        } else {
            restored.push_back(ch);
        }
    }
    return restored;
}

std::pair<std::string, std::string_view> strip_utf8_bom(std::string_view text) {
    constexpr std::string_view kBom("\xEF\xBB\xBF", 3U);
    if (text.substr(0, kBom.size()) == kBom) {
        return {std::string(kBom), text.substr(kBom.size())};
    }
    return {"", text};
}

std::size_t count_matches(std::string_view text, std::string_view needle) {
    if (needle.empty()) {
        return 0U;
    }

    std::size_t count = 0U;
    std::size_t offset = 0U;
    while (offset <= text.size()) {
        const std::size_t found = text.find(needle, offset);
        if (found == std::string_view::npos) {
            break;
        }
        ++count;
        offset = found + needle.size();
    }
    return count;
}

std::optional<std::string> replacement_string_field(const Json& object,
                                                    const char* primary,
                                                    const char* alias,
                                                    const char* secondary_alias) {
    const auto primary_iterator = object.find(primary);
    const auto alias_iterator = object.find(alias);
    const auto secondary_alias_iterator = object.find(secondary_alias);
    const int count = (primary_iterator != object.end() ? 1 : 0) +
                      (alias_iterator != object.end() ? 1 : 0) +
                      (secondary_alias_iterator != object.end() ? 1 : 0);
    if (count != 1) {
        return std::nullopt;
    }
    const Json& value = primary_iterator != object.end()
                            ? *primary_iterator
                            : (alias_iterator != object.end()
                                   ? *alias_iterator
                                   : *secondary_alias_iterator);
    if (!value.is_string()) {
        return std::nullopt;
    }
    return value.get<std::string>();
}

qt::foundation::Result<std::vector<ReplacementEdit>> parse_replacement_edits(
    const ToolRequest& request) {
    std::vector<ReplacementEdit> edits;
    bool top_level_replace_all = false;
    if (request.arguments.contains(kReplaceAllArgument)) {
        if (!request.arguments.at(kReplaceAllArgument).is_boolean()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "edit requires boolean replace_all",
            };
        }
        top_level_replace_all =
            request.arguments.at(kReplaceAllArgument).get<bool>();
    }

    if (request.arguments.contains(kEditsArgument)) {
        Json parsed_edits;
        const Json& raw_edits_argument = request.arguments.at(kEditsArgument);
        const Json* raw_edits = &raw_edits_argument;
        if (raw_edits_argument.is_string()) {
            try {
                parsed_edits =
                    Json::parse(raw_edits_argument.get<std::string>());
                if (parsed_edits.is_array()) {
                    raw_edits = &parsed_edits;
                }
            } catch (const Json::parse_error&) {
            }
        }
        if (!raw_edits->is_array() || raw_edits->empty()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "edit requires edits to contain at least one replacement",
            };
        }

        for (const auto& raw_edit : *raw_edits) {
            if (!raw_edit.is_object()) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::invalid_argument,
                    "edit entries require string oldText/newText, old_string/new_string, or oldString/newString fields",
                };
            }
            const auto old_text =
                replacement_string_field(raw_edit, kOldTextArgument,
                                         kOldStringArgument,
                                         kCamelOldStringArgument);
            const auto new_text =
                replacement_string_field(raw_edit, kNewTextArgument,
                                         kNewStringArgument,
                                         kCamelNewStringArgument);
            if (!old_text || !new_text) {
                return qt::foundation::Error{
                    qt::foundation::ErrorCode::invalid_argument,
                    "edit entries require exactly one oldText/old_string/oldString and one newText/new_string/newString field",
                };
            }
            bool edit_replace_all = top_level_replace_all;
            if (raw_edit.contains(kReplaceAllArgument)) {
                if (!raw_edit.at(kReplaceAllArgument).is_boolean()) {
                    return qt::foundation::Error{
                        qt::foundation::ErrorCode::invalid_argument,
                        "edit entries require boolean replace_all",
                    };
                }
                edit_replace_all = raw_edit.at(kReplaceAllArgument).get<bool>();
            }
            for (const auto& item : raw_edit.items()) {
                if (item.key() != kOldTextArgument && item.key() != kNewTextArgument &&
                    item.key() != kOldStringArgument && item.key() != kNewStringArgument &&
                    item.key() != kCamelOldStringArgument &&
                    item.key() != kCamelNewStringArgument &&
                    item.key() != kReplaceAllArgument) {
                    return qt::foundation::Error{
                        qt::foundation::ErrorCode::invalid_argument,
                        "edit entries accept only oldText, newText, old_string, new_string, oldString, newString, and replace_all fields",
                    };
                }
            }
            edits.push_back(ReplacementEdit{
                *old_text,
                *new_text,
                0U,
                0U,
                edit_replace_all,
            });
        }
    }

    const bool has_legacy_old_argument =
        request.arguments.contains(kOldTextArgument) ||
        request.arguments.contains(kOldStringArgument) ||
        request.arguments.contains(kCamelOldStringArgument);
    const bool has_legacy_new_argument =
        request.arguments.contains(kNewTextArgument) ||
        request.arguments.contains(kNewStringArgument) ||
        request.arguments.contains(kCamelNewStringArgument);
    if (has_legacy_old_argument || has_legacy_new_argument) {
        const auto old_text =
            replacement_string_field(request.arguments, kOldTextArgument,
                                     kOldStringArgument,
                                     kCamelOldStringArgument);
        const auto new_text =
            replacement_string_field(request.arguments, kNewTextArgument,
                                     kNewStringArgument,
                                     kCamelNewStringArgument);
        if (!old_text || !new_text) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "edit legacy arguments require exactly one oldText/old_string/oldString and one newText/new_string/newString field",
            };
        }
        const bool has_legacy_edit = !old_text->empty() || !new_text->empty() ||
                                     edits.empty();
        if (has_legacy_edit) {
            edits.push_back(ReplacementEdit{
                *old_text,
                *new_text,
                0U,
                0U,
                top_level_replace_all,
            });
        }
    }

    if (edits.empty()) {
        return qt::foundation::Error{
            qt::foundation::ErrorCode::invalid_argument,
            "edit requires an edits array or oldText/newText arguments",
        };
    }

    for (const auto& edit : edits) {
        if (edit.old_text.empty()) {
            return qt::foundation::Error{
                qt::foundation::ErrorCode::invalid_argument,
                "edit requires non-empty oldText values",
            };
        }
    }

    for (auto& edit : edits) {
        edit.old_text = normalize_to_lf(edit.old_text);
        edit.new_text = normalize_to_lf(edit.new_text);
    }

    return edits;
}

std::size_t first_changed_line(std::string_view source, std::size_t position) {
    return 1U + static_cast<std::size_t>(
                    std::count(source.begin(),
                               source.begin() + static_cast<std::ptrdiff_t>(position),
                               '\n'));
}

std::vector<std::string> split_diff_lines(std::string_view text) {
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
    if (!lines.empty() && lines.back().empty() && !text.empty() &&
        text.back() == '\n') {
        lines.pop_back();
    }
    return lines;
}

struct DiffRow {
    char marker = ' ';
    std::size_t line_number = 0U;
    std::string text;
};

std::string format_diff_rows(const std::vector<DiffRow>& rows,
                             std::size_t line_number_width) {
    std::ostringstream output;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        if (index > 0U) {
            output << '\n';
        }
        output << row.marker;
        if (row.line_number == 0U) {
            output << std::setw(static_cast<int>(line_number_width)) << "" << " ...";
        } else {
            output << std::setw(static_cast<int>(line_number_width))
                   << row.line_number << ' ' << row.text;
        }
    }
    return output.str();
}

std::string generate_edit_diff(std::string_view old_content,
                               std::string_view new_content,
                               std::size_t context_lines = 4U) {
    const auto old_lines = split_diff_lines(old_content);
    const auto new_lines = split_diff_lines(new_content);
    const std::size_t old_count = old_lines.size();
    const std::size_t new_count = new_lines.size();

    std::vector<std::vector<std::size_t>> lcs(
        old_count + 1U, std::vector<std::size_t>(new_count + 1U, 0U));
    for (std::size_t i = old_count; i > 0U; --i) {
        for (std::size_t j = new_count; j > 0U; --j) {
            if (old_lines[i - 1U] == new_lines[j - 1U]) {
                lcs[i - 1U][j - 1U] = lcs[i][j] + 1U;
            } else {
                lcs[i - 1U][j - 1U] = std::max(lcs[i][j - 1U],
                                                lcs[i - 1U][j]);
            }
        }
    }

    std::vector<DiffRow> rows;
    std::size_t old_index = 0U;
    std::size_t new_index = 0U;
    while (old_index < old_count || new_index < new_count) {
        if (old_index < old_count && new_index < new_count &&
            old_lines[old_index] == new_lines[new_index]) {
            rows.push_back({' ', old_index + 1U, old_lines[old_index]});
            ++old_index;
            ++new_index;
        } else if (new_index < new_count &&
                   (old_index == old_count ||
                    lcs[old_index][new_index + 1U] >= lcs[old_index + 1U][new_index])) {
            rows.push_back({'+', new_index + 1U, new_lines[new_index]});
            ++new_index;
        } else if (old_index < old_count) {
            rows.push_back({'-', old_index + 1U, old_lines[old_index]});
            ++old_index;
        }
    }

    std::vector<bool> keep(rows.size(), false);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].marker == ' ') {
            continue;
        }
        const std::size_t begin =
            index > context_lines ? index - context_lines : 0U;
        const std::size_t end =
            std::min(rows.size(), index + context_lines + 1U);
        for (std::size_t keep_index = begin; keep_index < end; ++keep_index) {
            keep[keep_index] = true;
        }
    }

    std::vector<DiffRow> collapsed;
    bool skipped = false;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (!keep[index]) {
            skipped = true;
            continue;
        }
        if (skipped) {
            collapsed.push_back({' ', 0U, ""});
            skipped = false;
        }
        collapsed.push_back(rows[index]);
    }

    const std::size_t max_line_num = std::max(old_count, new_count);
    const std::size_t width = std::max<std::size_t>(1U, std::to_string(max_line_num).size());
    return format_diff_rows(collapsed, width);
}

}  // namespace

EditTextTool::EditTextTool() {
    descriptor_.name = "edit";
    descriptor_.description =
        "Replace exact text matches in a path permitted by the runtime filesystem policy.";
    descriptor_.destructive = true;
    descriptor_.input_schema = Json{
        {"type", "object"},
        {"properties",
         {
             {"path",
              {
                  {"type", "string"},
                  {"description", "Relative or absolute path to the text file to edit."},
              }},
             {kFilePathArgument,
              {
                  {"type", "string"},
                  {"description", "Alias for path."},
              }},
             {kCamelFilePathArgument,
              {
                  {"type", "string"},
                  {"description", "opencode-compatible alias for path."},
              }},
             {"edits",
              {
                  {"type", "array"},
                  {"description",
                   "One or more exact replacements matched against the original file."},
                  {"items",
                   {
                       {"type", "object"},
                       {"properties",
                        {
                            {"oldText",
                             {
                                 {"type", "string"},
                                 {"description", "Exact text to replace. Must match exactly once."},
                             }},
                            {"newText",
                             {
                                 {"type", "string"},
                                 {"description", "Replacement text for oldText."},
                             }},
                            {"old_string",
                             {
                                 {"type", "string"},
                                 {"description", "Alias for oldText."},
                             }},
                            {"new_string",
                             {
                                 {"type", "string"},
                                 {"description", "Alias for newText."},
                             }},
                            {"replace_all",
                             {
                                 {"type", "boolean"},
                                 {"description",
                                  "Replace every non-overlapping occurrence for this edit."},
                            }},
                            {"oldString",
                             {
                                 {"type", "string"},
                                 {"description", "opencode-compatible alias for oldText."},
                             }},
                            {"newString",
                             {
                                 {"type", "string"},
                                 {"description", "opencode-compatible alias for newText."},
                             }},
                        }},
                       {"required", Json::array()},
                       {"additionalProperties", false},
                   }},
              }},
             {"oldText",
              {
                  {"type", "string"},
                  {"description", "Legacy exact text to replace. Must match exactly once."},
              }},
             {"newText",
              {
                  {"type", "string"},
                  {"description", "Legacy replacement text to write in place of oldText."},
              }},
             {"old_string",
              {
                  {"type", "string"},
                  {"description", "Claude/opencode-compatible alias for oldText."},
              }},
             {"new_string",
              {
                  {"type", "string"},
                  {"description", "Claude/opencode-compatible alias for newText."},
              }},
             {"replace_all",
              {
                  {"type", "boolean"},
                  {"description", "Replace every non-overlapping old text occurrence."},
              }},
             {"oldString",
              {
                  {"type", "string"},
                  {"description", "opencode-compatible alias for oldText."},
              }},
             {"newString",
              {
                  {"type", "string"},
                  {"description", "opencode-compatible alias for newText."},
              }},
         }},
        {"required", Json::array({kPathArgument})},
        {"additionalProperties", false},
    };
}

const ToolDescriptor& EditTextTool::descriptor() const { return descriptor_; }

ToolResult EditTextTool::invoke(const ToolRequest& request,
                                const ToolContext& context) const {
    if (!is_valid(request)) {
        return make_invalid_request("invalid tool request", request);
    }
    if (request.tool_name != descriptor_.name) {
        return make_invalid_request("tool request name does not match edit", request);
    }
    if (!has_only_arguments(
            request, {kPathArgument, kFilePathArgument, kCamelFilePathArgument,
                      kEditsArgument, kOldTextArgument, kNewTextArgument,
                      kOldStringArgument, kNewStringArgument,
                      kCamelOldStringArgument, kCamelNewStringArgument,
                      kReplaceAllArgument})) {
        return make_invalid_request(
            "edit accepts only the path, file_path, filePath, edits, oldText, newText, old_string, new_string, oldString, newString, and replace_all arguments", request);
    }

    ToolRequest path_request = request;
    const bool has_path = request.arguments.contains(kPathArgument);
    const bool has_file_path = request.arguments.contains(kFilePathArgument);
    const bool has_camel_file_path = request.arguments.contains(kCamelFilePathArgument);
    const int path_count = (has_path ? 1 : 0) + (has_file_path ? 1 : 0) +
                           (has_camel_file_path ? 1 : 0);
    if (path_count != 1) {
        return make_invalid_request(
            path_count > 1 ? "edit accepts path, file_path, or filePath, not more than one"
                           : "edit requires a path, file_path, or filePath argument",
            request);
    }
    if (!has_path) {
        path_request.arguments[kPathArgument] = has_file_path
            ? request.arguments.at(kFilePathArgument)
            : request.arguments.at(kCamelFilePathArgument);
    }
    const auto resolved_path = resolve_workspace_path(path_request, context, descriptor_.name);
    if (!resolved_path) {
        return make_invalid_request(resolved_path.error().summary(), request);
    }

    auto edits = parse_replacement_edits(request);
    if (!edits) {
        return make_invalid_request(edits.error().summary(), request);
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

    const auto existing_text = qt::foundation::read_text_file(resolved_path.value());
    if (!existing_text) {
        ToolResultStatus status = ToolResultStatus::execution_error;
        switch (existing_text.error().code) {
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
        return make_execution_error(status, existing_text.error().summary(), request);
    }

    if (contains_nul_byte(existing_text.value())) {
        return make_execution_error(
            ToolResultStatus::invalid_request,
            "edit refuses to modify binary file " +
                qt::foundation::path_for_display(resolved_path.value()),
            request);
    }

    const auto [bom, source_without_bom] = strip_utf8_bom(existing_text.value());
    const LineEndingStyle line_ending_style = line_ending_style_for(source_without_bom);
    const std::string normalized_source = normalize_to_lf(source_without_bom);
    bool use_fuzzy_base = false;
    for (const auto& edit : edits.value()) {
        const auto match = find_edit_match(normalized_source, edit.old_text);
        if (match.used_fuzzy_match) {
            use_fuzzy_base = true;
            break;
        }
    }
    const std::string fuzzy_source =
        use_fuzzy_base ? normalize_for_fuzzy_match(normalized_source) : std::string{};
    const std::string& source = use_fuzzy_base ? fuzzy_source : normalized_source;
    std::size_t old_bytes = 0U;
    std::size_t new_bytes = 0U;
    std::size_t changed_edits = 0U;
    std::size_t first_position = source.size();
    std::vector<ReplacementEdit> replacements;

    for (auto& edit : edits.value()) {
        const EditMatch match = find_edit_match(source, edit.old_text);
        const std::string match_old_text =
            match.used_fuzzy_match ? normalize_for_fuzzy_match(edit.old_text)
                                   : edit.old_text;
        const std::string occurrence_source = normalize_for_fuzzy_match(source);
        const std::string occurrence_old_text =
            normalize_for_fuzzy_match(edit.old_text);
        const std::size_t matches =
            count_matches(occurrence_source, occurrence_old_text);
        if (!match.found || matches == 0U) {
            return make_execution_error(
                ToolResultStatus::not_found,
                "oldText/old_string did not match any content in " +
                    qt::foundation::path_for_display(resolved_path.value()),
                request);
        }
        if (matches > 1U && !edit.replace_all) {
            return make_invalid_request(
                "oldText/old_string must match exactly once unless replace_all is true in " +
                    qt::foundation::path_for_display(resolved_path.value()),
                request);
        }
        if (edit.replace_all) {
            std::size_t offset = 0U;
            while (offset <= source.size()) {
                const auto position = source.find(match_old_text, offset);
                if (position == std::string::npos) {
                    break;
                }
                ReplacementEdit replacement = edit;
                replacement.position = position;
                replacement.match_length = match_old_text.size();
                replacements.push_back(std::move(replacement));
                first_position = std::min(first_position, position);
                old_bytes += match_old_text.size();
                new_bytes += edit.new_text.size();
                if (match_old_text != edit.new_text) {
                    ++changed_edits;
                }
                offset = position + match_old_text.size();
            }
        } else {
            edit.position = match.position;
            edit.match_length = match.match_length;
            replacements.push_back(edit);
            first_position = std::min(first_position, edit.position);
            old_bytes += match_old_text.size();
            new_bytes += edit.new_text.size();
            if (match_old_text != edit.new_text) {
                ++changed_edits;
            }
        }
    }

    std::vector<ReplacementEdit> sorted_edits = replacements;
    std::sort(sorted_edits.begin(), sorted_edits.end(),
              [](const ReplacementEdit& lhs, const ReplacementEdit& rhs) {
                  return lhs.position < rhs.position;
              });
    for (std::size_t index = 1U; index < sorted_edits.size(); ++index) {
        const auto& previous = sorted_edits.at(index - 1U);
        const auto& current = sorted_edits.at(index);
        if (current.position < previous.position + previous.match_length) {
            return make_invalid_request(
                "edit replacements must not overlap in " +
                    qt::foundation::path_for_display(resolved_path.value()),
                request);
        }
    }

    if (changed_edits == 0U) {
        return make_invalid_request(
            edits.value().size() == 1U
                ? "No changes made to " +
                      qt::foundation::path_for_display(resolved_path.value()) +
                      ". The replacement produced identical content. This might "
                      "indicate an issue with special characters or the text not "
                      "existing as expected."
                : "No changes made to " +
                      qt::foundation::path_for_display(resolved_path.value()) +
                      ". The replacements produced identical content.",
            request);
    }

    std::string updated = source;
    for (auto edit = sorted_edits.rbegin(); edit != sorted_edits.rend(); ++edit) {
        updated.replace(edit->position, edit->match_length, edit->new_text);
    }

    if (auto queued = require_mutation_approval(
            ToolMutationKind::file_edit,
            "edit " + qt::foundation::path_for_display(resolved_path.value()) +
                " replacing " + std::to_string(old_bytes) +
                " bytes with " + std::to_string(new_bytes) + " bytes across " +
                std::to_string(replacements.size()) + " matches",
            request, context)) {
        return *queued;
    }

    const std::string final_content =
        bom + restore_line_endings(updated, line_ending_style);
    const auto write_status =
        qt::foundation::write_text_file(resolved_path.value(), final_content);
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
    const std::string diff = generate_edit_diff(source, updated);
    result.output = Json{
        {"path", qt::foundation::path_for_display(resolved_path.value())},
        {"replaced", true},
        {"written", true},
        {"match_count", replacements.size()},
        {"edit_count", edits.value().size()},
        {"bytes", final_content.size()},
        {"old_bytes", old_bytes},
        {"new_bytes", new_bytes},
        {"changed_edits", changed_edits},
        {"diff", diff},
        {"first_changed_line", first_changed_line(source, first_position)},
    };
    return result;
}

}  // namespace qt::tools
