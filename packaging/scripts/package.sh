#!/usr/bin/env bash
# Build qt (GUI) and qt-cli in release mode and package them for the host
# platform. Used by .github/workflows/release.yml and for local builds.
#
# Usage: packaging/scripts/package.sh [--skip-build] [--formats LIST] [--dist DIR]
#
#   --skip-build    reuse target/<triple>/release binaries (or target/release)
#   --formats LIST  comma list overriding the defaults below; "none" packages
#                   only the archive (tar.gz / zip) without cargo-packager
#   --dist DIR      output directory (default: target/dist)
#
# Defaults per host:
#   Linux   qt-<ver>-<triple>.tar.gz + AppImage + .deb   (cargo-packager: appimage,deb)
#   macOS   qt-<ver>-<triple>.app.zip (unsigned .app)   (cargo-packager: app)
#   Windows qt-<ver>-<triple>.zip + .msi                (cargo-packager: wix)
#
# Environment:
#   CARGO_PACKAGER   cargo-packager command (default: "cargo packager")
#   CARGO_TARGET     optional target triple passed to cargo build / packager
#   QT_LLAMA_*       forwarded to the qt-llama-sys build (see README "Build")
#
# The AppImage step downloads linuxdeploy, its AppImage plugin and AppRun from
# GitHub on first use (cached in ~/.cache/.cargo-packager); the WiX step
# downloads the WiX 3 toolset. Everything else works offline.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SKIP_BUILD=0
FORMATS=""
DIST="$ROOT/target/dist"

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1 ;;
        --formats) FORMATS="${2:?--formats needs a value}"; shift ;;
        --formats=*) FORMATS="${1#*=}" ;;
        --dist) DIST="${2:?--dist needs a value}"; shift ;;
        --dist=*) DIST="${1#*=}" ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

cd "$ROOT"
read -r -a PACKAGER <<< "${CARGO_PACKAGER:-cargo packager}"

HOST_TRIPLE="$(rustc -vV | tr -d '\r' | sed -n 's/^host: //p')"
TRIPLE="${CARGO_TARGET:-$HOST_TRIPLE}"
# [workspace.package] version (CR stripped for Windows checkouts).
VERSION="$(tr -d '\r' < Cargo.toml | sed -n 's/^version = "\(.*\)"$/\1/p' | head -n1)"
if [ -z "$VERSION" ]; then
    echo "could not read the workspace version from Cargo.toml" >&2
    exit 1
fi
STEM="qt-$VERSION-$TRIPLE"

case "$(uname -s)" in
    Linux) PLATFORM=linux; EXE="" ; DEFAULT_FORMATS="appimage,deb" ;;
    Darwin) PLATFORM=macos; EXE="" ; DEFAULT_FORMATS="app" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM=windows; EXE=".exe"; DEFAULT_FORMATS="wix" ;;
    *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
esac
FORMATS="${FORMATS:-$DEFAULT_FORMATS}"

CARGO_TARGET_ARGS=()
BIN_DIR="$ROOT/target/release"
if [ -n "${CARGO_TARGET:-}" ]; then
    CARGO_TARGET_ARGS=(--target "$CARGO_TARGET")
    BIN_DIR="$ROOT/target/$CARGO_TARGET/release"
fi

if [ "$SKIP_BUILD" -eq 0 ]; then
    cargo build --release --locked -p qt-app -p qt-cli "${CARGO_TARGET_ARGS[@]}"
fi
for bin in qt qt-cli; do
    if [ ! -f "$BIN_DIR/$bin$EXE" ]; then
        echo "missing $BIN_DIR/$bin$EXE (build with: cargo build --release -p qt-app -p qt-cli)" >&2
        exit 1
    fi
done

rm -rf "$DIST"
mkdir -p "$DIST"
STAGE="$ROOT/target/packaging-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# --- portable archive -------------------------------------------------------
case "$PLATFORM" in
    linux)
        dir="$STAGE/$STEM"
        mkdir -p "$dir/bin" "$dir/share/applications" "$dir/share/icons/hicolor/scalable/apps"
        install -m 0755 "$BIN_DIR/qt" "$BIN_DIR/qt-cli" "$dir/bin/"
        install -m 0644 packaging/linux/qt.desktop "$dir/share/applications/qt.desktop"
        install -m 0644 packaging/icons/qt.svg "$dir/share/icons/hicolor/scalable/apps/qt.svg"
        for png in packaging/icons/png/qt-*.png; do
            size="${png##*/qt-}"; size="${size%.png}"
            mkdir -p "$dir/share/icons/hicolor/${size}x${size}/apps"
            install -m 0644 "$png" "$dir/share/icons/hicolor/${size}x${size}/apps/qt.png"
        done
        install -m 0755 packaging/linux/install.sh "$dir/install.sh"
        install -m 0644 README.md "$dir/README.md"
        tar -C "$STAGE" -czf "$DIST/$STEM.tar.gz" "$STEM"
        ;;
    windows)
        dir="$STAGE/$STEM"
        mkdir -p "$dir"
        cp "$BIN_DIR/qt.exe" "$BIN_DIR/qt-cli.exe" README.md packaging/icons/qt.ico "$dir/"
        if command -v 7z >/dev/null 2>&1; then
            (cd "$STAGE" && 7z a -tzip -bd "$DIST/$STEM.zip" "$STEM" >/dev/null)
        else
            powershell.exe -NoProfile -Command \
                "Compress-Archive -Path '$(cygpath -w "$dir")' -DestinationPath '$(cygpath -w "$DIST/$STEM.zip")'"
        fi
        ;;
    macos) ;; # the .app bundle below is the macOS archive
esac

# --- cargo-packager formats -------------------------------------------------
if [ "$FORMATS" != "none" ]; then
    PACKAGER_OUT="$ROOT/target/packager"
    rm -rf "$PACKAGER_OUT"
    PACKAGER_ARGS=(--release --formats "$FORMATS" --out-dir "$PACKAGER_OUT" --binaries-dir "$BIN_DIR")
    if [ -n "${CARGO_TARGET:-}" ]; then
        PACKAGER_ARGS+=(--target "$CARGO_TARGET")
    fi
    # Run from the crate so cargo-packager reads [package.metadata.packager]
    # and resolves the relative icon/template paths against crates/qt-app.
    (cd crates/qt-app && "${PACKAGER[@]}" "${PACKAGER_ARGS[@]}")

    shopt -s nullglob
    for appimage in "$PACKAGER_OUT"/*.AppImage; do
        cp "$appimage" "$DIST/$STEM.AppImage"
    done
    for deb in "$PACKAGER_OUT"/*.deb; do
        cp "$deb" "$DIST/$STEM.deb"
    done
    for msi in "$PACKAGER_OUT"/*.msi; do
        cp "$msi" "$DIST/$STEM.msi"
    done
    for app in "$PACKAGER_OUT"/*.app; do
        for bin in qt qt-cli; do
            if [ ! -x "$app/Contents/MacOS/$bin" ]; then
                echo "$app is missing Contents/MacOS/$bin" >&2
                exit 1
            fi
        done
        # ditto keeps the bundle's symlinks, permissions and resource forks.
        ditto -c -k --sequesterRsrc --keepParent "$app" "$DIST/$STEM.app.zip"
    done
    shopt -u nullglob
fi

# --- checksums --------------------------------------------------------------
(
    cd "$DIST"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum -- * > "SHA256SUMS-$TRIPLE.txt"
    else
        shasum -a 256 -- * > "SHA256SUMS-$TRIPLE.txt"
    fi
)
rm -rf "$STAGE"

echo "Packages in $DIST:"
ls -l "$DIST"
