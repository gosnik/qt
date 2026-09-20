#include "qt/app/interactive/scripted_extension_component.hpp"

#include "qt/tui/core/component.hpp"

#include <utility>

namespace qt::app {
namespace {

using Json = ScriptedExtensionComponent::Json;
using foundation::ErrorCode;
using foundation::Result;

Result<Json> invalid(std::string message) {
  return Result<Json>::failure(ErrorCode::invalid_argument, std::move(message));
}

}  // namespace

ScriptedExtensionComponent::ScriptedExtensionComponent(
    Callback callback, CompletionCallback on_complete)
    : callback_(std::move(callback)), on_complete_(std::move(on_complete)) {}

ScriptedExtensionComponent::~ScriptedExtensionComponent() { dispose(); }

Result<qt::tui::RenderBuffer> ScriptedExtensionComponent::render(
    const qt::tui::RenderConstraints& constraints) const {
  Callback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_) return Result<qt::tui::RenderBuffer>(*error_);
    if (disposed_ || !callback_) {
      return Result<qt::tui::RenderBuffer>::failure(
          ErrorCode::invalid_argument,
          "scripted extension component is disposed");
    }
    callback = callback_;
  }

  Json event{{"type", "render"},
             {"width", constraints.max_width},
             {"height", constraints.max_height}};
  auto response = callback(event);
  if (!response) {
    record_error(response.error());
    return Result<qt::tui::RenderBuffer>(response.error());
  }
  if (!response.value().is_object() || !response.value().contains("lines") ||
      !response.value().at("lines").is_array()) {
    auto failure = invalid("scripted extension render must return {lines: []}");
    record_error(failure.error());
    return failure.error();
  }

  std::vector<std::string> lines;
  try {
    for (const auto& line : response.value().at("lines")) {
      if (!line.is_string()) {
        auto failure = invalid("scripted extension render lines must be strings");
        record_error(failure.error());
        return failure.error();
      }
      lines.push_back(line.get<std::string>());
    }
  } catch (const Json::exception& error) {
    auto failure = invalid(std::string("invalid scripted extension render: ") +
                           error.what());
    record_error(failure.error());
    return failure.error();
  }
  return qt::tui::make_buffer(std::move(lines), constraints);
}

void ScriptedExtensionComponent::handle_input(std::string_view bytes) {
  Callback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disposed_ || completed_ || !callback_) return;
    callback = callback_;
  }

  Json event{{"type", "input"}, {"data", std::string(bytes)}};
  auto response = callback(event);
  if (!response) {
    record_error(response.error());
    return;
  }
  if (!response.value().is_object()) {
    record_error(invalid("scripted extension input must return an object").error());
    return;
  }
  if (response.value().contains("handled") &&
      !response.value().at("handled").is_boolean()) {
    record_error(invalid("scripted extension input handled must be boolean").error());
    return;
  }
  if (response.value().contains("done")) {
    complete(Result<Json>::success(response.value().at("done")));
  }
}

void ScriptedExtensionComponent::dispose() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (disposed_) return;
  disposed_ = true;
  callback_ = {};
  on_complete_ = {};
}

bool ScriptedExtensionComponent::completed() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_;
}

bool ScriptedExtensionComponent::disposed() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return disposed_;
}

void ScriptedExtensionComponent::record_error(qt::foundation::Error error) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!error_) error_ = std::move(error);
}

void ScriptedExtensionComponent::complete(Result<Json> result) {
  CompletionCallback callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disposed_ || completed_) return;
    completed_ = true;
    callback = on_complete_;
  }
  if (callback) callback(std::move(result));
}

}  // namespace qt::app
