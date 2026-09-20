#include "qt/foundation/logging.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace qt::foundation {
namespace {

bool is_key_start(char value) {
    return std::isalpha(static_cast<unsigned char>(value)) != 0 || value == '_';
}

bool is_key_char(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '_' ||
           value == '.' || value == '-';
}

bool is_reserved_key(std::string_view key) {
    return key == "level" || key == "component" || key == "name" ||
           key == "message";
}

std::string normalize_key(std::string_view key) {
    std::string normalized;
    normalized.reserve(key.size() + 6);

    for (char value : key) {
        normalized.push_back(is_key_char(value) ? value : '_');
    }

    if (normalized.empty()) {
        normalized = "field";
    } else if (!is_key_start(normalized.front())) {
        normalized.insert(0, "field_");
    }

    if (is_reserved_key(normalized)) {
        normalized.insert(0, "field_");
    }

    return normalized;
}

bool needs_quotes(std::string_view value) {
    if (value.empty()) {
        return true;
    }

    return std::any_of(value.begin(), value.end(), [](char ch) {
        return std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '"' ||
               ch == '\\' || ch == '=';
    });
}

std::string quote(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    escaped.push_back('"');
    return escaped;
}

std::string format_string_value(std::string_view value) {
    if (needs_quotes(value)) {
        return quote(value);
    }
    return std::string(value);
}

std::string format_log_value(const LogValue& value) {
    return std::visit(
        [](const auto& entry) -> std::string {
            using Entry = std::decay_t<decltype(entry)>;
            if constexpr (std::is_same_v<Entry, std::string>) {
                return format_string_value(entry);
            } else if constexpr (std::is_same_v<Entry, bool>) {
                return entry ? "true" : "false";
            } else if constexpr (std::is_same_v<Entry, double>) {
                std::ostringstream stream;
                stream << std::setprecision(15) << entry;
                return stream.str();
            } else {
                return std::to_string(entry);
            }
        },
        value);
}

spdlog::level::level_enum to_spdlog_level(LogLevel level) {
    switch (level) {
        case LogLevel::trace:
            return spdlog::level::trace;
        case LogLevel::debug:
            return spdlog::level::debug;
        case LogLevel::info:
            return spdlog::level::info;
        case LogLevel::warn:
            return spdlog::level::warn;
        case LogLevel::error:
            return spdlog::level::err;
        case LogLevel::critical:
            return spdlog::level::critical;
        case LogLevel::off:
            return spdlog::level::off;
    }
    return spdlog::level::info;
}

std::string env_or_empty(const char* name) {
    if (const char* value = std::getenv(name)) {
        return std::string(value);
    }
    return {};
}

bool env_truthy(std::string_view value) {
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::vector<std::string> split_paths(std::string_view value) {
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find(':', start);
        std::string path(value.substr(
            start, separator == std::string_view::npos
                       ? std::string_view::npos
                       : separator - start));
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return paths;
}

std::string lowercase_ascii(std::string_view value) {
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return lowered;
}

class SpdlogSink final : public LogSink {
  public:
    explicit SpdlogSink(std::shared_ptr<spdlog::logger> logger)
        : logger_(std::move(logger)) {}

    void write(const LogRecord& record, std::string formatted) override {
        if (!logger_) {
            return;
        }
        logger_->log(to_spdlog_level(record.level), "{}", formatted);
    }

  private:
    std::shared_ptr<spdlog::logger> logger_;
};

std::mutex& global_logging_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<LogSink>& global_sink() {
    static std::shared_ptr<LogSink> sink;
    return sink;
}

LogLevel& global_level() {
    static LogLevel level = LogLevel::off;
    return level;
}

}  // namespace

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::trace:
            return "trace";
        case LogLevel::debug:
            return "debug";
        case LogLevel::info:
            return "info";
        case LogLevel::warn:
            return "warn";
        case LogLevel::error:
            return "error";
        case LogLevel::critical:
            return "critical";
        case LogLevel::off:
            return "off";
    }

    return "unknown";
}

std::optional<LogLevel> parse_log_level(std::string_view level) noexcept {
    const auto normalized = lowercase_ascii(level);
    if (normalized == "trace") {
        return LogLevel::trace;
    }
    if (normalized == "debug") {
        return LogLevel::debug;
    }
    if (normalized == "info") {
        return LogLevel::info;
    }
    if (normalized == "warn" || normalized == "warning") {
        return LogLevel::warn;
    }
    if (normalized == "error") {
        return LogLevel::error;
    }
    if (normalized == "critical") {
        return LogLevel::critical;
    }
    if (normalized == "off" || normalized == "none") {
        return LogLevel::off;
    }
    return std::nullopt;
}

LogField log_field(std::string key, std::string value) {
    return LogField{std::move(key), std::move(value)};
}

LogField log_field(std::string key, std::string_view value) {
    return LogField{std::move(key), std::string(value)};
}

LogField log_field(std::string key, const char* value) {
    return LogField{std::move(key), std::string(value == nullptr ? "" : value)};
}

LogField log_field(std::string key, bool value) {
    return LogField{std::move(key), value};
}

LogField log_field(std::string key, int value) {
    return LogField{std::move(key), static_cast<std::int64_t>(value)};
}

LogField log_field(std::string key, std::int64_t value) {
    return LogField{std::move(key), value};
}

LogField log_field(std::string key, unsigned int value) {
    return LogField{std::move(key), static_cast<std::uint64_t>(value)};
}

LogField log_field(std::string key, std::uint64_t value) {
    return LogField{std::move(key), value};
}

LogField log_field(std::string key, double value) {
    return LogField{std::move(key), value};
}

std::string format_log_record(const LogRecord& record) {
    std::ostringstream stream;
    stream << "level=" << to_string(record.level)
           << " component=" << format_string_value(record.component)
           << " name=" << format_string_value(record.name)
           << " message=" << format_string_value(record.message);

    for (const auto& field : record.fields) {
        stream << ' ' << normalize_key(field.key) << '=' << format_log_value(field.value);
    }

    return stream.str();
}

void VectorLogSink::write(const LogRecord& record, std::string formatted) {
    records_.push_back(record);
    lines_.push_back(std::move(formatted));
}

const std::vector<LogRecord>& VectorLogSink::records() const noexcept {
    return records_;
}

const std::vector<std::string>& VectorLogSink::lines() const noexcept {
    return lines_;
}

void VectorLogSink::clear() {
    records_.clear();
    lines_.clear();
}

OstreamLogSink::OstreamLogSink(std::ostream& stream) : stream_(&stream) {}

void OstreamLogSink::write(const LogRecord& record, std::string formatted) {
    (void)record;
    if (stream_ != nullptr) {
        *stream_ << formatted << '\n';
    }
}

Logger::Logger(std::string component, std::string name, std::shared_ptr<LogSink> sink)
    : component_(std::move(component)), name_(std::move(name)), sink_(std::move(sink)) {}

const std::string& Logger::component() const noexcept { return component_; }

const std::string& Logger::name() const noexcept { return name_; }

LogLevel Logger::minimum_level() const noexcept { return minimum_level_; }

void Logger::set_minimum_level(LogLevel level) noexcept { minimum_level_ = level; }

bool Logger::enabled(LogLevel level) const noexcept {
    return sink_ != nullptr && level != LogLevel::off && minimum_level_ != LogLevel::off &&
           static_cast<int>(level) >= static_cast<int>(minimum_level_);
}

void Logger::log(LogLevel level, std::string message, std::vector<LogField> fields) const {
    if (!enabled(level)) {
        return;
    }

    LogRecord record;
    record.level = level;
    record.component = component_;
    record.name = name_;
    record.message = std::move(message);
    record.fields = std::move(fields);
    sink_->write(record, format_log_record(record));
}

void Logger::trace(std::string message, std::vector<LogField> fields) const {
    log(LogLevel::trace, std::move(message), std::move(fields));
}

void Logger::debug(std::string message, std::vector<LogField> fields) const {
    log(LogLevel::debug, std::move(message), std::move(fields));
}

void Logger::info(std::string message, std::vector<LogField> fields) const {
    log(LogLevel::info, std::move(message), std::move(fields));
}

void Logger::warn(std::string message, std::vector<LogField> fields) const {
    log(LogLevel::warn, std::move(message), std::move(fields));
}

void Logger::error(std::string message, std::vector<LogField> fields) const {
    log(LogLevel::error, std::move(message), std::move(fields));
}

void Logger::critical(std::string message, std::vector<LogField> fields) const {
    log(LogLevel::critical, std::move(message), std::move(fields));
}

void configure_logging(const LoggingOptions& options) {
    std::lock_guard<std::mutex> lock(global_logging_mutex());
    std::vector<spdlog::sink_ptr> sinks;

    for (const auto& file : options.files) {
        if (!file.empty()) {
            const auto parent = std::filesystem::path(file).parent_path();
            if (!parent.empty()) {
                std::error_code ignored;
                std::filesystem::create_directories(parent, ignored);
            }
            sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                file, true));
        }
    }
    if (options.stderr_enabled) {
        sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    }

    if (sinks.empty() || options.level == LogLevel::off) {
        global_sink().reset();
        global_level() = LogLevel::off;
        return;
    }

    auto spd_logger = std::make_shared<spdlog::logger>(
        "qt", sinks.begin(), sinks.end());
    spd_logger->set_level(to_spdlog_level(options.level));
    spd_logger->set_pattern("[%Y-%m-%dT%H:%M:%S.%e%z] %v");
    spd_logger->flush_on(spdlog::level::warn);
    spdlog::drop("qt");
    spdlog::register_logger(spd_logger);
    spdlog::set_default_logger(spd_logger);
    global_sink() = std::make_shared<SpdlogSink>(std::move(spd_logger));
    global_level() = options.level;
}

LoggingOptions logging_options_from_environment() {
    LoggingOptions options;
    if (const auto files = env_or_empty("QT_LOG_FILE"); !files.empty()) {
        options.files = split_paths(files);
    }
    if (const auto stderr_value = env_or_empty("QT_LOG_STDERR");
        !stderr_value.empty()) {
        options.stderr_enabled = env_truthy(stderr_value);
    }
    if (const auto level_value = env_or_empty("QT_LOG_LEVEL");
        !level_value.empty()) {
        if (const auto parsed = parse_log_level(level_value)) {
            options.level = *parsed;
        }
    }
    return options;
}

Logger logger(std::string component, std::string name) {
    std::lock_guard<std::mutex> lock(global_logging_mutex());
    Logger value(std::move(component), std::move(name), global_sink());
    value.set_minimum_level(global_level());
    return value;
}

void shutdown_logging() {
    std::lock_guard<std::mutex> lock(global_logging_mutex());
    if (global_sink()) {
        spdlog::shutdown();
    }
    global_sink().reset();
    global_level() = LogLevel::off;
}

}  // namespace qt::foundation
