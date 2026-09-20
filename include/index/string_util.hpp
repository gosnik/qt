#pragma once

#include <string>
#include <vector>

namespace qt::index {

std::string lowercase_ascii(std::string value);
std::string trim_ascii(std::string value);
std::vector<std::string> split_shell_like(const std::string& text);

}  // namespace qt::index
