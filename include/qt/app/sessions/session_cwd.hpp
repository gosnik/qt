#pragma once

#include <optional>
#include <string>

#include "qt/app/cli/startup.hpp"

namespace qt::app {

struct ReferenceMissingSessionCwdIssue {
  std::optional<std::string> session_file;
  std::string session_cwd;
  std::string fallback_cwd;
};

std::optional<ReferenceMissingSessionCwdIssue>
missing_session_cwd_issue(
    const ReferenceStartupSession& startup_session,
    const std::string& fallback_cwd);

std::optional<ReferenceMissingSessionCwdIssue>
missing_session_cwd_issue_from_file(
    const std::string& session_file,
    const std::string& fallback_cwd);

std::string format_missing_session_cwd_error(
    const ReferenceMissingSessionCwdIssue& issue);

std::string format_missing_session_cwd_prompt(
    const ReferenceMissingSessionCwdIssue& issue);

bool missing_session_cwd_choice_is_continue(std::string choice);

}  // namespace qt::app
