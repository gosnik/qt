#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace qt::foundation {

enum class ErrorCode {
    none = 0,
    invalid_argument,
    not_found,
    io_error,
    encoding_error,
    internal_error,
};

const char* to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::none;
    std::string message;
    int system_error = 0;

    Error() = default;
    Error(ErrorCode error_code, std::string text, int system_error_code = 0);

    bool ok() const noexcept;
    explicit operator bool() const noexcept;
    std::string summary() const;
};

class Status {
  public:
    Status() = default;
    explicit Status(Error error);

    static Status success();
    static Status failure(ErrorCode code, std::string message, int system_error = 0);

    bool ok() const noexcept;
    explicit operator bool() const noexcept;
    const Error& error() const;

  private:
    std::optional<Error> error_;
};

template <typename T>
class Result {
  public:
    Result(const T& value) : value_(value) {}
    Result(T&& value) : value_(std::move(value)) {}
    Result(const Error& error) : error_(error) {}
    Result(Error&& error) : error_(std::move(error)) {}

    static Result success(const T& value) { return Result(value); }
    static Result success(T&& value) { return Result(std::move(value)); }
    static Result failure(ErrorCode code, std::string message, int system_error = 0) {
        return Result(Error{code, std::move(message), system_error});
    }

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const T& value() const {
        if (!value_) {
            throw std::logic_error("result has no value");
        }
        return *value_;
    }

    T& value() {
        if (!value_) {
            throw std::logic_error("result has no value");
        }
        return *value_;
    }

    const Error& error() const {
        if (value_) {
            throw std::logic_error("result has no error");
        }
        return *error_;
    }

    T value_or(T fallback) const {
        if (value_) {
            return *value_;
        }
        return fallback;
    }

  private:
    std::optional<T> value_;
    std::optional<Error> error_;
};

}  // namespace qt::foundation
