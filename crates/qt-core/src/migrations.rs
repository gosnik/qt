//! Startup migrations of the agent home (archived
//! `app/config/migrations.cpp`) plus the Rust milestone-1 provider id
//! migration.
//!
//! - `oauth.json` and `settings.json` `apiKeys` move into `auth.json`
//!   (Pi's legacy shapes, which [`crate::auth::AuthStore`] reads).
//! - Session files stored directly in the agent home move to
//!   `sessions/--<encoded cwd>--/`.
//! - Managed `fd` / `rg` binaries move from `tools/` to `bin/`.
//! - `commands/` directories (agent home and `<cwd>/.pi`) become `prompts/`.
//! - `defaultProvider: "ollama"` (the first Rust milestone's id for the
//!   first Ollama instance) becomes `ollama:<instance id>`.

use std::fs;
use std::io::{BufRead as _, BufReader};
use std::path::Path;

use serde_json::{Map, Value, json};

use crate::config::{AgentHome, read_json_object, write_json_atomic};

/// What [`run_startup_migrations`] changed.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct StartupMigrationResult {
    /// Providers whose credentials moved into `auth.json`.
    pub migrated_auth_providers: Vec<String>,
    pub moved_agent_root_sessions: bool,
    pub moved_managed_binaries: bool,
    pub migrated_global_commands_dir: bool,
    pub migrated_project_commands_dir: bool,
    /// `(from, to)` when `defaultProvider` was rewritten.
    pub migrated_default_provider: Option<(String, String)>,
}

impl StartupMigrationResult {
    pub fn changed_anything(&self) -> bool {
        !self.migrated_auth_providers.is_empty()
            || self.moved_agent_root_sessions
            || self.moved_managed_binaries
            || self.migrated_global_commands_dir
            || self.migrated_project_commands_dir
            || self.migrated_default_provider.is_some()
    }
}

fn read_json(path: &Path) -> Option<Value> {
    let text = fs::read_to_string(path).ok()?;
    serde_json::from_str(&text).ok()
}

/// Move `oauth.json` and `settings.json` `apiKeys` into a new `auth.json`
/// (skipped when `auth.json` exists). Returns the migrated provider ids.
pub fn migrate_auth_to_auth_json(agent_home: &Path) -> Vec<String> {
    let auth_path = agent_home.join("auth.json");
    let oauth_path = agent_home.join("oauth.json");
    let settings_path = agent_home.join("settings.json");
    let mut providers = Vec::new();
    if auth_path.exists() {
        return providers;
    }
    let mut migrated = Map::new();
    if let Some(Value::Object(oauth)) = read_json(&oauth_path) {
        for (provider, record) in oauth {
            let Value::Object(mut record) = record else {
                continue;
            };
            record.insert("type".into(), json!("oauth"));
            migrated.insert(provider.clone(), Value::Object(record));
            providers.push(provider);
        }
        let mut target = oauth_path.clone().into_os_string();
        target.push(".migrated");
        let _ = fs::rename(&oauth_path, target);
    }
    if let Some(Value::Object(mut settings)) = read_json(&settings_path)
        && let Some(Value::Object(keys)) = settings.get("apiKeys").cloned()
    {
        for (provider, key) in keys {
            let Value::String(key) = key else {
                continue;
            };
            if migrated.contains_key(&provider) {
                continue;
            }
            migrated.insert(provider.clone(), json!({"type": "api_key", "key": key}));
            providers.push(provider);
        }
        settings.remove("apiKeys");
        let _ = write_json_atomic(&settings_path, &Value::Object(settings), false);
    }
    if !migrated.is_empty() {
        let _ = write_json_atomic(&auth_path, &Value::Object(migrated), true);
    }
    providers
}

fn first_non_empty_line(path: &Path) -> Option<String> {
    let file = fs::File::open(path).ok()?;
    BufReader::new(file)
        .lines()
        .map_while(Result::ok)
        .find(|l| !l.trim().is_empty())
}

/// Pi's per-cwd session folder name: `--<cwd with separators as ->--`.
pub fn encode_session_cwd(cwd: &str) -> String {
    let trimmed = cwd.trim_start_matches(['/', '\\']);
    let encoded: String = trimmed
        .chars()
        .map(|c| {
            if matches!(c, '/' | '\\' | ':') {
                '-'
            } else {
                c
            }
        })
        .collect();
    format!("--{encoded}--")
}

fn move_file_if_missing_target(source: &Path, target: &Path) -> bool {
    if target.exists() {
        return false;
    }
    if let Some(parent) = target.parent()
        && fs::create_dir_all(parent).is_err()
    {
        return false;
    }
    fs::rename(source, target).is_ok()
}

/// Move `*.jsonl` session files stored in the agent home itself into
/// `sessions/<encoded cwd>/`.
pub fn migrate_sessions_from_agent_root(agent_home: &Path) -> bool {
    let Ok(entries) = fs::read_dir(agent_home) else {
        return false;
    };
    let mut moved_any = false;
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_file() || path.extension().and_then(|e| e.to_str()) != Some("jsonl") {
            continue;
        }
        let Some(line) = first_non_empty_line(&path) else {
            continue;
        };
        let Ok(Value::Object(header)) = serde_json::from_str::<Value>(&line) else {
            continue;
        };
        if header.get("type").and_then(Value::as_str) != Some("session") {
            continue;
        }
        let Some(cwd) = header.get("cwd").and_then(Value::as_str) else {
            continue;
        };
        let Some(name) = path.file_name() else {
            continue;
        };
        let target = agent_home
            .join("sessions")
            .join(encode_session_cwd(cwd))
            .join(name);
        moved_any |= move_file_if_missing_target(&path, &target);
    }
    moved_any
}

/// Move managed `fd`/`rg` binaries from `tools/` to `bin/`; a source whose
/// target already exists is removed.
pub fn migrate_tools_to_bin(agent_home: &Path) -> bool {
    let tools = agent_home.join("tools");
    let bin = agent_home.join("bin");
    let mut moved_any = false;
    for name in ["fd", "rg", "fd.exe", "rg.exe"] {
        let source = tools.join(name);
        if !source.exists() {
            continue;
        }
        if fs::create_dir_all(&bin).is_err() {
            continue;
        }
        let target = bin.join(name);
        if target.exists() {
            let _ = fs::remove_file(&source);
            continue;
        }
        if fs::rename(&source, &target).is_ok() {
            moved_any = true;
        }
    }
    moved_any
}

/// Rename `<base>/commands` to `<base>/prompts` when only the former exists.
pub fn migrate_commands_to_prompts(base: &Path) -> bool {
    let commands = base.join("commands");
    let prompts = base.join("prompts");
    if !commands.exists() || prompts.exists() {
        return false;
    }
    fs::rename(&commands, &prompts).is_ok()
}

/// The provider id a milestone-1 id maps to: bare `ollama` becomes
/// `ollama:<first instance id>` (`ollama:local` without instances).
pub fn migrate_provider_id(provider: &str, settings: &Map<String, Value>) -> Option<String> {
    if provider != "ollama" {
        return None;
    }
    let first = settings
        .get("ollama")
        .and_then(|o| o.get("instances"))
        .and_then(Value::as_array)
        .and_then(|list| {
            list.iter()
                .find(|i| i.get("enabled").and_then(Value::as_bool) != Some(false))
                .or_else(|| list.first())
        })
        .and_then(|i| i.get("id"))
        .and_then(Value::as_str)
        .filter(|id| !id.trim().is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| "local".to_string());
    Some(crate::ollama::provider_id(&first))
}

/// Rewrite `defaultProvider: "ollama"` in the global settings file.
pub fn migrate_default_provider(agent_home: &Path) -> Option<(String, String)> {
    let path = agent_home.join("settings.json");
    let mut settings = read_json_object(&path).ok()?;
    let current = settings.get("defaultProvider")?.as_str()?.to_string();
    let next = migrate_provider_id(&current, &settings)?;
    settings.insert("defaultProvider".into(), json!(next));
    write_json_atomic(&path, &Value::Object(settings), false).ok()?;
    Some((current, next))
}

/// Run every startup migration for `home` and the project at `cwd`.
pub fn run_startup_migrations(cwd: &Path, home: &AgentHome) -> StartupMigrationResult {
    let root = home.root();
    StartupMigrationResult {
        migrated_auth_providers: migrate_auth_to_auth_json(root),
        moved_agent_root_sessions: migrate_sessions_from_agent_root(root),
        moved_managed_binaries: migrate_tools_to_bin(root),
        migrated_global_commands_dir: migrate_commands_to_prompts(root),
        migrated_project_commands_dir: migrate_commands_to_prompts(&cwd.join(".pi")),
        migrated_default_provider: migrate_default_provider(root),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn migrates_credentials_sessions_binaries_and_commands() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path().join("home"));
        let root = home.root();
        fs::create_dir_all(root.join("tools")).unwrap();
        fs::create_dir_all(root.join("bin")).unwrap();
        fs::create_dir_all(root.join("commands")).unwrap();
        fs::write(root.join("commands/review.md"), "Review").unwrap();
        fs::write(
            root.join("oauth.json"),
            r#"{"anthropic":{"access":"a","refresh":"r","expires":1}}"#,
        )
        .unwrap();
        fs::write(
            root.join("settings.json"),
            r#"{"apiKeys":{"openai":"sk-o","anthropic":"ignored"},"defaultProvider":"ollama",
               "ollama":{"instances":[{"id":"gpu","baseUrl":"http://x","enabled":true}]}}"#,
        )
        .unwrap();
        fs::write(
            root.join("old.jsonl"),
            "\n{\"type\":\"session\",\"id\":\"s\",\"cwd\":\"/work/app\"}\n",
        )
        .unwrap();
        fs::write(root.join("notes.jsonl"), "{\"type\":\"other\"}\n").unwrap();
        fs::write(root.join("tools/rg"), "rg").unwrap();
        fs::write(root.join("tools/fd"), "fd-old").unwrap();
        fs::write(root.join("bin/fd"), "fd-new").unwrap();
        let project = dir.path().join("proj");
        fs::create_dir_all(project.join(".pi/commands")).unwrap();

        let result = run_startup_migrations(&project, &home);
        assert_eq!(result.migrated_auth_providers, ["anthropic", "openai"]);
        assert!(result.moved_agent_root_sessions);
        assert!(result.moved_managed_binaries);
        assert!(result.migrated_global_commands_dir);
        assert!(result.migrated_project_commands_dir);
        assert_eq!(
            result.migrated_default_provider,
            Some(("ollama".into(), "ollama:gpu".into()))
        );
        assert!(result.changed_anything());

        assert!(root.join("oauth.json.migrated").exists());
        let auth = crate::auth::AuthStore::load(&home).unwrap();
        assert_eq!(auth.stored_api_key("openai").as_deref(), Some("sk-o"));
        assert!(matches!(
            auth.credential("anthropic"),
            Some(crate::auth::Credential::OAuth(_))
        ));
        let settings = read_json_object(&root.join("settings.json")).unwrap();
        assert!(!settings.contains_key("apiKeys"));
        assert_eq!(settings["defaultProvider"], "ollama:gpu");
        assert!(root.join("sessions/--work-app--/old.jsonl").exists());
        assert!(root.join("notes.jsonl").exists());
        assert_eq!(fs::read_to_string(root.join("bin/rg")).unwrap(), "rg");
        assert_eq!(fs::read_to_string(root.join("bin/fd")).unwrap(), "fd-new");
        assert!(!root.join("tools/fd").exists());
        assert!(root.join("prompts/review.md").exists());
        assert!(project.join(".pi/prompts").is_dir());

        // Second run is a no-op.
        let again = run_startup_migrations(&project, &home);
        assert!(!again.changed_anything(), "{again:?}");
    }

    #[test]
    fn provider_id_migration_defaults_to_local() {
        assert_eq!(
            migrate_provider_id("ollama", &Map::new()).as_deref(),
            Some("ollama:local")
        );
        assert_eq!(migrate_provider_id("openai", &Map::new()), None);
        assert_eq!(encode_session_cwd("C:\\work\\x"), "--C--work-x--");
    }
}
