#!/usr/bin/env bash
# tools/perf_probe.sh — CPU 治理的人读报告(信息值为主, 硬断言仅一条宽松线)
# ============================================================================
# 背景: 桌宠(静态立绘 + 环境摆动 + 低频定时器)曾因"定时器表非空/环境动画
# 钉死 16ms 全页重绘"空转到 ~60fps —— 单窗 CPU 两位数, 多窗即可把机器拖到
# 假死。治理后的节拍口径(hn_rt_frame):
#   仅 ambient(hn-mesh/GIF/Lottie)活跃 → 33ms(~30fps, 轻路径免 relayout)
#   interactive(过渡/keyframes/媒体/滚动)→ 16ms
#   定时器表非空(无动画无 RAF)  → min(250, 最近到期) —— 不再 60fps 空转
# 本探针给出真窗口的实测对照(采样均值, 有噪声 —— 硬口径在 C 探针:
# bash tools/timer_probe.sh 的节拍断言; 这里只对人讲故事):
#   ① pet.html 真窗 6s: ps -o time 差分(rusage 口径)取均值 + RSS 两点采样
#      (环境动画页的治理效果; 配 HN_ANIM_FPS=60 可对照"钮放开到 60fps"的代价);
#   ② 图片解码缓存命中/未命中(pet 4s, HN_IMG_CACHE_STATS=1): 引擎按
#      (path, mtime, size) 的有界 LRU 缓存 IMAGE/MESH 解码结果 —— 首帧必
#      miss、其后 ambient 帧必命中。硬断言: 末行 hit≥3 且 miss≥1 且
#      bytes≤48MB 预算(缓存有界的机器可验证口径);
#   ③ 最小页(只有 setTimeout(fn,5000), 无动画无 RAF)6s: 同①口径采样;
#      硬断言 —— 均值 < 5%(治理前该页 ~60fps 空转, 两位数;
#      治理后 4Hz 醒来, 应近 0; 阈值放宽到 5% 容采样抖动);
#   ④⑤ media-demo / canvas-demo 6s: 交互 16ms 全路径 + 图层合成两页的
#      同口径 CPU/RSS(信息值);
#   ⑥ 打印四页对照表(写明采样窗口); 收口纪律同 media_window_probe
#      (exec 让 $! 即 hnapp 本体 + trap 按"探针特有参数"扫尾)。
#
# 用法: bash tools/perf_probe.sh [dist_dir]
#   缺省 dist_dir = $ROOT/dist。治理前后对比: 把旧产物拷到一个目录传进来
#   (如 bash tools/perf_probe.sh /tmp/hn-dist-before), 与新 dist 各跑一遍。
set -u  # 不用 -e: 断言里自己收 failed 计数

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="${1:-$ROOT/dist}"
HN_APP="$DIST/hnapp-macos-arm64"
PET="$ROOT/examples/pet.html"
SBX="$(mktemp -d /tmp/hn-perf-probe-XXXXXX)"
PIDS=""
fails=0

ck() { # ck <描述> <0|1 通过>
    if [ "$2" = "1" ]; then printf '  v %s\n' "$1"
    else printf '  X %s\n' "$1"; fails=$((fails + 1)); fi
}

cleanup() {
    # 先 TERM(优雅关窗)后 KILL 兜底; 再按"本 dist 二进制 + 探针特有文档
    # 名"pkill 扫尾 —— 模式不会命中用户自己开的窗口。
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    pkill -f "hnapp-macos-arm64 pet.html" 2>/dev/null
    pkill -f "hnapp-macos-arm64 perf-timer.html" 2>/dev/null
    pkill -f "hnapp-macos-arm64 media-demo.html" 2>/dev/null
    pkill -f "hnapp-macos-arm64 canvas-demo.html" 2>/dev/null
    rm -rf "$SBX"
}
trap cleanup EXIT INT TERM

# 临时 HOME 隔离(几何/store 账本不碰开发者正在用的)
export HOME="$SBX/home"
mkdir -p "$HOME"

echo "== CPU 治理报告 (perf_probe | $DIST) =="

if [ ! -x "$HN_APP" ]; then
    echo "  X 缺少 $HN_APP —— 先跑 tools/build-multiplatform.sh"; exit 1
fi
if [ ! -f "$PET" ]; then
    echo "  X 缺少 $PET"; exit 1
fi

# sample_cpu <pid>: ps 的 %CPU(衰减均值; 仅作采样花絮打印)
sample_cpu() {
    ps -o %cpu= -p "$1" 2>/dev/null | tr -d ' ' | head -1
}

# run_window <html-abs> <secs> → stdout: "avg=NN.N cpu=SS.Ss samples=a/b/c"
# 后台 exec 起真窗; 起步 1s(启动+首帧)后取**累计 CPU 时间**差(ps -o time,
# 本进程的精确记账, 不受整机负载/调度噪声污染 —— %cpu 衰减均值在共享
# 开发机上会把空闲页都吹到两位数, 不能做断言依据), 途中顺带采 3 次
# %CPU 作花絮。
run_window() {
    local html="$1" secs="$2"
    local log="$SBX/win-$$.log"
    (cd "$ROOT/examples" && exec env HN_ANIM_FPS="${HN_ANIM_FPS:-30}" "$HN_APP" "$html" 360 360) >"$log" 2>&1 &
    local pid=$!
    PIDS="$PIDS $pid"
    sleep 1
    local t0=$(ps -o time= -p "$pid" 2>/dev/null | tr -d ' ')
    local r0=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
    local s1="" s2="" s3=""
    s1=$(sample_cpu "$pid")
    sleep 2; s2=$(sample_cpu "$pid")
    local remain=$((secs - 3))
    [ "$remain" -lt 2 ] && remain=2
    sleep "$remain"; s3=$(sample_cpu "$pid")
    local t1=$(ps -o time= -p "$pid" 2>/dev/null | tr -d ' ')
    local r1=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    PIDS="${PIDS%% $pid}"   # 已收口, 免 trap 重复 kill
    PIDS="${PIDS# }"
    # CPU% = CPU 时间差 / 墙钟 ×100(bash 无浮点, 交给 awk); RSS 取暖机后/收尾两点
    CPU_OUT=$(awk -v a="$t0" -v b="$t1" -v r0="$r0" -v r1="$r1" -v s1="$s1" -v s2="$s2" -v s3="$s3" -v w="$secs" '
      function secs(t) { if (t == "" || index(t, ":") == 0) return -1
        n = split(t, p, ":"); return p[n-1] * 60 + p[n] }
      BEGIN { c0 = secs(a); c1 = secs(b)
        if (c0 < 0 || c1 < 0) printf "avg=-1.0 samples=%s/%s/%s rss=?MB", s1, s2, s3
        else { d = c1 - c0; if (d < 0) d = 0
          printf "avg=%.1f cpu=%.2fs samples=%s/%s/%s rss=%d->%dMB", d * 100 / w, d, s1, s2, s3, r0 / 1024, r1 / 1024 } }')
    echo "$CPU_OUT"
}

# ---------- ① pet.html: 桌宠全景(ambient 摆动 + 3s 气泡 + 眨眼链) ----------
printf '  ① pet.html 6s 真窗 CPU/RSS ... '
PET_R=$(run_window "$PET" 6)
PET_AVG=$(printf '%s' "$PET_R" | sed -n 's/.*avg=\([0-9.]*\).*/\1/p')
echo "| $PET_R"
ck "① pet.html 采样成功(均值 $PET_AVG%)" "$([ -n "$PET_AVG" ] && [ "$(printf '%.0f' "$PET_AVG" 2>/dev/null || echo -1)" -ge 0 ] && echo 1 || echo 0)"

# ---------- ② 图片解码缓存命中/未命中(hnsoft img cache 硬断言) ----------
# 动画页的 IMAGE/MESH 指令每帧都要同一张图: 引擎按 (path, mtime, size) 做
# 有界 LRU 解码缓存(48MB 软上限)。HN_IMG_CACHE_STATS=1 让引擎逐帧向 stderr
# 打 hit/miss 行 —— 首帧必 miss(冷), 其后 ambient 帧必命中。取**末行**断言。
printf '  ② img cache 命中/未命中(pet 4s, HN_IMG_CACHE_STATS=1) ... '
export HN_IMG_CACHE_STATS=1
CACHE_R=$(run_window "$PET" 4)
unset HN_IMG_CACHE_STATS
CACHE_LINE=$(grep 'hn_img_cache:' "$SBX/win-$$.log" 2>/dev/null | tail -1)
C_HIT=$(printf '%s' "$CACHE_LINE" | sed -n 's/.*hit=\([0-9]*\).*/\1/p')
C_MISS=$(printf '%s' "$CACHE_LINE" | sed -n 's/.*miss=\([0-9]*\).*/\1/p')
C_EVICT=$(printf '%s' "$CACHE_LINE" | sed -n 's/.*evict=\([0-9]*\).*/\1/p')
C_BYTES=$(printf '%s' "$CACHE_LINE" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
CACHE_OK=0
if [ -n "$C_HIT" ] && [ -n "$C_MISS" ] \
   && [ "$C_HIT" -ge 3 ] && [ "$C_MISS" -ge 1 ] \
   && [ "${C_BYTES:-0}" -le $((48 * 1024 * 1024)) ]; then
    CACHE_OK=1
fi
ck "② img cache 命中≥3 且未命中≥1 且字节≤48MB 预算(末行: $CACHE_LINE)" "$CACHE_OK"

# ---------- ③ 最小定时器页: 只有 setTimeout(fn,5000), 无动画无 RAF ----------
cat > "$SBX/perf-timer.html" <<'HTML'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="hn-window" content="360x360"></head>
<body style="background:#101418">
<h2 id="t" style="color:#e8eaf5">perf: timer-only</h2>
<p id="s" style="color:#9aa7b8">setTimeout(fn,5000) — 无动画无 RAF</p>
<script>setTimeout(function(){ hnSetText('s', 'fired at ~5s'); }, 5000);</script>
</body></html>
HTML
printf '  ③ timer-only 页 6s 真窗 CPU/RSS ... '
TMR_R=$(run_window "$SBX/perf-timer.html" 6)
TMR_AVG=$(printf '%s' "$TMR_R" | sed -n 's/.*avg=\([0-9.]*\).*/\1/p')
echo "| $TMR_R"
# 唯一硬断言(宽松): 定时器页不再被钉死 60fps → CPU < 5%。
# 治理前: 表非空 ⇒ 16ms 帧循环全页重绘, 实测两位数; 治理后 4Hz 醒来。
TMR_OK=0
if [ -n "$TMR_AVG" ]; then
    awk -v v="$TMR_AVG" 'BEGIN { exit !(v < 5.0) }' && TMR_OK=1
fi
ck "③ timer-only 页 CPU < 5%(治理前 ~60fps 空转两位数; got ${TMR_AVG:-?}%)" "$TMR_OK"

# ---------- ④ media-demo / canvas-demo: 交互 16ms 全路径 + 图层合成 ----------
# 信息值口径同①: 6s 真窗, ps -o time 差分(rusage 口径) ÷ 墙钟, RSS 两点采样。
printf '  ④ media-demo.html 6s 真窗 CPU/RSS ... '
MED_R=$(run_window "$ROOT/examples/media-demo.html" 6)
MED_AVG=$(printf '%s' "$MED_R" | sed -n 's/.*avg=\([0-9.]*\).*/\1/p')
echo "| $MED_R"
printf '  ⑤ canvas-demo.html 6s 真窗 CPU/RSS ... '
CNV_R=$(run_window "$ROOT/examples/canvas-demo.html" 6)
CNV_AVG=$(printf '%s' "$CNV_R" | sed -n 's/.*avg=\([0-9.]*\).*/\1/p')
echo "| $CNV_R"

# ---------- ⑥ 对照(信息值; 采样窗口 6s 墙钟, CPU=进程 rusage 差分) ----------
echo "  ----------------------------------------"
echo "  | 页面                            | %CPU 均值(6s 窗口) |"
echo "  | pet.html(ambient 33ms 轻路径)   | ${PET_AVG:-?}% |"
echo "  | media-demo.html(媒体+图层合成)  | ${MED_AVG:-?}% |"
echo "  | canvas-demo.html(JS+RAF 16ms)   | ${CNV_AVG:-?}% |"
echo "  | timer-only(250ms 上限节拍)      | ${TMR_AVG:-?}% |"
echo "  | img cache: hit=${C_HIT:-?} miss=${C_MISS:-?} evict=${C_EVICT:-?} bytes=${C_BYTES:-?} |"
echo "  | 环境钮: HN_ANIM_FPS=${HN_ANIM_FPS:-30}(16/30/60) |"
echo "  | 硬节拍口径见 timer_probe.c §6/§8(bash tools/timer_probe.sh) |"

echo "----------------------------------------"
if [ "$fails" = 0 ]; then echo "perf-probe: 全部通过"; else echo "perf-probe: 失败 $fails 项"; fi
exit $([ "$fails" = 0 ] && echo 0 || echo 1)
