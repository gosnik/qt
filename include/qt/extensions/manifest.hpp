#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <qt/foundation/error.hpp>

namespace qt::extensions {

using Json = nlohmann::json;

constexpr int kExtensionManifestVersion = 1;

enum class ExtensionTransport {
  kSubprocessJsonRpc,
  kReferenceModule,
  kAngelScript,
};

struct ExtensionCommandContribution {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
};

struct ExtensionToolContribution {
  std::string name;
  std::optional<std::string> title;
  std::optional<std::string> description;
};

struct ExtensionProviderContribution {
  std::string name;
  Json config = Json::object();
};

struct ExtensionFlagContribution {
  std::string name;
  std::optional<std::string> description;
  std::string type = "boolean";
  Json default_value = Json(nullptr);
};

struct ExtensionManifest {
  int manifest_version = kExtensionManifestVersion;
  std::string name;
  std::optional<std::string> display_name;
  ExtensionTransport transport = ExtensionTransport::kSubprocessJsonRpc;
  std::string executable;
  std::optional<std::string> entrypoint;
  std::vector<std::string> args;
  std::optional<std::string> working_directory;
  std::vector<ExtensionCommandContribution> commands;
  std::vector<ExtensionToolContribution> tools;
  std::vector<ExtensionProviderContribution> providers;
  std::vector<ExtensionFlagContribution> flags;
  Json metadata = Json::object();
};

struct ManifestValidationIssue {
  qt::foundation::ErrorCode code = qt::foundation::ErrorCode::invalid_argument;
  std::string field;
  std::string message;

  std::string summary() const;
};

struct ManifestValidationResult {
  std::vector<ManifestValidationIssue> issues;

  bool ok() const noexcept { return issues.empty(); }
  explicit operator bool() const noexcept { return ok(); }

  void add_issue(qt::foundation::ErrorCode code, std::string field,
                 std::string message);
};

std::string to_string(ExtensionTransport transport);
bool parse_extension_transport(std::string_view text,
                               ExtensionTransport& transport);
bool is_valid_extension_identifier(std::string_view text);

ManifestValidationResult validate_manifest(const ExtensionManifest& manifest);

qt::foundation::Result<ExtensionManifest> decode_extension_manifest(
    const Json& json);
qt::foundation::Result<ExtensionManifest> decode_extension_manifest_json(
    std::string_view text);

void to_json(Json& json, const ExtensionCommandContribution& contribution);
void from_json(const Json& json, ExtensionCommandContribution& contribution);

void to_json(Json& json, const ExtensionToolContribution& contribution);
void from_json(const Json& json, ExtensionToolContribution& contribution);

void to_json(Json& json, const ExtensionProviderContribution& contribution);
void from_json(const Json& json, ExtensionProviderContribution& contribution);

void to_json(Json& json, const ExtensionFlagContribution& contribution);
void from_json(const Json& json, ExtensionFlagContribution& contribution);

void to_json(Json& json, const ExtensionManifest& manifest);
void from_json(const Json& json, ExtensionManifest& manifest);

}  // namespace qt::extensions
