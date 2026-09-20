#include <qt/app/config/settings.hpp>

#include <qt/foundation/config.hpp>
#include <qt/foundation/filesystem.hpp>
#include <qt/harness/context_discovery.hpp>
#include <qt/harness/repository.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qt::app {
namespace {

using Json = nlohmann::json;
using qt::foundation::ErrorCode;
using qt::foundation::Result;
using Path = std::filesystem::path;

constexpr const char* kSettingsFilename = "settings.json";
constexpr const char* kYamlSettingsFilename = "settings.yaml";
constexpr const char* kYmlSettingsFilename = "settings.yml";

std::string path_to_string(const Path& path) { return path.generic_string(); }

std::string safe_pretty_json_dump(const Json& value) {
  return value.dump(2, ' ', false, Json::error_handler_t::replace);
}

std::vector<std::string> default_tool_entry_compaction_enabled_tools() {
  return {
      "append_text",
      "apply_patch",
      "bash",
      "clipboard",
      "complete_path",
      "edit_text",
      "find",
      "list_directory",
      "list_mcp_resources",
      "package_manager",
      "search_text",
      "web_fetch",
      "web_search",
      "web_run",
      "write_text",
  };
}

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::optional<std::string> getenv_string(const char* name) {
  const char* value = std::getenv(name);
  if (!value || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
}

std::string expand_user_path(const std::string& path) {
  if (path.empty() || path[0] != '~') {
    return path;
  }

  const auto home = getenv_string("HOME");
  if (!home) {
    return path;
  }

  if (path == "~") {
    return *home;
  }
  if (path.size() >= 2 && path[1] == '/') {
    return *home + path.substr(1);
  }

  return path;
}

std::filesystem::path resolve_persisted_model_path(const std::string& cwd,
                                                   const std::string& path) {
  std::filesystem::path resolved = expand_user_path(path);
  if (resolved.is_relative()) {
    resolved = std::filesystem::path(cwd) / resolved;
  }
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(resolved, ec);
  return ec ? resolved.lexically_normal() : canonical;
}

std::string normalized_model_path_key(const std::string& settings_directory,
                                      const std::string& path) {
  return lowercase_ascii(
      resolve_persisted_model_path(settings_directory, path).generic_string());
}

std::string unique_model_id(std::string id, std::set<std::string>& used_ids) {
  if (id.empty()) {
    id = "local-model";
  }

  const std::string base = id;
  std::string candidate = base;
  int suffix = 2;
  while (!used_ids.insert(lowercase_ascii(candidate)).second) {
    candidate = base + "-" + std::to_string(suffix++);
  }
  return candidate;
}

Result<Json> read_settings_json_for_update(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return Json::object();
  }
  const auto content = qt::foundation::read_text_file(path);
  if (!content) {
    return Result<Json>::failure(content.error().code, content.error().message,
                                 content.error().system_error);
  }
  try {
    Json json = Json::parse(content.value());
    if (!json.is_object()) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "settings document must be a JSON object");
    }
    return json;
  } catch (const Json::parse_error& error) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        "invalid settings JSON in " + path.generic_string() + ": " + error.what());
  }
}

Result<std::string> canonicalize_setting_path(const qt::harness::RepositoryView& repository,
                                              const std::string& value,
                                              const std::string& base_directory,
                                              const std::string& field_name) {
  if (value.empty()) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument, field_name + " must not be empty");
  }

  Path path = Path(expand_user_path(value));
  if (path.is_relative()) {
    path = Path(base_directory) / path;
  }

  const auto canonical = repository.canonicalize_path(path_to_string(path));
  if (!canonical) {
    return Result<std::string>::failure(
        ErrorCode::invalid_argument,
        field_name + " could not be canonicalized: " + path_to_string(path));
  }
  return *canonical;
}

bool is_resource_filter_entry(const std::string& value) {
  return !value.empty() &&
         (value.front() == '!' || value.front() == '+' || value.front() == '-');
}

Result<std::vector<std::string>> parse_resource_path_array(
    const qt::harness::RepositoryView& repository,
    const Json& json,
    const char* field_name,
    const std::string& base_directory,
    std::optional<std::vector<std::string>>& filter_target) {
  if (!json.is_array()) {
    return Result<std::vector<std::string>>::failure(
        ErrorCode::invalid_argument, std::string(field_name) + " must be an array");
  }

  std::vector<std::string> paths;
  std::vector<std::string> filters;
  for (std::size_t index = 0; index < json.size(); ++index) {
    if (!json[index].is_string()) {
      return Result<std::vector<std::string>>::failure(
          ErrorCode::invalid_argument, std::string(field_name) +
                                           "[" + std::to_string(index) +
                                           "] must be a string");
    }

    const std::string value = json[index].get<std::string>();
    if (is_resource_filter_entry(value)) {
      filters.push_back(value);
      continue;
    }

    const auto resolved = canonicalize_setting_path(
        repository, value, base_directory, field_name);
    if (!resolved) {
      return resolved.error();
    }

    if (std::find(paths.begin(), paths.end(), resolved.value()) == paths.end()) {
      paths.push_back(resolved.value());
    }
  }

  filter_target = std::move(filters);
  return paths;
}

Result<std::vector<std::string>> parse_string_array(const Json& json,
                                                    const char* field_name) {
  if (!json.is_array()) {
    return Result<std::vector<std::string>>::failure(
        ErrorCode::invalid_argument, std::string(field_name) + " must be an array");
  }

  std::vector<std::string> values;
  values.reserve(json.size());
  for (std::size_t index = 0; index < json.size(); ++index) {
    if (!json[index].is_string()) {
      return Result<std::vector<std::string>>::failure(
          ErrorCode::invalid_argument, std::string(field_name) +
                                           "[" + std::to_string(index) +
                                           "] must be a string");
    }
    values.push_back(json[index].get<std::string>());
  }
  return values;
}

Result<std::map<std::string, std::string>> parse_string_map(
    const Json& json, const std::string& field_name) {
  if (!json.is_object()) {
    return Result<std::map<std::string, std::string>>::failure(
        ErrorCode::invalid_argument, field_name + " must be an object");
  }
  std::map<std::string, std::string> values;
  for (const auto& item : json.items()) {
    if (!item.value().is_string()) {
      return Result<std::map<std::string, std::string>>::failure(
          ErrorCode::invalid_argument,
          field_name + "." + item.key() + " must be a string");
    }
    values[item.key()] = item.value().get<std::string>();
  }
  return values;
}

Result<int> parse_int_value(const Json& json, const std::string& field_name);

Result<std::vector<McpServerSettings>> parse_mcp_servers(
    const Json& json, const std::string& field_name) {
  if (!json.is_object()) {
    return Result<std::vector<McpServerSettings>>::failure(
        ErrorCode::invalid_argument, field_name + " must be an object");
  }

  std::vector<McpServerSettings> servers;
  for (const auto& item : json.items()) {
    if (!item.value().is_object()) {
      return Result<std::vector<McpServerSettings>>::failure(
          ErrorCode::invalid_argument,
          field_name + "." + item.key() + " must be an object");
    }
    const Json& value = item.value();
    McpServerSettings server;
    server.name = item.key();
    if (const auto enabled = value.find("enabled"); enabled != value.end()) {
      if (!enabled->is_boolean()) {
        return Result<std::vector<McpServerSettings>>::failure(
            ErrorCode::invalid_argument,
            field_name + "." + item.key() + ".enabled must be a boolean");
      }
      server.enabled = enabled->get<bool>();
    }
    if (const auto command = value.find("command"); command != value.end()) {
      if (!command->is_string()) {
        return Result<std::vector<McpServerSettings>>::failure(
            ErrorCode::invalid_argument,
            field_name + "." + item.key() + ".command must be a string");
      }
      server.command = command->get<std::string>();
      server.transport = "stdio";
    }
    if (const auto url = value.find("url"); url != value.end()) {
      if (!url->is_string()) {
        return Result<std::vector<McpServerSettings>>::failure(
            ErrorCode::invalid_argument,
            field_name + "." + item.key() + ".url must be a string");
      }
      server.url = url->get<std::string>();
      if (!server.command) {
        server.transport = "streamable-http";
      }
    }
    if (const auto transport = value.find("transport");
        transport != value.end()) {
      if (!transport->is_string()) {
        return Result<std::vector<McpServerSettings>>::failure(
            ErrorCode::invalid_argument,
            field_name + "." + item.key() + ".transport must be a string");
      }
      server.transport = transport->get<std::string>();
    }
    if (const auto args = value.find("args"); args != value.end()) {
      const auto parsed = parse_string_array(*args,
          (field_name + "." + item.key() + ".args").c_str());
      if (!parsed) {
        return Result<std::vector<McpServerSettings>>::failure(
            parsed.error().code, parsed.error().message);
      }
      server.args = parsed.value();
    }
    if (const auto env = value.find("env"); env != value.end()) {
      const auto parsed =
          parse_string_map(*env, field_name + "." + item.key() + ".env");
      if (!parsed) {
        return Result<std::vector<McpServerSettings>>::failure(
            parsed.error().code, parsed.error().message);
      }
      server.env = parsed.value();
    }
    if (const auto headers = value.find("headers"); headers != value.end()) {
      const auto parsed =
          parse_string_map(*headers, field_name + "." + item.key() + ".headers");
      if (!parsed) {
        return Result<std::vector<McpServerSettings>>::failure(
            parsed.error().code, parsed.error().message);
      }
      server.headers = parsed.value();
    }
    if (const auto sampling = value.find("sampling"); sampling != value.end()) {
      if (!sampling->is_object()) {
        return Result<std::vector<McpServerSettings>>::failure(
            ErrorCode::invalid_argument,
            field_name + "." + item.key() + ".sampling must be an object");
      }
      server.sampling = *sampling;
    }
    if (const auto timeout = value.find("timeout"); timeout != value.end()) {
      const auto parsed =
          parse_int_value(*timeout, field_name + "." + item.key() + ".timeout");
      if (!parsed) {
        return Result<std::vector<McpServerSettings>>::failure(
            parsed.error().code, parsed.error().message);
      }
      server.timeout_seconds = std::max(1, parsed.value());
    }
    if (const auto timeout = value.find("timeoutSeconds");
        timeout != value.end()) {
      const auto parsed = parse_int_value(
          *timeout, field_name + "." + item.key() + ".timeoutSeconds");
      if (!parsed) {
        return Result<std::vector<McpServerSettings>>::failure(
            parsed.error().code, parsed.error().message);
      }
      server.timeout_seconds = std::max(1, parsed.value());
    }
    if (const auto timeout = value.find("connectTimeoutSeconds");
        timeout != value.end()) {
      const auto parsed = parse_int_value(
          *timeout, field_name + "." + item.key() + ".connectTimeoutSeconds");
      if (!parsed) {
        return Result<std::vector<McpServerSettings>>::failure(
            parsed.error().code, parsed.error().message);
      }
      server.connect_timeout_seconds = std::max(1, parsed.value());
    }
    servers.push_back(std::move(server));
  }
  return servers;
}

bool valid_ollama_instance_id(const std::string& value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) {
           return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
         });
}

Result<std::vector<ai::OllamaInstanceSettings>> parse_ollama_instances(
    const Json& json, const std::string& field_name) {
  if (!json.is_object() || !json.contains("instances") ||
      !json.at("instances").is_array()) {
    return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
        ErrorCode::invalid_argument, field_name + ".instances must be an array");
  }
  std::vector<ai::OllamaInstanceSettings> instances;
  std::set<std::string> ids;
  for (std::size_t index = 0; index < json.at("instances").size(); ++index) {
    const auto& value = json.at("instances").at(index);
    const std::string prefix = field_name + ".instances[" +
                               std::to_string(index) + "]";
    if (!value.is_object() || !value.contains("id") ||
        !value.at("id").is_string()) {
      return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
          ErrorCode::invalid_argument, prefix + ".id must be a string");
    }
    ai::OllamaInstanceSettings instance;
    instance.id = value.at("id").get<std::string>();
    if (!valid_ollama_instance_id(instance.id)) {
      return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
          ErrorCode::invalid_argument,
          prefix + ".id must contain only letters, numbers, '-' or '_'");
    }
    std::string normalized_id = instance.id;
    std::transform(normalized_id.begin(), normalized_id.end(),
                   normalized_id.begin(), [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
    if (!ids.insert(normalized_id).second) {
      return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
          ErrorCode::invalid_argument,
          field_name + ".instances contains duplicate id: " + instance.id);
    }
    instance.name = value.value("name", instance.id);
    instance.base_url = ai::normalize_ollama_base_url(
        value.value("baseUrl", std::string("http://localhost:11434")));
    if (instance.base_url.rfind("http://", 0U) != 0U &&
        instance.base_url.rfind("https://", 0U) != 0U) {
      return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
          ErrorCode::invalid_argument,
          prefix + ".baseUrl must start with http:// or https://");
    }
    if (value.contains("apiKey")) {
      if (!value.at("apiKey").is_string()) {
        return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
            ErrorCode::invalid_argument, prefix + ".apiKey must be a string");
      }
      const auto api_key = value.at("apiKey").get<std::string>();
      if (!api_key.empty()) {
        instance.api_key = api_key;
      }
    }
    if (value.contains("enabled")) {
      if (!value.at("enabled").is_boolean()) {
        return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
            ErrorCode::invalid_argument, prefix + ".enabled must be a boolean");
      }
      instance.enabled = value.at("enabled").get<bool>();
    }
    if (value.contains("timeoutMs")) {
      if (!value.at("timeoutMs").is_number_integer()) {
        return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
            ErrorCode::invalid_argument, prefix + ".timeoutMs must be an integer");
      }
      instance.timeout_ms =
          std::max<std::int64_t>(1, value.at("timeoutMs").get<std::int64_t>());
    }
    if (value.contains("models")) {
      if (!value.at("models").is_array()) {
        return Result<std::vector<ai::OllamaInstanceSettings>>::failure(
            ErrorCode::invalid_argument, prefix + ".models must be an array");
      }
      for (const auto& cached : value.at("models")) {
        if (!cached.is_object() || !cached.contains("name") ||
            !cached.at("name").is_string()) {
          continue;
        }
        ai::OllamaModelInfo model;
        model.name = cached.at("name").get<std::string>();
        model.modified_at = cached.value("modifiedAt", std::string{});
        model.digest = cached.value("digest", std::string{});
        model.size = cached.value("size", std::uint64_t{0});
        model.context_window = cached.value(
            "contextWindow", ai::kDefaultOllamaContextWindow);
        model.max_tokens = cached.value(
            "maxTokens", std::min(model.context_window,
                                   ai::kDefaultOllamaMaxTokens));
        model.details = cached.value("details", Json::object());
        if (cached.contains("capabilities") &&
            cached.at("capabilities").is_array()) {
          for (const auto& capability : cached.at("capabilities")) {
            if (capability.is_string()) {
              model.capabilities.push_back(capability.get<std::string>());
            }
          }
        }
        instance.models.push_back(std::move(model));
      }
    }
    instances.push_back(std::move(instance));
  }
  return Result<std::vector<ai::OllamaInstanceSettings>>::success(
      std::move(instances));
}

Result<bool> parse_boolean_value(const Json& json,
                                 const std::string& field_name) {
  if (!json.is_boolean()) {
    return Result<bool>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be a boolean");
  }
  return json.get<bool>();
}

Result<int> parse_int_value(const Json& json, const std::string& field_name) {
  if (!json.is_number_integer()) {
    return Result<int>::failure(ErrorCode::invalid_argument,
                                field_name + " must be an integer");
  }
  const auto value = json.get<std::int64_t>();
  if (value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return Result<int>::failure(ErrorCode::invalid_argument,
                                field_name + " is out of range");
  }
  return static_cast<int>(value);
}

Result<std::int64_t> parse_int64_value(const Json& json,
                                       const std::string& field_name) {
  if (!json.is_number_integer() && !json.is_number_unsigned()) {
    return Result<std::int64_t>::failure(ErrorCode::invalid_argument,
                                         field_name + " must be an integer");
  }
  if (json.is_number_unsigned()) {
    const auto value = json.get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
      return Result<std::int64_t>::failure(ErrorCode::invalid_argument,
                                           field_name + " is out of range");
    }
    return static_cast<std::int64_t>(value);
  }
  return json.get<std::int64_t>();
}

Result<ProviderRetrySettings> parse_provider_retry_settings(
    const Json& json, const std::string& field_name) {
  if (!json.is_object()) {
    return Result<ProviderRetrySettings>::failure(
        ErrorCode::invalid_argument, field_name + " must be an object");
  }

  ProviderRetrySettings settings;
  if (const auto timeout = json.find("timeoutMs"); timeout != json.end()) {
    const auto parsed = parse_int64_value(*timeout, field_name + ".timeoutMs");
    if (!parsed) {
      return Result<ProviderRetrySettings>::failure(
          parsed.error().code, parsed.error().message,
          parsed.error().system_error);
    }
    if (parsed.value() < 0) {
      return Result<ProviderRetrySettings>::failure(
          ErrorCode::invalid_argument,
          field_name + ".timeoutMs must be non-negative");
    }
    settings.timeout_ms = parsed.value();
  }
  if (const auto max_retries = json.find("maxRetries");
      max_retries != json.end()) {
    const auto parsed = parse_int_value(*max_retries,
                                        field_name + ".maxRetries");
    if (!parsed) {
      return Result<ProviderRetrySettings>::failure(
          parsed.error().code, parsed.error().message,
          parsed.error().system_error);
    }
    if (parsed.value() < 0) {
      return Result<ProviderRetrySettings>::failure(
          ErrorCode::invalid_argument,
          field_name + ".maxRetries must be non-negative");
    }
    settings.max_retries = parsed.value();
  }
  if (const auto max_retry_delay = json.find("maxRetryDelayMs");
      max_retry_delay != json.end()) {
    const auto parsed = parse_int64_value(*max_retry_delay,
                                          field_name + ".maxRetryDelayMs");
    if (!parsed) {
      return Result<ProviderRetrySettings>::failure(
          parsed.error().code, parsed.error().message,
          parsed.error().system_error);
    }
    if (parsed.value() < 0) {
      return Result<ProviderRetrySettings>::failure(
          ErrorCode::invalid_argument,
          field_name + ".maxRetryDelayMs must be non-negative");
    }
    settings.max_retry_delay_ms = parsed.value();
  }
  return settings;
}

Json provider_retry_settings_to_json(const ProviderRetrySettings& settings) {
  Json json = Json::object();
  if (settings.timeout_ms) {
    json["timeoutMs"] = *settings.timeout_ms;
  }
  if (settings.max_retries) {
    json["maxRetries"] = *settings.max_retries;
  }
  if (settings.max_retry_delay_ms) {
    json["maxRetryDelayMs"] = *settings.max_retry_delay_ms;
  }
  return json;
}

Result<std::uint32_t> parse_uint32_value(const Json& json,
                                         const std::string& field_name) {
  if (!json.is_number_integer() && !json.is_number_unsigned()) {
    return Result<std::uint32_t>::failure(
        ErrorCode::invalid_argument, field_name + " must be an integer");
  }
  if (json.is_number_unsigned()) {
    const auto value = json.get<std::uint64_t>();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      return Result<std::uint32_t>::failure(ErrorCode::invalid_argument,
                                            field_name + " is out of range");
    }
    return static_cast<std::uint32_t>(value);
  }
  const auto signed_value = json.get<std::int64_t>();
  if (signed_value < 0 ||
      signed_value > std::numeric_limits<std::uint32_t>::max()) {
    return Result<std::uint32_t>::failure(ErrorCode::invalid_argument,
                                          field_name + " is out of range");
  }
  return static_cast<std::uint32_t>(signed_value);
}

Result<double> parse_double_value(const Json& json,
                                  const std::string& field_name) {
  if (!json.is_number()) {
    return Result<double>::failure(ErrorCode::invalid_argument,
                                   field_name + " must be a number");
  }
  return json.get<double>();
}

Result<std::string> parse_string_value(const Json& json,
                                       const std::string& field_name) {
  if (!json.is_string()) {
    return Result<std::string>::failure(ErrorCode::invalid_argument,
                                        field_name + " must be a string");
  }
  return json.get<std::string>();
}

Result<MemorySettingsPatch> parse_memory_settings_patch(
    const Json& json, const std::string& field_name) {
  if (!json.is_object()) {
    return Result<MemorySettingsPatch>::failure(
        ErrorCode::invalid_argument, field_name + " must be an object");
  }

  MemorySettingsPatch patch;
  const auto parse_bool = [&](const char* key, std::optional<bool>& target)
      -> Result<bool> {
    const auto it = json.find(key);
    if (it == json.end()) {
      return false;
    }
    const auto parsed =
        parse_boolean_value(*it, field_name + "." + std::string(key));
    if (!parsed) {
      return parsed.error();
    }
    target = parsed.value();
    return true;
  };
  const auto parse_string = [&](const Json& object, const char* key,
                                const std::string& prefix,
                                std::optional<std::string>& target)
      -> Result<bool> {
    const auto it = object.find(key);
    if (it == object.end()) {
      return false;
    }
    const auto parsed = parse_string_value(*it, prefix + "." + key);
    if (!parsed) {
      return parsed.error();
    }
    target = parsed.value();
    return true;
  };
  const auto parse_size = [&](const Json& object, const char* key,
                              const std::string& prefix,
                              std::optional<std::size_t>& target)
      -> Result<bool> {
    const auto it = object.find(key);
    if (it == object.end()) {
      return false;
    }
    const auto parsed = parse_uint32_value(*it, prefix + "." + key);
    if (!parsed) {
      return parsed.error();
    }
    target = static_cast<std::size_t>(parsed.value());
    return true;
  };

  for (const auto& result :
       {parse_bool("enabled", patch.enabled),
        parse_bool("automaticRecall", patch.automatic_recall),
        parse_bool("automaticCapture", patch.automatic_capture)}) {
    if (!result) {
      return result.error();
    }
  }
  {
    const auto parsed =
        parse_string(json, "recallPolicy", field_name, patch.recall_policy);
    if (!parsed) {
      return parsed.error();
    }
  }
  for (const auto& result :
       {parse_size(json, "contextTokenBudget", field_name,
                   patch.context_token_budget),
        parse_size(json, "candidateLimit", field_name,
                   patch.candidate_limit)}) {
    if (!result) {
      return result.error();
    }
  }

  if (const auto retrieval = json.find("retrieval");
      retrieval != json.end()) {
    if (!retrieval->is_object()) {
      return Result<MemorySettingsPatch>::failure(
          ErrorCode::invalid_argument,
          field_name + ".retrieval must be an object");
    }
    const auto parse_retrieval_bool = [&](const char* key,
                                          std::optional<bool>& target)
        -> Result<bool> {
      const auto it = retrieval->find(key);
      if (it == retrieval->end()) return false;
      const auto parsed = parse_boolean_value(
          *it, field_name + ".retrieval." + std::string(key));
      if (!parsed) return parsed.error();
      target = parsed.value();
      return true;
    };
    for (const auto& result :
         {parse_retrieval_bool("graphEnabled", patch.retrieval.graph_enabled),
          parse_retrieval_bool("denseEnabled", patch.retrieval.dense_enabled),
          parse_retrieval_bool("codeEnabled", patch.retrieval.code_enabled)}) {
      if (!result) return result.error();
    }
    if (const auto model = retrieval->find("embeddingModel");
        model != retrieval->end()) {
      if (model->is_null()) {
        patch.retrieval.embedding_model = std::optional<std::string>{};
      } else {
        const auto parsed = parse_string_value(
            *model, field_name + ".retrieval.embeddingModel");
        if (!parsed) return parsed.error();
        patch.retrieval.embedding_model = parsed.value();
      }
    }
    for (const auto& result :
         {parse_string(*retrieval, "queryPrefix", field_name + ".retrieval",
                       patch.retrieval.query_prefix),
          parse_string(*retrieval, "documentPrefix",
                       field_name + ".retrieval",
                       patch.retrieval.document_prefix),
          parse_size(*retrieval, "maxDepth", field_name + ".retrieval",
                     patch.retrieval.max_depth),
          parse_size(*retrieval, "maxSeeds", field_name + ".retrieval",
                     patch.retrieval.max_seeds),
          parse_size(*retrieval, "maxEdges", field_name + ".retrieval",
                     patch.retrieval.max_edges),
          parse_size(*retrieval, "maxGraphResults", field_name + ".retrieval",
                     patch.retrieval.max_graph_results),
          parse_size(*retrieval, "maxFanout", field_name + ".retrieval",
                     patch.retrieval.max_fanout),
          parse_size(*retrieval, "queryCacheEntries",
                     field_name + ".retrieval",
                     patch.retrieval.query_cache_entries)}) {
      if (!result) return result.error();
    }
  }

  if (const auto prehandler = json.find("prehandler");
      prehandler != json.end()) {
    if (!prehandler->is_object()) {
      return Result<MemorySettingsPatch>::failure(
          ErrorCode::invalid_argument,
          field_name + ".prehandler must be an object");
    }
    if (const auto model = prehandler->find("model");
        model != prehandler->end()) {
      if (model->is_null()) {
        patch.prehandler.model = std::optional<std::string>{};
      } else {
        const auto parsed =
            parse_string_value(*model, field_name + ".prehandler.model");
        if (!parsed) {
          return parsed.error();
        }
        patch.prehandler.model = parsed.value();
      }
    }
    {
      const auto parsed = parse_string(*prehandler, "thinking",
                                       field_name + ".prehandler",
                                       patch.prehandler.thinking);
      if (!parsed) {
        return parsed.error();
      }
    }
    if (const auto timeout = prehandler->find("timeoutMs");
        timeout != prehandler->end()) {
      const auto parsed = parse_int64_value(
          *timeout, field_name + ".prehandler.timeoutMs");
      if (!parsed) {
        return parsed.error();
      }
      patch.prehandler.timeout_ms = parsed.value();
    }
    if (const auto fail_open = prehandler->find("failOpen");
        fail_open != prehandler->end()) {
      const auto parsed = parse_boolean_value(
          *fail_open, field_name + ".prehandler.failOpen");
      if (!parsed) {
        return parsed.error();
      }
      patch.prehandler.fail_open = parsed.value();
    }
    {
      const auto parsed = parse_size(
          *prehandler, "maxModelInputTokens", field_name + ".prehandler",
          patch.prehandler.max_model_input_tokens);
      if (!parsed) {
        return parsed.error();
      }
    }
  }

  if (const auto capture = json.find("capture"); capture != json.end()) {
    if (!capture->is_object()) {
      return Result<MemorySettingsPatch>::failure(
          ErrorCode::invalid_argument, field_name + ".capture must be an object");
    }
    for (const auto& result :
         {parse_string(*capture, "postProcess", field_name + ".capture",
                       patch.capture.post_process),
          parse_string(*capture, "defaultStatus", field_name + ".capture",
                       patch.capture.default_status),
          parse_size(*capture, "maxEvidenceChars", field_name + ".capture",
                     patch.capture.max_evidence_chars)}) {
      if (!result) {
        return result.error();
      }
    }
  }

  if (const auto dreaming = json.find("dreaming"); dreaming != json.end()) {
    if (!dreaming->is_object()) {
      return Result<MemorySettingsPatch>::failure(
          ErrorCode::invalid_argument,
          field_name + ".dreaming must be an object");
    }
    if (const auto enabled = dreaming->find("enabled");
        enabled != dreaming->end()) {
      const auto parsed = parse_boolean_value(
          *enabled, field_name + ".dreaming.enabled");
      if (!parsed) {
        return parsed.error();
      }
      patch.dreaming.enabled = parsed.value();
    }
    for (const auto& result :
         {parse_string(*dreaming, "trigger", field_name + ".dreaming",
                       patch.dreaming.trigger),
          parse_string(*dreaming, "researchPolicy",
                       field_name + ".dreaming",
                       patch.dreaming.research_policy),
          parse_string(*dreaming, "promotionPolicy",
                       field_name + ".dreaming",
                       patch.dreaming.promotion_policy),
          parse_size(*dreaming, "candidateThreshold",
                     field_name + ".dreaming",
                     patch.dreaming.candidate_threshold),
          parse_size(*dreaming, "maxModelInputTokens",
                     field_name + ".dreaming",
                     patch.dreaming.max_model_input_tokens),
          parse_size(*dreaming, "maxCandidates", field_name + ".dreaming",
                     patch.dreaming.max_candidates)}) {
      if (!result) {
        return result.error();
      }
    }
    if (const auto interval = dreaming->find("intervalSeconds");
        interval != dreaming->end()) {
      const auto parsed = parse_uint32_value(
          *interval, field_name + ".dreaming.intervalSeconds");
      if (!parsed) {
        return parsed.error();
      }
      patch.dreaming.interval_seconds = parsed.value();
    }
    if (const auto idle = dreaming->find("idleSeconds");
        idle != dreaming->end()) {
      const auto parsed = parse_uint32_value(
          *idle, field_name + ".dreaming.idleSeconds");
      if (!parsed) {
        return parsed.error();
      }
      patch.dreaming.idle_seconds = parsed.value();
    }
    if (const auto model = dreaming->find("model"); model != dreaming->end()) {
      if (model->is_null()) {
        patch.dreaming.model = std::optional<std::string>{};
      } else {
        const auto parsed =
            parse_string_value(*model, field_name + ".dreaming.model");
        if (!parsed) {
          return parsed.error();
        }
        patch.dreaming.model = parsed.value();
      }
    }
    if (const auto verifier_model = dreaming->find("verifierModel");
        verifier_model != dreaming->end()) {
      if (verifier_model->is_null()) {
        patch.dreaming.verifier_model = std::optional<std::string>{};
      } else {
        const auto parsed = parse_string_value(
            *verifier_model, field_name + ".dreaming.verifierModel");
        if (!parsed) {
          return parsed.error();
        }
        patch.dreaming.verifier_model = parsed.value();
      }
    }
    if (const auto timeout = dreaming->find("timeoutMs");
        timeout != dreaming->end()) {
      const auto parsed =
          parse_int64_value(*timeout, field_name + ".dreaming.timeoutMs");
      if (!parsed) {
        return parsed.error();
      }
      patch.dreaming.timeout_ms = parsed.value();
    }
  }

  return patch;
}

Json memory_settings_patch_to_json(const MemorySettingsPatch& patch) {
  Json json = Json::object();
  if (patch.enabled) {
    json["enabled"] = *patch.enabled;
  }
  if (patch.automatic_recall) {
    json["automaticRecall"] = *patch.automatic_recall;
  }
  if (patch.automatic_capture) {
    json["automaticCapture"] = *patch.automatic_capture;
  }
  if (patch.recall_policy) {
    json["recallPolicy"] = *patch.recall_policy;
  }
  if (patch.context_token_budget) {
    json["contextTokenBudget"] = *patch.context_token_budget;
  }
  if (patch.candidate_limit) {
    json["candidateLimit"] = *patch.candidate_limit;
  }
  Json retrieval = Json::object();
  if (patch.retrieval.graph_enabled) retrieval["graphEnabled"] = *patch.retrieval.graph_enabled;
  if (patch.retrieval.dense_enabled) retrieval["denseEnabled"] = *patch.retrieval.dense_enabled;
  if (patch.retrieval.code_enabled) retrieval["codeEnabled"] = *patch.retrieval.code_enabled;
  if (patch.retrieval.embedding_model) {
    retrieval["embeddingModel"] = *patch.retrieval.embedding_model
        ? Json(**patch.retrieval.embedding_model) : Json(nullptr);
  }
  if (patch.retrieval.query_prefix) retrieval["queryPrefix"] = *patch.retrieval.query_prefix;
  if (patch.retrieval.document_prefix) retrieval["documentPrefix"] = *patch.retrieval.document_prefix;
  if (patch.retrieval.max_depth) retrieval["maxDepth"] = *patch.retrieval.max_depth;
  if (patch.retrieval.max_seeds) retrieval["maxSeeds"] = *patch.retrieval.max_seeds;
  if (patch.retrieval.max_edges) retrieval["maxEdges"] = *patch.retrieval.max_edges;
  if (patch.retrieval.max_graph_results) retrieval["maxGraphResults"] = *patch.retrieval.max_graph_results;
  if (patch.retrieval.max_fanout) retrieval["maxFanout"] = *patch.retrieval.max_fanout;
  if (patch.retrieval.query_cache_entries) retrieval["queryCacheEntries"] = *patch.retrieval.query_cache_entries;
  if (!retrieval.empty()) json["retrieval"] = std::move(retrieval);

  Json prehandler = Json::object();
  if (patch.prehandler.model) {
    prehandler["model"] = *patch.prehandler.model
                              ? Json(**patch.prehandler.model)
                              : Json(nullptr);
  }
  if (patch.prehandler.thinking) {
    prehandler["thinking"] = *patch.prehandler.thinking;
  }
  if (patch.prehandler.timeout_ms) {
    prehandler["timeoutMs"] = *patch.prehandler.timeout_ms;
  }
  if (patch.prehandler.fail_open) {
    prehandler["failOpen"] = *patch.prehandler.fail_open;
  }
  if (patch.prehandler.max_model_input_tokens) {
    prehandler["maxModelInputTokens"] =
        *patch.prehandler.max_model_input_tokens;
  }
  if (!prehandler.empty()) {
    json["prehandler"] = std::move(prehandler);
  }

  Json capture = Json::object();
  if (patch.capture.post_process) {
    capture["postProcess"] = *patch.capture.post_process;
  }
  if (patch.capture.default_status) {
    capture["defaultStatus"] = *patch.capture.default_status;
  }
  if (patch.capture.max_evidence_chars) {
    capture["maxEvidenceChars"] = *patch.capture.max_evidence_chars;
  }
  if (!capture.empty()) {
    json["capture"] = std::move(capture);
  }

  Json dreaming = Json::object();
  if (patch.dreaming.enabled) {
    dreaming["enabled"] = *patch.dreaming.enabled;
  }
  if (patch.dreaming.trigger) {
    dreaming["trigger"] = *patch.dreaming.trigger;
  }
  if (patch.dreaming.interval_seconds) {
    dreaming["intervalSeconds"] = *patch.dreaming.interval_seconds;
  }
  if (patch.dreaming.idle_seconds) {
    dreaming["idleSeconds"] = *patch.dreaming.idle_seconds;
  }
  if (patch.dreaming.candidate_threshold) {
    dreaming["candidateThreshold"] = *patch.dreaming.candidate_threshold;
  }
  if (patch.dreaming.model) {
    dreaming["model"] = *patch.dreaming.model
                            ? Json(**patch.dreaming.model)
                            : Json(nullptr);
  }
  if (patch.dreaming.verifier_model) {
    dreaming["verifierModel"] = *patch.dreaming.verifier_model
                                     ? Json(**patch.dreaming.verifier_model)
                                     : Json(nullptr);
  }
  if (patch.dreaming.timeout_ms) {
    dreaming["timeoutMs"] = *patch.dreaming.timeout_ms;
  }
  if (patch.dreaming.max_model_input_tokens) {
    dreaming["maxModelInputTokens"] =
        *patch.dreaming.max_model_input_tokens;
  }
  if (patch.dreaming.max_candidates) {
    dreaming["maxCandidates"] = *patch.dreaming.max_candidates;
  }
  if (patch.dreaming.research_policy) {
    dreaming["researchPolicy"] = *patch.dreaming.research_policy;
  }
  if (patch.dreaming.promotion_policy) {
    dreaming["promotionPolicy"] = *patch.dreaming.promotion_policy;
  }
  if (!dreaming.empty()) {
    json["dreaming"] = std::move(dreaming);
  }
  return json;
}

template <typename T, typename Parser>
Result<bool> parse_optional_field(const Json& object, const char* key,
                                  const std::string& field_name,
                                  std::optional<T>& target, Parser parser) {
  const auto it = object.find(key);
  if (it == object.end()) {
    return false;
  }
  const auto parsed = parser(*it, field_name);
  if (!parsed) {
    return parsed.error();
  }
  target = parsed.value();
  return true;
}

Result<qt::ai::LlamaGenerationSettings> parse_llama_generation_settings(
    const Json& json, const std::string& field_prefix) {
  if (!json.is_object()) {
    return Result<qt::ai::LlamaGenerationSettings>::failure(
        ErrorCode::invalid_argument, field_prefix + " must be an object");
  }

  qt::ai::LlamaGenerationSettings settings;
  const auto assign = [&](auto& target, const char* key, const char* name,
                          auto parser) -> Result<bool> {
    return parse_optional_field(json, key, field_prefix + "." + name, target,
                                parser);
  };

  Result<bool> parsed = assign(settings.temperature, "temperature",
                               "temperature", parse_double_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.top_k, "topK", "topK", parse_int_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.top_p, "topP", "topP", parse_double_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.min_p, "minP", "minP", parse_double_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.min_keep, "minKeep", "minKeep", parse_int_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.seed, "seed", "seed", parse_uint32_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.max_output_tokens, "maxOutputTokens",
                  "maxOutputTokens", parse_int64_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.repeat_penalty, "repeatPenalty", "repeatPenalty",
                  parse_double_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.presence_penalty, "presencePenalty",
                  "presencePenalty", parse_double_value);
  if (!parsed) return parsed.error();
  parsed = assign(settings.frequency_penalty, "frequencyPenalty",
                  "frequencyPenalty", parse_double_value);
  if (!parsed) return parsed.error();

  return settings;
}

Result<qt::ai::LlamaSettings> parse_llama_settings(
    const qt::harness::RepositoryView& repository, const Json& json,
    const std::string& base_directory) {
  if (!json.is_object()) {
    return Result<qt::ai::LlamaSettings>::failure(ErrorCode::invalid_argument,
                                                  "llama must be an object");
  }

  qt::ai::LlamaSettings settings;

  if (const auto selected = json.find("selectedModel");
      selected != json.end()) {
    const auto parsed =
        parse_string_value(*selected, "llama.selectedModel");
    if (!parsed) {
      return parsed.error();
    }
    settings.selected_model_id = parsed.value();
  }

  if (const auto backend = json.find("backend"); backend != json.end()) {
    if (!backend->is_object()) {
      return Result<qt::ai::LlamaSettings>::failure(
          ErrorCode::invalid_argument, "llama.backend must be an object");
    }
    const auto assign = [&](auto& target, const char* key, const char* name,
                            auto parser) -> Result<bool> {
      return parse_optional_field(*backend, key,
                                  std::string("llama.backend.") + name, target,
                                  parser);
    };
    Result<bool> parsed =
        assign(settings.backend.enabled, "enabled", "enabled", parse_boolean_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.backend.gpu_layers, "gpuLayers", "gpuLayers",
                    parse_int_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.backend.main_gpu, "mainGpu", "mainGpu",
                    parse_int_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.backend.split_mode, "splitMode", "splitMode",
                    parse_string_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.backend.use_mmap, "useMmap", "useMmap",
                    parse_boolean_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.backend.use_mlock, "useMlock", "useMlock",
                    parse_boolean_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.backend.offload_kqv, "offloadKqv", "offloadKqv",
                    parse_boolean_value);
    if (!parsed) return parsed.error();

    if (const auto split = backend->find("tensorSplit"); split != backend->end()) {
      if (!split->is_array()) {
        return Result<qt::ai::LlamaSettings>::failure(
            ErrorCode::invalid_argument,
            "llama.backend.tensorSplit must be an array");
      }
      settings.backend.tensor_split.clear();
      settings.backend.tensor_split.reserve(split->size());
      for (std::size_t index = 0; index < split->size(); ++index) {
        const auto value = parse_double_value(
            (*split)[index],
            "llama.backend.tensorSplit[" + std::to_string(index) + "]");
        if (!value) {
          return value.error();
        }
        settings.backend.tensor_split.push_back(value.value());
      }
      settings.backend.tensor_split_configured = true;
    }
  }

  if (const auto context = json.find("context"); context != json.end()) {
    if (!context->is_object()) {
      return Result<qt::ai::LlamaSettings>::failure(
          ErrorCode::invalid_argument, "llama.context must be an object");
    }
    const auto assign = [&](auto& target, const char* key, const char* name,
                            auto parser) -> Result<bool> {
      return parse_optional_field(*context, key,
                                  std::string("llama.context.") + name, target,
                                  parser);
    };
    Result<bool> parsed = assign(settings.context.context_size, "contextSize",
                                 "contextSize", parse_int64_value);
    if (!parsed) return parsed.error();
    parsed =
        assign(settings.context.batch_size, "batchSize", "batchSize",
               parse_int64_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.context.ubatch_size, "ubatchSize", "ubatchSize",
                    parse_int64_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.context.threads, "threads", "threads",
                    parse_int_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.context.batch_threads, "batchThreads",
                    "batchThreads", parse_int_value);
    if (!parsed) return parsed.error();
  }

  if (const auto generation = json.find("generation");
      generation != json.end()) {
    const auto parsed =
        parse_llama_generation_settings(*generation, "llama.generation");
    if (!parsed) {
      return parsed.error();
    }
    settings.generation = parsed.value();
  }

  if (const auto kv_cache = json.find("kvCache"); kv_cache != json.end()) {
    if (!kv_cache->is_object()) {
      return Result<qt::ai::LlamaSettings>::failure(
          ErrorCode::invalid_argument, "llama.kvCache must be an object");
    }
    const auto assign = [&](auto& target, const char* key, const char* name,
                            auto parser) -> Result<bool> {
      return parse_optional_field(*kv_cache, key,
                                  std::string("llama.kvCache.") + name, target,
                                  parser);
    };
    Result<bool> parsed =
        assign(settings.kv_cache.policy, "policy", "policy", parse_string_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.kv_cache.max_sessions, "maxSessions",
                    "maxSessions", parse_int_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.kv_cache.clear_on_model_change,
                    "clearOnModelChange", "clearOnModelChange",
                    parse_boolean_value);
    if (!parsed) return parsed.error();
  }

  if (const auto grammar = json.find("grammar"); grammar != json.end()) {
    if (!grammar->is_object()) {
      return Result<qt::ai::LlamaSettings>::failure(
          ErrorCode::invalid_argument, "llama.grammar must be an object");
    }
    Result<bool> parsed =
        parse_optional_field(*grammar, "root", "llama.grammar.root",
                             settings.grammar.root, parse_string_value);
    if (!parsed) return parsed.error();
    if (const auto default_it = grammar->find("default");
        default_it != grammar->end() && !default_it->is_null()) {
      const auto value =
          parse_string_value(*default_it, "llama.grammar.default");
      if (!value) {
        return value.error();
      }
      settings.grammar.default_grammar = value.value();
    }
    if (const auto files = grammar->find("files"); files != grammar->end()) {
      if (!files->is_object()) {
        return Result<qt::ai::LlamaSettings>::failure(
            ErrorCode::invalid_argument,
            "llama.grammar.files must be an object");
      }
      for (auto it = files->begin(); it != files->end(); ++it) {
        if (!it.value().is_string()) {
          return Result<qt::ai::LlamaSettings>::failure(
              ErrorCode::invalid_argument,
              "llama.grammar.files." + it.key() + " must be a string");
        }
        const auto resolved = canonicalize_setting_path(
            repository, it.value().get<std::string>(), base_directory,
            "llama.grammar.files." + it.key());
        if (!resolved) {
          return resolved.error();
        }
        settings.grammar.files[it.key()] = resolved.value();
      }
      settings.grammar.files_configured = true;
    }
  }

  if (const auto embeddings = json.find("embeddings");
      embeddings != json.end()) {
    if (!embeddings->is_object()) {
      return Result<qt::ai::LlamaSettings>::failure(
          ErrorCode::invalid_argument, "llama.embeddings must be an object");
    }
    const auto assign = [&](auto& target, const char* key, const char* name,
                            auto parser) -> Result<bool> {
      return parse_optional_field(*embeddings, key,
                                  std::string("llama.embeddings.") + name,
                                  target, parser);
    };
    Result<bool> parsed = assign(settings.embeddings.enabled, "enabled",
                                 "enabled", parse_boolean_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.embeddings.pooling, "pooling", "pooling",
                    parse_string_value);
    if (!parsed) return parsed.error();
    parsed = assign(settings.embeddings.normalize, "normalize", "normalize",
                    parse_boolean_value);
    if (!parsed) return parsed.error();
  }

  if (const auto models = json.find("models"); models != json.end()) {
    if (!models->is_array()) {
      return Result<qt::ai::LlamaSettings>::failure(
          ErrorCode::invalid_argument, "llama.models must be an array");
    }
    settings.models.clear();
    settings.models.reserve(models->size());
    for (std::size_t index = 0; index < models->size(); ++index) {
      const auto& item = (*models)[index];
      const std::string prefix = "llama.models[" + std::to_string(index) + "]";
      if (!item.is_object()) {
        return Result<qt::ai::LlamaSettings>::failure(
            ErrorCode::invalid_argument, prefix + " must be an object");
      }
      qt::ai::LlamaModelSettings model;
      if (const auto id = item.find("id"); id != item.end()) {
        const auto value = parse_string_value(*id, prefix + ".id");
        if (!value) {
          return value.error();
        }
        model.id = value.value();
      }
      if (const auto name = item.find("name"); name != item.end()) {
        const auto value = parse_string_value(*name, prefix + ".name");
        if (!value) {
          return value.error();
        }
        model.name = value.value();
      }
      if (const auto path = item.find("path"); path != item.end()) {
        if (!path->is_string()) {
          return Result<qt::ai::LlamaSettings>::failure(
              ErrorCode::invalid_argument, prefix + ".path must be a string");
        }
        const auto resolved = canonicalize_setting_path(
            repository, path->get<std::string>(), base_directory, prefix + ".path");
        if (!resolved) {
          return resolved.error();
        }
        model.path = resolved.value();
      }
      Result<bool> parsed = parse_optional_field(
          item, "blockCount", prefix + ".blockCount", model.block_count,
          parse_int64_value);
      if (!parsed) return parsed.error();
      parsed = parse_optional_field(
          item, "contextWindow", prefix + ".contextWindow",
          model.context_window, parse_int64_value);
      if (!parsed) return parsed.error();
      parsed = parse_optional_field(item, "maxTokens", prefix + ".maxTokens",
                                    model.max_tokens, parse_int64_value);
      if (!parsed) return parsed.error();
      parsed = parse_optional_field(item, "supportsEmbeddings",
                                    prefix + ".supportsEmbeddings",
                                    model.supports_embeddings,
                                    parse_boolean_value);
      if (!parsed) return parsed.error();
      if (const auto generation = item.find("generation");
          generation != item.end()) {
        const auto parsed_generation =
            parse_llama_generation_settings(*generation,
                                            prefix + ".generation");
        if (!parsed_generation) {
          return parsed_generation.error();
        }
        model.generation = parsed_generation.value();
      }
      settings.models.push_back(std::move(model));
    }
    settings.models_configured = true;
  }

  return settings;
}

std::string lower_copy(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const char character : value) {
    if (character >= 'A' && character <= 'Z') {
      lowered.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      lowered.push_back(character);
    }
  }
  return lowered;
}

bool is_yaml_settings_path(const std::string& path) {
  const std::string extension = lower_copy(Path(path).extension().generic_string());
  return extension == ".yaml" || extension == ".yml";
}

std::string trim_yaml_line_left(std::string_view line) {
  std::size_t index = 0;
  while (index < line.size() && line[index] == ' ') {
    ++index;
  }
  return std::string(line.substr(index));
}

std::string sequence_key(std::size_t index) {
  std::ostringstream stream;
  stream << std::setw(12) << std::setfill('0') << index;
  return stream.str();
}

bool is_blank_or_comment_line(const std::string& trimmed) {
  return trimmed.empty() || trimmed.front() == '#';
}

std::optional<std::string> path_array_field_waiting_for_sequence(
    const std::string& trimmed) {
  const std::size_t separator = trimmed.find(':');
  if (separator == std::string::npos) {
    return std::nullopt;
  }

  const std::string key = trimmed.substr(0, separator);
  if (key != "promptRoots" && key != "skillRoots" && key != "themeRoots") {
    return std::nullopt;
  }

  const std::string rest = trim_yaml_line_left(
      std::string_view(trimmed).substr(separator + 1));
  if (!rest.empty() && rest.front() != '#') {
    return std::nullopt;
  }

  return key;
}

std::string normalize_yaml_sequences(std::string_view content) {
  std::istringstream input{std::string(content)};
  std::ostringstream output;
  std::string line;
  std::size_t sequence_index = 0;
  std::optional<std::size_t> active_sequence_parent_indent;
  std::optional<std::size_t> active_sequence_item_indent;

  while (std::getline(input, line)) {
    const bool had_carriage_return = !line.empty() && line.back() == '\r';
    if (had_carriage_return) {
      line.pop_back();
    }

    const std::size_t indent = line.find_first_not_of(' ');
    const std::size_t first_non_space =
        indent == std::string::npos ? line.size() : indent;
    const std::string trimmed = trim_yaml_line_left(line);

    if (trimmed.rfind("- ", 0) == 0 || trimmed == "-") {
      if (!active_sequence_parent_indent ||
          first_non_space <= *active_sequence_parent_indent) {
        output << line;
        if (had_carriage_return) {
          output << '\r';
        }
        output << '\n';
        continue;
      }
      if (!active_sequence_item_indent) {
        active_sequence_item_indent = first_non_space;
      }
      if (*active_sequence_item_indent != first_non_space) {
        output << line;
        if (had_carriage_return) {
          output << '\r';
        }
        output << '\n';
        continue;
      }

      output << line.substr(0, first_non_space) << sequence_key(sequence_index++)
             << ":";
      if (trimmed.size() > 1) {
        output << trimmed.substr(1);
      }
      if (had_carriage_return) {
        output << '\r';
      }
      output << '\n';
      continue;
    }

    output << line;
    if (had_carriage_return) {
      output << '\r';
    }
    output << '\n';

    if (is_blank_or_comment_line(trimmed)) {
      continue;
    }

    if (const auto path_array_field = path_array_field_waiting_for_sequence(trimmed);
        path_array_field) {
      active_sequence_parent_indent = first_non_space;
      active_sequence_item_indent = std::nullopt;
      sequence_index = 0;
    } else if (active_sequence_parent_indent &&
               first_non_space <= *active_sequence_parent_indent) {
      active_sequence_parent_indent = std::nullopt;
      active_sequence_item_indent = std::nullopt;
    }
  }

  return output.str();
}

Result<Json> config_value_to_json(const qt::foundation::ConfigValue& value) {
  if (value.is_scalar()) {
    return Json(value.scalar());
  }

  Json json = Json::object();
  for (const auto& [key, child] : value.map()) {
    const auto child_json = config_value_to_json(child);
    if (!child_json) {
      return child_json.error();
    }
    json[key] = child_json.value();
  }
  return json;
}

Result<Json> parse_yaml_settings_json(std::string_view content) {
  const std::string normalized = normalize_yaml_sequences(content);
  const auto document = qt::foundation::parse_config(normalized);
  if (!document) {
    return Result<Json>::failure(document.error().code, document.error().message,
                                 document.error().system_error);
  }

  return config_value_to_json(document.value().root);
}

Result<Json> yaml_scalar_string(const Json& json,
                                const std::string& field_name) {
  if (!json.is_string()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be a string");
  }
  return json;
}

Result<Json> yaml_path_array(const Json& json, const std::string& field_name) {
  if (!json.is_object()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be an array");
  }

  Json array = Json::array();
  for (const auto& [key, value] : json.items()) {
    (void)key;
    if (!value.is_string()) {
      return Result<Json>::failure(
          ErrorCode::invalid_argument,
          field_name + "[" + std::to_string(array.size()) + "] must be a string");
    }
    array.push_back(value);
  }
  return array;
}

Result<Json> yaml_string_array(const Json& json, const std::string& field_name) {
  return yaml_path_array(json, field_name);
}

Result<Json> yaml_boolean(const Json& json, const std::string& field_name) {
  if (!json.is_string()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be a boolean");
  }
  const std::string value = lower_copy(json.get<std::string>());
  if (value == "true" || value == "yes" || value == "on" || value == "1") {
    return Json(true);
  }
  if (value == "false" || value == "no" || value == "off" || value == "0") {
    return Json(false);
  }
  return Result<Json>::failure(ErrorCode::invalid_argument,
                               field_name + " must be a boolean");
}

Result<Json> yaml_uint32(const Json& json, const std::string& field_name) {
  if (!json.is_string()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be an integer");
  }
  const std::string text = json.get<std::string>();
  std::uint32_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be an integer");
  }
  return Json(value);
}

Result<Json> yaml_int64(const Json& json, const std::string& field_name) {
  if (!json.is_string()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be an integer");
  }
  const std::string text = json.get<std::string>();
  std::int64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 field_name + " must be an integer");
  }
  return Json(value);
}

Result<Json> normalize_yaml_settings_json(const Json& yaml_json) {
  if (!yaml_json.is_object()) {
    return Result<Json>::failure(ErrorCode::invalid_argument,
                                 "settings document must be a JSON object");
  }

  if (yaml_json.find("llama") != yaml_json.end()) {
    return Result<Json>::failure(
        ErrorCode::invalid_argument,
        "llama YAML settings are not supported yet; use settings.json");
  }

  Json json = Json::object();

  for (const auto* field :
       {"cwd", "workspaceRoot", "sessionDir", "agentHome", "defaultProvider",
        "defaultModel", "defaultThinkingLevel", "theme", "steeringMode",
        "followUpMode", "transport", "shellPath", "shellCommandPrefix"}) {
    if (const auto it = yaml_json.find(field); it != yaml_json.end()) {
      const auto value = yaml_scalar_string(*it, field);
      if (!value) {
        return value.error();
      }
      json[field] = value.value();
    }
  }

  for (const auto* field :
       {"promptRoots", "skillRoots", "themeRoots", "prompts", "skills",
        "themes"}) {
    if (const auto it = yaml_json.find(field); it != yaml_json.end()) {
      const auto value = yaml_path_array(*it, field);
      if (!value) {
        return value.error();
      }
      json[field] = value.value();
    }
  }

  for (const auto* field : {"npmCommand", "enabledModels"}) {
    if (const auto it = yaml_json.find(field); it != yaml_json.end()) {
      const auto value = yaml_string_array(*it, field);
      if (!value) {
        return value.error();
      }
      json[field] = value.value();
    }
  }

  for (const auto* field : {"quietStartup", "collapseChangelog",
                            "hideThinkingBlock", "enableSkillCommands",
                            "allowExternalFilesystemAccess"}) {
    if (const auto it = yaml_json.find(field); it != yaml_json.end()) {
      const auto value = yaml_boolean(*it, field);
      if (!value) {
        return value.error();
      }
      json[field] = value.value();
    }
  }

  if (const auto images = yaml_json.find("images"); images != yaml_json.end()) {
    if (!images->is_object()) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "images must be an object");
    }

    Json normalized_images = Json::object();
    if (const auto auto_resize = images->find("autoResize");
        auto_resize != images->end()) {
      const auto value = yaml_boolean(*auto_resize, "images.autoResize");
      if (!value) {
        return value.error();
      }
      normalized_images["autoResize"] = value.value();
    }
    json["images"] = std::move(normalized_images);
  }

  if (const auto terminal = yaml_json.find("terminal");
      terminal != yaml_json.end()) {
    if (!terminal->is_object()) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "terminal must be an object");
    }

    Json normalized_terminal = Json::object();
    if (const auto show_progress = terminal->find("showTerminalProgress");
        show_progress != terminal->end()) {
      const auto value =
          yaml_boolean(*show_progress, "terminal.showTerminalProgress");
      if (!value) {
        return value.error();
      }
      normalized_terminal["showTerminalProgress"] = value.value();
    }
    if (const auto history_limit = terminal->find("promptHistoryLimit");
        history_limit != terminal->end()) {
      const auto value =
          parse_uint32_value(*history_limit, "terminal.promptHistoryLimit");
      if (!value) {
        return value.error();
      }
      normalized_terminal["promptHistoryLimit"] = value.value();
    }
    if (const auto enabled_tools =
            terminal->find("toolEntryCompactionEnabledTools");
        enabled_tools != terminal->end()) {
      const auto value = yaml_string_array(
          *enabled_tools, "terminal.toolEntryCompactionEnabledTools");
      if (!value) {
        return value.error();
      }
      normalized_terminal["toolEntryCompactionEnabledTools"] = value.value();
    }
    json["terminal"] = std::move(normalized_terminal);
  }

  if (const auto retry = yaml_json.find("retry"); retry != yaml_json.end()) {
    if (!retry->is_object()) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "retry must be an object");
    }

    Json normalized_retry = Json::object();
    if (const auto enabled = retry->find("enabled"); enabled != retry->end()) {
      const auto value = yaml_boolean(*enabled, "retry.enabled");
      if (!value) {
        return value.error();
      }
      normalized_retry["enabled"] = value.value();
    }
    if (const auto provider = retry->find("provider");
        provider != retry->end()) {
      const auto value =
          parse_provider_retry_settings(*provider, "retry.provider");
      if (!value) {
        return Result<Json>::failure(value.error().code,
                                     value.error().message,
                                     value.error().system_error);
      }
      normalized_retry["provider"] =
          provider_retry_settings_to_json(value.value());
    }
    json["retry"] = std::move(normalized_retry);
  }

  if (const auto memory = yaml_json.find("memory"); memory != yaml_json.end()) {
    if (!memory->is_object()) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "memory must be an object");
    }
    Json normalized_memory = Json::object();
    for (const auto* field :
         {"enabled", "automaticRecall", "automaticCapture"}) {
      if (const auto it = memory->find(field); it != memory->end()) {
        const auto value = yaml_boolean(*it, std::string("memory.") + field);
        if (!value) {
          return value.error();
        }
        normalized_memory[field] = value.value();
      }
    }
    if (const auto policy = memory->find("recallPolicy");
        policy != memory->end()) {
      const auto value = yaml_scalar_string(*policy, "memory.recallPolicy");
      if (!value) {
        return value.error();
      }
      normalized_memory["recallPolicy"] = value.value();
    }
    for (const auto* field : {"contextTokenBudget", "candidateLimit"}) {
      if (const auto it = memory->find(field); it != memory->end()) {
        const auto value = yaml_uint32(*it, std::string("memory.") + field);
        if (!value) {
          return value.error();
        }
        normalized_memory[field] = value.value();
      }
    }

    if (const auto retrieval = memory->find("retrieval");
        retrieval != memory->end()) {
      if (!retrieval->is_object()) {
        return Result<Json>::failure(ErrorCode::invalid_argument,
                                     "memory.retrieval must be an object");
      }
      Json normalized_retrieval = Json::object();
      for (const auto* field : {"graphEnabled", "denseEnabled", "codeEnabled"}) {
        if (const auto it = retrieval->find(field); it != retrieval->end()) {
          const auto value = yaml_boolean(*it, std::string("memory.retrieval.") + field);
          if (!value) return value.error();
          normalized_retrieval[field] = value.value();
        }
      }
      for (const auto* field : {"queryPrefix", "documentPrefix", "embeddingModel"}) {
        if (const auto it = retrieval->find(field); it != retrieval->end()) {
          if (it->is_null() && std::string(field) == "embeddingModel") {
            normalized_retrieval[field] = nullptr;
          } else {
            const auto value = yaml_scalar_string(*it, std::string("memory.retrieval.") + field);
            if (!value) return value.error();
            normalized_retrieval[field] = value.value();
          }
        }
      }
      for (const auto* field : {"maxDepth", "maxSeeds", "maxEdges",
                                "maxGraphResults", "maxFanout", "queryCacheEntries"}) {
        if (const auto it = retrieval->find(field); it != retrieval->end()) {
          const auto value = yaml_uint32(*it, std::string("memory.retrieval.") + field);
          if (!value) return value.error();
          normalized_retrieval[field] = value.value();
        }
      }
      normalized_memory["retrieval"] = std::move(normalized_retrieval);
    }

    if (const auto prehandler = memory->find("prehandler");
        prehandler != memory->end()) {
      if (!prehandler->is_object()) {
        return Result<Json>::failure(
            ErrorCode::invalid_argument,
            "memory.prehandler must be an object");
      }
      Json normalized_prehandler = Json::object();
      if (const auto model = prehandler->find("model");
          model != prehandler->end()) {
        if (!model->is_string()) {
          return Result<Json>::failure(
              ErrorCode::invalid_argument,
              "memory.prehandler.model must be a string or null");
        }
        const std::string value = model->get<std::string>();
        const std::string lowered = lower_copy(value);
        normalized_prehandler["model"] =
            (lowered == "null" || value == "~") ? Json(nullptr) : Json(value);
      }
      if (const auto thinking = prehandler->find("thinking");
          thinking != prehandler->end()) {
        const auto value = yaml_scalar_string(
            *thinking, "memory.prehandler.thinking");
        if (!value) {
          return value.error();
        }
        normalized_prehandler["thinking"] = value.value();
      }
      if (const auto timeout = prehandler->find("timeoutMs");
          timeout != prehandler->end()) {
        const auto value =
            yaml_int64(*timeout, "memory.prehandler.timeoutMs");
        if (!value) {
          return value.error();
        }
        normalized_prehandler["timeoutMs"] = value.value();
      }
      if (const auto fail_open = prehandler->find("failOpen");
          fail_open != prehandler->end()) {
        const auto value =
            yaml_boolean(*fail_open, "memory.prehandler.failOpen");
        if (!value) {
          return value.error();
        }
        normalized_prehandler["failOpen"] = value.value();
      }
      if (const auto limit = prehandler->find("maxModelInputTokens");
          limit != prehandler->end()) {
        const auto value = yaml_uint32(
            *limit, "memory.prehandler.maxModelInputTokens");
        if (!value) {
          return value.error();
        }
        normalized_prehandler["maxModelInputTokens"] = value.value();
      }
      normalized_memory["prehandler"] = std::move(normalized_prehandler);
    }

    if (const auto capture = memory->find("capture");
        capture != memory->end()) {
      if (!capture->is_object()) {
        return Result<Json>::failure(ErrorCode::invalid_argument,
                                     "memory.capture must be an object");
      }
      Json normalized_capture = Json::object();
      for (const auto* field : {"postProcess", "defaultStatus"}) {
        if (const auto it = capture->find(field); it != capture->end()) {
          const auto value = yaml_scalar_string(
              *it, std::string("memory.capture.") + field);
          if (!value) {
            return value.error();
          }
          normalized_capture[field] = value.value();
        }
      }
      if (const auto limit = capture->find("maxEvidenceChars");
          limit != capture->end()) {
        const auto value =
            yaml_uint32(*limit, "memory.capture.maxEvidenceChars");
        if (!value) {
          return value.error();
        }
        normalized_capture["maxEvidenceChars"] = value.value();
      }
      normalized_memory["capture"] = std::move(normalized_capture);
    }

    if (const auto dreaming = memory->find("dreaming");
        dreaming != memory->end()) {
      if (!dreaming->is_object()) {
        return Result<Json>::failure(ErrorCode::invalid_argument,
                                     "memory.dreaming must be an object");
      }
      Json normalized_dreaming = Json::object();
      if (const auto enabled = dreaming->find("enabled");
          enabled != dreaming->end()) {
        const auto value =
            yaml_boolean(*enabled, "memory.dreaming.enabled");
        if (!value) {
          return value.error();
        }
        normalized_dreaming["enabled"] = value.value();
      }
      for (const auto* field :
           {"trigger", "researchPolicy", "promotionPolicy"}) {
        if (const auto it = dreaming->find(field); it != dreaming->end()) {
          const auto value = yaml_scalar_string(
              *it, std::string("memory.dreaming.") + field);
          if (!value) {
            return value.error();
          }
          normalized_dreaming[field] = value.value();
        }
      }
      for (const auto* field : {"intervalSeconds", "idleSeconds",
                                "candidateThreshold", "maxModelInputTokens",
                                "maxCandidates"}) {
        if (const auto it = dreaming->find(field); it != dreaming->end()) {
          const auto value = yaml_uint32(
              *it, std::string("memory.dreaming.") + field);
          if (!value) {
            return value.error();
          }
          normalized_dreaming[field] = value.value();
        }
      }
      if (const auto model = dreaming->find("model");
          model != dreaming->end()) {
        if (!model->is_string()) {
          return Result<Json>::failure(
              ErrorCode::invalid_argument,
              "memory.dreaming.model must be a string or null");
        }
        const std::string value = model->get<std::string>();
        const std::string lowered = lower_copy(value);
        normalized_dreaming["model"] =
            (lowered == "null" || value == "~") ? Json(nullptr)
                                                  : Json(value);
      }
      if (const auto verifier_model = dreaming->find("verifierModel");
          verifier_model != dreaming->end()) {
        if (!verifier_model->is_string()) {
          return Result<Json>::failure(
              ErrorCode::invalid_argument,
              "memory.dreaming.verifierModel must be a string or null");
        }
        const std::string value = verifier_model->get<std::string>();
        const std::string lowered = lower_copy(value);
        normalized_dreaming["verifierModel"] =
            (lowered == "null" || value == "~") ? Json(nullptr)
                                                  : Json(value);
      }
      if (const auto timeout = dreaming->find("timeoutMs");
          timeout != dreaming->end()) {
        const auto value =
            yaml_int64(*timeout, "memory.dreaming.timeoutMs");
        if (!value) {
          return value.error();
        }
        normalized_dreaming["timeoutMs"] = value.value();
      }
      normalized_memory["dreaming"] = std::move(normalized_dreaming);
    }
    json["memory"] = std::move(normalized_memory);
  }

  if (const auto features = yaml_json.find("features"); features != yaml_json.end()) {
    if (!features->is_object()) {
      return Result<Json>::failure(ErrorCode::invalid_argument,
                                   "features must be an object");
    }

    Json normalized_features = Json::object();
    for (const auto* field :
         {"loadPromptTemplates", "loadSkills", "loadThemes", "persistSessions"}) {
      if (const auto feature = features->find(field); feature != features->end()) {
        const auto value = yaml_boolean(
            *feature, std::string("features.") + std::string(field));
        if (!value) {
          return value.error();
        }
        normalized_features[field] = value.value();
      }
    }
    json["features"] = std::move(normalized_features);
  }

  return json;
}

Result<AppSettingsPatch> parse_settings_patch(
    const qt::harness::RepositoryView& repository, const Json& json,
    const std::string& base_directory) {
  if (!json.is_object()) {
    return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                             "settings document must be a JSON object");
  }

  AppSettingsPatch patch;

  if (const auto it = json.find("cwd"); it != json.end()) {
    if (!it->is_string()) {
      return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                               "cwd must be a string");
    }
    const auto resolved = canonicalize_setting_path(repository, it->get<std::string>(),
                                                    base_directory, "cwd");
    if (!resolved) {
      return resolved.error();
    }
    patch.cwd = resolved.value();
  }

  if (const auto it = json.find("workspaceRoot"); it != json.end()) {
    if (!it->is_string()) {
      return Result<AppSettingsPatch>::failure(
          ErrorCode::invalid_argument, "workspaceRoot must be a string");
    }
    const auto resolved = canonicalize_setting_path(
        repository, it->get<std::string>(), base_directory, "workspaceRoot");
    if (!resolved) {
      return resolved.error();
    }
    patch.workspace_root = resolved.value();
  }

  if (const auto it = json.find("sessionDir"); it != json.end()) {
    if (!it->is_string()) {
      return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                               "sessionDir must be a string");
    }
    const auto resolved = canonicalize_setting_path(repository, it->get<std::string>(),
                                                    base_directory, "sessionDir");
    if (!resolved) {
      return resolved.error();
    }
    patch.session_dir = resolved.value();
  }

  if (const auto it = json.find("agentHome"); it != json.end()) {
    if (!it->is_string()) {
      return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                               "agentHome must be a string");
    }
    const auto resolved = canonicalize_setting_path(repository, it->get<std::string>(),
                                                    base_directory, "agentHome");
    if (!resolved) {
      return resolved.error();
    }
    patch.agent_home = resolved.value();
  }

  for (const auto* field :
       {"defaultProvider", "defaultModel", "defaultThinkingLevel", "theme",
        "steeringMode", "followUpMode", "transport", "shellPath",
        "shellCommandPrefix"}) {
    if (const auto it = json.find(field); it != json.end()) {
      if (!it->is_string()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument, std::string(field) + " must be a string");
      }
      const std::string value = it->get<std::string>();
      if (std::string(field) == "defaultProvider") {
        patch.default_provider = value;
      } else if (std::string(field) == "defaultModel") {
        patch.default_model = value;
      } else if (std::string(field) == "defaultThinkingLevel") {
        patch.default_thinking_level = value;
      } else if (std::string(field) == "theme") {
        patch.theme = value;
      } else if (std::string(field) == "steeringMode") {
        patch.steering_mode = value;
      } else if (std::string(field) == "followUpMode") {
        patch.follow_up_mode = value;
      } else if (std::string(field) == "transport") {
        patch.transport = value;
      } else if (std::string(field) == "shellPath") {
        patch.shell_path = value;
      } else if (std::string(field) == "shellCommandPrefix") {
        patch.shell_command_prefix = value;
      }
    }
  }

  for (const auto* field : {"quietStartup", "collapseChangelog",
                            "hideThinkingBlock", "enableSkillCommands",
                            "allowExternalFilesystemAccess"}) {
    if (const auto it = json.find(field); it != json.end()) {
      if (!it->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument, std::string(field) + " must be a boolean");
      }
      if (std::string(field) == "quietStartup") {
        patch.quiet_startup = it->get<bool>();
      } else if (std::string(field) == "collapseChangelog") {
        patch.collapse_changelog = it->get<bool>();
      } else if (std::string(field) == "hideThinkingBlock") {
        patch.hide_thinking_block = it->get<bool>();
      } else if (std::string(field) == "enableSkillCommands") {
        patch.enable_skill_commands = it->get<bool>();
      } else if (std::string(field) == "allowExternalFilesystemAccess") {
        patch.allow_external_filesystem_access = it->get<bool>();
      }
    }
  }

  if (const auto it = json.find("images"); it != json.end()) {
    if (!it->is_object()) {
      return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                               "images must be an object");
    }

    if (const auto auto_resize = it->find("autoResize");
        auto_resize != it->end()) {
      if (!auto_resize->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument,
            "images.autoResize must be a boolean");
      }
      patch.image_auto_resize = auto_resize->get<bool>();
    }
  }

  if (const auto it = json.find("terminal"); it != json.end()) {
    if (!it->is_object()) {
      return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                               "terminal must be an object");
    }

    if (const auto show_progress = it->find("showTerminalProgress");
        show_progress != it->end()) {
      if (!show_progress->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument,
            "terminal.showTerminalProgress must be a boolean");
      }
      patch.terminal_show_progress = show_progress->get<bool>();
    }
    if (const auto history_limit = it->find("promptHistoryLimit");
        history_limit != it->end()) {
      const auto value = parse_uint32_value(
          *history_limit, "terminal.promptHistoryLimit");
      if (!value) {
        return value.error();
      }
      patch.terminal_prompt_history_limit =
          static_cast<std::size_t>(value.value());
    }
    if (const auto enabled_tools =
            it->find("toolEntryCompactionEnabledTools");
        enabled_tools != it->end()) {
      const auto values = parse_string_array(
          *enabled_tools, "terminal.toolEntryCompactionEnabledTools");
      if (!values) {
        return values.error();
      }
      patch.terminal_tool_entry_compaction_enabled_tools = values.value();
    }
  }

  if (const auto it = json.find("retry"); it != json.end()) {
    if (!it->is_object()) {
      return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                               "retry must be an object");
    }

    if (const auto enabled = it->find("enabled"); enabled != it->end()) {
      if (!enabled->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument, "retry.enabled must be a boolean");
      }
      patch.retry_enabled = enabled->get<bool>();
    }
    if (const auto provider = it->find("provider"); provider != it->end()) {
      const auto parsed =
          parse_provider_retry_settings(*provider, "retry.provider");
      if (!parsed) {
        return Result<AppSettingsPatch>::failure(
            parsed.error().code, parsed.error().message,
            parsed.error().system_error);
      }
      patch.provider_retry = parsed.value();
    }
  }

  if (const auto it = json.find("npmCommand"); it != json.end()) {
    const auto values = parse_string_array(*it, "npmCommand");
    if (!values) {
      return values.error();
    }
    patch.npm_command = values.value();
  }

  if (const auto it = json.find("enabledModels"); it != json.end()) {
    const auto values = parse_string_array(*it, "enabledModels");
    if (!values) {
      return values.error();
    }
    patch.enabled_models = values.value();
  }

  if (const auto it = json.find("mcpServers"); it != json.end()) {
    const auto values = parse_mcp_servers(*it, "mcpServers");
    if (!values) {
      return Result<AppSettingsPatch>::failure(values.error().code,
                                               values.error().message);
    }
    patch.mcp_servers = values.value();
  } else if (const auto alias = json.find("mcp_servers");
             alias != json.end()) {
    const auto values = parse_mcp_servers(*alias, "mcp_servers");
    if (!values) {
      return Result<AppSettingsPatch>::failure(values.error().code,
                                               values.error().message);
    }
    patch.mcp_servers = values.value();
  }

  if (const auto it = json.find("ollama"); it != json.end()) {
    const auto values = parse_ollama_instances(*it, "ollama");
    if (!values) {
      return Result<AppSettingsPatch>::failure(values.error().code,
                                               values.error().message);
    }
    patch.ollama_instances = values.value();
  }

  const auto parse_roots_alias =
      [&](const char* preferred, const char* alias,
          std::optional<std::vector<std::string>>& target,
          std::optional<std::vector<std::string>>& filter_target)
      -> Result<bool> {
    const auto preferred_it = json.find(preferred);
    const auto alias_it = json.find(alias);
    if (preferred_it == json.end() && alias_it == json.end()) {
      return false;
    }
    const auto& value =
        preferred_it != json.end() ? *preferred_it : *alias_it;
    const char* field_name = preferred_it != json.end() ? preferred : alias;
    const auto roots = parse_resource_path_array(
        repository, value, field_name, base_directory, filter_target);
    if (!roots) {
      return Result<bool>::failure(roots.error().code, roots.error().message);
    }
    target = roots.value();
    return true;
  };

  {
    const auto parsed = parse_roots_alias("promptRoots", "prompts",
                                          patch.prompt_roots,
                                          patch.prompt_template_filters);
    if (!parsed) {
      return Result<AppSettingsPatch>::failure(parsed.error().code,
                                               parsed.error().message);
    }
  }

  {
    const auto parsed = parse_roots_alias("skillRoots", "skills",
                                          patch.skill_roots,
                                          patch.skill_filters);
    if (!parsed) {
      return Result<AppSettingsPatch>::failure(parsed.error().code,
                                               parsed.error().message);
    }
  }

  {
    const auto parsed = parse_roots_alias("themeRoots", "themes",
                                          patch.theme_roots,
                                          patch.theme_filters);
    if (!parsed) {
      return Result<AppSettingsPatch>::failure(parsed.error().code,
                                               parsed.error().message);
    }
  }

  if (const auto it = json.find("features"); it != json.end()) {
    if (!it->is_object()) {
      return Result<AppSettingsPatch>::failure(ErrorCode::invalid_argument,
                                               "features must be an object");
    }

    if (const auto feature = it->find("loadPromptTemplates");
        feature != it->end()) {
      if (!feature->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument,
            "features.loadPromptTemplates must be a boolean");
      }
      patch.features.load_prompt_templates = feature->get<bool>();
    }

    if (const auto feature = it->find("loadSkills"); feature != it->end()) {
      if (!feature->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument, "features.loadSkills must be a boolean");
      }
      patch.features.load_skills = feature->get<bool>();
    }

    if (const auto feature = it->find("loadThemes"); feature != it->end()) {
      if (!feature->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument, "features.loadThemes must be a boolean");
      }
      patch.features.load_themes = feature->get<bool>();
    }

    if (const auto feature = it->find("persistSessions"); feature != it->end()) {
      if (!feature->is_boolean()) {
        return Result<AppSettingsPatch>::failure(
            ErrorCode::invalid_argument,
            "features.persistSessions must be a boolean");
      }
      patch.features.persist_sessions = feature->get<bool>();
    }
  }

  if (const auto it = json.find("memory"); it != json.end()) {
    const auto parsed = parse_memory_settings_patch(*it, "memory");
    if (!parsed) {
      return Result<AppSettingsPatch>::failure(
          parsed.error().code, parsed.error().message,
          parsed.error().system_error);
    }
    patch.memory = parsed.value();
  }

  if (const auto it = json.find("llama"); it != json.end()) {
    const auto parsed = parse_llama_settings(repository, *it, base_directory);
    if (!parsed) {
      return Result<AppSettingsPatch>::failure(parsed.error().code,
                                               parsed.error().message);
    }
    patch.llama = parsed.value();
  }

  return patch;
}

std::optional<std::string> default_agent_home() {
  const auto home = getenv_string("HOME");
  if (!home) {
    return std::nullopt;
  }
  return *home + "/.qt";
}

std::vector<std::string> default_theme_roots(
    const qt::harness::RepositoryView& repository,
    const qt::harness::RepoContextDiscovery& discovery,
    const std::string& agent_home) {
  std::vector<std::string> roots;
  const auto append_if_directory = [&](const Path& path) {
    const std::string value = path_to_string(path);
    if (repository.directory_exists(value) &&
        std::find(roots.begin(), roots.end(), value) == roots.end()) {
      roots.push_back(value);
    }
  };

  append_if_directory(Path(agent_home) / "themes");
  if (discovery.repository_root) {
    append_if_directory(Path(*discovery.repository_root) / ".pi" / "themes");
    append_if_directory(Path(*discovery.repository_root) / ".agents" / "themes");
  }
  return roots;
}

AppSettingsPatch default_settings_patch(
    const qt::harness::RepositoryView& repository,
    const qt::harness::RepoContextDiscovery& discovery,
    const std::string& agent_home) {
  AppSettingsPatch patch;
  patch.cwd = discovery.cwd;
  patch.workspace_root = discovery.repository_root.value_or(discovery.cwd);
  patch.agent_home = agent_home;
  patch.session_dir = path_to_string(Path(agent_home) / "sessions");
  patch.theme = "dark";
  patch.quiet_startup = false;
  patch.collapse_changelog = false;
  patch.hide_thinking_block = false;
  patch.enable_skill_commands = true;
  patch.allow_external_filesystem_access = false;
  patch.image_auto_resize = true;
  patch.terminal_show_progress = false;
  patch.terminal_prompt_history_limit = kDefaultPromptHistoryLimit;
  patch.terminal_tool_entry_compaction_enabled_tools =
      default_tool_entry_compaction_enabled_tools();
  patch.retry_enabled = true;
  patch.steering_mode = "one-at-a-time";
  patch.follow_up_mode = "one-at-a-time";
  patch.transport = "sse";
  patch.enabled_models = std::vector<std::string>{};
  patch.mcp_servers = std::vector<McpServerSettings>{};
  patch.ollama_instances = std::vector<ai::OllamaInstanceSettings>{};
  patch.prompt_roots = discovery.prompt_roots;
  patch.skill_roots = discovery.skill_roots;
  patch.theme_roots = default_theme_roots(repository, discovery, agent_home);
  patch.features.load_prompt_templates = true;
  patch.features.load_skills = true;
  patch.features.load_themes = true;
  patch.features.persist_sessions = true;
  patch.memory.enabled = false;
  patch.memory.automatic_recall = true;
  patch.memory.automatic_capture = true;
  patch.memory.recall_policy = "balanced";
  patch.memory.context_token_budget = 900;
  patch.memory.candidate_limit = 50;
  patch.memory.prehandler.model = std::optional<std::string>{};
  patch.memory.prehandler.thinking = "off";
  patch.memory.prehandler.timeout_ms = 30000;
  patch.memory.prehandler.fail_open = true;
  patch.memory.prehandler.max_model_input_tokens = 12000;
  patch.memory.capture.post_process = "turn_end";
  patch.memory.capture.default_status = "hypothesis";
  patch.memory.capture.max_evidence_chars = 32000;
  patch.memory.dreaming.enabled = false;
  patch.memory.dreaming.trigger = "post_turn";
  patch.memory.dreaming.interval_seconds = 3600;
  patch.memory.dreaming.idle_seconds = 300;
  patch.memory.dreaming.candidate_threshold = 1;
  patch.memory.dreaming.model = std::optional<std::string>{};
  patch.memory.dreaming.verifier_model = std::optional<std::string>{};
  patch.memory.dreaming.timeout_ms = 600000;
  patch.memory.dreaming.max_model_input_tokens = 6000;
  patch.memory.dreaming.max_candidates = 8;
  patch.memory.dreaming.research_policy = "disabled";
  patch.memory.dreaming.promotion_policy = "review_only";
  patch.memory.retrieval.graph_enabled = false;
  patch.memory.retrieval.dense_enabled = false;
  patch.memory.retrieval.code_enabled = false;
  patch.memory.retrieval.embedding_model = std::optional<std::string>{};
  patch.memory.retrieval.query_prefix = "";
  patch.memory.retrieval.document_prefix = "";
  patch.memory.retrieval.max_depth = 2;
  patch.memory.retrieval.max_seeds = 16;
  patch.memory.retrieval.max_edges = 256;
  patch.memory.retrieval.max_graph_results = 64;
  patch.memory.retrieval.max_fanout = 32;
  patch.memory.retrieval.query_cache_entries = 64;
  return patch;
}

AppSettings materialize_settings(const AppSettingsPatch& patch) {
  AppSettings settings;
  settings.cwd = patch.cwd.value_or("");
  settings.workspace_root = patch.workspace_root.value_or(settings.cwd);
  settings.session_dir = patch.session_dir.value_or("");
  settings.agent_home = patch.agent_home.value_or("");
  settings.default_provider = patch.default_provider;
  settings.default_model = patch.default_model;
  settings.default_thinking_level = patch.default_thinking_level;
  settings.theme = patch.theme.value_or("dark");
  settings.quiet_startup = patch.quiet_startup.value_or(false);
  settings.collapse_changelog = patch.collapse_changelog.value_or(false);
  settings.hide_thinking_block = patch.hide_thinking_block.value_or(false);
  settings.enable_skill_commands = patch.enable_skill_commands.value_or(true);
  settings.allow_external_filesystem_access =
      patch.allow_external_filesystem_access.value_or(false);
  settings.image_auto_resize = patch.image_auto_resize.value_or(true);
  settings.terminal_show_progress =
      patch.terminal_show_progress.value_or(false);
  settings.terminal_prompt_history_limit =
      patch.terminal_prompt_history_limit.value_or(kDefaultPromptHistoryLimit);
  settings.terminal_tool_entry_compaction_enabled_tools =
      patch.terminal_tool_entry_compaction_enabled_tools.value_or(
          default_tool_entry_compaction_enabled_tools());
  settings.retry_enabled = patch.retry_enabled.value_or(true);
  settings.provider_retry = patch.provider_retry;
  settings.steering_mode = patch.steering_mode.value_or("one-at-a-time");
  settings.follow_up_mode = patch.follow_up_mode.value_or("one-at-a-time");
  settings.transport = patch.transport.value_or("sse");
  settings.shell_path = patch.shell_path;
  settings.shell_command_prefix = patch.shell_command_prefix;
  settings.npm_command = patch.npm_command;
  settings.enabled_models = patch.enabled_models.value_or(std::vector<std::string>{});
  settings.mcp_servers =
      patch.mcp_servers.value_or(std::vector<McpServerSettings>{});
  settings.ollama_instances = patch.ollama_instances.value_or(
      std::vector<ai::OllamaInstanceSettings>{});
  settings.prompt_roots = patch.prompt_roots.value_or(std::vector<std::string>{});
  settings.skill_roots = patch.skill_roots.value_or(std::vector<std::string>{});
  settings.theme_roots = patch.theme_roots.value_or(std::vector<std::string>{});
  settings.resource_filters.prompt_templates =
      patch.prompt_template_filters.value_or(std::vector<std::string>{});
  settings.resource_filters.skills =
      patch.skill_filters.value_or(std::vector<std::string>{});
  settings.resource_filters.themes =
      patch.theme_filters.value_or(std::vector<std::string>{});
  settings.features.load_prompt_templates =
      patch.features.load_prompt_templates.value_or(true);
  settings.features.load_skills = patch.features.load_skills.value_or(true);
  settings.features.load_themes = patch.features.load_themes.value_or(true);
  settings.features.persist_sessions =
      patch.features.persist_sessions.value_or(true);
  settings.memory.enabled = patch.memory.enabled.value_or(false);
  settings.memory.automatic_recall =
      patch.memory.automatic_recall.value_or(true);
  settings.memory.automatic_capture =
      patch.memory.automatic_capture.value_or(true);
  settings.memory.recall_policy =
      patch.memory.recall_policy.value_or("balanced");
  settings.memory.context_token_budget =
      patch.memory.context_token_budget.value_or(900);
  settings.memory.candidate_limit = patch.memory.candidate_limit.value_or(50);
  settings.memory.prehandler.model = patch.memory.prehandler.model
                                         ? *patch.memory.prehandler.model
                                         : std::optional<std::string>{};
  settings.memory.prehandler.thinking =
      patch.memory.prehandler.thinking.value_or("off");
  settings.memory.prehandler.timeout_ms =
      patch.memory.prehandler.timeout_ms.value_or(30000);
  settings.memory.prehandler.fail_open =
      patch.memory.prehandler.fail_open.value_or(true);
  settings.memory.prehandler.max_model_input_tokens =
      patch.memory.prehandler.max_model_input_tokens.value_or(12000);
  settings.memory.capture.post_process =
      patch.memory.capture.post_process.value_or("turn_end");
  settings.memory.capture.default_status =
      patch.memory.capture.default_status.value_or("hypothesis");
  settings.memory.capture.max_evidence_chars =
      patch.memory.capture.max_evidence_chars.value_or(32000);
  settings.memory.dreaming.enabled =
      patch.memory.dreaming.enabled.value_or(false);
  settings.memory.dreaming.trigger =
      patch.memory.dreaming.trigger.value_or("post_turn");
  settings.memory.dreaming.interval_seconds =
      patch.memory.dreaming.interval_seconds.value_or(3600);
  settings.memory.dreaming.idle_seconds =
      patch.memory.dreaming.idle_seconds.value_or(300);
  settings.memory.dreaming.candidate_threshold =
      patch.memory.dreaming.candidate_threshold.value_or(1);
  settings.memory.dreaming.model = patch.memory.dreaming.model
                                       ? *patch.memory.dreaming.model
                                       : std::optional<std::string>{};
  settings.memory.dreaming.verifier_model = patch.memory.dreaming.verifier_model
                                                ? *patch.memory.dreaming.verifier_model
                                                : std::optional<std::string>{};
  settings.memory.dreaming.timeout_ms =
      patch.memory.dreaming.timeout_ms.value_or(600000);
  settings.memory.dreaming.max_model_input_tokens =
      patch.memory.dreaming.max_model_input_tokens.value_or(6000);
  settings.memory.dreaming.max_candidates =
      patch.memory.dreaming.max_candidates.value_or(8);
  settings.memory.dreaming.research_policy =
      patch.memory.dreaming.research_policy.value_or("disabled");
  settings.memory.dreaming.promotion_policy =
      patch.memory.dreaming.promotion_policy.value_or("review_only");
  settings.memory.retrieval.graph_enabled =
      patch.memory.retrieval.graph_enabled.value_or(false);
  settings.memory.retrieval.dense_enabled =
      patch.memory.retrieval.dense_enabled.value_or(false);
  settings.memory.retrieval.code_enabled =
      patch.memory.retrieval.code_enabled.value_or(false);
  settings.memory.retrieval.embedding_model = patch.memory.retrieval.embedding_model
      ? *patch.memory.retrieval.embedding_model : std::optional<std::string>{};
  settings.memory.retrieval.query_prefix =
      patch.memory.retrieval.query_prefix.value_or("");
  settings.memory.retrieval.document_prefix =
      patch.memory.retrieval.document_prefix.value_or("");
  settings.memory.retrieval.max_depth = patch.memory.retrieval.max_depth.value_or(2);
  settings.memory.retrieval.max_seeds = patch.memory.retrieval.max_seeds.value_or(16);
  settings.memory.retrieval.max_edges = patch.memory.retrieval.max_edges.value_or(256);
  settings.memory.retrieval.max_graph_results =
      patch.memory.retrieval.max_graph_results.value_or(64);
  settings.memory.retrieval.max_fanout = patch.memory.retrieval.max_fanout.value_or(32);
  settings.memory.retrieval.query_cache_entries =
      patch.memory.retrieval.query_cache_entries.value_or(64);
  settings.llama = patch.llama.value_or(qt::ai::LlamaSettings{});
  return settings;
}

Result<AppSettings> validate_settings(const AppSettingsPatch& patch) {
  if (!patch.cwd || patch.cwd->empty()) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        "resolved settings must include cwd");
  }

  if (!patch.workspace_root || patch.workspace_root->empty()) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument, "resolved settings must include workspace_root");
  }

  if (!patch.session_dir || patch.session_dir->empty()) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument, "resolved settings must include session_dir");
  }

  if (!patch.agent_home || patch.agent_home->empty()) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument, "resolved settings must include agent_home");
  }

  const auto validate_choice = [](const std::optional<std::string>& value,
                                  const char* field,
                                  const std::vector<std::string>& allowed)
      -> std::optional<std::string> {
    if (!value || value->empty() ||
        std::find(allowed.begin(), allowed.end(), *value) != allowed.end()) {
      return std::nullopt;
    }
    return std::string("resolved settings ") + field +
           " has unsupported value: " + *value;
  };

  const auto steering =
      validate_choice(patch.steering_mode, "steering_mode", {"all", "one-at-a-time"});
  if (steering) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument, *steering);
  }
  const auto follow_up =
      validate_choice(patch.follow_up_mode, "follow_up_mode", {"all", "one-at-a-time"});
  if (follow_up) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument, *follow_up);
  }
  const auto transport =
      validate_choice(patch.transport, "transport", {"sse", "websocket", "auto"});
  if (transport) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument, *transport);
  }

  const auto recall_policy = validate_choice(
      patch.memory.recall_policy, "memory.recallPolicy",
      {"strict", "balanced", "exploratory", "debugging", "architecture"});
  if (recall_policy) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        *recall_policy);
  }
  const auto prehandler_thinking = validate_choice(
      patch.memory.prehandler.thinking, "memory.prehandler.thinking",
      {"off", "minimal", "low", "medium", "high", "xhigh"});
  if (prehandler_thinking) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        *prehandler_thinking);
  }
  const auto post_process = validate_choice(
      patch.memory.capture.post_process, "memory.capture.postProcess",
      {"turn_end", "session_end", "disabled"});
  if (post_process) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        *post_process);
  }
  const auto default_status = validate_choice(
      patch.memory.capture.default_status, "memory.capture.defaultStatus",
      {"candidate", "hypothesis"});
  if (default_status) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        *default_status);
  }
  const auto dreaming_trigger = validate_choice(
      patch.memory.dreaming.trigger, "memory.dreaming.trigger",
      {"post_turn", "interval", "candidate_threshold"});
  if (dreaming_trigger) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        *dreaming_trigger);
  }
  const auto dreaming_research = validate_choice(
      patch.memory.dreaming.research_policy,
      "memory.dreaming.researchPolicy", {"disabled", "manual", "required"});
  if (dreaming_research) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        *dreaming_research);
  }
  const auto dreaming_promotion = validate_choice(
      patch.memory.dreaming.promotion_policy,
      "memory.dreaming.promotionPolicy",
      {"review_only", "manual", "auto_validate", "disabled"});
  if (dreaming_promotion) {
    return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                        *dreaming_promotion);
  }
  if (patch.memory.prehandler.model &&
      patch.memory.prehandler.model->has_value() &&
      patch.memory.prehandler.model->value().empty()) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.prehandler.model must not be empty");
  }
  if (patch.memory.dreaming.model &&
      patch.memory.dreaming.model->has_value() &&
      patch.memory.dreaming.model->value().empty()) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.dreaming.model must not be empty");
  }
  if (patch.memory.dreaming.verifier_model &&
      patch.memory.dreaming.verifier_model->has_value()) {
    const std::string& verifier_model =
        patch.memory.dreaming.verifier_model->value();
    if (verifier_model.empty() || lower_copy(verifier_model) == "auto") {
      return Result<AppSettings>::failure(
          ErrorCode::invalid_argument,
          "resolved settings memory.dreaming.verifierModel must not be empty or auto");
    }
  }
  if (patch.memory.context_token_budget &&
      *patch.memory.context_token_budget == 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.contextTokenBudget must be positive");
  }
  if (patch.memory.candidate_limit && *patch.memory.candidate_limit == 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.candidateLimit must be positive");
  }
  if (patch.memory.retrieval.embedding_model &&
      patch.memory.retrieval.embedding_model->has_value() &&
      patch.memory.retrieval.embedding_model->value().empty()) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.retrieval.embeddingModel must not be empty");
  }
  const auto validate_retrieval_bound = [&](const std::optional<std::size_t>& value,
                                            std::size_t cap,
                                            const char* field) -> Result<bool> {
    if (value && (*value == 0 || *value > cap)) {
      return Result<bool>::failure(
          ErrorCode::invalid_argument,
          std::string("resolved settings ") + field + " must be between 1 and " +
              std::to_string(cap));
    }
    return true;
  };
  for (const auto& result :
       {validate_retrieval_bound(patch.memory.retrieval.max_depth, 8,
                                 "memory.retrieval.maxDepth"),
        validate_retrieval_bound(patch.memory.retrieval.max_seeds, 128,
                                 "memory.retrieval.maxSeeds"),
        validate_retrieval_bound(patch.memory.retrieval.max_edges, 8192,
                                 "memory.retrieval.maxEdges"),
        validate_retrieval_bound(patch.memory.retrieval.max_graph_results, 1000,
                                 "memory.retrieval.maxGraphResults"),
        validate_retrieval_bound(patch.memory.retrieval.max_fanout, 256,
                                 "memory.retrieval.maxFanout"),
        validate_retrieval_bound(patch.memory.retrieval.query_cache_entries, 1024,
                                 "memory.retrieval.queryCacheEntries")}) {
    if (!result) return result.error();
  }
  if (patch.memory.prehandler.timeout_ms &&
      *patch.memory.prehandler.timeout_ms <= 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.prehandler.timeoutMs must be positive");
  }
  if (patch.memory.prehandler.max_model_input_tokens &&
      *patch.memory.prehandler.max_model_input_tokens == 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.prehandler.maxModelInputTokens must be positive");
  }
  if (patch.memory.capture.max_evidence_chars &&
      *patch.memory.capture.max_evidence_chars == 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.capture.maxEvidenceChars must be positive");
  }
  if (patch.memory.dreaming.interval_seconds &&
      (*patch.memory.dreaming.interval_seconds == 0 ||
       *patch.memory.dreaming.interval_seconds > 31536000)) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.dreaming.intervalSeconds must be between 1 and 31536000");
  }
  if (patch.memory.dreaming.idle_seconds &&
      (*patch.memory.dreaming.idle_seconds == 0 ||
       *patch.memory.dreaming.idle_seconds > 86400)) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.dreaming.idleSeconds must be between 1 and 86400");
  }
  if (patch.memory.dreaming.candidate_threshold &&
      (*patch.memory.dreaming.candidate_threshold == 0 ||
       *patch.memory.dreaming.candidate_threshold > 1000)) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.dreaming.candidateThreshold must be between 1 and 1000");
  }
  if (patch.memory.dreaming.timeout_ms &&
      (*patch.memory.dreaming.timeout_ms <= 0 ||
       *patch.memory.dreaming.timeout_ms > 600000)) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.dreaming.timeoutMs must be between 1 and 600000");
  }
  if (patch.memory.dreaming.max_model_input_tokens &&
      (*patch.memory.dreaming.max_model_input_tokens == 0 ||
       *patch.memory.dreaming.max_model_input_tokens > 6000)) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.dreaming.maxModelInputTokens must be between 1 and 6000");
  }
  if (patch.memory.dreaming.max_candidates &&
      (*patch.memory.dreaming.max_candidates < 2 ||
       *patch.memory.dreaming.max_candidates > 8)) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings memory.dreaming.maxCandidates must be between 2 and 8");
  }

  if (patch.llama) {
    const auto llama_errors = qt::ai::validate(*patch.llama);
    if (!llama_errors.empty()) {
      return Result<AppSettings>::failure(ErrorCode::invalid_argument,
                                          llama_errors.front());
    }
  }

  if (patch.provider_retry.timeout_ms && *patch.provider_retry.timeout_ms < 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings retry.provider.timeoutMs must be non-negative");
  }
  if (patch.provider_retry.max_retries && *patch.provider_retry.max_retries < 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings retry.provider.maxRetries must be non-negative");
  }
  if (patch.provider_retry.max_retry_delay_ms &&
      *patch.provider_retry.max_retry_delay_ms < 0) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "resolved settings retry.provider.maxRetryDelayMs must be non-negative");
  }

  return materialize_settings(patch);
}

std::optional<std::string> maybe_existing_settings_path(
    const qt::harness::RepositoryView& repository, const std::string& directory) {
  for (const auto* filename :
       {kSettingsFilename, kYamlSettingsFilename, kYmlSettingsFilename}) {
    const std::string path = path_to_string(Path(directory) / filename);
    if (repository.file_exists(path)) {
      return path;
    }
  }
  return std::nullopt;
}

std::vector<std::string> existing_mcp_config_paths(
    const qt::harness::RepositoryView& repository, const std::string& agent_home,
    const std::optional<std::string>& workspace_root) {
  std::vector<std::string> paths;
  const auto append_if_file = [&](const Path& path) {
    const std::string value = path_to_string(path);
    if (repository.file_exists(value) &&
        std::find(paths.begin(), paths.end(), value) == paths.end()) {
      paths.push_back(value);
    }
  };
  if (const auto home = getenv_string("HOME")) {
    append_if_file(Path(*home) / ".config" / "mcp" / "mcp.json");
  }
  append_if_file(Path(agent_home) / "mcp.json");
  if (workspace_root && !workspace_root->empty()) {
    append_if_file(Path(*workspace_root) / ".mcp.json");
    append_if_file(Path(*workspace_root) / ".pi" / "mcp.json");
  }
  return paths;
}

Result<AppSettingsPatch> resolve_override_patch(
    const qt::harness::RepositoryView& repository, const AppSettingsPatch& patch,
    const std::string& base_directory) {
  Json json = Json::object();
  if (patch.cwd) {
    json["cwd"] = *patch.cwd;
  }
  if (patch.workspace_root) {
    json["workspaceRoot"] = *patch.workspace_root;
  }
  if (patch.session_dir) {
    json["sessionDir"] = *patch.session_dir;
  }
  if (patch.agent_home) {
    json["agentHome"] = *patch.agent_home;
  }
  if (patch.default_provider) {
    json["defaultProvider"] = *patch.default_provider;
  }
  if (patch.default_model) {
    json["defaultModel"] = *patch.default_model;
  }
  if (patch.default_thinking_level) {
    json["defaultThinkingLevel"] = *patch.default_thinking_level;
  }
  if (patch.theme) {
    json["theme"] = *patch.theme;
  }
  if (patch.quiet_startup) {
    json["quietStartup"] = *patch.quiet_startup;
  }
  if (patch.collapse_changelog) {
    json["collapseChangelog"] = *patch.collapse_changelog;
  }
  if (patch.hide_thinking_block) {
    json["hideThinkingBlock"] = *patch.hide_thinking_block;
  }
  if (patch.enable_skill_commands) {
    json["enableSkillCommands"] = *patch.enable_skill_commands;
  }
  if (patch.allow_external_filesystem_access) {
    json["allowExternalFilesystemAccess"] =
        *patch.allow_external_filesystem_access;
  }
  if (patch.image_auto_resize) {
    json["images"] = Json{{"autoResize", *patch.image_auto_resize}};
  }
  if (patch.terminal_show_progress || patch.terminal_prompt_history_limit ||
      patch.terminal_tool_entry_compaction_enabled_tools) {
    Json terminal = Json::object();
    if (patch.terminal_show_progress) {
      terminal["showTerminalProgress"] = *patch.terminal_show_progress;
    }
    if (patch.terminal_prompt_history_limit) {
      terminal["promptHistoryLimit"] =
          *patch.terminal_prompt_history_limit;
    }
    if (patch.terminal_tool_entry_compaction_enabled_tools) {
      terminal["toolEntryCompactionEnabledTools"] =
          *patch.terminal_tool_entry_compaction_enabled_tools;
    }
    json["terminal"] = std::move(terminal);
  }
  if (patch.retry_enabled || !patch.provider_retry.empty()) {
    Json retry = Json::object();
    if (patch.retry_enabled) {
      retry["enabled"] = *patch.retry_enabled;
    }
    if (!patch.provider_retry.empty()) {
      retry["provider"] = provider_retry_settings_to_json(patch.provider_retry);
    }
    json["retry"] = std::move(retry);
  }
  if (patch.steering_mode) {
    json["steeringMode"] = *patch.steering_mode;
  }
  if (patch.follow_up_mode) {
    json["followUpMode"] = *patch.follow_up_mode;
  }
  if (patch.transport) {
    json["transport"] = *patch.transport;
  }
  if (patch.shell_path) {
    json["shellPath"] = *patch.shell_path;
  }
  if (patch.shell_command_prefix) {
    json["shellCommandPrefix"] = *patch.shell_command_prefix;
  }
  if (patch.npm_command) {
    json["npmCommand"] = *patch.npm_command;
  }
  if (patch.enabled_models) {
    json["enabledModels"] = *patch.enabled_models;
  }
  if (patch.mcp_servers) {
    Json servers = Json::object();
    for (const auto& server : *patch.mcp_servers) {
      Json entry = Json::object();
      entry["enabled"] = server.enabled;
      entry["transport"] = server.transport;
      if (server.command) {
        entry["command"] = *server.command;
      }
      if (!server.args.empty()) {
        entry["args"] = server.args;
      }
      if (!server.env.empty()) {
        entry["env"] = server.env;
      }
      if (server.url) {
        entry["url"] = *server.url;
      }
      if (!server.headers.empty()) {
        entry["headers"] = server.headers;
      }
      if (!server.sampling.empty()) {
        entry["sampling"] = server.sampling;
      }
      entry["timeoutSeconds"] = server.timeout_seconds;
      entry["connectTimeoutSeconds"] = server.connect_timeout_seconds;
      servers[server.name] = std::move(entry);
    }
    json["mcpServers"] = std::move(servers);
  }
  if (patch.ollama_instances) {
    Json instances = Json::array();
    for (const auto& instance : *patch.ollama_instances) {
      Json value{{"id", instance.id},
                 {"name", instance.name},
                 {"baseUrl", instance.base_url},
                 {"enabled", instance.enabled},
                 {"timeoutMs", instance.timeout_ms}};
      if (instance.api_key) {
        value["apiKey"] = *instance.api_key;
      }
      value["models"] = Json::array();
      for (const auto& model : instance.models) {
        value["models"].push_back(
            Json{{"name", model.name},
                 {"modifiedAt", model.modified_at},
                 {"digest", model.digest},
                 {"size", model.size},
                 {"capabilities", model.capabilities},
                 {"contextWindow", model.context_window},
                 {"maxTokens", model.max_tokens},
                 {"details", model.details}});
      }
      instances.push_back(std::move(value));
    }
    json["ollama"] = Json{{"instances", std::move(instances)}};
  }
  if (patch.prompt_roots) {
    Json values = *patch.prompt_roots;
    if (patch.prompt_template_filters) {
      for (const auto& filter : *patch.prompt_template_filters) {
        values.push_back(filter);
      }
    }
    json["promptRoots"] = std::move(values);
  }
  if (patch.skill_roots) {
    Json values = *patch.skill_roots;
    if (patch.skill_filters) {
      for (const auto& filter : *patch.skill_filters) {
        values.push_back(filter);
      }
    }
    json["skillRoots"] = std::move(values);
  }
  if (patch.theme_roots) {
    Json values = *patch.theme_roots;
    if (patch.theme_filters) {
      for (const auto& filter : *patch.theme_filters) {
        values.push_back(filter);
      }
    }
    json["themeRoots"] = std::move(values);
  }
  Json features = Json::object();
  if (patch.features.load_prompt_templates) {
    features["loadPromptTemplates"] = *patch.features.load_prompt_templates;
  }
  if (patch.features.load_skills) {
    features["loadSkills"] = *patch.features.load_skills;
  }
  if (patch.features.load_themes) {
    features["loadThemes"] = *patch.features.load_themes;
  }
  if (patch.features.persist_sessions) {
    features["persistSessions"] = *patch.features.persist_sessions;
  }
  if (!features.empty()) {
    json["features"] = std::move(features);
  }
  if (!patch.memory.empty()) {
    json["memory"] = memory_settings_patch_to_json(patch.memory);
  }
  if (patch.llama) {
    json["llama"] = *patch.llama;
  }

  return parse_settings_patch(repository, json, base_directory);
}

}  // namespace

bool FeatureSettingsPatch::empty() const noexcept {
  return !load_prompt_templates.has_value() && !load_skills.has_value() &&
         !load_themes.has_value() && !persist_sessions.has_value();
}

bool ProviderRetrySettings::empty() const noexcept {
  return !timeout_ms.has_value() && !max_retries.has_value() &&
         !max_retry_delay_ms.has_value();
}

bool MemoryPrehandlerSettingsPatch::empty() const noexcept {
  return !model.has_value() && !thinking.has_value() &&
         !timeout_ms.has_value() && !fail_open.has_value() &&
         !max_model_input_tokens.has_value();
}

bool MemoryCaptureSettingsPatch::empty() const noexcept {
  return !post_process.has_value() && !default_status.has_value() &&
         !max_evidence_chars.has_value();
}

bool MemoryDreamingSettingsPatch::empty() const noexcept {
  return !enabled.has_value() && !trigger.has_value() &&
      !interval_seconds.has_value() && !idle_seconds.has_value() &&
         !candidate_threshold.has_value() && !model.has_value() &&
         !verifier_model.has_value() &&
         !timeout_ms.has_value() &&
         !max_model_input_tokens.has_value() && !max_candidates.has_value() &&
         !research_policy.has_value() && !promotion_policy.has_value();
}

bool MemoryRetrievalSettingsPatch::empty() const noexcept {
  return !graph_enabled.has_value() && !dense_enabled.has_value() &&
         !code_enabled.has_value() && !embedding_model.has_value() &&
         !query_prefix.has_value() && !document_prefix.has_value() &&
         !max_depth.has_value() && !max_seeds.has_value() &&
         !max_edges.has_value() && !max_graph_results.has_value() &&
         !max_fanout.has_value() && !query_cache_entries.has_value();
}

bool MemorySettingsPatch::empty() const noexcept {
  return !enabled.has_value() && !automatic_recall.has_value() &&
         !automatic_capture.has_value() && !recall_policy.has_value() &&
         !context_token_budget.has_value() && !candidate_limit.has_value() &&
         prehandler.empty() && capture.empty() && dreaming.empty() &&
         retrieval.empty();
}

bool AppSettingsPatch::empty() const noexcept {
  return !cwd.has_value() && !workspace_root.has_value() &&
         !session_dir.has_value() && !agent_home.has_value() &&
         !default_provider.has_value() && !default_model.has_value() &&
         !default_thinking_level.has_value() && !theme.has_value() &&
         !quiet_startup.has_value() && !collapse_changelog.has_value() &&
         !hide_thinking_block.has_value() &&
         !enable_skill_commands.has_value() &&
         !allow_external_filesystem_access.has_value() &&
         !image_auto_resize.has_value() &&
         !terminal_show_progress.has_value() &&
         !terminal_prompt_history_limit.has_value() &&
         !terminal_tool_entry_compaction_enabled_tools.has_value() &&
         !retry_enabled.has_value() && provider_retry.empty() &&
         !steering_mode.has_value() &&
         !follow_up_mode.has_value() && !transport.has_value() &&
         !shell_path.has_value() && !shell_command_prefix.has_value() &&
         !npm_command.has_value() && !enabled_models.has_value() &&
         !mcp_servers.has_value() &&
         !ollama_instances.has_value() &&
         !prompt_roots.has_value() && !skill_roots.has_value() &&
         !theme_roots.has_value() && !prompt_template_filters.has_value() &&
         !skill_filters.has_value() && !theme_filters.has_value() &&
         features.empty() && memory.empty() && !llama.has_value();
}

AppSettingsPatch merge_settings_patches(const AppSettingsPatch& base,
                                        const AppSettingsPatch& overlay) {
  AppSettingsPatch merged = base;

  if (overlay.cwd) {
    merged.cwd = overlay.cwd;
  }
  if (overlay.workspace_root) {
    merged.workspace_root = overlay.workspace_root;
  }
  if (overlay.session_dir) {
    merged.session_dir = overlay.session_dir;
  }
  if (overlay.agent_home) {
    merged.agent_home = overlay.agent_home;
  }
  if (overlay.default_provider) {
    merged.default_provider = overlay.default_provider;
  }
  if (overlay.default_model) {
    merged.default_model = overlay.default_model;
  }
  if (overlay.default_thinking_level) {
    merged.default_thinking_level = overlay.default_thinking_level;
  }
  if (overlay.theme) {
    merged.theme = overlay.theme;
  }
  if (overlay.quiet_startup) {
    merged.quiet_startup = overlay.quiet_startup;
  }
  if (overlay.collapse_changelog) {
    merged.collapse_changelog = overlay.collapse_changelog;
  }
  if (overlay.hide_thinking_block) {
    merged.hide_thinking_block = overlay.hide_thinking_block;
  }
  if (overlay.enable_skill_commands) {
    merged.enable_skill_commands = overlay.enable_skill_commands;
  }
  if (overlay.allow_external_filesystem_access) {
    merged.allow_external_filesystem_access =
        overlay.allow_external_filesystem_access;
  }
  if (overlay.image_auto_resize) {
    merged.image_auto_resize = overlay.image_auto_resize;
  }
  if (overlay.terminal_show_progress) {
    merged.terminal_show_progress = overlay.terminal_show_progress;
  }
  if (overlay.terminal_prompt_history_limit) {
    merged.terminal_prompt_history_limit =
        overlay.terminal_prompt_history_limit;
  }
  if (overlay.terminal_tool_entry_compaction_enabled_tools) {
    merged.terminal_tool_entry_compaction_enabled_tools =
        overlay.terminal_tool_entry_compaction_enabled_tools;
  }
  if (overlay.retry_enabled) {
    merged.retry_enabled = overlay.retry_enabled;
  }
  if (overlay.provider_retry.timeout_ms) {
    merged.provider_retry.timeout_ms = overlay.provider_retry.timeout_ms;
  }
  if (overlay.provider_retry.max_retries) {
    merged.provider_retry.max_retries = overlay.provider_retry.max_retries;
  }
  if (overlay.provider_retry.max_retry_delay_ms) {
    merged.provider_retry.max_retry_delay_ms =
        overlay.provider_retry.max_retry_delay_ms;
  }
  if (overlay.steering_mode) {
    merged.steering_mode = overlay.steering_mode;
  }
  if (overlay.follow_up_mode) {
    merged.follow_up_mode = overlay.follow_up_mode;
  }
  if (overlay.transport) {
    merged.transport = overlay.transport;
  }
  if (overlay.shell_path) {
    merged.shell_path = overlay.shell_path;
  }
  if (overlay.shell_command_prefix) {
    merged.shell_command_prefix = overlay.shell_command_prefix;
  }
  if (overlay.npm_command) {
    merged.npm_command = overlay.npm_command;
  }
  if (overlay.enabled_models) {
    merged.enabled_models = overlay.enabled_models;
  }
  if (overlay.mcp_servers) {
    std::map<std::string, McpServerSettings> by_name;
    if (merged.mcp_servers) {
      for (const auto& server : *merged.mcp_servers) {
        by_name[server.name] = server;
      }
    }
    for (const auto& server : *overlay.mcp_servers) {
      by_name[server.name] = server;
    }
    std::vector<McpServerSettings> servers;
    servers.reserve(by_name.size());
    for (auto& item : by_name) {
      servers.push_back(std::move(item.second));
    }
    merged.mcp_servers = std::move(servers);
  }
  if (overlay.ollama_instances) {
    std::map<std::string, ai::OllamaInstanceSettings> by_id;
    if (merged.ollama_instances) {
      for (const auto& instance : *merged.ollama_instances) {
        by_id[instance.id] = instance;
      }
    }
    for (const auto& instance : *overlay.ollama_instances) {
      by_id[instance.id] = instance;
    }
    std::vector<ai::OllamaInstanceSettings> instances;
    instances.reserve(by_id.size());
    for (auto& item : by_id) {
      instances.push_back(std::move(item.second));
    }
    merged.ollama_instances = std::move(instances);
  }
  if (overlay.prompt_roots) {
    merged.prompt_roots = overlay.prompt_roots;
  }
  if (overlay.skill_roots) {
    merged.skill_roots = overlay.skill_roots;
  }
  if (overlay.theme_roots) {
    merged.theme_roots = overlay.theme_roots;
  }
  if (overlay.prompt_template_filters) {
    merged.prompt_template_filters = overlay.prompt_template_filters;
  }
  if (overlay.skill_filters) {
    merged.skill_filters = overlay.skill_filters;
  }
  if (overlay.theme_filters) {
    merged.theme_filters = overlay.theme_filters;
  }
  if (overlay.features.load_prompt_templates) {
    merged.features.load_prompt_templates = overlay.features.load_prompt_templates;
  }
  if (overlay.features.load_skills) {
    merged.features.load_skills = overlay.features.load_skills;
  }
  if (overlay.features.load_themes) {
    merged.features.load_themes = overlay.features.load_themes;
  }
  if (overlay.features.persist_sessions) {
    merged.features.persist_sessions = overlay.features.persist_sessions;
  }
  if (overlay.memory.enabled) {
    merged.memory.enabled = overlay.memory.enabled;
  }
  if (overlay.memory.automatic_recall) {
    merged.memory.automatic_recall = overlay.memory.automatic_recall;
  }
  if (overlay.memory.automatic_capture) {
    merged.memory.automatic_capture = overlay.memory.automatic_capture;
  }
  if (overlay.memory.recall_policy) {
    merged.memory.recall_policy = overlay.memory.recall_policy;
  }
  if (overlay.memory.context_token_budget) {
    merged.memory.context_token_budget = overlay.memory.context_token_budget;
  }
  if (overlay.memory.candidate_limit) {
    merged.memory.candidate_limit = overlay.memory.candidate_limit;
  }
  if (overlay.memory.prehandler.model) {
    merged.memory.prehandler.model = overlay.memory.prehandler.model;
  }
  if (overlay.memory.prehandler.thinking) {
    merged.memory.prehandler.thinking = overlay.memory.prehandler.thinking;
  }
  if (overlay.memory.prehandler.timeout_ms) {
    merged.memory.prehandler.timeout_ms = overlay.memory.prehandler.timeout_ms;
  }
  if (overlay.memory.prehandler.fail_open) {
    merged.memory.prehandler.fail_open = overlay.memory.prehandler.fail_open;
  }
  if (overlay.memory.prehandler.max_model_input_tokens) {
    merged.memory.prehandler.max_model_input_tokens =
        overlay.memory.prehandler.max_model_input_tokens;
  }
  if (overlay.memory.capture.post_process) {
    merged.memory.capture.post_process = overlay.memory.capture.post_process;
  }
  if (overlay.memory.capture.default_status) {
    merged.memory.capture.default_status = overlay.memory.capture.default_status;
  }
  if (overlay.memory.capture.max_evidence_chars) {
    merged.memory.capture.max_evidence_chars =
        overlay.memory.capture.max_evidence_chars;
  }
  if (overlay.memory.dreaming.enabled) {
    merged.memory.dreaming.enabled = overlay.memory.dreaming.enabled;
  }
  if (overlay.memory.dreaming.trigger) {
    merged.memory.dreaming.trigger = overlay.memory.dreaming.trigger;
  }
  if (overlay.memory.dreaming.interval_seconds) {
    merged.memory.dreaming.interval_seconds =
        overlay.memory.dreaming.interval_seconds;
  }
  if (overlay.memory.dreaming.idle_seconds) {
    merged.memory.dreaming.idle_seconds =
        overlay.memory.dreaming.idle_seconds;
  }
  if (overlay.memory.dreaming.candidate_threshold) {
    merged.memory.dreaming.candidate_threshold =
        overlay.memory.dreaming.candidate_threshold;
  }
  if (overlay.memory.dreaming.model) {
    merged.memory.dreaming.model = overlay.memory.dreaming.model;
  }
  if (overlay.memory.dreaming.verifier_model) {
    merged.memory.dreaming.verifier_model = overlay.memory.dreaming.verifier_model;
  }
  if (overlay.memory.dreaming.timeout_ms) {
    merged.memory.dreaming.timeout_ms = overlay.memory.dreaming.timeout_ms;
  }
  if (overlay.memory.dreaming.max_model_input_tokens) {
    merged.memory.dreaming.max_model_input_tokens =
        overlay.memory.dreaming.max_model_input_tokens;
  }
  if (overlay.memory.dreaming.max_candidates) {
    merged.memory.dreaming.max_candidates =
        overlay.memory.dreaming.max_candidates;
  }
  if (overlay.memory.dreaming.research_policy) {
    merged.memory.dreaming.research_policy =
        overlay.memory.dreaming.research_policy;
  }
  if (overlay.memory.dreaming.promotion_policy) {
    merged.memory.dreaming.promotion_policy =
        overlay.memory.dreaming.promotion_policy;
  }
  if (overlay.memory.retrieval.graph_enabled) {
    merged.memory.retrieval.graph_enabled = overlay.memory.retrieval.graph_enabled;
  }
  if (overlay.memory.retrieval.dense_enabled) {
    merged.memory.retrieval.dense_enabled = overlay.memory.retrieval.dense_enabled;
  }
  if (overlay.memory.retrieval.code_enabled) {
    merged.memory.retrieval.code_enabled = overlay.memory.retrieval.code_enabled;
  }
  if (overlay.memory.retrieval.embedding_model) {
    merged.memory.retrieval.embedding_model = overlay.memory.retrieval.embedding_model;
  }
  if (overlay.memory.retrieval.query_prefix) {
    merged.memory.retrieval.query_prefix = overlay.memory.retrieval.query_prefix;
  }
  if (overlay.memory.retrieval.document_prefix) {
    merged.memory.retrieval.document_prefix = overlay.memory.retrieval.document_prefix;
  }
  if (overlay.memory.retrieval.max_depth) {
    merged.memory.retrieval.max_depth = overlay.memory.retrieval.max_depth;
  }
  if (overlay.memory.retrieval.max_seeds) {
    merged.memory.retrieval.max_seeds = overlay.memory.retrieval.max_seeds;
  }
  if (overlay.memory.retrieval.max_edges) {
    merged.memory.retrieval.max_edges = overlay.memory.retrieval.max_edges;
  }
  if (overlay.memory.retrieval.max_graph_results) {
    merged.memory.retrieval.max_graph_results = overlay.memory.retrieval.max_graph_results;
  }
  if (overlay.memory.retrieval.max_fanout) {
    merged.memory.retrieval.max_fanout = overlay.memory.retrieval.max_fanout;
  }
  if (overlay.memory.retrieval.query_cache_entries) {
    merged.memory.retrieval.query_cache_entries = overlay.memory.retrieval.query_cache_entries;
  }
  if (overlay.llama) {
    merged.llama = merged.llama
                       ? qt::ai::merge_llama_settings(*merged.llama,
                                                      *overlay.llama)
                       : overlay.llama;
  }

  return merged;
}

namespace {

Result<AppSettingsPatch> merge_project_settings_patch(
    const AppSettingsPatch& global_policy, const AppSettingsPatch& project) {
  const auto reject = [](const std::string& field) {
    return Result<AppSettingsPatch>::failure(
        ErrorCode::invalid_argument,
        "project settings may not broaden global policy: " + field);
  };
  if (project.allow_external_filesystem_access.value_or(false) &&
      !global_policy.allow_external_filesystem_access.value_or(false)) {
    return reject("allowExternalFilesystemAccess");
  }
  if (project.memory.enabled.value_or(false) &&
      !global_policy.memory.enabled.value_or(false)) {
    return reject("memory.enabled");
  }
  if (project.memory.automatic_recall.value_or(false) &&
      !global_policy.memory.automatic_recall.value_or(true)) {
    return reject("memory.automaticRecall");
  }
  if (project.memory.automatic_capture.value_or(false) &&
      !global_policy.memory.automatic_capture.value_or(true)) {
    return reject("memory.automaticCapture");
  }
  if (project.memory.dreaming.enabled.value_or(false) &&
      !global_policy.memory.dreaming.enabled.value_or(false)) {
    return reject("memory.dreaming.enabled");
  }
  if (project.memory.context_token_budget &&
      global_policy.memory.context_token_budget &&
      *project.memory.context_token_budget >
          *global_policy.memory.context_token_budget) {
    return reject("memory.contextTokenBudget");
  }
  if (project.memory.candidate_limit && global_policy.memory.candidate_limit &&
      *project.memory.candidate_limit > *global_policy.memory.candidate_limit) {
    return reject("memory.candidateLimit");
  }
  if (project.memory.prehandler.max_model_input_tokens &&
      global_policy.memory.prehandler.max_model_input_tokens &&
      *project.memory.prehandler.max_model_input_tokens >
          *global_policy.memory.prehandler.max_model_input_tokens) {
    return reject("memory.prehandler.maxModelInputTokens");
  }
  if (project.memory.capture.max_evidence_chars &&
      global_policy.memory.capture.max_evidence_chars &&
      *project.memory.capture.max_evidence_chars >
          *global_policy.memory.capture.max_evidence_chars) {
    return reject("memory.capture.maxEvidenceChars");
  }
  if (project.memory.dreaming.trigger &&
      global_policy.memory.dreaming.trigger &&
      *project.memory.dreaming.trigger !=
          *global_policy.memory.dreaming.trigger) {
    return reject("memory.dreaming.trigger");
  }
  if (project.memory.dreaming.interval_seconds &&
      global_policy.memory.dreaming.interval_seconds &&
      *project.memory.dreaming.interval_seconds <
          *global_policy.memory.dreaming.interval_seconds) {
    return reject("memory.dreaming.intervalSeconds");
  }
  if (project.memory.dreaming.idle_seconds &&
      global_policy.memory.dreaming.idle_seconds &&
      *project.memory.dreaming.idle_seconds <
          *global_policy.memory.dreaming.idle_seconds) {
    return reject("memory.dreaming.idleSeconds");
  }
  if (project.memory.dreaming.candidate_threshold &&
      global_policy.memory.dreaming.candidate_threshold &&
      *project.memory.dreaming.candidate_threshold <
          *global_policy.memory.dreaming.candidate_threshold) {
    return reject("memory.dreaming.candidateThreshold");
  }
  if (project.memory.dreaming.model &&
      (!global_policy.memory.dreaming.model ||
       *project.memory.dreaming.model !=
           *global_policy.memory.dreaming.model)) {
    return reject("memory.dreaming.model");
  }
  if (project.memory.dreaming.verifier_model &&
      (!global_policy.memory.dreaming.verifier_model ||
       *project.memory.dreaming.verifier_model !=
           *global_policy.memory.dreaming.verifier_model)) {
    return reject("memory.dreaming.verifierModel");
  }
  if (project.memory.dreaming.timeout_ms &&
      global_policy.memory.dreaming.timeout_ms &&
      *project.memory.dreaming.timeout_ms >
          *global_policy.memory.dreaming.timeout_ms) {
    return reject("memory.dreaming.timeoutMs");
  }
  if (project.memory.dreaming.max_model_input_tokens &&
      global_policy.memory.dreaming.max_model_input_tokens &&
      *project.memory.dreaming.max_model_input_tokens >
          *global_policy.memory.dreaming.max_model_input_tokens) {
    return reject("memory.dreaming.maxModelInputTokens");
  }
  if (project.memory.dreaming.max_candidates &&
      global_policy.memory.dreaming.max_candidates &&
      *project.memory.dreaming.max_candidates >
          *global_policy.memory.dreaming.max_candidates) {
    return reject("memory.dreaming.maxCandidates");
  }

  const auto capture_scope = [](const std::string& post_process) {
    if (post_process == "disabled") {
      return 0;
    }
    if (post_process == "session_end") {
      return 1;
    }
    return 2;
  };
  if (project.memory.capture.post_process &&
      global_policy.memory.capture.post_process &&
      capture_scope(*project.memory.capture.post_process) >
          capture_scope(*global_policy.memory.capture.post_process)) {
    return reject("memory.capture.postProcess");
  }

  const auto recall_policy_scope = [](const std::string& policy) {
    if (policy == "strict") {
      return 0;
    }
    if (policy == "exploratory") {
      return 2;
    }
    return 1;
  };
  if (project.memory.recall_policy && global_policy.memory.recall_policy &&
      recall_policy_scope(*project.memory.recall_policy) >
          recall_policy_scope(*global_policy.memory.recall_policy)) {
    return reject("memory.recallPolicy");
  }

  const auto research_policy_scope = [](const std::string& policy) {
    if (policy == "disabled") {
      return 0;
    }
    if (policy == "manual") {
      return 1;
    }
    return 2;
  };
  if (project.memory.dreaming.research_policy &&
      global_policy.memory.dreaming.research_policy &&
      research_policy_scope(*project.memory.dreaming.research_policy) >
          research_policy_scope(
              *global_policy.memory.dreaming.research_policy)) {
    return reject("memory.dreaming.researchPolicy");
  }
  const auto promotion_policy_scope = [](const std::string& policy) {
    if (policy == "disabled") return 0;
    if (policy == "auto_validate") return 2;
    return 1;
  };
  if (project.memory.dreaming.promotion_policy &&
      global_policy.memory.dreaming.promotion_policy &&
      promotion_policy_scope(*project.memory.dreaming.promotion_policy) >
          promotion_policy_scope(
              *global_policy.memory.dreaming.promotion_policy)) {
    return reject("memory.dreaming.promotionPolicy");
  }

  return merge_settings_patches(global_policy, project);
}

}  // namespace

Result<LoadedSettingsDocument> load_settings_document(
    const qt::harness::RepositoryView& repository, const std::string& path) {
  const auto canonical = repository.canonicalize_path(path);
  if (!canonical) {
    return Result<LoadedSettingsDocument>::failure(
        ErrorCode::invalid_argument, "settings path could not be canonicalized: " + path);
  }

  const auto content = repository.read_text_file(*canonical);
  if (!content) {
    return Result<LoadedSettingsDocument>::failure(
        ErrorCode::not_found, "settings file could not be read: " + *canonical);
  }

  Json json;
  if (is_yaml_settings_path(*canonical)) {
    const auto parsed = parse_yaml_settings_json(content.value());
    if (!parsed) {
      return Result<LoadedSettingsDocument>::failure(
          parsed.error().code,
          "invalid settings YAML in " + *canonical + ": " + parsed.error().message);
    }
    const auto normalized = normalize_yaml_settings_json(parsed.value());
    if (!normalized) {
      return Result<LoadedSettingsDocument>::failure(
          normalized.error().code,
          "invalid settings in " + *canonical + ": " + normalized.error().message);
    }
    json = normalized.value();
  } else {
    try {
      json = Json::parse(content.value());
    } catch (const Json::parse_error& error) {
      return Result<LoadedSettingsDocument>::failure(
          ErrorCode::invalid_argument,
          "invalid settings JSON in " + *canonical + ": " + error.what());
    }
  }

  const Path canonical_path(*canonical);
  const auto patch =
      parse_settings_patch(repository, json, path_to_string(canonical_path.parent_path()));
  if (!patch) {
    return Result<LoadedSettingsDocument>::failure(
        patch.error().code,
        "invalid settings in " + *canonical + ": " + patch.error().message);
  }

  return LoadedSettingsDocument{*canonical, patch.value()};
}

foundation::Result<PersistedLlamaModelDiscovery> persist_discovered_llama_models(
    const std::string& agent_home, const std::string& cwd,
    const ai::LlamaModelDiscoveryResult& discovery) {
  PersistedLlamaModelDiscovery result;
  if (discovery.models.empty()) {
    return result;
  }

  const std::filesystem::path settings_directory =
      resolve_persisted_model_path(cwd, agent_home);
  const std::filesystem::path settings_path =
      settings_directory / kSettingsFilename;
  result.settings_path = settings_path.generic_string();

  const auto settings_json = read_settings_json_for_update(settings_path);
  if (!settings_json) {
    return Result<PersistedLlamaModelDiscovery>::failure(
        settings_json.error().code, settings_json.error().message,
        settings_json.error().system_error);
  }

  Json json = settings_json.value();
  Json& llama = json["llama"];
  if (llama.is_null()) {
    llama = Json::object();
  }
  if (!llama.is_object()) {
    return Result<PersistedLlamaModelDiscovery>::failure(
        ErrorCode::invalid_argument, "settings llama field must be an object");
  }

  Json& models = llama["models"];
  if (models.is_null()) {
    models = Json::array();
  }
  if (!models.is_array()) {
    return Result<PersistedLlamaModelDiscovery>::failure(
        ErrorCode::invalid_argument, "settings llama.models field must be an array");
  }

  std::set<std::string> used_ids;
  std::set<std::string> used_paths;
  for (const auto& item : models) {
    if (!item.is_object()) {
      continue;
    }
    if (const auto id = item.find("id"); id != item.end() && id->is_string()) {
      used_ids.insert(lowercase_ascii(id->get<std::string>()));
    }
    if (const auto path = item.find("path");
        path != item.end() && path->is_string()) {
      used_paths.insert(normalized_model_path_key(settings_directory.generic_string(),
                                                  path->get<std::string>()));
    }
  }

  for (const auto& discovered : discovery.models) {
    ai::LlamaModelSettings model =
        ai::llama_model_settings_from_discovery(discovered);
    model.path =
        resolve_persisted_model_path(cwd, model.path).generic_string();
    const std::string path_key =
        normalized_model_path_key(settings_directory.generic_string(), model.path);
    if (used_paths.find(path_key) != used_paths.end()) {
      ++result.skipped;
      result.diagnostics.push_back(
          "local model discovery skipped duplicate llama model path: " +
          model.path);
      continue;
    }
    model.id = unique_model_id(std::move(model.id), used_ids);
    used_paths.insert(path_key);
    models.push_back(model);
    ++result.added;
  }

  if (result.added == 0U) {
    return result;
  }

  std::error_code ec;
  std::filesystem::create_directories(settings_directory, ec);
  if (ec) {
    return Result<PersistedLlamaModelDiscovery>::failure(
        ErrorCode::io_error,
        "could not create settings directory " +
            settings_directory.generic_string() + ": " + ec.message());
  }

  const auto status =
      qt::foundation::write_text_file(settings_path,
                                      safe_pretty_json_dump(json) + "\n");
  if (!status.ok()) {
    return Result<PersistedLlamaModelDiscovery>::failure(
        status.error().code, status.error().message, status.error().system_error);
  }

  result.diagnostics.push_back(
      "saved " + std::to_string(result.added) +
      " discovered llama model(s) to " + result.settings_path);
  return result;
}

Result<AppSettings> load_app_settings(const qt::harness::RepositoryView& repository,
                                      const LoadSettingsRequest& request) {
  const std::string requested_cwd =
      request.cwd.empty() ? repository.cwd() : request.cwd;
  const auto canonical_cwd = repository.canonicalize_path(requested_cwd);
  if (!canonical_cwd) {
    return Result<AppSettings>::failure(
        ErrorCode::invalid_argument,
        "cwd could not be canonicalized: " + requested_cwd);
  }

  std::string agent_home;
  if (request.agent_home && !request.agent_home->empty()) {
    const auto resolved = canonicalize_setting_path(repository, *request.agent_home,
                                                    *canonical_cwd, "agentHome");
    if (!resolved) {
      return resolved.error();
    }
    agent_home = resolved.value();
  } else {
    const auto fallback = default_agent_home();
    if (!fallback) {
      return Result<AppSettings>::failure(
          ErrorCode::invalid_argument,
          "agentHome was not provided and HOME is not set");
    }
    const auto resolved = canonicalize_setting_path(repository, *fallback, *canonical_cwd,
                                                    "agentHome");
    if (!resolved) {
      return resolved.error();
    }
    agent_home = resolved.value();
  }

  qt::harness::RepoDiscoveryRequest discovery_request;
  discovery_request.cwd = *canonical_cwd;
  discovery_request.agent_home = agent_home;
  discovery_request.repository_root_hint = request.workspace_root;
  discovery_request.include_context_files = false;

  const auto discovery =
      qt::harness::discover_repo_context(repository, discovery_request);

  AppSettingsPatch merged = default_settings_patch(repository, discovery, agent_home);

  if (const auto global_path = maybe_existing_settings_path(repository, agent_home)) {
    const auto global = load_settings_document(repository, *global_path);
    if (!global) {
      return Result<AppSettings>::failure(global.error().code,
                                          global.error().message);
    }
    merged = merge_settings_patches(merged, global.value().patch);
  }

  std::optional<std::string> project_settings_path;
  if (request.project_settings_path && !request.project_settings_path->empty()) {
    const auto resolved = canonicalize_setting_path(repository,
                                                    *request.project_settings_path,
                                                    *canonical_cwd,
                                                    "projectSettingsPath");
    if (!resolved) {
      return resolved.error();
    }
    if (repository.file_exists(resolved.value())) {
      project_settings_path = resolved.value();
    }
  } else if (merged.workspace_root) {
    const auto candidate =
        maybe_existing_settings_path(repository, path_to_string(Path(*merged.workspace_root) /
                                                                ".pi"));
    if (candidate) {
      project_settings_path = *candidate;
    }
  }

  if (project_settings_path) {
    const auto project = load_settings_document(repository, *project_settings_path);
    if (!project) {
      return Result<AppSettings>::failure(project.error().code,
                                          project.error().message);
    }
    const auto project_merged =
        merge_project_settings_patch(merged, project.value().patch);
    if (!project_merged) {
      return Result<AppSettings>::failure(
          project_merged.error().code,
          "invalid settings in " + *project_settings_path + ": " +
              project_merged.error().message,
          project_merged.error().system_error);
    }
    merged = project_merged.value();
  }

  for (const auto& path :
       existing_mcp_config_paths(repository, agent_home, merged.workspace_root)) {
    const auto mcp_config = load_settings_document(repository, path);
    if (!mcp_config) {
      return Result<AppSettings>::failure(mcp_config.error().code,
                                          mcp_config.error().message);
    }
    auto mcp_patch = mcp_config.value().patch;
    mcp_patch.allow_external_filesystem_access.reset();
    merged = merge_settings_patches(merged, mcp_patch);
  }

  if (request.override_settings) {
    const auto override_patch =
        resolve_override_patch(repository, *request.override_settings, *canonical_cwd);
    if (!override_patch) {
      return Result<AppSettings>::failure(override_patch.error().code,
                                          override_patch.error().message);
    }
    merged = merge_settings_patches(merged, override_patch.value());
  }

  return validate_settings(merged);
}

}  // namespace qt::app
