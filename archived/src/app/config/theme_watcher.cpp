#include "qt/app/config/theme_watcher.hpp"

#include <qt/app/export/resource_inspection.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace qt::app {
namespace {

// Read the entire file content into a string.
std::string read_file_contents(const std::string& path) {
  std::ifstream file(path, std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  std::ostringstream stream;
  stream << file.rdbuf();
  return stream.str();
}

// Check if a file exists.
bool file_exists(const std::string& path) {
#if defined(__linux__)
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#else
  std::ifstream file(path);
  return file.good();
#endif
}

}  // namespace

ThemeWatcher::ThemeWatcher() = default;

ThemeWatcher::~ThemeWatcher() {
  stop_watching();
}

bool ThemeWatcher::start_watching(std::string theme_name,
                                  std::string theme_file_path,
                                  ThemeReloadCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Stop any existing watcher.
  if (active_) {
    stop_watching();
  }

  // Validate inputs.
  if (theme_name.empty() || theme_file_path.empty() || !callback) {
    return false;
  }

  // Don't watch built-in themes (dark/light) — they have no file.
  if (theme_name == "dark" || theme_name == "light") {
    return false;
  }

  // Verify the file exists.
  if (!file_exists(theme_file_path)) {
    return false;
  }

  theme_name_ = std::move(theme_name);
  theme_file_path_ = std::move(theme_file_path);
  callback_ = std::move(callback);
  stop_requested_ = false;
  active_ = true;

  watcher_thread_ = std::thread(&ThemeWatcher::watcher_thread, this);
  return true;
}

void ThemeWatcher::stop_watching() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  cv_.notify_all();
  if (watcher_thread_.joinable()) {
    watcher_thread_.join();
  }
  active_ = false;
}

bool ThemeWatcher::active() const noexcept {
  return active_.load();
}

void ThemeWatcher::watcher_thread() {
#if !defined(__linux__)
  // Non-Linux: no-op watcher. Just wait for stop.
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stop_requested_.load(); });
  }
  return;
#endif

  // Linux inotify-based file watcher.
  const int inotify_fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (inotify_fd < 0) {
    active_ = false;
    return;
  }

  // Watch the parent directory because the file might be replaced (atomic
  // write via rename). We filter events for our specific filename.
  const auto dir_path = [&]() -> std::string {
    auto pos = theme_file_path_.find_last_of('/');
    if (pos == std::string::npos) {
      return ".";
    }
    return theme_file_path_.substr(0, pos);
  }();

  const auto filename = [&]() -> std::string {
    auto pos = theme_file_path_.find_last_of('/');
    if (pos == std::string::npos) {
      return theme_file_path_;
    }
    return theme_file_path_.substr(pos + 1);
  }();

  const int watch_fd = ::inotify_add_watch(
      inotify_fd, dir_path.c_str(),
      IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
  if (watch_fd < 0) {
    ::close(inotify_fd);
    active_ = false;
    return;
  }

  // Debounce: wait 100ms after last event before reloading.
  constexpr auto kDebounceDelay = std::chrono::milliseconds(100);
  auto last_event_time = std::chrono::steady_clock::time_point{};
  bool pending_reload = false;

  std::array<char, 4096> buffer{};

  while (!stop_requested_.load()) {
    // Check if we have a pending debounced reload.
    if (pending_reload) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = now - last_event_time;
      if (elapsed >= kDebounceDelay) {
        pending_reload = false;
        // Verify the file still exists and reload.
        if (file_exists(theme_file_path_)) {
          auto content = read_file_contents(theme_file_path_);
          if (!content.empty()) {
            auto parsed = parse_theme_document(theme_name_, content);
            if (parsed) {
              // Successfully parsed — invoke the callback so the caller
              // can update the theme and refresh the UI.
              if (callback_) {
                callback_();
              }
            }
          }
        }
        continue;
      }
    }

    // Wait for events with a timeout (for debounce checking).
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(inotify_fd, &read_fds);
    struct timeval tv {};
    tv.tv_sec = 0;
    tv.tv_usec = 25000;  // 25ms poll interval
    const int ret = ::select(inotify_fd + 1, &read_fds, nullptr, nullptr, &tv);

    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (ret == 0) {
      // Timeout — loop back to check debounce.
      continue;
    }

    const ssize_t bytes_read = ::read(inotify_fd, buffer.data(), buffer.size());
    if (bytes_read <= 0) {
      continue;
    }

    // Process inotify events.
    bool relevant = false;
    ssize_t offset = 0;
    while (offset < bytes_read) {
      const auto* event =
          reinterpret_cast<const struct inotify_event*>(buffer.data() + offset);
      if (event && event->len > 0) {
        if (filename == event->name) {
          relevant = true;
        }
      }
      offset += static_cast<ssize_t>(sizeof(struct inotify_event)) +
                static_cast<ssize_t>(event->len);
    }

    if (relevant) {
      last_event_time = std::chrono::steady_clock::now();
      pending_reload = true;
    }
  }

  ::inotify_rm_watch(inotify_fd, watch_fd);
  ::close(inotify_fd);
  active_ = false;
}

}  // namespace qt::app
