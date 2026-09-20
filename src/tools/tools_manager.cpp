#include <qt/tools/tools_manager.hpp>

#include <cstdlib>
#include <filesystem>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace qt::tools {

namespace {

int command_status(const std::string& command) {
    return std::system(command.c_str());
}

}  // namespace

ToolsManager::ToolsManager(std::string local_bin_dir)
    : local_bin_dir(std::move(local_bin_dir)) {}

std::optional<std::string> ToolsManager::check_command(const std::string& cmd) const {
#ifdef _WIN32
    const std::string test_cmd = "where " + cmd + " >nul 2>nul";
#else
    const std::string test_cmd = "command -v " + cmd + " >/dev/null 2>&1";
#endif
    if (command_status(test_cmd) == 0) {
        return cmd;
    }
    return std::nullopt;
}

std::optional<std::string> ToolsManager::find_in_local_dir(const std::string& binary_name) const {
    if (local_bin_dir.empty()) return std::nullopt;
    namespace fs = std::filesystem;
    const auto candidate = fs::path(local_bin_dir) / binary_name;
    if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
        return candidate.string();
    }
    // Also try with .exe on Windows
#ifdef _WIN32
    const auto candidate_exe = fs::path(local_bin_dir) / (binary_name + ".exe");
    if (fs::exists(candidate_exe) && fs::is_regular_file(candidate_exe)) {
        return candidate_exe.string();
    }
#endif
    return std::nullopt;
}

std::optional<std::string> ToolsManager::find_fd() const {
    // Check local bin directory first
    if (auto local = find_in_local_dir("fd")) return local;

    // Ubuntu/Debian ships fd as "fdfind"
    if (auto fdfind = check_command("fdfind")) return fdfind;

    // Standard name
    if (auto fd = check_command("fd")) return fd;

    return std::nullopt;
}

std::optional<std::string> ToolsManager::find_rg() const {
    // Check local bin directory first
    if (auto local = find_in_local_dir("rg")) return local;

    // Standard name
    if (auto rg = check_command("rg")) return rg;

    return std::nullopt;
}

std::optional<std::string> ToolsManager::ensure_fd(ToolEnsureOptions /*options*/) const {
    return find_fd();
}

std::optional<std::string> ToolsManager::ensure_rg(ToolEnsureOptions /*options*/) const {
    return find_rg();
}

}  // namespace qt::tools
