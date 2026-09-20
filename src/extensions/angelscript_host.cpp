#include <qt/extensions/angelscript_host.hpp>

#include <cctype>
#include <algorithm>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

#include <qt/extensions/api_compatibility.hpp>
#include <qt/extensions/manifest.hpp>

#if defined(__has_include)
#if __has_include(<angelscript.h>) && __has_include(<scriptstdstring.h>)
#include <angelscript.h>
#include <scriptstdstring.h>
#define QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER 1
#endif
#endif

namespace qt::extensions {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;

struct ParsedCall {
  std::string function;
  std::vector<std::string> arguments;
  std::size_t row = 1;
  std::size_t column = 1;
};

AngelScriptDiagnostic make_diagnostic(AngelScriptDiagnosticSeverity severity,
                                      std::string section, std::size_t row,
                                      std::size_t column,
                                      std::string message) {
  AngelScriptDiagnostic diagnostic;
  diagnostic.severity = severity;
  diagnostic.section = std::move(section);
  diagnostic.row = row;
  diagnostic.column = column;
  diagnostic.message = std::move(message);
  return diagnostic;
}

void advance_position(char ch, std::size_t& row, std::size_t& column) {
  if (ch == '\n') {
    ++row;
    column = 1;
    return;
  }
  ++column;
}

void skip_space(std::string_view text, std::size_t& offset, std::size_t& row,
                std::size_t& column) {
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    advance_position(text[offset], row, column);
    ++offset;
  }
}

bool starts_with_at(std::string_view text, std::size_t offset,
                    std::string_view token) {
  return offset + token.size() <= text.size() &&
         text.substr(offset, token.size()) == token;
}

bool is_identifier_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

void skip_plain_space(std::string_view text, std::size_t& offset) {
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
}

bool is_function_call_at(std::string_view text, std::size_t offset,
                         std::string_view function) {
  if (!starts_with_at(text, offset, function)) {
    return false;
  }
  if (offset > 0 && (is_identifier_char(text[offset - 1]) || text[offset - 1] == '.')) {
    return false;
  }
  const std::size_t after = offset + function.size();
  if (after < text.size() && is_identifier_char(text[after])) {
    return false;
  }
  std::size_t cursor = after;
  skip_plain_space(text, cursor);
  return cursor < text.size() && text[cursor] == '(';
}

bool is_ts_identifier_start(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_' ||
         ch == '$';
}

bool is_ts_identifier_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' ||
         ch == '$';
}

std::optional<std::string> parse_ts_identifier(std::string_view text,
                                               std::size_t& offset) {
  skip_plain_space(text, offset);
  if (offset >= text.size() || !is_ts_identifier_start(text[offset])) {
    return std::nullopt;
  }
  const std::size_t start = offset;
  ++offset;
  while (offset < text.size() && is_ts_identifier_char(text[offset])) {
    ++offset;
  }
  return std::string(text.substr(start, offset - start));
}

std::optional<std::string> parse_typescript_string_literal(
    std::string_view text, std::size_t& offset) {
  skip_plain_space(text, offset);
  if (offset >= text.size()) {
    return std::nullopt;
  }

  const char quote = text[offset];
  if (quote != '"' && quote != '\'' && quote != '`') {
    return std::nullopt;
  }
  ++offset;

  std::string value;
  bool escaped = false;
  for (; offset < text.size(); ++offset) {
    const char ch = text[offset];
    if (escaped) {
      switch (ch) {
        case 'n':
          value.push_back('\n');
          break;
        case 't':
          value.push_back('\t');
          break;
        default:
          value.push_back(ch);
          break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == quote) {
      ++offset;
      return value;
    }
    value.push_back(ch);
  }
  return std::nullopt;
}

bool is_one_of(const std::string& value,
               std::initializer_list<const char*> allowed) {
  for (const char* candidate : allowed) {
    if (value == candidate) {
      return true;
    }
  }
  return false;
}

bool is_valid_lifecycle_event(const std::string& value) {
  return is_one_of(
      value,
      {"resources_discover", "instructions_loaded", "session_start", "session_before_switch",
       "session_before_fork", "session_before_compact", "session_compact",
       "session_shutdown", "session_before_tree", "session_tree", "context",
       "before_provider_request", "after_provider_response",
       "before_agent_start", "agent_start", "agent_end", "turn_start",
       "turn_end", "message_start", "message_update", "message_end",
       "tool_execution_start", "tool_execution_update", "tool_execution_end",
       "model_select", "thinking_level_select", "tool_call", "tool_result",
       "user_bash", "input"});
}

bool is_valid_handler_identifier(std::string_view text) {
  if (text.empty()) {
    return false;
  }

  const auto is_start = [](char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_';
  };
  const auto is_body = [](char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
  };

  if (!is_start(text.front())) {
    return false;
  }

  for (char ch : text) {
    if (!is_body(ch)) {
      return false;
    }
  }

  return true;
}

std::optional<std::string> parse_string_literal(
    std::string_view text, std::size_t& offset, std::size_t& row,
    std::size_t& column, std::vector<AngelScriptDiagnostic>& diagnostics,
    std::string_view section) {
  if (offset >= text.size() || text[offset] != '"') {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), row,
        column, "expected string literal argument"));
    return std::nullopt;
  }

  ++offset;
  ++column;

  std::string value;
  while (offset < text.size()) {
    const char ch = text[offset];
    if (ch == '"') {
      ++offset;
      ++column;
      return value;
    }

    if (ch == '\n') {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), row,
          column, "unterminated string literal"));
      return std::nullopt;
    }

    if (ch == '\\') {
      if (offset + 1 >= text.size()) {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section), row,
            column, "unterminated escape sequence"));
        return std::nullopt;
      }
      const char escaped = text[offset + 1];
      switch (escaped) {
        case '"':
        case '\\':
          value.push_back(escaped);
          break;
        case 'n':
          value.push_back('\n');
          break;
        case 't':
          value.push_back('\t');
          break;
        default:
          diagnostics.push_back(make_diagnostic(
              AngelScriptDiagnosticSeverity::kError, std::string(section), row,
              column, "unsupported escape sequence in host registration"));
          return std::nullopt;
      }
      offset += 2;
      column += 2;
      continue;
    }

    value.push_back(ch);
    ++offset;
    ++column;
  }

  diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kError,
                                        std::string(section), row, column,
                                        "unterminated string literal"));
  return std::nullopt;
}

std::optional<ParsedCall> parse_registration_call(
    std::string_view text, std::size_t& offset, std::size_t& row,
    std::size_t& column, std::string_view function,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  ParsedCall call;
  call.function = std::string(function);
  call.row = row;
  call.column = column;

  offset += function.size();
  column += function.size();
  skip_space(text, offset, row, column);

  if (offset >= text.size() || text[offset] != '(') {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), row,
        column, "expected '(' after " + std::string(function)));
    return std::nullopt;
  }
  ++offset;
  ++column;

  while (offset < text.size()) {
    skip_space(text, offset, row, column);
    if (offset < text.size() && text[offset] == ')') {
      ++offset;
      ++column;
      return call;
    }

    auto argument =
        parse_string_literal(text, offset, row, column, diagnostics, section);
    if (!argument) {
      return std::nullopt;
    }
    call.arguments.push_back(std::move(argument.value()));

    skip_space(text, offset, row, column);
    if (offset < text.size() && text[offset] == ',') {
      ++offset;
      ++column;
      continue;
    }
    if (offset < text.size() && text[offset] == ')') {
      ++offset;
      ++column;
      return call;
    }

    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), row,
        column, "expected ',' or ')' in host registration"));
    return std::nullopt;
  }

  diagnostics.push_back(make_diagnostic(
      AngelScriptDiagnosticSeverity::kError, std::string(section), row, column,
      "unterminated host registration call"));
  return std::nullopt;
}

std::optional<std::string> parse_string_literal_at(
    std::string_view text, std::size_t offset,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  skip_plain_space(text, offset);
  if (offset >= text.size()) {
    return std::nullopt;
  }
  if (text[offset] != '"') {
    return parse_typescript_string_literal(text, offset);
  }
  std::size_t row = 0;
  std::size_t column = 0;
  return parse_string_literal(text, offset, row, column, diagnostics, section);
}

std::optional<std::string_view> extract_balanced_region(
    std::string_view text, std::size_t open_offset, char open, char close) {
  if (open_offset >= text.size() || text[open_offset] != open) {
    return std::nullopt;
  }

  std::size_t depth = 1;
  const std::size_t body_start = open_offset + 1;
  char quote = '\0';
  bool escaped = false;
  for (std::size_t offset = body_start; offset < text.size(); ++offset) {
    const char ch = text[offset];
    if (quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'' || ch == '`') {
      quote = ch;
      continue;
    }
    if (ch == open) {
      ++depth;
    } else if (ch == close) {
      --depth;
      if (depth == 0) {
        return text.substr(body_start, offset - body_start);
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> first_object_argument(std::string_view text,
                                                      std::size_t offset) {
  skip_plain_space(text, offset);
  if (offset < text.size() && text[offset] == '{') {
    return extract_balanced_region(text, offset, '{', '}');
  }
  const auto identifier = parse_ts_identifier(text, offset);
  if (!identifier) {
    return std::nullopt;
  }
  skip_plain_space(text, offset);
  if (offset >= text.size() || text[offset] != '(') {
    return std::nullopt;
  }
  const auto body = extract_balanced_region(text, offset, '(', ')');
  if (!body) {
    return std::nullopt;
  }
  std::size_t body_offset = 0;
  skip_plain_space(*body, body_offset);
  if (body_offset >= body->size() || (*body)[body_offset] != '{') {
    return std::nullopt;
  }
  return extract_balanced_region(*body, body_offset, '{', '}');
}

std::map<std::string, std::string> collect_object_definitions(
    std::string_view text) {
  std::map<std::string, std::string> definitions;
  for (std::size_t offset = 0; offset < text.size(); ++offset) {
    std::string_view keyword;
    if (starts_with_at(text, offset, "const")) {
      keyword = "const";
    } else if (starts_with_at(text, offset, "let")) {
      keyword = "let";
    } else if (starts_with_at(text, offset, "var")) {
      keyword = "var";
    } else {
      continue;
    }

    const std::size_t after_keyword = offset + keyword.size();
    if (after_keyword < text.size() &&
        is_ts_identifier_char(text[after_keyword])) {
      continue;
    }

    std::size_t cursor = after_keyword;
    const auto identifier = parse_ts_identifier(text, cursor);
    if (!identifier) {
      continue;
    }
    while (cursor < text.size() && text[cursor] != '=' &&
           text[cursor] != ';' && text[cursor] != '\n') {
      ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '=') {
      continue;
    }
    ++cursor;
    const auto object = first_object_argument(text, cursor);
    if (object) {
      definitions[*identifier] = std::string(*object);
    }
  }
  return definitions;
}

std::optional<std::string_view> resolve_object_argument(
    std::string_view body, const std::map<std::string, std::string>& objects,
    std::optional<std::string>& owned) {
  std::size_t offset = 0;
  skip_plain_space(body, offset);
  if (offset < body.size() && body[offset] == '{') {
    return extract_balanced_region(body, offset, '{', '}');
  }

  const auto identifier = parse_ts_identifier(body, offset);
  if (!identifier) {
    return std::nullopt;
  }
  const auto found = objects.find(*identifier);
  if (found == objects.end()) {
    return std::nullopt;
  }
  owned = found->second;
  return std::string_view(*owned);
}

std::optional<std::string> extract_object_string_property(
    std::string_view object, std::string_view property,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  int depth = 0;
  char quote = '\0';
  bool escaped = false;
  for (std::size_t offset = 0; offset < object.size(); ++offset) {
    const char ch = object[offset];
    if (quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'' || ch == '`') {
      quote = ch;
      continue;
    }
    if (ch == '{' || ch == '[' || ch == '(') {
      ++depth;
      continue;
    }
    if (ch == '}' || ch == ']' || ch == ')') {
      --depth;
      continue;
    }
    if (depth != 0) {
      continue;
    }

    std::size_t cursor = offset;
    std::optional<std::string> key;
    if (ch == '"' || ch == '\'') {
      key = parse_string_literal_at(object, cursor, diagnostics, section);
    } else if (is_ts_identifier_start(ch)) {
      key = parse_ts_identifier(object, cursor);
    }
    if (!key || *key != property) {
      continue;
    }
    skip_plain_space(object, cursor);
    if (cursor >= object.size() || object[cursor] != ':') {
      continue;
    }
    ++cursor;
    return parse_string_literal_at(object, cursor, diagnostics, section);
  }

  return std::nullopt;
}

std::optional<std::string> first_object_string_property(
    std::string_view object, std::initializer_list<const char*> properties,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  for (const char* property : properties) {
    auto value =
        extract_object_string_property(object, property, diagnostics, section);
    if (value) {
      return value;
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> extract_balanced_call_body(
    std::string_view text, std::size_t function_offset, std::string_view function,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  std::size_t offset = function_offset + function.size();
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
  if (offset >= text.size() || text[offset] != '(') {
    return std::nullopt;
  }

  const std::size_t body_start = offset + 1;
  int depth = 1;
  char quote = '\0';
  bool escaped = false;
  for (++offset; offset < text.size(); ++offset) {
    const char ch = text[offset];
    if (quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'' || ch == '`') {
      quote = ch;
      continue;
    }
    if (ch == '(') {
      ++depth;
    } else if (ch == ')') {
      --depth;
      if (depth == 0) {
        return text.substr(body_start, offset - body_start);
      }
    }
  }

  diagnostics.push_back(make_diagnostic(
      AngelScriptDiagnosticSeverity::kError, std::string(section), 0, 0,
      "unterminated reference extension registration call"));
  return std::nullopt;
}

std::optional<ParsedCall> parse_command_call(
    std::string_view body, const std::map<std::string, std::string>& objects,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  std::size_t offset = 0;
  skip_plain_space(body, offset);
  auto name = parse_string_literal_at(body, offset, diagnostics, section);
  if (!name) {
    return std::nullopt;
  }

  ParsedCall call;
  call.function = "qt_register_command";
  call.arguments.push_back(std::move(name.value()));
  while (offset < body.size() && body[offset] != ',') {
    ++offset;
  }
  std::optional<std::string> owned_object;
  std::optional<std::string_view> options_object;
  if (offset < body.size() && body[offset] == ',') {
    ++offset;
    options_object = resolve_object_argument(body.substr(offset),
                                                       objects, owned_object);
  }
  if (options_object) {
    if (const auto title = first_object_string_property(
            *options_object, {"title", "label"}, diagnostics, section)) {
      call.arguments.push_back(*title);
    } else {
      call.arguments.push_back({});
    }
    if (const auto description = extract_object_string_property(
            *options_object, "description", diagnostics, section)) {
      call.arguments.push_back(*description);
    }
  }
  while (!call.arguments.empty() && call.arguments.back().empty()) {
    call.arguments.pop_back();
  }
  return call;
}

std::optional<ParsedCall> parse_tool_call(
    std::string_view body, const std::map<std::string, std::string>& objects,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  std::optional<std::string> owned_object;
  const auto object =
      resolve_object_argument(body, objects, owned_object);
  if (!object) {
    diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kError,
                                          std::string(section), 0, 0,
                                          "pi.registerTool expects object"));
    return std::nullopt;
  }

  auto name =
      extract_object_string_property(*object, "name", diagnostics, section);
  if (!name) {
    diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kError,
                                          std::string(section), 0, 0,
                                          "pi.registerTool expects string name"));
    return std::nullopt;
  }

  ParsedCall call;
  call.function = "qt_register_tool";
  call.arguments.push_back(std::move(name.value()));
  if (const auto title =
          first_object_string_property(*object, {"title", "label"}, diagnostics,
                                       section)) {
    call.arguments.push_back(*title);
  } else {
    call.arguments.push_back({});
  }
  if (const auto description = extract_object_string_property(
          *object, "description", diagnostics, section)) {
    call.arguments.push_back(*description);
  }
  while (!call.arguments.empty() && call.arguments.back().empty()) {
    call.arguments.pop_back();
  }
  return call;
}

std::optional<ParsedCall> parse_lifecycle_call(
    std::string_view body, std::size_t index,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  std::size_t offset = 0;
  auto event = parse_string_literal_at(body, offset, diagnostics, section);
  if (!event) {
    return std::nullopt;
  }
  while (offset < body.size() && body[offset] != ',') {
    ++offset;
  }
  if (offset >= body.size()) {
    diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kError,
                                          std::string(section), 0, 0,
                                          "pi.on expects event, handler"));
    return std::nullopt;
  }
  ++offset;

  std::string handler;
  const auto make_synthetic_handler = [&]() {
    handler = "reference_" + *event;
    for (char& ch : handler) {
      if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
        ch = '_';
      }
    }
    handler += "_" + std::to_string(index);
  };

  if (auto identifier = parse_ts_identifier(body, offset)) {
    if (*identifier == "async") {
      std::size_t next = offset;
      if (auto next_identifier = parse_ts_identifier(body, next);
          next_identifier && *next_identifier == "function") {
        if (auto function_name = parse_ts_identifier(body, next)) {
          handler = *function_name;
        } else {
          make_synthetic_handler();
        }
      } else {
        make_synthetic_handler();
      }
    } else if (*identifier == "function") {
      if (auto function_name = parse_ts_identifier(body, offset)) {
        handler = *function_name;
      } else {
        make_synthetic_handler();
      }
    } else {
      handler = *identifier;
    }
  } else {
    make_synthetic_handler();
  }

  ParsedCall call;
  call.function = "qt_on";
  call.arguments.push_back(std::move(event.value()));
  call.arguments.push_back(std::move(handler));
  return call;
}

void append_api_calls(
    std::string_view text, std::vector<ParsedCall>& calls,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  const auto object_definitions = collect_object_definitions(text);
  std::size_t synthetic_handler_index = 0;
  for (std::size_t offset = 0; offset < text.size(); ++offset) {
    if (starts_with_at(text, offset, "pi.registerCommand")) {
      auto body = extract_balanced_call_body(text, offset, "pi.registerCommand",
                                            diagnostics, section);
      if (body) {
        auto call = parse_command_call(
            *body, object_definitions, diagnostics, section);
        if (call) {
          calls.push_back(std::move(call.value()));
        }
      }
      continue;
    }
    if (starts_with_at(text, offset, "pi.registerTool")) {
      auto body = extract_balanced_call_body(text, offset, "pi.registerTool",
                                            diagnostics, section);
      if (body) {
        auto call =
            parse_tool_call(*body, object_definitions, diagnostics,
                                      section);
        if (call) {
          calls.push_back(std::move(call.value()));
        }
      }
      continue;
    }
    if (starts_with_at(text, offset, "pi.on")) {
      auto body =
          extract_balanced_call_body(text, offset, "pi.on", diagnostics, section);
      if (body) {
        auto call = parse_lifecycle_call(
            *body, ++synthetic_handler_index, diagnostics, section);
        if (call) {
          calls.push_back(std::move(call.value()));
        }
      }
      continue;
    }
  }
}

std::vector<ParsedCall> parse_registration_calls(
    std::string_view text, std::string_view section,
    std::vector<AngelScriptDiagnostic>& diagnostics) {
  std::vector<ParsedCall> calls;
  std::size_t offset = 0;
  std::size_t row = 1;
  std::size_t column = 1;

  while (offset < text.size()) {
    if (is_function_call_at(text, offset, "qt_register_command")) {
      auto call = parse_registration_call(text, offset, row, column,
                                          "qt_register_command", diagnostics,
                                          section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    if (is_function_call_at(text, offset, "registerCommand")) {
      auto call = parse_registration_call(text, offset, row, column,
                                          "registerCommand", diagnostics,
                                          section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    if (is_function_call_at(text, offset, "qt_register_tool")) {
      auto call = parse_registration_call(text, offset, row, column,
                                          "qt_register_tool", diagnostics,
                                          section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    if (is_function_call_at(text, offset, "registerTool")) {
      auto call = parse_registration_call(text, offset, row, column,
                                          "registerTool", diagnostics,
                                          section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    if (is_function_call_at(text, offset, "qt_register_resource")) {
      auto call = parse_registration_call(text, offset, row, column,
                                          "qt_register_resource", diagnostics,
                                          section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    if (is_function_call_at(text, offset, "registerResource")) {
      auto call = parse_registration_call(text, offset, row, column,
                                          "registerResource", diagnostics,
                                          section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    if (is_function_call_at(text, offset, "qt_on")) {
      auto call = parse_registration_call(text, offset, row, column, "qt_on",
                                          diagnostics, section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    if (is_function_call_at(text, offset, "on")) {
      auto call = parse_registration_call(text, offset, row, column, "on",
                                          diagnostics, section);
      if (call) {
        calls.push_back(std::move(call.value()));
      }
      continue;
    }
    advance_position(text[offset], row, column);
    ++offset;
  }

  append_api_calls(text, calls, diagnostics, section);
  return calls;
}

std::optional<std::string> optional_argument(const ParsedCall& call,
                                             std::size_t index) {
  if (index >= call.arguments.size() || call.arguments[index].empty()) {
    return std::nullopt;
  }
  return call.arguments[index];
}

bool is_safe_extension_resource_path(std::string_view path);

void add_registration_call(AngelScriptPluginRegistration& registration,
                           const ParsedCall& call,
                           std::vector<AngelScriptDiagnostic>& diagnostics,
                           std::string_view section) {
  std::string function = call.function;
  if (function == "registerCommand") {
    function = "qt_register_command";
  } else if (function == "registerTool") {
    function = "qt_register_tool";
  } else if (function == "registerResource") {
    function = "qt_register_resource";
  } else if (function == "registerProvider") {
    function = "qt_register_provider";
  } else if (function == "on") {
    function = "qt_on";
  } else if (function == "registerFlag") {
    function = "qt_register_flag";
  } else if (function == "registerShortcut") {
    function = "qt_register_shortcut";
  } else if (function == "registerMessageRenderer") {
    function = "qt_register_message_renderer";
  }

  if (function == "qt_on") {
    if (call.arguments.size() != 2) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
          call.column, "qt_on expects event, handler"));
      return;
    }

    const auto& event = call.arguments[0];
    if (!is_valid_lifecycle_event(event)) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
          call.column, "invalid extension lifecycle event: " + event));
      return;
    }

    const auto& handler = call.arguments[1];
    if (!is_valid_handler_identifier(handler)) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
          call.column, "invalid extension lifecycle handler: " + handler));
      return;
    }

    registration.lifecycle_handlers.push_back(
        AngelScriptLifecycleRegistration{event, handler});
    return;
  }

  if (function == "qt_register_provider") {
    if (call.arguments.size() != 2) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
          call.column, "qt_register_provider expects name, config JSON"));
      return;
    }
    const auto& name = call.arguments[0];
    if (!is_valid_extension_identifier(name)) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
          call.column, "invalid extension provider name: " + name));
      return;
    }
    Json config;
    try {
      config = Json::parse(call.arguments[1]);
    } catch (const std::exception& error) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
          call.column,
          "extension provider config JSON is invalid: " +
              std::string(error.what())));
      return;
    }
    if (!config.is_object()) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
          call.column, "extension provider config must be an object"));
      return;
    }
    registration.providers.push_back(AngelScriptProviderRegistration{
        name, std::move(config)});
    return;
  }

  // Flags and shortcuts have signatures that do not share the ordinary
  // contribution arity.  Validate them before the generic name check: a
  // shortcut key is intentionally a key sequence (for example, "ctrl+x"),
  // rather than an extension identifier.
  if (function == "qt_register_flag") {
    if ((call.arguments.size() != 1 && call.arguments.size() != 4) ||
        !is_valid_extension_identifier(call.arguments.empty()
                                           ? std::string_view{}
                                           : std::string_view(call.arguments[0]))) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section),
          call.row, call.column, "qt_register_flag expects a name"));
      return;
    }
    AngelScriptFlagRegistration flag;
    flag.name = call.arguments[0];
    if (call.arguments.size() == 4) {
      flag.type = call.arguments[1];
      if (flag.type != "boolean" && flag.type != "string") {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section),
            call.row, call.column,
            "extension flag type must be boolean or string"));
        return;
      }
      try {
        flag.default_value = Json::parse(call.arguments[2]);
      } catch (...) {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section),
            call.row, call.column, "extension flag default must be JSON"));
        return;
      }
      if ((flag.type == "boolean" && !flag.default_value.is_boolean()) ||
          (flag.type == "string" && !flag.default_value.is_string())) {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section),
            call.row, call.column, "extension flag default type mismatch"));
        return;
      }
      flag.description = optional_argument(call, 3);
    }
    registration.flags.push_back(std::move(flag));
    return;
  }

  if (function == "qt_register_shortcut") {
    if ((call.arguments.size() != 1 && call.arguments.size() != 3) ||
        call.arguments[0].empty() ||
        !is_valid_handler_identifier(call.arguments.size() == 1
                                         ? call.arguments[0]
                                         : call.arguments[1])) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section),
          call.row, call.column, "qt_register_shortcut expects a key and handler"));
      return;
    }
    AngelScriptShortcutRegistration shortcut;
    shortcut.key = call.arguments[0];
    shortcut.handler = call.arguments.size() == 1 ? call.arguments[0]
                                                   : call.arguments[1];
    shortcut.description = optional_argument(call, 2);
    registration.shortcuts.push_back(std::move(shortcut));
    return;
  }

  if (function == "qt_register_message_renderer") {
    if (call.arguments.size() != 2 || call.arguments[0].empty() ||
        !is_valid_handler_identifier(call.arguments[1])) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section),
          call.row, call.column,
          "qt_register_message_renderer expects custom type and handler"));
      return;
    }
    registration.message_renderers.push_back(
        AngelScriptMessageRendererRegistration{call.arguments[0],
                                               call.arguments[1]});
    return;
  }

  if (call.arguments.empty() || call.arguments.size() > 3) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
        call.column,
        function + " expects name, optional title, optional description"));
    return;
  }

  const auto& name = call.arguments[0];
  if (!is_valid_extension_identifier(name)) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
        call.column, "invalid extension contribution name: " + name));
    return;
  }

  if (function == "qt_register_command") {
    registration.commands.push_back(AngelScriptCommandRegistration{
        name, optional_argument(call, 1), optional_argument(call, 2)});
    return;
  }

  if (function == "qt_register_tool") {
    registration.tools.push_back(AngelScriptToolRegistration{
        name, optional_argument(call, 1), optional_argument(call, 2)});
    return;
  }

  if (function == "qt_register_flag") {
    if ((call.arguments.size() != 1 && call.arguments.size() != 4) ||
        !is_valid_extension_identifier(call.arguments[0])) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section),
          call.row, call.column, "qt_register_flag expects a name"));
      return;
    }
    AngelScriptFlagRegistration flag;
    flag.name = call.arguments[0];
    if (call.arguments.size() == 4) {
      flag.type = call.arguments[1];
      if (flag.type != "boolean" && flag.type != "string") {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section),
            call.row, call.column, "extension flag type must be boolean or string"));
        return;
      }
      try {
        flag.default_value = Json::parse(call.arguments[2]);
      } catch (...) {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section),
            call.row, call.column, "extension flag default must be JSON"));
        return;
      }
      if ((flag.type == "boolean" && !flag.default_value.is_boolean()) ||
          (flag.type == "string" && !flag.default_value.is_string())) {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section),
            call.row, call.column, "extension flag default type mismatch"));
        return;
      }
      flag.description = optional_argument(call, 3);
    }
    registration.flags.push_back(std::move(flag));
    return;
  }

  if (function == "qt_register_shortcut") {
    if ((call.arguments.size() != 1 && call.arguments.size() != 3) ||
        call.arguments[0].empty() ||
        !is_valid_handler_identifier(call.arguments.size() == 1
                                         ? call.arguments[0]
                                         : call.arguments[1])) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section),
          call.row, call.column, "qt_register_shortcut expects a name"));
      return;
    }
    AngelScriptShortcutRegistration shortcut;
    shortcut.key = call.arguments[0];
    shortcut.handler = call.arguments.size() == 1 ? call.arguments[0]
                                                   : call.arguments[1];
    shortcut.description = optional_argument(call, 2);
    registration.shortcuts.push_back(std::move(shortcut));
    return;
  }

  if (call.arguments.size() < 2 || call.arguments.size() > 3) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
        call.column,
        function + " expects type, path, optional name"));
    return;
  }

  if (!is_one_of(name, {"skill", "prompt", "theme"})) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
        call.column, "invalid extension resource type: " + name));
    return;
  }

  if (call.arguments[1].empty()) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
        call.column, "extension resource path must not be empty"));
    return;
  }
  if (!is_safe_extension_resource_path(call.arguments[1])) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), call.row,
        call.column,
        "extension resource path must stay within the extension directory"));
    return;
  }

  registration.resources.push_back(AngelScriptResourceRegistration{
      name, call.arguments[1], optional_argument(call, 2)});
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

bool is_safe_extension_resource_path(std::string_view path) {
  if (path.empty()) {
    return false;
  }
  if (path.front() == '/' || path.front() == '\\') {
    return false;
  }
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 &&
      path[1] == ':') {
    return false;
  }

  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find_first_of("/\\", start);
    const std::string_view segment =
        path.substr(start, slash == std::string_view::npos
                               ? std::string_view::npos
                               : slash - start);
    if (segment == "..") {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

bool is_adapter_request(const AngelScriptCompileRequest& request) {
  return ends_with(request.script_name, ".ts") ||
         ends_with(request.script_name, ".js");
}

const AngelScriptBindingCatalog& binding_catalog() {
  static const AngelScriptBindingCatalog catalog{{
      {"Status",
       "qt::foundation::Status",
       "Operation status and error metadata.",
       {{"bool ok()", "qt/foundation/error.hpp"},
        {"string errorMessage()", "qt/foundation/error.hpp"},
        {"string errorCode()", "qt/foundation/error.hpp"}}},
      {"RepositoryView",
       "qt::harness::RepositoryView",
       "Read-only repository filesystem view.",
       {{"string cwd()", "qt/harness/repository.hpp"},
        {"bool exists(const string &in path)", "qt/harness/repository.hpp"},
        {"bool isDirectory(const string &in path)",
         "qt/harness/repository.hpp"},
        {"string readTextFile(const string &in path)",
         "qt/harness/repository.hpp"},
        {"string listDirectoryJson(const string &in path)",
         "qt/harness/repository.hpp"}}},
      {"ContextDiscovery",
       "qt::harness::RepoContextDiscovery",
       "Repository context discovery result.",
       {{"string repositoryRoot()", "qt/harness/context_discovery.hpp"},
        {"string cwd()", "qt/harness/context_discovery.hpp"},
        {"string contextFilesJson()", "qt/harness/context_discovery.hpp"},
        {"string skillRootsJson()", "qt/harness/context_discovery.hpp"},
        {"string promptRootsJson()", "qt/harness/context_discovery.hpp"}}},
      {"PromptAssembly",
       "qt::harness::PromptAssembly",
       "System prompt and instruction section assembly.",
       {{"string systemPrompt()", "qt/harness/prompt_assembly.hpp"},
        {"string sectionsJson()", "qt/harness/prompt_assembly.hpp"},
        {"int sectionCount()", "qt/harness/prompt_assembly.hpp"}}},
      {"SkillResources",
       "qt::harness::SkillResources",
       "Loaded skill resource collection.",
       {{"int count()", "qt/harness/skills.hpp"},
        {"string skillsJson()", "qt/harness/skills.hpp"},
        {"string diagnosticsJson()", "qt/harness/skills.hpp"}}},
      {"PromptTemplateResources",
       "qt::harness::PromptTemplateResources",
       "Loaded prompt template resource collection.",
       {{"int count()", "qt/harness/prompt_templates.hpp"},
        {"string templatesJson()", "qt/harness/prompt_templates.hpp"},
        {"string diagnosticsJson()", "qt/harness/prompt_templates.hpp"}}},
      {"SessionEntry",
       "qt::harness::SessionEntry",
       "Single persisted conversation/session entry.",
       {{"string id()", "qt/harness/session.hpp"},
        {"string kind()", "qt/harness/session.hpp"},
        {"string role()", "qt/harness/session.hpp"},
        {"string text()", "qt/harness/session.hpp"},
        {"string toJson()", "qt/harness/session.hpp"}}},
      {"SessionRepository",
       "qt::harness::SessionRepository",
       "Session persistence and JSONL access.",
       {{"string path()", "qt/harness/session_repository.hpp"},
        {"string loadEntriesJson()", "qt/harness/session_repository.hpp"},
        {"Status appendEntry(const string &in json)",
         "qt/harness/session_repository.hpp"},
        {"Status saveMetadata(const string &in json)",
         "qt/harness/session_repository.hpp"}}},
      {"SessionManager",
       "qt::harness::SessionManager",
       "Active session graph and context manager.",
       {{"string sessionId()", "qt/harness/session_manager.hpp"},
        {"string leafId()", "qt/harness/session_manager.hpp"},
        {"string sessionName()", "qt/harness/session_manager.hpp"},
        {"Status setSessionName(const string &in name)",
         "qt/harness/session_manager.hpp"},
        {"string buildSessionContextJson()",
         "qt/harness/session_manager.hpp"},
        {"string statsJson()", "qt/harness/session_manager.hpp"}}},
      {"AgentHarness",
       "qt::harness::AgentHarness",
       "Agent execution harness and active model/tool state.",
       {{"string modelProvider()", "qt/harness/agent_harness.hpp"},
        {"string modelName()", "qt/harness/agent_harness.hpp"},
        {"string thinkingLevel()", "qt/harness/agent_harness.hpp"},
        {"string activeToolsJson()", "qt/harness/agent_harness.hpp"},
        {"Status setActiveToolsJson(const string &in json)",
         "qt/harness/agent_harness.hpp"},
        {"Status setThinkingLevel(const string &in level)",
         "qt/harness/agent_harness.hpp"}}},
      {"ModelCatalog",
       "qt::ai::ModelCatalog",
       "Provider and model catalog.",
       {{"string providersJson()", "qt/ai/model_catalog.hpp"},
        {"string modelsJson()", "qt/ai/model_catalog.hpp"},
        {"bool hasModel(const string &in provider, const string &in model)",
         "qt/ai/model_catalog.hpp"},
        {"string modelJson(const string &in provider, const string &in model)",
         "qt/ai/model_catalog.hpp"}}},
      {"Provider",
       "qt::ai::Provider",
       "AI provider completion interface.",
       {{"string providerId()", "qt/ai/provider.hpp"},
        {"string completeJson(const string &in requestJson)",
         "qt/ai/provider.hpp"},
        {"string streamJson(const string &in requestJson)",
         "qt/ai/provider.hpp"}}},
      {"AuthStorage",
       "qt::ai::AuthStorage",
       "Provider credential storage.",
       {{"bool hasCredential(const string &in provider)",
         "qt/ai/auth_storage.hpp"},
        {"string credentialSource(const string &in provider)",
         "qt/ai/auth_storage.hpp"},
        {"Status setApiKey(const string &in provider, const string &in key)",
         "qt/ai/auth_storage.hpp"},
        {"Status logout(const string &in provider)",
         "qt/ai/auth_storage.hpp"}}},
      {"ToolDefinition",
       "qt::ai::ToolDefinition",
       "Model-facing tool declaration.",
       {{"string name()", "qt/ai/models.hpp"},
        {"string description()", "qt/ai/models.hpp"},
        {"string schemaJson()", "qt/ai/models.hpp"}}},
      {"ToolRegistry",
       "qt::tools::ToolRegistry",
       "Registered native and extension tools.",
       {{"string toolsJson()", "qt/tools/registry.hpp"},
        {"bool hasTool(const string &in name)", "qt/tools/registry.hpp"},
        {"string definitionJson(const string &in name)",
         "qt/tools/registry.hpp"},
        {"string executeJson(const string &in name, const string &in inputJson)",
         "qt/tools/registry.hpp"}}},
      {"SlashCommandRegistry",
       "qt::app::SlashCommandRegistry",
       "Interactive slash command registry.",
       {{"string commandsJson()", "qt/app/slash/slash_commands.hpp"},
        {"bool hasCommand(const string &in name)",
         "qt/app/slash/slash_commands.hpp"},
        {"string executeJson(const string &in input)",
         "qt/app/slash/slash_commands.hpp"}}},
      {"InteractiveTerminalSession",
       "qt::app::InteractiveTerminalSession",
       "Interactive transcript, overlay, and editor session.",
       {{"Status appendTranscriptLine(const string &in line)",
         "qt/app/interactive/interactive_terminal_session.hpp"},
        {"Status setStatusMessage(const string &in message)",
         "qt/app/interactive/interactive_terminal_session.hpp"},
        {"string editorText()",
         "qt/app/interactive/interactive_terminal_session.hpp"},
        {"Status setEditorText(const string &in text)",
         "qt/app/interactive/interactive_terminal_session.hpp"},
        {"Status showOverlayJson(const string &in overlayJson)",
         "qt/app/interactive/interactive_terminal_session.hpp"},
        {"Status hideOverlay()",
         "qt/app/interactive/interactive_terminal_session.hpp"}}},
      {"Editor",
       "qt::tui::Editor",
       "Prompt editor text, cursor, history, and wrapping state.",
       {{"string text()", "qt/tui/input/editor.hpp"},
        {"Status setText(const string &in text)", "qt/tui/input/editor.hpp"},
        {"int cursorRow()", "qt/tui/input/editor.hpp"},
        {"int cursorColumn()", "qt/tui/input/editor.hpp"},
        {"Status insertText(const string &in text)", "qt/tui/input/editor.hpp"},
        {"Status moveUp()", "qt/tui/input/editor.hpp"},
        {"Status moveDown()", "qt/tui/input/editor.hpp"},
        {"Status undo()", "qt/tui/input/editor.hpp"},
        {"Status redo()", "qt/tui/input/editor.hpp"}}},
      {"Terminal",
       "qt::tui::Terminal",
       "Terminal size, mode, and output sink.",
       {{"int rows()", "qt/tui/terminal/terminal.hpp"},
        {"int columns()", "qt/tui/terminal/terminal.hpp"},
        {"Status write(const string &in text)",
         "qt/tui/terminal/terminal.hpp"},
        {"Status flush()", "qt/tui/terminal/terminal.hpp"}}},
      {"LlamaProvider",
       "qt::ai::LlamaProvider",
       "Local llama.cpp runtime provider.",
       {{"string selectedModel()", "qt/ai/llama_provider.hpp"},
        {"string statusJson()", "qt/ai/llama_provider.hpp"},
        {"string runtimeStatusJson(const string &in tag)",
         "qt/ai/llama_provider.hpp"},
        {"string runJson(const string &in tag, const string &in text)",
         "qt/ai/llama_provider.hpp"},
        {"string embedJson(const string &in text)",
         "qt/ai/llama_provider.hpp"}}},
      {"ExtensionHost",
       "qt::extensions::ExtensionHost",
       "Extension contribution and lifecycle host.",
       {{"string commandsJson()", "qt/extensions/loader.hpp"},
        {"string toolsJson()", "qt/extensions/loader.hpp"},
        {"string providersJson()", "qt/extensions/loader.hpp"},
        {"Status emitLifecycle(const string &in event, const string &in json)",
         "qt/extensions/harness_lifecycle.hpp"}}},
  }};
  return catalog;
}

std::string_view trim_ascii_space(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

bool starts_with_keyword(std::string_view text, std::string_view keyword) {
  if (!starts_with_at(text, 0, keyword)) {
    return false;
  }
  return text.size() == keyword.size() ||
         !is_identifier_char(text[keyword.size()]);
}

void append_sandbox_policy_diagnostics(
    const AngelScriptCompileRequest& request, std::string_view section,
    std::vector<AngelScriptDiagnostic>& diagnostics) {
  if (request.source.size() > request.sandbox.max_source_bytes) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section), 0, 0,
        "AngelScript source exceeds sandbox size limit"));
  }
  if (request.sandbox.allow_module_imports) {
    return;
  }

  std::size_t row = 1;
  std::size_t offset = 0;
  while (offset <= request.source.size()) {
    const std::size_t newline = request.source.find('\n', offset);
    const std::size_t end =
        newline == std::string::npos ? request.source.size() : newline;
    const std::string_view line(request.source.data() + offset, end - offset);
    const std::string_view trimmed = trim_ascii_space(line);
    if (starts_with_at(trimmed, 0, "#include") ||
        starts_with_at(trimmed, 0, "#pragma") ||
        starts_with_keyword(trimmed, "import")) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), row, 1,
          "AngelScript sandbox disallows module imports and preprocessor "
          "includes"));
    }
    if (newline == std::string::npos) {
      break;
    }
    offset = newline + 1;
    ++row;
  }
}

struct ActiveEngineCompileState {
  AngelScriptPluginRegistration* registration = nullptr;
  std::vector<AngelScriptDiagnostic>* diagnostics = nullptr;
  std::string section;
};

thread_local ActiveEngineCompileState* g_active_engine_compile_state = nullptr;

void append_engine_registration_call(std::string function,
                                     std::vector<std::string> arguments) {
  if (g_active_engine_compile_state == nullptr ||
      g_active_engine_compile_state->registration == nullptr ||
      g_active_engine_compile_state->diagnostics == nullptr) {
#if defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
    if (auto* context = asGetActiveContext()) {
      context->SetException("registration APIs are only available during activate()");
    }
#endif
    return;
  }

  ParsedCall call;
  call.function = std::move(function);
  call.arguments = std::move(arguments);
  call.row = 0;
  call.column = 0;
  add_registration_call(*g_active_engine_compile_state->registration, call,
                        *g_active_engine_compile_state->diagnostics,
                        g_active_engine_compile_state->section);
}

void qt_register_command_1(const std::string& name) {
  append_engine_registration_call("qt_register_command", {name});
}

void qt_register_command_2(const std::string& name, const std::string& title) {
  append_engine_registration_call("qt_register_command", {name, title});
}

void qt_register_command_3(const std::string& name, const std::string& title,
                           const std::string& description) {
  append_engine_registration_call("qt_register_command",
                                  {name, title, description});
}

void qt_register_tool_1(const std::string& name) {
  append_engine_registration_call("qt_register_tool", {name});
}

void qt_register_tool_2(const std::string& name, const std::string& title) {
  append_engine_registration_call("qt_register_tool", {name, title});
}

void qt_register_tool_3(const std::string& name, const std::string& title,
                        const std::string& description) {
  append_engine_registration_call("qt_register_tool", {name, title, description});
}

void qt_register_resource_2(const std::string& type, const std::string& path) {
  append_engine_registration_call("qt_register_resource", {type, path});
}

void qt_register_resource_3(const std::string& type, const std::string& path,
                            const std::string& name) {
  append_engine_registration_call("qt_register_resource", {type, path, name});
}

void qt_register_provider_2(const std::string& name,
                            const std::string& config_json) {
  append_engine_registration_call("qt_register_provider", {name, config_json});
}

void qt_on_2(const std::string& event, const std::string& handler) {
  append_engine_registration_call("qt_on", {event, handler});
}

void qt_on_1(const std::string& event) {
  append_engine_registration_call("qt_on", {event});
}

void qt_register_flag_1(const std::string& name) {
  append_engine_registration_call("qt_register_flag", {name});
}

void qt_register_flag_4(const std::string& name, const std::string& type,
                        const std::string& default_json,
                        const std::string& description) {
  append_engine_registration_call("qt_register_flag",
                                  {name, type, default_json, description});
}

void qt_register_shortcut_1(const std::string& name) {
  append_engine_registration_call("qt_register_shortcut", {name});
}

void qt_register_shortcut_3(const std::string& key, const std::string& handler,
                            const std::string& description) {
  append_engine_registration_call("qt_register_shortcut",
                                  {key, handler, description});
}

void qt_register_message_renderer_2(const std::string& custom_type,
                                    const std::string& handler) {
  append_engine_registration_call("qt_register_message_renderer",
                                  {custom_type, handler});
}

#if defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
struct RuntimeCallState {
  const ExtensionHostCall* host_call = nullptr;
};

thread_local RuntimeCallState* g_runtime_call_state = nullptr;

std::string qt_host_call_runtime(const std::string& method,
                                 const std::string& arguments_json) {
  if (g_runtime_call_state == nullptr ||
      g_runtime_call_state->host_call == nullptr) {
    if (auto* context = asGetActiveContext()) {
      context->SetException("extension host is not available");
    }
    return {};
  }
  Json arguments;
  try {
    arguments = Json::parse(arguments_json);
  } catch (...) {
    if (auto* context = asGetActiveContext()) {
      context->SetException("extension host arguments must be valid JSON");
    }
    return {};
  }
  const auto result = (*g_runtime_call_state->host_call)(method, arguments);
  if (!result) {
    if (auto* context = asGetActiveContext()) {
      context->SetException(result.error().message.c_str());
    }
    return {};
  }
  return result.value().dump();
}

std::string qt_json_get_runtime(const std::string& document,
                                const std::string& key) {
  try {
    const Json value = Json::parse(document);
    if (!value.is_object()) throw std::runtime_error("JSON document must be an object");
    const auto found = value.find(key);
    return found == value.end() ? "null" : found->dump();
  } catch (const std::exception& error) {
    if (auto* context = asGetActiveContext()) context->SetException(error.what());
    return {};
  }
}

std::string qt_json_string_runtime(const std::string& value) {
  try {
    const Json parsed = Json::parse(value);
    if (!parsed.is_string()) throw std::runtime_error("JSON value is not a string");
    return parsed.get<std::string>();
  } catch (const std::exception& error) {
    if (auto* context = asGetActiveContext()) context->SetException(error.what());
    return {};
  }
}
#endif

void append_not_implemented_api_diagnostics(
    std::string_view source, ExtensionApiSourceSyntax syntax,
    std::string_view section, std::vector<AngelScriptDiagnostic>& diagnostics);

#if defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
AngelScriptDiagnosticSeverity diagnostic_severity_from_message_type(
    asEMsgType type) {
  switch (type) {
    case asMSGTYPE_WARNING:
      return AngelScriptDiagnosticSeverity::kWarning;
    case asMSGTYPE_INFORMATION:
      return AngelScriptDiagnosticSeverity::kInfo;
    case asMSGTYPE_ERROR:
    default:
      return AngelScriptDiagnosticSeverity::kError;
  }
}

struct EngineMessageCapture {
  std::vector<AngelScriptDiagnostic>* diagnostics = nullptr;
  std::string default_section;
};

void capture_engine_message(const asSMessageInfo* message, void* parameter) {
  auto* capture = static_cast<EngineMessageCapture*>(parameter);
  if (capture == nullptr || capture->diagnostics == nullptr || message == nullptr) {
    return;
  }

  const std::string section =
      (message->section != nullptr && *message->section != '\0')
          ? std::string(message->section)
          : capture->default_section;
  capture->diagnostics->push_back(make_diagnostic(
      diagnostic_severity_from_message_type(message->type), section,
      message->row < 0 ? 0U : static_cast<std::size_t>(message->row),
      message->col < 0 ? 0U : static_cast<std::size_t>(message->col),
      message->message != nullptr ? std::string(message->message)
                                  : std::string("unknown AngelScript error")));
}

void ignore_engine_message(const asSMessageInfo*, void*) {}

void append_engine_api_registration_error(
    int status, std::string declaration, std::string_view section,
    std::vector<AngelScriptDiagnostic>& diagnostics) {
  diagnostics.push_back(make_diagnostic(
      AngelScriptDiagnosticSeverity::kError, std::string(section), 0, 0,
      "failed to register AngelScript host function: " + declaration +
          " (status " + std::to_string(status) + ")"));
}

bool register_engine_binding_catalog(
    asIScriptEngine* engine, std::string_view section,
    std::vector<AngelScriptDiagnostic>& diagnostics) {
  for (const auto& binding : binding_catalog().classes) {
    const int status = engine->RegisterInterface(binding.name.c_str());
    if (status < 0) {
      diagnostics.push_back(make_diagnostic(
          AngelScriptDiagnosticSeverity::kError, std::string(section), 0, 0,
          "failed to register AngelScript interface binding: " + binding.name +
              " (status " + std::to_string(status) + ")"));
      return false;
    }
  }

  for (const auto& binding : binding_catalog().classes) {
    for (const auto& method : binding.methods) {
      const int status = engine->RegisterInterfaceMethod(
          binding.name.c_str(), method.declaration.c_str());
      if (status < 0) {
        diagnostics.push_back(make_diagnostic(
            AngelScriptDiagnosticSeverity::kError, std::string(section), 0, 0,
            "failed to register AngelScript method binding: " + binding.name +
                "::" + method.declaration + " (status " +
                std::to_string(status) + ")"));
        return false;
      }
    }
  }

  return true;
}

struct EngineSandboxBudget {
  std::size_t max_steps = 0;
  std::size_t steps = 0;
};

void enforce_engine_sandbox_budget(asIScriptContext* context, void* parameter) {
  auto* budget = static_cast<EngineSandboxBudget*>(parameter);
  if (context == nullptr || budget == nullptr || budget->max_steps == 0) {
    return;
  }
  ++budget->steps;
  if (budget->steps > budget->max_steps) {
    context->Abort();
  }
}

bool register_engine_api(asIScriptEngine* engine, std::string_view section,
                         std::vector<AngelScriptDiagnostic>& diagnostics) {
  RegisterStdString(engine);

  struct FunctionRegistration {
    const char* declaration;
    asSFuncPtr pointer;
    asDWORD call_convention;
  };

  const FunctionRegistration registrations[] = {
      {"void qt_register_command(const string &in)",
       asFUNCTION(qt_register_command_1), asCALL_CDECL},
      {"void qt_register_command(const string &in, const string &in)",
       asFUNCTION(qt_register_command_2), asCALL_CDECL},
      {"void qt_register_command(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_command_3), asCALL_CDECL},
      {"void registerCommand(const string &in)",
       asFUNCTION(qt_register_command_1), asCALL_CDECL},
      {"void registerCommand(const string &in, const string &in)",
       asFUNCTION(qt_register_command_2), asCALL_CDECL},
      {"void registerCommand(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_command_3), asCALL_CDECL},
      {"void qt_register_tool(const string &in)", asFUNCTION(qt_register_tool_1),
       asCALL_CDECL},
      {"void qt_register_tool(const string &in, const string &in)",
       asFUNCTION(qt_register_tool_2), asCALL_CDECL},
      {"void qt_register_tool(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_tool_3), asCALL_CDECL},
      {"void registerTool(const string &in)", asFUNCTION(qt_register_tool_1),
       asCALL_CDECL},
      {"void registerTool(const string &in, const string &in)",
       asFUNCTION(qt_register_tool_2), asCALL_CDECL},
      {"void registerTool(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_tool_3), asCALL_CDECL},
      {"void qt_register_resource(const string &in, const string &in)",
       asFUNCTION(qt_register_resource_2), asCALL_CDECL},
      {"void qt_register_resource(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_resource_3), asCALL_CDECL},
      {"void registerResource(const string &in, const string &in)",
       asFUNCTION(qt_register_resource_2), asCALL_CDECL},
      {"void registerResource(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_resource_3), asCALL_CDECL},
      {"void qt_register_provider(const string &in, const string &in)",
       asFUNCTION(qt_register_provider_2), asCALL_CDECL},
      {"void registerProvider(const string &in, const string &in)",
       asFUNCTION(qt_register_provider_2), asCALL_CDECL},
      {"void qt_on(const string &in)", asFUNCTION(qt_on_1), asCALL_CDECL},
      {"string qt_host_call(const string &in, const string &in)",
       asFUNCTION(qt_host_call_runtime), asCALL_CDECL},
      {"string qt_json_get(const string &in, const string &in)",
       asFUNCTION(qt_json_get_runtime), asCALL_CDECL},
      {"string qt_json_string(const string &in)",
       asFUNCTION(qt_json_string_runtime), asCALL_CDECL},
      {"void qt_on(const string &in, const string &in)", asFUNCTION(qt_on_2),
       asCALL_CDECL},
      {"void on(const string &in)", asFUNCTION(qt_on_1), asCALL_CDECL},
      {"void on(const string &in, const string &in)", asFUNCTION(qt_on_2),
       asCALL_CDECL},
      {"void qt_register_flag(const string &in)",
       asFUNCTION(qt_register_flag_1), asCALL_CDECL},
      {"void qt_register_flag(const string &in, const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_flag_4), asCALL_CDECL},
      {"void registerFlag(const string &in)", asFUNCTION(qt_register_flag_1),
       asCALL_CDECL},
      {"void registerFlag(const string &in, const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_flag_4), asCALL_CDECL},
      {"void qt_register_shortcut(const string &in)",
       asFUNCTION(qt_register_shortcut_1), asCALL_CDECL},
      {"void qt_register_shortcut(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_shortcut_3), asCALL_CDECL},
      {"void registerShortcut(const string &in)",
       asFUNCTION(qt_register_shortcut_1), asCALL_CDECL},
      {"void registerShortcut(const string &in, const string &in, const string &in)",
       asFUNCTION(qt_register_shortcut_3), asCALL_CDECL},
      {"void qt_register_message_renderer(const string &in, const string &in)",
       asFUNCTION(qt_register_message_renderer_2), asCALL_CDECL},
      {"void registerMessageRenderer(const string &in, const string &in)",
       asFUNCTION(qt_register_message_renderer_2), asCALL_CDECL},
  };

  for (const auto& registration : registrations) {
    const int status = engine->RegisterGlobalFunction(
        registration.declaration, registration.pointer,
        registration.call_convention);
    if (status < 0) {
      append_engine_api_registration_error(status, registration.declaration,
                                           section, diagnostics);
      return false;
    }
  }

  return true;
}

AngelScriptCompileResult compile_with_engine(
    const AngelScriptCompileRequest& request,
    const AngelScriptDependencyInfo& dependency, const std::string& section) {
  AngelScriptCompileResult result;
  result.dependency = dependency;
  result.registration = request.seed_registration;

  EngineMessageCapture message_capture{&result.diagnostics, section};
  append_not_implemented_api_diagnostics(
      request.source, ExtensionApiSourceSyntax::kAngelScript, section,
      result.diagnostics);
  if (result.has_errors()) {
    result.ok = false;
    return result;
  }

  asIScriptEngine* engine = asCreateScriptEngine();
  if (engine == nullptr) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "failed to create AngelScript engine"));
    result.ok = false;
    return result;
  }

  const int callback_status =
      engine->SetMessageCallback(asFUNCTION(capture_engine_message),
                                 &message_capture, asCALL_CDECL);
  if (callback_status < 0) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "failed to register AngelScript message callback"));
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  if (!register_engine_api(engine, section, result.diagnostics)) {
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }
  if (!register_engine_binding_catalog(engine, section, result.diagnostics)) {
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  asIScriptModule* module =
      engine->GetModule(request.module_name.c_str(), asGM_ALWAYS_CREATE);
  if (module == nullptr) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "failed to create AngelScript module"));
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  const int add_section_status =
      module->AddScriptSection(section.c_str(), request.source.c_str(),
                               request.source.size(), 0);
  if (add_section_status < 0) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "failed to add AngelScript section"));
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  const int build_status = module->Build();
  if (build_status < 0) {
    result.ok = !result.has_errors();
    engine->ShutDownAndRelease();
    return result;
  }

  asIScriptFunction* activate = module->GetFunctionByDecl("void activate()");
  if (activate == nullptr) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "AngelScript entrypoint 'void activate()' was not found"));
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  asIScriptContext* context = engine->CreateContext();
  if (context == nullptr) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "failed to create AngelScript execution context"));
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  const int prepare_status = context->Prepare(activate);
  if (prepare_status < 0) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "failed to prepare AngelScript entrypoint"));
    context->Release();
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  EngineSandboxBudget budget{request.sandbox.max_activation_steps, 0};
  const int line_callback_status =
      context->SetLineCallback(asFUNCTION(enforce_engine_sandbox_budget),
                               &budget, asCALL_CDECL);
  if (line_callback_status < 0) {
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0,
        "failed to register AngelScript sandbox execution budget"));
    context->Release();
    engine->ShutDownAndRelease();
    result.ok = false;
    return result;
  }

  ActiveEngineCompileState active_state{&result.registration, &result.diagnostics,
                                        section};
  g_active_engine_compile_state = &active_state;
  const int execute_status = context->Execute();
  g_active_engine_compile_state = nullptr;

  if (execute_status != asEXECUTION_FINISHED) {
    std::string message = "AngelScript execution failed";
    if (execute_status == asEXECUTION_EXCEPTION) {
      const char* exception = context->GetExceptionString();
      if (exception != nullptr && *exception != '\0') {
        message += ": ";
        message += exception;
      }
    } else if (execute_status == asEXECUTION_ABORTED &&
               budget.max_steps > 0 && budget.steps > budget.max_steps) {
      message += ": sandbox activation step budget exceeded";
    } else {
      message += " (status " + std::to_string(execute_status) + ")";
    }
    result.diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, section, 0, 0, message));
  }

  context->Release();
  engine->ShutDownAndRelease();
  result.ok = !result.has_errors();
  return result;
}
#endif

void append_stub_dependency_diagnostic(
    const AngelScriptDependencyInfo& dependency,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section);

void append_stub_syntax_diagnostics(
    std::string_view source, std::string_view section,
    std::vector<AngelScriptDiagnostic>& diagnostics);

void append_not_implemented_api_diagnostics(
    std::string_view source, ExtensionApiSourceSyntax syntax,
    std::string_view section, std::vector<AngelScriptDiagnostic>& diagnostics) {
  for (const auto& diagnostic :
       find_not_implemented_extension_api_uses(source, syntax)) {
    diagnostics.push_back(make_diagnostic(
        AngelScriptDiagnosticSeverity::kError, std::string(section),
        diagnostic.row, diagnostic.column, diagnostic.message));
  }
}

AngelScriptCompileResult compile_with_static_parser(
    const AngelScriptCompileRequest& request,
    const AngelScriptDependencyInfo& dependency, const std::string& section,
    bool include_dependency_warning) {
  AngelScriptCompileResult result;
  result.dependency = dependency;
  result.registration = request.seed_registration;

  if (include_dependency_warning) {
    append_stub_dependency_diagnostic(result.dependency, result.diagnostics,
                                      section);
  }
  append_stub_syntax_diagnostics(request.source, section, result.diagnostics);
  append_not_implemented_api_diagnostics(
      request.source,
      is_adapter_request(request) ? ExtensionApiSourceSyntax::kReferenceModule
                                  : ExtensionApiSourceSyntax::kAngelScript,
      section, result.diagnostics);

  const auto calls =
      parse_registration_calls(request.source, section, result.diagnostics);
  for (const auto& call : calls) {
    add_registration_call(result.registration, call, result.diagnostics,
                          section);
  }

  result.ok = !result.has_errors();
  return result;
}

void append_stub_dependency_diagnostic(
    const AngelScriptDependencyInfo& dependency,
    std::vector<AngelScriptDiagnostic>& diagnostics, std::string_view section) {
  if (dependency.available) {
    return;
  }

  std::string message = "AngelScript dependency unavailable";
  if (!dependency.diagnostics.empty()) {
    message += ": " + dependency.diagnostics.front();
  }
  diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kWarning,
                                        std::string(section), 0, 0,
                                        std::move(message)));
}

bool has_unbalanced_delimiter(std::string_view text, char open, char close) {
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (char ch : text) {
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
      continue;
    }
    if (ch == open) {
      ++depth;
    } else if (ch == close) {
      --depth;
      if (depth < 0) {
        return true;
      }
    }
  }
  return depth != 0;
}

void append_stub_syntax_diagnostics(
    std::string_view source, std::string_view section,
    std::vector<AngelScriptDiagnostic>& diagnostics) {
  if (source.find("qt_compile_error") != std::string_view::npos) {
    diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kError,
                                          std::string(section), 0, 0,
                                          "script requested diagnostic failure"));
  }
  if (has_unbalanced_delimiter(source, '(', ')')) {
    diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kError,
                                          std::string(section), 0, 0,
                                          "unbalanced parentheses"));
  }
  if (has_unbalanced_delimiter(source, '{', '}')) {
    diagnostics.push_back(make_diagnostic(AngelScriptDiagnosticSeverity::kError,
                                          std::string(section), 0, 0,
                                          "unbalanced braces"));
  }
}

}  // namespace

bool AngelScriptCompileResult::has_errors() const noexcept {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == AngelScriptDiagnosticSeverity::kError) {
      return true;
    }
  }
  return false;
}

std::string to_string(AngelScriptDiagnosticSeverity severity) {
  switch (severity) {
    case AngelScriptDiagnosticSeverity::kInfo:
      return "info";
    case AngelScriptDiagnosticSeverity::kWarning:
      return "warning";
    case AngelScriptDiagnosticSeverity::kError:
      return "error";
  }
  return "info";
}

AngelScriptDependencyInfo detect_angelscript_dependency() {
  AngelScriptDependencyInfo info;
#if defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
  info.available = true;
  info.linked = true;
  info.source = "vendored PlatformIO package";
  if (const char* version = asGetLibraryVersion();
      version != nullptr && *version != '\0') {
    info.version = version;
  }
  info.diagnostics.push_back(
      "AngelScript engine and std::string add-on are linked from the vendored "
      "native package");
#else
  info.diagnostics.push_back(
      "https://github.com/anjo76/angelscript#v2.38.0 is not PlatformIO-ready as "
      "a git lib_deps entry in this tree; use a vendored or packaged SDK "
      "layout before enabling the real engine adapter");
#endif
  return info;
}

AngelScriptPluginRegistration adapt_manifest_to_angelscript_registration(
    const ExtensionManifest& manifest) {
  AngelScriptPluginRegistration registration;
  registration.manifest_name = manifest.name;
  registration.display_name = manifest.display_name;
  registration.metadata = manifest.metadata;

  registration.commands.reserve(manifest.commands.size());
  for (const auto& command : manifest.commands) {
    registration.commands.push_back(AngelScriptCommandRegistration{
        command.name, command.title, command.description});
  }

  registration.tools.reserve(manifest.tools.size());
  for (const auto& tool : manifest.tools) {
    registration.tools.push_back(
        AngelScriptToolRegistration{tool.name, tool.title, tool.description});
  }

  registration.providers.reserve(manifest.providers.size());
  for (const auto& provider : manifest.providers) {
    registration.providers.push_back(
        AngelScriptProviderRegistration{provider.name, provider.config});
  }

  return registration;
}

const AngelScriptBindingCatalog& angelscript_binding_catalog() {
  return binding_catalog();
}

AngelScriptHost::AngelScriptHost()
    : dependency_(detect_angelscript_dependency()) {}

AngelScriptHost::AngelScriptHost(AngelScriptDependencyInfo dependency)
    : dependency_(std::move(dependency)) {}

const AngelScriptDependencyInfo& AngelScriptHost::dependency() const noexcept {
  return dependency_;
}

Result<AngelScriptCompileResult> AngelScriptHost::compile(
    const AngelScriptCompileRequest& request) const {
  if (request.module_name.empty()) {
    return Result<AngelScriptCompileResult>::failure(
        ErrorCode::invalid_argument, "AngelScript module name is required");
  }

  const std::string section =
      request.script_name.empty() ? std::string("<memory>") : request.script_name;
  if (is_adapter_request(request)) {
    return compile_with_static_parser(request, dependency_, section, false);
  }

  std::vector<AngelScriptDiagnostic> sandbox_diagnostics;
  append_sandbox_policy_diagnostics(request, section, sandbox_diagnostics);
  if (!sandbox_diagnostics.empty()) {
    AngelScriptCompileResult result;
    result.dependency = dependency_;
    result.registration = request.seed_registration;
    result.diagnostics = std::move(sandbox_diagnostics);
    result.ok = false;
    return result;
  }

#if defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
  if (dependency_.available && dependency_.linked) {
    return compile_with_engine(request, dependency_, section);
  }
#endif

  return compile_with_static_parser(request, dependency_, section, true);
}

struct AngelScriptRuntime::Impl {
  AngelScriptPluginRegistration registration;
#if defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
  asIScriptEngine* engine = nullptr;
  asIScriptModule* module = nullptr;
  std::recursive_mutex mutex;
  std::size_t max_activation_steps = 100000U;
  ~Impl() {
    if (engine != nullptr) {
      engine->ShutDownAndRelease();
      engine = nullptr;
      module = nullptr;
    }
  }
#endif
};

thread_local unsigned g_runtime_invoke_depth = 0;

AngelScriptRuntime::AngelScriptRuntime() = default;
AngelScriptRuntime::~AngelScriptRuntime() = default;

AngelScriptRuntime::AngelScriptRuntime(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

AngelScriptRuntime::AngelScriptRuntime(AngelScriptRuntime&&) noexcept = default;
AngelScriptRuntime& AngelScriptRuntime::operator=(AngelScriptRuntime&&) noexcept =
    default;

const AngelScriptPluginRegistration& AngelScriptRuntime::registration() const {
  static const AngelScriptPluginRegistration empty;
  return impl_ == nullptr ? empty : impl_->registration;
}

qt::foundation::Result<AngelScriptRuntime> AngelScriptRuntime::create(
    const AngelScriptCompileRequest& request) {
  if (request.module_name.empty()) {
    return Result<AngelScriptRuntime>::failure(
        ErrorCode::invalid_argument, "AngelScript module name is required");
  }
#if !defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
  return Result<AngelScriptRuntime>::failure(
      ErrorCode::not_found, "AngelScript runtime is unavailable");
#else
  auto impl = std::make_unique<Impl>();
  impl->registration = request.seed_registration;
  impl->max_activation_steps = request.sandbox.max_activation_steps;
  const std::string section = request.script_name.empty()
                                   ? std::string("<memory>")
                                   : request.script_name;
  std::vector<AngelScriptDiagnostic> diagnostics;
  append_sandbox_policy_diagnostics(request, section, diagnostics);
  append_not_implemented_api_diagnostics(
      request.source, ExtensionApiSourceSyntax::kAngelScript, section,
      diagnostics);
  if (!diagnostics.empty()) {
    return Result<AngelScriptRuntime>::failure(
        ErrorCode::invalid_argument, diagnostics.front().message);
  }

  EngineMessageCapture message_capture{&diagnostics, section};
  impl->engine = asCreateScriptEngine();
  const auto shutdown_engine = [&impl]() {
    if (impl->engine != nullptr) {
      impl->engine->ShutDownAndRelease();
      impl->engine = nullptr;
      impl->module = nullptr;
    }
  };
  if (impl->engine == nullptr ||
      impl->engine->SetMessageCallback(asFUNCTION(capture_engine_message),
                                       &message_capture, asCALL_CDECL) < 0 ||
      !register_engine_api(impl->engine, section, diagnostics) ||
      !register_engine_binding_catalog(impl->engine, section, diagnostics)) {
    shutdown_engine();
    return Result<AngelScriptRuntime>::failure(
        ErrorCode::invalid_argument,
        diagnostics.empty() ? "failed to initialize AngelScript runtime"
                             : diagnostics.front().message);
  }
  impl->module = impl->engine->GetModule(request.module_name.c_str(),
                                         asGM_ALWAYS_CREATE);
  if (impl->module == nullptr ||
      impl->module->AddScriptSection(section.c_str(), request.source.c_str(),
                                     request.source.size(), 0) < 0 ||
      impl->module->Build() < 0) {
    shutdown_engine();
    return Result<AngelScriptRuntime>::failure(
        ErrorCode::invalid_argument, "failed to build AngelScript module");
  }
  auto* activate = impl->module->GetFunctionByDecl("void activate()");
  if (activate == nullptr) {
    shutdown_engine();
    return Result<AngelScriptRuntime>::failure(
        ErrorCode::invalid_argument,
        "AngelScript entrypoint 'void activate()' was not found");
  }
  auto* context = impl->engine->CreateContext();
  if (context == nullptr || context->Prepare(activate) < 0) {
    if (context != nullptr) context->Release();
    shutdown_engine();
    return Result<AngelScriptRuntime>::failure(
        ErrorCode::invalid_argument, "failed to prepare AngelScript activation");
  }
  EngineSandboxBudget budget{request.sandbox.max_activation_steps, 0};
  context->SetLineCallback(asFUNCTION(enforce_engine_sandbox_budget), &budget,
                           asCALL_CDECL);
  ActiveEngineCompileState active_state{&impl->registration, &diagnostics,
                                        section};
  g_active_engine_compile_state = &active_state;
  const int status = context->Execute();
  g_active_engine_compile_state = nullptr;
  context->Release();
  impl->engine->SetMessageCallback(asFUNCTION(ignore_engine_message), nullptr,
                                   asCALL_CDECL);
  const bool has_errors = std::any_of(
      diagnostics.begin(), diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.severity == AngelScriptDiagnosticSeverity::kError;
      });
  if (status != asEXECUTION_FINISHED || has_errors) {
    shutdown_engine();
    return Result<AngelScriptRuntime>::failure(
        ErrorCode::invalid_argument,
        diagnostics.empty() ? "AngelScript activation failed"
                             : diagnostics.front().message);
  }
  return Result<AngelScriptRuntime>::success(
      AngelScriptRuntime(std::move(impl)));
#endif
}

qt::foundation::Result<Json> AngelScriptRuntime::invoke(
    std::string_view handler, const Json& arguments,
    const ExtensionHostCall& host_call) {
#if !defined(QT_EXTENSIONS_HAS_ANGELSCRIPT_HEADER)
  (void)handler;
  (void)arguments;
  (void)host_call;
  return Result<Json>::failure(ErrorCode::not_found,
                               "AngelScript runtime is unavailable");
#else
  if (impl_ == nullptr || impl_->engine == nullptr || handler.empty()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 "AngelScript handler is unavailable");
  }
  std::unique_lock<std::recursive_mutex> lock(impl_->mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 "AngelScript runtime invocation is reentrant");
  }
  if (g_runtime_invoke_depth >= 8U) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 "AngelScript runtime invocation depth exceeded");
  }
  struct InvokeDepthGuard {
    InvokeDepthGuard() { ++g_runtime_invoke_depth; }
    ~InvokeDepthGuard() { --g_runtime_invoke_depth; }
  } depth_guard;
  const std::string name(handler);
  asIScriptFunction* function =
      impl_->module->GetFunctionByDecl(("string " + name +
                                        "(const string &in)")
                                           .c_str());
  bool returns_string = function != nullptr;
  if (function == nullptr) {
    function = impl_->module->GetFunctionByDecl(
        ("void " + name + "(const string &in)").c_str());
  }
  if (function == nullptr) {
    function = impl_->module->GetFunctionByDecl(("void " + name + "()").c_str());
  }
  if (function == nullptr) {
    return Result<Json>::failure(ErrorCode::not_found,
                                 "AngelScript handler was not found");
  }
  auto* context = impl_->engine->CreateContext();
  if (context == nullptr || context->Prepare(function) < 0) {
    if (context != nullptr) context->Release();
    return Result<Json>::failure(ErrorCode::internal_error,
                                 "failed to prepare AngelScript handler");
  }
  std::string input = arguments.dump();
  if (function->GetParamCount() == 1) {
    context->SetArgObject(0, &input);
  }
  RuntimeCallState state{&host_call};
  RuntimeCallState* previous_call_state = g_runtime_call_state;
  g_runtime_call_state = &state;
  EngineSandboxBudget budget{impl_->max_activation_steps, 0};
  context->SetLineCallback(asFUNCTION(enforce_engine_sandbox_budget), &budget,
                           asCALL_CDECL);
  const int status = context->Execute();
  g_runtime_call_state = previous_call_state;
  if (status != asEXECUTION_FINISHED) {
    const char* exception = context->GetExceptionString();
    const std::string message = exception != nullptr && *exception != '\0'
                                    ? exception
                                    : "AngelScript handler execution failed";
    context->Release();
    return Result<Json>::failure(ErrorCode::io_error, message);
  }
  if (!returns_string) {
    context->Release();
    return Result<Json>::success(Json(nullptr));
  }
  const auto* returned = static_cast<const std::string*>(context->GetReturnObject());
  if (returned == nullptr) {
    context->Release();
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 "AngelScript handler returned no JSON");
  }
  try {
    Json result = Json::parse(*returned);
    context->Release();
    return Result<Json>::success(std::move(result));
  } catch (...) {
    context->Release();
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 "AngelScript handler returned invalid JSON");
  }
#endif
}

}  // namespace qt::extensions
