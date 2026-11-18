#pragma once

#include <string>
#include <string_view>

namespace qt::tui::style {

inline constexpr std::string_view reset = "\x1b[0m";

inline std::string sgr(std::string_view code, std::string_view text) {
  return std::string("\x1b[") + std::string(code) + "m" + std::string(text) +
         std::string(reset);
}

inline std::string bold(std::string_view text) { return sgr("1", text); }

inline std::string italic(std::string_view text) { return sgr("3", text); }

inline std::string dim(std::string_view text) { return sgr("2", text); }

inline std::string cyan(std::string_view text) { return sgr("36", text); }

inline std::string green(std::string_view text) { return sgr("32", text); }

inline std::string red(std::string_view text) { return sgr("31", text); }

inline std::string magenta(std::string_view text) { return sgr("35", text); }

inline std::string reverse(std::string_view text) { return sgr("7", text); }

inline std::string foreground_256(int color, std::string_view text) {
  return sgr("38;5;" + std::to_string(color), text);
}

inline std::string background_256(int color, std::string_view text) {
  const std::string background = "48;5;" + std::to_string(color);
  const std::string start = "\x1b[" + background + "m";
  std::string output = start;
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto reset_offset = text.find(reset, offset);
    if (reset_offset == std::string_view::npos) {
      output.append(text.substr(offset));
      break;
    }
    output.append(text.substr(offset, reset_offset - offset));
    output.append(reset);
    output.append(start);
    offset = reset_offset + reset.size();
  }
  output.append(reset);
  return output;
}

}  // namespace qt::tui::style
