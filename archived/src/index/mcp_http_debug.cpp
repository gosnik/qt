#include "index/mcp_http_debug.hpp"

#include <nlohmann/json.hpp>

#include <boost/process.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

std::filesystem::path relative_project_path(const std::string& value) {
    const auto relative = std::filesystem::path{value}.lexically_normal();
    if (relative.empty() || relative.is_absolute()) {
        throw std::invalid_argument("file must be a relative path inside the project");
    }
    for (const auto& part : relative) {
        if (part == "..") {
            throw std::invalid_argument("file must not contain '..'");
        }
        if (part == ".git") {
            throw std::invalid_argument("path must not reference .git");
        }
    }
    return relative;
}

std::filesystem::path checked_project_path(const BackgroundService& service,
                                           const std::filesystem::path& relative) {
    const auto root = std::filesystem::weakly_canonical(service.project_root());
    const auto full = std::filesystem::weakly_canonical((root / relative).lexically_normal());
    const auto root_text = root.generic_string();
    const auto full_text = full.generic_string();
    if (full_text != root_text && full_text.rfind(root_text + "/", 0) != 0) {
        throw std::invalid_argument("path escapes project root");
    }
    return full;
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

std::filesystem::path requested_tool_path(const HttpRequest& request,
                                          const std::string& key,
                                          const std::vector<std::string>& defaults) {
    auto names = defaults;
    const auto requested = query_value(request, key);
    if (!requested.empty()) {
        if (requested.find('/') != std::string::npos || requested.front() == '-') {
            throw std::invalid_argument(key + " must be a PATH executable name");
        }
        names.insert(names.begin(), requested);
    }
    return executable_on_path(names);
}

json debug_command_payload(const std::filesystem::path& executable,
                           const std::vector<std::string>& args,
                           const HttpRequest& request) {
    json planned_args = json::array();
    for (const auto& arg : args) {
        planned_args.push_back(arg);
    }
    json payload{{"available", !executable.empty()},
                 {"executable", executable.empty() ? "" : executable.string()},
                 {"args", planned_args},
                 {"dry_run", query_bool(request, "dry_run", false)},
                 {"query", request.query}};
    if (query_bool(request, "dry_run", false)) {
        return payload;
    }
    if (executable.empty()) {
        payload["exit_code"] = -1;
        payload["stderr"] = json::array({"debug executable not found on PATH"});
        payload["stdout"] = json::array();
        return payload;
    }

    bp::ipstream stdout_stream;
    bp::ipstream stderr_stream;
    bp::child child(bp::exe = executable.string(),
                    bp::args = args,
                    bp::std_out > stdout_stream,
                    bp::std_err > stderr_stream);

    json stdout_lines = json::array();
    json stderr_lines = json::array();
    const auto limit = query_size(request, "limit", 300);
    const auto timeout_ms = std::min<std::size_t>(query_size(request, "timeout_ms", 5000), 60000);
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    auto read_stdout = std::async(std::launch::async, [&]() {
        std::string line;
        while (std::getline(stdout_stream, line)) {
            if (limit != 0 && stdout_lines.size() >= limit) {
                stdout_truncated = true;
                continue;
            }
            stdout_lines.push_back(line);
        }
    });
    auto read_stderr = std::async(std::launch::async, [&]() {
        std::string line;
        while (std::getline(stderr_stream, line)) {
            if (limit != 0 && stderr_lines.size() >= limit) {
                stderr_truncated = true;
                continue;
            }
            stderr_lines.push_back(line);
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool timed_out = false;
    while (child.running()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            child.terminate();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    child.wait();
    read_stdout.wait();
    read_stderr.wait();
    payload["exit_code"] = child.exit_code();
    payload["stdout"] = stdout_lines;
    payload["stderr"] = stderr_lines;
    payload["timed_out"] = timed_out;
    payload["truncated"] = stdout_truncated || stderr_truncated;
    return payload;
}

std::filesystem::path optional_debug_executable(const BackgroundService& service, const HttpRequest& request) {
    const auto value = query_value(request, "executable");
    if (value.empty()) {
        return {};
    }
    const auto relative = relative_project_path(value);
    const auto full = checked_project_path(service, relative);
    std::error_code ec;
    if (!std::filesystem::exists(full, ec) || std::filesystem::is_directory(full, ec)) {
        throw std::invalid_argument("debug executable does not exist: " + relative.generic_string());
    }
    return full;
}

std::vector<std::string> base_gdb_args(const BackgroundService& service,
                                       const HttpRequest& request,
                                       const std::vector<std::string>& commands) {
    std::vector<std::string> args{"-nx", "-q", "--batch"};
    const auto executable = optional_debug_executable(service, request);
    if (!executable.empty()) {
        args.push_back(executable.string());
    }
    const auto target = query_value(request, "target");
    if (!target.empty()) {
        args.push_back("-ex");
        args.push_back("target remote " + target);
    }
    for (const auto& command : commands) {
        args.push_back("-ex");
        args.push_back(command);
    }
    return args;
}

json gdb_payload(const BackgroundService& service,
                 const HttpRequest& request,
                 const std::vector<std::string>& commands,
                 const std::string& operation) {
    auto payload = debug_command_payload(requested_tool_path(request, "gdb", {"gdb-multiarch", "arm-none-eabi-gdb", "gdb"}),
                                         base_gdb_args(service, request, commands),
                                         request);
    payload["operation"] = operation;
    payload["commands"] = commands;
    return payload;
}

}  // namespace

json debug_gdb_command_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto command = query_value(request, "command", "info registers");
    if (command.empty()) {
        throw std::invalid_argument("gdb command is required");
    }
    return gdb_payload(service, request, {command}, "gdb_command");
}

json debug_breakpoint_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto symbol = query_value(request, "symbol");
    const auto address = query_value(request, "address");
    if (symbol.empty() && address.empty()) {
        throw std::invalid_argument("breakpoint requires symbol or address");
    }
    return gdb_payload(service, request, {"break " + (symbol.empty() ? ("*" + address) : symbol), "info breakpoints"},
                       "breakpoint");
}

json debug_memory_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto address = query_value(request, "address");
    if (address.empty()) {
        throw std::invalid_argument("memory read requires address");
    }
    const auto count = query_size(request, "count", 16);
    const auto format = query_value(request, "format", "xw");
    return gdb_payload(service, request, {"x/" + std::to_string(count) + format + " " + address}, "memory");
}

json debug_registers_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto group = query_value(request, "group", "all-registers");
    return gdb_payload(service, request, {"info " + group}, "registers");
}

json debug_backtrace_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto limit = query_size(request, "frames", 32);
    return gdb_payload(service, request, {"bt " + std::to_string(limit)}, "backtrace");
}

json debug_step_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto kind = query_value(request, "kind", "stepi");
    if (kind != "step" && kind != "next" && kind != "stepi" && kind != "nexti" && kind != "continue") {
        throw std::invalid_argument("step kind must be step, next, stepi, nexti, or continue");
    }
    return gdb_payload(service, request, {kind, "info program"}, "step");
}

json debug_rtos_tasks_payload(const BackgroundService& service, const HttpRequest& request) {
    return gdb_payload(service, request, {"info threads", "thread apply all bt 4"}, "rtos_tasks");
}

json openocd_payload(const HttpRequest& request) {
    std::vector<std::string> args;
    const auto interface_cfg = query_value(request, "interface");
    const auto target_cfg = query_value(request, "target_cfg");
    const auto config_file = query_value(request, "config");
    if (!interface_cfg.empty()) {
        args.push_back("-f");
        args.push_back(interface_cfg);
    }
    if (!target_cfg.empty()) {
        args.push_back("-f");
        args.push_back(target_cfg);
    }
    if (!config_file.empty()) {
        args.push_back("-f");
        args.push_back(config_file);
    }
    const auto command = query_value(request, "command", "version; shutdown");
    args.push_back("-c");
    args.push_back(command);
    auto payload = debug_command_payload(requested_tool_path(request, "openocd", {"openocd"}), args, request);
    payload["operation"] = "openocd_command";
    payload["commands"] = json::array({command});
    return payload;
}

json debug_tools_payload(const HttpRequest& request) {
    const auto gdb = requested_tool_path(request, "gdb", {"gdb-multiarch", "arm-none-eabi-gdb", "gdb"});
    const auto openocd = requested_tool_path(request, "openocd", {"openocd"});
    return {{"gdb", {{"available", !gdb.empty()}, {"path", gdb.empty() ? "" : gdb.string()}}},
            {"openocd", {{"available", !openocd.empty()}, {"path", openocd.empty() ? "" : openocd.string()}}},
            {"query", request.query}};
}

}  // namespace qt::index
