#include "qt/foundation/error.hpp"

#include <sstream>

namespace qt::foundation {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::none:
            return "none";
        case ErrorCode::invalid_argument:
            return "invalid_argument";
        case ErrorCode::not_found:
            return "not_found";
        case ErrorCode::io_error:
            return "io_error";
        case ErrorCode::encoding_error:
            return "encoding_error";
        case ErrorCode::internal_error:
            return "internal_error";
    }

    return "unknown";
}

Error::Error(ErrorCode error_code, std::string text, int system_error_code)
    : code(error_code), message(std::move(text)), system_error(system_error_code) {}

bool Error::ok() const noexcept { return code == ErrorCode::none; }

Error::operator bool() const noexcept { return !ok(); }

std::string Error::summary() const {
    std::ostringstream stream;
    stream << to_string(code);
    if (!message.empty()) {
        stream << ": " << message;
    }
    if (system_error != 0) {
        stream << " (" << system_error << ")";
    }
    return stream.str();
}

Status::Status(Error error) : error_(std::move(error)) {}

Status Status::success() { return Status(); }

Status Status::failure(ErrorCode code, std::string message, int system_error) {
    return Status(Error{code, std::move(message), system_error});
}

bool Status::ok() const noexcept { return !error_.has_value() || error_->ok(); }

Status::operator bool() const noexcept { return ok(); }

const Error& Status::error() const {
    if (!error_.has_value()) {
        throw std::logic_error("status has no error");
    }
    return *error_;
}

}  // namespace qt::foundation
