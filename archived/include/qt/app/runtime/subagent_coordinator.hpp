#pragma once

#include "qt/app/runtime/isolated_agent_runner.hpp"
#include "qt/harness/session.hpp"

#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace qt::app::runtime {

struct SubagentCoordinatorOptions {
  qt::foundation::Path directory;
  std::string parent_session_id;
  std::string parent_path = "/root";
  std::size_t concurrency = 4;
  IsolatedAgentRunnerOptions runner;
};

struct PersistentSubagentRequest {
  std::string name;
  std::string agent;
  std::string task;
  IsolatedAgentRequest process;
  qt::ai::Json permissions = qt::ai::Json::object();
  // "none", "all", or a positive number of parent user turns.
  qt::ai::Json fork = "none";
};

// Owns process-isolated logical workers. All child state is changed through
// mailboxes; callbacks never receive a mutable parent or child harness.
class SubagentCoordinator final {
public:
  using Json = qt::ai::Json;
  using EventSink = std::function<void(const Json &)>;
  using ContextSnapshot = std::function<std::vector<qt::ai::Message>()>;

  explicit SubagentCoordinator(SubagentCoordinatorOptions options);
  ~SubagentCoordinator();
  SubagentCoordinator(const SubagentCoordinator &) = delete;
  SubagentCoordinator &operator=(const SubagentCoordinator &) = delete;

  void set_event_sink(EventSink sink);
  void set_context_snapshot(ContextSnapshot snapshot);
  qt::foundation::Result<Json> spawn(PersistentSubagentRequest request);
  qt::foundation::Result<Json> send(const std::string &target,
                                    std::string message, bool wake);
  qt::foundation::Result<Json> interrupt(const std::string &target);
  qt::foundation::Result<Json> list() const;
  qt::foundation::Result<Json> wait(const std::string &target,
                                    std::chrono::milliseconds timeout);
  std::vector<Json> take_events(std::size_t maximum =
                                    std::numeric_limits<std::size_t>::max());
  qt::foundation::Status acknowledge_event(const std::string &child_id,
                                           std::uint64_t event_id);
  bool has_active_workers() const;
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace qt::app::runtime
