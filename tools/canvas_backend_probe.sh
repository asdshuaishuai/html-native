#!/usr/bin/env bash
# Canvas-only regression runner: compile real C engine and C++ adapters separately.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/_engine_sources.sh"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/hn-canvas-backends-XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
hn_sources_soft
DEFS=(-DHN_HAVE_CANVAS_ITY)
BACKENDS=(hn_canvas_ity.cpp)
if [ "$(uname -s)" = Darwin ] && [ "${HN_CANVAS_PROBE_NO_CG:-0}" != 1 ]; then
    DEFS+=(-DHN_HAVE_CANVAS_CG)
    BACKENDS+=(hn_canvas_cg.cpp)
fi
INCS=(-I "$HN_ENG" -I "$HN_ENG/include" -I "$HN_REPO_ROOT/platform" -I "$ROOT/vendor/canvas_ity")
SAN=(-fno-omit-frame-pointer)
if [ "${HN_CANVAS_PROBE_SANITIZE:-0}" = 1 ]; then
    SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
FT_CFLAGS=()
FT_LIBS=()
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists freetype2 2>/dev/null; then
    # pkg-config handles Homebrew and system prefixes without a machine-specific -L.
    FT_CFLAGS=( $(pkg-config --cflags freetype2) )
    FT_LIBS=( $(pkg-config --libs freetype2) )
else
    FT_INC=""
    if FT_INC="$(hn_freetype_inc)"; then
        FT_CFLAGS=(-I "$FT_INC")
    else
        echo "canvas_backend_probe.sh: FAIL 缺 FreeType 头文件(pkg-config freetype2 或 brew install freetype)" >&2
        exit 1
    fi
    FT_LIB_DIR=""
    for d in /opt/homebrew/lib /usr/local/lib /usr/lib; do
        if [ -f "$d/libfreetype.dylib" ] || [ -f "$d/libfreetype.a" ] ||
           [ -f "$d/libfreetype.so" ]; then
            FT_LIB_DIR="$d"
            break
        fi
    done
    if [ -n "$FT_LIB_DIR" ]; then
        FT_LIBS=(-L "$FT_LIB_DIR" -lfreetype)
    else
        echo "canvas_backend_probe.sh: FAIL 缺 FreeType 库(pkg-config freetype2 或安装 freetype)" >&2
        exit 1
    fi
fi
OBJS=()
for SRC in "${HN_SOURCES[@]}" "$ROOT/tools/canvas_backend_probe.c"; do
    OBJ="$OUT/$(basename "$SRC").o"
    "${CC:-cc}" -std=c99 -O1 -g -Wall "${SAN[@]}" "${DEFS[@]}" "${INCS[@]}" \
        "${FT_CFLAGS[@]}" -c "$SRC" -o "$OBJ"
    OBJS+=("$OBJ")
done
for SRC in "${BACKENDS[@]}"; do
    OBJ="$OUT/$SRC.o"
    "${CXX:-c++}" -std=c++11 -O1 -g -Wall "${SAN[@]}" "${DEFS[@]}" "${INCS[@]}" \
        -c "$HN_ENG/$SRC" -o "$OBJ"
    OBJS+=("$OBJ")
done
"${CXX:-c++}" "${SAN[@]}" "${OBJS[@]}" "${FT_LIBS[@]}" -lm -o "$OUT/probe"
if [ "$#" -gt 0 ]; then
    HN_CANVAS_BACKEND="$1" "$OUT/probe" "$1"
else
    env -u HN_CANVAS_BACKEND "$OUT/probe"
    for BACKEND in ledger invalid ity cg system; do
        HN_CANVAS_BACKEND="$BACKEND" "$OUT/probe" "$BACKEND"
    done
fi
