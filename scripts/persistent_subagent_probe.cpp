#include "qt/app/runtime/subagent_coordinator.hpp"
#include "qt/ai/images.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <csignal>
#include <mutex>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Json = qt::ai::Json;
using qt::app::runtime::PersistentSubagentRequest;
using qt::app::runtime::SubagentCoordinator;
using qt::app::runtime::SubagentCoordinatorOptions;

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
std::string text_file(const std::filesystem::path& p) {
  std::ifstream in(p); return {std::istreambuf_iterator<char>(in), {}};
}
std::string id_of(const Json& result) {
  for (const char* key : {"id", "child_id", "worker_id", "workerId", "session_id"})
    if (result.is_object() && result.contains(key) && result.at(key).is_string())
      return result.at(key).get<std::string>();
  throw std::runtime_error("spawn response has no worker id: " + result.dump());
}
std::string path_of(const Json& result) {
  require(result.is_object() && result.contains("path") && result.at("path").is_string(),
          "spawn response has no session path: " + result.dump());
  return result.at("path").get<std::string>();
}
PersistentSubagentRequest request(std::string task, Json fork = "none") {
  static int next_name = 1;
  PersistentSubagentRequest r;
  r.name = "probe-" + std::to_string(next_name++);
  r.agent = "probe"; r.task = task; r.fork = std::move(fork);
  r.process.arguments = {"--task", task};
  return r;
}
void check_success(const qt::foundation::Result<Json>& r, const char* what) {
  require(static_cast<bool>(r), std::string(what) + " failed: " +
          (r ? "" : r.error().summary()));
}

int run(const std::filesystem::path& child, const std::filesystem::path& root) {
  std::filesystem::remove_all(root); std::filesystem::create_directories(root);
  const auto trace = root / "trace.log";
  SubagentCoordinatorOptions options;
  options.directory = root; options.parent_session_id = "parent-1";
  options.concurrency = 1; options.runner.executable = child.string();
  options.runner.executable_arguments = {"--trace", trace.string()};
  SubagentCoordinator coordinator(options);
  {
    SubagentCoordinator contended(options);
    require(!contended.list(), "concurrent coordinator acquired the same index");
    require(contended.take_events().empty(), "locked coordinator exposed events");
    require(!std::filesystem::exists(root / "children.json"), "locked coordinator overwrote the index");
  }
  std::mutex mutex; std::condition_variable cv; std::vector<Json> events;
  coordinator.set_event_sink([&](const Json& event) {
    std::lock_guard lock(mutex); events.push_back(event); cv.notify_all();
  });
  coordinator.set_context_snapshot([] {
    qt::ai::Message user; user.role = qt::ai::MessageRole::user;
    user.content.emplace_back(qt::ai::TextContent{"parent message", std::nullopt});
    qt::ai::Message assistant; assistant.role = qt::ai::MessageRole::assistant;
    assistant.content.emplace_back(qt::ai::TextContent{"parent reply one", std::nullopt});
    assistant.content.emplace_back(qt::ai::ToolCallContent{"tool-1", "probe", Json{{"x", 1}}});
    qt::ai::Message user_two; user_two.role = qt::ai::MessageRole::user;
    user_two.content.emplace_back(qt::ai::TextContent{"parent message two", std::nullopt});
    qt::ai::Message assistant_two; assistant_two.role = qt::ai::MessageRole::assistant;
    assistant_two.content.emplace_back(qt::ai::TextContent{"parent reply two", std::nullopt});
    return std::vector<qt::ai::Message>{user, assistant, user_two, assistant_two};
  });

  const auto spawn_started = std::chrono::steady_clock::now();
  auto first = coordinator.spawn(request("sleep=250 first")); check_success(first, "spawn");
  const auto spawn_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - spawn_started);
  const std::string id = id_of(first.value());
  require(spawn_elapsed < std::chrono::milliseconds(200),
          "spawn waited for child completion");
  for (int attempt = 0; attempt < 40 && !std::filesystem::exists(trace); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  require(text_file(trace).find("end sleep=250 first") == std::string::npos,
          "spawn waited for child completion");
  auto sent = coordinator.send(id, "queued while idle", false); check_success(sent, "idle send");
  require(text_file(trace).find("queued while idle") == std::string::npos,
          "wake=false idle send launched a child");
  check_success(coordinator.send(id, "followup", true), "wake send");
  auto waited = coordinator.wait(id, std::chrono::seconds(4)); check_success(waited, "wait");
  require(waited.value().is_object(), "wait result is not an object");
  { std::unique_lock lock(mutex); cv.wait_for(lock, std::chrono::seconds(2), [&] {
      return events.size() >= 1; }); }
  std::size_t completions = 0;
  std::set<std::uint64_t> completion_generations;
  for (const auto& event : events)
    if (event.value("type", "") == "subagent_complete") {
      ++completions;
      completion_generations.insert(event.value("generation", 0ULL));
    }
  require(completions == completion_generations.size() && completions >= 1,
          "completion event was not delivered exactly once per generation");
  const std::string trace_text = text_file(trace);
  require(trace_text.find("followup") != std::string::npos, "mailbox followup was lost");

  auto interrupted = coordinator.spawn(request("sleep=1000 interrupt")); check_success(interrupted, "interrupt spawn");
  const std::string reused = id_of(interrupted.value());
  check_success(coordinator.interrupt(reused), "interrupt");
  check_success(coordinator.send(reused, "reuse", true), "interrupt reuse");
  check_success(coordinator.wait(reused, std::chrono::seconds(4)), "reuse wait");

  auto timeout = coordinator.spawn(request("sleep=1000 timeout")); check_success(timeout, "timeout spawn");
  const std::string timeout_id = id_of(timeout.value());
  auto timed = coordinator.wait(timeout_id, std::chrono::milliseconds(20));
  require(timed && !timed.value().value("completed", true),
          "wait timeout unexpectedly completed");

  struct ForkRecord { std::string id; std::string path; Json fork; };
  std::vector<ForkRecord> forked_ids;
  for (const Json& fork : {Json("none"), Json("all"), Json(2)}) {
    auto forked = coordinator.spawn(request("fork-content", fork)); check_success(forked, "fork spawn");
    const std::string fork_id = id_of(forked.value());
    const std::string fork_path = path_of(forked.value());
    check_success(coordinator.wait(fork_id, std::chrono::seconds(4)), "fork wait");
    forked_ids.push_back({fork_id, fork_path, fork});
  }
  for (const auto& record : forked_ids) {
    const std::string session = text_file(record.path);
    require(session.find("\"type\":\"session\"") != std::string::npos,
            "fork session header missing");
    if (record.fork == "none") {
      require(session.find("parent message") == std::string::npos,
              "none fork unexpectedly copied parent messages");
    } else {
      require(session.find("parent message") != std::string::npos &&
                  (session.find("toolCall") != std::string::npos ||
                   session.find("tool_call") != std::string::npos),
              "fork JSONL lost parent text or tool call content");
      if (record.fork.is_number_integer()) {
        require(session.find("parent message two") != std::string::npos &&
                    session.find("parent message one") == std::string::npos,
                "last-N fork did not begin at the requested user turn");
      }
    }
  }
  auto malformed = coordinator.spawn(request("malformed"));
  require(static_cast<bool>(malformed), "malformed child should be accepted as a process");
  const auto malformed_wait = coordinator.wait(id_of(malformed.value()), std::chrono::seconds(4));
  check_success(malformed_wait, "malformed wait");
  require(malformed_wait.value().value("status", "") == "failed", "malformed output was reported as success");

  auto bad_fork = coordinator.spawn(request("bad-fork", Json("bogus")));
  require(!bad_fork, "unknown fork mode was accepted");

  auto descendant = coordinator.spawn(request("descendant sleep=1000"));
  check_success(descendant, "descendant spawn");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  coordinator.shutdown();
  require(!coordinator.has_active_workers(), "shutdown left active workers");
  const std::string after_shutdown = text_file(trace);
  const auto marker = after_shutdown.rfind("descendant ");
  require(marker != std::string::npos, "child did not create a descendant");
  const auto pid_start = marker + std::string("descendant ").size();
  const auto pid_end = after_shutdown.find('\n', pid_start);
  const pid_t descendant_pid = static_cast<pid_t>(std::stol(
      after_shutdown.substr(pid_start, pid_end - pid_start)));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  require(::kill(descendant_pid, 0) != 0, "shutdown left descendant alive");
  std::filesystem::path state = root / "state";
  bool found_state = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().filename() == "trace.log" ||
        entry.path().filename() == "children.lock") continue;
    found_state = true;
    const auto permissions = entry.status().permissions();
    require((permissions & std::filesystem::perms::group_read) ==
                std::filesystem::perms::none &&
                (permissions & std::filesystem::perms::others_read) ==
                std::filesystem::perms::none,
            "persistent coordinator state is readable by group/other");
    const std::string contents = text_file(entry.path());
    require(contents.find("parent-1") != std::string::npos,
            "persisted state lost parent session lineage");
  }
  require(found_state, "coordinator did not persist state");
  SubagentCoordinator restored(options);
  auto listed = restored.list(); check_success(listed, "restore list");
  require(listed.value().is_object() && listed.value().at("children").is_array() &&
              !listed.value().at("children").empty(), "restore list malformed");
  const auto& restored_worker = listed.value().at("children").front();
  require(restored_worker.value("id", "").rfind("/root/", 0) == 0 &&
              restored_worker.value("path", "").find("session.jsonl") != std::string::npos &&
              restored_worker.contains("permissions") && restored_worker.contains("mailbox"),
          "restored worker identity/state is incomplete");
  const std::string restored_id = restored_worker.at("id").get<std::string>();
  check_success(restored.send(restored_id, "restored followup", true), "restored followup");
  check_success(restored.wait(restored_id, std::chrono::seconds(4)), "restored wait");
  restored.shutdown();
  { std::ofstream broken(root / "children.json"); broken << "{broken-index"; }
  SubagentCoordinator corrupt(options);
  require(!corrupt.list(), "corrupt index was silently accepted");
  require(corrupt.take_events().empty(), "corrupt index exposed events");
  require(text_file(root / "children.json") == "{broken-index", "corrupt index was overwritten");
  return 0;
}
}  // namespace

// The coordinator only serializes text/tool messages. Keep this probe link
// narrow by providing the image parser symbol used by the general model JSON
// serializer without pulling the image codec/HTTP implementation into it.
namespace qt::ai {
std::optional<ImageDataUrl> parse_image_data_url(const std::string&) {
  return std::nullopt;
}
}  // namespace qt::ai

int main(int argc, char** argv) {
  try { require(argc == 3, "usage: probe CHILD RUNTIME_DIR"); return run(argv[1], argv[2]); }
  catch (const std::exception& error) { std::cerr << "persistent subagent probe failed: " << error.what() << '\n'; return 1; }
}
