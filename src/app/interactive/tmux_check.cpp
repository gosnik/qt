#include "qt/app/interactive/tmux_check.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

namespace qt::app {

namespace {

/// Run a tmux command and return stdout, or nullopt on failure.
std::optional<std::string> run_tmux_show(const std::string& option) {
    const std::string cmd = "tmux show -gv " + option + " 2>/dev/null";

    std::array<char, 256> buffer{};
    std::string result;

    // Use popen with explicit close for timeout safety
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return std::nullopt;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                               result.back() == ' ')) {
        result.pop_back();
    }

    return result.empty() ? std::optional<std::string>{} : result;
}

bool has_env(const char* name) {
    return std::getenv(name) != nullptr;
}

} // namespace

std::optional<std::string> check_tmux_keyboard_setup() {
    if (!has_env("TMUX")) {
        return std::nullopt;
    }

    const auto extended_keys = run_tmux_show("extended-keys");
    const auto extended_keys_format = run_tmux_show("extended-keys-format");

    // If we couldn't query tmux (not installed, sandbox, etc.), don't warn
    if (!extended_keys.has_value()) {
        return std::nullopt;
    }

    if (*extended_keys != "on" && *extended_keys != "always") {
        return "tmux extended-keys is off. Modified Enter keys may not work. "
               "Add `set -g extended-keys on` to ~/.tmux.conf and restart tmux.";
    }

    if (extended_keys_format.has_value() && *extended_keys_format == "xterm") {
        return "tmux extended-keys-format is xterm. Pi works best with csi-u. "
               "Add `set -g extended-keys-format csi-u` to ~/.tmux.conf and restart tmux.";
    }

    return std::nullopt;
}

}