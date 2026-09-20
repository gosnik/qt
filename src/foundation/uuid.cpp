#include "qt/foundation/uuid.hpp"

#include <chrono>
#include <exception>
#include <random>

namespace qt::foundation {

namespace {

std::mt19937_64 make_uuid_engine() {
    try {
        std::random_device random;
        std::seed_seq seed {
            random(),
            random(),
            random(),
            random(),
            static_cast<unsigned int>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count()),
        };
        return std::mt19937_64(seed);
    } catch (const std::exception&) {
        const auto ticks = static_cast<std::mt19937_64::result_type>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        return std::mt19937_64(ticks);
    }
}

std::mt19937_64& uuid_engine() {
    static thread_local std::mt19937_64 engine = make_uuid_engine();
    return engine;
}

char hex_digit(std::uint8_t value) {
    constexpr char digits[] = "0123456789abcdef";
    return digits[value & 0x0f];
}

}  // namespace

Uuid generate_uuid_v4() {
    Uuid uuid {};
    auto& engine = uuid_engine();
    for (std::size_t offset = 0; offset < uuid.size(); offset += sizeof(std::uint64_t)) {
        std::uint64_t value = engine();
        for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
            uuid[offset + byte] = static_cast<std::uint8_t>(value & 0xffu);
            value >>= 8u;
        }
    }

    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x40u);
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
    return uuid;
}

std::string format_uuid(const Uuid& uuid) {
    std::string output;
    output.reserve(36);
    for (std::size_t index = 0; index < uuid.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output.push_back('-');
        }
        output.push_back(hex_digit(static_cast<std::uint8_t>(uuid[index] >> 4u)));
        output.push_back(hex_digit(uuid[index]));
    }
    return output;
}

std::string generate_uuid_v4_string() { return format_uuid(generate_uuid_v4()); }

}  // namespace qt::foundation
