#!/usr/bin/env bash
# tools/neb3_surface_probe.sh — 原生纹理直入合成器(IOSurface 互操作)探针薄封装
#
# 流程: cargo 构建 neb3-wgpu 后端(release) → cc 编译探针(仅含 neb3.h) →
# NEB3_LIB 指向后端运行。断言面见 neb3_surface_probe.c 头注:
# 双路径逐位一致 / native_info 一致 / generation 递增 / NULL 健壮性。
#
# 门禁口径: macOS + 可用 GPU; 非 macOS/无 cargo → skip(exit 0)。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="neb3_surface_probe.sh"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "$LABEL: skip(非 macOS, IOSurface 互操作仅 Apple)"
    exit 0
fi
command -v cargo >/dev/null 2>&1 || { echo "$LABEL: skip(缺 cargo)"; exit 0; }

echo "$LABEL: cargo build --release(neb3-wgpu 后端)"
cargo build --release --manifest-path backend/neb3-wgpu/Cargo.toml

DYLIB="$(pwd)/backend/neb3-wgpu/target/release/libneb3_wgpu.dylib"
[ -s "$DYLIB" ] || { echo "$LABEL: FAIL 后端 dylib 缺失($DYLIB)"; exit 1; }

D="$(hn_tmpd probe)"
BIN="$D/neb3_surface_probe"
echo "$LABEL: 编译探针"
"${CC:-cc}" -std=c99 -O2 -Wall -I Sources/CHtmlNative/include \
    tools/neb3_surface_probe.c -o "$BIN"

echo "$LABEL: 运行(NEB3_LIB=$DYLIB)"
rc=0
NEB3_LIB="$DYLIB" "$BIN" 2>&1 || rc=$?

if [ "$rc" -ne 0 ]; then
    echo "$LABEL: FAIL 退出码 $rc"
    exit 1
fi
echo "$LABEL: OK"
