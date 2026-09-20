#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace qt::app {

std::string trim_copy(std::string_view value);
std::string lowercase_copy(std::string_view value);
std::string provider_key(std::string_view value);
bool provider_ids_equal(std::string_view lhs, std::string_view rhs);
bool contains_provider_id(const std::vector<std::string>& providers,
                          std::string_view provider);

}  // namespace qt::app
