#!/usr/bin/env bash
# tools/_engine_sources.sh — 探针脚本共享库(被 source, 不单独执行)。
# ============================================================================
# 语言纯度收敛的落点: 16 个 *_probe.py 门禁薄封装全部转成 bash 后, 这里是
# **唯一**一份"引擎源文件清单 + 编译口径"。以后加/删引擎源文件只改本文件,
# 16 个探针 .sh 自动跟上(此前清单散在 16 份 python 里, 是漂移温床)。
#
# 提供四组能力:
#   1. 路径常量 + 引擎源清单(核心 20 源为公共子集, 全量 = 核心+wasm+hnsoft)
#   2. hn_probe_compile: 统一 cc 编译(打印命令 / 失败透传 stderr 尾部 /
#      产物缓存: 产物已存在且比全部源新 → 跳过编译)
#   3. 探针运行/身份校验/临时目录等薄工具
#   4. 两个现场编译的 C99 小助手(源码内嵌于本文件, 与 pet_probe.sh 的
#      "内嵌探针现场 cc"同一手法, 仓库不新增 C 文件):
#      - hn-pngstats: PNG 解码 → 尺寸/四角 alpha/透明占比/半透明占比/内容量
#        (替代原 css_probe.py / pet_probe.sh 里的 python zlib 解码器;
#         直接链引擎自带的 Sources/CHtmlNative/hn_png.c, 仍零外部依赖)
#      - hn-rpc: Unix socket JSON-lines 单发客户端(替代原 window_probe.sh /
#        resident_probe.sh 里的 python socket 客户端)
#
# 口径与被替换的 python 版逐条对齐:
#   - CC 环境变量优先(缺省 cc)
#   - 编译失败: stderr 尾部 4000 字节透传, 返回非 0
#   - 传参二进制须含探针 banner 字面量才复用(身份判据, 门禁会把
#     dist 引擎二进制也传进来, 盲跑只会撞 usage)
#
# 兼容 macOS 自带 bash 3.2: 不用关联数组/mapfile 等/bash4 特性。

# ---- 1. 路径常量与源清单 ----------------------------------------------------

_HES_SELF="${BASH_SOURCE[0]}"
HN_TOOLS_DIR="$(cd "$(dirname "$_HES_SELF")" && pwd)"
HN_REPO_ROOT="$(cd "$HN_TOOLS_DIR/.." && pwd)"
HN_ENG="$HN_REPO_ROOT/Sources/CHtmlNative"
HN_RT_DIR="$HN_REPO_ROOT/rt"
HN_WASM3_DIR="$HN_REPO_ROOT/vendor/wasm3"

# 所有 build*.sh 与探针共用的必需 C99 引擎清单。
# hn_paint 硬引用 pe/cubism/three; 无 SDK 宏时对应模块安全降级。
# Canvas C++ 后端是可选项，由 hn_canvas_build 独立编译并配套定义宏。
HN_ENGINE_CORE=(hn_arena.c hn_html.c hn_md.c hn_css.c hn_style.c hn_layout.c
                hn_paint.c hn_context.c hn_theme.c hn_canvas.c hn_json.c
                hn_lottie.c hn_mesh.c hn_png.c hn_gif.c hn_media.c hn_fmt.c
                hn_pe.c hn_cubism.c hn_three.c neb3_soft.c)
HN_ENGINE_SOFT=(hnsoft.c)      # 软渲染宿主(hnsoft 位图后端)
HN_ENGINE_WASM=(hn_wasm.c)     # WASM 缝(须连 vendor/wasm3 一起链)
HN_WASM3=(m3_bind.c m3_code.c m3_compile.c m3_core.c m3_env.c m3_exec.c
          m3_function.c m3_info.c m3_module.c m3_parse.c m3_validate.c)
HN_RT_SRC=(hn_rt.c hn_sys.c hn_http.c)   # 公共运行时 + 单源 sys:// 路由 + HTTP
HN_WASM3_EXPECT=11             # vendor 快照 = 契约最小 11 源(glob 数漂移即报错)

HN_QUICKJS_A="/opt/homebrew/opt/quickjs/lib/quickjs/libquickjs.a"
HN_QUICKJS_INC="/opt/homebrew/opt/quickjs/include/quickjs"

# ---- 源清单装配(结果进 HN_SOURCES, 全部为绝对路径) ----

hn_sources_core() {  # 纯引擎核心(无 hnsoft: <include> 装载/布局/显示列表够用)
  HN_SOURCES=()
  local f
  for f in "${HN_ENGINE_CORE[@]}"; do HN_SOURCES+=("$HN_ENG/$f"); done
}

hn_sources_soft() {  # 核心 + hnsoft(fmt/md/stream/gif/quad3d 直染口径)
  hn_sources_core
  local f
  for f in "${HN_ENGINE_SOFT[@]}"; do HN_SOURCES+=("$HN_ENG/$f"); done
}

hn_sources_full() {  # 核心 + hnsoft + wasm(anim/canvas/timer, 与 hn_app 同链)
  hn_sources_soft
  local f
  for f in "${HN_ENGINE_WASM[@]}"; do HN_SOURCES+=("$HN_ENG/$f"); done
}

hn_sources_check() {  # 任一缺源 → 打印并返回非 0(门禁口径: 缺源 → exit 1)
  local s
  for s in "$@"; do
    if [ ! -f "$s" ]; then
      echo "FAIL 缺源文件 $s"
      return 1
    fi
  done
  return 0
}

# FreeType 头目录(homebrew 常见前缀); 找不到返回非 0(编译会自己报)
hn_freetype_inc() {
  local p
  for p in /opt/homebrew/include/freetype2 /usr/local/include/freetype2; do
    if [ -f "$p/ft2build.h" ]; then
      printf '%s\n' "$p"
      return 0
    fi
  done
  return 1
}

# hn_canvas_build <target|native> <object-dir> <C++ compiler> [compiler args...]
# HN_CANVAS_ITY=1 / HN_CANVAS_CG=1 opt in; flags and objects always travel together.
# C++ is compiled separately so C99 callers keep their language mode.
hn_canvas_build() {
  local target="$1" objdir="$2"; shift 2
  HN_CANVAS_DEFS=() HN_CANVAS_OBJECTS=() HN_CANVAS_LIBS=()
  local ity="${HN_CANVAS_ITY:-0}" cg="${HN_CANVAS_CG:-0}"
  case "$ity:$cg" in 0:0) return 0 ;; 0:1|1:0|1:1) ;; *)
    printf '%s\n' 'HN_CANVAS_ITY / HN_CANVAS_CG must be 0 or 1' >&2; return 1 ;;
  esac
  local flags=()
  if [ "$target" != native ]; then flags=(-target "$target"); fi
  if [ "$cg" = 1 ]; then
    case "$target" in
      *macos*|*apple-darwin*) ;;
      native) [ "$(uname -s)" = Darwin ] || { printf '%s\n' 'Canvas CG requires macOS' >&2; return 1; } ;;
      *) printf '%s\n' 'Canvas CG requires a macOS target' >&2; return 1 ;;
    esac
  fi
  mkdir -p "$objdir"
  local backend macro
  for backend in ity cg; do
    case "$backend" in ity) [ "$ity" = 1 ] || continue; macro=HN_HAVE_CANVAS_ITY ;;
                       cg) [ "$cg" = 1 ] || continue; macro=HN_HAVE_CANVAS_CG ;; esac
    "$@" "${flags[@]+"${flags[@]}"}" -std=c++17 -fno-exceptions -fno-rtti -O2 \
      -I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_REPO_ROOT/vendor/canvas_ity" \
      -c "$HN_ENG/hn_canvas_$backend.cpp" -o "$objdir/hn_canvas_$backend.o" || return 1
    HN_CANVAS_DEFS+=("-D$macro")
    HN_CANVAS_OBJECTS+=("$objdir/hn_canvas_$backend.o")
  done
  case "$target" in
    *macos*|*apple-darwin*) HN_CANVAS_LIBS=(-lc++ -lc++abi) ;;
    native) if [ "$(uname -s)" = Darwin ]; then HN_CANVAS_LIBS=(-lc++); else HN_CANVAS_LIBS=(-lstdc++); fi ;;
    *) HN_CANVAS_LIBS=(-lc++) ;;
  esac
}

# ---- 2. 统一编译 ------------------------------------------------------------

HN_CC="${CC:-cc}"
HN_CFLAGS=()   # 例: (-std=c99 -O2 -Wall -ffp-contract=off)
HN_DEFS=()     # 例: (-DHN_HAVE_QUICKJS)
HN_INCS=()     # 例: (-I "$HN_ENG/include" -I "$HN_ENG")
HN_LIBS=()     # 例: (-lm)

# hn_up_to_date <产物> [源...] — 产物存在、可执行、且比所有源都新 → 0(缓存命中)
hn_up_to_date() {
  local bin="$1"; shift
  [ -x "$bin" ] || return 1
  local s
  for s in "$@"; do
    [ "$bin" -nt "$s" ] || return 1
  done
  return 0
}

# hn_probe_compile <标签> <产物路径> — 按 HN_CFLAGS/DEFS/INCS/SOURCES/LIBS 编译。
# 打印完整命令(与 python 版同口径); 失败打印"编译失败"+ stderr 尾部 4000 字节。
# 缓存: 产物已存在且比 HN_SOURCES 全部源新 → 跳过编译(防重复重编, 门禁跑全量
# 时 16 份探针只各编一次)。
hn_probe_compile() {
  local label="$1" out="$2"
  if [ "${HN_CANVAS_ITY:-0}:${HN_CANVAS_CG:-0}" = 0:0 ] && \
     hn_up_to_date "$out" "$HN_TOOLS_DIR/_engine_sources.sh" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"}; then
    echo "$label: 缓存命中(产物比全部源新), 跳过编译: $out"
    return 0
  fi
  echo "$label: 编译: $HN_CC ${HN_CFLAGS[*]+"${HN_CFLAGS[*]}"} ${HN_DEFS[*]+"${HN_DEFS[*]}"} ${HN_INCS[*]+"${HN_INCS[*]}"} ${HN_SOURCES[*]+"${HN_SOURCES[*]}"} ${HN_LIBS[*]+"${HN_LIBS[*]}"} -o $out"
  local errlog
  errlog="$(mktemp "${TMPDIR:-/tmp}/hncc-XXXXXX")"
  local canvas_dir
  canvas_dir="$(mktemp -d "${TMPDIR:-/tmp}/hncanvas-XXXXXX")"
  if ! hn_canvas_build native "$canvas_dir" "${CXX:-c++}"; then
    rm -rf "$canvas_dir"
    return 1
  fi
  # shellcheck disable=SC2086
  if "$HN_CC" ${HN_CFLAGS[@]+"${HN_CFLAGS[@]}"} ${HN_DEFS[@]+"${HN_DEFS[@]}"} \
       ${HN_CANVAS_DEFS[@]+"${HN_CANVAS_DEFS[@]}"} \
       ${HN_INCS[@]+"${HN_INCS[@]}"} ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} \
       ${HN_CANVAS_OBJECTS[@]+"${HN_CANVAS_OBJECTS[@]}"} \
       ${HN_LIBS[@]+"${HN_LIBS[@]}"} ${HN_CANVAS_LIBS[@]+"${HN_CANVAS_LIBS[@]}"} -o "$out" 2>"$errlog"; then
    rm -rf "$canvas_dir"
    rm -f "$errlog"
    return 0
  fi
  rm -rf "$canvas_dir"
  echo "$label: 编译失败" >&2
  if [ -s "$errlog" ]; then
    tail -c 4000 "$errlog" >&2
  fi
  rm -f "$errlog"
  return 1
}

# ---- 3. 探针运行薄工具 --------------------------------------------------------

# hn_tmpd <前缀> — 临时目录(python tempfile.mkdtemp(prefix=...) 的对应物)
hn_tmpd() {
  mktemp -d "${TMPDIR:-/tmp}/hn$1-XXXXXX"
}

# hn_bin_has <二进制> <字面量> — 探针身份判据: banner 字面量必入 __TEXT
hn_bin_has() {
  local bin="$1" needle="$2"
  [ -f "$bin" ] || return 1
  LC_ALL=C grep -qF -- "$needle" "$bin" 2>/dev/null
}

# hn_probe_run <标签> <二进制> [参数...] — cwd=仓库根运行, 透传退出码,
# 结束后打 OK / FAIL 退出码(与 python 版相同的收尾行)。
hn_probe_run() {
  local label="$1" bin="$2"; shift 2
  local rc=0
  ( cd "$HN_REPO_ROOT" && "$bin" "$@" ) || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "$label: FAIL 退出码 $rc"
  else
    echo "$label: OK"
  fi
  return "$rc"
}

# ---- 4. 现场编译的 C99 小助手(源码内嵌, 不新增仓库文件) ----------------------

HN_HELPER_DIR=""

hn_helper_dir() {
  if [ -z "$HN_HELPER_DIR" ] || [ ! -d "$HN_HELPER_DIR" ]; then
    HN_HELPER_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hn-helpers-XXXXXX")"
  fi
  printf '%s\n' "$HN_HELPER_DIR"
}

# hn_pngstats_build — 编译 PNG 像素统计助手, 输出二进制路径。
# 用法: hn-pngstats <png>; 输出一行:
#   w=<宽> h=<高> corners=<四角alpha逗号分隔> trpct=<透明%> papct=<半透明%> content=<alpha>16像素数>
# trpct/papct 沿用原 python 解码器口径: 每 7 像素抽样(0,7,14,…)。
hn_pngstats_build() {
  local dir bin
  dir="$(hn_helper_dir)"
  bin="$dir/hn-pngstats"
  if [ -x "$bin" ]; then
    printf '%s\n' "$bin"
    return 0
  fi
  cat > "$dir/hn_pngstats.c" <<'CEOF'
/* hn-pngstats — PNG 逐像素 alpha 统计(css_probe/pet_probe 共用, 现场编译)。
 * 链引擎自带的 hn_png.c(自带 inflate, 零外部依赖)。 */
#include <stdio.h>
#include <stdlib.h>
#include "hn_png.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: hn-pngstats <png>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("hn-pngstats: open"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 2; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 2; }
    fclose(f);
    int w = 0, h = 0;
    unsigned char *px = hn_png_decode(buf, (size_t)n, &w, &h);
    free(buf);
    if (!px || w <= 0 || h <= 0) { fprintf(stderr, "hn-pngstats: decode failed\n"); return 2; }
    long total = (long)w * h;
    long tot = 0, tr = 0, pa = 0, content = 0, i;
    /* 透明/半透明占比: 每 7 像素抽样(与原 python 解码器同口径) */
    for (i = 0; i < total; i += 7) {
        int a = px[i * 4 + 3];
        tot++;
        if (a == 0) tr++;
        else if (a != 255) pa++;
    }
    /* 内容量: 全量扫 alpha>16(实画内容) */
    for (i = 0; i < total; i++)
        if (px[i * 4 + 3] > 16) content++;
    printf("w=%d h=%d corners=%d,%d,%d,%d trpct=%.4f papct=%.4f content=%ld\n",
           w, h,
           px[3],
           px[((size_t)w - 1) * 4 + 3],
           px[((size_t)(h - 1) * w) * 4 + 3],
           px[((size_t)(h - 1) * w + (size_t)w - 1) * 4 + 3],
           tot ? (double)tr * 100.0 / (double)tot : 0.0,
           tot ? (double)pa * 100.0 / (double)tot : 0.0,
           content);
    free(px);
    return 0;
}
CEOF
  if ! "$HN_CC" -std=c99 -O2 -Wall -I "$HN_ENG" -I "$HN_ENG/include" \
        "$dir/hn_pngstats.c" "$HN_ENG/hn_png.c" -o "$bin" -lm 2>"$dir/pngstats.log"; then
    echo "hn-pngstats 编译失败" >&2
    tail -c 2000 "$dir/pngstats.log" >&2
    return 1
  fi
  printf '%s\n' "$bin"
}

# hn_rpc_build — 编译 Unix socket JSON-lines 单发客户端, 输出二进制路径。
# 用法: hn-rpc <socket路径> <一行JSON>; 连接、发送(补 \n)、读一行回包打到 stdout。
hn_rpc_build() {
  local dir bin
  dir="$(hn_helper_dir)"
  bin="$dir/hn-rpc"
  if [ -x "$bin" ]; then
    printf '%s\n' "$bin"
    return 0
  fi
  cat > "$dir/hn_rpc.c" <<'CEOF'
/* hn-rpc — daemon Unix socket JSON-lines 单发客户端(现场编译)。
 * 与被替换的 python 客户端同协议: 连接 → 发一行 → 读一行(到 \n 或对端关) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: hn-rpc <socket> <line>\n"); return 2; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", argv[1]);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("hn-rpc: socket"); return 2; }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("hn-rpc: connect"); return 2;
    }
    size_t ln = strlen(argv[2]);
    if (write(fd, argv[2], ln) != (ssize_t)ln || write(fd, "\n", 1) != 1) {
        perror("hn-rpc: write"); return 2;
    }
    size_t cap = (size_t)1 << 22, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { return 2; }
    while (n + 1 < cap) {
        ssize_t r = read(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
        char *nl = (char *)memchr(buf, '\n', n);
        if (nl) { n = (size_t)(nl - buf); break; }
    }
    buf[n] = 0;
    fwrite(buf, 1, n, stdout);
    fputc('\n', stdout);
    close(fd);
    free(buf);
    return 0;
}
CEOF
  if ! "$HN_CC" -std=c99 -O2 -Wall "$dir/hn_rpc.c" -o "$bin" 2>"$dir/rpc.log"; then
    echo "hn-rpc 编译失败" >&2
    tail -c 2000 "$dir/rpc.log" >&2
    return 1
  fi
  printf '%s\n' "$bin"
}

# hn_rpc <socket路径> <一行JSON> — 单发请求, 回包一行(stdout)
hn_rpc() {
  local rpcbin
  rpcbin="$(hn_rpc_build)" || return 2
  "$rpcbin" "$1" "$2"
}

# ---- 5. 纯 bash 探针(原 css/html/layout/lottie/media_element_probe.py)共享 ----
# 五份 hncore 驱动型探针的公共骨架: HN 解析(调用方 cwd 优先, 再仓库根相对)、
# boxes 输出解析、四元组近似比较、通过/失败账本。报告逐字对齐原 python 版。

# hn_find_hn <参数1或空> <缺省路径> — 解析 hncore 路径到 HN。
# 口径: 先按调用方 cwd 解析; 不存在且参数非绝对 → 再以仓库根为基准解析一次;
# 都不存在 → 打印提示(先构建)并 exit 1。
#
# C1 裁决(ISSUES-1): 不传参时**不再盲信调用方缺省路径**(历史上是 /tmp/hncore,
# 极易是陈旧副本 —— "改完引擎跑探针"会拿到旧二进制的结果, 看起来像修复没生效)。
# 顺序: dist 里的本机架构产物 → 调用方缺省路径。最终落在 /tmp 下时打醒目告警
# (带 mtime), 提示显式传 ./dist/hncore-<os>-<arch>。
hn_find_hn() {
  local raw
  if [ "${1:-}" = "" ]; then
    # 本机架构的 dist 产物(Darwin→macos; linux arm64 口径是 aarch64)
    local _os _arch _cand
    _os="$(uname -s 2>/dev/null || echo Darwin)"
    _arch="$(uname -m 2>/dev/null || echo arm64)"
    case "$_os" in Darwin) _os="macos" ;; Linux) _os="linux" ;; esac
    [ "$_arch" = "arm64" ] && [ "$_os" = "linux" ] && _arch="aarch64"
    _cand="$HN_REPO_ROOT/dist/hncore-${_os}-${_arch}"
    if [ -e "$_cand" ]; then
      raw="$_cand"
    else
      raw="$2"                                   # dist 缺席 → 沿用调用方缺省
    fi
  else
    raw="$1"
  fi
  HN="$(cd "$(dirname "$raw")" 2>/dev/null && printf '%s/%s' "$(pwd)" "$(basename "$raw")")"
  if [ ! -e "$HN" ] && [ "${1:-}" != "" ] && [ "${1#/}" = "$1" ]; then
    local alt="$HN_REPO_ROOT/$1"
    if [ -e "$alt" ]; then HN="$alt"; fi
  fi
  if [ ! -e "$HN" ]; then
    echo "找不到 hncore: $HN(调用方 cwd 与仓库根下都不存在)"
    echo "先构建: bash tools/build-multiplatform.sh"
    exit 1
  fi
  case "$HN" in
    /tmp/*)
      echo "⚠️  用的是 /tmp 副本: $HN (mtime: $(stat -f '%Sm' "$HN" 2>/dev/null || stat -c '%y' "$HN" 2>/dev/null))" >&2
      echo "    可能是陈旧构建; 建议显式传 ./dist/hncore-<os>-<arch>" >&2
      ;;
  esac
}

# hn_boxes_lines <boxes 原始输出> — 解析成 "id x y w h" 行(文档序, 全保留;
# 首行 header 跳过; id 为空或坐标非数则丢行, 与原 python 解析一致)。
hn_boxes_lines() {
  awk -F, '
    NR > 1 && NF >= 6 && $2 != "" {
      x = $3; y = $4; w = $5; h = $6
      gsub(/^[ \t]+|[ \t]+$/, "", $2)
      gsub(/^[ \t]+|[ \t]+$/, "", x); gsub(/^[ \t]+|[ \t]+$/, "", y)
      gsub(/^[ \t]+|[ \t]+$/, "", w); gsub(/^[ \t]+|[ \t]+$/, "", h)
      if (x !~ /^-?[0-9.]+$/ || y !~ /^-?[0-9.]+$/ ||
          w !~ /^-?[0-9.]+$/ || h !~ /^-?[0-9.]+$/) next
      print $2, x, y, w, h
    }'
}

# hn_box_of <boxes 行> <id> [first|last] — 取该 id 的 "x y w h";
# 缺失 → "MISSING"(python res.get(id, "MISSING") 对应物)。
# 注: 原 python 里 layout/media_element 取"最后一条", css/html 取"第一条";
# 各探针 id 在单页内唯一, 两种取法结果一致 —— 缺省 last, 可显式 first。
hn_box_of() {
  local got
  got="$(printf '%s\n' "$1" | awk -v id="$2" -v which="${3:-last}" '
    $1 == id { x=$2; y=$3; w=$4; h=$5; if (which == "first") exit }
    END { if (x != "") print x, y, w, h }')"
  printf '%s\n' "${got:-MISSING}"
}

# hn_quad_near <got "x y w h"|MISSING> <wx> <wy> <ww> <wh> [tol=1] — 逐分量
# 绝对差 <= tol(python near() 对应物; MISSING 视为失败)。
hn_quad_near() {
  local got="$1"
  [ "$got" = "MISSING" ] && return 1
  awk -v g="$got" -v w="$2 $3 $4 $5" -v t="${6:-1}" 'BEGIN {
    if (split(g, A, " ") < 4 || split(w, B, " ") < 4) exit 1
    for (i = 1; i <= 4; i++) { d = A[i] - B[i]; if (d < 0) d = -d
      if (d > t + 1e-9) exit 1 }
  }'
}

# hn_quad_eq <got> <wx> <wy> <ww> <wh> — 四元组精确相等(python ck() 元组对应物)
hn_quad_eq() {
  hn_quad_near "$1" "$2" "$3" "$4" "$5" 0
}

# hn_val_eq <got> <want> — 通用期望值比较: 两边都是四元组 → 按数值逐分量比
# (0.0 == 0, 对齐 python float 相等); 否则按字符串精确比。
hn_val_eq() {
  case "$1 $2" in
    *[!0-9.\ -]*|"") [ "$1" = "$2" ]; return ;;
  esac
  if printf '%s' "$1 $2" | grep -qE -- \
      '^-?[0-9.]+ -?[0-9.]+ -?[0-9.]+ -?[0-9.]+ -?[0-9.]+ -?[0-9.]+ -?[0-9.]+ -?[0-9.]+$'; then
    hn_quad_eq "$1" $2
  else
    [ "$1" = "$2" ]
  fi
}

# 探针账本: NPASS/NFAIL 计数 + FAILED_DESCS(预格式化的失败行)
HN_HARNESS_INITED=""
hn_harness_begin() {
  NPASS=0; NFAIL=0; FAILED_DESCS=()
  hn_harness_init_inited=1
}

hn_pass() { NPASS=$((NPASS + 1)); }

# hn_fail <名> <期望> <实际> — 记失败(printf 宽度由调用脚本的报告格式决定,
# 此处按各脚本的 %-Ns 对齐宽度传入第 4 参, 缺省 34)
hn_fail() {
  NFAIL=$((NFAIL + 1))
  local width="${4:-34}"
  FAILED_DESCS+=("$(printf '  ✘ %-*s 期望 %s  实际 %s' "$width" "$1" "$2" "$3")")
}

# hn_harness_report [标题前缀] — 打印"通过 N / 失败 M" + 失败明细;
# 有失败返回 1(门禁 exit 1 口径)。
hn_harness_report() {
  local prefix="${1:-}"
  echo "${prefix}通过 $NPASS / 失败 $NFAIL"
  local d
  for d in ${FAILED_DESCS[@]+"${FAILED_DESCS[@]}"}; do
    printf '%s\n' "$d"
  done
  [ "$NFAIL" -eq 0 ]
}
