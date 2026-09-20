#pragma once

#include <qt/app/config/themes.hpp>
#include <qt/foundation/error.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace qt::app {

// Callback invoked when a theme file is successfully reloaded.
// The caller should refresh the UI.
using ThemeReloadCallback = std::function<void()>;

// Watches a theme JSON file for on-disk modifications and reloads it.
// Uses Linux inotify; on non-Linux platforms the watcher is a no-op.
class ThemeWatcher {
 public:
  ThemeWatcher();
  ~ThemeWatcher();

  ThemeWatcher(const ThemeWatcher&) = delete;
  ThemeWatcher& operator=(const ThemeWatcher&) = delete;

  // Start watching a theme file. If the file does not exist or the
  // platform is not supported, returns false (no watcher started).
  bool start_watching(std::string theme_name,
                      std::string theme_file_path,
                      ThemeReloadCallback callback);

  // Stop watching and join the background thread.
  void stop_watching();

  // Returns true if the watcher is currently active.
  bool active() const noexcept;

 private:
  void watcher_thread();

  std::string theme_name_;
  std::string theme_file_path_;
  ThemeReloadCallback callback_;
  std::atomic<bool> active_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread watcher_thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace qt::app
