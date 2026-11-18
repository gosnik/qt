//! Prompt resources: skills, prompt templates, system prompt files and
//! themes, discovered and loaded into one [`ResourceSnapshot`].
//!
//! Discovery follows the archived qt (`harness/context_discovery.cpp`,
//! `app/config/settings.cpp`, `app/export/resource_inspection.cpp`):
//!
//! - **Repository root:** the `workspace_root` hint when it contains `cwd`,
//!   else the nearest ancestor of `cwd` with `.git` or a `.pi/` directory.
//! - **Skill roots:** `<agent_home>/skills`, `~/.agents/skills` (only when the
//!   agent home is `~/.pi/agent`), `<repo>/.pi/skills`, then
//!   `.agents/skills` in every directory from the repository root (or the
//!   filesystem root) down to `cwd`.
//! - **Prompt roots:** `<agent_home>/prompts`, `<repo>/.pi/prompts`, then
//!   `.agents/prompts` from the repository root down to `cwd`.
//! - **Theme roots:** `<agent_home>/themes`, `<repo>/.pi/themes`,
//!   `<repo>/.agents/themes`.
//!
//! Only existing directories become default roots. Settings `skillRoots`
//! (etc.) replace the defaults; CLI paths are prepended (skills) or appended
//! (prompts, themes). Settings filters and `features.load*` switches apply
//! last.

pub mod filters;
pub mod fsutil;
pub mod settings;
pub mod skills;
pub mod system_files;
pub mod templates;
pub mod theme_watcher;
pub mod themes;

use std::collections::HashSet;
use std::path::{Path, PathBuf};

use crate::config::AgentHome;

pub use settings::ResourceSettings;
pub use skills::{Skill, SkillResources};
pub use system_files::{PromptContribution, PromptContributions};
pub use templates::{PromptTemplate, PromptTemplateResources};
pub use theme_watcher::ThemeWatcher;
pub use themes::{Theme, ThemeColor, ThemePalette, ThemeResources, ThemeToken};

use fsutil::{canonical_or_normal, normalize, path_string, resolve_user_path};

/// Which resource a diagnostic concerns.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ResourceKind {
    Discovery,
    Settings,
    SystemPrompt,
    PromptTemplate,
    Skill,
    Theme,
}

/// A non-fatal problem found while loading resources.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResourceDiagnostic {
    pub kind: ResourceKind,
    pub message: String,
}

/// Inputs to [`load_resources`] beyond the agent home.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ResourceOptions {
    /// Working directory of the session.
    pub cwd: PathBuf,
    /// `workspaceRoot` hint; must contain `cwd` to be used.
    pub workspace_root: Option<PathBuf>,
    /// `--skill` paths (resolved against `cwd`), searched before other roots.
    pub skill_paths: Vec<String>,
    /// `--prompt-template` paths, searched after other roots.
    pub prompt_template_paths: Vec<String>,
    /// `--theme` paths, searched after other roots.
    pub theme_paths: Vec<String>,
    /// `--no-skills`: only `skill_paths` are loaded.
    pub no_skills: bool,
    /// `--no-prompt-templates`.
    pub no_prompt_templates: bool,
    /// `--no-themes`.
    pub no_themes: bool,
    /// `--system-prompt` (text or a file path).
    pub system_prompt: Option<String>,
    /// `--append-system-prompt` values (text or file paths).
    pub append_system_prompt: Vec<String>,
}

impl ResourceOptions {
    pub fn new(cwd: impl Into<PathBuf>) -> Self {
        Self {
            cwd: cwd.into(),
            ..Default::default()
        }
    }
}

/// Discovered locations before settings are applied.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ResourceDiscovery {
    pub cwd: PathBuf,
    pub repository_root: Option<PathBuf>,
    pub skill_roots: Vec<PathBuf>,
    pub prompt_roots: Vec<PathBuf>,
    pub theme_roots: Vec<PathBuf>,
    pub diagnostics: Vec<String>,
}

/// Nearest ancestor (inclusive) with `.git` or a `.pi/` directory.
pub fn find_repository_root(start: &Path) -> Option<PathBuf> {
    let mut current = canonical_or_normal(start);
    if current.is_file() {
        current = current.parent()?.to_path_buf();
    }
    for dir in current.ancestors() {
        if dir.join(".git").exists() || dir.join(".pi").is_dir() {
            return Some(dir.to_path_buf());
        }
    }
    None
}

fn user_agents_skill_root(agent_home: &Path) -> Option<PathBuf> {
    let pi = agent_home.parent()?;
    if fsutil::file_name(agent_home) != "agent" || fsutil::file_name(pi) != ".pi" {
        return None;
    }
    let user_home = pi.parent()?;
    Some(user_home.join(".agents").join("skills"))
}

/// Directories from `stop_at` (or the filesystem root) down to `cwd`.
fn walk_down(cwd: &Path, stop_at: Option<&Path>) -> Vec<PathBuf> {
    let mut dirs = Vec::new();
    for dir in cwd.ancestors() {
        dirs.push(dir.to_path_buf());
        if Some(dir) == stop_at {
            break;
        }
    }
    dirs.reverse();
    dirs
}

fn push_dir(list: &mut Vec<PathBuf>, path: PathBuf) {
    if path.is_dir() && !list.contains(&path) {
        list.push(path);
    }
}

/// Default resource roots for `cwd`.
pub fn discover_resource_roots(
    agent_home: &Path,
    cwd: &Path,
    workspace_hint: Option<&Path>,
) -> ResourceDiscovery {
    let mut out = ResourceDiscovery::default();
    out.cwd = match std::fs::canonicalize(cwd) {
        Ok(c) => c,
        Err(_) => {
            out.diagnostics
                .push(format!("cwd could not be canonicalized: {}", cwd.display()));
            normalize(cwd)
        }
    };
    out.repository_root = match workspace_hint.filter(|h| !h.as_os_str().is_empty()) {
        Some(hint) => match std::fs::canonicalize(hint) {
            Ok(hint) if out.cwd.starts_with(&hint) => Some(hint),
            Ok(hint) => {
                out.diagnostics.push(format!(
                    "repository_root_hint does not contain cwd: {}",
                    hint.display()
                ));
                find_repository_root(&out.cwd)
            }
            Err(_) => {
                out.diagnostics.push(format!(
                    "repository_root_hint could not be canonicalized: {}",
                    hint.display()
                ));
                find_repository_root(&out.cwd)
            }
        },
        None => find_repository_root(&out.cwd),
    };
    let home = std::fs::canonicalize(agent_home).ok();
    let resource_dirs = walk_down(&out.cwd, out.repository_root.as_deref());

    if let Some(home) = &home {
        push_dir(&mut out.skill_roots, home.join("skills"));
        if let Some(root) = user_agents_skill_root(home) {
            push_dir(&mut out.skill_roots, root);
        }
        push_dir(&mut out.prompt_roots, home.join("prompts"));
        push_dir(&mut out.theme_roots, home.join("themes"));
    }
    if let Some(repo) = &out.repository_root {
        push_dir(&mut out.skill_roots, repo.join(".pi").join("skills"));
        push_dir(&mut out.prompt_roots, repo.join(".pi").join("prompts"));
        push_dir(&mut out.theme_roots, repo.join(".pi").join("themes"));
        push_dir(&mut out.theme_roots, repo.join(".agents").join("themes"));
    }
    for dir in &resource_dirs {
        push_dir(&mut out.skill_roots, dir.join(".agents").join("skills"));
    }
    for dir in &resource_dirs {
        push_dir(&mut out.prompt_roots, dir.join(".agents").join("prompts"));
    }
    out
}

/// A slash command contributed by a prompt template or skill.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResourceCommand {
    /// Name without the leading `/` (`review`, `skill:pdf`).
    pub name: String,
    pub description: String,
    pub argument_hint: Option<String>,
    pub source: ResourceCommandSource,
    pub source_path: PathBuf,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResourceCommandSource {
    Prompt,
    Skill,
}

/// Everything loaded for a session, with diagnostics.
#[derive(Debug, Clone, Default)]
pub struct ResourceSnapshot {
    pub cwd: PathBuf,
    pub repository_root: Option<PathBuf>,
    /// `workspace_root` hint, else the repository root, else `cwd`.
    pub workspace_root: PathBuf,
    pub agent_home: PathBuf,
    pub settings: ResourceSettings,
    pub skills: SkillResources,
    pub prompt_templates: PromptTemplateResources,
    pub themes: ThemeResources,
    pub contributions: PromptContributions,
    /// `enableSkillCommands` (default true).
    pub enable_skill_commands: bool,
    pub diagnostics: Vec<ResourceDiagnostic>,
}

fn add_diagnostics(
    out: &mut Vec<ResourceDiagnostic>,
    kind: ResourceKind,
    messages: impl IntoIterator<Item = String>,
) {
    out.extend(
        messages
            .into_iter()
            .filter(|m| !m.is_empty())
            .map(|message| ResourceDiagnostic { kind, message }),
    );
}

fn append_unique(list: &mut Vec<PathBuf>, additions: impl IntoIterator<Item = PathBuf>) {
    let mut seen: HashSet<PathBuf> = list.iter().cloned().collect();
    for path in additions {
        if !path.as_os_str().is_empty() && seen.insert(path.clone()) {
            list.push(path);
        }
    }
}

/// Discover and load all prompt resources for a session.
pub fn load_resources(home: &AgentHome, options: &ResourceOptions) -> ResourceSnapshot {
    let mut diagnostics = Vec::new();
    let discovery =
        discover_resource_roots(home.root(), &options.cwd, options.workspace_root.as_deref());
    add_diagnostics(
        &mut diagnostics,
        ResourceKind::Discovery,
        discovery.diagnostics.clone(),
    );

    let workspace_root = options
        .workspace_root
        .as_ref()
        .filter(|p| !p.as_os_str().is_empty())
        .map(|p| canonical_or_normal(p))
        .or_else(|| discovery.repository_root.clone())
        .unwrap_or_else(|| discovery.cwd.clone());

    // Settings: global, then project.
    let mut settings_messages = Vec::new();
    let mut settings =
        ResourceSettings::load_file(&home.settings_path(), home.root(), &mut settings_messages);
    let project_dir = workspace_root.join(".pi");
    let project = ResourceSettings::load_file(
        &project_dir.join("settings.json"),
        &project_dir,
        &mut settings_messages,
    );
    settings.overlay(&project);
    add_diagnostics(&mut diagnostics, ResourceKind::Settings, settings_messages);

    let cli_paths = |values: &[String]| -> Vec<PathBuf> {
        values
            .iter()
            .filter(|v| !v.is_empty())
            .map(|v| resolve_user_path(v, &options.cwd))
            .collect()
    };
    let home_base = home.root().to_path_buf();

    // Prompt templates.
    let mut prompt_roots = settings
        .prompts
        .roots
        .clone()
        .unwrap_or(discovery.prompt_roots.clone());
    let mut load_prompts = settings.load_prompt_templates.unwrap_or(true);
    if options.no_prompt_templates {
        load_prompts = false;
        prompt_roots.clear();
    } else {
        append_unique(&mut prompt_roots, cli_paths(&options.prompt_template_paths));
    }
    let mut prompt_templates = PromptTemplateResources {
        roots: prompt_roots.clone(),
        ..Default::default()
    };
    if load_prompts {
        prompt_templates = templates::load_prompt_templates(&prompt_roots);
        filters::apply(
            &mut prompt_templates.templates,
            settings.prompts.filters.as_deref().unwrap_or_default(),
            &home_base,
            filters::MatchKind::Generic,
            |t| (t.path.as_path(), t.root.as_path()),
        );
    }
    add_diagnostics(
        &mut diagnostics,
        ResourceKind::PromptTemplate,
        prompt_templates
            .diagnostics
            .iter()
            .map(|d| d.message.clone()),
    );

    // Skills.
    let mut skill_roots = settings
        .skills
        .roots
        .clone()
        .unwrap_or(discovery.skill_roots.clone());
    let mut load_skill_files = settings.load_skills.unwrap_or(true);
    if options.no_skills {
        skill_roots.clear();
        append_unique(&mut skill_roots, cli_paths(&options.skill_paths));
        load_skill_files = !skill_roots.is_empty();
    } else {
        let mut reordered = Vec::new();
        append_unique(&mut reordered, cli_paths(&options.skill_paths));
        append_unique(&mut reordered, skill_roots);
        skill_roots = reordered;
    }
    let mut skills = SkillResources {
        roots: skill_roots.clone(),
        ..Default::default()
    };
    if load_skill_files {
        skills = skills::load_skills(&skill_roots);
        filters::apply(
            &mut skills.skills,
            settings.skills.filters.as_deref().unwrap_or_default(),
            &home_base,
            filters::MatchKind::Skill,
            |s| (s.path.as_path(), s.root.as_path()),
        );
    }
    add_diagnostics(
        &mut diagnostics,
        ResourceKind::Skill,
        skills.diagnostics.clone(),
    );

    // Themes.
    let mut theme_roots = settings
        .themes
        .roots
        .clone()
        .unwrap_or(discovery.theme_roots.clone());
    let mut load_theme_files = settings.load_themes.unwrap_or(true);
    if options.no_themes {
        load_theme_files = false;
        theme_roots.clear();
    } else {
        append_unique(&mut theme_roots, cli_paths(&options.theme_paths));
    }
    let mut theme_resources = ThemeResources {
        roots: theme_roots.clone(),
        ..Default::default()
    };
    if load_theme_files {
        theme_resources = themes::load_themes(&theme_roots);
        filters::apply(
            &mut theme_resources.themes,
            settings.themes.filters.as_deref().unwrap_or_default(),
            &home_base,
            filters::MatchKind::Generic,
            |t| (t.path.as_path(), t.root.as_path()),
        );
    }
    add_diagnostics(
        &mut diagnostics,
        ResourceKind::Theme,
        theme_resources.diagnostics.clone(),
    );

    // SYSTEM.md / APPEND_SYSTEM.md and CLI overrides.
    let mut contributions =
        system_files::load_prompt_contributions(&discovery.cwd, Some(home.root()));
    system_files::apply_cli_overrides(
        &mut contributions,
        options.system_prompt.as_deref(),
        &options.append_system_prompt,
        &options.cwd,
    );
    add_diagnostics(
        &mut diagnostics,
        ResourceKind::SystemPrompt,
        contributions.diagnostics.clone(),
    );

    ResourceSnapshot {
        cwd: discovery.cwd,
        repository_root: discovery.repository_root,
        workspace_root,
        agent_home: home.root().to_path_buf(),
        enable_skill_commands: settings.enable_skill_commands.unwrap_or(true),
        settings,
        skills,
        prompt_templates,
        themes: theme_resources,
        contributions,
        diagnostics,
    }
}

/// A `/command args` line split into its parts.
fn split_invocation(text: &str) -> Option<(&str, &str)> {
    let rest = text.strip_prefix('/')?;
    let end = rest.find(char::is_whitespace).unwrap_or(rest.len());
    let name = &rest[..end];
    if name.is_empty() {
        return None;
    }
    Some((name, rest[end..].trim_start()))
}

impl ResourceSnapshot {
    /// The system prompt replacement (`SYSTEM.md` or `--system-prompt`).
    pub fn custom_system_prompt(&self) -> Option<&str> {
        self.contributions
            .system_prompt
            .as_ref()
            .map(|c| c.content.as_str())
            .filter(|c| !c.is_empty())
    }

    /// Text appended to the system prompt (`APPEND_SYSTEM.md` and/or
    /// `--append-system-prompt`).
    pub fn append_system_prompt(&self) -> Option<&str> {
        self.contributions
            .append_system_prompt
            .as_ref()
            .map(|c| c.content.as_str())
            .filter(|c| !c.is_empty())
    }

    /// Slash commands for prompt templates and (when `enableSkillCommands`)
    /// skills, skipping names for which `is_reserved` is true or that an
    /// earlier command took. Conflicts produce the archived diagnostics.
    pub fn commands(
        &self,
        is_reserved: impl Fn(&str) -> bool,
    ) -> (Vec<ResourceCommand>, Vec<String>) {
        let mut commands: Vec<ResourceCommand> = Vec::new();
        let mut diagnostics = Vec::new();
        let mut taken: HashSet<String> = HashSet::new();
        for template in &self.prompt_templates.templates {
            if template.name.is_empty() {
                continue;
            }
            if is_reserved(&template.name) || taken.contains(&template.name) {
                diagnostics.push(format!(
                    "prompt command '/{}' conflicts with an existing slash command and was skipped: {}",
                    template.name,
                    template.path.display()
                ));
                continue;
            }
            taken.insert(template.name.clone());
            commands.push(ResourceCommand {
                name: template.name.clone(),
                description: template.description.clone(),
                argument_hint: template.argument_hint.clone(),
                source: ResourceCommandSource::Prompt,
                source_path: template.path.clone(),
            });
        }
        if self.enable_skill_commands {
            for skill in &self.skills.skills {
                if skill.name.is_empty() {
                    continue;
                }
                let name = format!("skill:{}", skill.name);
                if is_reserved(&name) || taken.contains(&name) {
                    diagnostics.push(format!(
                        "skill command '/{name}' conflicts with an existing slash command and was skipped: {}",
                        skill.path.display()
                    ));
                    continue;
                }
                taken.insert(name.clone());
                commands.push(ResourceCommand {
                    name,
                    description: skills::skill_command_description(skill),
                    argument_hint: None,
                    source: ResourceCommandSource::Skill,
                    source_path: skill.path.clone(),
                });
            }
        }
        (commands, diagnostics)
    }

    /// Expand `/template args` or `/skill:name args` into the user message
    /// to send. Returns `None` when the text is not such a command (or skill
    /// commands are disabled).
    pub fn expand_command(&self, text: &str) -> Option<String> {
        let (name, arguments) = split_invocation(text)?;
        if let Some(skill_name) = name.strip_prefix("skill:") {
            if !self.enable_skill_commands {
                return None;
            }
            let skill = self.skills.find(skill_name)?;
            return Some(skills::expand_skill_command(skill, arguments));
        }
        let template = self.prompt_templates.find(name)?;
        Some(templates::expand_prompt_template(
            &template.content,
            arguments,
        ))
    }

    /// The theme named by settings (`theme`), falling back to `fallback`.
    pub fn selected_theme(&self, fallback: &str) -> Theme {
        self.settings
            .theme
            .as_deref()
            .and_then(|name| self.themes.resolve(name))
            .or_else(|| self.themes.resolve(fallback))
            .or_else(|| themes::builtin_theme("dark"))
            .expect("built-in dark theme parses")
    }

    /// Diagnostics rendered as `kind: message` lines.
    pub fn diagnostic_lines(&self) -> Vec<String> {
        self.diagnostics
            .iter()
            .map(|d| {
                let kind = match d.kind {
                    ResourceKind::Discovery => "discovery",
                    ResourceKind::Settings => "settings",
                    ResourceKind::SystemPrompt => "system_prompt",
                    ResourceKind::PromptTemplate => "prompt_template",
                    ResourceKind::Skill => "skill",
                    ResourceKind::Theme => "theme",
                };
                format!("{kind}: {}", d.message)
            })
            .collect()
    }
}

/// Render a path the way resource diagnostics do.
pub fn display_resource_path(path: &Path) -> String {
    path_string(path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn write(path: &Path, text: &str) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    fn skill(name: &str) -> String {
        format!("---\nname: {name}\ndescription: {name} skill\n---\nUse {name}.")
    }

    struct Fixture {
        _dir: tempfile::TempDir,
        home: AgentHome,
        repo: PathBuf,
        cwd: PathBuf,
    }

    fn fixture() -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let base = fs::canonicalize(dir.path()).unwrap();
        let home = AgentHome::new(base.join("home"));
        let repo = base.join("repo");
        let cwd = repo.join("pkg/app");
        fs::create_dir_all(repo.join(".git")).unwrap();
        fs::create_dir_all(&cwd).unwrap();
        write(
            &home.root().join("skills/home-skill/SKILL.md"),
            &skill("home-skill"),
        );
        write(
            &home.root().join("skills/shared/SKILL.md"),
            &skill("shared"),
        );
        write(&repo.join(".pi/skills/shared/SKILL.md"), &skill("shared"));
        write(
            &repo.join(".pi/skills/repo-skill/SKILL.md"),
            &skill("repo-skill"),
        );
        write(
            &repo.join(".agents/skills/agents-top/SKILL.md"),
            &skill("agents-top"),
        );
        write(
            &repo.join("pkg/.agents/skills/agents-pkg/SKILL.md"),
            &skill("agents-pkg"),
        );
        // Above the repository root: not searched.
        write(
            &base.join(".agents/skills/outside/SKILL.md"),
            &skill("outside"),
        );
        write(
            &home.root().join("prompts/review.md"),
            "---\ndescription: Review\n---\nReview $1 then {{args}}",
        );
        write(&repo.join(".pi/prompts/fix.md"), "Fix $@");
        write(&repo.join("pkg/.agents/prompts/deploy.md"), "Deploy ${@:2}");
        write(
            &home.root().join("themes/ocean.json"),
            &themes::tests::theme_json("ocean", "#102030"),
        );
        Fixture {
            _dir: dir,
            home,
            repo,
            cwd,
        }
    }

    #[test]
    fn discovery_order_and_snapshot() {
        let f = fixture();
        let d = discover_resource_roots(f.home.root(), &f.cwd, None);
        assert_eq!(d.repository_root.as_deref(), Some(f.repo.as_path()));
        assert_eq!(
            d.skill_roots,
            vec![
                f.home.root().join("skills"),
                f.repo.join(".pi/skills"),
                f.repo.join(".agents/skills"),
                f.repo.join("pkg/.agents/skills"),
            ]
        );
        assert_eq!(
            d.prompt_roots,
            vec![
                f.home.root().join("prompts"),
                f.repo.join(".pi/prompts"),
                f.repo.join("pkg/.agents/prompts"),
            ]
        );
        assert_eq!(d.theme_roots, vec![f.home.root().join("themes")]);

        let snap = load_resources(&f.home, &ResourceOptions::new(&f.cwd));
        let names: Vec<&str> = snap.skills.skills.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(
            names,
            vec![
                "home-skill",
                "shared",
                "repo-skill",
                "agents-top",
                "agents-pkg"
            ]
        );
        assert_eq!(
            snap.skills.find("shared").unwrap().path,
            f.home.root().join("skills/shared/SKILL.md")
        );
        assert!(
            snap.diagnostic_lines()
                .iter()
                .any(|l| l.starts_with("skill: name \"shared\" collision: "))
        );
        assert_eq!(snap.workspace_root, f.repo);
        assert!(snap.enable_skill_commands);

        let (commands, conflicts) = snap.commands(|name| name == "fix");
        let command_names: Vec<&str> = commands.iter().map(|c| c.name.as_str()).collect();
        assert_eq!(
            command_names,
            vec![
                "review",
                "deploy",
                "skill:home-skill",
                "skill:shared",
                "skill:repo-skill",
                "skill:agents-top",
                "skill:agents-pkg"
            ]
        );
        assert_eq!(
            conflicts,
            vec![format!(
                "prompt command '/fix' conflicts with an existing slash command and was skipped: {}",
                f.repo.join(".pi/prompts/fix.md").display()
            )]
        );
        assert_eq!(
            snap.expand_command("/review main.rs  quickly please ")
                .as_deref(),
            Some("Review main.rs then main.rs  quickly please")
        );
        assert_eq!(
            snap.expand_command("/deploy a b c").as_deref(),
            Some("Deploy b c")
        );
        assert_eq!(
            snap.expand_command("/skill:repo-skill go").as_deref(),
            Some("Use repo-skill.\n\nUser: go")
        );
        assert_eq!(snap.expand_command("/unknown"), None);
        assert_eq!(snap.expand_command("plain text"), None);
        assert_eq!(snap.selected_theme("dark").name, "dark");
        assert!(snap.themes.resolve("ocean").is_some());
    }

    #[test]
    fn settings_filters_features_and_cli_overrides() {
        let f = fixture();
        write(
            &f.home.settings_path(),
            r#"{"skills":["!agents-*","+agents-pkg"],"enableSkillCommands":false,
                "features":{"loadThemes":false},"theme":"ocean"}"#,
        );
        write(
            &f.repo.join(".pi/settings.json"),
            r#"{"prompts":["../extra-prompts"]}"#,
        );
        write(&f.repo.join("extra-prompts/only.md"), "Only");
        let cli_skill = f.repo.join("cli-skill.md");
        write(&cli_skill, "CLI skill body");
        let mut options = ResourceOptions::new(&f.cwd);
        options.skill_paths = vec![cli_skill.display().to_string()];
        let snap = load_resources(&f.home, &options);
        let names: Vec<&str> = snap.skills.skills.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(
            names,
            vec![
                "cli-skill",
                "home-skill",
                "shared",
                "repo-skill",
                "agents-pkg"
            ]
        );
        assert!(!snap.enable_skill_commands);
        assert_eq!(snap.expand_command("/skill:shared"), None);
        let (commands, _) = snap.commands(|_| false);
        assert!(
            commands
                .iter()
                .all(|c| c.source == ResourceCommandSource::Prompt)
        );
        let templates: Vec<&str> = snap
            .prompt_templates
            .templates
            .iter()
            .map(|t| t.name.as_str())
            .collect();
        assert_eq!(templates, vec!["only"]);
        assert!(snap.themes.themes.is_empty());
        // Theme loading is off, so the configured custom theme falls back.
        assert_eq!(snap.selected_theme("light").name, "light");

        let mut options = ResourceOptions::new(&f.cwd);
        options.no_skills = true;
        options.no_prompt_templates = true;
        options.no_themes = true;
        let snap = load_resources(&f.home, &options);
        assert!(snap.skills.skills.is_empty());
        assert!(snap.prompt_templates.templates.is_empty());
        assert!(snap.themes.themes.is_empty());
    }

    #[test]
    fn system_prompt_files_through_snapshot() {
        let f = fixture();
        write(&f.home.root().join("APPEND_SYSTEM.md"), "home append");
        write(&f.cwd.join(".pi/SYSTEM.md"), "project system");
        let mut options = ResourceOptions::new(&f.cwd);
        options.append_system_prompt = vec!["cli append".into()];
        let snap = load_resources(&f.home, &options);
        assert_eq!(snap.custom_system_prompt(), Some("project system"));
        assert_eq!(snap.append_system_prompt(), Some("home append\ncli append"));
    }

    #[test]
    fn workspace_hint_and_pi_agent_home() {
        let dir = tempfile::tempdir().unwrap();
        let base = fs::canonicalize(dir.path()).unwrap();
        let home = base.join("user/.pi/agent");
        fs::create_dir_all(&home).unwrap();
        fs::create_dir_all(base.join("user/.agents/skills")).unwrap();
        let cwd = base.join("work/a");
        fs::create_dir_all(&cwd).unwrap();
        let d = discover_resource_roots(&home, &cwd, Some(&base.join("work")));
        assert_eq!(
            d.repository_root.as_deref(),
            Some(base.join("work").as_path())
        );
        assert_eq!(d.skill_roots, vec![base.join("user/.agents/skills")]);
        let d = discover_resource_roots(&home, &cwd, Some(&base.join("user")));
        assert!(d.diagnostics[0].starts_with("repository_root_hint does not contain cwd: "));
    }
}
