//! Skills: discovery, frontmatter, ignore files, collisions, the
//! `<available_skills>` prompt block and `/skill:<name>` expansion.
//!
//! Ported from the archived qt (`harness/skills.cpp`), which follows Pi's
//! Agent Skills layout: a directory containing `SKILL.md` is one skill (its
//! subdirectories are not scanned), `*.md` files directly in a skill root are
//! skills too (except in `.agents/skills` roots), and a `package.json` with a
//! `pi.skills` array replaces the scan of its directory.

use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use regex::Regex;

use super::fsutil::{
    canonical_or_normal, expand_tilde, file_name, generic, is_markdown, list_files,
    list_subdirectories, normalize_newlines, path_string, read_pi_manifest_entries, relative_to,
    resolve_manifest_entry, trim, unquote,
};

pub const MAX_SKILL_NAME_LENGTH: usize = 64;
pub const MAX_SKILL_DESCRIPTION_LENGTH: usize = 1024;
const IGNORE_FILE_NAMES: [&str; 3] = [".gitignore", ".ignore", ".fdignore"];

/// A loaded skill.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Skill {
    /// The skill root the skill was found under.
    pub root: PathBuf,
    /// The skill's base directory (parent of its file).
    pub directory: PathBuf,
    /// The skill file (`SKILL.md` or a root-level `*.md`).
    pub path: PathBuf,
    pub name: String,
    pub description: String,
    /// The file body after the frontmatter, trimmed.
    pub content: String,
    /// `disable-model-invocation: true` hides the skill from the prompt.
    pub disable_model_invocation: bool,
}

/// Skills loaded from a list of roots, with diagnostics.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SkillResources {
    pub roots: Vec<PathBuf>,
    pub skills: Vec<Skill>,
    pub diagnostics: Vec<String>,
}

impl SkillResources {
    fn diag(&mut self, message: String) {
        if !message.is_empty() {
            self.diagnostics.push(message);
        }
    }

    pub fn find(&self, name: &str) -> Option<&Skill> {
        self.skills.iter().find(|s| s.name == name)
    }
}

#[derive(Default)]
struct ParsedSkill {
    had_frontmatter: bool,
    name: Option<String>,
    description: Option<String>,
    disable_model_invocation: bool,
    body: String,
}

fn parse_skill_document(raw: &str, skill_path: &str, diagnostics: &mut Vec<String>) -> ParsedSkill {
    let mut doc = ParsedSkill::default();
    let text = normalize_newlines(raw);
    let mut lines = text.split('\n');
    if lines.next() != Some("---") {
        doc.body = trim(&text).to_string();
        return doc;
    }
    doc.had_frontmatter = true;
    let mut offset = text.find('\n').map_or(text.len(), |i| i + 1);
    let mut closed = false;
    for line in lines {
        let line_len = line.len();
        if line == "---" {
            closed = true;
            let after = offset + line_len;
            doc.body = if after < text.len() {
                trim(&text[after + 1..]).to_string()
            } else {
                String::new()
            };
            break;
        }
        let trimmed = trim(line);
        if !trimmed.is_empty() && !trimmed.starts_with('#') {
            let Some(separator) = trimmed.find(':') else {
                diagnostics.push(format!(
                    "failed to parse skill file: {skill_path} (frontmatter line is missing ':' separator)"
                ));
                return doc;
            };
            let key = trim(&trimmed[..separator]);
            let value = unquote(trim(&trimmed[separator + 1..]));
            match key {
                "name" if !value.is_empty() => doc.name = Some(value.to_string()),
                "description" => doc.description = Some(value.to_string()),
                "disable-model-invocation" => doc.disable_model_invocation = value == "true",
                _ => {}
            }
        }
        offset += line_len + 1;
    }
    if !closed {
        diagnostics.push(format!(
            "failed to parse skill file: {skill_path} (frontmatter block is missing closing delimiter)"
        ));
    }
    doc
}

fn name_diagnostics(name: &str, parent: &str, skill_path: &str, diagnostics: &mut Vec<String>) {
    if name != parent {
        diagnostics.push(format!(
            "name \"{name}\" does not match parent directory \"{parent}\": {skill_path}"
        ));
    }
    let len = name.chars().count();
    if len > MAX_SKILL_NAME_LENGTH {
        diagnostics.push(format!("name exceeds 64 characters ({len}): {skill_path}"));
    }
    if !name
        .chars()
        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
    {
        diagnostics.push(format!(
            "name contains invalid characters (must be lowercase a-z, 0-9, hyphens only): {skill_path}"
        ));
    }
    if !name.is_empty() && (name.starts_with('-') || name.ends_with('-')) {
        diagnostics.push(format!(
            "name must not start or end with a hyphen: {skill_path}"
        ));
    }
    if name.contains("--") {
        diagnostics.push(format!(
            "name must not contain consecutive hyphens: {skill_path}"
        ));
    }
}

fn load_skill_file(root: &Path, file: &Path, out: &mut SkillResources) {
    let skill_path = path_string(file);
    if !file.is_file() {
        out.diag(format!("skill entry is not a readable file: {skill_path}"));
        return;
    }
    let Ok(content) = fs::read_to_string(file) else {
        out.diag(format!("failed to read skill file: {skill_path}"));
        return;
    };
    let standard = file_name(file) == "SKILL.md";
    let directory = file.parent().map(Path::to_path_buf).unwrap_or_default();
    let parent_name = file_name(&directory);
    let mut diagnostics = Vec::new();
    let parsed = parse_skill_document(&content, &skill_path, &mut diagnostics);
    let name = parsed.name.clone().unwrap_or_else(|| {
        if standard {
            parent_name.clone()
        } else {
            file.file_stem()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_default()
        }
    });
    let description = parsed.description.clone().unwrap_or_default();
    if parsed.had_frontmatter {
        name_diagnostics(&name, &parent_name, &skill_path, &mut diagnostics);
        if trim(&description).is_empty() {
            diagnostics.push(format!("description is required: {skill_path}"));
            out.diagnostics.extend(diagnostics);
            return;
        }
        let len = description.chars().count();
        if len > MAX_SKILL_DESCRIPTION_LENGTH {
            diagnostics.push(format!(
                "description exceeds 1024 characters ({len}): {skill_path}"
            ));
        }
    }
    out.diagnostics.extend(diagnostics);
    out.skills.push(Skill {
        root: root.to_path_buf(),
        directory: PathBuf::from(path_string(&directory)),
        path: PathBuf::from(skill_path),
        name,
        description,
        content: parsed.body,
        disable_model_invocation: parsed.disable_model_invocation,
    });
}

// ---- ignore files ---------------------------------------------------------

fn display_path(path: &Path, root: &Path) -> String {
    let rel = relative_to(path, root);
    if rel.as_os_str().is_empty() {
        return file_name(path);
    }
    generic(&rel)
}

fn prefixed_ignore_pattern(line: &str, prefix: &str) -> Option<String> {
    let mut line = trim(line);
    if line.is_empty() || line.starts_with('#') {
        return None;
    }
    let mut negated = false;
    if let Some(rest) = line.strip_prefix('!') {
        negated = true;
        line = rest;
    } else if line.starts_with("\\!") || line.starts_with("\\#") {
        line = &line[1..];
    }
    let line = line.strip_prefix('/').unwrap_or(line);
    if line.is_empty() {
        return None;
    }
    let pattern = format!("{prefix}{line}");
    Some(if negated {
        format!("!{pattern}")
    } else {
        pattern
    })
}

fn add_ignore_patterns(directory: &Path, root: &Path, patterns: &mut Vec<String>) {
    let prefix = if directory == root {
        String::new()
    } else {
        format!("{}/", display_path(directory, root))
    };
    for name in IGNORE_FILE_NAMES {
        let path = directory.join(name);
        if !path.is_file() {
            continue;
        }
        let Ok(content) = fs::read_to_string(&path) else {
            continue;
        };
        for line in content.split('\n') {
            let line = line.strip_suffix('\r').unwrap_or(line);
            if let Some(p) = prefixed_ignore_pattern(line, &prefix) {
                patterns.push(p);
            }
        }
    }
}

/// The skills loader's glob: a leading `**/` matches any directory prefix.
fn ignore_glob(pattern: &str) -> Option<Regex> {
    let mut expr = String::from("^");
    let rest = if let Some(rest) = pattern.strip_prefix("**/") {
        expr.push_str("(?:.*/)?");
        rest
    } else {
        pattern
    };
    let chars: Vec<char> = rest.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        match chars[i] {
            '*' if chars.get(i + 1) == Some(&'*') => {
                expr.push_str(".*");
                i += 1;
            }
            '*' => expr.push_str("[^/]*"),
            '?' => expr.push_str("[^/]"),
            c => expr.push_str(&regex::escape(&c.to_string())),
        }
        i += 1;
    }
    expr.push('$');
    Regex::new(&expr).ok()
}

fn ignore_pattern_matches(display: &str, is_dir: bool, pattern: &str) -> bool {
    let normalized = pattern.trim_start_matches('/');
    if normalized.is_empty() {
        return false;
    }
    if let Some(dir_pattern) = normalized.strip_suffix('/') {
        return (is_dir && display == dir_pattern)
            || display.starts_with(&format!("{dir_pattern}/"));
    }
    let Some(regex) = ignore_glob(normalized) else {
        return false;
    };
    if regex.is_match(display) {
        return true;
    }
    if !normalized.contains('/') {
        let base = display.rsplit('/').next().unwrap_or(display);
        return regex.is_match(base);
    }
    false
}

fn ignored(path: &Path, root: &Path, is_dir: bool, patterns: &[String]) -> bool {
    let display = display_path(path, root);
    let mut result = false;
    for pattern in patterns {
        let (negated, effective) = match pattern.strip_prefix('!') {
            Some(rest) => (true, rest),
            None => (false, pattern.as_str()),
        };
        if !effective.is_empty() && ignore_pattern_matches(&display, is_dir, effective) {
            result = !negated;
        }
    }
    result
}

// ---- directory walk -------------------------------------------------------

fn load_skill_directory(
    root: &Path,
    directory: &Path,
    scan_root: &Path,
    include_root_files: bool,
    mut patterns: Vec<String>,
    out: &mut SkillResources,
) {
    add_ignore_patterns(directory, scan_root, &mut patterns);

    let package_json = directory.join("package.json");
    if package_json.is_file() {
        let declared = match read_pi_manifest_entries(&package_json, "skills") {
            Ok(entries) => entries,
            Err(message) => {
                out.diag(message);
                Vec::new()
            }
        };
        if !declared.is_empty() {
            for entry in declared {
                let resolved = resolve_manifest_entry(directory, &entry);
                if ignored(&resolved, scan_root, resolved.is_dir(), &patterns) {
                    continue;
                }
                if resolved.is_dir() {
                    load_skill_directory(root, &resolved, scan_root, true, patterns.clone(), out);
                } else if resolved.is_file() && is_markdown(&resolved) {
                    load_skill_file(root, &resolved, out);
                } else {
                    out.diag(format!(
                        "package skill entry does not exist: {}",
                        path_string(&resolved)
                    ));
                }
            }
            return;
        }
    }

    let skill_file = directory.join("SKILL.md");
    if skill_file.exists() {
        if !ignored(&skill_file, scan_root, false, &patterns) {
            load_skill_file(root, &skill_file, out);
        }
        return;
    }

    if include_root_files {
        for file in list_files(directory, is_markdown) {
            if !ignored(&file, scan_root, false, &patterns) {
                load_skill_file(root, &file, out);
            }
        }
    }

    for child in list_subdirectories(directory) {
        if !ignored(&child, scan_root, true, &patterns) {
            load_skill_directory(root, &child, scan_root, false, patterns.clone(), out);
        }
    }
}

fn is_agents_skills_root(root: &Path) -> bool {
    file_name(root) == "skills" && root.parent().is_some_and(|p| file_name(p) == ".agents")
}

/// Load skills from `roots` in order. Earlier roots win name collisions.
pub fn load_skills(roots: &[PathBuf]) -> SkillResources {
    let mut out = SkillResources {
        roots: roots.to_vec(),
        ..Default::default()
    };
    for root in roots {
        if root.as_os_str().is_empty() {
            out.diag("skill root was empty".to_string());
            continue;
        }
        let root_path = expand_tilde(&root.to_string_lossy());
        let resolved = path_string(&root_path);
        if root_path.is_file() && is_markdown(&root_path) {
            let parent = root_path
                .parent()
                .map(Path::to_path_buf)
                .unwrap_or_default();
            load_skill_file(&PathBuf::from(path_string(&parent)), &root_path, &mut out);
            continue;
        }
        if !root_path.exists() {
            out.diag(format!("skill root does not exist: {resolved}"));
            continue;
        }
        if !root_path.is_dir() {
            out.diag(format!("skill root is not a directory: {resolved}"));
            continue;
        }
        load_skill_directory(
            &PathBuf::from(&resolved),
            &root_path,
            &root_path,
            !is_agents_skills_root(&root_path),
            Vec::new(),
            &mut out,
        );
    }

    let mut seen_paths: HashSet<PathBuf> = HashSet::new();
    let mut winners: HashMap<String, PathBuf> = HashMap::new();
    let mut kept = Vec::with_capacity(out.skills.len());
    for skill in std::mem::take(&mut out.skills) {
        let canonical = canonical_or_normal(&skill.path);
        if seen_paths.contains(&canonical) {
            continue;
        }
        if let Some(winner) = winners.get(&skill.name) {
            out.diagnostics.push(format!(
                "name \"{}\" collision: {} (winner: {})",
                skill.name,
                skill.path.display(),
                winner.display()
            ));
            continue;
        }
        seen_paths.insert(canonical);
        winners.insert(skill.name.clone(), skill.path.clone());
        kept.push(skill);
    }
    out.skills = kept;
    out
}

/// XML-escape for prompt blocks.
pub fn escape_xml(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    for c in value.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&apos;"),
            c => out.push(c),
        }
    }
    out
}

/// The `<available_skills>` block appended to the system prompt, starting
/// with two newlines. Empty when no skill is visible to the model.
pub fn format_skills_for_prompt(skills: &[Skill]) -> String {
    let visible: Vec<&Skill> = skills
        .iter()
        .filter(|s| !s.disable_model_invocation)
        .collect();
    if visible.is_empty() {
        return String::new();
    }
    let mut lines = vec![
        "\n\nThe following skills provide specialized instructions for specific tasks.".to_string(),
        "Use the read tool to load a skill's file when the task matches its description.".to_string(),
        "When a skill file references a relative path, resolve it against the skill directory (parent of SKILL.md / dirname of the path) and use that absolute path in tool commands.".to_string(),
        String::new(),
        "<available_skills>".to_string(),
    ];
    for skill in visible {
        lines.push("  <skill>".to_string());
        lines.push(format!("    <name>{}</name>", escape_xml(&skill.name)));
        lines.push(format!(
            "    <description>{}</description>",
            escape_xml(&skill.description)
        ));
        lines.push(format!(
            "    <location>{}</location>",
            escape_xml(&generic(&skill.path))
        ));
        lines.push("  </skill>".to_string());
    }
    lines.push("</available_skills>".to_string());
    lines.join("\n")
}

/// The user message sent for `/skill:<name> [args]`: the skill body (or
/// `Use the skill "<name>".` when the body is empty), followed by
/// `\n\nUser: <args>` when arguments were given.
pub fn expand_skill_command(skill: &Skill, arguments: &str) -> String {
    let mut output = if skill.content.is_empty() {
        format!("Use the skill \"{}\".", skill.name)
    } else {
        skill.content.clone()
    };
    let args = trim(arguments);
    if !args.is_empty() {
        output.push_str("\n\nUser: ");
        output.push_str(args);
    }
    output
}

/// Slash-command description for a skill.
pub fn skill_command_description(skill: &Skill) -> String {
    if skill.description.is_empty() {
        format!("Use skill {}", skill.name)
    } else {
        skill.description.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(path: &Path, text: &str) {
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, text).unwrap();
    }

    fn skill_md(name: &str, description: &str) -> String {
        format!("---\nname: {name}\ndescription: \"{description}\"\n---\n\n# Body of {name}\n")
    }

    #[test]
    fn discovers_skill_dirs_root_files_and_frontmatter() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("skills");
        write(
            &root.join("alpha/SKILL.md"),
            &skill_md("alpha", "Alpha <tool>"),
        );
        // Nested skills under a SKILL.md directory are not scanned.
        write(&root.join("alpha/inner/SKILL.md"), &skill_md("inner", "x"));
        write(&root.join("group/beta/SKILL.md"), &skill_md("beta", "Beta"));
        write(&root.join("loose.md"), "Just a body");
        // Root-level *.md only at the top of the root.
        write(&root.join("group/notes.md"), "ignored");
        write(
            &root.join("hidden/SKILL.md"),
            "---\nname: hidden\ndescription: Hidden\ndisable-model-invocation: true\n---\nbody",
        );
        let res = load_skills(std::slice::from_ref(&root));
        let names: Vec<&str> = res.skills.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(names, vec!["loose", "alpha", "beta", "hidden"]);
        let alpha = res.find("alpha").unwrap();
        assert_eq!(alpha.description, "Alpha <tool>");
        assert_eq!(alpha.content, "# Body of alpha");
        assert_eq!(alpha.directory, root.join("alpha"));
        let loose = res.find("loose").unwrap();
        assert_eq!(loose.description, "");
        assert_eq!(loose.content, "Just a body");
        assert!(res.find("hidden").unwrap().disable_model_invocation);
        assert!(res.diagnostics.is_empty(), "{:?}", res.diagnostics);

        let block = format_skills_for_prompt(&res.skills);
        assert!(block.starts_with(
            "\n\nThe following skills provide specialized instructions for specific tasks.\n"
        ));
        assert!(block.contains(&format!(
            "  <skill>\n    <name>alpha</name>\n    <description>Alpha &lt;tool&gt;</description>\n    <location>{}</location>\n  </skill>",
            root.join("alpha/SKILL.md").display()
        )));
        assert!(!block.contains("hidden"));
        assert!(block.ends_with("</available_skills>"));
    }

    #[test]
    fn agents_skills_root_skips_root_markdown() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join(".agents/skills");
        write(&root.join("loose.md"), "body");
        write(&root.join("gamma/SKILL.md"), &skill_md("gamma", "G"));
        let res = load_skills(std::slice::from_ref(&root));
        let names: Vec<&str> = res.skills.iter().map(|s| s.name.as_str()).collect();
        assert_eq!(names, vec!["gamma"]);
    }

    #[test]
    fn diagnostics_and_collisions() {
        let dir = tempfile::tempdir().unwrap();
        let a = dir.path().join("a");
        let b = dir.path().join("b");
        write(&a.join("dup/SKILL.md"), &skill_md("dup", "first"));
        write(&b.join("dup/SKILL.md"), &skill_md("dup", "second"));
        write(&a.join("Bad--Name-/SKILL.md"), &skill_md("Bad--Name-", "d"));
        write(&a.join("nodesc/SKILL.md"), "---\nname: nodesc\n---\nbody");
        write(
            &a.join("long/SKILL.md"),
            &skill_md("long", &"x".repeat(1025)),
        );
        write(&a.join("broken/SKILL.md"), "---\nname broken\n---\n");
        write(
            &a.join("open/SKILL.md"),
            "---\nname: open\ndescription: d\n",
        );
        write(&a.join("mismatch/SKILL.md"), &skill_md("other", "d"));
        let missing = dir.path().join("missing");
        let res = load_skills(&[a.clone(), b.clone(), PathBuf::new(), missing.clone()]);
        let d = &res.diagnostics;
        let has = |s: &str| d.iter().any(|x| x.contains(s));
        assert!(has(&format!(
            "name \"dup\" collision: {} (winner: {})",
            b.join("dup/SKILL.md").display(),
            a.join("dup/SKILL.md").display()
        )));
        assert!(has(
            "name contains invalid characters (must be lowercase a-z, 0-9, hyphens only)"
        ));
        assert!(has("name must not start or end with a hyphen"));
        assert!(has("name must not contain consecutive hyphens"));
        assert!(has(&format!(
            "description is required: {}",
            a.join("nodesc/SKILL.md").display()
        )));
        assert!(has("description exceeds 1024 characters (1025)"));
        assert!(has("(frontmatter line is missing ':' separator)"));
        assert!(has("(frontmatter block is missing closing delimiter)"));
        assert!(has(
            "name \"other\" does not match parent directory \"mismatch\""
        ));
        assert!(has("skill root was empty"));
        assert!(has(&format!(
            "skill root does not exist: {}",
            missing.display()
        )));
        assert_eq!(res.find("dup").unwrap().description, "first");
        assert!(res.find("nodesc").is_none());
        assert!(res.find("long").is_some());
    }

    #[test]
    fn ignore_files_and_package_manifests() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("skills");
        write(
            &root.join(".gitignore"),
            "# comment\nbuild/\nsecret\n*.tmp.md\n",
        );
        write(&root.join("build/x/SKILL.md"), &skill_md("x", "d"));
        write(&root.join("secret/SKILL.md"), &skill_md("secret", "d"));
        write(&root.join("scratch.tmp.md"), "tmp");
        write(&root.join("keep/SKILL.md"), &skill_md("keep", "d"));
        write(&root.join("keep/.ignore"), "");
        write(&root.join("nested/.fdignore"), "drop\n");
        write(&root.join("nested/drop/SKILL.md"), &skill_md("drop", "d"));
        write(&root.join("nested/stay/SKILL.md"), &skill_md("stay", "d"));
        write(
            &root.join("pkg/package.json"),
            r#"{"name":"p","pi":{"skills":["./declared","one.md","missing"]}}"#,
        );
        write(
            &root.join("pkg/declared/SKILL.md"),
            &skill_md("declared", "d"),
        );
        write(&root.join("pkg/one.md"), "one body");
        write(
            &root.join("pkg/undeclared/SKILL.md"),
            &skill_md("undeclared", "d"),
        );
        write(&root.join("badpkg/package.json"), "{nope");
        write(&root.join("badpkg/fine/SKILL.md"), &skill_md("fine", "d"));
        let res = load_skills(std::slice::from_ref(&root));
        let mut names: Vec<&str> = res.skills.iter().map(|s| s.name.as_str()).collect();
        names.sort();
        assert_eq!(names, vec!["declared", "fine", "keep", "one", "stay"]);
        assert!(res.diagnostics.iter().any(|d| d
            == &format!(
                "package skill entry does not exist: {}",
                root.join("pkg/missing").display()
            )));
        assert!(
            res.diagnostics
                .iter()
                .any(|d| d.starts_with("failed to decode package manifest: "))
        );
    }

    #[test]
    fn single_file_root_and_skill_command() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("tool.md");
        write(&file, "Do the thing.");
        let res = load_skills(std::slice::from_ref(&file));
        let skill = &res.skills[0];
        assert_eq!(skill.name, "tool");
        assert_eq!(skill.root, dir.path());
        assert_eq!(
            expand_skill_command(skill, "  now please "),
            "Do the thing.\n\nUser: now please"
        );
        assert_eq!(expand_skill_command(skill, " "), "Do the thing.");
        let empty = Skill {
            content: String::new(),
            ..skill.clone()
        };
        assert_eq!(expand_skill_command(&empty, ""), "Use the skill \"tool\".");
        assert_eq!(skill_command_description(skill), "Use skill tool");
    }
}
