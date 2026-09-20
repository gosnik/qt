#include <qt/harness/session_repository.hpp>

#include <cerrno>
#include <fstream>
#include <sstream>

#include <qt/foundation/filesystem.hpp>

namespace qt::harness {
namespace {

using qt::foundation::Error;
using qt::foundation::ErrorCode;
using qt::foundation::Path;
using qt::foundation::Result;
using qt::foundation::Status;

std::string make_line_error_message(const std::string& path, std::size_t line_number,
                                    const std::string& error) {
  std::ostringstream stream;
  stream << path << ":" << line_number << ": " << error;
  return stream.str();
}

}  // namespace

SessionRepository::SessionRepository(std::string session_path)
    : session_path_(std::move(session_path)) {}

Status SessionRepository::append(const SessionEntry& entry) const {
  const Path path(session_path_);
  const Path parent = path.parent_path();
  if (!parent.empty()) {
    const Status directory_status = qt::foundation::ensure_directory(parent);
    if (!directory_status) {
      return directory_status;
    }
  }

  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    return Status::failure(ErrorCode::io_error,
                           qt::foundation::path_for_display(path), errno);
  }

  const std::string encoded = encode_session_entry_jsonl(entry);
  output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
  output.close();
  if (!output) {
    return Status::failure(ErrorCode::io_error,
                           qt::foundation::path_for_display(path), errno);
  }

  return Status::success();
}

Status SessionRepository::replace_all(
    const std::vector<SessionEntry>& entries) const {
  const Path path(session_path_);
  const Path parent = path.parent_path();
  if (!parent.empty()) {
    const Status directory_status = qt::foundation::ensure_directory(parent);
    if (!directory_status) {
      return directory_status;
    }
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return Status::failure(ErrorCode::io_error,
                           qt::foundation::path_for_display(path), errno);
  }

  for (const auto& entry : entries) {
    const std::string encoded = encode_session_entry_jsonl(entry);
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!output) {
      return Status::failure(ErrorCode::io_error,
                             qt::foundation::path_for_display(path), errno);
    }
  }

  output.close();
  if (!output) {
    return Status::failure(ErrorCode::io_error,
                           qt::foundation::path_for_display(path), errno);
  }

  return Status::success();
}

Result<std::vector<SessionEntry>> SessionRepository::load() const {
  const Path path(session_path_);
  const auto lines = qt::foundation::read_lines(path);
  if (!lines) {
    if (lines.error().code == ErrorCode::not_found) {
      return std::vector<SessionEntry>{};
    }
    return lines.error();
  }

  std::vector<SessionEntry> entries;
  entries.reserve(lines.value().size());
  for (std::size_t index = 0; index < lines.value().size(); ++index) {
    const auto decoded = decode_session_entry_jsonl(lines.value()[index]);
    if (!decoded) {
      return Error{ErrorCode::encoding_error,
                   make_line_error_message(qt::foundation::path_for_display(path),
                                           index + 1, decoded.error)};
    }
    entries.push_back(*decoded.entry);
  }

  return entries;
}

}  // namespace qt::harness
