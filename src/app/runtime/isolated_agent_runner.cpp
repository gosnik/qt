#include "qt/app/runtime/isolated_agent_runner.hpp"

#include <string_view>
#include <utility>

namespace qt::app::runtime {
namespace {

std::string assistant_text_from_message(const qt::ai::Json& message) {
  if (!message.is_object() || !message.contains("content") ||
      !message.at("content").is_array()) {
    return {};
  }

  std::string output;
  for (const auto& content : message.at("content")) {
    if (!content.is_object() ||
        content.value("type", std::string{}) != "text" ||
        !content.contains("text") || !content.at("text").is_string()) {
      continue;
    }
    if (!output.empty()) {
      output.push_back('\n');
    }
    output += content.at("text").get<std::string>();
  }
  return output;
}

void parse_json_lines(std::string_view stdout_text,
                      std::vector<qt::ai::Json>* events,
                      qt::ai::Json& final_messages,
                      std::string& final_output,
                      std::vector<std::string>* diagnostics) {
  std::size_t line_number = 0U;
  std::size_t offset = 0U;
  while (offset <= stdout_text.size()) {
    ++line_number;
    const std::size_t newline = stdout_text.find('\n', offset);
    std::string_view line = stdout_text.substr(
        offset, newline == std::string_view::npos ? std::string_view::npos
                                                 : newline - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1U);
    }
    if (!line.empty()) {
      try {
        qt::ai::Json event = qt::ai::Json::parse(line);
        if (events != nullptr) {
          events->push_back(event);
        }
        if (event.is_object() &&
            event.value("type", std::string{}) == "agent_end" &&
            event.contains("messages") && event.at("messages").is_array()) {
          final_messages = event.at("messages");
          for (const auto& message : final_messages) {
            const std::string text = assistant_text_from_message(message);
            if (!text.empty()) {
              final_output = text;
            }
          }
        }
      } catch (const qt::ai::Json::parse_error& error) {
        if (diagnostics != nullptr) {
          diagnostics->push_back("ignored non-JSON child stdout line " +
                                 std::to_string(line_number) + ": " +
                                 error.what());
        }
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    offset = newline + 1U;
  }
}

}  // namespace

IsolatedAgentRunner::IsolatedAgentRunner(IsolatedAgentRunnerOptions options)
    : options_(std::move(options)) {
  if (options_.executable.empty()) {
    options_.executable = "qt";
  }
}

qt::foundation::Result<IsolatedAgentResult> IsolatedAgentRunner::run(
    const IsolatedAgentRequest& request) const {
  qt::foundation::ProcessRequest process_request;
  process_request.mode = qt::foundation::ProcessLaunchMode::argv;
  process_request.argv.reserve(1U + options_.executable_arguments.size() +
                               request.arguments.size());
  process_request.argv.push_back(options_.executable);
  process_request.argv.insert(process_request.argv.end(),
                              options_.executable_arguments.begin(),
                              options_.executable_arguments.end());
  process_request.argv.insert(process_request.argv.end(),
                              request.arguments.begin(),
                              request.arguments.end());
  process_request.stdin_text = request.stdin_payload;
  process_request.cwd = request.cwd;
  process_request.environment = request.environment;
  process_request.timeout_ms = request.timeout_ms;
  process_request.cancellation_token = request.cancellation_token;
  if (request.output_callback) {
    process_request.output_callback =
        [&callback = request.output_callback](
            const qt::foundation::ProcessOutputUpdate& update) {
          callback(parse_output_update(update.stdout_text, update.stderr_text));
        };
  }

  const auto process = qt::foundation::run_process(process_request);
  if (!process) {
    return qt::foundation::Result<IsolatedAgentResult>(process.error());
  }

  IsolatedAgentResult result;
  result.exit_code = process.value().exit_code;
  result.cancelled = process.value().cancelled;
  result.timed_out = process.value().timed_out;
  result.stdout_text = process.value().stdout_text;
  result.stderr_text = process.value().stderr_text;
  parse_json_lines(result.stdout_text, &result.events, result.final_messages,
                   result.final_output, &result.diagnostics);
  return qt::foundation::Result<IsolatedAgentResult>::success(
      std::move(result));
}

IsolatedAgentOutputUpdate IsolatedAgentRunner::parse_output_update(
    std::string stdout_text, std::string stderr_text) {
  IsolatedAgentOutputUpdate update;
  update.stdout_text = std::move(stdout_text);
  update.stderr_text = std::move(stderr_text);
  parse_json_lines(update.stdout_text, nullptr, update.final_messages,
                   update.final_output, nullptr);
  return update;
}

}  // namespace qt::app::runtime
