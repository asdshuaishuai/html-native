#!/usr/bin/env bash
# tools/neb3_fallback_probe.sh — hn_three neb3 永久回退路径的缓存槽泄漏回归
# (ISSUES.md 附: 存量简化 "neb3 永久回退后其余缓存槽泄漏" 的回归锚)。
#
# 手法: 现场 cc 一个全符号齐的 fake neb3 dylib(tools/fake_neb3.c, 对象级
# 存活计数), NEB3_LIB 指向它 → hn_three 走真 backend 路径装配 4 槽;
# 探针经 fake 的控制面逐点打失败(create/scene/render), 用 fake_neb3_live()
# 断言回退后**所有**槽整链释放 —— 泄漏判据精确到对象, 不靠 RSS。
#
# 口径: g_neb3_state=2 是进程级永久态 → 三场景各起一个进程。
# 用法: bash tools/neb3_fallback_probe.sh [normal|asan|all](缺省 all)

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

MODE="${1:-all}"
case "$MODE" in normal|asan|all) ;; *) echo "usage: $0 [normal|asan|all]" >&2; exit 2 ;; esac

hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/neb3_fallback_probe.c" "${HN_SOURCES[@]}")
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
  local out="${TMPDIR:-/tmp}/neb3_fallback_${flavor}_$$"
  local dylib="${TMPDIR:-/tmp}/fake_neb3_${flavor}_$$.dylib"
  local cflags=(-std=c99 -O1 -g -fno-omit-frame-pointer -ffp-contract=off)
  local san=()
  if [ "$flavor" = asan ]; then
    san=(-fsanitize=address,undefined)
  fi
  local ft=()
  if [ -n "$FT_INC" ]; then ft=("$FT_INC" "${FT_LIB[@]}"); fi

  echo "neb3_fallback_probe.sh: compile fake dylib ($flavor)"
  local dshared=(-dynamiclib)
  [ "$(uname -s)" = Darwin ] || dshared=(-shared -fPIC)
  "$HN_CC" -std=c99 -O1 -I "$HN_ENG/include" "${dshared[@]}" \
      "$HN_TOOLS_DIR/fake_neb3.c" -o "$dylib"

  echo "neb3_fallback_probe.sh: compile probe ($flavor)"
  "$HN_CC" "${cflags[@]}" ${san[@]+"${san[@]}"} -I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_REPO_ROOT/platform" \
      "${HN_SOURCES[@]}" ${ft[@]+"${ft[@]}"} -lm -o "$out"

  echo "neb3_fallback_probe.sh: run $flavor"
  local asan_opts="halt_on_error=1:abort_on_error=1"
  [ "$(uname -s)" = Darwin ] || asan_opts="detect_leaks=1:$asan_opts"
  local rc=0
  for scenario in render_fail create_fail scene_fail; do
    echo "  -- 场景 $scenario --"
    FAKE_NEB3_PATH="$dylib" \
    ASAN_OPTIONS="$asan_opts" \
    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" "$out" "$scenario" || rc=1
  done
  rm -f "$out" "$dylib"
  return $rc
}

case "$MODE" in
  normal) compile_and_run normal ;;
  asan) compile_and_run asan ;;
  all) compile_and_run normal; compile_and_run asan ;;
esac
printf '%s\n' 'neb3_fallback_probe.sh: PASS'
