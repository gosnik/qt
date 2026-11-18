//! `SYSTEM.md` / `APPEND_SYSTEM.md` and the `--system-prompt` /
//! `--append-system-prompt` inputs.
//!
//! As in the archived qt, the project file `<cwd>/.pi/SYSTEM.md` wins over
//! `<agent_home>/SYSTEM.md`, and likewise for `APPEND_SYSTEM.md`.
//! `--system-prompt` replaces the discovered system prompt, and the
//! `--append-system-prompt` values (joined with newlines) are appended to the
//! discovered append text. As in Pi, a CLI value naming an existing file is
//! replaced by that file's contents.

use std::fs;
use std::path::{Path, PathBuf};

use super::fsutil::{path_string, resolve_user_path};

/// Source label of a `--system-prompt` contribution.
pub const CLI_SYSTEM_PROMPT_SOURCE: &str = "<cli:system-prompt>";
/// Source label of a `--append-system-prompt` contribution.
pub const CLI_APPEND_SYSTEM_PROMPT_SOURCE: &str = "<cli:append-system-prompt>";

/// Text contributed to the system prompt, with where it came from (a file
/// path or one of the `<cli:…>` labels).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PromptContribution {
    pub source: String,
    pub content: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PromptContributions {
    /// Replaces the default system prompt body.
    pub system_prompt: Option<PromptContribution>,
    /// Appended after the system prompt body.
    pub append_system_prompt: Option<PromptContribution>,
    pub diagnostics: Vec<String>,
}

fn load_optional(
    path: &Path,
    label: &str,
    diagnostics: &mut Vec<String>,
) -> Option<PromptContribution> {
    if !path.is_file() {
        return None;
    }
    match fs::read_to_string(path) {
        Ok(content) => Some(PromptContribution {
            source: path_string(path),
            content,
        }),
        Err(_) => {
            diagnostics.push(format!(
                "failed to read {label} file: {}",
                path_string(path)
            ));
            None
        }
    }
}

/// Discover `SYSTEM.md` and `APPEND_SYSTEM.md` for `cwd`.
pub fn load_prompt_contributions(cwd: &Path, agent_home: Option<&Path>) -> PromptContributions {
    let mut out = PromptContributions::default();
    let project = cwd.join(".pi");
    out.system_prompt = load_optional(
        &project.join("SYSTEM.md"),
        "project system prompt",
        &mut out.diagnostics,
    );
    if out.system_prompt.is_none()
        && let Some(home) = agent_home
    {
        out.system_prompt = load_optional(
            &home.join("SYSTEM.md"),
            "global system prompt",
            &mut out.diagnostics,
        );
    }
    out.append_system_prompt = load_optional(
        &project.join("APPEND_SYSTEM.md"),
        "project append system prompt",
        &mut out.diagnostics,
    );
    if out.append_system_prompt.is_none()
        && let Some(home) = agent_home
    {
        out.append_system_prompt = load_optional(
            &home.join("APPEND_SYSTEM.md"),
            "global append system prompt",
            &mut out.diagnostics,
        );
    }
    out
}

/// A CLI prompt value: the contents of the file it names (resolved against
/// `cwd`), else the text itself.
pub fn resolve_prompt_input(
    input: &str,
    cwd: &Path,
    description: &str,
    diagnostics: &mut Vec<String>,
) -> String {
    if input.is_empty() || input.contains('\n') {
        return input.to_string();
    }
    let path: PathBuf = resolve_user_path(input, cwd);
    if !path.is_file() {
        return input.to_string();
    }
    match fs::read_to_string(&path) {
        Ok(content) => content,
        Err(err) => {
            diagnostics.push(format!(
                "Could not read {description} file {}: {err}",
                path_string(&path)
            ));
            input.to_string()
        }
    }
}

/// Apply `--system-prompt` and `--append-system-prompt` values.
pub fn apply_cli_overrides(
    contributions: &mut PromptContributions,
    system_prompt: Option<&str>,
    append_system_prompt: &[String],
    cwd: &Path,
) {
    if let Some(text) = system_prompt.filter(|t| !t.is_empty()) {
        let content =
            resolve_prompt_input(text, cwd, "system prompt", &mut contributions.diagnostics);
        contributions.system_prompt = Some(PromptContribution {
            source: CLI_SYSTEM_PROMPT_SOURCE.to_string(),
            content,
        });
    }
    let resolved: Vec<String> = append_system_prompt
        .iter()
        .filter(|line| !line.is_empty())
        .map(|line| {
            resolve_prompt_input(
                line,
                cwd,
                "append system prompt",
                &mut contributions.diagnostics,
            )
        })
        .filter(|line| !line.is_empty())
        .collect();
    if resolved.is_empty() {
        return;
    }
    let joined = resolved.join("\n");
    match contributions.append_system_prompt.as_mut() {
        Some(existing) if !existing.content.is_empty() => {
            existing.content.push('\n');
            existing.content.push_str(&joined);
        }
        _ => {
            contributions.append_system_prompt = Some(PromptContribution {
                source: CLI_APPEND_SYSTEM_PROMPT_SOURCE.to_string(),
                content: joined,
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn project_files_win_over_agent_home() {
        let dir = tempfile::tempdir().unwrap();
        let home = dir.path().join("home");
        let cwd = dir.path().join("proj");
        fs::create_dir_all(cwd.join(".pi")).unwrap();
        fs::create_dir_all(&home).unwrap();
        fs::write(home.join("SYSTEM.md"), "global system").unwrap();
        fs::write(home.join("APPEND_SYSTEM.md"), "global append").unwrap();
        let c = load_prompt_contributions(&cwd, Some(&home));
        assert_eq!(c.system_prompt.as_ref().unwrap().content, "global system");
        assert_eq!(
            c.append_system_prompt.as_ref().unwrap().content,
            "global append"
        );

        fs::write(cwd.join(".pi/SYSTEM.md"), "project system").unwrap();
        fs::write(cwd.join(".pi/APPEND_SYSTEM.md"), "project append").unwrap();
        let mut c = load_prompt_contributions(&cwd, Some(&home));
        assert_eq!(c.system_prompt.as_ref().unwrap().content, "project system");
        assert_eq!(
            c.system_prompt.as_ref().unwrap().source,
            path_string(&cwd.join(".pi/SYSTEM.md"))
        );

        fs::write(cwd.join("extra.md"), "from file").unwrap();
        apply_cli_overrides(
            &mut c,
            Some("cli system"),
            &["one".to_string(), String::new(), "extra.md".to_string()],
            &cwd,
        );
        let system = c.system_prompt.unwrap();
        assert_eq!(system.source, CLI_SYSTEM_PROMPT_SOURCE);
        assert_eq!(system.content, "cli system");
        assert_eq!(
            c.append_system_prompt.unwrap().content,
            "project append\none\nfrom file"
        );
    }

    #[test]
    fn cli_append_without_discovered_file() {
        let dir = tempfile::tempdir().unwrap();
        let mut c = load_prompt_contributions(dir.path(), None);
        assert!(c.system_prompt.is_none() && c.append_system_prompt.is_none());
        fs::write(dir.path().join("sys.txt"), "file system prompt").unwrap();
        apply_cli_overrides(&mut c, Some("sys.txt"), &["tail".into()], dir.path());
        assert_eq!(c.system_prompt.unwrap().content, "file system prompt");
        let append = c.append_system_prompt.unwrap();
        assert_eq!(append.source, CLI_APPEND_SYSTEM_PROMPT_SOURCE);
        assert_eq!(append.content, "tail");
    }
}
