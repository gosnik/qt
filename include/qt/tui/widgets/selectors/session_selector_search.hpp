#pragma once

#include "qt/tui/layout/box.hpp"
#include "qt/tui/core/component.hpp"
#include "qt/tui/input/input.hpp"
#include "qt/tui/widgets/selectors/select.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qt::tui {

struct SessionSelectorSearchItem {
  std::string session_id;
  std::string label;
  std::string path;
};

class SessionSelectorSearchComponent final : public Component, public Focusable {
 public:
  using SelectCallback = std::function<void(SessionSelectorSearchItem item)>;
  using CancelCallback = std::function<void()>;

  SessionSelectorSearchComponent(std::vector<SessionSelectorSearchItem> sessions,
                                 SelectCallback on_select,
                                 CancelCallback on_cancel);

  [[nodiscard]] foundation::Result<RenderBuffer> render(
      const RenderConstraints& constraints) const override;
  void handle_input(std::string_view bytes) override;
  void set_focused(bool focused) override;
  [[nodiscard]] bool focused() const noexcept override;

  [[nodiscard]] Input& search_input() noexcept;
  [[nodiscard]] const std::vector<SessionSelectorSearchItem>& filtered_sessions()
      const noexcept;

 private:
  void apply_filter();
  void rebuild();

  std::vector<SessionSelectorSearchItem> sessions_;
  std::vector<SessionSelectorSearchItem> filtered_;
  std::string last_filter_query_;
  bool filter_initialized_ = false;
  SelectCallback on_select_;
  CancelCallback on_cancel_;
  std::shared_ptr<Input> search_input_;
  std::shared_ptr<Select> select_;
  std::shared_ptr<Box> box_;
  bool focused_ = false;
};

}  // namespace qt::tui
