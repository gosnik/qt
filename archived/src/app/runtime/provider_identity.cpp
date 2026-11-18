#include "qt/app/runtime/provider_identity.hpp"

#include <algorithm>
#include <cctype>

namespace qt::app {

std::string trim_copy(std::string_view value) {
  const auto first =
      std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
      });
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
      }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

std::string lowercase_copy(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return result;
}

std::string provider_key(std::string_view value) {
  return lowercase_copy(trim_copy(value));
}

bool provider_ids_equal(std::string_view lhs, std::string_view rhs) {
  return provider_key(lhs) == provider_key(rhs);
}

bool contains_provider_id(const std::vector<std::string>& providers,
                          std::string_view provider) {
  const std::string wanted = provider_key(provider);
  if (wanted.empty()) {
    return false;
  }

  return std::any_of(providers.begin(), providers.end(),
                     [&](std::string_view candidate) {
                       return provider_key(candidate) == wanted;
                     });
}

}  // namespace qt::app
