#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace qt::foundation {

constexpr std::uint64_t kStableHash64Offset = 14695981039346656037ull;
constexpr std::uint64_t kStableHash64Prime = 1099511628211ull;

std::uint64_t stable_hash64(std::string_view value) noexcept;
std::string hex_u64(std::uint64_t value);
std::string stable_hash_hex(std::string_view value);
std::string make_stable_session_id(std::string_view seed,
                                   std::string_view prefix = "session");

}  // namespace qt::foundation
