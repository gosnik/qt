#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <qt/foundation/error.hpp>
#include <qt/foundation/process.hpp>
#include <qt/harness/session.hpp>

namespace qt::app {

struct ExportHtmlImage {
  std::string mime_type = "image/png";
  std::string data;
};

struct ParsedExportSkillBlock {
  std::string name;
  std::string location;
  std::string content;
  std::optional<std::string> user_message;
};

struct ExportHtmlUserMessageRequest {
  std::string entry_id;
  std::string text;
  std::vector<ExportHtmlImage> images;
};

struct ShareSessionGistPlan {
  std::string temp_html_path;
  std::vector<std::string> auth_status_argv;
  std::vector<std::string> gist_create_argv;
};

struct ShareSessionGistResult {
  std::string gist_url;
  std::string preview_url;
  std::string status_text;
};

[[nodiscard]] std::string export_html_escape(std::string_view text);

[[nodiscard]] bool is_dangerous_export_html_url(std::string_view url);

[[nodiscard]] std::optional<ParsedExportSkillBlock> parse_export_skill_block(
    std::string_view text);

[[nodiscard]] std::string render_export_markdown_fragment(std::string_view markdown);

[[nodiscard]] std::string ansi_lines_to_export_html(
    const std::vector<std::string>& lines);

[[nodiscard]] std::vector<std::string> trim_export_rendered_lines(
    const std::vector<std::string>& lines);

[[nodiscard]] std::string render_export_user_message_html(
    const ExportHtmlUserMessageRequest& request);

[[nodiscard]] std::string export_html_whitespace_css();

[[nodiscard]] std::string sanitize_export_html_filename_stem(
    std::string_view stem);

[[nodiscard]] std::string default_export_html_output_path(
    std::string_view session_path,
    std::string_view app_name = "qt");

[[nodiscard]] std::string plan_export_html_output_path(
    std::optional<std::string_view> requested_path,
    std::string_view session_path,
    std::string_view app_name = "qt");

[[nodiscard]] std::string share_viewer_url(
    std::string_view gist_id,
    std::optional<std::string_view> configured_base_url = std::nullopt);

[[nodiscard]] std::optional<std::string> extract_gist_id_from_gh_output(
    std::string_view output);

[[nodiscard]] ShareSessionGistPlan plan_share_session_gist(
    std::optional<std::string_view> temp_directory = std::nullopt);

[[nodiscard]] qt::foundation::Result<ShareSessionGistResult>
execute_share_session_gist(
    const ShareSessionGistPlan& plan,
    const std::function<qt::foundation::Result<qt::foundation::ProcessResult>(
        const std::vector<std::string>&)>& run_argv,
    const std::function<qt::foundation::Result<std::string>(
        std::string_view)>& export_to_html);

[[nodiscard]] std::string render_export_session_html(
    const std::vector<qt::harness::SessionEntry>& entries,
    std::string_view session_path,
    std::string_view app_name = "qt");

[[nodiscard]] qt::foundation::Result<std::string> export_session_file_to_html(
    std::string_view input_path,
    std::optional<std::string_view> output_path = std::nullopt,
    std::string_view app_name = "qt");

}  // namespace qt::app
