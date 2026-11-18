#pragma once

#include <map>
#include <optional>
#include <cstdint>
#include <string>
#include <vector>

#include "qt/ai/auth_storage.hpp"
#include "qt/app/runtime/model_resolution.hpp"
#include "qt/foundation/error.hpp"

namespace qt::app {

enum class AuthCredentialKind {
  api_key,
  token,
};

enum class AuthLoginPlanAction {
  use_existing_auth,
  store_new_credential,
};

enum class AuthProviderStatusSource {
  none,
  auth_storage_api_key,
  auth_storage_token,
  environment_api_key,
  models_json_key,
  models_json_command,
};

enum class AuthSelectorAuthType {
  oauth,
  api_key,
};

enum class OAuthDevicePollAction {
  wait,
  access_token,
  failed,
  timed_out,
  cancelled,
};

struct AuthLoginPlanRequest {
  ModelResolutionRequest model;
  AuthCredentialKind credential_kind = AuthCredentialKind::api_key;
};

struct AuthOAuthProviderOption {
  std::string id;
  std::string name;
};

struct AuthSelectorProvider {
  std::string id;
  std::string name;
  AuthSelectorAuthType auth_type = AuthSelectorAuthType::api_key;
};

struct AuthLoginSelectorRequest {
  ai::ModelCatalog catalog;
  std::vector<AuthOAuthProviderOption> oauth_providers;
  std::vector<std::string> built_in_model_provider_ids;
  std::optional<AuthSelectorAuthType> auth_type;
};

struct AuthLogoutSelectorRequest {
  ai::ModelCatalog catalog;
  ai::AuthStorage auth_storage;
  std::vector<AuthOAuthProviderOption> oauth_providers;
};

struct PlannedAuthCredential {
  std::string provider;
  AuthCredentialKind kind = AuthCredentialKind::api_key;
  std::string auth_storage_path;
  bool replaces_existing_auth = false;
};

struct AuthLoginPlan {
  ResolvedModelSelection selection;
  AuthLoginPlanAction action = AuthLoginPlanAction::use_existing_auth;
  std::optional<PlannedAuthCredential> credential;
  std::vector<std::string> steps;
};

struct AuthProviderStatus {
  std::string provider;
  bool configured = false;
  AuthProviderStatusSource source = AuthProviderStatusSource::none;
  std::string label;
};

struct OAuthAuthorizationPlanRequest {
  std::string provider;
  std::string verifier;
  std::string challenge;
  std::string state;
  std::string originator = "pi";
  std::string enterprise_domain;
};

struct OAuthAuthorizationPlan {
  std::string provider;
  std::string name;
  bool uses_callback_server = false;
  std::string authorize_url;
  std::string token_url;
  std::string redirect_uri;
  std::string callback_host;
  std::uint16_t callback_port = 0;
  std::string callback_path;
  std::string manual_prompt;
  std::string instructions;
};

struct OAuthAuthorizationCode {
  std::string code;
  std::optional<std::string> state;
};

struct OAuthRefreshPlan {
  std::string provider;
  bool refresh_required = false;
  std::string token_url;
  std::string method = "POST";
  std::string content_type;
  std::map<std::string, std::string> headers;
  std::string request_body;
  std::string failure_message_prefix;
};

struct OAuthTokenResponseRequest {
  std::string provider;
  ai::Json response = ai::Json::object();
  std::int64_t now_ms = 0;
  std::string refresh_token;
  std::string enterprise_domain;
};

struct OAuthDeviceCodePlanRequest {
  std::string provider;
  ai::Json response = ai::Json::object();
  std::int64_t now_ms = 0;
  std::string enterprise_domain;
};

struct OAuthDeviceCodePlan {
  std::string provider;
  std::string device_code;
  std::string user_code;
  std::string verification_uri;
  std::string instructions;
  std::string access_token_url;
  std::string method = "POST";
  std::map<std::string, std::string> headers;
  std::string request_body;
  std::int64_t poll_interval_ms = 0;
  std::int64_t interval_multiplier_per_mille = 1200;
  std::int64_t slow_down_responses = 0;
  std::int64_t deadline_ms = 0;
  std::int64_t next_poll_at_ms = 0;
};

struct OAuthDevicePollDecisionRequest {
  std::string provider;
  ai::Json response = ai::Json::object();
  std::int64_t now_ms = 0;
  std::int64_t deadline_ms = 0;
  std::int64_t poll_interval_ms = 0;
  std::int64_t interval_multiplier_per_mille = 1200;
  std::int64_t slow_down_responses = 0;
  bool cancelled = false;
};

struct OAuthDevicePollDecision {
  std::string provider;
  OAuthDevicePollAction action = OAuthDevicePollAction::wait;
  std::optional<std::string> access_token;
  std::string error_message;
  std::int64_t poll_interval_ms = 0;
  std::int64_t interval_multiplier_per_mille = 1200;
  std::int64_t slow_down_responses = 0;
  std::int64_t next_poll_at_ms = 0;
};

struct OAuthCopilotModelEnablementPlanRequest {
  ai::ModelCatalog catalog;
  std::string access_token;
  std::string enterprise_domain;
};

struct OAuthCopilotModelEnablementRequest {
  std::string model_id;
  std::string url;
  std::string method = "POST";
  std::map<std::string, std::string> headers;
  std::string request_body;
};

struct OAuthCopilotModelEnablementPlan {
  std::string base_url;
  std::vector<OAuthCopilotModelEnablementRequest> requests;
};

struct OAuthCallbackResponsePlanRequest {
  std::string provider;
  std::string request_target;
  std::string expected_state;
};

struct OAuthCallbackResponsePlan {
  std::string provider;
  int status_code = 200;
  std::string content_type = "text/html; charset=utf-8";
  bool completed = false;
  std::optional<OAuthAuthorizationCode> authorization;
  std::string message;
  std::string details;
  std::string response_body;
};

struct OAuthCallbackHttpRequest {
  std::string provider;
  std::string request_line;
  std::string expected_state;
};

struct OAuthCallbackHttpResponse {
  OAuthCallbackResponsePlan callback;
  std::string status_text;
  std::map<std::string, std::string> headers;
  std::string raw_response;
};

struct OAuthAuthorizationResolutionRequest {
  std::string provider;
  std::string expected_state;
  std::optional<OAuthCallbackResponsePlan> callback;
  std::string manual_input;
};

struct OAuthAuthorizationResolution {
  std::string provider;
  std::string source;
  OAuthAuthorizationCode authorization;
};

struct OAuthLiveExecutionGateRequest {
  std::string provider;
  std::string enable_env_var = "QT_ENABLE_LIVE_OAUTH_FLOWS";
};

struct OAuthLiveExecutionGate {
  std::string provider;
  std::string enable_env_var;
  bool enabled = false;
  std::string reason;
};

const char* to_string(AuthCredentialKind kind) noexcept;
const char* to_string(AuthLoginPlanAction action) noexcept;
const char* to_string(AuthProviderStatusSource source) noexcept;
const char* to_string(AuthSelectorAuthType auth_type) noexcept;
const char* to_string(OAuthDevicePollAction action) noexcept;

std::vector<AuthOAuthProviderOption> built_in_oauth_provider_options();

bool is_api_key_login_provider(
    const std::string& provider,
    const std::vector<std::string>& oauth_provider_ids,
    const std::vector<std::string>& built_in_model_provider_ids = {});

std::vector<AuthSelectorProvider> build_auth_login_provider_options(
    const AuthLoginSelectorRequest& request);

std::vector<AuthSelectorProvider> build_auth_logout_provider_options(
    const AuthLogoutSelectorRequest& request);

AuthProviderStatus describe_auth_provider_status(
    const ai::AuthStorage& storage, const std::string& provider,
    const std::optional<std::string>& provider_request_api_key = std::nullopt);

foundation::Result<OAuthAuthorizationPlan> plan_oauth_authorization(
    const OAuthAuthorizationPlanRequest& request);

foundation::Result<OAuthAuthorizationCode> parse_oauth_authorization_input(
    const std::string& input,
    const std::optional<std::string>& expected_state = std::nullopt);

foundation::Result<OAuthRefreshPlan> plan_oauth_refresh(
    const ai::ProviderAuthRecord& record, std::int64_t now_ms);

foundation::Result<ai::ProviderAuthRecord> parse_oauth_token_response(
    const OAuthTokenResponseRequest& request);

foundation::Result<OAuthDeviceCodePlan> plan_oauth_device_code_polling(
    const OAuthDeviceCodePlanRequest& request);

foundation::Result<OAuthDevicePollDecision> plan_oauth_device_poll_response(
    const OAuthDevicePollDecisionRequest& request);

foundation::Result<OAuthCopilotModelEnablementPlan>
plan_github_copilot_model_enablement(
    const OAuthCopilotModelEnablementPlanRequest& request);

foundation::Result<OAuthCallbackResponsePlan> plan_oauth_callback_response(
    const OAuthCallbackResponsePlanRequest& request);

foundation::Result<OAuthCallbackHttpResponse> plan_oauth_callback_http_response(
    const OAuthCallbackHttpRequest& request);

foundation::Result<OAuthAuthorizationResolution>
resolve_oauth_authorization_result(
    const OAuthAuthorizationResolutionRequest& request);

OAuthLiveExecutionGate plan_oauth_live_execution_gate(
    const OAuthLiveExecutionGateRequest& request = {});

foundation::Result<AuthLoginPlan> plan_auth_login(
    const AuthLoginPlanRequest& request);

foundation::Result<ai::AuthStorage> apply_auth_credential(
    const ai::AuthStorage& storage, const PlannedAuthCredential& credential,
    const std::string& secret);

}  // namespace qt::app
