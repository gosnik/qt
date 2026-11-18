#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace qt::memory {

struct OfflineImportPackage {
  std::string format;
  std::size_t schema_version = 0;
  nlohmann::json state;
  std::vector<std::string> source_scopes;
  std::vector<std::string> warnings;
};

class QtmemV3ImportAdapter final {
 public:
  static bool is_sqlite_database(const std::filesystem::path& path);
  static OfflineImportPackage read(const std::filesystem::path& path);
};

}  // namespace qt::memory
