#pragma once

#include "qt/harness/session.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace qt::app {

class ReferenceFooterDataProvider {
 public:
  using BranchChangeCallback = std::function<void()>;

  explicit ReferenceFooterDataProvider(
      std::string cwd,
      std::chrono::milliseconds refresh_debounce = std::chrono::milliseconds(500));

  [[nodiscard]] const std::string& cwd() const noexcept;
  void set_cwd(std::string cwd);

  [[nodiscard]] std::optional<std::string> git_branch() const;
  bool refresh(bool force = false);

  void set_extension_status(std::string key, std::string text);
  void clear_extension_statuses();
  [[nodiscard]] const std::map<std::string, std::string>& extension_statuses()
      const noexcept;

  void set_available_provider_count(std::size_t count) noexcept;
  [[nodiscard]] std::size_t available_provider_count() const noexcept;

  void on_branch_change(BranchChangeCallback callback);

 private:
  struct GitPaths {
    std::string repo_dir;
    std::string common_git_dir;
    std::string head_path;
  };

  [[nodiscard]] std::optional<GitPaths> find_git_paths() const;
  [[nodiscard]] std::optional<std::string> resolve_git_branch() const;
  void notify_branch_change() const;

  std::string cwd_;
  std::chrono::milliseconds refresh_debounce_;
  mutable std::optional<GitPaths> git_paths_;
  mutable std::optional<std::string> cached_branch_;
  std::chrono::steady_clock::time_point last_refresh_;
  std::map<std::string, std::string> extension_statuses_;
  std::size_t available_provider_count_ = 0;
  std::vector<BranchChangeCallback> branch_change_callbacks_;
};

struct ReferenceFooterRenderState {
  std::string cwd;
  std::optional<std::string> session_name;
  std::string model_provider;
  std::string model_name;
  bool reasoning_supported = false;
  std::string thinking_level;
  std::optional<std::string> provider_status;
  qt::harness::SessionStats session_stats;
};

std::vector<std::string> build_interactive_footer_lines(
    const ReferenceFooterRenderState& state,
    const ReferenceFooterDataProvider& provider,
    std::size_t width);

}  // namespace qt::app
