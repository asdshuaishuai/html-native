#!/usr/bin/env bash
# build-multiplatform.sh — 三平台引擎核心二进制(0.1)
#
# 为什么可行: 引擎是纯 ISO C99, 零平台 API。只要 C 编译器能产出目标平台
# 的 libc, 就能构建。zig cc 自带三平台 libc(无需各平台 SDK/sysroot),
# 因此 macOS 上一条命令即可产出三平台产物 —— 这本身就是"引擎无关性"的证明。
#
# 产物: dist/hncore-<os>-<arch>[.exe]
#   macOS   arm64 / x86_64
#   Linux   x86_64 / aarch64 (musl 静态: 无 glibc 版本依赖)
#   Windows x86_64 (mingw, 静态链接)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist"
ZIG="${ZIG:-zig}"
VERSION="${VERSION:-0.1.0}"

cd "$ROOT"
mkdir -p "$OUT"

SRC=(tools/hncore.c Sources/CHtmlNative/hn_arena.c Sources/CHtmlNative/hn_html.c \
     Sources/CHtmlNative/hn_css.c Sources/CHtmlNative/hn_style.c \
     Sources/CHtmlNative/hn_layout.c Sources/CHtmlNative/hn_paint.c \
     Sources/CHtmlNative/hn_context.c Sources/CHtmlNative/hn_theme.c)

# -ffp-contract=off: 禁用乘加融合(FMA)。
# arm64 有 FMA 而 x86_64 基线没有, 默认融合会导致同一文档在不同架构上
# 算出不同的 px 值(实测差 0.1px)。布局引擎要求跨平台确定性, 因此关闭。
CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off -I Sources/CHtmlNative/include
        -DHN_VERSION="\"$VERSION\"")

# tag: <os>-<arch>
build() {
    local tag="$1" target="$2" ext="${3:-}"
    local out="$OUT/hncore-$tag$ext"
    printf '  %-22s ' "$tag"
    if "$ZIG" cc -target "$target" "${CFLAGS[@]}" "${SRC[@]}" -o "$out" 2>/tmp/hn_build_err.txt; then
        local size
        size=$(wc -c < "$out" | tr -d ' ')
        printf '✓  %s (%s KB)\n' "$(basename "$out")" "$((size / 1024))"
    else
        printf '✘ 失败\n'
        sed 's/^/      /' /tmp/hn_build_err.txt | head -6
        return 1
    fi
}

echo "构建 hncore $VERSION (三平台)"
echo
echo "本机验证(与交叉产物同源, 保证语义一致):"
build "macos-arm64"    "aarch64-macos"
build "macos-x86_64"   "x86_64-macos"
echo
echo "Linux (musl 静态, 无 glibc 依赖):"
build "linux-x86_64"   "x86_64-linux-musl"
build "linux-aarch64"  "aarch64-linux-musl"
echo
echo "Windows (mingw 静态):"
build "windows-x86_64" "x86_64-windows-gnu" ".exe"
echo

# 本机产物自检(其余平台无法在本机执行, 但可验证是可执行格式)
HOST_BIN="$OUT/hncore-macos-arm64"
if [ -x "$HOST_BIN" ]; then
    echo "自检(macOS arm64 实跑):"
    if "$HOST_BIN" verify "$ROOT/examples/dashboard.html" 480 700 | sed 's/^/  /'; then
        echo "  ✓ 布局不变量检查通过"
    else
        echo "  ✘ 自检失败"
        exit 1
    fi
fi

echo
echo "产物清单:"
for f in "$OUT"/hncore-*; do
    [ -f "$f" ] || continue
    printf '  %-34s %8s KB  %s\n' "$(basename "$f")" "$(( $(wc -c < "$f" | tr -d ' ') / 1024 ))" \
        "$(file -b "$f" | cut -c1-56)"
done

# 校验和(发布用)
if command -v shasum >/dev/null 2>&1; then
    (cd "$OUT" && shasum -a 256 hncore-* > SHA256SUMS)
    echo
    echo "已生成 $OUT/SHA256SUMS"
fi
