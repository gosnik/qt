#include <qt/extensions/api_compatibility.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace qt::extensions {
namespace {

bool is_identifier_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' ||
         ch == '$';
}

bool starts_with_at(std::string_view text, std::size_t offset,
                    std::string_view token) {
  return offset + token.size() <= text.size() &&
         text.substr(offset, token.size()) == token;
}

std::size_t skip_plain_space(std::string_view text, std::size_t offset) {
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
  return offset;
}

bool is_api_call_at(std::string_view text, std::size_t offset,
                    std::string_view api) {
  if (!starts_with_at(text, offset, api)) {
    return false;
  }
  if (offset > 0 &&
      (is_identifier_char(text[offset - 1]) || text[offset - 1] == '.')) {
    return false;
  }
  const std::size_t after = offset + api.size();
  if (after < text.size() && is_identifier_char(text[after])) {
    return false;
  }
  return skip_plain_space(text, after) < text.size() &&
         text[skip_plain_space(text, after)] == '(';
}

struct SourceCursor {
  std::size_t row = 1;
  std::size_t column = 1;

  void advance(char ch) {
    if (ch == '\n') {
      ++row;
      column = 1;
      return;
    }
    ++column;
  }
};

std::string not_implemented_message(std::string_view api) {
  std::string message = "not_implemented: extension API ";
  message += api;
  message += " is not implemented by the native extension host";
  return message;
}

const std::vector<ExtensionApiCompatibilityEntry>& table() {
  static const std::vector<ExtensionApiCompatibilityEntry> entries = {
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.on",
       ExtensionApiSupport::kSupported, "Lifecycle handler discovery"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.registerCommand",
       ExtensionApiSupport::kSupported, "Slash command contribution discovery"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.registerTool",
       ExtensionApiSupport::kSupported, "Tool contribution discovery"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.registerFlag",
       ExtensionApiSupport::kSupported, "Flag contribution discovery"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.registerProvider",
       ExtensionApiSupport::kSupported, "Provider contribution discovery"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.unregisterProvider",
       ExtensionApiSupport::kSupported, "Provider unregister discovery metadata"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.sendMessage",
       ExtensionApiSupport::kNotImplemented, "Session mutation API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.sendUserMessage",
       ExtensionApiSupport::kNotImplemented, "Session mutation API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.appendEntry",
       ExtensionApiSupport::kNotImplemented, "Session mutation API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.setSessionName",
       ExtensionApiSupport::kNotImplemented, "Session metadata API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.getSessionName",
       ExtensionApiSupport::kNotImplemented, "Session metadata API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.setLabel",
       ExtensionApiSupport::kNotImplemented, "Session metadata API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.getCommands",
       ExtensionApiSupport::kNotImplemented, "Command query API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.registerMessageRenderer",
       ExtensionApiSupport::kNotImplemented, "Custom renderer API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.registerShortcut",
       ExtensionApiSupport::kNotImplemented, "Shortcut API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.exec",
       ExtensionApiSupport::kNotImplemented, "Extension shell API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.getActiveTools",
       ExtensionApiSupport::kNotImplemented, "Active tools API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.getAllTools",
       ExtensionApiSupport::kNotImplemented, "Active tools API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.setActiveTools",
       ExtensionApiSupport::kNotImplemented, "Active tools API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.setModel",
       ExtensionApiSupport::kNotImplemented, "Model mutation API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.getThinkingLevel",
       ExtensionApiSupport::kNotImplemented, "Thinking-level API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "pi.setThinkingLevel",
       ExtensionApiSupport::kNotImplemented, "Thinking-level API pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.select",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.confirm",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.input",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.editor",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.notify",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.custom",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.onTerminalInput",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setStatus",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setWorkingMessage",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setWorkingVisible",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule,
       "ctx.ui.setWorkingIndicator", ExtensionApiSupport::kNotImplemented,
       "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule,
       "ctx.ui.setHiddenThinkingLabel", ExtensionApiSupport::kNotImplemented,
       "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setWidget",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setFooter",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setHeader",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setTitle",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.pasteToEditor",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setEditorText",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.getEditorText",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule,
       "ctx.ui.addAutocompleteProvider", ExtensionApiSupport::kNotImplemented,
       "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule,
       "ctx.ui.setEditorComponent", ExtensionApiSupport::kNotImplemented,
       "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule,
       "ctx.ui.getEditorComponent", ExtensionApiSupport::kNotImplemented,
       "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.getAllThemes",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.getTheme",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setTheme",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.getToolsExpanded",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kReferenceModule, "ctx.ui.setToolsExpanded",
       ExtensionApiSupport::kNotImplemented, "Extension UI presenter pending"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_on",
       ExtensionApiSupport::kSupported, "Lifecycle handler discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_host_call",
       ExtensionApiSupport::kSupported, "Native runtime and mode UI dispatch"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_json_get",
       ExtensionApiSupport::kSupported, "JSON field access"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_json_string",
       ExtensionApiSupport::kSupported, "JSON string decoding"},
      {ExtensionApiSourceSyntax::kAngelScript, "on",
       ExtensionApiSupport::kSupported, "Lifecycle handler discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_register_command",
       ExtensionApiSupport::kSupported, "Command contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "registerCommand",
       ExtensionApiSupport::kSupported, "Command contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_register_tool",
       ExtensionApiSupport::kSupported, "Tool contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "registerTool",
       ExtensionApiSupport::kSupported, "Tool contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_register_resource",
       ExtensionApiSupport::kSupported, "Resource contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "registerResource",
       ExtensionApiSupport::kSupported, "Resource contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_register_provider",
       ExtensionApiSupport::kSupported, "Provider contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "registerProvider",
       ExtensionApiSupport::kSupported, "Provider contribution discovery"},
      {ExtensionApiSourceSyntax::kAngelScript, "Status",
       ExtensionApiSupport::kSupported, "Core status binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "RepositoryView",
       ExtensionApiSupport::kSupported, "Repository binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "AgentHarness",
       ExtensionApiSupport::kSupported, "Agent harness binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "SessionManager",
       ExtensionApiSupport::kSupported, "Session manager binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "ToolRegistry",
       ExtensionApiSupport::kSupported, "Tool registry binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "InteractiveTerminalSession",
       ExtensionApiSupport::kSupported, "Interactive TUI binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "Editor",
       ExtensionApiSupport::kSupported, "Editor binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "ModelCatalog",
       ExtensionApiSupport::kSupported, "Model catalog binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "Provider",
       ExtensionApiSupport::kSupported, "Provider binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "LlamaProvider",
       ExtensionApiSupport::kSupported, "llama.cpp provider binding interface"},
      {ExtensionApiSourceSyntax::kAngelScript, "registerFlag",
       ExtensionApiSupport::kSupported, "AngelScript flag registration"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_register_flag",
       ExtensionApiSupport::kSupported, "AngelScript flag registration"},
      {ExtensionApiSourceSyntax::kAngelScript, "registerShortcut",
       ExtensionApiSupport::kSupported, "AngelScript shortcut registration"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_register_shortcut",
       ExtensionApiSupport::kSupported, "AngelScript shortcut registration"},
      {ExtensionApiSourceSyntax::kAngelScript, "registerMessageRenderer",
       ExtensionApiSupport::kSupported, "AngelScript custom message renderer registration"},
      {ExtensionApiSourceSyntax::kAngelScript, "qt_register_message_renderer",
       ExtensionApiSupport::kSupported, "AngelScript custom message renderer registration"},
  };
  return entries;
}

}  // namespace

const std::vector<ExtensionApiCompatibilityEntry>&
extension_api_compatibility_table() {
  return table();
}

std::vector<ExtensionApiUseDiagnostic> find_not_implemented_extension_api_uses(
    std::string_view source, ExtensionApiSourceSyntax syntax) {
  std::vector<ExtensionApiUseDiagnostic> diagnostics;
  std::set<std::pair<std::string, std::size_t>> emitted;

  SourceCursor cursor;
  bool in_line_comment = false;
  bool in_block_comment = false;
  char string_quote = '\0';
  bool escaped = false;

  for (std::size_t offset = 0; offset < source.size(); ++offset) {
    const char ch = source[offset];

    if (in_line_comment) {
      in_line_comment = ch != '\n';
      cursor.advance(ch);
      continue;
    }
    if (in_block_comment) {
      if (ch == '*' && offset + 1U < source.size() && source[offset + 1U] == '/') {
        cursor.advance(ch);
        ++offset;
        cursor.advance(source[offset]);
        in_block_comment = false;
        continue;
      }
      cursor.advance(ch);
      continue;
    }
    if (string_quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == string_quote) {
        string_quote = '\0';
      }
      cursor.advance(ch);
      continue;
    }

    if (ch == '/' && offset + 1U < source.size()) {
      if (source[offset + 1U] == '/') {
        cursor.advance(ch);
        ++offset;
        cursor.advance(source[offset]);
        in_line_comment = true;
        continue;
      }
      if (source[offset + 1U] == '*') {
        cursor.advance(ch);
        ++offset;
        cursor.advance(source[offset]);
        in_block_comment = true;
        continue;
      }
    }
    if (ch == '"' || ch == '\'' || ch == '`') {
      string_quote = ch;
      escaped = false;
      cursor.advance(ch);
      continue;
    }

    for (const auto& entry : table()) {
      if (entry.syntax != syntax ||
          entry.support != ExtensionApiSupport::kNotImplemented) {
        continue;
      }
      if (!is_api_call_at(source, offset, entry.api)) {
        continue;
      }
      const auto key = std::make_pair(entry.api, offset);
      if (!emitted.insert(key).second) {
        continue;
      }
      diagnostics.push_back(ExtensionApiUseDiagnostic{
          entry.api, not_implemented_message(entry.api), offset, cursor.row,
          cursor.column});
    }

    cursor.advance(ch);
  }

  std::sort(diagnostics.begin(), diagnostics.end(),
            [](const ExtensionApiUseDiagnostic& lhs,
               const ExtensionApiUseDiagnostic& rhs) {
              return lhs.offset < rhs.offset;
            });
  return diagnostics;
}

}  // namespace qt::extensions
