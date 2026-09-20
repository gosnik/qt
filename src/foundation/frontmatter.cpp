#include "qt/foundation/frontmatter.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace qt::foundation {
namespace {

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

std::string maybe_unquote(std::string value) {
    if (value.size() >= 2U &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string normalize_newlines(std::string text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                text.erase(i, 1);
            } else {
                text[i] = '\n';
            }
        }
    }
    return text;
}

}  // namespace

FrontmatterParseResult parse_frontmatter(std::string_view content) {
    FrontmatterParseResult result;
    const std::string normalized = normalize_newlines(std::string(content));
    const std::size_t first_line_end = normalized.find('\n');
    const std::string first_line = normalized.substr(0, first_line_end);
    if (first_line != "---") {
        return result;
    }

    result.had_frontmatter = true;
    std::size_t offset =
        first_line_end == std::string::npos ? normalized.size() : first_line_end + 1U;

    while (offset <= normalized.size()) {
        const std::size_t line_end = normalized.find('\n', offset);
        const std::string line = normalized.substr(
            offset, line_end == std::string::npos ? std::string::npos : line_end - offset);

        if (line == "---") {
            result.complete = true;
            break;
        }

        const std::string trimmed = trim(line);
        if (!trimmed.empty() && trimmed.front() != '#') {
            const std::size_t separator = trimmed.find(':');
            if (separator == std::string::npos) {
                result.diagnostics.push_back(
                    "frontmatter line is missing ':' separator");
            } else {
                const std::string key = trim(trimmed.substr(0, separator));
                const std::string value =
                    maybe_unquote(trim(trimmed.substr(separator + 1U)));
                if (!key.empty()) {
                    result.fields[key] = value;
                }
            }
        }

        if (line_end == std::string::npos) {
            break;
        }
        offset = line_end + 1U;
    }

    if (!result.complete) {
        result.diagnostics.push_back(
            "frontmatter block is missing closing delimiter");
    }

    return result;
}

FrontmatterParseResult parse_code_fence_frontmatter(std::string_view content) {
    FrontmatterParseResult result;
    const std::string normalized = normalize_newlines(std::string(content));
    const std::size_t first_line_end = normalized.find('\n');
    std::string first_line = normalized.substr(0, first_line_end);

    if (first_line.rfind("```skill", 0) != 0 &&
        first_line.rfind("```SKILL", 0) != 0) {
        return result;
    }

    result.had_frontmatter = true;

    if (first_line.size() > 2 && first_line[2] == '`' &&
        (first_line.rfind("```skill ", 0) == 0 || first_line.rfind("```SKILL ", 0) == 0)) {
        const std::size_t lang_start = 9;
        if (first_line.size() > lang_start) {
            result.fields["_language"] = first_line.substr(lang_start);
        }
    }

    std::size_t offset =
        first_line_end == std::string::npos ? normalized.size() : first_line_end + 1U;

    while (offset <= normalized.size()) {
        const std::size_t line_end = normalized.find('\n', offset);
        const std::string line = normalized.substr(
            offset, line_end == std::string::npos ? std::string::npos : line_end - offset);

        if (line == "```") {
            result.complete = true;
            break;
        }

        const std::string trimmed = trim(line);
        if (!trimmed.empty()) {
            const std::size_t separator = trimmed.find(':');
            if (separator == std::string::npos) {
                result.diagnostics.push_back(
                    "frontmatter line is missing ':' separator");
            } else {
                const std::string key = trim(trimmed.substr(0, separator));
                const std::string value =
                    maybe_unquote(trim(trimmed.substr(separator + 1U)));
                if (!key.empty()) {
                    result.fields[key] = value;
                }
            }
        }

        if (line_end == std::string::npos) {
            break;
        }
        offset = line_end + 1U;
    }

    if (!result.complete) {
        result.diagnostics.push_back(
            "code-fence frontmatter block is missing closing delimiter");
    }

    return result;
}

}  // namespace qt::foundation
