#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace qt::foundation {

enum class LogLevel {
    trace = 0,
    debug,
    info,
    warn,
    error,
    critical,
    off,
};

const char* to_string(LogLevel level) noexcept;
std::optional<LogLevel> parse_log_level(std::string_view level) noexcept;

using LogValue = std::variant<std::string, bool, std::int64_t, std::uint64_t, double>;

struct LogField {
    std::string key;
    LogValue value;
};

LogField log_field(std::string key, std::string value);
LogField log_field(std::string key, std::string_view value);
LogField log_field(std::string key, const char* value);
LogField log_field(std::string key, bool value);
LogField log_field(std::string key, int value);
LogField log_field(std::string key, std::int64_t value);
LogField log_field(std::string key, unsigned int value);
LogField log_field(std::string key, std::uint64_t value);
LogField log_field(std::string key, double value);

struct LogRecord {
    LogLevel level = LogLevel::info;
    std::string component;
    std::string name;
    std::string message;
    std::vector<LogField> fields;
};

std::string format_log_record(const LogRecord& record);

class LogSink {
  public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record, std::string formatted) = 0;
};

class VectorLogSink final : public LogSink {
  public:
    void write(const LogRecord& record, std::string formatted) override;

    [[nodiscard]] const std::vector<LogRecord>& records() const noexcept;
    [[nodiscard]] const std::vector<std::string>& lines() const noexcept;
    void clear();

  private:
    std::vector<LogRecord> records_;
    std::vector<std::string> lines_;
};

class OstreamLogSink final : public LogSink {
  public:
    explicit OstreamLogSink(std::ostream& stream);

    void write(const LogRecord& record, std::string formatted) override;

  private:
    std::ostream* stream_;
};

class Logger {
  public:
    Logger(std::string component, std::string name, std::shared_ptr<LogSink> sink);

    [[nodiscard]] const std::string& component() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] LogLevel minimum_level() const noexcept;

    void set_minimum_level(LogLevel level) noexcept;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;

    void log(LogLevel level, std::string message,
             std::vector<LogField> fields = {}) const;

    void trace(std::string message, std::vector<LogField> fields = {}) const;
    void debug(std::string message, std::vector<LogField> fields = {}) const;
    void info(std::string message, std::vector<LogField> fields = {}) const;
    void warn(std::string message, std::vector<LogField> fields = {}) const;
    void error(std::string message, std::vector<LogField> fields = {}) const;
    void critical(std::string message, std::vector<LogField> fields = {}) const;

  private:
    std::string component_;
    std::string name_;
    std::shared_ptr<LogSink> sink_;
    LogLevel minimum_level_ = LogLevel::info;
};

struct LoggingOptions {
    std::vector<std::string> files;
    bool stderr_enabled = false;
    LogLevel level = LogLevel::info;
};

void configure_logging(const LoggingOptions& options);
LoggingOptions logging_options_from_environment();
Logger logger(std::string component, std::string name);
void shutdown_logging();

}  // namespace qt::foundation
