//! OAuth PKCE logins with a localhost callback (Pi `utils/oauth`):
//! Anthropic (Claude Pro/Max) and OpenAI Codex (ChatGPT Plus/Pro).
//!
//! [`login`] starts the callback server, hands the authorization URL to the
//! caller (which opens a browser), and waits for either the browser redirect
//! or a pasted code / redirect URL. [`refresh_token`] renews credentials.

use std::time::Duration;

use anyhow::{Result, anyhow, bail};
use base64::Engine as _;
use serde_json::{Value, json};
use sha2::{Digest as _, Sha256};
use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
use tokio::net::TcpListener;
use tokio_util::sync::CancellationToken;

use super::{OAuthCredentials, jwt_account_id};
use crate::message::now_millis;

/// A provider that supports OAuth login.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OAuthProviderInfo {
    pub id: &'static str,
    pub name: &'static str,
}

/// Providers with a `/login` flow.
pub const OAUTH_PROVIDERS: [OAuthProviderInfo; 2] = [
    OAuthProviderInfo {
        id: "anthropic",
        name: "Anthropic (Claude Pro/Max)",
    },
    OAuthProviderInfo {
        id: "openai-codex",
        name: "ChatGPT Plus/Pro (Codex Subscription)",
    },
];

pub const ANTHROPIC_CLIENT_ID: &str = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
pub const ANTHROPIC_SCOPES: &str = "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
pub const CODEX_CLIENT_ID: &str = "app_EMoamEEZ73f0CkXaXp7hrann";
pub const CODEX_SCOPE: &str = "openid profile email offline_access";

/// Where a provider's OAuth flow talks to. Tests point these at local servers.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Endpoints {
    pub authorize_url: String,
    pub token_url: String,
    pub callback_host: String,
    pub callback_port: u16,
    pub callback_path: String,
    pub redirect_uri: String,
}

impl Endpoints {
    /// The real endpoints for `provider` (`PI_OAUTH_CALLBACK_HOST` overrides the bind host).
    pub fn for_provider(provider: &str) -> Option<Self> {
        let host = std::env::var("PI_OAUTH_CALLBACK_HOST")
            .ok()
            .filter(|h| !h.is_empty())
            .unwrap_or_else(|| "127.0.0.1".into());
        match provider {
            "anthropic" => Some(Self {
                authorize_url: "https://claude.ai/oauth/authorize".into(),
                token_url: "https://platform.claude.com/v1/oauth/token".into(),
                callback_host: host,
                callback_port: 53692,
                callback_path: "/callback".into(),
                redirect_uri: "http://localhost:53692/callback".into(),
            }),
            "openai-codex" => Some(Self {
                authorize_url: "https://auth.openai.com/oauth/authorize".into(),
                token_url: "https://auth.openai.com/oauth/token".into(),
                callback_host: host,
                callback_port: 1455,
                callback_path: "/auth/callback".into(),
                redirect_uri: "http://localhost:1455/auth/callback".into(),
            }),
            _ => None,
        }
    }
}

/// PKCE verifier and S256 challenge.
pub fn generate_pkce() -> (String, String) {
    let bytes: [u8; 32] = rand::random();
    let engine = base64::engine::general_purpose::URL_SAFE_NO_PAD;
    let verifier = engine.encode(bytes);
    let challenge = engine.encode(Sha256::digest(verifier.as_bytes()));
    (verifier, challenge)
}

fn random_state() -> String {
    let bytes: [u8; 16] = rand::random();
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// Parse a pasted code: a redirect URL, `code#state`, a query string, or a bare code.
pub fn parse_authorization_input(input: &str) -> (Option<String>, Option<String>) {
    let value = input.trim();
    if value.is_empty() {
        return (None, None);
    }
    let from_pairs = |pairs: Vec<(String, String)>| {
        let get = |k: &str| {
            pairs
                .iter()
                .find(|(key, _)| key == k)
                .map(|(_, v)| v.clone())
        };
        (get("code"), get("state"))
    };
    if let Ok(url) = reqwest::Url::parse(value)
        && url.has_host()
    {
        return from_pairs(
            url.query_pairs()
                .map(|(k, v)| (k.into_owned(), v.into_owned()))
                .collect(),
        );
    }
    if let Some((code, state)) = value.split_once('#') {
        return (Some(code.to_string()), Some(state.to_string()));
    }
    if value.contains("code=") {
        let query = value.trim_start_matches('?');
        let url = reqwest::Url::parse(&format!("http://x/?{query}")).ok();
        return from_pairs(
            url.map(|u| {
                u.query_pairs()
                    .map(|(k, v)| (k.into_owned(), v.into_owned()))
                    .collect()
            })
            .unwrap_or_default(),
        );
    }
    (Some(value.to_string()), None)
}

fn escape_html(value: &str) -> String {
    value
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

fn render_page(title: &str, message: &str, details: Option<&str>) -> String {
    let details = details
        .map(|d| format!("<div class=\"details\">{}</div>", escape_html(d)))
        .unwrap_or_default();
    format!(
        "<!doctype html>\n<html lang=\"en\">\n<head>\n  <meta charset=\"utf-8\" />\n  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n  <title>{title}</title>\n  <style>\n    html {{ color-scheme: dark; }}\n    body {{ margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 24px; background: #09090b; color: #fafafa; font-family: ui-sans-serif, system-ui, sans-serif; text-align: center; }}\n    main {{ max-width: 560px; }}\n    h1 {{ margin: 0 0 10px; font-size: 28px; font-weight: 650; }}\n    p {{ margin: 0; line-height: 1.7; color: #a1a1aa; font-size: 15px; }}\n    .details {{ margin-top: 16px; font-family: ui-monospace, monospace; font-size: 13px; color: #a1a1aa; white-space: pre-wrap; word-break: break-word; }}\n  </style>\n</head>\n<body>\n  <main>\n    <h1>{title}</h1>\n    <p>{}</p>\n    {details}\n  </main>\n</body>\n</html>",
        escape_html(message)
    )
}

/// Page shown after a successful redirect.
pub fn success_html(message: &str) -> String {
    render_page("Authentication successful", message, None)
}

/// Page shown when the redirect is rejected.
pub fn error_html(message: &str, details: Option<&str>) -> String {
    render_page("Authentication failed", message, details)
}

/// The URL to open and what to tell the user.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthInfo {
    pub url: String,
    pub instructions: String,
}

/// Progress callback for [`login`].
pub type ProgressFn = Box<dyn Fn(&str) + Send + Sync>;

/// Callbacks and inputs for [`login`].
pub struct LoginOptions {
    /// Receives the authorization URL; the front end opens it in a browser.
    pub on_auth: Box<dyn FnOnce(AuthInfo) + Send>,
    pub on_progress: Option<ProgressFn>,
    /// A pasted authorization code or redirect URL, racing the browser callback.
    pub manual_input: Option<async_channel::Receiver<String>>,
    pub cancel: CancellationToken,
    /// Codex `originator` (default `pi`).
    pub originator: Option<String>,
    /// Override the provider endpoints (tests).
    pub endpoints: Option<Endpoints>,
}

impl LoginOptions {
    pub fn new(on_auth: impl FnOnce(AuthInfo) + Send + 'static) -> Self {
        Self {
            on_auth: Box::new(on_auth),
            on_progress: None,
            manual_input: None,
            cancel: CancellationToken::new(),
            originator: None,
            endpoints: None,
        }
    }
}

/// Run the login flow for `provider` and return credentials to store.
pub async fn login(
    http: &reqwest::Client,
    provider: &str,
    options: LoginOptions,
) -> Result<OAuthCredentials> {
    let endpoints = match options.endpoints.clone() {
        Some(e) => e,
        None => Endpoints::for_provider(provider)
            .ok_or_else(|| anyhow!("Unknown OAuth provider: {provider}"))?,
    };
    let (verifier, challenge) = generate_pkce();
    let anthropic = provider == "anthropic";
    let state = if anthropic {
        verifier.clone()
    } else {
        random_state()
    };
    let (url, instructions, manual_prompt_state) = if anthropic {
        let url = reqwest::Url::parse_with_params(
            &endpoints.authorize_url,
            &[
                ("code", "true"),
                ("client_id", ANTHROPIC_CLIENT_ID),
                ("response_type", "code"),
                ("redirect_uri", endpoints.redirect_uri.as_str()),
                ("scope", ANTHROPIC_SCOPES),
                ("code_challenge", challenge.as_str()),
                ("code_challenge_method", "S256"),
                ("state", verifier.as_str()),
            ],
        )?;
        (
            url,
            "Complete login in your browser. If the browser is on another machine, paste the final redirect URL here.",
            verifier.clone(),
        )
    } else if provider == "openai-codex" {
        let originator = options.originator.clone().unwrap_or_else(|| "pi".into());
        let url = reqwest::Url::parse_with_params(
            &endpoints.authorize_url,
            &[
                ("response_type", "code"),
                ("client_id", CODEX_CLIENT_ID),
                ("redirect_uri", endpoints.redirect_uri.as_str()),
                ("scope", CODEX_SCOPE),
                ("code_challenge", challenge.as_str()),
                ("code_challenge_method", "S256"),
                ("state", state.as_str()),
                ("id_token_add_organizations", "true"),
                ("codex_cli_simplified_flow", "true"),
                ("originator", originator.as_str()),
            ],
        )?;
        (
            url,
            "A browser window should open. Complete login to finish.",
            state.clone(),
        )
    } else {
        bail!("Unknown OAuth provider: {provider}");
    };

    let listener =
        TcpListener::bind((endpoints.callback_host.as_str(), endpoints.callback_port)).await;
    if listener.is_err() && options.manual_input.is_none() {
        return Err(anyhow!(
            "Could not listen on {}:{} for the OAuth callback: {}",
            endpoints.callback_host,
            endpoints.callback_port,
            listener.err().map(|e| e.to_string()).unwrap_or_default()
        ));
    }
    (options.on_auth)(AuthInfo {
        url: url.to_string(),
        instructions: instructions.into(),
    });

    let callback = async {
        match &listener {
            Ok(l) => wait_for_callback(l, &endpoints.callback_path, &state, provider).await,
            Err(_) => std::future::pending().await,
        }
    };
    let manual = async {
        match &options.manual_input {
            Some(rx) => match rx.recv().await {
                Ok(input) => Some(input),
                Err(_) => std::future::pending().await,
            },
            None => std::future::pending().await,
        }
    };
    let code = tokio::select! {
        code = callback => code,
        input = manual => {
            let input = input.unwrap_or_default();
            let (code, parsed_state) = parse_authorization_input(&input);
            if let Some(s) = parsed_state
                && s != manual_prompt_state
            {
                bail!(if anthropic { "OAuth state mismatch" } else { "State mismatch" });
            }
            code.filter(|c| !c.is_empty()).ok_or_else(|| anyhow!("Missing authorization code"))?
        }
        _ = options.cancel.cancelled() => bail!("Login cancelled"),
    };
    drop(listener);
    if let Some(progress) = &options.on_progress {
        progress("Exchanging authorization code for tokens...");
    }
    if anthropic {
        exchange_anthropic(http, &endpoints, &code, &state, &verifier).await
    } else {
        exchange_codex(http, &endpoints, &code, &verifier).await
    }
}

/// Serve redirects until one carries a valid code.
async fn wait_for_callback(
    listener: &TcpListener,
    path: &str,
    state: &str,
    provider: &str,
) -> String {
    let label = if provider == "anthropic" {
        "Anthropic"
    } else {
        "OpenAI"
    };
    loop {
        let Ok((mut socket, _)) = listener.accept().await else {
            continue;
        };
        let Some(target) = read_request_target(&mut socket).await else {
            continue;
        };
        let url = reqwest::Url::parse(&format!("http://localhost{target}")).ok();
        let get = |k: &str| {
            url.as_ref().and_then(|u| {
                u.query_pairs()
                    .find(|(key, _)| key == k)
                    .map(|(_, v)| v.into_owned())
            })
        };
        let (status, page, code) = if url.as_ref().map(|u| u.path()) != Some(path) {
            (404, error_html("Callback route not found.", None), None)
        } else if let Some(error) = get("error") {
            (
                400,
                error_html(
                    &format!("{label} authentication did not complete."),
                    Some(&format!("Error: {error}")),
                ),
                None,
            )
        } else if get("state").as_deref() != Some(state) {
            if get("code").is_none() || get("state").is_none() {
                (
                    400,
                    error_html(
                        if provider == "anthropic" {
                            "Missing code or state parameter."
                        } else {
                            "State mismatch."
                        },
                        None,
                    ),
                    None,
                )
            } else {
                (400, error_html("State mismatch.", None), None)
            }
        } else if let Some(code) = get("code").filter(|c| !c.is_empty()) {
            (
                200,
                success_html(&format!(
                    "{label} authentication completed. You can close this window."
                )),
                Some(code),
            )
        } else {
            (400, error_html("Missing authorization code.", None), None)
        };
        let reason = if status == 200 {
            "OK"
        } else if status == 404 {
            "Not Found"
        } else {
            "Bad Request"
        };
        let response = format!(
            "HTTP/1.1 {status} {reason}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{page}",
            page.len()
        );
        let _ = socket.write_all(response.as_bytes()).await;
        let _ = socket.shutdown().await;
        if let Some(code) = code {
            return code;
        }
    }
}

async fn read_request_target(socket: &mut tokio::net::TcpStream) -> Option<String> {
    let mut buf = Vec::new();
    let mut chunk = [0u8; 2048];
    let read = async {
        loop {
            let n = socket.read(&mut chunk).await.ok()?;
            if n == 0 {
                break;
            }
            buf.extend_from_slice(&chunk[..n]);
            if buf.windows(4).any(|w| w == b"\r\n\r\n") || buf.len() > 16_384 {
                break;
            }
        }
        Some(())
    };
    tokio::time::timeout(Duration::from_secs(10), read)
        .await
        .ok()??;
    let head = String::from_utf8_lossy(&buf);
    let mut first = head.lines().next()?.split_whitespace();
    let _method = first.next()?;
    Some(first.next()?.to_string())
}

async fn post_json(http: &reqwest::Client, url: &str, body: Value) -> Result<String> {
    let response = http
        .post(url)
        .header("Content-Type", "application/json")
        .header("Accept", "application/json")
        .json(&body)
        .timeout(Duration::from_secs(30))
        .send()
        .await?;
    let status = response.status().as_u16();
    let text = response.text().await.unwrap_or_default();
    if !(200..300).contains(&status) {
        bail!("HTTP request failed. status={status}; url={url}; body={text}");
    }
    Ok(text)
}

fn anthropic_credentials(text: &str, url: &str) -> Result<OAuthCredentials> {
    let data: Value = serde_json::from_str(text).map_err(|e| {
        anyhow!("Token exchange returned invalid JSON. url={url}; body={text}; details={e}")
    })?;
    let access = data.get("access_token").and_then(Value::as_str);
    let refresh = data.get("refresh_token").and_then(Value::as_str);
    let expires_in = data.get("expires_in").and_then(Value::as_i64);
    let (Some(access), Some(refresh), Some(expires_in)) = (access, refresh, expires_in) else {
        bail!("Anthropic token response missing access_token, refresh_token, or expires_in");
    };
    Ok(OAuthCredentials {
        access: access.into(),
        refresh: refresh.into(),
        expires: now_millis() + expires_in * 1000 - 5 * 60 * 1000,
        ..Default::default()
    })
}

async fn exchange_anthropic(
    http: &reqwest::Client,
    endpoints: &Endpoints,
    code: &str,
    state: &str,
    verifier: &str,
) -> Result<OAuthCredentials> {
    let url = &endpoints.token_url;
    let text = post_json(
        http,
        url,
        json!({
            "grant_type": "authorization_code",
            "client_id": ANTHROPIC_CLIENT_ID,
            "code": code,
            "state": state,
            "redirect_uri": endpoints.redirect_uri,
            "code_verifier": verifier,
        }),
    )
    .await
    .map_err(|e| {
        anyhow!(
            "Token exchange request failed. url={url}; redirect_uri={}; response_type=authorization_code; details={e}",
            endpoints.redirect_uri
        )
    })?;
    anthropic_credentials(&text, url)
}

async fn codex_token(
    http: &reqwest::Client,
    url: &str,
    form: &[(&str, &str)],
    what: &str,
) -> Result<OAuthCredentials> {
    let response = http
        .post(url)
        .form(form)
        .timeout(Duration::from_secs(30))
        .send()
        .await
        .map_err(|e| anyhow!("OpenAI Codex token {what} error: {e}"))?;
    let status = response.status();
    let text = response.text().await.unwrap_or_default();
    if !status.is_success() {
        let detail = if text.is_empty() {
            status.canonical_reason().unwrap_or_default().to_string()
        } else {
            text
        };
        bail!(
            "OpenAI Codex token {what} failed ({}): {detail}",
            status.as_u16()
        );
    }
    let json: Value = serde_json::from_str(&text).unwrap_or(Value::Null);
    let access = json.get("access_token").and_then(Value::as_str);
    let refresh = json.get("refresh_token").and_then(Value::as_str);
    let expires_in = json.get("expires_in").and_then(Value::as_i64);
    let (Some(access), Some(refresh), Some(expires_in)) = (access, refresh, expires_in) else {
        bail!("OpenAI Codex token {what} response missing fields: {json}");
    };
    let account_id =
        jwt_account_id(access).ok_or_else(|| anyhow!("Failed to extract accountId from token"))?;
    Ok(OAuthCredentials {
        access: access.into(),
        refresh: refresh.into(),
        expires: now_millis() + expires_in * 1000,
        account_id: Some(account_id),
        ..Default::default()
    })
}

async fn exchange_codex(
    http: &reqwest::Client,
    endpoints: &Endpoints,
    code: &str,
    verifier: &str,
) -> Result<OAuthCredentials> {
    codex_token(
        http,
        &endpoints.token_url,
        &[
            ("grant_type", "authorization_code"),
            ("client_id", CODEX_CLIENT_ID),
            ("code", code),
            ("code_verifier", verifier),
            ("redirect_uri", endpoints.redirect_uri.as_str()),
        ],
        "exchange",
    )
    .await
}

/// Refresh `provider`'s credentials with the real token endpoint.
pub async fn refresh_token(
    http: &reqwest::Client,
    provider: &str,
    refresh: &str,
) -> Result<OAuthCredentials> {
    let endpoints = Endpoints::for_provider(provider)
        .ok_or_else(|| anyhow!("OAuth token refresh is not supported for {provider}"))?;
    refresh_with(http, provider, refresh, &endpoints.token_url).await
}

/// Refresh against an explicit token URL.
pub async fn refresh_with(
    http: &reqwest::Client,
    provider: &str,
    refresh: &str,
    token_url: &str,
) -> Result<OAuthCredentials> {
    match provider {
        "anthropic" => {
            let text = post_json(
                http,
                token_url,
                json!({ "grant_type": "refresh_token", "client_id": ANTHROPIC_CLIENT_ID, "refresh_token": refresh }),
            )
            .await
            .map_err(|e| anyhow!("Anthropic token refresh request failed. url={token_url}; details={e}"))?;
            anthropic_credentials(&text, token_url)
        }
        "openai-codex" => {
            codex_token(
                http,
                token_url,
                &[
                    ("grant_type", "refresh_token"),
                    ("refresh_token", refresh),
                    ("client_id", CODEX_CLIENT_ID),
                ],
                "refresh",
            )
            .await
        }
        other => bail!("OAuth token refresh is not supported for {other}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::provider::test_server::{MockResponse, serve};

    fn codex_jwt() -> String {
        let engine = base64::engine::general_purpose::URL_SAFE_NO_PAD;
        format!(
            "h.{}.s",
            engine.encode(r#"{"https://api.openai.com/auth":{"chatgpt_account_id":"acct"}}"#)
        )
    }

    #[test]
    fn pkce_challenge_is_sha256_of_verifier() {
        let (verifier, challenge) = generate_pkce();
        assert_eq!(verifier.len(), 43);
        let expected = base64::engine::general_purpose::URL_SAFE_NO_PAD
            .encode(Sha256::digest(verifier.as_bytes()));
        assert_eq!(challenge, expected);
        assert_ne!(generate_pkce().0, verifier);
    }

    #[test]
    fn parses_pasted_codes() {
        assert_eq!(
            parse_authorization_input("http://localhost:1455/auth/callback?code=abc&state=xyz"),
            (Some("abc".into()), Some("xyz".into()))
        );
        assert_eq!(
            parse_authorization_input("abc#xyz"),
            (Some("abc".into()), Some("xyz".into()))
        );
        assert_eq!(
            parse_authorization_input("code=abc&state=s"),
            (Some("abc".into()), Some("s".into()))
        );
        assert_eq!(
            parse_authorization_input(" abc "),
            (Some("abc".into()), None)
        );
        assert_eq!(parse_authorization_input("  "), (None, None));
    }

    #[test]
    fn pages_escape_html() {
        let page = error_html("bad <x>", Some("Error: a&b"));
        assert!(page.contains("<title>Authentication failed</title>"));
        assert!(page.contains("bad &lt;x&gt;"));
        assert!(page.contains("Error: a&amp;b"));
        assert!(success_html("ok").contains("Authentication successful"));
    }

    async fn free_port() -> u16 {
        let l = TcpListener::bind("127.0.0.1:0").await.unwrap();
        l.local_addr().unwrap().port()
    }

    #[tokio::test]
    async fn anthropic_login_via_browser_callback() {
        let (token_base, requests) = serve(vec![MockResponse::json(
            200,
            r#"{"access_token":"sk-ant-oat-x","refresh_token":"ref","expires_in":3600}"#,
        )])
        .await;
        let port = free_port().await;
        let endpoints = Endpoints {
            authorize_url: "https://claude.ai/oauth/authorize".into(),
            token_url: format!("{token_base}/v1/oauth/token"),
            callback_host: "127.0.0.1".into(),
            callback_port: port,
            callback_path: "/callback".into(),
            redirect_uri: format!("http://localhost:{port}/callback"),
        };
        let (url_tx, url_rx) = async_channel::bounded::<AuthInfo>(1);
        let mut options = LoginOptions::new(move |info| {
            let _ = url_tx.try_send(info);
        });
        options.endpoints = Some(endpoints);
        let http = reqwest::Client::new();
        let login_http = http.clone();
        let task = tokio::spawn(async move { login(&login_http, "anthropic", options).await });
        let info = url_rx.recv().await.unwrap();
        let url = reqwest::Url::parse(&info.url).unwrap();
        let q = |k: &str| {
            url.query_pairs()
                .find(|(key, _)| key == k)
                .unwrap()
                .1
                .into_owned()
        };
        assert_eq!(q("client_id"), ANTHROPIC_CLIENT_ID);
        assert_eq!(q("code_challenge_method"), "S256");
        assert_eq!(q("scope"), ANTHROPIC_SCOPES);
        let state = q("state");
        // A wrong state is rejected and the server keeps waiting.
        let bad = http
            .get(format!(
                "http://127.0.0.1:{port}/callback?code=c&state=nope"
            ))
            .send()
            .await
            .unwrap();
        assert_eq!(bad.status().as_u16(), 400);
        let ok = http
            .get(format!(
                "http://127.0.0.1:{port}/callback?code=the-code&state={state}"
            ))
            .send()
            .await
            .unwrap();
        assert_eq!(ok.status().as_u16(), 200);
        assert!(
            ok.text()
                .await
                .unwrap()
                .contains("Anthropic authentication completed")
        );
        let creds = task.await.unwrap().unwrap();
        assert_eq!(creds.access, "sk-ant-oat-x");
        assert_eq!(creds.refresh, "ref");
        let expected = now_millis() + 3_600_000 - 300_000;
        assert!((creds.expires - expected).abs() < 5_000);
        let body = requests.lock().unwrap()[0].json();
        assert_eq!(body["grant_type"], "authorization_code");
        assert_eq!(body["code"], "the-code");
        assert_eq!(body["code_verifier"], state);
    }

    #[tokio::test]
    async fn codex_login_via_pasted_redirect() {
        let token = codex_jwt();
        let (token_base, requests) = serve(vec![MockResponse::json(
            200,
            json!({"access_token": token, "refresh_token": "r", "expires_in": 100}).to_string(),
        )])
        .await;
        let port = free_port().await;
        let (tx, rx) = async_channel::bounded::<String>(1);
        let (url_tx, url_rx) = async_channel::bounded::<AuthInfo>(1);
        let mut options = LoginOptions::new(move |info| {
            let _ = url_tx.try_send(info);
        });
        options.manual_input = Some(rx);
        options.endpoints = Some(Endpoints {
            authorize_url: "https://auth.openai.com/oauth/authorize".into(),
            token_url: format!("{token_base}/oauth/token"),
            callback_host: "127.0.0.1".into(),
            callback_port: port,
            callback_path: "/auth/callback".into(),
            redirect_uri: "http://localhost:1455/auth/callback".into(),
        });
        let http = reqwest::Client::new();
        let task = tokio::spawn(async move { login(&http, "openai-codex", options).await });
        let info = url_rx.recv().await.unwrap();
        let url = reqwest::Url::parse(&info.url).unwrap();
        let state = url
            .query_pairs()
            .find(|(k, _)| k == "state")
            .unwrap()
            .1
            .into_owned();
        assert_eq!(state.len(), 32);
        assert!(info.url.contains("originator=pi"));
        assert!(info.url.contains("codex_cli_simplified_flow=true"));
        tx.send(format!(
            "http://localhost:1455/auth/callback?code=pasted&state={state}"
        ))
        .await
        .unwrap();
        let creds = task.await.unwrap().unwrap();
        assert_eq!(creds.account_id.as_deref(), Some("acct"));
        assert_eq!(creds.refresh, "r");
        let form = requests.lock().unwrap()[0].body.clone();
        assert!(form.contains("grant_type=authorization_code"));
        assert!(form.contains("code=pasted"));
        assert!(form.contains(&format!("client_id={CODEX_CLIENT_ID}")));
    }

    #[tokio::test]
    async fn pasted_state_mismatch_and_cancel() {
        let port = free_port().await;
        let (tx, rx) = async_channel::bounded::<String>(1);
        let mut options = LoginOptions::new(|_| {});
        options.manual_input = Some(rx);
        let mut endpoints = Endpoints::for_provider("anthropic").unwrap();
        endpoints.callback_host = "127.0.0.1".into();
        endpoints.callback_port = port;
        options.endpoints = Some(endpoints.clone());
        tx.send("code#wrong-state".into()).await.unwrap();
        let err = login(&reqwest::Client::new(), "anthropic", options)
            .await
            .unwrap_err();
        assert_eq!(err.to_string(), "OAuth state mismatch");

        let mut options = LoginOptions::new(|_| {});
        options.endpoints = Some(endpoints);
        options.cancel.cancel();
        let err = login(&reqwest::Client::new(), "anthropic", options)
            .await
            .unwrap_err();
        assert_eq!(err.to_string(), "Login cancelled");
    }

    #[tokio::test]
    async fn refreshes_tokens() {
        let token = codex_jwt();
        let (base, requests) = serve(vec![
            MockResponse::json(
                200,
                r#"{"access_token":"new","refresh_token":"r2","expires_in":60}"#,
            ),
            MockResponse::json(
                200,
                json!({"access_token": token, "refresh_token": "r3", "expires_in": 60}).to_string(),
            ),
            MockResponse::json(401, "denied"),
        ])
        .await;
        let http = reqwest::Client::new();
        let a = refresh_with(&http, "anthropic", "r1", &format!("{base}/t"))
            .await
            .unwrap();
        assert_eq!((a.access.as_str(), a.refresh.as_str()), ("new", "r2"));
        let c = refresh_with(&http, "openai-codex", "r2", &format!("{base}/t"))
            .await
            .unwrap();
        assert_eq!(c.account_id.as_deref(), Some("acct"));
        let err = refresh_with(&http, "openai-codex", "r3", &format!("{base}/t"))
            .await
            .unwrap_err();
        assert_eq!(
            err.to_string(),
            "OpenAI Codex token refresh failed (401): denied"
        );
        {
            let reqs = requests.lock().unwrap();
            assert_eq!(reqs[0].json()["grant_type"], "refresh_token");
            assert!(reqs[1].body.contains("grant_type=refresh_token"));
        }
        assert!(refresh_token(&http, "github-copilot", "x").await.is_err());
    }
}
