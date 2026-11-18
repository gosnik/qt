//! Global and project settings layers.
//!
//! The agent home holds the global settings (`~/.qt/settings.json`, or
//! `settings.yaml` / `settings.yml`); a project may add
//! `<workspace>/.pi/settings.{json,yaml,yml}`. Project values are merged
//! over the global ones, except that a project may only narrow the global
//! policy (archived `merge_project_settings_patch`): it cannot enable
//! external filesystem access, enable memory features the global settings
//! leave off, raise memory budgets or relax memory policies. A violating
//! project file is ignored as a whole and reported as a diagnostic.

use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

use crate::config::{AgentHome, Settings, read_settings_file};

/// The two settings layers and their effective merge.
#[derive(Debug, Clone, Default)]
pub struct LayeredSettings {
    /// Global settings (the layer qt writes back).
    pub global: Settings,
    /// Project settings as read (empty when absent or rejected).
    pub project: Map<String, Value>,
    /// The project settings file, when one exists.
    pub project_path: Option<PathBuf>,
    /// Problems reading or validating the project layer.
    pub diagnostics: Vec<String>,
}

impl LayeredSettings {
    /// Load the global settings from `home` and the project settings from
    /// `<workspace_root>/.pi/` (`workspace_root` defaults to the policy root
    /// derived from `cwd`).
    pub fn load(home: &AgentHome, cwd: &Path) -> Self {
        let mut diagnostics = Vec::new();
        let global = match Settings::load(home) {
            Ok(s) => s,
            Err(e) => {
                diagnostics.push(format!("{e:#}"));
                Settings::from_raw(Map::new(), Some(home.clone()))
            }
        };
        let root = crate::policy::WorkspacePolicy::from_settings(cwd, global.raw(), None)
            .map(|p| p.root().to_path_buf())
            .unwrap_or_else(|_| cwd.to_path_buf());
        let mut layered = Self {
            global,
            project: Map::new(),
            project_path: None,
            diagnostics,
        };
        layered.load_project(&root.join(".pi"));
        layered
    }

    /// Replace the project layer with the settings file in `dir`.
    pub fn load_project(&mut self, dir: &Path) {
        self.project = Map::new();
        self.project_path = crate::config::existing_settings_file(dir);
        let Some(path) = self.project_path.clone() else {
            return;
        };
        match read_settings_file(&path) {
            Ok(project) => match validate_project_narrowing(self.global.raw(), &project) {
                Ok(()) => self.project = project,
                Err(e) => self
                    .diagnostics
                    .push(format!("invalid settings in {}: {e}", path.display())),
            },
            Err(e) => self.diagnostics.push(format!("{e:#}")),
        }
    }

    /// Global settings with the project layer merged on top.
    pub fn effective(&self) -> Settings {
        if self.project.is_empty() {
            self.global.clone()
        } else {
            self.global.merged_with(&self.project)
        }
    }
}

fn lookup<'a>(map: &'a Map<String, Value>, path: &str) -> Option<&'a Value> {
    let mut parts = path.split('.');
    let mut current = map.get(parts.next()?)?;
    for part in parts {
        current = current.get(part)?;
    }
    Some(current)
}

fn get_bool(map: &Map<String, Value>, path: &str) -> Option<bool> {
    lookup(map, path).and_then(Value::as_bool)
}

fn get_num(map: &Map<String, Value>, path: &str) -> Option<f64> {
    lookup(map, path).and_then(Value::as_f64)
}

fn get_str<'a>(map: &'a Map<String, Value>, path: &str) -> Option<&'a str> {
    lookup(map, path).and_then(Value::as_str)
}

fn capture_scope(value: &str) -> i32 {
    match value {
        "disabled" => 0,
        "session_end" => 1,
        _ => 2,
    }
}

fn recall_policy_scope(value: &str) -> i32 {
    match value {
        "strict" => 0,
        "exploratory" => 2,
        _ => 1,
    }
}

fn research_policy_scope(value: &str) -> i32 {
    match value {
        "disabled" => 0,
        "manual" => 1,
        _ => 2,
    }
}

fn promotion_policy_scope(value: &str) -> i32 {
    match value {
        "disabled" => 0,
        "auto_validate" => 2,
        _ => 1,
    }
}

/// Check that `project` only narrows `global` (archived
/// `merge_project_settings_patch`). The error names the offending key in
/// the archived wording.
pub fn validate_project_narrowing(
    global: &Map<String, Value>,
    project: &Map<String, Value>,
) -> Result<(), String> {
    let reject = |field: &str| {
        Err(format!(
            "project settings may not broaden global policy: {field}"
        ))
    };

    // Booleans the project may only switch off: (key, global default).
    let switches: [(&str, bool); 5] = [
        ("allowExternalFilesystemAccess", false),
        ("memory.enabled", false),
        ("memory.automaticRecall", true),
        ("memory.automaticCapture", true),
        ("memory.dreaming.enabled", false),
    ];
    for (key, default) in switches {
        if get_bool(project, key) == Some(true) && !get_bool(global, key).unwrap_or(default) {
            return reject(key);
        }
    }
    // Upper bounds: the project value may not exceed the global one.
    for key in [
        "memory.contextTokenBudget",
        "memory.candidateLimit",
        "memory.prehandler.maxModelInputTokens",
        "memory.capture.maxEvidenceChars",
        "memory.dreaming.timeoutMs",
        "memory.dreaming.maxModelInputTokens",
        "memory.dreaming.maxCandidates",
    ] {
        if let (Some(p), Some(g)) = (get_num(project, key), get_num(global, key))
            && p > g
        {
            return reject(key);
        }
    }
    // Lower bounds: the project value may not be below the global one.
    for key in [
        "memory.dreaming.intervalSeconds",
        "memory.dreaming.idleSeconds",
        "memory.dreaming.candidateThreshold",
    ] {
        if let (Some(p), Some(g)) = (get_num(project, key), get_num(global, key))
            && p < g
        {
            return reject(key);
        }
    }
    if let (Some(p), Some(g)) = (
        get_str(project, "memory.dreaming.trigger"),
        get_str(global, "memory.dreaming.trigger"),
    ) && p != g
    {
        return reject("memory.dreaming.trigger");
    }
    for key in ["memory.dreaming.model", "memory.dreaming.verifierModel"] {
        if let Some(p) = get_str(project, key)
            && get_str(global, key) != Some(p)
        {
            return reject(key);
        }
    }
    type Scope = fn(&str) -> i32;
    let scoped: [(&str, Scope); 4] = [
        ("memory.capture.postProcess", capture_scope),
        ("memory.recallPolicy", recall_policy_scope),
        ("memory.dreaming.researchPolicy", research_policy_scope),
        ("memory.dreaming.promotionPolicy", promotion_policy_scope),
    ];
    for (key, scope) in scoped {
        if let (Some(p), Some(g)) = (get_str(project, key), get_str(global, key))
            && scope(p) > scope(g)
        {
            return reject(key);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn obj(v: Value) -> Map<String, Value> {
        v.as_object().unwrap().clone()
    }

    #[test]
    fn project_may_narrow_but_not_broaden() {
        let global = obj(json!({
            "memory": {"enabled": true, "contextTokenBudget": 2000, "recallPolicy": "balanced",
                       "dreaming": {"intervalSeconds": 600, "model": "a/b"}}
        }));
        let ok = obj(json!({
            "allowExternalFilesystemAccess": false,
            "memory": {"enabled": false, "contextTokenBudget": 1000, "recallPolicy": "strict",
                       "dreaming": {"intervalSeconds": 900, "model": "a/b"}}
        }));
        assert!(validate_project_narrowing(&global, &ok).is_ok());
        let cases = [
            (
                json!({"allowExternalFilesystemAccess": true}),
                "allowExternalFilesystemAccess",
            ),
            (
                json!({"memory": {"contextTokenBudget": 5000}}),
                "memory.contextTokenBudget",
            ),
            (
                json!({"memory": {"recallPolicy": "exploratory"}}),
                "memory.recallPolicy",
            ),
            (
                json!({"memory": {"dreaming": {"intervalSeconds": 60}}}),
                "memory.dreaming.intervalSeconds",
            ),
            (
                json!({"memory": {"dreaming": {"model": "x/y"}}}),
                "memory.dreaming.model",
            ),
            (
                json!({"memory": {"dreaming": {"enabled": true}}}),
                "memory.dreaming.enabled",
            ),
        ];
        for (project, key) in cases {
            assert_eq!(
                validate_project_narrowing(&global, &obj(project)).unwrap_err(),
                format!("project settings may not broaden global policy: {key}")
            );
        }
        // automaticRecall defaults to on globally, so a project may set it.
        assert!(
            validate_project_narrowing(
                &Map::new(),
                &obj(json!({"memory": {"automaticRecall": true}}))
            )
            .is_ok()
        );
    }

    #[test]
    fn layers_merge_and_reject_broadening_projects() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path().join("home"));
        std::fs::create_dir_all(home.root()).unwrap();
        std::fs::write(
            home.settings_path(),
            r#"{"defaultProvider":"openai","images":{"autoResize":true},"retry":{"enabled":true}}"#,
        )
        .unwrap();
        let project = dir.path().join("proj");
        std::fs::create_dir_all(project.join(".pi")).unwrap();
        std::fs::create_dir_all(project.join(".git")).unwrap();
        std::fs::write(
            project.join(".pi/settings.yaml"),
            "defaultModel: gpt-5\nimages:\n  autoResize: false\n",
        )
        .unwrap();
        let layered = LayeredSettings::load(&home, &project);
        assert!(layered.diagnostics.is_empty(), "{:?}", layered.diagnostics);
        let eff = layered.effective();
        assert_eq!(eff.default_provider().as_deref(), Some("openai"));
        assert_eq!(eff.default_model().as_deref(), Some("gpt-5"));
        assert!(!eff.image_auto_resize());
        assert_eq!(eff.raw()["retry"]["enabled"], true);
        assert!(layered.global.default_model().is_none());

        std::fs::remove_file(project.join(".pi/settings.yaml")).unwrap();
        std::fs::write(
            project.join(".pi/settings.json"),
            r#"{"allowExternalFilesystemAccess":true,"defaultModel":"x"}"#,
        )
        .unwrap();
        let layered = LayeredSettings::load(&home, &project);
        assert_eq!(layered.diagnostics.len(), 1);
        assert!(layered.diagnostics[0].ends_with(
            "project settings may not broaden global policy: allowExternalFilesystemAccess"
        ));
        assert!(layered.effective().default_model().is_none());
    }
}
