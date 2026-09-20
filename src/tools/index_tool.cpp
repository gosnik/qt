#include "qt/tools/index_tool.hpp"

#include "qt/ai/models.hpp"
#include "qt/foundation/filesystem.hpp"
#include "qt/tools/tool_validation.hpp"
#include "index/build_metadata.hpp"
#include "index/code_index.hpp"
#include "index/compilation_database.hpp"
#include "index/file_search.hpp"
#include "index/mcp_http_build.hpp"
#include "index/mcp_http_code_index.hpp"
#include "index/mcp_http_debug.hpp"
#include "index/mcp_http_fetch.hpp"
#include "index/mcp_http_filesystem.hpp"
#include "index/mcp_http_git.hpp"
#include "index/mcp_http_request.hpp"
#include "index/mcp_http_rtos.hpp"
#include "index/mcp_http_serial.hpp"
#include "index/mcp_http_utils.hpp"
#include "index/project_scan.hpp"
#include "index/service.hpp"
#include "index/sqlite_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace qt::tools {
namespace {

qt::foundation::Result<qt::foundation::Path> workspace_root_from_context(
    const ToolContext& context) {
    if (context.workspace_root) {
        return qt::foundation::canonicalize_path(*context.workspace_root);
    }
    if (context.current_directory) {
        return qt::foundation::canonicalize_path(*context.current_directory);
    }
    return qt::foundation::canonicalize_path(std::filesystem::current_path());
}

ToolResult make_exception_result(const ToolRequest& request, const std::exception& error) {
    return make_execution_error(ToolResultStatus::execution_error, error.what(), request);
}

std::string optional_string_argument(const ToolRequest& request, const char* name) {
    if (!request.arguments.contains(name)) {
        return {};
    }
    if (!request.arguments.at(name).is_string()) {
        throw std::invalid_argument(request.tool_name + " requires a string " + name +
                                    " argument");
    }
    return request.arguments.at(name).get<std::string>();
}

ToolResult make_success(const ToolRequest& request, Json output) {
    ToolResult result;
    result.status = ToolResultStatus::success;
    result.call_id = request.call_id;
    result.output = std::move(output);
    return result;
}

std::string argument_to_query_text(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "1" : "0";
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<std::uint64_t>());
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number_float()) {
        return std::to_string(value.get<double>());
    }
    return {};
}

std::string plain_fts_query(const std::string& query) {
    std::istringstream input(query);
    std::string token;
    std::string translated;
    while (input >> token) {
        std::string escaped;
        escaped.reserve(token.size());
        for (const char character : token) {
            escaped.push_back(character);
            if (character == '"') {
                escaped.push_back('"');
            }
        }
        if (!translated.empty()) {
            translated.push_back(' ');
        }
        translated.push_back('"');
        translated += escaped;
        translated.push_back('"');
    }
    return translated;
}

void require_project_file(const qt::index::BackgroundService& service,
                          const std::string& file,
                          std::string_view tool_name) {
    const std::filesystem::path relative(file);
    if (relative.empty() || relative.is_absolute()) {
        throw std::invalid_argument(std::string(tool_name) +
                                    " requires a project-relative file");
    }
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || *normalized.begin() == ".." ||
        *normalized.begin() == ".git") {
        throw std::invalid_argument(std::string(tool_name) +
                                    " file must stay inside the project");
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(service.project_root() / normalized,
                                          error)) {
        throw std::invalid_argument(std::string(tool_name) +
                                    " file does not exist: " + file);
    }
}

qt::index::HttpRequest request_from_arguments(const Json& arguments) {
    qt::index::HttpRequest request;
    if (!arguments.is_object()) {
        return request;
    }
    for (const auto& item : arguments.items()) {
        const std::string text = argument_to_query_text(item.value());
        if (!text.empty() || item.value().is_string()) {
            request.query[item.key()] = text;
        }
    }
    return request;
}

std::unique_ptr<qt::index::BackgroundService> make_index_service(
    const qt::foundation::Path& root) {
    qt::index::ServiceOptions options;
    options.project_root = root;
    options.state_directory = root / ".index";
    options.compile_commands_output = root / "compile_commands.json";
    options.tree_sitter.output_directory = options.state_directory / "tree-sitter";
    return std::make_unique<qt::index::BackgroundService>(std::move(options));
}

bool index_tool_requires_scan(const std::string& name) {
    static const std::vector<std::string> scan_tools{
        "index_status",
        "index_metadata",
        "index_tree_sitter_failures",
        "index_project_summary",
        "index_memory_graph",
        "index_graph_schema",
        "index_search_graph",
        "index_trace_call_path",
        "index_dead_code",
        "index_code_snippet",
        "index_build_target_graph",
        "index_build_compile_flags",
        "index_build_include_graph",
        "index_build_compile_commands",
        "index_rtos_task_graph",
        "index_files",
        "index_file_info",
        "index_file_analysis",
        "index_symbol",
        "index_search",
        "index_fts_search",
        "index_ast",
        "index_classes",
        "index_functions",
        "index_callgraph",
        "index_read_context",
    };
    return std::find(scan_tools.begin(), scan_tools.end(), name) != scan_tools.end();
}

Json service_status_payload(const qt::index::BackgroundService& service,
                            const qt::index::HttpRequest& request) {
    Json status = Json::parse(qt::index::service_status_json(service.status()));
    if (!request.query.empty()) {
        status["query"] = request.query;
    }
    const auto tree_sitter = request.query.find("include_tree_sitter");
    if (tree_sitter != request.query.end() &&
        (tree_sitter->second == "false" || tree_sitter->second == "0")) {
        status.erase("tree_sitter");
    }
    if (request.query.find("include_build_metadata") == request.query.end()) {
        status.erase("build_metadata");
    }
    return status;
}

Json metadata_payload(const qt::index::BackgroundService& service,
                      const qt::index::HttpRequest& request) {
    Json payload = Json::object();
    if (request.query.empty() ||
        request.query.find("include_compile_commands") != request.query.end()) {
        payload["compile_commands"] =
            service.options().compile_commands_output.generic_string();
    }
    if (request.query.empty() ||
        request.query.find("include_tree_sitter") != request.query.end()) {
        payload["tree_sitter"] =
            service.options().tree_sitter.output_directory.generic_string();
    }
    if (request.query.empty() ||
        request.query.find("include_sqlite") != request.query.end()) {
        payload["sqlite"] = service.status().sqlite_database.generic_string();
    }
    if (!request.query.empty()) {
        payload["query"] = request.query;
    }
    return payload;
}

Json tree_sitter_failures_payload(const qt::index::BackgroundService& service,
                                  const qt::index::HttpRequest& request) {
    const auto status = service.status();
    const std::size_t limit = request.query.count("limit") == 0
                                  ? 200U
                                  : static_cast<std::size_t>(
                                        std::strtoull(request.query.at("limit").c_str(),
                                                     nullptr, 10));
    const auto path_filter = [&](const std::string& file) {
        const auto matches = [&](const char* key) {
            const auto found = request.query.find(key);
            return found == request.query.end() || found->second.empty() ||
                   file.rfind(found->second, 0U) == 0U;
        };
        const auto contains = request.query.find("path_contains");
        return matches("path") && matches("path_prefix") &&
               (contains == request.query.end() || contains->second.empty() ||
                file.find(contains->second) != std::string::npos);
    };

    Json failures = Json::array();
    std::size_t matched = 0U;
    for (const auto& file : status.tree_sitter.failed_files) {
        if (!path_filter(file)) {
            continue;
        }
        ++matched;
        if (failures.size() < limit) {
            failures.push_back(Json{{"file", file}});
        }
    }
    return Json{{"attempted", status.tree_sitter.attempted},
                {"available", status.tree_sitter.available},
                {"generated", status.tree_sitter.generated},
                {"mode", status.tree_sitter.mode},
                {"files_failed", status.tree_sitter.files_failed},
                {"files_skipped", status.tree_sitter.files_skipped},
                {"matched", matched},
                {"failures", std::move(failures)},
                {"skipped_files", status.tree_sitter.skipped_files},
                {"message", status.tree_sitter.message},
                {"query", request.query}};
}

Json call_index_payload(const std::string& name,
                          qt::index::BackgroundService& service,
                          const qt::index::HttpRequest& request) {
    if (name == "index_status") {
        return service_status_payload(service, request);
    }
    if (name == "index_rescan") {
        if (service.status().running) {
            service.request_scan();
        } else {
            service.run_once();
        }
        return Json{{"requested", true}, {"query", request.query}};
    }
    if (name == "index_metadata") {
        return metadata_payload(service, request);
    }
    if (name == "index_tree_sitter_failures") {
        return tree_sitter_failures_payload(service, request);
    }
    if (name == "index_project_summary") {
        return qt::index::project_summary_payload(service, request);
    }
    if (name == "index_memory_graph") {
        Json graph = qt::index::memory_graph_payload(service);
        const auto bool_query = [&](const char* key, bool fallback) {
            const auto found = request.query.find(key);
            return found == request.query.end() || found->second.empty()
                       ? fallback
                       : (found->second != "0" && found->second != "false");
        };
        if (!bool_query("include_nodes", true)) {
            graph.erase("nodes");
        }
        if (!bool_query("include_edges", true)) {
            graph.erase("edges");
        }
        if (!request.query.empty()) {
            graph["query"] = request.query;
        }
        return bool_query("persist", true) ? qt::index::persist_memory_graph(service, graph)
                                           : graph;
    }
    if (name == "index_graph_schema") {
        return qt::index::graph_schema_payload(service, request);
    }
    if (name == "index_search_graph") {
        return qt::index::search_graph_payload(service, request);
    }
    if (name == "index_trace_call_path") {
        return qt::index::trace_call_path_payload(service, request);
    }
    if (name == "index_dead_code") {
        return qt::index::dead_code_payload(service, request);
    }
    if (name == "index_code_snippet") {
        return qt::index::code_snippet_payload(service, request);
    }
    if (name == "index_fs_stat") {
        return qt::index::filesystem_stat_payload(service, request);
    }
    if (name == "index_fs_list") {
        return qt::index::filesystem_list_payload(service, request);
    }
    if (name == "index_fs_tree") {
        return qt::index::filesystem_tree_payload(service, request);
    }
    if (name == "index_fs_read") {
        return qt::index::filesystem_read_payload(service, request);
    }
    if (name == "index_fs_search") {
        return qt::index::filesystem_search_payload(service, request);
    }
    if (name == "index_git_status") {
        return qt::index::git_status_payload(service, request);
    }
    if (name == "index_git_log") {
        return qt::index::git_log_payload(service, request);
    }
    if (name == "index_git_diff") {
        return qt::index::git_diff_payload(service, request);
    }
    if (name == "index_git_show") {
        return qt::index::git_show_payload(service, request);
    }
    if (name == "index_git_blame") {
        return qt::index::git_blame_payload(service, request);
    }
    if (name == "index_git_branches") {
        return qt::index::git_branches_payload(service, request);
    }
    if (name == "index_serial_list") {
        return qt::index::serial_list_payload(request);
    }
    if (name == "index_serial_open") {
        return qt::index::serial_open_payload(request);
    }
    if (name == "index_serial_close") {
        return qt::index::serial_close_payload(request);
    }
    if (name == "index_serial_write") {
        return qt::index::serial_write_payload(request);
    }
    if (name == "index_serial_read") {
        return qt::index::serial_read_payload(request);
    }
    if (name == "index_serial_clear") {
        return qt::index::serial_clear_payload(request);
    }
    if (name == "index_serial_transact") {
        return qt::index::serial_transact_payload(request);
    }
    if (name == "index_debug_tools") {
        return qt::index::debug_tools_payload(request);
    }
    if (name == "index_openocd_command") {
        return qt::index::openocd_payload(request);
    }
    if (name == "index_gdb_command") {
        return qt::index::debug_gdb_command_payload(service, request);
    }
    if (name == "index_debug_breakpoint") {
        return qt::index::debug_breakpoint_payload(service, request);
    }
    if (name == "index_debug_memory") {
        return qt::index::debug_memory_payload(service, request);
    }
    if (name == "index_debug_registers") {
        return qt::index::debug_registers_payload(service, request);
    }
    if (name == "index_debug_backtrace") {
        return qt::index::debug_backtrace_payload(service, request);
    }
    if (name == "index_debug_step") {
        return qt::index::debug_step_payload(service, request);
    }
    if (name == "index_debug_rtos_tasks") {
        return qt::index::debug_rtos_tasks_payload(service, request);
    }
    if (name == "index_build_target_graph") {
        return qt::index::cmake_target_graph_payload(service, request);
    }
    if (name == "index_build_compile_flags") {
        return qt::index::cmake_compile_flags_payload(service, request);
    }
    if (name == "index_build_include_graph") {
        return qt::index::cmake_include_graph_payload(service, request);
    }
    if (name == "index_build_compile_commands") {
        return qt::index::cmake_compile_commands_payload(service, request);
    }
    if (name == "index_rtos_task_graph") {
        return qt::index::rtos_task_graph_payload(service, request);
    }
    if (name == "index_fetch") {
        return qt::index::fetch_payload(request);
    }
    if (name == "index_util_json_validate") {
        return qt::index::util_json_validate_payload(request);
    }
    if (name == "index_util_json_format") {
        return qt::index::util_json_format_payload(request);
    }
    if (name == "index_util_regex_test") {
        return qt::index::util_regex_test_payload(request);
    }
    if (name == "index_util_base64_encode") {
        return qt::index::util_base64_encode_payload(request);
    }
    if (name == "index_util_base64_decode") {
        return qt::index::util_base64_decode_payload(request);
    }
    if (name == "index_util_uuid") {
        return qt::index::util_uuid_payload(request);
    }
    if (name == "index_util_hash") {
        return qt::index::util_hash_payload(request);
    }
    if (name == "index_util_jwt_decode") {
        return qt::index::util_jwt_decode_payload(request);
    }
    if (name == "index_util_text_stats") {
        return qt::index::util_text_stats_payload(request);
    }
    if (name == "index_util_slugify") {
        return qt::index::util_slugify_payload(request);
    }
    if (name == "index_util_url_parse") {
        return qt::index::util_url_parse_payload(request);
    }
    if (name == "index_util_markdown_to_html") {
        return qt::index::util_markdown_to_html_payload(request);
    }
    if (name == "index_util_html_to_markdown") {
        return qt::index::util_html_to_markdown_payload(request);
    }
    if (name == "index_util_csv_to_json") {
        return qt::index::util_csv_to_json_payload(request);
    }
    if (name == "index_util_json_to_csv") {
        return qt::index::util_json_to_csv_payload(request);
    }
    if (name == "index_util_timestamp") {
        return qt::index::util_timestamp_payload(request);
    }
    if (name == "index_util_datetime_format") {
        return qt::index::util_datetime_format_payload(request);
    }
    if (name == "index_util_cron_next") {
        return qt::index::util_cron_next_payload(request);
    }
    if (name == "index_util_text_diff") {
        return qt::index::util_text_diff_payload(request);
    }
    if (name == "index_files") {
        return qt::index::files_payload(service, request);
    }
    if (name == "index_file_info") {
        return qt::index::file_info_payload(service, request);
    }
    if (name == "index_file_analysis") {
        return qt::index::file_analysis_payload(service, request);
    }
    if (name == "index_symbol") {
        return qt::index::symbol_payload(service, request);
    }
    if (name == "index_search") {
        return qt::index::search_payload(service, request);
    }
    if (name == "index_fts_search") {
        const auto query = request.query.count("query") == 0 ? std::string{} : request.query.at("query");
        const auto limit = request.query.count("limit") == 0
                               ? 100U
                               : static_cast<std::size_t>(
                                     std::strtoull(request.query.at("limit").c_str(), nullptr, 10));
        return qt::index::sqlite_fts_search_payload(service.status().sqlite_database,
                                                    service.project_root(),
                                                    query,
                                                    limit);
    }
    if (name == "index_ast") {
        return qt::index::ast_payload(service, request);
    }
    if (name == "index_classes") {
        return qt::index::classes_payload(service, request);
    }
    if (name == "index_functions") {
        return qt::index::functions_payload(service, request);
    }
    if (name == "index_callgraph") {
        return qt::index::callgraph_payload(service, request);
    }
    if (name == "index_read_context") {
        return qt::index::read_context_payload(service, request);
    }
    throw std::invalid_argument("unknown index tool: " + name);
}

struct IndexToolSpec {
    const char* category;
    const char* operation;
    const char* handler;
    const char* description;
    std::vector<const char*> parameters;
    std::vector<const char*> required;
};

const std::vector<IndexToolSpec>& index_tool_specs() {
    static const std::vector<IndexToolSpec> specs{
        {"core", "status", "index_status",
         "Return index scan, build metadata, and Tree-sitter status.",
         {"include_build_metadata", "include_tree_sitter"}, {}},
        {"core", "rescan", "index_rescan", "Run an immediate index rescan.",
         {"reason"}, {}},
        {"core", "metadata", "index_metadata",
         "Return generated index metadata locations.",
         {"include_compile_commands", "include_tree_sitter", "include_sqlite"}, {}},
        {"core", "tree_sitter_failures", "index_tree_sitter_failures",
         "List Tree-sitter parse failures.",
         {"path", "path_prefix", "path_contains", "limit"}, {}},
        {"core", "project_summary", "index_project_summary",
         "Return a ranked index project summary.", {"limit"}, {}},
        {"core", "memory_graph", "index_memory_graph",
         "Return and optionally persist the index memory graph.",
         {"persist", "include_nodes", "include_edges"}, {}},
        {"core", "graph_schema", "index_graph_schema",
         "Return index memory graph schema counts.", {"include_counts"}, {}},
        {"core", "search_graph", "index_search_graph",
         "Search index memory graph nodes.",
         {"label", "name_pattern", "file", "limit", "offset"}, {}},
        {"core", "trace_call_path", "index_trace_call_path",
         "Trace inbound, outbound, or bidirectional call paths.",
         {"function_name", "direction", "depth", "limit"}, {"function_name"}},
        {"core", "dead_code", "index_dead_code",
         "List indexed functions with no observed callers.", {"limit"}, {}},
        {"core", "code_snippet", "index_code_snippet",
         "Read source context for a function or method.",
         {"qualified_name", "before", "after"}, {"qualified_name"}},

        {"fs", "stat", "index_fs_stat",
         "Stat a project-relative filesystem path.", {"path"}, {}},
        {"fs", "list", "index_fs_list", "List a project-relative directory.",
         {"path", "recursive", "depth", "limit"}, {}},
        {"fs", "tree", "index_fs_tree",
         "Return a bounded recursive project filesystem tree.",
         {"path", "depth", "limit"}, {}},
        {"fs", "read", "index_fs_read", "Read project-relative file lines.",
         {"file", "start_line", "end_line", "max_lines"}, {"file"}},
        {"fs", "search", "index_fs_search",
         "Search project-relative file paths by substring.",
         {"pattern", "path", "limit"}, {"pattern"}},

        {"git", "status", "index_git_status", "Return read-only git status.",
         {"short", "limit"}, {}},
        {"git", "log", "index_git_log", "Return recent git commits.",
         {"limit", "file"}, {}},
        {"git", "diff", "index_git_diff", "Return git diff output.",
         {"file", "staged", "stat", "limit"}, {}},
        {"git", "show", "index_git_show",
         "Show a read-only git revision summary.", {"rev", "limit"}, {}},
        {"git", "blame", "index_git_blame",
         "Return git blame lines for a file range.",
         {"file", "start_line", "end_line", "limit"}, {"file"}},
        {"git", "branches", "index_git_branches", "List git branches.",
         {"all"}, {}},

        {"serial", "list", "index_serial_list",
         "List likely local Serial/UART device paths.", {"pattern", "limit"}, {}},
        {"serial", "open", "index_serial_open", "Open a Serial/UART session.",
         {"port", "baud", "session"}, {"port"}},
        {"serial", "close", "index_serial_close",
         "Close a Serial/UART session.", {"session"}, {"session"}},
        {"serial", "write", "index_serial_write",
         "Write bytes or commands to a Serial/UART session.",
         {"session", "data", "newline"}, {"session", "data"}},
        {"serial", "read", "index_serial_read",
         "Read buffered Serial/UART logs.",
         {"session", "offset", "max_bytes"}, {"session"}},
        {"serial", "clear", "index_serial_clear",
         "Clear buffered Serial/UART logs.", {"session"}, {"session"}},
        {"serial", "transact", "index_serial_transact",
         "Write to Serial/UART and read resulting output.",
         {"session", "data", "wait_ms"}, {"session", "data"}},

        {"debug", "tools", "index_debug_tools",
         "Report local GDB/OpenOCD executable availability.", {"gdb", "openocd"}, {}},
        {"debug", "openocd_command", "index_openocd_command",
         "Run or dry-run an OpenOCD command.",
         {"command", "interface", "target_cfg", "config", "dry_run"}, {}},
        {"debug", "gdb_command", "index_gdb_command",
         "Run or dry-run a GDB command.",
         {"command", "target", "executable", "dry_run"}, {"command"}},
        {"debug", "breakpoint", "index_debug_breakpoint",
         "Create a GDB breakpoint and list breakpoints.",
         {"symbol", "address", "target", "executable", "dry_run"}, {}},
        {"debug", "memory", "index_debug_memory",
         "Read target memory through GDB.",
         {"address", "count", "format", "target", "dry_run"}, {"address"}},
        {"debug", "registers", "index_debug_registers",
         "Inspect target registers through GDB.", {"group", "target", "dry_run"}, {}},
        {"debug", "backtrace", "index_debug_backtrace",
         "Capture a GDB backtrace.", {"frames", "target", "dry_run"}, {}},
        {"debug", "step", "index_debug_step",
         "Step target execution through GDB.", {"kind", "target", "dry_run"}, {}},
        {"debug", "rtos_tasks", "index_debug_rtos_tasks",
         "Inspect RTOS-like task/thread state through GDB.",
         {"target", "executable", "dry_run"}, {}},

        {"build", "target_graph", "index_build_target_graph",
         "Return a best-effort CMake/Ninja target graph.", {"limit"}, {}},
        {"build", "compile_flags", "index_build_compile_flags",
         "Return compile flags from compile_commands.json.", {"file", "limit"}, {}},
        {"build", "include_graph", "index_build_include_graph",
         "Return source-to-header include edges.", {"file", "limit"}, {}},
        {"build", "compile_commands", "index_build_compile_commands",
         "Return generated compile_commands entries.", {"file", "limit"}, {}},

        {"rtos", "task_graph", "index_rtos_task_graph",
         "Extract a static RTOS graph.", {"file", "limit"}, {}},
        {"fetch", "url", "index_fetch",
         "Fetch a URL and return simplified or raw content.",
         {"url", "max_length", "start_index", "raw"}, {"url"}},

        {"util", "json_validate", "index_util_json_validate",
         "Validate JSON text.", {"json", "text"}, {"json"}},
        {"util", "json_format", "index_util_json_format",
         "Format JSON text.", {"json", "text", "indent"}, {"json"}},
        {"util", "regex_test", "index_util_regex_test",
         "Run a regular expression against text.",
         {"pattern", "text", "flags", "limit"}, {"pattern"}},
        {"util", "base64_encode", "index_util_base64_encode",
         "Base64-encode text or data.", {"text", "data"}, {}},
        {"util", "base64_decode", "index_util_base64_decode",
         "Base64-decode text or data.", {"text", "data"}, {"text"}},
        {"util", "uuid", "index_util_uuid", "Generate UUID values.",
         {"count"}, {}},
        {"util", "hash", "index_util_hash",
         "Hash text using common digest algorithms.",
         {"text", "data", "algorithm"}, {}},
        {"util", "jwt_decode", "index_util_jwt_decode",
         "Decode a JWT header and payload without verifying.", {"token"}, {"token"}},
        {"util", "text_stats", "index_util_text_stats",
         "Return byte, word, line, and paragraph counts.", {"text"}, {}},
        {"util", "slugify", "index_util_slugify",
         "Convert text into a URL-safe slug.", {"text", "max_length"}, {}},
        {"util", "url_parse", "index_util_url_parse",
         "Parse a URL into components.", {"url"}, {"url"}},
        {"util", "markdown_to_html", "index_util_markdown_to_html",
         "Convert basic markdown to HTML.", {"markdown", "text"}, {}},
        {"util", "html_to_markdown", "index_util_html_to_markdown",
         "Convert HTML into simplified markdown text.", {"html", "text"}, {}},
        {"util", "csv_to_json", "index_util_csv_to_json",
         "Convert CSV text into JSON rows.", {"csv", "text", "header"}, {}},
        {"util", "json_to_csv", "index_util_json_to_csv",
         "Convert JSON arrays into CSV text.", {"json"}, {"json"}},
        {"util", "timestamp", "index_util_timestamp",
         "Format a Unix timestamp as UTC datetime.", {"timestamp", "format"}, {}},
        {"util", "datetime_format", "index_util_datetime_format",
         "Format parsed datetimes.",
         {"datetime", "input_format", "output_format"}, {"datetime"}},
        {"util", "cron_next", "index_util_cron_next",
         "Compute the next UTC time for a cron expression.",
         {"expression", "cron", "from_timestamp"}, {"expression"}},
        {"util", "text_diff", "index_util_text_diff",
         "Return a simple line-oriented text diff.", {"before", "after", "a", "b"}, {}},

        {"code", "files", "index_files", "List scanned project files.",
         {"kind", "path", "path_prefix", "path_contains", "extension", "limit"}, {}},
        {"code", "file_info", "index_file_info",
         "Return metadata for one project-relative file.", {"file"}, {"file"}},
        {"code", "file_analysis", "index_file_analysis",
         "Analyze one file with symbols and call edges.", {"file", "limit"}, {"file"}},
        {"code", "symbol", "index_symbol",
         "Find classes, methods, or functions by name.",
         {"name", "file", "path", "path_prefix", "path_contains", "kind",
          "context", "before", "after", "limit"},
         {"name"}},
        {"code", "search", "index_search", "Search project text files.",
         {"pattern", "file", "path", "path_prefix", "path_contains", "kind",
          "before", "after", "limit"},
         {"pattern"}},
        {"code", "fts_search", "index_fts_search",
         "Search the persisted SQLite FTS index.", {"query", "limit"}, {"query"}},
        {"code", "ast", "index_ast",
         "Return generated Tree-sitter AST text for a file.",
         {"file", "max_lines"}, {"file"}},
        {"code", "classes", "index_classes", "List indexed C++ classes.",
         {"name", "qualified_name", "file", "path", "path_prefix", "path_contains",
          "limit"},
         {}},
        {"code", "functions", "index_functions",
         "List indexed C/C++ functions and methods.",
         {"name", "qualified_name", "kind", "file", "path", "path_prefix",
          "path_contains", "limit"},
         {}},
        {"code", "callgraph", "index_callgraph",
         "List approximate C/C++ call graph edges.",
         {"caller", "callee", "file", "path", "path_prefix", "path_contains",
          "limit"},
         {}},
        {"code", "read_context", "index_read_context",
         "Read source lines around a file and line number.",
         {"file", "line", "before", "after"}, {"file", "line"}},
    };
    return specs;
}

Json object_schema(Json properties, Json required = Json::array()) {
    return Json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)},
                {"additionalProperties", false}};
}

Json parameter_schema(const std::vector<const char*>& parameters,
                      const std::vector<const char*>& required = {}) {
    Json properties = Json::object();
    for (const char* parameter : parameters) {
        const std::string name(parameter);
        const bool boolean =
            name == "context" || name == "raw" || name == "dry_run" ||
            name == "recursive" || name == "include_hidden" ||
            name == "include_nodes" || name == "include_edges" ||
            name == "persist" || name == "include_tree_sitter" ||
            name == "include_build_metadata" ||
            name == "include_compile_commands" || name == "include_sqlite" ||
            name == "header";
        const bool number =
            name == "limit" || name == "before" || name == "after" ||
            name == "line" || name == "depth" || name == "max_depth" ||
            name == "max_lines" || name == "max_length" ||
            name == "start_index" || name == "count" ||
            name == "wait_ms" || name == "frames" || name == "indent" ||
            name == "timestamp" || name == "from_timestamp";
        std::string description = "Value for " + name + ".";
        if (name == "name") {
            description =
                "C/C++ class, function, method, or qualified symbol name.";
        } else if (name == "pattern") {
            description = "Text or regular-expression search pattern.";
        } else if (name == "file") {
            description = "Project-relative file path.";
        } else if (name == "context") {
            description = "Include source context around each symbol match.";
        } else if (name == "limit") {
            description = "Maximum number of results to return.";
        }
        properties[parameter] =
            Json{{"type", boolean ? "boolean" : number ? "integer" : "string"},
                 {"description", std::move(description)}};
    }
    Json required_json = Json::array();
    for (const char* parameter : required) {
        required_json.push_back(parameter);
    }
    return object_schema(std::move(properties), std::move(required_json));
}

Json index_router_schema() {
    return object_schema(
        Json{{"action",
              Json{{"type", "string"},
                   {"enum", Json::array({"tree", "call"})},
                   {"description",
                    "Use tree to discover grouped index operations; use call to run one."}}},
             {"category",
              Json{{"type", "string"},
                   {"description",
                    "Optional operation group such as code, fs, git, build, debug, util."}}},
             {"operation",
              Json{{"type", "string"},
                   {"description",
                    "Operation name, or category.operation when category is omitted. "
                    "For code navigation use code.symbol, code.functions, "
                    "code.classes, code.callgraph, or code.read_context."}}},
             {"arguments",
              Json{{"type", "object"},
                   {"description", "Arguments forwarded to the selected operation."}}},
             {"include_schemas",
              Json{{"type", "boolean"},
                   {"description", "Include per-operation JSON schemas in tree output."}}}},
        Json::array({"action"}));
}

std::string public_operation_name(const IndexToolSpec& spec) {
    if (std::string_view(spec.category) == "core") {
        return spec.operation;
    }
    return std::string(spec.category) + "." + spec.operation;
}

Json string_array(const std::vector<const char*>& values) {
    Json result = Json::array();
    for (const char* value : values) {
        result.push_back(value);
    }
    return result;
}

Json spec_summary(const IndexToolSpec& spec, bool include_schema) {
    Json summary{{"name", spec.operation},
                 {"operation", public_operation_name(spec)},
                 {"description", spec.description},
                 {"parameters", string_array(spec.parameters)},
                 {"required", string_array(spec.required)}};
    if (include_schema) {
        summary["input_schema"] = parameter_schema(spec.parameters, spec.required);
    }
    return summary;
}

const IndexToolSpec* find_index_spec(const std::string& category,
                                         const std::string& operation) {
    for (const auto& spec : index_tool_specs()) {
        if (operation == public_operation_name(spec) ||
            (!category.empty() && operation == spec.operation)) {
            if (category.empty() || category == spec.category ||
                (category == "core" && std::string_view(spec.category) == "core")) {
                return &spec;
            }
        }
    }
    return nullptr;
}

class IndexRouterTool final : public Tool {
  public:
    IndexRouterTool() {
        descriptor_.name = "index";
        descriptor_.description =
            "Discover and invoke grouped project index, code, filesystem, git, "
            "debug, build, serial, fetch, RTOS, and utility operations. Prefer the "
            "direct symbol tool for C/C++ definitions. For other calls use, for "
            "example, action=call, operation=code.callgraph, arguments={...}; use "
            "action=tree only when the operation is unknown.";
        descriptor_.input_schema = index_router_schema();
        descriptor_.deterministic = true;
        descriptor_.destructive = false;
        descriptor_.local_only = true;
    }

    const ToolDescriptor& descriptor() const override { return descriptor_; }

    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override {
        if (!is_valid(request)) {
            return make_invalid_request("invalid tool request", request);
        }
        if (request.tool_name != descriptor_.name) {
            return make_invalid_request("tool request name does not match index",
                                        request);
        }

        const auto action = require_string_argument(request, "action", request.tool_name);
        if (!action) {
            return make_invalid_request(action.error().summary(), request);
        }

        try {
            if (action.value() == "tree") {
                return make_success(request, tree_payload(request));
            }
            if (action.value() != "call") {
                return make_invalid_request("index action must be tree or call",
                                            request);
            }

            const auto operation =
                require_string_argument(request, "operation", request.tool_name);
            if (!operation) {
                return make_invalid_request(operation.error().summary(), request);
            }
            const std::string category =
                optional_string_argument(request, "category");
            const IndexToolSpec* spec =
                find_index_spec(category, operation.value());
            if (spec == nullptr) {
                return make_invalid_request("unknown index operation: " +
                                                operation.value(),
                                            request);
            }

            Json forwarded_arguments = Json::object();
            if (request.arguments.contains("arguments")) {
                forwarded_arguments = request.arguments.at("arguments");
                if (!forwarded_arguments.is_object()) {
                    return make_invalid_request(
                        "index arguments must be an object", request);
                }
            }
            qt::ai::ToolDefinition operation_definition{
                public_operation_name(*spec), spec->description,
                parameter_schema(spec->parameters, spec->required)};
            qt::ai::ToolCallContent operation_call;
            operation_call.name = public_operation_name(*spec);
            operation_call.arguments = forwarded_arguments;
            const auto validated = qt::ai::validate_tool_arguments(
                operation_definition, operation_call);
            if (!validated) {
                auto result = make_invalid_request(validated.error().message,
                                                   request);
                result.output =
                    Json{{"code", "invalid_index_arguments"},
                         {"operation", public_operation_name(*spec)},
                         {"retryable", true},
                         {"expected_schema",
                          operation_definition.input_schema},
                         {"received_arguments", forwarded_arguments}};
                return result;
            }
            forwarded_arguments = validated.value();

            const auto root = workspace_root_from_context(context);
            if (!root) {
                return make_execution_error(ToolResultStatus::execution_error,
                                            root.error().summary(), request);
            }

            std::unique_ptr<qt::index::BackgroundService> local_service;
            qt::index::BackgroundService* service = nullptr;
            const auto context_service = context.index_service;
            if (context_service &&
                std::filesystem::absolute(context_service->project_root()) ==
                    std::filesystem::absolute(root.value())) {
                service = context_service.get();
            } else {
                local_service = make_index_service(root.value());
                service = local_service.get();
            }
            if (index_tool_requires_scan(spec->handler)) {
                const auto status = service->status();
                if (status.running) {
                    if (status.generation == 0U && !status.scan_active) {
                        service->request_scan();
                    }
                } else {
                    service->run_once();
                }
            }

            Json output = call_index_payload(
                spec->handler, *service, request_from_arguments(forwarded_arguments));
            return make_success(
                request,
                Json{{"category", spec->category},
                     {"operation", public_operation_name(*spec)},
                     {"output", std::move(output)}});
        } catch (const std::invalid_argument& error) {
            return make_invalid_request(error.what(), request);
        } catch (const std::exception& error) {
            return make_exception_result(request, error);
        }
    }

  private:
    static Json tree_payload(const ToolRequest& request) {
        const std::string category = optional_string_argument(request, "category");
        const bool include_schema =
            request.arguments.value("include_schemas", false);

        Json categories = Json::array();
        std::map<std::string, Json> by_category;
        for (const auto& spec : index_tool_specs()) {
            if (!category.empty() && category != spec.category) {
                continue;
            }
            auto& entry = by_category[spec.category];
            if (entry.is_null()) {
                entry = Json{{"name", spec.category}, {"operations", Json::array()}};
            }
            entry["operations"].push_back(spec_summary(spec, include_schema));
        }

        for (auto& item : by_category) {
            categories.push_back(std::move(item.second));
        }

        return Json{{"usage",
                     Json{{"discover_all",
                           Json{{"action", "tree"},
                                {"include_schemas", false}}},
                          {"discover_category",
                           Json{{"action", "tree"}, {"category", "code"}}},
                          {"call",
                           Json{{"action", "call"},
                                {"operation", "fs.read"},
                                {"arguments", Json{{"file", "PLAN.md"}}}}}}},
                    {"categories", std::move(categories)}};
    }

    ToolDescriptor descriptor_;
};

using DirectIndexPayload =
    std::function<Json(qt::index::BackgroundService&,
                       const qt::index::HttpRequest&)>;

ToolResult invoke_direct_index_payload(const ToolRequest& request,
                                       const ToolContext& context,
                                       std::string_view expected_name,
                                       const DirectIndexPayload& payload) {
    if (!is_valid(request) || request.tool_name != expected_name) {
        return make_invalid_request(
            "tool request name does not match " + std::string(expected_name),
            request);
    }
    const auto root = workspace_root_from_context(context);
    if (!root) {
        return make_execution_error(ToolResultStatus::execution_error,
                                    root.error().summary(), request);
    }

    std::unique_ptr<qt::index::BackgroundService> local_service;
    qt::index::BackgroundService* service = nullptr;
    if (context.index_service &&
        std::filesystem::absolute(context.index_service->project_root()) ==
            std::filesystem::absolute(root.value())) {
        service = context.index_service.get();
    } else {
        local_service = make_index_service(root.value());
        service = local_service.get();
    }

    auto status = service->status();
    if (status.generation == 0U && status.running) {
        service->request_scan();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (status.generation == 0U &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            status = service->status();
        }
    } else if (status.generation == 0U) {
        try {
            service->run_once();
        } catch (const std::exception& error) {
            return make_exception_result(request, error);
        }
        status = service->status();
    }
    if (status.generation == 0U) {
        auto result = make_execution_error(
            ToolResultStatus::execution_error,
            "project code index is still building; retry " +
                std::string(expected_name),
            request);
        result.output = Json{{"code", "index_not_ready"},
                             {"retryable", true},
                             {"scan_active", status.scan_active},
                             {"scan_phase", status.scan_phase},
                             {"scan_progress_percent",
                              status.scan_progress_percent},
                             {"last_error", status.last_error}};
        return result;
    }

    try {
        Json output =
            payload(*service, request_from_arguments(request.arguments));
        status = service->status();
        if (output.is_object()) {
            output["index"] = Json{{"generation", status.generation},
                                   {"stale", status.index_stale},
                                   {"snapshot_loaded", status.snapshot_loaded}};
        }
        auto result = make_success(request, std::move(output));
        apply_output_guards(result, output_guard_options_from_context(context));
        return result;
    } catch (const std::invalid_argument& error) {
        return make_invalid_request(error.what(), request);
    } catch (const std::exception& error) {
        return make_exception_result(request, error);
    }
}

class DirectIndexTool final : public Tool {
  public:
    DirectIndexTool(std::string name, std::string description, Json schema,
                    DirectIndexPayload payload)
        : payload_(std::move(payload)) {
        descriptor_.name = std::move(name);
        descriptor_.description = std::move(description);
        descriptor_.input_schema = std::move(schema);
        descriptor_.deterministic = true;
        descriptor_.destructive = false;
        descriptor_.local_only = true;
    }

    const ToolDescriptor& descriptor() const override { return descriptor_; }

    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override {
        qt::ai::ToolDefinition definition{descriptor_.name,
                                          descriptor_.description,
                                          descriptor_.input_schema};
        qt::ai::ToolCallContent call;
        call.id = request.call_id.value_or("");
        call.name = request.tool_name;
        call.arguments = request.arguments;
        const auto validated = qt::ai::validate_tool_arguments(definition, call);
        if (!validated) {
            auto result = make_invalid_request(validated.error().message, request);
            result.output = Json{{"code", "invalid_arguments"},
                                 {"tool", descriptor_.name},
                                 {"retryable", true},
                                 {"received_arguments", request.arguments},
                                 {"expected_schema", descriptor_.input_schema}};
            return result;
        }
        ToolRequest prepared = request;
        prepared.arguments = validated.value();
        return invoke_direct_index_payload(prepared, context, descriptor_.name,
                                           payload_);
    }

  private:
    ToolDescriptor descriptor_;
    DirectIndexPayload payload_;
};

class CodeSymbolTool final : public Tool {
  public:
    CodeSymbolTool() {
        descriptor_.name = "symbol";
        descriptor_.description =
            "Find C/C++ class, function, or method definitions by identifier "
            "using the project code index. Prefer this over grep for symbol "
            "lookup. Example: {\"name\":\"ToolRegistry::prepare_request\","
            "\"context\":true}.";
        descriptor_.input_schema = object_schema(
            Json{{"name",
                  Json{{"type", "string"},
                       {"minLength", 1},
                       {"description",
                        "C/C++ identifier or qualified symbol name."}}},
                 {"file",
                  Json{{"type", "string"},
                       {"description", "Optional project-relative file filter."}}},
                 {"kind",
                  Json{{"type", "string"},
                       {"description",
                        "Optional symbol kind filter such as function or method."}}},
                 {"context",
                  Json{{"type", "boolean"},
                       {"description",
                        "Include source lines around each match."}}},
                 {"before",
                  Json{{"type", "integer"},
                       {"minimum", 0},
                       {"description", "Context lines before each match."}}},
                 {"after",
                  Json{{"type", "integer"},
                       {"minimum", 0},
                       {"description", "Context lines after each match."}}},
                 {"limit",
                  Json{{"type", "integer"},
                       {"minimum", 1},
                       {"maximum", 100},
                       {"description", "Maximum matching symbols."}}}},
            Json::array({"name"}));
        descriptor_.deterministic = true;
        descriptor_.destructive = false;
        descriptor_.local_only = true;
    }

    const ToolDescriptor& descriptor() const override { return descriptor_; }

    ToolResult invoke(const ToolRequest& request,
                      const ToolContext& context) const override {
        if (!is_valid(request) || request.tool_name != descriptor_.name) {
            return make_invalid_request(
                "tool request name does not match symbol", request);
        }
        const auto name =
            require_string_argument(request, "name", descriptor_.name);
        if (!name || name.value().empty()) {
            return make_invalid_request(
                name ? "symbol name must not be empty" : name.error().summary(),
                request);
        }
        return invoke_direct_index_payload(
            request, context, descriptor_.name,
            [](qt::index::BackgroundService& service,
               const qt::index::HttpRequest& index_request) {
                return call_index_payload("index_symbol", service,
                                          index_request);
            });
    }

  private:
    ToolDescriptor descriptor_;
};

}  // namespace

void register_index_tools(ToolRegistry& registry) {
    register_index_tools(registry, [](const std::string&) { return true; });
}

void register_index_tools(
    ToolRegistry& registry,
    const std::function<bool(const std::string&)>& enabled) {
    if (enabled("index")) {
        registry.register_tool(std::make_unique<IndexRouterTool>());
    }
    if (enabled("symbol")) {
        registry.register_tool(std::make_unique<CodeSymbolTool>());
    }
    if (enabled("analyze_file")) {
        registry.register_tool(std::make_unique<DirectIndexTool>(
            "analyze_file",
            "Analyze one project-relative source file with the code index. "
            "Returns its C/C++ classes, functions, line statistics, and "
            "approximate incoming and outgoing call edges.",
            object_schema(
                Json{{"file",
                      Json{{"type", "string"},
                           {"minLength", 1},
                           {"description", "Project-relative source file."}}},
                     {"limit",
                      Json{{"type", "integer"},
                           {"minimum", 1},
                           {"maximum", 100},
                           {"description",
                            "Maximum incoming and outgoing call edges."}}}},
                Json::array({"file"})),
            [](qt::index::BackgroundService& service,
               const qt::index::HttpRequest& request) {
                require_project_file(service, request.query.at("file"),
                                     "analyze_file");
                qt::index::HttpRequest bounded = request;
                if (bounded.query.find("limit") == bounded.query.end()) {
                    bounded.query["limit"] = "100";
                }
                Json output = call_index_payload("index_file_analysis", service,
                                                 bounded);
                output["call_graph_approximate"] = true;
                return output;
            }));
    }
    if (enabled("references")) {
        registry.register_tool(std::make_unique<DirectIndexTool>(
            "references",
            "Trace approximate indexed C/C++ callers and callees for a "
            "function or method. Prefer this over text search for call impact "
            "analysis, then verify important edges in source.",
            object_schema(
                Json{{"symbol",
                      Json{{"type", "string"},
                           {"minLength", 1},
                           {"description",
                            "Function, method, or qualified symbol name."}}},
                     {"direction",
                      Json{{"type", "string"},
                           {"enum", Json::array({"inbound", "outbound", "both"})},
                           {"description",
                            "Trace callers, callees, or both. Defaults to both."}}},
                     {"depth",
                      Json{{"type", "integer"},
                           {"minimum", 1},
                           {"maximum", 5},
                           {"description", "Maximum call traversal depth."}}},
                     {"limit",
                      Json{{"type", "integer"},
                           {"minimum", 1},
                           {"maximum", 100},
                           {"description", "Maximum call-path edges."}}}},
                Json::array({"symbol"})),
            [](qt::index::BackgroundService& service,
               const qt::index::HttpRequest& request) {
                qt::index::HttpRequest translated = request;
                translated.query["function_name"] = translated.query.at("symbol");
                translated.query.erase("symbol");
                if (translated.query.find("direction") == translated.query.end()) {
                    translated.query["direction"] = "both";
                }
                if (translated.query.find("depth") == translated.query.end()) {
                    translated.query["depth"] = "3";
                }
                if (translated.query.find("limit") == translated.query.end()) {
                    translated.query["limit"] = "100";
                }
                Json output = call_index_payload("index_trace_call_path", service,
                                                 translated);
                output["approximate"] = true;
                output["verification"] =
                    "Verify material call edges against current source.";
                return output;
            }));
    }
    if (enabled("build_context")) {
        registry.register_tool(std::make_unique<DirectIndexTool>(
            "build_context",
            "Return indexed compiler and include context for one "
            "project-relative C/C++ file, including compile flags, defines, "
            "include paths, standards, and resolved include edges.",
            object_schema(
                Json{{"file",
                      Json{{"type", "string"},
                           {"minLength", 1},
                           {"description",
                            "Project-relative C/C++ source or header file."}}},
                     {"limit",
                      Json{{"type", "integer"},
                           {"minimum", 1},
                           {"maximum", 100},
                           {"description", "Maximum entries or include edges."}}}},
                Json::array({"file"})),
            [](qt::index::BackgroundService& service,
               const qt::index::HttpRequest& request) {
                require_project_file(service, request.query.at("file"),
                                     "build_context");
                const std::size_t limit = request.query.count("limit") == 0U
                                              ? 100U
                                              : static_cast<std::size_t>(
                                                    std::stoull(
                                                        request.query.at("limit")));
                qt::index::HttpRequest unbounded = request;
                unbounded.query["limit"] = "0";
                Json compile_flags = call_index_payload(
                    "index_build_compile_flags", service, unbounded);
                Json include_graph = call_index_payload(
                    "index_build_include_graph", service, unbounded);
                const std::string file = request.query.at("file");
                const auto filter_and_limit =
                    [&](Json& values, const char* field) {
                        values.erase(
                            std::remove_if(
                                values.begin(), values.end(),
                                [&](const Json& value) {
                                    return value.value(field, std::string{}) != file;
                                }),
                            values.end());
                        const bool truncated = values.size() > limit;
                        if (truncated) {
                            values.erase(
                                values.begin() +
                                    static_cast<Json::difference_type>(limit),
                                values.end());
                        }
                        return truncated;
                    };
                bool compile_flags_truncated = false;
                if (compile_flags.contains("entries") &&
                    compile_flags.at("entries").is_array()) {
                    compile_flags_truncated =
                        filter_and_limit(compile_flags["entries"], "file");
                }
                bool include_graph_truncated = false;
                if (include_graph.contains("edges") &&
                    include_graph.at("edges").is_array()) {
                    include_graph_truncated =
                        filter_and_limit(include_graph["edges"], "from");
                }
                compile_flags["truncated"] = compile_flags_truncated;
                include_graph["truncated"] = include_graph_truncated;
                compile_flags["query"] = request.query;
                include_graph["query"] = request.query;
                return Json{{"file", file},
                            {"compile_flags", std::move(compile_flags)},
                            {"include_graph", std::move(include_graph)}};
            }));
    }
    if (enabled("search_index")) {
        registry.register_tool(std::make_unique<DirectIndexTool>(
            "search_index",
            "Search the persisted project full-text index. Use for broad "
            "repository concepts and known text; use symbol for C/C++ "
            "identifiers and grep for regexes or freshly changed text.",
            object_schema(
                Json{{"query",
                      Json{{"type", "string"},
                           {"minLength", 1},
                           {"description",
                            "Search terms, or an FTS5 expression when syntax is "
                            "fts5."}}},
                     {"syntax",
                      Json{{"type", "string"},
                           {"enum", Json::array({"plain", "fts5"})},
                           {"description",
                            "plain safely searches all whitespace-separated "
                            "terms and is the default; fts5 accepts raw MATCH "
                            "syntax."}}},
                     {"limit",
                      Json{{"type", "integer"},
                           {"minimum", 1},
                           {"maximum", 100},
                           {"description", "Maximum indexed matches."}}}},
                Json::array({"query"})),
            [](qt::index::BackgroundService& service,
               const qt::index::HttpRequest& request) {
                qt::index::HttpRequest translated = request;
                const std::string syntax = translated.query.count("syntax") == 0U
                                               ? "plain"
                                               : translated.query.at("syntax");
                translated.query.erase("syntax");
                if (syntax == "plain") {
                    translated.query["query"] =
                        plain_fts_query(translated.query.at("query"));
                }
                if (translated.query.find("limit") == translated.query.end()) {
                    translated.query["limit"] = "50";
                }
                if (!service.status().sqlite_fts_available) {
                    throw std::runtime_error(
                        "persisted full-text index is unavailable; request an "
                        "index rescan before retrying search_index");
                }
                Json output;
                try {
                    output = call_index_payload("index_fts_search", service,
                                                translated);
                } catch (const std::runtime_error& error) {
                    if (syntax == "fts5") {
                        throw std::invalid_argument(
                            std::string("invalid FTS5 query: ") + error.what());
                    }
                    throw;
                }
                output["query"] = request.query.at("query");
                output["syntax"] = syntax;
                if (service.status().index_stale) {
                    output["snapshot_warning"] =
                        "Results may lag behind files changed since the last scan.";
                }
                return output;
            }));
    }
}

}  // namespace qt::tools
