//! Gitignore-aware project scan with the archived file classification.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::cancel::{Cancelled, Pacer};
use crate::gitignore::GitIgnoreMatcher;
use crate::util::{
    extension_lower, file_name, generic, is_nix_store_path, is_regular_file, lowercase_ascii,
    should_skip_scan_directory, sorted_dir_entries, weakly_canonical,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ProjectFileKind {
    CSource,
    CxxSource,
    RustSource,
    TypeScriptSource,
    VueSource,
    PythonSource,
    Header,
    BuildFile,
    Config,
    Other,
}

impl ProjectFileKind {
    /// Name used in the SQLite store and in handler payloads.
    pub fn as_str(self) -> &'static str {
        match self {
            Self::CSource => "c_source",
            Self::CxxSource => "cxx_source",
            Self::RustSource => "rust_source",
            Self::TypeScriptSource => "typescript_source",
            Self::VueSource => "vue_source",
            Self::PythonSource => "python_source",
            Self::Header => "header",
            Self::BuildFile => "build_file",
            Self::Config => "config",
            Self::Other => "other",
        }
    }

    pub fn parse(value: &str) -> Self {
        match value {
            "c_source" => Self::CSource,
            "cxx_source" => Self::CxxSource,
            "rust_source" => Self::RustSource,
            "typescript_source" => Self::TypeScriptSource,
            "vue_source" => Self::VueSource,
            "python_source" => Self::PythonSource,
            "header" => Self::Header,
            "build_file" => Self::BuildFile,
            "config" => Self::Config,
            _ => Self::Other,
        }
    }

    /// C and C++ translation units.
    pub fn is_native_source(self) -> bool {
        matches!(self, Self::CSource | Self::CxxSource)
    }

    /// Files covered by the C/C++ code index and tree-sitter metadata.
    pub fn is_cpp_candidate(self) -> bool {
        matches!(self, Self::CSource | Self::CxxSource | Self::Header)
    }

    /// Text kinds stored in the full-text index and searched by `code.search`.
    pub fn is_searchable(self) -> bool {
        !matches!(self, Self::Other)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProjectFile {
    /// Path relative to the project root.
    pub path: PathBuf,
    pub kind: ProjectFileKind,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ProjectScanResult {
    pub root: PathBuf,
    pub files: Vec<ProjectFile>,
    /// Relative paths of existing `compile_commands.json` files (root and `build/*/`).
    pub existing_compilation_databases: Vec<PathBuf>,
    pub build_files_by_type: BTreeMap<String, usize>,
    pub skipped_directories: usize,
}

/// Build-system family of a file name, as reported in `build_file_types`.
pub fn build_file_type(path: &Path) -> Option<&'static str> {
    let name = file_name(path);
    let lower = lowercase_ascii(&name);
    let ext = extension_lower(path);
    let name = name.as_str();
    let lower = lower.as_str();
    let ext = ext.as_str();
    if name == "CMakeLists.txt"
        || name == "CMakeLists.txt.in"
        || ext == ".cmake"
        || name == "CMakePresets.json"
        || name == "CMakeUserPresets.json"
    {
        return Some("CMake");
    }
    if name == "Makefile" || name == "makefile" || name == "GNUmakefile" || ext == ".mk" {
        return Some("Make");
    }
    if lower == "build.ninja" || lower == "rules.ninja" || ext == ".ninja" {
        return Some("Ninja");
    }
    if matches!(
        lower,
        "meson.build"
            | "meson.options"
            | "meson_options.txt"
            | "cross-file.txt"
            | "native-file.txt"
    ) {
        return Some("Meson");
    }
    if matches!(
        lower,
        "configure.ac" | "configure.in" | "makefile.am" | "makefile.in" | "aclocal.m4"
    ) {
        return Some("Autotools");
    }
    if matches!(
        name,
        "WORKSPACE" | "WORKSPACE.bazel" | "MODULE.bazel" | "BUILD" | "BUILD.bazel" | ".bazelrc"
    ) {
        return Some("Bazel");
    }
    if matches!(name, "BUCK" | "BUCK.v2" | ".buckconfig") {
        return Some("Buck");
    }
    if matches!(ext, ".pro" | ".pri" | ".prf") {
        return Some("QMake");
    }
    if lower == "sconstruct" || lower == "sconscript" {
        return Some("SCons");
    }
    if lower == "premake4.lua" || lower == "premake5.lua" {
        return Some("Premake");
    }
    if lower == "xmake.lua" {
        return Some("Xmake");
    }
    if lower == "wscript" {
        return Some("Waf");
    }
    if name == "BUILD.gn" || ext == ".gni" || name == ".gn" || name == "args.gn" {
        return Some("GN");
    }
    if matches!(
        ext,
        ".sln" | ".vcxproj" | ".filters" | ".props" | ".targets"
    ) {
        return Some("MSBuild");
    }
    if ext == ".xcodeproj"
        || ext == ".xcworkspace"
        || name == "project.pbxproj"
        || name == "Package.swift"
    {
        return Some("Xcode");
    }
    if matches!(
        lower,
        "platformio.ini" | "library.json" | "module.json" | "library.properties" | "sketch.yaml"
    ) {
        return Some("Embedded");
    }
    if matches!(lower, "cargo.toml" | "cargo.lock" | "build.rs") {
        return Some("Cargo");
    }
    if matches!(
        lower,
        "package.json"
            | "package-lock.json"
            | "vite.config.ts"
            | "vite.config.js"
            | "tsconfig.json"
    ) {
        return Some("Npm");
    }
    if matches!(
        lower,
        "setup.py" | "setup.cfg" | "pyproject.toml" | "requirements.txt"
    ) {
        return Some("Python");
    }
    if ext == ".px4board" || ext == ".board" {
        return Some("Embedded");
    }
    if matches!(
        lower,
        "vcpkg.json"
            | "conanfile.txt"
            | "conanfile.py"
            | "conan.lock"
            | "manifest.json"
            | "packages.config"
    ) {
        return Some("Package");
    }
    if matches!(
        lower,
        "flake.nix" | "flake.lock" | "shell.nix" | "default.nix"
    ) {
        return Some("Nix");
    }
    if name == "Dockerfile"
        || lower == "docker-compose.yml"
        || lower == "docker-compose.yaml"
        || lower == "devcontainer.json"
    {
        return Some("Container");
    }
    None
}

/// Classify a project file by name and extension.
pub fn classify_file(path: &Path) -> ProjectFileKind {
    if build_file_type(path).is_some() {
        return ProjectFileKind::BuildFile;
    }
    match extension_lower(path).as_str() {
        ".c" => ProjectFileKind::CSource,
        ".cc" | ".cpp" | ".cxx" | ".c++" | ".ino" => ProjectFileKind::CxxSource,
        ".rs" => ProjectFileKind::RustSource,
        ".ts" | ".tsx" => ProjectFileKind::TypeScriptSource,
        ".vue" => ProjectFileKind::VueSource,
        ".py" => ProjectFileKind::PythonSource,
        ".h" | ".hh" | ".hpp" | ".hxx" | ".ipp" | ".inl" => ProjectFileKind::Header,
        ".json" | ".yaml" | ".yml" | ".toml" | ".xml" | ".ini" | ".conf" => ProjectFileKind::Config,
        _ => ProjectFileKind::Other,
    }
}

/// Error from [`scan_project_tree`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ScanError {
    MissingRoot(PathBuf),
    Cancelled,
}

impl std::fmt::Display for ScanError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MissingRoot(root) => write!(f, "project root does not exist: {}", root.display()),
            Self::Cancelled => f.write_str("index scan cancelled"),
        }
    }
}

impl From<Cancelled> for ScanError {
    fn from(_: Cancelled) -> Self {
        Self::Cancelled
    }
}

/// Walk the project, skipping VCS/build/vendor directories, Nix store paths
/// and gitignored entries, and classify every regular file.
pub fn scan_project_tree(root: &Path, pacer: &Pacer) -> Result<ProjectScanResult, ScanError> {
    if !root.exists() {
        return Err(ScanError::MissingRoot(root.to_path_buf()));
    }
    let root = weakly_canonical(root);
    let gitignore = GitIgnoreMatcher::load(&root);
    let mut result = ProjectScanResult {
        root: root.clone(),
        ..Default::default()
    };

    let root_db = root.join("compile_commands.json");
    if is_regular_file(&root_db) {
        result
            .existing_compilation_databases
            .push(PathBuf::from("compile_commands.json"));
    }
    let build_dir = root.join("build");
    if build_dir.is_dir() {
        for entry in sorted_dir_entries(&build_dir) {
            if !crate::util::entry_is_dir(&entry) {
                continue;
            }
            let candidate = entry.path().join("compile_commands.json");
            if is_regular_file(&candidate) {
                let relative = candidate
                    .strip_prefix(&root)
                    .map(Path::to_path_buf)
                    .unwrap_or(candidate);
                result.existing_compilation_databases.push(relative);
            }
        }
    }

    let mut visited = 0usize;
    let mut files = Vec::new();
    let mut skipped = 0usize;
    let mut by_type: BTreeMap<String, usize> = BTreeMap::new();
    crate::util::walk(&root, |path, is_dir, _| {
        visited += 1;
        pacer.checkpoint(visited, 64, 10)?;
        if is_dir {
            if is_nix_store_path(path)
                || should_skip_scan_directory(path)
                || gitignore.ignores(path)
            {
                skipped += 1;
                return Ok(false);
            }
            return Ok(true);
        }
        if !is_regular_file(path) || is_nix_store_path(path) || gitignore.ignores(path) {
            return Ok(true);
        }
        let relative = path
            .strip_prefix(&root)
            .map(Path::to_path_buf)
            .unwrap_or_else(|_| path.to_path_buf());
        let kind = classify_file(&relative);
        if kind == ProjectFileKind::BuildFile
            && let Some(t) = build_file_type(&relative)
        {
            *by_type.entry(t.to_string()).or_default() += 1;
        }
        files.push(ProjectFile {
            path: relative,
            kind,
        });
        Ok(true)
    })?;

    files.sort_by_key(|a| generic(&a.path));
    result.files = files;
    result.skipped_directories = skipped;
    result.build_files_by_type = by_type;
    result
        .existing_compilation_databases
        .sort_by_key(|a| generic(a));
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    #[test]
    fn classification_matches_archived_rules() {
        assert_eq!(
            classify_file(Path::new("CMakeLists.txt")),
            ProjectFileKind::BuildFile
        );
        assert_eq!(
            classify_file(Path::new("a/package.json")),
            ProjectFileKind::BuildFile
        );
        assert_eq!(classify_file(Path::new("x.json")), ProjectFileKind::Config);
        assert_eq!(classify_file(Path::new("main.C")), ProjectFileKind::CSource);
        assert_eq!(
            classify_file(Path::new("sketch.ino")),
            ProjectFileKind::CxxSource
        );
        assert_eq!(classify_file(Path::new("a.hpp")), ProjectFileKind::Header);
        assert_eq!(
            classify_file(Path::new("lib.rs")),
            ProjectFileKind::RustSource
        );
        assert_eq!(
            classify_file(Path::new("build.rs")),
            ProjectFileKind::BuildFile
        );
        assert_eq!(
            classify_file(Path::new("App.vue")),
            ProjectFileKind::VueSource
        );
        assert_eq!(
            classify_file(Path::new("README.md")),
            ProjectFileKind::Other
        );
        assert_eq!(
            build_file_type(Path::new("board.px4board")),
            Some("Embedded")
        );
        assert_eq!(build_file_type(Path::new("wscript")), Some("Waf"));
    }

    #[test]
    fn scan_skips_ignored_and_build_directories() {
        let dir = tempfile::tempdir().unwrap();
        let root = fs::canonicalize(dir.path()).unwrap();
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("build/debug")).unwrap();
        fs::create_dir_all(root.join("node_modules/x")).unwrap();
        fs::create_dir_all(root.join("gen")).unwrap();
        fs::write(root.join(".gitignore"), "gen/\n*.log\n").unwrap();
        fs::write(root.join("src/main.cpp"), "int main() {}\n").unwrap();
        fs::write(root.join("src/a.h"), "").unwrap();
        fs::write(root.join("CMakeLists.txt"), "").unwrap();
        fs::write(root.join("x.log"), "").unwrap();
        fs::write(root.join("gen/g.c"), "").unwrap();
        fs::write(root.join("node_modules/x/y.c"), "").unwrap();
        fs::write(root.join("build/debug/compile_commands.json"), "[]").unwrap();
        let scan = scan_project_tree(&root, &Pacer::unpaced()).unwrap();
        let paths: Vec<String> = scan.files.iter().map(|f| generic(&f.path)).collect();
        assert_eq!(
            paths,
            vec![".gitignore", "CMakeLists.txt", "src/a.h", "src/main.cpp"]
        );
        assert_eq!(scan.build_files_by_type.get("CMake"), Some(&1));
        assert_eq!(scan.skipped_directories, 3);
        assert_eq!(
            scan.existing_compilation_databases,
            vec![PathBuf::from("build/debug/compile_commands.json")]
        );
    }

    #[test]
    fn missing_root_is_an_error() {
        let err =
            scan_project_tree(Path::new("/definitely/missing/qt"), &Pacer::unpaced()).unwrap_err();
        assert!(err.to_string().starts_with("project root does not exist"));
    }
}
