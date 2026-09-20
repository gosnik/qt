#include "index/mcp_http_utils.hpp"

#include "index/string_util.hpp"

#include <nlohmann/json.hpp>

#include <boost/process.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <map>
#include <vector>

namespace qt::index {
namespace {

namespace bp = boost::process;
using json = nlohmann::json;

std::size_t query_size(const HttpRequest& request, const std::string& key, std::size_t fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return static_cast<std::size_t>(std::strtoull(found->second.c_str(), nullptr, 10));
}

bool query_bool(const HttpRequest& request, const std::string& key, bool fallback) {
    const auto found = request.query.find(key);
    if (found == request.query.end() || found->second.empty()) {
        return fallback;
    }
    return found->second != "0" && found->second != "false";
}

std::string query_value(const HttpRequest& request, const std::string& key, const std::string& fallback = {}) {
    const auto found = request.query.find(key);
    return found == request.query.end() || found->second.empty() ? fallback : found->second;
}

std::filesystem::path executable_on_path(const std::vector<std::string>& names) {
    if (const char* path_env = std::getenv("PATH"); path_env != nullptr) {
        std::stringstream path_stream(path_env);
        std::string directory;
        while (std::getline(path_stream, directory, ':')) {
            if (directory.empty()) {
                continue;
            }
            for (const auto& name : names) {
                std::error_code ec;
                const auto candidate = std::filesystem::path{directory} / name;
                if (std::filesystem::exists(candidate, ec)) {
                    return candidate;
                }
            }
        }
    }
    return {};
}


std::string url_decode(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const std::string hex = value.substr(index + 1, 2);
            char* end = nullptr;
            const long decoded = std::strtol(hex.c_str(), &end, 16);
            if (end != hex.c_str() && *end == '\0') {
                out.push_back(static_cast<char>(decoded));
                index += 2;
                continue;
            }
        }
        out.push_back(value[index] == '+' ? ' ' : value[index]);
    }
    return out;
}

std::map<std::string, std::string> target_query(const std::string& target) {
    const auto query_pos = target.find('?');
    std::map<std::string, std::string> query;
    if (query_pos == std::string::npos) {
        return query;
    }
    const auto query_text = target.substr(query_pos + 1);
    std::size_t start = 0;
    while (start <= query_text.size()) {
        const auto amp = query_text.find('&', start);
        const auto part = query_text.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!part.empty()) {
            const auto eq = part.find('=');
            const auto key = url_decode(part.substr(0, eq));
            const auto value = eq == std::string::npos ? std::string{} : url_decode(part.substr(eq + 1));
            query[key] = value;
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }
    return query;
}

std::string html_entity_decode(std::string text) {
    const std::vector<std::pair<std::string, std::string>> entities{{"&amp;", "&"},
                                                                    {"&lt;", "<"},
                                                                    {"&gt;", ">"},
                                                                    {"&quot;", "\""},
                                                                    {"&#39;", "'"},
                                                                    {"&nbsp;", " "}};
    for (const auto& [from, to] : entities) {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }
    return text;
}

std::string strip_html_for_fetch(std::string html) {
    html = std::regex_replace(html, std::regex(R"(<script\b[^>]*>.*?</script>)", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex(R"(<style\b[^>]*>.*?</style>)", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex(R"(<\s*(br|p|div|section|article|tr|li|h[1-6])\b[^>]*>)", std::regex::icase), "\n");
    html = std::regex_replace(html, std::regex(R"(</\s*(p|div|section|article|tr|li|h[1-6])\s*>)", std::regex::icase), "\n");
    html = std::regex_replace(html, std::regex(R"(<[^>]+>)"), " ");
    html = html_entity_decode(std::move(html));
    std::stringstream input(html);
    std::string output;
    std::string line;
    bool previous_blank = false;
    while (std::getline(input, line)) {
        line = trim_ascii(std::regex_replace(line, std::regex(R"(\s+)"), " "));
        if (line.empty()) {
            if (!previous_blank && !output.empty()) {
                output.push_back('\n');
                previous_blank = true;
            }
            continue;
        }
        if (!output.empty() && output.back() != '\n') {
            output.push_back('\n');
        }
        output += line;
        previous_blank = false;
    }
    return output;
}

}  // namespace

std::string html_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::vector<std::string> split_lines_keep_empty(const std::string& text) {
    std::vector<std::string> lines;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    if (!text.empty() && text.back() == '\n') {
        lines.push_back({});
    }
    return lines;
}

json util_json_validate_payload(const HttpRequest& request) {
    const auto text = query_value(request, "json", query_value(request, "text"));
    if (text.empty()) {
        throw std::invalid_argument("json validation requires json or text");
    }
    try {
        const auto parsed = json::parse(text);
        return {{"valid", true}, {"type", parsed.type_name()}, {"query", request.query}};
    } catch (const std::exception& exc) {
        return {{"valid", false}, {"error", exc.what()}, {"query", request.query}};
    }
}

json util_json_format_payload(const HttpRequest& request) {
    const auto text = query_value(request, "json", query_value(request, "text"));
    if (text.empty()) {
        throw std::invalid_argument("json format requires json or text");
    }
    const auto parsed = json::parse(text);
    const auto indent = static_cast<int>(std::min<std::size_t>(query_size(request, "indent", 2), 8));
    return {{"formatted", parsed.dump(indent)}, {"type", parsed.type_name()}, {"query", request.query}};
}

json util_regex_test_payload(const HttpRequest& request) {
    const auto pattern = query_value(request, "pattern");
    const auto text = query_value(request, "text");
    if (pattern.empty()) {
        throw std::invalid_argument("regex test requires pattern");
    }
    std::regex::flag_type flags = std::regex::ECMAScript;
    if (query_value(request, "flags").find('i') != std::string::npos ||
        query_bool(request, "ignore_case", false)) {
        flags |= std::regex::icase;
    }
    const std::regex expression(pattern, flags);
    json matches = json::array();
    for (std::sregex_iterator it(text.begin(), text.end(), expression), end; it != end; ++it) {
        const auto& match = *it;
        json groups = json::array();
        for (std::size_t index = 1; index < match.size(); ++index) {
            groups.push_back(match[index].str());
        }
        matches.push_back({{"match", match.str()}, {"position", match.position()}, {"groups", groups}});
        if (query_size(request, "limit", 100) != 0 && matches.size() >= query_size(request, "limit", 100)) {
            break;
        }
    }
    return {{"matched", !matches.empty()}, {"matches", matches}, {"query", request.query}};
}

const std::string& base64_alphabet() {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return alphabet;
}

std::string base64_encode_text(const std::string& text) {
    std::string output;
    int value = 0;
    int bits = -6;
    for (const unsigned char ch : text) {
        value = (value << 8) + ch;
        bits += 8;
        while (bits >= 0) {
            output.push_back(base64_alphabet()[static_cast<std::size_t>((value >> bits) & 0x3F)]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        output.push_back(base64_alphabet()[static_cast<std::size_t>(((value << 8) >> (bits + 8)) & 0x3F)]);
    }
    while (output.size() % 4 != 0) {
        output.push_back('=');
    }
    return output;
}

std::string base64_decode_text(std::string text) {
    std::array<int, 256> table{};
    table.fill(-1);
    for (std::size_t index = 0; index < base64_alphabet().size(); ++index) {
        table[static_cast<unsigned char>(base64_alphabet()[index])] = static_cast<int>(index);
    }
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; }),
               text.end());
    std::string output;
    int value = 0;
    int bits = -8;
    for (const unsigned char ch : text) {
        if (ch == '=') {
            break;
        }
        if (table[ch] == -1) {
            throw std::invalid_argument("invalid base64 input");
        }
        value = (value << 6) + table[ch];
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

std::string base64url_decode_text(std::string text) {
    std::replace(text.begin(), text.end(), '-', '+');
    std::replace(text.begin(), text.end(), '_', '/');
    while (text.size() % 4 != 0) {
        text.push_back('=');
    }
    return base64_decode_text(text);
}

json util_base64_encode_payload(const HttpRequest& request) {
    const auto text = query_value(request, "text", query_value(request, "data"));
    return {{"encoded", base64_encode_text(text)}, {"bytes", text.size()}, {"query", request.query}};
}

json util_base64_decode_payload(const HttpRequest& request) {
    const auto text = query_value(request, "text", query_value(request, "data"));
    if (text.empty()) {
        throw std::invalid_argument("base64 decode requires text or data");
    }
    const auto decoded = base64_decode_text(text);
    return {{"decoded", decoded}, {"bytes", decoded.size()}, {"query", request.query}};
}

json util_uuid_payload(const HttpRequest& request) {
    const auto count = std::min<std::size_t>(query_size(request, "count", 1), 100);
    boost::uuids::random_generator generator;
    if (count <= 1) {
        const auto uuid = generator();
        return {{"uuid", boost::uuids::to_string(uuid)}, {"query", request.query}};
    }
    json values = json::array();
    for (std::size_t index = 0; index < count; ++index) {
        values.push_back(boost::uuids::to_string(generator()));
    }
    return {{"uuids", values}, {"count", values.size()}, {"query", request.query}};
}

std::string fnv1a_hex(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

json util_hash_payload(const HttpRequest& request) {
    const auto text = query_value(request, "text", query_value(request, "data"));
    const auto algorithm = lowercase_ascii(query_value(request, "algorithm", "sha256"));
    const std::map<std::string, std::string> tools{{"md5", "md5sum"},
                                                   {"sha1", "sha1sum"},
                                                   {"sha256", "sha256sum"},
                                                   {"sha512", "sha512sum"}};
    const auto tool_it = tools.find(algorithm);
    if (tool_it != tools.end()) {
        const auto executable = executable_on_path({tool_it->second});
        if (!executable.empty()) {
            bp::opstream stdin_stream;
            bp::ipstream stdout_stream;
            bp::ipstream stderr_stream;
            bp::child child(bp::exe = executable.string(),
                            bp::std_in < stdin_stream,
                            bp::std_out > stdout_stream,
                            bp::std_err > stderr_stream);
            stdin_stream << text;
            stdin_stream.flush();
            stdin_stream.pipe().close();
            std::string digest;
            stdout_stream >> digest;
            std::ostringstream stderr_text;
            stderr_text << stderr_stream.rdbuf();
            child.wait();
            return {{"algorithm", algorithm},
                    {"digest", digest},
                    {"executable", executable.string()},
                    {"exit_code", child.exit_code()},
                    {"stderr", stderr_text.str()},
                    {"query", request.query}};
        }
    }
    return {{"algorithm", "fnv1a64"},
            {"requested_algorithm", algorithm},
            {"digest", fnv1a_hex(text)},
            {"query", request.query}};
}

json parse_json_or_text(const std::string& text) {
    const auto parsed = json::parse(text, nullptr, false);
    return parsed.is_discarded() ? json(text) : parsed;
}

json util_jwt_decode_payload(const HttpRequest& request) {
    const auto token = query_value(request, "token");
    if (token.empty()) {
        throw std::invalid_argument("jwt decode requires token");
    }
    std::vector<std::string> parts;
    boost::split(parts, token, boost::is_any_of("."));
    if (parts.size() < 2) {
        throw std::invalid_argument("jwt token must contain at least header and payload segments");
    }
    const auto header = base64url_decode_text(parts[0]);
    const auto payload = base64url_decode_text(parts[1]);
    return {{"header", parse_json_or_text(header)},
            {"payload", parse_json_or_text(payload)},
            {"signature_present", parts.size() > 2 && !parts[2].empty()},
            {"query", request.query}};
}

json util_text_stats_payload(const HttpRequest& request) {
    const auto text = query_value(request, "text");
    const auto lines = split_lines_keep_empty(text);
    std::regex word_pattern(R"([A-Za-z0-9_]+)");
    const auto words = static_cast<std::size_t>(std::distance(std::sregex_iterator(text.begin(), text.end(), word_pattern),
                                                              std::sregex_iterator()));
    std::size_t paragraphs = 0;
    bool in_paragraph = false;
    for (const auto& line : lines) {
        if (trim_ascii(line).empty()) {
            in_paragraph = false;
        } else if (!in_paragraph) {
            ++paragraphs;
            in_paragraph = true;
        }
    }
    return {{"bytes", text.size()},
            {"characters", text.size()},
            {"words", words},
            {"lines", lines.empty() ? 0 : lines.size()},
            {"paragraphs", paragraphs},
            {"query", request.query}};
}

json util_slugify_payload(const HttpRequest& request) {
    const auto text = lowercase_ascii(query_value(request, "text"));
    const auto max_length = query_size(request, "max_length", 0);
    std::string slug;
    bool previous_dash = false;
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) != 0) {
            slug.push_back(static_cast<char>(ch));
            previous_dash = false;
        } else if (!previous_dash && !slug.empty()) {
            slug.push_back('-');
            previous_dash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    if (max_length != 0 && slug.size() > max_length) {
        slug.resize(max_length);
        while (!slug.empty() && slug.back() == '-') {
            slug.pop_back();
        }
    }
    return {{"slug", slug}, {"query", request.query}};
}

json util_url_parse_payload(const HttpRequest& request) {
    const auto url = query_value(request, "url");
    if (url.empty()) {
        throw std::invalid_argument("url parse requires url");
    }
    std::regex pattern(R"(^([A-Za-z][A-Za-z0-9+.-]*):\/\/([^\/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$)");
    std::smatch match;
    if (!std::regex_match(url, match, pattern)) {
        throw std::invalid_argument("unsupported URL format");
    }
    auto authority = match[2].str();
    std::string userinfo;
    if (const auto at = authority.find('@'); at != std::string::npos) {
        userinfo = authority.substr(0, at);
        authority.erase(0, at + 1);
    }
    std::string host = authority;
    std::string port;
    if (const auto colon = authority.rfind(':'); colon != std::string::npos && authority.find(']') == std::string::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    json query_params = json::object();
    auto query_text = match[4].str();
    if (!query_text.empty() && query_text.front() == '?') {
        query_text.erase(query_text.begin());
    }
    HttpRequest query_request;
    query_request.query = target_query("?" + query_text);
    for (const auto& [key, value] : query_request.query) {
        query_params[key] = value;
    }
    return {{"scheme", match[1].str()},
            {"authority", match[2].str()},
            {"userinfo", userinfo},
            {"host", host},
            {"port", port},
            {"path", match[3].str().empty() ? "/" : match[3].str()},
            {"query", query_text},
            {"query_params", query_params},
            {"fragment", match[5].str().empty() ? "" : match[5].str().substr(1)}};
}

json util_markdown_to_html_payload(const HttpRequest& request) {
    const auto markdown = query_value(request, "markdown", query_value(request, "text"));
    std::ostringstream html;
    bool in_list = false;
    for (const auto& line : split_lines_keep_empty(markdown)) {
        const auto trimmed = trim_ascii(line);
        if (trimmed.empty()) {
            if (in_list) {
                html << "</ul>\n";
                in_list = false;
            }
            continue;
        }
        if (trimmed.rfind("- ", 0) == 0 || trimmed.rfind("* ", 0) == 0) {
            if (!in_list) {
                html << "<ul>\n";
                in_list = true;
            }
            html << "<li>" << html_escape(trimmed.substr(2)) << "</li>\n";
            continue;
        }
        if (in_list) {
            html << "</ul>\n";
            in_list = false;
        }
        std::size_t hashes = 0;
        while (hashes < trimmed.size() && trimmed[hashes] == '#') {
            ++hashes;
        }
        if (hashes > 0 && hashes <= 6 && hashes + 1 < trimmed.size() && trimmed[hashes] == ' ') {
            html << "<h" << hashes << ">" << html_escape(trimmed.substr(hashes + 1)) << "</h" << hashes << ">\n";
        } else {
            html << "<p>" << html_escape(trimmed) << "</p>\n";
        }
    }
    if (in_list) {
        html << "</ul>\n";
    }
    return {{"html", html.str()}, {"query", request.query}};
}

json util_html_to_markdown_payload(const HttpRequest& request) {
    auto html = query_value(request, "html", query_value(request, "text"));
    html = std::regex_replace(html, std::regex(R"(<\s*h([1-6])\b[^>]*>(.*?)</\s*h\1\s*>)", std::regex::icase), "\n# $2\n");
    html = std::regex_replace(html, std::regex(R"(<\s*li\b[^>]*>)", std::regex::icase), "\n- ");
    return {{"markdown", strip_html_for_fetch(std::move(html))}, {"query", request.query}};
}

std::string csv_escape(const std::string& value) {
    const bool quote = value.find_first_of(",\"\n\r") != std::string::npos;
    if (!quote) {
        return value;
    }
    std::string escaped = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            escaped += "\"\"";
        } else {
            escaped.push_back(ch);
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::vector<std::vector<std::string>> parse_csv_records(const std::string& csv) {
    std::vector<std::vector<std::string>> records{{}};
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < csv.size(); ++index) {
        const char ch = csv[index];
        if (quoted) {
            if (ch == '"' && index + 1 < csv.size() && csv[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else if (ch == '"') {
                quoted = false;
            } else {
                field.push_back(ch);
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            records.back().push_back(field);
            field.clear();
        } else if (ch == '\n') {
            records.back().push_back(field);
            field.clear();
            records.push_back({});
        } else if (ch != '\r') {
            field.push_back(ch);
        }
    }
    records.back().push_back(field);
    if (records.size() > 1 && records.back().size() == 1 && records.back()[0].empty()) {
        records.pop_back();
    }
    return records;
}

json util_csv_to_json_payload(const HttpRequest& request) {
    const auto csv = query_value(request, "csv", query_value(request, "text"));
    const bool header = query_bool(request, "header", true);
    const auto records = parse_csv_records(csv);
    json rows = json::array();
    if (records.empty()) {
        return {{"rows", rows}, {"query", request.query}};
    }
    std::vector<std::string> headers;
    std::size_t start = 0;
    if (header) {
        headers = records.front();
        start = 1;
    }
    for (std::size_t row_index = start; row_index < records.size(); ++row_index) {
        if (header) {
            json object = json::object();
            for (std::size_t col = 0; col < records[row_index].size() && col < headers.size(); ++col) {
                object[headers[col]] = records[row_index][col];
            }
            rows.push_back(object);
        } else {
            rows.push_back(records[row_index]);
        }
    }
    return {{"rows", rows}, {"count", rows.size()}, {"query", request.query}};
}

json util_json_to_csv_payload(const HttpRequest& request) {
    const auto text = query_value(request, "json", query_value(request, "text"));
    const auto parsed = json::parse(text);
    if (!parsed.is_array()) {
        throw std::invalid_argument("json to csv requires an array");
    }
    std::vector<std::string> headers;
    for (const auto& row : parsed) {
        if (row.is_object()) {
            for (const auto& [key, _] : row.items()) {
                if (std::find(headers.begin(), headers.end(), key) == headers.end()) {
                    headers.push_back(key);
                }
            }
        }
    }
    std::ostringstream csv;
    if (!headers.empty()) {
        for (std::size_t index = 0; index < headers.size(); ++index) {
            if (index != 0) {
                csv << ',';
            }
            csv << csv_escape(headers[index]);
        }
        csv << '\n';
        for (const auto& row : parsed) {
            for (std::size_t index = 0; index < headers.size(); ++index) {
                if (index != 0) {
                    csv << ',';
                }
                if (row.contains(headers[index])) {
                    const auto& value = row[headers[index]];
                    csv << csv_escape(value.is_string() ? value.get<std::string>() : value.dump());
                }
            }
            csv << '\n';
        }
    } else {
        for (const auto& row : parsed) {
            if (!row.is_array()) {
                continue;
            }
            for (std::size_t index = 0; index < row.size(); ++index) {
                if (index != 0) {
                    csv << ',';
                }
                csv << csv_escape(row[index].is_string() ? row[index].get<std::string>() : row[index].dump());
            }
            csv << '\n';
        }
    }
    return {{"csv", csv.str()}, {"query", request.query}};
}

std::string format_time_utc(std::time_t timestamp, const std::string& format) {
    std::tm tm{};
    gmtime_r(&timestamp, &tm);
    std::ostringstream stream;
    stream << std::put_time(&tm, format.c_str());
    return stream.str();
}

json util_timestamp_payload(const HttpRequest& request) {
    const auto timestamp = static_cast<std::time_t>(std::strtoll(query_value(request, "timestamp", "0").c_str(), nullptr, 10));
    const auto format = query_value(request, "format", "%Y-%m-%dT%H:%M:%SZ");
    return {{"timestamp", static_cast<std::int64_t>(timestamp)},
            {"utc", format_time_utc(timestamp, format)},
            {"query", request.query}};
}

json util_datetime_format_payload(const HttpRequest& request) {
    if (request.query.count("timestamp") != 0) {
        return util_timestamp_payload(request);
    }
    const auto text = query_value(request, "datetime");
    std::tm tm{};
    std::istringstream input(text);
    input >> std::get_time(&tm, query_value(request, "input_format", "%Y-%m-%dT%H:%M:%S").c_str());
    if (input.fail()) {
        throw std::invalid_argument("datetime did not match input_format");
    }
    const auto output_format = query_value(request, "output_format", "%Y-%m-%dT%H:%M:%SZ");
    std::ostringstream output;
    output << std::put_time(&tm, output_format.c_str());
    return {{"formatted", output.str()}, {"query", request.query}};
}

bool cron_field_matches(const std::string& field, int value) {
    if (field == "*") {
        return true;
    }
    if (field.rfind("*/", 0) == 0) {
        const auto step = std::max(1, std::atoi(field.substr(2).c_str()));
        return value % step == 0;
    }
    std::vector<std::string> parts;
    boost::split(parts, field, boost::is_any_of(","));
    for (const auto& part : parts) {
        const auto dash = part.find('-');
        if (dash != std::string::npos) {
            const int start = std::atoi(part.substr(0, dash).c_str());
            const int end = std::atoi(part.substr(dash + 1).c_str());
            if (value >= start && value <= end) {
                return true;
            }
        } else if (value == std::atoi(part.c_str())) {
            return true;
        }
    }
    return false;
}

json util_cron_next_payload(const HttpRequest& request) {
    const auto expression = query_value(request, "expression", query_value(request, "cron"));
    std::vector<std::string> fields;
    boost::split(fields, expression, boost::is_any_of(" "), boost::token_compress_on);
    if (fields.size() != 5) {
        throw std::invalid_argument("cron expression must have five fields");
    }
    auto start = static_cast<std::time_t>(std::strtoll(query_value(request, "from_timestamp", "0").c_str(), nullptr, 10));
    if (start == 0) {
        start = std::time(nullptr);
    }
    start -= start % 60;
    for (int minutes = 1; minutes <= 366 * 24 * 60; ++minutes) {
        const auto candidate = start + minutes * 60;
        std::tm tm{};
        gmtime_r(&candidate, &tm);
        if (cron_field_matches(fields[0], tm.tm_min) && cron_field_matches(fields[1], tm.tm_hour) &&
            cron_field_matches(fields[2], tm.tm_mday) && cron_field_matches(fields[3], tm.tm_mon + 1) &&
            cron_field_matches(fields[4], tm.tm_wday)) {
            return {{"expression", expression},
                    {"next_timestamp", static_cast<std::int64_t>(candidate)},
                    {"next_utc", format_time_utc(candidate, "%Y-%m-%dT%H:%M:%SZ")},
                    {"query", request.query}};
        }
    }
    throw std::runtime_error("no cron match found within one year");
}

json util_text_diff_payload(const HttpRequest& request) {
    const auto before = split_lines_keep_empty(query_value(request, "before", query_value(request, "a")));
    const auto after = split_lines_keep_empty(query_value(request, "after", query_value(request, "b")));
    json lines = json::array();
    const auto count = std::max(before.size(), after.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto old_line = index < before.size() ? before[index] : std::string{};
        const auto new_line = index < after.size() ? after[index] : std::string{};
        if (index < before.size() && index < after.size() && old_line == new_line) {
            lines.push_back({{"op", "equal"}, {"line", old_line}});
        } else {
            if (index < before.size()) {
                lines.push_back({{"op", "delete"}, {"line", old_line}});
            }
            if (index < after.size()) {
                lines.push_back({{"op", "insert"}, {"line", new_line}});
            }
        }
    }
    return {{"diff", lines}, {"query", request.query}};
}


}  // namespace qt::index
