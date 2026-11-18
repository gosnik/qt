# Tree-sitter Unicode Runtime Provenance

The files in `unicode/` are an unmodified copy of
`tree-sitter/tree-sitter/lib/src/unicode/` at upstream commit
`0e76a0bd53144326916a858d156bff0cf95cd91a`.

The adjacent `unicode.h` has SHA-256
`6c974327d7e2a1e9c7f660d56ee3df6d6fbe9944aa0b364567531271783dec9d`
and is byte-identical to `lib/src/unicode.h` at that commit. It includes
`unicode/utf8.h` and `unicode/utf16.h`, so the complete companion directory is
part of the Tree-sitter runtime source rather than an optional system ICU
dependency.

Tree-sitter's subset records ICU commit
`552b01f61127d30d6589aa4bf99468224979b661` in `unicode/ICU_SHA`. Its license
is preserved verbatim in `unicode/LICENSE`; `unicode/README.md` documents the
selected headers and intentionally empty compatibility headers.

When updating this runtime, update `unicode.h` and the complete `unicode/`
directory together from one reviewed Tree-sitter commit. Do not replace this
subset with a host ICU dependency: the embedded Tree-sitter C runtime is
intended to remain self-contained.
