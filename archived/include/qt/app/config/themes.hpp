#pragma once

#include <qt/foundation/error.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace qt::app {

enum class ThemeColorMode {
  kTrueColor,
  kAnsi256,
};

using ThemeColorValue = std::variant<std::string, int>;

struct ParsedThemeDocument {
  std::string name;
  std::string canonical_name;
  std::map<std::string, ThemeColorValue> colors;
  std::map<std::string, ThemeColorValue> export_colors;
};

std::string canonicalize_theme_name(std::string_view name);

ThemeColorMode detect_theme_color_mode(
    const std::map<std::string, std::string>& environment);

const std::vector<std::string>& required_theme_color_tokens();

qt::foundation::Result<ParsedThemeDocument> parse_theme_document(
    std::string_view label, std::string_view content);

qt::foundation::Result<std::string> theme_color_to_hex(
    const ThemeColorValue& value, std::string_view default_hex = "");

qt::foundation::Result<std::string> theme_foreground_ansi(
    const ThemeColorValue& value, ThemeColorMode mode);

qt::foundation::Result<std::string> theme_background_ansi(
    const ThemeColorValue& value, ThemeColorMode mode);

std::string ansi256_to_hex(int index);

}  // namespace qt::app
