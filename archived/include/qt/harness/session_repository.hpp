#pragma once

#include <string>
#include <vector>

#include <qt/foundation/error.hpp>
#include <qt/harness/session.hpp>

namespace qt::harness {

class SessionRepository {
 public:
  explicit SessionRepository(std::string session_path);

  const std::string& session_path() const noexcept { return session_path_; }

  qt::foundation::Status append(const SessionEntry& entry) const;
  qt::foundation::Status replace_all(
      const std::vector<SessionEntry>& entries) const;
  qt::foundation::Result<std::vector<SessionEntry>> load() const;

 private:
  std::string session_path_;
};

}  // namespace qt::harness
