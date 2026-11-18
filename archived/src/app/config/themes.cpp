#include <qt/app/config/themes.hpp>

#include <nlohmann/json.hpp>
#include <uni_algo/case.h>
#include <uni_algo/norm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace qt::app {
namespace {

using Json = nlohmann::json;
using qt::foundation::ErrorCode;
using qt::foundation::Result;

constexpr std::array<const char*, 51> kRequiredThemeColorTokens = {
    "accent",         "border",        "borderAccent", "borderMuted",
    "success",        "error",         "warning",      "muted",
    "dim",            "text",          "thinkingText", "selectedBg",
    "userMessageBg",  "userMessageText", "customMessageBg",
    "customMessageText", "customMessageLabel", "toolPendingBg",
    "toolSuccessBg",  "toolErrorBg",   "toolTitle",    "toolOutput",
    "mdHeading",      "mdLink",        "mdLinkUrl",    "mdCode",
    "mdCodeBlock",    "mdCodeBlockBorder", "mdQuote",  "mdQuoteBorder",
    "mdHr",           "mdListBullet",  "toolDiffAdded", "toolDiffRemoved",
    "toolDiffContext", "syntaxComment", "syntaxKeyword", "syntaxFunction",
    "syntaxVariable", "syntaxString",  "syntaxNumber", "syntaxType",
    "syntaxOperator", "syntaxPunctuation", "thinkingOff",
    "thinkingMinimal", "thinkingLow",  "thinkingMedium", "thinkingHigh",
    "thinkingXhigh",  "bashMode"};

std::string trim_copy(std::string_view value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }

  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return std::string(value.substr(start, end - start));
}

bool is_valid_hex_color(std::string_view value) {
  if (value.size() != 7 || value.front() != '#') {
    return false;
  }
  for (std::size_t index = 1; index < value.size(); ++index) {
    if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
      return false;
    }
  }
  return true;
}

std::string normalize_hex_color(std::string_view value) {
  return una::cases::to_lowercase_utf8(
      una::norm::to_nfkc_utf8(std::string(value)));
}

Result<const Json*> require_object_field(const Json& json, const char* field_name,
                                         const char* expected) {
  const auto it = json.find(field_name);
  if (it == json.end()) {
    return Result<const Json*>::failure(
        ErrorCode::invalid_argument,
        std::string(field_name) + " is required");
  }
  if (!it->is_object()) {
    return Result<const Json*>::failure(
        ErrorCode::invalid_argument,
        std::string(field_name) + " must be " + expected);
  }
  return &*it;
}

Result<std::string> require_string_field(const Json& json, const char* field_name) {
  const auto it = json.find(field_name);
  if (it == json.end()) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        std::string(field_name) + " is required");
  }
  if (!it->is_string()) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        std::string(field_name) + " must be a string");
  }
  const std::string value = trim_copy(it->get<std::string>());
  if (value.empty()) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        std::string(field_name) + " must not be empty");
  }
  return value;
}

std::string error_path(std::string_view prefix, std::string_view key) {
  if (prefix.empty()) {
    return std::string(key);
  }
  return std::string(prefix) + "." + std::string(key);
}

Result<ThemeColorValue> parse_theme_color_value(
    const Json& value, const std::string& path, const Json* vars,
    std::set<std::string>& resolving_variables) {
  if (value.is_number_integer()) {
    const int index = value.get<int>();
    if (index < 0 || index > 255) {
      return Result<ThemeColorValue>::failure(
          ErrorCode::invalid_argument,
          path + " must be an integer from 0 to 255");
    }
    return ThemeColorValue{index};
  }

  if (!value.is_string()) {
    return Result<ThemeColorValue>::failure(
        ErrorCode::invalid_argument,
        path + " must be a string or an integer from 0 to 255");
  }

  const std::string string_value = trim_copy(value.get<std::string>());
  if (string_value.empty()) {
    return ThemeColorValue{std::string{}};
  }
  if (string_value.front() == '#') {
    if (!is_valid_hex_color(string_value)) {
      return Result<ThemeColorValue>::failure(
          ErrorCode::invalid_argument,
          path + " must be a 6-digit hex color");
    }
    return ThemeColorValue{normalize_hex_color(string_value)};
  }

  if (!vars) {
    return Result<ThemeColorValue>::failure(
        ErrorCode::invalid_argument,
        path + " references unknown variable " + string_value);
  }

  const auto it = vars->find(string_value);
  if (it == vars->end()) {
    return Result<ThemeColorValue>::failure(
        ErrorCode::invalid_argument,
        path + " references unknown variable " + string_value);
  }

  if (!resolving_variables.insert(string_value).second) {
    return Result<ThemeColorValue>::failure(
        ErrorCode::invalid_argument,
        path + " has a circular variable reference at " + string_value);
  }

  const auto resolved = parse_theme_color_value(
      *it, "vars." + string_value, vars, resolving_variables);
  resolving_variables.erase(string_value);
  if (!resolved) {
    return resolved.error();
  }
  return resolved.value();
}

enum class ThemeTokenRequirement { partial, required };

Result<std::map<std::string, ThemeColorValue>> parse_theme_color_map(
    const Json& colors, const Json* vars, std::string_view prefix,
    ThemeTokenRequirement token_requirement) {
  std::map<std::string, ThemeColorValue> resolved;
  if (!colors.is_object()) {
    return Result<std::map<std::string, ThemeColorValue>>::failure(
        ErrorCode::invalid_argument,
        std::string(prefix) + " must be an object");
  }

  if (token_requirement == ThemeTokenRequirement::required) {
    std::vector<std::string> missing;
    for (const char* key : kRequiredThemeColorTokens) {
      if (colors.find(key) == colors.end()) {
        missing.push_back(key);
      }
    }
    if (!missing.empty()) {
      std::ostringstream stream;
      stream << prefix << " is missing required tokens:";
      for (const auto& key : missing) {
        stream << ' ' << key;
      }
      return Result<std::map<std::string, ThemeColorValue>>::failure(
          ErrorCode::invalid_argument, stream.str());
    }
  }

  for (const auto& [key, value] : colors.items()) {
    std::set<std::string> resolving_variables;
    const auto parsed = parse_theme_color_value(
        value, error_path(prefix, key), vars, resolving_variables);
    if (!parsed) {
      return parsed.error();
    }
    resolved.emplace(key, parsed.value());
  }

  return resolved;
}

struct Rgb {
  int r = 0;
  int g = 0;
  int b = 0;
};

Result<Rgb> hex_to_rgb(std::string_view hex) {
  if (!is_valid_hex_color(hex)) {
    return Result<Rgb>::failure(ErrorCode::invalid_argument,
                                "invalid hex color");
  }

  const auto parse_component = [&](std::size_t offset) -> int {
    return std::stoi(std::string(hex.substr(offset, 2)), nullptr, 16);
  };

  return Rgb{parse_component(1), parse_component(3), parse_component(5)};
}

constexpr std::array<int, 6> kCubeValues = {0, 95, 135, 175, 215, 255};

constexpr std::array<int, 24> make_gray_values() {
  std::array<int, 24> values{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index] = 8 + static_cast<int>(index) * 10;
  }
  return values;
}

constexpr auto kGrayValues = make_gray_values();

template <std::size_t N>
int find_closest_index(int value, const std::array<int, N>& candidates) {
  int closest_index = 0;
  int closest_distance = std::abs(value - candidates[0]);
  for (std::size_t index = 1; index < candidates.size(); ++index) {
    const int distance = std::abs(value - candidates[index]);
    if (distance < closest_distance) {
      closest_distance = distance;
      closest_index = static_cast<int>(index);
    }
  }
  return closest_index;
}

double color_distance(const Rgb& lhs, const Rgb& rhs) {
  const double dr = static_cast<double>(lhs.r - rhs.r);
  const double dg = static_cast<double>(lhs.g - rhs.g);
  const double db = static_cast<double>(lhs.b - rhs.b);
  return dr * dr * 0.299 + dg * dg * 0.587 + db * db * 0.114;
}

int rgb_to_ansi256(const Rgb& rgb) {
  const int r_index = find_closest_index(rgb.r, kCubeValues);
  const int g_index = find_closest_index(rgb.g, kCubeValues);
  const int b_index = find_closest_index(rgb.b, kCubeValues);
  const Rgb cube{kCubeValues[r_index], kCubeValues[g_index], kCubeValues[b_index]};
  const int cube_index = 16 + 36 * r_index + 6 * g_index + b_index;
  const double cube_distance = color_distance(rgb, cube);

  const int gray =
      static_cast<int>(std::lround(0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b));
  const int gray_index = find_closest_index(gray, kGrayValues);
  const Rgb gray_rgb{kGrayValues[gray_index], kGrayValues[gray_index],
                     kGrayValues[gray_index]};
  const int ansi_gray_index = 232 + gray_index;
  const double gray_distance = color_distance(rgb, gray_rgb);

  const int spread =
      std::max({rgb.r, rgb.g, rgb.b}) - std::min({rgb.r, rgb.g, rgb.b});
  if (spread < 10 && gray_distance < cube_distance) {
    return ansi_gray_index;
  }

  return cube_index;
}

enum class AnsiColorTarget { foreground, background };

std::string ansi_sequence(const ThemeColorValue& value, ThemeColorMode mode,
                          AnsiColorTarget target) {
  const bool background = target == AnsiColorTarget::background;
  const char* reset = background ? "\x1b[49m" : "\x1b[39m";
  const int base_code = background ? 48 : 38;

  if (const auto* integer = std::get_if<int>(&value)) {
    std::ostringstream stream;
    stream << "\x1b[" << base_code << ";5;" << *integer << 'm';
    return stream.str();
  }

  const auto* string_value = std::get_if<std::string>(&value);
  if (!string_value || string_value->empty()) {
    return reset;
  }

  const auto rgb = hex_to_rgb(*string_value);
  if (!rgb) {
    return reset;
  }
  if (mode == ThemeColorMode::kTrueColor) {
    std::ostringstream stream;
    stream << "\x1b[" << base_code << ";2;" << rgb.value().r << ';'
           << rgb.value().g << ';' << rgb.value().b << 'm';
    return stream.str();
  }

  std::ostringstream stream;
  stream << "\x1b[" << base_code << ";5;" << rgb_to_ansi256(rgb.value()) << 'm';
  return stream.str();
}

}  // namespace

std::string canonicalize_theme_name(std::string_view name) {
  const std::string normalized = una::norm::to_nfkc_utf8(std::string(name));
  return una::cases::to_casefold_utf8(normalized);
}

ThemeColorMode detect_theme_color_mode(
    const std::map<std::string, std::string>& environment) {
  const auto value = [&](const std::string& name) -> std::string {
    const auto it = environment.find(name);
    return it == environment.end() ? std::string{} : it->second;
  };
  const auto has = [&](const std::string& name) {
    const auto it = environment.find(name);
    return it != environment.end() && !it->second.empty();
  };

  const std::string color_term = value("COLORTERM");
  if (color_term == "truecolor" || color_term == "24bit") {
    return ThemeColorMode::kTrueColor;
  }
  if (has("WT_SESSION")) {
    return ThemeColorMode::kTrueColor;
  }

  const std::string term = value("TERM");
  if (term.empty() || term == "dumb" || term == "linux") {
    return ThemeColorMode::kAnsi256;
  }
  if (value("TERM_PROGRAM") == "Apple_Terminal") {
    return ThemeColorMode::kAnsi256;
  }
  if (term == "screen" || term.rfind("screen-", 0) == 0 ||
      term.rfind("screen.", 0) == 0) {
    return ThemeColorMode::kAnsi256;
  }
  return ThemeColorMode::kTrueColor;
}

const std::vector<std::string>& required_theme_color_tokens() {
  static const std::vector<std::string> tokens = [] {
    std::vector<std::string> values;
    values.reserve(kRequiredThemeColorTokens.size());
    for (const char* token : kRequiredThemeColorTokens) {
      values.emplace_back(token);
    }
    return values;
  }();
  return tokens;
}

Result<ParsedThemeDocument> parse_theme_document(std::string_view label,
                                                 std::string_view content) {
  Json document;
  try {
    document = Json::parse(content);
  } catch (const std::exception& error) {
    return Result<ParsedThemeDocument>::failure(
        ErrorCode::invalid_argument,
        std::string("failed to parse theme ") + std::string(label) + ": " +
            error.what());
  }

  if (!document.is_object()) {
    return Result<ParsedThemeDocument>::failure(
        ErrorCode::invalid_argument, "theme document must be an object");
  }

  ParsedThemeDocument parsed;
  const auto name = require_string_field(document, "name");
  if (!name) {
    return name.error();
  }
  parsed.name = name.value();
  parsed.canonical_name = canonicalize_theme_name(parsed.name);

  const Json* vars = nullptr;
  if (const auto it = document.find("vars"); it != document.end()) {
    if (!it->is_object()) {
      return Result<ParsedThemeDocument>::failure(
          ErrorCode::invalid_argument, "vars must be an object");
    }
    vars = &*it;
  }

  const auto colors = require_object_field(document, "colors", "an object");
  if (!colors) {
    return colors.error();
  }
  const auto parsed_colors =
      parse_theme_color_map(*colors.value(), vars, "colors",
                            ThemeTokenRequirement::required);
  if (!parsed_colors) {
    return parsed_colors.error();
  }
  parsed.colors = parsed_colors.value();

  if (const auto it = document.find("export"); it != document.end()) {
    const auto export_colors =
        parse_theme_color_map(*it, vars, "export",
                              ThemeTokenRequirement::partial);
    if (!export_colors) {
      return export_colors.error();
    }
    parsed.export_colors = export_colors.value();
  }

  return parsed;
}

Result<std::string> theme_color_to_hex(const ThemeColorValue& value,
                                       std::string_view default_hex) {
  if (const auto* integer = std::get_if<int>(&value)) {
    if (*integer < 0 || *integer > 255) {
      return Result<std::string>::failure(
          ErrorCode::invalid_argument,
          "ANSI256 color index must be from 0 to 255");
    }
    return ansi256_to_hex(*integer);
  }

  const auto* string_value = std::get_if<std::string>(&value);
  if (!string_value) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument, "color value must be a string or integer");
  }
  if (string_value->empty()) {
    return std::string(default_hex);
  }
  if (!is_valid_hex_color(*string_value)) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument, "color value must be a 6-digit hex color");
  }
  return normalize_hex_color(*string_value);
}

Result<std::string> theme_foreground_ansi(const ThemeColorValue& value,
                                          ThemeColorMode mode) {
  return ansi_sequence(value, mode, AnsiColorTarget::foreground);
}

Result<std::string> theme_background_ansi(const ThemeColorValue& value,
                                          ThemeColorMode mode) {
  return ansi_sequence(value, mode, AnsiColorTarget::background);
}

std::string ansi256_to_hex(int index) {
  static const std::array<const char*, 16> kBasicColors = {
      "#000000", "#800000", "#008000", "#808000",
      "#000080", "#800080", "#008080", "#c0c0c0",
      "#808080", "#ff0000", "#00ff00", "#ffff00",
      "#0000ff", "#ff00ff", "#00ffff", "#ffffff"};

  if (index >= 0 && index < 16) {
    return kBasicColors[index];
  }

  if (index >= 16 && index < 232) {
    const int cube_index = index - 16;
    const int r = cube_index / 36;
    const int g = (cube_index % 36) / 6;
    const int b = cube_index % 6;
    const auto to_hex = [](int component) {
      const int value = component == 0 ? 0 : 55 + component * 40;
      std::ostringstream stream;
      stream << std::hex << std::nouppercase << std::setw(2) << std::setfill('0')
             << value;
      return stream.str();
    };
    return "#" + to_hex(r) + to_hex(g) + to_hex(b);
  }

  if (index >= 232 && index < 256) {
    const int gray = 8 + (index - 232) * 10;
    std::ostringstream stream;
    stream << '#'
           << std::hex << std::nouppercase << std::setw(2) << std::setfill('0')
           << gray << std::setw(2) << gray << std::setw(2) << gray;
    return stream.str();
  }

  return "#000000";
}

}  // namespace qt::app
