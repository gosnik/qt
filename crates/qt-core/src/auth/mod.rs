//! Credentials: `auth.json`, provider environment variables, OAuth tokens.
//!
//! `auth.json` is written in the archived qt shape
//! `{ "version": 1, "providers": { "<id>": { "apiKey" | "token", "metadata" } } }`
//! where OAuth logins store the access token in `token` and the full
//! credential (`type: "oauth"`, `access`, `refresh`, `expires`, `accountId`)
//! in `metadata`. The legacy Pi shape (`{ "<id>": { "type": "api_key", "key" } }`
//! or `{ "<id>": { "type": "oauth", "access", ... } }` at the top level) is
//! migrated on load. Unknown keys are preserved.

pub mod oauth;

use std::fmt;
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, anyhow};
use base64::Engine as _;
use serde_json::{Map, Value, json};

use crate::config::{AgentHome, read_json_object, write_json_atomic};
use crate::message::now_millis;

/// OAuth tokens for a subscription provider.
#[derive(Clone, PartialEq, Default)]
pub struct OAuthCredentials {
    pub access: String,
    pub refresh: String,
    /// Expiry in Unix milliseconds (Anthropic already subtracts a 5 min margin).
    pub expires: i64,
    /// ChatGPT account id (openai-codex).
    pub account_id: Option<String>,
    /// Other fields kept from storage.
    pub extra: Map<String, Value>,
}

impl fmt::Debug for OAuthCredentials {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("OAuthCredentials")
            .field("expires", &self.expires)
            .field("account_id", &self.account_id)
            .finish_non_exhaustive()
    }
}

impl OAuthCredentials {
    pub fn is_expired(&self, now_ms: i64) -> bool {
        now_ms >= self.expires
    }

    /// The stored `metadata` object.
    pub fn to_metadata(&self) -> Value {
        let mut map = self.extra.clone();
        map.insert("type".into(), json!("oauth"));
        map.insert("access".into(), json!(self.access));
        map.insert("refresh".into(), json!(self.refresh));
        map.insert("expires".into(), json!(self.expires));
        if let Some(account) = &self.account_id {
            map.insert("accountId".into(), json!(account));
        }
        Value::Object(map)
    }

    fn from_metadata(metadata: &Value, token: Option<&str>) -> Option<Self> {
        let object = metadata.as_object()?;
        if object.get("type").and_then(Value::as_str) != Some("oauth") {
            return None;
        }
        let access = object
            .get("access")
            .and_then(Value::as_str)
            .or(token)?
            .trim()
            .to_string();
        if access.is_empty() {
            return None;
        }
        let mut extra = object.clone();
        for key in ["type", "access", "refresh", "expires", "accountId"] {
            extra.remove(key);
        }
        Some(Self {
            access,
            refresh: object
                .get("refresh")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string(),
            expires: object.get("expires").and_then(Value::as_i64).unwrap_or(0),
            account_id: object
                .get("accountId")
                .and_then(Value::as_str)
                .map(str::to_string),
            extra,
        })
    }
}

/// A stored credential.
#[derive(Clone, PartialEq)]
pub enum Credential {
    ApiKey(String),
    OAuth(OAuthCredentials),
}

impl fmt::Debug for Credential {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ApiKey(_) => f.write_str("ApiKey(<redacted>)"),
            Self::OAuth(c) => f.debug_tuple("OAuth").field(c).finish(),
        }
    }
}

impl Credential {
    /// The secret sent to the provider.
    pub fn secret(&self) -> &str {
        match self {
            Self::ApiKey(k) => k,
            Self::OAuth(c) => &c.access,
        }
    }
}

/// `auth.json`.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct AuthStore {
    raw: Map<String, Value>,
    path: Option<PathBuf>,
    /// Provider entries changed since load; only these are written back so
    /// tokens refreshed by another session or process are not overwritten.
    dirty: std::collections::BTreeSet<String>,
}

fn trimmed(value: Option<&Value>) -> Option<String> {
    value
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
}

/// Convert a legacy Pi record into the qt v1 record.
fn migrate_record(record: &Value) -> Option<Value> {
    match record.get("type").and_then(Value::as_str)? {
        "api_key" => Some(json!({ "apiKey": trimmed(record.get("key"))? })),
        "oauth" => {
            let access = trimmed(record.get("access"))?;
            Some(json!({ "token": access, "metadata": record }))
        }
        _ => None,
    }
}

impl AuthStore {
    pub fn load(home: &AgentHome) -> Result<Self> {
        Self::load_path(&home.auth_path())
    }

    /// Load from an explicit path (legacy shapes are migrated in memory).
    pub fn load_path(path: &Path) -> Result<Self> {
        let mut raw = read_json_object(path)?;
        if !raw.contains_key("providers") {
            let legacy: Vec<String> = raw
                .iter()
                .filter(|(_, v)| v.get("type").is_some())
                .map(|(k, _)| k.clone())
                .collect();
            if !legacy.is_empty() {
                let mut providers = Map::new();
                for id in legacy {
                    if let Some(record) = raw.remove(&id).as_ref().and_then(migrate_record) {
                        providers.insert(id, record);
                    }
                }
                raw.insert("version".into(), json!(1));
                raw.insert("providers".into(), Value::Object(providers));
            }
        }
        Ok(Self {
            raw,
            path: Some(path.to_path_buf()),
            dirty: Default::default(),
        })
    }

    pub fn save(&self, home: &AgentHome) -> Result<()> {
        self.save_to(&home.auth_path())
    }

    /// Write the store. The current file is re-read and only provider
    /// entries changed through this store are replaced; if the file cannot
    /// be parsed the whole store is written.
    pub fn save_to(&self, path: &Path) -> Result<()> {
        let mut raw = match Self::load_path(path) {
            Ok(disk) if !disk.raw.is_empty() => {
                let mut raw = disk.raw;
                let providers = raw.entry("providers").or_insert_with(|| json!({}));
                if !providers.is_object() {
                    *providers = json!({});
                }
                let providers = providers.as_object_mut().expect("object");
                let ours = self.raw.get("providers").and_then(Value::as_object);
                for id in &self.dirty {
                    match ours.and_then(|p| p.get(id)) {
                        Some(entry) => {
                            providers.insert(id.clone(), entry.clone());
                        }
                        None => {
                            providers.remove(id);
                        }
                    }
                }
                raw
            }
            _ => self.raw.clone(),
        };
        raw.entry("version").or_insert(json!(1));
        write_json_atomic(path, &Value::Object(raw), true)
    }

    /// Where this store was loaded from.
    pub fn path(&self) -> Option<&Path> {
        self.path.as_deref()
    }

    fn entry(&self, provider: &str) -> Option<&Value> {
        let providers = self.raw.get("providers")?.as_object()?;
        providers.get(provider).or_else(|| {
            let wanted = provider.trim().to_lowercase();
            providers
                .iter()
                .find(|(k, _)| k.trim().to_lowercase() == wanted)
                .map(|(_, v)| v)
        })
    }

    fn entry_mut(&mut self, provider: &str) -> &mut Map<String, Value> {
        self.dirty.insert(provider.to_string());
        let providers = self.raw.entry("providers").or_insert_with(|| json!({}));
        if !providers.is_object() {
            *providers = json!({});
        }
        let providers = providers.as_object_mut().expect("object");
        let entry = providers
            .entry(provider.to_string())
            .or_insert_with(|| json!({}));
        if !entry.is_object() {
            *entry = json!({});
        }
        entry.as_object_mut().expect("object")
    }

    /// Provider ids with stored credentials.
    pub fn providers(&self) -> Vec<String> {
        self.raw
            .get("providers")
            .and_then(Value::as_object)
            .map(|p| {
                p.iter()
                    .filter(|(id, _)| self.credential(id).is_some())
                    .map(|(id, _)| id.clone())
                    .collect()
            })
            .unwrap_or_default()
    }

    /// The stored credential for `provider` (API key, token or OAuth).
    pub fn credential(&self, provider: &str) -> Option<Credential> {
        let entry = self.entry(provider)?;
        let token = trimmed(entry.get("token"));
        if let Some(metadata) = entry.get("metadata")
            && let Some(creds) = OAuthCredentials::from_metadata(metadata, token.as_deref())
        {
            return Some(Credential::OAuth(creds));
        }
        // Tolerate legacy records nested under `providers`.
        if let Some(creds) = OAuthCredentials::from_metadata(entry, None) {
            return Some(Credential::OAuth(creds));
        }
        trimmed(entry.get("apiKey"))
            .or_else(|| {
                (entry.get("type").and_then(Value::as_str) == Some("api_key"))
                    .then(|| trimmed(entry.get("key")))
                    .flatten()
            })
            .or(token)
            .map(Credential::ApiKey)
    }

    /// Stored secret for `provider`, falling back to its environment variables.
    pub fn api_key(&self, provider: &str) -> Option<String> {
        self.stored_api_key(provider)
            .or_else(|| env_api_key(provider))
    }

    /// Stored secret (API key, token or OAuth access token).
    pub fn stored_api_key(&self, provider: &str) -> Option<String> {
        self.credential(provider).map(|c| c.secret().to_string())
    }

    pub fn set_api_key(&mut self, provider: &str, key: &str) {
        self.entry_mut(provider)
            .insert("apiKey".into(), json!(key.trim()));
    }

    /// Remove the stored API key (an OAuth login is kept).
    pub fn remove_api_key(&mut self, provider: &str) {
        self.dirty.insert(provider.to_string());
        let Some(providers) = self.raw.get_mut("providers").and_then(Value::as_object_mut) else {
            return;
        };
        if let Some(entry) = providers.get_mut(provider).and_then(Value::as_object_mut) {
            entry.remove("apiKey");
            if entry.is_empty() {
                providers.remove(provider);
            }
        }
    }

    /// Store OAuth credentials (replacing any API key for the provider).
    pub fn set_oauth(&mut self, provider: &str, credentials: &OAuthCredentials) {
        let entry = self.entry_mut(provider);
        entry.remove("apiKey");
        entry.insert("token".into(), json!(credentials.access));
        entry.insert("metadata".into(), credentials.to_metadata());
    }

    /// Remove every stored credential for `provider`; returns whether one existed.
    pub fn logout(&mut self, provider: &str) -> bool {
        self.dirty.insert(provider.to_string());
        self.raw
            .get_mut("providers")
            .and_then(Value::as_object_mut)
            .and_then(|p| p.remove(provider))
            .is_some()
    }
}

/// Environment variables consulted for `provider`, in priority order.
pub fn env_var_names(provider: &str) -> &'static [&'static str] {
    match provider.trim().to_lowercase().as_str() {
        "github-copilot" => &["COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"],
        "anthropic" => &["ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"],
        "openai" => &["OPENAI_API_KEY"],
        "azure-openai-responses" => &["AZURE_OPENAI_API_KEY"],
        "deepseek" => &["DEEPSEEK_API_KEY"],
        "google" => &["GEMINI_API_KEY"],
        "google-vertex" => &["GOOGLE_CLOUD_API_KEY"],
        "groq" => &["GROQ_API_KEY"],
        "cerebras" => &["CEREBRAS_API_KEY"],
        "xai" => &["XAI_API_KEY"],
        "openrouter" => &["OPENROUTER_API_KEY"],
        "vercel-ai-gateway" => &["AI_GATEWAY_API_KEY"],
        "zai" => &["ZAI_API_KEY"],
        "mistral" => &["MISTRAL_API_KEY"],
        "minimax" => &["MINIMAX_API_KEY"],
        "minimax-cn" => &["MINIMAX_CN_API_KEY"],
        "moonshotai" | "moonshotai-cn" => &["MOONSHOT_API_KEY"],
        "huggingface" => &["HF_TOKEN"],
        "fireworks" => &["FIREWORKS_API_KEY"],
        "together" => &["TOGETHER_API_KEY"],
        "opencode" | "opencode-go" => &["OPENCODE_API_KEY"],
        "kimi-coding" => &["KIMI_API_KEY"],
        "cloudflare-workers-ai" | "cloudflare-ai-gateway" => &["CLOUDFLARE_API_KEY"],
        "xiaomi" => &["XIAOMI_API_KEY"],
        "xiaomi-token-plan-cn" => &["XIAOMI_TOKEN_PLAN_CN_API_KEY"],
        "xiaomi-token-plan-ams" => &["XIAOMI_TOKEN_PLAN_AMS_API_KEY"],
        "xiaomi-token-plan-sgp" => &["XIAOMI_TOKEN_PLAN_SGP_API_KEY"],
        _ => &[],
    }
}

/// The first environment variable consulted for `provider`.
pub fn env_var_for(provider: &str) -> Option<&'static str> {
    env_var_names(provider).first().copied()
}

fn env_nonempty(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.trim().is_empty())
}

/// Environment variables that are set for `provider`.
pub fn find_env_vars(provider: &str) -> Vec<&'static str> {
    env_var_names(provider)
        .iter()
        .copied()
        .filter(|name| env_nonempty(name).is_some())
        .collect()
}

/// Key from the environment; `"<authenticated>"` for ambient Vertex/Bedrock
/// credentials (archived qt `get_env_api_key`).
pub fn env_api_key(provider: &str) -> Option<String> {
    let key = provider.trim().to_lowercase();
    for name in env_var_names(&key) {
        if let Some(value) = env_nonempty(name) {
            let marker = value.trim() == "gcp-vertex-credentials"
                || (value.trim().len() > 2
                    && value.trim().starts_with('<')
                    && value.trim().ends_with('>'));
            if key == "google-vertex" && marker {
                continue;
            }
            return Some(value);
        }
    }
    match key.as_str() {
        "google-vertex" if has_vertex_adc() => Some("<authenticated>".into()),
        "amazon-bedrock" if has_bedrock_credentials() => Some("<authenticated>".into()),
        _ => None,
    }
}

fn has_vertex_adc() -> bool {
    let project = env_nonempty("GOOGLE_CLOUD_PROJECT").or_else(|| env_nonempty("GCLOUD_PROJECT"));
    if project.is_none() || env_nonempty("GOOGLE_CLOUD_LOCATION").is_none() {
        return false;
    }
    if let Some(path) = env_nonempty("GOOGLE_APPLICATION_CREDENTIALS") {
        return Path::new(&path).exists();
    }
    dirs::home_dir()
        .map(|h| {
            h.join(".config/gcloud/application_default_credentials.json")
                .exists()
        })
        .unwrap_or(false)
}

fn has_bedrock_credentials() -> bool {
    env_nonempty("AWS_PROFILE").is_some()
        || (env_nonempty("AWS_ACCESS_KEY_ID").is_some()
            && env_nonempty("AWS_SECRET_ACCESS_KEY").is_some())
        || [
            "AWS_BEARER_TOKEN_BEDROCK",
            "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI",
            "AWS_CONTAINER_CREDENTIALS_FULL_URI",
            "AWS_WEB_IDENTITY_TOKEN_FILE",
        ]
        .iter()
        .any(|n| env_nonempty(n).is_some())
}

fn join_names(names: &[&str]) -> String {
    let mut out = String::new();
    for (i, name) in names.iter().enumerate() {
        if i > 0 {
            out.push_str(if i + 1 == names.len() { " or " } else { ", " });
        }
        out.push_str(name);
    }
    out
}

/// Message shown when a provider has no credential (archived qt wording).
pub fn missing_credential_message(provider: &str) -> String {
    let key = provider.trim().to_lowercase();
    let display = if key.is_empty() {
        "the selected provider".to_string()
    } else {
        provider.trim().to_string()
    };
    let hint = match key.as_str() {
        "google-vertex" => "set GOOGLE_CLOUD_API_KEY or configure Google Application Default Credentials with GOOGLE_CLOUD_PROJECT and GOOGLE_CLOUD_LOCATION".to_string(),
        "amazon-bedrock" => "set AWS_PROFILE, AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY, or AWS_BEARER_TOKEN_BEDROCK".to_string(),
        _ => {
            let names = env_var_names(&key);
            if names.is_empty() {
                String::new()
            } else {
                format!("set {}", join_names(names))
            }
        }
    };
    let mut message = format!("No API key found for {display}.");
    if hint.is_empty() {
        message.push_str(" Run /login");
    } else {
        message.push_str(&format!(" {hint}, or run /login"));
    }
    if !key.is_empty() {
        message.push(' ');
        message.push_str(provider.trim());
    }
    message.push('.');
    message
}

/// The JWT claim holding the ChatGPT account.
pub const OPENAI_JWT_CLAIM: &str = "https://api.openai.com/auth";

/// Decode a JWT payload without verifying it.
pub fn decode_jwt_payload(token: &str) -> Option<Value> {
    let mut parts = token.split('.');
    let (_, payload, _) = (parts.next()?, parts.next()?, parts.next()?);
    if parts.next().is_some() {
        return None;
    }
    let trimmed = payload.trim_end_matches('=');
    let bytes = base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(trimmed)
        .or_else(|_| base64::engine::general_purpose::STANDARD_NO_PAD.decode(trimmed))
        .ok()?;
    serde_json::from_slice(&bytes).ok()
}

/// `chatgpt_account_id` from an OpenAI access token.
pub fn jwt_account_id(token: &str) -> Option<String> {
    decode_jwt_payload(token)?
        .get(OPENAI_JWT_CLAIM)?
        .get("chatgpt_account_id")?
        .as_str()
        .filter(|s| !s.is_empty())
        .map(str::to_string)
}

/// Run the OAuth login for `provider` and store the credentials in the
/// home's `auth.json` (replacing an API key for that provider).
pub async fn login_and_store(
    http: &reqwest::Client,
    home: &AgentHome,
    provider: &str,
    options: oauth::LoginOptions,
) -> Result<OAuthCredentials> {
    let credentials = oauth::login(http, provider, options).await?;
    let mut store = AuthStore::load(home)?;
    store.set_oauth(provider, &credentials);
    store.save(home)?;
    Ok(credentials)
}

/// Remove `provider`'s stored credentials from the home's `auth.json`.
pub fn logout(home: &AgentHome, provider: &str) -> Result<bool> {
    let mut store = AuthStore::load(home)?;
    let removed = store.logout(provider);
    if removed {
        store.save(home)?;
    }
    Ok(removed)
}

/// The provider's access token, refreshed (and persisted) when expired.
pub async fn fresh_access_token(
    http: &reqwest::Client,
    provider: &str,
    binding: &crate::provider::OAuthBinding,
) -> Result<String> {
    static REFRESH: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());
    let now = now_millis();
    if !binding.credentials.is_expired(now) {
        return Ok(binding.credentials.access.clone());
    }
    let _guard = REFRESH.lock().await;
    // Another session or process may already have refreshed the token.
    let mut current = binding.credentials.clone();
    if let Some(path) = &binding.auth_path
        && let Ok(store) = AuthStore::load_path(path)
        && let Some(Credential::OAuth(stored)) = store.credential(provider)
    {
        if !stored.is_expired(now_millis()) {
            return Ok(stored.access);
        }
        current = stored;
    }
    if current.refresh.is_empty() {
        return Err(anyhow!(
            "OAuth credentials for {provider} have expired and have no refresh token; run /login {provider}"
        ));
    }
    let refreshed = oauth::refresh_token(http, provider, &current.refresh)
        .await
        .with_context(|| format!("refreshing the {provider} OAuth token"))?;
    let refreshed = OAuthCredentials {
        extra: current.extra.clone(),
        account_id: refreshed.account_id.clone().or(current.account_id.clone()),
        ..refreshed
    };
    if let Some(path) = &binding.auth_path {
        let mut store = AuthStore::load_path(path).unwrap_or_default();
        store.set_oauth(provider, &refreshed);
        store.save_to(path)?;
    }
    Ok(refreshed.access)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_keys_and_preserves_unknown_fields() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        std::fs::write(
            home.auth_path(),
            r#"{"version":1,"providers":{"zai":{"apiKey":" zk ","note":"x"}},"other":1}"#,
        )
        .unwrap();
        let mut store = AuthStore::load(&home).unwrap();
        assert_eq!(store.stored_api_key("ZAI").as_deref(), Some("zk"));
        store.set_api_key("openrouter", " sk-test ");
        store.save(&home).unwrap();
        let mut store = AuthStore::load(&home).unwrap();
        assert_eq!(
            store.stored_api_key("openrouter").as_deref(),
            Some("sk-test")
        );
        store.remove_api_key("openrouter");
        store.remove_api_key("zai");
        let raw = &store.raw;
        assert!(raw["providers"].get("openrouter").is_none());
        assert_eq!(raw["providers"]["zai"]["note"], "x");
        assert_eq!(raw["other"], 1);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt as _;
            let mode = std::fs::metadata(home.auth_path())
                .unwrap()
                .permissions()
                .mode();
            assert_eq!(mode & 0o777, 0o600);
        }
    }

    #[test]
    fn saving_a_stale_store_keeps_entries_changed_elsewhere() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        let mut first = AuthStore::load(&home).unwrap();
        first.set_api_key("zai", "z1");
        first.save(&home).unwrap();
        let mut stale = AuthStore::load(&home).unwrap();
        // Another session refreshes an OAuth login meanwhile.
        let mut other = AuthStore::load(&home).unwrap();
        other.set_oauth(
            "anthropic",
            &OAuthCredentials {
                access: "new".into(),
                refresh: "r".into(),
                expires: 1,
                ..Default::default()
            },
        );
        other.save(&home).unwrap();
        stale.set_api_key("groq", "g");
        stale.save(&home).unwrap();
        let disk = AuthStore::load(&home).unwrap();
        assert_eq!(disk.stored_api_key("anthropic").as_deref(), Some("new"));
        assert_eq!(disk.stored_api_key("groq").as_deref(), Some("g"));
        assert_eq!(disk.stored_api_key("zai").as_deref(), Some("z1"));
        assert!(logout(&home, "groq").unwrap());
        assert!(!logout(&home, "groq").unwrap());
        assert!(AuthStore::load(&home).unwrap().credential("groq").is_none());
    }

    #[test]
    fn migrates_legacy_pi_shapes() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path());
        std::fs::write(
            home.auth_path(),
            r#"{"anthropic":{"type":"api_key","key":"sk-ant"},
                "openai-codex":{"type":"oauth","access":"acc","refresh":"ref","expires":123,"accountId":"a1"},
                "broken":{"type":"weird"}}"#,
        )
        .unwrap();
        let store = AuthStore::load(&home).unwrap();
        assert_eq!(
            store.credential("anthropic"),
            Some(Credential::ApiKey("sk-ant".into()))
        );
        let Some(Credential::OAuth(c)) = store.credential("openai-codex") else {
            panic!()
        };
        assert_eq!(
            (c.access.as_str(), c.refresh.as_str(), c.expires),
            ("acc", "ref", 123)
        );
        assert_eq!(c.account_id.as_deref(), Some("a1"));
        let mut providers = store.providers();
        providers.sort();
        assert_eq!(providers, ["anthropic", "openai-codex"]);
        store.save(&home).unwrap();
        let text = std::fs::read_to_string(home.auth_path()).unwrap();
        let saved: Value = serde_json::from_str(&text).unwrap();
        assert_eq!(saved["version"], 1);
        assert_eq!(saved["providers"]["anthropic"]["apiKey"], "sk-ant");
        assert_eq!(saved["providers"]["openai-codex"]["token"], "acc");
        assert_eq!(
            saved["providers"]["openai-codex"]["metadata"]["type"],
            "oauth"
        );
    }

    #[test]
    fn oauth_storage_and_logout() {
        let mut store = AuthStore::default();
        store.set_api_key("anthropic", "old");
        let creds = OAuthCredentials {
            access: "sk-ant-oat-1".into(),
            refresh: "r".into(),
            expires: 99,
            ..Default::default()
        };
        store.set_oauth("anthropic", &creds);
        assert_eq!(
            store.credential("anthropic"),
            Some(Credential::OAuth(creds))
        );
        assert_eq!(
            store.stored_api_key("anthropic").as_deref(),
            Some("sk-ant-oat-1")
        );
        assert!(store.logout("anthropic"));
        assert!(!store.logout("anthropic"));
        assert!(store.credential("anthropic").is_none());
        // A plain token record is used as a key (archived "auth.json token").
        let mut store = AuthStore::default();
        store.entry_mut("groq").insert("token".into(), json!("gt"));
        assert_eq!(
            store.credential("groq"),
            Some(Credential::ApiKey("gt".into()))
        );
    }

    #[test]
    fn env_table_and_messages() {
        assert_eq!(
            env_var_names("anthropic"),
            ["ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"]
        );
        assert_eq!(env_var_for("moonshotai-cn"), Some("MOONSHOT_API_KEY"));
        assert_eq!(
            env_var_for("xiaomi-token-plan-sgp"),
            Some("XIAOMI_TOKEN_PLAN_SGP_API_KEY")
        );
        assert_eq!(env_var_for("nope"), None);
        assert_eq!(
            missing_credential_message("anthropic"),
            "No API key found for anthropic. set ANTHROPIC_OAUTH_TOKEN or ANTHROPIC_API_KEY, or run /login anthropic."
        );
        assert_eq!(
            missing_credential_message("github-copilot"),
            "No API key found for github-copilot. set COPILOT_GITHUB_TOKEN, GH_TOKEN or GITHUB_TOKEN, or run /login github-copilot."
        );
        assert_eq!(
            missing_credential_message("custom"),
            "No API key found for custom. Run /login custom."
        );
    }

    #[test]
    fn extracts_jwt_account() {
        let engine = base64::engine::general_purpose::URL_SAFE_NO_PAD;
        let payload =
            engine.encode(r#"{"https://api.openai.com/auth":{"chatgpt_account_id":"acc_9"}}"#);
        assert_eq!(
            jwt_account_id(&format!("h.{payload}.s")).as_deref(),
            Some("acc_9")
        );
        assert_eq!(jwt_account_id("h.e30.s"), None);
        assert_eq!(jwt_account_id("not-a-jwt"), None);
    }

    #[tokio::test]
    async fn fresh_tokens_are_reused_and_newer_stored_tokens_win() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("auth.json");
        let http = reqwest::Client::new();
        let valid = OAuthCredentials {
            access: "a1".into(),
            refresh: "r1".into(),
            expires: now_millis() + 60_000,
            ..Default::default()
        };
        let binding = crate::provider::OAuthBinding {
            credentials: valid.clone(),
            auth_path: Some(path.clone()),
        };
        assert_eq!(
            fresh_access_token(&http, "anthropic", &binding)
                .await
                .unwrap(),
            "a1"
        );

        // Expired in memory, but another process stored a fresh token.
        let mut store = AuthStore::default();
        store.set_oauth(
            "anthropic",
            &OAuthCredentials {
                access: "a2".into(),
                ..valid.clone()
            },
        );
        store.save_to(&path).unwrap();
        let stale = crate::provider::OAuthBinding {
            credentials: OAuthCredentials {
                expires: 0,
                ..valid
            },
            auth_path: Some(path),
        };
        assert_eq!(
            fresh_access_token(&http, "anthropic", &stale)
                .await
                .unwrap(),
            "a2"
        );

        let no_refresh = crate::provider::OAuthBinding {
            credentials: OAuthCredentials {
                access: "x".into(),
                expires: 0,
                ..Default::default()
            },
            auth_path: None,
        };
        let err = fresh_access_token(&http, "openai-codex", &no_refresh)
            .await
            .unwrap_err();
        assert!(err.to_string().contains("run /login openai-codex"));
    }
}
