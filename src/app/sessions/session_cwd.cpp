#include "qt/app/sessions/session_cwd.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

namespace qt::app {

using Json = nlohmann::json;

std::optional<ReferenceMissingSessionCwdIssue>
missing_session_cwd_issue(
    const ReferenceStartupSession& startup_session,
    const std::string& fallback_cwd) {
  if (!startup_session.repository_path || !startup_session.working_directory ||
      startup_session.working_directory->empty()) {
    return std::nullopt;
  }

  std::error_code exists_error;
  if (std::filesystem::exists(*startup_session.working_directory, exists_error)) {
    return std::nullopt;
  }

  ReferenceMissingSessionCwdIssue issue;
  issue.session_file = startup_session.repository_path;
  issue.session_cwd = *startup_session.working_directory;
  issue.fallback_cwd = fallback_cwd;
  return issue;
}

std::optional<ReferenceMissingSessionCwdIssue>
missing_session_cwd_issue_from_file(
    const std::string& session_file, const std::string& fallback_cwd) {
  std::ifstream input(session_file);
  if (!input) {
    return std::nullopt;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
      continue;
    }

    try {
      const auto header = Json::parse(line);
      if (header.value("type", std::string{}) != "session" ||
          !header.contains("cwd") || !header.at("cwd").is_string()) {
        return std::nullopt;
      }
      const std::string session_cwd = header.at("cwd").get<std::string>();
      if (session_cwd.empty()) {
        return std::nullopt;
      }
      std::error_code exists_error;
      if (std::filesystem::exists(session_cwd, exists_error)) {
        return std::nullopt;
      }
      return ReferenceMissingSessionCwdIssue{session_file, session_cwd,
                                             fallback_cwd};
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::string format_missing_session_cwd_error(
    const ReferenceMissingSessionCwdIssue& issue) {
  std::string message =
      "Stored session working directory does not exist: " + issue.session_cwd;
  if (issue.session_file && !issue.session_file->empty()) {
    message += "\nSession file: " + *issue.session_file;
  }
  message += "\nCurrent working directory: " + issue.fallback_cwd;
  return message;
}

std::string format_missing_session_cwd_prompt(
    const ReferenceMissingSessionCwdIssue& issue) {
  return "cwd from session file does not exist\n" + issue.session_cwd +
         "\n\ncontinue in current cwd\n" + issue.fallback_cwd;
}

bool missing_session_cwd_choice_is_continue(std::string choice) {
  choice.erase(choice.begin(),
               std::find_if(choice.begin(), choice.end(), [](char ch) {
                 return std::isspace(static_cast<unsigned char>(ch)) == 0;
               }));
  choice.erase(std::find_if(choice.rbegin(), choice.rend(), [](char ch) {
                 return std::isspace(static_cast<unsigned char>(ch)) == 0;
               }).base(),
               choice.end());
  std::transform(choice.begin(), choice.end(), choice.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return choice == "continue" || choice == "c" || choice == "yes" ||
         choice == "y";
}

}  // namespace qt::app
