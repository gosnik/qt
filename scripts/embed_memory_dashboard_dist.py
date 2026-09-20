"""Generate embedded qt memory dashboard web assets for the native executable."""

from __future__ import annotations

import gzip
import hashlib
import os
import sys
import tempfile
from pathlib import Path

Import("env")  # type: ignore[name-defined]

SCRIPT_DIR = Path(env.subst("$PROJECT_DIR")).resolve() / "scripts"  # type: ignore[name-defined]
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

def _content_type(path: str) -> str:
    suffix = Path(path).suffix.lower()
    return {
        ".html": "text/html; charset=utf-8",
        ".js": "application/javascript; charset=utf-8",
        ".wasm": "application/wasm",
        ".data": "application/octet-stream",
        ".css": "text/css; charset=utf-8",
        ".json": "application/json; charset=utf-8",
        ".txt": "text/plain; charset=utf-8",
        ".svg": "image/svg+xml",
        ".png": "image/png",
        ".jpg": "image/jpeg",
        ".jpeg": "image/jpeg",
        ".ico": "image/x-icon",
        ".ttf": "font/ttf",
        ".otf": "font/otf",
        ".woff": "font/woff",
        ".woff2": "font/woff2",
    }.get(suffix, "application/octet-stream")


def _symbol_for(relative_path: str) -> str:
    digest = hashlib.sha1(relative_path.encode("utf-8")).hexdigest()[:12]
    stem = "".join(ch if ch.isalnum() else "_" for ch in relative_path)
    return f"asset_{stem}_{digest}"


def _asm_string(path: Path) -> str:
    return str(path).replace("\\", "\\\\").replace('"', '\\"')


def _write_if_changed(path: Path, content: str) -> None:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _gzip_asset(source: Path, destination: Path) -> None:
    compressed = gzip.compress(source.read_bytes(), compresslevel=9, mtime=0)
    if destination.exists() and destination.read_bytes() == compressed:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(compressed)


def _collect_assets(dist_dir: Path, compressed_dir: Path) -> list[tuple[str, Path, str, str, int]]:
    assets: list[tuple[str, Path, str, str, int]] = []
    for path in sorted(dist_dir.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(dist_dir).as_posix()
        route = "/" + relative
        compressed_path = compressed_dir / (relative + ".gz")
        _gzip_asset(path, compressed_path)
        assets.append((route, compressed_path.resolve(), _content_type(relative), _symbol_for(relative), path.stat().st_size))
    return assets


def _dist_digest(dist_dir: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(dist_dir.rglob("*")):
        if not path.is_file():
            continue
        digest.update(path.relative_to(dist_dir).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


project_dir = Path(env.subst("$PROJECT_DIR")).resolve()  # type: ignore[name-defined]
dist_option = os.environ.get("QT_MEMORY_DASHBOARD_DIST") or env.GetProjectOption("custom_memory_dashboard_dist", "dashboard_wasm/web/dist")  # type: ignore[name-defined]
dist_dir = (project_dir / dist_option).resolve()
generated_dir = project_dir / "src/app/interactive/generated"
header_path = generated_dir / "memory_dashboard_embedded_assets.hpp"
source_path = generated_dir / "memory_dashboard_embedded_assets.cpp"

header = """#pragma once

#include <cstddef>

namespace qt::app::generated {

struct EmbeddedAsset {
    const char* path;
    const char* content_type;
    const unsigned char* data;
    std::size_t size;
    std::size_t uncompressed_size;
    bool gzip_encoded;
};

const EmbeddedAsset* find_dashboard_asset(const char* path);
std::size_t dashboard_asset_count();
bool dashboard_assets_available();

}  // namespace qt::app::generated
"""

source_lines = [
    '#include "memory_dashboard_embedded_assets.hpp"',
    "",
    "#include <cstring>",
    "",
    "namespace qt::app::generated {",
]

if dist_dir.exists():
    compressed_dir = (Path(tempfile.gettempdir()) /
                      "qt_memory_dashboard_embedded_assets" /
                      _dist_digest(dist_dir))
    assets = _collect_assets(dist_dir, compressed_dir)
else:
    assets = []

if assets:
    for route, absolute_path, _mime, symbol, _uncompressed_size in assets:
        source_lines.extend(
            [
                '#if defined(__APPLE__)',
                'asm(".section __DATA,__const\\n"',
                '    ".balign 16\\n"',
                f'    ".globl _{symbol}_begin\\n"',
                f'    "_{symbol}_begin:\\n"',
                f'    ".incbin \\"{_asm_string(absolute_path)}\\"\\n"',
                f'    ".globl _{symbol}_end\\n"',
                f'    "_{symbol}_end:\\n"',
                '    ".previous\\n");',
                f'extern const unsigned char {symbol}_begin[] asm("_{symbol}_begin");',
                f'extern const unsigned char {symbol}_end[] asm("_{symbol}_end");',
                "#else",
                'asm(".section .rodata\\n"',
                '    ".balign 16\\n"',
                f'    ".global {symbol}_begin\\n"',
                f'    "{symbol}_begin:\\n"',
                f'    ".incbin \\"{_asm_string(absolute_path)}\\"\\n"',
                f'    ".global {symbol}_end\\n"',
                f'    "{symbol}_end:\\n"',
                '    ".previous\\n");',
                f'extern const unsigned char {symbol}_begin[] asm("{symbol}_begin");',
                f'extern const unsigned char {symbol}_end[] asm("{symbol}_end");',
                "#endif",
                "",
            ]
        )

    source_lines.extend(
        [
            "namespace {",
            "const EmbeddedAsset kAssets[] = {",
        ]
    )
    for route, _absolute_path, mime, symbol, uncompressed_size in assets:
        source_lines.append(
            f'    {{"{route}", "{mime}", {symbol}_begin, static_cast<std::size_t>({symbol}_end - {symbol}_begin), {uncompressed_size}U, true}},'
        )
    source_lines.extend(
        [
            "};",
            "}  // namespace",
            "",
            "const EmbeddedAsset* find_dashboard_asset(const char* path) {",
            "    if (path == nullptr) {",
            "        return nullptr;",
            "    }",
            "    for (const auto& asset : kAssets) {",
            "        if (std::strcmp(asset.path, path) == 0) {",
            "            return &asset;",
            "        }",
            "    }",
            "    return nullptr;",
            "}",
            "",
            "std::size_t dashboard_asset_count() {",
            "    return sizeof(kAssets) / sizeof(kAssets[0]);",
            "}",
            "",
            "bool dashboard_assets_available() {",
            "    return dashboard_asset_count() != 0;",
            "}",
        ]
    )
else:
    source_lines.extend(
        [
            "const EmbeddedAsset* find_dashboard_asset(const char* path) {",
            "    (void)path;",
            "    return nullptr;",
            "}",
            "",
            "std::size_t dashboard_asset_count() {",
            "    return 0;",
            "}",
            "",
            "bool dashboard_assets_available() {",
            "    return false;",
            "}",
        ]
    )

source_lines.extend(
    [
        "",
        "}  // namespace qt::app::generated",
        "",
    ]
)

_write_if_changed(header_path, header)
_write_if_changed(source_path, "\n".join(source_lines))
