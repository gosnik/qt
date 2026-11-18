//! Live reload of a custom theme file.
//!
//! Mirrors the archived qt's `ThemeWatcher` and Pi's `startThemeWatcher`:
//! built-in themes are never watched, the file's parent directory is watched
//! (so atomic rename-over writes are seen), events are debounced by 100 ms,
//! and the callback only fires when the file still exists and parses — an
//! invalid intermediate save keeps the last good theme.

use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, RecvTimeoutError, Sender};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use notify::event::{AccessKind, AccessMode};
use notify::{EventKind, RecommendedWatcher, RecursiveMode, Watcher as _};

use super::themes::{Theme, builtin_theme, load_theme_file};

/// Delay between the last file event and the reload.
pub const THEME_RELOAD_DEBOUNCE: Duration = Duration::from_millis(100);

enum Message {
    Event,
    Stop,
}

/// File changes worth a reload (the archived inotify mask: modify,
/// close-after-write, moved-to, create, delete). Opens and reads are
/// ignored, so the watcher's own reloads do not retrigger it.
fn is_change(kind: &EventKind) -> bool {
    matches!(
        kind,
        EventKind::Create(_)
            | EventKind::Modify(_)
            | EventKind::Remove(_)
            | EventKind::Access(AccessKind::Close(AccessMode::Write))
    )
}

/// Watches one theme file; dropping it stops the watcher thread.
pub struct ThemeWatcher {
    theme_name: String,
    path: PathBuf,
    sender: Sender<Message>,
    watcher: Option<RecommendedWatcher>,
    thread: Option<JoinHandle<()>>,
}

impl std::fmt::Debug for ThemeWatcher {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ThemeWatcher")
            .field("theme_name", &self.theme_name)
            .field("path", &self.path)
            .finish()
    }
}

impl ThemeWatcher {
    /// Start watching `path` for the theme `theme_name`. `on_reload` runs on
    /// the watcher thread with each successfully re-parsed theme.
    ///
    /// Returns `Ok(None)` for built-in themes and for missing files.
    pub fn start<F>(theme_name: &str, path: &Path, on_reload: F) -> notify::Result<Option<Self>>
    where
        F: Fn(Theme) + Send + 'static,
    {
        if theme_name.is_empty() || builtin_theme(theme_name).is_some() || !path.is_file() {
            return Ok(None);
        }
        let path = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
        let Some(file_name) = path.file_name().map(|n| n.to_os_string()) else {
            return Ok(None);
        };
        let directory = path
            .parent()
            .map(Path::to_path_buf)
            .unwrap_or_else(|| PathBuf::from("."));

        let (sender, receiver) = mpsc::channel::<Message>();
        let event_sender = sender.clone();
        let watched_name = file_name.clone();
        let mut watcher =
            notify::recommended_watcher(move |result: notify::Result<notify::Event>| {
                if let Ok(event) = result
                    && is_change(&event.kind)
                    && event
                        .paths
                        .iter()
                        .any(|p| p.file_name() == Some(watched_name.as_os_str()))
                {
                    let _ = event_sender.send(Message::Event);
                }
            })?;
        watcher.watch(&directory, RecursiveMode::NonRecursive)?;

        let reload_path = path.clone();
        let thread = std::thread::Builder::new()
            .name("qt-theme-watcher".into())
            .spawn(move || {
                let mut deadline: Option<Instant> = None;
                loop {
                    let message = match deadline {
                        Some(at) => {
                            receiver.recv_timeout(at.saturating_duration_since(Instant::now()))
                        }
                        None => receiver.recv().map_err(|_| RecvTimeoutError::Disconnected),
                    };
                    match message {
                        Ok(Message::Event) => {
                            deadline = Some(Instant::now() + THEME_RELOAD_DEBOUNCE)
                        }
                        Ok(Message::Stop) | Err(RecvTimeoutError::Disconnected) => break,
                        Err(RecvTimeoutError::Timeout) => {
                            deadline = None;
                            if reload_path.is_file()
                                && let Ok(theme) = load_theme_file(&reload_path)
                            {
                                on_reload(theme);
                            }
                        }
                    }
                }
            })
            .map_err(notify::Error::io)?;

        Ok(Some(Self {
            theme_name: theme_name.to_string(),
            path,
            sender,
            watcher: Some(watcher),
            thread: Some(thread),
        }))
    }

    pub fn theme_name(&self) -> &str {
        &self.theme_name
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for ThemeWatcher {
    fn drop(&mut self) {
        self.watcher.take();
        let _ = self.sender.send(Message::Stop);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::themes::ThemeToken;
    use super::super::themes::tests::theme_json;
    use super::*;

    #[test]
    fn builtin_and_missing_are_not_watched() {
        let dir = tempfile::tempdir().unwrap();
        assert!(
            ThemeWatcher::start("dark", &dir.path().join("dark.json"), |_| {})
                .unwrap()
                .is_none()
        );
        assert!(
            ThemeWatcher::start("x", &dir.path().join("x.json"), |_| {})
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn reloads_valid_changes_and_ignores_invalid_ones() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("ocean.json");
        std::fs::write(&path, theme_json("ocean", "#000000")).unwrap();
        let (tx, rx) = mpsc::channel();
        let watcher = ThemeWatcher::start("ocean", &path, move |theme| {
            let _ = tx.send(theme);
        })
        .unwrap()
        .expect("watching");
        assert_eq!(watcher.theme_name(), "ocean");

        std::fs::write(&path, "{ invalid").unwrap();
        assert!(rx.recv_timeout(Duration::from_millis(600)).is_err());

        // Atomic replace via rename, as editors do.
        let tmp = dir.path().join("ocean.json.tmp");
        std::fs::write(&tmp, theme_json("ocean", "#123456")).unwrap();
        std::fs::rename(&tmp, &path).unwrap();
        let theme = rx.recv_timeout(Duration::from_secs(5)).expect("reload");
        assert_eq!(theme.color(ThemeToken::Accent).to_hex(""), "#123456");

        // Unrelated files in the directory do not trigger reloads. Let any
        // trailing debounced reload of the rename settle first.
        std::thread::sleep(Duration::from_millis(400));
        while rx.try_recv().is_ok() {}
        std::fs::write(dir.path().join("other.json"), "{}").unwrap();
        assert!(rx.recv_timeout(Duration::from_millis(500)).is_err());
        drop(watcher);
    }
}
