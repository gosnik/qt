#include "qt/tui/input/keybindings.hpp"

#include <map>
#include <set>

namespace qt::tui {
namespace {

std::vector<KeyId> normalize_keys(const std::vector<KeyId>& keys) {
  std::vector<KeyId> normalized;
  std::set<KeyId> seen;
  for (const auto& key : keys) {
    if (seen.insert(key).second) {
      normalized.push_back(key);
    }
  }
  return normalized;
}

}  // namespace

const KeybindingDefinitions& default_keybindings() {
  static const KeybindingDefinitions definitions{
      {"tui.input.submit", {{"enter"}, "Submit input"}},
      {"tui.input.tab", {{"tab", "ctrl+space"}, "Insert tab or trigger completion"}},
      {"tui.input.cancel", {{"escape", "ctrl+c"}, "Cancel input"}},
      {"tui.input.newLine", {{"shift+enter"}, "Insert newline"}},
      {"tui.input.copy", {{"ctrl+c"}, "Copy selection"}},
      {"tui.cursor.up", {{"up", "ctrl+p"}, "Move up"}},
      {"tui.cursor.down", {{"down", "ctrl+n"}, "Move down"}},
      {"tui.cursor.left", {{"left", "ctrl+b"}, "Move left"}},
      {"tui.cursor.right", {{"right", "ctrl+f"}, "Move right"}},
      {"tui.edit.backspace", {{"backspace"}, "Delete backward"}},
      {"tui.edit.delete", {{"ctrl+d"}, "Delete forward"}},
      {"tui.edit.delete_word_backward", {{"ctrl+w"}, "Delete word backward"}},
      {"tui.edit.delete_word_forward", {{"alt+d"}, "Delete word forward"}},
      {"tui.edit.yank", {{"ctrl+y"}, "Yank deleted text"}},
      {"tui.edit.yank_pop", {{"alt+y"}, "Cycle yanked text"}},
      {"tui.editor.cursorUp", {{"up"}, "Move cursor up"}},
      {"tui.editor.cursorDown", {{"down"}, "Move cursor down"}},
      {"tui.editor.cursorLeft", {{"left", "ctrl+b"}, "Move cursor left"}},
      {"tui.editor.cursorRight", {{"right", "ctrl+f"}, "Move cursor right"}},
      {"tui.editor.cursorWordLeft", {{"alt+left", "ctrl+left", "alt+b"},
                                      "Move cursor word left"}},
      {"tui.editor.cursorWordRight", {{"alt+right", "ctrl+right", "alt+f"},
                                       "Move cursor word right"}},
      {"tui.editor.cursorLineStart", {{"home", "ctrl+a"}, "Move to line start"}},
      {"tui.editor.cursorLineEnd", {{"end", "ctrl+e"}, "Move to line end"}},
      {"tui.editor.pageUp", {{"pageUp"}, "Page up"}},
      {"tui.editor.pageDown", {{"pageDown"}, "Page down"}},
      {"tui.editor.deleteCharBackward", {{"backspace"}, "Delete character backward"}},
      {"tui.editor.deleteCharForward", {{"delete", "ctrl+d"}, "Delete character forward"}},
      {"tui.editor.deleteWordBackward", {{"ctrl+w", "alt+backspace"},
                                          "Delete word backward"}},
      {"tui.editor.deleteWordForward", {{"alt+d", "alt+delete"},
                                         "Delete word forward"}},
      {"tui.editor.deleteToLineStart", {{"ctrl+u"}, "Delete to line start"}},
      {"tui.editor.deleteToLineEnd", {{"ctrl+k"}, "Delete to line end"}},
      {"tui.editor.yank", {{"ctrl+y"}, "Yank"}},
      {"tui.editor.yankPop", {{"alt+y"}, "Yank pop"}},
      {"tui.editor.undo", {{"ctrl+-", "ctrl+_"}, "Undo"}},
      {"tui.editor.jump_forward", {{"ctrl+]"}, "Jump forward to character"}},
      {"tui.editor.jump_backward", {{"ctrl+alt+]"}, "Jump backward to character"}},
      {"tui.editor.jumpForward", {{"ctrl+]"}, "Jump forward to character"}},
      {"tui.editor.jumpBackward", {{"ctrl+alt+]"}, "Jump backward to character"}},
      {"app.interrupt", {{"escape"}, "Cancel or abort"}},
      {"app.clear", {{"ctrl+c"}, "Clear editor"}},
      {"app.exit", {{"ctrl+d"}, "Exit when editor is empty"}},
      {"app.editor.external", {{"ctrl+g"}, "Open external editor"}},
      {"app.suspend", {{"ctrl+z"}, "Suspend to background"}},
      {"app.clipboard.pasteImage", {{"ctrl+v"}, "Paste image from clipboard"}},
      {"app.message.dequeue", {{"alt+up"}, "Restore queued messages"}},
      {"tui.select.up", {{"up"}, "Move selection up"}},
      {"tui.select.down", {{"down"}, "Move selection down"}},
      {"tui.select.pageUp", {{"pageUp"}, "Selection page up"}},
      {"tui.select.pageDown", {{"pageDown"}, "Selection page down"}},
      {"tui.select.confirm", {{"enter"}, "Confirm selection"}},
      {"tui.select.cancel", {{"escape", "ctrl+c"}, "Cancel selection"}},
  };
  return definitions;
}

KeybindingManager::KeybindingManager(KeybindingDefinitions definitions)
    : definitions_(std::move(definitions)) {}

KeybindingManager::KeybindingManager(KeybindingDefinitions definitions,
                                     KeybindingsConfig user_bindings)
    : definitions_(std::move(definitions)), user_bindings_(std::move(user_bindings)) {}

bool KeybindingManager::matches(std::string_view input,
                                const std::string& action) const {
  for (const auto& key : resolved_keys_for(action)) {
    if (matches_key(input, key)) {
      return true;
    }
  }
  return false;
}

std::vector<KeyId> KeybindingManager::keys_for(const std::string& action) const {
  return resolved_keys_for(action);
}

std::vector<KeybindingConflict> KeybindingManager::conflicts() const {
  std::map<KeyId, std::vector<std::string>> by_key;
  for (const auto& [action, keys] : user_bindings_) {
    if (definitions_.find(action) == definitions_.end()) {
      continue;
    }
    for (const auto& key : normalize_keys(keys)) {
      by_key[key].push_back(action);
    }
  }

  std::vector<KeybindingConflict> conflicts;
  for (const auto& [key, actions] : by_key) {
    if (actions.size() > 1) {
      conflicts.push_back(KeybindingConflict{key, actions});
    }
  }
  return conflicts;
}

void KeybindingManager::set_user_bindings(KeybindingsConfig user_bindings) {
  user_bindings_ = std::move(user_bindings);
}

const KeybindingsConfig& KeybindingManager::user_bindings() const noexcept {
  return user_bindings_;
}

KeybindingsConfig KeybindingManager::resolved_bindings() const {
  KeybindingsConfig resolved;
  for (const auto& [action, _definition] : definitions_) {
    resolved[action] = resolved_keys_for(action);
  }
  return resolved;
}

std::vector<KeyId> KeybindingManager::resolved_keys_for(const std::string& action) const {
  const auto it = definitions_.find(action);
  if (it == definitions_.end()) {
    return {};
  }
  const auto user = user_bindings_.find(action);
  if (user != user_bindings_.end()) {
    return normalize_keys(user->second);
  }
  return normalize_keys(it->second.default_keys);
}

KeybindingManager& global_keybindings() {
  static KeybindingManager manager;
  return manager;
}

void set_keybindings(KeybindingManager keybindings) {
  global_keybindings() = std::move(keybindings);
}

KeybindingManager& get_keybindings() { return global_keybindings(); }

const KeybindingManager& get_keybindings_const() { return global_keybindings(); }

}  // namespace qt::tui
