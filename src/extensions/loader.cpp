#include <qt/extensions/loader.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>

#include <qt/extensions/api_compatibility.hpp>
#include <qt/extensions/angelscript_host.hpp>

namespace qt::extensions {
namespace {

using Path = std::filesystem::path;

std::string path_to_string(const Path& path) {
  return path.lexically_normal().generic_string();
}

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

bool is_extension_file(const Path& path) {
  const std::string extension = path.extension().generic_string();
  return extension == ".ts" || extension == ".js";
}

bool is_angelscript_extension_file(const Path& path) {
  return path.extension().generic_string() == ".as";
}

bool is_script_extension_file(const Path& path) {
  return is_extension_file(path) || is_angelscript_extension_file(path);
}

Path normalize_extension_root_path(const std::string& root) {
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

std::vector<Path> list_directory_entries(const Path& directory);

bool is_override_pattern(const std::string& pattern) {
  return !pattern.empty() &&
         (pattern.front() == '!' || pattern.front() == '+' ||
          pattern.front() == '-');
}

std::string strip_dot_slash(std::string pattern) {
  if (pattern.rfind("./", 0) == 0) {
    return pattern.substr(2);
  }
  return pattern;
}

std::string normalize_pattern(std::string pattern) {
  std::replace(pattern.begin(), pattern.end(), '\\', '/');
  return strip_dot_slash(std::move(pattern));
}

bool glob_match_after_starstar(const std::string& text, std::size_t text_index,
                               const std::string& pattern,
                               std::size_t pattern_index);

bool glob_match_from(const std::string& text, std::size_t text_index,
                     const std::string& pattern, std::size_t pattern_index) {
  while (pattern_index < pattern.size()) {
    if (pattern[pattern_index] == '*') {
      const bool double_star = pattern_index + 1 < pattern.size() &&
                               pattern[pattern_index + 1] == '*';
      if (double_star) {
        return glob_match_after_starstar(text, text_index, pattern,
                                         pattern_index + 2);
      }

      ++pattern_index;
      for (std::size_t next = text_index; next <= text.size(); ++next) {
        if (next > text_index && text[next - 1] == '/') {
          break;
        }
        if (glob_match_from(text, next, pattern, pattern_index)) {
          return true;
        }
      }
      return false;
    }

    if (text_index >= text.size()) {
      return false;
    }
    if (pattern[pattern_index] == '?') {
      if (text[text_index] == '/') {
        return false;
      }
    } else if (pattern[pattern_index] != text[text_index]) {
      return false;
    }
    ++text_index;
    ++pattern_index;
  }

  return text_index == text.size();
}

bool glob_match_after_starstar(const std::string& text, std::size_t text_index,
                               const std::string& pattern,
                               std::size_t pattern_index) {
  if (pattern_index < pattern.size() && pattern[pattern_index] == '/') {
    if (glob_match_from(text, text_index, pattern, pattern_index + 1)) {
      return true;
    }
  }
  for (std::size_t next = text_index; next <= text.size(); ++next) {
    if (glob_match_from(text, next, pattern, pattern_index)) {
      return true;
    }
  }
  return false;
}

bool glob_match(const std::string& text, const std::string& pattern) {
  return glob_match_from(text, 0, normalize_pattern(pattern), 0);
}

std::string relative_to_base(const Path& path, const Path& base) {
  std::error_code error;
  Path relative = std::filesystem::relative(path, base, error);
  if (error) {
    relative = path.lexically_relative(base);
  }
  return relative.empty() ? path.filename().generic_string()
                          : relative.lexically_normal().generic_string();
}

bool matches_pattern_path(const Path& path, const std::string& pattern,
                          const Path& base) {
  const std::string normalized = normalize_pattern(pattern);
  const std::string relative = relative_to_base(path, base);
  const std::string rooted_relative =
      (base.filename() / Path(relative)).generic_string();
  const std::string name = path.filename().generic_string();
  const std::string absolute = path_to_string(path);
  return glob_match(relative, normalized) || glob_match(name, normalized) ||
         glob_match(rooted_relative, normalized) ||
         glob_match(absolute, normalized);
}

bool matches_exact_path(const Path& path, const std::string& pattern,
                        const Path& base) {
  const std::string normalized = normalize_pattern(pattern);
  const std::string relative = relative_to_base(path, base);
  const std::string rooted_relative =
      (base.filename() / Path(relative)).generic_string();
  const std::string absolute = path_to_string(path);
  return normalized == relative || normalized == rooted_relative ||
         normalized == absolute ||
         normalized == path.filename().generic_string();
}

std::vector<Path> apply_filter_patterns(std::vector<Path> paths,
                                        const std::vector<std::string>& patterns,
                                        const Path& base) {
  if (patterns.empty()) {
    return paths;
  }

  std::vector<std::string> includes;
  std::vector<std::string> excludes;
  std::vector<std::string> force_includes;
  std::vector<std::string> force_excludes;
  for (const auto& pattern : patterns) {
    if (pattern.empty()) {
      continue;
    }
    if (pattern.front() == '+') {
      force_includes.push_back(pattern.substr(1));
    } else if (pattern.front() == '-') {
      force_excludes.push_back(pattern.substr(1));
    } else if (pattern.front() == '!') {
      excludes.push_back(pattern.substr(1));
    } else {
      includes.push_back(pattern);
    }
  }

  auto matches_any_pattern = [&](const Path& path,
                                 const std::vector<std::string>& candidates) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const std::string& pattern) {
                         return matches_pattern_path(path, pattern, base);
                       });
  };
  auto matches_any_exact = [&](const Path& path,
                               const std::vector<std::string>& candidates) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const std::string& pattern) {
                         return matches_exact_path(path, pattern, base);
                       });
  };

  std::vector<Path> filtered;
  if (includes.empty()) {
    filtered = paths;
  } else {
    std::copy_if(paths.begin(), paths.end(), std::back_inserter(filtered),
                 [&](const Path& path) {
                   return matches_any_pattern(path, includes);
                 });
  }

  if (!excludes.empty()) {
    filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
                                  [&](const Path& path) {
                                    return matches_any_pattern(path, excludes);
                                  }),
                   filtered.end());
  }

  if (!force_includes.empty()) {
    for (const auto& path : paths) {
      if (std::find(filtered.begin(), filtered.end(), path) ==
              filtered.end() &&
          matches_any_exact(path, force_includes)) {
        filtered.push_back(path);
      }
    }
  }

  if (!force_excludes.empty()) {
    filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
                                  [&](const Path& path) {
                                    return matches_any_exact(path,
                                                             force_excludes);
                                  }),
                   filtered.end());
  }

  return filtered;
}

bool extension_record_enabled(const LoadedExtensionRecord& record,
                              const std::vector<std::string>& patterns) {
  if (patterns.empty()) {
    return true;
  }
  const auto filtered =
      apply_filter_patterns({Path(record.path)}, patterns, Path(record.root));
  return !filtered.empty();
}

std::vector<Path> list_subdirectories(const Path& directory) {
  std::vector<Path> entries;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    const Path path = it->path();
    if (path_is_directory(path)) {
      entries.push_back(path);
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const Path& lhs, const Path& rhs) {
              return lhs.generic_string() < rhs.generic_string();
            });
  return entries;
}

void collect_script_extension_entries(const Path& directory,
                                      std::vector<Path>& entries) {
  const Path index_ts = directory / "index.ts";
  const Path index_js = directory / "index.js";
  const Path index_as = directory / "index.as";
  if (path_is_regular_file(index_ts)) {
    entries.push_back(index_ts);
    return;
  }
  if (path_is_regular_file(index_js)) {
    entries.push_back(index_js);
    return;
  }
  if (path_is_regular_file(index_as)) {
    entries.push_back(index_as);
    return;
  }

  for (const auto& entry : list_directory_entries(directory)) {
    if (path_is_regular_file(entry) && is_script_extension_file(entry)) {
      entries.push_back(entry);
    }
  }

  for (const auto& child : list_subdirectories(directory)) {
    collect_script_extension_entries(child, entries);
  }
}

std::vector<Path> list_directory_entries(const Path& directory) {
  std::vector<Path> entries;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    const Path path = it->path();
    const std::string filename = path.filename().generic_string();
    if (!filename.empty() && filename.front() == '.') {
      continue;
    }
    if (filename == "node_modules") {
      continue;
    }
    entries.push_back(path);
  }

  std::sort(entries.begin(), entries.end(),
            [](const Path& lhs, const Path& rhs) {
              return lhs.generic_string() < rhs.generic_string();
            });
  return entries;
}

std::string join_validation_issues(const ManifestValidationResult& validation) {
  std::ostringstream stream;
  for (std::size_t index = 0; index < validation.issues.size(); ++index) {
    if (index > 0) {
      stream << "; ";
    }
    stream << validation.issues[index].summary();
  }
  return stream.str();
}

std::string sanitize_identifier_component(std::string text) {
  std::string out;
  for (char value : text) {
    const unsigned char ch = static_cast<unsigned char>(value);
    if (std::isalnum(ch)) {
      out.push_back(static_cast<char>(std::tolower(ch)));
    } else if (value == '.' || value == '_' || value == '-') {
      out.push_back(value);
    } else {
      out.push_back('-');
    }
  }
  while (!out.empty() &&
         !(out.front() >= 'a' && out.front() <= 'z') &&
         !(out.front() >= '0' && out.front() <= '9')) {
    out.erase(out.begin());
  }
  return out.empty() ? "extension" : out;
}

std::string extension_name(const Path& entrypoint) {
  const Path parent = entrypoint.parent_path();
  if (entrypoint.filename() == "index.ts" || entrypoint.filename() == "index.js" ||
      entrypoint.filename() == "index.as") {
    return sanitize_identifier_component(parent.filename().generic_string());
  }
  return sanitize_identifier_component(entrypoint.stem().generic_string());
}

struct PackageExtensionEntries {
  bool declared = false;
  std::vector<Path> entries;
  std::vector<std::string> declared_paths;
  Json package_metadata = Json::object();
};

std::optional<std::string> optional_json_string(const Json& json,
                                                const char* key) {
  const auto value = json.find(key);
  if (value != json.end() && value->is_string()) {
    return value->get<std::string>();
  }
  return std::nullopt;
}

bool starts_with_at(std::string_view text, std::size_t offset,
                    std::string_view needle) {
  return offset <= text.size() && needle.size() <= text.size() - offset &&
         text.substr(offset, needle.size()) == needle;
}

void skip_whitespace(std::string_view text, std::size_t& offset) {
  while (offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[offset])) != 0) {
    ++offset;
  }
}

std::optional<std::string> parse_typescript_string_literal(
    std::string_view text, std::size_t& offset) {
  skip_whitespace(text, offset);
  if (offset >= text.size()) {
    return std::nullopt;
  }

  const char quote = text[offset];
  if (quote != '"' && quote != '\'' && quote != '`') {
    return std::nullopt;
  }
  ++offset;

  std::string value;
  bool escaped = false;
  for (; offset < text.size(); ++offset) {
    const char ch = text[offset];
    if (escaped) {
      value.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == quote) {
      ++offset;
      return value;
    }
    value.push_back(ch);
  }
  return std::nullopt;
}

std::optional<std::string_view> extract_balanced_region(
    std::string_view text, std::size_t open_offset, char open, char close) {
  if (open_offset >= text.size() || text[open_offset] != open) {
    return std::nullopt;
  }

  std::size_t depth = 1;
  std::size_t body_start = open_offset + 1;
  char quote = '\0';
  bool escaped = false;
  for (std::size_t offset = body_start; offset < text.size(); ++offset) {
    const char ch = text[offset];
    if (quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'' || ch == '`') {
      quote = ch;
      continue;
    }
    if (ch == open) {
      ++depth;
    } else if (ch == close) {
      --depth;
      if (depth == 0) {
        return text.substr(body_start, offset - body_start);
      }
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> extract_call_body(std::string_view text,
                                                  std::size_t offset,
                                                  std::string_view name) {
  offset += name.size();
  skip_whitespace(text, offset);
  return extract_balanced_region(text, offset, '(', ')');
}

bool is_identifier_start(char ch) {
  return std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_' ||
         ch == '$';
}

bool is_identifier_char(char ch) {
  return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' ||
         ch == '$';
}

std::optional<std::string> parse_identifier(std::string_view text,
                                            std::size_t& offset) {
  skip_whitespace(text, offset);
  if (offset >= text.size() || !is_identifier_start(text[offset])) {
    return std::nullopt;
  }
  const std::size_t start = offset;
  ++offset;
  while (offset < text.size() && is_identifier_char(text[offset])) {
    ++offset;
  }
  return std::string(text.substr(start, offset - start));
}

std::optional<std::string_view> first_object_argument(std::string_view text,
                                                      std::size_t offset) {
  skip_whitespace(text, offset);
  if (offset < text.size() && text[offset] == '{') {
    return extract_balanced_region(text, offset, '{', '}');
  }
  const auto identifier = parse_identifier(text, offset);
  if (!identifier) {
    return std::nullopt;
  }
  skip_whitespace(text, offset);
  if (offset >= text.size() || text[offset] != '(') {
    return std::nullopt;
  }
  auto body = extract_balanced_region(text, offset, '(', ')');
  if (!body) {
    return std::nullopt;
  }
  std::size_t body_offset = 0;
  skip_whitespace(*body, body_offset);
  if (body_offset >= body->size() || (*body)[body_offset] != '{') {
    return std::nullopt;
  }
  return extract_balanced_region(*body, body_offset, '{', '}');
}

std::map<std::string, std::string> collect_object_definitions(
    std::string_view text) {
  std::map<std::string, std::string> definitions;
  for (std::size_t offset = 0; offset < text.size(); ++offset) {
    std::string_view keyword;
    if (starts_with_at(text, offset, "const")) {
      keyword = "const";
    } else if (starts_with_at(text, offset, "let")) {
      keyword = "let";
    } else if (starts_with_at(text, offset, "var")) {
      keyword = "var";
    } else {
      continue;
    }

    const std::size_t after_keyword = offset + keyword.size();
    if (after_keyword < text.size() && is_identifier_char(text[after_keyword])) {
      continue;
    }
    std::size_t cursor = after_keyword;
    auto identifier = parse_identifier(text, cursor);
    if (!identifier) {
      continue;
    }
    while (cursor < text.size() && text[cursor] != '=' &&
           text[cursor] != ';' && text[cursor] != '\n') {
      ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != '=') {
      continue;
    }
    ++cursor;
    const auto object = first_object_argument(text, cursor);
    if (object) {
      definitions[*identifier] = std::string(*object);
    }
  }
  return definitions;
}

std::optional<std::string> object_string_property(std::string_view body,
                                                  std::string_view property) {
  int depth = 0;
  char quote = '\0';
  bool escaped = false;
  for (std::size_t offset = 0; offset < body.size(); ++offset) {
    const char ch = body[offset];
    if (quote != '\0') {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == quote) {
        quote = '\0';
      }
      continue;
    }
    if (ch == '"' || ch == '\'' || ch == '`') {
      quote = ch;
      continue;
    }
    if (ch == '{' || ch == '[' || ch == '(') {
      ++depth;
      continue;
    }
    if (ch == '}' || ch == ']' || ch == ')') {
      --depth;
      continue;
    }
    if (depth != 0) {
      continue;
    }

    std::size_t cursor = offset;
    std::optional<std::string> key;
    if (ch == '"' || ch == '\'') {
      key = parse_typescript_string_literal(body, cursor);
    } else if (is_identifier_start(ch)) {
      key = parse_identifier(body, cursor);
    }
    if (!key || *key != property) {
      continue;
    }
    skip_whitespace(body, cursor);
    if (cursor >= body.size() || body[cursor] != ':') {
      continue;
    }
    ++cursor;
    return parse_typescript_string_literal(body, cursor);
  }
  return std::nullopt;
}

std::optional<std::pair<std::string, std::size_t>> parse_object_key(
    std::string_view body, std::size_t offset) {
  skip_whitespace(body, offset);
  if (offset >= body.size()) {
    return std::nullopt;
  }
  std::optional<std::string> key;
  if (body[offset] == '"' || body[offset] == '\'' || body[offset] == '`') {
    key = parse_typescript_string_literal(body, offset);
  } else if (is_identifier_start(body[offset])) {
    key = parse_identifier(body, offset);
  }
  if (!key) {
    return std::nullopt;
  }
  skip_whitespace(body, offset);
  if (offset >= body.size() || body[offset] != ':') {
    return std::nullopt;
  }
  return std::make_pair(*key, offset + 1);
}

std::size_t skip_typescript_value(std::string_view body, std::size_t offset) {
  skip_whitespace(body, offset);
  if (offset >= body.size()) {
    return offset;
  }
  if (body[offset] == '"' || body[offset] == '\'' || body[offset] == '`') {
    (void)parse_typescript_string_literal(body, offset);
    return offset;
  }
  if (body[offset] == '{') {
    if (const auto nested = extract_balanced_region(body, offset, '{', '}')) {
      return offset + nested->size() + 2;
    }
  }
  if (body[offset] == '[') {
    if (const auto nested = extract_balanced_region(body, offset, '[', ']')) {
      return offset + nested->size() + 2;
    }
  }
  if (body[offset] == '(') {
    if (const auto nested = extract_balanced_region(body, offset, '(', ')')) {
      return offset + nested->size() + 2;
    }
  }
  while (offset < body.size() && body[offset] != ',' && body[offset] != '}' &&
         body[offset] != ']') {
    ++offset;
  }
  return offset;
}

std::optional<Json> typescript_object_literal_to_json(std::string_view body);
std::optional<Json> typescript_array_literal_to_json(std::string_view body);

std::optional<Json> typescript_literal_value_to_json(std::string_view body,
                                                     std::size_t& offset) {
  skip_whitespace(body, offset);
  if (offset >= body.size()) {
    return std::nullopt;
  }
  if (body[offset] == '"' || body[offset] == '\'' || body[offset] == '`') {
    if (auto value = parse_typescript_string_literal(body, offset)) {
      return Json(*value);
    }
    return std::nullopt;
  }
  if (body[offset] == '{') {
    const auto nested = extract_balanced_region(body, offset, '{', '}');
    if (!nested) {
      return std::nullopt;
    }
    offset += nested->size() + 2;
    return typescript_object_literal_to_json(*nested);
  }
  if (body[offset] == '[') {
    const auto nested = extract_balanced_region(body, offset, '[', ']');
    if (!nested) {
      return std::nullopt;
    }
    offset += nested->size() + 2;
    return typescript_array_literal_to_json(*nested);
  }
  if (starts_with_at(body, offset, "true")) {
    offset += 4;
    return Json(true);
  }
  if (starts_with_at(body, offset, "false")) {
    offset += 5;
    return Json(false);
  }
  if (starts_with_at(body, offset, "null")) {
    offset += 4;
    return Json(nullptr);
  }

  const std::size_t start = offset;
  if (body[offset] == '-') {
    ++offset;
  }
  while (offset < body.size() &&
         std::isdigit(static_cast<unsigned char>(body[offset])) != 0) {
    ++offset;
  }
  bool has_fraction = false;
  if (offset < body.size() && body[offset] == '.') {
    const auto fraction_start = offset;
    ++offset;
    while (offset < body.size() &&
           std::isdigit(static_cast<unsigned char>(body[offset])) != 0) {
      ++offset;
    }
    has_fraction = offset > fraction_start + 1;
    if (!has_fraction) {
      offset = fraction_start;
    }
  }
  if (offset > start && (body[start] != '-' || offset > start + 1)) {
    try {
      if (has_fraction) {
        return Json(std::stod(std::string(body.substr(start, offset - start))));
      }
      return Json(std::stoll(std::string(body.substr(start, offset - start))));
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<Json> typescript_array_literal_to_json(std::string_view body) {
  Json result = Json::array();
  for (std::size_t offset = 0; offset < body.size();) {
    skip_whitespace(body, offset);
    if (offset < body.size() && body[offset] == ',') {
      ++offset;
      continue;
    }
    if (offset >= body.size()) {
      break;
    }

    std::size_t value_offset = offset;
    if (auto value = typescript_literal_value_to_json(body, value_offset)) {
      result.push_back(*value);
      offset = value_offset;
    } else {
      offset = skip_typescript_value(body, offset);
    }
    skip_whitespace(body, offset);
    if (offset < body.size() && body[offset] == ',') {
      ++offset;
    }
  }
  return result;
}

std::optional<Json> typescript_object_literal_to_json(std::string_view body) {
  Json result = Json::object();
  for (std::size_t offset = 0; offset < body.size();) {
    skip_whitespace(body, offset);
    if (offset < body.size() && body[offset] == ',') {
      ++offset;
      continue;
    }
    const auto parsed_key = parse_object_key(body, offset);
    if (!parsed_key) {
      break;
    }
    const auto& key = parsed_key->first;
    offset = parsed_key->second;
    std::size_t value_offset = offset;
    if (auto value = typescript_literal_value_to_json(body, value_offset)) {
      result[key] = *value;
      offset = value_offset;
    } else {
      if (key == "streamSimple" || key == "getToken" ||
          key == "modifyModels") {
        result[key] = true;
      }
      offset = skip_typescript_value(body, offset);
    }
    skip_whitespace(body, offset);
    if (offset < body.size() && body[offset] == ',') {
      ++offset;
    }
  }
  return result;
}

std::optional<std::string_view> resolve_object_argument(
    std::string_view body, const std::map<std::string, std::string>& objects,
    std::optional<std::string>& owned) {
  std::size_t offset = 0;
  skip_whitespace(body, offset);
  if (offset < body.size() && body[offset] == '{') {
    return extract_balanced_region(body, offset, '{', '}');
  }
  auto identifier = parse_identifier(body, offset);
  if (!identifier) {
    return std::nullopt;
  }
  const auto found = objects.find(*identifier);
  if (found == objects.end()) {
    return std::nullopt;
  }
  owned = found->second;
  return std::string_view(*owned);
}

void append_not_implemented_api_metadata(
    ExtensionManifest& manifest,
    const std::vector<ExtensionApiUseDiagnostic>& diagnostics) {
  if (diagnostics.empty()) {
    return;
  }
  if (!manifest.metadata.contains("not_implemented_apis") ||
      !manifest.metadata["not_implemented_apis"].is_array()) {
    manifest.metadata["not_implemented_apis"] = Json::array();
  }
  for (const auto& diagnostic : diagnostics) {
    manifest.metadata["not_implemented_apis"].push_back(Json{
        {"api", diagnostic.api},
        {"message", diagnostic.message},
        {"row", diagnostic.row},
        {"column", diagnostic.column},
    });
  }
}

void append_not_implemented_api_load_diagnostics(
    ExtensionLoadResult& result, const std::string& path,
    const std::vector<ExtensionApiUseDiagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    result.add_diagnostic(diagnostic.message + ": " + path + ":" +
                          std::to_string(diagnostic.row) + ":" +
                          std::to_string(diagnostic.column));
  }
}

void populate_module_registrations(ExtensionManifest& manifest,
                                             std::string_view source) {
  const auto object_definitions = collect_object_definitions(source);

  for (std::size_t offset = 0; offset < source.size(); ++offset) {
    if (starts_with_at(source, offset, "pi.on")) {
      auto body = extract_call_body(source, offset, "pi.on");
      if (!body) {
        continue;
      }
      std::size_t cursor = 0;
      auto event_name = parse_typescript_string_literal(*body, cursor);
      if (!event_name) {
        continue;
      }
      if (!manifest.metadata.contains("lifecycle_handlers") ||
          !manifest.metadata["lifecycle_handlers"].is_array()) {
        manifest.metadata["lifecycle_handlers"] = Json::array();
      }
      manifest.metadata["lifecycle_handlers"].push_back(
          Json{{"event", *event_name}});
      continue;
    }

    if (starts_with_at(source, offset, "pi.registerCommand")) {
      auto body = extract_call_body(source, offset, "pi.registerCommand");
      if (!body) {
        continue;
      }
      std::size_t cursor = 0;
      auto name = parse_typescript_string_literal(*body, cursor);
      if (!name) {
        continue;
      }
      while (cursor < body->size() && (*body)[cursor] != ',') {
        ++cursor;
      }
      std::optional<std::string> owned_object;
      std::optional<std::string_view> options_object;
      if (cursor < body->size() && (*body)[cursor] == ',') {
        ++cursor;
        options_object =
            resolve_object_argument(body->substr(cursor),
                                              object_definitions, owned_object);
      }
      ExtensionCommandContribution command;
      command.name = *name;
      if (options_object) {
        if (const auto title = object_string_property(*options_object, "title")) {
          command.title = *title;
        } else if (const auto label =
                       object_string_property(*options_object, "label")) {
          command.title = *label;
        }
        if (const auto description =
                object_string_property(*options_object, "description")) {
          command.description = *description;
        }
      }
      manifest.commands.push_back(std::move(command));
      continue;
    }

    if (starts_with_at(source, offset, "pi.registerFlag")) {
      auto body = extract_call_body(source, offset, "pi.registerFlag");
      if (!body) {
        continue;
      }
      std::size_t cursor = 0;
      auto name = parse_typescript_string_literal(*body, cursor);
      if (!name) {
        continue;
      }
      while (cursor < body->size() && (*body)[cursor] != ',') {
        ++cursor;
      }
      std::optional<std::string> owned_object;
      std::optional<std::string_view> options_object;
      if (cursor < body->size() && (*body)[cursor] == ',') {
        ++cursor;
        options_object =
            resolve_object_argument(body->substr(cursor),
                                              object_definitions, owned_object);
      }
      ExtensionFlagContribution flag;
      flag.name = *name;
      if (options_object) {
        if (const auto description =
                object_string_property(*options_object, "description")) {
          flag.description = *description;
        }
        if (const auto parsed_options =
                typescript_object_literal_to_json(*options_object);
            parsed_options && parsed_options->is_object()) {
          flag.type =
              parsed_options->value("type", std::string("boolean"));
          if (parsed_options->contains("default")) {
            flag.default_value = parsed_options->at("default");
          }
        }
      }
      manifest.flags.push_back(std::move(flag));
      continue;
    }

    if (starts_with_at(source, offset, "pi.registerTool")) {
      auto body = extract_call_body(source, offset, "pi.registerTool");
      if (!body) {
        continue;
      }
      std::optional<std::string> owned_object;
      const auto object =
          resolve_object_argument(*body, object_definitions,
                                            owned_object);
      if (!object) {
        continue;
      }
      const auto name = object_string_property(*object, "name");
      if (!name) {
        continue;
      }
      ExtensionToolContribution tool;
      tool.name = *name;
      if (const auto title = object_string_property(*object, "title")) {
        tool.title = *title;
      } else if (const auto label = object_string_property(*object, "label")) {
        tool.title = *label;
      }
      if (const auto description =
              object_string_property(*object, "description")) {
        tool.description = *description;
      }
      manifest.tools.push_back(std::move(tool));
      continue;
    }

    if (starts_with_at(source, offset, "pi.registerProvider")) {
      auto body = extract_call_body(source, offset, "pi.registerProvider");
      if (!body) {
        continue;
      }
      std::size_t cursor = 0;
      auto name = parse_typescript_string_literal(*body, cursor);
      if (!name) {
        continue;
      }
      while (cursor < body->size() && (*body)[cursor] != ',') {
        ++cursor;
      }
      if (cursor >= body->size() || (*body)[cursor] != ',') {
        continue;
      }
      ++cursor;
      std::optional<std::string> owned_object;
      const auto object =
          resolve_object_argument(body->substr(cursor),
                                            object_definitions, owned_object);
      if (!object) {
        continue;
      }
      const auto config = typescript_object_literal_to_json(*object);
      if (!config || !config->is_object()) {
        continue;
      }
      manifest.providers.push_back(
          ExtensionProviderContribution{*name, *config});
      continue;
    }

    if (starts_with_at(source, offset, "pi.unregisterProvider")) {
      auto body = extract_call_body(source, offset, "pi.unregisterProvider");
      if (!body) {
        continue;
      }
      std::size_t cursor = 0;
      auto name = parse_typescript_string_literal(*body, cursor);
      if (!name) {
        continue;
      }
      manifest.metadata["provider_unregistrations"].push_back(
          Json{{"name", *name}});
    }
  }
}

Json package_dependency_names(const Json& package_json) {
  Json names = Json::array();
  const auto dependencies = package_json.find("dependencies");
  if (dependencies == package_json.end() || !dependencies->is_object()) {
    return names;
  }
  for (auto it = dependencies->begin(); it != dependencies->end(); ++it) {
    names.push_back(it.key());
  }
  return names;
}

Json script_entry_metadata(const char* source, const Path& entrypoint,
                           const std::string& discovery_kind,
                           const Json& package_metadata = Json::object(),
                           const std::string& declared_path = {}) {
  Json metadata = Json{
      {"source", source},
      {"discovery", discovery_kind},
      {"entrypoint_extension", entrypoint.extension().generic_string()},
  };

  if (!declared_path.empty()) {
    metadata["declared_entry"] = declared_path;
  }
  if (!package_metadata.empty()) {
    metadata["package"] = package_metadata;
  }
  return metadata;
}

void load_angelscript_extension_entry(
    const qt::harness::RepositoryView& repository, const std::string& root,
    const Path& entrypoint, const std::string& discovery_kind,
    const Json& package_metadata, const std::string& declared_path,
    ExtensionLoadResult& result) {
  const Path normalized_entrypoint = entrypoint.lexically_normal();
  if (!path_is_regular_file(normalized_entrypoint)) {
    result.add_diagnostic("angelscript extension entry is not a readable file: " +
                          path_to_string(normalized_entrypoint));
    return;
  }

  const std::string entrypoint_path = path_to_string(normalized_entrypoint);
  const auto source = repository.read_text_file(entrypoint_path);
  if (!source) {
    result.add_diagnostic("failed to read angelscript extension entry: " +
                          entrypoint_path);
    return;
  }

  ExtensionManifest manifest;
  manifest.name = extension_name(normalized_entrypoint);
  manifest.transport = ExtensionTransport::kAngelScript;
  manifest.entrypoint = entrypoint_path;
  manifest.working_directory =
      path_to_string(normalized_entrypoint.parent_path());
  manifest.metadata = script_entry_metadata(
      "angelscript", normalized_entrypoint, discovery_kind, package_metadata,
      declared_path);

  AngelScriptCompileRequest request;
  request.module_name = manifest.name;
  request.script_name = entrypoint_path;
  request.source = *source;
  request.seed_registration =
      adapt_manifest_to_angelscript_registration(manifest);

  AngelScriptHost host;
  std::shared_ptr<AngelScriptRuntime> script_runtime;
  AngelScriptPluginRegistration registration;
  if (host.dependency().available && host.dependency().linked) {
    auto created = AngelScriptRuntime::create(request);
    if (!created) {
      result.add_diagnostic("failed to compile angelscript extension entry: " +
                            entrypoint_path + " (" + created.error().message +
                            ")");
      return;
    }
    script_runtime = std::make_shared<AngelScriptRuntime>(
        std::move(created.value()));
    registration = script_runtime->registration();
  } else {
    const auto compiled = host.compile(request);
    if (!compiled) {
      result.add_diagnostic("failed to compile angelscript extension entry: " +
                            entrypoint_path + " (" + compiled.error().message +
                            ")");
      return;
    }
    for (const auto& diagnostic : compiled.value().diagnostics) {
      if (diagnostic.severity == AngelScriptDiagnosticSeverity::kError) {
        result.add_diagnostic("angelscript extension compile failed: " +
                              entrypoint_path + " (" + diagnostic.message + ")");
        return;
      }
    }
    registration = compiled.value().registration;
  }

  for (const auto& command : registration.commands) {
    manifest.commands.push_back(
        ExtensionCommandContribution{command.name, command.title,
                                     command.description});
  }
  for (const auto& tool : registration.tools) {
    manifest.tools.push_back(ExtensionToolContribution{
        tool.name, tool.title, tool.description});
  }
  for (const auto& provider : registration.providers) {
    manifest.providers.push_back(
        ExtensionProviderContribution{provider.name, provider.config});
  }
  for (const auto& flag : registration.flags) {
    manifest.flags.push_back(ExtensionFlagContribution{
        flag.name, flag.description, flag.type, flag.default_value});
  }
  if (!registration.shortcuts.empty()) {
    manifest.metadata["shortcuts"] = Json::array();
    for (const auto& shortcut : registration.shortcuts) {
      manifest.metadata["shortcuts"].push_back(
          Json{{"key", shortcut.key}, {"handler", shortcut.handler},
               {"description", shortcut.description.value_or("")}});
    }
  }
  if (!registration.lifecycle_handlers.empty()) {
    manifest.metadata["lifecycle_handlers"] = Json::array();
    for (const auto& handler : registration.lifecycle_handlers) {
      manifest.metadata["lifecycle_handlers"].push_back(
          Json{{"event", handler.event}, {"handler", handler.handler}});
    }
  }
  if (!registration.resources.empty()) {
    manifest.metadata["resources"] = Json::array();
    for (const auto& resource : registration.resources) {
      Json item{{"type", resource.type}, {"path", resource.path}};
      if (resource.name.has_value()) {
        item["name"] = *resource.name;
      }
      manifest.metadata["resources"].push_back(std::move(item));
    }
  }

  const auto validation = validate_manifest(manifest);
  if (!validation.ok()) {
    result.add_diagnostic("angelscript extension manifest failed validation: " +
                          entrypoint_path + " (" +
                          join_validation_issues(validation) + ")");
    return;
  }

  result.loaded_extensions.push_back(LoadedExtensionRecord{
      root,
      path_to_string(normalized_entrypoint.parent_path()),
      entrypoint_path,
      std::move(manifest),
      std::move(script_runtime),
  });
}

void load_extension_entry(const qt::harness::RepositoryView& repository,
                                    const std::string& root,
                                    const Path& entrypoint,
                                    const std::string& discovery_kind,
                                    const Json& package_metadata,
                                    const std::string& declared_path,
                                    ExtensionLoadResult& result) {
  const Path normalized_entrypoint = entrypoint.lexically_normal();
  if (!path_is_regular_file(normalized_entrypoint)) {
    result.add_diagnostic("reference extension entry is not a readable file: " +
                          path_to_string(normalized_entrypoint));
    return;
  }

  ExtensionManifest manifest;
  manifest.name = extension_name(normalized_entrypoint);
  manifest.transport = ExtensionTransport::kReferenceModule;
  manifest.entrypoint = path_to_string(normalized_entrypoint);
  manifest.working_directory =
      path_to_string(normalized_entrypoint.parent_path());
  manifest.metadata = script_entry_metadata(
      "reference_module", normalized_entrypoint, discovery_kind,
      package_metadata, declared_path);

  if (const auto source =
          repository.read_text_file(path_to_string(normalized_entrypoint))) {
    populate_module_registrations(manifest, *source);
    const auto api_diagnostics = find_not_implemented_extension_api_uses(
        *source, ExtensionApiSourceSyntax::kReferenceModule);
    append_not_implemented_api_metadata(manifest, api_diagnostics);
    append_not_implemented_api_load_diagnostics(
        result, path_to_string(normalized_entrypoint), api_diagnostics);
  } else {
    result.add_diagnostic("failed to read reference extension entry: " +
                          path_to_string(normalized_entrypoint));
    return;
  }

  const auto validation = validate_manifest(manifest);
  if (!validation.ok()) {
    result.add_diagnostic("reference extension manifest failed validation: " +
                          path_to_string(normalized_entrypoint) + " (" +
                          join_validation_issues(validation) + ")");
    return;
  }

  result.loaded_extensions.push_back(LoadedExtensionRecord{
      root,
      path_to_string(normalized_entrypoint.parent_path()),
      path_to_string(normalized_entrypoint),
      std::move(manifest),
      nullptr,
  });
}

void load_script_extension_entry(const qt::harness::RepositoryView& repository,
                                 const std::string& root,
                                 const Path& entrypoint,
                                 const std::string& discovery_kind,
                                 const Json& package_metadata,
                                 const std::string& declared_path,
                                 ExtensionLoadResult& result) {
  if (is_angelscript_extension_file(entrypoint)) {
    load_angelscript_extension_entry(repository, root, entrypoint,
                                     discovery_kind, package_metadata,
                                     declared_path, result);
    return;
  }
  load_extension_entry(repository, root, entrypoint, discovery_kind,
                                 package_metadata, declared_path, result);
}

void load_script_extension_entry(const qt::harness::RepositoryView& repository,
                                 const std::string& root,
                                 const Path& entrypoint,
                                 const std::string& discovery_kind,
                                 ExtensionLoadResult& result) {
  load_script_extension_entry(repository, root, entrypoint, discovery_kind,
                              Json::object(), {}, result);
}

PackageExtensionEntries resolve_package_extension_entries(
    const qt::harness::RepositoryView& repository, const Path& directory,
    ExtensionLoadResult& result) {
  const Path package_json = directory / "package.json";
  if (!path_is_regular_file(package_json)) {
    return {};
  }

  const std::string package_path = path_to_string(package_json);
  const auto content = repository.read_text_file(package_path);
  if (!content) {
    result.add_diagnostic("failed to read package manifest: " + package_path);
    return {};
  }

  std::vector<std::string> declared_entries;
  Json package_metadata = Json::object();
  try {
    const Json json = Json::parse(*content);
    const auto pi = json.find("pi");
    if (pi == json.end() || !pi->is_object()) {
      return {};
    }
    const auto entries = pi->find("extensions");
    if (entries == pi->end() || !entries->is_array()) {
      return {};
    }
    declared_entries = entries->get<std::vector<std::string>>();

    package_metadata["manifest"] = package_path;
    if (const auto name = optional_json_string(json, "name")) {
      package_metadata["name"] = *name;
    }
    if (const auto version = optional_json_string(json, "version")) {
      package_metadata["version"] = *version;
    }
    if (const auto type = optional_json_string(json, "type")) {
      package_metadata["type"] = *type;
    }
    const Json dependency_names = package_dependency_names(json);
    if (!dependency_names.empty()) {
      package_metadata["dependencies"] = dependency_names;
    }
  } catch (const std::exception& error) {
    result.add_diagnostic("failed to decode package manifest: " +
                          package_path + " (" + error.what() + ")");
    return {};
  }

  if (declared_entries.empty()) {
    return {};
  }

  PackageExtensionEntries resolved_entries;
  resolved_entries.declared = true;
  resolved_entries.declared_paths = declared_entries;
  resolved_entries.package_metadata = std::move(package_metadata);
  std::vector<Path> entries;
  for (const auto& declared : declared_entries) {
    if (is_override_pattern(declared)) {
      continue;
    }
    const Path resolved = Path(declared).is_absolute()
                              ? Path(declared)
                              : directory / Path(declared);
    if (path_is_directory(resolved)) {
      collect_script_extension_entries(resolved, entries);
    } else if (path_is_regular_file(resolved) &&
               is_script_extension_file(resolved)) {
      entries.push_back(resolved);
    } else {
      result.add_diagnostic("package extension entry does not exist: " +
                            path_to_string(resolved));
    }
  }

  std::vector<std::string> patterns;
  std::copy_if(declared_entries.begin(), declared_entries.end(),
               std::back_inserter(patterns), is_override_pattern);
  resolved_entries.entries =
      apply_filter_patterns(std::move(entries), patterns, directory);
  return resolved_entries;
}

std::string declared_path_for_entry(const Path& package_directory,
                                    const Path& entry,
                                    const std::vector<std::string>& declared) {
  const Path normalized_entry = entry.lexically_normal();
  for (const auto& path : declared) {
    if (is_override_pattern(path)) {
      continue;
    }
    const Path candidate = (Path(path).is_absolute()
                                ? Path(path)
                                : package_directory / Path(path))
                               .lexically_normal();
    if (candidate == normalized_entry) {
      return path;
    }
  }
  return {};
}

bool load_script_extension_directory(
    const qt::harness::RepositoryView& repository, const std::string& root,
    const Path& directory, ExtensionLoadResult& result) {
  const auto package_entries =
      resolve_package_extension_entries(repository, directory, result);
  if (package_entries.declared) {
    for (const auto& entry : package_entries.entries) {
      load_script_extension_entry(
          repository, root, entry, "package_manifest",
          package_entries.package_metadata,
          declared_path_for_entry(directory, entry,
                                  package_entries.declared_paths),
          result);
    }
    return true;
  }

  const Path index_ts = directory / "index.ts";
  const Path index_js = directory / "index.js";
  const Path index_as = directory / "index.as";
  if (path_is_regular_file(index_ts)) {
    load_script_extension_entry(repository, root, index_ts, "index", result);
    return true;
  }
  if (path_is_regular_file(index_js)) {
    load_script_extension_entry(repository, root, index_js, "index", result);
    return true;
  }
  if (path_is_regular_file(index_as)) {
    load_script_extension_entry(repository, root, index_as, "index", result);
    return true;
  }

  return false;
}

void load_extension_directory(const qt::harness::RepositoryView& repository,
                              const std::string& root, const Path& directory,
                              ExtensionLoadResult& result) {
  const Path manifest_file = directory / kExtensionManifestFileName;
  if (path_exists(manifest_file)) {
    const std::string manifest_path = path_to_string(manifest_file);
    if (!path_is_regular_file(manifest_file)) {
      result.add_diagnostic("extension manifest is not a readable file: " +
                            manifest_path);
      return;
    }

    const auto content = repository.read_text_file(manifest_path);
    if (!content) {
      result.add_diagnostic("failed to read extension manifest: " +
                            manifest_path);
      return;
    }

    const auto decoded = decode_extension_manifest_json(*content);
    if (!decoded) {
      result.add_diagnostic("failed to decode extension manifest: " +
                            manifest_path + " (" + decoded.error().message +
                            ")");
      return;
    }

    const auto validation = validate_manifest(decoded.value());
    if (!validation.ok()) {
      result.add_diagnostic("extension manifest failed validation: " +
                            manifest_path + " (" +
                            join_validation_issues(validation) + ")");
      return;
    }

    result.loaded_extensions.push_back(LoadedExtensionRecord{
        root,
        path_to_string(directory),
        manifest_path,
        decoded.value(),
        nullptr,
    });
    return;
  }

  if (load_script_extension_directory(repository, root, directory, result)) {
    return;
  }

  for (const auto& entry : list_directory_entries(directory)) {
    if (path_is_regular_file(entry) && is_script_extension_file(entry)) {
      load_script_extension_entry(repository, root, entry, "direct_file", result);
    }
  }

  for (const auto& child : list_subdirectories(directory)) {
    load_extension_directory(repository, root, child, result);
  }
}

}  // namespace

ExtensionLoadResult load_extension_manifests(
    const qt::harness::RepositoryView& repository,
    const std::vector<std::string>& extension_roots) {
  return load_extension_manifests(repository, extension_roots,
                                  ExtensionLoadOptions{});
}

ExtensionLoadResult load_extension_manifests(
    const qt::harness::RepositoryView& repository,
    const std::vector<std::string>& extension_roots,
    const ExtensionLoadOptions& options) {
  ExtensionLoadResult result;
  for (const auto& root : extension_roots) {
    if (root.empty()) {
      result.roots.push_back(root);
      result.add_diagnostic("extension root was empty");
      continue;
    }

    const Path root_path = normalize_extension_root_path(root);
    const std::string root_display = path_to_string(root_path);
    result.roots.push_back(root_display);
    if (path_is_regular_file(root_path) && is_script_extension_file(root_path)) {
      load_script_extension_entry(repository,
                                  path_to_string(root_path.parent_path()),
                                  root_path, "explicit_file", result);
      continue;
    }
    if (!path_exists(root_path)) {
      result.add_diagnostic("extension root does not exist: " + root_display);
      continue;
    }
    if (!path_is_directory(root_path)) {
      result.add_diagnostic("extension root is not a directory: " + root_display);
      continue;
    }

    load_extension_directory(repository, root_display, root_path, result);
  }

  if (!options.filter_patterns.empty()) {
    result.loaded_extensions.erase(
        std::remove_if(result.loaded_extensions.begin(),
                       result.loaded_extensions.end(),
                       [&](const LoadedExtensionRecord& record) {
                         return !extension_record_enabled(
                             record, options.filter_patterns);
                       }),
        result.loaded_extensions.end());
  }

  return result;
}

}  // namespace qt::extensions
