#!/usr/bin/env bash
# three_probe.sh — P1-3 texture-gate lifetime + P1-8 source/build smoke.
# Usage: bash tools/three_probe.sh [normal|asan|all]
set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

MODE="${1:-all}"
case "$MODE" in normal|asan|all) ;; *) echo "usage: $0 [normal|asan|all]" >&2; exit 2 ;; esac

hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/three_probe.c" "${HN_SOURCES[@]}")
hn_sources_check "${HN_SOURCES[@]}"

FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
elif [ -d /usr/include/freetype2 ]; then
  FT_INC="-I/usr/include/freetype2"
  FT_LIB=(-lfreetype)
fi

compile_and_run() {
  local flavor="$1"
  local out="${TMPDIR:-/tmp}/three_probe_${flavor}_$$"
  local cflags=(-std=c99 -O1 -g -fno-omit-frame-pointer -ffp-contract=off)
  local san=()
  if [ "$flavor" = asan ]; then
    san=(-fsanitize=address,undefined)
  fi
  local ft=()
  if [ -n "$FT_INC" ]; then ft=("$FT_INC" "${FT_LIB[@]}"); fi
  echo "three_probe.sh: compile $flavor"
  "$HN_CC" "${cflags[@]}" ${san[@]+"${san[@]}"} -I "$HN_ENG/include" -I "$HN_ENG" \
      "${HN_SOURCES[@]}" ${ft[@]+"${ft[@]}"} -lm -o "$out"
  echo "three_probe.sh: run $flavor"
  # macOS ASan 不支持 detect_leaks(abort trap 6), 按平台取值。
  local asan_opts="halt_on_error=1:abort_on_error=1"
  [ "$(uname -s)" = Darwin ] || asan_opts="detect_leaks=1:$asan_opts"
  ASAN_OPTIONS="$asan_opts" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" "$out"
  rm -f "$out"
}

case "$MODE" in
  normal) compile_and_run normal ;;
  asan) compile_and_run asan ;;
  all) compile_and_run normal; compile_and_run asan ;;
esac
printf '%s\n' 'three_probe.sh: PASS'
