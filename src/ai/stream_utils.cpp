#include "qt/ai/stream_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace qt::ai {
namespace {

bool is_hex(char value) {
  return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

std::optional<std::uint32_t> parse_hex(std::string_view text) {
  std::uint32_t value = 0;
  for (const char ch : text) {
    if (!is_hex(ch)) {
      return std::nullopt;
    }
    if (value > (0x10ffffU >> 4U)) {
      return std::nullopt;
    }
    value <<= 4U;
    if (ch >= '0' && ch <= '9') {
      value += static_cast<std::uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      value += static_cast<std::uint32_t>(ch - 'a' + 10);
    } else {
      value += static_cast<std::uint32_t>(ch - 'A' + 10);
    }
  }
  return value;
}

bool is_high_surrogate(std::uint32_t value) {
  return value >= 0xd800U && value <= 0xdbffU;
}

bool is_low_surrogate(std::uint32_t value) {
  return value >= 0xdc00U && value <= 0xdfffU;
}

bool is_unicode_scalar(std::uint32_t value) {
  return value <= 0x10ffffU && !is_high_surrogate(value) &&
         !is_low_surrogate(value);
}

void append_utf8(std::string& output, std::uint32_t code_point) {
  if (code_point <= 0x7fU) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else if (code_point <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  }
}

bool is_control_character(char ch) {
  const auto value = static_cast<unsigned char>(ch);
  return value <= 0x1fU;
}

void append_control_escape(std::string& output, char ch) {
  switch (ch) {
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default: {
      constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5',
                                             '6', '7', '8', '9', 'a', 'b',
                                             'c', 'd', 'e', 'f'};
      const auto value = static_cast<unsigned char>(ch);
      output += "\\u00";
      output.push_back(kHex[value >> 4U]);
      output.push_back(kHex[value & 0x0fU]);
      break;
    }
  }
}

bool has_valid_json_escape(char ch) {
  switch (ch) {
    case '"':
    case '\\':
    case '/':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
    case 'u':
      return true;
    default:
      return false;
  }
}

std::string trim_right(std::string text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.pop_back();
  }
  return text;
}

bool append_closers_for_prefix(std::string_view prefix, std::string& output) {
  std::vector<char> stack;
  bool in_string = false;
  bool escaped = false;

  for (const char ch : prefix) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
    } else if (ch == '{' || ch == '[') {
      stack.push_back(ch);
    } else if (ch == '}' || ch == ']') {
      if (stack.empty()) {
        return false;
      }
      const char expected = stack.back() == '{' ? '}' : ']';
      if (ch != expected) {
        return false;
      }
      stack.pop_back();
    }
  }

  if (in_string) {
    return false;
  }

  output.assign(prefix);
  while (!output.empty()) {
    const char ch = output.back();
    if (std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == ',') {
      output.pop_back();
      continue;
    }
    break;
  }

  for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
    output.push_back(*it == '{' ? '}' : ']');
  }
  return true;
}

std::optional<Json> parse_json_text(std::string_view json) {
  try {
    return Json::parse(json.begin(), json.end());
  } catch (const Json::parse_error&) {
    return std::nullopt;
  }
}

std::vector<std::size_t> candidate_prefixes(std::string_view json,
                                            std::size_t max_candidates) {
  std::vector<std::size_t> candidates;
  bool in_string = false;
  bool escaped = false;

  for (std::size_t index = 0; index < json.size(); ++index) {
    const char ch = json[index];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
        candidates.push_back(index + 1U);
      }
    } else {
      if (ch == '"') {
        in_string = true;
      } else {
        candidates.push_back(index + 1U);
      }
    }

    if (candidates.size() > max_candidates * 2U) {
      candidates.erase(candidates.begin(),
                       candidates.begin() +
                           static_cast<std::ptrdiff_t>(max_candidates));
    }
  }

  if (candidates.size() > max_candidates) {
    candidates.erase(candidates.begin(),
                     candidates.end() -
                         static_cast<std::ptrdiff_t>(max_candidates));
  }
  return candidates;
}

}  // namespace

std::string sanitize_json_unicode_escapes(std::string_view text) {
  std::string sanitized;
  sanitized.reserve(text.size());

  for (std::size_t index = 0; index < text.size();) {
    if (text[index] != '\\' || index + 1U >= text.size() ||
        text[index + 1U] != 'u') {
      sanitized.push_back(text[index]);
      ++index;
      continue;
    }

    if (index + 2U < text.size() && text[index + 2U] == '{') {
      const auto end = text.find('}', index + 3U);
      if (end == std::string_view::npos || end == index + 3U) {
        index += 2U;
        continue;
      }
      const auto code_point =
          parse_hex(text.substr(index + 3U, end - index - 3U));
      if (code_point && is_unicode_scalar(*code_point)) {
        append_utf8(sanitized, *code_point);
      }
      index = end + 1U;
      continue;
    }

    if (index + 6U > text.size()) {
      index += 2U;
      continue;
    }

    const auto first = parse_hex(text.substr(index + 2U, 4U));
    if (!first) {
      index += 2U;
      continue;
    }

    if (is_high_surrogate(*first)) {
      if (index + 12U <= text.size() && text[index + 6U] == '\\' &&
          text[index + 7U] == 'u') {
        const auto second = parse_hex(text.substr(index + 8U, 4U));
        if (second && is_low_surrogate(*second)) {
          sanitized.append(text.substr(index, 12U));
          index += 12U;
          continue;
        }
      }
      index += 6U;
      continue;
    }

    if (is_low_surrogate(*first)) {
      index += 6U;
      continue;
    }

    sanitized.append(text.substr(index, 6U));
    index += 6U;
  }

  return sanitized;
}

std::string repair_json_string_literals(std::string_view json) {
  std::string repaired;
  repaired.reserve(json.size());
  bool in_string = false;

  for (std::size_t index = 0; index < json.size(); ++index) {
    const char ch = json[index];

    if (!in_string) {
      repaired.push_back(ch);
      if (ch == '"') {
        in_string = true;
      }
      continue;
    }

    if (ch == '"') {
      repaired.push_back(ch);
      in_string = false;
      continue;
    }

    if (ch == '\\') {
      const bool has_next = index + 1U < json.size();
      const char next = has_next ? json[index + 1U] : '\0';
      if (!has_next) {
        repaired += "\\\\";
        continue;
      }

      if (next == 'u' && index + 6U <= json.size()) {
        const auto digits = json.substr(index + 2U, 4U);
        if (parse_hex(digits)) {
          repaired += "\\u";
          repaired.append(digits);
          index += 5U;
          continue;
        }
      }

      if (has_valid_json_escape(next)) {
        repaired.push_back('\\');
        repaired.push_back(next);
        ++index;
        continue;
      }

      repaired += "\\\\";
      continue;
    }

    if (is_control_character(ch)) {
      append_control_escape(repaired, ch);
    } else {
      repaired.push_back(ch);
    }
  }

  return repaired;
}

std::optional<Json> parse_json_with_repair(std::string_view json) {
  const auto sanitized = sanitize_json_unicode_escapes(json);
  if (auto parsed = parse_json_text(sanitized)) {
    return parsed;
  }

  const auto repaired = repair_json_string_literals(sanitized);
  if (repaired != sanitized) {
    return parse_json_text(repaired);
  }
  return std::nullopt;
}

std::string trim_incomplete_json_fragment(std::string_view json,
                                          const PartialJsonOptions& options) {
  if (json.empty() || options.max_bytes == 0U) {
    return {};
  }

  const auto bounded = json.substr(0, std::min(json.size(), options.max_bytes));
  const auto repaired = repair_json_string_literals(
      sanitize_json_unicode_escapes(trim_right(std::string(bounded))));

  if (parse_json_text(repaired)) {
    return repaired;
  }

  const auto candidate_limit =
      std::min<std::size_t>(4096U,
                            std::max<std::size_t>(1U, options.max_candidates));
  const auto candidates = candidate_prefixes(repaired, candidate_limit);
  for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
    std::string candidate;
    if (!append_closers_for_prefix(repaired.substr(0, *it), candidate)) {
      continue;
    }
    candidate = trim_right(candidate);
    if (candidate.empty()) {
      continue;
    }
    if (parse_json_text(candidate)) {
      return candidate;
    }
  }

  return {};
}

Json parse_streaming_json(std::string_view json,
                          const PartialJsonOptions& options) {
  if (json.empty() ||
      std::all_of(json.begin(), json.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0;
      })) {
    return Json::object();
  }

  if (auto parsed = parse_json_with_repair(json)) {
    return *parsed;
  }

  const auto cleaned = trim_incomplete_json_fragment(json, options);
  if (!cleaned.empty()) {
    if (auto parsed = parse_json_with_repair(cleaned)) {
      return *parsed;
    }
  }

  return Json::object();
}

}  // namespace qt::ai
