#include "qt/app/export/export_html.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <variant>

#include <md4c-html.h>

#include <qt/harness/session_repository.hpp>

namespace qt::app {

namespace {

constexpr std::string_view kSkillPrefix = "<skill name=\"";
constexpr std::string_view kLocationMarker = "\" location=\"";
constexpr std::string_view kSkillOpenEnd = "\">\n";
constexpr std::string_view kSkillClose = "\n</skill>";
constexpr std::string_view kAnsiPrefix = "\x1b[";
constexpr std::string_view kDefaultShareViewerUrl = "https://pi.dev/session/";

std::string trim_copy(std::string_view text) {
  std::size_t begin = 0U;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }

  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0) {
    --end;
  }

  return std::string(text.substr(begin, end - begin));
}

std::string lower_ascii(std::string_view text) {
  std::string lowered;
  lowered.reserve(text.size());
  for (const char ch : text) {
    lowered.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

std::string path_filename(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) {
    return std::string(path);
  }
  return std::string(path.substr(slash + 1U));
}

std::string strip_jsonl_extension(std::string filename) {
  constexpr std::string_view kJsonl = ".jsonl";
  if (filename.size() >= kJsonl.size() &&
      lower_ascii(std::string_view(filename).substr(filename.size() -
                                                    kJsonl.size())) == kJsonl) {
    filename.resize(filename.size() - kJsonl.size());
  }
  return filename;
}

std::vector<std::string_view> split_paragraphs(std::string_view markdown) {
  std::vector<std::string_view> paragraphs;
  std::size_t begin = 0U;
  while (begin < markdown.size()) {
    auto end = markdown.find("\n\n", begin);
    if (end == std::string_view::npos) {
      end = markdown.size();
    }
    const auto paragraph = markdown.substr(begin, end - begin);
    if (!trim_copy(paragraph).empty()) {
      paragraphs.push_back(paragraph);
    }
    begin = end;
    while (begin < markdown.size() && markdown[begin] == '\n') {
      ++begin;
    }
  }
  return paragraphs;
}

void append_md4c_html(const MD_CHAR* text, MD_SIZE size, void* userdata) {
  auto& html = *static_cast<std::string*>(userdata);
  html.append(text, static_cast<std::size_t>(size));
}

void trim_rendered_markdown_html(std::string& html) {
  while (!html.empty() && (html.back() == '\n' || html.back() == '\r')) {
    html.pop_back();
  }
}

bool html_tag_name_matches(std::string_view html,
                           std::size_t tag_begin,
                           std::string_view name) {
  if (tag_begin + 1U + name.size() > html.size() ||
      html.substr(tag_begin + 1U, name.size()) != name) {
    return false;
  }
  const auto after = tag_begin + 1U + name.size();
  return after == html.size() || html[after] == ' ' || html[after] == '>' ||
         html[after] == '/';
}

std::optional<std::string> html_attribute_value(std::string_view tag,
                                                std::string_view attribute) {
  for (std::size_t cursor = 0U; cursor < tag.size();) {
    const auto found = tag.find(attribute, cursor);
    if (found == std::string_view::npos) {
      return std::nullopt;
    }
    const bool boundary_before =
        found == 0U || std::isspace(static_cast<unsigned char>(tag[found - 1U])) != 0;
    const auto after_name = found + attribute.size();
    if (!boundary_before || after_name >= tag.size() || tag[after_name] != '=') {
      cursor = found + attribute.size();
      continue;
    }
    const auto quote = after_name + 1U;
    if (quote >= tag.size() || (tag[quote] != '"' && tag[quote] != '\'')) {
      return std::nullopt;
    }
    const auto value_begin = quote + 1U;
    const auto value_end = tag.find(tag[quote], value_begin);
    if (value_end == std::string_view::npos) {
      return std::nullopt;
    }
    return std::string(tag.substr(value_begin, value_end - value_begin));
  }
  return std::nullopt;
}

std::string sanitize_md4c_generated_links(std::string_view html) {
  std::string sanitized;
  sanitized.reserve(html.size());
  bool skip_next_closing_anchor = false;

  for (std::size_t cursor = 0U; cursor < html.size();) {
    const auto tag_begin = html.find('<', cursor);
    if (tag_begin == std::string_view::npos) {
      sanitized.append(html.substr(cursor));
      break;
    }
    sanitized.append(html.substr(cursor, tag_begin - cursor));
    const auto tag_end = html.find('>', tag_begin + 1U);
    if (tag_end == std::string_view::npos) {
      sanitized.append(html.substr(tag_begin));
      break;
    }
    const auto tag = html.substr(tag_begin, tag_end - tag_begin + 1U);

    if (html_tag_name_matches(html, tag_begin, "a")) {
      const auto href = html_attribute_value(tag, "href");
      if (href && is_dangerous_export_html_url(*href)) {
        skip_next_closing_anchor = true;
        cursor = tag_end + 1U;
        continue;
      }
    } else if (html_tag_name_matches(html, tag_begin, "img")) {
      const auto src = html_attribute_value(tag, "src");
      if (src && is_dangerous_export_html_url(*src)) {
        cursor = tag_end + 1U;
        continue;
      }
    } else if (tag == "</a>") {
      if (skip_next_closing_anchor) {
        skip_next_closing_anchor = false;
        cursor = tag_end + 1U;
        continue;
      }
      sanitized.append(tag);
      cursor = tag_end + 1U;
      continue;
    }

    sanitized.append(tag);
    cursor = tag_end + 1U;
  }

  return sanitized;
}

std::string render_images(const std::vector<ExportHtmlImage>& images,
                          std::string_view css_class) {
  if (images.empty()) {
    return "";
  }

  std::string html = "<div class=\"message-images\">";
  for (const auto& image : images) {
    html += "<img src=\"data:";
    html += export_html_escape(image.mime_type.empty() ? "image/png" : image.mime_type);
    html += ";base64,";
    html += export_html_escape(image.data);
    html += "\" class=\"";
    html += export_html_escape(css_class);
    html += "\" />";
  }
  html += "</div>";
  return html;
}

std::string entry_dom_id(const qt::harness::SessionUpstreamTreeFields& upstream,
                         std::size_t fallback_index) {
  if (upstream.id && !upstream.id->empty()) {
    return *upstream.id;
  }
  return "entry-" + std::to_string(fallback_index);
}

std::string user_blocks_text(
    const std::vector<qt::harness::SessionUserContentBlock>& blocks) {
  std::string text;
  for (const auto& block : blocks) {
    if (block.kind != qt::harness::SessionUserContentBlockKind::kText) {
      continue;
    }
    if (!text.empty()) {
      text += "\n\n";
    }
    text += block.text;
  }
  return text;
}

std::vector<ExportHtmlImage> user_blocks_images(
    const std::vector<qt::harness::SessionUserContentBlock>& blocks) {
  std::vector<ExportHtmlImage> images;
  for (const auto& block : blocks) {
    if (block.kind != qt::harness::SessionUserContentBlockKind::kImage) {
      continue;
    }
    images.push_back(ExportHtmlImage{block.mime_type, block.data});
  }
  return images;
}

std::string tool_result_body_html(
    const qt::harness::SessionToolResultMessageEntry& message) {
  std::string html;
  const auto images = user_blocks_images(message.content);
  html += render_images(images, "tool-image");
  const auto text = user_blocks_text(message.content);
  if (!trim_copy(text).empty()) {
    html += "<div class=\"tool-output\">";
    html += ansi_lines_to_export_html(trim_export_rendered_lines(
        split_paragraphs(text).empty() ? std::vector<std::string>{text}
                                       : std::vector<std::string>{text}));
    html += "</div>";
  }
  if (message.tool_name == "edit" && message.details &&
      message.details->is_object() && message.details->contains("diff") &&
      message.details->at("diff").is_string() &&
      !message.details->at("diff").get<std::string>().empty()) {
    html += "<details class=\"tool-diff\" open><summary>diff</summary><pre>";
    html += export_html_escape(message.details->at("diff").get<std::string>());
    html += "</pre></details>";
  }
  return html;
}

std::string assistant_message_html(
    const qt::harness::SessionAssistantMessageEntry& message,
    std::string_view id,
    const std::map<std::string,
                   const qt::harness::SessionToolResultMessageEntry*>&
        tool_results) {
  std::string html = "<section class=\"message assistant-message\" id=\"entry-" +
                     export_html_escape(id) + "\">";
  html += "<div class=\"role\">assistant</div>";
  for (const auto& block : message.content) {
    if (block.kind == qt::harness::SessionAssistantContentBlockKind::kText) {
      html += "<div class=\"markdown-content\">";
      html += render_export_markdown_fragment(block.text);
      html += "</div>";
    } else if (block.kind ==
               qt::harness::SessionAssistantContentBlockKind::kThinking) {
      html += "<details class=\"thinking\"><summary>thinking</summary><pre>";
      html += export_html_escape(block.thinking);
      html += "</pre></details>";
    } else {
      html += "<div class=\"tool-call\"><code>";
      html += export_html_escape(block.tool_name);
      html += "</code><pre>";
      html += export_html_escape(block.tool_arguments.dump(2));
      html += "</pre>";
      if (const auto result = tool_results.find(block.tool_call_id);
          result != tool_results.end() && result->second != nullptr) {
        html += tool_result_body_html(*result->second);
      }
      html += "</div>";
    }
  }
  if (message.error_message) {
    html += "<div class=\"error\">";
    html += export_html_escape(*message.error_message);
    html += "</div>";
  }
  html += "</section>";
  return html;
}

std::string bash_execution_html(
    const qt::harness::SessionBashExecutionEntry& message,
    std::string_view id) {
  std::string html = "<section class=\"message tool-execution";
  if (message.cancelled || (message.exit_code && *message.exit_code != 0)) {
    html += " error";
  }
  html += "\" id=\"entry-" + export_html_escape(id) + "\">";
  html += "<div class=\"role\">bash</div><div class=\"tool-command\">$ ";
  html += export_html_escape(message.command);
  html += "</div>";
  if (!message.output.empty()) {
    html += "<div class=\"tool-output\">";
    html += ansi_lines_to_export_html(trim_export_rendered_lines(
        std::vector<std::string>{message.output}));
    html += "</div>";
  }
  if (message.cancelled) {
    html += "<div class=\"error\">cancelled</div>";
  } else if (message.exit_code && *message.exit_code != 0) {
    html += "<div class=\"error\">exit ";
    html += std::to_string(*message.exit_code);
    html += "</div>";
  }
  html += "</section>";
  return html;
}

std::string current_export_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string join_model_names(
    const std::map<std::string, qt::harness::SessionModelStats>& models) {
  std::string joined;
  for (const auto& [model, stats] : models) {
    (void)stats;
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += model;
  }
  return joined.empty() ? std::string("unknown") : joined;
}

std::string custom_message_html(
    const qt::harness::SessionCustomMessageEntry& message,
    std::string_view id) {
  if (!message.display) {
    return "";
  }

  std::string html = "<section class=\"message custom-message\" id=\"entry-" +
                     export_html_escape(id) + "\">";
  html += "<div class=\"role\">";
  html += export_html_escape(message.custom_type.empty() ? "custom"
                                                         : message.custom_type);
  html += "</div>";
  if (const auto* text = std::get_if<std::string>(&message.content)) {
    html += render_export_markdown_fragment(*text);
  } else {
    const auto& blocks =
        std::get<std::vector<qt::harness::SessionUserContentBlock>>(
            message.content);
    html += render_images(user_blocks_images(blocks), "message-image");
    html += render_export_markdown_fragment(user_blocks_text(blocks));
  }
  html += "</section>";
  return html;
}

std::string text_from_json_content(const qt::harness::Json& content) {
  if (content.is_string()) {
    return content.get<std::string>();
  }
  if (!content.is_array()) {
    return "";
  }

  std::string text;
  for (const auto& block : content) {
    if (!block.is_object() || block.value("type", std::string{}) != "text" ||
        !block.contains("text") || !block.at("text").is_string()) {
      continue;
    }
    if (!text.empty()) {
      text += "\n\n";
    }
    text += block.at("text").get<std::string>();
  }
  return text;
}

std::vector<ExportHtmlImage> images_from_json_content(
    const qt::harness::Json& content) {
  std::vector<ExportHtmlImage> images;
  if (!content.is_array()) {
    return images;
  }
  for (const auto& block : content) {
    if (!block.is_object() || block.value("type", std::string{}) != "image" ||
        !block.contains("data") || !block.at("data").is_string()) {
      continue;
    }
    images.push_back(ExportHtmlImage{
        block.value("mimeType", std::string{"image/png"}),
        block.at("data").get<std::string>(),
    });
  }
  return images;
}

std::string raw_upstream_message_html(const qt::harness::Json& payload,
                                      std::size_t fallback_index) {
  if (!payload.is_object() || payload.value("type", std::string{}) != "message" ||
      !payload.contains("message") || !payload.at("message").is_object()) {
    return "";
  }

  const auto& message = payload.at("message");
  const std::string id =
      payload.value("id", "entry-" + std::to_string(fallback_index));
  const std::string role = message.value("role", std::string{});
  if (role == "user" && message.contains("content")) {
    return render_export_user_message_html(ExportHtmlUserMessageRequest{
        id,
        text_from_json_content(message.at("content")),
        images_from_json_content(message.at("content")),
    });
  }
  if (role == "assistant" && message.contains("content")) {
    std::string html =
        "<section class=\"message assistant-message\" id=\"entry-" +
        export_html_escape(id) + "\"><div class=\"role\">assistant</div>";
    html += "<div class=\"markdown-content\">";
    html += render_export_markdown_fragment(
        text_from_json_content(message.at("content")));
    html += "</div></section>";
    return html;
  }
  return "";
}

bool is_blank_ansi_rendered_line(std::string_view line) {
  std::string plain;
  for (std::size_t index = 0U; index < line.size();) {
    if (line.substr(index, kAnsiPrefix.size()) == kAnsiPrefix) {
      const auto end = line.find('m', index + kAnsiPrefix.size());
      if (end != std::string_view::npos) {
        index = end + 1U;
        continue;
      }
    }
    plain.push_back(line[index]);
    ++index;
  }
  return trim_copy(plain).empty();
}

std::string ansi_to_export_html(std::string_view text) {
  std::string html;
  bool span_open = false;

  const auto close_span = [&]() {
    if (span_open) {
      html += "</span>";
      span_open = false;
    }
  };

  for (std::size_t index = 0U; index < text.size();) {
    if (text.substr(index, kAnsiPrefix.size()) == kAnsiPrefix) {
      const auto end = text.find('m', index + kAnsiPrefix.size());
      if (end != std::string_view::npos) {
        const auto code = text.substr(index + kAnsiPrefix.size(),
                                      end - index - kAnsiPrefix.size());
        close_span();
        if (code == "31") {
          html += "<span style=\"color:#800000\">";
          span_open = true;
        }
        index = end + 1U;
        continue;
      }
    }

    html += export_html_escape(text.substr(index, 1U));
    ++index;
  }

  close_span();
  return html;
}

}  // namespace

std::string export_html_escape(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&#039;";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

bool is_dangerous_export_html_url(std::string_view url) {
  const auto trimmed = trim_copy(url);
  const auto colon = trimmed.find(':');
  if (colon == std::string::npos) {
    return false;
  }

  const auto scheme = lower_ascii(trimmed.substr(0U, colon));
  return scheme == "javascript" || scheme == "vbscript" || scheme == "data";
}

std::optional<ParsedExportSkillBlock> parse_export_skill_block(
    std::string_view text) {
  if (text.substr(0U, kSkillPrefix.size()) != kSkillPrefix) {
    return std::nullopt;
  }

  const auto name_begin = kSkillPrefix.size();
  const auto name_end = text.find(kLocationMarker, name_begin);
  if (name_end == std::string_view::npos) {
    return std::nullopt;
  }

  const auto location_begin = name_end + kLocationMarker.size();
  const auto location_end = text.find(kSkillOpenEnd, location_begin);
  if (location_end == std::string_view::npos) {
    return std::nullopt;
  }

  const auto content_begin = location_end + kSkillOpenEnd.size();
  const auto close_begin = text.find(kSkillClose, content_begin);
  if (close_begin == std::string_view::npos) {
    return std::nullopt;
  }

  const auto close_end = close_begin + kSkillClose.size();
  if (close_end != text.size() && text.substr(close_end, 2U) != "\n\n") {
    return std::nullopt;
  }

  ParsedExportSkillBlock block;
  block.name = std::string(text.substr(name_begin, name_end - name_begin));
  block.location =
      std::string(text.substr(location_begin, location_end - location_begin));
  block.content =
      std::string(text.substr(content_begin, close_begin - content_begin));
  if (close_end + 2U <= text.size()) {
    const auto user_message = trim_copy(text.substr(close_end + 2U));
    if (!user_message.empty()) {
      block.user_message = user_message;
    }
  }
  return block;
}

std::string render_export_markdown_fragment(std::string_view markdown) {
  if (trim_copy(markdown).empty()) {
    return "";
  }

  std::string html;
  const auto parser_flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML;
  const auto renderer_flags = MD_HTML_FLAG_SKIP_UTF8_BOM;
  const auto parsed = md_html(markdown.data(), static_cast<MD_SIZE>(markdown.size()),
                              append_md4c_html, &html, parser_flags,
                              renderer_flags);
  if (parsed != 0) {
    return "<p>" + export_html_escape(markdown) + "</p>";
  }

  trim_rendered_markdown_html(html);
  return sanitize_md4c_generated_links(html);
}

std::string ansi_lines_to_export_html(const std::vector<std::string>& lines) {
  std::string html;
  for (const auto& line : lines) {
    html += "<div class=\"ansi-line\">";
    const auto line_html = ansi_to_export_html(line);
    html += line_html.empty() ? "&nbsp;" : line_html;
    html += "</div>";
  }
  return html;
}

std::vector<std::string> trim_export_rendered_lines(
    const std::vector<std::string>& lines) {
  std::size_t begin = 0U;
  std::size_t end = lines.size();
  while (begin < end && is_blank_ansi_rendered_line(lines[begin])) {
    ++begin;
  }
  while (end > begin && is_blank_ansi_rendered_line(lines[end - 1U])) {
    --end;
  }
  return std::vector<std::string>(lines.begin() + static_cast<std::ptrdiff_t>(begin),
                                  lines.begin() + static_cast<std::ptrdiff_t>(end));
}

std::string render_export_user_message_html(
    const ExportHtmlUserMessageRequest& request) {
  const auto entry_dom_id = "entry-" + export_html_escape(request.entry_id);
  const auto copy_button =
      "<button class=\"copy-link-btn\" data-entry-id=\"" +
      export_html_escape(request.entry_id) +
      "\" title=\"Copy link to this message\"></button>";
  const auto skill_block = parse_export_skill_block(request.text);
  if (skill_block) {
    const bool has_user_content =
        skill_block->user_message.has_value() || !request.images.empty();
    std::string html = "<div class=\"skill-user-entry\" id=\"" + entry_dom_id +
                       "\">" + copy_button;
    html += "<div class=\"skill-invocation\">";
    html += "<div class=\"skill-invocation-label\">[skill] ";
    html += export_html_escape(skill_block->name);
    html += "</div><div class=\"skill-invocation-collapsed\">";
    html += export_html_escape(skill_block->name);
    html += " (click to expand)</div><div class=\"skill-invocation-content markdown-content\">";
    html += render_export_markdown_fragment(skill_block->content);
    html += "</div></div>";

    if (has_user_content) {
      html += "<div class=\"user-message\">";
      html += render_images(request.images, "message-image");
      if (skill_block->user_message) {
        html += "<div class=\"markdown-content\">";
        html += render_export_markdown_fragment(*skill_block->user_message);
        html += "</div>";
      }
      html += "</div>";
    }

    html += "</div>";
    return html;
  }

  std::string html = "<div class=\"user-message\" id=\"" + entry_dom_id + "\">" +
                     copy_button;
  html += render_images(request.images, "message-image");
  if (!trim_copy(request.text).empty()) {
    html += "<div class=\"markdown-content\">";
    html += render_export_markdown_fragment(request.text);
    html += "</div>";
  }
  html += "</div>";
  return html;
}

std::string export_html_whitespace_css() {
  return ".tool-output > div:not(.output-preview):not(.output-full),"
         ".output-preview > div:not(.expand-hint),"
         ".output-full > div:not(.expand-hint){white-space:pre-wrap;}"
         ".ansi-line{white-space:pre;}";
}

std::string sanitize_export_html_filename_stem(std::string_view stem) {
  std::string sanitized;
  sanitized.reserve(stem.size());
  bool previous_separator = false;

  for (const char value : stem) {
    const unsigned char ch = static_cast<unsigned char>(value);
    const bool allowed = std::isalnum(ch) != 0 || value == '.' || value == '_' ||
                         value == '-';
    if (allowed) {
      sanitized.push_back(value);
      previous_separator = false;
      continue;
    }

    if (!previous_separator) {
      sanitized.push_back('-');
      previous_separator = true;
    }
  }

  while (!sanitized.empty() &&
         (sanitized.front() == '.' || sanitized.front() == '-' ||
          sanitized.front() == '_')) {
    sanitized.erase(sanitized.begin());
  }
  while (!sanitized.empty() &&
         (sanitized.back() == '.' || sanitized.back() == '-' ||
          sanitized.back() == '_')) {
    sanitized.pop_back();
  }

  if (sanitized.empty()) {
    return "session";
  }
  if (sanitized == "." || sanitized == "..") {
    return "session";
  }
  return sanitized;
}

std::string default_export_html_output_path(std::string_view session_path,
                                            std::string_view app_name) {
  std::string stem = strip_jsonl_extension(path_filename(session_path));
  stem = sanitize_export_html_filename_stem(stem);
  const std::string safe_app_name =
      sanitize_export_html_filename_stem(app_name.empty() ? "qt" : app_name);
  return safe_app_name + "-session-" + stem + ".html";
}

std::string plan_export_html_output_path(
    std::optional<std::string_view> requested_path,
    std::string_view session_path,
    std::string_view app_name) {
  if (requested_path.has_value()) {
    const auto requested = trim_copy(*requested_path);
    if (!requested.empty()) {
      return requested;
    }
  }
  return default_export_html_output_path(session_path, app_name);
}

std::string share_viewer_url(
    std::string_view gist_id,
    std::optional<std::string_view> configured_base_url) {
  std::string_view base_url = kDefaultShareViewerUrl;
  if (configured_base_url && !configured_base_url->empty()) {
    base_url = *configured_base_url;
  } else if (!configured_base_url) {
    if (const char* env = std::getenv("PI_SHARE_VIEWER_URL");
        env != nullptr && env[0] != '\0') {
      base_url = env;
    }
  }

  std::string url(base_url);
  url.push_back('#');
  url.append(gist_id);
  return url;
}

std::optional<std::string> extract_gist_id_from_gh_output(
    std::string_view output) {
  const auto gist_url = trim_copy(output);
  if (gist_url.empty()) {
    return std::nullopt;
  }
  const auto slash = gist_url.find_last_of('/');
  if (slash == std::string::npos) {
    return gist_url;
  }
  if (slash + 1U >= gist_url.size()) {
    return std::nullopt;
  }
  return gist_url.substr(slash + 1U);
}

ShareSessionGistPlan plan_share_session_gist(
    std::optional<std::string_view> temp_directory) {
  const auto directory =
      temp_directory ? std::filesystem::path(std::string(*temp_directory))
                     : std::filesystem::temp_directory_path();
  const std::string temp_html_path =
      (directory / "session.html").generic_string();
  return ShareSessionGistPlan{
      temp_html_path,
      {"gh", "auth", "status"},
      {"gh", "gist", "create", "--public=false", temp_html_path}};
}

qt::foundation::Result<ShareSessionGistResult> execute_share_session_gist(
    const ShareSessionGistPlan& plan,
    const std::function<qt::foundation::Result<qt::foundation::ProcessResult>(
        const std::vector<std::string>&)>& run_argv,
    const std::function<qt::foundation::Result<std::string>(
        std::string_view)>& export_to_html) {
  const auto auth = run_argv(plan.auth_status_argv);
  if (!auth) {
    return qt::foundation::Result<ShareSessionGistResult>::failure(
        qt::foundation::ErrorCode::not_found,
        "GitHub CLI (gh) is not installed. Install it from https://cli.github.com/");
  }
  if (auth.value().exit_code != 0) {
    return qt::foundation::Result<ShareSessionGistResult>::failure(
        qt::foundation::ErrorCode::invalid_argument,
        "GitHub CLI is not logged in. Run 'gh auth login' first.");
  }

  const auto exported = export_to_html(plan.temp_html_path);
  if (!exported) {
    return qt::foundation::Result<ShareSessionGistResult>::failure(
        exported.error().code,
        "Failed to export session: " + exported.error().message,
        exported.error().system_error);
  }

  const auto gist = run_argv(plan.gist_create_argv);
  std::error_code remove_error;
  std::filesystem::remove(plan.temp_html_path, remove_error);
  if (!gist) {
    return qt::foundation::Result<ShareSessionGistResult>::failure(
        qt::foundation::ErrorCode::io_error,
        "Failed to create gist: " + gist.error().message,
        gist.error().system_error);
  }
  if (gist.value().exit_code != 0) {
    const auto error_msg = trim_copy(gist.value().stderr_text);
    return qt::foundation::Result<ShareSessionGistResult>::failure(
        qt::foundation::ErrorCode::io_error,
        "Failed to create gist: " +
            (error_msg.empty() ? std::string("Unknown error") : error_msg));
  }

  const std::string gist_url = trim_copy(gist.value().stdout_text);
  const auto gist_id = extract_gist_id_from_gh_output(gist_url);
  if (!gist_id) {
    return qt::foundation::Result<ShareSessionGistResult>::failure(
        qt::foundation::ErrorCode::encoding_error,
        "Failed to parse gist ID from gh output");
  }

  ShareSessionGistResult result;
  result.gist_url = gist_url;
  result.preview_url = share_viewer_url(*gist_id);
  result.status_text =
      "Share URL: " + result.preview_url + "\nGist: " + result.gist_url;
  return qt::foundation::Result<ShareSessionGistResult>::success(
      std::move(result));
}

std::string render_export_session_html(
    const std::vector<qt::harness::SessionEntry>& entries,
    std::string_view session_path,
    std::string_view app_name) {
  std::string title = std::string(app_name) + " session export";
  std::string session_id = "unknown";
  std::string session_cwd = "unknown";
  for (const auto& entry : entries) {
    if (const auto* metadata =
            std::get_if<qt::harness::SessionMetadataEntry>(&entry.data);
        metadata != nullptr) {
      if (!metadata->metadata.session_id.empty()) {
        session_id = metadata->metadata.session_id;
      }
      if (!metadata->metadata.working_directory.empty()) {
        session_cwd = metadata->metadata.working_directory;
      }
      if (metadata->metadata.display_name &&
          !metadata->metadata.display_name->empty()) {
        title = *metadata->metadata.display_name;
      }
      break;
    }
  }
  const auto stats = qt::harness::compute_session_stats(entries);
  std::map<std::string, const qt::harness::SessionToolResultMessageEntry*>
      tool_results;
  for (const auto& entry : entries) {
    if (const auto* result =
            std::get_if<qt::harness::SessionToolResultMessageEntry>(
                &entry.data);
        result != nullptr && !result->tool_call_id.empty()) {
      tool_results[result->tool_call_id] = result;
    }
  }

  std::string body;
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    const auto& entry = entries[index];
    body += std::visit(
        [&](const auto& value) -> std::string {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value,
                                       qt::harness::SessionUserImageMessageEntry>) {
            return render_export_user_message_html(ExportHtmlUserMessageRequest{
                entry_dom_id(value.upstream, index),
                user_blocks_text(value.content),
                user_blocks_images(value.content),
            });
          } else if constexpr (std::is_same_v<
                                   Value,
                                   qt::harness::SessionAssistantMessageEntry>) {
            return assistant_message_html(value, entry_dom_id(value.upstream, index),
                                          tool_results);
          } else if constexpr (std::is_same_v<
                                   Value,
                                   qt::harness::SessionToolResultMessageEntry>) {
            return std::string{};
          } else if constexpr (std::is_same_v<
                                   Value,
                                   qt::harness::SessionCustomMessageEntry>) {
            return custom_message_html(value, entry_dom_id(value.upstream, index));
          } else if constexpr (std::is_same_v<
                                   Value,
                                   qt::harness::SessionBashExecutionEntry>) {
            return bash_execution_html(value, entry_dom_id(value.upstream, index));
          } else if constexpr (std::is_same_v<Value,
                                             qt::harness::SessionSummaryEntry>) {
            return "<section class=\"message summary\"><div class=\"role\">summary</div>" +
                   render_export_markdown_fragment(value.summary) + "</section>";
          } else if constexpr (std::is_same_v<
                                   Value,
                                   qt::harness::SessionCompactionEntry>) {
            return "<section class=\"message summary\"><div class=\"role\">compaction</div>" +
                   render_export_markdown_fragment(value.summary) + "</section>";
          } else if constexpr (std::is_same_v<Value,
                                             qt::harness::SessionCustomEntry>) {
            return raw_upstream_message_html(value.payload, index);
          } else {
            return std::string{};
          }
        },
        entry.data);
  }

  if (body.empty()) {
    body = "<p class=\"empty\">No displayable session messages.</p>";
  }

  std::string html = "<!doctype html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  html += "<title>";
  html += export_html_escape(title);
  html += "</title><style>";
  html += ":root{color-scheme:light dark}body{font-family:system-ui,-apple-system,"
          "Segoe UI,sans-serif;margin:0;background:#111;color:#eee}"
          "main{max-width:920px;margin:0 auto;padding:32px 18px}"
          "h1{font-size:22px;margin:0 0 4px}.source{color:#aaa;margin:0 0 24px}"
          ".metadata{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));"
          "gap:10px;margin:0 0 24px}.metadata div{background:#181818;border:1px solid #333;"
          "border-radius:6px;padding:8px}.metadata dt{color:#aaa;font-size:12px}.metadata dd{"
          "margin:2px 0 0;overflow-wrap:anywhere}"
          ".message{border-top:1px solid #333;padding:16px 0}.role{color:#9ad;"
          "font-size:12px;text-transform:uppercase;letter-spacing:.08em;margin-bottom:8px}"
          ".user-message,.assistant-message,.tool-result,.custom-message{line-height:1.45}"
          ".markdown-content p{margin:0 0 10px}.markdown-content code,pre{background:#1d1d1d;"
          "border-radius:4px;padding:2px 4px}pre{overflow:auto;padding:10px}"
          ".message-image{max-width:100%;height:auto;border:1px solid #333;border-radius:6px}"
          ".skill-invocation{border-left:3px solid #9ad;padding-left:12px;margin-bottom:10px}"
          ".skill-invocation-collapsed{display:none}.error{color:#ffb4b4}"
          ".tool-output,.tool-diff pre{background:#161616;border-radius:6px;padding:10px;overflow:auto}"
          ".tool-diff summary{cursor:pointer;color:#9ad;font-size:12px;text-transform:uppercase;"
          "letter-spacing:.08em;margin:8px 0}";
  html += export_html_whitespace_css();
  html += "</style></head><body><main><h1>";
  html += export_html_escape(title);
  html += "</h1><p class=\"source\">";
  html += export_html_escape(session_path);
  html += "</p><dl class=\"metadata\"><div><dt>Session ID</dt><dd>";
  html += export_html_escape(session_id);
  html += "</dd></div><div><dt>Models</dt><dd>";
  html += export_html_escape(join_model_names(stats.models));
  html += "</dd></div><div><dt>CWD</dt><dd>";
  html += export_html_escape(session_cwd);
  html += "</dd></div><div><dt>Messages</dt><dd>";
  html += std::to_string(stats.total_messages);
  html += "</dd></div><div><dt>Exported</dt><dd>";
  html += export_html_escape(current_export_timestamp());
  html += "</dd></div></dl>";
  html += body;
  html += "</main></body></html>\n";
  return html;
}

qt::foundation::Result<std::string> export_session_file_to_html(
    std::string_view input_path,
    std::optional<std::string_view> output_path,
    std::string_view app_name) {
  const std::string input(input_path);
  const qt::harness::SessionRepository repository(input);
  auto entries = repository.load();
  if (!entries) {
    return entries.error();
  }

  const std::string output =
      plan_export_html_output_path(output_path, input_path, app_name);
  const auto html = render_export_session_html(entries.value(), input_path, app_name);

  const std::filesystem::path path(output);
  const auto parent = path.parent_path();
  std::error_code directory_error;
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, directory_error);
    if (directory_error) {
      return qt::foundation::Result<std::string>::failure(
          qt::foundation::ErrorCode::io_error, parent.generic_string(),
          directory_error.value());
    }
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::io_error, output, errno);
  }
  file.write(html.data(), static_cast<std::streamsize>(html.size()));
  if (!file) {
    return qt::foundation::Result<std::string>::failure(
        qt::foundation::ErrorCode::io_error, output, errno);
  }
  return qt::foundation::Result<std::string>::success(output);
}

}  // namespace qt::app
