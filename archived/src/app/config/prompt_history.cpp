#include "qt/app/config/prompt_history.hpp"

#include "qt/foundation/filesystem.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <deque>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace qt::app {

namespace {

using Json = nlohmann::json;
using qt::foundation::Error;
using qt::foundation::ErrorCode;
using qt::foundation::Path;
using qt::foundation::Result;
using qt::foundation::Status;

struct ParsedHistory {
  std::deque<std::string> oldest_first;
  bool file_exists = false;
  bool needs_rewrite = false;
  std::size_t valid_entries = 0;
  std::size_t retained_bytes = 0;
};

std::string trim_copy(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

Status filesystem_status(ErrorCode code, const Path& path,
                         const std::error_code& error) {
  return Status::failure(code, qt::foundation::path_for_display(path) +
                                   ": " + error.message(),
                         error.value());
}

class ExclusiveHistoryLock final {
 public:
  ExclusiveHistoryLock() = default;
  ExclusiveHistoryLock(const ExclusiveHistoryLock&) = delete;
  ExclusiveHistoryLock& operator=(const ExclusiveHistoryLock&) = delete;

  ExclusiveHistoryLock(ExclusiveHistoryLock&& other) noexcept
#if !defined(_WIN32)
      : descriptor_(std::exchange(other.descriptor_, -1))
#endif
  {
#if defined(_WIN32)
    (void)other;
#endif
  }

  ExclusiveHistoryLock& operator=(ExclusiveHistoryLock&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    release();
#if !defined(_WIN32)
    descriptor_ = std::exchange(other.descriptor_, -1);
#endif
    return *this;
  }

  ~ExclusiveHistoryLock() { release(); }

  static Result<ExclusiveHistoryLock> acquire(const Path& history_path) {
    const auto directory_status =
        qt::foundation::ensure_directory(history_path.parent_path());
    if (!directory_status) {
      return directory_status.error();
    }

    ExclusiveHistoryLock lock;
#if !defined(_WIN32)
    const Path lock_path = history_path.string() + ".lock";
    lock.descriptor_ =
        ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (lock.descriptor_ < 0) {
      return Error{ErrorCode::io_error,
                   "unable to open prompt history lock " +
                       qt::foundation::path_for_display(lock_path),
                   errno};
    }
    if (::fchmod(lock.descriptor_, S_IRUSR | S_IWUSR) != 0) {
      const int system_error = errno;
      lock.release();
      return Error{ErrorCode::io_error,
                   "unable to secure prompt history lock " +
                       qt::foundation::path_for_display(lock_path),
                   system_error};
    }
    if (::flock(lock.descriptor_, LOCK_EX) != 0) {
      const int system_error = errno;
      lock.release();
      return Error{ErrorCode::io_error,
                   "unable to lock prompt history " +
                       qt::foundation::path_for_display(history_path),
                   system_error};
    }
#else
    (void)history_path;
#endif
    return lock;
  }

 private:
  void release() noexcept {
#if !defined(_WIN32)
    if (descriptor_ >= 0) {
      (void)::flock(descriptor_, LOCK_UN);
      (void)::close(descriptor_);
      descriptor_ = -1;
    }
#endif
  }

#if !defined(_WIN32)
  int descriptor_ = -1;
#endif
};

Result<ParsedHistory> read_history_unlocked(const Path& path,
                                            std::size_t max_entries) {
  std::error_code exists_error;
  const bool exists = std::filesystem::exists(path, exists_error);
  if (exists_error) {
    return Error{ErrorCode::io_error,
                 qt::foundation::path_for_display(path) + ": " +
                     exists_error.message(),
                 exists_error.value()};
  }
  if (!exists) {
    return ParsedHistory{};
  }

  const auto content = qt::foundation::read_text_file(path);
  if (!content) {
    return content.error();
  }

  ParsedHistory parsed;
  parsed.file_exists = true;
  std::size_t line_number = 0;
  std::size_t offset = 0;
  while (offset < content.value().size()) {
    const std::size_t end = content.value().find('\n', offset);
    const std::size_t length =
        end == std::string::npos ? content.value().size() - offset
                                 : end - offset;
    std::string_view line(content.value().data() + offset, length);
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!trim_copy(line).empty()) {
      Json record;
      try {
        record = Json::parse(line);
      } catch (const Json::exception& error) {
        return Result<ParsedHistory>::failure(
            ErrorCode::invalid_argument,
            "invalid prompt history JSON at " +
                qt::foundation::path_for_display(path) + ":" +
                std::to_string(line_number) + ": " + error.what());
      }
      if (!record.is_object() || !record.contains("prompt") ||
          !record.at("prompt").is_string()) {
        return Result<ParsedHistory>::failure(
            ErrorCode::invalid_argument,
            "invalid prompt history record at " +
                qt::foundation::path_for_display(path) + ":" +
                std::to_string(line_number));
      }
      const std::string raw_prompt = record.at("prompt").get<std::string>();
      std::string prompt = trim_copy(raw_prompt);
      if (prompt.empty()) {
        parsed.needs_rewrite = true;
      } else {
        if (prompt.size() > kMaximumPromptHistoryBytes) {
          return Result<ParsedHistory>::failure(
              ErrorCode::invalid_argument,
              "prompt history entry exceeds the retained byte limit at " +
                  qt::foundation::path_for_display(path) + ":" +
                  std::to_string(line_number));
        }
        parsed.needs_rewrite = parsed.needs_rewrite || prompt != raw_prompt;
        ++parsed.valid_entries;
        if (max_entries > 0U) {
          parsed.oldest_first.push_back(std::move(prompt));
          parsed.retained_bytes += parsed.oldest_first.back().size();
          if (parsed.oldest_first.size() > max_entries) {
            parsed.retained_bytes -= parsed.oldest_first.front().size();
            parsed.oldest_first.pop_front();
          }
          while (parsed.retained_bytes > kMaximumPromptHistoryBytes &&
                 !parsed.oldest_first.empty()) {
            parsed.retained_bytes -= parsed.oldest_first.front().size();
            parsed.oldest_first.pop_front();
            parsed.needs_rewrite = true;
          }
        }
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1U;
  }
  parsed.needs_rewrite =
      parsed.needs_rewrite || parsed.valid_entries > max_entries;
  return parsed;
}

Status secure_file(const Path& path) {
#if !defined(_WIN32)
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    return Status::failure(ErrorCode::io_error,
                           "unable to secure prompt history " +
                               qt::foundation::path_for_display(path),
                           errno);
  }
#else
  (void)path;
#endif
  return Status::success();
}

Path temporary_path_for(const Path& path) {
  static std::atomic_uint64_t sequence{0U};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return path.string() + ".tmp." + std::to_string(tick) + "." +
         std::to_string(sequence.fetch_add(1U));
}

Status write_history_unlocked(const Path& path,
                              const std::deque<std::string>& oldest_first) {
  std::string content;
  for (const auto& prompt : oldest_first) {
    content += Json{{"prompt", prompt}}.dump();
    content.push_back('\n');
  }

  const Path temporary = temporary_path_for(path);
#if !defined(_WIN32)
  const int descriptor = ::open(temporary.c_str(),
                                O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
                                S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    return Status::failure(ErrorCode::io_error,
                           "unable to create prompt history temporary file " +
                               qt::foundation::path_for_display(temporary),
                           errno);
  }
  std::size_t written = 0;
  while (written < content.size()) {
    const auto count = ::write(descriptor, content.data() + written,
                               content.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      const int system_error = errno;
      (void)::close(descriptor);
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return Status::failure(ErrorCode::io_error,
                             "unable to write prompt history temporary file " +
                                 qt::foundation::path_for_display(temporary),
                             system_error);
    }
    written += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0) {
    const int system_error = errno;
    (void)::close(descriptor);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Status::failure(ErrorCode::io_error,
                           "unable to sync prompt history temporary file " +
                               qt::foundation::path_for_display(temporary),
                           system_error);
  }
  if (::close(descriptor) != 0) {
    const int system_error = errno;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return Status::failure(ErrorCode::io_error,
                           "unable to close prompt history temporary file " +
                               qt::foundation::path_for_display(temporary),
                           system_error);
  }
#else
  const auto write_status = qt::foundation::write_text_file(temporary, content);
  if (!write_status) {
    return write_status;
  }
#endif
  const auto secure_status = secure_file(temporary);
  if (!secure_status) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return secure_status;
  }

  std::error_code rename_error;
#if defined(_WIN32)
  std::filesystem::remove(path, rename_error);
  rename_error.clear();
#endif
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return filesystem_status(ErrorCode::io_error, path, rename_error);
  }
  return secure_file(path);
}

Status remove_history_unlocked(const Path& path) {
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
  if (remove_error) {
    return filesystem_status(ErrorCode::io_error, path, remove_error);
  }
  return Status::success();
}

Result<std::deque<std::string>> normalize_newest_first(
    const std::vector<std::string>& newest_first, std::size_t max_entries) {
  std::vector<std::string> normalized;
  normalized.reserve(std::min(newest_first.size(), max_entries));
  std::size_t retained_bytes = 0U;
  for (const auto& candidate : newest_first) {
    std::string prompt = trim_copy(candidate);
    if (prompt.empty()) {
      continue;
    }
    if (!normalized.empty() && normalized.back() == prompt) {
      continue;
    }
    if (prompt.size() > kMaximumPromptHistoryBytes) {
      return Error{ErrorCode::invalid_argument,
                   "prompt history entry exceeds the retained byte limit"};
    }
    if (retained_bytes > kMaximumPromptHistoryBytes - prompt.size()) {
      continue;
    }
    normalized.push_back(std::move(prompt));
    retained_bytes += normalized.back().size();
    if (normalized.size() >= max_entries) {
      break;
    }
  }
  return std::deque<std::string>(normalized.rbegin(), normalized.rend());
}

}  // namespace

PromptHistoryStore::PromptHistoryStore(Path agent_home,
                                       std::size_t max_entries)
    : path_(std::move(agent_home) / std::string(kPromptHistoryFilename)),
      max_entries_(max_entries) {}

const Path& PromptHistoryStore::path() const noexcept { return path_; }

std::size_t PromptHistoryStore::max_entries() const noexcept {
  return max_entries_;
}

Result<PromptHistorySnapshot> PromptHistoryStore::load() const {
  const auto lock = ExclusiveHistoryLock::acquire(path_);
  if (!lock) {
    return lock.error();
  }

  if (max_entries_ == 0U) {
    std::error_code exists_error;
    const bool existed = std::filesystem::exists(path_, exists_error);
    if (exists_error) {
      return Error{ErrorCode::io_error,
                   qt::foundation::path_for_display(path_) + ": " +
                       exists_error.message(),
                   exists_error.value()};
    }
    const auto removed = remove_history_unlocked(path_);
    if (!removed) {
      return removed.error();
    }
    return PromptHistorySnapshot{{}, existed};
  }

  const auto parsed = read_history_unlocked(path_, max_entries_);
  if (!parsed) {
    return parsed.error();
  }
  if (parsed.value().needs_rewrite) {
    const auto rewritten =
        write_history_unlocked(path_, parsed.value().oldest_first);
    if (!rewritten) {
      return rewritten.error();
    }
  } else if (parsed.value().file_exists) {
    const auto secured = secure_file(path_);
    if (!secured) {
      return secured.error();
    }
  }

  PromptHistorySnapshot snapshot;
  snapshot.file_exists = parsed.value().file_exists;
  snapshot.prompts.assign(parsed.value().oldest_first.rbegin(),
                          parsed.value().oldest_first.rend());
  return snapshot;
}

Status PromptHistoryStore::append(std::string_view prompt) const {
  const auto lock = ExclusiveHistoryLock::acquire(path_);
  if (!lock) {
    return Status(lock.error());
  }
  if (max_entries_ == 0U) {
    return remove_history_unlocked(path_);
  }

  const auto parsed = read_history_unlocked(path_, max_entries_);
  if (!parsed) {
    return Status(parsed.error());
  }
  auto entries = parsed.value().oldest_first;
  std::string normalized = trim_copy(prompt);
  if (normalized.size() > kMaximumPromptHistoryBytes) {
    return Status::failure(ErrorCode::invalid_argument,
                           "prompt history entry exceeds the retained byte limit");
  }
  if (!normalized.empty() &&
      (entries.empty() || entries.back() != normalized)) {
    entries.push_back(std::move(normalized));
  }
  while (entries.size() > max_entries_) {
    entries.pop_front();
  }
  std::size_t retained_bytes = 0U;
  for (const auto& entry : entries) retained_bytes += entry.size();
  while (retained_bytes > kMaximumPromptHistoryBytes && !entries.empty()) {
    retained_bytes -= entries.front().size();
    entries.pop_front();
  }
  if (!parsed.value().file_exists && entries.empty()) {
    return Status::success();
  }
  return write_history_unlocked(path_, entries);
}

Status PromptHistoryStore::save(
    const std::vector<std::string>& newest_first) const {
  const auto lock = ExclusiveHistoryLock::acquire(path_);
  if (!lock) {
    return Status(lock.error());
  }
  if (max_entries_ == 0U) {
    return remove_history_unlocked(path_);
  }
  const auto normalized = normalize_newest_first(newest_first, max_entries_);
  if (!normalized) return Status(normalized.error());
  return write_history_unlocked(path_, normalized.value());
}

PromptHistoryWriter::PromptHistoryWriter(PromptHistoryStore store,
                                         std::size_t max_pending,
                                         std::size_t max_pending_bytes)
    : store_(std::move(store)), max_pending_(std::max<std::size_t>(1U, max_pending)),
      max_pending_bytes_(std::max<std::size_t>(1U, max_pending_bytes)),
      worker_([this]() { run(); }) {}

PromptHistoryWriter::~PromptHistoryWriter() { stop(); }

Status PromptHistoryWriter::append(
    std::string prompt, std::size_t max_entries,
    std::function<Status()> after, std::size_t retained_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) {
    return Status::failure(ErrorCode::internal_error,
                           "prompt history writer is stopping");
  }
  const std::size_t operation_bytes = prompt.size() + retained_bytes;
  if (operation_bytes < prompt.size() || operations_.size() >= max_pending_ ||
      operation_bytes > max_pending_bytes_ ||
      pending_bytes_ > max_pending_bytes_ - operation_bytes) {
    return Status::failure(ErrorCode::internal_error,
                           "prompt history persistence queue is full");
  }
  pending_bytes_ += operation_bytes;
  operations_.push_back(
      Operation{OperationKind::append, std::move(prompt), {}, max_entries, std::move(after),
                retained_bytes});
  latest_accepted_.insert(latest_accepted_.begin(), operations_.back().prompt);
  if (latest_accepted_.size() > max_entries) latest_accepted_.resize(max_entries);
  ready_.notify_one();
  return Status::success();
}

Status PromptHistoryWriter::clear(
    std::size_t max_entries, std::function<Status()> after) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) {
    return Status::failure(ErrorCode::internal_error,
                           "prompt history writer is stopping");
  }
  if (operations_.size() >= max_pending_) {
    return Status::failure(ErrorCode::internal_error,
                           "prompt history persistence queue is full");
  }
  operations_.push_back(
      Operation{OperationKind::clear, {}, {}, max_entries, std::move(after), 0U});
  latest_accepted_.clear();
  ready_.notify_one();
  return Status::success();
}

Status PromptHistoryWriter::save(std::vector<std::string> newest_first,
                                 std::size_t max_entries) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || operations_.size() >= max_pending_) {
    return Status::failure(ErrorCode::internal_error,
                           "prompt history persistence queue is full");
  }
  std::size_t bytes = 0U;
  for (const auto& prompt : newest_first) bytes += prompt.size();
  if (bytes > max_pending_bytes_ || pending_bytes_ > max_pending_bytes_ - bytes) {
    return Status::failure(ErrorCode::internal_error,
                           "prompt history persistence queue is full");
  }
  pending_bytes_ += bytes;
  latest_accepted_ = newest_first;
  if (latest_accepted_.size() > max_entries) latest_accepted_.resize(max_entries);
  operations_.push_back(Operation{OperationKind::save, {}, std::move(newest_first),
                                  max_entries, {}, bytes});
  ready_.notify_one();
  return Status::success();
}

std::vector<std::string> PromptHistoryWriter::latest_accepted_history() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_accepted_;
}

std::vector<Error> PromptHistoryWriter::take_errors() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto errors = std::move(errors_);
  errors_.clear();
  return errors;
}

Status PromptHistoryWriter::flush() {
  std::unique_lock<std::mutex> lock(mutex_);
  drained_.wait(lock, [this]() { return operations_.empty() && !writing_; });
  if (errors_.empty()) {
    return Status::success();
  }
  return Status(errors_.back());
}

void PromptHistoryWriter::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  ready_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void PromptHistoryWriter::run() {
  for (;;) {
    Operation operation;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this]() { return stopping_ || !operations_.empty(); });
      if (operations_.empty()) {
        drained_.notify_all();
        if (stopping_) return;
        continue;
      }
      operation = std::move(operations_.front());
      pending_bytes_ -= operation.prompt.size() + operation.retained_bytes;
      operations_.pop_front();
      writing_ = true;
    }
    const PromptHistoryStore store(store_.path().parent_path(), operation.max_entries);
    const Status history_status = operation.kind == OperationKind::append
        ? store.append(operation.prompt)
        : operation.kind == OperationKind::clear ? store.save({})
                                                : store.save(operation.history);
    const Status after_status = operation.after ? operation.after() : Status::success();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      writing_ = false;
      const auto retain_error = [this](const Error& error) {
        constexpr std::size_t kMaximumReportedErrors = 64U;
        if (errors_.size() == kMaximumReportedErrors) errors_.erase(errors_.begin());
        errors_.push_back(error);
      };
      if (!history_status) retain_error(history_status.error());
      if (!after_status) retain_error(after_status.error());
      if (operations_.empty()) drained_.notify_all();
    }
  }
}

}  // namespace qt::app
