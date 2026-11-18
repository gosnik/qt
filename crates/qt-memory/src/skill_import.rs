//! Imports `SKILL.md` packages as active, provenance-tracked skill memories
//! (archived `skill_import.cpp`). Unchanged skills are detected by snapshot
//! hash; changed skills supersede their prior memory through governance.

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use crate::error::{MemoryError, Result, invalid};
use crate::model::*;
use crate::service::{GovernanceService, MemoryService};
use crate::text::{normalize_content, stable_hash_hex, truncate_bytes};

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SkillImportItem {
    pub skill_path: PathBuf,
    pub memory_id: String,
    /// `created`, `updated`, `unchanged` or `failed`.
    pub action: String,
    pub diagnostic: String,
    pub related_files: usize,
}

pub type SkillImportProgress = Box<dyn Fn(&SkillImportItem) + Send + Sync>;

pub struct SkillImportOptions {
    pub root: PathBuf,
    /// When set, the canonical scan root must stay inside this directory.
    pub containment_root: Option<PathBuf>,
    pub recursive: bool,
    pub maximum_skill_bytes: usize,
    pub maximum_related_files: usize,
    pub maximum_package_files: usize,
    pub maximum_related_file_bytes: usize,
    pub maximum_skills: usize,
    pub maximum_walk_entries: usize,
    pub maximum_depth: usize,
    pub progress: Option<SkillImportProgress>,
}

impl SkillImportOptions {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self {
            root: root.into(),
            containment_root: None,
            recursive: false,
            maximum_skill_bytes: 96 * 1024,
            maximum_related_files: 32,
            maximum_package_files: 512,
            maximum_related_file_bytes: 12 * 1024,
            maximum_skills: 1024,
            maximum_walk_entries: 65_536,
            maximum_depth: 64,
            progress: None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct SkillImportReport {
    pub root: PathBuf,
    pub recursive: bool,
    pub discovered: usize,
    pub created: usize,
    pub updated: usize,
    pub unchanged: usize,
    pub failed: usize,
    pub items: Vec<SkillImportItem>,
}

const IGNORED_DIRECTORIES: [&str; 7] = [
    ".git",
    ".pio",
    ".svn",
    "node_modules",
    "build",
    "dist",
    "__pycache__",
];
const TEXT_EXTENSIONS: [&str; 32] = [
    ".md", ".txt", ".json", ".jsonl", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf", ".xml",
    ".csv", ".sh", ".bash", ".zsh", ".py", ".js", ".jsx", ".ts", ".tsx", ".as", ".c", ".cc",
    ".cpp", ".h", ".hh", ".hpp", ".rs", ".go", ".java", ".rb", ".php",
];

fn file_name(path: &Path) -> String {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default()
}

fn ignored_directory(path: &Path) -> bool {
    IGNORED_DIRECTORIES.contains(&file_name(path).as_str())
}

fn likely_text_file(path: &Path) -> bool {
    let name = file_name(path);
    if name.is_empty() || name.starts_with('.') {
        return false;
    }
    let extension = path
        .extension()
        .map(|ext| format!(".{}", ext.to_string_lossy().to_ascii_lowercase()))
        .unwrap_or_default();
    TEXT_EXTENSIONS.contains(&extension.as_str())
}

fn regular_non_symlink(path: &Path) -> bool {
    std::fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_file())
}

fn generic(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

fn bounded_file_text(path: &Path, limit: usize) -> Result<String> {
    use std::io::Read as _;
    let file = std::fs::File::open(path)
        .map_err(|_| MemoryError::storage(format!("unable to read {}", generic(path))))?;
    let mut bytes = Vec::new();
    file.take(limit as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.contains(&0) {
        return Err(MemoryError::storage("binary content"));
    }
    let mut content = String::from_utf8_lossy(&bytes).into_owned();
    if bytes.len() > limit {
        const MARKER: &str = "\n\n[truncated]";
        if limit > MARKER.len() {
            truncate_bytes(&mut content, limit - MARKER.len());
            content.push_str(MARKER);
        } else {
            truncate_bytes(&mut content, limit);
        }
    }
    Ok(content)
}

struct Walk<'a> {
    options: &'a SkillImportOptions,
    entries: usize,
    truncated: bool,
}

struct Entry {
    path: PathBuf,
    is_symlink: bool,
    is_dir: bool,
    is_file: bool,
}

impl Walk<'_> {
    fn sorted_entries(&mut self, directory: &Path) -> Vec<Entry> {
        let mut entries = Vec::new();
        let Ok(iterator) = std::fs::read_dir(directory) else {
            return entries;
        };
        for entry in iterator {
            let Ok(entry) = entry else { break };
            if self.entries >= self.options.maximum_walk_entries {
                self.truncated = true;
                break;
            }
            self.entries += 1;
            let Ok(kind) = entry.file_type() else {
                continue;
            };
            let path = entry.path();
            let (is_dir, is_file) = if kind.is_symlink() {
                (false, false)
            } else {
                (kind.is_dir(), kind.is_file())
            };
            entries.push(Entry {
                path,
                is_symlink: kind.is_symlink(),
                is_dir,
                is_file,
            });
        }
        entries.sort_by_key(|entry| generic(&entry.path));
        entries
    }

    fn find_skills_below(&mut self, directory: &Path, depth: usize, result: &mut Vec<PathBuf>) {
        if self.truncated
            || result.len() >= self.options.maximum_skills
            || depth >= self.options.maximum_depth
        {
            self.truncated = true;
            return;
        }
        for entry in self.sorted_entries(directory) {
            if self.truncated || result.len() >= self.options.maximum_skills {
                self.truncated = true;
                return;
            }
            if entry.is_symlink || !entry.is_dir || ignored_directory(&entry.path) {
                continue;
            }
            let skill = entry.path.join("SKILL.md");
            if regular_non_symlink(&skill) {
                result.push(skill);
                continue;
            }
            self.find_skills_below(&entry.path, depth + 1, result);
        }
    }

    fn collect_related(
        &mut self,
        directory: &Path,
        skill: &Path,
        depth: usize,
        candidates: &mut Vec<PathBuf>,
    ) {
        if self.truncated || depth >= self.options.maximum_depth {
            self.truncated = true;
            return;
        }
        for entry in self.sorted_entries(directory) {
            if self.truncated {
                return;
            }
            if entry.is_symlink {
                continue;
            }
            if entry.is_dir {
                let name = file_name(&entry.path);
                if !ignored_directory(&entry.path)
                    && !name.starts_with('.')
                    && !regular_non_symlink(&entry.path.join("SKILL.md"))
                {
                    self.collect_related(&entry.path, skill, depth + 1, candidates);
                }
            } else if entry.is_file && entry.path != skill && likely_text_file(&entry.path) {
                candidates.push(entry.path);
            }
        }
    }
}

fn find_skills(root: &Path, options: &SkillImportOptions) -> Vec<PathBuf> {
    let direct = root.join("SKILL.md");
    if regular_non_symlink(&direct) {
        return vec![direct];
    }
    let mut result = Vec::new();
    if options.recursive {
        Walk {
            options,
            entries: 0,
            truncated: false,
        }
        .find_skills_below(root, 0, &mut result);
    }
    result.sort();
    result.dedup();
    result
}

fn relative_display(path: &Path, base: &Path) -> String {
    match path.strip_prefix(base) {
        Ok(relative) => generic(relative),
        Err(_) => file_name(path),
    }
}

fn skill_snapshot(
    skill: &Path,
    options: &SkillImportOptions,
    related_count: &mut usize,
) -> Result<String> {
    let package_root = skill.parent().unwrap_or_else(|| Path::new("."));
    let mut content = format!(
        "# Skill memory: {}\n\n## SKILL.md\n\n{}\n",
        file_name(package_root),
        bounded_file_text(skill, options.maximum_skill_bytes)?
    );
    let mut candidates = Vec::new();
    Walk {
        options,
        entries: 0,
        truncated: false,
    }
    .collect_related(package_root, skill, 0, &mut candidates);
    candidates.sort();
    candidates.truncate(options.maximum_package_files);
    let remaining = |content: &str| options.maximum_skill_bytes.saturating_sub(content.len());
    for path in candidates.iter().take(options.maximum_related_files) {
        let left = remaining(&content);
        if left < 128 {
            break;
        }
        let file_limit = options.maximum_related_file_bytes.min(left - 64);
        if let Ok(text) = bounded_file_text(path, file_limit) {
            content += &format!(
                "\n## Related: {}\n\n{text}\n",
                relative_display(path, package_root)
            );
            *related_count += 1;
        }
    }
    if !candidates.is_empty() {
        content += "\n## Package inventory\n\n";
        let shown = candidates.len().min(128);
        for path in &candidates[..shown] {
            content += &format!("- {}\n", relative_display(path, package_root));
        }
        if candidates.len() > shown {
            content += &format!(
                "- … {} additional files omitted\n",
                candidates.len() - shown
            );
        }
    }
    if content.len() > options.maximum_skill_bytes {
        const MARKER: &str = "\n\n[skill memory truncated]";
        truncate_bytes(&mut content, options.maximum_skill_bytes - MARKER.len());
        content.push_str(MARKER);
    }
    Ok(normalize_content(&content))
}

static IMPORT_LOCK: Mutex<()> = Mutex::new(());

/// Scans `options.root` for skill packages and imports each as a skill
/// memory in a repository scope.
pub fn import_skill_memories(
    memories: &MemoryService,
    governance: &GovernanceService,
    scope: &MemoryScope,
    options: &SkillImportOptions,
) -> Result<SkillImportReport> {
    if options.root.as_os_str().is_empty() {
        return invalid("skill scan root is required");
    }
    let _guard = IMPORT_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let root = std::fs::canonicalize(&options.root)
        .ok()
        .filter(|root| root.is_dir())
        .ok_or_else(|| {
            MemoryError::invalid(format!(
                "skill scan root is not a readable directory: {}",
                generic(&options.root)
            ))
        })?;
    if let Some(containment) = &options.containment_root {
        let inside = std::fs::canonicalize(containment)
            .ok()
            .filter(|containment| containment.is_dir())
            .is_some_and(|containment| root.starts_with(containment));
        if !inside {
            return invalid("skill scan root is outside the allowed workspace");
        }
    }
    if options.maximum_skill_bytes < 1024
        || options.maximum_skill_bytes > 1024 * 1024
        || options.maximum_related_file_bytes == 0
        || options.maximum_related_file_bytes > options.maximum_skill_bytes
        || options.maximum_skills == 0
        || options.maximum_skills > 10_000
        || options.maximum_walk_entries == 0
        || options.maximum_walk_entries > 1_000_000
        || options.maximum_depth == 0
        || options.maximum_depth > 256
    {
        return invalid("skill import limits are invalid");
    }
    if scope.kind != ScopeKind::Repository {
        return invalid("skill imports require repository memory scope");
    }
    let mut report = SkillImportReport {
        root: root.clone(),
        recursive: options.recursive,
        ..Default::default()
    };
    let skills = find_skills(&root, options);
    report.discovered = skills.len();
    let existing = memories.query(
        scope,
        &MemoryQuery {
            memory_type: Some(MemoryType::Skill),
            policy: RecallPolicy::Strict,
            limit: 0,
            ..Default::default()
        },
    )?;
    for skill in skills {
        let mut item = SkillImportItem {
            skill_path: skill.clone(),
            ..Default::default()
        };
        let outcome = (|| -> Result<()> {
            let mut related = 0;
            let snapshot = skill_snapshot(&skill, options, &mut related)?;
            item.related_files = related;
            let source_key = stable_hash_hex(&format!("file://{}", generic(&skill)));
            let prior = existing
                .iter()
                .find(|memory| memory.provenance.source_id == source_key);
            let snapshot_hash = stable_hash_hex(&snapshot);
            if let Some(prior) = prior.filter(|prior| prior.provenance.source_hash == snapshot_hash)
            {
                item.action = "unchanged".into();
                item.memory_id = prior.id.clone();
                report.unchanged += 1;
                return Ok(());
            }
            let skill_name = skill.parent().map(file_name).unwrap_or_default();
            let write = MemoryWrite {
                memory_type: MemoryType::Skill,
                status: MemoryStatus::Active,
                provenance: Provenance {
                    source_id: source_key.clone(),
                    source_type: SourceType::File,
                    source_uri: format!("skill://local/{source_key}"),
                    source_hash: snapshot_hash,
                    created_by: "import".into(),
                    evidence_strength: 0.95,
                    ..Default::default()
                },
                source: "user:skill-scan".into(),
                content: snapshot,
                tags: vec![
                    "skill".into(),
                    "skill:imported".into(),
                    format!("skill-source:{source_key}"),
                    format!("skill:name:{skill_name}"),
                ],
                confidence: 0.95,
                quality_score: 0.9,
            };
            item.memory_id = memories.remember(scope, write)?;
            if let Some(prior) = prior {
                if let Err(error) = governance.supersede(
                    scope,
                    &prior.id,
                    &item.memory_id,
                    "user:skill-scan",
                    "skill source changed",
                ) {
                    let _ = memories.forget(scope, &item.memory_id);
                    item.memory_id.clear();
                    return Err(error);
                }
                item.action = "updated".into();
                report.updated += 1;
            } else {
                item.action = "created".into();
                report.created += 1;
            }
            Ok(())
        })();
        if let Err(error) = outcome {
            item.action = "failed".into();
            item.diagnostic = error.to_string();
            report.failed += 1;
        }
        if let Some(progress) = &options.progress {
            // Reporting failures must not roll back a durable import.
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| progress(&item)));
        }
        report.items.push(item);
    }
    Ok(report)
}
