#pragma once

#include "qt/foundation/error.hpp"
#include "qt/foundation/filesystem.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace qt::foundation {

enum class ConfigValueKind {
    scalar,
    map,
};

class ConfigValue {
  public:
    using Map = std::map<std::string, ConfigValue>;

    ConfigValue();

    static ConfigValue scalar(std::string value);
    static ConfigValue mapping(Map values = {});

    ConfigValueKind kind() const noexcept;
    bool is_scalar() const noexcept;
    bool is_map() const noexcept;

    const std::string& scalar() const;
    const Map& map() const;

    const ConfigValue* find(std::string_view key) const;
    const ConfigValue* find_path(std::string_view dotted_path) const;

    Result<bool> as_bool() const;
    Result<std::int64_t> as_integer() const;
    Result<double> as_number() const;

  private:
    ConfigValueKind kind_ = ConfigValueKind::map;
    std::string scalar_;
    Map map_;
};

struct ConfigDocument {
    ConfigValue root;

    const ConfigValue* find(std::string_view key) const;
    const ConfigValue* find_path(std::string_view dotted_path) const;
};

Result<ConfigDocument> parse_config(std::string_view content);
Result<ConfigDocument> load_config_file(const Path& path);

}  // namespace qt::foundation
