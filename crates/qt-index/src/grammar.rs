//! Tree-sitter grammars bundled with the index.
//!
//! C/C++ files use the C++ grammar (as in the archived index); Rust uses
//! `tree-sitter-rust`; TypeScript uses the TypeScript grammar, and `.tsx` and
//! JavaScript files use the TSX grammar, which also parses plain JavaScript
//! and JSX.

use std::path::Path;

use crate::scan::ProjectFileKind;
use crate::util::extension_lower;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Grammar {
    Cpp,
    Rust,
    TypeScript,
    Tsx,
}

impl Grammar {
    /// The grammar for a scanned file, or `None` for files without one.
    pub fn for_file(kind: ProjectFileKind, path: &Path) -> Option<Self> {
        match kind {
            k if k.is_cpp_candidate() => Some(Self::Cpp),
            ProjectFileKind::RustSource => Some(Self::Rust),
            ProjectFileKind::TypeScriptSource if extension_lower(path) == ".tsx" => Some(Self::Tsx),
            ProjectFileKind::TypeScriptSource => Some(Self::TypeScript),
            ProjectFileKind::JavaScriptSource => Some(Self::Tsx),
            _ => None,
        }
    }

    pub fn language(self) -> tree_sitter::Language {
        match self {
            Self::Cpp => tree_sitter_cpp::LANGUAGE.into(),
            Self::Rust => tree_sitter_rust::LANGUAGE.into(),
            Self::TypeScript => tree_sitter_typescript::LANGUAGE_TYPESCRIPT.into(),
            Self::Tsx => tree_sitter_typescript::LANGUAGE_TSX.into(),
        }
    }

    /// Name recorded in the tree-sitter manifest.
    pub fn name(self) -> &'static str {
        match self {
            Self::Cpp => "cpp",
            Self::Rust => "rust",
            Self::TypeScript => "typescript",
            Self::Tsx => "tsx",
        }
    }

    /// A parser for this grammar, or `None` when its ABI is incompatible with
    /// the linked tree-sitter runtime.
    pub fn parser(self) -> Option<tree_sitter::Parser> {
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&self.language()).ok()?;
        Some(parser)
    }
}
