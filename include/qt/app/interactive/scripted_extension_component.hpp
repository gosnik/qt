#pragma once

#include "qt/tui/core/component.hpp"

#include <functional>
#include <mutex>
#include <optional>

#include <nlohmann/json.hpp>

namespace qt::app {

// Adapter for an extension-owned component. The callback receives a JSON
// render/input event and returns the extension's response. It is intentionally
// a TUI component plus InputHandler so OverlaySurface can route terminal input
// to it without knowing anything about the extension runtime.
class ScriptedExtensionComponent final : public qt::tui::Component,
                                          public qt::tui::InputHandler {
 public:
  using Json = nlohmann::json;
  using Callback = std::function<qt::foundation::Result<Json>(const Json&)>;
  using CompletionCallback =
      std::function<void(qt::foundation::Result<Json>)>;

  ScriptedExtensionComponent(Callback callback,
                             CompletionCallback on_complete = {});
  ~ScriptedExtensionComponent() override;

  ScriptedExtensionComponent(const ScriptedExtensionComponent&) = delete;
  ScriptedExtensionComponent& operator=(const ScriptedExtensionComponent&) =
      delete;

  [[nodiscard]] qt::foundation::Result<qt::tui::RenderBuffer> render(
      const qt::tui::RenderConstraints& constraints) const override;
  void handle_input(std::string_view bytes) override;

  // Releases callbacks and guarantees that completion is delivered at most
  // once. Safe to call repeatedly; the destructor calls it as a final guard.
  void dispose() noexcept;
  [[nodiscard]] bool completed() const noexcept;
  [[nodiscard]] bool disposed() const noexcept;

 private:
  void record_error(qt::foundation::Error error) const;
  void complete(qt::foundation::Result<Json> result);

  mutable std::mutex mutex_;
  mutable Callback callback_;
  mutable std::optional<qt::foundation::Error> error_;
  CompletionCallback on_complete_;
  bool completed_ = false;
  bool disposed_ = false;
};

}  // namespace qt::app
