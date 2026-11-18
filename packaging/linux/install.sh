#!/bin/sh
# Install an unpacked qt release tarball into PREFIX (default: ~/.local).
#
#   ./install.sh                 # ~/.local/bin/qt, ~/.local/bin/qt-cli, desktop entry, icons
#   PREFIX=/usr/local sudo -E ./install.sh
#   ./install.sh --uninstall
#
# qt and qt-cli are installed side by side: subagent workers, the memory
# pre-handler and Dreaming start the qt-cli next to the running qt binary.
set -eu

PREFIX="${PREFIX:-$HOME/.local}"
HERE="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"

icon_files() {
    (cd "$HERE/share/icons" && find hicolor -type f -name 'qt.*')
}

refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$PREFIX/bin/qt" "$PREFIX/bin/qt-cli" "$PREFIX/share/applications/qt.desktop"
    for icon in $(icon_files); do
        rm -f "$PREFIX/share/icons/$icon"
    done
    refresh_caches
    echo "Removed qt from $PREFIX"
    exit 0
fi

if [ $# -gt 0 ]; then
    echo "usage: $0 [--uninstall]   (set PREFIX to change the target, default ~/.local)" >&2
    exit 2
fi

install -d "$PREFIX/bin" "$PREFIX/share/applications"
install -m 0755 "$HERE/bin/qt" "$PREFIX/bin/qt"
install -m 0755 "$HERE/bin/qt-cli" "$PREFIX/bin/qt-cli"
install -m 0644 "$HERE/share/applications/qt.desktop" "$PREFIX/share/applications/qt.desktop"
for icon in $(icon_files); do
    install -d "$PREFIX/share/icons/$(dirname "$icon")"
    install -m 0644 "$HERE/share/icons/$icon" "$PREFIX/share/icons/$icon"
done
refresh_caches

echo "Installed qt and qt-cli into $PREFIX/bin"
case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) echo "Note: $PREFIX/bin is not on PATH" ;;
esac
