//! Runtime logging (archived `foundation/logging.cpp` on spdlog, here on
//! `tracing`).
//!
//! Logging is off unless a sink is configured: `--log-file <path>`
//! (repeatable; `QT_LOG_FILE` holds `:`-separated paths), `--log-stderr`
//! (`QT_LOG_STDERR=1|true|yes|on`). `--log-level` / `QT_LOG_LEVEL` picks
//! `trace|debug|info|warn|error|critical|off` (default `info`). Command-line
//! flags are applied over the environment. Files are truncated when opened,
//! their parent directories created, and every line reads
//! `[<local time with ms and offset>] <LEVEL> <target>: <message> <fields>`.

use std::fs;
use std::io::{self, Write};
use std::path::PathBuf;
use std::sync::{Arc, Mutex, OnceLock};

use tracing::{Event, Level, Subscriber};
use tracing_subscriber::fmt::format::Writer;
use tracing_subscriber::fmt::{FmtContext, FormatEvent, FormatFields, MakeWriter};
use tracing_subscriber::registry::LookupSpan;

/// Log levels of the archived logger.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Default)]
pub enum LogLevel {
    Trace,
    Debug,
    #[default]
    Info,
    Warn,
    Error,
    Critical,
    Off,
}

impl LogLevel {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Trace => "trace",
            Self::Debug => "debug",
            Self::Info => "info",
            Self::Warn => "warn",
            Self::Error => "error",
            Self::Critical => "critical",
            Self::Off => "off",
        }
    }

    /// Parse a level name (case-insensitive; `warning` = `warn`).
    pub fn parse(value: &str) -> Option<Self> {
        Some(match value.trim().to_ascii_lowercase().as_str() {
            "trace" => Self::Trace,
            "debug" => Self::Debug,
            "info" => Self::Info,
            "warn" | "warning" => Self::Warn,
            "error" => Self::Error,
            "critical" => Self::Critical,
            "off" => Self::Off,
            _ => return None,
        })
    }

    fn max_level(self) -> Option<Level> {
        match self {
            Self::Trace => Some(Level::TRACE),
            Self::Debug => Some(Level::DEBUG),
            Self::Info => Some(Level::INFO),
            Self::Warn => Some(Level::WARN),
            // `tracing` has no critical level: critical messages are errors.
            Self::Error | Self::Critical => Some(Level::ERROR),
            Self::Off => None,
        }
    }
}

/// Where and how much to log.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct LoggingOptions {
    pub files: Vec<PathBuf>,
    pub stderr: bool,
    pub level: LogLevel,
}

fn env_truthy(value: &str) -> bool {
    matches!(value, "1" | "true" | "yes" | "on")
}

impl LoggingOptions {
    /// `QT_LOG_FILE`, `QT_LOG_STDERR`, `QT_LOG_LEVEL`.
    pub fn from_env() -> Self {
        Self::from_env_with(|name| std::env::var(name).ok())
    }

    fn from_env_with(get: impl Fn(&str) -> Option<String>) -> Self {
        let mut options = Self::default();
        if let Some(files) = get("QT_LOG_FILE").filter(|v| !v.is_empty()) {
            options.files = files
                .split(':')
                .filter(|p| !p.is_empty())
                .map(PathBuf::from)
                .collect();
        }
        if let Some(value) = get("QT_LOG_STDERR").filter(|v| !v.is_empty()) {
            options.stderr = env_truthy(&value);
        }
        if let Some(level) = get("QT_LOG_LEVEL").and_then(|v| LogLevel::parse(&v)) {
            options.level = level;
        }
        options
    }

    /// The environment overlaid with `--log-file`, `--log-file=`,
    /// `--log-stderr`, `--log-level`, `--log-level=` from `args`. Values
    /// that look like options are not consumed; unknown levels are ignored.
    pub fn from_args<S: AsRef<str>>(args: &[S]) -> Self {
        let mut options = Self::from_env();
        options.apply_args(args);
        options
    }

    pub fn apply_args<S: AsRef<str>>(&mut self, args: &[S]) {
        let looks_like_option = |v: &str| v.len() > 1 && v.starts_with('-');
        let mut index = 0;
        while index < args.len() {
            let arg = args[index].as_ref();
            let next = args.get(index + 1).map(|v| v.as_ref());
            if arg == "--log-file"
                && let Some(value) = next.filter(|v| !looks_like_option(v))
            {
                self.files.push(PathBuf::from(value));
                index += 2;
                continue;
            }
            if let Some(value) = arg.strip_prefix("--log-file=") {
                self.files.push(PathBuf::from(value));
            } else if arg == "--log-stderr" {
                self.stderr = true;
            } else if arg == "--log-level"
                && let Some(value) = next.filter(|v| !looks_like_option(v))
            {
                if let Some(level) = LogLevel::parse(value) {
                    self.level = level;
                }
                index += 2;
                continue;
            } else if let Some(value) = arg.strip_prefix("--log-level=")
                && let Some(level) = LogLevel::parse(value)
            {
                self.level = level;
            }
            index += 1;
        }
    }

    /// Whether any sink would receive messages.
    pub fn is_enabled(&self) -> bool {
        self.level != LogLevel::Off
            && (self.stderr || self.files.iter().any(|f| !f.as_os_str().is_empty()))
    }
}

#[derive(Clone)]
struct Sinks {
    files: Arc<Vec<Mutex<fs::File>>>,
    stderr: bool,
}

struct SinkWriter {
    sinks: Sinks,
    buffer: Vec<u8>,
}

impl Write for SinkWriter {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.buffer.extend_from_slice(buf);
        Ok(buf.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Drop for SinkWriter {
    fn drop(&mut self) {
        if self.buffer.is_empty() {
            return;
        }
        for file in self.sinks.files.iter() {
            if let Ok(mut f) = file.lock() {
                let _ = f.write_all(&self.buffer);
                let _ = f.flush();
            }
        }
        if self.sinks.stderr {
            let _ = io::stderr().lock().write_all(&self.buffer);
        }
    }
}

impl<'a> MakeWriter<'a> for Sinks {
    type Writer = SinkWriter;
    fn make_writer(&'a self) -> Self::Writer {
        SinkWriter {
            sinks: self.clone(),
            buffer: Vec::new(),
        }
    }
}

/// `[2026-11-18T10:00:00.123+0100] INFO qt_core::engine: message key=value`
struct LineFormat;

impl<S, N> FormatEvent<S, N> for LineFormat
where
    S: Subscriber + for<'a> LookupSpan<'a>,
    N: for<'a> FormatFields<'a> + 'static,
{
    fn format_event(
        &self,
        ctx: &FmtContext<'_, S, N>,
        mut writer: Writer<'_>,
        event: &Event<'_>,
    ) -> std::fmt::Result {
        let now = chrono::Local::now().format("%Y-%m-%dT%H:%M:%S%.3f%z");
        let meta = event.metadata();
        write!(writer, "[{now}] {} {}: ", meta.level(), meta.target())?;
        ctx.field_format().format_fields(writer.by_ref(), event)?;
        writeln!(writer)
    }
}

static INSTALLED: OnceLock<()> = OnceLock::new();

/// Install the global subscriber for `options`. Returns `Ok(false)` when
/// logging stays off (no sinks or level `off`) or a subscriber was already
/// installed; file errors are returned.
pub fn init_logging(options: &LoggingOptions) -> io::Result<bool> {
    let Some(level) = options.level.max_level() else {
        return Ok(false);
    };
    if !options.is_enabled() || INSTALLED.get().is_some() {
        return Ok(false);
    }
    let mut files = Vec::new();
    for path in options.files.iter().filter(|p| !p.as_os_str().is_empty()) {
        if let Some(parent) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
            let _ = fs::create_dir_all(parent);
        }
        files.push(Mutex::new(fs::File::create(path)?));
    }
    let sinks = Sinks {
        files: Arc::new(files),
        stderr: options.stderr,
    };
    let subscriber = tracing_subscriber::fmt()
        .with_max_level(level)
        .with_ansi(false)
        .event_format(LineFormat)
        .with_writer(sinks)
        .finish();
    if tracing::subscriber::set_global_default(subscriber).is_err() {
        return Ok(false);
    }
    let _ = INSTALLED.set(());
    tracing::info!(target: "qt::app", level = options.level.as_str(), "logging configured");
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    #[test]
    fn parses_environment_and_flags() {
        let env: HashMap<&str, &str> = [
            ("QT_LOG_FILE", "/tmp/a.log::/tmp/b.log"),
            ("QT_LOG_STDERR", "yes"),
            ("QT_LOG_LEVEL", "WARNING"),
        ]
        .into_iter()
        .collect();
        let mut options = LoggingOptions::from_env_with(|k| env.get(k).map(|v| v.to_string()));
        assert_eq!(options.files.len(), 2);
        assert!(options.stderr);
        assert_eq!(options.level, LogLevel::Warn);
        options.apply_args(&[
            "--log-file",
            "/tmp/c.log",
            "--log-level",
            "--log-stderr",
            "--log-level=debug",
            "--log-file=/tmp/d.log",
            "--log-level",
            "bogus",
        ]);
        assert_eq!(options.files.len(), 4);
        assert_eq!(options.level, LogLevel::Debug);
        assert!(options.is_enabled());
        assert!(!LoggingOptions::default().is_enabled());
        assert_eq!(LogLevel::parse("critical"), Some(LogLevel::Critical));
        let off = LoggingOptions {
            stderr: true,
            level: LogLevel::Off,
            ..Default::default()
        };
        assert!(!off.is_enabled());
        assert!(!init_logging(&LoggingOptions::default()).unwrap());
    }

    #[test]
    fn writes_formatted_lines_to_files() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("logs/qt.log");
        let file = {
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::File::create(&path).unwrap()
        };
        let sinks = Sinks {
            files: Arc::new(vec![Mutex::new(file)]),
            stderr: false,
        };
        let subscriber = tracing_subscriber::fmt()
            .with_max_level(Level::INFO)
            .with_ansi(false)
            .event_format(LineFormat)
            .with_writer(sinks)
            .finish();
        tracing::subscriber::with_default(subscriber, || {
            tracing::info!(target: "qt::test", provider = "openai", "request sent");
            tracing::debug!(target: "qt::test", "hidden");
        });
        let text = fs::read_to_string(&path).unwrap();
        assert_eq!(text.lines().count(), 1, "{text}");
        let line = text.lines().next().unwrap();
        assert!(line.starts_with('['));
        assert!(
            line.ends_with("] INFO qt::test: request sent provider=\"openai\""),
            "{line}"
        );
    }
}
