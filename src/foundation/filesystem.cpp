#include "qt/foundation/filesystem.hpp"

#include <array>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <system_error>

namespace qt::foundation {

namespace {

Error make_filesystem_error(ErrorCode code, const Path& path, const std::error_code& error) {
    std::ostringstream stream;
    stream << path_for_display(path);
    if (error) {
        stream << ": " << error.message();
    }
    return Error{code, stream.str(), error.value()};
}

}  // namespace

Status ensure_directory(const Path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return Status(make_filesystem_error(ErrorCode::io_error, path, error));
    }

    if (exists) {
        if (!std::filesystem::is_directory(path, error)) {
            if (error) {
                return Status(make_filesystem_error(ErrorCode::io_error, path, error));
            }
            return Status::failure(ErrorCode::invalid_argument,
                                   path_for_display(path) + " is not a directory");
        }
        return Status::success();
    }

    std::filesystem::create_directories(path, error);
    if (error) {
        return Status(make_filesystem_error(ErrorCode::io_error, path, error));
    }
    return Status::success();
}

Result<Path> canonicalize_path(const Path& path) {
    std::error_code error;
    const Path weakly_canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return make_filesystem_error(ErrorCode::io_error, path, error);
    }
    return weakly_canonical;
}

Result<std::string> read_text_file(const Path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            return Error{ErrorCode::io_error, path_for_display(path), error.value()};
        }
        const ErrorCode code = exists ? ErrorCode::io_error : ErrorCode::not_found;
        return Error{code, path_for_display(path), errno};
    }

    std::ostringstream stream;
    std::array<char, 8192> buffer;
    while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           input.gcount() > 0) {
        stream.write(buffer.data(), input.gcount());
    }
    if (input.bad() || !input.eof()) {
        return Error{ErrorCode::io_error, path_for_display(path), errno};
    }
    return stream.str();
}

Result<std::vector<std::string>> read_lines(const Path& path) {
    auto content = read_text_file(path);
    if (!content) {
        return content.error();
    }

    std::vector<std::string> lines;
    std::istringstream stream(content.value());
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

Status write_text_file(const Path& path, std::string_view content) {
    const Path parent = path.parent_path();
    if (!parent.empty()) {
        const Status directory_status = ensure_directory(parent);
        if (!directory_status) {
            return directory_status;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Status::failure(ErrorCode::io_error, path_for_display(path), errno);
    }

    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        return Status::failure(ErrorCode::io_error, path_for_display(path), errno);
    }

    return Status::success();
}

std::string path_for_display(const Path& path) { return path.generic_string(); }

}  // namespace qt::foundation
