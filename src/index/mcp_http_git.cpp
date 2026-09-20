#include "index/mcp_http_git.hpp"

#include <nlohmann/json.hpp>

#include <boost/process.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
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

std::filesystem::path requested_relative_file(const HttpRequest& request) {
    const auto file_it = request.query.find("file");
    if (file_it == request.query.end() || file_it->second.empty()) {
        throw std::invalid_argument("file query parameter is required");
    }
    return relative_project_path(file_it->second);
}

std::filesystem::path optional_git_file(const HttpRequest& request) {
    const auto found = request.query.find("file");
    if (found == request.query.end() || found->second.empty()) {
        return {};
    }
    return relative_project_path(found->second);
}

std::string checked_git_revision(const HttpRequest& request, const std::string& fallback = "HEAD") {
    const auto revision = query_value(request, "rev", fallback);
    if (revision.empty() || revision.front() == '-') {
        throw std::invalid_argument("git revision must not be empty or start with '-'");
    }
    return revision;
}

std::filesystem::path git_on_path() {
    if (const char* path_env = std::getenv("PATH"); path_env != nullptr) {
        std::stringstream path_stream(path_env);
        std::string directory;
        while (std::getline(path_stream, directory, ':')) {
            if (directory.empty()) {
                continue;
            }
            std::error_code ec;
            const auto candidate = std::filesystem::path{directory} / "git";
            if (std::filesystem::exists(candidate, ec)) {
                return candidate;
            }
        }
    }
    return {};
}

json git_command_payload(const BackgroundService& service,
                         const std::vector<std::string>& args,
                         std::size_t max_lines = 200) {
    bp::ipstream stdout_stream;
    bp::ipstream stderr_stream;
    const auto git = git_on_path();
    if (git.empty()) {
        throw std::runtime_error("git executable not found on PATH");
    }
    std::vector<std::string> full_args{"-C", service.project_root().string()};
    full_args.insert(full_args.end(), args.begin(), args.end());
    bp::child child(bp::exe = git.string(),
                    bp::args = full_args,
                    bp::std_out > stdout_stream,
                    bp::std_err > stderr_stream);

    json stdout_lines = json::array();
    json stderr_lines = json::array();
    std::string line;
    bool stdout_truncated = false;
    while (std::getline(stdout_stream, line)) {
        if (max_lines != 0 && stdout_lines.size() >= max_lines) {
            stdout_truncated = true;
            continue;
        }
        stdout_lines.push_back(line);
    }
    while (std::getline(stderr_stream, line)) {
        stderr_lines.push_back(line);
    }
    child.wait();
    return {{"args", full_args},
            {"exit_code", child.exit_code()},
            {"stdout", stdout_lines},
            {"stderr", stderr_lines},
            {"truncated", stdout_truncated}};
}

}  // namespace

json git_status_payload(const BackgroundService& service, const HttpRequest& request) {
    std::vector<std::string> args{"status"};
    if (query_bool(request, "short", true)) {
        args.push_back("--short");
        args.push_back("--branch");
    }
    auto payload = git_command_payload(service, args, query_size(request, "limit", 200));
    payload["query"] = request.query;
    return payload;
}

json git_log_payload(const BackgroundService& service, const HttpRequest& request) {
    std::vector<std::string> args{"log", "--oneline", "--decorate",
                                  "--max-count=" + std::to_string(query_size(request, "limit", 20))};
    const auto file = optional_git_file(request);
    if (!file.empty()) {
        args.push_back("--");
        args.push_back(file.generic_string());
    }
    auto payload = git_command_payload(service, args, query_size(request, "limit", 20));
    payload["query"] = request.query;
    return payload;
}

json git_diff_payload(const BackgroundService& service, const HttpRequest& request) {
    std::vector<std::string> args{"diff"};
    if (query_bool(request, "staged", false)) {
        args.push_back("--cached");
    }
    if (query_bool(request, "stat", false)) {
        args.push_back("--stat");
    }
    const auto file = optional_git_file(request);
    if (!file.empty()) {
        args.push_back("--");
        args.push_back(file.generic_string());
    }
    auto payload = git_command_payload(service, args, query_size(request, "limit", 300));
    payload["query"] = request.query;
    return payload;
}

json git_show_payload(const BackgroundService& service, const HttpRequest& request) {
    std::vector<std::string> args{"show", "--stat", "--decorate", checked_git_revision(request)};
    auto payload = git_command_payload(service, args, query_size(request, "limit", 200));
    payload["query"] = request.query;
    return payload;
}

json git_blame_payload(const BackgroundService& service, const HttpRequest& request) {
    const auto file = requested_relative_file(request);
    std::vector<std::string> args{"blame"};
    const auto start_line = query_size(request, "start_line", 1);
    const auto end_line = query_size(request, "end_line", start_line);
    args.push_back("-L");
    args.push_back(std::to_string(start_line) + "," + std::to_string(end_line));
    args.push_back("--");
    args.push_back(file.generic_string());
    auto payload = git_command_payload(service, args, query_size(request, "limit", 200));
    payload["query"] = request.query;
    return payload;
}

json git_branches_payload(const BackgroundService& service, const HttpRequest& request) {
    std::vector<std::string> args{"branch", "--list"};
    if (query_bool(request, "all", false)) {
        args.insert(args.begin() + 1, "--all");
    }
    auto payload = git_command_payload(service, args, query_size(request, "limit", 200));
    payload["query"] = request.query;
    return payload;
}

}  // namespace qt::index
