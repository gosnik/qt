//! Agent definitions: Markdown files with frontmatter, the built-in roles and
//! discovery across the user and project scopes.
//!
//! Follows the archived qt (`archived/src/tools/subagent_tool.cpp`):
//! definitions are `*.md` files (recursively, `*.chain.md` skipped) whose
//! frontmatter names the agent and whose body is the system prompt. User
//! definitions live in `<agent_home>/agents`; project definitions in the
//! nearest ancestor `.pi/agents` (Pi) or legacy `.agents` directory.

use std::collections::BTreeMap;
use std::fmt;
use std::path::{Path, PathBuf};

/// Where a definition came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SubagentSource {
    Builtin,
    User,
    Project,
}

impl SubagentSource {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Builtin => "builtin",
            Self::User => "user",
            Self::Project => "project",
        }
    }
}

impl fmt::Display for SubagentSource {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

/// How the definition body is applied to the child's system prompt.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SystemPromptMode {
    /// `--system-prompt`: the body replaces the default prompt.
    #[default]
    Replace,
    /// `--append-system-prompt`: the body is appended to the default prompt.
    Append,
}

impl SystemPromptMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Replace => "replace",
            Self::Append => "append",
        }
    }

    /// CLI flag carrying the body to the child.
    pub fn flag(self) -> &'static str {
        match self {
            Self::Replace => "--system-prompt",
            Self::Append => "--append-system-prompt",
        }
    }
}

/// One executable agent role.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SubagentDefinition {
    /// Runtime name (`<package>.<name>` when the definition sets `package`).
    pub name: String,
    pub description: String,
    /// Tool allowlist passed as `--tools`; empty means `--no-tools`.
    pub tools: Vec<String>,
    pub model: Option<String>,
    pub thinking: Option<String>,
    /// Definition body.
    pub system_prompt: String,
    pub system_prompt_mode: SystemPromptMode,
    /// Keep AGENTS.md / CLAUDE.md context files (otherwise `--no-context-files`).
    pub inherit_project_context: bool,
    /// Keep skills (otherwise `--no-skills`).
    pub inherit_skills: bool,
    /// Upper bound for the child's own subagent depth.
    pub max_subagent_depth: Option<usize>,
    pub disabled: bool,
    pub source: SubagentSource,
    /// Definition file; `None` for built-ins.
    pub file_path: Option<PathBuf>,
}

impl SubagentDefinition {
    fn new(name: String, description: String, source: SubagentSource) -> Self {
        Self {
            name,
            description,
            tools: Vec::new(),
            model: None,
            thinking: None,
            system_prompt: String::new(),
            system_prompt_mode: SystemPromptMode::Replace,
            inherit_project_context: false,
            inherit_skills: false,
            max_subagent_depth: None,
            disabled: false,
            source,
            file_path: None,
        }
    }
}

/// Discovery scope (`agentScope`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AgentScope {
    #[default]
    User,
    Project,
    Both,
}

impl AgentScope {
    /// Parse `user`, `project` or `both` (trimmed, case-insensitive).
    pub fn parse(value: &str) -> Option<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "user" => Some(Self::User),
            "project" => Some(Self::Project),
            "both" => Some(Self::Both),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Self::User => "user",
            Self::Project => "project",
            Self::Both => "both",
        }
    }

    fn includes_user(self) -> bool {
        self != Self::Project
    }

    fn includes_project(self) -> bool {
        self != Self::User
    }
}

/// Result of [`discover_subagents`].
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SubagentDiscovery {
    /// Enabled agents sorted by name; later scopes override earlier ones.
    pub agents: Vec<SubagentDefinition>,
    pub diagnostics: Vec<String>,
    /// Nearest project directory (`.pi/agents` preferred over `.agents`).
    pub project_agents_directory: Option<PathBuf>,
}

impl SubagentDiscovery {
    pub fn find(&self, name: &str) -> Option<&SubagentDefinition> {
        self.agents.iter().find(|a| a.name == name)
    }
}

/// Frontmatter fields of a definition file.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Frontmatter {
    pub fields: BTreeMap<String, String>,
    /// The file starts with a `---` line.
    pub had_frontmatter: bool,
    /// A closing `---` line was found.
    pub complete: bool,
    pub diagnostics: Vec<String>,
}

fn normalize_newlines(text: &str) -> String {
    text.replace("\r\n", "\n").replace('\r', "\n")
}

fn unquote(value: &str) -> &str {
    let bytes = value.as_bytes();
    if bytes.len() >= 2
        && ((bytes[0] == b'"' && bytes[bytes.len() - 1] == b'"')
            || (bytes[0] == b'\'' && bytes[bytes.len() - 1] == b'\''))
    {
        &value[1..value.len() - 1]
    } else {
        value
    }
}

/// Parse the archived qt's line-based frontmatter: `key: value` pairs between
/// `---` lines, `#` comments and blank lines skipped, values unquoted.
pub fn parse_frontmatter(content: &str) -> Frontmatter {
    let mut result = Frontmatter::default();
    let normalized = normalize_newlines(content);
    let mut lines = normalized.split('\n');
    if lines.next() != Some("---") {
        return result;
    }
    result.had_frontmatter = true;
    for line in lines {
        if line == "---" {
            result.complete = true;
            break;
        }
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        match trimmed.split_once(':') {
            None => result
                .diagnostics
                .push("frontmatter line is missing ':' separator".into()),
            Some((key, value)) => {
                let key = key.trim();
                if !key.is_empty() {
                    result
                        .fields
                        .insert(key.to_string(), unquote(value.trim()).to_string());
                }
            }
        }
    }
    if !result.complete {
        result
            .diagnostics
            .push("frontmatter block is missing closing delimiter".into());
    }
    result
}

/// The definition body (everything after the closing `---`), trimmed.
pub fn document_body(content: &str) -> String {
    let content = normalize_newlines(content);
    if !content.starts_with("---\n") {
        return content.trim().to_string();
    }
    let Some(closing) = content[4..].find("\n---").map(|i| i + 4) else {
        return content.trim().to_string();
    };
    let mut body = closing + 4;
    if content.as_bytes().get(body) == Some(&b'\n') {
        body += 1;
    }
    content[body..].trim().to_string()
}

/// Archived boolean parsing: `true/yes/1`, `false/no/0`, otherwise `fallback`.
pub fn parse_boolean(value: &str, fallback: bool) -> bool {
    match value.trim().to_ascii_lowercase().as_str() {
        "true" | "yes" | "1" => true,
        "false" | "no" | "0" => false,
        _ => fallback,
    }
}

/// Split a comma- or whitespace-separated list, dropping empties and duplicates.
pub fn split_list(value: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for item in value.split(|c: char| c == ',' || c.is_whitespace()) {
        let item = item.trim();
        if !item.is_empty() && !out.iter().any(|existing| existing == item) {
            out.push(item.to_string());
        }
    }
    out
}

/// A non-negative integer with nothing else around it.
pub fn parse_nonnegative(value: &str) -> Option<usize> {
    let value = value.trim();
    if value.is_empty() || !value.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    value.parse().ok()
}

/// Why a definition file was rejected.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DefinitionError {
    /// Missing or unterminated frontmatter.
    Invalid,
    /// `name` or `description` missing or empty.
    MissingNameOrDescription,
}

/// Parse one definition file's content.
pub fn parse_definition(
    content: &str,
    source: SubagentSource,
    file_path: Option<PathBuf>,
) -> Result<SubagentDefinition, DefinitionError> {
    let frontmatter = parse_frontmatter(content);
    if !frontmatter.had_frontmatter || !frontmatter.complete {
        return Err(DefinitionError::Invalid);
    }
    let field = |key: &str| frontmatter.fields.get(key).map(|v| v.trim());
    let name = field("name").filter(|v| !v.is_empty());
    let description = field("description").filter(|v| !v.is_empty());
    let (Some(name), Some(description)) = (name, description) else {
        return Err(DefinitionError::MissingNameOrDescription);
    };
    let name = match field("package").filter(|p| !p.is_empty()) {
        Some(package) => format!("{package}.{name}"),
        None => name.to_string(),
    };
    let mut agent = SubagentDefinition::new(name, description.to_string(), source);
    agent.system_prompt = document_body(content);
    agent.file_path = file_path;
    if let Some(tools) = frontmatter.fields.get("tools") {
        agent.tools = split_list(tools);
    }
    agent.model = field("model").filter(|v| !v.is_empty()).map(str::to_string);
    agent.thinking = field("thinking")
        .filter(|v| !v.is_empty())
        .map(str::to_string);
    let is_delegate = agent.name == "delegate";
    match field("systemPromptMode") {
        Some(mode) => match mode.to_ascii_lowercase().as_str() {
            "append" => agent.system_prompt_mode = SystemPromptMode::Append,
            "replace" => agent.system_prompt_mode = SystemPromptMode::Replace,
            _ => {}
        },
        None if is_delegate => agent.system_prompt_mode = SystemPromptMode::Append,
        None => {}
    }
    match field("inheritProjectContext") {
        Some(value) => agent.inherit_project_context = parse_boolean(value, false),
        None if is_delegate => agent.inherit_project_context = true,
        None => {}
    }
    if let Some(value) = field("inheritSkills") {
        agent.inherit_skills = parse_boolean(value, false);
    }
    for key in ["maxSubagentDepth", "maxDepth"] {
        if let Some(value) = field(key) {
            agent.max_subagent_depth = parse_nonnegative(value);
            break;
        }
    }
    if let Some(value) = field("disabled") {
        agent.disabled = parse_boolean(value, false);
    }
    Ok(agent)
}

fn collect_markdown_files(dir: &Path, out: &mut Vec<PathBuf>, diagnostics: &mut Vec<String>) {
    let entries = match std::fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(e) => {
            diagnostics.push(format!(
                "unable to inspect subagent directory '{}': {e}",
                dir.display()
            ));
            return;
        }
    };
    let mut paths: Vec<PathBuf> = entries.filter_map(|e| e.ok().map(|e| e.path())).collect();
    paths.sort();
    for path in paths {
        let Ok(link_meta) = std::fs::symlink_metadata(&path) else {
            continue;
        };
        if link_meta.is_dir() {
            collect_markdown_files(&path, out, diagnostics);
            continue;
        }
        let is_file = std::fs::metadata(&path).is_ok_and(|m| m.is_file());
        let name = path
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default();
        if is_file && path.extension().is_some_and(|ext| ext == "md") && !name.contains(".chain.md")
        {
            out.push(path);
        }
    }
}

fn load_agent_directory(
    dir: &Path,
    source: SubagentSource,
    agents: &mut BTreeMap<String, SubagentDefinition>,
    diagnostics: &mut Vec<String>,
) {
    if !dir.is_dir() {
        return;
    }
    let mut files = Vec::new();
    collect_markdown_files(dir, &mut files, diagnostics);
    for file in files {
        let shown = file.to_string_lossy().replace('\\', "/");
        let content = match std::fs::read_to_string(&file) {
            Ok(content) => content,
            Err(_) => {
                diagnostics.push(format!("unable to read subagent definition '{shown}'"));
                continue;
            }
        };
        match parse_definition(&content, source, Some(file.clone())) {
            Ok(agent) => {
                agents.insert(agent.name.clone(), agent);
            }
            Err(DefinitionError::Invalid) => {
                diagnostics.push(format!("ignored invalid subagent definition '{shown}'"));
            }
            Err(DefinitionError::MissingNameOrDescription) => diagnostics.push(format!(
                "ignored subagent definition without name and description '{shown}'"
            )),
        }
    }
}

const INDEXED_READ_TOOLS: [&str; 10] = [
    "read",
    "symbol",
    "analyze_file",
    "references",
    "build_context",
    "search_index",
    "index",
    "grep",
    "find",
    "ls",
];

/// The built-in leaf roles `scout`, `planner`, `reviewer` and `worker`.
pub fn builtin_subagents() -> Vec<SubagentDefinition> {
    let leaf = |name: &str, description: &str, extra: &[&str], prompt: &str| {
        let mut agent =
            SubagentDefinition::new(name.into(), description.into(), SubagentSource::Builtin);
        agent.tools = INDEXED_READ_TOOLS
            .iter()
            .chain(extra.iter())
            .map(|t| t.to_string())
            .collect();
        agent.system_prompt = prompt.into();
        agent.max_subagent_depth = Some(0);
        agent
    };
    vec![
        leaf(
            "scout",
            "Map code and return concise evidence for a handoff",
            &["bash"],
            "You are a read-only scouting agent. Locate the relevant code with the symbol index for C/C++ identifiers and targeted text searches for literal content, follow callers and tests, verify every material claim, and return concise findings with exact file paths. Do not modify the repository.",
        ),
        leaf(
            "planner",
            "Design an implementation plan without changing files",
            &[],
            "You are a planning agent. Inspect the actual repository, identify owners and constraints, compare viable approaches, and return an ordered implementation and verification plan. Do not modify files.",
        ),
        leaf(
            "reviewer",
            "Review correctness, safety, and verification gaps",
            &["bash"],
            "You are a review-only agent. Inspect the requested code and current diff, run focused read-only checks when useful, and report actionable findings ordered by severity with exact file paths. Do not edit files.",
        ),
        leaf(
            "worker",
            "Implement one bounded task and verify the result",
            &["bash", "edit", "write", "apply_patch"],
            "You are the single implementation worker for the delegated task. Inspect before editing, make a narrow production-ready change, follow repository instructions, run proportionate verification, and report the completed result and any remaining blocker.",
        ),
    ]
}

/// Project definition directories found from `cwd` upwards.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ProjectAgentDirectories {
    /// `.pi/agents` when present, otherwise `.agents`.
    pub display_directory: Option<PathBuf>,
    /// Legacy `.agents` first so `.pi/agents` overrides it.
    pub load_order: Vec<PathBuf>,
}

/// Find the nearest ancestor of `cwd` holding `.pi/agents` or `.agents`.
pub fn find_project_agent_directories(cwd: &Path) -> ProjectAgentDirectories {
    let start = if cwd.is_absolute() {
        cwd.to_path_buf()
    } else {
        match std::env::current_dir() {
            Ok(base) => base.join(cwd),
            Err(_) => return ProjectAgentDirectories::default(),
        }
    };
    let mut dir: Option<&Path> = Some(&start);
    while let Some(current) = dir {
        let canonical = current.join(".pi").join("agents");
        let legacy = current.join(".agents");
        let has_canonical = canonical.is_dir();
        let has_legacy = legacy.is_dir();
        if has_canonical || has_legacy {
            let mut found = ProjectAgentDirectories {
                display_directory: Some(if has_canonical {
                    canonical.clone()
                } else {
                    legacy.clone()
                }),
                load_order: Vec::new(),
            };
            if has_legacy {
                found.load_order.push(legacy);
            }
            if has_canonical {
                found.load_order.push(canonical);
            }
            return found;
        }
        dir = current.parent();
    }
    ProjectAgentDirectories::default()
}

/// Discover built-in, user (`<agent_home>/agents`) and project agents.
///
/// `scope` is validated like the archived qt: anything other than
/// `user|project|both` yields no agents and an `invalid subagent scope`
/// diagnostic. Built-ins are always included unless a user or project
/// definition with the same name overrides (or disables) them.
pub fn discover_subagents(agent_home: &Path, cwd: &Path, scope: &str) -> SubagentDiscovery {
    let mut result = SubagentDiscovery::default();
    let Some(scope) = AgentScope::parse(scope) else {
        result
            .diagnostics
            .push(format!("invalid subagent scope '{scope}'"));
        return result;
    };
    let mut agents: BTreeMap<String, SubagentDefinition> = builtin_subagents()
        .into_iter()
        .map(|a| (a.name.clone(), a))
        .collect();
    if scope.includes_user() {
        load_agent_directory(
            &agent_home.join("agents"),
            SubagentSource::User,
            &mut agents,
            &mut result.diagnostics,
        );
    }
    let project = find_project_agent_directories(cwd);
    result.project_agents_directory = project.display_directory;
    if scope.includes_project() {
        for dir in &project.load_order {
            load_agent_directory(
                dir,
                SubagentSource::Project,
                &mut agents,
                &mut result.diagnostics,
            );
        }
    }
    result.agents = agents.into_values().filter(|a| !a.disabled).collect();
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(path: &Path, content: &str) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, content).unwrap();
    }

    #[test]
    fn parses_every_frontmatter_key() {
        let content = "---\r\nname: helper\r\ndescription: \"Helps out\"\r\n# comment\r\ntools: read, grep  find,read\r\nmodel: gpt-5\r\nthinking: high\r\nsystemPromptMode: APPEND\r\ninheritProjectContext: yes\r\ninheritSkills: 1\r\nmaxDepth: 3\r\npackage: acme\r\n---\r\n\r\nYou help.\r\n";
        let agent = parse_definition(content, SubagentSource::User, None).unwrap();
        assert_eq!(agent.name, "acme.helper");
        assert_eq!(agent.description, "Helps out");
        assert_eq!(agent.tools, vec!["read", "grep", "find"]);
        assert_eq!(agent.model.as_deref(), Some("gpt-5"));
        assert_eq!(agent.thinking.as_deref(), Some("high"));
        assert_eq!(agent.system_prompt_mode, SystemPromptMode::Append);
        assert!(agent.inherit_project_context);
        assert!(agent.inherit_skills);
        assert_eq!(agent.max_subagent_depth, Some(3));
        assert!(!agent.disabled);
        assert_eq!(agent.system_prompt, "You help.");
    }

    #[test]
    fn delegate_defaults_and_overrides() {
        let agent = parse_definition(
            "---\nname: delegate\ndescription: d\n---\nbody",
            SubagentSource::User,
            None,
        )
        .unwrap();
        assert_eq!(agent.system_prompt_mode, SystemPromptMode::Append);
        assert!(agent.inherit_project_context);
        let agent = parse_definition(
            "---\nname: delegate\ndescription: d\nsystemPromptMode: replace\ninheritProjectContext: no\nmaxSubagentDepth: x\nmaxDepth: 4\n---\n",
            SubagentSource::User,
            None,
        )
        .unwrap();
        assert_eq!(agent.system_prompt_mode, SystemPromptMode::Replace);
        assert!(!agent.inherit_project_context);
        // maxSubagentDepth wins over maxDepth even when it is invalid.
        assert_eq!(agent.max_subagent_depth, None);
        assert_eq!(agent.system_prompt, "");
    }

    #[test]
    fn rejects_invalid_definitions() {
        assert_eq!(
            parse_definition("no frontmatter", SubagentSource::User, None),
            Err(DefinitionError::Invalid)
        );
        assert_eq!(
            parse_definition("---\nname: a\n", SubagentSource::User, None),
            Err(DefinitionError::Invalid)
        );
        assert_eq!(
            parse_definition(
                "---\nname: a\ndescription:  \n---\n",
                SubagentSource::User,
                None
            ),
            Err(DefinitionError::MissingNameOrDescription)
        );
    }

    #[test]
    fn builtins_are_leaf_roles() {
        let builtins = builtin_subagents();
        let names: Vec<&str> = builtins.iter().map(|a| a.name.as_str()).collect();
        assert_eq!(names, ["scout", "planner", "reviewer", "worker"]);
        assert!(builtins.iter().all(|a| a.max_subagent_depth == Some(0)));
        let worker = &builtins[3];
        assert!(worker.tools.contains(&"apply_patch".to_string()));
        assert!(!builtins[1].tools.contains(&"bash".to_string()));
        assert!(
            builtins[0]
                .system_prompt
                .starts_with("You are a read-only scouting agent.")
        );
    }

    #[test]
    fn discovery_scopes_and_overrides() {
        let home = tempfile::tempdir().unwrap();
        let repo = tempfile::tempdir().unwrap();
        write(
            &home.path().join("agents/nested/user-one.md"),
            "---\nname: one\ndescription: user one\n---\nU",
        );
        write(
            &home.path().join("agents/flow.chain.md"),
            "---\nname: flow\ndescription: chain\n---\n",
        );
        write(&home.path().join("agents/broken.md"), "---\nname: broken\n");
        write(
            &home.path().join("agents/off.md"),
            "---\nname: planner\ndescription: off\ndisabled: true\n---\n",
        );
        write(
            &repo.path().join(".agents/legacy.md"),
            "---\nname: one\ndescription: legacy one\n---\nL",
        );
        write(
            &repo.path().join(".pi/agents/proj.md"),
            "---\nname: one\ndescription: project one\n---\nP",
        );
        let sub = repo.path().join("src/deep");
        std::fs::create_dir_all(&sub).unwrap();

        let user = discover_subagents(home.path(), &sub, "user");
        let names: Vec<&str> = user.agents.iter().map(|a| a.name.as_str()).collect();
        assert_eq!(names, ["one", "reviewer", "scout", "worker"]);
        assert_eq!(user.find("one").unwrap().source, SubagentSource::User);
        assert_eq!(
            user.project_agents_directory.as_deref(),
            Some(repo.path().join(".pi/agents").as_path())
        );
        assert_eq!(user.diagnostics.len(), 1);
        assert!(user.diagnostics[0].starts_with("ignored invalid subagent definition '"));

        let both = discover_subagents(home.path(), &sub, " Both ");
        let one = both.find("one").unwrap();
        assert_eq!(one.source, SubagentSource::Project);
        assert_eq!(one.description, "project one");

        let project = discover_subagents(home.path(), &sub, "project");
        assert!(project.find("planner").is_some());
        assert_eq!(project.find("one").unwrap().system_prompt, "P");

        let invalid = discover_subagents(home.path(), &sub, "all");
        assert!(invalid.agents.is_empty());
        assert_eq!(invalid.diagnostics, ["invalid subagent scope 'all'"]);
    }

    #[test]
    fn legacy_directory_is_displayed_when_alone() {
        let repo = tempfile::tempdir().unwrap();
        std::fs::create_dir_all(repo.path().join(".agents")).unwrap();
        let found = find_project_agent_directories(repo.path());
        assert_eq!(found.display_directory, Some(repo.path().join(".agents")));
        assert_eq!(found.load_order, vec![repo.path().join(".agents")]);
    }

    #[test]
    fn helpers() {
        assert_eq!(parse_nonnegative(" 7 "), Some(7));
        assert_eq!(parse_nonnegative("-1"), None);
        assert_eq!(parse_nonnegative("3x"), None);
        assert!(parse_boolean("maybe", true));
        assert_eq!(document_body("plain\n"), "plain");
        assert_eq!(AgentScope::parse("PROJECT"), Some(AgentScope::Project));
    }
}
