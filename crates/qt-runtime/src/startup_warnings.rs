//! Interactive startup warnings (archived
//! `interactive_startup_diagnostics.cpp`): the Anthropic subscription-auth
//! notice and the list of installed packages that have updates. The runtime
//! appends them to its diagnostics for interactive use; print, JSON and RPC
//! runs never showed them.

use std::path::Path;

use qt_core::config::AuthStore;
use qt_core::provider::ModelRef;
use qt_extensions::packages::{
    PackageCommand, PackageCommandKind, UpdateTarget, UpdateTargetKind, load_package_settings,
    package_settings_plan,
};

/// Archived wording, shown when Anthropic is used with subscription auth.
pub const ANTHROPIC_SUBSCRIPTION_AUTH_WARNING: &str = "Anthropic subscription auth is active. Third-party harness usage draws from extra usage and is billed per token, not your Claude plan limits. Manage extra usage at https://claude.ai/settings/usage.";

fn is_subscription_key(secret: &str) -> bool {
    secret.starts_with("sk-ant-oat")
}

/// The subscription-auth warning when `model` is an Anthropic model and the
/// stored credential is an OAuth login or an `sk-ant-oat…` token, or the
/// environment secret (`env_secret`, `ANTHROPIC_OAUTH_TOKEN` then
/// `ANTHROPIC_API_KEY`) is one.
pub fn anthropic_subscription_auth_warning(
    model: Option<&ModelRef>,
    auth: &AuthStore,
    env_secret: Option<&str>,
) -> Option<String> {
    if model?.provider != "anthropic" {
        return None;
    }
    let stored = match auth.stored_credential("anthropic") {
        Some(qt_core::auth::Credential::OAuth(_)) => true,
        Some(qt_core::auth::Credential::ApiKey(key)) => is_subscription_key(&key),
        None => false,
    };
    (stored || env_secret.is_some_and(is_subscription_key))
        .then(|| ANTHROPIC_SUBSCRIPTION_AUTH_WARNING.to_string())
}

/// `Package updates available: <sources>` for the configured packages an
/// `update` would refresh (unpinned npm and git sources), as the archived
/// `make_package_update_warning`. Nothing offline or when the package
/// settings cannot be read.
pub fn package_update_warning(root: &Path, agent_home: &Path, offline: bool) -> Option<String> {
    if offline || qt_extensions::packages::offline_mode() {
        return None;
    }
    let settings = load_package_settings(root, agent_home).ok()?;
    let command = PackageCommand {
        command: PackageCommandKind::Update,
        source: None,
        local: false,
        force: false,
        help: false,
        update_target: Some(UpdateTarget {
            kind: UpdateTargetKind::Extensions,
            source: None,
        }),
        invalid_option: None,
        invalid_argument: None,
        missing_option_value: None,
        conflicting_options: None,
    };
    let plan = package_settings_plan(&command, root, agent_home, &settings);
    if plan.package_updates.is_empty() {
        return None;
    }
    let sources: Vec<&str> = plan
        .package_updates
        .iter()
        .map(|p| p.source.as_str())
        .collect();
    Some(format!("Package updates available: {}", sources.join(", ")))
}

/// Append `message` unless it is already there (archived
/// `append_unique_message`).
pub fn push_unique(diagnostics: &mut Vec<String>, message: String) {
    if !message.is_empty() && !diagnostics.contains(&message) {
        diagnostics.push(message);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn auth(home: &Path, value: serde_json::Value) -> AuthStore {
        let path = home.join("auth.json");
        std::fs::write(&path, value.to_string()).unwrap();
        AuthStore::load_path(&path).unwrap()
    }

    #[test]
    fn subscription_auth_warning_conditions() {
        let dir = tempfile::tempdir().unwrap();
        let anthropic = ModelRef::new("anthropic", "claude-sonnet-4-5");
        let other = ModelRef::new("openai", "gpt-4o");
        let empty = AuthStore::default();
        assert_eq!(
            anthropic_subscription_auth_warning(Some(&anthropic), &empty, Some("sk-ant-oat01-x"))
                .as_deref(),
            Some(ANTHROPIC_SUBSCRIPTION_AUTH_WARNING)
        );
        assert!(
            anthropic_subscription_auth_warning(Some(&anthropic), &empty, Some("sk-ant-api03"))
                .is_none()
        );
        assert!(
            anthropic_subscription_auth_warning(Some(&other), &empty, Some("sk-ant-oat01-x"))
                .is_none()
        );
        assert!(anthropic_subscription_auth_warning(None, &empty, Some("sk-ant-oat")).is_none());
        let stored = auth(
            dir.path(),
            json!({"providers": {"anthropic": {"type": "api_key", "key": "sk-ant-oat01-stored"}}}),
        );
        assert!(anthropic_subscription_auth_warning(Some(&anthropic), &stored, None).is_some());
        let plain = auth(
            dir.path(),
            json!({"providers": {"anthropic": {"type": "api_key", "key": "sk-ant-api03-x"}}}),
        );
        assert!(anthropic_subscription_auth_warning(Some(&anthropic), &plain, None).is_none());
    }

    #[test]
    fn package_updates_list_unpinned_sources() {
        let dir = tempfile::tempdir().unwrap();
        let home = dir.path().join("home");
        let work = dir.path().join("work");
        std::fs::create_dir_all(&home).unwrap();
        std::fs::create_dir_all(&work).unwrap();
        assert!(package_update_warning(&work, &home, false).is_none());
        std::fs::write(
            home.join("settings.json"),
            json!({"packages": ["npm:foo", "npm:bar@1.2.3", "git:github.com/a/b", "./local"]})
                .to_string(),
        )
        .unwrap();
        assert_eq!(
            package_update_warning(&work, &home, false).as_deref(),
            Some("Package updates available: npm:foo, git:github.com/a/b")
        );
        assert!(package_update_warning(&work, &home, true).is_none());
    }

    #[test]
    fn unique_messages() {
        let mut d = vec!["a".to_string()];
        push_unique(&mut d, "a".into());
        push_unique(&mut d, String::new());
        push_unique(&mut d, "b".into());
        assert_eq!(d, ["a", "b"]);
    }
}
