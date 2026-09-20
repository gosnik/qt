#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace qt::extensions {

enum class ExtensionApiSourceSyntax {
  kReferenceModule,
  kAngelScript,
};

enum class ExtensionApiSupport {
  kSupported,
  kNotImplemented,
};

struct ExtensionApiCompatibilityEntry {
  ExtensionApiSourceSyntax syntax = ExtensionApiSourceSyntax::kReferenceModule;
  std::string api;
  ExtensionApiSupport support = ExtensionApiSupport::kSupported;
  std::string note;
};

struct ExtensionApiUseDiagnostic {
  std::string api;
  std::string message;
  std::size_t offset = 0;
  std::size_t row = 1;
  std::size_t column = 1;
};

const std::vector<ExtensionApiCompatibilityEntry>&
extension_api_compatibility_table();

std::vector<ExtensionApiUseDiagnostic> find_not_implemented_extension_api_uses(
    std::string_view source, ExtensionApiSourceSyntax syntax);

}  // namespace qt::extensions
