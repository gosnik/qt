//! Session export: HTML (archived `app/export/export_html.cpp`), JSONL,
//! `/share` through a private GitHub gist, and the last assistant text for
//! `/copy`.

use std::collections::{BTreeSet, HashMap};
use std::path::{Path, PathBuf};

use anyhow::{Context as _, Result, anyhow, bail};
use serde_json::Value;

use crate::message::{
    AgentMessage, AssistantMessage, ContentBlock, Message, StopReason, UserContent,
};
use crate::session::{EntryKind, SessionEntry, SessionHeader};

/// Viewer used for share links unless `PI_SHARE_VIEWER_URL` is set.
pub const DEFAULT_SHARE_VIEWER_URL: &str = "https://pi.dev/session/";

const SKILL_PREFIX: &str = "<skill name=\"";
const LOCATION_MARKER: &str = "\" location=\"";
const SKILL_OPEN_END: &str = "\">\n";
const SKILL_CLOSE: &str = "\n</skill>";
const ANSI_PREFIX: &str = "\x1b[";

/// HTML-escape `& < > " '`.
pub fn html_escape(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for ch in text.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#039;"),
            other => out.push(other),
        }
    }
    out
}

/// `javascript:`, `vbscript:` and `data:` URLs are removed from rendered
/// markdown.
pub fn is_dangerous_url(url: &str) -> bool {
    let trimmed = url.trim();
    let Some((scheme, _)) = trimmed.split_once(':') else {
        return false;
    };
    matches!(
        scheme.to_ascii_lowercase().as_str(),
        "javascript" | "vbscript" | "data"
    )
}

/// Render a markdown fragment (GFM, raw HTML escaped, unsafe link
/// protocols removed); empty input renders nothing.
pub fn render_markdown(markdown: &str) -> String {
    if markdown.trim().is_empty() {
        return String::new();
    }
    match markdown::to_html_with_options(markdown, &markdown::Options::gfm()) {
        Ok(html) => html.trim_end_matches(['\n', '\r']).to_string(),
        Err(_) => format!("<p>{}</p>", html_escape(markdown)),
    }
}

/// A user message that starts with a Pi `<skill …>` block.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SkillBlock {
    pub name: String,
    pub location: String,
    pub content: String,
    pub user_message: Option<String>,
}

/// Archived `parse_export_skill_block`.
pub fn parse_skill_block(text: &str) -> Option<SkillBlock> {
    let rest = text.strip_prefix(SKILL_PREFIX)?;
    let name_end = rest.find(LOCATION_MARKER)?;
    let name = &rest[..name_end];
    let after_name = &rest[name_end + LOCATION_MARKER.len()..];
    let location_end = after_name.find(SKILL_OPEN_END)?;
    let location = &after_name[..location_end];
    let body = &after_name[location_end + SKILL_OPEN_END.len()..];
    let close = body.find(SKILL_CLOSE)?;
    let content = &body[..close];
    let tail = &body[close + SKILL_CLOSE.len()..];
    if !tail.is_empty() && !tail.starts_with("\n\n") {
        return None;
    }
    let user_message = tail
        .strip_prefix("\n\n")
        .map(str::trim)
        .filter(|t| !t.is_empty())
        .map(str::to_string);
    Some(SkillBlock {
        name: name.to_string(),
        location: location.to_string(),
        content: content.to_string(),
        user_message,
    })
}

fn is_blank_ansi_line(line: &str) -> bool {
    let mut plain = String::new();
    let mut rest = line;
    while !rest.is_empty() {
        if let Some(after) = rest.strip_prefix(ANSI_PREFIX)
            && let Some(end) = after.find('m')
        {
            rest = &after[end + 1..];
            continue;
        }
        let ch = rest.chars().next().unwrap_or_default();
        plain.push(ch);
        rest = &rest[ch.len_utf8()..];
    }
    plain.trim().is_empty()
}

/// ANSI text to HTML: `31` (red) becomes a coloured span, other SGR codes
/// are dropped.
pub fn ansi_to_html(text: &str) -> String {
    let mut html = String::new();
    let mut span_open = false;
    let mut rest = text;
    while !rest.is_empty() {
        if let Some(after) = rest.strip_prefix(ANSI_PREFIX)
            && let Some(end) = after.find('m')
        {
            if span_open {
                html.push_str("</span>");
                span_open = false;
            }
            if &after[..end] == "31" {
                html.push_str("<span style=\"color:#800000\">");
                span_open = true;
            }
            rest = &after[end + 1..];
            continue;
        }
        let ch = rest.chars().next().unwrap_or_default();
        html.push_str(&html_escape(&ch.to_string()));
        rest = &rest[ch.len_utf8()..];
    }
    if span_open {
        html.push_str("</span>");
    }
    html
}

fn ansi_lines_html(lines: &[&str]) -> String {
    let begin = lines.iter().position(|l| !is_blank_ansi_line(l));
    let Some(begin) = begin else {
        return String::new();
    };
    let end = lines
        .iter()
        .rposition(|l| !is_blank_ansi_line(l))
        .unwrap_or(begin);
    let mut html = String::new();
    for line in &lines[begin..=end] {
        html.push_str("<div class=\"ansi-line\">");
        let rendered = ansi_to_html(line);
        html.push_str(if rendered.is_empty() {
            "&nbsp;"
        } else {
            &rendered
        });
        html.push_str("</div>");
    }
    html
}

fn render_images(images: &[(String, String)], class: &str) -> String {
    if images.is_empty() {
        return String::new();
    }
    let mut html = String::from("<div class=\"message-images\">");
    for (mime, data) in images {
        let mime = if mime.is_empty() { "image/png" } else { mime };
        html.push_str(&format!(
            "<img src=\"data:{};base64,{}\" class=\"{}\" />",
            html_escape(mime),
            html_escape(data),
            html_escape(class)
        ));
    }
    html.push_str("</div>");
    html
}

fn blocks_text(blocks: &[ContentBlock]) -> String {
    blocks
        .iter()
        .filter_map(|b| match b {
            ContentBlock::Text { text } => Some(text.as_str()),
            _ => None,
        })
        .collect::<Vec<_>>()
        .join("\n\n")
}

fn blocks_images(blocks: &[ContentBlock]) -> Vec<(String, String)> {
    blocks
        .iter()
        .filter_map(|b| match b {
            ContentBlock::Image { data, mime_type } => Some((mime_type.clone(), data.clone())),
            _ => None,
        })
        .collect()
}

/// Archived `render_export_user_message_html`.
pub fn render_user_message(entry_id: &str, text: &str, images: &[(String, String)]) -> String {
    let dom_id = format!("entry-{}", html_escape(entry_id));
    let copy_button = format!(
        "<button class=\"copy-link-btn\" data-entry-id=\"{}\" title=\"Copy link to this message\"></button>",
        html_escape(entry_id)
    );
    if let Some(skill) = parse_skill_block(text) {
        let has_user_content = skill.user_message.is_some() || !images.is_empty();
        let mut html = format!("<div class=\"skill-user-entry\" id=\"{dom_id}\">{copy_button}");
        html.push_str(
            "<div class=\"skill-invocation\"><div class=\"skill-invocation-label\">[skill] ",
        );
        html.push_str(&html_escape(&skill.name));
        html.push_str("</div><div class=\"skill-invocation-collapsed\">");
        html.push_str(&html_escape(&skill.name));
        html.push_str(
            " (click to expand)</div><div class=\"skill-invocation-content markdown-content\">",
        );
        html.push_str(&render_markdown(&skill.content));
        html.push_str("</div></div>");
        if has_user_content {
            html.push_str("<div class=\"user-message\">");
            html.push_str(&render_images(images, "message-image"));
            if let Some(message) = &skill.user_message {
                html.push_str("<div class=\"markdown-content\">");
                html.push_str(&render_markdown(message));
                html.push_str("</div>");
            }
            html.push_str("</div>");
        }
        html.push_str("</div>");
        return html;
    }
    let mut html = format!("<div class=\"user-message\" id=\"{dom_id}\">{copy_button}");
    html.push_str(&render_images(images, "message-image"));
    if !text.trim().is_empty() {
        html.push_str("<div class=\"markdown-content\">");
        html.push_str(&render_markdown(text));
        html.push_str("</div>");
    }
    html.push_str("</div>");
    html
}

fn tool_result_html(result: &crate::message::ToolResultMessage) -> String {
    let mut html = render_images(&blocks_images(&result.content), "tool-image");
    let text = blocks_text(&result.content);
    if !text.trim().is_empty() {
        html.push_str("<div class=\"tool-output\">");
        html.push_str(&ansi_lines_html(&[text.as_str()]));
        html.push_str("</div>");
    }
    if result.tool_name == "edit"
        && let Some(diff) = result
            .details
            .as_ref()
            .and_then(|d| d.get("diff"))
            .and_then(Value::as_str)
            .filter(|d| !d.is_empty())
    {
        html.push_str("<details class=\"tool-diff\" open><summary>diff</summary><pre>");
        html.push_str(&html_escape(diff));
        html.push_str("</pre></details>");
    }
    html
}

fn assistant_html(
    message: &AssistantMessage,
    id: &str,
    results: &HashMap<String, crate::message::ToolResultMessage>,
) -> String {
    let mut html = format!(
        "<section class=\"message assistant-message\" id=\"entry-{}\"><div class=\"role\">assistant</div>",
        html_escape(id)
    );
    for block in &message.content {
        match block {
            ContentBlock::Text { text } => {
                html.push_str("<div class=\"markdown-content\">");
                html.push_str(&render_markdown(text));
                html.push_str("</div>");
            }
            ContentBlock::Thinking { thinking, .. } => {
                html.push_str("<details class=\"thinking\"><summary>thinking</summary><pre>");
                html.push_str(&html_escape(thinking));
                html.push_str("</pre></details>");
            }
            ContentBlock::ToolCall {
                id: call_id,
                name,
                arguments,
            } => {
                html.push_str("<div class=\"tool-call\"><code>");
                html.push_str(&html_escape(name));
                html.push_str("</code><pre>");
                html.push_str(&html_escape(
                    &serde_json::to_string_pretty(arguments).unwrap_or_default(),
                ));
                html.push_str("</pre>");
                if let Some(result) = results.get(call_id) {
                    html.push_str(&tool_result_html(result));
                }
                html.push_str("</div>");
            }
            ContentBlock::Image { .. } => {}
        }
    }
    if let Some(error) = &message.error_message {
        html.push_str("<div class=\"error\">");
        html.push_str(&html_escape(error));
        html.push_str("</div>");
    }
    html.push_str("</section>");
    html
}

fn bash_html(message: &crate::message::BashExecutionMessage, id: &str) -> String {
    let failed = message.cancelled || message.exit_code.is_some_and(|c| c != 0);
    let mut html = format!(
        "<section class=\"message tool-execution{}\" id=\"entry-{}\">",
        if failed { " error" } else { "" },
        html_escape(id)
    );
    html.push_str("<div class=\"role\">bash</div><div class=\"tool-command\">$ ");
    html.push_str(&html_escape(&message.command));
    html.push_str("</div>");
    if !message.output.is_empty() {
        html.push_str("<div class=\"tool-output\">");
        html.push_str(&ansi_lines_html(&[message.output.as_str()]));
        html.push_str("</div>");
    }
    if message.cancelled {
        html.push_str("<div class=\"error\">cancelled</div>");
    } else if let Some(code) = message.exit_code.filter(|c| *c != 0) {
        html.push_str(&format!("<div class=\"error\">exit {code}</div>"));
    }
    html.push_str("</section>");
    html
}

fn custom_html(custom_type: &str, content: &UserContent, display: bool, id: &str) -> String {
    if !display {
        return String::new();
    }
    let mut html = format!(
        "<section class=\"message custom-message\" id=\"entry-{}\"><div class=\"role\">{}</div>",
        html_escape(id),
        html_escape(if custom_type.is_empty() {
            "custom"
        } else {
            custom_type
        })
    );
    match content {
        UserContent::Text(text) => html.push_str(&render_markdown(text)),
        UserContent::Blocks(blocks) => {
            html.push_str(&render_images(&blocks_images(blocks), "message-image"));
            html.push_str(&render_markdown(&blocks_text(blocks)));
        }
    }
    html.push_str("</section>");
    html
}

fn summary_html(role: &str, summary: &str) -> String {
    format!(
        "<section class=\"message summary\"><div class=\"role\">{role}</div>{}</section>",
        render_markdown(summary)
    )
}

fn json_content_text(content: &Value) -> String {
    match content {
        Value::String(s) => s.clone(),
        Value::Array(items) => items
            .iter()
            .filter(|b| b.get("type").and_then(Value::as_str) == Some("text"))
            .filter_map(|b| b.get("text").and_then(Value::as_str))
            .collect::<Vec<_>>()
            .join("\n\n"),
        _ => String::new(),
    }
}

fn raw_message_html(payload: &Value, index: usize) -> String {
    if payload.get("type").and_then(Value::as_str) != Some("message") {
        return String::new();
    }
    let Some(message) = payload.get("message").filter(|m| m.is_object()) else {
        return String::new();
    };
    let id = payload
        .get("id")
        .and_then(Value::as_str)
        .map(str::to_string)
        .unwrap_or_else(|| format!("entry-{index}"));
    let Some(content) = message.get("content") else {
        return String::new();
    };
    match message.get("role").and_then(Value::as_str) {
        Some("user") => {
            let images: Vec<(String, String)> = content
                .as_array()
                .into_iter()
                .flatten()
                .filter(|b| b.get("type").and_then(Value::as_str) == Some("image"))
                .filter_map(|b| {
                    Some((
                        b.get("mimeType")
                            .and_then(Value::as_str)
                            .unwrap_or("image/png")
                            .to_string(),
                        b.get("data")?.as_str()?.to_string(),
                    ))
                })
                .collect();
            render_user_message(&id, &json_content_text(content), &images)
        }
        Some("assistant") => format!(
            "<section class=\"message assistant-message\" id=\"entry-{}\"><div class=\"role\">assistant</div><div class=\"markdown-content\">{}</div></section>",
            html_escape(&id),
            render_markdown(&json_content_text(content))
        ),
        _ => String::new(),
    }
}

/// `stem` reduced to `[A-Za-z0-9._-]`, separators collapsed, ends trimmed;
/// `session` when nothing is left.
pub fn sanitize_filename_stem(stem: &str) -> String {
    let mut out = String::new();
    let mut previous_separator = false;
    for ch in stem.chars() {
        if ch.is_ascii_alphanumeric() || matches!(ch, '.' | '_' | '-') {
            out.push(ch);
            previous_separator = false;
        } else if !previous_separator {
            out.push('-');
            previous_separator = true;
        }
    }
    let trimmed = out.trim_matches(['.', '-', '_']);
    if trimmed.is_empty() || trimmed == "." || trimmed == ".." {
        "session".into()
    } else {
        trimmed.to_string()
    }
}

/// `qt-session-<file stem>.html` for a session file.
pub fn default_html_filename(session_path: &Path) -> String {
    let name = session_path
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or_default();
    let stem = if name.to_ascii_lowercase().ends_with(".jsonl") {
        &name[..name.len() - 6]
    } else {
        name
    };
    format!("qt-session-{}.html", sanitize_filename_stem(stem))
}

const STYLE: &str = ":root{color-scheme:light dark}body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:#111;color:#eee}main{max-width:920px;margin:0 auto;padding:32px 18px}h1{font-size:22px;margin:0 0 4px}.source{color:#aaa;margin:0 0 24px}.metadata{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:10px;margin:0 0 24px}.metadata div{background:#181818;border:1px solid #333;border-radius:6px;padding:8px}.metadata dt{color:#aaa;font-size:12px}.metadata dd{margin:2px 0 0;overflow-wrap:anywhere}.message{border-top:1px solid #333;padding:16px 0}.role{color:#9ad;font-size:12px;text-transform:uppercase;letter-spacing:.08em;margin-bottom:8px}.user-message,.assistant-message,.tool-result,.custom-message{line-height:1.45}.markdown-content p{margin:0 0 10px}.markdown-content code,pre{background:#1d1d1d;border-radius:4px;padding:2px 4px}pre{overflow:auto;padding:10px}.message-image{max-width:100%;height:auto;border:1px solid #333;border-radius:6px}.skill-invocation{border-left:3px solid #9ad;padding-left:12px;margin-bottom:10px}.skill-invocation-collapsed{display:none}.error{color:#ffb4b4}.tool-output,.tool-diff pre{background:#161616;border-radius:6px;padding:10px;overflow:auto}.tool-diff summary{cursor:pointer;color:#9ad;font-size:12px;text-transform:uppercase;letter-spacing:.08em;margin:8px 0}.tool-output > div:not(.output-preview):not(.output-full),.output-preview > div:not(.expand-hint),.output-full > div:not(.expand-hint){white-space:pre-wrap;}.ansi-line{white-space:pre;}";

/// Render every entry of a session (all branches, file order) as a
/// standalone HTML page in the archived layout.
pub fn render_session_html(
    header: &SessionHeader,
    entries: &[SessionEntry],
    session_path: &str,
) -> String {
    let mut title = "qt session export".to_string();
    if let Some(name) = entries.iter().rev().find_map(|e| match &e.kind {
        EntryKind::SessionInfo { name } => Some(name.clone()),
        _ => None,
    }) && let Some(name) = name.filter(|n| !n.trim().is_empty())
    {
        title = name;
    }
    let session_id = if header.id.is_empty() {
        "unknown".to_string()
    } else {
        header.id.clone()
    };
    let cwd = if header.cwd.is_empty() {
        "unknown".to_string()
    } else {
        header.cwd.clone()
    };
    let mut models = BTreeSet::new();
    let mut total_messages = 0usize;
    let mut results = HashMap::new();
    for entry in entries {
        if let Some(message) = entry.message() {
            total_messages += 1;
            match message {
                AgentMessage::Llm(Message::Assistant(a)) => {
                    models.insert(format!("{}/{}", a.provider, a.model));
                }
                AgentMessage::Llm(Message::ToolResult(r)) if !r.tool_call_id.is_empty() => {
                    results.insert(r.tool_call_id.clone(), r.clone());
                }
                _ => {}
            }
        }
    }
    let models = if models.is_empty() {
        "unknown".to_string()
    } else {
        models.into_iter().collect::<Vec<_>>().join(", ")
    };
    let mut body = String::new();
    for (index, entry) in entries.iter().enumerate() {
        let id = if entry.id.is_empty() {
            format!("entry-{index}")
        } else {
            entry.id.clone()
        };
        body.push_str(&match &entry.kind {
            EntryKind::Message(message) => match message {
                AgentMessage::Llm(Message::User { content, .. }) => {
                    let blocks = content.blocks();
                    render_user_message(&id, &blocks_text(&blocks), &blocks_images(&blocks))
                }
                AgentMessage::Llm(Message::Assistant(a)) => assistant_html(a, &id, &results),
                AgentMessage::Llm(Message::ToolResult(_)) => String::new(),
                AgentMessage::Custom(c) => custom_html(&c.custom_type, &c.content, c.display, &id),
                AgentMessage::BashExecution(b) => bash_html(b, &id),
                AgentMessage::BranchSummary(s) => summary_html("summary", &s.summary),
                AgentMessage::CompactionSummary(s) => summary_html("compaction", &s.summary),
            },
            EntryKind::CustomMessage(c) => custom_html(&c.custom_type, &c.content, c.display, &id),
            EntryKind::BranchSummary(s) => summary_html("summary", &s.summary),
            EntryKind::Compaction(c) => summary_html("compaction", &c.summary),
            EntryKind::Unknown(raw) => raw_message_html(raw, index),
            _ => String::new(),
        });
    }
    if body.is_empty() {
        body = "<p class=\"empty\">No displayable session messages.</p>".into();
    }
    let exported = chrono::Utc::now().format("%Y-%m-%dT%H:%M:%SZ");
    let mut html = String::from(
        "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>",
    );
    html.push_str(&html_escape(&title));
    html.push_str("</title><style>");
    html.push_str(STYLE);
    html.push_str("</style></head><body><main><h1>");
    html.push_str(&html_escape(&title));
    html.push_str("</h1><p class=\"source\">");
    html.push_str(&html_escape(session_path));
    html.push_str("</p><dl class=\"metadata\"><div><dt>Session ID</dt><dd>");
    html.push_str(&html_escape(&session_id));
    html.push_str("</dd></div><div><dt>Models</dt><dd>");
    html.push_str(&html_escape(&models));
    html.push_str("</dd></div><div><dt>CWD</dt><dd>");
    html.push_str(&html_escape(&cwd));
    html.push_str("</dd></div><div><dt>Messages</dt><dd>");
    html.push_str(&total_messages.to_string());
    html.push_str("</dd></div><div><dt>Exported</dt><dd>");
    html.push_str(&html_escape(&exported.to_string()));
    html.push_str("</dd></div></dl>");
    html.push_str(&body);
    html.push_str("</main></body></html>\n");
    html
}

/// The session as JSONL: header line plus one line per entry.
pub fn render_session_jsonl(header: &SessionHeader, entries: &[SessionEntry]) -> String {
    let mut out = header.to_value().to_string();
    out.push('\n');
    for entry in entries {
        out.push_str(&entry.to_value().to_string());
        out.push('\n');
    }
    out
}

/// Write `content` to `path`, creating parent directories.
pub fn write_export(path: &Path, content: &str) -> Result<()> {
    if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        std::fs::create_dir_all(parent).with_context(|| parent.display().to_string())?;
    }
    std::fs::write(path, content).with_context(|| path.display().to_string())
}

/// Pi `getLastAssistantText`: the text of the newest assistant message,
/// skipping aborted responses without content.
pub fn last_assistant_text(messages: &[AgentMessage]) -> Option<String> {
    messages
        .iter()
        .rev()
        .find_map(|m| {
            let a = m.as_assistant()?;
            if a.stop_reason == StopReason::Aborted && a.content.is_empty() {
                return None;
            }
            Some(a.text().trim().to_string())
        })
        .filter(|t| !t.is_empty())
}

/// `<base>#<gist id>`; `base` defaults to `PI_SHARE_VIEWER_URL`, else
/// [`DEFAULT_SHARE_VIEWER_URL`].
pub fn share_viewer_url(gist_id: &str, configured_base: Option<&str>) -> String {
    let base = match configured_base {
        Some(b) if !b.is_empty() => b.to_string(),
        _ => std::env::var("PI_SHARE_VIEWER_URL")
            .ok()
            .filter(|v| !v.is_empty())
            .unwrap_or_else(|| DEFAULT_SHARE_VIEWER_URL.to_string()),
    };
    format!("{base}#{gist_id}")
}

/// The gist id: the last path segment of the URL `gh` prints.
pub fn gist_id_from_output(output: &str) -> Option<String> {
    let url = output.trim();
    if url.is_empty() {
        return None;
    }
    match url.rsplit_once('/') {
        None => Some(url.to_string()),
        Some((_, "")) => None,
        Some((_, id)) => Some(id.to_string()),
    }
}

/// Result of `/share`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ShareResult {
    pub gist_url: String,
    pub preview_url: String,
    /// `Share URL: …\nGist: …`
    pub status_text: String,
}

async fn run(program: &Path, args: &[&str]) -> std::io::Result<std::process::Output> {
    tokio::process::Command::new(program)
        .args(args)
        .stdin(std::process::Stdio::null())
        .output()
        .await
}

/// Upload `html` as a private gist with `gh` (archived
/// `execute_share_session_gist` wording). `gh` names the GitHub CLI
/// executable (normally `gh`).
pub async fn share_html_gist(gh: &Path, html: &str) -> Result<ShareResult> {
    match run(gh, &["auth", "status"]).await {
        Err(_) => {
            bail!("GitHub CLI (gh) is not installed. Install it from https://cli.github.com/")
        }
        Ok(out) if !out.status.success() => {
            bail!("GitHub CLI is not logged in. Run 'gh auth login' first.")
        }
        Ok(_) => {}
    }
    let dir = std::env::temp_dir().join(format!("qt-share-{}", crate::util::short_id()));
    let path = dir.join("session.html");
    write_export(&path, html).map_err(|e| anyhow!("Failed to export session: {e:#}"))?;
    let path_text = path.to_string_lossy().into_owned();
    let gist = run(gh, &["gist", "create", "--public=false", &path_text]).await;
    let _ = std::fs::remove_file(&path);
    let _ = std::fs::remove_dir(&dir);
    let gist = gist.map_err(|e| anyhow!("Failed to create gist: {e}"))?;
    if !gist.status.success() {
        let stderr = String::from_utf8_lossy(&gist.stderr).trim().to_string();
        bail!(
            "Failed to create gist: {}",
            if stderr.is_empty() {
                "Unknown error"
            } else {
                &stderr
            }
        );
    }
    let gist_url = String::from_utf8_lossy(&gist.stdout).trim().to_string();
    let id = gist_id_from_output(&gist_url)
        .ok_or_else(|| anyhow!("Failed to parse gist ID from gh output"))?;
    let preview_url = share_viewer_url(&id, None);
    Ok(ShareResult {
        status_text: format!("Share URL: {preview_url}\nGist: {gist_url}"),
        gist_url,
        preview_url,
    })
}

/// Resolve an export path argument against `cwd` (quotes stripped).
pub fn resolve_export_path(argument: &str, cwd: &Path) -> Option<PathBuf> {
    let trimmed = argument.trim();
    if trimmed.is_empty() {
        return None;
    }
    let unquoted = match trimmed.chars().next() {
        Some(q @ ('"' | '\'')) => {
            let inner = &trimmed[1..];
            inner.split(q).next().unwrap_or(inner)
        }
        _ => trimmed,
    };
    let path = crate::resources::fsutil::expand_tilde(unquoted);
    Some(if path.is_absolute() {
        path
    } else {
        cwd.join(path)
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::{BashExecutionMessage, ToolResultMessage};
    use serde_json::json;

    fn entry(id: &str, kind: EntryKind) -> SessionEntry {
        SessionEntry {
            id: id.into(),
            parent_id: None,
            timestamp: "2026-01-01T00:00:00Z".into(),
            kind,
        }
    }

    #[test]
    fn markdown_escapes_html_and_drops_unsafe_links() {
        assert_eq!(render_markdown("  "), "");
        assert_eq!(
            render_markdown("**hi** <b>x</b>"),
            "<p><strong>hi</strong> &lt;b&gt;x&lt;/b&gt;</p>"
        );
        let html = render_markdown("[x](javascript:alert(1)) [y](https://e.com)");
        assert!(!html.contains("javascript:"), "{html}");
        assert!(html.contains("href=\"https://e.com\""));
        assert!(is_dangerous_url(" Data:text/html,x"));
        assert!(!is_dangerous_url("https://x"));
    }

    #[test]
    fn skill_blocks_and_filenames() {
        let text =
            "<skill name=\"pdf\" location=\"/s/pdf/SKILL.md\">\nUse pdf.\n</skill>\n\nplease";
        let block = parse_skill_block(text).unwrap();
        assert_eq!(block.name, "pdf");
        assert_eq!(block.user_message.as_deref(), Some("please"));
        assert!(parse_skill_block("<skill name=\"x\" location=\"y\">\nz\n</skill>tail").is_none());
        let html = render_user_message("e1", text, &[]);
        assert!(html.starts_with("<div class=\"skill-user-entry\" id=\"entry-e1\">"));
        assert!(html.contains("[skill] pdf"));
        assert_eq!(
            default_html_filename(Path::new("/s/native-1 2.JSONL")),
            "qt-session-native-1-2.html"
        );
        assert_eq!(sanitize_filename_stem("..//.."), "session");
        assert_eq!(
            ansi_to_html("\x1b[31mred\x1b[0m <x>"),
            "<span style=\"color:#800000\">red</span> &lt;x&gt;"
        );
    }

    #[test]
    fn session_html_renders_all_entry_kinds() {
        let header = SessionHeader {
            version: 3,
            id: "sid".into(),
            timestamp: String::new(),
            cwd: "/work".into(),
            parent_session: None,
        };
        let assistant = AssistantMessage {
            content: vec![
                ContentBlock::Thinking {
                    thinking: "hmm".into(),
                    signature: None,
                    redacted: false,
                },
                ContentBlock::text("Done **now**"),
                ContentBlock::ToolCall {
                    id: "c1".into(),
                    name: "edit".into(),
                    arguments: json!({"path": "a"}),
                },
            ],
            provider: "openai".into(),
            model: "gpt-5".into(),
            ..Default::default()
        };
        let entries = vec![
            entry(
                "u1",
                EntryKind::SessionInfo {
                    name: Some("My <session>".into()),
                },
            ),
            entry(
                "u2",
                EntryKind::Message(AgentMessage::Llm(Message::user_text("Fix it"))),
            ),
            entry(
                "a1",
                EntryKind::Message(AgentMessage::Llm(Message::Assistant(assistant))),
            ),
            entry(
                "r1",
                EntryKind::Message(AgentMessage::Llm(Message::ToolResult(ToolResultMessage {
                    tool_call_id: "c1".into(),
                    tool_name: "edit".into(),
                    content: vec![ContentBlock::text("ok")],
                    is_error: false,
                    details: Some(json!({"diff": "-a\n+b"})),
                    timestamp: None,
                }))),
            ),
            entry(
                "b1",
                EntryKind::Message(AgentMessage::BashExecution(BashExecutionMessage {
                    command: "false".into(),
                    output: "\nboom\n".into(),
                    exit_code: Some(1),
                    cancelled: false,
                    truncated: false,
                    full_output_path: None,
                    timestamp: 0,
                    exclude_from_context: false,
                })),
            ),
        ];
        let html = render_session_html(&header, &entries, "/s/x.jsonl");
        assert!(html.starts_with("<!doctype html><html><head><meta charset=\"utf-8\">"));
        assert!(html.contains("<title>My &lt;session&gt;</title>"));
        assert!(html.contains("<dt>Session ID</dt><dd>sid</dd>"));
        assert!(html.contains("<dt>Models</dt><dd>openai/gpt-5</dd>"));
        assert!(html.contains("<dt>Messages</dt><dd>4</dd>"));
        assert!(html.contains("<div class=\"user-message\" id=\"entry-u2\">"));
        assert!(html.contains(
            "<details class=\"thinking\"><summary>thinking</summary><pre>hmm</pre></details>"
        ));
        assert!(html.contains("<p>Done <strong>now</strong></p>"));
        assert!(html.contains("<div class=\"tool-call\"><code>edit</code><pre>{\n  &quot;path&quot;: &quot;a&quot;\n}</pre><div class=\"tool-output\"><div class=\"ansi-line\">ok</div></div><details class=\"tool-diff\" open><summary>diff</summary><pre>-a\n+b</pre></details></div>"));
        assert!(html.contains("<section class=\"message tool-execution error\" id=\"entry-b1\"><div class=\"role\">bash</div><div class=\"tool-command\">$ false</div><div class=\"tool-output\"><div class=\"ansi-line\">\nboom\n</div></div><div class=\"error\">exit 1</div></section>"));
        assert!(html.ends_with("</main></body></html>\n"));
        let empty = render_session_html(&header, &[], "p");
        assert!(empty.contains("No displayable session messages."));
        let jsonl = render_session_jsonl(&header, &entries);
        assert_eq!(jsonl.lines().count(), 6);
        assert!(jsonl.starts_with("{\"type\":\"session\""));
    }

    #[test]
    fn last_text_skips_empty_aborts() {
        let done = AgentMessage::Llm(Message::Assistant(AssistantMessage {
            content: vec![ContentBlock::text(" answer ")],
            ..Default::default()
        }));
        let aborted = AgentMessage::Llm(Message::Assistant(AssistantMessage {
            stop_reason: StopReason::Aborted,
            ..Default::default()
        }));
        assert_eq!(
            last_assistant_text(&[done.clone(), aborted]).as_deref(),
            Some("answer")
        );
        assert_eq!(last_assistant_text(&[]), None);
        assert_eq!(
            gist_id_from_output("https://gist.github.com/u/abc\n").as_deref(),
            Some("abc")
        );
        assert_eq!(gist_id_from_output("https://x/"), None);
        assert_eq!(
            share_viewer_url("abc", Some("https://v/")),
            "https://v/#abc"
        );
    }

    #[cfg(unix)]
    #[tokio::test]
    async fn share_uses_gh_and_reports_archived_errors() {
        use std::os::unix::fs::PermissionsExt as _;
        let dir = tempfile::tempdir().unwrap();
        let gh = dir.path().join("gh");
        let log = dir.path().join("log");
        std::fs::write(
            &gh,
            format!(
                "#!/bin/sh\necho \"$@\" >> {}\nif [ \"$1\" = auth ]; then exit ${{AUTH_EXIT:-0}}; fi\ntest -f \"$4\" || exit 9\necho https://gist.github.com/me/deadbeef\n",
                log.display()
            ),
        )
        .unwrap();
        std::fs::set_permissions(&gh, std::fs::Permissions::from_mode(0o755)).unwrap();
        let result = share_html_gist(&gh, "<html></html>").await.unwrap();
        assert_eq!(result.gist_url, "https://gist.github.com/me/deadbeef");
        assert!(result.preview_url.ends_with("#deadbeef"));
        assert!(result.status_text.starts_with("Share URL: "));
        let calls = std::fs::read_to_string(&log).unwrap();
        assert!(
            calls.starts_with("auth status\ngist create --public=false "),
            "{calls}"
        );
        assert!(calls.trim_end().ends_with("session.html"));
        let missing = share_html_gist(&dir.path().join("nope"), "x")
            .await
            .unwrap_err();
        assert_eq!(
            missing.to_string(),
            "GitHub CLI (gh) is not installed. Install it from https://cli.github.com/"
        );
        let failing = dir.path().join("gh2");
        std::fs::write(&failing, "#!/bin/sh\nexit 1\n").unwrap();
        std::fs::set_permissions(&failing, std::fs::Permissions::from_mode(0o755)).unwrap();
        assert_eq!(
            share_html_gist(&failing, "x")
                .await
                .unwrap_err()
                .to_string(),
            "GitHub CLI is not logged in. Run 'gh auth login' first."
        );
    }
}
