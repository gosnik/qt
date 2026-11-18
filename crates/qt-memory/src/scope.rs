//! Scope validation, keys and visibility rules (archived `scope.cpp`).

use std::path::{Path, PathBuf};

use crate::error::{Result, invalid};
use crate::model::{MemoryScope, RecallPolicy, ScopeKind};
use crate::text::stable_hash_hex;

/// Enforces the dimensions each scope kind requires and forbids.
pub fn validate_scope(scope: &MemoryScope) -> Result<()> {
    if scope.user.is_empty() {
        return invalid("memory scope user is required");
    }
    let turn_dimensions = !scope.task.is_empty()
        || !scope.agent.is_empty()
        || !scope.session.is_empty()
        || !scope.conversation.is_empty();
    match scope.kind {
        ScopeKind::Exact => {
            if scope.project.is_empty() || scope.repository.is_empty() {
                return invalid("exact memory scope requires project and repository");
            }
        }
        ScopeKind::Branch => {
            if scope.project.is_empty() || scope.repository.is_empty() || scope.branch.is_empty() {
                return invalid("branch memory scope requires project, repository, and branch");
            }
            if turn_dimensions {
                return invalid("branch memory scope cannot contain turn dimensions");
            }
        }
        ScopeKind::Repository => {
            if scope.project.is_empty() || scope.repository.is_empty() {
                return invalid("repository memory scope requires project and repository");
            }
            if !scope.branch.is_empty() || turn_dimensions {
                return invalid("repository memory scope cannot contain turn dimensions");
            }
        }
        ScopeKind::Project => {
            if scope.project.is_empty()
                || !scope.repository.is_empty()
                || !scope.branch.is_empty()
                || turn_dimensions
            {
                return invalid(
                    "project memory scope requires project and forbids narrower dimensions",
                );
            }
        }
        ScopeKind::GlobalUser => {
            if !scope.organization.is_empty()
                || !scope.project.is_empty()
                || !scope.repository.is_empty()
                || !scope.branch.is_empty()
                || turn_dimensions
            {
                return invalid("global-user memory scope may only contain user identity");
            }
        }
    }
    Ok(())
}

/// Length-prefixed, unambiguous key stored in every scoped table.
pub fn scope_key(scope: &MemoryScope) -> Result<String> {
    validate_scope(scope)?;
    let mut key = String::from(scope.kind.as_str());
    key.push('|');
    for value in [
        &scope.user,
        &scope.organization,
        &scope.project,
        &scope.repository,
        &scope.branch,
        &scope.task,
        &scope.agent,
        &scope.session,
        &scope.conversation,
    ] {
        key.push_str(&value.len().to_string());
        key.push(':');
        key.push_str(value);
        key.push('|');
    }
    Ok(key)
}

/// Canonical durable form: repository when no branch is known, otherwise
/// branch, with turn-scoped dimensions cleared. Every surface that reads the
/// stored review queue addresses it through this form.
pub fn durable_memory_scope(mut scope: MemoryScope) -> MemoryScope {
    scope.kind = if scope.branch.is_empty() {
        ScopeKind::Repository
    } else {
        ScopeKind::Branch
    };
    scope.task.clear();
    scope.agent.clear();
    scope.session.clear();
    scope.conversation.clear();
    scope
}

/// The caller's scope followed by the ancestors a policy may read.
pub fn recall_scopes(
    scope: &MemoryScope,
    policy: RecallPolicy,
    include_global_user: bool,
) -> Result<Vec<MemoryScope>> {
    validate_scope(scope)?;
    let mut result = vec![scope.clone()];
    if policy == RecallPolicy::Strict {
        return Ok(result);
    }
    let mut keys = vec![scope_key(scope)?];
    let mut append = |item: MemoryScope| -> Result<()> {
        let key = scope_key(&item)?;
        if !keys.contains(&key) {
            keys.push(key);
            result.push(item);
        }
        Ok(())
    };
    let mut ancestor = scope.clone();
    ancestor.task.clear();
    ancestor.agent.clear();
    ancestor.session.clear();
    ancestor.conversation.clear();
    if !ancestor.repository.is_empty() && !ancestor.branch.is_empty() {
        ancestor.kind = ScopeKind::Branch;
        append(ancestor.clone())?;
    }
    ancestor.branch.clear();
    if !ancestor.repository.is_empty() {
        ancestor.kind = ScopeKind::Repository;
        append(ancestor.clone())?;
    }
    ancestor.repository.clear();
    ancestor.repository_display.clear();
    if !ancestor.project.is_empty() {
        ancestor.kind = ScopeKind::Project;
        append(ancestor)?;
    }
    if include_global_user {
        append(MemoryScope {
            kind: ScopeKind::GlobalUser,
            user: scope.user.clone(),
            ..Default::default()
        })?;
    }
    Ok(result)
}

/// Scope keys visible to `scope` under `policy`, nearest first.
pub fn allowed_keys(
    scope: &MemoryScope,
    policy: RecallPolicy,
    include_global: bool,
) -> Result<Vec<String>> {
    recall_scopes(scope, policy, include_global)?
        .iter()
        .map(scope_key)
        .collect()
}

/// Whether a caller may read a record stored at `stored`.
pub fn scope_can_access(
    caller: &MemoryScope,
    stored: &MemoryScope,
    policy: RecallPolicy,
    include_global_user: bool,
) -> bool {
    if caller.user != stored.user {
        return false;
    }
    if stored.kind != ScopeKind::GlobalUser && caller.organization != stored.organization {
        return false;
    }
    let same_turn = caller.task == stored.task
        && caller.session == stored.session
        && caller.agent == stored.agent
        && caller.conversation == stored.conversation;
    if caller.kind == stored.kind
        && caller.organization == stored.organization
        && caller.project == stored.project
        && caller.repository == stored.repository
        && caller.branch == stored.branch
        && same_turn
    {
        return true;
    }
    if policy == RecallPolicy::Strict {
        return false;
    }
    match stored.kind {
        ScopeKind::Exact => {
            caller.project == stored.project
                && caller.repository == stored.repository
                && caller.branch == stored.branch
                && same_turn
        }
        ScopeKind::Branch => {
            caller.project == stored.project
                && caller.repository == stored.repository
                && !caller.branch.is_empty()
                && caller.branch == stored.branch
        }
        ScopeKind::Repository => {
            caller.project == stored.project && caller.repository == stored.repository
        }
        ScopeKind::Project => caller.project == stored.project,
        ScopeKind::GlobalUser => include_global_user,
    }
}

/// Recall weight for a visible scope; `0` when inaccessible.
pub fn scope_score(caller: &MemoryScope, stored: &MemoryScope) -> f64 {
    if !scope_can_access(caller, stored, RecallPolicy::Broad, true) {
        return 0.0;
    }
    match stored.kind {
        ScopeKind::Exact => 1.0,
        ScopeKind::Branch => 0.94,
        ScopeKind::Repository => 0.86,
        ScopeKind::Project => 0.68,
        ScopeKind::GlobalUser => 0.50,
    }
}

/// The scope a runtime working in `cwd` writes to (archived
/// `derive_runtime_memory_scope`). Absolute paths never reach durable state:
/// the repository is a stable hash of the primary remote (or the local root)
/// and the display/project fields are basenames or remote identities.
pub fn derive_runtime_memory_scope(cwd: &Path, session: &str, agent: &str) -> MemoryScope {
    let mut scope = MemoryScope {
        kind: ScopeKind::Exact,
        user: "local_user".into(),
        session: session.into(),
        agent: agent.into(),
        ..Default::default()
    };
    let root = repository_root(cwd);
    let display_root = normalize_lexically(root.as_deref().unwrap_or(cwd));
    scope.project = display_root
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    scope.repository_display = scope.project.clone();
    scope.branch = root.as_deref().map(branch_name).unwrap_or_default();
    let mut identity = display_root.to_string_lossy().replace('\\', "/");
    if let Some(remote) = root.as_deref().and_then(primary_remote) {
        identity = normalized_remote_identity(&remote);
        scope.repository_display = identity.clone();
    }
    scope.repository = format!("repo_{}", stable_hash_hex(&identity));
    scope
}

fn normalize_lexically(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                out.pop();
            }
            other => out.push(other.as_os_str()),
        }
    }
    out
}

pub(crate) fn repository_root(cwd: &Path) -> Option<PathBuf> {
    let mut path = std::fs::canonicalize(cwd)
        .ok()
        .or_else(|| std::path::absolute(cwd).ok())?;
    if !path.is_dir() {
        path = path.parent()?.to_path_buf();
    }
    loop {
        if path.join(".git").exists() {
            return Some(path);
        }
        if !path.pop() {
            return None;
        }
    }
}

pub(crate) fn git_directory(root: &Path) -> Option<PathBuf> {
    let marker = root.join(".git");
    if marker.is_dir() {
        return Some(marker);
    }
    let text = std::fs::read_to_string(&marker).ok()?;
    let line = text.lines().next()?;
    let rest = line.strip_prefix("gitdir:")?;
    let path = PathBuf::from(rest.trim());
    Some(normalize_lexically(&if path.is_relative() {
        root.join(path)
    } else {
        path
    }))
}

fn primary_remote(root: &Path) -> Option<String> {
    let config = std::fs::read_to_string(git_directory(root)?.join("config")).ok()?;
    let mut section = String::new();
    let mut first_remote = None;
    for line in config.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            section = line.to_string();
            continue;
        }
        if !line.starts_with("url") {
            continue;
        }
        let Some(equals) = line.find('=') else {
            continue;
        };
        let url = line[equals + 1..].trim().to_string();
        if first_remote.is_none() {
            first_remote = Some(url.clone());
        }
        if section == "[remote \"origin\"]" {
            return Some(url);
        }
    }
    first_remote
}

fn branch_name(root: &Path) -> String {
    git_directory(root)
        .and_then(|git| std::fs::read_to_string(git.join("HEAD")).ok())
        .and_then(|head| head.lines().next().map(|line| line.trim().to_string()))
        .and_then(|head| head.strip_prefix("ref: refs/heads/").map(str::to_string))
        .unwrap_or_default()
}

/// `host/path` for SSH (`git@host:path`) and protocol remotes; the raw remote
/// otherwise (archived `normalized_remote_identity` over `parse_git_url`).
fn normalized_remote_identity(remote: &str) -> String {
    parse_remote(remote.trim()).unwrap_or_else(|| remote.to_string())
}

fn parse_remote(url: &str) -> Option<String> {
    let strip = |path: &str| {
        let path = path.trim_end_matches('/');
        path.strip_suffix(".git").unwrap_or(path).to_string()
    };
    let without_ref = |path: &str| -> String {
        // A trailing `@ref` after a `/` in the path is a pinned ref, not part of the repository.
        match path.rfind('@') {
            Some(at) if path[..at].contains('/') && at + 1 < path.len() && at > 0 => {
                path[..at].to_string()
            }
            _ => path.to_string(),
        }
    };
    let (host, path) = if let Some(rest) = url.strip_prefix("git@") {
        let colon = rest.find(':')?;
        (
            rest[..colon].trim_end_matches('/').to_string(),
            strip(&without_ref(&rest[colon + 1..])),
        )
    } else if ["https://", "http://", "ssh://", "git://"]
        .iter()
        .any(|prefix| url.starts_with(prefix))
    {
        let after = &url[url.find("://")? + 3..];
        let slash = after.find('/')?;
        let host_port = &after[..slash];
        let host = host_port
            .rsplit_once('@')
            .map(|(_, host)| host)
            .unwrap_or(host_port);
        (host.to_string(), strip(&without_ref(&after[slash + 1..])))
    } else {
        return None;
    };
    if host.is_empty() || path.is_empty() || !path.contains('/') {
        return None;
    }
    Some(format!("{host}/{path}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn exact() -> MemoryScope {
        MemoryScope {
            kind: ScopeKind::Exact,
            user: "u".into(),
            project: "p".into(),
            repository: "r".into(),
            branch: "main".into(),
            session: "s".into(),
            ..Default::default()
        }
    }

    #[test]
    fn scope_key_is_length_prefixed() {
        assert_eq!(
            scope_key(&exact()).unwrap(),
            "exact|1:u|0:|1:p|1:r|4:main|0:|0:|1:s|0:|"
        );
    }

    #[test]
    fn validation_messages_match_archive() {
        let mut scope = exact();
        scope.user.clear();
        assert_eq!(
            validate_scope(&scope).unwrap_err().to_string(),
            "memory scope user is required"
        );
        let mut scope = exact();
        scope.kind = ScopeKind::Branch;
        assert_eq!(
            validate_scope(&scope).unwrap_err().to_string(),
            "branch memory scope cannot contain turn dimensions"
        );
        let mut scope = exact();
        scope.kind = ScopeKind::Repository;
        scope.session.clear();
        assert_eq!(
            validate_scope(&scope).unwrap_err().to_string(),
            "repository memory scope cannot contain turn dimensions"
        );
        let global = MemoryScope {
            kind: ScopeKind::GlobalUser,
            user: "u".into(),
            project: "p".into(),
            ..Default::default()
        };
        assert_eq!(
            validate_scope(&global).unwrap_err().to_string(),
            "global-user memory scope may only contain user identity"
        );
    }

    #[test]
    fn recall_scopes_walk_ancestors() {
        let kinds: Vec<_> = recall_scopes(&exact(), RecallPolicy::Balanced, true)
            .unwrap()
            .iter()
            .map(|scope| scope.kind)
            .collect();
        assert_eq!(
            kinds,
            [
                ScopeKind::Exact,
                ScopeKind::Branch,
                ScopeKind::Repository,
                ScopeKind::Project,
                ScopeKind::GlobalUser
            ]
        );
        assert_eq!(
            recall_scopes(&exact(), RecallPolicy::Strict, true)
                .unwrap()
                .len(),
            1
        );
    }

    #[test]
    fn durable_scope_and_access_rules() {
        let durable = durable_memory_scope(exact());
        assert_eq!(durable.kind, ScopeKind::Branch);
        assert!(durable.session.is_empty());
        assert!(scope_can_access(
            &exact(),
            &durable,
            RecallPolicy::Balanced,
            false
        ));
        assert!(!scope_can_access(
            &exact(),
            &durable,
            RecallPolicy::Strict,
            false
        ));
        let global = MemoryScope {
            kind: ScopeKind::GlobalUser,
            user: "u".into(),
            ..Default::default()
        };
        assert!(!scope_can_access(
            &exact(),
            &global,
            RecallPolicy::Broad,
            false
        ));
        assert!(scope_can_access(
            &exact(),
            &global,
            RecallPolicy::Broad,
            true
        ));
        assert_eq!(scope_score(&exact(), &durable), 0.94);
    }

    #[test]
    fn remote_identity_normalisation() {
        assert_eq!(
            normalized_remote_identity("git@github.com:org/repo.git"),
            "github.com/org/repo"
        );
        assert_eq!(
            normalized_remote_identity("https://user@example.com/org/repo/"),
            "example.com/org/repo"
        );
        assert_eq!(normalized_remote_identity("/srv/git/repo"), "/srv/git/repo");
    }

    #[test]
    fn derives_scope_from_git_checkout() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("proj");
        std::fs::create_dir_all(root.join(".git")).unwrap();
        std::fs::write(root.join(".git/HEAD"), "ref: refs/heads/feature\n").unwrap();
        std::fs::write(
            root.join(".git/config"),
            "[remote \"origin\"]\n\turl = git@github.com:org/proj.git\n",
        )
        .unwrap();
        std::fs::create_dir_all(root.join("src")).unwrap();
        let scope = derive_runtime_memory_scope(&root.join("src"), "sess", "agent");
        assert_eq!(scope.user, "local_user");
        assert_eq!(scope.project, "proj");
        assert_eq!(scope.branch, "feature");
        assert_eq!(scope.repository_display, "github.com/org/proj");
        assert_eq!(
            scope.repository,
            format!("repo_{}", stable_hash_hex("github.com/org/proj"))
        );
        validate_scope(&scope).unwrap();
    }
}
