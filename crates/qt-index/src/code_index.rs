//! Heuristic, line-based C/C++ code index: classes, methods, functions and
//! an approximate call graph.
//!
//! Each line has comments and string/char literals blanked out. A line that
//! contains `class`/`struct` followed by a name opens a class scope at the next
//! brace depth; a line where an identifier is followed by `( ... )` and then
//! `{` (optionally after `const`/`noexcept`/`override`/`final`) is a function
//! definition (a method when inside a class scope). Inside a function body,
//! every `identifier (` that is not a control keyword or the function itself
//! is recorded as a call edge.

use std::path::{Path, PathBuf};

use serde_json::{Value, json};

use crate::cancel::{Cancelled, Pacer};
use crate::scan::ProjectScanResult;
use crate::util::generic;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MethodSymbol {
    pub name: String,
    pub qualified_name: String,
    pub file: PathBuf,
    pub line: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClassSymbol {
    pub name: String,
    pub qualified_name: String,
    pub file: PathBuf,
    pub line: usize,
    pub methods: Vec<MethodSymbol>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FunctionSymbol {
    pub name: String,
    pub qualified_name: String,
    pub file: PathBuf,
    pub line: usize,
    /// `function` or `method`.
    pub kind: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CallGraphEdge {
    pub caller: String,
    pub callee: String,
    pub file: PathBuf,
    pub line: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SkippedCodeIndexFile {
    pub file: PathBuf,
    pub reason: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CodeIndex {
    pub classes: Vec<ClassSymbol>,
    pub functions: Vec<FunctionSymbol>,
    pub call_graph: Vec<CallGraphEdge>,
    pub skipped_files: Vec<SkippedCodeIndexFile>,
}

/// Why a C/C++ file is excluded from the code index and tree-sitter metadata.
pub fn dependency_or_generated_reason(relative: &Path) -> Option<&'static str> {
    let parts: Vec<String> = relative
        .components()
        .map(|c| c.as_os_str().to_string_lossy().into_owned())
        .filter(|p| !p.is_empty() && p != ".")
        .collect();
    let first = parts.first()?.to_ascii_lowercase();
    if matches!(
        first.as_str(),
        ".pio" | ".index" | "node_modules" | "build" | "cmake-build-debug" | "cmake-build-release"
    ) {
        return Some("build, cache, or dependency directory");
    }
    if matches!(first.as_str(), "vendor" | "third_party" | "external") {
        return Some("vendored dependency directory");
    }
    if first == "lib" && parts.len() > 1 {
        let second = parts[1].to_ascii_lowercase();
        if second.starts_with("boost_") || second == "tree-sitter" || second == "tree-sitter-cpp" {
            return Some("vendored dependency directory");
        }
    }
    let filename = relative
        .file_name()
        .map(|n| n.to_string_lossy().to_ascii_lowercase())
        .unwrap_or_default();
    if filename.starts_with("moc_")
        || filename.starts_with("qrc_")
        || filename.starts_with("ui_")
        || (filename.len() > 6 && filename.ends_with(".pb.cc"))
    {
        return Some("generated source file");
    }
    None
}

/// Blank out comments and string/char literals, tracking block comments
/// across lines.
fn strip_comments_and_strings(line: &[u8], block_comment: &mut bool) -> Vec<u8> {
    let mut out = Vec::with_capacity(line.len());
    let mut string_literal = false;
    let mut char_literal = false;
    let mut escaped = false;
    let mut index = 0;
    while index < line.len() {
        let ch = line[index];
        let next = line.get(index + 1).copied().unwrap_or(0);
        if *block_comment {
            if ch == b'*' && next == b'/' {
                *block_comment = false;
                index += 1;
            }
            out.push(b' ');
            index += 1;
            continue;
        }
        if string_literal || char_literal {
            out.push(b' ');
            if escaped {
                escaped = false;
            } else if ch == b'\\' {
                escaped = true;
            } else if string_literal && ch == b'"' {
                string_literal = false;
            } else if char_literal && ch == b'\'' {
                char_literal = false;
            }
            index += 1;
            continue;
        }
        if ch == b'/' && next == b'*' {
            *block_comment = true;
            out.push(b' ');
            index += 2;
            continue;
        }
        if ch == b'/' && next == b'/' {
            break;
        }
        if ch == b'"' {
            string_literal = true;
            out.push(b' ');
            index += 1;
            continue;
        }
        if ch == b'\'' {
            char_literal = true;
            out.push(b' ');
            index += 1;
            continue;
        }
        out.push(ch);
        index += 1;
    }
    out
}

fn brace_delta(line: &[u8]) -> i64 {
    line.iter().fold(0, |delta, ch| match ch {
        b'{' => delta + 1,
        b'}' => delta - 1,
        _ => delta,
    })
}

fn looks_like_control_keyword(name: &[u8]) -> bool {
    matches!(
        name,
        b"if"
            | b"for"
            | b"while"
            | b"switch"
            | b"catch"
            | b"return"
            | b"sizeof"
            | b"alignof"
            | b"static_cast"
            | b"reinterpret_cast"
            | b"const_cast"
            | b"dynamic_cast"
            | b"decltype"
    )
}

fn is_identifier_start(ch: u8) -> bool {
    ch.is_ascii_alphabetic() || ch == b'_' || ch == b'~'
}

fn is_identifier_continue(ch: u8) -> bool {
    ch.is_ascii_alphanumeric() || ch == b'_' || ch == b':' || ch == b'~'
}

fn unqualified_name(name: &[u8]) -> &[u8] {
    match name.windows(2).rposition(|w| w == b"::") {
        Some(sep) => &name[sep + 2..],
        None => name,
    }
}

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

fn find_class_name(line: &[u8]) -> Option<String> {
    let mut index = 0;
    while index < line.len() {
        if !is_identifier_start(line[index]) {
            index += 1;
            continue;
        }
        let start = index;
        while index < line.len() && is_identifier_continue(line[index]) {
            index += 1;
        }
        let word = &line[start..index];
        if word != b"class" && word != b"struct" {
            continue;
        }
        while index < line.len() && line[index].is_ascii_whitespace() {
            index += 1;
        }
        if index >= line.len() || !is_identifier_start(line[index]) {
            return None;
        }
        let name_start = index;
        while index < line.len() && (line[index].is_ascii_alphanumeric() || line[index] == b'_') {
            index += 1;
        }
        let name = &line[name_start..index];
        return if name.is_empty() {
            None
        } else {
            Some(text(name))
        };
    }
    None
}

fn has_function_body_after_close_paren(line: &[u8], close: usize) -> bool {
    let mut index = close + 1;
    while index < line.len() {
        while index < line.len() && line[index].is_ascii_whitespace() {
            index += 1;
        }
        if index >= line.len() {
            return false;
        }
        match line[index] {
            b'{' => return true,
            b';' | b'=' => return false,
            ch if !is_identifier_start(ch) => return false,
            _ => {}
        }
        let start = index;
        while index < line.len() && is_identifier_continue(line[index]) {
            index += 1;
        }
        let qualifier = &line[start..index];
        if !matches!(qualifier, b"const" | b"noexcept" | b"override" | b"final") {
            return false;
        }
    }
    false
}

fn find_function_name(line: &[u8]) -> Option<String> {
    let mut search = 0;
    while let Some(offset) = line[search..].iter().position(|c| *c == b'(') {
        let open = search + offset;
        search = open + 1;
        let Some(close) = line[open + 1..]
            .iter()
            .position(|c| *c == b')')
            .map(|i| i + open + 1)
        else {
            continue;
        };
        if !has_function_body_after_close_paren(line, close) {
            continue;
        }
        let mut end = open;
        while end > 0 && line[end - 1].is_ascii_whitespace() {
            end -= 1;
        }
        let mut start = end;
        while start > 0 && is_identifier_continue(line[start - 1]) {
            start -= 1;
        }
        if start == end {
            continue;
        }
        let name = unqualified_name(&line[start..end]);
        if !name.is_empty() && !looks_like_control_keyword(name) {
            return Some(text(name));
        }
    }
    None
}

fn append_calls(
    line: &[u8],
    current_function: &str,
    file: &Path,
    line_number: usize,
    call_graph: &mut Vec<CallGraphEdge>,
) {
    let mut index = 0;
    while index < line.len() {
        if !is_identifier_start(line[index]) {
            index += 1;
            continue;
        }
        let start = index;
        while index < line.len() && is_identifier_continue(line[index]) {
            index += 1;
        }
        let callee = &line[start..index];
        let short = unqualified_name(callee);
        while index < line.len() && line[index].is_ascii_whitespace() {
            index += 1;
        }
        if index < line.len()
            && line[index] == b'('
            && !looks_like_control_keyword(short)
            && short != current_function.as_bytes()
            && callee != current_function.as_bytes()
        {
            call_graph.push(CallGraphEdge {
                caller: current_function.to_string(),
                callee: text(callee),
                file: file.to_path_buf(),
                line: line_number,
            });
        }
    }
}

fn qualified(scopes: &[String], name: &str) -> String {
    let mut parts: Vec<&str> = scopes.iter().map(String::as_str).collect();
    parts.push(name);
    parts.join("::")
}

/// Index one file's text into `index`.
fn index_file(
    index: &mut CodeIndex,
    relative: &Path,
    content: &[u8],
    pacer: &Pacer,
    operations: &mut usize,
) -> Result<(), Cancelled> {
    let mut block_comment = false;
    let mut brace_depth: i64 = 0;
    let mut class_scopes: Vec<i64> = Vec::new();
    let mut scope_names: Vec<String> = Vec::new();
    let mut current_function = String::new();
    let mut function_depth: i64 = -1;

    let body = content.strip_suffix(b"\n").unwrap_or(content);
    if content.is_empty() {
        return Ok(());
    }
    for (line_index, raw) in body.split(|b| *b == b'\n').enumerate() {
        *operations += 1;
        pacer.checkpoint(*operations, 128, 10)?;
        let line_number = line_index + 1;
        let line = strip_comments_and_strings(raw, &mut block_comment);

        if let Some(name) = find_class_name(&line) {
            let qualified_name = qualified(&scope_names, &name);
            scope_names.push(name.clone());
            class_scopes.push(brace_depth + 1);
            index.classes.push(ClassSymbol {
                name,
                qualified_name,
                file: relative.to_path_buf(),
                line: line_number,
                methods: Vec::new(),
            });
        }

        if let Some(name) = find_function_name(&line) {
            let in_class = !scope_names.is_empty();
            let qualified_name = qualified(&scope_names, &name);
            index.functions.push(FunctionSymbol {
                name: name.clone(),
                qualified_name: qualified_name.clone(),
                file: relative.to_path_buf(),
                line: line_number,
                kind: if in_class { "method" } else { "function" }.into(),
            });
            if in_class && let Some(class) = index.classes.last_mut() {
                class.methods.push(MethodSymbol {
                    name,
                    qualified_name: qualified_name.clone(),
                    file: relative.to_path_buf(),
                    line: line_number,
                });
            }
            current_function = qualified_name;
            function_depth = brace_depth + 1;
        }

        if !current_function.is_empty() {
            append_calls(
                &line,
                &current_function,
                relative,
                line_number,
                &mut index.call_graph,
            );
        }

        brace_depth += brace_delta(&line);
        while class_scopes
            .last()
            .is_some_and(|depth| brace_depth < *depth)
        {
            class_scopes.pop();
            scope_names.pop();
        }
        if !current_function.is_empty() && brace_depth < function_depth {
            current_function.clear();
            function_depth = -1;
        }
    }
    Ok(())
}

/// Build the code index for all C/C++ sources and headers in the scan.
pub fn build_code_index(scan: &ProjectScanResult, pacer: &Pacer) -> Result<CodeIndex, Cancelled> {
    let mut index = CodeIndex::default();
    let mut operations = 0usize;
    for file in &scan.files {
        operations += 1;
        pacer.checkpoint(operations, 128, 10)?;
        if !file.kind.is_cpp_candidate() {
            continue;
        }
        if let Some(reason) = dependency_or_generated_reason(&file.path) {
            index.skipped_files.push(SkippedCodeIndexFile {
                file: file.path.clone(),
                reason: reason.into(),
            });
            continue;
        }
        let Ok(content) = std::fs::read(scan.root.join(&file.path)) else {
            continue;
        };
        index_file(&mut index, &file.path, &content, pacer, &mut operations)?;
    }
    Ok(index)
}

impl CodeIndex {
    pub fn summary(&self) -> Value {
        json!({
            "classes": self.classes.len(),
            "functions": self.functions.len(),
            "call_graph_edges": self.call_graph.len(),
            "skipped_files": self.skipped_files.len(),
        })
    }
}

pub fn method_json(method: &MethodSymbol) -> Value {
    json!({
        "name": method.name,
        "qualified_name": method.qualified_name,
        "file": generic(&method.file),
        "line": method.line,
    })
}

pub fn class_json(class: &ClassSymbol) -> Value {
    json!({
        "name": class.name,
        "qualified_name": class.qualified_name,
        "file": generic(&class.file),
        "line": class.line,
        "methods": class.methods.iter().map(method_json).collect::<Vec<_>>(),
    })
}

pub fn function_json(function: &FunctionSymbol) -> Value {
    json!({
        "name": function.name,
        "qualified_name": function.qualified_name,
        "file": generic(&function.file),
        "line": function.line,
        "kind": function.kind,
    })
}

pub fn call_json(edge: &CallGraphEdge) -> Value {
    json!({
        "caller": edge.caller,
        "callee": edge.callee,
        "file": generic(&edge.file),
        "line": edge.line,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn index_text(text: &str) -> CodeIndex {
        let mut index = CodeIndex::default();
        let mut ops = 0;
        index_file(
            &mut index,
            Path::new("src/a.cpp"),
            text.as_bytes(),
            &Pacer::unpaced(),
            &mut ops,
        )
        .unwrap();
        index
    }

    #[test]
    fn classes_methods_functions_and_calls() {
        let index = index_text(
            "namespace n {\nclass Widget {\npublic:\n  int size() const { return compute(1); }\n  void draw() {\n    // paint(\"x\");\n    paint(\"a(b)\");\n    if (x) { helper::run(); }\n  }\n};\n}\nint main() {\n  Widget w;\n  w.draw();\n  return 0;\n}\nstruct Fwd;\nvoid decl();\n",
        );
        let classes: Vec<_> = index
            .classes
            .iter()
            .map(|c| c.qualified_name.as_str())
            .collect();
        assert_eq!(classes, vec!["Widget", "Fwd"]);
        assert_eq!(index.classes[0].methods.len(), 2);
        assert_eq!(index.classes[0].line, 2);
        let functions: Vec<_> = index
            .functions
            .iter()
            .map(|f| (f.qualified_name.as_str(), f.kind.as_str(), f.line))
            .collect();
        assert_eq!(
            functions,
            vec![
                ("Widget::size", "method", 4),
                ("Widget::draw", "method", 5),
                ("main", "function", 12)
            ]
        );
        let calls: Vec<_> = index
            .call_graph
            .iter()
            .map(|e| (e.caller.as_str(), e.callee.as_str(), e.line))
            .collect();
        // The definition line's own name is recorded as a call when the
        // function is qualified (archived behaviour).
        assert_eq!(
            calls,
            vec![
                ("Widget::size", "size", 4),
                ("Widget::size", "compute", 4),
                ("Widget::draw", "draw", 5),
                ("Widget::draw", "paint", 7),
                ("Widget::draw", "helper::run", 8),
                ("main", "draw", 14)
            ]
        );
    }

    #[test]
    fn block_comments_span_lines() {
        let index = index_text("/* void hidden() {\n} */\nvoid shown() {\n}\n");
        assert_eq!(index.functions.len(), 1);
        assert_eq!(index.functions[0].name, "shown");
    }

    #[test]
    fn skip_reasons() {
        assert_eq!(
            dependency_or_generated_reason(Path::new("build/x.cpp")),
            Some("build, cache, or dependency directory")
        );
        assert_eq!(
            dependency_or_generated_reason(Path::new("third_party/a.h")),
            Some("vendored dependency directory")
        );
        assert_eq!(
            dependency_or_generated_reason(Path::new("lib/boost_1_86_0/a.h")),
            Some("vendored dependency directory")
        );
        assert_eq!(
            dependency_or_generated_reason(Path::new("src/moc_widget.cpp")),
            Some("generated source file")
        );
        assert_eq!(
            dependency_or_generated_reason(Path::new("x/msg.pb.cc")),
            Some("generated source file")
        );
        assert_eq!(
            dependency_or_generated_reason(Path::new("src/main.cpp")),
            None
        );
    }
}
