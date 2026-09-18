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

# ---------------- 自检 ----------------
# 1) 布局不变量 + 命中自洽(引擎语义, 与平台无关)
# 2) **跨架构确定性**: 同为 HN_NO_TEXT 口径下, 不同架构的产物对同一文档
#    必须给出逐字节相同的布局输出。这是 `-ffp-contract=off` 存在的理由
#    (arm64 有 FMA 而 x86_64 基线没有, 默认融合会让同一文档算出不同 px 值),
#    但此前没有任何门禁守住它 —— 只能靠人肉比对。
#    Linux/Windows 产物无法在本机执行, 因此这里比的是本机可跑的两个目标
#    (macOS arm64 vs macOS x86_64, 后者经 Rosetta 执行)。
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
echo "跨架构确定性(同 HN_NO_TEXT 口径, 布局输出须逐字节一致):"
# 注意口径: 发布用的 macos-arm64 带 FreeType(真实字形测量), 与无文本产物
# 的排版结果本就不同。要验的是"同一份引擎代码在不同架构上算出的几何一致",
# 所以这里额外编一个 **HN_NO_TEXT 的本机架构**产物专门用于比对
# (只进临时目录, 不污染 dist 的发布产物)。
HOST_NT="$(mktemp -t hncore-notext)"
trap 'rm -f "$HOST_NT"' EXIT
# zig cc 产出的文件权限是 0600, 必须 chmod 才能执行(否则后面调用会
# "permission denied", 且 `set -e` 会让整个脚本以 126 退出)
if "$ZIG" cc -target aarch64-macos "${CFLAGS[@]}" -DHN_NO_TEXT "${SRC[@]}" \
        -o "$HOST_NT" 2>/dev/null && chmod +x "$HOST_NT"; then
    det_ok=0
    det_bad=0
    for f in dashboard showcase webpage lottie transparent; do
        html="$ROOT/examples/$f.html"
        [ -f "$html" ] || continue
        a=$("$HOST_NT" boxes "$html" 460 560 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
        b=$("$OUT/hncore-macos-x86_64" boxes "$html" 460 560 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
        if [ -n "$a" ] && [ "$a" = "$b" ]; then
            printf '  ✓ %-12s %s\n' "$f" "${a:0:16}"
            det_ok=$((det_ok + 1))
        else
            printf '  ✗ %-12s arm64=%s x86_64=%s\n' "$f" "${a:0:16}" "${b:0:16}"
            det_bad=$((det_bad + 1))
        fi
    done
    if [ "$det_bad" -gt 0 ]; then
        echo "  ✗ 跨架构不一致 $det_bad 个 —— 检查 -ffp-contract=off 是否被去掉"
        exit 1
    fi
    echo "  ✓ $det_ok 个文档跨架构一致"
else
    echo "  (跳过: 无法构建 HN_NO_TEXT 本机产物用于比对)"
fi
if [ "$det_bad" -gt 0 ]; then
    echo "  ✘ 跨架构不一致 $det_bad 个 —— 检查 -ffp-contract=off 是否被去掉"
    exit 1
fi
echo "  ✓ $det_ok 个文档跨架构一致"

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
