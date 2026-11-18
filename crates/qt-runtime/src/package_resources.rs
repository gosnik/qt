//! Package resources: the `/extensions`, `/skills`, `/prompts` and
//! `/themes` selectors (archived `load_package_resource_items` and the
//! resource toggle in `interactive_mode.cpp`) and the loading of package
//! skills, prompt templates and themes.
//!
//! Installed `packages` entries (project first, then user; the first entry
//! of a package identity wins) contribute their resources. A package written
//! as a bare source enables everything; the object form filters each kind
//! with the archived `+path` / `-path` / glob rules, and an object without a
//! list for a kind disables that kind (archived `filtered && empty`).

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use qt_core::harness::hooks::DiscoveredResourcePaths;
use qt_core::resources::{skills, templates, themes};
use qt_extensions::filters::{display_path, lexical_normal, resource_enabled_by_filters};
use qt_extensions::loader::load_extension_roots;
use qt_extensions::packages::{
    ConfiguredPackage, PackageScope, PackageSettings, ResourceKind, ResourceToggle,
    load_package_settings, resource_pattern, resource_toggle_plan, settings_path, source_identity,
    write_scope_packages,
};
use qt_extensions::{ExtensionOrigin, SandboxPolicy};

/// Pattern written for kinds that were implicitly "all" when a bare
/// package entry becomes an object (keeps them enabled).
pub const ALL_RESOURCES_PATTERN: &str = "**";

/// The package an item belongs to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackageRef {
    pub source: String,
    pub scope: PackageScope,
    pub root: PathBuf,
}

/// One row of a package resource selector.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackageResourceItem {
    /// The normalised resource path (unique within a listing).
    pub id: String,
    pub kind: ResourceKind,
    pub label: String,
    pub description: String,
    pub resource_path: PathBuf,
    /// `None` for extensions loaded outside package settings.
    pub package: Option<PackageRef>,
    pub enabled: bool,
    pub can_toggle: bool,
}

impl PackageResourceItem {
    /// The archived value column: `on`, `off` or `loaded` (read-only).
    pub fn state_label(&self) -> &'static str {
        match (self.can_toggle, self.enabled) {
            (false, _) => "loaded",
            (true, true) => "on",
            (true, false) => "off",
        }
    }
}

/// A resource found under a package root: `(path, name, description)`.
pub type FoundResource = (PathBuf, String, String);

fn normalize(path: &Path) -> String {
    display_path(&lexical_normal(path))
}

fn identity(path: &Path) -> String {
    normalize(&std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf()))
}

/// Every resource of `kind` under `root`, unfiltered.
pub fn resources_in(root: &Path, kind: ResourceKind) -> Vec<FoundResource> {
    let roots = [root.to_path_buf()];
    match kind {
        ResourceKind::Skills => skills::load_skills(&roots)
            .skills
            .into_iter()
            .map(|s| (s.path, s.name, s.description))
            .collect(),
        ResourceKind::Prompts => templates::load_prompt_templates(&roots)
            .templates
            .into_iter()
            .map(|t| (t.path, t.name, t.description))
            .collect(),
        ResourceKind::Themes => themes::load_themes(&roots)
            .themes
            .into_iter()
            .map(|t| (t.path, t.theme.name, String::new()))
            .collect(),
        ResourceKind::Extensions => {
            let origin = ExtensionOrigin::Package {
                source: String::new(),
                scope: PackageScope::User,
            };
            load_extension_roots(
                &[root.to_string_lossy().into_owned()],
                &[],
                &origin,
                SandboxPolicy::default(),
            )
            .extensions
            .into_iter()
            .map(|e| {
                let name = if e.manifest.name.is_empty() {
                    e.directory
                        .file_name()
                        .map(|n| n.to_string_lossy().into_owned())
                        .unwrap_or_default()
                } else {
                    e.manifest.name.clone()
                };
                (
                    e.path,
                    name,
                    e.manifest.display_name.clone().unwrap_or_default(),
                )
            })
            .collect()
        }
    }
}

/// Whether a package loads resources of `kind` at all.
pub fn kind_enabled(package: &ConfiguredPackage, kind: ResourceKind) -> bool {
    !(package.filtered && package.filters(kind).is_empty())
}

/// Whether `resource` of `package` passes the package filters.
pub fn resource_enabled(
    package: &ConfiguredPackage,
    kind: ResourceKind,
    root: &Path,
    resource: &Path,
) -> bool {
    kind_enabled(package, kind)
        && resource_enabled_by_filters(
            package.filters(kind),
            &resource_pattern(root, resource),
            resource,
        )
}

/// Installed packages in precedence order (project, then user), the first
/// entry of each package identity only.
pub fn effective_packages(settings: &PackageSettings) -> Vec<ConfiguredPackage> {
    let mut seen = HashSet::new();
    settings
        .project
        .iter()
        .chain(settings.user.iter())
        .filter(|p| p.installed_path.is_some())
        .filter(|p| seen.insert(source_identity(&p.source, None)))
        .cloned()
        .collect()
}

/// The enabled package resources of `kind`.
pub fn enabled_package_resources(settings: &PackageSettings, kind: ResourceKind) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for package in effective_packages(settings) {
        let Some(root) = package.installed_path.as_deref().map(PathBuf::from) else {
            continue;
        };
        if !kind_enabled(&package, kind) {
            continue;
        }
        for (path, _, _) in resources_in(&root, kind) {
            if resource_enabled(&package, kind, &root, &path) && !out.contains(&path) {
                out.push(path);
            }
        }
    }
    out
}

/// What the switches allow: skills, prompts, themes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ResourceSwitches {
    pub skills: bool,
    pub prompts: bool,
    pub themes: bool,
}

/// Skill, prompt and theme paths contributed by installed packages (the
/// `resources_discover` contribution of the runtime).
pub fn package_resource_contributions(
    workspace_root: &Path,
    agent_home: &Path,
    switches: ResourceSwitches,
) -> Result<DiscoveredResourcePaths, String> {
    let settings = load_package_settings(workspace_root, agent_home)?;
    let strings = |kind: ResourceKind, on: bool| -> Vec<String> {
        if !on {
            return Vec::new();
        }
        enabled_package_resources(&settings, kind)
            .into_iter()
            .map(|p| p.to_string_lossy().into_owned())
            .collect()
    };
    Ok(DiscoveredResourcePaths {
        skill_paths: strings(ResourceKind::Skills, switches.skills),
        prompt_paths: strings(ResourceKind::Prompts, switches.prompts),
        theme_paths: strings(ResourceKind::Themes, switches.themes),
    })
}

/// Inputs of [`package_resource_items`].
#[derive(Debug, Clone)]
pub struct ResourceListing<'a> {
    pub kind: ResourceKind,
    /// Resources of `kind` that are loaded now (enabled state).
    pub loaded: &'a [PathBuf],
    /// Extensions loaded outside package settings (`-e`, `.pi/extensions`,
    /// `<agent home>/extensions`) as `(path, label)`: read-only rows.
    pub external_extensions: &'a [(PathBuf, String)],
}

/// The selector rows for `listing.kind` (archived ordering: label, source,
/// path).
pub fn package_resource_items(
    settings: &PackageSettings,
    listing: &ResourceListing<'_>,
) -> Vec<PackageResourceItem> {
    let kind = listing.kind;
    let loaded: HashSet<String> = listing.loaded.iter().map(|p| identity(p)).collect();
    let external: HashSet<String> = listing
        .external_extensions
        .iter()
        .map(|(p, _)| identity(p))
        .collect();
    let mut seen = HashSet::new();
    let mut items = Vec::new();
    for package in settings.project.iter().chain(settings.user.iter()) {
        let Some(root) = package.installed_path.as_deref().map(PathBuf::from) else {
            continue;
        };
        let root_text = normalize(&root);
        for (path, name, description) in resources_in(&root, kind) {
            let key = identity(&path);
            if !seen.insert(key.clone()) {
                continue;
            }
            let shown = normalize(&path);
            let externally_loaded = kind == ResourceKind::Extensions && external.contains(&key);
            let detail = if kind == ResourceKind::Prompts && !description.is_empty() {
                description
            } else {
                shown.clone()
            };
            let mut text = format!(
                "{} package {} - {detail}",
                package.scope.as_str(),
                package.source
            );
            if externally_loaded {
                text.push_str(" (read-only; loaded outside package settings)");
            }
            items.push(PackageResourceItem {
                id: shown.clone(),
                kind,
                label: name,
                description: text,
                resource_path: path,
                package: Some(PackageRef {
                    source: package.source.clone(),
                    scope: package.scope,
                    root: PathBuf::from(&root_text),
                }),
                enabled: loaded.contains(&key),
                can_toggle: !externally_loaded,
            });
        }
    }
    if kind == ResourceKind::Extensions {
        for (path, label) in listing.external_extensions {
            if !seen.insert(identity(path)) {
                continue;
            }
            let shown = normalize(path);
            items.push(PackageResourceItem {
                id: shown.clone(),
                kind,
                label: label.clone(),
                description: format!(
                    "Loaded extension - {shown} (read-only; managed outside package settings)"
                ),
                resource_path: path.clone(),
                package: None,
                enabled: true,
                can_toggle: false,
            });
        }
    }
    items.sort_by(|a, b| {
        let source = |i: &PackageResourceItem| {
            i.package
                .as_ref()
                .map(|p| p.source.clone())
                .unwrap_or_default()
        };
        a.label
            .cmp(&b.label)
            .then_with(|| source(a).cmp(&source(b)))
            .then_with(|| a.resource_path.cmp(&b.resource_path))
    });
    items
}

/// Rows matching `filter` (case-insensitive substring of label,
/// description or path).
pub fn filter_items(items: &[PackageResourceItem], filter: &str) -> Vec<PackageResourceItem> {
    let filter = filter.trim().to_lowercase();
    items
        .iter()
        .filter(|item| {
            filter.is_empty()
                || item.label.to_lowercase().contains(&filter)
                || item.description.to_lowercase().contains(&filter)
                || item.id.to_lowercase().contains(&filter)
        })
        .cloned()
        .collect()
}

/// The archived "nothing matched" line.
pub fn empty_listing_line(kind: ResourceKind) -> &'static str {
    if kind == ResourceKind::Extensions {
        "No extensions matched."
    } else {
        "No package-managed resources matched."
    }
}

/// Enable or disable `item` in its package's settings file. Returns the
/// archived confirmation (`Enabled skills review.`).
pub fn toggle_package_resource(
    workspace_root: &Path,
    agent_home: &Path,
    item: &PackageResourceItem,
    enabled: bool,
) -> Result<String, String> {
    let package = item
        .package
        .as_ref()
        .filter(|_| item.can_toggle)
        .ok_or_else(|| format!("{} is managed outside package settings", item.label))?;
    let settings = load_package_settings(workspace_root, agent_home)?;
    let request = ResourceToggle {
        source: package.source.clone(),
        scope: package.scope,
        kind: item.kind,
        package_root: package.root.clone(),
        resource_path: item.resource_path.clone(),
        enabled,
    };
    let mut outcome = resource_toggle_plan(&request, &settings);
    if let Some(error) = outcome.error {
        return Err(error);
    }
    if outcome.settings_changed {
        // A bare entry turned into an object: keep the other kinds loading
        // everything instead of disabling them.
        let id = source_identity(&package.source, None);
        let was_bare = settings.all().iter().any(|c| {
            c.scope == package.scope && source_identity(&c.source, None) == id && !c.filtered
        });
        if was_bare {
            for c in &mut outcome.configured {
                if c.scope == package.scope && source_identity(&c.source, None) == id {
                    for kind in [
                        ResourceKind::Extensions,
                        ResourceKind::Skills,
                        ResourceKind::Prompts,
                        ResourceKind::Themes,
                    ] {
                        if kind != item.kind && c.filters(kind).is_empty() {
                            set_filters(c, kind, vec![ALL_RESOURCES_PATTERN.to_string()]);
                        }
                    }
                }
            }
        }
        write_scope_packages(
            &settings_path(package.scope, workspace_root, agent_home),
            package.scope,
            &outcome.configured,
        )?;
    }
    Ok(format!(
        "{} {} {}.",
        if enabled { "Enabled" } else { "Disabled" },
        item.kind.as_str(),
        item.label
    ))
}

fn set_filters(package: &mut ConfiguredPackage, kind: ResourceKind, filters: Vec<String>) {
    match kind {
        ResourceKind::Extensions => package.extensions = filters,
        ResourceKind::Skills => package.skills = filters,
        ResourceKind::Prompts => package.prompts = filters,
        ResourceKind::Themes => package.themes = filters,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    struct Fixture {
        _dir: tempfile::TempDir,
        home: PathBuf,
        work: PathBuf,
        pkg: PathBuf,
    }

    fn fixture(entry: serde_json::Value) -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let home = dir.path().join("home");
        let work = dir.path().join("work");
        let pkg = dir.path().join("pkg");
        std::fs::create_dir_all(&home).unwrap();
        std::fs::create_dir_all(&work).unwrap();
        std::fs::create_dir_all(pkg.join("skills/review")).unwrap();
        std::fs::create_dir_all(pkg.join("skills/lint")).unwrap();
        std::fs::create_dir_all(pkg.join("prompts")).unwrap();
        std::fs::write(
            pkg.join("skills/review/SKILL.md"),
            "---\nname: review\ndescription: Review code\n---\nbody",
        )
        .unwrap();
        std::fs::write(
            pkg.join("skills/lint/SKILL.md"),
            "---\nname: lint\ndescription: Lint code\n---\nbody",
        )
        .unwrap();
        std::fs::write(
            pkg.join("prompts/fix.md"),
            "---\ndescription: Fix it\n---\nFix $1",
        )
        .unwrap();
        std::fs::write(
            pkg.join("package.json"),
            json!({"name": "pkg", "pi": {"skills": ["skills"], "prompts": ["prompts"]}})
                .to_string(),
        )
        .unwrap();
        let mut entry = entry;
        if let Some(o) = entry.as_object_mut() {
            o.insert("source".into(), json!(pkg.to_string_lossy()));
        } else {
            entry = json!(pkg.to_string_lossy());
        }
        std::fs::write(
            home.join("settings.json"),
            json!({"packages": [entry]}).to_string(),
        )
        .unwrap();
        Fixture {
            _dir: dir,
            home,
            work,
            pkg,
        }
    }

    fn names(paths: &[PathBuf]) -> Vec<String> {
        let mut out: Vec<String> = paths
            .iter()
            .map(|p| {
                p.parent()
                    .and_then(|d| d.file_name())
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_default()
            })
            .collect();
        out.sort();
        out
    }

    #[test]
    fn bare_packages_contribute_everything_and_toggles_persist() {
        let f = fixture(json!(null));
        let switches = ResourceSwitches {
            skills: true,
            prompts: true,
            themes: true,
        };
        let paths = package_resource_contributions(&f.work, &f.home, switches).unwrap();
        assert_eq!(paths.skill_paths.len(), 2);
        assert_eq!(paths.prompt_paths.len(), 1);
        assert!(paths.theme_paths.is_empty());
        let off = package_resource_contributions(
            &f.work,
            &f.home,
            ResourceSwitches {
                skills: false,
                ..switches
            },
        )
        .unwrap();
        assert!(off.skill_paths.is_empty());

        let settings = load_package_settings(&f.work, &f.home).unwrap();
        let loaded = enabled_package_resources(&settings, ResourceKind::Skills);
        assert_eq!(names(&loaded), ["lint", "review"]);
        let items = package_resource_items(
            &settings,
            &ResourceListing {
                kind: ResourceKind::Skills,
                loaded: &loaded,
                external_extensions: &[],
            },
        );
        assert_eq!(
            items.iter().map(|i| i.label.as_str()).collect::<Vec<_>>(),
            ["lint", "review"]
        );
        assert!(items.iter().all(|i| i.enabled && i.can_toggle));
        assert_eq!(items[0].state_label(), "on");
        assert!(items[1].description.starts_with("user package "));
        assert_eq!(filter_items(&items, "REV").len(), 1);

        let message = toggle_package_resource(&f.work, &f.home, &items[1], false).unwrap();
        assert_eq!(message, "Disabled skills review.");
        let settings = load_package_settings(&f.work, &f.home).unwrap();
        let entry = &settings.user[0];
        assert!(entry.filtered);
        assert_eq!(entry.skills, ["-skills/review/SKILL.md"]);
        // Other kinds stay fully enabled.
        assert_eq!(entry.prompts, [ALL_RESOURCES_PATTERN]);
        assert_eq!(entry.extensions, [ALL_RESOURCES_PATTERN]);
        assert_eq!(
            names(&enabled_package_resources(&settings, ResourceKind::Skills)),
            ["lint"]
        );
        assert_eq!(
            enabled_package_resources(&settings, ResourceKind::Prompts).len(),
            1
        );
        // Enabling again replaces the exclusion.
        let mut item = items[1].clone();
        item.enabled = false;
        assert_eq!(
            toggle_package_resource(&f.work, &f.home, &item, true).unwrap(),
            "Enabled skills review."
        );
        let settings = load_package_settings(&f.work, &f.home).unwrap();
        assert_eq!(settings.user[0].skills, ["+skills/review/SKILL.md"]);
        assert_eq!(
            enabled_package_resources(&settings, ResourceKind::Skills).len(),
            2
        );
        let _ = f.pkg;
    }

    #[test]
    fn object_entries_without_a_kind_disable_it_and_externals_are_read_only() {
        let f = fixture(json!({"prompts": ["prompts/*.md"]}));
        let settings = load_package_settings(&f.work, &f.home).unwrap();
        assert!(enabled_package_resources(&settings, ResourceKind::Skills).is_empty());
        assert_eq!(
            enabled_package_resources(&settings, ResourceKind::Prompts).len(),
            1
        );
        let external = [(f.work.join("ext.rhai"), "local".to_string())];
        let items = package_resource_items(
            &settings,
            &ResourceListing {
                kind: ResourceKind::Extensions,
                loaded: &[],
                external_extensions: &external,
            },
        );
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].state_label(), "loaded");
        assert!(
            items[0]
                .description
                .ends_with("(read-only; managed outside package settings)")
        );
        assert!(toggle_package_resource(&f.work, &f.home, &items[0], false).is_err());
        assert_eq!(
            empty_listing_line(ResourceKind::Extensions),
            "No extensions matched."
        );
        assert_eq!(
            empty_listing_line(ResourceKind::Themes),
            "No package-managed resources matched."
        );
    }
}
