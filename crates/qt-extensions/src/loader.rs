//! Extension discovery and loading (archived `extensions/loader.cpp` and
//! `app/runtime/extensions.cpp`).
//!
//! Roots are files or directories. A directory with `extension.json` is one
//! extension; otherwise a `package.json` `pi.extensions` list, then
//! `index.rhai` / `index.ts` / `index.js`, then every `*.rhai` / `*.ts` /
//! `*.js` file of the directory, then each subdirectory recursively. Hidden
//! entries and `node_modules` are skipped.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use serde_json::{Value, json};

use crate::filters::{apply_filter_patterns, display_path, is_override_pattern, lexical_normal};
use crate::manifest::{
    CommandContribution, ExtensionManifest, MANIFEST_FILE_NAME, ToolContribution, Transport,
    decode_manifest_str, join_issues, validate_manifest,
};
use crate::packages::{PackageScope, load_package_settings};
use crate::reference::{find_not_implemented_api_uses, scan_module_registrations};
use crate::script::{SandboxPolicy, ScriptRuntime};

/// Where an extension was found.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ExtensionOrigin {
    /// `-e <path>` on the command line.
    Cli,
    /// `<workspace>/.pi/extensions`.
    Project,
    /// `<agent_home>/extensions`.
    User,
    /// A `packages` settings entry.
    Package { source: String, scope: PackageScope },
}

/// One loaded extension.
#[derive(Debug, Clone)]
pub struct LoadedExtension {
    /// The root it was discovered under.
    pub root: String,
    /// Directory holding the manifest or entry file.
    pub directory: PathBuf,
    /// `extension.json` or the entry file.
    pub path: PathBuf,
    pub manifest: ExtensionManifest,
    /// The Rhai runtime for script extensions.
    pub script: Option<Arc<ScriptRuntime>>,
    pub origin: ExtensionOrigin,
}

/// Extensions and diagnostics of a load.
#[derive(Debug, Clone, Default)]
pub struct LoadResult {
    pub roots: Vec<String>,
    pub extensions: Vec<LoadedExtension>,
    pub diagnostics: Vec<String>,
}

impl LoadResult {
    fn diag(&mut self, message: impl Into<String>) {
        let m = message.into();
        if !m.is_empty() {
            self.diagnostics.push(m);
        }
    }
}

fn is_reference_file(path: &Path) -> bool {
    matches!(
        path.extension().and_then(|e| e.to_str()),
        Some("ts") | Some("js")
    )
}

fn is_rhai_file(path: &Path) -> bool {
    path.extension().and_then(|e| e.to_str()) == Some("rhai")
}

fn is_script_file(path: &Path) -> bool {
    is_reference_file(path) || is_rhai_file(path)
}

const INDEX_FILES: &[&str] = &["index.rhai", "index.ts", "index.js"];

/// `~`, `~/x` and `~x` expanded against the home directory.
pub fn expand_home(raw: &str) -> PathBuf {
    if let Some(home) = dirs::home_dir() {
        if raw == "~" {
            return home;
        }
        if let Some(rest) = raw.strip_prefix("~/") {
            return home.join(rest);
        }
        if let Some(rest) = raw.strip_prefix('~') {
            return home.join(rest);
        }
    }
    PathBuf::from(raw)
}

fn list_entries(dir: &Path) -> Vec<PathBuf> {
    let mut entries: Vec<PathBuf> = std::fs::read_dir(dir)
        .map(|rd| {
            rd.filter_map(Result::ok)
                .map(|e| e.path())
                .filter(|p| {
                    let name = p
                        .file_name()
                        .map(|n| n.to_string_lossy())
                        .unwrap_or_default();
                    !name.starts_with('.') && name != "node_modules"
                })
                .collect()
        })
        .unwrap_or_default();
    entries.sort();
    entries
}

fn subdirectories(dir: &Path) -> Vec<PathBuf> {
    list_entries(dir)
        .into_iter()
        .filter(|p| p.is_dir())
        .collect()
}

fn collect_script_entries(dir: &Path, out: &mut Vec<PathBuf>) {
    for index in INDEX_FILES {
        let p = dir.join(index);
        if p.is_file() {
            out.push(p);
            return;
        }
    }
    for entry in list_entries(dir) {
        if entry.is_file() && is_script_file(&entry) {
            out.push(entry);
        }
    }
    for child in subdirectories(dir) {
        collect_script_entries(&child, out);
    }
}

/// Lowercased; characters outside `[a-z0-9._-]` become `-`; leading
/// non-alphanumerics dropped; `extension` when nothing is left.
pub fn sanitize_identifier(text: &str) -> String {
    let mapped: String = text
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() {
                c.to_ascii_lowercase()
            } else if matches!(c, '.' | '_' | '-') {
                c
            } else {
                '-'
            }
        })
        .collect();
    let trimmed =
        mapped.trim_start_matches(|c: char| !c.is_ascii_lowercase() && !c.is_ascii_digit());
    if trimmed.is_empty() {
        "extension".into()
    } else {
        trimmed.into()
    }
}

/// Extension name for an entry file: its directory for `index.*`, else the stem.
pub fn extension_name(entry: &Path) -> String {
    let file = entry
        .file_name()
        .map(|n| n.to_string_lossy())
        .unwrap_or_default();
    if INDEX_FILES.contains(&file.as_ref()) {
        let parent = entry
            .parent()
            .and_then(Path::file_name)
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        return sanitize_identifier(&parent);
    }
    sanitize_identifier(
        &entry
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_default(),
    )
}

struct Loader<'a> {
    policy: SandboxPolicy,
    origin: &'a ExtensionOrigin,
    result: LoadResult,
}

fn entry_metadata(
    source: &str,
    entry: &Path,
    discovery: &str,
    package: &Value,
    declared: Option<&str>,
) -> Value {
    let mut m = json!({
        "source": source,
        "discovery": discovery,
        "entrypoint_extension": entry.extension().map(|e| format!(".{}", e.to_string_lossy())).unwrap_or_default(),
    });
    if let Some(d) = declared {
        m["declared_entry"] = json!(d);
    }
    if package.as_object().is_some_and(|o| !o.is_empty()) {
        m["package"] = package.clone();
    }
    m
}

/// Merge a script registration into its manifest.
fn apply_registration(manifest: &mut ExtensionManifest, script: &ScriptRuntime) {
    let r = script.registration();
    manifest.commands.extend(r.commands.iter().cloned());
    manifest.tools.extend(r.tools.iter().cloned());
    manifest.providers.extend(r.providers.iter().cloned());
    manifest.flags.extend(r.flags.iter().cloned());
    r.metadata_into(&mut manifest.metadata);
}

impl Loader<'_> {
    fn push(
        &mut self,
        root: &str,
        directory: PathBuf,
        path: PathBuf,
        manifest: ExtensionManifest,
        script: Option<Arc<ScriptRuntime>>,
    ) {
        self.result.extensions.push(LoadedExtension {
            root: root.into(),
            directory,
            path,
            manifest,
            script,
            origin: self.origin.clone(),
        });
    }

    fn load_rhai(&mut self, path: &Path, name: &str) -> Option<Arc<ScriptRuntime>> {
        let shown = display_path(path);
        let source = match std::fs::read_to_string(path) {
            Ok(s) => s,
            Err(_) => {
                self.result
                    .diag(format!("failed to read rhai extension entry: {shown}"));
                return None;
            }
        };
        match ScriptRuntime::load(name, &source, self.policy) {
            Ok(rt) => Some(Arc::new(rt)),
            Err(e) => {
                self.result.diag(format!(
                    "failed to compile rhai extension entry: {shown} ({e})"
                ));
                None
            }
        }
    }

    fn load_entry(
        &mut self,
        root: &str,
        entry: &Path,
        discovery: &str,
        package: &Value,
        declared: Option<&str>,
    ) {
        let entry = lexical_normal(entry);
        let shown = display_path(&entry);
        let rhai = is_rhai_file(&entry);
        let kind = if rhai { "rhai" } else { "reference" };
        if !entry.is_file() {
            self.result.diag(format!(
                "{kind} extension entry is not a readable file: {shown}"
            ));
            return;
        }
        let directory = entry.parent().map(Path::to_path_buf).unwrap_or_default();
        let mut manifest = ExtensionManifest {
            name: extension_name(&entry),
            transport: if rhai {
                Transport::Rhai
            } else {
                Transport::ReferenceModule
            },
            entrypoint: Some(shown.clone()),
            working_directory: Some(display_path(&directory)),
            metadata: entry_metadata(
                if rhai { "rhai" } else { "reference_module" },
                &entry,
                discovery,
                package,
                declared,
            ),
            ..Default::default()
        };
        let mut script = None;
        if rhai {
            let Some(rt) = self.load_rhai(&entry, &manifest.name) else {
                return;
            };
            apply_registration(&mut manifest, &rt);
            script = Some(rt);
        } else {
            let Ok(source) = std::fs::read_to_string(&entry) else {
                self.result
                    .diag(format!("failed to read reference extension entry: {shown}"));
                return;
            };
            scan_module_registrations(&mut manifest, &source);
            let uses = find_not_implemented_api_uses(&source);
            if !uses.is_empty() {
                manifest.metadata["not_implemented_apis"] = uses
                    .iter()
                    .map(|d| json!({"api": d.api, "message": d.message, "row": d.row, "column": d.column}))
                    .collect();
            }
            for d in uses {
                self.result
                    .diag(format!("{}: {shown}:{}:{}", d.message, d.row, d.column));
            }
        }
        let issues = validate_manifest(&manifest);
        if !issues.is_empty() {
            self.result.diag(format!(
                "{kind} extension manifest failed validation: {shown} ({})",
                join_issues(&issues)
            ));
            return;
        }
        self.push(root, directory, entry, manifest, script);
    }

    /// `pi.extensions` from `package.json`: `Some(entries)` when declared.
    fn package_entries(&mut self, dir: &Path) -> Option<(Vec<PathBuf>, Vec<String>, Value)> {
        let package_json = dir.join("package.json");
        if !package_json.is_file() {
            return None;
        }
        let shown = display_path(&package_json);
        let Ok(text) = std::fs::read_to_string(&package_json) else {
            self.result
                .diag(format!("failed to read package manifest: {shown}"));
            return None;
        };
        let json: Value = match serde_json::from_str(&text) {
            Ok(v) => v,
            Err(e) => {
                self.result
                    .diag(format!("failed to decode package manifest: {shown} ({e})"));
                return None;
            }
        };
        let declared = json.get("pi")?.get("extensions")?.as_array()?;
        let declared: Vec<String> = match declared
            .iter()
            .map(|v| v.as_str().map(str::to_string))
            .collect::<Option<Vec<_>>>()
        {
            Some(d) => d,
            None => {
                self.result.diag(format!(
                    "failed to decode package manifest: {shown} (pi.extensions entries must be strings)"
                ));
                return None;
            }
        };
        if declared.is_empty() {
            return None;
        }
        let mut meta = json!({ "manifest": shown });
        for key in ["name", "version", "type"] {
            if let Some(v) = json.get(key).and_then(Value::as_str) {
                meta[key] = json!(v);
            }
        }
        if let Some(deps) = json.get("dependencies").and_then(Value::as_object)
            && !deps.is_empty()
        {
            meta["dependencies"] = deps.keys().cloned().collect();
        }
        let mut entries = Vec::new();
        for d in declared.iter().filter(|d| !is_override_pattern(d)) {
            let p = if Path::new(d).is_absolute() {
                PathBuf::from(d)
            } else {
                dir.join(d)
            };
            if p.is_dir() {
                collect_script_entries(&p, &mut entries);
            } else if p.is_file() && is_script_file(&p) {
                entries.push(p);
            } else {
                self.result.diag(format!(
                    "package extension entry does not exist: {}",
                    display_path(&p)
                ));
            }
        }
        let patterns: Vec<String> = declared
            .iter()
            .filter(|d| is_override_pattern(d))
            .cloned()
            .collect();
        Some((
            apply_filter_patterns(entries, &patterns, dir),
            declared,
            meta,
        ))
    }

    fn load_manifest_dir(&mut self, root: &str, dir: &Path, manifest_file: &Path) {
        let shown = display_path(manifest_file);
        if !manifest_file.is_file() {
            self.result.diag(format!(
                "extension manifest is not a readable file: {shown}"
            ));
            return;
        }
        let Ok(text) = std::fs::read_to_string(manifest_file) else {
            self.result
                .diag(format!("failed to read extension manifest: {shown}"));
            return;
        };
        let mut manifest = match decode_manifest_str(&text) {
            Ok(m) => m,
            Err(e) => {
                self.result.diag(format!(
                    "failed to decode extension manifest: {shown} ({e})"
                ));
                return;
            }
        };
        let issues = validate_manifest(&manifest);
        if !issues.is_empty() {
            self.result.diag(format!(
                "extension manifest failed validation: {shown} ({})",
                join_issues(&issues)
            ));
            return;
        }
        let mut script = None;
        if manifest.transport == Transport::Rhai {
            let entry = dir.join(manifest.entrypoint.as_deref().unwrap_or_default());
            let Some(rt) = self.load_rhai(&entry, &manifest.name) else {
                return;
            };
            apply_registration(&mut manifest, &rt);
            let issues = validate_manifest(&manifest);
            if !issues.is_empty() {
                self.result.diag(format!(
                    "extension manifest failed validation: {shown} ({})",
                    join_issues(&issues)
                ));
                return;
            }
            script = Some(rt);
        }
        self.push(
            root,
            dir.to_path_buf(),
            manifest_file.to_path_buf(),
            manifest,
            script,
        );
    }

    fn load_dir(&mut self, root: &str, dir: &Path) {
        let manifest_file = dir.join(MANIFEST_FILE_NAME);
        if manifest_file.exists() {
            self.load_manifest_dir(root, dir, &manifest_file);
            return;
        }
        if let Some((entries, declared, meta)) = self.package_entries(dir) {
            for entry in entries {
                let normal = lexical_normal(&entry);
                let declared_path = declared
                    .iter()
                    .filter(|d| !is_override_pattern(d))
                    .find(|d| {
                        let c = if Path::new(d).is_absolute() {
                            PathBuf::from(d)
                        } else {
                            dir.join(d)
                        };
                        lexical_normal(&c) == normal
                    })
                    .cloned();
                self.load_entry(
                    root,
                    &entry,
                    "package_manifest",
                    &meta,
                    declared_path.as_deref(),
                );
            }
            return;
        }
        for index in INDEX_FILES {
            let p = dir.join(index);
            if p.is_file() {
                self.load_entry(root, &p, "index", &json!({}), None);
                return;
            }
        }
        for entry in list_entries(dir) {
            if entry.is_file() && is_script_file(&entry) {
                self.load_entry(root, &entry, "direct_file", &json!({}), None);
            }
        }
        for child in subdirectories(dir) {
            self.load_dir(root, &child);
        }
    }
}

/// Load every extension under `roots`; `filters` (package `extensions`
/// patterns) select which loaded extensions are kept.
pub fn load_extension_roots(
    roots: &[String],
    filters: &[String],
    origin: &ExtensionOrigin,
    policy: SandboxPolicy,
) -> LoadResult {
    let mut loader = Loader {
        policy,
        origin,
        result: LoadResult::default(),
    };
    for root in roots {
        if root.is_empty() {
            loader.result.roots.push(String::new());
            loader.result.diag("extension root was empty");
            continue;
        }
        let path = expand_home(root);
        let shown = display_path(&path);
        loader.result.roots.push(shown.clone());
        if path.is_file() && is_script_file(&path) {
            let parent = path.parent().map(display_path).unwrap_or_default();
            loader.load_entry(&parent, &path, "explicit_file", &json!({}), None);
            continue;
        }
        if !path.exists() {
            loader
                .result
                .diag(format!("extension root does not exist: {shown}"));
            continue;
        }
        if !path.is_dir() {
            loader
                .result
                .diag(format!("extension root is not a directory: {shown}"));
            continue;
        }
        loader.load_dir(&shown, &path);
    }
    if !filters.is_empty() {
        loader.result.extensions.retain(|e| {
            !apply_filter_patterns(vec![e.path.clone()], filters, Path::new(&e.root)).is_empty()
        });
    }
    loader.result
}

/// Inputs of [`discover_extensions`].
#[derive(Debug, Clone, Default)]
pub struct DiscoveryOptions {
    pub cwd: PathBuf,
    /// Project root holding `.pi/` (the workspace root; defaults to `cwd`).
    pub workspace_root: Option<PathBuf>,
    pub agent_home: PathBuf,
    /// `-e` paths, loaded even when discovery is off.
    pub cli_paths: Vec<String>,
    /// `false` for `--no-extensions`.
    pub include_discovered: bool,
    pub policy: SandboxPolicy,
}

fn identity(path: &Path) -> PathBuf {
    std::fs::canonicalize(path).unwrap_or_else(|_| lexical_normal(path))
}

fn merge(target: &mut LoadResult, loaded: LoadResult) {
    let mut roots: Vec<PathBuf> = target
        .roots
        .iter()
        .map(|r| identity(Path::new(r)))
        .collect();
    for root in loaded.roots {
        let id = identity(Path::new(&root));
        if !roots.contains(&id) {
            roots.push(id);
            target.roots.push(root);
        }
    }
    let mut ids: Vec<PathBuf> = target
        .extensions
        .iter()
        .map(|e| identity(&e.path))
        .collect();
    for ext in loaded.extensions {
        let id = identity(&ext.path);
        if !ids.contains(&id) {
            ids.push(id);
            target.extensions.push(ext);
        }
    }
    target.diagnostics.extend(loaded.diagnostics);
}

/// The archived discovery order: `-e` paths, `<workspace>/.pi/extensions`,
/// `<agent_home>/extensions`, then project and user settings packages (with
/// their `extensions` filters). The same file is loaded once (canonical path).
pub fn discover_extensions(options: &DiscoveryOptions) -> LoadResult {
    let mut aggregate = LoadResult::default();
    if !options.cli_paths.is_empty() {
        merge(
            &mut aggregate,
            load_extension_roots(
                &options.cli_paths,
                &[],
                &ExtensionOrigin::Cli,
                options.policy,
            ),
        );
    }
    if !options.include_discovered {
        return aggregate;
    }
    let project = options
        .workspace_root
        .clone()
        .unwrap_or_else(|| options.cwd.clone());
    let mut defaults = vec![(
        project.join(".pi").join("extensions"),
        ExtensionOrigin::Project,
    )];
    if !options.agent_home.as_os_str().is_empty() {
        defaults.push((options.agent_home.join("extensions"), ExtensionOrigin::User));
    }
    for (root, origin) in defaults {
        if root.is_dir() {
            merge(
                &mut aggregate,
                load_extension_roots(&[display_path(&root)], &[], &origin, options.policy),
            );
        }
    }
    if options.agent_home.as_os_str().is_empty() {
        return aggregate;
    }
    let settings = match load_package_settings(&project, &options.agent_home) {
        Ok(s) => s,
        Err(e) => {
            aggregate.diag(format!("failed to load package settings: {e}"));
            return aggregate;
        }
    };
    for package in settings.project.iter().chain(settings.user.iter()) {
        let Some(installed) = &package.installed_path else {
            continue;
        };
        if package.filtered && package.extensions.is_empty() {
            continue;
        }
        let origin = ExtensionOrigin::Package {
            source: package.source.clone(),
            scope: package.scope,
        };
        merge(
            &mut aggregate,
            load_extension_roots(
                std::slice::from_ref(installed),
                &package.extensions,
                &origin,
                options.policy,
            ),
        );
    }
    aggregate
}

/// Commands contributed by loaded extensions, in load order.
pub fn extension_commands(result: &LoadResult) -> Vec<(&LoadedExtension, &CommandContribution)> {
    result
        .extensions
        .iter()
        .flat_map(|e| e.manifest.commands.iter().map(move |c| (e, c)))
        .collect()
}

/// Tools contributed by loaded extensions, in load order.
pub fn extension_tools(result: &LoadResult) -> Vec<(&LoadedExtension, &ToolContribution)> {
    result
        .extensions
        .iter()
        .flat_map(|e| e.manifest.tools.iter().map(move |t| (e, t)))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(path: &Path, text: &str) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, text).unwrap();
    }

    const RHAI: &str = r#"fn activate() { registerCommand("hi"); }"#;

    #[test]
    fn names() {
        assert_eq!(sanitize_identifier("My Ext!"), "my-ext-");
        assert_eq!(sanitize_identifier("--x"), "x");
        assert_eq!(sanitize_identifier("@@"), "extension");
        assert_eq!(extension_name(Path::new("/a/Tools/index.rhai")), "tools");
        assert_eq!(extension_name(Path::new("/a/b/plan_mode.ts")), "plan_mode");
    }

    #[test]
    fn scan_rules() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path();
        // manifest directory
        write(
            &root.join("m/extension.json"),
            r#"{"name":"m","executable":"run.sh","commands":[{"name":"mc"}]}"#,
        );
        write(&root.join("m/ignored.rhai"), "garbage");
        // index precedence: rhai first
        write(&root.join("idx/index.rhai"), RHAI);
        write(&root.join("idx/index.ts"), "pi.registerCommand('ts')");
        // package.json with pi.extensions and filters
        write(
            &root.join("pkg/package.json"),
            r#"{"name":"pkg","version":"1.0.0","pi":{"extensions":["./ext","!ext/skip.rhai","missing.ts"]}}"#,
        );
        write(&root.join("pkg/ext/keep.rhai"), RHAI);
        write(&root.join("pkg/ext/skip.rhai"), RHAI);
        // direct files and nested directories; hidden and node_modules skipped
        write(&root.join("loose/a.js"), "pi.registerTool({ name: 'jt' })");
        write(&root.join("loose/deep/b.rhai"), RHAI);
        write(&root.join("loose/.hidden/c.rhai"), RHAI);
        write(&root.join("loose/node_modules/d.rhai"), RHAI);
        // broken script
        write(&root.join("broken/index.rhai"), "fn activate( {");

        let r = load_extension_roots(
            &[display_path(root)],
            &[],
            &ExtensionOrigin::User,
            SandboxPolicy::default(),
        );
        let names: Vec<_> = r
            .extensions
            .iter()
            .map(|e| e.manifest.name.as_str())
            .collect();
        assert_eq!(names, ["idx", "a", "b", "m", "keep"]);
        let idx = &r.extensions[0];
        assert_eq!(idx.manifest.transport, Transport::Rhai);
        assert!(idx.script.is_some());
        assert_eq!(idx.manifest.commands[0].name, "hi");
        assert_eq!(r.extensions[1].manifest.tools[0].name, "jt");
        let keep = &r.extensions[4];
        assert_eq!(
            keep.manifest.metadata["discovery"],
            json!("package_manifest")
        );
        assert_eq!(keep.manifest.metadata["declared_entry"], Value::Null);
        assert_eq!(keep.manifest.metadata["package"]["name"], json!("pkg"));
        assert!(r.diagnostics.iter().any(|d| {
            d.starts_with("failed to compile rhai extension entry: ")
                && d.contains("broken/index.rhai")
        }));
        assert!(r.diagnostics.iter().any(|d| {
            d.starts_with("package extension entry does not exist: ")
                && d.ends_with("pkg/missing.ts")
        }));
    }

    #[test]
    fn explicit_files_filters_and_errors() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("one.rhai");
        write(&file, RHAI);
        let r = load_extension_roots(
            &[
                display_path(&file),
                String::new(),
                display_path(&dir.path().join("nope")),
                display_path(&dir.path().join("x.txt")),
            ],
            &[],
            &ExtensionOrigin::Cli,
            SandboxPolicy::default(),
        );
        assert_eq!(r.extensions.len(), 1);
        assert_eq!(
            r.extensions[0].manifest.metadata["discovery"],
            json!("explicit_file")
        );
        assert_eq!(r.diagnostics[0], "extension root was empty");
        assert!(r.diagnostics[1].starts_with("extension root does not exist: "));
        write(&dir.path().join("x.txt"), "");
        let r = load_extension_roots(
            &[display_path(&dir.path().join("x.txt"))],
            &[],
            &ExtensionOrigin::Cli,
            SandboxPolicy::default(),
        );
        assert!(r.diagnostics[0].starts_with("extension root is not a directory: "));

        write(&dir.path().join("pkg/a.rhai"), RHAI);
        write(&dir.path().join("pkg/b.rhai"), RHAI);
        let r = load_extension_roots(
            &[display_path(&dir.path().join("pkg"))],
            &["-b.rhai".into()],
            &ExtensionOrigin::Cli,
            SandboxPolicy::default(),
        );
        assert_eq!(r.extensions.len(), 1);
        assert_eq!(r.extensions[0].manifest.name, "a");
    }

    #[test]
    fn rhai_manifest_transport_loads_entrypoint() {
        let dir = tempfile::tempdir().unwrap();
        write(
            &dir.path().join("x/extension.json"),
            r#"{"name":"x","transport":"rhai","entrypoint":"main.rhai","commands":[{"name":"declared"}]}"#,
        );
        write(&dir.path().join("x/main.rhai"), RHAI);
        let r = load_extension_roots(
            &[display_path(dir.path())],
            &[],
            &ExtensionOrigin::Cli,
            SandboxPolicy::default(),
        );
        let m = &r.extensions[0].manifest;
        assert_eq!(
            m.commands
                .iter()
                .map(|c| c.name.as_str())
                .collect::<Vec<_>>(),
            ["declared", "hi"]
        );
        assert!(r.extensions[0].script.is_some());
    }

    #[test]
    fn discovery_dedups_and_loads_packages() {
        let dir = tempfile::tempdir().unwrap();
        let project = dir.path().join("proj");
        let home = dir.path().join("home");
        write(&project.join(".pi/extensions/p.rhai"), RHAI);
        write(&home.join("extensions/u.rhai"), RHAI);
        write(&dir.path().join("pkgs/tool/a.rhai"), RHAI);
        write(&dir.path().join("pkgs/tool/b.rhai"), RHAI);
        write(&dir.path().join("pkgs/off/c.rhai"), RHAI);
        let tool = display_path(&dir.path().join("pkgs/tool"));
        let off = display_path(&dir.path().join("pkgs/off"));
        write(
            &home.join("settings.json"),
            &json!({"packages":[{"source": tool, "extensions":["a.rhai"]}, {"source": off, "skills": []}]}).to_string(),
        );
        let opts = DiscoveryOptions {
            cwd: project.clone(),
            workspace_root: None,
            agent_home: home.clone(),
            cli_paths: vec![display_path(&project.join(".pi/extensions/p.rhai"))],
            include_discovered: true,
            policy: SandboxPolicy::default(),
        };
        let r = discover_extensions(&opts);
        let got: Vec<_> = r
            .extensions
            .iter()
            .map(|e| (e.manifest.name.as_str(), e.origin.clone()))
            .collect();
        assert_eq!(got.len(), 3, "{got:?} {:?}", r.diagnostics);
        assert_eq!(got[0], ("p", ExtensionOrigin::Cli));
        assert_eq!(got[1], ("u", ExtensionOrigin::User));
        assert_eq!(got[2].0, "a");
        assert!(matches!(got[2].1, ExtensionOrigin::Package { .. }));
        assert!(r.diagnostics.is_empty(), "{:?}", r.diagnostics);

        let r = discover_extensions(&DiscoveryOptions {
            include_discovered: false,
            ..opts
        });
        assert_eq!(r.extensions.len(), 1);
    }
}
