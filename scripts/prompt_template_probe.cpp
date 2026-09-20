#include <qt/harness/prompt_templates.hpp>

#include <iostream>
#include <string>
#include <vector>

int main() {
  const std::vector<std::string> arguments{"first", "second", "third"};
  struct Case {
    std::string content;
    std::string expected;
  };
  const std::vector<Case> cases{
      {"$1/$3/$4/$0", "first/third//"},
      {"${@:2:1}", "second"},
      {"${@:0:2}", "first second"},
      {"${@:2:0}", ""},
      {"${@:2}", "second third"},
      {"$18446744073709551617", ""},
      {"${@:18446744073709551617}", ""},
      {"${@:2:18446744073709551615}", "second third"},
      {"${@:2:18446744073709551616}", "second third"},
      {"${@:" + std::string(400, '9') + "}", ""},
      {"${@:2:" + std::string(400, '9') + "}", "second third"},
  };
  int failures = 0;
  for (const auto& test : cases) {
    const auto actual = qt::harness::expand_prompt_template_content(test.content, arguments);
    if (actual != test.expected) {
      std::cerr << "[FAIL] " << test.content << ": expected '" << test.expected
                << "', got '" << actual << "'\n";
      ++failures;
    }
  }
  if (failures == 0) {
    std::cout << "[PASS] prompt positions and slices handle oversized numbers safely\n";
  }
  return failures == 0 ? 0 : 1;
}
