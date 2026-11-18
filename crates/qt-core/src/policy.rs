//! Workspace path policy for model-facing tools.
//!
//! Mirrors the archived qt policy (`archived/src/tools/tool_validation.cpp`,
//! `archived/src/app/config/settings.cpp`): every filesystem path argument and
//! the bash working directory are resolved (with symlinks followed) and must
//! stay under the canonical workspace root unless external filesystem access
//! is enabled in the global settings. Project settings may only narrow the
//! global policy.

use std::collections::HashSet;
use std::fmt;
use std::path::{Component, Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use serde_json::{Map, Value};

use crate::tools::MutationPolicy;

/// Settings key naming the workspace root.
pub const WORKSPACE_ROOT_KEY: &str = "workspaceRoot";
/// Settings key that lets tools use paths outside the workspace (global only).
pub const ALLOW_EXTERNAL_KEY: &str = "allowExternalFilesystemAccess";

/// Where model-facing tools may touch the filesystem.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorkspacePolicy {
    root: PathBuf,
    allow_external: bool,
}

/// A path argument after resolution against the working directory.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResolvedPath {
    /// Absolute path as the model named it (lexically normalised, symlinks kept).
    pub path: PathBuf,
    /// Fully resolved path (symlinks followed for every existing component).
    pub canonical: PathBuf,
}

/// Why a path was refused.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PolicyError {
    /// The path argument was empty.
    Empty,
    /// The resolved path lies outside the workspace root.
    Escapes { path: PathBuf },
}

impl fmt::Display for PolicyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PolicyError::Empty => f.write_str("path must not be empty"),
            PolicyError::Escapes { path } => {
                write!(f, "path escapes workspace_root: {}", path.display())
            }
        }
    }
}

impl std::error::Error for PolicyError {}

impl WorkspacePolicy {
    /// Confine tools to `root` (the archived secure default).
    pub fn confined(root: impl AsRef<Path>) -> Self {
        Self {
            root: canonicalize_lossy(root.as_ref()),
            allow_external: false,
        }
    }

    /// Allow or forbid paths outside the workspace root.
    pub fn with_external_access(mut self, allow: bool) -> Self {
        self.allow_external = allow;
        self
    }

    /// Build the policy from the global settings (`~/.qt/settings.json`) and
    /// the optional project settings (`<workspace>/.pi/settings.json`).
    ///
    /// Without `workspaceRoot` the root is the enclosing git repository of
    /// `cwd`, or `cwd` itself. `allowExternalFilesystemAccess` is honoured only
    /// from the global settings; a project may set it to `false` (narrowing)
    /// or point `workspaceRoot` at a directory inside the global root.
    pub fn from_settings(
        cwd: &Path,
        global: &Map<String, Value>,
        project: Option<&Map<String, Value>>,
    ) -> Result<Self, String> {
        let global_root = setting_path(global, cwd)?;
        let global_allow = setting_bool(global)?;
        let mut policy = WorkspacePolicy {
            root: global_root.unwrap_or_else(|| discover_workspace_root(cwd)),
            allow_external: global_allow.unwrap_or(false),
        };
        if let Some(project) = project {
            if setting_bool(project)?.is_some_and(|allow| allow) && !policy.allow_external {
                return Err(format!(
                    "project settings may not broaden global policy: {ALLOW_EXTERNAL_KEY}"
                ));
            }
            if setting_bool(project)? == Some(false) {
                policy.allow_external = false;
            }
            if let Some(root) = setting_path(project, &policy.root)? {
                if !policy.allow_external && !is_within(&root, &policy.root) {
                    return Err(format!(
                        "project settings may not broaden global policy: {WORKSPACE_ROOT_KEY}"
                    ));
                }
                policy.root = root;
            }
        }
        Ok(policy)
    }

    /// [`Self::from_settings`] reading the project file from disk.
    pub fn load(cwd: &Path, global: &Map<String, Value>) -> Result<Self, String> {
        let base = Self::from_settings(cwd, global, None)?;
        let project_file = base.root.join(".pi").join("settings.json");
        let project = match std::fs::read_to_string(&project_file) {
            Ok(text) => match serde_json::from_str::<Value>(&text) {
                Ok(Value::Object(map)) => Some(map),
                Ok(_) => {
                    return Err(format!(
                        "invalid settings in {}: expected a JSON object",
                        project_file.display()
                    ));
                }
                Err(e) => {
                    return Err(format!(
                        "invalid settings in {}: {e}",
                        project_file.display()
                    ));
                }
            },
            Err(_) => None,
        };
        match project {
            Some(project) => Self::from_settings(cwd, global, Some(&project))
                .map_err(|e| format!("invalid settings in {}: {e}", project_file.display())),
            None => Ok(base),
        }
    }

    /// Canonical workspace root.
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Whether tools may use paths outside the workspace root.
    pub fn allows_external_access(&self) -> bool {
        self.allow_external
    }

    /// Whether the canonical `path` is permitted.
    pub fn permits(&self, canonical: &Path) -> bool {
        self.allow_external || is_within(canonical, &self.root)
    }

    /// Resolve a model-supplied path argument against `cwd` and check it.
    ///
    /// A leading `@` (mention syntax) and `~` are expanded first.
    pub fn resolve(&self, cwd: &Path, raw: &str) -> Result<ResolvedPath, PolicyError> {
        let resolved = resolve_argument(cwd, raw).ok_or(PolicyError::Empty)?;
        if self.permits(&resolved.canonical) {
            Ok(resolved)
        } else {
            Err(PolicyError::Escapes {
                path: resolved.path,
            })
        }
    }

    /// Like [`Self::resolve`] but also admits files registered with
    /// [`allow_read_artifact`] (for example truncated bash output).
    pub fn resolve_for_read(&self, cwd: &Path, raw: &str) -> Result<ResolvedPath, PolicyError> {
        let resolved = resolve_argument(cwd, raw).ok_or(PolicyError::Empty)?;
        if self.permits(&resolved.canonical) || is_read_artifact(&resolved.canonical) {
            Ok(resolved)
        } else {
            Err(PolicyError::Escapes {
                path: resolved.path,
            })
        }
    }

    /// Check an already absolute path (for example a directory walk entry).
    pub fn check(&self, path: &Path) -> Result<PathBuf, PolicyError> {
        let canonical = weakly_canonical(path);
        if self.permits(&canonical) {
            Ok(canonical)
        } else {
            Err(PolicyError::Escapes {
                path: path.to_path_buf(),
            })
        }
    }

    /// The archived `<tool_permissions>` sentence for the system prompt.
    pub fn tool_permissions_text(&self, mutation: MutationPolicy) -> String {
        let mutation = match mutation {
            MutationPolicy::Execute => "tool mutations execute immediately",
            MutationPolicy::Queue => "tool mutations require queued approval",
        };
        let paths = if self.allow_external {
            "allowed outside the workspace"
        } else {
            "confined to the workspace"
        };
        format!("{mutation}; filesystem path arguments are {paths}")
    }

    /// Environment-context lines describing the policy, in the archived
    /// system prompt format (`<workspace_root>` and `<tool_permissions>`).
    pub fn prompt_text(&self, mutation: MutationPolicy) -> String {
        format!(
            "  <workspace_root>{}</workspace_root>\n  <tool_permissions>{}</tool_permissions>",
            escape_xml(&self.root.display().to_string()),
            escape_xml(&self.tool_permissions_text(mutation))
        )
    }
}

/// Resolve `raw` against `cwd` without applying any policy.
pub fn resolve_argument(cwd: &Path, raw: &str) -> Option<ResolvedPath> {
    let trimmed = raw.trim();
    let trimmed = trimmed.strip_prefix('@').unwrap_or(trimmed);
    if trimmed.is_empty() {
        return None;
    }
    let expanded = expand_home(trimmed);
    let joined = if expanded.is_absolute() {
        expanded
    } else {
        cwd.join(expanded)
    };
    let path = normalize_lexically(&joined);
    let canonical = weakly_canonical(&path);
    Some(ResolvedPath { path, canonical })
}

fn expand_home(path: &str) -> PathBuf {
    if path == "~" {
        return dirs::home_dir().unwrap_or_else(|| PathBuf::from(path));
    }
    if let Some(rest) = path.strip_prefix("~/")
        && let Some(home) = dirs::home_dir()
    {
        return home.join(rest);
    }
    PathBuf::from(path)
}

/// Remove `.` and resolve `..` components without touching the filesystem.
pub fn normalize_lexically(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                if !out.pop() {
                    out.push(component);
                }
            }
            other => out.push(other),
        }
    }
    out
}

/// Resolve symlinks for every existing component of `path` and normalise the
/// non-existing remainder lexically (like `std::filesystem::weakly_canonical`).
pub fn weakly_canonical(path: &Path) -> PathBuf {
    if let Ok(canonical) = path.canonicalize() {
        return canonical;
    }
    let mut resolved = PathBuf::new();
    let mut exists = true;
    for component in path.components() {
        match component {
            Component::Prefix(_) | Component::RootDir => resolved.push(component),
            Component::CurDir => {}
            Component::ParentDir => {
                resolved.pop();
            }
            Component::Normal(name) => {
                resolved.push(name);
                if exists {
                    match resolved.canonicalize() {
                        Ok(canonical) => resolved = canonical,
                        Err(_) => exists = false,
                    }
                }
            }
        }
    }
    resolved
}

fn canonicalize_lossy(path: &Path) -> PathBuf {
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .map(|cwd| cwd.join(path))
            .unwrap_or_else(|_| path.to_path_buf())
    };
    weakly_canonical(&normalize_lexically(&absolute))
}

/// Component-wise prefix test (`/a/bc` is not inside `/a/b`).
pub fn is_within(path: &Path, root: &Path) -> bool {
    path.starts_with(root)
}

/// The enclosing git repository of `cwd`, or `cwd` itself.
pub fn discover_workspace_root(cwd: &Path) -> PathBuf {
    let start = canonicalize_lossy(cwd);
    let mut current = Some(start.as_path());
    while let Some(dir) = current {
        if dir.join(".git").exists() {
            return dir.to_path_buf();
        }
        current = dir.parent();
    }
    start
}

fn setting_path(map: &Map<String, Value>, base: &Path) -> Result<Option<PathBuf>, String> {
    match map.get(WORKSPACE_ROOT_KEY) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(text)) if text.trim().is_empty() => {
            Err(format!("{WORKSPACE_ROOT_KEY} must not be empty"))
        }
        Some(Value::String(text)) => {
            let expanded = expand_home(text.trim());
            let joined = if expanded.is_absolute() {
                expanded
            } else {
                base.join(expanded)
            };
            joined.canonicalize().map(Some).map_err(|_| {
                format!(
                    "{WORKSPACE_ROOT_KEY} could not be canonicalized: {}",
                    joined.display()
                )
            })
        }
        Some(_) => Err(format!("{WORKSPACE_ROOT_KEY} must be a string")),
    }
}

fn setting_bool(map: &Map<String, Value>) -> Result<Option<bool>, String> {
    match map.get(ALLOW_EXTERNAL_KEY) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::Bool(b)) => Ok(Some(*b)),
        Some(_) => Err(format!("{ALLOW_EXTERNAL_KEY} must be a boolean")),
    }
}

fn escape_xml(text: &str) -> String {
    text.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn read_artifacts() -> &'static Mutex<HashSet<PathBuf>> {
    static ARTIFACTS: OnceLock<Mutex<HashSet<PathBuf>>> = OnceLock::new();
    ARTIFACTS.get_or_init(Default::default)
}

/// Let the read tool open `path` even when it lies outside the workspace.
///
/// Used for files qt itself wrote for the model, such as the full output of
/// a truncated bash command in the temp directory.
pub fn allow_read_artifact(path: &Path) {
    let canonical = weakly_canonical(path);
    if let Ok(mut set) = read_artifacts().lock() {
        set.insert(canonical);
    }
}

fn is_read_artifact(canonical: &Path) -> bool {
    read_artifacts()
        .lock()
        .map(|set| set.contains(canonical))
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn map(value: Value) -> Map<String, Value> {
        match value {
            Value::Object(m) => m,
            _ => unreachable!(),
        }
    }

    #[test]
    fn confines_relative_absolute_and_dotdot_paths() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir(dir.path().join("sub")).unwrap();
        let policy = WorkspacePolicy::confined(dir.path());
        let cwd = dir.path().join("sub");
        let ok = policy.resolve(&cwd, "new/file.txt").unwrap();
        assert_eq!(ok.path, cwd.join("new/file.txt"));
        assert!(ok.canonical.starts_with(policy.root()));
        assert!(policy.resolve(&cwd, "../x").is_ok());
        let err = policy.resolve(&cwd, "../../outside").unwrap_err();
        assert!(err.to_string().starts_with("path escapes workspace_root: "));
        assert!(policy.resolve(&cwd, "/etc/passwd").is_err());
        assert_eq!(policy.resolve(&cwd, "  "), Err(PolicyError::Empty));
        assert!(policy.resolve(&cwd, "@sub").is_ok());
        let open = policy.clone().with_external_access(true);
        assert!(open.resolve(&cwd, "/etc/passwd").is_ok());
    }

    #[cfg(unix)]
    #[test]
    fn follows_symlinks_out_of_the_workspace() {
        let outside = tempfile::tempdir().unwrap();
        let dir = tempfile::tempdir().unwrap();
        std::os::unix::fs::symlink(outside.path(), dir.path().join("link")).unwrap();
        let policy = WorkspacePolicy::confined(dir.path());
        let err = policy.resolve(dir.path(), "link/secret.txt").unwrap_err();
        assert!(matches!(err, PolicyError::Escapes { .. }));
        // A prefix that only shares characters is not inside the root.
        let sibling = PathBuf::from(format!("{}x", policy.root().display()));
        assert!(!policy.permits(&sibling));
    }

    #[test]
    fn weakly_canonical_handles_missing_tails() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().canonicalize().unwrap();
        assert_eq!(
            weakly_canonical(&dir.path().join("a/../b/c")),
            root.join("b/c")
        );
    }

    #[test]
    fn settings_layering_and_project_narrowing() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join("proj/inner")).unwrap();
        let cwd = dir.path().join("proj");
        let root = cwd.canonicalize().unwrap();

        let policy = WorkspacePolicy::from_settings(&cwd, &Map::new(), None).unwrap();
        assert!(!policy.allows_external_access());

        let global = map(json!({ "workspaceRoot": "inner" }));
        let policy = WorkspacePolicy::from_settings(&cwd, &global, None).unwrap();
        assert_eq!(policy.root(), root.join("inner"));

        let project = map(json!({ "allowExternalFilesystemAccess": true }));
        let err = WorkspacePolicy::from_settings(&cwd, &Map::new(), Some(&project)).unwrap_err();
        assert_eq!(
            err,
            "project settings may not broaden global policy: allowExternalFilesystemAccess"
        );

        let global = map(json!({ "allowExternalFilesystemAccess": true }));
        let narrow = map(json!({ "allowExternalFilesystemAccess": false }));
        let policy = WorkspacePolicy::from_settings(&cwd, &global, Some(&narrow)).unwrap();
        assert!(!policy.allows_external_access());

        let global = map(json!({ "workspaceRoot": "." }));
        let project = map(json!({ "workspaceRoot": ".." }));
        let err = WorkspacePolicy::from_settings(&cwd, &global, Some(&project)).unwrap_err();
        assert!(err.contains("workspaceRoot"));
        let project = map(json!({ "workspaceRoot": "inner" }));
        let policy = WorkspacePolicy::from_settings(&cwd, &global, Some(&project)).unwrap();
        assert_eq!(policy.root(), root.join("inner"));

        let bad = map(json!({ "allowExternalFilesystemAccess": "yes" }));
        assert!(WorkspacePolicy::from_settings(&cwd, &bad, None).is_err());
        let bad = map(json!({ "workspaceRoot": 3 }));
        assert!(WorkspacePolicy::from_settings(&cwd, &bad, None).is_err());
    }

    #[test]
    fn load_reads_project_file_and_discovers_git_root() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(dir.path().join(".git")).unwrap();
        std::fs::create_dir_all(dir.path().join("src")).unwrap();
        std::fs::create_dir_all(dir.path().join(".pi")).unwrap();
        let cwd = dir.path().join("src");
        let policy = WorkspacePolicy::load(&cwd, &Map::new()).unwrap();
        assert_eq!(policy.root(), dir.path().canonicalize().unwrap());
        std::fs::write(
            dir.path().join(".pi/settings.json"),
            r#"{"allowExternalFilesystemAccess": true}"#,
        )
        .unwrap();
        let err = WorkspacePolicy::load(&cwd, &Map::new()).unwrap_err();
        assert!(err.starts_with("invalid settings in "));
    }

    #[test]
    fn prompt_text_matches_archived_wording() {
        let policy = WorkspacePolicy::confined("/");
        assert_eq!(
            policy.tool_permissions_text(MutationPolicy::Execute),
            "tool mutations execute immediately; filesystem path arguments are confined to the workspace"
        );
        let open = policy.with_external_access(true);
        assert_eq!(
            open.tool_permissions_text(MutationPolicy::Queue),
            "tool mutations require queued approval; filesystem path arguments are allowed outside the workspace"
        );
        assert!(
            open.prompt_text(MutationPolicy::Execute)
                .contains("<tool_permissions>tool mutations execute immediately;")
        );
    }

    #[test]
    fn read_artifacts_bypass_confinement_for_reads_only() {
        let outside = tempfile::tempdir().unwrap();
        let file = outside.path().join("out.log");
        std::fs::write(&file, "x").unwrap();
        let dir = tempfile::tempdir().unwrap();
        let policy = WorkspacePolicy::confined(dir.path());
        let raw = file.to_str().unwrap();
        assert!(policy.resolve_for_read(dir.path(), raw).is_err());
        allow_read_artifact(&file);
        assert!(policy.resolve_for_read(dir.path(), raw).is_ok());
        assert!(policy.resolve(dir.path(), raw).is_err());
    }
}
