//! The status line under the composer (archived `interactive_footer.cpp`):
//! working directory with the git branch and session name, the model with
//! its provider and thinking level, and OpenRouter credits. Also the
//! startup diagnostics, the proactive memory review notification
//! (archived `poll_review_notifications`) and live transcript reloads after
//! compactions this view did not run.
//!
//! Git HEAD and the credits are read on the engine runtime; the UI thread
//! only applies the results.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use gpui_kit::component::WindowExt as _;
use gpui_kit::component::notification::Notification;
use gpui_kit::*;

use super::Workspace;
use crate::RuntimeGlobal;

/// How often git HEAD is re-read (the archived footer refreshed every
/// 500 ms from its input loop; the GUI polls off the UI thread).
const GIT_POLL: Duration = Duration::from_secs(2);
/// OpenRouter credits refresh interval (archived).
const CREDITS_REFRESH: Duration = Duration::from_secs(5 * 60);
/// How often the credits schedule is checked.
const CREDITS_POLL: Duration = Duration::from_secs(15);
/// Memory review poll interval (archived).
const REVIEW_POLL: Duration = Duration::from_secs(5);
/// Startup diagnostics shown as separate notifications; the rest are
/// summarised in one.
const MAX_DIAGNOSTIC_NOTIFICATIONS: usize = 5;

/// Status-line state owned by the workspace.
#[derive(Default)]
pub struct StatusLine {
    /// Current git branch of the working directory (`detached` for a
    /// detached HEAD).
    pub branch: Option<String>,
    credits: CreditsState,
    /// Pending + due-deferred review items last seen; `None` until the
    /// first count is known.
    known_review_attention: Option<u64>,
}

#[derive(Default)]
struct CreditsState {
    /// `credits $X` / `credits unavailable`, shown while an OpenRouter
    /// model is selected.
    text: Option<String>,
    last_started: Option<Instant>,
    pending: bool,
    /// The model was an OpenRouter model at the last check (switching to
    /// one forces a refresh).
    was_openrouter: bool,
}

/// Archived `abbreviate_home`.
pub fn abbreviate_home(path: &str, home: Option<&str>) -> String {
    let Some(home) = home.filter(|h| !h.is_empty()) else {
        return path.to_string();
    };
    if path == home {
        return "~".into();
    }
    match path.strip_prefix(home) {
        Some(rest) if rest.starts_with('/') || rest.starts_with('\\') => format!("~{rest}"),
        _ => path.to_string(),
    }
}

/// The `HEAD` file of the repository containing `cwd` (a `.git` directory,
/// or a `.git` file with `gitdir:` for worktrees and submodules).
pub fn find_git_head(cwd: &Path) -> Option<PathBuf> {
    let mut dir = Some(cwd);
    while let Some(current) = dir {
        let git = current.join(".git");
        if git.is_dir() {
            let head = git.join("HEAD");
            return head.exists().then_some(head);
        }
        if git.is_file() {
            let content = fs::read_to_string(&git).ok()?;
            let gitdir = content
                .lines()
                .next()?
                .trim()
                .strip_prefix("gitdir:")?
                .trim();
            let resolved = current.join(gitdir);
            let head = resolved.join("HEAD");
            return head.exists().then_some(head);
        }
        dir = current.parent();
    }
    None
}

/// Archived `resolve_git_branch`: the branch named by HEAD, `detached`
/// for a commit, `None` without a repository.
pub fn git_branch(cwd: &Path) -> Option<String> {
    let head = find_git_head(cwd)?;
    let content = fs::read_to_string(head).ok()?;
    let content = content.lines().next().unwrap_or_default().trim();
    if let Some(branch) = content.strip_prefix("ref: refs/heads/") {
        return Some(branch.to_string());
    }
    (!content.is_empty()).then(|| "detached".to_string())
}

/// First footer line: `~/dir (branch) • session name`.
pub fn path_line(cwd: &str, branch: Option<&str>, session_name: Option<&str>) -> String {
    let home = std::env::var("HOME")
        .ok()
        .or_else(|| std::env::var("USERPROFILE").ok());
    let mut line = abbreviate_home(cwd, home.as_deref());
    if let Some(branch) = branch.filter(|b| !b.is_empty()) {
        line.push_str(&format!(" ({branch})"));
    }
    if let Some(name) = session_name.filter(|n| !n.is_empty()) {
        line.push_str(&format!(" • {name}"));
    }
    line
}

/// What the model side of the footer shows.
pub struct ModelStatus<'a> {
    pub provider: &'a str,
    pub model: Option<&'a str>,
    /// The model supports reasoning (its thinking level is shown).
    pub reasoning: bool,
    pub thinking: &'a str,
    /// Providers with available models; the provider is shown when more
    /// than one is available.
    pub provider_count: usize,
    /// Provider status such as OpenRouter credits.
    pub status: Option<&'a str>,
}

/// Archived footer right side: `(provider) model • thinking • status`.
pub fn model_line(status: &ModelStatus) -> String {
    let mut line = status.model.unwrap_or("no-model").to_string();
    if status.reasoning {
        let thinking = if status.thinking.is_empty() {
            "off"
        } else {
            status.thinking
        };
        line.push_str(&format!(" • {thinking}"));
    }
    if status.provider_count > 1 && !status.provider.is_empty() {
        line = format!("({}) {line}", status.provider);
    }
    if let Some(extra) = status.status.filter(|s| !s.is_empty()) {
        line.push_str(&format!(" • {extra}"));
    }
    line
}

/// Archived `format_openrouter_credit_amount`: two decimals below $10, one
/// above, trailing zeros trimmed.
pub fn format_credit_amount(value: f64) -> String {
    let text = if value < 10.0 {
        format!("{value:.2}")
    } else {
        format!("{value:.1}")
    };
    if !text.contains('.') {
        return text;
    }
    text.trim_end_matches('0').trim_end_matches('.').to_string()
}

/// The status text for a credits result.
pub fn credits_text(result: &Result<f64, String>) -> String {
    match result {
        Ok(remaining) => format!("credits ${}", format_credit_amount(*remaining)),
        Err(_) => "credits unavailable".into(),
    }
}

/// Archived review notification text.
pub fn review_notification(new_items: u64) -> String {
    format!("Memory review: {new_items} new/due item(s). Open /memory review.")
}

/// The archived de-duplication: notify only when the attention count grew
/// since the last poll. Returns the number of new items to announce.
pub fn review_delta(known: Option<u64>, attention: u64) -> Option<u64> {
    match known {
        Some(known) if attention > known => Some(attention - known),
        _ => None,
    }
}

/// Startup diagnostics as notification texts (the first few verbatim, the
/// rest summarised).
pub fn diagnostic_notifications(diagnostics: &[String]) -> Vec<String> {
    let mut out: Vec<String> = diagnostics
        .iter()
        .take(MAX_DIAGNOSTIC_NOTIFICATIONS)
        .cloned()
        .collect();
    if diagnostics.len() > MAX_DIAGNOSTIC_NOTIFICATIONS {
        out.push(format!(
            "{} more startup diagnostic(s); run qt from a terminal to see them all.",
            diagnostics.len() - MAX_DIAGNOSTIC_NOTIFICATIONS
        ));
    }
    out
}

fn is_openrouter(provider: &str) -> bool {
    provider.eq_ignore_ascii_case("openrouter")
}

impl Workspace {
    /// Start the git, credits and memory review pollers and show the
    /// startup diagnostics.
    pub(super) fn start_status_line(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.watch_git_branch(window, cx);
        self.watch_session_changes(window, cx);
        self.refresh_credits(true, cx);
        cx.spawn_in(window, async move |this, cx| {
            loop {
                cx.background_executor().timer(CREDITS_POLL).await;
                if this
                    .update(cx, |this, cx| this.refresh_credits(false, cx))
                    .is_err()
                {
                    break;
                }
            }
        })
        .detach();
        self.watch_memory_reviews(window, cx);
        let diagnostics = cx.global::<RuntimeGlobal>().0.diagnostics();
        if !diagnostics.is_empty() {
            cx.defer_in(window, move |_, window, cx| {
                for text in diagnostic_notifications(&diagnostics) {
                    window.push_notification(Notification::warning(text), cx);
                }
            });
        }
    }

    /// Reload the transcript when the current session is compacted or its
    /// tree position changes outside a run this view pumps (an extension's
    /// `ctx.compact()` / `ctx.navigateTree()`), so the compaction and branch
    /// summary markers appear live. Runs shown here reload on their own
    /// `CompactionEnd`.
    pub(super) fn watch_session_changes(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        use qt_core::harness::hooks::SessionEvent;
        let (tx, rx) = async_channel::unbounded::<String>();
        self.engine
            .hooks()
            .add_session_listener(std::sync::Arc::new(move |event: &SessionEvent| {
                if let SessionEvent::Compact { session_id, .. }
                | SessionEvent::Tree { session_id, .. } = event
                {
                    let _ = tx.try_send(session_id.clone());
                }
            }));
        cx.spawn_in(window, async move |this, cx| {
            while let Ok(session_id) = rx.recv().await {
                let alive = this.update(cx, |this, cx| {
                    if this.running.is_none() && this.conversation.session_id() == session_id {
                        this.reload_transcript(cx);
                        cx.notify();
                    }
                });
                if alive.is_err() {
                    break;
                }
            }
        })
        .detach();
    }

    /// Re-read git HEAD on the engine runtime every [`GIT_POLL`]; a changed
    /// branch re-renders the status line.
    fn watch_git_branch(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let engine = self.engine.clone();
        cx.spawn_in(window, async move |this, cx| {
            loop {
                let cwd = engine.cwd().to_path_buf();
                let handle = engine
                    .runtime_handle()
                    .spawn_blocking(move || git_branch(&cwd));
                let branch = handle.await.unwrap_or(None);
                let alive = this
                    .update(cx, |this, cx| {
                        if this.status.branch != branch {
                            this.status.branch = branch;
                            cx.notify();
                        }
                    })
                    .is_ok();
                if !alive {
                    break;
                }
                cx.background_executor().timer(GIT_POLL).await;
            }
        })
        .detach();
    }

    /// Archived `start_openrouter_credit_refresh` + poll: credits are shown
    /// while an OpenRouter model is selected, refreshed every five minutes
    /// and right away when an OpenRouter model becomes current.
    pub(super) fn refresh_credits(&mut self, force: bool, cx: &mut Context<Self>) {
        let provider = self
            .conversation
            .model()
            .map(|m| m.provider)
            .unwrap_or_default();
        let credits = &mut self.status.credits;
        if !is_openrouter(&provider) || self.engine.is_offline() {
            if credits.text.take().is_some() || credits.was_openrouter {
                credits.was_openrouter = false;
                cx.notify();
            }
            return;
        }
        let mut force = force;
        if !credits.was_openrouter {
            force = true;
            credits.was_openrouter = true;
        }
        if credits.pending {
            return;
        }
        let now = Instant::now();
        if !force
            && credits
                .last_started
                .is_some_and(|t| now.duration_since(t) < CREDITS_REFRESH)
        {
            return;
        }
        let Some((key, base_url)) = self
            .engine
            .provider(&provider)
            .and_then(|p| p.api_key.filter(|k| !k.is_empty()).map(|k| (k, p.base_url)))
        else {
            credits.text = Some(credits_text(&Err(String::new())));
            cx.notify();
            return;
        };
        credits.last_started = Some(now);
        credits.pending = true;
        let http = self.engine.http().clone();
        let handle = self.engine.spawn(async move {
            qt_core::catalog::openrouter::fetch_credits(&http, &key, &base_url)
                .await
                .map(|c| c.remaining())
                .map_err(|e| e.to_string())
        });
        cx.spawn(async move |this, cx| {
            let result = handle.await.unwrap_or_else(|e| Err(e.to_string()));
            let _ = this.update(cx, |this, cx| {
                let credits = &mut this.status.credits;
                credits.pending = false;
                if credits.was_openrouter {
                    credits.text = Some(credits_text(&result));
                    cx.notify();
                }
            });
        })
        .detach();
    }

    /// The model part of the status line.
    pub(super) fn model_status_text(&self) -> String {
        let model = self.conversation.model();
        let info = model
            .as_ref()
            .and_then(|m| self.models.iter().find(|i| i.model_ref() == *m));
        let mut providers: Vec<&str> = self.models.iter().map(|m| m.provider.as_str()).collect();
        providers.sort_unstable();
        providers.dedup();
        let thinking = self.conversation.thinking();
        let provider = model.as_ref().map(|m| m.provider.as_str()).unwrap_or("");
        model_line(&ModelStatus {
            provider,
            model: model.as_ref().map(|m| m.id.as_str()),
            reasoning: info.is_some_and(|i| i.reasoning),
            thinking: thinking.as_str(),
            provider_count: providers.len(),
            status: self.status.credits.text.as_deref(),
        })
    }

    /// The path part of the status line.
    pub(super) fn path_status_text(&self) -> String {
        path_line(
            &self.engine.cwd().display().to_string(),
            self.status.branch.as_deref(),
            self.conversation.name().as_deref(),
        )
    }

    /// Archived `poll_review_notifications`: every five seconds, while
    /// idle with an empty composer and no dialog open, announce review
    /// items that became pending or due since the last poll.
    fn watch_memory_reviews(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let runtime = cx.global::<RuntimeGlobal>().0.clone();
        let Some(memory) = runtime.memory().map(|m| m.runtime().clone()) else {
            return;
        };
        let attention = move || -> Option<u64> {
            let stats = memory.reviews().review_statistics().ok()?;
            Some(stats.pending + stats.due_deferred)
        };
        cx.spawn_in(window, async move |this, cx| {
            let initial = crate::memory::blocking(&runtime, attention.clone())
                .await
                .ok()
                .flatten();
            if this
                .update(cx, |this, _| this.status.known_review_attention = initial)
                .is_err()
            {
                return;
            }
            loop {
                cx.background_executor().timer(REVIEW_POLL).await;
                let idle = this.update_in(cx, |this, window, cx| {
                    this.running.is_none()
                        && this.bash.is_none()
                        && this.composer_text(cx).is_empty()
                        && !window.has_active_dialog(cx)
                });
                match idle {
                    Ok(true) => {}
                    Ok(false) => continue,
                    Err(_) => break,
                }
                let Some(count) = crate::memory::blocking(&runtime, attention.clone())
                    .await
                    .ok()
                    .flatten()
                else {
                    // Advisory only: a failed read never interrupts input.
                    continue;
                };
                let alive = this.update_in(cx, |this, window, cx| {
                    let known = this.status.known_review_attention;
                    if let Some(new_items) = review_delta(known, count) {
                        window.push_notification(
                            Notification::info(review_notification(new_items)),
                            cx,
                        );
                    }
                    this.status.known_review_attention = Some(count);
                });
                if alive.is_err() {
                    break;
                }
            }
        })
        .detach();
    }
}

#[cfg(test)]
mod tests {
    // Not `super::*`: the gpui glob import would shadow `#[test]`.
    use super::{
        ModelStatus, abbreviate_home, credits_text, diagnostic_notifications, format_credit_amount,
        git_branch, model_line, path_line, review_delta, review_notification,
    };
    use std::fs;

    #[test]
    fn path_line_matches_the_archived_footer() {
        assert_eq!(abbreviate_home("/home/a/x", Some("/home/a")), "~/x");
        assert_eq!(abbreviate_home("/home/a", Some("/home/a")), "~");
        assert_eq!(abbreviate_home("/home/ab", Some("/home/a")), "/home/ab");
        assert_eq!(abbreviate_home("/srv", None), "/srv");
        let line = path_line("/srv/p", Some("main"), Some("Demo"));
        assert_eq!(line, "/srv/p (main) • Demo");
        assert_eq!(path_line("/srv/p", None, Some("")), "/srv/p");
    }

    #[test]
    fn model_line_shows_provider_thinking_and_credits() {
        let mut s = ModelStatus {
            provider: "openrouter",
            model: Some("qwen/qwen3"),
            reasoning: true,
            thinking: "high",
            provider_count: 2,
            status: Some("credits $8.5"),
        };
        assert_eq!(
            model_line(&s),
            "(openrouter) qwen/qwen3 • high • credits $8.5"
        );
        s.provider_count = 1;
        s.reasoning = false;
        s.status = None;
        assert_eq!(model_line(&s), "qwen/qwen3");
        s.model = None;
        s.reasoning = true;
        s.thinking = "";
        assert_eq!(model_line(&s), "no-model • off");
    }

    #[test]
    fn credit_amounts_and_texts() {
        assert_eq!(format_credit_amount(8.5), "8.5");
        assert_eq!(format_credit_amount(3.456), "3.46");
        assert_eq!(format_credit_amount(12.0), "12");
        assert_eq!(format_credit_amount(123.45), "123.5");
        assert_eq!(format_credit_amount(0.0), "0");
        assert_eq!(credits_text(&Ok(8.5)), "credits $8.5");
        assert_eq!(credits_text(&Err("x".into())), "credits unavailable");
    }

    #[test]
    fn git_branch_reads_head_files_and_worktrees() {
        let dir = tempfile::tempdir().unwrap();
        let repo = dir.path().join("repo");
        fs::create_dir_all(repo.join(".git")).unwrap();
        fs::create_dir_all(repo.join("src/deep")).unwrap();
        assert_eq!(git_branch(&repo), None);
        fs::write(repo.join(".git/HEAD"), "ref: refs/heads/feature/x\n").unwrap();
        assert_eq!(
            git_branch(&repo.join("src/deep")).as_deref(),
            Some("feature/x")
        );
        fs::write(repo.join(".git/HEAD"), "0123abcd\n").unwrap();
        assert_eq!(git_branch(&repo).as_deref(), Some("detached"));
        // A worktree: `.git` is a file pointing at the real git dir.
        let wt = dir.path().join("wt");
        let wt_git = repo.join(".git/worktrees/wt");
        fs::create_dir_all(&wt_git).unwrap();
        fs::create_dir_all(&wt).unwrap();
        fs::write(wt_git.join("HEAD"), "ref: refs/heads/topic\n").unwrap();
        fs::write(wt.join(".git"), format!("gitdir: {}\n", wt_git.display())).unwrap();
        assert_eq!(git_branch(&wt).as_deref(), Some("topic"));
        assert_eq!(git_branch(&dir.path().join("none")), None);
    }

    #[test]
    fn review_notifications_only_announce_growth() {
        assert_eq!(review_delta(None, 3), None);
        assert_eq!(review_delta(Some(3), 3), None);
        assert_eq!(review_delta(Some(3), 1), None);
        assert_eq!(review_delta(Some(1), 4), Some(3));
        assert_eq!(
            review_notification(2),
            "Memory review: 2 new/due item(s). Open /memory review."
        );
    }

    #[test]
    fn diagnostics_are_capped() {
        let many: Vec<String> = (0..7).map(|i| format!("d{i}")).collect();
        let shown = diagnostic_notifications(&many);
        assert_eq!(shown.len(), 6);
        assert_eq!(shown[0], "d0");
        assert!(shown[5].starts_with("2 more startup diagnostic(s)"));
        assert_eq!(diagnostic_notifications(&many[..2]).len(), 2);
    }
}
