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
     Sources/CHtmlNative/hn_context.c Sources/CHtmlNative/hn_theme.c \
     Sources/CHtmlNative/hn_json.c Sources/CHtmlNative/hn_lottie.c \
     Sources/CHtmlNative/hn_mesh.c Sources/CHtmlNative/hn_png.c \
     Sources/CHtmlNative/hnsoft.c)

# -ffp-contract=off: 禁用乘加融合(FMA)。
# arm64 有 FMA 而 x86_64 基线没有, 默认融合会导致同一文档在不同架构上
# 算出不同的 px 值(实测差 0.1px)。布局引擎要求跨平台确定性, 因此关闭。
# HN_NO_TEXT: 交叉产物不链 FreeType(各平台自行链接或用 HN_NO_TEXT 构建),
# 文本光栅化需要 FreeType —— macOS 用 homebrew, Linux 用系统包, Windows 用 vcpkg。
# include 路径: freetype2 头文件在 homebrew 的子目录里
FT_INC=""
if [ -d /opt/homebrew/include/freetype2 ]; then FT_INC="-I/opt/homebrew/include/freetype2"; fi
CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off -I Sources/CHtmlNative/include
        -I Sources/CHtmlNative $FT_INC -DHN_VERSION="\"$VERSION\"")

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
# 本机架构: FreeType 可用(homebrew arm64), 文本可渲染
# 其余目标: HN_NO_TEXT(无文本, 但矩形/渐变/裁剪/布局全部可用)
HOST_ARCH=$(uname -m)
# build <tag> <target> <ext> <text?> [<额外链接库>] [<源文件...>]
# 源文件缺省 = SRC(引擎核心 CLI)。Windows 运行时目标传入自己的源文件,
# 产物名 <tag><ext>(与核心产物同名同目录, 便于部署时二选一)。
build() {  # tag target ext with_text [libs] [srcs...]
    # 先把参数取进数组再解析 —— 不要用 `shift N`: 调用方有时只传 2 个参数
    # (tag + target), `shift 4` 会失败, 而脚本带 `set -e` 会直接退出。
    local -a args=("$@")
    local tag="${args[0]:-}" target="${args[1]:-}"
    local ext="${args[2]:-}" with_text="${args[3]:-}"
    local libs="${args[4]:-}"
    local srcs=("${SRC[@]}")
    if [ "${#args[@]}" -gt 5 ]; then srcs=("${args[@]:5}"); fi
    local out="$OUT/$tag$ext"
    printf '  %-22s ' "$tag"
    local extra=""
    if [ "$with_text" = "text" ]; then
        extra="-L/opt/homebrew/lib -lfreetype"
    else
        extra="-DHN_NO_TEXT"
    fi
    if "$ZIG" cc -target "$target" "${CFLAGS[@]}" $extra $libs "${srcs[@]}" -o "$out" 2>/tmp/hn_build_err.txt; then
        local size
        size=$(wc -c < "$out" | tr -d ' ')
        printf '✓  %s (%s KB)%s\n' "$(basename "$out")" "$((size / 1024))" \
            "$([ "$with_text" = "text" ] && echo ' +文本' || echo ' 无文本')"
    else
        printf '✘ 失败\n'
        sed 's/^/      /' /tmp/hn_build_err.txt | head -6
        return 1
    fi
}
# 本机 arm64 带文本; 其余 HN_NO_TEXT(部署平台自行接 FreeType 即可开启)
build "macos-arm64"    "aarch64-macos" "" "text"
build "macos-x86_64"   "x86_64-macos"
echo
echo "Linux (musl 静态, HN_NO_TEXT — 文本需平台 FreeType):"
build "linux-x86_64"   "x86_64-linux-musl" "" "-DHN_NO_TEXT"
build "linux-aarch64"  "aarch64-linux-musl" "" "-DHN_NO_TEXT"
echo
echo "Windows (mingw 静态, HN_NO_TEXT):"
build "windows-x86_64" "x86_64-windows-gnu" ".exe" "-DHN_NO_TEXT"

# Windows 运行时: Win32 窗口 + hnsoft 软件光栅 + sys:// 系统桥 + WebView2 兜底。
# 需要链接 win32 系统库(user32/gdi32/ole32/oleaut32/shell32)。
# 这三个文件(hnwin.c / sysbridge.c / hnwebview.c)此前**不在任何构建入口里** ——
# 提交了却从未被编译, 所以 hnwebview.c 的 COM vtable 类型错误能一直留着。
# 接进来之后交叉编译立刻暴露它们。
WIN_RT=(tools/hnwin.c tools/sysbridge.c tools/hnwebview.c
        Sources/CHtmlNative/hn_arena.c Sources/CHtmlNative/hn_html.c
        Sources/CHtmlNative/hn_css.c Sources/CHtmlNative/hn_style.c
        Sources/CHtmlNative/hn_layout.c Sources/CHtmlNative/hn_paint.c
        Sources/CHtmlNative/hn_context.c Sources/CHtmlNative/hn_theme.c
        Sources/CHtmlNative/hn_json.c Sources/CHtmlNative/hn_lottie.c
        Sources/CHtmlNative/hn_mesh.c Sources/CHtmlNative/hn_png.c
        Sources/CHtmlNative/hnsoft.c)
WIN_LIBS="-luser32 -lgdi32 -lole32 -loleaut32 -lshell32"
build "hnwin-windows-x86_64" "x86_64-windows-gnu" ".exe" "-DHN_NO_TEXT" \
      "$WIN_LIBS" "${WIN_RT[@]}"
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
