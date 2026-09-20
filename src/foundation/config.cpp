#include "qt/foundation/config.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qt::foundation {
namespace {

struct ParsedLine {
    std::size_t line_number = 0;
    std::size_t indent = 0;
    std::string text;
};

std::string trim_copy(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }

    return std::string(text.substr(begin, end - begin));
}

std::string line_error(std::size_t line_number, std::string message) {
    std::ostringstream stream;
    stream << "line " << line_number << ": " << message;
    return stream.str();
}

Result<std::string> strip_comment(std::string_view line, std::size_t line_number) {
    std::string output;
    output.reserve(line.size());
    char quote = '\0';
    bool escaped = false;

    for (char character : line) {
        if (quote != '\0') {
            output.push_back(character);
            if (quote == '"' && character == '\\' && !escaped) {
                escaped = true;
                continue;
            }
            if (character == quote && !escaped) {
                quote = '\0';
            }
            escaped = false;
            continue;
        }

        if (character == '\'' || character == '"') {
            quote = character;
            output.push_back(character);
            continue;
        }
        if (character == '#') {
            break;
        }
        output.push_back(character);
    }

    if (quote != '\0') {
        return Result<std::string>::failure(ErrorCode::encoding_error,
                                            line_error(line_number, "unterminated quote"));
    }
    return output;
}

Result<std::vector<ParsedLine>> collect_lines(std::string_view content) {
    std::vector<ParsedLine> lines;
    std::istringstream stream{std::string(content)};
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(stream, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && line.front() == '\t') {
            return Result<std::vector<ParsedLine>>::failure(
                ErrorCode::invalid_argument,
                line_error(line_number, "tabs are not valid indentation"));
        }

        const auto without_comment = strip_comment(line, line_number);
        if (!without_comment) {
            return without_comment.error();
        }

        std::size_t indent = 0;
        while (indent < without_comment.value().size() &&
               without_comment.value()[indent] == ' ') {
            ++indent;
        }

        std::string text = trim_copy(std::string_view(without_comment.value()).substr(indent));
        if (text.empty()) {
            continue;
        }
        lines.push_back(ParsedLine{line_number, indent, std::move(text)});
    }

    if (!lines.empty()) {
        std::size_t minimum_indent = lines.front().indent;
        for (const auto& parsed_line : lines) {
            if (parsed_line.indent < minimum_indent) {
                minimum_indent = parsed_line.indent;
            }
        }
        for (auto& parsed_line : lines) {
            parsed_line.indent -= minimum_indent;
        }
    }

    return lines;
}

Result<std::size_t> find_key_separator(std::string_view text, std::size_t line_number) {
    char quote = '\0';
    bool escaped = false;

    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (quote != '\0') {
            if (quote == '"' && character == '\\' && !escaped) {
                escaped = true;
                continue;
            }
            if (character == quote && !escaped) {
                quote = '\0';
            }
            escaped = false;
            continue;
        }

        if (character == '\'' || character == '"') {
            quote = character;
            continue;
        }
        if (character == ':') {
            return index;
        }
    }

    return Result<std::size_t>::failure(ErrorCode::invalid_argument,
                                        line_error(line_number, "expected key: value"));
}

Result<std::string> parse_quoted(std::string_view text, std::size_t line_number) {
    if (text.size() < 2) {
        return Result<std::string>::failure(ErrorCode::encoding_error,
                                            line_error(line_number, "unterminated quote"));
    }

    const char quote = text.front();
    if (text.back() != quote) {
        return Result<std::string>::failure(
            ErrorCode::encoding_error,
            line_error(line_number, "quoted values must end at the closing quote"));
    }

    std::string value;
    value.reserve(text.size() - 2);
    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        const char character = text[index];
        if (quote == '"' && character == '\\') {
            if (index + 2 >= text.size()) {
                return Result<std::string>::failure(
                    ErrorCode::encoding_error, line_error(line_number, "unterminated escape"));
            }
            const char escaped = text[++index];
            switch (escaped) {
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case '\\':
                case '"':
                    value.push_back(escaped);
                    break;
                default:
                    return Result<std::string>::failure(
                        ErrorCode::encoding_error,
                        line_error(line_number, "unsupported escape sequence"));
            }
            continue;
        }
        value.push_back(character);
    }

    return value;
}

Result<std::string> parse_key(std::string_view text, std::size_t line_number) {
    const std::string trimmed = trim_copy(text);
    if (trimmed.empty()) {
        return Result<std::string>::failure(ErrorCode::invalid_argument,
                                            line_error(line_number, "key must not be empty"));
    }
    if (trimmed.front() == '\'' || trimmed.front() == '"') {
        return parse_quoted(trimmed, line_number);
    }
    return trimmed;
}

Result<ConfigValue> parse_scalar(std::string_view text, std::size_t line_number) {
    const std::string trimmed = trim_copy(text);
    if (!trimmed.empty() && (trimmed.front() == '\'' || trimmed.front() == '"')) {
        const auto parsed = parse_quoted(trimmed, line_number);
        if (!parsed) {
            return parsed.error();
        }
        return ConfigValue::scalar(parsed.value());
    }
    return ConfigValue::scalar(trimmed);
}

Result<ConfigValue> parse_map_at(const std::vector<ParsedLine>& lines,
                                 std::size_t& index,
                                 std::size_t indent) {
    ConfigValue::Map values;

    while (index < lines.size()) {
        const ParsedLine& line = lines[index];
        if (line.indent < indent) {
            break;
        }
        if (line.indent > indent) {
            return Result<ConfigValue>::failure(
                ErrorCode::invalid_argument,
                line_error(line.line_number, "unexpected indentation"));
        }

        const auto separator = find_key_separator(line.text, line.line_number);
        if (!separator) {
            return separator.error();
        }

        const auto key = parse_key(std::string_view(line.text).substr(0, separator.value()),
                                   line.line_number);
        if (!key) {
            return key.error();
        }
        if (values.find(key.value()) != values.end()) {
            return Result<ConfigValue>::failure(
                ErrorCode::invalid_argument,
                line_error(line.line_number, "duplicate key: " + key.value()));
        }

        const std::string rest =
            trim_copy(std::string_view(line.text).substr(separator.value() + 1));
        ++index;

        if (!rest.empty()) {
            const auto scalar = parse_scalar(rest, line.line_number);
            if (!scalar) {
                return scalar.error();
            }
            values.emplace(key.value(), scalar.value());
            continue;
        }

        if (index >= lines.size() || lines[index].indent <= line.indent) {
            values.emplace(key.value(), ConfigValue::mapping());
            continue;
        }

        const std::size_t child_indent = lines[index].indent;
        const auto child = parse_map_at(lines, index, child_indent);
        if (!child) {
            return child.error();
        }
        values.emplace(key.value(), child.value());
    }

    return ConfigValue::mapping(std::move(values));
}

std::string lower_copy(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (char character : text) {
        if (character >= 'A' && character <= 'Z') {
            lowered.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            lowered.push_back(character);
        }
    }
    return lowered;
}

}  // namespace

ConfigValue::ConfigValue() = default;

ConfigValue ConfigValue::scalar(std::string value) {
    ConfigValue config_value;
    config_value.kind_ = ConfigValueKind::scalar;
    config_value.scalar_ = std::move(value);
    config_value.map_.clear();
    return config_value;
}

ConfigValue ConfigValue::mapping(Map values) {
    ConfigValue config_value;
    config_value.kind_ = ConfigValueKind::map;
    config_value.map_ = std::move(values);
    config_value.scalar_.clear();
    return config_value;
}

ConfigValueKind ConfigValue::kind() const noexcept { return kind_; }

bool ConfigValue::is_scalar() const noexcept {
    return kind_ == ConfigValueKind::scalar;
}

bool ConfigValue::is_map() const noexcept { return kind_ == ConfigValueKind::map; }

const std::string& ConfigValue::scalar() const {
    if (!is_scalar()) {
        throw std::logic_error("config value is not a scalar");
    }
    return scalar_;
}

const ConfigValue::Map& ConfigValue::map() const {
    if (!is_map()) {
        throw std::logic_error("config value is not a map");
    }
    return map_;
}

const ConfigValue* ConfigValue::find(std::string_view key) const {
    if (!is_map()) {
        return nullptr;
    }
    const auto found = map_.find(std::string(key));
    if (found == map_.end()) {
        return nullptr;
    }
    return &found->second;
}

const ConfigValue* ConfigValue::find_path(std::string_view dotted_path) const {
    const ConfigValue* current = this;
    std::size_t offset = 0;
    while (offset <= dotted_path.size()) {
        const std::size_t dot = dotted_path.find('.', offset);
        const std::size_t end = dot == std::string_view::npos ? dotted_path.size() : dot;
        if (end == offset) {
            return nullptr;
        }
        current = current->find(dotted_path.substr(offset, end - offset));
        if (!current) {
            return nullptr;
        }
        if (dot == std::string_view::npos) {
            return current;
        }
        offset = dot + 1;
    }
    return nullptr;
}

Result<bool> ConfigValue::as_bool() const {
    if (!is_scalar()) {
        return Result<bool>::failure(ErrorCode::invalid_argument,
                                     "config value is not a scalar");
    }
    const std::string value = lower_copy(trim_copy(scalar_));
    if (value == "true" || value == "yes" || value == "on" || value == "1") {
        return true;
    }
    if (value == "false" || value == "no" || value == "off" || value == "0") {
        return false;
    }
    return Result<bool>::failure(ErrorCode::invalid_argument,
                                 "config value is not a boolean: " + scalar_);
}

Result<std::int64_t> ConfigValue::as_integer() const {
    if (!is_scalar()) {
        return Result<std::int64_t>::failure(ErrorCode::invalid_argument,
                                             "config value is not a scalar");
    }

    const std::string value = trim_copy(scalar_);
    if (value.empty()) {
        return Result<std::int64_t>::failure(ErrorCode::invalid_argument,
                                             "config value is not an integer");
    }

    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' ||
        parsed < std::numeric_limits<std::int64_t>::min() ||
        parsed > std::numeric_limits<std::int64_t>::max()) {
        return Result<std::int64_t>::failure(ErrorCode::invalid_argument,
                                             "config value is not an integer: " + scalar_);
    }
    return static_cast<std::int64_t>(parsed);
}

Result<double> ConfigValue::as_number() const {
    if (!is_scalar()) {
        return Result<double>::failure(ErrorCode::invalid_argument,
                                       "config value is not a scalar");
    }

    const std::string value = trim_copy(scalar_);
    if (value.empty()) {
        return Result<double>::failure(ErrorCode::invalid_argument,
                                       "config value is not a number");
    }

    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(value.c_str(), &end);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        return Result<double>::failure(ErrorCode::invalid_argument,
                                       "config value is not a number: " + scalar_);
    }
    return parsed;
}

const ConfigValue* ConfigDocument::find(std::string_view key) const {
    return root.find(key);
}

const ConfigValue* ConfigDocument::find_path(std::string_view dotted_path) const {
    return root.find_path(dotted_path);
}

Result<ConfigDocument> parse_config(std::string_view content) {
    auto lines = collect_lines(content);
    if (!lines) {
        return lines.error();
    }

    std::size_t index = 0;
    auto root = parse_map_at(lines.value(), index, 0);
    if (!root) {
        return root.error();
    }
    if (index != lines.value().size()) {
        return Result<ConfigDocument>::failure(
            ErrorCode::invalid_argument,
            line_error(lines.value()[index].line_number, "could not parse line"));
    }

    ConfigDocument document;
    document.root = root.value();
    return document;
}

Result<ConfigDocument> load_config_file(const Path& path) {
    auto content = read_text_file(path);
    if (!content) {
        return content.error();
    }
    return parse_config(content.value());
}

}  // namespace qt::foundation
