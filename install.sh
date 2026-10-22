#!/usr/bin/env bash
# Composix install (s331). Puts the built app where the desktop finds it:
#
#   ./install.sh                 for you only: ~/.local (no sudo)
#   ./install.sh --system        for everyone: /usr/local (asks for sudo)
#   ./install.sh --prefix DIR    anywhere else (sudo if DIR isn't yours)
#   ./install.sh --build         build first (./build.sh), then install
#
# What goes in (and nothing else -- the icons and stylesheet ride inside
# the binary):
#   PREFIX/bin/composix
#   PREFIX/share/applications/io.github.scott8420.Composix.desktop
#   PREFIX/share/icons/hicolor/scalable/apps/io.github.scott8420.Composix.svg
#   PREFIX/share/icons/hicolor/symbolic/apps/io.github.scott8420.Composix-symbolic.svg
#   PREFIX/share/composix/install-manifest.txt   (what uninstall.sh takes out)
#
# Installing again over an install replaces it. Your documents, settings,
# recents and swatch library are never touched.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
SRC="$(pwd)"
APP_ID="io.github.scott8420.Composix"

prefix="$HOME/.local"
build=0
while (( $# )); do
    case "$1" in
        --system) prefix="/usr/local" ;;
        --prefix) shift; [[ $# -gt 0 ]] || { echo "--prefix needs a folder" >&2; exit 1; }; prefix="$1" ;;
        --prefix=*) prefix="${1#--prefix=}" ;;
        --build) build=1 ;;
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1  (try --help)" >&2; exit 1 ;;
    esac
    shift
done
prefix="${prefix%/}"

if (( build )); then ./build.sh; fi
BIN="$SRC/build/composix"
if [[ ! -x "$BIN" ]]; then
    echo "No build/composix here -- run ./build.sh first (or ./install.sh --build)." >&2
    exit 1
fi

# sudo only when the prefix isn't writable by you
SUDO=()
probe="$prefix"
while [[ ! -e "$probe" ]]; do probe="$(dirname "$probe")"; done
if [[ ! -w "$probe" ]]; then
    command -v sudo >/dev/null || { echo "$prefix isn't writable and there's no sudo." >&2; exit 1; }
    SUDO=(sudo)
    echo "Installing to $prefix needs sudo."
fi

bindir="$prefix/bin"
appdir="$prefix/share/applications"
icondir="$prefix/share/icons/hicolor"
datadir="$prefix/share/composix"
manifest="$datadir/install-manifest.txt"

files=(
    "$bindir/composix"
    "$appdir/$APP_ID.desktop"
    "$icondir/scalable/apps/$APP_ID.svg"
    "$icondir/symbolic/apps/$APP_ID-symbolic.svg"
    "$manifest"
)

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
sed "s|@BINDIR@|$bindir|g" "data/$APP_ID.desktop.in" > "$tmp/$APP_ID.desktop"
printf '%s\n' "# Composix install manifest -- uninstall.sh removes these" "# prefix: $prefix" "${files[@]}" > "$tmp/manifest"

"${SUDO[@]}" install -Dm755 "$BIN"                                              "$bindir/composix"
"${SUDO[@]}" install -Dm644 "$tmp/$APP_ID.desktop"                              "$appdir/$APP_ID.desktop"
"${SUDO[@]}" install -Dm644 "data/$APP_ID.svg"                                  "$icondir/scalable/apps/$APP_ID.svg"
"${SUDO[@]}" install -Dm644 "resources/icons/scalable/apps/composix-logo-symbolic.svg" "$icondir/symbolic/apps/$APP_ID-symbolic.svg"
"${SUDO[@]}" install -Dm644 "$tmp/manifest"                                     "$manifest"

# the desktop's caches, where it has the tools (harmless when it doesn't)
if command -v gtk-update-icon-cache >/dev/null && [[ -f "$icondir/index.theme" ]]; then
    "${SUDO[@]}" gtk-update-icon-cache -q -t -f "$icondir" || true
fi
if command -v update-desktop-database >/dev/null; then
    "${SUDO[@]}" update-desktop-database -q "$appdir" || true
fi

build_tag="$(grep -o 'COMPOSIX_BUILD "[^"]*"' CMakeLists.txt | cut -d'"' -f2 || true)"
echo "Composix ${build_tag:-} installed to $prefix"
echo "  run:  $bindir/composix   (or Composix in your applications)"
case ":$PATH:" in
    *":$bindir:"*) ;;
    *) echo "  note: $bindir isn't on your PATH -- the launcher works; to type 'composix', add it to PATH." ;;
esac
echo "  remove with: ./uninstall.sh$( [[ "$prefix" == "$HOME/.local" ]] || echo " --prefix $prefix" )"
