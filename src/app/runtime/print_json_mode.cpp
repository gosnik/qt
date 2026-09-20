#include "qt/app/runtime/print_json_mode.hpp"

#include "qt/app/runtime/initial_message.hpp"
#include "qt/app/runtime/noninteractive_mode.hpp"
#include "qt/app/runtime/runtime.hpp"
#include "qt/app/runtime/extension_runtime_binding.hpp"
#include "qt/app/runtime/subagent_runtime.hpp"
#include "qt/app/runtime/subagent_coordinator.hpp"
#include "qt/app/runtime/extension_lifecycle_runner.hpp"
#include "qt/app/cli/startup.hpp"
#include "qt/app/export/stream_output.hpp"
#include "qt/foundation/logging.hpp"
#include "qt/harness/session_repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace qt::app {
namespace {

class SessionFinalizer {
 public:
  explicit SessionFinalizer(std::function<foundation::Status()> finish)
      : finish_(std::move(finish)) {}
  ~SessionFinalizer() {
    try {
      const auto status = finish();
      if (!status) std::cerr << "qt: " << status.error().message << '\n';
    } catch (const std::exception& error) {
      std::cerr << "qt: extension shutdown failed: " << error.what() << '\n';
    }
  }
  foundation::Status finish() {
    if (!finish_) return foundation::Status::success();
    auto finish = std::move(finish_);
    return finish();
  }
 private:
  std::function<foundation::Status()> finish_;
};

std::string default_cwd() {
  return std::filesystem::current_path().generic_string();
}

bool json_stream_event_visible(const ai::Json& event) {
  if (!event.is_object()) {
    return true;
  }

  const auto type_it = event.find("type");
  if (type_it == event.end() || !type_it->is_string()) {
    return true;
  }

  const auto& type = type_it->get_ref<const std::string&>();
  return type != "provider_request" && type != "provider_event";
}

StreamOutputPlan plan_json_output(const NoninteractiveRunResult& run) {
  StreamOutputPlan output;
  output.exit_code = run.turn.ok ? EXIT_SUCCESS : EXIT_FAILURE;
  for (const auto& event : run.json_events) {
    if (!json_stream_event_visible(event)) {
      continue;
    }
    output.stdout_chunks.push_back(event.dump() + "\n");
  }
  return output;
}

}  // namespace

int run_print_or_json_mode(
    const ReferenceCliArgs& parsed, ReferenceStartupMode mode,
    const ReferenceStartupSession& startup_session,
    const std::optional<std::string>& stdin_content) {
  auto log = qt::foundation::logger("app", "print_json_mode");
  log.info("mode_begin",
           {qt::foundation::log_field(
                "mode", mode == ReferenceStartupMode::json ? "json" : "print"),
            qt::foundation::log_field("messages",
                                      static_cast<std::uint64_t>(
                                          parsed.messages.size())),
            qt::foundation::log_field(
                "stdin", stdin_content.has_value())});
  const auto startup_settings = load_startup_settings(startup_session);
  if (!startup_settings) {
    log.error("settings_load_failed",
              {qt::foundation::log_field("error",
                                         startup_settings.error().message)});
    std::cerr << "qt: " << startup_settings.error().message << '\n';
    return EXIT_FAILURE;
  }

  PrepareInitialMessageRequest initial_request;
  initial_request.messages = parsed.messages;
  initial_request.file_args = parsed.file_args;
  initial_request.stdin_content = stdin_content;
  initial_request.cwd = default_cwd();
  initial_request.auto_resize_images =
      startup_settings.value().image_auto_resize;
  const auto initial = prepare_initial_message(initial_request);
  StreamOutputPlan diagnostics_output;
  const bool has_initial_errors =
      append_initial_message_diagnostics(diagnostics_output, initial.diagnostics);
  emit_stream_output(diagnostics_output, std::cout, std::cerr);
  if (has_initial_errors) {
    log.warn("initial_message_diagnostics_failed");
    return EXIT_FAILURE;
  }
  auto runtime = make_startup_runtime(parsed, startup_session);
  if (!runtime) {
    log.error("runtime_create_failed",
              {qt::foundation::log_field("error", runtime.error().message)});
    std::cerr << "qt: " << runtime.error().message << '\n';
    return EXIT_FAILURE;
  }
  auto runtime_ptr = std::make_shared<ReferenceStartupRuntime>(
      std::move(runtime.value()));
  bind_extension_host_call(runtime_ptr, parsed);
  bind_subagent_runtime(runtime_ptr);
  std::size_t persisted_entries = load_startup_session_entries(startup_session).size();
  const auto persist_entries = [&]() -> foundation::Status {
    if (!startup_session.persistent || !startup_session.repository_path) {
      return foundation::Status::success();
    }
    const auto& entries = runtime_ptr->host.harness().session_manager().get_entries();
    harness::SessionRepository repository(*startup_session.repository_path);
    while (persisted_entries < entries.size()) {
      const auto appended = repository.append(
          repository_ready_session_entry(entries[persisted_entries]));
      if (!appended) return appended;
      ++persisted_entries;
    }
    return foundation::Status::success();
  };
  SessionFinalizer finalizer([&]() -> foundation::Status {
    const auto shutdown = run_extension_session_shutdown_hooks_subprocess(
        runtime_ptr->services.resource_bundle.extension_host,
        qt::extensions::ExtensionSessionShutdownReason::kQuit,
        std::nullopt);
    const auto persisted = persist_entries();
    if (!persisted) return persisted;
    if (!shutdown) return foundation::Status(shutdown.error());
    return foundation::Status::success();
  });
  // Runtime creation emits session_start before extensions are bound. Replay
  // the lifecycle hook through the bound registry without adding a duplicate
  // harness event.
  const auto session_started = run_extension_session_start_hooks_subprocess(
      runtime_ptr->services.resource_bundle.extension_host,
      qt::extensions::ExtensionSessionStartReason::kStartup, std::nullopt);
  if (!session_started) {
    log.warn("extension_session_start_failed",
             std::vector<qt::foundation::LogField>{qt::foundation::log_field(
                 "error", session_started.error().message)});
  }

  if (mode == ReferenceStartupMode::json) {
    auto header =
        session_header_json(runtime_ptr->host.session_state().metadata());
    if (startup_session.parent_repository_path.has_value()) {
      header["parentSession"] = *startup_session.parent_repository_path;
    }
    StreamOutputPlan header_output;
    header_output.stdout_chunks.push_back(header.dump() + "\n");
    emit_stream_output(header_output, std::cout, std::cerr);
  }

  std::vector<std::string> prompts;
  if (initial.initial_message.has_value() && !initial.initial_message->empty()) {
    prompts.push_back(*initial.initial_message);
  }
  prompts.insert(prompts.end(), initial.remaining_messages.begin(),
                 initial.remaining_messages.end());

  int last_exit_code = EXIT_SUCCESS;
  std::optional<StreamOutputPlan> deferred_print_output;
  std::vector<ai::Json> prompt_metadata(prompts.size(), ai::Json::object());
  for (std::size_t index = 0; ; ++index) {
    if (index == prompts.size()) {
      const auto coordinator = runtime_ptr->services.subagent_coordinator;
      if (!coordinator) break;
      auto events = coordinator->take_events();
      while (events.empty() && coordinator->has_active_workers()) {
        const auto waited = coordinator->wait("", std::chrono::milliseconds(1000));
        if (!waited) {
          std::cerr << "qt: " << waited.error().message << '\n';
          return EXIT_FAILURE;
        }
        events = coordinator->take_events();
      }
      if (events.empty()) break;
      for (const auto& event : events) {
        if (mode == ReferenceStartupMode::json) {
          std::cout << ai::Json{{"type", "subagent_event"}, {"subagent", event}}.dump() << '\n';
        }
        prompts.push_back(event.dump());
        prompt_metadata.push_back(ai::Json{{"role", "custom"},
            {"customType", "subagent_event"}, {"display", true}, {"details", event}});
      }
    }
    std::vector<ai::ImageContent> images;
    if (index == 0U) {
      images.reserve(initial.initial_images.size());
      for (const auto& image : initial.initial_images) {
        images.push_back(image.content);
      }
    }

    std::ostringstream message_id;
    message_id << "cli-message-" << (index + 1U);
    const auto run = run_noninteractive_prompt(
        runtime_ptr->host.harness(),
        NoninteractivePromptRequest{prompts[index], message_id.str(),
                                    std::move(images), prompt_metadata[index]});
    if (!run) {
      log.error("prompt_run_failed",
                {qt::foundation::log_field("index",
                                           static_cast<std::uint64_t>(index)),
                 qt::foundation::log_field("error", run.error().message)});
      std::cerr << "qt: " << run.error().message << '\n';
      return EXIT_FAILURE;
    }

    if (startup_session.persistent && startup_session.repository_path) {
      const auto appended = persist_entries();
      if (!appended) {
        log.error("session_append_failed",
                  {qt::foundation::log_field("error",
                                             appended.error().message)});
        std::cerr << "qt: " << appended.error().message << '\n';
        return EXIT_FAILURE;
      }
    }

    const auto rendered = plan_noninteractive_output(
        run.value(), NoninteractiveOutputMode::print);
    const auto json_rendered = plan_json_output(run.value());
    if (mode == ReferenceStartupMode::json) {
      emit_stream_output(json_rendered, std::cout, std::cerr);
    } else {
      deferred_print_output = rendered;
    }
    last_exit_code = mode == ReferenceStartupMode::json ? json_rendered.exit_code
                                                        : rendered.exit_code;
    if (last_exit_code != EXIT_SUCCESS) {
      log.warn("mode_exit",
               {qt::foundation::log_field("code", last_exit_code)});
      if (mode == ReferenceStartupMode::print) {
        emit_stream_output(rendered, std::cout, std::cerr);
      }
      return last_exit_code;
    }
  }

  if (mode == ReferenceStartupMode::print && deferred_print_output.has_value()) {
    emit_stream_output(*deferred_print_output, std::cout, std::cerr);
  }

  const auto session_shutdown = finalizer.finish();
  if (!session_shutdown) {
    log.warn("extension_session_shutdown_failed",
             std::vector<qt::foundation::LogField>{qt::foundation::log_field(
                 "error", session_shutdown.error().message)});
    std::cerr << "qt: " << session_shutdown.error().message << '\n';
    return EXIT_FAILURE;
  }

  log.info("mode_exit", {qt::foundation::log_field("code", last_exit_code)});
  return last_exit_code;
}

}  // namespace qt::app
