#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace qt::foundation {

using Uuid = std::array<std::uint8_t, 16>;

Uuid generate_uuid_v4();
std::string format_uuid(const Uuid& uuid);
std::string generate_uuid_v4_string();

}  // namespace qt::foundation
