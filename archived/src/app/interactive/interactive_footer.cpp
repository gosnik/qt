#include "qt/app/interactive/interactive_footer.hpp"

#include "qt/tui/core/style.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace qt::app {
namespace {

std::string trim_ascii(std::string value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [](char ch) {
                return std::isspace(static_cast<unsigned char>(ch)) == 0;
              }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](char ch) {
                return std::isspace(static_cast<unsigned char>(ch)) == 0;
              }).base(),
              value.end());
  return value;
}

std::string sanitize_status_text(std::string text) {
  for (char& ch : text) {
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      ch = ' ';
    }
  }

  std::string collapsed;
  collapsed.reserve(text.size());
  bool previous_space = false;
  for (char ch : text) {
    const bool space = std::isspace(static_cast<unsigned char>(ch)) != 0;
    if (space) {
      if (!previous_space) {
        collapsed.push_back(' ');
      }
      previous_space = true;
      continue;
    }
    collapsed.push_back(ch);
    previous_space = false;
  }
  return trim_ascii(std::move(collapsed));
}

std::string format_tokens(std::int64_t count) {
  if (count < 1000) {
    return std::to_string(count);
  }
  if (count < 10000) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << static_cast<double>(count) / 1000.0 << "k";
    return out.str();
  }
  if (count < 1000000) {
    return std::to_string(static_cast<std::int64_t>(std::llround(
                               static_cast<double>(count) / 1000.0))) +
           "k";
  }
  if (count < 10000000) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << static_cast<double>(count) / 1000000.0 << "M";
    return out.str();
  }
  return std::to_string(static_cast<std::int64_t>(
             std::llround(static_cast<double>(count) / 1000000.0))) +
         "M";
}

std::string truncate_right(std::string text, std::size_t width) {
  if (text.size() <= width) {
    return text;
  }
  if (width <= 3) {
    return text.substr(0, width);
  }
  return text.substr(0, width - 3) + "...";
}

std::string styled_footer_path(std::string text) {
  return qt::tui::style::green(text);
}

std::string styled_footer_stats(std::string text) {
  return qt::tui::style::green(text);
}

std::string styled_footer_model(std::string text) {
  return qt::tui::style::cyan(text);
}

std::string styled_footer_status(std::string text) {
  return qt::tui::style::cyan(text);
}

std::string abbreviate_home(const std::string& path) {
  const char* home = std::getenv("HOME");
#if defined(_WIN32)
  if (home == nullptr) {
    home = std::getenv("USERPROFILE");
  }
#endif
  if (home == nullptr) {
    return path;
  }
  const std::string home_path(home);
  if (path == home_path) {
    return "~";
  }
  if (path.size() > home_path.size() &&
      path.compare(0, home_path.size(), home_path) == 0 &&
      (path[home_path.size()] == '/' || path[home_path.size()] == '\\')) {
    return "~" + path.substr(home_path.size());
  }
  return path;
}

}  // namespace

ReferenceFooterDataProvider::ReferenceFooterDataProvider(
    std::string cwd, std::chrono::milliseconds refresh_debounce)
    : cwd_(std::move(cwd)),
      refresh_debounce_(refresh_debounce),
      git_paths_(find_git_paths()),
      last_refresh_(std::chrono::steady_clock::now()) {}

const std::string& ReferenceFooterDataProvider::cwd() const noexcept {
  return cwd_;
}

void ReferenceFooterDataProvider::set_cwd(std::string cwd) {
  if (cwd_ == cwd) {
    return;
  }
  cwd_ = std::move(cwd);
  git_paths_ = find_git_paths();
  cached_branch_.reset();
  last_refresh_ = std::chrono::steady_clock::now() - refresh_debounce_;
  const auto branch = resolve_git_branch();
  if (cached_branch_ != branch) {
    cached_branch_ = std::move(branch);
    notify_branch_change();
  } else {
    cached_branch_ = std::move(branch);
  }
}

std::optional<std::string> ReferenceFooterDataProvider::git_branch() const {
  if (!cached_branch_.has_value()) {
    cached_branch_ = resolve_git_branch();
  }
  return cached_branch_;
}

bool ReferenceFooterDataProvider::refresh(bool force) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - last_refresh_ < refresh_debounce_) {
    return false;
  }
  last_refresh_ = now;

  const auto next_branch = resolve_git_branch();
  if (next_branch == cached_branch_) {
    return false;
  }
  cached_branch_ = next_branch;
  notify_branch_change();
  return true;
}

void ReferenceFooterDataProvider::set_extension_status(std::string key,
                                                       std::string text) {
  extension_statuses_[std::move(key)] = std::move(text);
}

void ReferenceFooterDataProvider::clear_extension_statuses() {
  extension_statuses_.clear();
}

const std::map<std::string, std::string>&
ReferenceFooterDataProvider::extension_statuses() const noexcept {
  return extension_statuses_;
}

void ReferenceFooterDataProvider::set_available_provider_count(
    std::size_t count) noexcept {
  available_provider_count_ = count;
}

std::size_t ReferenceFooterDataProvider::available_provider_count() const
    noexcept {
  return available_provider_count_;
}

void ReferenceFooterDataProvider::on_branch_change(
    BranchChangeCallback callback) {
  branch_change_callbacks_.push_back(std::move(callback));
}

std::optional<ReferenceFooterDataProvider::GitPaths>
ReferenceFooterDataProvider::find_git_paths() const {
  std::filesystem::path dir(cwd_);
  std::error_code error;
  while (!dir.empty()) {
    const auto git_path = dir / ".git";
    if (std::filesystem::exists(git_path, error)) {
      if (std::filesystem::is_directory(git_path, error)) {
        const auto head_path = git_path / "HEAD";
        if (std::filesystem::exists(head_path, error)) {
          return GitPaths{dir.generic_string(), git_path.generic_string(),
                          head_path.generic_string()};
        }
        return std::nullopt;
      }

      std::ifstream in(git_path);
      std::string content;
      std::getline(in, content);
      content = trim_ascii(std::move(content));
      constexpr std::string_view kGitDirPrefix = "gitdir:";
      if (content.rfind(kGitDirPrefix, 0) != 0) {
        return std::nullopt;
      }

      auto git_dir = trim_ascii(content.substr(kGitDirPrefix.size()));
      std::filesystem::path resolved = std::filesystem::path(dir) / git_dir;
      resolved = std::filesystem::weakly_canonical(resolved, error);
      if (error) {
        resolved = std::filesystem::path(dir) / git_dir;
      }

      auto common_git_dir = resolved;
      const auto commondir_path = resolved / "commondir";
      if (std::filesystem::exists(commondir_path, error)) {
        std::ifstream commondir_stream(commondir_path);
        std::string commondir;
        std::getline(commondir_stream, commondir);
        commondir = trim_ascii(std::move(commondir));
        if (!commondir.empty()) {
          common_git_dir = std::filesystem::path(resolved) / commondir;
          common_git_dir =
              std::filesystem::weakly_canonical(common_git_dir, error);
          if (error) {
            common_git_dir = std::filesystem::path(resolved) / commondir;
          }
        }
      }

      const auto head_path = resolved / "HEAD";
      if (!std::filesystem::exists(head_path, error)) {
        return std::nullopt;
      }
      return GitPaths{dir.generic_string(), common_git_dir.generic_string(),
                      head_path.generic_string()};
    }

    const auto parent = dir.parent_path();
    if (parent == dir) {
      break;
    }
    dir = parent;
  }
  return std::nullopt;
}

std::optional<std::string> ReferenceFooterDataProvider::resolve_git_branch()
    const {
  if (!git_paths_) {
    return std::nullopt;
  }

  std::ifstream head_stream(git_paths_->head_path);
  std::string content;
  std::getline(head_stream, content);
  content = trim_ascii(std::move(content));
  constexpr std::string_view kHeadPrefix = "ref: refs/heads/";
  if (content.rfind(kHeadPrefix, 0) == 0) {
    return content.substr(kHeadPrefix.size());
  }
  if (content.empty()) {
    return std::nullopt;
  }
  return std::string("detached");
}

void ReferenceFooterDataProvider::notify_branch_change() const {
  for (const auto& callback : branch_change_callbacks_) {
    callback();
  }
}

std::vector<std::string> build_interactive_footer_lines(
    const ReferenceFooterRenderState& state,
    const ReferenceFooterDataProvider& provider, std::size_t width) {
  std::vector<std::string> lines;

  std::string pwd = abbreviate_home(state.cwd);
  if (const auto branch = provider.git_branch(); branch && !branch->empty()) {
    pwd += " (" + *branch + ")";
  }
  if (state.session_name && !state.session_name->empty()) {
    pwd += " • " + *state.session_name;
  }
  lines.push_back(styled_footer_path(truncate_right(pwd, width)));

  std::vector<std::string> stats_parts;
  if (state.session_stats.tokens.input > 0) {
    stats_parts.push_back("↑" + format_tokens(state.session_stats.tokens.input));
  }
  if (state.session_stats.tokens.output > 0) {
    stats_parts.push_back("↓" +
                          format_tokens(state.session_stats.tokens.output));
  }
  if (state.session_stats.tokens.cache_read > 0) {
    stats_parts.push_back("R" +
                          format_tokens(state.session_stats.tokens.cache_read));
  }
  if (state.session_stats.tokens.cache_write > 0) {
    stats_parts.push_back("W" +
                          format_tokens(state.session_stats.tokens.cache_write));
  }
  if (state.session_stats.context_usage) {
    const auto& usage = *state.session_stats.context_usage;
    std::string context_display;
    if (usage.percent) {
      std::ostringstream out;
      out.setf(std::ios::fixed);
      out.precision(1);
      out << *usage.percent << "%/" << format_tokens(usage.context_window);
      context_display = out.str();
    } else {
      context_display = "?/" + format_tokens(usage.context_window);
    }
    stats_parts.push_back(context_display);
  }

  std::string left;
  for (std::size_t index = 0; index < stats_parts.size(); ++index) {
    if (index > 0) {
      left += " ";
    }
    left += stats_parts[index];
  }

  std::string model_name = state.model_name.empty() ? "no-model" : state.model_name;
  std::string right = model_name;
  if (state.reasoning_supported) {
    right += " • " + (state.thinking_level.empty() ? std::string("off")
                                                  : state.thinking_level);
  }
  if (provider.available_provider_count() > 1 && !state.model_provider.empty()) {
    right = "(" + state.model_provider + ") " + right;
  }
  if (state.provider_status && !state.provider_status->empty()) {
    right += " • " + *state.provider_status;
  }

  if (left.empty()) {
    lines.push_back(styled_footer_model(truncate_right(right, width)));
  } else {
    const std::size_t min_padding = 2;
    if (left.size() + min_padding + right.size() <= width) {
      lines.push_back(styled_footer_stats(left) +
                      std::string(width - left.size() - right.size(), ' ') +
                      styled_footer_model(right));
    } else if (left.size() + min_padding < width) {
      const std::size_t available_right = width - left.size() - min_padding;
      lines.push_back(styled_footer_stats(left) + std::string(min_padding, ' ') +
                      styled_footer_model(truncate_right(right, available_right)));
    } else {
      lines.push_back(styled_footer_stats(truncate_right(left, width)));
    }
  }

  if (!provider.extension_statuses().empty()) {
    std::string status_line;
    for (const auto& [key, text] : provider.extension_statuses()) {
      (void)key;
      const std::string sanitized = sanitize_status_text(text);
      if (sanitized.empty()) {
        continue;
      }
      if (!status_line.empty()) {
        status_line.push_back(' ');
      }
      status_line += sanitized;
    }
    if (!status_line.empty()) {
      lines.push_back(styled_footer_status(truncate_right(status_line, width)));
    }
  }

  return lines;
}

}  // namespace qt::app
