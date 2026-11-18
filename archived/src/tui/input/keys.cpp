#include "qt/tui/input/keys.hpp"

#include <cctype>
#include <charconv>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace qt::tui {
namespace {

bool kitty_protocol_active = false;

std::optional<int> parse_int(std::string_view value) {
  int result = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto parsed = std::from_chars(begin, end, result);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return result;
}

std::vector<std::string_view> split(std::string_view value, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(delimiter, start);
    if (end == std::string_view::npos) {
      parts.push_back(value.substr(start));
      break;
    }
    parts.push_back(value.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

bool env_present(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0';
}

bool is_local_windows_terminal_session() {
  return env_present("WT_SESSION") && !env_present("SSH_CONNECTION") &&
         !env_present("SSH_CLIENT") && !env_present("SSH_TTY");
}

std::string key_name_from_codepoint(int codepoint) {
  switch (codepoint) {
    case 9:
      return "tab";
    case 13:
      return "enter";
    case 27:
      return "escape";
    case 32:
      return "space";
    case 127:
      return "backspace";
    case 57399:
      return "0";
    case 57400:
      return "1";
    case 57401:
      return "2";
    case 57402:
      return "3";
    case 57403:
      return "4";
    case 57404:
      return "5";
    case 57405:
      return "6";
    case 57406:
      return "7";
    case 57407:
      return "8";
    case 57408:
      return "9";
    case 57409:
      return ".";
    case 57410:
      return "/";
    case 57411:
      return "*";
    case 57412:
      return "-";
    case 57413:
      return "+";
    case 57414:
      return "enter";
    case 57415:
      return "=";
    case 57416:
      return ",";
    case 57417:
      return "left";
    case 57418:
      return "right";
    case 57419:
      return "up";
    case 57420:
      return "down";
    case 57421:
      return "pageUp";
    case 57422:
      return "pageDown";
    case 57423:
      return "home";
    case 57424:
      return "end";
    case 57425:
      return "insert";
    case 57426:
      return "delete";
    default:
      break;
  }

  if (codepoint >= 32 && codepoint <= 126) {
    char ch = static_cast<char>(codepoint);
    if (std::isalpha(static_cast<unsigned char>(ch)) != 0) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return std::string(1, ch);
  }
  return {};
}

std::optional<std::string> utf8_from_codepoint(int codepoint) {
  if (codepoint < 0 || codepoint > 0x10ffff ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
    return std::nullopt;
  }

  std::string output;
  if (codepoint <= 0x7f) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | ((codepoint >> 6) & 0x1f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | ((codepoint >> 12) & 0x0f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | ((codepoint >> 18) & 0x07)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
  return output;
}

int normalize_kitty_functional_codepoint(int codepoint) {
  switch (codepoint) {
    case 57399:
      return '0';
    case 57400:
      return '1';
    case 57401:
      return '2';
    case 57402:
      return '3';
    case 57403:
      return '4';
    case 57404:
      return '5';
    case 57405:
      return '6';
    case 57406:
      return '7';
    case 57407:
      return '8';
    case 57408:
      return '9';
    case 57409:
      return '.';
    case 57410:
      return '/';
    case 57411:
      return '*';
    case 57412:
      return '-';
    case 57413:
      return '+';
    case 57415:
      return '=';
    case 57416:
      return ',';
    default:
      return codepoint;
  }
}

std::string apply_modifiers(std::string key, int encoded_modifier) {
  const int mask = std::max(0, encoded_modifier - 1);
  std::string prefix;
  if ((mask & 1) != 0 && key.size() == 1 &&
      std::isalpha(static_cast<unsigned char>(key.front())) != 0) {
    prefix += "shift+";
  } else if ((mask & 1) != 0) {
    prefix += "shift+";
  }
  if ((mask & 4) != 0) {
    prefix += "ctrl+";
  }
  if ((mask & 2) != 0) {
    prefix += "alt+";
  }
  if ((mask & 8) != 0) {
    prefix += "super+";
  }
  return prefix + key;
}

std::optional<int> codepoint_from_key_name(std::string_view key) {
  if (key == "tab") {
    return 9;
  }
  if (key == "enter") {
    return 13;
  }
  if (key == "escape") {
    return 27;
  }
  if (key == "space") {
    return 32;
  }
  if (key == "backspace") {
    return 127;
  }
  if (key == "left") {
    return 57417;
  }
  if (key == "right") {
    return 57418;
  }
  if (key == "up") {
    return 57419;
  }
  if (key == "down") {
    return 57420;
  }
  if (key == "pageUp") {
    return 57421;
  }
  if (key == "pageDown") {
    return 57422;
  }
  if (key == "home") {
    return 57423;
  }
  if (key == "end") {
    return 57424;
  }
  if (key == "insert") {
    return 57425;
  }
  if (key == "delete") {
    return 57426;
  }
  if (key.size() == 1U) {
    return static_cast<unsigned char>(key.front());
  }
  return std::nullopt;
}

bool is_printable_key_name(std::string_view key) {
  if (key == "space") {
    return true;
  }
  return key.size() == 1U &&
         std::isprint(static_cast<unsigned char>(key.front())) != 0;
}

std::optional<int> encoded_modifier_from_key_id(std::string_view key_id) {
  const auto parts = split(key_id, '+');
  if (parts.empty()) {
    return 1;
  }

  int modifier = 1;
  for (std::size_t index = 0; index + 1U < parts.size(); ++index) {
    const auto part = parts[index];
    if (part == "shift") {
      modifier |= 1;
      continue;
    }
    if (part == "alt") {
      modifier |= 2;
      continue;
    }
    if (part == "ctrl") {
      modifier |= 4;
      continue;
    }
    if (part == "super") {
      modifier |= 8;
      continue;
    }
    return std::nullopt;
  }

  if (parts.back().empty()) {
    return std::nullopt;
  }
  return modifier;
}

std::optional<KeyId> parse_modified_key(std::string_view codepoint_text,
                                        std::string_view modifier_text) {
  auto codepoint = parse_int(codepoint_text);
  auto modifier = parse_int(modifier_text);
  if (!codepoint || !modifier) {
    return std::nullopt;
  }
  const int mask = std::max(0, *modifier - 1);
  if ((mask & ~15) != 0) {
    return std::nullopt;
  }
  auto key = key_name_from_codepoint(*codepoint);
  if (key.empty()) {
    return std::nullopt;
  }
  return apply_modifiers(std::move(key), *modifier);
}

std::optional<KeyId> parse_escape_sequence(std::string_view input) {
  if (input == "\x1b") {
    return KeyId{"escape"};
  }
  if (kitty_protocol_active && (input == "\x1b\r" || input == "\n")) {
    return KeyId{"shift+enter"};
  }
  if (input == "\x1b[A") {
    return KeyId{"up"};
  }
  if (input == "\x1b[B") {
    return KeyId{"down"};
  }
  if (input == "\x1b[C") {
    return KeyId{"right"};
  }
  if (input == "\x1b[D") {
    return KeyId{"left"};
  }
  if (input == "\x1b[E") {
    return KeyId{"clear"};
  }
  if (input == "\x1bOE") {
    return KeyId{"clear"};
  }
  if (input == "\x1b[e") {
    return KeyId{"shift+clear"};
  }
  if (input == "\x1bOe") {
    return KeyId{"ctrl+clear"};
  }
  if (input == "\x1b[Z") {
    return KeyId{"shift+tab"};
  }
  if (input == "\x1bOA") {
    return KeyId{"up"};
  }
  if (input == "\x1bOB") {
    return KeyId{"down"};
  }
  if (input == "\x1bOC") {
    return KeyId{"right"};
  }
  if (input == "\x1bOD") {
    return KeyId{"left"};
  }
  if (input == "\x1bOH") {
    return KeyId{"home"};
  }
  if (input == "\x1bOF") {
    return KeyId{"end"};
  }
  if (input == "\x1bOP") {
    return KeyId{"f1"};
  }
  if (input == "\x1bOQ") {
    return KeyId{"f2"};
  }
  if (input == "\x1bOR") {
    return KeyId{"f3"};
  }
  if (input == "\x1bOS") {
    return KeyId{"f4"};
  }
  if (input == "\x1b[[A") {
    return KeyId{"f1"};
  }
  if (input == "\x1b[[B") {
    return KeyId{"f2"};
  }
  if (input == "\x1b[[C") {
    return KeyId{"f3"};
  }
  if (input == "\x1b[[D") {
    return KeyId{"f4"};
  }
  if (input == "\x1b[[E") {
    return KeyId{"f5"};
  }
  if (input == "\x1b[[5~") {
    return KeyId{"pageUp"};
  }
  if (input == "\x1b[[6~") {
    return KeyId{"pageDown"};
  }
  if (input == "\x1b[a") {
    return KeyId{"shift+up"};
  }
  if (input == "\x1b[b") {
    return KeyId{"shift+down"};
  }
  if (input == "\x1b[c") {
    return KeyId{"shift+right"};
  }
  if (input == "\x1b[d") {
    return KeyId{"shift+left"};
  }
  if (input == "\x1bOa") {
    return KeyId{"ctrl+up"};
  }
  if (input == "\x1bOb") {
    return KeyId{"ctrl+down"};
  }
  if (input == "\x1bOc") {
    return KeyId{"ctrl+right"};
  }
  if (input == "\x1bOd") {
    return KeyId{"ctrl+left"};
  }
  if (input == "\x1b[H" || input == "\x1b[1~" || input == "\x1b[7~") {
    return KeyId{"home"};
  }
  if (input == "\x1b[F" || input == "\x1b[4~" || input == "\x1b[8~") {
    return KeyId{"end"};
  }
  if (input == "\x1b[2~") {
    return KeyId{"insert"};
  }
  if (input == "\x1b[3~") {
    return KeyId{"delete"};
  }
  if (input == "\x1b[5~") {
    return KeyId{"pageUp"};
  }
  if (input == "\x1b[6~") {
    return KeyId{"pageDown"};
  }
  if (input == "\x1b[11~") {
    return KeyId{"f1"};
  }
  if (input == "\x1b[12~") {
    return KeyId{"f2"};
  }
  if (input == "\x1b[13~") {
    return KeyId{"f3"};
  }
  if (input == "\x1b[14~") {
    return KeyId{"f4"};
  }
  if (input == "\x1b[15~") {
    return KeyId{"f5"};
  }
  if (input == "\x1b[17~") {
    return KeyId{"f6"};
  }
  if (input == "\x1b[18~") {
    return KeyId{"f7"};
  }
  if (input == "\x1b[19~") {
    return KeyId{"f8"};
  }
  if (input == "\x1b[20~") {
    return KeyId{"f9"};
  }
  if (input == "\x1b[21~") {
    return KeyId{"f10"};
  }
  if (input == "\x1b[23~") {
    return KeyId{"f11"};
  }
  if (input == "\x1b[24~") {
    return KeyId{"f12"};
  }
  if (input == "\x1b[2$") {
    return KeyId{"shift+insert"};
  }
  if (input == "\x1b[2^") {
    return KeyId{"ctrl+insert"};
  }
  if (input == "\x1b[3$") {
    return KeyId{"shift+delete"};
  }
  if (input == "\x1b[3^") {
    return KeyId{"ctrl+delete"};
  }
  if (input == "\x1b[5$") {
    return KeyId{"shift+pageUp"};
  }
  if (input == "\x1b[6$") {
    return KeyId{"shift+pageDown"};
  }
  if (input == "\x1b[7$") {
    return KeyId{"shift+home"};
  }
  if (input == "\x1b[8$") {
    return KeyId{"shift+end"};
  }
  if (input == "\x1b[5^") {
    return KeyId{"ctrl+pageUp"};
  }
  if (input == "\x1b[6^") {
    return KeyId{"ctrl+pageDown"};
  }
  if (input == "\x1b[7^") {
    return KeyId{"ctrl+home"};
  }
  if (input == "\x1b[8^") {
    return KeyId{"ctrl+end"};
  }
  if (input == "\x1b[45;5u") {
    return KeyId{"ctrl+-"};
  }
  if (!kitty_protocol_active && input == "\x1b\r") {
    return KeyId{"alt+enter"};
  }
  if (input == "\x1b" "b") {
    return KeyId{"alt+left"};
  }
  if (input == "\x1b" "f") {
    return KeyId{"alt+right"};
  }
  if (input == "\x1b" "p") {
    return KeyId{"alt+up"};
  }
  if (input == "\x1b" "n") {
    return KeyId{"alt+down"};
  }
  if (!kitty_protocol_active && input == "\x1b" "B") {
    return KeyId{"alt+left"};
  }
  if (!kitty_protocol_active && input == "\x1b" "F") {
    return KeyId{"alt+right"};
  }
  if (input == "\x1b\x1d") {
    return KeyId{"ctrl+alt+]"};
  }
  if (input == "\x1b\x1b") {
    return KeyId{"ctrl+alt+["};
  }
  if (input == "\x1b\x1c") {
    return KeyId{"ctrl+alt+\\"};
  }
  if (input == "\x1b\x1f") {
    return KeyId{"ctrl+alt+-"};
  }
  if (input.size() > 3 && input.substr(0, 2) == "\x1b[" &&
      (input.back() == 'A' || input.back() == 'B' || input.back() == 'C' ||
       input.back() == 'D' || input.back() == 'H' || input.back() == 'F')) {
    const auto body = input.substr(2, input.size() - 3);
    const auto parts = split(body, ';');
    if (parts.size() >= 2) {
      const auto modifier = parse_int(parts[1]);
      if (modifier) {
        std::string key;
        switch (input.back()) {
          case 'A':
            key = "up";
            break;
          case 'B':
            key = "down";
            break;
          case 'C':
            key = "right";
            break;
          case 'D':
            key = "left";
            break;
          case 'H':
            key = "home";
            break;
          case 'F':
            key = "end";
            break;
        }
        if (!key.empty()) {
          return apply_modifiers(std::move(key), *modifier);
        }
      }
    }
  }
  if (input.size() > 4 && input.substr(0, 2) == "\x1b[" && input.back() == '~') {
    const auto body = input.substr(2, input.size() - 3);
    const auto parts = split(body, ';');
    if (parts.size() == 3 && parts[0] == "27") {
      return parse_modified_key(parts[2], parts[1]);
    }
    if (parts.size() >= 2) {
      std::string key;
      if (parts[0] == "2") {
        key = "insert";
      } else if (parts[0] == "3") {
        key = "delete";
      } else if (parts[0] == "5") {
        key = "pageUp";
      } else if (parts[0] == "6") {
        key = "pageDown";
      }
      const auto modifier = parse_int(parts[1]);
      if (!key.empty() && modifier) {
        return apply_modifiers(std::move(key), *modifier);
      }
    }
  }
  if (input.size() > 4 && input.substr(0, 2) == "\x1b[" && input.back() == 'u') {
    const auto body = input.substr(2, input.size() - 3);
    const auto semicolon = body.find(';');
    const auto raw_codepoint_text =
        semicolon == std::string_view::npos ? body : body.substr(0, semicolon);
    const auto codepoint_fields = split(raw_codepoint_text, ':');
    std::string_view codepoint_text = codepoint_fields.front();
    const auto primary_codepoint = parse_int(codepoint_text);
    const bool primary_is_logical_key =
        primary_codepoint && !key_name_from_codepoint(*primary_codepoint).empty();
    if (!primary_is_logical_key && codepoint_fields.size() >= 3 &&
        !codepoint_fields[2].empty()) {
      codepoint_text = codepoint_fields[2];
    }
    if (semicolon == std::string_view::npos) {
      const auto codepoint = parse_int(codepoint_text);
      if (!codepoint) {
        return std::nullopt;
      }
      auto key = key_name_from_codepoint(*codepoint);
      return key.empty() ? std::nullopt : std::optional<KeyId>{key};
    }
    const auto modifier_fields = split(body.substr(semicolon + 1), ':');
    if (!modifier_fields.empty()) {
      return parse_modified_key(codepoint_text, modifier_fields.front());
    }
  }
  if (!kitty_protocol_active && input.size() == 2U &&
      std::isprint(static_cast<unsigned char>(input[1])) != 0) {
    if (input == "\x1b ") {
      return KeyId{"alt+space"};
    }
    return KeyId{"alt+"} +
           static_cast<char>(std::tolower(static_cast<unsigned char>(input[1])));
  }
  if (input.size() == 2U) {
    const auto nested = parse_key(input.substr(1));
    if (!kitty_protocol_active && nested && nested->rfind("ctrl+", 0) == 0) {
      return KeyId{"ctrl+alt+"} + nested->substr(5);
    }
    if (nested && *nested == "backspace") {
      return KeyId{"alt+backspace"};
    }
  }
  return std::nullopt;
}

}  // namespace

std::string canonical_key_id(std::string_view key_id);

void set_kitty_protocol_active(bool active) noexcept { kitty_protocol_active = active; }

bool is_kitty_protocol_active() noexcept { return kitty_protocol_active; }

std::optional<KeyId> parse_key(std::string_view input) {
  if (input.empty()) {
    return std::nullopt;
  }

  if (const auto sequence = parse_escape_sequence(input)) {
    return sequence;
  }

  if (input == "\r" || (!kitty_protocol_active && input == "\n")) {
    return KeyId{"enter"};
  }
  if (input == "\t") {
    return KeyId{"tab"};
  }
  if (input == " ") {
    return KeyId{"space"};
  }
  if (input == "\x7f" || input == "\b") {
    if (input == "\b" && is_local_windows_terminal_session()) {
      return KeyId{"ctrl+backspace"};
    }
    return KeyId{"backspace"};
  }

  if (input.size() == 1) {
    const unsigned char value = static_cast<unsigned char>(input.front());
    if (value == 0) {
      return KeyId{"ctrl+space"};
    }
    if (value >= 1 && value <= 26) {
      const char letter = static_cast<char>('a' + (value - 1));
      return KeyId{"ctrl+"} + letter;
    }
    if (value == 28) {
      return KeyId{"ctrl+\\"};
    }
    if (value == 29) {
      return KeyId{"ctrl+]"};
    }
    if (value == 31) {
      return KeyId{"ctrl+-"};
    }

    if (std::isalpha(value) != 0) {
      return KeyId{static_cast<char>(std::tolower(value))};
    }
    return KeyId{input};
  }

  return std::nullopt;
}

std::optional<KeyId> key_id_from_codepoint(std::uint32_t codepoint, unsigned modifiers) {
  constexpr unsigned supported = 1U | 2U | 4U | 8U | 64U | 128U;
  if ((modifiers & ~supported) != 0U) {
    return std::nullopt;
  }

  auto key = key_name_from_codepoint(static_cast<int>(codepoint));
  if (key.empty()) {
    return std::nullopt;
  }

  const int encoded_modifier =
      1 + static_cast<int>(modifiers & ~(64U | 128U));
  return canonical_key_id(apply_modifiers(std::move(key), encoded_modifier));
}

std::optional<std::string> input_bytes_for_key_id(std::string_view key_id) {
  const auto canonical = canonical_key_id(key_id);
  const auto encoded_modifier = encoded_modifier_from_key_id(canonical);
  if (!encoded_modifier) {
    return std::nullopt;
  }

  const auto parts = split(canonical, '+');
  const std::string_view base = parts.empty() ? std::string_view{} : parts.back();
  if (base.empty()) {
    return std::nullopt;
  }

  if (*encoded_modifier == 1) {
    if (base == "enter") {
      return std::string("\r");
    }
    if (base == "tab") {
      return std::string("\t");
    }
    if (base == "escape") {
      return std::string("\x1b");
    }
    if (base == "space") {
      return std::string(" ");
    }
    if (base == "backspace") {
      return std::string("\x7f");
    }
    if (base == "left") {
      return std::string("\x1b[D");
    }
    if (base == "right") {
      return std::string("\x1b[C");
    }
    if (base == "up") {
      return std::string("\x1b[A");
    }
    if (base == "down") {
      return std::string("\x1b[B");
    }
    if (base == "home") {
      return std::string("\x1b[H");
    }
    if (base == "end") {
      return std::string("\x1b[F");
    }
    if (base == "insert") {
      return std::string("\x1b[2~");
    }
    if (base == "delete") {
      return std::string("\x1b[3~");
    }
    if (base == "pageUp") {
      return std::string("\x1b[5~");
    }
    if (base == "pageDown") {
      return std::string("\x1b[6~");
    }
    if (base.size() == 1U) {
      return std::string(base);
    }
    return std::nullopt;
  }

  if (base == "left" || base == "right" || base == "up" || base == "down" ||
      base == "home" || base == "end") {
    char suffix = 'A';
    if (base == "down") {
      suffix = 'B';
    } else if (base == "right") {
      suffix = 'C';
    } else if (base == "left") {
      suffix = 'D';
    } else if (base == "home") {
      suffix = 'H';
    } else if (base == "end") {
      suffix = 'F';
    }

    return std::string("\x1b[1;") + std::to_string(*encoded_modifier) + suffix;
  }

  if (base == "insert" || base == "delete" || base == "pageUp" ||
      base == "pageDown") {
    int code = 2;
    if (base == "delete") {
      code = 3;
    } else if (base == "pageUp") {
      code = 5;
    } else if (base == "pageDown") {
      code = 6;
    }

    return std::string("\x1b[") + std::to_string(code) + ";" +
           std::to_string(*encoded_modifier) + "~";
  }

  const auto codepoint = codepoint_from_key_name(base);
  if (!codepoint) {
    return std::nullopt;
  }
  if (is_printable_key_name(base) || base == "enter" || base == "tab" ||
      base == "escape" || base == "backspace") {
    return std::string("\x1b[") + std::to_string(*codepoint) + ";" +
           std::to_string(*encoded_modifier) + "u";
  }

  return std::nullopt;
}

bool has_key_event_suffix(std::string_view input, std::string_view event_marker) noexcept {
  if (input.find("\x1b[200~") != std::string_view::npos) {
    return false;
  }

  static constexpr std::string_view suffixes[] = {"u", "~", "A", "B", "C", "D", "H", "F"};
  for (const auto suffix : suffixes) {
    const auto event_offset = input.find(event_marker);
    if (event_offset == std::string_view::npos) {
      return false;
    }
    if (event_offset + event_marker.size() + suffix.size() <= input.size() &&
        input.substr(event_offset + event_marker.size(), suffix.size()) == suffix) {
      return true;
    }
  }
  return false;
}

bool is_key_release(std::string_view input) noexcept { return has_key_event_suffix(input, ":3"); }

bool is_key_repeat(std::string_view input) noexcept { return has_key_event_suffix(input, ":2"); }

std::optional<std::string> decode_printable_key(std::string_view input) {
  if (input.size() > 4 && input.substr(0, 2) == "\x1b[" && input.back() == 'u') {
    const auto body = input.substr(2, input.size() - 3);
    const auto semicolon = body.find(';');
    const auto raw_codepoint_text =
        semicolon == std::string_view::npos ? body : body.substr(0, semicolon);
    const auto codepoint_fields = split(raw_codepoint_text, ':');
    const auto codepoint = parse_int(codepoint_fields.front());
    if (!codepoint) {
      return std::nullopt;
    }
    std::optional<int> shifted;
    if (codepoint_fields.size() >= 2 && !codepoint_fields[1].empty()) {
      shifted = parse_int(codepoint_fields[1]);
    }

    int encoded_modifier = 1;
    if (semicolon != std::string_view::npos) {
      const auto modifier_fields = split(body.substr(semicolon + 1), ':');
      if (modifier_fields.empty()) {
        return std::nullopt;
      }
      const auto parsed_modifier = parse_int(modifier_fields.front());
      if (!parsed_modifier) {
        return std::nullopt;
      }
      encoded_modifier = *parsed_modifier;
    }

    const int modifier = std::max(0, encoded_modifier - 1);
    constexpr int shift = 1;
    constexpr int alt = 2;
    constexpr int ctrl = 4;
    constexpr int lock_mask = 64 + 128;
    constexpr int allowed = shift | lock_mask;
    if ((modifier & ~allowed) != 0 || (modifier & (alt | ctrl)) != 0) {
      return std::nullopt;
    }

    int effective = *codepoint;
    if ((modifier & shift) != 0 && shifted) {
      effective = *shifted;
    }
    effective = normalize_kitty_functional_codepoint(effective);
    if (effective < 32) {
      return std::nullopt;
    }
    return utf8_from_codepoint(effective);
  }

  if (input.size() > 4 && input.substr(0, 2) == "\x1b[" && input.back() == '~') {
    const auto body = input.substr(2, input.size() - 3);
    const auto parts = split(body, ';');
    if (parts.size() == 3 && parts[0] == "27") {
      const auto modifier = parse_int(parts[1]);
      const auto codepoint = parse_int(parts[2]);
      if (!modifier || !codepoint) {
        return std::nullopt;
      }
      constexpr int shift = 1;
      const int mask = std::max(0, *modifier - 1);
      if ((mask & ~shift) != 0 || *codepoint < 32) {
        return std::nullopt;
      }
      return utf8_from_codepoint(*codepoint);
    }
  }

  return std::nullopt;
}

std::string canonical_key_id(std::string_view key_id) {
  if (key_id == "esc") {
    return "escape";
  }
  if (key_id == "return") {
    return "enter";
  }
  if (key_id == "ctrl+_") {
    return "ctrl+-";
  }
  if (key_id.find('+') == std::string_view::npos) {
    return std::string(key_id);
  }

  const auto parts = split(key_id, '+');
  if (parts.empty()) {
    return std::string(key_id);
  }

  std::vector<std::string_view> modifiers;
  std::string_view base = parts.back();
  for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
    modifiers.push_back(parts[index]);
  }

  const std::vector<std::string_view> order{"shift", "ctrl", "alt", "super"};
  std::string output;
  for (const auto modifier : order) {
    if (std::find(modifiers.begin(), modifiers.end(), modifier) != modifiers.end()) {
      output += std::string(modifier) + "+";
    }
  }
  output += base;
  return output;
}

bool matches_key(std::string_view input, std::string_view key_id) {
  const auto parsed = parse_key(input);
  if (!parsed) {
    return false;
  }
  if ((*parsed == "backspace" || *parsed == "ctrl+backspace") &&
      key_id == "ctrl+h") {
    return true;
  }
  return canonical_key_id(*parsed) == canonical_key_id(key_id);
}

}  // namespace qt::tui
