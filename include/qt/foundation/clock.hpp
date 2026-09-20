#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace qt::foundation {

using Timestamp = std::chrono::system_clock::time_point;

class Clock {
  public:
    virtual ~Clock() = default;
    virtual Timestamp now() const = 0;
};

class SystemClock final : public Clock {
  public:
    Timestamp now() const override;
};

class FixedClock final : public Clock {
  public:
    explicit FixedClock(Timestamp timestamp);

    Timestamp now() const override;
    void set_now(Timestamp timestamp);
    void advance(std::chrono::milliseconds duration);

  private:
    Timestamp timestamp_;
};

Timestamp system_timestamp();
std::int64_t unix_millis(Timestamp timestamp) noexcept;
std::int64_t current_unix_millis(const Clock& clock) noexcept;
Timestamp timestamp_from_unix_millis(std::int64_t millis) noexcept;
std::string format_timestamp_utc(Timestamp timestamp);

}  // namespace qt::foundation
