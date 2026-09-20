#include "qt/app/runtime/subagent_coordinator.hpp"

#include "qt/foundation/filesystem.hpp"
#include "qt/foundation/uuid.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <regex>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace qt::app::runtime {
namespace {

using Json = SubagentCoordinator::Json;
using ErrorCode = qt::foundation::ErrorCode;

std::mutex global_reservation_mutex;
std::condition_variable global_reservation_cv;
std::size_t global_active_reservations = 0;
constexpr std::size_t kGlobalReservationLimit = 4;

qt::foundation::Result<Json> failure(ErrorCode code, std::string message) {
  return qt::foundation::Result<Json>::failure(code, std::move(message));
}

bool valid_name(const std::string &value) {
  static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$");
  return !value.empty() && std::regex_match(value, pattern) && value != "." &&
         value != "..";
}

// Create private files before writing any conversation or permission data.
bool write_private(const std::filesystem::path &path,
                   const std::string &bytes) {
  const int fd =
      ::open(path.c_str(),
             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return false;
  bool ok = ::fchmod(fd, 0600) == 0;
  std::size_t offset = 0;
  while (ok && offset < bytes.size()) {
    const auto count =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (ok)
    ok = ::fsync(fd) == 0;
  if (::close(fd) != 0)
    ok = false;
  return ok;
}

Json upstream_content(Json block) {
  const auto type = block.value("type", "");
  if (type == "tool_call")
    block["type"] = "toolCall";
  if (type == "thinking") {
    block["thinking"] = block.value("text", "");
    block.erase("text");
  }
  for (const auto &pair :
       {std::pair{"media_type", "mimeType"},
        std::pair{"thought_signature", "thoughtSignature"},
        std::pair{"signature", type == "thinking" ? "thinkingSignature"
                                                  : "textSignature"}}) {
    if (block.contains(pair.first)) {
      block[pair.second] = block[pair.first];
      block.erase(pair.first);
    }
  }
  return block;
}

} // namespace

struct SubagentCoordinator::Impl {
  struct Worker {
    std::string id;
    std::string name;
    std::string agent;
    std::string path;
    std::string status = "idle";
    std::string last_error;
    std::uint64_t generation = 0;
    std::uint64_t event_id = 0;
    Json fork_context = Json::array();
    bool running = false;
    bool interrupted = false;
    bool completed = false;
    bool failed = false;
    bool wake = false;
    std::atomic_bool stop{false};
    PersistentSubagentRequest request;
    std::deque<std::string> mailbox;
    std::shared_ptr<qt::foundation::ProcessCancellationToken> cancellation;
    std::thread thread;
    std::condition_variable cv;
  };

  explicit Impl(SubagentCoordinatorOptions value) : options(std::move(value)) {
    if (options.directory.empty())
      options.directory = ".qt/subagents";
    const auto directory_status =
        qt::foundation::ensure_directory(options.directory);
    if (!directory_status)
      persistence_error = directory_status.error().message;
    if (persistence_error.empty()) {
      lock_fd = ::open((options.directory / "children.lock").c_str(),
                       O_CREAT | O_RDWR | O_CLOEXEC, 0600);
      if (lock_fd < 0 || ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0)
        persistence_error = "another subagent coordinator owns the directory";
    }
    if (persistence_error.empty()) {
      load_index();
      if (persistence_error.empty() && !events.empty()) {
        std::lock_guard<std::mutex> lock(mutex);
        (void)persist_locked();
      }
    }
  }

  ~Impl() {
    shutdown();
    if (lock_fd >= 0) {
      (void)::flock(lock_fd, LOCK_UN);
      (void)::close(lock_fd);
    }
  }

  SubagentCoordinatorOptions options;
  mutable std::mutex mutex;
  std::condition_variable state_cv;
  std::unordered_map<std::string, std::shared_ptr<Worker>> workers;
  std::vector<Json> events;
  EventSink sink;
  ContextSnapshot snapshot;
  std::size_t active = 0;
  std::uint64_t completion_count = 0;
  std::atomic_bool stopping{false};
  int lock_fd = -1;
  std::string persistence_error;
  const std::filesystem::path index_path() const {
    return options.directory / "children.json";
  }

  void load_index() {
    std::ifstream input(index_path());
    if (!input)
      return;
    try {
      Json root;
      input >> root;
      if (!root.is_object() || root.value("version", 0) != 1 ||
          !root.contains("children") || !root.at("children").is_array())
        throw std::runtime_error("invalid children index");
      if (root.value("parent_session_id", "") != options.parent_session_id)
        throw std::runtime_error(
            "child index belongs to another parent session");
      if (root.contains("pending_events") &&
          root.at("pending_events").is_array())
        for (const auto &event : root.at("pending_events")) {
          if (!event.is_object())
            throw std::runtime_error("invalid pending event");
          events.push_back(event);
        }
      for (const auto &item : root.at("children")) {
        if (!item.is_object() || !item.contains("id") ||
            !item.at("id").is_string() || !item.contains("request") ||
            !item.at("request").is_object())
          throw std::runtime_error("invalid child record");
        auto worker = std::make_shared<Worker>();
        worker->id = item.at("id").get<std::string>();
        worker->name = item.value("name", worker->id);
        worker->agent = item.value("agent", "");
        worker->path = item.value("path", "");
        if (!valid_name(worker->name) ||
            worker->id != options.parent_path + "/" + worker->name ||
            std::filesystem::path(worker->path) !=
                options.directory / worker->name / "session.jsonl")
          throw std::runtime_error("invalid child identity or session path");
        worker->status = item.value("status", "interrupted");
        const bool was_running = worker->status == "running";
        worker->wake = item.value("wake", false);
        worker->last_error = item.value("last_error", "");
        worker->generation = item.value("generation", 0ULL);
        worker->event_id = item.value("event_id", 0ULL);
        worker->fork_context = item.value("fork_context", Json::array());
        const auto &stored = item.at("request");
        worker->request.name = worker->name;
        worker->request.agent = stored.value("agent", worker->agent);
        worker->request.task = stored.value("task", "");
        worker->request.permissions =
            stored.value("permissions", Json::object());
        worker->request.fork = stored.value("fork", Json("none"));
        const auto &process = stored.at("process");
        worker->request.process.arguments =
            process.value("arguments", std::vector<std::string>{});
        worker->request.process.stdin_payload =
            process.value("stdin_payload", "");
        if (process.contains("cwd") && process.at("cwd").is_string())
          worker->request.process.cwd = process.at("cwd").get<std::string>();
        worker->request.process.timeout_ms =
            process.value("timeout_ms", Json(nullptr)).is_null()
                ? std::optional<std::int64_t>{}
                : std::optional<std::int64_t>(
                      process.at("timeout_ms").get<std::int64_t>());
        if (process.contains("environment") &&
            process.at("environment").is_array()) {
          for (const auto &env : process.at("environment")) {
            if (!env.is_object() || !env.contains("name") ||
                !env.at("name").is_string())
              throw std::runtime_error("invalid child environment");
            qt::foundation::ProcessEnvironmentVariable value;
            value.name = env.at("name").get<std::string>();
            if (env.contains("value") && !env.at("value").is_null())
              value.value = env.at("value").get<std::string>();
            worker->request.process.environment.push_back(std::move(value));
          }
        }
        if (item.contains("mailbox") && item.at("mailbox").is_array())
          for (const auto &message : item.at("mailbox")) {
            if (!message.is_string())
              throw std::runtime_error("invalid child mailbox");
            worker->mailbox.push_back(message.get<std::string>());
          }
        worker->completed = worker->status == "completed";
        worker->failed = worker->status == "failed";
        worker->interrupted = worker->status == "interrupted" || was_running;
        if (was_running || worker->wake) {
          worker->status = "interrupted";
          worker->interrupted = true;
          worker->wake = false;
          ++worker->event_id;
          events.push_back(Json{{"type", "subagent_complete"},
                                {"child_id", worker->id},
                                {"generation", worker->generation},
                                {"event_id", worker->event_id},
                                {"status", "interrupted"},
                                {"interrupted", true},
                                {"recovered", true}});
        }
        if (!worker->id.empty())
          workers[worker->id] = std::move(worker);
      }
    } catch (const std::exception &error) {
      workers.clear();
      persistence_error =
          std::string("invalid children index: ") + error.what();
    }
  }

  Json worker_json(const Worker &worker) const {
    Json result{{"id", worker.id},
                {"name", worker.name},
                {"agent", worker.agent},
                {"path", worker.path},
                {"status", worker.status},
                {"generation", worker.generation},
                {"event_id", worker.event_id},
                {"wake", worker.wake},
                {"mailbox_depth", worker.mailbox.size()},
                {"mailbox", worker.mailbox},
                {"parent_session_id", options.parent_session_id},
                {"permissions", worker.request.permissions},
                {"fork", worker.request.fork}};
    result["request"] = Json{
        {"agent", worker.request.agent},
        {"task", worker.request.task},
        {"permissions", worker.request.permissions},
        {"fork", worker.request.fork},
        {"process",
         Json{{"arguments", worker.request.process.arguments},
              {"stdin_payload", worker.request.process.stdin_payload},
              {"cwd", worker.request.process.cwd
                          ? Json(worker.request.process.cwd->string())
                          : Json(nullptr)},
              {"timeout_ms", worker.request.process.timeout_ms
                                 ? Json(*worker.request.process.timeout_ms)
                                 : Json(nullptr)},
              {"environment", [&] {
                 Json values = Json::array();
                 for (const auto &env : worker.request.process.environment)
                   values.push_back(Json{{"name", env.name},
                                         {"value", env.value ? Json(*env.value)
                                                             : Json(nullptr)}});
                 return values;
               }()}}}};
    result["fork_context"] = worker.fork_context;
    if (!worker.last_error.empty())
      result["last_error"] = worker.last_error;
    return result;
  }

  bool persist_locked() {
    Json root{{"version", 1},
              {"parent_session_id", options.parent_session_id},
              {"pending_events", events},
              {"children", Json::array()}};
    for (const auto &[id, worker] : workers) {
      (void)id;
      root["children"].push_back(worker_json(*worker));
    }
    const auto temporary = index_path().string() + ".tmp-" +
                           qt::foundation::generate_uuid_v4_string();
    if (!write_private(temporary, root.dump(2) + "\n")) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      persistence_error = "cannot write children index";
      return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, index_path(), error);
    if (error) {
      std::filesystem::remove(temporary);
      persistence_error = "cannot atomically replace children index";
      return false;
    }
    persistence_error.clear();
    return true;
  }

  std::shared_ptr<Worker> find_locked(const std::string &target) const {
    auto it = workers.find(target);
    if (it != workers.end())
      return it->second;
    it = workers.find(options.parent_path + "/" + target);
    if (it != workers.end())
      return it->second;
    return nullptr;
  }

  void emit(Json event) {
    EventSink callback;
    {
      std::lock_guard<std::mutex> lock(mutex);
      // Starts are observable to RPC listeners but only terminal results wake
      // parents.
      (void)persist_locked();
      callback = sink;
    }
    if (callback) {
      try {
        callback(event);
      } catch (...) { /* sinks cannot kill workers */
      }
    }
  }

  void run_worker(const std::shared_ptr<Worker> &worker) {
    for (;;) {
      PersistentSubagentRequest request;
      std::size_t reservation_limit = 1;
      bool launch_allowed = true;
      std::string launch_error;
      {
        std::unique_lock<std::mutex> lock(mutex);
        worker->cv.wait(
            lock, [&] { return stopping || worker->stop || worker->wake; });
        if (stopping || worker->stop)
          return;
        reservation_limit =
            std::min(kGlobalReservationLimit,
                     std::max<std::size_t>(1, options.concurrency));
        lock.unlock();
        std::unique_lock<std::mutex> global_lock(global_reservation_mutex);
        global_reservation_cv.wait(global_lock, [&] {
          return stopping || worker->stop ||
                 global_active_reservations < reservation_limit;
        });
        if (stopping || worker->stop)
          return;
        ++global_active_reservations;
        global_lock.unlock();
        lock.lock();
        if (stopping || worker->stop || !worker->wake) {
          lock.unlock();
          {
            std::lock_guard<std::mutex> reservation_lock(
                global_reservation_mutex);
            --global_active_reservations;
          }
          global_reservation_cv.notify_all();
          continue;
        }
        if (active >= std::max<std::size_t>(1, options.concurrency)) {
          lock.unlock();
          {
            std::lock_guard<std::mutex> reservation_lock(
                global_reservation_mutex);
            --global_active_reservations;
          }
          global_reservation_cv.notify_all();
          continue;
        }
        worker->wake = false;
        worker->running = true;
        ++worker->generation;
        worker->interrupted = false;
        worker->status = "running";
        ++active;
        request = worker->request;
        if (!worker->mailbox.empty()) {
          request.task = worker->mailbox.front();
          worker->mailbox.pop_front();
        }
        worker->cancellation =
            std::make_shared<qt::foundation::ProcessCancellationToken>();
        request.process.cancellation_token = worker->cancellation;
        request.process.environment.push_back({"QT_SUBAGENT_PATH", worker->id});
        request.process.environment.push_back(
            {"QT_SUBAGENT_PARENT_SESSION", options.parent_session_id});
        launch_allowed = persist_locked();
        launch_error = persistence_error;
      }

      Json event;
      event["type"] = "subagent_start";
      event["child_id"] = worker->id;
      event["generation"] = worker->generation;
      emit(event);

      qt::foundation::Result<IsolatedAgentResult> result =
          qt::foundation::Result<IsolatedAgentResult>::failure(
              ErrorCode::internal_error, "worker did not run");
      try {
        if (!launch_allowed) {
          result = qt::foundation::Result<IsolatedAgentResult>::failure(
              ErrorCode::io_error, launch_error);
        } else {
          auto process = request.process;
          process.arguments.push_back("--session");
          process.arguments.push_back(worker->path);
          process.stdin_payload = request.task + "\n";
          result = IsolatedAgentRunner(options.runner).run(process);
        }
      } catch (const std::exception &error) {
        result = qt::foundation::Result<IsolatedAgentResult>::failure(
            ErrorCode::internal_error, error.what());
      } catch (...) {
        result = qt::foundation::Result<IsolatedAgentResult>::failure(
            ErrorCode::internal_error, "unknown worker exception");
      }

      bool interrupted = false;
      bool run_again = false;
      std::uint64_t event_generation = 0;
      std::uint64_t event_id = 0;
      std::string event_status;
      EventSink completion_sink;
      {
        std::lock_guard<std::mutex> lock(mutex);
        interrupted =
            worker->interrupted || (result && result.value().cancelled);
        worker->running = false;
        --active;
        {
          std::lock_guard<std::mutex> reservation_lock(
              global_reservation_mutex);
          if (global_active_reservations > 0)
            --global_active_reservations;
        }
        global_reservation_cv.notify_all();
        worker->cancellation.reset();
        if (!result) {
          worker->failed = true;
          worker->status = interrupted ? "interrupted" : "failed";
          worker->last_error = result.error().message;
        } else {
          const bool has_final = std::any_of(result.value().events.begin(), result.value().events.end(),
              [](const Json& value) { return value.is_object() && value.value("type", "") == "agent_end" &&
                                           value.contains("messages") && value["messages"].is_array(); });
          worker->failed = !result.value().completed() || !has_final;
          worker->completed = !worker->failed;
          worker->status = interrupted
                               ? "interrupted"
                               : (worker->failed ? "failed" : "completed");
          worker->last_error.clear();
          if (worker->failed) {
            worker->last_error = result.value().stderr_text;
            if (worker->last_error.empty()) worker->last_error = !has_final
                ? "child did not produce a final JSON result" : "child process failed";
          }
        }
        ++worker->event_id;
        ++completion_count;
        event_generation = worker->generation;
        event_id = worker->event_id;
        event_status = worker->status;
        run_again = !stopping && (!interrupted || worker->wake) &&
                    !worker->mailbox.empty();
        worker->wake = run_again;
        event = Json{
            {"type", "subagent_complete"},    {"child_id", worker->id},
            {"generation", event_generation}, {"event_id", event_id},
            {"status", event_status},         {"interrupted", interrupted}};
        if (result) {
          event["exit_code"] = result.value().exit_code;
          event["output"] = result.value().final_output;
          event["messages"] = result.value().final_messages;
          if (worker->failed)
            event["error"] = worker->last_error;
        } else
          event["error"] = result.error().message;
        events.push_back(event);
        persist_locked();
        completion_sink = sink;
        state_cv.notify_all();
      }
      if (completion_sink) {
        try {
          completion_sink(event);
        } catch (...) {
        }
      }
    }
  }

  void start_worker_locked(const std::shared_ptr<Worker> &worker) {
    if (worker->thread.joinable())
      return;
    worker->thread =
        std::thread([impl = this, worker] { impl->run_worker(worker); });
  }

  bool ensure_session_header(const Worker &worker,
                             const PersistentSubagentRequest &request) {
    const std::filesystem::path path(worker.path);
    Json header{{"type", "session"},
                {"version", 3},
                {"id", qt::foundation::generate_uuid_v4_string()},
                {"cwd", request.process.cwd ? request.process.cwd->string()
                                            : std::string{}},
                {"parentSession", options.parent_session_id},
                {"subagent", Json{{"childId", worker.id},
                                  {"agent", request.agent},
                                  {"fork", request.fork},
                                  {"permissions", request.permissions}}}};
    std::string bytes = header.dump() + "\n";
    Json previous = nullptr;
    std::unordered_map<std::string, std::string> pending_calls;
    const auto append = [&](Json message) {
      const auto id = qt::foundation::generate_uuid_v4_string();
      bytes += Json{{"type", "message"},
                    {"id", id},
                    {"parentId", previous},
                    {"message", std::move(message)}}
                   .dump() +
               "\n";
      previous = id;
    };
    for (const auto &message : worker.fork_context) {
      const auto role = message.value("role", "");
      Json content = Json::array();
      for (const auto &block : message.at("content")) {
        if (block.value("type", "") == "tool_result") {
          const auto call_id = block.value("tool_call_id", "");
          pending_calls.erase(call_id);
          const auto result = block.value("result", Json::object());
          const auto result_content =
              result.is_object() && result.contains("content") &&
                      result["content"].is_array()
                  ? result["content"]
                  : Json::array({Json{{"type", "text"},
                                      {"text", result.is_string()
                                                   ? result.get<std::string>()
                                                   : result.dump()}}});
          append(Json{{"role", "toolResult"},
                      {"toolCallId", call_id},
                      {"toolName", block.value("name", "")},
                      {"content", result_content},
                      {"details", result},
                      {"isError", block.value("is_error", false)}});
        } else {
          if (block.value("type", "") == "tool_call")
            pending_calls[block.value("id", "")] = block.value("name", "");
          content.push_back(upstream_content(block));
        }
      }
      if (content.empty())
        continue;
      Json converted{{"role", role}, {"content", content}};
      if (role == "assistant") {
        const auto metadata = message.value("metadata", Json::object());
        for (const auto *key : {"api", "provider", "model"})
          converted[key] = metadata.value(key, "");
        converted["usage"] = metadata.value("usage", Json::object());
        converted["stopReason"] = metadata.value("stopReason", "stop");
      }
      append(std::move(converted));
    }
    // A fork taken inside a tool invocation can contain unfinished calls.
    // Preserve them and explicitly close them in the child's independent
    // history.
    for (const auto &[id, name] : pending_calls)
      append(Json{
          {"role", "toolResult"},
          {"toolCallId", id},
          {"toolName", name},
          {"isError", true},
          {"content",
           Json::array({Json{{"type", "text"},
                             {"text", "Parent tool call was still running when "
                                      "this context was forked."}}})}});
    if (!write_private(path, bytes)) {
      persistence_error = "cannot create child session";
      return false;
    }
    return true;
  }

  void shutdown() noexcept {
    std::vector<std::shared_ptr<Worker>> join;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping)
        return;
      stopping = true;
      for (auto &[id, worker] : workers) {
        (void)id;
        worker->stop = true;
        worker->wake = false;
        if (worker->cancellation)
          worker->cancellation->cancel();
        worker->cv.notify_all();
        join.push_back(worker);
      }
      state_cv.notify_all();
    }
    global_reservation_cv.notify_all();
    for (auto &worker : join)
      if (worker->thread.joinable())
        worker->thread.join();
    std::lock_guard<std::mutex> lock(mutex);
    if (lock_fd >= 0) {
      (void)::flock(lock_fd, LOCK_UN);
      (void)::close(lock_fd);
      lock_fd = -1;
    }
  }
};

SubagentCoordinator::SubagentCoordinator(SubagentCoordinatorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
SubagentCoordinator::~SubagentCoordinator() = default;

void SubagentCoordinator::set_event_sink(EventSink sink) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->sink = std::move(sink);
}
void SubagentCoordinator::set_context_snapshot(ContextSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->snapshot = std::move(snapshot);
}

qt::foundation::Result<SubagentCoordinator::Json>
SubagentCoordinator::spawn(PersistentSubagentRequest request) {
  if (!valid_name(request.name) || request.name.find('/') != std::string::npos)
    return failure(ErrorCode::invalid_argument, "invalid child name");
  if (request.agent.empty() || request.task.empty())
    return failure(ErrorCode::invalid_argument, "agent and task are required");
  const bool valid_fork =
      (request.fork.is_string() &&
       (request.fork == "none" || request.fork == "all")) ||
      (request.fork.is_number_integer() && request.fork.get<long long>() > 0);
  if (!valid_fork)
    return failure(ErrorCode::invalid_argument,
                   "fork must be none, all, or a positive turn count");
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->persistence_error.empty())
      return failure(ErrorCode::io_error, impl_->persistence_error);
  }
  std::vector<qt::ai::Message> context;
  if (request.fork != "none") {
    ContextSnapshot callback;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      callback = impl_->snapshot;
    }
    if (!callback)
      return failure(ErrorCode::invalid_argument,
                     "fork requested without context snapshot");
    try {
      context = callback();
    } catch (...) {
      return failure(ErrorCode::internal_error, "context snapshot failed");
    }
  }
  std::shared_ptr<Impl::Worker> worker;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopping)
      return failure(ErrorCode::internal_error, "coordinator is shut down");
    worker = impl_->find_locked(request.name);
    if (worker)
      return failure(
          ErrorCode::invalid_argument,
          "child already exists; use send with wake=true to reuse it");
    if (!worker) {
      worker = std::make_shared<Impl::Worker>();
      worker->id = impl_->options.parent_path + "/" + request.name;
      worker->name = request.name;
      worker->path =
          (impl_->options.directory / request.name / "session.jsonl").string();
      const auto directory = qt::foundation::ensure_directory(
          std::filesystem::path(worker->path).parent_path());
      if (!directory)
        return failure(directory.error().code, directory.error().message);
      impl_->workers.emplace(worker->id, worker);
    } else {
      ++worker->generation;
      worker->completed = worker->failed = worker->interrupted = false;
      worker->last_error.clear();
    }
    worker->agent = request.agent;
    const Json fork_mode = request.fork;
    worker->request = std::move(request);
    worker->fork_context = Json::array();
    if (fork_mode != "none") {
      std::size_t begin = 0;
      if (fork_mode.is_number_integer()) {
        auto remaining = fork_mode.get<std::uint64_t>();
        for (std::size_t index = context.size(); index > 0; --index)
          if (context[index - 1].role == qt::ai::MessageRole::user &&
              context[index - 1].metadata.value("role", "") != "custom" &&
              --remaining == 0) {
            begin = index - 1;
            break;
          }
      }
      for (std::size_t index = begin; index < context.size(); ++index)
        worker->fork_context.push_back(context[index]);
    }
    worker->request.process.cancellation_token.reset();
    worker->status = "idle";
    worker->wake = true;
    if (!impl_->ensure_session_header(*worker, worker->request) ||
        !impl_->persist_locked()) {
      impl_->workers.erase(worker->id);
      return failure(ErrorCode::io_error, impl_->persistence_error);
    }
    impl_->start_worker_locked(worker);
    worker->cv.notify_one();
    impl_->state_cv.notify_all();
    return Json{{"child_id", worker->id},
                {"generation", worker->generation},
                {"status", worker->status},
                {"path", worker->path}};
  }
}

qt::foundation::Result<SubagentCoordinator::Json>
SubagentCoordinator::send(const std::string &target, std::string message,
                          bool wake) {
  if (message.empty())
    return failure(ErrorCode::invalid_argument, "message is empty");
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!impl_->persistence_error.empty())
    return failure(ErrorCode::io_error, impl_->persistence_error);
  if (impl_->stopping)
    return failure(ErrorCode::internal_error, "coordinator is shut down");
  auto worker = impl_->find_locked(target);
  if (!worker)
    return failure(ErrorCode::not_found, "unknown child: " + target);
  worker->mailbox.push_back(std::move(message));
  if (wake)
    worker->wake = true;
  if (!impl_->persist_locked())
    return failure(ErrorCode::io_error, impl_->persistence_error);
  if (wake)
    impl_->start_worker_locked(worker);
  worker->cv.notify_one();
  impl_->state_cv.notify_all();
  return Json{{"child_id", worker->id},
              {"queued", true},
              {"wake", wake},
              {"mailbox_depth", worker->mailbox.size()}};
}

qt::foundation::Result<SubagentCoordinator::Json>
SubagentCoordinator::interrupt(const std::string &target) {
  EventSink callback;
  Json event;
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!impl_->persistence_error.empty())
    return failure(ErrorCode::io_error, impl_->persistence_error);
  auto worker = impl_->find_locked(target);
  if (!worker)
    return failure(ErrorCode::not_found, "unknown child: " + target);
  if (!worker->running) {
    if (!worker->wake && worker->status != "idle")
      return Json{{"child_id", worker->id},
                  {"interrupted", false},
                  {"status", worker->status}};
    worker->wake = false;
    worker->interrupted = true;
    worker->status = "interrupted";
    ++worker->event_id;
    ++impl_->completion_count;
    event = Json{{"type", "subagent_complete"},
                 {"child_id", worker->id},
                 {"generation", worker->generation},
                 {"event_id", worker->event_id},
                 {"status", "interrupted"},
                 {"interrupted", true},
                 {"queued", true}};
    impl_->events.push_back(event);
    if (!impl_->persist_locked())
      return failure(ErrorCode::io_error, impl_->persistence_error);
    callback = impl_->sink;
    impl_->state_cv.notify_all();
    lock.unlock();
    if (callback) {
      try {
        callback(event);
      } catch (...) {
      }
    }
    return Json{
        {"child_id", worker->id}, {"interrupted", true}, {"status", "interrupted"}};
  }
  worker->interrupted = true;
  if (worker->cancellation)
    worker->cancellation->cancel();
  worker->wake = false;
  if (!impl_->persist_locked())
    return failure(ErrorCode::io_error, impl_->persistence_error);
  return Json{
      {"child_id", worker->id}, {"interrupted", true}, {"status", "interrupting"}};
}

qt::foundation::Result<SubagentCoordinator::Json>
SubagentCoordinator::list() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->persistence_error.empty())
    return failure(ErrorCode::io_error, impl_->persistence_error);
  Json result = Json::array();
  for (const auto &[id, worker] : impl_->workers) {
    (void)id;
    result.push_back(impl_->worker_json(*worker));
  }
  return Json{{"children", std::move(result)}};
}

qt::foundation::Result<SubagentCoordinator::Json>
SubagentCoordinator::wait(const std::string &target,
                          std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  if (!impl_->persistence_error.empty())
    return failure(ErrorCode::io_error, impl_->persistence_error);
  if (target.empty()) {
    const auto before = impl_->completion_count;
    const bool done = impl_->state_cv.wait_for(lock, timeout, [&] {
      return impl_->stopping || !impl_->events.empty() ||
             impl_->completion_count > before;
    });
    return Json{{"completed", done},
                {"status", done ? "completed" : "timeout"}};
  }
  auto worker = impl_->find_locked(target);
  if (!worker)
    return failure(ErrorCode::not_found, "unknown child: " + target);
  const bool done = impl_->state_cv.wait_for(
      lock, timeout, [&] { return !worker->running && !worker->wake; });
  return Json{{"child_id", worker->id},
              {"completed", done},
              {"status", worker->status},
              {"event_id", worker->event_id}};
}

std::vector<SubagentCoordinator::Json> SubagentCoordinator::take_events() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->persistence_error.empty()) return {};
  auto result = std::move(impl_->events);
  impl_->events.clear();
  if (!impl_->stopping && !impl_->persist_locked()) {
    impl_->events = std::move(result);
    return {};
  }
  return result;
}
qt::foundation::Status
SubagentCoordinator::acknowledge_event(const std::string &child_id,
                                       std::uint64_t event_id) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->persistence_error.empty() || impl_->lock_fd < 0)
    return qt::foundation::Status::failure(ErrorCode::io_error,
        impl_->persistence_error.empty() ? "coordinator no longer owns its index" : impl_->persistence_error);
  const auto previous = impl_->events;
  impl_->events.erase(
      std::remove_if(impl_->events.begin(), impl_->events.end(),
                     [&](const Json &event) {
                       return event.value("child_id", "") == child_id &&
                              event.value("event_id", 0ULL) == event_id;
                     }),
      impl_->events.end());
  if (!impl_->persist_locked()) {
    impl_->events = previous;
    return qt::foundation::Status::failure(ErrorCode::io_error,
                                           impl_->persistence_error);
  }
  return qt::foundation::Status::success();
}
bool SubagentCoordinator::has_active_workers() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping)
    return false;
  if (impl_->active != 0)
    return true;
  for (const auto &[id, worker] : impl_->workers)
    if (worker->running || worker->wake)
      return true;
  return false;
}
void SubagentCoordinator::shutdown() noexcept {
  if (impl_)
    impl_->shutdown();
}

} // namespace qt::app::runtime
