//! Prompt templates: Markdown files exposed as `/name` slash commands.
//!
//! Ported from the archived qt (`harness/prompt_templates.cpp` and the
//! template expansion in `app/slash/slash_commands.cpp`). Templates are the
//! `*.md` files directly inside a prompt root (or the entries of a
//! `package.json` `pi.prompts` array). Frontmatter may set `description`
//! and `argument-hint`; otherwise the description is the first non-empty
//! line, cut to 60 characters.

use std::fs;
use std::path::{Path, PathBuf};

use super::fsutil::{
    expand_tilde, is_markdown, list_files, normalize_newlines, path_string,
    read_pi_manifest_entries, resolve_manifest_entry, trim, unquote,
};

const DESCRIPTION_MAX_CHARS: usize = 60;

/// A loaded prompt template.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PromptTemplate {
    pub root: PathBuf,
    pub path: PathBuf,
    /// File name without `.md`; the slash command name.
    pub name: String,
    pub description: String,
    pub argument_hint: Option<String>,
    pub content: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TemplateDiagnosticCode {
    EmptyRoot,
    RootDoesNotExist,
    RootIsNotDirectory,
    PromptFileNotReadable,
    PromptFileReadFailed,
    InvalidFrontmatter,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TemplateDiagnostic {
    pub code: TemplateDiagnosticCode,
    pub path: String,
    pub message: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PromptTemplateResources {
    pub roots: Vec<PathBuf>,
    pub templates: Vec<PromptTemplate>,
    pub diagnostics: Vec<TemplateDiagnostic>,
}

impl PromptTemplateResources {
    fn diag(&mut self, code: TemplateDiagnosticCode, path: String, message: String) {
        self.diagnostics.push(TemplateDiagnostic {
            code,
            path,
            message,
        });
    }

    pub fn find(&self, name: &str) -> Option<&PromptTemplate> {
        self.templates.iter().find(|t| t.name == name)
    }
}

struct ParsedTemplate {
    description: String,
    argument_hint: Option<String>,
    content: String,
}

fn derive_description(content: &str) -> String {
    for line in content.split('\n') {
        let line = trim(line);
        if !line.is_empty() {
            if line.chars().count() <= DESCRIPTION_MAX_CHARS {
                return line.to_string();
            }
            let cut: String = line.chars().take(DESCRIPTION_MAX_CHARS).collect();
            return format!("{cut}...");
        }
    }
    String::new()
}

fn parse_template(raw: &str) -> Result<ParsedTemplate, &'static str> {
    let text = normalize_newlines(raw);
    let mut lines = text.split('\n');
    if lines.next() != Some("---") {
        let content = trim(&text).to_string();
        return Ok(ParsedTemplate {
            description: derive_description(&content),
            argument_hint: None,
            content,
        });
    }
    let mut description = String::new();
    let mut argument_hint = None;
    let mut offset = text.find('\n').map_or(text.len(), |i| i + 1);
    let mut body_start = None;
    for line in lines {
        if line == "---" {
            body_start = Some((offset + line.len() + 1).min(text.len()));
            break;
        }
        let trimmed = trim(line);
        if !trimmed.is_empty() {
            let separator = trimmed
                .find(':')
                .ok_or("frontmatter line is missing ':' separator")?;
            let key = trim(&trimmed[..separator]);
            let value = unquote(trim(&trimmed[separator + 1..]));
            match key {
                "description" => description = value.to_string(),
                "argument-hint" if !value.is_empty() => argument_hint = Some(value.to_string()),
                _ => {}
            }
        }
        offset += line.len() + 1;
    }
    let start = body_start.ok_or("frontmatter block is missing closing delimiter")?;
    let content = trim(&text[start..]).to_string();
    if description.is_empty() {
        description = derive_description(&content);
    }
    Ok(ParsedTemplate {
        description,
        argument_hint,
        content,
    })
}

fn load_template_file(root: &Path, path: &Path, out: &mut PromptTemplateResources) {
    let prompt_path = path_string(path);
    if !path.is_file() {
        out.diag(
            TemplateDiagnosticCode::PromptFileNotReadable,
            prompt_path.clone(),
            format!("prompt template entry is not a readable file: {prompt_path}"),
        );
        return;
    }
    let Ok(content) = fs::read_to_string(path) else {
        out.diag(
            TemplateDiagnosticCode::PromptFileReadFailed,
            prompt_path.clone(),
            format!("failed to read prompt template file: {prompt_path}"),
        );
        return;
    };
    match parse_template(&content) {
        Ok(parsed) => {
            let file = path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            let name = file.strip_suffix(".md").unwrap_or(&file).to_string();
            out.templates.push(PromptTemplate {
                root: root.to_path_buf(),
                path: PathBuf::from(prompt_path),
                name,
                description: parsed.description,
                argument_hint: parsed.argument_hint,
                content: parsed.content,
            });
        }
        Err(reason) => out.diag(
            TemplateDiagnosticCode::InvalidFrontmatter,
            prompt_path.clone(),
            format!("invalid prompt template frontmatter: {prompt_path} ({reason})"),
        ),
    }
}

fn load_package_templates(
    root: &Path,
    directory: &Path,
    out: &mut PromptTemplateResources,
) -> bool {
    let package_json = directory.join("package.json");
    if !package_json.is_file() {
        return false;
    }
    let declared = match read_pi_manifest_entries(&package_json, "prompts") {
        Ok(entries) => entries,
        Err(message) => {
            let code = if message.starts_with("failed to read") {
                TemplateDiagnosticCode::PromptFileReadFailed
            } else {
                TemplateDiagnosticCode::InvalidFrontmatter
            };
            out.diag(code, path_string(&package_json), message);
            Vec::new()
        }
    };
    if declared.is_empty() {
        return false;
    }
    for entry in declared {
        let resolved = resolve_manifest_entry(directory, &entry);
        if resolved.is_dir() {
            for file in list_files(&resolved, is_markdown) {
                load_template_file(root, &file, out);
            }
        } else if resolved.is_file() && is_markdown(&resolved) {
            load_template_file(root, &resolved, out);
        } else {
            let p = path_string(&resolved);
            out.diag(
                TemplateDiagnosticCode::PromptFileNotReadable,
                p.clone(),
                format!("package prompt template entry does not exist: {p}"),
            );
        }
    }
    true
}

/// Load templates from `roots` in order.
pub fn load_prompt_templates(roots: &[PathBuf]) -> PromptTemplateResources {
    let mut out = PromptTemplateResources {
        roots: roots.to_vec(),
        ..Default::default()
    };
    for root in roots {
        if root.as_os_str().is_empty() {
            out.diag(
                TemplateDiagnosticCode::EmptyRoot,
                String::new(),
                "prompt template root was empty".to_string(),
            );
            continue;
        }
        let root_path = expand_tilde(&root.to_string_lossy());
        let resolved = path_string(&root_path);
        // A single template file (`--prompt-template file.md`, package
        // resources) is loaded on its own, like skill and theme files.
        if root_path.is_file() && is_markdown(&root_path) {
            let parent = root_path.parent().map(path_string).unwrap_or_default();
            load_template_file(Path::new(&parent), &root_path, &mut out);
            continue;
        }
        if !root_path.exists() {
            out.diag(
                TemplateDiagnosticCode::RootDoesNotExist,
                resolved.clone(),
                format!("prompt template root does not exist: {resolved}"),
            );
            continue;
        }
        if !root_path.is_dir() {
            out.diag(
                TemplateDiagnosticCode::RootIsNotDirectory,
                resolved.clone(),
                format!("prompt template root is not a directory: {resolved}"),
            );
            continue;
        }
        let resolved_root = PathBuf::from(&resolved);
        if load_package_templates(&resolved_root, &root_path, &mut out) {
            continue;
        }
        for file in list_files(&root_path, is_markdown) {
            load_template_file(&resolved_root, &file, &mut out);
        }
    }
    out
}

// ---- argument expansion ---------------------------------------------------

/// Split arguments like a shell, without escapes: spaces and tabs separate,
/// `'…'` and `"…"` group (quotes are removed).
pub fn parse_template_arguments(arguments: &str) -> Vec<String> {
    let mut values = Vec::new();
    let mut current = String::new();
    let mut quote: Option<char> = None;
    for c in arguments.chars() {
        if let Some(q) = quote {
            if c == q {
                quote = None;
            } else {
                current.push(c);
            }
            continue;
        }
        match c {
            '\'' | '"' => quote = Some(c),
            ' ' | '\t' => {
                if !current.is_empty() {
                    values.push(std::mem::take(&mut current));
                }
            }
            c => current.push(c),
        }
    }
    if !current.is_empty() {
        values.push(current);
    }
    values
}

/// Saturating decimal parse of an ASCII digit run.
fn parse_decimal(digits: &[u8]) -> usize {
    digits.iter().fold(0usize, |acc, d| {
        acc.saturating_mul(10).saturating_add(usize::from(d - b'0'))
    })
}

fn join_slice(args: &[String], start: usize, length: Option<usize>) -> String {
    if start >= args.len() {
        return String::new();
    }
    let available = args.len() - start;
    let end = start + length.map_or(available, |l| l.min(available));
    args[start..end].join(" ")
}

fn digit_run(bytes: &[u8], from: usize) -> usize {
    let mut end = from;
    while end < bytes.len() && bytes[end].is_ascii_digit() {
        end += 1;
    }
    end
}

/// Expand `$1`…`$N`, `$@`, `$ARGUMENTS`, `${@:N}` and `${@:N:L}` in one
/// left-to-right pass (substituted text is never re-expanded). Out-of-range
/// positions expand to nothing; huge indices saturate instead of overflowing.
pub fn expand_template_content(content: &str, args: &[String]) -> String {
    let all = args.join(" ");
    let bytes = content.as_bytes();
    let mut out = String::with_capacity(content.len() + all.len());
    let mut i = 0;
    let mut literal_start = 0;
    while i < bytes.len() {
        if bytes[i] != b'$' {
            i += 1;
            continue;
        }
        out.push_str(&content[literal_start..i]);
        let (replacement, next) = expand_dollar(content, i, args, &all);
        match replacement {
            Some(text) => {
                out.push_str(&text);
                i = next;
            }
            None => {
                out.push('$');
                i += 1;
            }
        }
        literal_start = i;
    }
    out.push_str(&content[literal_start..]);
    out
}

fn expand_dollar(content: &str, i: usize, args: &[String], all: &str) -> (Option<String>, usize) {
    let bytes = content.as_bytes();
    let rest = &content[i..];
    if bytes.get(i + 1).is_some_and(u8::is_ascii_digit) {
        let end = digit_run(bytes, i + 1);
        let position = parse_decimal(&bytes[i + 1..end]);
        let value = if position >= 1 && position <= args.len() {
            args[position - 1].clone()
        } else {
            String::new()
        };
        return (Some(value), end);
    }
    if rest.starts_with("$ARGUMENTS") {
        return (Some(all.to_string()), i + "$ARGUMENTS".len());
    }
    if rest.starts_with("$@") {
        return (Some(all.to_string()), i + 2);
    }
    if rest.starts_with("${@:") {
        let start_begin = i + 4;
        let mut cursor = digit_run(bytes, start_begin);
        if cursor == start_begin {
            return (None, i);
        }
        let start_position = parse_decimal(&bytes[start_begin..cursor]);
        let mut length = None;
        if bytes.get(cursor) == Some(&b':') {
            cursor += 1;
            let length_begin = cursor;
            cursor = digit_run(bytes, length_begin);
            if cursor == length_begin {
                return (None, i);
            }
            length = Some(parse_decimal(&bytes[length_begin..cursor]));
        }
        if bytes.get(cursor) == Some(&b'}') {
            let start = start_position.saturating_sub(1);
            return (Some(join_slice(args, start, length)), cursor + 1);
        }
    }
    (None, i)
}

/// Rewrite the legacy placeholders `${N}` and `{{N}}` to `$N`.
fn normalize_legacy_positionals(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut i = 0;
    let mut literal_start = 0;
    while i < bytes.len() {
        let (open, close): (&[u8], &[u8]) = match bytes[i] {
            b'$' if bytes.get(i + 1) == Some(&b'{') => (b"${", b"}"),
            b'{' if bytes.get(i + 1) == Some(&b'{') => (b"{{", b"}}"),
            _ => {
                i += 1;
                continue;
            }
        };
        let digits_start = i + open.len();
        let digits_end = digit_run(bytes, digits_start);
        if digits_end > digits_start && bytes[digits_end..].starts_with(close) {
            out.push_str(&text[literal_start..i]);
            out.push('$');
            out.push_str(&text[digits_start..digits_end]);
            i = digits_end + close.len();
            literal_start = i;
        } else {
            i += 1;
        }
    }
    out.push_str(&text[literal_start..]);
    out
}

/// Expand a template for `/name <arguments>` as the archived qt did:
/// `{{args}}` / `{{arguments}}` become the trimmed raw argument string,
/// `${N}` / `{{N}}` act like `$N`, then `$…` placeholders are expanded with
/// shell-like parsed arguments.
pub fn expand_prompt_template(template: &str, arguments: &str) -> String {
    let raw = trim(arguments);
    let args = parse_template_arguments(arguments);
    // Split on the raw-argument placeholders so substituted text is never
    // re-expanded.
    let mut out = String::with_capacity(template.len() + raw.len());
    let mut rest = template;
    loop {
        let next = ["{{args}}", "{{arguments}}"]
            .iter()
            .filter_map(|p| rest.find(p).map(|i| (i, p.len())))
            .min();
        match next {
            Some((index, len)) => {
                out.push_str(&expand_template_content(
                    &normalize_legacy_positionals(&rest[..index]),
                    &args,
                ));
                out.push_str(raw);
                rest = &rest[index + len..];
            }
            None => {
                out.push_str(&expand_template_content(
                    &normalize_legacy_positionals(rest),
                    &args,
                ));
                break;
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn a(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn argument_parsing() {
        assert_eq!(
            parse_template_arguments("one \"two three\"\t'four five'  six"),
            a(&["one", "two three", "four five", "six"])
        );
        assert_eq!(parse_template_arguments("a\"b c\"d"), a(&["ab cd"]));
        assert!(parse_template_arguments("   ").is_empty());
    }

    #[test]
    fn placeholder_expansion() {
        let args = a(&["a", "b", "c", "d"]);
        let e = |t: &str| expand_template_content(t, &args);
        assert_eq!(e("$1-$2-$5-$0"), "a-b--");
        assert_eq!(e("$@|$ARGUMENTS"), "a b c d|a b c d");
        assert_eq!(e("${@:2}"), "b c d");
        assert_eq!(e("${@:2:2}"), "b c");
        assert_eq!(e("${@:0:1}"), "a");
        assert_eq!(e("${@:9}"), "");
        assert_eq!(e("${@:2:0}"), "");
        assert_eq!(e("${@:}"), "${@:}");
        assert_eq!(e("${@:2:}"), "${@:2:}");
        assert_eq!(e("${@:2"), "${@:2");
        assert_eq!(e("cost $ and $x"), "cost $ and $x");
        assert_eq!(e("$99999999999999999999999999"), "");
        assert_eq!(e("${@:99999999999999999999999:99999999999999999999}"), "");
        assert_eq!(e("${@:1:99999999999999999999999}"), "a b c d");
        // Substituted text is not re-expanded.
        assert_eq!(expand_template_content("$1", &a(&["$2", "x"])), "$2");
        assert_eq!(e("é$1ü"), "éaü");
    }

    #[test]
    fn legacy_placeholders() {
        assert_eq!(
            expand_prompt_template(
                "Fix {{args}} / {{arguments}} / ${1} / {{2}} / $@",
                "  x 'y z'  "
            ),
            "Fix x 'y z' / x 'y z' / x / y z / x y z"
        );
        assert_eq!(expand_prompt_template("{{args}}", "$1 $@"), "$1 $@");
        assert_eq!(
            expand_prompt_template("${x} {{y}} {1}", "a"),
            "${x} {{y}} {1}"
        );
    }

    #[test]
    fn loads_templates_with_frontmatter_and_packages() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("prompts");
        fs::create_dir_all(root.join("sub")).unwrap();
        fs::write(
            root.join("review.md"),
            "---\ndescription: \"Review code\"\nargument-hint: <file>\n---\n\nReview $1 carefully.\n",
        )
        .unwrap();
        fs::write(
            root.join("plain.md"),
            "\n\n  First line of the template that is definitely longer than sixty chars\nmore",
        )
        .unwrap();
        fs::write(root.join("bad.md"), "---\nnot frontmatter\n---\nbody").unwrap();
        fs::write(root.join("open.md"), "---\ndescription: x\n").unwrap();
        fs::write(root.join("notes.txt"), "ignored").unwrap();
        fs::write(root.join("sub/nested.md"), "not scanned").unwrap();
        let res = load_prompt_templates(&[root.clone(), dir.path().join("none")]);
        let names: Vec<&str> = res.templates.iter().map(|t| t.name.as_str()).collect();
        assert_eq!(names, vec!["plain", "review"]);
        let review = res.find("review").unwrap();
        assert_eq!(review.description, "Review code");
        assert_eq!(review.argument_hint.as_deref(), Some("<file>"));
        assert_eq!(review.content, "Review $1 carefully.");
        let plain = res.find("plain").unwrap();
        assert_eq!(
            plain.description,
            "First line of the template that is definitely longer than si..."
        );
        let messages: Vec<&str> = res.diagnostics.iter().map(|d| d.message.as_str()).collect();
        assert!(messages.contains(&format!(
            "invalid prompt template frontmatter: {} (frontmatter line is missing ':' separator)",
            root.join("bad.md").display()
        ).as_str()));
        assert!(messages.contains(&format!(
            "invalid prompt template frontmatter: {} (frontmatter block is missing closing delimiter)",
            root.join("open.md").display()
        ).as_str()));
        assert!(
            messages.contains(
                &format!(
                    "prompt template root does not exist: {}",
                    dir.path().join("none").display()
                )
                .as_str()
            )
        );

        let pkg = dir.path().join("pkg");
        fs::create_dir_all(pkg.join("p")).unwrap();
        fs::write(
            pkg.join("package.json"),
            r#"{"pi":{"prompts":["p","gone.md"]}}"#,
        )
        .unwrap();
        fs::write(pkg.join("p/one.md"), "One").unwrap();
        fs::write(pkg.join("top.md"), "not declared").unwrap();
        let res = load_prompt_templates(std::slice::from_ref(&pkg));
        let names: Vec<&str> = res.templates.iter().map(|t| t.name.as_str()).collect();
        assert_eq!(names, vec!["one"]);
        assert_eq!(
            res.diagnostics[0].code,
            TemplateDiagnosticCode::PromptFileNotReadable
        );

        // A single file root loads just that template.
        let res = load_prompt_templates(&[pkg.join("p/one.md"), pkg.join("package.json")]);
        let names: Vec<&str> = res.templates.iter().map(|t| t.name.as_str()).collect();
        assert_eq!(names, vec!["one"]);
        assert_eq!(res.templates[0].root, pkg.join("p"));
        assert_eq!(
            res.diagnostics[0].code,
            TemplateDiagnosticCode::RootIsNotDirectory
        );
    }
}
