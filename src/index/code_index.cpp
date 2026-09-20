#include "index/code_index.hpp"

#include "index/cancellation.hpp"
#include "index/path_util.hpp"
#include "index/string_util.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

namespace qt::index {
namespace {

using json = nlohmann::json;

void low_priority_checkpoint(bool low_priority, std::size_t operations) {
    scan_cancellation_checkpoint();
    if (!low_priority) {
        return;
    }
    std::this_thread::yield();
    if (operations % 128U == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool is_cpp_index_candidate(ProjectFileKind kind) {
    return kind == ProjectFileKind::CSource || kind == ProjectFileKind::CxxSource ||
           kind == ProjectFileKind::Header;
}

std::optional<std::string> dependency_or_generated_reason(const std::filesystem::path& relative_path) {
    std::vector<std::string> parts;
    for (const auto& part : relative_path) {
        const auto value = part.generic_string();
        if (!value.empty() && value != ".") {
            parts.push_back(value);
        }
    }
    if (parts.empty()) {
        return std::nullopt;
    }

    const auto first = lowercase_ascii(parts[0]);
    if (first == ".pio" || first == ".index" || first == "node_modules" ||
        first == "build" || first == "cmake-build-debug" || first == "cmake-build-release") {
        return "build, cache, or dependency directory";
    }
    if (first == "vendor" || first == "third_party" || first == "external") {
        return "vendored dependency directory";
    }
    if (first == "lib" && parts.size() > 1) {
        const auto second = lowercase_ascii(parts[1]);
        if (second.rfind("boost_", 0) == 0 || second == "tree-sitter" ||
            second == "tree-sitter-cpp") {
            return "vendored dependency directory";
        }
    }

    const auto filename = relative_path.filename().generic_string();
    const auto lower_filename = lowercase_ascii(filename);
    if (lower_filename.rfind("moc_", 0) == 0 || lower_filename.rfind("qrc_", 0) == 0 ||
        lower_filename.rfind("ui_", 0) == 0 ||
        (lower_filename.size() > 6 &&
         lower_filename.substr(lower_filename.size() - 6) == ".pb.cc")) {
        return "generated source file";
    }
    return std::nullopt;
}

std::string strip_comments_and_strings(std::string line, bool& block_comment) {
    std::string out;
    out.reserve(line.size());
    bool string_literal = false;
    bool char_literal = false;
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        const char next = index + 1 < line.size() ? line[index + 1] : '\0';
        if (block_comment) {
            if (ch == '*' && next == '/') {
                block_comment = false;
                ++index;
            }
            out.push_back(' ');
            continue;
        }
        if (string_literal || char_literal) {
            out.push_back(' ');
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (string_literal && ch == '"') {
                string_literal = false;
            } else if (char_literal && ch == '\'') {
                char_literal = false;
            }
            continue;
        }
        if (ch == '/' && next == '*') {
            block_comment = true;
            out.push_back(' ');
            ++index;
            continue;
        }
        if (ch == '/' && next == '/') {
            break;
        }
        if (ch == '"') {
            string_literal = true;
            out.push_back(' ');
            continue;
        }
        if (ch == '\'') {
            char_literal = true;
            out.push_back(' ');
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

int brace_delta(const std::string& line) {
    int delta = 0;
    for (const char ch : line) {
        if (ch == '{') {
            ++delta;
        } else if (ch == '}') {
            --delta;
        }
    }
    return delta;
}

bool looks_like_control_keyword(const std::string& name) {
    static const std::set<std::string> keywords{"if",       "for",      "while", "switch",
                                                "catch",    "return",   "sizeof", "alignof",
                                                "static_cast", "reinterpret_cast",
                                                "const_cast", "dynamic_cast", "decltype"};
    return keywords.find(name) != keywords.end();
}

bool is_identifier_start(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '~';
}

bool is_identifier_continue(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == ':' || ch == '~';
}

std::string unqualified_name(std::string name) {
    const auto separator = name.rfind("::");
    if (separator != std::string::npos) {
        name = name.substr(separator + 2);
    }
    return name;
}

bool find_class_name(const std::string& line, std::string& name) {
    std::size_t index = 0;
    while (index < line.size()) {
        if (!is_identifier_start(line[index])) {
            ++index;
            continue;
        }
        const auto start = index;
        while (index < line.size() && is_identifier_continue(line[index])) {
            ++index;
        }
        const auto word = line.substr(start, index - start);
        if (word != "class" && word != "struct") {
            continue;
        }
        while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index >= line.size() || !is_identifier_start(line[index])) {
            return false;
        }
        const auto name_start = index;
        while (index < line.size() &&
               (std::isalnum(static_cast<unsigned char>(line[index])) || line[index] == '_')) {
            ++index;
        }
        name = line.substr(name_start, index - name_start);
        return !name.empty();
    }
    return false;
}

bool has_function_body_after_close_paren(const std::string& line, std::size_t close) {
    std::size_t index = close + 1;
    while (index < line.size()) {
        while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index >= line.size()) {
            return false;
        }
        if (line[index] == '{') {
            return true;
        }
        if (line[index] == ';' || line[index] == '=') {
            return false;
        }
        if (!is_identifier_start(line[index])) {
            return false;
        }
        const auto start = index;
        while (index < line.size() && is_identifier_continue(line[index])) {
            ++index;
        }
        const auto qualifier = line.substr(start, index - start);
        if (qualifier != "const" && qualifier != "noexcept" && qualifier != "override" &&
            qualifier != "final") {
            return false;
        }
    }
    return false;
}

bool find_function_name(const std::string& line, std::string& name) {
    for (std::size_t open = line.find('('); open != std::string::npos;
         open = line.find('(', open + 1)) {
        const auto close = line.find(')', open + 1);
        if (close == std::string::npos || !has_function_body_after_close_paren(line, close)) {
            continue;
        }
        std::size_t end = open;
        while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1]))) {
            --end;
        }
        std::size_t start = end;
        while (start > 0 && is_identifier_continue(line[start - 1])) {
            --start;
        }
        if (start == end) {
            continue;
        }
        name = unqualified_name(line.substr(start, end - start));
        if (!name.empty() && !looks_like_control_keyword(name)) {
            return true;
        }
    }
    return false;
}

void append_calls(const std::string& line,
                  const std::string& current_function,
                  const std::filesystem::path& file,
                  std::size_t line_number,
                  std::vector<CallGraphEdge>& call_graph) {
    std::size_t index = 0;
    while (index < line.size()) {
        if (!is_identifier_start(line[index])) {
            ++index;
            continue;
        }
        const auto start = index;
        while (index < line.size() && is_identifier_continue(line[index])) {
            ++index;
        }
        auto callee = line.substr(start, index - start);
        const auto short_callee = unqualified_name(callee);
        while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index < line.size() && line[index] == '(' &&
            !looks_like_control_keyword(short_callee) && short_callee != current_function &&
            callee != current_function) {
            call_graph.push_back({current_function, std::move(callee), file, line_number});
        }
    }
}

std::string qualified(const std::vector<std::string>& scopes, const std::string& name) {
    std::string result;
    for (const auto& scope : scopes) {
        if (!result.empty()) {
            result += "::";
        }
        result += scope;
    }
    if (!result.empty()) {
        result += "::";
    }
    result += name;
    return result;
}

json method_json(const MethodSymbol& method) {
    return {{"name", method.name},
            {"qualified_name", method.qualified_name},
            {"file", method.file.generic_string()},
            {"line", method.line}};
}

json class_json(const ClassSymbol& klass) {
    json methods = json::array();
    for (const auto& method : klass.methods) {
        methods.push_back(method_json(method));
    }
    return {{"name", klass.name},
            {"qualified_name", klass.qualified_name},
            {"file", klass.file.generic_string()},
            {"line", klass.line},
            {"methods", methods}};
}

json function_json(const FunctionSymbol& function) {
    return {{"name", function.name},
            {"qualified_name", function.qualified_name},
            {"file", function.file.generic_string()},
            {"line", function.line},
            {"kind", function.kind}};
}

json call_json(const CallGraphEdge& edge) {
    return {{"caller", edge.caller},
            {"callee", edge.callee},
            {"file", edge.file.generic_string()},
            {"line", edge.line}};
}

json skipped_file_json(const SkippedCodeIndexFile& skipped) {
    return {{"file", skipped.file.generic_string()}, {"reason", skipped.reason}};
}

}  // namespace

CodeIndex build_code_index(const ProjectScanResult& scan) {
    return build_code_index(scan, false);
}

CodeIndex build_code_index(const ProjectScanResult& scan, bool low_priority) {
    CodeIndex index;

    std::size_t operations = 0;
    for (const auto& file : scan.files) {
        low_priority_checkpoint(low_priority, ++operations);
        if (!is_cpp_index_candidate(file.kind)) {
            continue;
        }
        if (const auto reason = dependency_or_generated_reason(file.path)) {
            index.skipped_files.push_back({file.path, *reason});
            continue;
        }
        std::ifstream input(scan.root / file.path);
        if (!input) {
            continue;
        }

        bool block_comment = false;
        int brace_depth = 0;
        std::vector<std::pair<std::string, int>> class_scopes;
        std::vector<std::string> scope_names;
        std::string current_function;
        int function_depth = -1;

        std::string raw;
        std::size_t line_number = 0;
        while (std::getline(input, raw)) {
            low_priority_checkpoint(low_priority, ++operations);
            ++line_number;
            const auto line = strip_comments_and_strings(raw, block_comment);

            std::string class_name;
            if (find_class_name(line, class_name)) {
                const std::string name = class_name;
                scope_names.push_back(name);
                class_scopes.emplace_back(name, brace_depth + 1);
                ClassSymbol klass;
                klass.name = name;
                klass.qualified_name = qualified(std::vector<std::string>(scope_names.begin(),
                                                                          scope_names.end() - 1),
                                                 name);
                klass.file = file.path;
                klass.line = line_number;
                index.classes.push_back(std::move(klass));
            }

            std::string function_name;
            if (find_function_name(line, function_name)) {
                const bool in_class = !scope_names.empty();
                FunctionSymbol function;
                function.name = function_name;
                function.qualified_name = qualified(scope_names, function_name);
                function.file = file.path;
                function.line = line_number;
                function.kind = in_class ? "method" : "function";
                index.functions.push_back(function);
                if (in_class && !index.classes.empty()) {
                    MethodSymbol method;
                    method.name = function_name;
                    method.qualified_name = function.qualified_name;
                    method.file = file.path;
                    method.line = line_number;
                    index.classes.back().methods.push_back(std::move(method));
                }
                current_function = function.qualified_name;
                function_depth = brace_depth + 1;
            }

            if (!current_function.empty()) {
                append_calls(line, current_function, file.path, line_number, index.call_graph);
            }

            brace_depth += brace_delta(line);
            while (!class_scopes.empty() && brace_depth < class_scopes.back().second) {
                class_scopes.pop_back();
                if (!scope_names.empty()) {
                    scope_names.pop_back();
                }
            }
            if (!current_function.empty() && brace_depth < function_depth) {
                current_function.clear();
                function_depth = -1;
            }
        }
    }
    return index;
}

std::string code_index_json(const CodeIndex& index) {
    json classes = json::array();
    json functions = json::array();
    json call_graph = json::array();
    json skipped_files = json::array();
    for (const auto& klass : index.classes) {
        classes.push_back(class_json(klass));
    }
    for (const auto& function : index.functions) {
        functions.push_back(function_json(function));
    }
    for (const auto& edge : index.call_graph) {
        call_graph.push_back(call_json(edge));
    }
    for (const auto& skipped : index.skipped_files) {
        skipped_files.push_back(skipped_file_json(skipped));
    }
    return json{{"classes", classes},
                {"functions", functions},
                {"call_graph", call_graph},
                {"skipped_files", skipped_files}}
        .dump(2);
}

std::string code_index_summary_json(const CodeIndex& index) {
    return json{{"classes", index.classes.size()},
                {"functions", index.functions.size()},
                {"call_graph_edges", index.call_graph.size()},
                {"skipped_files", index.skipped_files.size()}}
        .dump(2);
}

}  // namespace qt::index
