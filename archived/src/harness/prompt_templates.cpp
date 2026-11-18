#include <qt/harness/prompt_templates.hpp>

#include <qt/foundation/error.hpp>
#include <qt/harness/repository.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string_view>
#include <system_error>

namespace qt::harness {
namespace {

using qt::foundation::ErrorCode;
using qt::foundation::Result;
using Path = std::filesystem::path;

struct ParsedPromptTemplateDocument {
  std::string description;
  std::optional<std::string> argument_hint;
  std::string content;
};

std::string path_to_string(const Path& path) { return path.generic_string(); }

bool path_exists(const Path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

bool path_is_directory(const Path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error) && !error;
}

bool path_is_regular_file(const Path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool has_markdown_extension(const Path& path) {
  return path.extension().generic_string() == ".md";
}

Path normalize_prompt_root_path(const std::string& root) {
  if (root == "~" || root.rfind("~/", 0U) == 0U ||
      (root.size() > 1U && root.front() == '~' && root[1U] != '/')) {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
      if (root == "~") {
        return Path(home);
      }
      const std::string suffix = root.rfind("~/", 0U) == 0U ? root.substr(2U)
                                                            : root.substr(1U);
      return Path(home) / Path(suffix);
    }
  }
  return Path(root);
}

std::string normalize_newlines(std::string text) {
  std::string normalized;
  normalized.reserve(text.size());

  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\r') {
      normalized.push_back('\n');
      if (index + 1 < text.size() && text[index + 1] == '\n') {
        ++index;
      }
      continue;
    }

    normalized.push_back(text[index]);
  }

  return normalized;
}

std::string trim_copy(const std::string& value) {
  std::size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }

  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return value.substr(start, end - start);
}

std::string maybe_unquote(std::string value) {
  if (value.size() < 2) {
    return value;
  }

  const char first = value.front();
  const char last = value.back();
  if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
    return value.substr(1, value.size() - 2);
  }

  return value;
}

std::string join_arguments(const std::vector<std::string>& arguments,
                           std::size_t start,
                           std::size_t length) {
  if (start >= arguments.size() || length == 0) {
    return "";
  }

  const std::size_t end = start + std::min(length, arguments.size() - start);
  std::string joined;
  for (std::size_t index = start; index < end; ++index) {
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += arguments[index];
  }
  return joined;
}

std::string join_arguments_from(const std::vector<std::string>& arguments,
                                std::size_t start) {
  if (start >= arguments.size()) {
    return "";
  }
  return join_arguments(arguments, start, arguments.size() - start);
}

bool is_digit(char value) {
  return std::isdigit(static_cast<unsigned char>(value)) != 0;
}

std::size_t parse_decimal(std::string_view text) {
  std::size_t value = 0;
  for (const char ch : text) {
    const auto digit = static_cast<std::size_t>(ch - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
      return std::numeric_limits<std::size_t>::max();
    }
    value = (value * 10U) + digit;
  }
  return value;
}

std::string derive_description(const std::string& content) {
  std::size_t start = 0;
  while (start < content.size()) {
    const std::size_t end = content.find('\n', start);
    const std::string line = trim_copy(
        content.substr(start, end == std::string::npos ? end : end - start));
    if (!line.empty()) {
      if (line.size() <= 60) {
        return line;
      }
      return line.substr(0, 60) + "...";
    }

    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  return "";
}

Result<ParsedPromptTemplateDocument> parse_prompt_template_document(
    const std::string& raw_content) {
  ParsedPromptTemplateDocument document;
  const std::string normalized = normalize_newlines(raw_content);

  const std::size_t first_line_end = normalized.find('\n');
  const std::string first_line = normalized.substr(0, first_line_end);
  if (first_line != "---") {
    document.content = trim_copy(normalized);
    document.description = derive_description(document.content);
    return document;
  }

  std::size_t offset =
      first_line_end == std::string::npos ? normalized.size() : first_line_end + 1;
  bool found_closing_delimiter = false;

  while (offset <= normalized.size()) {
    const std::size_t line_end = normalized.find('\n', offset);
    const std::string line = normalized.substr(
        offset, line_end == std::string::npos ? line_end : line_end - offset);

    if (line == "---") {
      found_closing_delimiter = true;
      offset = line_end == std::string::npos ? normalized.size() : line_end + 1;
      break;
    }

    const std::string trimmed = trim_copy(line);
    if (!trimmed.empty()) {
      const std::size_t separator = trimmed.find(':');
      if (separator == std::string::npos) {
        return Result<ParsedPromptTemplateDocument>::failure(
            ErrorCode::invalid_argument,
            "frontmatter line is missing ':' separator");
      }

      const std::string key = trim_copy(trimmed.substr(0, separator));
      const std::string value =
          maybe_unquote(trim_copy(trimmed.substr(separator + 1)));

      if (key == "description") {
        document.description = value;
      } else if (key == "argument-hint" && !value.empty()) {
        document.argument_hint = value;
      }
    }

    if (line_end == std::string::npos) {
      break;
    }
    offset = line_end + 1;
  }

  if (!found_closing_delimiter) {
    return Result<ParsedPromptTemplateDocument>::failure(
        ErrorCode::invalid_argument,
        "frontmatter block is missing closing delimiter");
  }

  document.content = trim_copy(normalized.substr(offset));
  if (document.description.empty()) {
    document.description = derive_description(document.content);
  }

  return document;
}

std::vector<Path> list_prompt_files(const Path& root) {
  std::vector<Path> files;
  std::error_code error;
  for (std::filesystem::directory_iterator it(root, error), end; !error && it != end;
       it.increment(error)) {
    const Path path = it->path();
    if (!has_markdown_extension(path)) {
      continue;
    }
    if (!path_is_regular_file(path)) {
      continue;
    }
    files.push_back(path);
  }

  std::sort(files.begin(), files.end(),
            [](const Path& lhs, const Path& rhs) {
              return lhs.filename().generic_string() <
                     rhs.filename().generic_string();
            });
  return files;
}

std::vector<std::string> read_pi_manifest_entries(
    const RepositoryView& repository, const Path& package_json,
    PromptTemplateResources& resources) {
  const std::string package_path = path_to_string(package_json);
  const auto content = repository.read_text_file(package_path);
  if (!content) {
    resources.add_diagnostic(PromptTemplateDiagnostic{
        PromptTemplateDiagnosticCode::kPromptFileReadFailed,
        package_path,
        "failed to read package manifest: " + package_path,
    });
    return {};
  }

  try {
    const auto json = nlohmann::json::parse(*content);
    const auto pi = json.find("pi");
    if (pi == json.end() || !pi->is_object()) {
      return {};
    }
    const auto entries = pi->find("prompts");
    if (entries == pi->end() || !entries->is_array()) {
      return {};
    }
    return entries->get<std::vector<std::string>>();
  } catch (const std::exception& error) {
    resources.add_diagnostic(PromptTemplateDiagnostic{
        PromptTemplateDiagnosticCode::kInvalidFrontmatter,
        package_path,
        "failed to decode package manifest: " + package_path + " (" +
            error.what() + ")",
    });
    return {};
  }
}

void load_prompt_file(const RepositoryView& repository, const std::string& root,
                      const Path& path, PromptTemplateResources& resources) {
  const std::string prompt_path = path_to_string(path);
  if (!path_is_regular_file(path)) {
    resources.add_diagnostic(PromptTemplateDiagnostic{
        PromptTemplateDiagnosticCode::kPromptFileNotReadable,
        prompt_path,
        "prompt template entry is not a readable file: " + prompt_path,
    });
    return;
  }

  const auto content = repository.read_text_file(prompt_path);
  if (!content) {
    resources.add_diagnostic(PromptTemplateDiagnostic{
        PromptTemplateDiagnosticCode::kPromptFileReadFailed,
        prompt_path,
        "failed to read prompt template file: " + prompt_path,
    });
    return;
  }

  const auto parsed = parse_prompt_template_document(*content);
  if (!parsed) {
    resources.add_diagnostic(PromptTemplateDiagnostic{
        PromptTemplateDiagnosticCode::kInvalidFrontmatter,
        prompt_path,
        "invalid prompt template frontmatter: " + prompt_path + " (" +
            parsed.error().message + ")",
    });
    return;
  }

  std::string name = path.filename().generic_string();
  if (name.size() >= 3) {
    name.erase(name.size() - 3);
  }

  resources.loaded_prompt_templates.push_back(LoadedPromptTemplateRecord{
      root,
      prompt_path,
      std::move(name),
      parsed.value().description,
      parsed.value().argument_hint,
      parsed.value().content,
  });
}

bool load_package_prompt_templates(const RepositoryView& repository,
                                   const std::string& root,
                                   const Path& directory,
                                   PromptTemplateResources& resources) {
  const Path package_json = directory / "package.json";
  if (!path_is_regular_file(package_json)) {
    return false;
  }

  const auto declared = read_pi_manifest_entries(repository, package_json, resources);
  if (declared.empty()) {
    return false;
  }

  for (const auto& entry : declared) {
    const Path resolved = Path(entry).is_absolute()
                              ? Path(entry)
                              : directory / Path(entry);
    if (path_is_directory(resolved)) {
      for (const auto& prompt_file : list_prompt_files(resolved)) {
        load_prompt_file(repository, root, prompt_file, resources);
      }
    } else if (path_is_regular_file(resolved) && has_markdown_extension(resolved)) {
      load_prompt_file(repository, root, resolved, resources);
    } else {
      const std::string resolved_path = path_to_string(resolved);
      resources.add_diagnostic(PromptTemplateDiagnostic{
          PromptTemplateDiagnosticCode::kPromptFileNotReadable,
          resolved_path,
          "package prompt template entry does not exist: " + resolved_path,
      });
    }
  }

  return true;
}

void load_prompt_root(const RepositoryView& repository,
                      const std::string& root,
                      PromptTemplateResources& resources) {
  const Path root_path(root);
  if (load_package_prompt_templates(repository, root, root_path, resources)) {
    return;
  }

  for (const auto& path : list_prompt_files(root_path)) {
    load_prompt_file(repository, root, path, resources);
  }
}

}  // namespace

PromptTemplateResources load_prompt_template_resources(
    const RepositoryView& repository,
    const std::vector<std::string>& prompt_roots) {
  PromptTemplateResources resources;
  resources.roots = prompt_roots;

  for (const auto& root : prompt_roots) {
    if (root.empty()) {
      resources.add_diagnostic(PromptTemplateDiagnostic{
          PromptTemplateDiagnosticCode::kEmptyRoot,
          "",
          "prompt template root was empty",
      });
      continue;
    }

    const Path root_path = normalize_prompt_root_path(root);
    const std::string resolved_root = path_to_string(root_path);
    if (!path_exists(root_path)) {
      resources.add_diagnostic(PromptTemplateDiagnostic{
          PromptTemplateDiagnosticCode::kRootDoesNotExist,
          resolved_root,
          "prompt template root does not exist: " + resolved_root,
      });
      continue;
    }
    if (!path_is_directory(root_path)) {
      resources.add_diagnostic(PromptTemplateDiagnostic{
          PromptTemplateDiagnosticCode::kRootIsNotDirectory,
          resolved_root,
          "prompt template root is not a directory: " + resolved_root,
      });
      continue;
    }

    load_prompt_root(repository, resolved_root, resources);
  }

  return resources;
}

PromptTemplateResources discover_and_load_prompt_templates(
    const RepositoryView& repository,
    const RepoContextDiscovery& discovery) {
  return load_prompt_template_resources(repository, discovery.prompt_roots);
}

std::vector<std::string> parse_prompt_template_arguments(
    std::string_view arguments) {
  std::vector<std::string> values;
  std::string current;
  char quote = '\0';

  for (const char value : arguments) {
    if (quote != '\0') {
      if (value == quote) {
        quote = '\0';
      } else {
        current.push_back(value);
      }
      continue;
    }

    if (value == '\'' || value == '"') {
      quote = value;
      continue;
    }

    if (value == ' ' || value == '\t') {
      if (!current.empty()) {
        values.push_back(std::move(current));
        current.clear();
      }
      continue;
    }

    current.push_back(value);
  }

  if (!current.empty()) {
    values.push_back(std::move(current));
  }
  return values;
}

std::string expand_prompt_template_content(
    std::string_view content,
    const std::vector<std::string>& arguments) {
  const std::string all_arguments = join_arguments_from(arguments, 0);
  std::string expanded;
  expanded.reserve(content.size() + all_arguments.size());

  for (std::size_t index = 0; index < content.size();) {
    if (content[index] != '$') {
      expanded.push_back(content[index]);
      ++index;
      continue;
    }

    if (index + 1 < content.size() && is_digit(content[index + 1])) {
      std::size_t number_end = index + 2;
      while (number_end < content.size() && is_digit(content[number_end])) {
        ++number_end;
      }

      const std::size_t position =
          parse_decimal(content.substr(index + 1, number_end - index - 1));
      if (position >= 1 && position <= arguments.size()) {
        expanded += arguments[position - 1];
      }
      index = number_end;
      continue;
    }

    constexpr std::string_view arguments_placeholder = "$ARGUMENTS";
    if (content.substr(index, arguments_placeholder.size()) ==
        arguments_placeholder) {
      expanded += all_arguments;
      index += arguments_placeholder.size();
      continue;
    }

    if (index + 1 < content.size() && content[index + 1] == '@') {
      expanded += all_arguments;
      index += 2;
      continue;
    }

    constexpr std::string_view slice_prefix = "${@:";
    if (content.substr(index, slice_prefix.size()) == slice_prefix) {
      std::size_t cursor = index + slice_prefix.size();
      const std::size_t start_begin = cursor;
      while (cursor < content.size() && is_digit(content[cursor])) {
        ++cursor;
      }

      if (cursor > start_begin) {
        const std::size_t start_position =
            parse_decimal(content.substr(start_begin, cursor - start_begin));
        std::optional<std::size_t> length;

        if (cursor < content.size() && content[cursor] == ':') {
          ++cursor;
          const std::size_t length_begin = cursor;
          while (cursor < content.size() && is_digit(content[cursor])) {
            ++cursor;
          }
          if (cursor > length_begin) {
            length = parse_decimal(
                content.substr(length_begin, cursor - length_begin));
          } else {
            expanded.push_back(content[index]);
            ++index;
            continue;
          }
        }

        if (cursor < content.size() && content[cursor] == '}') {
          const std::size_t start =
              start_position == 0 ? 0 : start_position - 1;
          expanded += length ? join_arguments(arguments, start, *length)
                             : join_arguments_from(arguments, start);
          index = cursor + 1;
          continue;
        }
      }
    }

    expanded.push_back(content[index]);
    ++index;
  }

  return expanded;
}

std::string expand_prompt_template_content(
    std::string_view content,
    std::string_view arguments) {
  return expand_prompt_template_content(
      content, parse_prompt_template_arguments(arguments));
}

}  // namespace qt::harness
