//! Tree-sitter code index for Rust, TypeScript and JavaScript.
//!
//! Each source is parsed with its grammar ([`Grammar`]) and walked for the
//! same symbols the C/C++ index records, so every code operation and tool
//! works across languages:
//!
//! - **Types** become [`ClassSymbol`]s with a `kind`: Rust `struct`, `enum`,
//!   `union`, `trait` and `type`; TypeScript `class`, `interface`, `enum` and
//!   `type`.
//! - **Functions**: Rust `fn` items (`method` inside `impl` and `trait`
//!   blocks); TypeScript function declarations, class methods, and top-level
//!   or class-field arrow functions and function expressions. Code nested in
//!   a function body (closures, local functions) belongs to that function.
//! - **Calls** inside function bodies become [`CallGraphEdge`]s: the callee is
//!   the called path (`helper`, `Foo::new`, `Config.load`) or, for a method
//!   call on a value (`x.run()`, `this.run()`), the method name.
//!
//! Rust names are qualified with the crate name (from the nearest
//! `Cargo.toml`) and the module path derived from the file path
//! (`qt_core::session::SessionEntry::from_value`); methods of an `impl` block
//! are attached to their type even when it is defined in another file.
//! TypeScript names are qualified by namespace and class with `.`
//! (`Store.save`); the file disambiguates top-level names.
//!
//! Results are cached per file by content hash in a [`SyntaxCache`], so the
//! background service's periodic rescans only reparse changed files.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use tree_sitter::{Node, Parser};

use crate::cancel::{Cancelled, Pacer};
use crate::code_index::{
    CallGraphEdge, ClassSymbol, CodeIndex, FunctionSymbol, MethodSymbol, SkippedCodeIndexFile,
    dependency_or_generated_reason,
};
use crate::grammar::Grammar;
use crate::scan::ProjectScanResult;
use crate::util::fnv1a64;

/// Files larger than this are skipped (bundles and generated tables).
const MAX_SOURCE_BYTES: usize = 2 * 1024 * 1024;

/// Symbols extracted from one file, before cross-file method attachment.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FileSymbols {
    pub classes: Vec<ClassSymbol>,
    pub functions: Vec<FunctionSymbol>,
    pub calls: Vec<CallGraphEdge>,
    /// Rust `impl` methods, attached to their type after every file is read.
    pub impl_methods: Vec<ImplMethod>,
}

/// A method from a Rust `impl` block.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImplMethod {
    /// Qualified type path as written in this module (`crate::m::Type`).
    pub owner: String,
    /// The type's last path segment (`Type`).
    pub owner_name: String,
    pub method: MethodSymbol,
}

/// Per-file extraction results from earlier scans, keyed by content hash.
#[derive(Debug, Default)]
pub struct SyntaxCache {
    entries: HashMap<PathBuf, (u64, Arc<FileSymbols>)>,
}

impl SyntaxCache {
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

/// Parsers created on first use, one per grammar.
#[derive(Default)]
struct Parsers {
    parsers: HashMap<Grammar, Option<Parser>>,
}

impl Parsers {
    fn get(&mut self, grammar: Grammar) -> Option<&mut Parser> {
        self.parsers
            .entry(grammar)
            .or_insert_with(|| grammar.parser())
            .as_mut()
    }
}

/// Add the scan's Rust, TypeScript and JavaScript files to `index`, reusing
/// `cache` entries for unchanged files and dropping entries for files that
/// are gone.
pub fn index_syntax_files(
    index: &mut CodeIndex,
    scan: &ProjectScanResult,
    cache: &mut SyntaxCache,
    pacer: &Pacer,
    operations: &mut usize,
) -> Result<(), Cancelled> {
    let mut parsers = Parsers::default();
    let mut crates = CrateNames::default();
    let mut seen: HashMap<PathBuf, (u64, Arc<FileSymbols>)> = HashMap::new();
    let mut impl_methods = Vec::new();
    for file in &scan.files {
        if !file.kind.is_syntax_candidate() {
            continue;
        }
        *operations += 1;
        pacer.checkpoint(*operations, 16, 5)?;
        let Some(grammar) = Grammar::for_file(file.kind, &file.path) else {
            continue;
        };
        let skip = |index: &mut CodeIndex, reason: &str| {
            index.skipped_files.push(SkippedCodeIndexFile {
                file: file.path.clone(),
                reason: reason.into(),
            });
        };
        if let Some(reason) = syntax_skip_reason(&file.path) {
            skip(index, reason);
            continue;
        }
        let Ok(content) = std::fs::read(scan.root.join(&file.path)) else {
            continue;
        };
        if content.len() > MAX_SOURCE_BYTES {
            skip(index, "file too large");
            continue;
        }
        let hash = fnv1a64(&content);
        let symbols = match cache.entries.get(&file.path) {
            Some((cached, symbols)) if *cached == hash => symbols.clone(),
            _ => {
                let Some(parser) = parsers.get(grammar) else {
                    skip(index, "tree-sitter grammar unavailable");
                    continue;
                };
                let Some(tree) = parser.parse(&content, None) else {
                    skip(index, "parse failed");
                    continue;
                };
                let module = match grammar {
                    Grammar::Rust => rust_module_path(&scan.root, &file.path, &mut crates),
                    _ => String::new(),
                };
                let mut extractor = Extractor {
                    source: &content,
                    file: &file.path,
                    out: FileSymbols::default(),
                };
                match grammar {
                    Grammar::Rust => extractor.rust_items(tree.root_node(), &module, None),
                    _ => extractor.ts_items(tree.root_node(), ""),
                }
                Arc::new(extractor.out)
            }
        };
        index.classes.extend(symbols.classes.iter().cloned());
        index.functions.extend(symbols.functions.iter().cloned());
        index.call_graph.extend(symbols.calls.iter().cloned());
        impl_methods.extend(symbols.impl_methods.iter().cloned());
        seen.insert(file.path.clone(), (hash, symbols));
    }
    cache.entries = seen;
    attach_impl_methods(&mut index.classes, impl_methods);
    Ok(())
}

/// Attach `impl` methods to their type: by qualified path first, then by
/// name when exactly one type has it.
fn attach_impl_methods(classes: &mut [ClassSymbol], methods: Vec<ImplMethod>) {
    let mut by_qualified: HashMap<String, usize> = HashMap::new();
    let mut by_name: HashMap<String, Vec<usize>> = HashMap::new();
    for (i, class) in classes.iter().enumerate() {
        if !class.file.extension().is_some_and(|e| e == "rs") {
            continue;
        }
        by_qualified.insert(class.qualified_name.clone(), i);
        by_name.entry(class.name.clone()).or_default().push(i);
    }
    for method in methods {
        let target = by_qualified.get(&method.owner).copied().or_else(|| {
            match by_name.get(&method.owner_name).map(Vec::as_slice) {
                Some([only]) => Some(*only),
                _ => None,
            }
        });
        if let Some(i) = target {
            classes[i].methods.push(method.method);
        }
    }
}

/// Crate names by directory, read from `Cargo.toml` `[package] name`.
#[derive(Default)]
struct CrateNames {
    by_dir: HashMap<PathBuf, Option<String>>,
}

impl CrateNames {
    fn name(&mut self, root: &Path, dir: &Path) -> Option<String> {
        self.by_dir
            .entry(dir.to_path_buf())
            .or_insert_with(|| {
                let text = std::fs::read_to_string(root.join(dir).join("Cargo.toml")).ok()?;
                Some(package_name(&text).unwrap_or_default().replace('-', "_"))
            })
            .clone()
    }
}

/// `name` from the `[package]` table of a `Cargo.toml` (empty for a
/// workspace manifest).
fn package_name(text: &str) -> Option<String> {
    let mut in_package = false;
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_package = line == "[package]";
            continue;
        }
        if !in_package {
            continue;
        }
        let Some(value) = line
            .strip_prefix("name")
            .and_then(|rest| rest.trim_start().strip_prefix('='))
        else {
            continue;
        };
        return Some(
            value
                .trim()
                .trim_matches(|c| c == '"' || c == '\'')
                .to_string(),
        );
    }
    None
}

/// Module path of a Rust file: the crate name of the nearest `Cargo.toml`
/// above it, then the path below `src/` with `lib.rs`, `main.rs` and
/// `mod.rs` dropped (`crates/qt-core/src/session/mod.rs` →
/// `qt_core::session`). Files outside `src/` (tests, examples) keep their
/// directory names; files in no crate use their project path.
fn rust_module_path(root: &Path, relative: &Path, crates: &mut CrateNames) -> String {
    let mut dir = relative.parent().unwrap_or(Path::new(""));
    let (crate_name, crate_dir) = loop {
        match crates.name(root, dir) {
            Some(name) => break (name, dir.to_path_buf()),
            None => match dir.parent() {
                Some(parent) => dir = parent,
                None => break (String::new(), PathBuf::new()),
            },
        }
    };
    let inner = relative.strip_prefix(&crate_dir).unwrap_or(relative);
    let mut parts: Vec<String> = inner
        .with_extension("")
        .components()
        .map(|c| c.as_os_str().to_string_lossy().replace('-', "_"))
        .collect();
    if parts.first().is_some_and(|p| p == "src") {
        parts.remove(0);
        if parts.len() == 1 && (parts[0] == "lib" || parts[0] == "main") {
            parts.clear();
        }
    }
    if parts.last().is_some_and(|p| p == "mod") {
        parts.pop();
    }
    let mut path = Vec::new();
    if !crate_name.is_empty() {
        path.push(crate_name);
    }
    path.extend(parts);
    path.join("::")
}

fn join(scope: &str, name: &str, separator: &str) -> String {
    if scope.is_empty() {
        name.to_string()
    } else {
        format!("{scope}{separator}{name}")
    }
}

/// Resolve a leading `crate::`, `self::` or `super::` in `path` against the
/// module path `scope` (whose first segment is the crate name).
fn resolve_rust_path(scope: &str, path: &str) -> String {
    let mut module: Vec<&str> = scope.split("::").filter(|s| !s.is_empty()).collect();
    let mut rest = path;
    loop {
        if let Some(r) = rest.strip_prefix("crate::") {
            module.truncate(1);
            rest = r;
        } else if let Some(r) = rest.strip_prefix("self::") {
            rest = r;
        } else if let Some(r) = rest.strip_prefix("super::") {
            module.pop();
            rest = r;
        } else {
            break;
        }
    }
    if rest.len() == path.len() {
        return path.to_string();
    }
    module.push(rest);
    module.join("::")
}

/// Directories holding build output or dependencies at any depth, and
/// bundled files, skipped by the syntax index.
pub fn syntax_skip_reason(relative: &Path) -> Option<&'static str> {
    if let Some(reason) = dependency_or_generated_reason(relative) {
        return Some(reason);
    }
    let directories = relative.parent().unwrap_or(Path::new(""));
    for part in directories.components() {
        let part = part.as_os_str().to_string_lossy();
        if matches!(
            part.as_ref(),
            "node_modules"
                | "target"
                | "dist"
                | "coverage"
                | ".next"
                | ".nuxt"
                | ".svelte-kit"
                | ".turbo"
                | ".output"
        ) {
            return Some("build, cache, or dependency directory");
        }
    }
    let name = relative
        .file_name()
        .map(|n| n.to_string_lossy().to_ascii_lowercase())
        .unwrap_or_default();
    if [".min.js", ".min.mjs", ".min.cjs", ".bundle.js"]
        .iter()
        .any(|suffix| name.ends_with(suffix))
    {
        return Some("generated source file");
    }
    None
}

/// Remove generic arguments (`Vec::<u8>::new` → `Vec::new`).
fn strip_generics(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    let mut depth = 0usize;
    for ch in text.chars() {
        match ch {
            '<' => depth += 1,
            '>' if depth > 0 => depth -= 1,
            _ if depth == 0 && !ch.is_whitespace() => out.push(ch),
            _ => {}
        }
    }
    out.replace("::::", "::").trim_end_matches("::").to_string()
}

struct Extractor<'a> {
    source: &'a [u8],
    file: &'a Path,
    out: FileSymbols,
}

impl Extractor<'_> {
    fn text(&self, node: Node) -> String {
        node.utf8_text(self.source).unwrap_or_default().to_string()
    }

    fn field_text(&self, node: Node, field: &str) -> Option<String> {
        node.child_by_field_name(field)
            .map(|n| self.text(n))
            .filter(|t| !t.is_empty())
    }

    fn line(node: Node) -> usize {
        node.start_position().row + 1
    }

    fn class(&mut self, node: Node, name: String, qualified_name: String, kind: &str) -> usize {
        self.out.classes.push(ClassSymbol {
            name,
            qualified_name,
            file: self.file.to_path_buf(),
            line: Self::line(node),
            kind: kind.into(),
            methods: Vec::new(),
        });
        self.out.classes.len() - 1
    }

    fn function(&mut self, node: Node, name: &str, qualified_name: &str, kind: &str) {
        self.out.functions.push(FunctionSymbol {
            name: name.into(),
            qualified_name: qualified_name.into(),
            file: self.file.to_path_buf(),
            line: Self::line(node),
            kind: kind.into(),
        });
    }

    fn method(&self, node: Node, name: &str, qualified_name: &str) -> MethodSymbol {
        MethodSymbol {
            name: name.into(),
            qualified_name: qualified_name.into(),
            file: self.file.to_path_buf(),
            line: Self::line(node),
        }
    }

    fn call(&mut self, caller: &str, callee: String, node: Node) {
        if callee.is_empty() {
            return;
        }
        self.out.calls.push(CallGraphEdge {
            caller: caller.into(),
            callee,
            file: self.file.to_path_buf(),
            line: Self::line(node),
        });
    }

    // ---- Rust ----

    /// Items of a source file, `mod` block, `impl` or `trait` body. `owner`
    /// is the `impl`/`trait` type: (qualified path, name, class index when
    /// the type is in this file's classes).
    fn rust_items(&mut self, node: Node, scope: &str, owner: Option<(&str, &str, Option<usize>)>) {
        let mut cursor = node.walk();
        for item in node.named_children(&mut cursor) {
            match item.kind() {
                "mod_item" => {
                    let (Some(name), Some(body)) = (
                        self.field_text(item, "name"),
                        item.child_by_field_name("body"),
                    ) else {
                        continue;
                    };
                    self.rust_items(body, &join(scope, &name, "::"), None);
                }
                "struct_item" | "enum_item" | "union_item" | "type_item" => {
                    let Some(name) = self.field_text(item, "name") else {
                        continue;
                    };
                    let kind = match item.kind() {
                        "struct_item" => "struct",
                        "enum_item" => "enum",
                        "union_item" => "union",
                        _ => "type",
                    };
                    let qualified = join(scope, &name, "::");
                    self.class(item, name, qualified, kind);
                }
                "trait_item" => {
                    let Some(name) = self.field_text(item, "name") else {
                        continue;
                    };
                    let qualified = join(scope, &name, "::");
                    let class = self.class(item, name.clone(), qualified.clone(), "trait");
                    if let Some(body) = item.child_by_field_name("body") {
                        self.rust_items(body, scope, Some((&qualified, &name, Some(class))));
                    }
                }
                "impl_item" => {
                    let (Some(ty), Some(body)) = (
                        item.child_by_field_name("type"),
                        item.child_by_field_name("body"),
                    ) else {
                        continue;
                    };
                    let (owner, owner_name) = self.rust_impl_owner(ty, scope);
                    self.rust_items(body, scope, Some((&owner, &owner_name, None)));
                }
                "function_signature_item" => {
                    if let (Some((owner, _, Some(class))), Some(name)) =
                        (owner, self.field_text(item, "name"))
                    {
                        let method = self.method(item, &name, &join(owner, &name, "::"));
                        self.out.classes[class].methods.push(method);
                    }
                }
                "function_item" => {
                    let Some(name) = self.field_text(item, "name") else {
                        continue;
                    };
                    let (qualified, kind) = match owner {
                        Some((owner, owner_name, class)) => {
                            let qualified = join(owner, &name, "::");
                            let method = self.method(item, &name, &qualified);
                            match class {
                                Some(class) => self.out.classes[class].methods.push(method),
                                None => self.out.impl_methods.push(ImplMethod {
                                    owner: owner.into(),
                                    owner_name: owner_name.into(),
                                    method,
                                }),
                            }
                            (qualified, "method")
                        }
                        None => (join(scope, &name, "::"), "function"),
                    };
                    self.function(item, &name, &qualified, kind);
                    if let Some(body) = item.child_by_field_name("body") {
                        let self_type = owner.map(|(_, name, _)| name);
                        self.rust_calls(body, &qualified, self_type);
                    }
                }
                // `#[cfg(..)]`-gated or macro-wrapped item lists.
                "declaration_list" => self.rust_items(item, scope, owner),
                _ => {}
            }
        }
    }

    /// Qualified path and name of an `impl` block's type.
    fn rust_impl_owner(&self, ty: Node, scope: &str) -> (String, String) {
        match ty.kind() {
            "generic_type" => match ty.child_by_field_name("type") {
                Some(inner) => self.rust_impl_owner(inner, scope),
                None => {
                    let text = strip_generics(&self.text(ty));
                    (join(scope, &text, "::"), text)
                }
            },
            "type_identifier" => {
                let name = self.text(ty);
                (join(scope, &name, "::"), name)
            }
            "scoped_type_identifier" => {
                let path = resolve_rust_path(scope, &strip_generics(&self.text(ty)));
                let name = self
                    .field_text(ty, "name")
                    .unwrap_or_else(|| path.rsplit("::").next().unwrap_or_default().to_string());
                (path, name)
            }
            _ => {
                let text = strip_generics(&self.text(ty));
                (join(scope, &text, "::"), text)
            }
        }
    }

    fn rust_calls(&mut self, node: Node, caller: &str, self_type: Option<&str>) {
        let mut stack = vec![node];
        while let Some(node) = stack.pop() {
            if node.kind() == "call_expression"
                && let Some(function) = node.child_by_field_name("function")
                && let Some(callee) = self.rust_callee(function, self_type)
            {
                self.call(caller, callee, node);
            }
            let mut cursor = node.walk();
            let children: Vec<Node> = node.named_children(&mut cursor).collect();
            // Visit children in source order.
            stack.extend(children.into_iter().rev());
        }
    }

    fn rust_callee(&self, function: Node, self_type: Option<&str>) -> Option<String> {
        match function.kind() {
            "identifier" => Some(self.text(function)),
            "scoped_identifier" => {
                let path = strip_generics(&self.text(function));
                Some(match (path.strip_prefix("Self::"), self_type) {
                    (Some(rest), Some(ty)) => format!("{ty}::{rest}"),
                    _ => path,
                })
            }
            "field_expression" => self.field_text(function, "field"),
            "generic_function" => function
                .child_by_field_name("function")
                .and_then(|f| self.rust_callee(f, self_type)),
            _ => None,
        }
    }

    // ---- TypeScript / JavaScript ----

    /// Statements of a program, namespace body or export wrapper.
    fn ts_items(&mut self, node: Node, scope: &str) {
        let mut cursor = node.walk();
        for item in node.named_children(&mut cursor) {
            match item.kind() {
                "export_statement" | "ambient_declaration" | "expression_statement" => {
                    self.ts_items(item, scope)
                }
                "internal_module" | "module" => {
                    let (Some(name), Some(body)) = (
                        self.field_text(item, "name"),
                        item.child_by_field_name("body"),
                    ) else {
                        continue;
                    };
                    let name = name.trim_matches(|c| c == '"' || c == '\'').to_string();
                    self.ts_items(body, &join(scope, &name, "."));
                }
                "class_declaration" | "abstract_class_declaration" | "class" => {
                    if let Some(name) = self.field_text(item, "name") {
                        self.ts_class(item, scope, &name);
                    }
                }
                "interface_declaration" => {
                    let Some(name) = self.field_text(item, "name") else {
                        continue;
                    };
                    let qualified = join(scope, &name, ".");
                    let class = self.class(item, name, qualified.clone(), "interface");
                    if let Some(body) = item.child_by_field_name("body") {
                        self.ts_signatures(body, &qualified, class);
                    }
                }
                "enum_declaration" | "type_alias_declaration" => {
                    let Some(name) = self.field_text(item, "name") else {
                        continue;
                    };
                    let kind = if item.kind() == "enum_declaration" {
                        "enum"
                    } else {
                        "type"
                    };
                    let qualified = join(scope, &name, ".");
                    self.class(item, name, qualified, kind);
                }
                "function_declaration" | "generator_function_declaration" => {
                    let Some(name) = self.field_text(item, "name") else {
                        continue;
                    };
                    let qualified = join(scope, &name, ".");
                    self.function(item, &name, &qualified, "function");
                    if let Some(body) = item.child_by_field_name("body") {
                        self.ts_calls(body, &qualified);
                    }
                }
                "lexical_declaration" | "variable_declaration" => {
                    let mut inner = item.walk();
                    for declarator in item.named_children(&mut inner) {
                        if declarator.kind() == "variable_declarator" {
                            self.ts_declarator(declarator, scope);
                        }
                    }
                }
                _ => {}
            }
        }
    }

    /// `const name = () => ..` / `function () ..` / `class { .. }`.
    fn ts_declarator(&mut self, declarator: Node, scope: &str) {
        let (Some(name_node), Some(value)) = (
            declarator.child_by_field_name("name"),
            declarator.child_by_field_name("value"),
        ) else {
            return;
        };
        if name_node.kind() != "identifier" {
            return;
        }
        let name = self.text(name_node);
        match value.kind() {
            "arrow_function" | "function_expression" | "function" | "generator_function" => {
                let qualified = join(scope, &name, ".");
                self.function(declarator, &name, &qualified, "function");
                if let Some(body) = value.child_by_field_name("body") {
                    self.ts_calls(body, &qualified);
                }
            }
            "class" => self.ts_class(value, scope, &name),
            _ => {}
        }
    }

    fn ts_class(&mut self, node: Node, scope: &str, name: &str) {
        let qualified = join(scope, name, ".");
        let class = self.class(node, name.into(), qualified.clone(), "class");
        let Some(body) = node.child_by_field_name("body") else {
            return;
        };
        // One method entry per name: an implementation replaces its overload
        // signatures, and a getter/setter pair is listed once.
        let mut signature_only: HashMap<String, usize> = HashMap::new();
        let mut listed: HashSet<String> = HashSet::new();
        let mut cursor = body.walk();
        for member in body.named_children(&mut cursor) {
            let function_body = match member.kind() {
                "method_definition" => member.child_by_field_name("body"),
                "public_field_definition" => member
                    .child_by_field_name("value")
                    .filter(|v| matches!(v.kind(), "arrow_function" | "function_expression"))
                    .and_then(|v| v.child_by_field_name("body")),
                "abstract_method_signature" | "method_signature" => {
                    if let Some(method_name) = self.field_text(member, "name")
                        && listed.insert(method_name.clone())
                    {
                        let method =
                            self.method(member, &method_name, &join(&qualified, &method_name, "."));
                        let methods = &mut self.out.classes[class].methods;
                        signature_only.insert(method_name, methods.len());
                        methods.push(method);
                    }
                    continue;
                }
                _ => continue,
            };
            let (Some(method_name), Some(function_body)) =
                (self.field_text(member, "name"), function_body)
            else {
                continue;
            };
            let method_qualified = join(&qualified, &method_name, ".");
            let method = self.method(member, &method_name, &method_qualified);
            if let Some(slot) = signature_only.remove(&method_name) {
                self.out.classes[class].methods[slot] = method;
            } else if listed.insert(method_name.clone()) {
                self.out.classes[class].methods.push(method);
            }
            self.function(member, &method_name, &method_qualified, "method");
            self.ts_calls(function_body, &method_qualified);
        }
    }

    /// Method signatures of an interface body.
    fn ts_signatures(&mut self, body: Node, qualified: &str, class: usize) {
        let mut cursor = body.walk();
        for member in body.named_children(&mut cursor) {
            let is_method = member.kind() == "method_signature"
                || (member.kind() == "property_signature"
                    && member
                        .child_by_field_name("type")
                        .and_then(|t| t.named_child(0))
                        .is_some_and(|t| t.kind() == "function_type"));
            if !is_method {
                continue;
            }
            if let Some(name) = self.field_text(member, "name") {
                let method = self.method(member, &name, &join(qualified, &name, "."));
                self.out.classes[class].methods.push(method);
            }
        }
    }

    fn ts_calls(&mut self, node: Node, caller: &str) {
        let mut stack = vec![node];
        while let Some(node) = stack.pop() {
            let callee = match node.kind() {
                "call_expression" => node
                    .child_by_field_name("function")
                    .and_then(|f| self.ts_callee(f)),
                "new_expression" => node
                    .child_by_field_name("constructor")
                    .and_then(|f| self.ts_callee(f)),
                _ => None,
            };
            if let Some(callee) = callee
                && !matches!(callee.as_str(), "require" | "import" | "super")
            {
                self.call(caller, callee, node);
            }
            let mut cursor = node.walk();
            let children: Vec<Node> = node.named_children(&mut cursor).collect();
            stack.extend(children.into_iter().rev());
        }
    }

    fn ts_callee(&self, function: Node) -> Option<String> {
        match function.kind() {
            "identifier" => Some(self.text(function)),
            "member_expression" => {
                let property = self.field_text(function, "property")?;
                // `Type.method()` keeps the type; calls on values keep the
                // method name only.
                match function.child_by_field_name("object") {
                    Some(object)
                        if object.kind() == "identifier"
                            && self
                                .text(object)
                                .starts_with(|c: char| c.is_ascii_uppercase()) =>
                    {
                        Some(format!("{}.{property}", self.text(object)))
                    }
                    _ => Some(property),
                }
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::scan_project_tree;
    use std::fs;

    fn build(files: &[(&str, &str)]) -> (tempfile::TempDir, CodeIndex, SyntaxCache) {
        let dir = tempfile::tempdir().unwrap();
        for (path, text) in files {
            let path = dir.path().join(path);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, text).unwrap();
        }
        let pacer = Pacer::unpaced();
        let scan = scan_project_tree(dir.path(), &pacer).unwrap();
        let mut index = CodeIndex::default();
        let mut cache = SyntaxCache::default();
        index_syntax_files(&mut index, &scan, &mut cache, &pacer, &mut 0).unwrap();
        (dir, index, cache)
    }

    fn class<'a>(index: &'a CodeIndex, qualified: &str) -> &'a ClassSymbol {
        index
            .classes
            .iter()
            .find(|c| c.qualified_name == qualified)
            .unwrap_or_else(|| panic!("no class {qualified}: {:#?}", index.classes))
    }

    fn function<'a>(index: &'a CodeIndex, qualified: &str) -> &'a FunctionSymbol {
        index
            .functions
            .iter()
            .find(|f| f.qualified_name == qualified)
            .unwrap_or_else(|| panic!("no function {qualified}: {:#?}", index.functions))
    }

    fn callees(index: &CodeIndex, caller: &str) -> Vec<String> {
        index
            .call_graph
            .iter()
            .filter(|e| e.caller == caller)
            .map(|e| e.callee.clone())
            .collect()
    }

    #[test]
    fn rust_types_functions_methods_and_calls() {
        let (_dir, index, _) = build(&[
            (
                "crates/my-core/Cargo.toml",
                "[package]\nname = \"my-core\"\n\n[dependencies]\nname = \"x\"\n",
            ),
            (
                "crates/my-core/src/lib.rs",
                "pub mod session;\n\npub fn start() {\n    session::Store::open(\"p\").save();\n}\n",
            ),
            (
                "crates/my-core/src/session/mod.rs",
                r#"/// A store.
pub struct Store<T> { items: Vec<T> }

pub enum Mode { A, B }

pub trait Persist {
    fn save(&self);
    fn flush(&self) { self.save(); }
}

impl<T: Clone> Store<T> {
    pub fn open(path: &str) -> Self {
        let v = Vec::<u8>::new();
        helper(path);
        Self::empty()
    }
    fn empty() -> Self { todo!() }
}

impl<T> Persist for Store<T> {
    fn save(&self) { let f = |x: u8| inner(x); f(1); }
}

fn helper(_: &str) {}
fn inner(_: u8) {}

#[cfg(test)]
mod tests {
    #[test]
    fn works() { super::helper("x"); }
}
"#,
            ),
            (
                "crates/my-core/src/extra.rs",
                "impl super::session::Mode { pub fn name(&self) {} }\n",
            ),
        ]);
        let store = class(&index, "my_core::session::Store");
        assert_eq!(store.kind, "struct");
        assert_eq!(store.line, 2);
        let methods: Vec<&str> = store.methods.iter().map(|m| m.name.as_str()).collect();
        assert_eq!(methods, ["open", "empty", "save"]);
        assert_eq!(class(&index, "my_core::session::Mode").kind, "enum");
        // A method in another file is attached by name.
        assert_eq!(
            class(&index, "my_core::session::Mode").methods[0].name,
            "name"
        );
        let persist = class(&index, "my_core::session::Persist");
        assert_eq!(persist.kind, "trait");
        let trait_methods: Vec<&str> = persist.methods.iter().map(|m| m.name.as_str()).collect();
        assert_eq!(trait_methods, ["save", "flush"]);

        assert_eq!(function(&index, "my_core::start").kind, "function");
        assert_eq!(
            function(&index, "my_core::session::Store::open").kind,
            "method"
        );
        assert_eq!(
            function(&index, "my_core::session::Persist::flush").kind,
            "method"
        );
        assert_eq!(function(&index, "my_core::session::helper").line, 24);
        // `impl super::session::Mode` resolves to the type's module.
        function(&index, "my_core::session::Mode::name");
        function(&index, "my_core::session::tests::works");

        assert_eq!(
            callees(&index, "my_core::session::Store::open"),
            ["Vec::new", "helper", "Store::empty"]
        );
        assert_eq!(
            callees(&index, "my_core::start"),
            ["save", "session::Store::open"]
        );
        // Closure calls belong to the enclosing method.
        assert_eq!(
            callees(&index, "my_core::session::Store::save"),
            ["inner", "f"]
        );
    }

    #[test]
    fn typescript_and_javascript_symbols_and_calls() {
        let (_dir, index, _) = build(&[
            (
                "web/src/store.ts",
                r#"import { load } from "./io";

export interface Saver {
  save(): void;
  onDone: (ok: boolean) => void;
  name: string;
}

export enum Mode { A, B }
export type Id = string;

export abstract class Base { abstract run(): void; }

export class Store extends Base implements Saver {
  private items: string[] = [];
  onDone = (ok: boolean) => { this.log(ok); };
  constructor() { super(); }
  save(): void { load(); Config.read(); this.items.push("x"); }
  run() { new Worker(); }
  private log(x: unknown) { console.log(x); }
  get size(): number { return 0; }
  set size(n: number) {}
  find(a: string): void;
  find(a: number): void;
  find(a: unknown) {}
}

export function makeStore(): Store { return new Store(); }
export const helper = async (n: number) => { await makeStore().save(); };

namespace Util { export function pad() {} }
"#,
            ),
            (
                "web/src/view.jsx",
                "export default function App() { return <div onClick={() => go()} />; }\nfunction go() { App(); }\n",
            ),
            ("web/dist/bundle.js", "function skipped() {}\n"),
            ("web/src/lib.min.js", "function skipped() {}\n"),
        ]);
        assert_eq!(class(&index, "Saver").kind, "interface");
        let saver: Vec<&str> = class(&index, "Saver")
            .methods
            .iter()
            .map(|m| m.name.as_str())
            .collect();
        assert_eq!(saver, ["save", "onDone"]);
        assert_eq!(class(&index, "Mode").kind, "enum");
        assert_eq!(class(&index, "Id").kind, "type");
        assert_eq!(class(&index, "Base").methods[0].name, "run");
        let store: Vec<&str> = class(&index, "Store")
            .methods
            .iter()
            .map(|m| m.name.as_str())
            .collect();
        assert_eq!(
            store,
            [
                "onDone",
                "constructor",
                "save",
                "run",
                "log",
                "size",
                "find"
            ]
        );
        // The implementation replaces the overload signatures.
        let find = class(&index, "Store")
            .methods
            .iter()
            .find(|m| m.name == "find")
            .unwrap();
        assert_eq!(find.line, 25);
        assert_eq!(function(&index, "Store.save").kind, "method");
        assert_eq!(function(&index, "makeStore").kind, "function");
        assert_eq!(function(&index, "helper").kind, "function");
        function(&index, "Util.pad");
        function(&index, "App");
        assert_eq!(
            callees(&index, "Store.save"),
            ["load", "Config.read", "push"]
        );
        assert_eq!(callees(&index, "Store.run"), ["Worker"]);
        assert_eq!(callees(&index, "Store.onDone"), ["log"]);
        assert_eq!(callees(&index, "helper"), ["save", "makeStore"]);
        assert_eq!(callees(&index, "App"), ["go"]);
        assert!(index.functions.iter().all(|f| f.name != "skipped"));
        assert!(
            index
                .skipped_files
                .iter()
                .any(|s| s.file == Path::new("web/src/lib.min.js"))
        );
    }

    #[test]
    fn unchanged_files_are_served_from_the_cache() {
        let (dir, _, mut cache) = build(&[
            ("src/a.rs", "fn a() { b(); }\n"),
            ("src/b.rs", "fn b() {}\n"),
        ]);
        assert_eq!(cache.len(), 2);
        let pacer = Pacer::unpaced();
        fs::remove_file(dir.path().join("src/b.rs")).unwrap();
        fs::write(dir.path().join("src/a.rs"), "fn a2() {}\n").unwrap();
        let scan = scan_project_tree(dir.path(), &pacer).unwrap();
        let mut index = CodeIndex::default();
        index_syntax_files(&mut index, &scan, &mut cache, &pacer, &mut 0).unwrap();
        assert_eq!(cache.len(), 1);
        let names: Vec<&str> = index
            .functions
            .iter()
            .map(|f| f.qualified_name.as_str())
            .collect();
        assert_eq!(names, ["a::a2"]);
    }

    #[test]
    fn module_paths() {
        let dir = tempfile::tempdir().unwrap();
        fs::create_dir_all(dir.path().join("app/src/bin")).unwrap();
        fs::write(
            dir.path().join("app/Cargo.toml"),
            "[package]\nname = 'my-app'\n",
        )
        .unwrap();
        fs::write(
            dir.path().join("Cargo.toml"),
            "[workspace]\nmembers = ['app']\n",
        )
        .unwrap();
        let mut crates = CrateNames::default();
        let path =
            |p: &str, crates: &mut CrateNames| rust_module_path(dir.path(), Path::new(p), crates);
        assert_eq!(path("app/src/main.rs", &mut crates), "my_app");
        assert_eq!(path("app/src/a/b.rs", &mut crates), "my_app::a::b");
        assert_eq!(path("app/tests/it.rs", &mut crates), "my_app::tests::it");
        assert_eq!(
            path("app/src/bin/tool.rs", &mut crates),
            "my_app::bin::tool"
        );
        // The workspace manifest has no package, so root files use their path.
        assert_eq!(path("scripts/x.rs", &mut crates), "scripts::x");
        assert_eq!(strip_generics("Vec::<u8>::new"), "Vec::new");
        assert_eq!(strip_generics("HashMap<K, V>"), "HashMap");
        assert_eq!(resolve_rust_path("c::a::b", "super::x::T"), "c::a::x::T");
        assert_eq!(resolve_rust_path("c::a::b", "crate::T"), "c::T");
        assert_eq!(resolve_rust_path("c::a", "self::T"), "c::a::T");
        assert_eq!(resolve_rust_path("c::a", "other::T"), "other::T");
        assert_eq!(
            package_name("[package]\nnamespace = 1\nname = \"x-y\"\n"),
            Some("x-y".into())
        );
    }
}
