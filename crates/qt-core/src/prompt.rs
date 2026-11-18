//! System prompt construction (mirrors Pi's `buildSystemPrompt`).

use std::path::{Path, PathBuf};

use crate::config::AgentHome;
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

pub fn build_system_prompt(
    tools: &ToolRegistry,
    context_files: &[ContextFile],
    cwd: &Path,
) -> String {
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
    add("Be concise in your responses");
    add("Show file paths clearly when working with files");
    let guidelines = guidelines
        .iter()
        .map(|g| format!("- {g}"))
        .collect::<Vec<_>>()
        .join("\n");

    let mut prompt = format!(
        "You are an expert coding assistant operating inside qt, a coding agent harness. You help users by reading files, executing commands, editing code, and writing new files.\n\n\
         Available tools:\n{tools_list}\n\n\
         In addition to the tools above, you may have access to other custom tools depending on the project.\n\n\
         Guidelines:\n{guidelines}"
    );
    if !context_files.is_empty() {
        prompt
            .push_str("\n\n# Project Context\n\nProject-specific instructions and guidelines:\n\n");
        for file in context_files {
            prompt.push_str(&format!(
                "## {}\n\n{}\n\n",
                file.path.display(),
                file.content
            ));
        }
    }
    let date = chrono::Local::now().format("%A, %B %-d, %Y");
    prompt.push_str(&format!("\nCurrent date: {date}"));
    prompt.push_str(&format!("\nCurrent working directory: {}", cwd.display()));
    prompt
}

#[cfg(test)]
mod tests {
    use super::*;

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

        let prompt = build_system_prompt(&ToolRegistry::coding(), &files, &project);
        assert!(prompt.contains("- read: Read file contents"));
        assert!(prompt.contains("Prefer grep/find/ls tools over bash"));
        assert!(prompt.contains("Use edit for precise changes"));
        assert!(prompt.contains("project rules"));
        assert!(prompt.ends_with(&format!("Current working directory: {}", project.display())));
    }
}
