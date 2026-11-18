#pragma once

#include "qt/tools/package_manager_tool.hpp"

namespace qt::tools::package_manager_internal {

qt::foundation::Result<Json> read_settings_json_if_present(
    const qt::foundation::Path& path);

}  // namespace qt::tools::package_manager_internal
