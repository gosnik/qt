#pragma once

#include <optional>
#include <string>

namespace qt::app {

/// Check tmux keyboard configuration and return a warning message if misconfigured.
///
/// Returns std::nullopt if:
/// - Not running inside tmux (no TMUX env var)
/// - tmux is not installed
/// - tmux settings are correctly configured
/// - tmux query times out
///
/// Returns a warning string if:
/// - extended-keys is not "on" or "always"
/// - extended-keys-format is "xterm" (should be "csi-u")
[[nodiscard]] std::optional<std::string> check_tmux_keyboard_setup();

}