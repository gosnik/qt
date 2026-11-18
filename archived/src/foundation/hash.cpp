#include "qt/foundation/hash.hpp"

#include <iomanip>
#include <sstream>

namespace qt::foundation {

std::uint64_t stable_hash64(std::string_view value) noexcept {
    std::uint64_t hash = kStableHash64Offset;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= kStableHash64Prime;
    }
    return hash;
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0') << std::setw(16)
           << value;
    return stream.str();
}

std::string stable_hash_hex(std::string_view value) {
    return hex_u64(stable_hash64(value));
}

std::string make_stable_session_id(std::string_view seed, std::string_view prefix) {
    const std::string suffix = stable_hash_hex(seed);
    if (prefix.empty()) {
        return suffix;
    }
    return std::string(prefix) + "-" + suffix;
}

}  // namespace qt::foundation
