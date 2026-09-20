#pragma once

#include <optional>
#include <string>
#include <vector>

#include "qt/ai/models.hpp"
#include "qt/foundation/error.hpp"
#include "qt/foundation/filesystem.hpp"

namespace qt::ai {

struct ProviderAuthRecord {
  std::string provider;
  std::optional<std::string> api_key;
  std::optional<std::string> token;
  Json metadata = Json::object();
};

struct AuthStorage {
  std::vector<ProviderAuthRecord> providers;
};

enum class ProviderCredentialSource {
  none,
  runtime_api_key,
  auth_storage_api_key,
  auth_storage_token,
  environment_api_key,
};

struct ProviderCredentialResolutionRequest {
  std::string provider;
  AuthStorage auth_storage;
  std::optional<ProviderAuthRecord> auth_record;
  std::optional<std::string> runtime_api_key;
};

struct ProviderCredentialResolution {
  std::string provider;
  ProviderCredentialSource source = ProviderCredentialSource::none;
  std::optional<std::string> secret;
  std::string label;
};

ProviderAuthRecord normalized(const ProviderAuthRecord& record);
AuthStorage normalized(const AuthStorage& storage);

bool is_valid(const ProviderAuthRecord& record);
bool is_valid(const AuthStorage& storage);

std::vector<std::string> validate(const ProviderAuthRecord& record);
std::vector<std::string> validate(const AuthStorage& storage);

const ProviderAuthRecord* find_provider_auth_record(
    const AuthStorage& storage, const std::string& provider);
std::optional<std::string> get_stored_auth_secret(const AuthStorage& storage,
                                                  const std::string& provider);
const char* to_string(ProviderCredentialSource source) noexcept;
ProviderCredentialResolution resolve_provider_credential(
    const ProviderCredentialResolutionRequest& request);
std::string missing_provider_credential_message(const std::string& provider);
std::vector<std::string> list_auth_provider_ids(const AuthStorage& storage);
bool has_provider_auth_record(const AuthStorage& storage,
                              const std::string& provider);
AuthStorage upsert_provider_auth_record(const AuthStorage& storage,
                                        const ProviderAuthRecord& record);
AuthStorage remove_provider_auth_record(const AuthStorage& storage,
                                        const std::string& provider);
AuthStorage merge_auth_storage(const AuthStorage& base, const AuthStorage& overlay);

std::vector<std::string> auth_env_var_names_for_provider(
    const std::string& provider);
std::vector<std::string> find_auth_env_vars(const std::string& provider);
std::optional<std::string> get_env_api_key(const std::string& provider);

foundation::Result<AuthStorage> parse_auth_storage_json(const Json& json);
Json to_json(const AuthStorage& storage);

foundation::Result<AuthStorage> load_auth_storage(const foundation::Path& path);
foundation::Status save_auth_storage(const foundation::Path& path,
                                     const AuthStorage& storage);

}  // namespace qt::ai
