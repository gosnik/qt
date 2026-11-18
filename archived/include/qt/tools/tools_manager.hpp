#pragma once

#include <optional>
#include <string>

namespace qt::tools {

struct ToolEnsureOptions {
  bool silent = false;
};

/// Detects availability of external tools (fd, rg) used by the tool subsystem.
/// Checks the local bin directory first, then system PATH. Equivalent to the
/// reference's `getToolPath()` / `ensureTool()` detection logic (without network download).
struct ToolsManager {
  /// Path to a local bin directory where tool binaries may be installed.
  /// If empty, only system PATH is searched.
  std::string local_bin_dir;

  explicit ToolsManager(std::string local_bin_dir = {});

  /// Try to find fd. Returns the resolved path/command or nullopt.
  /// Checks local_bin_dir, then tries "fdfind" (Ubuntu), then "fd".
  std::optional<std::string> find_fd() const;

  /// Try to find rg. Returns the resolved path/command or nullopt.
  /// Checks local_bin_dir, then tries "rg".
  std::optional<std::string> find_rg() const;

  /// Ensure fd is available (same as find_fd). Returns path or nullopt.
  /// The reference's ensureTool() can download; the native version only detects.
  std::optional<std::string> ensure_fd(ToolEnsureOptions options = {}) const;

  /// Ensure rg is available (same as find_rg). Returns path or nullopt.
  std::optional<std::string> ensure_rg(ToolEnsureOptions options = {}) const;

private:
  std::optional<std::string> check_command(const std::string& cmd) const;
  std::optional<std::string> find_in_local_dir(const std::string& binary_name) const;
};

}  // namespace qt::tools
