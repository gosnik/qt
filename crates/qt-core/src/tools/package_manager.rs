//! `package_manager`: detect package managers from marker files and plan
//! install/build/test/check commands without running them (the model-facing
//! part of archived `package_manager_tool.cpp`).

use std::path::{Path, PathBuf};

use futures::future::BoxFuture;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use super::{Tool, ToolContext, ToolOutput, parse_args};

const DEFAULT_BUILD_DIRECTORY: &str = "build";
const ACTIONS: &[&str] = &["detect", "install", "build", "test", "check"];

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum PackageManagerKind {
    Pnpm,
    Yarn,
    Npm,
    Cargo,
    Cmake,
}

impl PackageManagerKind {
    pub fn as_str(self) -> &'static str {
        match self {
            PackageManagerKind::Pnpm => "pnpm",
            PackageManagerKind::Yarn => "yarn",
            PackageManagerKind::Npm => "npm",
            PackageManagerKind::Cargo => "cargo",
            PackageManagerKind::Cmake => "cmake",
        }
    }

    fn is_javascript(self) -> bool {
        matches!(
            self,
            PackageManagerKind::Pnpm | PackageManagerKind::Yarn | PackageManagerKind::Npm
        )
    }
}

/// A manager found in `root`, with the marker files that identified it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackageManagerDetection {
    pub kind: PackageManagerKind,
    pub root: PathBuf,
    pub evidence: Vec<String>,
}

/// A command the model could run for an action.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PackageManagerCommandPlan {
    pub kind: PackageManagerKind,
    pub root: PathBuf,
    pub action: String,
    pub command: String,
    pub argv: Vec<String>,
}

fn existing(dir: &Path, names: &[&str]) -> Vec<String> {
    names
        .iter()
        .filter(|n| dir.join(n).is_file())
        .map(|n| n.to_string())
        .collect()
}

fn detect_javascript(dir: &Path) -> Option<PackageManagerDetection> {
    let with_manifest = |mut evidence: Vec<String>| {
        if dir.join("package.json").is_file() {
            evidence.push("package.json".into());
        }
        evidence
    };
    let candidates: [(PackageManagerKind, &[&str]); 3] = [
        (PackageManagerKind::Pnpm, &["pnpm-lock.yaml"]),
        (PackageManagerKind::Yarn, &["yarn.lock"]),
        (
            PackageManagerKind::Npm,
            &["package-lock.json", "npm-shrinkwrap.json"],
        ),
    ];
    for (kind, locks) in candidates {
        let evidence = existing(dir, locks);
        if !evidence.is_empty() {
            return Some(PackageManagerDetection {
                kind,
                root: dir.to_path_buf(),
                evidence: with_manifest(evidence),
            });
        }
    }
    let manifest = existing(dir, &["package.json"]);
    (!manifest.is_empty()).then(|| PackageManagerDetection {
        kind: PackageManagerKind::Npm,
        root: dir.to_path_buf(),
        evidence: manifest,
    })
}

fn detect_at(dir: &Path) -> Vec<PackageManagerDetection> {
    let mut found: Vec<PackageManagerDetection> = detect_javascript(dir).into_iter().collect();
    let cargo = existing(dir, &["Cargo.lock", "Cargo.toml"]);
    if !cargo.is_empty() {
        found.push(PackageManagerDetection {
            kind: PackageManagerKind::Cargo,
            root: dir.to_path_buf(),
            evidence: cargo,
        });
    }
    let cmake = existing(dir, &["CMakePresets.json", "CMakeLists.txt"]);
    if !cmake.is_empty() {
        found.push(PackageManagerDetection {
            kind: PackageManagerKind::Cmake,
            root: dir.to_path_buf(),
            evidence: cmake,
        });
    }
    found.sort_by_key(|d| d.kind);
    found
}

/// Walk from `start` up to `stop` (inclusive; the filesystem root when
/// `None`) and report the nearest detection of each manager kind. Only the
/// nearest JavaScript manager is reported.
pub fn detect_package_managers(start: &Path, stop: Option<&Path>) -> Vec<PackageManagerDetection> {
    let mut result: Vec<PackageManagerDetection> = Vec::new();
    let mut seen_javascript = false;
    let mut current = Some(start);
    while let Some(dir) = current {
        for detection in detect_at(dir) {
            if detection.kind.is_javascript() && seen_javascript {
                continue;
            }
            if result.iter().any(|d| d.kind == detection.kind) {
                continue;
            }
            seen_javascript |= detection.kind.is_javascript();
            result.push(detection);
        }
        if stop.is_some_and(|s| s == dir) {
            break;
        }
        current = dir.parent();
    }
    result
}

fn argv_for(kind: PackageManagerKind, action: &str) -> Option<Vec<String>> {
    let v = |args: &[&str]| Some(args.iter().map(|s| s.to_string()).collect());
    match (kind, action) {
        (PackageManagerKind::Npm, "install") => v(&["npm", "install"]),
        (PackageManagerKind::Npm, "build" | "test" | "check") => v(&["npm", "run", action]),
        (PackageManagerKind::Pnpm, "install") => v(&["pnpm", "install"]),
        (PackageManagerKind::Pnpm, "build" | "test" | "check") => v(&["pnpm", "run", action]),
        (PackageManagerKind::Yarn, "install") => v(&["yarn", "install"]),
        (PackageManagerKind::Yarn, "build" | "test" | "check") => v(&["yarn", action]),
        (PackageManagerKind::Cargo, "install") => v(&["cargo", "fetch"]),
        (PackageManagerKind::Cargo, "build") => v(&["cargo", "build"]),
        (PackageManagerKind::Cargo, "test") => v(&["cargo", "test"]),
        (PackageManagerKind::Cargo, "check") => v(&["cargo", "check"]),
        (PackageManagerKind::Cmake, "install") => {
            v(&["cmake", "-S", ".", "-B", DEFAULT_BUILD_DIRECTORY])
        }
        (PackageManagerKind::Cmake, "build") => v(&["cmake", "--build", DEFAULT_BUILD_DIRECTORY]),
        (PackageManagerKind::Cmake, "test" | "check") => {
            v(&["ctest", "--test-dir", DEFAULT_BUILD_DIRECTORY])
        }
        _ => None,
    }
}

/// Command plans for `action` (`install`, `build`, `test` or `check`).
pub fn package_manager_command_plans(
    detections: &[PackageManagerDetection],
    action: &str,
) -> Vec<PackageManagerCommandPlan> {
    detections
        .iter()
        .filter_map(|d| {
            let argv = argv_for(d.kind, action)?;
            Some(PackageManagerCommandPlan {
                kind: d.kind,
                root: d.root.clone(),
                action: action.to_string(),
                command: argv.join(" "),
                argv,
            })
        })
        .collect()
}

pub struct PackageManagerTool;

#[derive(Deserialize)]
struct Args {
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    action: Option<String>,
}

impl Tool for PackageManagerTool {
    fn name(&self) -> &'static str {
        "package_manager"
    }
    fn description(&self) -> String {
        "Detect package managers from project marker files and generate command plans without executing them.".into()
    }
    fn snippet(&self) -> &'static str {
        "Detect package managers and plan install/build/test/check commands"
    }
    fn parameters(&self) -> Value {
        json!({
            "type": "object",
            "properties": {
                "path": { "type": "string", "description": "Directory or file path to start detection from. Defaults to the current directory." },
                "action": { "type": "string", "enum": ACTIONS, "description": "Action to plan. detect returns detections only; other actions also return command plans." }
            },
            "additionalProperties": false
        })
    }
    fn parallel_safe(&self) -> bool {
        true
    }

    fn execute(&self, args: Value, ctx: ToolContext) -> BoxFuture<'static, ToolOutput> {
        Box::pin(async move {
            let args: Args = match parse_args("package_manager", args) {
                Ok(a) => a,
                Err(e) => return e,
            };
            let action = args
                .action
                .as_deref()
                .unwrap_or("detect")
                .trim()
                .to_string();
            if !ACTIONS.contains(&action.as_str()) {
                return ToolOutput::error("package_manager action is not supported");
            }
            let resolved = match ctx.resolve_path(args.path.as_deref().unwrap_or(".")) {
                Ok(r) => r,
                Err(e) => return e,
            };
            let meta = match std::fs::metadata(&resolved.canonical) {
                Ok(m) => m,
                Err(_) => {
                    return ToolOutput::error(format!(
                        "Path not found: {}",
                        ctx.display(&resolved.path)
                    ));
                }
            };
            let start = if meta.is_file() {
                resolved
                    .canonical
                    .parent()
                    .map(Path::to_path_buf)
                    .unwrap_or(resolved.canonical.clone())
            } else {
                resolved.canonical.clone()
            };
            let stop = (!ctx.policy.allows_external_access()).then(|| ctx.policy.root());
            let detections = detect_package_managers(&start, stop);
            let plans = if action == "detect" {
                Vec::new()
            } else {
                package_manager_command_plans(&detections, &action)
            };
            render(&ctx, &start, &action, &detections, &plans)
        })
    }
}

fn render(
    ctx: &ToolContext,
    start: &Path,
    action: &str,
    detections: &[PackageManagerDetection],
    plans: &[PackageManagerCommandPlan],
) -> ToolOutput {
    // Detection roots are canonical; show them relative to the canonical cwd.
    let cwd = crate::policy::weakly_canonical(&ctx.cwd);
    let show = |p: &Path| match p.strip_prefix(&cwd) {
        Ok(rel) if rel.as_os_str().is_empty() => ".".to_string(),
        Ok(rel) => rel.to_string_lossy().replace('\\', "/"),
        Err(_) => p.display().to_string(),
    };
    let mut text = String::new();
    if detections.is_empty() {
        text.push_str("No package managers detected.");
    } else {
        text.push_str("Detected package managers:");
        for d in detections {
            text.push_str(&format!(
                "\n- {} in {} ({})",
                d.kind.as_str(),
                show(&d.root),
                d.evidence.join(", ")
            ));
        }
    }
    if action != "detect" {
        if plans.is_empty() {
            text.push_str(&format!("\n\nNo {action} command available."));
        } else {
            text.push_str(&format!("\n\nCommand plans for {action} (not executed):"));
            for p in plans {
                text.push_str(&format!("\n- {} (in {})", p.command, show(&p.root)));
            }
        }
    }
    ToolOutput::text(text).with_details(json!({
        "start_directory": show(start),
        "action": action,
        "detections": detections.iter().map(|d| json!({
            "manager": d.kind.as_str(),
            "root": show(&d.root),
            "evidence": d.evidence,
        })).collect::<Vec<_>>(),
        "command_plans": plans.iter().map(|p| json!({
            "manager": p.kind.as_str(),
            "root": show(&p.root),
            "action": p.action,
            "command": p.command,
            "argv": p.argv,
        })).collect::<Vec<_>>(),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_nearest_managers_up_to_the_root() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().canonicalize().unwrap();
        std::fs::create_dir_all(root.join("web/app")).unwrap();
        std::fs::write(root.join("Cargo.toml"), "").unwrap();
        std::fs::write(root.join("package.json"), "{}").unwrap();
        std::fs::write(root.join("web/package.json"), "{}").unwrap();
        std::fs::write(root.join("web/yarn.lock"), "").unwrap();
        let found = detect_package_managers(&root.join("web/app"), Some(&root));
        let kinds: Vec<_> = found.iter().map(|d| d.kind).collect();
        assert_eq!(kinds, [PackageManagerKind::Yarn, PackageManagerKind::Cargo]);
        assert_eq!(found[0].evidence, ["yarn.lock", "package.json"]);
        assert_eq!(found[0].root, root.join("web"));
        // Stopping below the Cargo project hides it.
        let found = detect_package_managers(&root.join("web/app"), Some(&root.join("web")));
        assert_eq!(found.len(), 1);

        let plans =
            package_manager_command_plans(&detect_package_managers(&root, Some(&root)), "test");
        let commands: Vec<_> = plans.iter().map(|p| p.command.as_str()).collect();
        assert_eq!(commands, ["npm run test", "cargo test"]);
        let cmake = PackageManagerDetection {
            kind: PackageManagerKind::Cmake,
            root: root.clone(),
            evidence: vec![],
        };
        assert_eq!(
            package_manager_command_plans(&[cmake], "install")[0].command,
            "cmake -S . -B build"
        );
    }

    #[tokio::test]
    async fn tool_renders_detections_and_plans() {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("Cargo.toml"), "").unwrap();
        std::fs::write(dir.path().join("Cargo.lock"), "").unwrap();
        let ctx = ToolContext::new(dir.path());
        let out = PackageManagerTool
            .execute(json!({"action": "build"}), ctx.clone())
            .await;
        assert_eq!(
            out.text_content(),
            "Detected package managers:\n- cargo in . (Cargo.lock, Cargo.toml)\n\nCommand plans for build (not executed):\n- cargo build (in .)"
        );
        let details = out.details.unwrap();
        assert_eq!(
            details["command_plans"][0]["argv"],
            json!(["cargo", "build"])
        );
        let out = PackageManagerTool
            .execute(json!({"action": "deploy"}), ctx.clone())
            .await;
        assert_eq!(
            out.text_content(),
            "package_manager action is not supported"
        );
        let out = PackageManagerTool.execute(json!({"path": "/"}), ctx).await;
        assert!(out.is_error);
    }
}
