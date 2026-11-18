//! System prompt construction.
//!
//! The body follows Pi's `buildSystemPrompt` (with "qt" for "pi" and no Pi
//! documentation section). The archived qt's additions are layered on top:
//! `SYSTEM.md` / `--system-prompt` replace the body, `APPEND_SYSTEM.md` /
//! `--append-system-prompt` follow it, the `<available_skills>` block and
//! custom guidance come after the project context, and the prompt ends with
//! the archived `# Environment Context` block (date, cwd, workspace, shell,
//! timezone, network, tool permissions, active tool count).

use std::path::{Path, PathBuf};

use crate::config::AgentHome;
use crate::resources::fsutil::env_flag_enabled;
use crate::resources::skills::{Skill, escape_xml, format_skills_for_prompt};
use crate::tools::ToolRegistry;

/// An instruction file folded into the system prompt.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ContextFile {
    pub path: PathBuf,
    pub content: String,
}

const CONTEXT_FILE_NAMES: [&str; 4] = ["AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD"];

/// The global `AGENTS.md` in the agent home, then one instruction file per
/// directory from the filesystem root down to `cwd`.
pub fn discover_context_files(home: &AgentHome, cwd: &Path) -> Vec<ContextFile> {
    let mut files = Vec::new();
    if let Some(file) = read_context_file(home.global_agents_file()) {
        files.push(file);
    }
    let mut dirs: Vec<&Path> = cwd.ancestors().collect();
    dirs.reverse();
    for dir in dirs {
        if let Some(file) = CONTEXT_FILE_NAMES
            .iter()
            .find_map(|name| read_context_file(dir.join(name)))
            && !files.iter().any(|f: &ContextFile| f.path == file.path)
        {
            files.push(file);
        }
    }
    files
}

fn read_context_file(path: PathBuf) -> Option<ContextFile> {
    if !path.is_file() {
        return None;
    }
    let content = std::fs::read_to_string(&path).ok()?;
    (!content.trim().is_empty()).then_some(ContextFile { path, content })
}

/// The archived qt's `<environment_context>` values. Empty fields are
/// omitted from the block.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RuntimeMetadata {
    pub working_directory: Option<String>,
    pub workspace_root: Option<String>,
    pub shell: Option<String>,
    /// `YYYY-MM-DD` (UTC).
    pub current_date: Option<String>,
    /// Local zone abbreviation and offset, e.g. `CET +0100`.
    pub timezone: Option<String>,
    pub network_access: Option<String>,
    pub tool_permissions: Option<String>,
    pub active_tool_count: Option<usize>,
}

/// How tool mutations and filesystem paths are governed, for the
/// `<tool_permissions>` text.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ToolPolicy {
    /// Mutating tools wait in an approval queue.
    pub queue_mutations: bool,
    /// Path arguments may leave the workspace.
    pub allow_external_filesystem_access: bool,
}

impl Default for ToolPolicy {
    /// The current runtime: mutations run immediately, paths are not
    /// confined.
    fn default() -> Self {
        Self {
            queue_mutations: false,
            allow_external_filesystem_access: true,
        }
    }
}

/// `tool mutations execute immediately; filesystem path arguments are …`
pub fn tool_permissions_summary(policy: ToolPolicy) -> String {
    let mutations = if policy.queue_mutations {
        "tool mutations require queued approval"
    } else {
        "tool mutations execute immediately"
    };
    let paths = if policy.allow_external_filesystem_access {
        "allowed outside the workspace"
    } else {
        "confined to the workspace"
    };
    format!("{mutations}; filesystem path arguments are {paths}")
}

/// `enabled`, or `disabled by PI_OFFLINE` when `PI_OFFLINE` is set or
/// `offline` (`--offline`) is true.
pub fn network_access_summary(offline: bool) -> String {
    if offline || env_flag_enabled("PI_OFFLINE") {
        "disabled by PI_OFFLINE".to_string()
    } else {
        "enabled".to_string()
    }
}

/// `settings.shellPath`, else `$SHELL`.
pub fn configured_shell(shell_path: Option<&str>) -> Option<String> {
    shell_path
        .filter(|s| !s.is_empty())
        .map(str::to_string)
        .or_else(|| std::env::var("SHELL").ok().filter(|s| !s.is_empty()))
}

/// Local timezone as `strftime("%Z %z")`.
pub fn current_timezone_string() -> String {
    #[cfg(unix)]
    {
        // SAFETY: `time`, `localtime_r` and `strftime` are given valid
        // pointers to stack values and a buffer of the stated length.
        unsafe {
            let now = libc::time(std::ptr::null_mut());
            let mut tm: libc::tm = std::mem::zeroed();
            if !libc::localtime_r(&now, &mut tm).is_null() {
                let mut buf = [0 as libc::c_char; 64];
                let len = libc::strftime(buf.as_mut_ptr(), buf.len(), c"%Z %z".as_ptr(), &tm);
                if len > 0 {
                    let bytes: Vec<u8> = buf[..len].iter().map(|c| *c as u8).collect();
                    return String::from_utf8_lossy(&bytes).into_owned();
                }
            }
        }
    }
    chrono::Local::now().format("%z").to_string()
}

impl RuntimeMetadata {
    /// Collect the archived qt's metadata for a session.
    pub fn detect(
        cwd: &Path,
        workspace_root: Option<&Path>,
        shell_path: Option<&str>,
        offline: bool,
        policy: ToolPolicy,
        active_tool_count: usize,
    ) -> Self {
        let cwd_text = cwd.display().to_string();
        Self {
            working_directory: Some(cwd_text.clone()),
            workspace_root: Some(
                workspace_root
                    .filter(|p| !p.as_os_str().is_empty())
                    .map(|p| p.display().to_string())
                    .unwrap_or(cwd_text),
            ),
            shell: configured_shell(shell_path),
            current_date: Some(chrono::Utc::now().format("%Y-%m-%d").to_string()),
            timezone: Some(current_timezone_string()),
            network_access: Some(network_access_summary(offline)),
            tool_permissions: Some(tool_permissions_summary(policy)),
            active_tool_count: Some(active_tool_count),
        }
    }

    fn is_empty(&self) -> bool {
        let blank = |v: &Option<String>| v.as_deref().is_none_or(str::is_empty);
        blank(&self.working_directory)
            && blank(&self.workspace_root)
            && blank(&self.shell)
            && blank(&self.current_date)
            && blank(&self.timezone)
            && blank(&self.network_access)
            && blank(&self.tool_permissions)
            && self.active_tool_count.is_none()
    }

    /// The `# Environment Context` section, without surrounding blank lines.
    pub fn render(&self) -> String {
        if self.is_empty() {
            return String::new();
        }
        let mut lines = vec![
            "# Environment Context".to_string(),
            String::new(),
            "<environment_context>".to_string(),
        ];
        let mut field = |tag: &str, value: &Option<String>| {
            if let Some(v) = value.as_deref().filter(|v| !v.is_empty()) {
                lines.push(format!("  <{tag}>{}</{tag}>", escape_xml(v)));
            }
        };
        field("cwd", &self.working_directory);
        field("workspace_root", &self.workspace_root);
        field("shell", &self.shell);
        field("current_date", &self.current_date);
        field("timezone", &self.timezone);
        field("network", &self.network_access);
        field("tool_permissions", &self.tool_permissions);
        if let Some(count) = self.active_tool_count {
            lines.push(format!("  <active_tools>{count}</active_tools>"));
        }
        lines.push("</environment_context>".to_string());
        lines.join("\n")
    }
}

/// Everything that goes into the system prompt.
#[derive(Clone, Copy)]
pub struct PromptInputs<'a> {
    pub tools: &'a ToolRegistry,
    pub context_files: &'a [ContextFile],
    pub cwd: &'a Path,
    /// Replaces the default body (`SYSTEM.md`, `--system-prompt`).
    pub custom_prompt: Option<&'a str>,
    /// Follows the body (`APPEND_SYSTEM.md`, `--append-system-prompt`).
    pub append_system_prompt: Option<&'a str>,
    /// Extra guideline bullets for the default body.
    pub extra_guidelines: &'a [String],
    pub skills: &'a [Skill],
    /// `--custom-guidance` text, placed after repository instructions.
    pub custom_guidance: Option<&'a str>,
    /// The environment block; `None` ends the prompt with Pi's
    /// `Current date:` / `Current working directory:` lines instead.
    pub runtime: Option<&'a RuntimeMetadata>,
}

impl<'a> PromptInputs<'a> {
    pub fn new(tools: &'a ToolRegistry, context_files: &'a [ContextFile], cwd: &'a Path) -> Self {
        Self {
            tools,
            context_files,
            cwd,
            custom_prompt: None,
            append_system_prompt: None,
            extra_guidelines: &[],
            skills: &[],
            custom_guidance: None,
            runtime: None,
        }
    }
}

fn default_body(tools: &ToolRegistry, extra_guidelines: &[String]) -> String {
    let names: Vec<&str> = tools.iter().map(|t| t.name()).collect();
    let tools_list = if names.is_empty() {
        "(none)".to_string()
    } else {
        tools
            .iter()
            .map(|t| format!("- {}: {}", t.name(), t.snippet()))
            .collect::<Vec<_>>()
            .join("\n")
    };

    let mut guidelines: Vec<String> = Vec::new();
    let mut add = |g: &str| {
        let g = g.trim();
        if !g.is_empty() && !guidelines.iter().any(|x| x == g) {
            guidelines.push(g.to_string());
        }
    };
    let has = |n: &str| names.contains(&n);
    if has("bash") && !has("grep") && !has("find") && !has("ls") {
        add("Use bash for file operations like ls, rg, find");
    } else if has("bash") && (has("grep") || has("find") || has("ls")) {
        add(
            "Prefer grep/find/ls tools over bash for file exploration (faster, respects .gitignore)",
        );
    }
    for tool in tools.iter() {
        for g in tool.guidelines() {
            add(g);
        }
    }
    for g in extra_guidelines {
        add(g);
    }
    add("Be concise in your responses");
    add("Show file paths clearly when working with files");
    let guidelines = guidelines
        .iter()
        .map(|g| format!("- {g}"))
        .collect::<Vec<_>>()
        .join("\n");

    format!(
        "You are an expert coding assistant operating inside qt, a coding agent harness. You help users by reading files, executing commands, editing code, and writing new files.\n\n\
         Available tools:\n{tools_list}\n\n\
         In addition to the tools above, you may have access to other custom tools depending on the project.\n\n\
         Guidelines:\n{guidelines}"
    )
}

/// Build the system prompt from `inputs`.
pub fn build_prompt(inputs: &PromptInputs<'_>) -> String {
    let mut prompt = match inputs.custom_prompt.filter(|p| !p.is_empty()) {
        Some(custom) => custom.to_string(),
        None => default_body(inputs.tools, inputs.extra_guidelines),
    };
    if let Some(append) = inputs.append_system_prompt.filter(|a| !a.is_empty()) {
        prompt.push_str("\n\n");
        prompt.push_str(append);
    }
    if !inputs.context_files.is_empty() {
        prompt
            .push_str("\n\n# Project Context\n\nProject-specific instructions and guidelines:\n\n");
        for file in inputs.context_files {
            prompt.push_str(&format!(
                "## {}\n\n{}\n\n",
                file.path.display(),
                file.content
            ));
        }
    }
    prompt.push_str(&format_skills_for_prompt(inputs.skills));
    if let Some(guidance) = inputs.custom_guidance.filter(|g| !g.is_empty()) {
        trim_trailing_newlines(&mut prompt);
        prompt.push_str("\n\nAdditional guidance (applies after repository instructions):\n");
        prompt.push_str(guidance);
    }
    match inputs
        .runtime
        .map(RuntimeMetadata::render)
        .filter(|r| !r.is_empty())
    {
        Some(block) => {
            trim_trailing_newlines(&mut prompt);
            prompt.push_str("\n\n");
            prompt.push_str(&block);
        }
        None => {
            let date = chrono::Local::now().format("%Y-%m-%d");
            prompt.push_str(&format!("\nCurrent date: {date}"));
            prompt.push_str(&format!(
                "\nCurrent working directory: {}",
                inputs.cwd.display()
            ));
        }
    }
    prompt
}

fn trim_trailing_newlines(text: &mut String) {
    while text.ends_with('\n') {
        text.pop();
    }
}

/// The default system prompt for `tools`, `context_files` and `cwd`, ending
/// with the environment block for the current runtime (immediate tool
/// mutations, unconfined paths).
pub fn build_system_prompt(
    tools: &ToolRegistry,
    context_files: &[ContextFile],
    cwd: &Path,
) -> String {
    let runtime = RuntimeMetadata::detect(
        cwd,
        None,
        None,
        false,
        ToolPolicy::default(),
        tools.iter().count(),
    );
    let inputs = PromptInputs {
        runtime: Some(&runtime),
        ..PromptInputs::new(tools, context_files, cwd)
    };
    build_prompt(&inputs)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn skill(name: &str, hidden: bool) -> Skill {
        Skill {
            root: PathBuf::from("/s"),
            directory: PathBuf::from(format!("/s/{name}")),
            path: PathBuf::from(format!("/s/{name}/SKILL.md")),
            name: name.to_string(),
            description: format!("{name} & co"),
            content: String::new(),
            disable_model_invocation: hidden,
        }
    }

    #[test]
    fn prompt_lists_tools_guidelines_and_context() {
        let dir = tempfile::tempdir().unwrap();
        let home = AgentHome::new(dir.path().join("home"));
        std::fs::create_dir_all(home.root()).unwrap();
        std::fs::write(home.global_agents_file(), "global rules").unwrap();
        let project = dir.path().join("proj");
        std::fs::create_dir_all(project.join("sub")).unwrap();
        std::fs::write(project.join("CLAUDE.md"), "project rules").unwrap();
        let files = discover_context_files(&home, &project.join("sub"));
        assert_eq!(files.len(), 2);
        assert_eq!(files[0].content, "global rules");

        let tools = ToolRegistry::coding();
        let prompt = build_system_prompt(&tools, &files, &project);
        assert!(prompt.contains("- read: Read file contents"));
        assert!(prompt.contains("Prefer grep/find/ls tools over bash"));
        assert!(prompt.contains("Use edit for precise changes"));
        assert!(
            prompt.contains("project rules\n\n# Environment Context\n\n<environment_context>\n")
        );
        assert!(prompt.contains(&format!("  <cwd>{}</cwd>\n", project.display())));
        assert!(prompt.contains(&format!(
            "  <active_tools>{}</active_tools>\n</environment_context>",
            tools.iter().count()
        )));
        assert!(prompt.contains("  <tool_permissions>tool mutations execute immediately; filesystem path arguments are allowed outside the workspace</tool_permissions>"));
        assert!(prompt.ends_with("</environment_context>"));
    }

    #[test]
    fn environment_block_matches_archived_layout() {
        let meta = RuntimeMetadata {
            working_directory: Some("/w/a".into()),
            workspace_root: Some("/w".into()),
            shell: Some("/bin/bash".into()),
            current_date: Some("2026-11-18".into()),
            timezone: Some("CET +0100".into()),
            network_access: Some("enabled".into()),
            tool_permissions: Some(tool_permissions_summary(ToolPolicy {
                queue_mutations: true,
                allow_external_filesystem_access: false,
            })),
            active_tool_count: Some(7),
        };
        assert_eq!(
            meta.render(),
            "# Environment Context\n\n<environment_context>\n  <cwd>/w/a</cwd>\n  <workspace_root>/w</workspace_root>\n  <shell>/bin/bash</shell>\n  <current_date>2026-11-18</current_date>\n  <timezone>CET +0100</timezone>\n  <network>enabled</network>\n  <tool_permissions>tool mutations require queued approval; filesystem path arguments are confined to the workspace</tool_permissions>\n  <active_tools>7</active_tools>\n</environment_context>"
        );
        let partial = RuntimeMetadata {
            working_directory: Some("a<b".into()),
            shell: Some(String::new()),
            ..Default::default()
        };
        assert_eq!(
            partial.render(),
            "# Environment Context\n\n<environment_context>\n  <cwd>a&lt;b</cwd>\n</environment_context>"
        );
        assert_eq!(RuntimeMetadata::default().render(), "");

        let detected = RuntimeMetadata::detect(
            Path::new("/x"),
            None,
            Some("/bin/fish"),
            true,
            ToolPolicy::default(),
            3,
        );
        assert_eq!(detected.workspace_root.as_deref(), Some("/x"));
        assert_eq!(detected.shell.as_deref(), Some("/bin/fish"));
        assert_eq!(
            detected.network_access.as_deref(),
            Some("disabled by PI_OFFLINE")
        );
        assert_eq!(detected.current_date.as_ref().map(String::len), Some(10));
        assert!(!detected.timezone.unwrap().is_empty());
    }

    #[test]
    fn custom_prompt_append_skills_and_guidance_order() {
        let tools = ToolRegistry::coding();
        let files = vec![ContextFile {
            path: PathBuf::from("/p/AGENTS.md"),
            content: "ctx".into(),
        }];
        let skills = vec![skill("alpha", false), skill("hidden", true)];
        let runtime = RuntimeMetadata {
            current_date: Some("2026-01-02".into()),
            ..Default::default()
        };
        let extra = vec!["Extra guideline".to_string()];
        let inputs = PromptInputs {
            custom_prompt: Some("CUSTOM BODY"),
            append_system_prompt: Some("APPENDED"),
            skills: &skills,
            custom_guidance: Some("GUIDE"),
            runtime: Some(&runtime),
            extra_guidelines: &extra,
            ..PromptInputs::new(&tools, &files, Path::new("/p"))
        };
        let prompt = build_prompt(&inputs);
        assert_eq!(
            prompt,
            "CUSTOM BODY\n\nAPPENDED\n\n# Project Context\n\nProject-specific instructions and guidelines:\n\n## /p/AGENTS.md\n\nctx\n\n\
             \n\nThe following skills provide specialized instructions for specific tasks.\n\
             Use the read tool to load a skill's file when the task matches its description.\n\
             When a skill file references a relative path, resolve it against the skill directory (parent of SKILL.md / dirname of the path) and use that absolute path in tool commands.\n\n\
             <available_skills>\n  <skill>\n    <name>alpha</name>\n    <description>alpha &amp; co</description>\n    <location>/s/alpha/SKILL.md</location>\n  </skill>\n</available_skills>\n\n\
             Additional guidance (applies after repository instructions):\nGUIDE\n\n\
             # Environment Context\n\n<environment_context>\n  <current_date>2026-01-02</current_date>\n</environment_context>"
        );

        let default = build_prompt(&PromptInputs {
            extra_guidelines: &extra,
            ..PromptInputs::new(&tools, &[], Path::new("/p"))
        });
        assert!(default.contains("- Extra guideline\n- Be concise in your responses"));
        assert!(default.ends_with("\nCurrent working directory: /p"));
    }
}
