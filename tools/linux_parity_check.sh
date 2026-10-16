#!/usr/bin/env bash
# linux_parity_check.sh — Linux/X11 平台壳对齐门禁(编译级 + 符号级, 0.1)
#
# 诚实口径: 本机(macOS 开发机)没有 X11 运行时, 桌宠形态的行为级验证
# (真开窗/真穿透/真悬浮)只能在 Linux+X11 主机上做。本脚本做四件
# 一台 macOS 就能跑完的事, 并把边界明说:
#
#   ① 编译级: zig 交叉编译 hnapp-linux(x86_64 + aarch64 两目标, 与
#      build-multiplatform.sh 的 hnapp-linux-x86_64 链接行同源);
#   ② 符号级: 对产物跑 strings(/nm)断言实现痕迹 —— EWMH 原子名、
#      dlsym 符号名都是字面 .rodata 字符串, 无需运行即可断言;
#   ③ 声明清单对账: hn_platform.h 的桌宠形态 desc 字段在 hnp_linux.c
#      里逐字段有消费点(grep 计数);
#   ④ 打印"运行时验证需 Linux+X11 主机"一行 —— 行为级验证的诚实边界。
#
# 退出码: 任何编译失败/断言失败/字段缺消费点 → 非 0。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ZIG="${ZIG:-zig}"
OUT="$(mktemp -d /tmp/hn_parity.XXXXXX)"   # 产物进临时目录, 不污染 dist/
trap 'rm -rf "$OUT"' EXIT

FAIL=0
say()  { printf '%s\n' "$*"; }
ok()   { printf '  ✓ %s\n' "$*"; }
bad()  { printf '  ✘ %s\n' "$*"; FAIL=1; }

# ---------------- ① 编译级: 交叉编译 hnapp-linux 两目标 ----------------
# 链接行镜像 build-multiplatform.sh 的 hnapp-linux-x86_64 目标(同源清单:
# 唯一主循环 + 媒体宿主 + hn_rt + hnp_linux + 引擎 + wasm3),
# HN_NO_TEXT / -lm -ldl 口径一致; aarch64 是同 recipe 换 target。
# 媒体宿主清单从 build-multiplatform.sh **现场提取**(MEDIA_SRC 那一行),
# 避免两处手抄漂移; 引擎源清单为稳定清单, 手抄同源(构建清单大改时同步)。
WASM3_SRC="vendor/wasm3/m3_bind.c vendor/wasm3/m3_code.c vendor/wasm3/m3_compile.c
vendor/wasm3/m3_core.c vendor/wasm3/m3_env.c vendor/wasm3/m3_exec.c
vendor/wasm3/m3_function.c vendor/wasm3/m3_info.c vendor/wasm3/m3_module.c
vendor/wasm3/m3_parse.c vendor/wasm3/m3_validate.c
Sources/CHtmlNative/hn_wasm.c"
ENGINE_SRC="app/hn_app.c rt/hn_rt.c rt/hn_sys.c rt/hn_http.c platform/hnp_linux.c
Sources/CHtmlNative/hn_arena.c Sources/CHtmlNative/hn_html.c Sources/CHtmlNative/hn_md.c
Sources/CHtmlNative/hn_fmt.c Sources/CHtmlNative/hn_css.c Sources/CHtmlNative/hn_style.c
Sources/CHtmlNative/hn_layout.c Sources/CHtmlNative/hn_paint.c Sources/CHtmlNative/hn_media.c
Sources/CHtmlNative/hn_context.c Sources/CHtmlNative/hn_theme.c Sources/CHtmlNative/hn_json.c
Sources/CHtmlNative/hn_lottie.c Sources/CHtmlNative/hn_mesh.c Sources/CHtmlNative/hn_png.c
Sources/CHtmlNative/hn_gif.c Sources/CHtmlNative/hn_canvas.c Sources/CHtmlNative/hnsoft.c
Sources/CHtmlNative/hn_pe.c Sources/CHtmlNative/hn_cubism.c Sources/CHtmlNative/hn_three.c
Sources/CHtmlNative/neb3_soft.c
$WASM3_SRC"
MEDIA_SRC="$(grep -m1 '^MEDIA_SRC=' tools/build-multiplatform.sh | sed 's/^MEDIA_SRC=//; s/"//g')"
[ -n "$MEDIA_SRC" ] || MEDIA_SRC="app/hn_media_platform.c app/hn_media_synth.c"
INC="-I Sources/CHtmlNative/include -I Sources/CHtmlNative -I rt -I platform -I vendor/wasm3"

say "① 交叉编译 hnapp-linux(zig cc, musl 静态, HN_NO_TEXT; 媒体宿主: $MEDIA_SRC):"
build_one() {
    # local 一行多赋值时右侧引用同语句左侧变量会在赋值前展开(set -u 直接炸,
    # 同 build-multiplatform.sh 里"先取进数组再解析"的坑) —— 拆成多行。
    local tag="$1"
    local target="$2"
    local bin="$OUT/hnapp-linux-$tag"
    printf '  %-22s ' "hnapp-linux-$tag"
    if "$ZIG" cc -target "$target" -std=c99 -O2 -ffp-contract=off $INC \
            -DHN_NO_TEXT -DHN_VERSION='"parity"' \
            $ENGINE_SRC $MEDIA_SRC -lm -ldl -o "$bin" 2>"$OUT/err-$tag.txt"; then
        printf '✓ (%s KB)\n' "$(( $(wc -c < "$bin" | tr -d ' ') / 1024 ))"
    else
        printf '✘ 失败\n'
        grep -i 'error' "$OUT/err-$tag.txt" | sort -u | sed 's/^/      /' | head -6
        bad "hnapp-linux-$tag 编译失败"
        return 1
    fi
}
build_one x86_64  x86_64-linux-musl  || true
build_one aarch64 aarch64-linux-musl || true

# ---------------- ② 符号级断言: strings(/nm)断言实现痕迹 ----------------
# EWMH 原子名(XInternAtom 的字面参数)与 dlsym 符号名字符串都在产物里,
# 交叉产物不运行也能断言"实现真的编进去了"。
# 两个坑(实测):
#   1) grep -q 在 pipefail 下会误杀 —— -q 命中即退出, strings 收到 SIGPIPE
#      非零退出, 整条管道被判负。用 grep -c 计数比较。
#   2) dlopen 的路径字面量(libX11.so.6/libXext.so.6)在 zig musl 静态链接
#      的产物里被折叠掉(musl 静态的 dlopen 本身是 stub, 见"遗留问题"),
#      不对它们做硬断言 —— 运行期 dlopen 能力是行为级验证的事。
assert_strs() {
    local bin="$1"
    shift
    local s
    for s in "$@"; do
        if [ "$(strings "$bin" | grep -cFx "$s")" -gt 0 ]; then
            ok "strings: $s"
        else
            bad "strings: 缺 $s"
        fi
    done
}
X11_TRACE=(_NET_WM_STATE _NET_WM_STATE_ABOVE _NET_WM_STATE_BELOW _NET_WM_STATE_STICKY
           _MOTIF_WM_HINTS
           XShapeCombineRectangles XTranslateCoordinates XMoveWindow
           XChangeProperty XSendEvent)
# 视频图层(Xv)原语痕迹: hn_platform.h 尾部契约(hnp_video_layer_attach/frame/
# detach)的 Linux 实现走 XVideo 扩展(platform/hnp_linux.c 文件尾, libXv 运行期
# dlopen)—— dlsym 符号名同为字面 .rodata 字符串, 交叉产物不运行也能断言
# "Xv 原语真的编进去了"。(dlopen 路径字面量 libXv.so.6 与上面 libX11/libXext
# 同款会被 musl 静态折叠, 不做硬断言。)
XVIDEO_TRACE=(XvQueryExtension XvQueryAdaptors XvGrabPort XvUngrabPort
              XvCreateImage XvPutImage XvStopVideo XvFreeAdaptorInfo)
say "② 符号级断言(产物 .rodata 实现痕迹):"
for bin in "$OUT"/hnapp-linux-x86_64 "$OUT"/hnapp-linux-aarch64; do
    [ -f "$bin" ] || continue
    assert_strs "$bin" "${X11_TRACE[@]}"
    assert_strs "$bin" "${XVIDEO_TRACE[@]}"
    # nm 尽力而为: zig 链接产物只保留部分局部符号(LTO 折叠), 符号缺席
    # 不判负 —— 字符串断言是硬门禁。
    if nm "$bin" >/dev/null 2>&1; then
        if [ "$(nm "$bin" 2>/dev/null | grep -c "hnp_window")" -gt 0 ]; then
            say "  (nm: $(basename "$bin") 保留 hnp_window* 符号)"
        else
            say "  (nm: $(basename "$bin") 无 hnp_window* 局部符号(LTO 折叠), 以 strings 为准)"
        fi
    else
        say "  (nm 读不了交叉 ELF, 跳过 —— strings 断言已覆盖)"
    fi
done

# ---------------- ③ 声明清单对账: desc 字段 → hnp_linux.c 消费点 ----------------
# hn_platform.h 的桌宠形态字段逐个对账: "消费点"= d-><字段> 字样(代码);
# no_shadow 是契约内的"优雅忽略"(X11 无标准对应), 对账其注释差异点。
say "③ 声明清单对账(hn_platform.h 桌宠字段 → hnp_linux.c 消费点):"
field_count() { grep -c "d->$1" platform/hnp_linux.c || true; }
audit_field() { # <字段> <最少出现次数> <说明>
    local n; n=$(field_count "$1")
    if [ "$n" -ge "$2" ]; then ok "d->$1 ×$n ($3)"
    else bad "d->$1 出现 $n 次(<$2)($3)"; fi
}
audit_field always_on_top   1 "层级: ABOVE/BELOW 属性+client message"
audit_field join_all_spaces 1 "跨空间: STICKY"
audit_field borderless      1 "无边框: _MOTIF_WM_HINTS"
audit_field click_through   1 "穿透: 初始态 + 运行期切换"
audit_field no_shadow       1 "无 X11 对应: 注释化优雅忽略(能力矩阵差异点)"
audit_field x               1 "摆位: XMoveWindow(HNP_WINDOW_POS_AUTO 哨兵)"
audit_field y               1 "摆位: XMoveWindow(HNP_WINDOW_POS_AUTO 哨兵)"
# origin 读回必须真的走 XTranslateCoordinates(返回 1 而非旧留位的恒 0)
if grep -q "XTranslateCoordinates(x11.dpy, w->xw" platform/hnp_linux.c; then
    ok "hnp_window_origin → XTranslateCoordinates(几何记忆采样口已接)"
else bad "hnp_window_origin 未走 XTranslateCoordinates"; fi

# 视频图层契约(hn_platform.h 尾部)在 hnp_linux.c 里的原语形态:
# attach = grab port + 建 XvImage(YV12/I420), frame = XvPutImage 摆位,
# detach = XvStopVideo/释放收尾。libXv 缺席/无可用端口 → attach 返 0,
# 调用方(app/hn_media_platform.c)回退 CPU 逐帧 —— 降级链不做 SHM 管线。
say "  (视频图层契约对账: hn_platform.h 尾部 → hnp_linux.c Xv 原语):"
if grep -q "XvGrabPort" platform/hnp_linux.c && grep -q "XvCreateImage" platform/hnp_linux.c; then
    ok "attach = XvGrabPort + XvCreateImage(grab 端口 + 建 YV12/I420 帧缓冲)"
else bad "视频图层 attach 缺 XvGrabPort/XvCreateImage"; fi
if grep -q "XvPutImage(x11.dpy" platform/hnp_linux.c; then
    ok "frame = XvPutImage 摆位(盒跟随 + 内容推进通道)"
else bad "视频图层 frame 缺 XvPutImage"; fi
if grep -q "XvStopVideo" platform/hnp_linux.c && grep -q "XvFreeAdaptorInfo" platform/hnp_linux.c; then
    ok "detach = XvStopVideo + XvFreeAdaptorInfo(+ 末层 XvUngrabPort)收尾"
else bad "视频图层 detach 收尾不完整"; fi
if grep -q "hn_xvl_close_window(w)" platform/hnp_linux.c; then
    ok "hnp_window_close 先摘图层子窗(父窗/GC 仍在的顺序保证)"
else bad "hnp_window_close 缺图层清理调用"; fi

# ---------------- ④ 诚实边界 ----------------
say "④ 边界:"
say "  运行时验证需 Linux+X11 主机(行为级: 真悬浮/真穿透/真摆位 —— 本脚本只到编译级+符号级)"
say "  视频图层同理: Xv 端口 grab/真摆图/降级回退需 Linux+X11+libXv 主机实测"

if [ "$FAIL" -eq 0 ]; then
    say "linux_parity_check: PASS"
    exit 0
fi
say "linux_parity_check: FAIL"
exit 1
