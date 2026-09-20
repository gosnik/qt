#pragma once

#include "qt/foundation/error.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace qt::memory {
class MemoryRuntime;
}

namespace qt::app {

struct MemoryDashboardLaunch {
  std::string url;
  bool started = false;
  bool browser_opened = false;
  std::string browser_error;
};

class MemoryDashboardServer final {
public:
  using RuntimeProvider =
      std::function<std::shared_ptr<qt::memory::MemoryRuntime>()>;

  explicit MemoryDashboardServer(RuntimeProvider runtime_provider);
  ~MemoryDashboardServer();
  MemoryDashboardServer(const MemoryDashboardServer &) = delete;
  MemoryDashboardServer &operator=(const MemoryDashboardServer &) = delete;

  qt::foundation::Result<MemoryDashboardLaunch> start(bool open_browser);
  void stop() noexcept;
  bool running() const noexcept;
  std::string url() const;

private:
  void serve() noexcept;
  void handle_client(int client) noexcept;

  RuntimeProvider runtime_provider_;
  mutable std::mutex mutex_;
  int listener_ = -1;
  unsigned short port_ = 0;
  std::string token_;
  std::thread thread_;
};

} // namespace qt::app
