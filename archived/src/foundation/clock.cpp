#include "qt/foundation/clock.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace qt::foundation {

Timestamp SystemClock::now() const { return system_timestamp(); }

FixedClock::FixedClock(Timestamp timestamp) : timestamp_(timestamp) {}

Timestamp FixedClock::now() const { return timestamp_; }

void FixedClock::set_now(Timestamp timestamp) { timestamp_ = timestamp; }

void FixedClock::advance(std::chrono::milliseconds duration) { timestamp_ += duration; }

Timestamp system_timestamp() { return std::chrono::system_clock::now(); }

std::int64_t unix_millis(Timestamp timestamp) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               timestamp.time_since_epoch())
        .count();
}

std::int64_t current_unix_millis(const Clock& clock) noexcept {
    return unix_millis(clock.now());
}

Timestamp timestamp_from_unix_millis(std::int64_t millis) noexcept {
    return Timestamp(std::chrono::milliseconds(millis));
}

std::string format_timestamp_utc(Timestamp timestamp) {
    const std::int64_t millis = unix_millis(timestamp);
    auto seconds_part = millis / 1000;
    auto millis_part = millis % 1000;
    if (millis_part < 0) {
        millis_part += 1000;
        --seconds_part;
    }
    const std::time_t seconds = static_cast<std::time_t>(seconds_part);

    std::tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif

    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << millis_part << 'Z';
    return stream.str();
}

}  // namespace qt::foundation
