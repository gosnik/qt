#pragma once

#include "qt/tui/input/keys.hpp"

#include <map>
#include <string>
#include <vector>

namespace qt::tui {

struct KeybindingDefinition {
  std::vector<KeyId> default_keys;
  std::string description;
};

struct KeybindingConflict {
  KeyId key;
  std::vector<std::string> actions;
};

using KeybindingDefinitions = std::map<std::string, KeybindingDefinition>;
using KeybindingsConfig = std::map<std::string, std::vector<KeyId>>;

const KeybindingDefinitions& default_keybindings();

class KeybindingManager {
 public:
  explicit KeybindingManager(KeybindingDefinitions definitions =
                                 default_keybindings());
  KeybindingManager(KeybindingDefinitions definitions, KeybindingsConfig user_bindings);

  bool matches(std::string_view input, const std::string& action) const;
  std::vector<KeyId> keys_for(const std::string& action) const;
  std::vector<KeybindingConflict> conflicts() const;
  void set_user_bindings(KeybindingsConfig user_bindings);
  [[nodiscard]] const KeybindingsConfig& user_bindings() const noexcept;
  [[nodiscard]] KeybindingsConfig resolved_bindings() const;

 private:
  [[nodiscard]] std::vector<KeyId> resolved_keys_for(const std::string& action) const;

  KeybindingDefinitions definitions_;
  KeybindingsConfig user_bindings_;
};

void set_keybindings(KeybindingManager keybindings);
[[nodiscard]] KeybindingManager& get_keybindings();
[[nodiscard]] const KeybindingManager& get_keybindings_const();

}  // namespace qt::tui
