#pragma once

#include <string>
#include <string_view>

namespace qt::foundation {

/// Build the reference-format Pi user-agent string.
/// Format: pi/<version> (<platform>; <runtime>; <arch>)
[[nodiscard]] std::string pi_user_agent(std::string_view version = "0.0.0");

}  // namespace qt::foundation
