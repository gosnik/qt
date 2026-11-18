#include "qt/tui/core/component.hpp"
#include "qt/tui/render/markdown.hpp"
#include "qt/tui/render/text.hpp"
#include "qt/tui/terminal/notcurses_backend.hpp"
#include "qt/tui/terminal/terminal.hpp"
#include "qt/tui/terminal/ansi.hpp"
#include "qt/tui/widgets/overlay.hpp"
#include "qt/tui/widgets/messages/assistant_message.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace qt::tui;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

class Fixed final : public Component {
 public:
  explicit Fixed(std::string value) : value_(std::move(value)) {}
  qt::foundation::Result<RenderBuffer> render(const RenderConstraints& constraints) const override {
    return make_buffer({value_}, constraints);
  }
 private:
  std::string value_;
};

bool active_red_at(std::string_view line, std::size_t stop) {
  bool red = false;
  for (std::size_t i = 0; i < stop;) {
    if (line.substr(i, 5) == "\x1b[31m") { red = true; i += 5; }
    else if (line.substr(i, 4) == "\x1b[0m") { red = false; i += 4; }
    else ++i;
  }
  return red;
}

std::string joined(const RenderBuffer& buffer) {
  std::string result;
  for (const auto& line : buffer.lines) result += line;
  return result;
}

void require_streaming_markdown_matches_fresh(std::string_view source, std::size_t width) {
  Markdown streaming("");
  std::string accumulated;
  for (const char ch : source) {
    accumulated.push_back(ch);
    streaming.set_text(accumulated);
    Markdown fresh(accumulated);
    const auto cached = streaming.render({width, 200});
    const auto baseline = fresh.render({width, 200});
    require(cached && baseline, "streaming Markdown render failed");
    if (cached.value().lines != baseline.value().lines) {
      throw std::runtime_error("append-aware Markdown reflow diverged at: " + accumulated);
    }
  }
}

std::unique_ptr<AssistantMessageComponent> make_snapshot_component() {
  qt::ai::Message message;
  message.role = qt::ai::MessageRole::assistant;
  message.content = {qt::ai::ThinkingContent{"original thinking", std::nullopt, false},
                     qt::ai::TextContent{"original answer", std::nullopt}};
  auto component = std::make_unique<AssistantMessageComponent>(&message);
  message.content = {qt::ai::TextContent{"mutated", std::nullopt}};
  return component;
}
}
int main() {
  for (const std::string& text : {std::string(10000, 'x'),
       std::string("one two three\nfour five six\nseven eight"),
       std::string("\x1b[31mone two three four five six\x1b[0m")}) {
    Text component(text, 1, 1);
    const auto full = component.render({12, 2000});
    require(static_cast<bool>(full), "full text render failed");
    for (std::size_t height : {1U, 3U, 6U}) {
      const auto bounded = component.render({12, height});
      require(bounded && bounded.value().lines == std::vector<std::string>(
          full.value().lines.begin(), full.value().lines.begin() +
              static_cast<std::ptrdiff_t>(std::min(height, full.value().lines.size()))),
          "viewport-bounded text wrapping differs from full rendering");
    }
    component.set_text("changed");
    require(joined(component.render({12, 6}).value()).find("changed") != std::string::npos,
            "text render cache retained stale content");
  }
  const std::string open_link = "\x1b]8;;https://example.test\x1b\\";
  const std::string close_link = "\x1b]8;;\x1b\\";
  const auto link_sequence = extract_ansi_sequence(open_link, 0);
  require(link_sequence && is_osc8_sequence(open_link, *link_sequence), "OSC8 classification");
  OverlaySurface linked(std::make_shared<Fixed>(open_link + "abcdef" + close_link));
  OverlayOptions link_options;
  link_options.width = OverlaySize::columns(2);
  link_options.col = OverlaySize::columns(2);
  link_options.row = OverlaySize::columns(0);
  link_options.frame = false;
  linked.show_overlay(std::make_shared<Fixed>("ZZ"), link_options);
  const auto link_output = linked.render({6, 1});
  require(link_output && link_output.value().lines[0].find(open_link + "ef") != std::string::npos,
          "overlay tail lost hyperlink");

  Markdown wrapped_link(open_link + "abcdefgh" + close_link);
  const auto wrapped = wrapped_link.render({4, 8});
  require(static_cast<bool>(wrapped), "wrapped OSC8 Markdown failed");
  bool saw_tail = false;
  for (const auto& line : wrapped.value().lines) {
    if (line.find("efgh") != std::string::npos) {
      saw_tail = true;
      require(line.find(open_link) != std::string::npos, "wrapped OSC8 state lost");
    }
  }
  require(saw_tail, "wrapped OSC8 tail missing");
  require_streaming_markdown_matches_fresh(
      "plain words that wrap repeatedly across a narrow terminal", 11);
  require_streaming_markdown_matches_fresh(
      "[reference][id]\n\n[id]: https://example.test\n\n```cpp\nvalue\n```\n\n| a | b |\n| - | - |\n| 1 | 2 |", 14);
  require_streaming_markdown_matches_fresh(
      std::string("before ") + open_link + "hyperlink content spanning lines" + close_link + " after", 9);
  for (const auto& escape : {std::string("\x1b"), std::string("\x1b["),
                            std::string("\x1b]"), std::string("\x1b_"), std::string("\x1b?")}) {
    const std::string input = "x" + escape;
    require(static_cast<bool>(measure_line_width(input)), "malformed width failed");
    require(static_cast<bool>(truncate_line(input, 8)), "malformed truncation failed");
    require(static_cast<bool>(Text(input).render({8, 2})), "malformed Text failed");
    require(static_cast<bool>(Markdown(input).render({8, 2})), "malformed Markdown failed");
  }
  const std::string malformed = "x\x1b";
  require(measure_line_width(malformed).value() == 1, "malformed ESC width");
  require(truncate_line(malformed, 8).value() == "x", "malformed ESC truncation");
  require(static_cast<bool>(Text(malformed).render({8, 2})), "Text malformed ESC");
  require(static_cast<bool>(Markdown(malformed).render({8, 2})), "Markdown malformed ESC");
  const auto plan = notcurses_render_plan(RenderBuffer{1, {"x" + std::string(terminal_line_reset_sequence)}});
  for (const auto& op : plan.operations) if (op.kind == NotcursesRenderOperationKind::put_text) require(op.text.find('\x1b') == std::string::npos, "notcurses control leaked");
  require(measure_line_width("a\xE1\xAA\xB0").value() == 1, "combining-mark width");
  OverlaySurface surface(std::make_shared<Fixed>("\x1b[31mabcdef\x1b[0m")); OverlayOptions options; options.width = OverlaySize::columns(2); options.col = OverlaySize::columns(2); options.row = OverlaySize::columns(0); options.frame = false; surface.show_overlay(std::make_shared<Fixed>("ZZ"), options);
  const auto output = surface.render({6, 1}); require(static_cast<bool>(output), "overlay render"); const auto tail = output.value().lines[0].find("ef"); require(tail != std::string::npos && active_red_at(output.value().lines[0], tail), "overlay tail lost SGR");

  const auto assistant = make_snapshot_component();
  const auto initial = assistant->render({40, 10});
  require(static_cast<bool>(initial) && joined(initial.value()).find("original answer") != std::string::npos,
          "assistant snapshot lost original content");
  require(joined(initial.value()).find("mutated") == std::string::npos,
          "assistant retained caller mutation");
  assistant->set_hide_thinking(true);
  const auto hidden = assistant->render({40, 10});
  require(static_cast<bool>(hidden) && joined(hidden.value()).find("original answer") != std::string::npos,
          "assistant hide-thinking rebuild lost snapshot");
  qt::ai::Message changing;
  changing.content = {qt::ai::ThinkingContent{"same block", std::nullopt, false}};
  AssistantMessageComponent styled(&changing, false, "Thinking...",
      [](std::string_view text) { return "\x1b[2m" + std::string(text) + "\x1b[0m"; });
  require(static_cast<bool>(styled.render({40, 10})), "thinking render failed");
  changing.content = {qt::ai::TextContent{"normal answer", std::nullopt}};
  styled.update_content(changing);
  const auto changed_kind = styled.render({40, 10});
  require(changed_kind && joined(changed_kind.value()).find("\x1b[2m") == std::string::npos,
          "normal text retained the previous thinking renderer style");
}
