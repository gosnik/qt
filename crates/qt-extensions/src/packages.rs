//! Extension packages: settings `packages` entries, `qt install/remove/update/list`
//! and `qt config` resource filters (archived `tools/package_manager_tool.cpp`,
//! `package_manager_execution.cpp` and `app/cli/package_command.cpp`).
//!
//! Sources are `npm:<spec>`, git URLs (`git:host/owner/repo[@ref]`,
//! `https://…`, `ssh://…`, `git://…`, `git:git@host:owner/repo`) or local
//! paths. User packages live in `<agent_home>/settings.json`, project
//! packages (`-l`) in `<cwd>/.pi/settings.json`. Git checkouts go to
//! `<agent_home>/git/<host>/<path>` or `<cwd>/.pi/git/…`; project npm packages
//! to `<cwd>/.pi/npm`, user npm packages are installed globally.

use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;

use serde_json::{Map, Value, json};

use crate::filters::{display_path, lexical_normal, resource_enabled_by_filters};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PackageScope {
    #[default]
    User,
    Project,
}

impl PackageScope {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::User => "user",
            Self::Project => "project",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResourceKind {
    Extensions,
    Skills,
    Prompts,
    Themes,
}

impl ResourceKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Extensions => "extensions",
            Self::Skills => "skills",
            Self::Prompts => "prompts",
            Self::Themes => "themes",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "extensions" | "extension" => Some(Self::Extensions),
            "skills" | "skill" => Some(Self::Skills),
            "prompts" | "prompt" | "prompt-templates" | "prompt-template" => Some(Self::Prompts),
            "themes" | "theme" => Some(Self::Themes),
            _ => None,
        }
    }
}

/// One `packages` entry.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct ConfiguredPackage {
    pub source: String,
    pub scope: PackageScope,
    /// Written as an object (filters apply; missing lists disable that kind).
    pub filtered: bool,
    pub extensions: Vec<String>,
    pub skills: Vec<String>,
    pub prompts: Vec<String>,
    pub themes: Vec<String>,
    /// Where the package is on disk, when installed.
    pub installed_path: Option<String>,
}

impl ConfiguredPackage {
    fn filters_mut(&mut self, kind: ResourceKind) -> &mut Vec<String> {
        match kind {
            ResourceKind::Extensions => &mut self.extensions,
            ResourceKind::Skills => &mut self.skills,
            ResourceKind::Prompts => &mut self.prompts,
            ResourceKind::Themes => &mut self.themes,
        }
    }

    pub fn filters(&self, kind: ResourceKind) -> &[String] {
        match kind {
            ResourceKind::Extensions => &self.extensions,
            ResourceKind::Skills => &self.skills,
            ResourceKind::Prompts => &self.prompts,
            ResourceKind::Themes => &self.themes,
        }
    }

    /// The settings JSON form: the bare source, or an object with filters.
    pub fn settings_json(&self) -> Value {
        let has_filters = self.filtered
            || !self.extensions.is_empty()
            || !self.skills.is_empty()
            || !self.prompts.is_empty()
            || !self.themes.is_empty();
        if !has_filters {
            return json!(self.source);
        }
        let mut entry = json!({ "source": self.source });
        for kind in [
            ResourceKind::Extensions,
            ResourceKind::Skills,
            ResourceKind::Prompts,
            ResourceKind::Themes,
        ] {
            if !self.filters(kind).is_empty() {
                entry[kind.as_str()] = json!(self.filters(kind));
            }
        }
        entry
    }
}

/// Packages and `npmCommand` from both settings files.
#[derive(Debug, Clone, Default)]
pub struct PackageSettings {
    pub npm_command: Option<Vec<String>>,
    pub user: Vec<ConfiguredPackage>,
    pub project: Vec<ConfiguredPackage>,
}

impl PackageSettings {
    /// User packages first, then project packages.
    pub fn all(&self) -> Vec<ConfiguredPackage> {
        self.user
            .iter()
            .chain(self.project.iter())
            .cloned()
            .collect()
    }
}

/// `PI_OFFLINE` set to anything but empty or `0`.
pub fn offline_mode() -> bool {
    std::env::var("PI_OFFLINE").is_ok_and(|v| !v.is_empty() && v != "0")
}

fn expand_home(text: &str) -> Option<PathBuf> {
    if !text.starts_with('~') {
        return None;
    }
    let home = dirs::home_dir()?;
    if text == "~" {
        return Some(home);
    }
    Some(match text.strip_prefix("~/") {
        Some(rest) => home.join(rest),
        None => home.join(&text[1..]),
    })
}

/// Parsed git source.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GitSource {
    pub host: String,
    pub path: String,
    pub repo: String,
    pub reference: Option<String>,
}

fn split_ref(path: &str) -> (String, Option<String>) {
    match path.find(['@', '#']) {
        Some(i) if i > 0 && i + 1 < path.len() => (path[..i].into(), Some(path[i + 1..].into())),
        _ => (path.into(), None),
    }
}

pub fn git_source(source: &str) -> Option<GitSource> {
    let has_prefix = source.starts_with("git:");
    let value = if has_prefix {
        &source[4..]
    } else if ["http://", "https://", "ssh://", "git://"]
        .iter()
        .any(|p| source.starts_with(p))
    {
        source
    } else {
        return None;
    };
    let mut details = if let Some(rest) = value.strip_prefix("git@") {
        let colon = rest.find(':')?;
        let host = rest[..colon].to_string();
        let (path, reference) = split_ref(&rest[colon + 1..]);
        GitSource {
            repo: format!("git@{host}:{path}"),
            host,
            path,
            reference,
        }
    } else if let Some(scheme_end) = value.find("://") {
        let scheme = &value[..scheme_end];
        let rest = &value[scheme_end + 3..];
        let slash = rest.find('/')?;
        let authority = &rest[..slash];
        let host = authority
            .rfind('@')
            .map(|at| authority[at + 1..].to_string())
            .unwrap_or_else(|| authority.to_string());
        let (path, reference) = split_ref(&rest[slash + 1..]);
        GitSource {
            repo: format!("{scheme}://{authority}/{path}"),
            host,
            path,
            reference,
        }
    } else {
        let slash = value.find('/')?;
        if !has_prefix {
            return None;
        }
        let host = value[..slash].to_string();
        let (path, reference) = split_ref(&value[slash + 1..]);
        GitSource {
            repo: format!("{host}/{path}"),
            host,
            path,
            reference,
        }
    };
    if details.path.len() > 4 && details.path.ends_with(".git") {
        details.path.truncate(details.path.len() - 4);
    }
    details.path = details.path.trim_start_matches('/').to_string();
    if details.host.is_empty() || details.path.is_empty() || !details.path.contains('/') {
        return None;
    }
    Some(details)
}

pub fn is_npm_source(source: &str) -> bool {
    source.starts_with("npm:")
}

pub fn is_local_source(source: &str) -> bool {
    !is_npm_source(source) && git_source(source).is_none()
}

/// npm package name of a spec (`@scope/name@1.2` → `@scope/name`).
pub fn npm_name(spec: &str) -> &str {
    if spec.is_empty() {
        return spec;
    }
    let from = usize::from(spec.starts_with('@'));
    match spec[from..].find('@') {
        Some(i) => &spec[..from + i],
        None => spec,
    }
}

fn normalize_local(source: &str, base: Option<&Path>) -> String {
    let mut s = source.replace('\\', "/");
    while s.len() > 1 && s.ends_with('/') {
        s.pop();
    }
    let Some(base) = base else {
        if let Some(p) = expand_home(&s) {
            return display_path(&p);
        }
        return s;
    };
    let mut p = expand_home(&s).unwrap_or_else(|| PathBuf::from(&s));
    if p.is_relative() {
        p = base.join(p);
    }
    let p = lexical_normal(&p);
    display_path(&std::fs::canonicalize(&p).unwrap_or(p))
}

/// `npm:<name>`, `git:<host>/<path>` or `local:<path>` (relative project paths
/// resolved against `<cwd>/.pi`).
pub fn source_identity(source: &str, base: Option<&Path>) -> String {
    if let Some(spec) = source.strip_prefix("npm:") {
        return format!("npm:{}", npm_name(spec));
    }
    if let Some(g) = git_source(source) {
        return format!("git:{}/{}", g.host, g.path);
    }
    format!("local:{}", normalize_local(source, base))
}

fn scope_base(cwd: &Path, scope: PackageScope) -> Option<PathBuf> {
    (scope == PackageScope::Project && !cwd.as_os_str().is_empty()).then(|| cwd.join(".pi"))
}

fn scoped_identity(source: &str, scope: PackageScope, cwd: &Path) -> String {
    source_identity(source, scope_base(cwd, scope).as_deref())
}

fn git_pinned(source: &str) -> bool {
    git_source(source).is_some_and(|g| g.reference.is_some())
}

pub fn is_update_candidate(source: &str) -> bool {
    if let Some(spec) = source.strip_prefix("npm:") {
        return npm_name(spec) == spec;
    }
    git_source(source).is_some() && !git_pinned(source)
}

pub fn update_skip_reason(source: &str) -> &'static str {
    if let Some(spec) = source.strip_prefix("npm:")
        && npm_name(spec) != spec
    {
        return "pinned npm package";
    }
    if git_source(source).is_some() && git_pinned(source) {
        return "pinned git ref";
    }
    if is_local_source(source) {
        return "local package sources are not updated";
    }
    "package source is not an update candidate"
}

fn git_root(cwd: &Path, agent_home: &Path, scope: PackageScope) -> PathBuf {
    match scope {
        PackageScope::Project => cwd.join(".pi").join("git"),
        PackageScope::User => agent_home.join("git"),
    }
}

fn git_path(g: &GitSource, cwd: &Path, agent_home: &Path, scope: PackageScope) -> PathBuf {
    git_root(cwd, agent_home, scope).join(&g.host).join(&g.path)
}

fn npm_project_root(cwd: &Path) -> PathBuf {
    cwd.join(".pi").join("npm")
}

fn installed_path(
    source: &str,
    scope: PackageScope,
    cwd: &Path,
    agent_home: &Path,
) -> Option<String> {
    let candidate = if let Some(spec) = source.strip_prefix("npm:") {
        if scope != PackageScope::Project {
            return None;
        }
        npm_project_root(cwd)
            .join("node_modules")
            .join(npm_name(spec))
    } else if let Some(g) = git_source(source) {
        if scope == PackageScope::User && agent_home.as_os_str().is_empty() {
            return None;
        }
        git_path(&g, cwd, agent_home, scope)
    } else {
        let base = match scope {
            PackageScope::Project => cwd.join(".pi"),
            PackageScope::User => agent_home.to_path_buf(),
        };
        let p = expand_home(source).unwrap_or_else(|| PathBuf::from(source));
        if p.is_relative() { base.join(p) } else { p }
    };
    candidate
        .exists()
        .then(|| display_path(&std::fs::canonicalize(&candidate).unwrap_or(candidate)))
}

fn string_list(object: &Map<String, Value>, key: &str) -> Result<Vec<String>, String> {
    match object.get(key) {
        None => Ok(Vec::new()),
        Some(Value::Array(items)) => items
            .iter()
            .map(|v| {
                v.as_str()
                    .map(str::to_string)
                    .ok_or_else(|| format!("{key} entries must be strings"))
            })
            .collect(),
        Some(_) => Err(format!("{key} must be an array")),
    }
}

fn parse_packages(
    settings: &Value,
    scope: PackageScope,
    cwd: &Path,
    agent_home: &Path,
) -> Result<Vec<ConfiguredPackage>, String> {
    let Some(packages) = settings.get("packages") else {
        return Ok(Vec::new());
    };
    let packages = packages
        .as_array()
        .ok_or_else(|| "packages must be an array".to_string())?;
    let mut out = Vec::new();
    for entry in packages {
        let mut p = ConfiguredPackage {
            scope,
            ..Default::default()
        };
        match entry {
            Value::String(s) => p.source = s.clone(),
            Value::Object(o) => {
                p.source = o
                    .get("source")
                    .and_then(Value::as_str)
                    .ok_or_else(|| "package source must be a string".to_string())?
                    .into();
                p.filtered = true;
                p.extensions = string_list(o, "extensions")?;
                p.skills = string_list(o, "skills")?;
                p.prompts = string_list(o, "prompts")?;
                p.themes = string_list(o, "themes")?;
            }
            _ => return Err("package entries must be strings or objects".into()),
        }
        p.installed_path = installed_path(&p.source, scope, cwd, agent_home);
        out.push(p);
    }
    Ok(out)
}

/// Read a settings document; a missing file is an empty object.
pub fn read_settings_document(path: &Path) -> Result<Value, String> {
    if !path.exists() {
        return Ok(json!({}));
    }
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", display_path(path)))?;
    let value: Value = serde_json::from_str(&text)
        .map_err(|e| format!("invalid settings JSON in {}: {e}", display_path(path)))?;
    if !value.is_object() {
        return Err("settings document must be a JSON object".into());
    }
    Ok(value)
}

/// Settings file of a scope.
pub fn settings_path(scope: PackageScope, cwd: &Path, agent_home: &Path) -> PathBuf {
    match scope {
        PackageScope::Project => cwd.join(".pi").join("settings.json"),
        PackageScope::User => agent_home.join("settings.json"),
    }
}

/// Load `packages` and `npmCommand` (user settings) for `cwd`.
pub fn load_package_settings(cwd: &Path, agent_home: &Path) -> Result<PackageSettings, String> {
    let user = read_settings_document(&agent_home.join("settings.json"))?;
    let project = read_settings_document(&cwd.join(".pi").join("settings.json"))?;
    let npm_command = match user.get("npmCommand") {
        None => None,
        Some(Value::Array(items)) => Some(
            items
                .iter()
                .map(|v| {
                    v.as_str()
                        .map(str::to_string)
                        .ok_or_else(|| "npmCommand entries must be strings".to_string())
                })
                .collect::<Result<Vec<_>, _>>()?,
        ),
        Some(_) => return Err("npmCommand must be an array".into()),
    };
    Ok(PackageSettings {
        npm_command,
        user: parse_packages(&user, PackageScope::User, cwd, agent_home)?,
        project: parse_packages(&project, PackageScope::Project, cwd, agent_home)?,
    })
}

/// Replace the `packages` key of a scope's settings file, keeping other keys.
pub fn write_scope_packages(
    path: &Path,
    scope: PackageScope,
    packages: &[ConfiguredPackage],
) -> Result<(), String> {
    let mut document = read_settings_document(path)?;
    document["packages"] = packages
        .iter()
        .filter(|p| p.scope == scope)
        .map(ConfiguredPackage::settings_json)
        .collect();
    let text = serde_json::to_string_pretty(&document).map_err(|e| e.to_string())? + "\n";
    if std::fs::read_to_string(path).ok().as_deref() == Some(text.as_str()) {
        return Ok(());
    }
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    std::fs::write(path, text).map_err(|e| e.to_string())
}

// ----- command parsing ------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PackageCommandKind {
    Install,
    Remove,
    Update,
    List,
}

impl PackageCommandKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Install => "install",
            Self::Remove => "remove",
            Self::Update => "update",
            Self::List => "list",
        }
    }

    pub fn usage(self) -> &'static str {
        match self {
            Self::Install => "qt install <source> [-l]",
            Self::Remove => "qt remove <source> [-l]",
            Self::Update => {
                "qt update [source|self|pi] [--self] [--extensions] [--extension <source>] [--force]"
            }
            Self::List => "qt list",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum UpdateTargetKind {
    #[default]
    All,
    SelfOnly,
    Extensions,
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct UpdateTarget {
    pub kind: UpdateTargetKind,
    pub source: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct PackageCommand {
    pub command: PackageCommandKind,
    pub source: Option<String>,
    pub local: bool,
    pub force: bool,
    pub help: bool,
    pub update_target: Option<UpdateTarget>,
    pub invalid_option: Option<String>,
    pub invalid_argument: Option<String>,
    pub missing_option_value: Option<String>,
    pub conflicting_options: Option<String>,
}

/// Parse `install|uninstall|remove|update|list ...`; `None` for other words.
pub fn parse_package_command(args: &[String]) -> Option<PackageCommand> {
    let command = match args.first()?.as_str() {
        "install" => PackageCommandKind::Install,
        "remove" | "uninstall" => PackageCommandKind::Remove,
        "update" => PackageCommandKind::Update,
        "list" => PackageCommandKind::List,
        _ => return None,
    };
    let mut r = PackageCommand {
        command,
        source: None,
        local: false,
        force: false,
        help: false,
        update_target: None,
        invalid_option: None,
        invalid_argument: None,
        missing_option_value: None,
        conflicting_options: None,
    };
    let is_update = command == PackageCommandKind::Update;
    let (mut self_flag, mut extensions_flag) = (false, false);
    let mut extension_source: Option<String> = None;
    let mut i = 1;
    while i < args.len() {
        let arg = &args[i];
        let invalid = |r: &mut PackageCommand| {
            r.invalid_option.get_or_insert_with(|| arg.clone());
        };
        match arg.as_str() {
            "-h" | "--help" => r.help = true,
            "-l" | "--local" => {
                if matches!(
                    command,
                    PackageCommandKind::Install | PackageCommandKind::Remove
                ) {
                    r.local = true;
                } else {
                    invalid(&mut r);
                }
            }
            "--self" if is_update => self_flag = true,
            "--extensions" if is_update => extensions_flag = true,
            "--force" if is_update => r.force = true,
            "--extension" if is_update => {
                let has_value = args
                    .get(i + 1)
                    .is_some_and(|v| !v.is_empty() && !v.starts_with('-'));
                if !has_value {
                    r.missing_option_value.get_or_insert_with(|| arg.clone());
                } else if extension_source.is_some() {
                    r.conflicting_options
                        .get_or_insert_with(|| "--extension can only be provided once".into());
                    i += 1;
                } else {
                    i += 1;
                    extension_source = Some(args[i].clone());
                }
            }
            _ if arg.starts_with('-') => invalid(&mut r),
            _ => {
                if r.source.is_none() {
                    r.source = Some(arg.clone());
                } else {
                    r.invalid_argument.get_or_insert_with(|| arg.clone());
                }
            }
        }
        i += 1;
    }
    if is_update {
        let mut target = UpdateTarget::default();
        if let Some(src) = extension_source {
            if self_flag || extensions_flag {
                r.conflicting_options.get_or_insert_with(|| {
                    "--extension cannot be combined with --self or --extensions".into()
                });
            }
            if r.source.is_some() {
                r.conflicting_options.get_or_insert_with(|| {
                    "--extension cannot be combined with a positional source".into()
                });
            }
            target.kind = UpdateTargetKind::Extensions;
            target.source = Some(src);
        } else if let Some(src) = &r.source {
            if src == "self" || src == "pi" {
                target.kind = if extensions_flag {
                    UpdateTargetKind::All
                } else {
                    UpdateTargetKind::SelfOnly
                };
            } else {
                if extensions_flag || self_flag {
                    r.conflicting_options.get_or_insert_with(|| {
                        "positional update targets cannot be combined with --self or --extensions"
                            .into()
                    });
                }
                target.kind = UpdateTargetKind::Extensions;
                target.source = Some(src.clone());
            }
        } else {
            target.kind = match (self_flag, extensions_flag) {
                (true, false) => UpdateTargetKind::SelfOnly,
                (false, true) => UpdateTargetKind::Extensions,
                _ => UpdateTargetKind::All,
            };
        }
        r.update_target = Some(target);
    }
    Some(r)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ConfigCommandKind {
    #[default]
    List,
    Enable,
    Disable,
}

impl ConfigCommandKind {
    pub fn usage(self) -> &'static str {
        match self {
            Self::List => "qt config [list]",
            Self::Enable => {
                "qt config enable <source> <extensions|skills|prompts|themes> <resource-path> --package-root <path> [-l]"
            }
            Self::Disable => {
                "qt config disable <source> <extensions|skills|prompts|themes> <resource-path> --package-root <path> [-l]"
            }
        }
    }
}

#[derive(Debug, Clone, PartialEq, Default)]
pub struct ConfigCommand {
    pub command: ConfigCommandKind,
    pub scope: PackageScope,
    pub source: Option<String>,
    pub resource_kind: Option<ResourceKind>,
    pub resource_path: Option<PathBuf>,
    pub package_root: Option<PathBuf>,
    pub help: bool,
    pub invalid_option: Option<String>,
    pub invalid_argument: Option<String>,
    pub missing_option_value: Option<String>,
}

/// Parse `config [list|enable|disable] ...`; `None` unless `args[0] == "config"`.
pub fn parse_config_command(args: &[String]) -> Option<ConfigCommand> {
    if args.first().map(String::as_str) != Some("config") {
        return None;
    }
    let mut r = ConfigCommand::default();
    let mut i = 1;
    if let Some(word) = args.get(1).filter(|a| !a.starts_with('-')) {
        match word.as_str() {
            "list" => i = 2,
            "enable" => {
                r.command = ConfigCommandKind::Enable;
                i = 2;
            }
            "disable" => {
                r.command = ConfigCommandKind::Disable;
                i = 2;
            }
            _ => {}
        }
    }
    let mut positional = Vec::new();
    while i < args.len() {
        let arg = &args[i];
        match arg.as_str() {
            "-h" | "--help" => r.help = true,
            "-l" | "--local" => {
                if r.command == ConfigCommandKind::List {
                    r.invalid_option.get_or_insert_with(|| arg.clone());
                } else {
                    r.scope = PackageScope::Project;
                }
            }
            "--package-root" => {
                if args
                    .get(i + 1)
                    .is_some_and(|v| !v.is_empty() && !v.starts_with('-'))
                {
                    i += 1;
                    r.package_root = Some(PathBuf::from(&args[i]));
                } else {
                    r.missing_option_value.get_or_insert_with(|| arg.clone());
                }
            }
            _ if arg.starts_with('-') => {
                r.invalid_option.get_or_insert_with(|| arg.clone());
            }
            _ => positional.push(arg.clone()),
        }
        i += 1;
    }
    if r.command == ConfigCommandKind::List {
        if let Some(first) = positional.first() {
            r.invalid_argument.get_or_insert_with(|| first.clone());
        }
        return Some(r);
    }
    r.source = positional.first().cloned();
    if let Some(kind) = positional.get(1) {
        r.resource_kind = ResourceKind::parse(kind);
        if r.resource_kind.is_none() {
            r.invalid_argument.get_or_insert_with(|| kind.clone());
        }
    }
    r.resource_path = positional.get(2).map(PathBuf::from);
    if let Some(extra) = positional.get(3) {
        r.invalid_argument.get_or_insert_with(|| extra.clone());
    }
    Some(r)
}

// ----- plans ----------------------------------------------------------------

/// A package-manager command to run for a source.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct SourceCommandPlan {
    pub scope: PackageScope,
    pub source: String,
    pub source_type: String,
    pub argv: Vec<String>,
    pub working_directory: Option<PathBuf>,
    pub install_root: Option<PathBuf>,
    pub install_path: Option<PathBuf>,
    /// Run only when `working_directory/package.json` exists.
    pub conditional: bool,
    pub skipped: bool,
    pub reason: Option<String>,
}

impl SourceCommandPlan {
    pub fn command_text(&self) -> String {
        self.argv.join(" ")
    }
}

fn npm_prefix(settings: &PackageSettings) -> Option<Vec<String>> {
    match &settings.npm_command {
        None => Some(vec!["npm".into()]),
        Some(c) if c.is_empty() => Some(vec!["npm".into()]),
        Some(c) if c[0].is_empty() => None,
        Some(c) => Some(c.clone()),
    }
}

fn skipped(source: &str, scope: PackageScope, reason: &str) -> SourceCommandPlan {
    SourceCommandPlan {
        scope,
        source: source.into(),
        source_type: if is_npm_source(source) {
            "npm"
        } else if git_source(source).is_some() {
            "git"
        } else {
            "local"
        }
        .into(),
        skipped: true,
        reason: Some(reason.into()),
        ..Default::default()
    }
}

/// Commands that install, remove or update `source` (archived plans).
pub fn source_command_plans(
    command: PackageCommandKind,
    source: &str,
    scope: PackageScope,
    cwd: &Path,
    agent_home: &Path,
    settings: &PackageSettings,
) -> Vec<SourceCommandPlan> {
    if command == PackageCommandKind::List {
        return vec![skipped(
            source,
            scope,
            "list does not require package-manager commands",
        )];
    }
    let Some(spec) = source.strip_prefix("npm:").filter(|s| !s.is_empty()) else {
        let Some(git) = git_source(source) else {
            return vec![skipped(
                source,
                scope,
                if is_local_source(source) {
                    "local package sources do not require package-manager commands"
                } else {
                    "only npm and git package command plans are deterministic without installation state"
                },
            )];
        };
        if scope == PackageScope::User && agent_home.as_os_str().is_empty() {
            return vec![skipped(
                source,
                scope,
                "agent_home is required for user git package command paths",
            )];
        }
        let root = git_root(cwd, agent_home, scope);
        let target = git_path(&git, cwd, agent_home, scope);
        let base = SourceCommandPlan {
            scope,
            source: source.into(),
            source_type: "git".into(),
            install_root: Some(root),
            install_path: Some(target.clone()),
            ..Default::default()
        };
        let dependencies = || {
            let mut d = SourceCommandPlan {
                working_directory: Some(target.clone()),
                conditional: true,
                ..base.clone()
            };
            match npm_prefix(settings) {
                Some(mut argv) => {
                    argv.push("install".into());
                    d.argv = argv;
                }
                None => {
                    d.skipped = true;
                    d.reason = Some("npmCommand is invalid".into());
                }
            }
            d
        };
        return match command {
            PackageCommandKind::Install => {
                let mut plans = vec![SourceCommandPlan {
                    argv: vec![
                        "git".into(),
                        "clone".into(),
                        git.repo.clone(),
                        display_path(&target),
                    ],
                    ..base.clone()
                }];
                if let Some(r) = &git.reference {
                    plans.push(SourceCommandPlan {
                        argv: vec!["git".into(), "checkout".into(), r.clone()],
                        working_directory: Some(target.clone()),
                        ..base.clone()
                    });
                }
                plans.push(dependencies());
                plans
            }
            PackageCommandKind::Remove => vec![SourceCommandPlan {
                argv: vec!["rm".into(), "-rf".into(), display_path(&target)],
                ..base
            }],
            _ => vec![
                SourceCommandPlan {
                    argv: vec![
                        "git".into(),
                        "fetch".into(),
                        "--prune".into(),
                        "origin".into(),
                    ],
                    working_directory: Some(target.clone()),
                    ..base.clone()
                },
                dependencies(),
            ],
        };
    };
    let Some(mut argv) = npm_prefix(settings) else {
        return vec![skipped(source, scope, "npmCommand is invalid")];
    };
    let name = npm_name(spec);
    let mut plan = SourceCommandPlan {
        scope,
        source: source.into(),
        source_type: "npm".into(),
        ..Default::default()
    };
    let target = match command {
        PackageCommandKind::Install => spec.to_string(),
        PackageCommandKind::Remove => name.to_string(),
        _ => format!("{name}@latest"),
    };
    let verb = if command == PackageCommandKind::Remove {
        "uninstall"
    } else {
        "install"
    };
    if scope == PackageScope::User {
        argv.extend([verb.into(), "-g".into(), target]);
    } else {
        let root = npm_project_root(cwd);
        plan.install_path = Some(root.join("node_modules").join(name));
        argv.extend([verb.into(), target, "--prefix".into(), display_path(&root)]);
        plan.install_root = Some(root);
    }
    plan.argv = argv;
    vec![plan]
}

/// The outcome of an install/remove/update/list command before execution.
#[derive(Debug, Clone, Default)]
pub struct PackageSettingsPlan {
    pub scope: Option<PackageScope>,
    pub configured: Vec<ConfiguredPackage>,
    pub settings_changed: bool,
    pub source_plans: Vec<SourceCommandPlan>,
    pub package_updates: Vec<ConfiguredPackage>,
    pub skipped_updates: Vec<(ConfiguredPackage, String)>,
    pub self_update: bool,
    pub error: Option<String>,
}

fn suggest_npm(source: &str, configured: &[ConfiguredPackage]) -> Option<String> {
    if is_npm_source(source) {
        return None;
    }
    let wanted = format!("npm:{}", npm_name(source));
    configured
        .iter()
        .find(|c| is_npm_source(&c.source) && source_identity(&c.source, None) == wanted)
        .map(|c| c.source.clone())
}

/// Apply a parsed package command to the configured packages.
pub fn package_settings_plan(
    parsed: &PackageCommand,
    cwd: &Path,
    agent_home: &Path,
    settings: &PackageSettings,
) -> PackageSettingsPlan {
    let mut plan = PackageSettingsPlan {
        configured: settings.all(),
        ..Default::default()
    };
    if let Some(o) = &parsed.invalid_option {
        plan.error = Some(format!(
            "Unknown option {o} for \"{}\".",
            parsed.command.as_str()
        ));
        return plan;
    }
    if let Some(o) = &parsed.missing_option_value {
        plan.error = Some(format!("Missing value for {o}."));
        return plan;
    }
    if let Some(a) = &parsed.invalid_argument {
        plan.error = Some(format!("Unexpected argument {a}."));
        return plan;
    }
    if let Some(c) = &parsed.conflicting_options {
        plan.error = Some(c.clone());
        return plan;
    }
    let scope = if parsed.local {
        PackageScope::Project
    } else {
        PackageScope::User
    };
    match parsed.command {
        PackageCommandKind::List => {}
        PackageCommandKind::Install | PackageCommandKind::Remove if parsed.source.is_none() => {
            plan.error = Some(format!("Missing {} source.", parsed.command.as_str()));
        }
        PackageCommandKind::Install => {
            let source = parsed.source.clone().unwrap_or_default();
            plan.scope = Some(scope);
            let id = scoped_identity(&source, scope, cwd);
            let exists = plan
                .configured
                .iter()
                .any(|c| c.scope == scope && scoped_identity(&c.source, c.scope, cwd) == id);
            if !exists {
                plan.configured.push(ConfiguredPackage {
                    source: source.clone(),
                    scope,
                    ..Default::default()
                });
                plan.settings_changed = true;
                plan.source_plans = source_command_plans(
                    PackageCommandKind::Install,
                    &source,
                    scope,
                    cwd,
                    agent_home,
                    settings,
                );
            }
        }
        PackageCommandKind::Remove => {
            let source = parsed.source.clone().unwrap_or_default();
            plan.scope = Some(scope);
            let id = scoped_identity(&source, scope, cwd);
            let mut next = Vec::new();
            for c in std::mem::take(&mut plan.configured) {
                if c.scope == scope && scoped_identity(&c.source, c.scope, cwd) == id {
                    plan.settings_changed = true;
                    plan.source_plans.extend(source_command_plans(
                        PackageCommandKind::Remove,
                        &c.source,
                        c.scope,
                        cwd,
                        agent_home,
                        settings,
                    ));
                } else {
                    next.push(c);
                }
            }
            plan.configured = next;
            if !plan.settings_changed {
                plan.error = Some(format!("No matching package found for {source}"));
            }
        }
        PackageCommandKind::Update => {
            let target = parsed.update_target.clone().unwrap_or_default();
            plan.self_update = matches!(
                target.kind,
                UpdateTargetKind::All | UpdateTargetKind::SelfOnly
            );
            let offline = offline_mode();
            if matches!(
                target.kind,
                UpdateTargetKind::All | UpdateTargetKind::Extensions
            ) {
                for c in &plan.configured {
                    if let Some(src) = &target.source
                        && scoped_identity(&c.source, c.scope, cwd)
                            != scoped_identity(src, c.scope, cwd)
                    {
                        continue;
                    }
                    if offline {
                        plan.skipped_updates
                            .push((c.clone(), "offline mode disables package updates".into()));
                    } else if is_update_candidate(&c.source) {
                        plan.package_updates.push(c.clone());
                    } else {
                        plan.skipped_updates
                            .push((c.clone(), update_skip_reason(&c.source).into()));
                    }
                }
                if let Some(src) = &target.source
                    && plan.package_updates.is_empty()
                    && plan.skipped_updates.is_empty()
                {
                    let mut error = format!("No matching package found for {src}");
                    if let Some(s) = suggest_npm(src, &plan.configured) {
                        error.push_str(&format!(". Did you mean {s}?"));
                    }
                    plan.error = Some(error);
                }
            }
            for u in plan.package_updates.clone() {
                plan.source_plans.extend(source_command_plans(
                    PackageCommandKind::Update,
                    &u.source,
                    u.scope,
                    cwd,
                    agent_home,
                    settings,
                ));
            }
        }
    }
    plan
}

/// Request of [`resource_toggle_plan`].
#[derive(Debug, Clone)]
pub struct ResourceToggle {
    pub source: String,
    pub scope: PackageScope,
    pub kind: ResourceKind,
    pub package_root: PathBuf,
    pub resource_path: PathBuf,
    pub enabled: bool,
}

/// Result of a resource toggle.
#[derive(Debug, Clone, Default)]
pub struct ToggleOutcome {
    pub configured: Vec<ConfiguredPackage>,
    pub settings_changed: bool,
    pub pattern: String,
    pub error: Option<String>,
}

/// The settings pattern of a resource: relative to the package root when inside it.
pub fn resource_pattern(package_root: &Path, resource: &Path) -> String {
    match lexical_normal(resource).strip_prefix(lexical_normal(package_root)) {
        Ok(rel) if !rel.as_os_str().is_empty() => display_path(rel),
        _ => display_path(resource),
    }
}

/// Enable or disable one package resource by appending `+pattern` / `-pattern`.
pub fn resource_toggle_plan(request: &ResourceToggle, settings: &PackageSettings) -> ToggleOutcome {
    let mut out = ToggleOutcome {
        configured: settings.all(),
        pattern: resource_pattern(&request.package_root, &request.resource_path),
        ..Default::default()
    };
    let id = source_identity(&request.source, None);
    for c in &mut out.configured {
        if c.scope != request.scope || source_identity(&c.source, None) != id {
            continue;
        }
        let filters = c.filters_mut(request.kind);
        if resource_enabled_by_filters(filters, &out.pattern, &request.resource_path)
            == request.enabled
        {
            return out;
        }
        let pattern = out.pattern.clone();
        filters.retain(|f| f.strip_prefix(['!', '+', '-']).unwrap_or(f) != pattern);
        filters.push(format!(
            "{}{pattern}",
            if request.enabled { "+" } else { "-" }
        ));
        c.filtered = true;
        out.settings_changed = true;
        return out;
    }
    out.error = Some(format!("No matching package found for {}", request.source));
    out
}

// ----- execution ------------------------------------------------------------

const CACHE_GITIGNORE: &str = "*\n!.gitignore\n";
const NPM_PROJECT_MANIFEST: &str = "{\n  \"name\": \"pi-extensions\",\n  \"private\": true\n}";

fn write_if_missing(path: &Path, content: &str) -> Result<(), String> {
    if path.exists() {
        return Ok(());
    }
    std::fs::write(path, content).map_err(|e| format!("{}: {e}", display_path(path)))
}

/// Prepare cache directories, then run one plan. Skipped plans and
/// conditional plans without a `package.json` do nothing.
pub fn execute_source_plan(plan: &SourceCommandPlan) -> Result<bool, String> {
    if plan.skipped || plan.argv.is_empty() {
        return Ok(false);
    }
    if let Some(root) = &plan.install_root {
        std::fs::create_dir_all(root).map_err(|e| format!("{}: {e}", display_path(root)))?;
        write_if_missing(&root.join(".gitignore"), CACHE_GITIGNORE)?;
        if plan.source_type == "npm" {
            write_if_missing(&root.join("package.json"), NPM_PROJECT_MANIFEST)?;
        }
    }
    if plan.conditional
        && !plan
            .working_directory
            .as_ref()
            .is_some_and(|d| d.join("package.json").is_file())
    {
        return Ok(false);
    }
    if plan.argv[0] == "rm" {
        // Removal of a git checkout; done in-process rather than via `rm`.
        if let Some(path) = &plan.install_path
            && path.exists()
        {
            std::fs::remove_dir_all(path).map_err(|e| format!("{}: {e}", display_path(path)))?;
            if let Some(root) = &plan.install_root {
                let mut dir = path.parent().map(Path::to_path_buf);
                while let Some(d) = dir {
                    if d == *root || std::fs::remove_dir(&d).is_err() {
                        break;
                    }
                    dir = d.parent().map(Path::to_path_buf);
                }
            }
        }
        return Ok(true);
    }
    if let Some(path) = &plan.install_path
        && plan.argv.get(1).map(String::as_str) == Some("clone")
        && let Some(parent) = path.parent()
    {
        std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", display_path(parent)))?;
    }
    let mut cmd = Command::new(&plan.argv[0]);
    cmd.args(&plan.argv[1..]);
    if let Some(dir) = &plan.working_directory {
        cmd.current_dir(dir);
    }
    let output = cmd
        .output()
        .map_err(|e| format!("{}: {e}", plan.command_text()))?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let detail = stderr.trim();
        return Err(format!(
            "{} failed ({}){}",
            plan.command_text(),
            output.status,
            if detail.is_empty() {
                String::new()
            } else {
                format!(": {detail}")
            }
        ));
    }
    Ok(true)
}

// ----- CLI ------------------------------------------------------------------

/// How the CLI runs package commands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PackageRunOptions {
    /// Run the npm/git commands (`false` prints them as a plan, the archived behaviour).
    pub execute: bool,
}

impl Default for PackageRunOptions {
    fn default() -> Self {
        Self { execute: true }
    }
}

fn print_source_plans(out: &mut dyn Write, plans: &[SourceCommandPlan], heading: &str) {
    if plans.is_empty() {
        return;
    }
    let _ = writeln!(out, "{heading}:");
    for p in plans {
        let _ = writeln!(out, "  [{}] {}", p.scope.as_str(), p.source);
        if p.skipped {
            let _ = match &p.reason {
                Some(r) => writeln!(out, "    skipped: {r}"),
                None => writeln!(out, "    skipped"),
            };
        } else {
            let _ = writeln!(out, "    {}", p.command_text());
        }
    }
}

fn print_package_list(out: &mut dyn Write, packages: &[ConfiguredPackage]) {
    if packages.is_empty() {
        let _ = writeln!(out, "No packages installed.");
        return;
    }
    fn group(
        out: &mut dyn Write,
        packages: &[ConfiguredPackage],
        title: &str,
        scope: PackageScope,
    ) -> bool {
        let items: Vec<_> = packages.iter().filter(|p| p.scope == scope).collect();
        if items.is_empty() {
            return false;
        }
        let _ = writeln!(out, "{title}:");
        for p in items {
            let _ = writeln!(
                out,
                "  {}{}",
                p.source,
                if p.filtered { " (filtered)" } else { "" }
            );
            if let Some(path) = &p.installed_path {
                let _ = writeln!(out, "    {path}");
            }
        }
        true
    }
    let user = group(out, packages, "User packages", PackageScope::User);
    if user && packages.iter().any(|p| p.scope == PackageScope::Project) {
        let _ = writeln!(out);
    }
    group(out, packages, "Project packages", PackageScope::Project);
}

fn print_configured_resources(out: &mut dyn Write, packages: &[ConfiguredPackage]) {
    let _ = writeln!(out, "\nConfigured packages:");
    if packages.is_empty() {
        let _ = writeln!(out, "  (none)");
        return;
    }
    for (label, scope) in [
        ("user", PackageScope::User),
        ("project", PackageScope::Project),
    ] {
        let items: Vec<_> = packages.iter().filter(|p| p.scope == scope).collect();
        if items.is_empty() {
            continue;
        }
        let _ = writeln!(out, "  {label}:");
        for p in items {
            let _ = writeln!(
                out,
                "    - {}{}",
                p.source,
                if p.filtered { " (filtered)" } else { "" }
            );
            for kind in [
                ResourceKind::Extensions,
                ResourceKind::Skills,
                ResourceKind::Prompts,
                ResourceKind::Themes,
            ] {
                let values = p.filters(kind);
                if values.is_empty() {
                    continue;
                }
                let _ = writeln!(out, "      {}:", kind.as_str());
                for v in values {
                    let _ = writeln!(out, "        - {v}");
                }
            }
        }
    }
}

fn str_list(settings: &Value, key: &str) -> Vec<String> {
    settings
        .get(key)
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(Value::as_str)
                .map(str::to_string)
                .collect()
        })
        .unwrap_or_default()
}

fn print_resource_summary(out: &mut dyn Write, cwd: &Path, agent_home: &Path) {
    let user = read_settings_document(&agent_home.join("settings.json")).unwrap_or_default();
    let project =
        read_settings_document(&cwd.join(".pi").join("settings.json")).unwrap_or_default();
    let merged = |key: &str| {
        let mut v = str_list(&user, key);
        v.extend(str_list(&project, key));
        v
    };
    let feature = |key: &str| {
        project
            .get("features")
            .and_then(|f| f.get(key))
            .or_else(|| user.get("features").and_then(|f| f.get(key)))
            .and_then(Value::as_bool)
            .unwrap_or(true)
    };
    let _ = writeln!(out, "Resource configuration:");
    for (label, feature_key, roots, filters) in [
        (
            "prompt_templates",
            "loadPromptTemplates",
            "promptRoots",
            "prompts",
        ),
        ("skills", "loadSkills", "skillRoots", "skills"),
        ("themes", "loadThemes", "themeRoots", "themes"),
    ] {
        let _ = writeln!(out, "  {label}:");
        let _ = writeln!(out, "    enabled: {}", feature(feature_key));
        for (title, values) in [("roots", merged(roots)), ("filters", merged(filters))] {
            let _ = writeln!(out, "    {title}:");
            if values.is_empty() {
                let _ = writeln!(out, "      (none)");
            }
            for v in values {
                let _ = writeln!(out, "      - {v}");
            }
        }
    }
}

/// Instruction printed for `qt update self` (qt has no self-updater).
pub const SELF_UPDATE_INSTRUCTION: &str = "Update qt using the package manager, wrapper, or source checkout that provides this installation.";

fn run_config(
    parsed: ConfigCommand,
    cwd: &Path,
    agent_home: &Path,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> i32 {
    if parsed.help {
        let _ = write!(
            out,
            "Usage:\n  qt config\n  qt config list\n  {}\n  {}\n\nShow or update settings-backed package resource filters.\n",
            ConfigCommandKind::Enable.usage(),
            ConfigCommandKind::Disable.usage()
        );
        return 0;
    }
    if let Some(o) = &parsed.invalid_option {
        let _ = write!(
            err,
            "Unknown option {o} for \"config\".\nUse \"qt --help\" or \"qt config\".\n"
        );
        return 1;
    }
    let usage = parsed.command.usage();
    if let Some(o) = &parsed.missing_option_value {
        let _ = write!(err, "Missing value for {o}.\nUsage: {usage}\n");
        return 1;
    }
    if let Some(a) = &parsed.invalid_argument {
        let _ = write!(err, "Unexpected argument {a}.\nUsage: {usage}\n");
        return 1;
    }
    let settings = match load_package_settings(cwd, agent_home) {
        Ok(s) => s,
        Err(e) => {
            let _ = writeln!(err, "qt: config command failed: invalid_argument: {e}");
            return 1;
        }
    };
    if parsed.command == ConfigCommandKind::List {
        print_resource_summary(out, cwd, agent_home);
        print_configured_resources(out, &settings.all());
        return 0;
    }
    let missing = [
        (parsed.source.is_none(), "Missing config source."),
        (
            parsed.resource_kind.is_none(),
            "Missing config resource type.",
        ),
        (
            parsed.resource_path.is_none(),
            "Missing config resource path.",
        ),
        (parsed.package_root.is_none(), "Missing --package-root."),
    ];
    if let Some((_, message)) = missing.iter().find(|(m, _)| *m) {
        let _ = write!(err, "{message}\nUsage: {usage}\n");
        return 1;
    }
    let mut package_root = parsed.package_root.clone().unwrap_or_default();
    if package_root.is_relative() {
        package_root = cwd.join(package_root);
    }
    let mut resource_path = parsed.resource_path.clone().unwrap_or_default();
    if resource_path.is_relative() {
        resource_path = package_root.join(resource_path);
    }
    let request = ResourceToggle {
        source: parsed.source.clone().unwrap_or_default(),
        scope: parsed.scope,
        kind: parsed.resource_kind.unwrap_or(ResourceKind::Extensions),
        package_root: lexical_normal(&package_root),
        resource_path: lexical_normal(&resource_path),
        enabled: parsed.command == ConfigCommandKind::Enable,
    };
    let outcome = resource_toggle_plan(&request, &settings);
    if let Some(e) = outcome.error {
        let _ = writeln!(err, "{e}");
        return 1;
    }
    if outcome.settings_changed
        && let Err(e) = write_scope_packages(
            &settings_path(request.scope, cwd, agent_home),
            request.scope,
            &outcome.configured,
        )
    {
        let _ = writeln!(err, "Error: {e}");
        return 1;
    }
    let _ = writeln!(
        out,
        "{} {} resource {} for {}",
        if request.enabled {
            "Enabled"
        } else {
            "Disabled"
        },
        request.kind.as_str(),
        outcome.pattern,
        request.source
    );
    0
}

fn run_plans(plans: &[SourceCommandPlan], out: &mut dyn Write, err: &mut dyn Write) -> bool {
    for p in plans.iter().filter(|p| !p.skipped) {
        match execute_source_plan(p) {
            Ok(true) => {
                let _ = writeln!(out, "  [{}] {}", p.scope.as_str(), p.command_text());
            }
            Ok(false) => {}
            Err(e) => {
                let _ = writeln!(err, "Error: {e}");
                return false;
            }
        }
    }
    true
}

/// `qt install|remove|uninstall|update|list|config ...`. Returns `None` when
/// `args[0]` is not a package or config command (the caller continues with
/// normal argument parsing).
pub fn run_package_command(
    args: &[String],
    cwd: &Path,
    agent_home: &Path,
    options: PackageRunOptions,
    out: &mut dyn Write,
    err: &mut dyn Write,
) -> Option<i32> {
    if let Some(parsed) = parse_config_command(args) {
        return Some(run_config(parsed, cwd, agent_home, out, err));
    }
    let parsed = parse_package_command(args)?;
    let usage = parsed.command.usage();
    if parsed.help {
        if parsed.command == PackageCommandKind::List {
            let _ = write!(
                out,
                "Usage:\n  {usage}\n\nList installed packages from user and project settings.\n"
            );
        } else {
            let _ = writeln!(out, "Usage:\n  {usage}");
        }
        return Some(0);
    }
    if let Some(o) = &parsed.invalid_option {
        let _ = write!(
            err,
            "Unknown option {o} for \"{}\".\nUse \"qt --help\" or \"{usage}\".\n",
            parsed.command.as_str()
        );
        return Some(1);
    }
    for (value, prefix) in [
        (&parsed.missing_option_value, "Missing value for "),
        (&parsed.invalid_argument, "Unexpected argument "),
    ] {
        if let Some(v) = value {
            let _ = write!(err, "{prefix}{v}.\nUsage: {usage}\n");
            return Some(1);
        }
    }
    if let Some(c) = &parsed.conflicting_options {
        let _ = write!(err, "{c}\nUsage: {usage}\n");
        return Some(1);
    }
    if matches!(
        parsed.command,
        PackageCommandKind::Install | PackageCommandKind::Remove
    ) && parsed.source.is_none()
    {
        let _ = write!(
            err,
            "Missing {} source.\nUsage: {usage}\n",
            parsed.command.as_str()
        );
        return Some(1);
    }
    let settings = match load_package_settings(cwd, agent_home) {
        Ok(s) => s,
        Err(e) => {
            let _ = writeln!(err, "Error: {e}");
            return Some(1);
        }
    };
    let plan = package_settings_plan(&parsed, cwd, agent_home, &settings);
    if let Some(e) = &plan.error {
        let _ = writeln!(err, "{e}");
        return Some(1);
    }
    let write_settings = |err: &mut dyn Write| -> bool {
        let Some(scope) = plan.scope else {
            return true;
        };
        if !plan.settings_changed {
            return true;
        }
        match write_scope_packages(
            &settings_path(scope, cwd, agent_home),
            scope,
            &plan.configured,
        ) {
            Ok(()) => true,
            Err(e) => {
                let _ = writeln!(err, "Error: {e}");
                false
            }
        }
    };
    let pending = plan.source_plans.iter().any(|p| !p.skipped);
    match parsed.command {
        PackageCommandKind::List => {
            print_package_list(out, &plan.configured);
            Some(0)
        }
        PackageCommandKind::Install => {
            if options.execute && pending && !run_plans(&plan.source_plans, out, err) {
                return Some(1);
            }
            if !write_settings(err) {
                return Some(1);
            }
            let _ = writeln!(
                out,
                "Installed {}",
                parsed.source.as_deref().unwrap_or_default()
            );
            if !options.execute && pending {
                print_source_plans(out, &plan.source_plans, "Planned package-manager commands");
            }
            Some(0)
        }
        PackageCommandKind::Remove => {
            if !write_settings(err) {
                return Some(1);
            }
            if options.execute && pending && !run_plans(&plan.source_plans, out, err) {
                return Some(1);
            }
            let _ = writeln!(
                out,
                "Removed {}",
                parsed.source.as_deref().unwrap_or_default()
            );
            if !options.execute && pending {
                print_source_plans(out, &plan.source_plans, "Planned package-manager commands");
            }
            Some(0)
        }
        PackageCommandKind::Update => {
            let mut printed = false;
            if !plan.source_plans.is_empty() {
                if options.execute {
                    let _ = writeln!(out, "Updating packages:");
                    if !run_plans(&plan.source_plans, out, err) {
                        return Some(1);
                    }
                } else {
                    print_source_plans(out, &plan.source_plans, "Planned package updates");
                }
                printed = true;
            }
            if !plan.skipped_updates.is_empty() {
                if printed {
                    let _ = writeln!(out);
                }
                let _ = writeln!(out, "Skipped package updates:");
                for (p, _) in &plan.skipped_updates {
                    let _ = writeln!(out, "  [{}] {}", p.scope.as_str(), p.source);
                }
                printed = true;
            }
            if plan.self_update {
                if printed {
                    let _ = writeln!(out);
                }
                let _ = write!(
                    out,
                    "Self update:\n  unavailable for this installation\n  {SELF_UPDATE_INSTRUCTION}\n"
                );
                printed = true;
            }
            if !printed {
                let _ = writeln!(out, "No package updates planned.");
            }
            Some(0)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(s: &str) -> Vec<String> {
        s.split_whitespace().map(str::to_string).collect()
    }

    #[test]
    fn source_parsing() {
        let g = git_source("git:github.com/owner/repo@v1").unwrap();
        assert_eq!(
            (g.host.as_str(), g.path.as_str(), g.reference.as_deref()),
            ("github.com", "owner/repo", Some("v1"))
        );
        assert_eq!(g.repo, "github.com/owner/repo");
        let g = git_source("https://user@github.com/owner/repo.git").unwrap();
        assert_eq!(
            (g.host.as_str(), g.path.as_str()),
            ("github.com", "owner/repo")
        );
        assert_eq!(g.repo, "https://user@github.com/owner/repo.git");
        let g = git_source("git:git@github.com:owner/repo#main").unwrap();
        assert_eq!(g.repo, "git@github.com:owner/repo");
        assert_eq!(g.reference.as_deref(), Some("main"));
        assert!(git_source("github.com/owner/repo").is_none());
        assert!(git_source("https://github.com/norepo").is_none());
        assert_eq!(npm_name("@scope/pkg@1.2.3"), "@scope/pkg");
        assert_eq!(npm_name("pkg"), "pkg");
        assert_eq!(source_identity("npm:pkg@2", None), "npm:pkg");
        assert_eq!(
            source_identity("https://github.com/a/b.git", None),
            "git:github.com/a/b"
        );
        assert_eq!(source_identity("./x/", None), "local:./x");
        assert!(is_update_candidate("npm:pkg"));
        assert!(!is_update_candidate("npm:pkg@1"));
        assert_eq!(update_skip_reason("npm:pkg@1"), "pinned npm package");
        assert_eq!(update_skip_reason("git:h/a/b@x"), "pinned git ref");
        assert_eq!(
            update_skip_reason("./local"),
            "local package sources are not updated"
        );
    }

    #[test]
    fn command_parsing() {
        let p = parse_package_command(&args("install npm:x -l")).unwrap();
        assert!(p.local);
        assert_eq!(p.source.as_deref(), Some("npm:x"));
        let p = parse_package_command(&args("list -l")).unwrap();
        assert_eq!(p.invalid_option.as_deref(), Some("-l"));
        let p = parse_package_command(&args("update --extension")).unwrap();
        assert_eq!(p.missing_option_value.as_deref(), Some("--extension"));
        let p = parse_package_command(&args("update x --self")).unwrap();
        assert_eq!(
            p.conflicting_options.as_deref(),
            Some("positional update targets cannot be combined with --self or --extensions")
        );
        let p = parse_package_command(&args("update self --extensions")).unwrap();
        assert_eq!(p.update_target.unwrap().kind, UpdateTargetKind::All);
        let p = parse_package_command(&args("update --extension a b")).unwrap();
        assert_eq!(
            p.conflicting_options.as_deref(),
            Some("--extension cannot be combined with a positional source")
        );
        assert!(parse_package_command(&args("run x")).is_none());
        let c = parse_config_command(&args(
            "config disable src skills a/SKILL.md --package-root /p -l",
        ))
        .unwrap();
        assert_eq!(c.command, ConfigCommandKind::Disable);
        assert_eq!(c.scope, PackageScope::Project);
        assert_eq!(c.resource_kind, Some(ResourceKind::Skills));
        let c = parse_config_command(&args("config enable src widgets x")).unwrap();
        assert_eq!(c.invalid_argument.as_deref(), Some("widgets"));
        let c = parse_config_command(&args("config list extra")).unwrap();
        assert_eq!(c.invalid_argument.as_deref(), Some("extra"));
    }

    #[test]
    fn plans() {
        let s = PackageSettings::default();
        let cwd = Path::new("/w");
        let home = Path::new("/h");
        let p = source_command_plans(
            PackageCommandKind::Install,
            "npm:@s/p@1",
            PackageScope::Project,
            cwd,
            home,
            &s,
        );
        assert_eq!(
            p[0].command_text(),
            "npm install @s/p@1 --prefix /w/.pi/npm"
        );
        let p = source_command_plans(
            PackageCommandKind::Remove,
            "npm:@s/p@1",
            PackageScope::User,
            cwd,
            home,
            &s,
        );
        assert_eq!(p[0].command_text(), "npm uninstall -g @s/p");
        let p = source_command_plans(
            PackageCommandKind::Update,
            "npm:p",
            PackageScope::User,
            cwd,
            home,
            &s,
        );
        assert_eq!(p[0].command_text(), "npm install -g p@latest");
        let p = source_command_plans(
            PackageCommandKind::Install,
            "git:github.com/a/b@v2",
            PackageScope::User,
            cwd,
            home,
            &s,
        );
        let texts: Vec<_> = p.iter().map(SourceCommandPlan::command_text).collect();
        assert_eq!(
            texts,
            [
                "git clone github.com/a/b /h/git/github.com/a/b",
                "git checkout v2",
                "npm install"
            ]
        );
        assert!(p[2].conditional);
        let p = source_command_plans(
            PackageCommandKind::Update,
            "https://github.com/a/b",
            PackageScope::Project,
            cwd,
            home,
            &s,
        );
        assert_eq!(p[0].command_text(), "git fetch --prune origin");
        let p = source_command_plans(
            PackageCommandKind::Install,
            "./local",
            PackageScope::User,
            cwd,
            home,
            &s,
        );
        assert_eq!(
            p[0].reason.as_deref(),
            Some("local package sources do not require package-manager commands")
        );
        let bad = PackageSettings {
            npm_command: Some(vec![String::new()]),
            ..Default::default()
        };
        let p = source_command_plans(
            PackageCommandKind::Install,
            "npm:x",
            PackageScope::User,
            cwd,
            home,
            &bad,
        );
        assert_eq!(p[0].reason.as_deref(), Some("npmCommand is invalid"));
    }

    fn run(a: &str, cwd: &Path, home: &Path, execute: bool) -> (i32, String, String) {
        let (mut out, mut err) = (Vec::new(), Vec::new());
        let code = run_package_command(
            &args(a),
            cwd,
            home,
            PackageRunOptions { execute },
            &mut out,
            &mut err,
        )
        .unwrap();
        (
            code,
            String::from_utf8(out).unwrap(),
            String::from_utf8(err).unwrap(),
        )
    }

    #[test]
    fn cli_install_list_remove_and_config() {
        let dir = tempfile::tempdir().unwrap();
        let cwd = dir.path().join("proj");
        let home = dir.path().join("home");
        std::fs::create_dir_all(cwd.join(".pi")).unwrap();
        std::fs::create_dir_all(home.join("pkgs/tool/skills/review")).unwrap();
        std::fs::write(home.join("settings.json"), "{\n  \"theme\": \"dark\"\n}\n").unwrap();

        assert_eq!(
            run("list", &cwd, &home, true),
            (0, "No packages installed.\n".into(), String::new())
        );
        let (code, out, _) = run("install pkgs/tool", &cwd, &home, true);
        assert_eq!((code, out.as_str()), (0, "Installed pkgs/tool\n"));
        let written = std::fs::read_to_string(home.join("settings.json")).unwrap();
        assert_eq!(
            written,
            "{\n  \"theme\": \"dark\",\n  \"packages\": [\n    \"pkgs/tool\"\n  ]\n}\n"
        );
        // Installing the same source again changes nothing.
        assert_eq!(run("install pkgs/tool/", &cwd, &home, true).0, 0);
        let (code, out, _) = run("install npm:demo@1 -l", &cwd, &home, false);
        assert_eq!(code, 0);
        assert_eq!(
            out,
            format!(
                "Installed npm:demo@1\nPlanned package-manager commands:\n  [project] npm:demo@1\n    npm install demo@1 --prefix {}\n",
                display_path(&cwd.join(".pi/npm"))
            )
        );
        let (_, out, _) = run("list", &cwd, &home, true);
        let tool = display_path(&std::fs::canonicalize(home.join("pkgs/tool")).unwrap());
        assert_eq!(
            out,
            format!("User packages:\n  pkgs/tool\n    {tool}\n\nProject packages:\n  npm:demo@1\n")
        );

        let (code, out, err) = run(
            &format!(
                "config disable pkgs/tool skills skills/review/SKILL.md --package-root {}",
                home.join("pkgs/tool").display()
            ),
            &cwd,
            &home,
            true,
        );
        assert_eq!((code, err.as_str()), (0, ""));
        assert_eq!(
            out,
            "Disabled skills resource skills/review/SKILL.md for pkgs/tool\n"
        );
        let s = load_package_settings(&cwd, &home).unwrap();
        assert_eq!(s.user[0].skills, ["-skills/review/SKILL.md"]);
        assert!(s.user[0].filtered);
        let (_, out, _) = run(
            &format!(
                "config enable pkgs/tool skills skills/review/SKILL.md --package-root {}",
                home.join("pkgs/tool").display()
            ),
            &cwd,
            &home,
            true,
        );
        assert_eq!(
            out,
            "Enabled skills resource skills/review/SKILL.md for pkgs/tool\n"
        );
        assert_eq!(
            load_package_settings(&cwd, &home).unwrap().user[0].skills,
            ["+skills/review/SKILL.md"]
        );
        let (_, out, _) = run("config", &cwd, &home, true);
        assert!(
            out.starts_with("Resource configuration:\n  prompt_templates:\n    enabled: true\n")
        );
        assert!(out.contains("Configured packages:\n  user:\n    - pkgs/tool (filtered)\n      skills:\n        - +skills/review/SKILL.md\n"));

        let (code, _, err) = run(
            "config enable other skills x --package-root /p",
            &cwd,
            &home,
            true,
        );
        assert_eq!(
            (code, err.as_str()),
            (1, "No matching package found for other\n")
        );
        let (code, _, err) = run("remove nothing", &cwd, &home, true);
        assert_eq!(
            (code, err.as_str()),
            (1, "No matching package found for nothing\n")
        );
        let (code, out, _) = run("remove pkgs/tool", &cwd, &home, true);
        assert_eq!((code, out.as_str()), (0, "Removed pkgs/tool\n"));
        assert!(load_package_settings(&cwd, &home).unwrap().user.is_empty());
    }

    #[test]
    fn cli_errors_and_update() {
        let dir = tempfile::tempdir().unwrap();
        let (cwd, home) = (dir.path().join("c"), dir.path().join("h"));
        assert_eq!(
            run("install", &cwd, &home, true).2,
            "Missing install source.\nUsage: qt install <source> [-l]\n"
        );
        assert_eq!(
            run("install a --bogus", &cwd, &home, true).2,
            "Unknown option --bogus for \"install\".\nUse \"qt --help\" or \"qt install <source> [-l]\".\n"
        );
        assert_eq!(
            run("remove a b", &cwd, &home, true).2,
            "Unexpected argument b.\nUsage: qt remove <source> [-l]\n"
        );
        assert_eq!(
            run("update", &cwd, &home, true).1,
            format!(
                "Self update:\n  unavailable for this installation\n  {SELF_UPDATE_INSTRUCTION}\n"
            )
        );
        assert_eq!(
            run("update --extensions", &cwd, &home, true).1,
            "No package updates planned.\n"
        );
        std::fs::create_dir_all(&home).unwrap();
        std::fs::write(
            home.join("settings.json"),
            r#"{"packages":["npm:x@1","./loc","npm:tool"]}"#,
        )
        .unwrap();
        let (code, _, err) = run("update tool", &cwd, &home, false);
        assert_eq!(
            (code, err.as_str()),
            (
                1,
                "No matching package found for tool. Did you mean npm:tool?\n"
            )
        );
        let (_, out, _) = run("update --extensions", &cwd, &home, false);
        assert_eq!(
            out,
            "Planned package updates:\n  [user] npm:tool\n    npm install -g tool@latest\n\nSkipped package updates:\n  [user] npm:x@1\n  [user] ./loc\n"
        );
        std::fs::write(home.join("settings.json"), r#"{"packages":5}"#).unwrap();
        assert_eq!(
            run("list", &cwd, &home, true).2,
            "Error: packages must be an array\n"
        );
        assert!(
            run_package_command(
                &args("chat"),
                &cwd,
                &home,
                PackageRunOptions::default(),
                &mut Vec::new(),
                &mut Vec::new()
            )
            .is_none()
        );
    }

    #[test]
    fn executes_git_install_from_local_repository() {
        if Command::new("git").arg("--version").output().is_err() {
            return;
        }
        let dir = tempfile::tempdir().unwrap();
        let repo = dir.path().join("src/owner/demo");
        std::fs::create_dir_all(&repo).unwrap();
        std::fs::write(repo.join("index.rhai"), "fn activate() {}").unwrap();
        let git = |a: &[&str]| {
            let ok = Command::new("git")
                .args(a)
                .current_dir(&repo)
                .env("GIT_AUTHOR_NAME", "t")
                .env("GIT_AUTHOR_EMAIL", "t@t")
                .env("GIT_COMMITTER_NAME", "t")
                .env("GIT_COMMITTER_EMAIL", "t@t")
                .output()
                .unwrap()
                .status
                .success();
            assert!(ok, "git {a:?}");
        };
        git(&["init", "-q"]);
        git(&["add", "."]);
        git(&["commit", "-q", "-m", "init"]);
        let home = dir.path().join("h");
        let source = "git:local/owner/demo".to_string();
        let target = home.join("git/local/owner/demo");
        let plan = SourceCommandPlan {
            scope: PackageScope::User,
            source: source.clone(),
            source_type: "git".into(),
            argv: vec![
                "git".into(),
                "clone".into(),
                format!("file://{}", repo.display()),
                display_path(&target),
            ],
            install_root: Some(home.join("git")),
            install_path: Some(target.clone()),
            ..Default::default()
        };
        assert!(execute_source_plan(&plan).unwrap());
        assert!(target.join("index.rhai").is_file());
        assert_eq!(
            std::fs::read_to_string(home.join("git/.gitignore")).unwrap(),
            CACHE_GITIGNORE
        );
        let deps = SourceCommandPlan {
            argv: vec!["false".into()],
            working_directory: Some(target.clone()),
            conditional: true,
            ..plan.clone()
        };
        assert!(!execute_source_plan(&deps).unwrap());
        let remove = SourceCommandPlan {
            argv: vec!["rm".into(), "-rf".into(), display_path(&target)],
            ..plan
        };
        assert!(execute_source_plan(&remove).unwrap());
        assert!(!target.exists());
        assert!(!home.join("git/local").exists());
        assert!(home.join("git").exists());
    }
}
