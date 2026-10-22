#!/usr/bin/env bash
# Composix uninstall (s331). Takes out exactly what install.sh put in (its
# manifest), from the same place:
#
#   ./uninstall.sh                 from ~/.local
#   ./uninstall.sh --system        from /usr/local (asks for sudo)
#   ./uninstall.sh --prefix DIR    from DIR
#   ./uninstall.sh --purge         also your Composix settings, recents and
#                                  cache -- NOT your documents, your swatch
#                                  library, or the pictures ODT import saved
#                                  (~/.local/share/composix/swatches, imported/)
set -euo pipefail
APP_ID="io.github.scott8420.Composix"

prefix="$HOME/.local"
purge=0
while (( $# )); do
    case "$1" in
        --system) prefix="/usr/local" ;;
        --prefix) shift; [[ $# -gt 0 ]] || { echo "--prefix needs a folder" >&2; exit 1; }; prefix="$1" ;;
        --prefix=*) prefix="${1#--prefix=}" ;;
        --purge) purge=1 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1  (try --help)" >&2; exit 1 ;;
    esac
    shift
done
prefix="${prefix%/}"
manifest="$prefix/share/composix/install-manifest.txt"

# The manifest's list -- only paths inside the prefix are taken (a damaged
# or edited manifest can't send rm anywhere else). Without one: the files
# install.sh writes, by name.
files=()
if [[ -f "$manifest" ]]; then
    while IFS= read -r f; do
        [[ -z "$f" || "$f" == \#* ]] && continue
        case "$f" in "$prefix"/*) files+=("$f") ;; *) echo "skipped (outside $prefix): $f" >&2 ;; esac
    done < "$manifest"
else
    files=(
        "$prefix/bin/composix"
        "$prefix/share/applications/$APP_ID.desktop"
        "$prefix/share/icons/hicolor/scalable/apps/$APP_ID.svg"
        "$prefix/share/icons/hicolor/symbolic/apps/$APP_ID-symbolic.svg"
    )
fi

present=()
for f in "${files[@]}"; do [[ -e "$f" || -L "$f" ]] && present+=("$f"); done

if (( ${#present[@]} )); then
    SUDO=()
    for f in "${present[@]}"; do [[ -w "$(dirname "$f")" ]] || { SUDO=(sudo); break; }; done
    if (( ${#SUDO[@]} )); then echo "Removing from $prefix needs sudo."; fi
    "${SUDO[@]}" rm -f -- "${present[@]}"
    "${SUDO[@]}" rmdir --ignore-fail-on-non-empty "$prefix/share/composix" 2>/dev/null || true
    for f in "${present[@]}"; do echo "removed $f"; done
    icondir="$prefix/share/icons/hicolor"
    if command -v gtk-update-icon-cache >/dev/null && [[ -f "$icondir/index.theme" ]]; then
        "${SUDO[@]}" gtk-update-icon-cache -q -t -f "$icondir" || true
    fi
    if command -v update-desktop-database >/dev/null && [[ -d "$prefix/share/applications" ]]; then
        "${SUDO[@]}" update-desktop-database -q "$prefix/share/applications" || true
    fi
    echo "Composix uninstalled from $prefix"
else
    echo "No Composix install found in $prefix"
fi

if (( purge )); then
    data="${XDG_DATA_HOME:-$HOME/.local/share}/composix"
    cache="${XDG_CACHE_HOME:-$HOME/.cache}/composix"
    for f in "$data/settings.json" "$data/recent.json"; do
        if [[ -e "$f" ]]; then rm -f -- "$f"; echo "removed $f"; fi
    done
    if [[ -d "$cache" ]]; then rm -rf -- "$cache"; echo "removed $cache"; fi
    rmdir --ignore-fail-on-non-empty "$data" 2>/dev/null || true
    if [[ -d "$data" ]]; then echo "kept $data (your swatch library / imported pictures)"; fi
fi
