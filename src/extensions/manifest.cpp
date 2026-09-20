#include <qt/extensions/manifest.hpp>

#include <set>
#include <sstream>
#include <stdexcept>

namespace qt::extensions {
namespace {

template <typename T>
void load_optional_string(const Json& json, const char* key,
                          std::optional<T>& target) {
  if (json.contains(key) && !json.at(key).is_null()) {
    target = json.at(key).get<T>();
  } else {
    target.reset();
  }
}

bool is_ascii_lowercase(char value) {
  return value >= 'a' && value <= 'z';
}

bool is_ascii_digit(char value) { return value >= '0' && value <= '9'; }

bool is_identifier_start(char value) {
  return is_ascii_lowercase(value) || is_ascii_digit(value);
}

bool is_identifier_body(char value) {
  return is_identifier_start(value) || value == '.' || value == '_' ||
         value == '-';
}

template <typename Contribution>
void validate_contributions(
    const std::vector<Contribution>& contributions, const char* collection_name,
    ManifestValidationResult& result) {
  std::set<std::string> names;
  for (std::size_t index = 0; index < contributions.size(); ++index) {
    const auto& contribution = contributions[index];
    const std::string prefix =
        std::string(collection_name) + "[" + std::to_string(index) + "]";

    if (!is_valid_extension_identifier(contribution.name)) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".name",
                       "must use lowercase letters, digits, '.', '_' or '-'");
    } else if (!names.insert(contribution.name).second) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".name", "must be unique within the manifest");
    }

    if (contribution.title.has_value() && contribution.title->empty()) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".title", "must not be empty when provided");
    }
  }
}

bool is_flag_type(std::string_view type) {
  return type == "boolean" || type == "string";
}

}  // namespace

std::string ManifestValidationIssue::summary() const {
  std::ostringstream stream;
  stream << qt::foundation::to_string(code);
  if (!field.empty()) {
    stream << " [" << field << "]";
  }
  if (!message.empty()) {
    stream << ": " << message;
  }
  return stream.str();
}

void ManifestValidationResult::add_issue(qt::foundation::ErrorCode code,
                                         std::string field,
                                         std::string message) {
  issues.push_back(ManifestValidationIssue{code, std::move(field),
                                           std::move(message)});
}

std::string to_string(ExtensionTransport transport) {
  switch (transport) {
    case ExtensionTransport::kSubprocessJsonRpc:
      return "subprocess_jsonrpc";
    case ExtensionTransport::kReferenceModule:
      return "reference_module";
    case ExtensionTransport::kAngelScript:
      return "angelscript";
  }

  return "subprocess_jsonrpc";
}

bool parse_extension_transport(std::string_view text,
                               ExtensionTransport& transport) {
  if (text == "subprocess_jsonrpc") {
    transport = ExtensionTransport::kSubprocessJsonRpc;
    return true;
  }
  if (text == "reference_module") {
    transport = ExtensionTransport::kReferenceModule;
    return true;
  }
  if (text == "angelscript") {
    transport = ExtensionTransport::kAngelScript;
    return true;
  }

  return false;
}

bool is_valid_extension_identifier(std::string_view text) {
  if (text.empty()) {
    return false;
  }

  if (!is_identifier_start(text.front())) {
    return false;
  }

  for (char value : text) {
    if (!is_identifier_body(value)) {
      return false;
    }
  }

  return true;
}

ManifestValidationResult validate_manifest(const ExtensionManifest& manifest) {
  ManifestValidationResult result;

  if (manifest.manifest_version != kExtensionManifestVersion) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                     "manifest_version", "unsupported manifest version");
  }

  if (!is_valid_extension_identifier(manifest.name)) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument, "name",
                     "must use lowercase letters, digits, '.', '_' or '-'");
  }

  if (manifest.display_name.has_value() && manifest.display_name->empty()) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                     "display_name", "must not be empty when provided");
  }

  if (manifest.transport == ExtensionTransport::kSubprocessJsonRpc &&
      manifest.executable.empty()) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument, "executable",
                     "must not be empty");
  }
  if (manifest.transport == ExtensionTransport::kReferenceModule &&
      (!manifest.entrypoint.has_value() || manifest.entrypoint->empty())) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument, "entrypoint",
                     "must not be empty");
  }
  if (manifest.transport == ExtensionTransport::kAngelScript &&
      (!manifest.entrypoint.has_value() || manifest.entrypoint->empty())) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument, "entrypoint",
                     "must not be empty");
  }

  for (std::size_t index = 0; index < manifest.args.size(); ++index) {
    if (manifest.args[index].empty()) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       "args[" + std::to_string(index) + "]",
                       "must not be empty");
    }
  }

  if (manifest.working_directory.has_value() &&
      manifest.working_directory->empty()) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                     "working_directory", "must not be empty when provided");
  }

  if (!manifest.metadata.is_object()) {
    result.add_issue(qt::foundation::ErrorCode::invalid_argument, "metadata",
                     "must be a JSON object");
  }

  validate_contributions(manifest.commands, "commands", result);
  validate_contributions(manifest.tools, "tools", result);
  std::set<std::string> flag_names;
  for (std::size_t index = 0; index < manifest.flags.size(); ++index) {
    const auto& flag = manifest.flags[index];
    const std::string prefix = "flags[" + std::to_string(index) + "]";
    if (!is_valid_extension_identifier(flag.name)) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".name",
                       "must use lowercase letters, digits, '.', '_' or '-'");
    } else if (!flag_names.insert(flag.name).second) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".name", "must be unique within the manifest");
    }
    if (!is_flag_type(flag.type)) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".type", "must be boolean or string");
    }
    if (!flag.default_value.is_null()) {
      const bool default_matches =
          (flag.type == "boolean" && flag.default_value.is_boolean()) ||
          (flag.type == "string" && flag.default_value.is_string());
      if (!default_matches) {
        result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                         prefix + ".default",
                         "must match the declared flag type");
      }
    }
  }
  std::set<std::string> provider_names;
  for (std::size_t index = 0; index < manifest.providers.size(); ++index) {
    const auto& provider = manifest.providers[index];
    const std::string prefix =
        "providers[" + std::to_string(index) + "]";
    if (!is_valid_extension_identifier(provider.name)) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".name",
                       "must use lowercase letters, digits, '.', '_' or '-'");
    } else if (!provider_names.insert(provider.name).second) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".name", "must be unique within the manifest");
    }
    if (!provider.config.is_object()) {
      result.add_issue(qt::foundation::ErrorCode::invalid_argument,
                       prefix + ".config", "must be a JSON object");
    }
  }
  return result;
}

qt::foundation::Result<ExtensionManifest> decode_extension_manifest(
    const Json& json) {
  try {
    ExtensionManifest manifest = json.get<ExtensionManifest>();
    return manifest;
  } catch (const std::exception& error) {
    return qt::foundation::Result<ExtensionManifest>::failure(
        qt::foundation::ErrorCode::encoding_error, error.what());
  }
}

qt::foundation::Result<ExtensionManifest> decode_extension_manifest_json(
    std::string_view text) {
  try {
    const Json json = Json::parse(text);
    return decode_extension_manifest(json);
  } catch (const std::exception& error) {
    return qt::foundation::Result<ExtensionManifest>::failure(
        qt::foundation::ErrorCode::encoding_error, error.what());
  }
}

void to_json(Json& json, const ExtensionCommandContribution& contribution) {
  json = Json{{"name", contribution.name}};
  if (contribution.title.has_value()) {
    json["title"] = *contribution.title;
  }
  if (contribution.description.has_value()) {
    json["description"] = *contribution.description;
  }
}

void from_json(const Json& json, ExtensionCommandContribution& contribution) {
  contribution.name = json.at("name").get<std::string>();
  load_optional_string(json, "title", contribution.title);
  load_optional_string(json, "description", contribution.description);
}

void to_json(Json& json, const ExtensionToolContribution& contribution) {
  json = Json{{"name", contribution.name}};
  if (contribution.title.has_value()) {
    json["title"] = *contribution.title;
  }
  if (contribution.description.has_value()) {
    json["description"] = *contribution.description;
  }
}

void from_json(const Json& json, ExtensionToolContribution& contribution) {
  contribution.name = json.at("name").get<std::string>();
  load_optional_string(json, "title", contribution.title);
  load_optional_string(json, "description", contribution.description);
}

void to_json(Json& json, const ExtensionProviderContribution& contribution) {
  json = Json{{"name", contribution.name}, {"config", contribution.config}};
}

void from_json(const Json& json, ExtensionProviderContribution& contribution) {
  contribution.name = json.at("name").get<std::string>();
  contribution.config = json.value("config", Json::object());
}

void to_json(Json& json, const ExtensionFlagContribution& contribution) {
  json = Json{{"name", contribution.name}, {"type", contribution.type}};
  if (contribution.description.has_value()) {
    json["description"] = *contribution.description;
  }
  if (!contribution.default_value.is_null()) {
    json["default"] = contribution.default_value;
  }
}

void from_json(const Json& json, ExtensionFlagContribution& contribution) {
  contribution.name = json.at("name").get<std::string>();
  load_optional_string(json, "description", contribution.description);
  contribution.type = json.value("type", std::string("boolean"));
  contribution.default_value =
      json.contains("default") ? json.at("default") : Json(nullptr);
}

void to_json(Json& json, const ExtensionManifest& manifest) {
  json = Json{
      {"manifest_version", manifest.manifest_version},
      {"name", manifest.name},
      {"transport", to_string(manifest.transport)},
      {"executable", manifest.executable},
      {"args", manifest.args},
      {"commands", manifest.commands},
      {"tools", manifest.tools},
      {"providers", manifest.providers},
      {"flags", manifest.flags},
      {"metadata", manifest.metadata},
  };

  if (manifest.display_name.has_value()) {
    json["display_name"] = *manifest.display_name;
  }
  if (manifest.entrypoint.has_value()) {
    json["entrypoint"] = *manifest.entrypoint;
  }
  if (manifest.working_directory.has_value()) {
    json["working_directory"] = *manifest.working_directory;
  }
}

void from_json(const Json& json, ExtensionManifest& manifest) {
  manifest.manifest_version =
      json.value("manifest_version", kExtensionManifestVersion);
  manifest.name = json.at("name").get<std::string>();

  const std::string transport_text =
      json.value("transport", std::string("subprocess_jsonrpc"));
  if (!parse_extension_transport(transport_text, manifest.transport)) {
    throw std::runtime_error("unsupported extension transport");
  }

  manifest.executable = json.value("executable", std::string{});
  manifest.args = json.value("args", std::vector<std::string>{});
  manifest.commands =
      json.value("commands", std::vector<ExtensionCommandContribution>{});
  manifest.tools = json.value("tools", std::vector<ExtensionToolContribution>{});
  manifest.providers =
      json.value("providers", std::vector<ExtensionProviderContribution>{});
  manifest.flags = json.value("flags", std::vector<ExtensionFlagContribution>{});
  manifest.metadata = json.value("metadata", Json::object());

  load_optional_string(json, "display_name", manifest.display_name);
  load_optional_string(json, "entrypoint", manifest.entrypoint);
  load_optional_string(json, "working_directory", manifest.working_directory);
}

}  // namespace qt::extensions
